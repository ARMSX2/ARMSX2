// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GS.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSLsfg.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "GS/Renderers/Vulkan/VKSwapChain.h"
#include "GS/Renderers/Vulkan/VKLibretro.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/TileGpu/GSTileGpuShaderVariant.h"

// Libretro presentation backbuffers: the frontend samples the published
// VkImageView asynchronously (including cached-frame replays long after the
// present), so it must never point at a pooled texture that GSDeviceVK can
// recycle. Frames are copied into this small ring of dedicated textures that
// live until device teardown.
namespace
{
	constexpr int kLibretroBackbuffers = 3;
	std::unique_ptr<GSTextureVK> s_libretro_bb[kLibretroBackbuffers];
	int s_libretro_bb_idx = 0;
	// Monotonic count of libretro presents; used to age out retired backbuffers.
	u64 s_libretro_present_count = 0;
	// A backbuffer displaced by a resolution change: the frontend may still be
	// replaying its image for a few frames (cached/duped frames up to its
	// swapchain depth), so it can't be freed immediately -- but leaving it here
	// forever leaks a full-resolution render target on every interlace<->
	// progressive switch (frequent in FMV-heavy games). Each is tagged with the
	// present count at retirement and reclaimed once enough presents have gone
	// by that the frontend can no longer reference it.
	struct RetiredBackbuffer { std::unique_ptr<GSTextureVK> tex; u64 retired_at; };
	std::vector<RetiredBackbuffer> s_libretro_bb_retired;
	// Comfortably beyond any libretro frontend's swapchain depth (2-3).
	constexpr u64 kLibretroRetireFrames = 6;
} // namespace
#include "GS/Renderers/Common/GSDevice.h"

#include "BuildVersion.h"
#include "Host.h"
#include "ImGui/ImGuiManager.h"

#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/Error.h"
#include "common/HostSys.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/Timer.h"

#include "imgui.h"

#ifdef ARMSX2_HAS_LIBRASHADER
// librashader only declares its Vulkan entry points when the consumer opts in, and it
// needs the Vulkan types already in scope — GSDeviceVK.h pulls those in above. Defined
// by CMake only when the Rust toolchain actually produced the library.
#define LIBRA_RUNTIME_VULKAN
#include "librashader.h"
#endif

#include <bit>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

// Tweakables
enum : u32
{
	MAX_DRAW_CALLS_PER_FRAME = 8192,
	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME = 2 * MAX_DRAW_CALLS_PER_FRAME,
	MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME =
		MAX_DRAW_CALLS_PER_FRAME, // assume at least half our draws aren't going to be shuffle/blending
	// CAS uses one storage image per frame, but the TFX texture set also carries the
	// two ROV storage-image bindings (TFX_TEXTURE_RT_ROV / _DEPTH_ROV), and every
	// vkAllocateDescriptorSets of that layout reserves both whether written or not.
	// On the ROV-without-push-descriptor path that is two per TFX draw, so
	// size to match the draw budget rather than the old CAS-only value of 4.
	MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME = 2 * MAX_DRAW_CALLS_PER_FRAME,
	MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
	MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME * 2,

	VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
	INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
	VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,
};


#ifdef ENABLE_OGL_DEBUG
static u32 s_debug_scope_depth = 0;
#endif

static bool IsDATEModePrimIDInit(u32 flag)
{
	return flag == 1 || flag == 2;
}

static VkAttachmentLoadOp GetLoadOpForTexture(GSTextureVK* tex)
{
	if (!tex)
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	// clang-format off
	switch (tex->GetState())
	{
	case GSTextureVK::State::Cleared:       tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case GSTextureVK::State::Invalidated:   tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	case GSTextureVK::State::Dirty:         return VK_ATTACHMENT_LOAD_OP_LOAD;
	default:                                return VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	// clang-format on
}

static constexpr VkClearValue s_present_clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

// Device extensions that are required for PCSX2.
// No hard-required device extensions beyond the swapchain (handled separately).
// VK_KHR_push_descriptor used to be required, but it's now OPTIONAL: some Mali
// drivers (e.g. Mali-G52) don't expose it at all, and we have a full
// non-push-descriptor binding fallback — so requiring it needlessly rejected
// otherwise-capable GPUs ("No physical devices found"). Kept as a (currently
// empty) list so the existing required-extension scan loops stay valid.
static constexpr std::array<const char*, 0> s_required_device_extensions = {};

GSDeviceVK::GSDeviceVK()
{
#ifdef ENABLE_OGL_DEBUG
	s_debug_scope_depth = 0;
#endif

	std::memset(&m_pipeline_selector, 0, sizeof(m_pipeline_selector));
}

GSDeviceVK::~GSDeviceVK() = default;

VkInstance GSDeviceVK::CreateVulkanInstance(const WindowInfo& wi, OptionalExtensions* oe, bool enable_debug_utils,
	bool enable_validation_layer)
{
	ExtensionList enabled_extensions;
	if (!SelectInstanceExtensions(&enabled_extensions, wi, oe, enable_debug_utils))
		return VK_NULL_HANDLE;

	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pNext = nullptr;
	app_info.pApplicationName = "PCSX2";
	app_info.applicationVersion = VK_MAKE_VERSION(
		BuildVersion::GitTagHi, BuildVersion::GitTagMid, BuildVersion::GitTagLo);
	app_info.pEngineName = "PCSX2";
	app_info.engineVersion = VK_MAKE_VERSION(
		BuildVersion::GitTagHi, BuildVersion::GitTagMid, BuildVersion::GitTagLo);
	app_info.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo instance_create_info = {};
	instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_create_info.pNext = nullptr;
	instance_create_info.flags = 0;
	instance_create_info.pApplicationInfo = &app_info;
	instance_create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
	instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
	instance_create_info.enabledLayerCount = 0;
	instance_create_info.ppEnabledLayerNames = nullptr;

	// Enable debug layer on debug builds
	if (enable_validation_layer)
	{
		static const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
		instance_create_info.enabledLayerCount = 1;
		instance_create_info.ppEnabledLayerNames = layer_names;
	}

	VkInstance instance;
	VkResult res = vkCreateInstance(&instance_create_info, nullptr, &instance);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateInstance failed: ");
		return nullptr;
	}

	return instance;
}

bool GSDeviceVK::SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, OptionalExtensions* oe,
	bool enable_debug_utils)
{
	u32 extension_count = 0;
	VkResult res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEnumerateInstanceExtensionProperties failed: ");
		return false;
	}

	if (extension_count == 0)
	{
		Console.Error("VK: No extensions supported by instance.");
		return false;
	}

	std::vector<VkExtensionProperties> available_extension_list(extension_count);
	res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extension_list.data());
	pxAssert(res == VK_SUCCESS);

	auto SupportsExtension = [&available_extension_list, extension_list](const char* name, bool required) {
		if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
				[name](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
			available_extension_list.end())
		{
			DevCon.WriteLn("VK: Enabling extension: %s", name);
			extension_list->push_back(name);
			return true;
		}

		if (required)
			Console.Error("VK: Missing required extension %s.", name);

		return false;
	};

	// Common extensions
	if (wi.type != WindowInfo::Type::Surfaceless && !SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true))
		return false;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
	if (wi.type == WindowInfo::Type::Win32 && !SupportsExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
	if (wi.type == WindowInfo::Type::X11 && !SupportsExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
	if (wi.type == WindowInfo::Type::Wayland && !SupportsExtension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
	if (wi.type == WindowInfo::Type::MacOS && !SupportsExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	if (wi.type == WindowInfo::Type::Android && !SupportsExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, true))
		return false;
#endif

	// VK_KHR_display direct-to-monitor surface (kmsdrm handhelds).
	// VK_KHR_get_display_properties2 is optional but lets us read HDR / extended
	// display info on ICDs that support it.
	if (wi.type == WindowInfo::Type::VulkanDirect)
	{
		if (!SupportsExtension(VK_KHR_DISPLAY_EXTENSION_NAME, true))
			return false;
		SupportsExtension(VK_KHR_GET_DISPLAY_PROPERTIES_2_EXTENSION_NAME, false);
	}

	// VK_EXT_debug_utils
	if (enable_debug_utils && !SupportsExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
		Console.Warning("VK: Debug report requested, but extension is not available.");

	oe->vk_swapchain_maintenance1 = wi.type != WindowInfo::Type::Surfaceless;
	if (wi.type != WindowInfo::Type::Surfaceless)
	{
		oe->vk_swapchain_maintenance1 = true;
		// VK_EXT_swapchain_maintenance1 requires VK_EXT_surface_maintenance1.
		// VK_KHR_swapchain_maintenance1 might require VK_KHR_surface_maintenance1 (It does on Nvidia).
		// If either VK_KHR_surface_maintenance1 is supported, or VK_EXT_swapchain_maintenance1 is unsupported, don't try VK_EXT_swapchain_maintenance1.
		oe->vk_swapchain_maintenance1_is_khr = SupportsExtension(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false) ||
			!SupportsExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false);
	}
	else
		oe->vk_swapchain_maintenance1 = false;

	// Needed for exclusive fullscreen control.
	SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

	return true;
}

GSDeviceVK::GPUList GSDeviceVK::EnumerateGPUs(VkInstance instance)
{
	GPUList gpus;

	u32 gpu_count = 0;
	VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
	if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || gpu_count == 0)
	{
		LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices (1) failed: ");
		return gpus;
	}

	std::vector<VkPhysicalDevice> physical_devices(gpu_count);
	res = vkEnumeratePhysicalDevices(instance, &gpu_count, physical_devices.data());
	if (res == VK_INCOMPLETE)
	{
		Console.Warning("VK: First vkEnumeratePhysicalDevices() call returned %zu devices, but second returned %u",
			physical_devices.size(), gpu_count);
	}
	else if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices (2) failed: ");
		return gpus;
	}

	// Maybe we lost a GPU?
	if (gpu_count < physical_devices.size())
		physical_devices.resize(gpu_count);

	gpus.reserve(physical_devices.size());
	for (VkPhysicalDevice device : physical_devices)
	{
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(device, &props);

		// Skip GPUs which don't support Vulkan 1.1, since we won't be able to create a device with them anyway.
		if (VK_API_VERSION_VARIANT(props.apiVersion) == 0 && VK_API_VERSION_MAJOR(props.apiVersion) <= 1 &&
			VK_API_VERSION_MINOR(props.apiVersion) < 1)
		{
			Console.Warning(fmt::format("VK: Ignoring GPU '{}' because it only claims support for Vulkan {}.{}.{}",
				props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
				VK_API_VERSION_PATCH(props.apiVersion)));
			continue;
		}

		// Query the extension list to ensure that we don't include GPUs that are missing the extensions we require.
		u32 extension_count = 0;
		res = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
		if (res != VK_SUCCESS)
		{
			Console.Warning(fmt::format("VK: Ignoring GPU '{}' because vkEnumerateInstanceExtensionProperties() failed: ",
				props.deviceName, Vulkan::VkResultToString(res)));
			continue;
		}

		std::vector<VkExtensionProperties> available_extension_list(extension_count);
		if (extension_count > 0)
		{
			res = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available_extension_list.data());
			pxAssert(res == VK_SUCCESS);
		}
		bool has_missing_extension = false;
		for (const char* required_extension_name : s_required_device_extensions)
		{
			if (std::find_if(available_extension_list.begin(), available_extension_list.end(), [required_extension_name](const VkExtensionProperties& ext) {
					return (std::strcmp(required_extension_name, ext.extensionName) == 0);
				}) == available_extension_list.end())
			{
				Console.Warning(fmt::format("VK: Ignoring GPU '{}' because is is missing required extension {}",
					props.deviceName, required_extension_name));
				has_missing_extension = true;
			}
		}
		if (has_missing_extension)
			continue;

		GSAdapterInfo ai;
		ai.name = props.deviceName;
		ai.max_texture_size = std::min(props.limits.maxFramebufferWidth, props.limits.maxImageDimension2D);
		ai.max_upscale_multiplier = GSGetMaxUpscaleMultiplier(ai.max_texture_size);

		// handle duplicate adapter names
		if (std::any_of(
				gpus.begin(), gpus.end(), [&ai](const auto& other) { return (ai.name == other.second.name); }))
		{
			std::string original_adapter_name = std::move(ai.name);

			u32 current_extra = 2;
			do
			{
				ai.name = fmt::format("{} ({})", original_adapter_name, current_extra);
				current_extra++;
			} while (std::any_of(
				gpus.begin(), gpus.end(), [&ai](const auto& other) { return (ai.name == other.second.name); }));
		}

		gpus.emplace_back(device, std::move(ai));
	}

	return gpus;
}

GSDeviceVK::GPUList GSDeviceVK::EnumerateGPUs()
{
	std::unique_lock lock(s_instance_mutex);

	// Device shouldn't be torn down since we have the lock.
	GPUList gpus;
	if (g_gs_device && Vulkan::IsVulkanLibraryLoaded())
	{
		gpus = EnumerateGPUs(GSDeviceVK::GetInstance()->GetVulkanInstance());
	}
	else
	{
		// The library may already be loaded by the host (libretro preloads it
		// for the context negotiation) — use it, and don't unload it after.
		const bool library_was_loaded = Vulkan::IsVulkanLibraryLoaded();
		if (library_was_loaded || Vulkan::LoadVulkanLibrary(nullptr))
		{
			OptionalExtensions oe = {};
			const VkInstance instance = CreateVulkanInstance(WindowInfo(), &oe, false, false);
			if (instance != VK_NULL_HANDLE)
			{
				if (Vulkan::LoadVulkanInstanceFunctions(instance))
					gpus = EnumerateGPUs(instance);

				vkDestroyInstance(instance, nullptr);
			}

			if (!library_was_loaded)
				Vulkan::UnloadVulkanLibrary();
		}
	}

	return gpus;
}

// Standing rule: gate optional functionality on the advertised-extension list resolved here,
// never on vkGetDeviceProcAddr returning non-NULL. Drivers ship stub entry points for
// extensions they do not advertise (Mali r44p1 resolves a vkCmdPushDescriptorSetKHR whose call
// is a plain no-op return), and the spec only defines behaviour for entry points of extensions
// that were actually enabled at device creation.
bool GSDeviceVK::SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface)
{
	u32 extension_count = 0;
	VkResult res = vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extension_count, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEnumerateDeviceExtensionProperties failed: ");
		return false;
	}

	if (extension_count == 0)
	{
		Console.Error("VK: No extensions supported by device.");
		return false;
	}

	std::vector<VkExtensionProperties> available_extension_list(extension_count);
	res = vkEnumerateDeviceExtensionProperties(
		m_physical_device, nullptr, &extension_count, available_extension_list.data());
	pxAssert(res == VK_SUCCESS);

	auto SupportsExtension = [&available_extension_list, extension_list](const char* name, bool required) {
		if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
				[name](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
			available_extension_list.end())
		{
			if (std::none_of(extension_list->begin(), extension_list->end(),
					[name](const char* existing_name) { return (std::strcmp(existing_name, name) == 0); }))
			{
				DevCon.WriteLn("VK: Enabling extension: %s", name);
				extension_list->push_back(name);
			}

			return true;
		}

		if (required)
			Console.Error("VK: Missing required extension %s.", name);

		return false;
	};

	if (enable_surface && !SupportsExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true))
		return false;

	// Required extensions.
	for (const char* extension_name : s_required_device_extensions)
	{
		if (!SupportsExtension(extension_name, true))
			return false;
	}

	// Optional now (was required). Enabled when present; CreateDevice decides
	// whether to actually use it (never on Mali — driver bug) or fall back.
	m_optional_extensions.vk_khr_push_descriptor = SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_provoking_vertex = SupportsExtension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_memory_budget = SupportsExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_calibrated_timestamps =
		SupportsExtension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, false);
	// ROAA is DUAL-NAMED: ARM shipped VK_ARM_rasterization_order_attachment_access (Mali
	// driver r36p0), and the promoted VK_EXT_ alias only landed at r40p0. The structs/enums
	// are identical (alias), so accept EITHER — otherwise Mali on r36-r39 blobs (a big chunk
	// of mid-tier, incl. Tensor G2/G3 on old blobs) exposes only the ARM name and gets
	// silently demoted to the per-primitive-barrier slideshow. SupportsExtension enables
	// whichever name it finds (EXT preferred via short-circuit). Matches upstream 5da4b7e.
	m_optional_extensions.vk_ext_rasterization_order_attachment_access =
		SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false) ||
		SupportsExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
	// VK_EXT_attachment_feedback_loop_layout: the in-tile feedback-loop path is what lets
	// accurate blending run WITHOUT the per-primitive texture-barrier "slideshow". We used to
	// blanket-disable it on Mali (vendorID 0x13B5) after the EmuCoreX dev saw stale-color /
	// device-lost on SOME MediaTek-Mali blobs — but that demoted EVERY modern Mali (e.g.
	// Mali-G615 on r44p1) to the barrier path, costing ~3-4x on blend-heavy games. izzy2lost's
	// PSX2 (PCSX2_ARM64) keeps it enabled on Mali and runs those same devices full-speed, so
	// the disable was over-broad. Enable wherever the driver advertises it; the authoritative
	// feature-bit reconciliation below (attachmentFeedbackLoopLayout == VK_TRUE) still filters
	// blobs that don't truly support it. If a specific old blob regresses, narrow by driver
	// version rather than re-blocking the whole vendor.
	m_optional_extensions.vk_ext_attachment_feedback_loop_layout =
		SupportsExtension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_line_rasterization = SupportsExtension(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, false);
	m_optional_extensions.vk_khr_driver_properties = SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);
	// VK_EXT_device_fault: post-mortem for VK_ERROR_DEVICE_LOST. The ~1-in-60 SD865
	// device loss in the opening frames exits carrying nothing but the error code
	// today; where the driver offers this (Turnip does), the loss is followed by
	// vkGetDeviceFaultInfoEXT and the fault's addresses and reasons go to the log,
	// so the next occurrence arrives with its own diagnosis instead of a rate.
	m_optional_extensions.vk_ext_device_fault = SupportsExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, false);

	if (m_optional_extensions.vk_swapchain_maintenance1)
	{
		const bool khr_swapchain_maintenance1 = SupportsExtension(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, false);
		// vk_swapchain_maintenance1_is_khr will be set if we havn't enabled VK_EXT_surface_maintenance1
		// This will happen if either the VK_EXT_surface_maintenance1 was unsupported, or we instead found the KHR version.
		// As the EXT version depends on the surface maintenance1 extension, we need to check that aswell.
		m_optional_extensions.vk_swapchain_maintenance1 = khr_swapchain_maintenance1 ? khr_swapchain_maintenance1 :
			(!m_optional_extensions.vk_swapchain_maintenance1_is_khr && SupportsExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, false));

		m_optional_extensions.vk_swapchain_maintenance1_is_khr = khr_swapchain_maintenance1;
	}

	// glslang generates debug info instructions before phi nodes at the beginning of blocks when non-semantic debug info
	// is enabled, triggering errors by spirv-val. Gate it by an environment variable if you want source debugging until
	// this is fixed.
	if (const char* val = std::getenv("USE_NON_SEMANTIC_DEBUG_INFO"); val && StringUtil::FromChars<bool>(val).value_or(false))
	{
		m_optional_extensions.vk_khr_shader_non_semantic_info =
			SupportsExtension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, false);
	}

#ifdef _WIN32
	m_optional_extensions.vk_ext_full_screen_exclusive =
		enable_surface && SupportsExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME, false);
#endif

	m_optional_extensions.vk_ext_fragment_shader_interlock = SupportsExtension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, false);
	// The TileGpu backend's device contract: indexed state tables addressed as storage-buffer
	// arrays (descriptor indexing) fed by an indirect draw stream. Detected here and enabled
	// below only if their feature bits truly hold. The device is created once, before any
	// renderer variant is chosen and never recreated on a variant switch, so these must be
	// enabled whenever present rather than gated on the variant; the other renderers never
	// touch them, so enabling them is inert. draw_indirect_count (count buffers) is a nicety,
	// not part of the capability gate.
	m_optional_extensions.vk_ext_descriptor_indexing =
		SupportsExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, false);
	m_optional_extensions.vk_khr_draw_indirect_count =
		SupportsExtension(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, false);
	// LSFG frame generation only. PCSX2 targets Vulkan 1.1, where neither is core — the memory
	// model is 1.2 and nullDescriptor never became core at all — so both come in as extensions.
	m_optional_extensions.vk_khr_vulkan_memory_model = SupportsExtension(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_robustness2_null_descriptor = SupportsExtension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, false);

	return true;
}

bool GSDeviceVK::SelectDeviceFeatures()
{
	VkPhysicalDeviceFeatures available_features;
	vkGetPhysicalDeviceFeatures(m_physical_device, &available_features);

	// Enable the features we use. dualSrcBlend is one of them for the HW renderer, which SW-blends
	// where it is absent -- and a TileGpu CONTRACT term, which has no such fallback: its fragment
	// shader declares an index-1 output and its blend table names SRC1_* factors, so a device
	// without it fails pipeline creation rather than rendering differently. CreateDevice folds it
	// into the gate.
	m_device_features.dualSrcBlend = available_features.dualSrcBlend;
	m_device_features.largePoints = available_features.largePoints;
	m_device_features.wideLines = available_features.wideLines;
	m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
	m_device_features.textureCompressionBC = available_features.textureCompressionBC;
	// Enabled at logical-device creation, not merely queried: sampling an ASTC image
	// without the feature bit enabled is a validation error and undefined behaviour on
	// some drivers. SelectDeviceFeatures() runs before vkCreateDevice().
	m_device_features.textureCompressionASTC_LDR = available_features.textureCompressionASTC_LDR;
	m_device_features.geometryShader = available_features.geometryShader;
	m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
	m_device_features.pipelineStatisticsQuery = available_features.pipelineStatisticsQuery;
	// TileGpu's indirect draw stream: multiDrawIndirect so one vkCmdDrawIndirect covers many
	// draws, drawIndirectFirstInstance so each indirect draw carries its own firstInstance (the
	// index into the state tables). Enabled only where the device has them; unused by the other
	// renderers. CreateDevice folds these into the TileGpu capability gate.
	m_device_features.multiDrawIndirect = available_features.multiDrawIndirect;
	m_device_features.drawIndirectFirstInstance = available_features.drawIndirectFirstInstance;
	// ...and shaderSampledImageArrayDynamicIndexing: tilegpu.glsl's rule-2 tap reads ONE
	// texelFetch out of the per-pass sampled-target array at an index that comes from the draw's
	// state row. A chain of literal indices needs no feature, but it is eight fetch sites and the
	// a650 has a hard fragment-program size budget the chain does not fit inside (see the comment
	// at the top of tilegpu.glsl). Core 1.0, but not one every implementation has to offer, so it
	// is checked, not assumed; CreateDevice folds it into the rule-2 gate.
	m_device_features.shaderSampledImageArrayDynamicIndexing =
		available_features.shaderSampledImageArrayDynamicIndexing;

	// ...and robustBufferAccess, under EmuCore/GS/TileGpuStrictMemory and nothing else. Left off,
	// a buffer read that leaves its binding returns whatever the address space holds there, which
	// is a different thing on every driver and can be a different thing on two runs of the same
	// one. On it, the read returns zero: the wrong pixel becomes a REPEATABLE wrong pixel, which is
	// what makes an out-of-bounds index distinguishable from a coherency fault. Core, and the spec
	// requires every implementation to offer it, so the availability check below is a formality --
	// it is here because silently not enabling a feature we told the log we enabled is worse than
	// the missing feature would be.
	if (GSConfig.TileGpuStrictMemory)
	{
		if (available_features.robustBufferAccess == VK_TRUE)
			m_device_features.robustBufferAccess = VK_TRUE;
		else
			Console.Warning("VK: TileGpuStrictMemory asked for robustBufferAccess and the device does not offer it.");
	}

	return true;
}

bool GSDeviceVK::CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer)
{
	u32 queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, nullptr);
	if (queue_family_count == 0)
	{
		Console.Error("No queue families found on specified vulkan physical device.");
		return false;
	}

	std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, queue_family_properties.data());
	DevCon.WriteLn("%u vulkan queue families", queue_family_count);

	// Find graphics and present queues.
	m_graphics_queue_family_index = queue_family_count;
	m_present_queue_family_index = queue_family_count;
	m_spin_queue_family_index = queue_family_count;
	u32 spin_queue_index = 0;
	for (uint32_t i = 0; i < queue_family_count; i++)
	{
		VkBool32 graphics_supported = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
		if (graphics_supported)
		{
			m_graphics_queue_family_index = i;
			// Quit now, no need for a present queue.
			if (!surface)
			{
				break;
			}
		}

		if (surface)
		{
			VkBool32 present_supported;
			VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, i, surface, &present_supported);
			if (res != VK_SUCCESS)
			{
				LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
				return false;
			}

			if (present_supported)
			{
				m_present_queue_family_index = i;
			}

			// Prefer one queue family index that does both graphics and present.
			if (graphics_supported && present_supported)
			{
				break;
			}
		}
	}
	for (uint32_t i = 0; i < queue_family_count; i++)
	{
		// Pick a queue for spinning
		if (!(queue_family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
			continue; // We need compute
		if (queue_family_properties[i].timestampValidBits == 0)
			continue; // We need timing
		const bool queue_is_used = i == m_graphics_queue_family_index || i == m_present_queue_family_index;
		if (queue_is_used && m_spin_queue_family_index != queue_family_count)
			continue; // Found a non-graphics queue to use
		spin_queue_index = 0;
		m_spin_queue_family_index = i;
		if (queue_is_used && queue_family_properties[i].queueCount > 1)
			spin_queue_index = 1;
		if (!(queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			break; // Async compute queue, definitely pick this one
	}
	if (m_graphics_queue_family_index == queue_family_count)
	{
		Console.Error("VK: Failed to find an acceptable graphics queue.");
		return false;
	}
	if (surface != VK_NULL_HANDLE && m_present_queue_family_index == queue_family_count)
	{
		Console.Error("VK: Failed to find an acceptable present queue.");
		return false;
	}

	VkDeviceCreateInfo device_info = {};
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.pNext = nullptr;
	device_info.flags = 0;
	device_info.queueCreateInfoCount = 0;

	static constexpr float queue_priorities[] = {1.0f, 0.0f}; // Low priority for the spin queue
	std::array<VkDeviceQueueCreateInfo, 3> queue_infos;
	VkDeviceQueueCreateInfo& graphics_queue_info = queue_infos[device_info.queueCreateInfoCount++];
	graphics_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	graphics_queue_info.pNext = nullptr;
	graphics_queue_info.flags = 0;
	graphics_queue_info.queueFamilyIndex = m_graphics_queue_family_index;
	graphics_queue_info.queueCount = 1;
	graphics_queue_info.pQueuePriorities = queue_priorities;

	if (surface != VK_NULL_HANDLE && m_graphics_queue_family_index != m_present_queue_family_index)
	{
		VkDeviceQueueCreateInfo& present_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		present_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		present_queue_info.pNext = nullptr;
		present_queue_info.flags = 0;
		present_queue_info.queueFamilyIndex = m_present_queue_family_index;
		present_queue_info.queueCount = 1;
		present_queue_info.pQueuePriorities = queue_priorities;
	}

	if (m_spin_queue_family_index == m_graphics_queue_family_index)
	{
		if (spin_queue_index != 0)
			graphics_queue_info.queueCount = 2;
	}
	else if (m_spin_queue_family_index == m_present_queue_family_index)
	{
		if (spin_queue_index != 0)
			queue_infos[1].queueCount = 2; // present queue
	}
	else if (m_spin_queue_family_index != queue_family_count)
	{
		VkDeviceQueueCreateInfo& spin_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		spin_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		spin_queue_info.pNext = nullptr;
		spin_queue_info.flags = 0;
		spin_queue_info.queueFamilyIndex = m_spin_queue_family_index;
		spin_queue_info.queueCount = 1;
		spin_queue_info.pQueuePriorities = queue_priorities + 1;
	}

	device_info.pQueueCreateInfos = queue_infos.data();

	ExtensionList enabled_extensions;
	if (!SelectDeviceExtensions(&enabled_extensions, surface != VK_NULL_HANDLE))
		return false;

	device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
	device_info.ppEnabledExtensionNames = enabled_extensions.data();

	// Check for required features before creating.
	if (!SelectDeviceFeatures())
		return false;

	device_info.pEnabledFeatures = &m_device_features;

	// The other half of TileGpuStrictMemory, taken here so it is settled before the first
	// allocation: every host-visible stream buffer REQUIRES a coherent memory type instead of
	// preferring one. Read once into a member rather than at each allocation, so a setting changed
	// mid-session cannot leave the device holding buffers on two different kinds of memory.
	m_strict_host_memory = GSConfig.TileGpuStrictMemory;
	if (m_strict_host_memory)
	{
		Console.WriteLn("VK: TileGpuStrictMemory engaged: robustBufferAccess %s, host-visible stream "
						"buffers pinned to a HOST_COHERENT memory type.",
			(m_device_features.robustBufferAccess == VK_TRUE) ? "on" : "UNAVAILABLE");
	}

	// Enable debug layer on debug builds
	if (enable_validation_layer)
	{
		static const char* layer_names[] = {"VK_LAYER_LUNARG_standard_validation"};
		device_info.enabledLayerCount = 1;
		device_info.ppEnabledLayerNames = layer_names;
	}

	// provoking vertex
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
	// VK_EXT_swapchain_maintenance1 types/enums are aliases of VK_KHR_swapchain_maintenance1 types/enums.
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragment_shader_interlock_ext_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
	VkPhysicalDeviceFaultFeaturesEXT device_fault_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
	VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
	VkPhysicalDeviceVulkanMemoryModelFeatures vulkan_memory_model_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
	VkPhysicalDeviceRobustness2FeaturesEXT robustness2_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};

	// An advertised EXTENSION does not guarantee its FEATURE bit, and asking for a feature the
	// driver does not have fails vkCreateDevice outright with VK_ERROR_FEATURE_NOT_PRESENT —
	// killing Vulkan entirely instead of quietly doing without one optional nicety. PowerVR
	// BXM-8-256 does exactly this: it exposes the extensions below, reports at least one of their
	// features as false, and the renderer then refuses to start at all with "Failed to create
	// render device".
	//
	// ProcessDeviceExtensions performs this same reconcile, but it runs AFTER vkCreateDevice, so it
	// can only ever describe the failure rather than prevent it. Probe every feature we are about to
	// request, up front, and drop the ones that are not really there. This subsumes the depth-ROAA
	// probe that used to be the only instance of this check.
	{
		VkPhysicalDeviceProvokingVertexFeaturesEXT probe_pv = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT probe_line = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT probe_roaa = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
		VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT probe_afl = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
		VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR probe_sm1 = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
		VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT probe_fsi = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
		VkPhysicalDeviceFaultFeaturesEXT probe_fault = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
		VkPhysicalDeviceDescriptorIndexingFeatures probe_di = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
		VkPhysicalDeviceVulkanMemoryModelFeatures probe_vmm = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
		VkPhysicalDeviceRobustness2FeaturesEXT probe_r2 = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};

		// Only chain what we would actually enable: querying a struct whose extension is absent is
		// not something the spec promises anything about.
		VkPhysicalDeviceFeatures2 probe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		if (m_optional_extensions.vk_ext_provoking_vertex)
			Vulkan::AddPointerToChain(&probe, &probe_pv);
		if (m_optional_extensions.vk_ext_line_rasterization)
			Vulkan::AddPointerToChain(&probe, &probe_line);
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
			Vulkan::AddPointerToChain(&probe, &probe_roaa);
		if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
			Vulkan::AddPointerToChain(&probe, &probe_afl);
		if (m_optional_extensions.vk_swapchain_maintenance1)
			Vulkan::AddPointerToChain(&probe, &probe_sm1);
		if (m_optional_extensions.vk_ext_fragment_shader_interlock)
			Vulkan::AddPointerToChain(&probe, &probe_fsi);
		if (m_optional_extensions.vk_ext_device_fault)
			Vulkan::AddPointerToChain(&probe, &probe_fault);
		if (m_optional_extensions.vk_ext_descriptor_indexing)
			Vulkan::AddPointerToChain(&probe, &probe_di);
		if (m_optional_extensions.vk_khr_vulkan_memory_model)
			Vulkan::AddPointerToChain(&probe, &probe_vmm);
		if (m_optional_extensions.vk_ext_robustness2_null_descriptor)
			Vulkan::AddPointerToChain(&probe, &probe_r2);
		vkGetPhysicalDeviceFeatures2(m_physical_device, &probe);

		// Returns the flag rather than taking it by reference: m_optional_extensions members are
		// bit-fields, which cannot bind to bool&.
		const auto keep = [](const char* name, bool advertised, bool supported) -> bool {
			if (advertised && !supported)
			{
				// Logged, because "Vulkan works but this one thing is off" is a very different
				// bug report from "Vulkan does not start", and the next person needs to know
				// which feature the driver advertised without supporting.
				Console.Warning(fmt::format(
					"VK: {} advertised but its feature is unsupported — not requesting it.", name));
				return false;
			}
			return advertised;
		};
		m_optional_extensions.vk_ext_provoking_vertex = keep("VK_EXT_provoking_vertex",
			m_optional_extensions.vk_ext_provoking_vertex, probe_pv.provokingVertexLast == VK_TRUE);
		m_optional_extensions.vk_ext_line_rasterization = keep("VK_EXT_line_rasterization",
			m_optional_extensions.vk_ext_line_rasterization, probe_line.bresenhamLines == VK_TRUE);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access =
			keep("VK_EXT_rasterization_order_attachment_access",
				m_optional_extensions.vk_ext_rasterization_order_attachment_access,
				probe_roaa.rasterizationOrderColorAttachmentAccess == VK_TRUE);
		m_optional_extensions.vk_ext_attachment_feedback_loop_layout =
			keep("VK_EXT_attachment_feedback_loop_layout",
				m_optional_extensions.vk_ext_attachment_feedback_loop_layout,
				probe_afl.attachmentFeedbackLoopLayout == VK_TRUE);
		m_optional_extensions.vk_swapchain_maintenance1 = keep("VK_EXT_swapchain_maintenance1",
			m_optional_extensions.vk_swapchain_maintenance1, probe_sm1.swapchainMaintenance1 == VK_TRUE);
		m_optional_extensions.vk_ext_fragment_shader_interlock = keep("VK_EXT_fragment_shader_interlock",
			m_optional_extensions.vk_ext_fragment_shader_interlock,
			probe_fsi.fragmentShaderPixelInterlock == VK_TRUE);
		m_optional_extensions.vk_ext_device_fault = keep("VK_EXT_device_fault",
			m_optional_extensions.vk_ext_device_fault, probe_fault.deviceFault == VK_TRUE);
		m_optional_extensions.vk_khr_vulkan_memory_model = keep("VK_KHR_vulkan_memory_model",
			m_optional_extensions.vk_khr_vulkan_memory_model, probe_vmm.vulkanMemoryModel == VK_TRUE);
		// The FEATURE we want is nullDescriptor specifically. VK_EXT_robustness2 also carries
		// robustBufferAccess2/robustImageAccess2, which cost performance and which nothing here
		// needs — they are deliberately left VK_FALSE below.
		m_optional_extensions.vk_ext_robustness2_null_descriptor = keep("VK_EXT_robustness2 (nullDescriptor)",
			m_optional_extensions.vk_ext_robustness2_null_descriptor, probe_r2.nullDescriptor == VK_TRUE);

		// Depth ROAA is an optional sub-feature: a driver can offer the extension and colour
		// access yet not depth.
		m_optional_extensions.vk_ext_roaa_depth =
			m_optional_extensions.vk_ext_rasterization_order_attachment_access &&
			probe_roaa.rasterizationOrderDepthAttachmentAccess == VK_TRUE;

		// Descriptor indexing is only useful to TileGpu if the specific sub-features its
		// bindless storage-buffer tables need are ALL present: non-uniform indexing of storage
		// buffers, an unbounded array, and partially-bound slots. An extension advertised with
		// the wrong bits is no extension at all for this purpose, and requesting a missing bit
		// would fail vkCreateDevice outright.
		const bool di_bits = probe_di.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
							 probe_di.runtimeDescriptorArray == VK_TRUE &&
							 probe_di.descriptorBindingPartiallyBound == VK_TRUE;
		m_optional_extensions.vk_ext_descriptor_indexing = keep("VK_EXT_descriptor_indexing",
			m_optional_extensions.vk_ext_descriptor_indexing, di_bits);

		// The TileGpu backend can run on this device only if its whole contract holds: the
		// descriptor-indexing sub-features above and an indirect draw stream that carries a per-draw
		// firstInstance. Recorded once.
		//
		// Dual-source blending LEFT this contract when the road below was built: there is now a
		// second road to the As blend factor and every device can take it. It was a real exclusion
		// while it lasted -- tilegpu.glsl declared a second
		// colour output at index 1 unconditionally and the blend table named SRC1_* factors, which
		// Vulkan forbids outright when dualSrcBlend is false, so a device without it did not render
		// badly, it failed pipeline creation on the first draw.
		//
		// The renderer does NOT refuse to construct without the contract -- it builds and runs
		// regardless -- but nothing gets that far any more: a contract-absent device resolves to
		// Classic at variant selection, and GSTileSelectionPolicy.h carries that decision record.
		// The downstream discard in GSRendererTileGpu::BuildAndExecutePlan is now only reachable
		// under EmuCore/GS/TileGpuIgnoreDeviceContract.
		m_optional_extensions.tilegpu_device_capable = m_optional_extensions.vk_ext_descriptor_indexing &&
													   m_device_features.multiDrawIndirect == VK_TRUE &&
													   m_device_features.drawIndirectFirstInstance == VK_TRUE;
		// The scissor is not part of that contract either, and needs no device feature: it is a
		// vkCmdSetScissor before each indirect call on every device, with the call cut wherever the
		// rectangle changes. The vertex-clip-plane road it replaced was cheaper (one call could
		// carry any number of scissors) but it was not the same test -- clipping is geometric, so a
		// cut primitive is re-interpolated from its clipped vertices and a fragment well inside the
		// rectangle can take an attribute value a ULP off the unclipped plane's, by float rules that
		// differ per vendor. The GS scissor is an integer rectangle test on rasterized pixels and
		// touches nothing else, which is what a Vulkan scissor is.
		//
		// ...and the As blend factor, which does need one and is decided here: the fragment module
		// either declares an index-1 output or it does not, and a module that declares one is a
		// module a driver without the feature may refuse. Three states -- zero asks the device,
		// positive forces the feature-free roads anywhere, negative asks for the second output,
		// which a device without dualSrcBlend still cannot give.
		m_optional_extensions.tilegpu_dual_source =
			gsTileGpuDualSourceRoad(GSConfig.TileGpuDualSrcRoad, m_device_features.dualSrcBlend == VK_TRUE);
		DevCon.WriteLn("VK: TileGpu device contract %s (descriptor-indexing=%s, indirect=%s, dual-src=%s), "
					   "As factor road %s (TileGpuDualSrcRoad=%d).",
			m_optional_extensions.tilegpu_device_capable ? "present" : "absent",
			m_optional_extensions.vk_ext_descriptor_indexing ? "yes" : "no",
			(m_device_features.multiDrawIndirect && m_device_features.drawIndirectFirstInstance) ? "yes" : "no",
			m_device_features.dualSrcBlend ? "yes" : "no",
			m_optional_extensions.tilegpu_dual_source ? "second output" : "alpha carrier",
			GSConfig.TileGpuDualSrcRoad);

		// Rule 2 (the tap reading a resident target instead of the bytes) needs one thing the rest
		// of the contract does not: indexing the per-pass sampled-target array by the draw's slot.
		// Without it rule 2 stays off and the shader compiles its array-less tap — a shader that
		// merely never executes a dynamic index it cannot legally take is still a shader the
		// driver may reject. NON-uniform indexing is deliberately NOT required: the executor
		// splits its indirect runs at a slot change, so the index is wave-uniform by construction
		// (ExecuteTileGpuPassPlan).
		//
		// EmuCore/GS/TileGpuDisableBindlessTargets holds it off whatever the device answers. It is
		// for a bring-up round on a device the contract has only just started passing on: every
		// road the contract gates comes up in the same boot, and rules 2 and 3 are the pair worth
		// seeing separately from the rest. Folded in HERE, not at the accessor, so the shader's
		// defines, the source-array build and the renderer all read one decision -- a shader that
		// declares the array while the renderer refuses to bind it is the one shape that must not
		// happen. Fail-closed: unset, the device's own answer stands.
		m_optional_extensions.tilegpu_bindless_targets =
			m_optional_extensions.tilegpu_device_capable && !GSConfig.TileGpuDisableBindlessTargets &&
			m_device_features.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
		DevCon.WriteLn("VK: TileGpu sampled targets %s (sampled-image array dynamic indexing=%s%s).",
			m_optional_extensions.tilegpu_bindless_targets ? "enabled" : "disabled",
			m_device_features.shaderSampledImageArrayDynamicIndexing ? "yes" : "no",
			GSConfig.TileGpuDisableBindlessTargets ? ", HELD OFF by TileGpuDisableBindlessTargets" : "");

		// The in-pass destination read: a pass declares its colour attachment as an input attachment
		// too and the fragment stage reads the pixel it is about to write, in rasterization order.
		// Needs the ROAA colour feature and nothing else -- the descriptor is an ordinary input
		// attachment. Without it the renderer admits no draw to the read, declares no pass, and keeps
		// the snapshot road for DATE, so a device that lacks it renders exactly what it rendered
		// before.
		//
		// EmuCore/GS/TileGpuDisableSelfRead holds it off whatever the device answers, which makes
		// that read-less road reachable on a device that HAS the extension. The read is only as
		// trustworthy as the driver's ROAA implementation, and the case this was written for is a
		// proprietary mobile driver showing run-to-run pixel nondeterminism with the read engaged --
		// set the key and the device answers as if the extension were absent, which isolates the
		// read for an A/B. It moves ADMISSION, not just a road, so the two arms may legitimately
		// differ in output; it is a diagnostic lever, not a pixel-inert switch. Folded in HERE for
		// the same reason as the key above: the pipelines and the renderer must read one decision.
		// Fail-closed: unset, the device's own answer stands.
		m_optional_extensions.tilegpu_self_read =
			m_optional_extensions.tilegpu_device_capable && !GSConfig.TileGpuDisableSelfRead &&
			m_optional_extensions.vk_ext_rasterization_order_attachment_access;
		DevCon.WriteLn("VK: TileGpu in-pass destination read %s (rasterization-order colour access=%s%s).",
			m_optional_extensions.tilegpu_self_read ? "enabled" : "disabled",
			m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "yes" : "no",
			GSConfig.TileGpuDisableSelfRead ? ", HELD OFF by TileGpuDisableSelfRead" : "");
	}

	if (m_optional_extensions.vk_ext_provoking_vertex)
	{
		provoking_vertex_feature.provokingVertexLast = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &provoking_vertex_feature);
	}
	if (m_optional_extensions.vk_ext_line_rasterization)
	{
		line_rasterization_feature.bresenhamLines = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &line_rasterization_feature);
	}
	if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
	{
		rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess = VK_TRUE;
		if (m_optional_extensions.vk_ext_roaa_depth)
			rasterization_order_access_feature.rasterizationOrderDepthAttachmentAccess = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &rasterization_order_access_feature);
	}
	if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
	{
		attachment_feedback_loop_feature.attachmentFeedbackLoopLayout = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &attachment_feedback_loop_feature);
	}
	if (m_optional_extensions.vk_swapchain_maintenance1)
	{
		swapchain_maintenance1_feature.swapchainMaintenance1 = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &swapchain_maintenance1_feature);
	}
	if (m_optional_extensions.vk_ext_fragment_shader_interlock)
	{
		fragment_shader_interlock_ext_feature.fragmentShaderPixelInterlock = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &fragment_shader_interlock_ext_feature);
	}
	if (m_optional_extensions.vk_ext_device_fault)
	{
		device_fault_feature.deviceFault = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &device_fault_feature);
	}
	if (m_optional_extensions.vk_ext_descriptor_indexing)
	{
		descriptor_indexing_feature.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
		descriptor_indexing_feature.runtimeDescriptorArray = VK_TRUE;
		descriptor_indexing_feature.descriptorBindingPartiallyBound = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &descriptor_indexing_feature);
	}
	if (m_optional_extensions.vk_khr_vulkan_memory_model)
	{
		vulkan_memory_model_feature.vulkanMemoryModel = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &vulkan_memory_model_feature);
	}
	if (m_optional_extensions.vk_ext_robustness2_null_descriptor)
	{
		// nullDescriptor ONLY — see the note by the probe above.
		robustness2_feature.nullDescriptor = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &robustness2_feature);
	}

	VkResult res = vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDevice failed: ");
		return false;
	}

	// With the device created, we can fill the remaining entry points.
	if (!Vulkan::LoadVulkanDeviceFunctions(m_device))
		return false;

	// Grab the graphics and present queues.
	vkGetDeviceQueue(m_device, m_graphics_queue_family_index, 0, &m_graphics_queue);
	if (surface)
	{
		vkGetDeviceQueue(m_device, m_present_queue_family_index, 0, &m_present_queue);
	}
	m_spinning_supported = m_spin_queue_family_index != queue_family_count &&
	                       queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
	                       m_device_properties.limits.timestampPeriod > 0;
	m_spin_queue_is_graphics_queue =
		m_spin_queue_family_index == m_graphics_queue_family_index && spin_queue_index == 0;

	m_gpu_timing_supported = (m_device_properties.limits.timestampComputeAndGraphics != 0 &&
							  queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
							  m_device_properties.limits.timestampPeriod > 0);
	DevCon.WriteLn("GPU timing is %s (TS=%u TS valid bits=%u, TS period=%f)",
		m_gpu_timing_supported ? "supported" : "not supported",
		static_cast<u32>(m_device_properties.limits.timestampComputeAndGraphics),
		queue_family_properties[m_graphics_queue_family_index].timestampValidBits,
		m_device_properties.limits.timestampPeriod);

#if defined(__ANDROID__)
	// Mali-G615 (Valhall 4th-gen) on the r44p1 blob advertises timestampValidBits>0, but its
	// timestamp query pool never resolves even after the command-buffer fence signals —
	// vkGetQueryPoolResults returns VK_NOT_READY every frame ("(CommandBufferCompleted)
	// vkGetQueryPoolResults failed: VK_NOT_READY"), and the present spin-manager that leans on
	// those timestamps stalls into a multi-second freeze (Burnout 3 at native res). Disable GPU
	// timing + present spinning on THIS GPU only: other Mali report better results with them on,
	// so the gate is deliberately narrow (deviceName match, not a blanket Mali rule). Costs only
	// the GPU-time OSD stat and a present-pacing optimisation; rendering correctness is unaffected.
	if (m_device_properties.vendorID == 0x13B5u &&
		std::string_view(m_device_properties.deviceName).find("Mali-G615") != std::string_view::npos)
	{
		Console.WriteLn("Mali-G615: disabling GPU timing + present spinning (r44p1 timestamp-query VK_NOT_READY freeze).");
		m_gpu_timing_supported = false;
		m_spinning_supported = false;
	}
#endif

	m_gpu_pipeline_statistics_supported = (m_device_features.pipelineStatisticsQuery != 0);
	DevCon.WriteLn("GPU pipeline statistics is %s", m_gpu_pipeline_statistics_supported ? "supported" : "not supported");

	if (!ProcessDeviceExtensions())
		return false;

	if (m_spinning_supported)
	{
		vkGetDeviceQueue(m_device, m_spin_queue_family_index, spin_queue_index, &m_spin_queue);

		m_spin_timestamp_scale = m_device_properties.limits.timestampPeriod;
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
		{
#ifdef _WIN32
			LARGE_INTEGER Freq;
			QueryPerformanceFrequency(&Freq);
			m_queryperfcounter_to_ns = 1000000000.0 / static_cast<double>(Freq.QuadPart);
#endif
			CalibrateSpinTimestamp();
		}
	}

	return true;
}

bool GSDeviceVK::ProcessDeviceExtensions()
{
	// advanced feature checks
	VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
	// VK_EXT_swapchain_maintenance1 types/enums are aliases of VK_KHR_swapchain_maintenance1 types/enums.
	// Preset to VK_FALSE, NOT VK_TRUE: Adreno's proprietary driver advertises
	// the extension string but doesn't recognize the feature struct ("Unknown
	// struct with type 0x3b9efc38" in logcat) and never writes it. With a
	// VK_TRUE preset the ignored struct reads back as supported, the device is
	// created without the feature actually enabled, and the swapchain then uses
	// present fences illegally. A driver that does know the struct overwrites
	// this preset either way.
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, nullptr, VK_FALSE};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragment_shader_interlock_ext_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };

	// add in optional feature structs
	if (m_optional_extensions.vk_ext_provoking_vertex)
		Vulkan::AddPointerToChain(&features2, &provoking_vertex_features);
	if (m_optional_extensions.vk_ext_line_rasterization)
		Vulkan::AddPointerToChain(&features2, &line_rasterization_feature);
	if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
		Vulkan::AddPointerToChain(&features2, &rasterization_order_access_feature);
	if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
		Vulkan::AddPointerToChain(&features2, &attachment_feedback_loop_feature);
	if (m_optional_extensions.vk_swapchain_maintenance1)
		Vulkan::AddPointerToChain(&features2, &swapchain_maintenance1_feature);
	if (m_optional_extensions.vk_ext_fragment_shader_interlock)
		Vulkan::AddPointerToChain(&features2, &fragment_shader_interlock_ext_feature);

	// query
	vkGetPhysicalDeviceFeatures2(m_physical_device, &features2);

	// confirm we actually support it
	m_optional_extensions.vk_ext_provoking_vertex &= (provoking_vertex_features.provokingVertexLast == VK_TRUE);
	m_optional_extensions.vk_ext_rasterization_order_attachment_access &=
		(rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
	// Depth ROAA is meaningless (and its subpass/pipeline flags invalid) without the color
	// extension being usable; keep them consistent after the post-create reconcile.
	m_optional_extensions.vk_ext_roaa_depth &= m_optional_extensions.vk_ext_rasterization_order_attachment_access;
		// ...and so is TileGpu's in-pass destination read, which is that extension plus the executor
		// contract. This reconcile runs after vkCreateDevice, so it can only ever narrow.
		m_optional_extensions.tilegpu_self_read &=
			m_optional_extensions.vk_ext_rasterization_order_attachment_access;
	m_optional_extensions.vk_ext_attachment_feedback_loop_layout &=
		(attachment_feedback_loop_feature.attachmentFeedbackLoopLayout == VK_TRUE);

	VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

	if (m_optional_extensions.vk_khr_driver_properties)
	{
		m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
		Vulkan::AddPointerToChain(&properties2, &m_device_driver_properties);
	}

	VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
	if (m_optional_extensions.vk_khr_push_descriptor)
		Vulkan::AddPointerToChain(&properties2, &push_descriptor_properties);

	// query
	vkGetPhysicalDeviceProperties2(m_physical_device, &properties2);

	// The Mali r44p1 blob mishandles the in-tile attachment-feedback-loop blend path and
	// loses the device under it — VK_ERROR_DEVICE_LOST on every game, but ONLY on this driver
	// (Motorola Edge 60 Pro / Mali-G615 r44p1; other Mali blobs, including other G615 units,
	// run it fine). The extension-select comment above anticipated exactly this: "if a specific
	// old blob regresses, narrow by driver version rather than re-blocking the whole vendor."
	// Demote only r44p1 to the slower-but-stable per-primitive barrier path.
	if (m_device_properties.vendorID == 0x13B5u && m_optional_extensions.vk_khr_driver_properties &&
		std::string_view(m_device_driver_properties.driverInfo).find("r44p1") != std::string_view::npos)
	{
		// NOTE: this layout disable alone did NOT stop the DEVICE_LOST, and the reason is that it
		// was vacuous — r44p1 does not advertise VK_EXT_attachment_feedback_loop_layout, so the
		// bit below was already false and nothing changed. It stays as defence for any r44p1
		// build that does expose the extension. The real fix forces r44p1 onto the RT-copy blend
		// path by ALSO disabling texture_barrier; see the matching "Mali r44p1:" block where
		// m_features.texture_barrier is resolved, and the mechanism account on rule
		// vk-arm-r44p1-attachment-self-read in GSGPUDriverProfile.cpp.
		Console.WriteLn("Mali r44p1: disabling attachment-feedback-loop blend path (DEVICE_LOST workaround).");
		m_optional_extensions.vk_ext_attachment_feedback_loop_layout = false;
	}

	// Decide whether to bind textures via VK_KHR_push_descriptor. It's optional
	// now — when it's absent (some Mali, e.g. Mali-G52), unusable, or known-buggy
	// we fall back to per-frame allocated descriptor sets so Vulkan still runs.
	m_use_push_descriptors = m_optional_extensions.vk_khr_push_descriptor;
	if (m_use_push_descriptors && push_descriptor_properties.maxPushDescriptors < NUM_TFX_TEXTURES)
	{
		Console.Warning("VK: maxPushDescriptors (%u) below required (%u) - using descriptor-set fallback.",
			push_descriptor_properties.maxPushDescriptors, NUM_TFX_TEXTURES);
		m_use_push_descriptors = false;
	}
	// Mali (ARM, vendorID 0x13B5): never use push descriptors. Two driver generations
	// justify the one gate, for different reasons. Older blobs really do advertise
	// VK_KHR_push_descriptor and then null-deref inside vkCmdPushDescriptorSetKHR on the
	// first textured draw — observed on a Mali-G52 MP2 running r29p0 (bmdhacks). Newer blobs
	// (r44p1) do not advertise the extension at all, so the gate is moot there — do not read
	// the null-deref story as applying to them when investigating that generation. Where the
	// extension is present and working, the gate is still the right call: a Mali
	// descriptor-set bind containing no dynamic descriptors is constant-time regardless of
	// set size, so push descriptors have nothing to buy there.
	if (m_use_push_descriptors && properties2.properties.vendorID == 0x13B5u)
		m_use_push_descriptors = false;
	// Adreno (Qualcomm, 0x5143): the pre-transplant backend measured a per-draw TFX
	// texture-rebind stall with push descriptors on Turnip (RP6), and a descriptor-set
	// fallback regression on the proprietary driver (8 Elite), so it allowed only the
	// proprietary driver. That Turnip measurement was of the OLD backend's binding code;
	// this backend has always shipped push descriptors on Turnip
	// (Adreno 610/650) and outperforms the fallback there. Allow the two drivers we have
	// evidence for; keep the conservative disable only for an unknown Adreno driver.
	if (m_use_push_descriptors && properties2.properties.vendorID == 0x5143u &&
		m_device_driver_properties.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
		m_device_driver_properties.driverID != VK_DRIVER_ID_MESA_TURNIP)
		m_use_push_descriptors = false;
	if (!m_use_push_descriptors)
		Console.Warning("VK: Using non-push-descriptor texture binding fallback.");

	// The Adreno PROPRIETARY driver mis-selects the provoking vertex with
	// VK_EXT_provoking_vertex (Eden strips it on Qualcomm); drop it there so GSRendererHW's
	// software provoking-vertex-first path runs instead. Turnip keeps the extension: the
	// this backend has shipped it on Turnip with no flat-shading reports, and the SW fallback
	// costs GS-thread CPU per flat-shaded batch.
	if (m_optional_extensions.vk_ext_provoking_vertex && properties2.properties.vendorID == 0x5143u &&
		m_device_driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
		m_optional_extensions.vk_ext_provoking_vertex = false;

	if (m_optional_extensions.vk_ext_line_rasterization && !line_rasterization_feature.bresenhamLines)
	{
		Console.Warning("VK: bresenhamLines is not supported.");
		m_optional_extensions.vk_ext_line_rasterization = false;
	}

	// VK_EXT_calibrated_timestamps checking
	if (m_optional_extensions.vk_ext_calibrated_timestamps)
	{
		u32 count = 0;
		vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(m_physical_device, &count, nullptr);
		std::unique_ptr<VkTimeDomainEXT[]> time_domains = std::make_unique<VkTimeDomainEXT[]>(count);
		vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(m_physical_device, &count, time_domains.get());
		const VkTimeDomainEXT* begin = &time_domains[0];
		const VkTimeDomainEXT* end = &time_domains[count];
		if (std::find(begin, end, VK_TIME_DOMAIN_DEVICE_EXT) == end)
			m_optional_extensions.vk_ext_calibrated_timestamps = false;
		VkTimeDomainEXT preferred_types[] = {
#ifdef _WIN32
			VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT,
#else
#ifdef CLOCK_MONOTONIC_RAW
			VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
#endif
			VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
#endif
		};
		m_calibrated_timestamp_type = VK_TIME_DOMAIN_DEVICE_EXT;
		for (VkTimeDomainEXT type : preferred_types)
		{
			if (std::find(begin, end, type) != end)
			{
				m_calibrated_timestamp_type = type;
				break;
			}
		}
		if (m_calibrated_timestamp_type == VK_TIME_DOMAIN_DEVICE_EXT)
			m_optional_extensions.vk_ext_calibrated_timestamps = false;
	}

	m_optional_extensions.vk_swapchain_maintenance1 &= 
		(swapchain_maintenance1_feature.swapchainMaintenance1 == VK_TRUE);

	m_optional_extensions.vk_ext_fragment_shader_interlock &=
		(fragment_shader_interlock_ext_feature.fragmentShaderPixelInterlock == VK_TRUE);

	Console.WriteLn(
		"VK_EXT_provoking_vertex is %s", m_optional_extensions.vk_ext_provoking_vertex ? "supported" : "NOT supported");
	Console.WriteLn(
		"VK_EXT_memory_budget is %s", m_optional_extensions.vk_ext_memory_budget ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_calibrated_timestamps is %s",
		m_optional_extensions.vk_ext_calibrated_timestamps ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_rasterization_order_attachment_access is %s",
		m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");
	Console.WriteLn("VK_%s_swapchain_maintenance1 is %s",
		m_optional_extensions.vk_swapchain_maintenance1_is_khr ? "KHR" : "EXT",
		m_optional_extensions.vk_swapchain_maintenance1 ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_full_screen_exclusive is %s",
		m_optional_extensions.vk_ext_full_screen_exclusive ? "supported" : "NOT supported");
	Console.WriteLn("VK_KHR_driver_properties is %s",
		m_optional_extensions.vk_khr_driver_properties ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_attachment_feedback_loop_layout is %s",
		m_optional_extensions.vk_ext_attachment_feedback_loop_layout ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_fragment_shader_interlock is %s",
		m_optional_extensions.vk_ext_fragment_shader_interlock ? "supported" : "NOT supported");

	return true;
}

bool GSDeviceVK::CreateAllocator()
{
	VmaAllocatorCreateInfo ci = {};
	ci.vulkanApiVersion = VK_API_VERSION_1_1;
	ci.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
	ci.physicalDevice = m_physical_device;
	ci.device = m_device;
	ci.instance = m_instance;

	if (m_optional_extensions.vk_ext_memory_budget)
		ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

	// Limit usage of the DEVICE_LOCAL upload heap when we're using a debug device.
	// On NVIDIA drivers, it results in frequently running out of device memory when trying to
	// play back captures in RenderDoc, making life very painful. Re-BAR GPUs should be fine.
	constexpr VkDeviceSize UPLOAD_HEAP_SIZE_THRESHOLD = 512 * 1024 * 1024;
	constexpr VkMemoryPropertyFlags UPLOAD_HEAP_PROPERTIES =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_size_limits;
	if (GSConfig.UseDebugDevice)
	{
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memory_properties);

		bool has_upload_heap = false;
		heap_size_limits.fill(VK_WHOLE_SIZE);
		for (u32 i = 0; i < memory_properties.memoryTypeCount; i++)
		{
			// Look for any memory types which are upload-like.
			const VkMemoryType& type = memory_properties.memoryTypes[i];
			if ((type.propertyFlags & UPLOAD_HEAP_PROPERTIES) != UPLOAD_HEAP_PROPERTIES)
				continue;

			const VkMemoryHeap& heap = memory_properties.memoryHeaps[type.heapIndex];
			if (heap.size >= UPLOAD_HEAP_SIZE_THRESHOLD)
				continue;

			if (heap_size_limits[type.heapIndex] == VK_WHOLE_SIZE)
			{
				Console.Warning("VK: Disabling allocation from upload heap #%u (%.2f MB) due to debug device.",
					type.heapIndex, static_cast<float>(heap.size) / 1048576.0f);
				heap_size_limits[type.heapIndex] = 0;
				has_upload_heap = true;
			}
		}

		if (has_upload_heap)
			ci.pHeapSizeLimit = heap_size_limits.data();
	}

	VkResult res = vmaCreateAllocator(&ci, &m_allocator);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateAllocator failed: ");
		return false;
	}

	return true;
}

bool GSDeviceVK::CreateCommandBuffers()
{
	VkResult res;

	uint32_t frame_index = 0;
	for (FrameResources& resources : m_frame_resources)
	{
		resources.needs_fence_wait = false;

		VkCommandPoolCreateInfo pool_info = {
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, m_graphics_queue_family_index};
		res = vkCreateCommandPool(m_device, &pool_info, nullptr, &resources.command_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateCommandPool failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, resources.command_pool, "Frame Command Pool %u", frame_index);

		VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
			resources.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			static_cast<u32>(resources.command_buffers.size())};

		res = vkAllocateCommandBuffers(m_device, &buffer_info, resources.command_buffers.data());
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
			return false;
		}
		for (u32 i = 0; i < resources.command_buffers.size(); i++)
		{
			Vulkan::SetObjectName(m_device, resources.command_buffers[i], "Frame %u %sCommand Buffer", frame_index,
				(i == 0) ? "Init" : "");
		}

		VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

		res = vkCreateFence(m_device, &fence_info, nullptr, &resources.fence);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateFence failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, resources.fence, "Frame Fence %u", frame_index);

		// The frame's descriptor pools are NOT created here. They are a chain grown on demand by
		// AllocateDescriptorSetFromFramePool, so a device that never allocates from it never has
		// one, and a device that needs a thousand sets gets a thousand sets.

		++frame_index;
	}

	// The out-of-band buffer: its own pool (reset per use, so the buffer is re-recorded
	// rather than re-allocated) and its own fence, created signalled like the frame ones.
	{
		VkCommandPoolCreateInfo pool_info = {
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, m_graphics_queue_family_index};
		res = vkCreateCommandPool(m_device, &pool_info, nullptr, &m_oob.command_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateCommandPool (out-of-band) failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, m_oob.command_pool, "Out-of-band Command Pool");

		VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
			m_oob.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1u};
		res = vkAllocateCommandBuffers(m_device, &buffer_info, &m_oob.command_buffer);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers (out-of-band) failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, m_oob.command_buffer, "Out-of-band Command Buffer");

		VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
		res = vkCreateFence(m_device, &fence_info, nullptr, &m_oob.fence);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateFence (out-of-band) failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, m_oob.fence, "Out-of-band Fence");
	}

	ActivateCommandBuffer(0);
	return true;
}

VkCommandBuffer GSDeviceVK::BeginOutOfBandCommandBuffer()
{
	if (m_oob.command_buffer == VK_NULL_HANDLE || m_oob.recording || m_last_submit_failed)
		return VK_NULL_HANDLE;

	// The previous out-of-band submission was waited for before it returned, so the pool
	// is free to reset.
	VkResult res = vkResetCommandPool(m_device, m_oob.command_pool, 0);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkResetCommandPool (out-of-band) failed: ");
		return VK_NULL_HANDLE;
	}

	const VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	res = vkBeginCommandBuffer(m_oob.command_buffer, &begin_info);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer (out-of-band) failed: ");
		return VK_NULL_HANDLE;
	}

	m_oob.recording = true;
	return m_oob.command_buffer;
}

bool GSDeviceVK::SubmitOutOfBandAndWait()
{
	pxAssert(m_oob.recording);
	m_oob.recording = false;

	// An out-of-band round trip is a readback too. m_readback_frame deliberately does not learn
	// that -- it is Classic's and exact-Tile's window and this must not move their kick cadence --
	// but the TileGpu kick's window must, or a kick that succeeds in moving every pull onto this
	// road starves the window that armed it. See m_tilegpu_readback_frame.
	m_tilegpu_readback_frame = m_frame;

	VkResult res = vkEndCommandBuffer(m_oob.command_buffer);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEndCommandBuffer (out-of-band) failed: ");
		return false;
	}

	res = vkResetFences(m_device, 1, &m_oob.fence);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkResetFences (out-of-band) failed: ");
		return false;
	}

	// No semaphores: this work depends on nothing unsubmitted (the caller's contract) and
	// nothing waits on it but the host, right here.
	const VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0u, nullptr, nullptr, 1u,
		&m_oob.command_buffer, 0u, nullptr};
	res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, m_oob.fence);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkQueueSubmit (out-of-band) failed: ");
		if (res == VK_ERROR_DEVICE_LOST)
			ReportDeviceFault();
		m_last_submit_failed = true;
		return false;
	}

	// Timed: this wait is deliberately OUTSIDE the drain accounting (the whole
	// point of the out-of-band road is not draining the frame's buffer), which
	// once made it a census blind spot — a regression built entirely of these
	// waits showed falling drains and falling command-buffer waits.
	const u64 oob_t0 = Common::Timer::GetCurrentValue();
	res = vkWaitForFences(m_device, 1, &m_oob.fence, VK_TRUE, UINT64_MAX);
	m_oob_wait_ns += Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - oob_t0);
	m_oob_wait_calls++;
	// Out of the drain accounting, but squarely inside the blocking-wait one: queue submissions
	// retire in order, so waiting on this fence waits on everything already submitted. The road's
	// name says "does not drain the frame's BUFFER", which is true and is not the same claim as
	// "does not drain the pipeline".
	g_perfmon.Put(GSPerfMon::GpuBlockingWaits, 1);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkWaitForFences (out-of-band) failed: ");
		if (res == VK_ERROR_DEVICE_LOST)
			ReportDeviceFault();
		m_last_submit_failed = true;
		return false;
	}
	return true;
}

bool GSDeviceVK::CreateGlobalDescriptorPool()
{
	static constexpr const VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}, // TFX vs_expand(2) + spin(1) + TileGpu state table(1) + VRAM(1)
	};

	VkDescriptorPoolCreateInfo pool_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
		VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		1024, // TODO: tweak this
		static_cast<u32>(std::size(pool_sizes)), pool_sizes};

	VkResult res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &m_global_descriptor_pool);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
		return false;
	}
	Vulkan::SetObjectName(m_device, m_global_descriptor_pool, "Global Descriptor Pool");

	if (m_gpu_timing_supported)
	{
		const VkQueryPoolCreateInfo query_create_info = {
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, NUM_COMMAND_BUFFERS * 4, 0};
		res = vkCreateQueryPool(m_device, &query_create_info, nullptr, &m_timestamp_query_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
			m_gpu_timing_supported = false;
			return false;
		}
	}

	if (m_gpu_pipeline_statistics_supported)
	{
		const VkQueryPoolCreateInfo query_create_info = {
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_PIPELINE_STATISTICS, NUM_COMMAND_BUFFERS,
			VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
		res = vkCreateQueryPool(m_device, &query_create_info, nullptr, &m_pipeline_statistics_query_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
			m_gpu_pipeline_statistics_supported = false;
			return false;
		}
	}

	return true;
}

VkRenderPass GSDeviceVK::GetRenderPass(VkFormat color_format, VkFormat depth_format, VkAttachmentLoadOp color_load_op,
	VkAttachmentStoreOp color_store_op, VkAttachmentLoadOp depth_load_op, VkAttachmentStoreOp depth_store_op,
	VkAttachmentLoadOp stencil_load_op, VkAttachmentStoreOp stencil_store_op, bool color_feedback_loop,
	bool depth_sampling, bool tilegpu_self_read)
{
	RenderPassCacheKey key = {};
	key.color_format = color_format;
	key.depth_format = depth_format;
	key.color_load_op = color_load_op;
	key.color_store_op = color_store_op;
	key.depth_load_op = depth_load_op;
	key.depth_store_op = depth_store_op;
	key.stencil_load_op = stencil_load_op;
	key.stencil_store_op = stencil_store_op;
	key.color_feedback_loop = color_feedback_loop;
	key.depth_sampling = depth_sampling;
	key.tilegpu_self_read = tilegpu_self_read;

	// Mali driver bug (ported from PPSSPP): a packed depth/stencil attachment whose
	// depth vs stencil load-ops MISMATCH corrupts on ARM Mali. PCSX2's GS uses one
	// combined D24S8/D32S8 attachment whose aspects are normally loaded/cleared
	// together, so this is a no-op in practice — normalize defensively (prefer the
	// depth aspect's op) so a stray mismatch can't trip the bug. Mali-only.
	if (IsDeviceMali() && key.stencil_load_op != key.depth_load_op)
	{
		key.stencil_load_op = key.depth_load_op;
		key.stencil_store_op = key.depth_store_op;
	}

	auto it = m_render_pass_cache.find(key.key);
	if (it != m_render_pass_cache.end())
		return it->second;

	return CreateCachedRenderPass(key);
}

VkRenderPass GSDeviceVK::GetRenderPassForRestarting(VkRenderPass pass)
{
	for (const auto& it : m_render_pass_cache)
	{
		if (it.second != pass)
			continue;

		RenderPassCacheKey modified_key;
		modified_key.key = it.first;
		if (modified_key.color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (modified_key.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (modified_key.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

		if (modified_key.key == it.first)
			return pass;

		auto fit = m_render_pass_cache.find(modified_key.key);
		if (fit != m_render_pass_cache.end())
			return fit->second;

		return CreateCachedRenderPass(modified_key);
	}

	return pass;
}

VkExtent2D GSDeviceVK::GetRenderAreaGranularity(VkRenderPass pass)
{
	const auto it = m_render_area_granularity.find(pass);
	if (it != m_render_area_granularity.end())
		return it->second;

	VkExtent2D granularity = {1, 1};
	vkGetRenderAreaGranularity(m_device, pass, &granularity);
	// An implementation may not return zero, but a zero would divide the caller's round-out.
	granularity.width = std::max<u32>(granularity.width, 1);
	granularity.height = std::max<u32>(granularity.height, 1);
	m_render_area_granularity.emplace(pass, granularity);
	return granularity;
}

VkCommandBuffer GSDeviceVK::GetCurrentInitCommandBuffer()
{
	FrameResources& res = m_frame_resources[m_current_frame];
	VkCommandBuffer buf = res.command_buffers[0];
	if (res.init_buffer_used)
		return buf;

	VkCommandBufferBeginInfo bi{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	vkBeginCommandBuffer(buf, &bi);
	res.init_buffer_used = true;
	return buf;
}

VkDescriptorSet GSDeviceVK::AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout)
{
	VkDescriptorSetAllocateInfo allocate_info = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_global_descriptor_pool, 1, &set_layout};

	VkDescriptorSet descriptor_set;
	VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
	if (res != VK_SUCCESS)
		return VK_NULL_HANDLE;

	return descriptor_set;
}

void GSDeviceVK::FreePersistentDescriptorSet(VkDescriptorSet set)
{
	vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &set);
}

// One link of a frame's descriptor-pool chain. The size is deliberately a slice of a frame rather
// than a frame's worst case: the chain grows on demand, so this constant decides how many links a
// heavy frame ends up holding, never whether that frame renders. It is small enough that ordinary
// corpus dumps grow the chain, and that is the point -- a growth path that only runs on the one
// title nobody renders locally is a path nobody has tested. Raising it is a memory/allocation
// trade, never a correctness fix.
static constexpr u32 FRAME_DESCRIPTOR_POOL_CHUNK_SETS = 256;

VkDescriptorPool GSDeviceVK::CreateFrameDescriptorPool()
{
	// Per-set budgets: the largest single set of each type that a layout allocated from this pool
	// declares, so an EMPTY link can serve one set of any of them. Undercount a type and that
	// layout is not slow, it is unservable -- every allocation of it fails, on every link, forever.
	//
	//   storage buffer: the TileGpu writeback set declares TWO -- the ring as words and the same
	//     buffer as quads, which is how its whole-word arm spells a 16-byte store -- and this pool
	//     once reserved none, so on the non-push path every writeback compute dispatch went unbound
	//     and its pages were never composed. The call site read the failure as "exhaustion
	//     implausible, skip it". Undercounting here is that failure again, not a slowdown.
	//   sampled image:  a TFX texture set declares four of them (palette, primid, and the colour
	//     and depth feedback bindings wherever those are not input attachments), not three.
	const u32 sets = FRAME_DESCRIPTOR_POOL_CHUNK_SETS;
	const VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * (GSTileGpuPassPlan::kMaxTexSourcesPerPass + 2)},
		{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets * 4},
		{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, sets * 2},
		{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets * 2},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2},
	};
	const VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, sets,
		static_cast<u32>(std::size(pool_sizes)), pool_sizes};

	VkDescriptorPool pool = VK_NULL_HANDLE;
	const VkResult res = vkCreateDescriptorPool(m_device, &pool_info, nullptr, &pool);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool (frame) failed: ");
		return VK_NULL_HANDLE;
	}
	return pool;
}

VkDescriptorSet GSDeviceVK::AllocateDescriptorSetFromFramePool(VkDescriptorSetLayout set_layout)
{
	FrameResources& resources = m_frame_resources[m_current_frame];

	// Walk the frame's chain, appending a link when every existing one is full. A fixed pool would
	// make the frame's set count a cliff -- and a cliff whose height is device-dependent, because
	// what allocates from here (per-pass sets on the push path, every texture set on the non-push
	// one) is decided by device features. So the frame gets as many links as it turns out to need;
	// they live for the device's lifetime and are reset, not freed, when the frame is recycled, so
	// the cost is paid once by the first heavy frame.
	for (;;)
	{
		// A link is full once it has served the sets it was created for -- our count, not the
		// driver's answer. A driver may serve past its pool's maxSets and one we test on does,
		// handing out thousands of sets from a pool created for one; waiting to be refused would
		// leave this path dead on every device we can run here and live only on the ones we ship
		// to, which is the whole reason the fixed pool's ceiling went unnoticed.
		if (resources.descriptor_pool_cursor < resources.descriptor_pools.size() &&
			resources.descriptor_pool_cursor_sets >= FRAME_DESCRIPTOR_POOL_CHUNK_SETS)
		{
			resources.descriptor_pool_cursor++;
			resources.descriptor_pool_cursor_sets = 0;
		}

		if (resources.descriptor_pool_cursor >= resources.descriptor_pools.size())
		{
			const VkDescriptorPool pool = CreateFrameDescriptorPool();
			if (pool == VK_NULL_HANDLE)
				return VK_NULL_HANDLE;

			resources.descriptor_pools.push_back(pool);
			resources.descriptor_pool_cursor_sets = 0;
			Vulkan::SetObjectName(m_device, pool, "Frame Descriptor Pool %u.%u", m_current_frame,
				static_cast<u32>(resources.descriptor_pools.size() - 1));
			Console.WriteLn("VK: frame %u descriptor pool chain grew to %u pools of %u sets.", m_current_frame,
				static_cast<u32>(resources.descriptor_pools.size()), FRAME_DESCRIPTOR_POOL_CHUNK_SETS);
		}

		const VkDescriptorSetAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
			resources.descriptor_pools[resources.descriptor_pool_cursor], 1, &set_layout};

		VkDescriptorSet descriptor_set;
		const VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
		if (res == VK_SUCCESS)
		{
			resources.descriptor_pool_cursor_sets++;
			return descriptor_set;
		}

		if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL)
		{
			// Out of host or device memory. Nothing to grow into; the caller's loud road is the
			// honest answer.
			LOG_VULKAN_ERROR(res, "vkAllocateDescriptorSets (frame) failed: ");
			return VK_NULL_HANDLE;
		}

		if (resources.descriptor_pool_cursor_sets == 0)
		{
			// A link that has served nothing since it was reset just refused this layout, so no
			// link of this shape ever will -- growing for it would append pools forever. The pool
			// shape in CreateFrameDescriptorPool is missing a descriptor type the layout declares.
			if (!m_frame_pool_layout_refused_warned)
			{
				m_frame_pool_layout_refused_warned = true;
				Console.Error("VK: an empty frame descriptor pool cannot serve a descriptor set layout -- the pool "
							  "reserves no descriptors of some type the layout declares.");
			}
			return VK_NULL_HANDLE;
		}

		resources.descriptor_pool_cursor++;
		resources.descriptor_pool_cursor_sets = 0;
	}
}

void GSDeviceVK::WaitForFenceCounter(u64 fence_counter, GpuWaitCause cause)
{
	if (m_completed_fence_counter >= fence_counter)
		return;

	// Find the first command buffer which covers this counter value.
	u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
	while (index != m_current_frame)
	{
		if (m_frame_resources[index].fence_counter >= fence_counter)
			break;

		index = (index + 1) % NUM_COMMAND_BUFFERS;
	}

	pxAssert(index != m_current_frame);
	WaitForCommandBufferCompletion(index, cause);
}

void GSDeviceVK::WaitForGPUIdle()
{
	const u64 t0 = Common::Timer::GetCurrentValue();
	vkDeviceWaitIdle(m_device);
	m_sync_wait_ns += Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - t0);
	m_sync_wait_calls++;
	g_perfmon.Put(GSPerfMon::GpuBlockingWaits, 1);
}

float GSDeviceVK::GetAndResetAccumulatedGPUTime()
{
	const float time = m_accumulated_gpu_time;
	m_accumulated_gpu_time = 0.0f;
	return time;
}

bool GSDeviceVK::SetGPUTimingEnabled(bool enabled)
{
	m_gpu_timing_enabled = enabled && m_gpu_timing_supported;
	return (enabled == m_gpu_timing_enabled);
}

GPUPipelineStatistics GSDeviceVK::GetAndResetAccumulatedGPUPipelineStatistics()
{
	GPUPipelineStatistics stats = m_accumulated_gpu_pipeline_statistics;
	m_accumulated_gpu_pipeline_statistics = {};
	return stats;
}

bool GSDeviceVK::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	m_gpu_pipeline_statistics_enabled = enabled && m_gpu_pipeline_statistics_supported;
	return (enabled == m_gpu_pipeline_statistics_enabled);
}

void GSDeviceVK::EnableExtendedStats(bool enabled)
{
	VKSwapChain::SetPresentStatsEnabled(enabled);
}

std::vector<std::string> GSDeviceVK::GetExtendedStats() const
{
	std::vector<std::string> lines;
	if (m_swap_chain)
	{
		const WindowInfo& wi = m_swap_chain->GetWindowInfo();
		const char* wsi_name = "?";
		switch (wi.type)
		{
			case WindowInfo::Type::Surfaceless: wsi_name = "Surfaceless"; break;
			case WindowInfo::Type::Win32:       wsi_name = "Win32"; break;
			case WindowInfo::Type::X11:         wsi_name = "X11"; break;
			case WindowInfo::Type::Wayland:     wsi_name = "Wayland"; break;
			case WindowInfo::Type::MacOS:       wsi_name = "MacOS"; break;
			case WindowInfo::Type::VulkanDirect: wsi_name = "VulkanDirect"; break;
		}
		const char* present_name = "?";
		switch (m_swap_chain->GetPresentMode())
		{
			case VK_PRESENT_MODE_IMMEDIATE_KHR:    present_name = "IMMEDIATE"; break;
			case VK_PRESENT_MODE_MAILBOX_KHR:      present_name = "MAILBOX"; break;
			case VK_PRESENT_MODE_FIFO_KHR:         present_name = "FIFO"; break;
			case VK_PRESENT_MODE_FIFO_RELAXED_KHR: present_name = "FIFO_RELAXED"; break;
			default: break;
		}
		lines.push_back(fmt::format(
			"Swapchain: {}x{} (scale {:.2f}) fmt={} present={} images={} wsi={}",
			m_swap_chain->GetWidth(), m_swap_chain->GetHeight(), wi.surface_scale,
			static_cast<unsigned>(m_swap_chain->GetTextureFormat()),
			present_name, m_swap_chain->GetImageCount(), wsi_name));
	}

	const VKSwapChain::PresentStats ps = VKSwapChain::GetPresentStats();
	const double acquire_avg_ms = ps.acquire_count ? (ps.acquire_total_ms / ps.acquire_count) : 0.0;
	const double present_avg_ms = ps.present_count ? (ps.present_total_ms / ps.present_count) : 0.0;
	lines.push_back(fmt::format(
		"vkAcquireNextImage: avg {:.3f} ms, max {:.3f} ms, n={}", acquire_avg_ms, ps.acquire_max_ms, ps.acquire_count));
	lines.push_back(fmt::format(
		"vkQueuePresent:     avg {:.3f} ms, max {:.3f} ms, n={}", present_avg_ms, ps.present_max_ms, ps.present_count));
	lines.push_back(fmt::format(
		"Suboptimal: {}, OutOfDate: {}", ps.suboptimal_count, ps.out_of_date_count));
	return lines;
}

void GSDeviceVK::ScanForCommandBufferCompletion()
{
	for (u32 check_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS; check_index != m_current_frame;
	     check_index = (check_index + 1) % NUM_COMMAND_BUFFERS)
	{
		FrameResources& resources = m_frame_resources[check_index];
		if (resources.fence_counter <= m_completed_fence_counter)
			continue; // Already completed
		if (vkGetFenceStatus(m_device, resources.fence) != VK_SUCCESS)
			break; // Fence not signaled, later fences won't be either
		CommandBufferCompleted(check_index);
		m_completed_fence_counter = resources.fence_counter;
	}
	for (SpinResources& resources : m_spin_resources)
	{
		if (!resources.in_progress)
			continue;
		if (vkGetFenceStatus(m_device, resources.fence) != VK_SUCCESS)
			continue;
		SpinCommandCompleted(&resources - &m_spin_resources[0]);
	}
}

// VK_EXT_device_fault post-mortem, called on VK_ERROR_DEVICE_LOST before the
// deliberate exit: the structured fault records — page fault addresses, their
// kind, vendor fault codes — are the difference between "the driver died,
// ~1-in-60" and a diagnosis. The vendor binary blob is deliberately not pulled
// (it can be megabytes and needs vendor tooling to read); the structured
// records are what a human acts on.
void GSDeviceVK::ReportDeviceFault()
{
	if (!m_optional_extensions.vk_ext_device_fault || !vkGetDeviceFaultInfoEXT)
		return;

	VkDeviceFaultCountsEXT counts = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
	if (vkGetDeviceFaultInfoEXT(m_device, &counts, nullptr) != VK_SUCCESS)
	{
		Console.Error("VK: device fault info counts query failed.");
		return;
	}

	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
	counts.vendorBinarySize = 0;
	VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
	info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
	if (vkGetDeviceFaultInfoEXT(m_device, &counts, &info) < VK_SUCCESS)
	{
		Console.Error("VK: device fault info query failed.");
		return;
	}

	Console.Error("VK: DEVICE LOST — driver fault report: \"%s\" (%u address record(s), %u vendor record(s))",
		info.description, counts.addressInfoCount, counts.vendorInfoCount);
	static constexpr const char* kAddressTypes[] = {"none", "read-invalid", "write-invalid", "execute-invalid",
		"instruction-pointer-unknown", "instruction-pointer-invalid", "instruction-pointer-fault"};
	for (u32 i = 0; i < counts.addressInfoCount; i++)
	{
		const VkDeviceFaultAddressInfoEXT& a = addresses[i];
		const u32 type = static_cast<u32>(a.addressType);
		Console.Error("VK:   address fault %u: %s at 0x%016llx (precision 0x%llx)", i,
			(type < std::size(kAddressTypes)) ? kAddressTypes[type] : "unknown-type",
			static_cast<unsigned long long>(a.reportedAddress), static_cast<unsigned long long>(a.addressPrecision));
	}
	for (u32 i = 0; i < counts.vendorInfoCount; i++)
	{
		const VkDeviceFaultVendorInfoEXT& v = vendors[i];
		Console.Error("VK:   vendor fault %u: \"%s\" code 0x%llx data 0x%llx", i, v.description,
			static_cast<unsigned long long>(v.vendorFaultCode), static_cast<unsigned long long>(v.vendorFaultData));
	}
}

void GSDeviceVK::WaitForCommandBufferCompletion(u32 index, GpuWaitCause cause)
{
	// Wait for this command buffer to be completed. Timed and attributed: a Sync wait is the GS
	// thread stalling out of turn, which serializes the whole frame whatever it carries, and it
	// was previously indistinguishable from the ring's own recycle wait in every counter we had.
	const u64 wait_t0 = Common::Timer::GetCurrentValue();
	const VkResult res = vkWaitForFences(m_device, 1, &m_frame_resources[index].fence, VK_TRUE, UINT64_MAX);
	const u64 wait_ns = Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - wait_t0);
	// Ring is backpressure and stays out of the blocking-wait population; the other two are in it.
	// SourceSet is a bucket of its own INSIDE that population, not a way out of it -- see
	// GpuWaitCause for why a recycling wait is counted with the drains rather than with the ring.
	switch (cause)
	{
		case GpuWaitCause::Sync:
			m_sync_wait_ns += wait_ns;
			m_sync_wait_calls++;
			g_perfmon.Put(GSPerfMon::GpuBlockingWaits, 1);
			break;
		case GpuWaitCause::SourceSet:
			m_source_set_wait_ns += wait_ns;
			m_source_set_wait_calls++;
			g_perfmon.Put(GSPerfMon::GpuBlockingWaits, 1);
			break;
		case GpuWaitCause::Ring:
			m_ring_wait_ns += wait_ns;
			m_ring_wait_calls++;
			break;
	}
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkWaitForFences failed: ");
		if (res == VK_ERROR_DEVICE_LOST)
			ReportDeviceFault();
		m_last_submit_failed = true;
		return;
	}

	// Clean up any resources for command buffers between the last known completed buffer and this
	// now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
	const u64 now_completed_counter = m_frame_resources[index].fence_counter;
	u32 cleanup_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
	while (cleanup_index != m_current_frame)
	{
		FrameResources& resources = m_frame_resources[cleanup_index];
		if (resources.fence_counter > now_completed_counter)
			break;

		if (resources.fence_counter > m_completed_fence_counter)
			CommandBufferCompleted(cleanup_index);

		cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
	}

	m_completed_fence_counter = now_completed_counter;
}

void GSDeviceVK::SubmitCommandBuffer(VKSwapChain* present_swap_chain)
{
	m_render_passes_since_submit = 0;

	FrameResources& resources = m_frame_resources[m_current_frame];

	// End the current command buffer.
	VkResult res;
	if (resources.init_buffer_used)
	{
		res = vkEndCommandBuffer(resources.command_buffers[0]);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
			pxFailRel("Failed to end command buffer");
		}
	}

	bool wants_timestamp = m_gpu_timing_enabled || m_spin_timer;
	if (wants_timestamp && resources.timestamp_written)
	{
		vkCmdWriteTimestamp(m_current_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool,
			m_current_frame * 2 + 1);
	}

	if (resources.pipeline_statistics_query == QueryState::Querying)
	{
		// Didn't end query in DoBeginPresent() so end it here.
		resources.pipeline_statistics_query = QueryState::Ready;
		vkCmdEndQuery(m_current_command_buffer, m_pipeline_statistics_query_pool, m_current_frame);
	}

	res = vkEndCommandBuffer(resources.command_buffers[1]);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
		pxFailRel("Failed to end command buffer");
	}

	// This command buffer now has commands, so can't be re-used without waiting.
	resources.needs_fence_wait = true;

	u32 spin_cycles = 0;
	const bool spin_enabled = m_spin_timer;
	if (spin_enabled)
	{
		ScanForCommandBufferCompletion();
		auto draw = m_spin_manager.DrawSubmitted(m_command_buffer_render_passes);
		u32 constant_offset =
			400000 * m_spin_manager.SpinsPerUnitTime(); // 400us, just to be safe since going over gets really bad
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
			constant_offset /=
				2; // Safety factor isn't as important here, going over just hurts this one submission a bit
		u32 minimum_spin = 200000 * m_spin_manager.SpinsPerUnitTime();
		u32 maximum_spin = std::max<u32>(1024, 16000000 * m_spin_manager.SpinsPerUnitTime()); // 16ms
		if (draw.recommended_spin > minimum_spin + constant_offset)
			spin_cycles = std::min(draw.recommended_spin - constant_offset, maximum_spin);
		resources.spin_id = draw.id;
	}
	else
	{
		resources.spin_id = -1;
	}
	m_command_buffer_render_passes = 0;

	if (present_swap_chain != VK_NULL_HANDLE && m_spinning_supported)
	{
		m_spin_manager.NextFrame();
		if (m_spin_timer)
			m_spin_timer--;
		// Calibrate a max of once per frame
		m_wants_new_timestamp_calibration = m_optional_extensions.vk_ext_calibrated_timestamps;
	}

	if (spin_cycles != 0)
		WaitForSpinCompletion(m_current_frame);

	if (spin_enabled && m_optional_extensions.vk_ext_calibrated_timestamps)
		resources.submit_timestamp = GetCPUTimestamp();

	uint32_t wait_bits = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSemaphore semas[2];
	VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit_info.commandBufferCount = resources.init_buffer_used ? 2u : 1u;
	submit_info.pCommandBuffers =
		resources.init_buffer_used ? resources.command_buffers.data() : &resources.command_buffers[1];

	if (present_swap_chain)
	{
		submit_info.pWaitSemaphores = present_swap_chain->GetImageAvailableSemaphorePtr();
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitDstStageMask = &wait_bits;

		if (spin_cycles != 0)
		{
			semas[0] = present_swap_chain->GetRenderingFinishedSemaphore();
			semas[1] = m_spin_resources[m_current_frame].semaphore;
			submit_info.signalSemaphoreCount = 2;
			submit_info.pSignalSemaphores = semas;
		}
		else
		{
			submit_info.pSignalSemaphores = present_swap_chain->GetRenderingFinishedSemaphorePtr();
			submit_info.signalSemaphoreCount = 1;
		}
	}
	else if (spin_cycles != 0)
	{
		submit_info.signalSemaphoreCount = 1;
		submit_info.pSignalSemaphores = &m_spin_resources[m_current_frame].semaphore;
	}

	res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkQueueSubmit failed: ");
		if (res == VK_ERROR_DEVICE_LOST)
			ReportDeviceFault();
		m_last_submit_failed = true;
		return;
	}

	if (spin_cycles != 0)
		SubmitSpinCommand(m_current_frame, spin_cycles);

	if (present_swap_chain)
	{
		// Consumed here rather than only reset in BeginPresent: RenderBlankFrame() presents
		// without going through BeginPresent at all, so a flag left set by the last real frame
		// would tell frame generation that a cleared image was fresh game output.
		const bool has_new_frame = std::exchange(m_present_has_new_frame, false);

		// Frame generation replaces this present entirely: it consumes the rendering-finished
		// semaphore for its own copy, then presents the interpolated frames and the real one in
		// order. It returns false without consuming anything if it cannot run this frame, which
		// is the ordinary path on every build and device without it.
		if (GSLsfg::IsActive() &&
			GSLsfg::PresentWithGeneration(m_present_queue, present_swap_chain,
				present_swap_chain->GetRenderingFinishedSemaphore(), has_new_frame))
		{
			present_swap_chain->AcquireNextImage();
			return;
		}

		const VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1,
			present_swap_chain->GetRenderingFinishedSemaphorePtr(), 1, present_swap_chain->GetSwapChainPtr(),
			present_swap_chain->GetCurrentImageIndexPtr(), nullptr};

		present_swap_chain->ResetImageAcquireResult();

		const bool stats = VKSwapChain::IsPresentStatsEnabled();
		const Common::Timer::Value t_present_start = stats ? Common::Timer::GetCurrentValue() : 0;
		res = vkQueuePresentKHR(m_present_queue, &present_info);
		if (stats)
		{
			const double present_elapsed_ms =
				Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - t_present_start);
			VKSwapChain::NotePresent(present_elapsed_ms, res);
		}
		if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		{
			// VK_ERROR_OUT_OF_DATE_KHR is not fatal, just means we need to recreate our swap chain.
			if (res == VK_ERROR_OUT_OF_DATE_KHR)
				// Defer until next frame, otherwise resizing would invalidate swapchain before next present.
				m_resize_requested = true;
			else
				LOG_VULKAN_ERROR(res, "vkQueuePresentKHR failed: ");

			return;
		}

		// Grab the next image as soon as possible, that way we spend less time blocked on the next
		// submission. Don't care if it fails, we'll deal with that at the presentation call site.
		// Credit to dxvk for the idea.
		present_swap_chain->AcquireNextImage();
	}
}

void GSDeviceVK::CommandBufferCompleted(u32 index)
{
	FrameResources& resources = m_frame_resources[index];

	for (auto& it : resources.cleanup_resources)
		it();
	resources.cleanup_resources.clear();

	bool wants_timestamps = m_gpu_timing_enabled || resources.spin_id >= 0;

	if (wants_timestamps && resources.timestamp_written)
	{
		std::array<u64, 2> timestamps;
		VkResult res =
			vkGetQueryPoolResults(m_device, m_timestamp_query_pool, index * 2, static_cast<u32>(timestamps.size()),
				sizeof(u64) * timestamps.size(), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
		if (res == VK_SUCCESS)
		{
			// if we didn't write the timestamp at the start of the cmdbuffer (just enabled timing), the first TS will be zero
			if (timestamps[0] > 0 && m_gpu_timing_enabled)
			{
				const double ns_diff =
					(timestamps[1] - timestamps[0]) * static_cast<double>(m_device_properties.limits.timestampPeriod);
				m_accumulated_gpu_time += ns_diff / 1000000.0;
			}
			if (resources.spin_id >= 0)
			{
				if (m_optional_extensions.vk_ext_calibrated_timestamps && timestamps[1] > 0)
				{
					u64 end = timestamps[1] * m_spin_timestamp_scale + m_spin_timestamp_offset;
					m_spin_manager.DrawCompleted(resources.spin_id, resources.submit_timestamp, end);
				}
				else if (!m_optional_extensions.vk_ext_calibrated_timestamps && timestamps[0] > 0)
				{
					u64 begin = timestamps[0] * m_spin_timestamp_scale;
					u64 end = timestamps[1] * m_spin_timestamp_scale;
					m_spin_manager.DrawCompleted(resources.spin_id, begin, end);
				}
			}
		}
		else
		{
			LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");
		}
	}
}

void GSDeviceVK::MoveToNextCommandBuffer()
{
	ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
}

void GSDeviceVK::ActivateCommandBuffer(u32 index)
{
	FrameResources& resources = m_frame_resources[index];

	// Wait for the GPU to finish with all resources for this command buffer. This one is the ring
	// coming round — the pipeline is full — not a stall anybody asked for.
	if (resources.fence_counter > m_completed_fence_counter)
		WaitForCommandBufferCompletion(index, GpuWaitCause::Ring);

	// Reset fence to unsignaled before starting.
	VkResult res = vkResetFences(m_device, 1, &resources.fence);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

	// Reset command pools to beginning since we can re-use the memory now
	res = vkResetCommandPool(m_device, resources.command_pool, 0);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

	// The GPU is done with this frame, so recycle its descriptor sets wholesale: reset every link
	// of the chain and allocate from the first again. Cheaper than per-set frees, matches the
	// command-pool lifecycle, and keeps the links the frame grew -- a heavy frame pays for its
	// chain once rather than on every pass through.
	for (const VkDescriptorPool pool : resources.descriptor_pools)
	{
		res = vkResetDescriptorPool(m_device, pool, 0);
		if (res != VK_SUCCESS)
			LOG_VULKAN_ERROR(res, "vkResetDescriptorPool failed: ");
	}
	resources.descriptor_pool_cursor = 0;
	resources.descriptor_pool_cursor_sets = 0;

	// Enable commands to be recorded to the two buffers again.
	// ONE_TIME_SUBMIT is load-bearing on Mali: without it (SIMULTANEOUS_USE) the blob switches
	// primary command buffers from direct backend dispatch to record-then-replay through an
	// internal arena, roughly doubling per-draw CPU. Every begin in this backend keeps this flag.
	VkCommandBufferBeginInfo begin_info = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	res = vkBeginCommandBuffer(resources.command_buffers[1], &begin_info);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");

	bool wants_timestamp = m_gpu_timing_enabled || m_spin_timer;
	if (wants_timestamp)
	{
		vkCmdResetQueryPool(resources.command_buffers[1], m_timestamp_query_pool, index * 2, 2);
		vkCmdWriteTimestamp(
			resources.command_buffers[1], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, index * 2);
	}

	if (resources.pipeline_statistics_query == QueryState::Ready)
	{
		// Collect the pipeline statistics from the last time this cmdbuffer was used.
		resources.pipeline_statistics_query = QueryState::None;
		GPUPipelineStatistics stats{};
		VkResult ps_res =
			vkGetQueryPoolResults(m_device, m_pipeline_statistics_query_pool, index, 1,
				sizeof(stats), &stats, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		if (ps_res == VK_SUCCESS)
		{
			m_accumulated_gpu_pipeline_statistics.vs_invocations += stats.vs_invocations;
			m_accumulated_gpu_pipeline_statistics.ps_invocations += stats.ps_invocations;
		}
		else
		{
			LOG_VULKAN_ERROR(ps_res, "vkGetQueryPoolResults failed: ");
		}
	}

	if (m_gpu_pipeline_statistics_enabled)
	{
		pxAssert(resources.pipeline_statistics_query == QueryState::None);
		resources.pipeline_statistics_query = QueryState::Querying;
		vkCmdResetQueryPool(resources.command_buffers[1], m_pipeline_statistics_query_pool, index, 1);
		vkCmdBeginQuery(resources.command_buffers[1], m_pipeline_statistics_query_pool, index, 0);
	}

	resources.fence_counter = m_next_fence_counter++;
	resources.init_buffer_used = false;
	resources.timestamp_written = wants_timestamp;

	m_current_frame = index;
	m_current_command_buffer = resources.command_buffers[1];

	// using the lower 32 bits of the fence index should be sufficient here, I hope...
	vmaSetCurrentFrameIndex(m_allocator, static_cast<u32>(m_next_fence_counter));
}

void GSDeviceVK::ExecuteCommandBuffer(WaitType wait_for_completion)
{
	if (m_last_submit_failed)
		return;

	const u32 current_frame = m_current_frame;
	SubmitCommandBuffer(nullptr);
	MoveToNextCommandBuffer();

	if (wait_for_completion != WaitType::None)
	{
		// Calibrate while we wait
		if (m_wants_new_timestamp_calibration)
			CalibrateSpinTimestamp();
		if (wait_for_completion == WaitType::Spin)
		{
			while (vkGetFenceStatus(m_device, m_frame_resources[current_frame].fence) == VK_NOT_READY)
				ShortSpin();
		}
		WaitForCommandBufferCompletion(current_frame);
	}

	// Push constants need to be refreshed each command buffer.
	m_dirty_flags |= DIRTY_FLAG_VS_PUSH_CONSTANTS;
}

void GSDeviceVK::DeferBufferDestruction(VkBuffer object, VmaAllocation allocation)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back(
		[this, object, allocation]() { vmaDestroyBuffer(m_allocator, object, allocation); });
}

void GSDeviceVK::DeferFramebufferDestruction(VkFramebuffer object)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back([this, object]() { vkDestroyFramebuffer(m_device, object, nullptr); });
}

void GSDeviceVK::DeferImageDestruction(VkImage object, VmaAllocation allocation)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back(
		[this, object, allocation]() { vmaDestroyImage(m_allocator, object, allocation); });
}

void GSDeviceVK::DeferImageViewDestruction(VkImageView object)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back([this, object]() { vkDestroyImageView(m_device, object, nullptr); });
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		Console.Error("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}
	else if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
	{
		Console.Warning("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
	{
		Console.WriteLn("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}
	else
	{
		DevCon.WriteLn("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}

	return VK_FALSE;
}

bool GSDeviceVK::EnableDebugUtils()
{
	// Already enabled?
	if (m_debug_messenger_callback != VK_NULL_HANDLE)
		return true;

	// Check for presence of the functions before calling
	if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT || !vkSubmitDebugUtilsMessageEXT)
	{
		return false;
	}

	VkDebugUtilsMessengerCreateInfoEXT messenger_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		nullptr, 0,
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		DebugMessengerCallback, nullptr};

	const VkResult res =
		vkCreateDebugUtilsMessengerEXT(m_instance, &messenger_info, nullptr, &m_debug_messenger_callback);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDebugUtilsMessengerEXT failed: ");
		return false;
	}

	return true;
}

void GSDeviceVK::DisableDebugUtils()
{
	if (m_debug_messenger_callback != VK_NULL_HANDLE)
	{
		vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger_callback, nullptr);
		m_debug_messenger_callback = VK_NULL_HANDLE;
	}
}

VkRenderPass GSDeviceVK::CreateCachedRenderPass(RenderPassCacheKey key)
{
	VkAttachmentReference color_reference;
	VkAttachmentReference* color_reference_ptr = nullptr;
	VkAttachmentReference depth_reference;
	VkAttachmentReference* depth_reference_ptr = nullptr;
	std::array<VkAttachmentReference, 2> input_reference;
	u32 num_subpass_inputs = 0;
	std::array<VkSubpassDependency, 3> subpass_dependency;
	u32 num_subpass_dependencies = 0;
	std::array<VkAttachmentDescription, 2> attachments;
	u32 num_attachments = 0;
	if (key.color_format != VK_FORMAT_UNDEFINED)
	{
		// The TileGpu declared read pins the shape rather than deriving it: GENERAL for the
		// attachment's whole life, referenced as colour AND input, the rasterization-order subpass
		// flag, and no dependency of any kind. That is the exact contract the ROAA correctness probe
		// carried through five formats and 63-draw chains on both device tiers; the alternatives it
		// tried alongside are the ones that silently drop content on Turnip.
		const VkImageLayout layout =
			key.tilegpu_self_read ?
				VK_IMAGE_LAYOUT_GENERAL :
				(key.color_feedback_loop ?
						(UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT :
												   VK_IMAGE_LAYOUT_GENERAL) :
						VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		attachments[num_attachments] = {0, static_cast<VkFormat>(key.color_format), VK_SAMPLE_COUNT_1_BIT,
			static_cast<VkAttachmentLoadOp>(key.color_load_op), static_cast<VkAttachmentStoreOp>(key.color_store_op),
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
		color_reference.attachment = num_attachments;
		color_reference.layout = layout;
		color_reference_ptr = &color_reference;

		if (key.tilegpu_self_read)
		{
			input_reference[num_subpass_inputs].attachment = num_attachments;
			input_reference[num_subpass_inputs].layout = layout;
			num_subpass_inputs++;
		}
		else if (key.color_feedback_loop)
		{
			if (!UseFeedbackLoopLayout())
			{
				input_reference[num_subpass_inputs].attachment = num_attachments;
				input_reference[num_subpass_inputs].layout = layout;
				num_subpass_inputs++;
			}

			if (!m_features.framebuffer_fetch)
			{
				// don't need the framebuffer-local dependency when we have rasterization order attachment access
				subpass_dependency[num_subpass_dependencies].srcSubpass = 0;
				subpass_dependency[num_subpass_dependencies].dstSubpass = 0;
				subpass_dependency[num_subpass_dependencies].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				subpass_dependency[num_subpass_dependencies].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				subpass_dependency[num_subpass_dependencies].srcAccessMask =
					VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				subpass_dependency[num_subpass_dependencies].dstAccessMask =
					UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
				subpass_dependency[num_subpass_dependencies].dependencyFlags =
					UseFeedbackLoopLayout() ? (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
											  VK_DEPENDENCY_BY_REGION_BIT;
				num_subpass_dependencies++;
			}
		}

		num_attachments++;
	}
	if (key.depth_format != VK_FORMAT_UNDEFINED)
	{
		const VkImageLayout layout =
			key.depth_sampling ?
				((m_features.depth_feedback && UseFeedbackLoopLayout()) ?
					VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT :
					VK_IMAGE_LAYOUT_GENERAL) :
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[num_attachments] = {0, static_cast<VkFormat>(key.depth_format), VK_SAMPLE_COUNT_1_BIT,
			static_cast<VkAttachmentLoadOp>(key.depth_load_op), static_cast<VkAttachmentStoreOp>(key.depth_store_op),
			static_cast<VkAttachmentLoadOp>(key.stencil_load_op),
			static_cast<VkAttachmentStoreOp>(key.stencil_store_op), layout, layout};
		depth_reference.attachment = num_attachments;
		depth_reference.layout = layout;
		depth_reference_ptr = &depth_reference;

		if (key.depth_sampling)
		{
			if (!UseFeedbackLoopLayout())
			{
				input_reference[num_subpass_inputs].attachment = num_attachments;
				input_reference[num_subpass_inputs].layout = layout;
				num_subpass_inputs++;
			}

			if (!m_features.framebuffer_fetch)
			{
				// don't need the framebuffer-local dependency when we have rasterization order attachment access
				subpass_dependency[num_subpass_dependencies].srcSubpass = 0;
				subpass_dependency[num_subpass_dependencies].dstSubpass = 0;
				subpass_dependency[num_subpass_dependencies].srcStageMask =
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				subpass_dependency[num_subpass_dependencies].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				subpass_dependency[num_subpass_dependencies].srcAccessMask =
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				subpass_dependency[num_subpass_dependencies].dstAccessMask =
					UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
				subpass_dependency[num_subpass_dependencies].dependencyFlags =
					UseFeedbackLoopLayout() ? (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
											  VK_DEPENDENCY_BY_REGION_BIT;
				num_subpass_dependencies++;
			}
		}

		num_attachments++;
	}

	// The incoming edge. A render pass that declares no dependency from VK_SUBPASS_EXTERNAL gets an
	// implicit one, and the implicit one's FIRST synchronization scope is srcStageMask =
	// TOP_OF_PIPE, srcAccessMask = 0 -- it performs no availability operation, so nothing recorded
	// before the pass is ordered ahead of the pass's attachment accesses.
	//
	// Almost everywhere that costs nothing, because the backend moves an attachment's LAYOUT before
	// it binds it and the transition barrier carries the real scopes. It costs where the layout does
	// NOT move: GSTextureVK::TransitionToLayout returns early when the layout is unchanged, so two
	// render passes in a row that use the same image as the same kind of attachment emit no barrier
	// of any kind, and the second pass's LOAD races the first pass's stores. TileGpu meets that on
	// every byte-road seed -- the seed pass fills the target's pages and the geometry pass it was
	// emitted for LOADs exactly those pages, ~94 times a frame on FlatOut 2 -- and the plain
	// pass-to-pass split of one target's draws has the same shape.
	//
	// So declare what the implicit dependency only pretends to be: its destination scope unchanged,
	// with a first scope that actually names the framebuffer-space stages the earlier pass wrote in.
	// BY_REGION keeps it framebuffer-LOCAL, which is the point on a tiler -- the ordering wanted is
	// per-tile (the seed writes pixel (x,y) and the pass loads pixel (x,y)), so an implementation is
	// still free to keep the attachment resident in tile memory across the two passes instead of
	// storing and reloading it. A blanket barrier between passes forbids exactly that.
	//
	// The outgoing (subpass -> EXTERNAL) direction is deliberately not declared: every consumer of
	// an attachment outside a render pass -- a compute sample, a transfer copy, a fragment sample --
	// reaches the image through a layout transition, and those carry their own scopes.
	if (num_attachments > 0)
	{
		VkSubpassDependency& dep = subpass_dependency[num_subpass_dependencies++];
		dep = {};
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.dstSubpass = 0;
		if (color_reference_ptr)
		{
			dep.srcStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dep.dstStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dep.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dep.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		}
		if (depth_reference_ptr)
		{
			constexpr VkPipelineStageFlags ds_stages =
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dep.srcStageMask |= ds_stages;
			dep.dstStageMask |= ds_stages;
			dep.srcAccessMask |=
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dep.dstAccessMask |=
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
		// subpassLoad reads the fragment's own coordinate, so this stays framebuffer-local too. A
		// SAMPLER read of the attachment (the feedback-loop-layout road) is VK_ACCESS_SHADER_READ,
		// not INPUT_ATTACHMENT_READ, and is deliberately absent here for that reason -- it was
		// absent from the implicit dependency as well, and its ordering comes from the layout
		// transition and the framebuffer-local self-dependency built above.
		if (num_subpass_inputs > 0)
		{
			dep.dstStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dep.dstAccessMask |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		}
		dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	}

	VkSubpassDescriptionFlags subpass_flags =
		((key.color_feedback_loop || key.tilegpu_self_read) &&
			m_optional_extensions.vk_ext_rasterization_order_attachment_access) ?
			VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT :
			0;
	// Mobile ordered depth feedback: on the framebuffer_fetch path the depth self-dependency above
	// is skipped, so declare ordered depth access here to make the in-tile subpassLoad of depth
	// coherent (mirror of the colour flag). Gated on depth_feedback (HWROV) so it never fires when
	// the toggle is off; the pipeline built for this pass sets the matching depth-stencil flag.
	if (key.depth_sampling && m_features.depth_feedback && m_features.framebuffer_fetch &&
		m_optional_extensions.vk_ext_roaa_depth)
		subpass_flags |= VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT;
	const VkSubpassDescription subpass = {subpass_flags, VK_PIPELINE_BIND_POINT_GRAPHICS, num_subpass_inputs,
		num_subpass_inputs ? input_reference.data() : nullptr, color_reference_ptr ? 1u : 0u,
		color_reference_ptr ? color_reference_ptr : nullptr, nullptr, depth_reference_ptr, 0, nullptr};
	const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0u, num_attachments,
		attachments.data(), 1u, &subpass, num_subpass_dependencies, num_subpass_dependencies ? subpass_dependency.data() : nullptr};

	VkRenderPass pass;
	const VkResult res = vkCreateRenderPass(m_device, &pass_info, nullptr, &pass);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateRenderPass failed: ");
		return VK_NULL_HANDLE;
	}

	m_render_pass_cache.emplace(key.key, pass);
	return pass;
}

static constexpr std::string_view SPIN_SHADER = R"(
#version 460 core

layout(std430, set=0, binding=0) buffer SpinBuffer { uint spin[]; };
layout(push_constant) uniform constants { uint cycles; };
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

void main()
{
	uint value = spin[0];
	// The compiler doesn't know, but spin[0] == 0, so this loop won't actually go anywhere
	for (uint i = 0; i < cycles; i++)
		value = spin[value];
	// Store the result back to the buffer so the compiler can't optimize it away
	spin[0] = value;
}
)";

bool GSDeviceVK::InitSpinResources()
{
	if (!m_spinning_supported)
		return true;

	// TODO: Move to safe destroy functions, use scoped guard.

	VkResult res;
#define CHECKED_CREATE(create_fn, create_struct, output_struct) \
	do \
	{ \
		if ((res = create_fn(m_device, create_struct, nullptr, output_struct)) != VK_SUCCESS) \
		{ \
			LOG_VULKAN_ERROR(res, #create_fn " failed: "); \
			return false; \
		} \
	} while (0)

	VkDescriptorSetLayoutBinding set_layout_binding = {};
	set_layout_binding.binding = 0;
	set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	set_layout_binding.descriptorCount = 1;
	set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo desc_set_layout_create = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	desc_set_layout_create.bindingCount = 1;
	desc_set_layout_create.pBindings = &set_layout_binding;
	CHECKED_CREATE(vkCreateDescriptorSetLayout, &desc_set_layout_create, &m_spin_descriptor_set_layout);

	const VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32)};
	VkPipelineLayoutCreateInfo pl_layout_create = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl_layout_create.setLayoutCount = 1;
	pl_layout_create.pSetLayouts = &m_spin_descriptor_set_layout;
	pl_layout_create.pushConstantRangeCount = 1;
	pl_layout_create.pPushConstantRanges = &push_constant_range;
	CHECKED_CREATE(vkCreatePipelineLayout, &pl_layout_create, &m_spin_pipeline_layout);

	VkShaderModule shader_module = g_vulkan_shader_cache->GetComputeShader(SPIN_SHADER);
	if (shader_module == VK_NULL_HANDLE)
		return false;

	Vulkan::SetObjectName(m_device, shader_module, "Spin Shader");

	VkComputePipelineCreateInfo pl_create = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pl_create.layout = m_spin_pipeline_layout;
	pl_create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pl_create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pl_create.stage.pName = "main";
	pl_create.stage.module = shader_module;
	res = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pl_create, nullptr, &m_spin_pipeline);
	vkDestroyShaderModule(m_device, shader_module, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateComputePipelines failed: ");
		return false;
	}
	Vulkan::SetObjectName(m_device, m_spin_pipeline, "Spin Pipeline");

	VmaAllocationCreateInfo buf_vma_create = {};
	buf_vma_create.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	VkBufferCreateInfo buf_create = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buf_create.size = 4;
	buf_create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if ((res = vmaCreateBuffer(m_allocator, &buf_create, &buf_vma_create, &m_spin_buffer, &m_spin_buffer_allocation,
			 nullptr)) != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer failed: ");
		return false;
	}
	Vulkan::SetObjectName(m_device, m_spin_buffer, "Spin Buffer");

	VkDescriptorSetAllocateInfo desc_set_allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	desc_set_allocate.descriptorPool = m_global_descriptor_pool;
	desc_set_allocate.descriptorSetCount = 1;
	desc_set_allocate.pSetLayouts = &m_spin_descriptor_set_layout;
	if ((res = vkAllocateDescriptorSets(m_device, &desc_set_allocate, &m_spin_descriptor_set)) != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkAllocateDescriptorSets failed: ");
		return false;
	}
	const VkDescriptorBufferInfo desc_buffer_info = {m_spin_buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet desc_set_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	desc_set_write.dstSet = m_spin_descriptor_set;
	desc_set_write.dstBinding = 0;
	desc_set_write.descriptorCount = 1;
	desc_set_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	desc_set_write.pBufferInfo = &desc_buffer_info;
	vkUpdateDescriptorSets(m_device, 1, &desc_set_write, 0, nullptr);

	for (SpinResources& resources : m_spin_resources)
	{
		u32 index = &resources - &m_spin_resources[0];
		VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pool_info.queueFamilyIndex = m_spin_queue_family_index;
		CHECKED_CREATE(vkCreateCommandPool, &pool_info, &resources.command_pool);
		Vulkan::SetObjectName(m_device, resources.command_pool, "Spin Command Pool %u", index);

		VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		buffer_info.commandPool = resources.command_pool;
		buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		buffer_info.commandBufferCount = 1;
		res = vkAllocateCommandBuffers(m_device, &buffer_info, &resources.command_buffer);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, resources.command_buffer, "Spin Command Buffer %u", index);

		VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CHECKED_CREATE(vkCreateFence, &fence_info, &resources.fence);
		Vulkan::SetObjectName(m_device, resources.fence, "Spin Fence %u", index);

		if (!m_spin_queue_is_graphics_queue)
		{
			VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			CHECKED_CREATE(vkCreateSemaphore, &sem_info, &resources.semaphore);
			Vulkan::SetObjectName(m_device, resources.semaphore, "Draw to Spin Semaphore %u", index);
		}
	}

#undef CHECKED_CREATE
	return true;
}

void GSDeviceVK::DestroySpinResources()
{
#define CHECKED_DESTROY(destructor, obj) \
	do \
	{ \
		if (obj != VK_NULL_HANDLE) \
		{ \
			destructor(m_device, obj, nullptr); \
			obj = VK_NULL_HANDLE; \
		} \
	} while (0)

	if (m_spin_buffer)
	{
		vmaDestroyBuffer(m_allocator, m_spin_buffer, m_spin_buffer_allocation);
		m_spin_buffer = VK_NULL_HANDLE;
		m_spin_buffer_allocation = VK_NULL_HANDLE;
	}
	CHECKED_DESTROY(vkDestroyPipeline, m_spin_pipeline);
	CHECKED_DESTROY(vkDestroyPipelineLayout, m_spin_pipeline_layout);
	CHECKED_DESTROY(vkDestroyDescriptorSetLayout, m_spin_descriptor_set_layout);
	if (m_spin_descriptor_set != VK_NULL_HANDLE)
	{
		vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &m_spin_descriptor_set);
		m_spin_descriptor_set = VK_NULL_HANDLE;
	}
	for (SpinResources& resources : m_spin_resources)
	{
		CHECKED_DESTROY(vkDestroySemaphore, resources.semaphore);
		CHECKED_DESTROY(vkDestroyFence, resources.fence);
		if (resources.command_buffer != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(m_device, resources.command_pool, 1, &resources.command_buffer);
			resources.command_buffer = VK_NULL_HANDLE;
		}
		CHECKED_DESTROY(vkDestroyCommandPool, resources.command_pool);
	}
#undef CHECKED_DESTROY
}

void GSDeviceVK::WaitForSpinCompletion(u32 index)
{
	SpinResources& resources = m_spin_resources[index];
	if (!resources.in_progress)
		return;

	const VkResult res = vkWaitForFences(m_device, 1, &resources.fence, VK_TRUE, UINT64_MAX);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkWaitForFences failed: ");
		if (res == VK_ERROR_DEVICE_LOST)
			ReportDeviceFault();
		m_last_submit_failed = true;
		return;
	}
	SpinCommandCompleted(index);
}

void GSDeviceVK::SpinCommandCompleted(u32 index)
{
	SpinResources& resources = m_spin_resources[index];
	resources.in_progress = false;
	const u32 timestamp_base = (index + NUM_COMMAND_BUFFERS) * 2;
	std::array<u64, 2> timestamps;
	const VkResult res =
		vkGetQueryPoolResults(m_device, m_timestamp_query_pool, timestamp_base, static_cast<u32>(timestamps.size()),
			sizeof(timestamps), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
	if (res == VK_SUCCESS)
	{
		u64 begin, end;
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
		{
			begin = timestamps[0] * m_spin_timestamp_scale + m_spin_timestamp_offset;
			end = timestamps[1] * m_spin_timestamp_scale + m_spin_timestamp_offset;
		}
		else
		{
			begin = timestamps[0] * m_spin_timestamp_scale;
			end = timestamps[1] * m_spin_timestamp_scale;
		}
		m_spin_manager.SpinCompleted(resources.cycles, begin, end);
	}
	else
	{
		LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");
	}
}

void GSDeviceVK::SubmitSpinCommand(u32 index, u32 cycles)
{
	SpinResources& resources = m_spin_resources[index];
	VkResult res;

	// Reset fence to unsignaled before starting.
	if ((res = vkResetFences(m_device, 1, &resources.fence)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

	// Reset command pools to beginning since we can re-use the memory now
	if ((res = vkResetCommandPool(m_device, resources.command_pool, 0)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

	// Enable commands to be recorded to the two buffers again.
	VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ((res = vkBeginCommandBuffer(resources.command_buffer, &begin_info)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");

	if (!m_spin_buffer_initialized)
	{
		m_spin_buffer_initialized = true;
		vkCmdFillBuffer(resources.command_buffer, m_spin_buffer, 0, VK_WHOLE_SIZE, 0);
		VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.srcQueueFamilyIndex = m_spin_queue_family_index;
		barrier.dstQueueFamilyIndex = m_spin_queue_family_index;
		barrier.buffer = m_spin_buffer;
		barrier.offset = 0;
		barrier.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
	}

	if (m_spin_queue_is_graphics_queue)
		vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);

	const u32 timestamp_base = (index + NUM_COMMAND_BUFFERS) * 2;
	vkCmdResetQueryPool(resources.command_buffer, m_timestamp_query_pool, timestamp_base, 2);
	vkCmdWriteTimestamp(
		resources.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, timestamp_base);
	vkCmdPushConstants(
		resources.command_buffer, m_spin_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32), &cycles);
	vkCmdBindPipeline(resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spin_pipeline);
	vkCmdBindDescriptorSets(resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spin_pipeline_layout, 0, 1,
		&m_spin_descriptor_set, 0, nullptr);
	vkCmdDispatch(resources.command_buffer, 1, 1, 1);
	vkCmdWriteTimestamp(
		resources.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, timestamp_base + 1);

	if ((res = vkEndCommandBuffer(resources.command_buffer)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");

	VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &resources.command_buffer;
	VkPipelineStageFlags sema_waits[] = {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
	if (!m_spin_queue_is_graphics_queue)
	{
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &resources.semaphore;
		submit_info.pWaitDstStageMask = sema_waits;
	}
	vkQueueSubmit(m_spin_queue, 1, &submit_info, resources.fence);
	resources.in_progress = true;
	resources.cycles = cycles;
}

void GSDeviceVK::CalibrateSpinTimestamp()
{
	if (!m_optional_extensions.vk_ext_calibrated_timestamps)
		return;
	VkCalibratedTimestampInfoEXT infos[2] = {
		{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
		{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, m_calibrated_timestamp_type},
	};
	u64 timestamps[2];
	u64 maxDeviation;
	constexpr u64 MAX_MAX_DEVIATION = 100000; // 100us
	for (int i = 0; i < 4; i++) // 4 tries to get under MAX_MAX_DEVIATION
	{
		const VkResult res = vkGetCalibratedTimestampsEXT(m_device, std::size(infos), infos, timestamps, &maxDeviation);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkGetCalibratedTimestampsEXT failed: ");
			return;
		}
		if (maxDeviation < MAX_MAX_DEVIATION)
			break;
	}
	if (maxDeviation >= MAX_MAX_DEVIATION)
		Console.Warning("vkGetCalibratedTimestampsEXT returned high max deviation of %lluus", maxDeviation / 1000);
	const double gpu_time = timestamps[0] * m_spin_timestamp_scale;
#ifdef _WIN32
	const double cpu_time = timestamps[1] * m_queryperfcounter_to_ns;
#else
	const double cpu_time = timestamps[1];
#endif
	m_spin_timestamp_offset = cpu_time - gpu_time;
}

u64 GSDeviceVK::GetCPUTimestamp()
{
#ifdef _WIN32
	LARGE_INTEGER value = {};
	QueryPerformanceCounter(&value);
	return static_cast<u64>(static_cast<double>(value.QuadPart) * m_queryperfcounter_to_ns);
#else
#ifdef CLOCK_MONOTONIC_RAW
	const bool use_raw = m_calibrated_timestamp_type == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
	const clockid_t clock = use_raw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#else
	const clockid_t clock = CLOCK_MONOTONIC;
#endif
	timespec ts = {};
	clock_gettime(clock, &ts);
	return static_cast<u64>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}

bool GSDeviceVK::AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, VmaAllocation* gpu_allocation,
	VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback)
{
	// Try to place the fixed index buffer in GPU local memory.
	// Use the staging buffer to copy into it.

	const VkBufferCreateInfo cpu_bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
	const VmaAllocationCreateInfo cpu_aci = {VMA_ALLOCATION_CREATE_MAPPED_BIT, VMA_MEMORY_USAGE_CPU_ONLY, 0, 0};
	VkBuffer cpu_buffer;
	VmaAllocation cpu_allocation;
	VmaAllocationInfo cpu_ai;
	VkResult res = vmaCreateBuffer(m_allocator, &cpu_bci, &cpu_aci, &cpu_buffer, &cpu_allocation, &cpu_ai);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer() for CPU expand buffer failed: ");
		return false;
	}

	const VkBufferCreateInfo gpu_bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
	const VmaAllocationCreateInfo gpu_aci = {0, VMA_MEMORY_USAGE_GPU_ONLY, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
	VmaAllocationInfo ai;
	res = vmaCreateBuffer(m_allocator, &gpu_bci, &gpu_aci, gpu_buffer, gpu_allocation, &ai);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer() for expand buffer failed: ");
		vmaDestroyBuffer(m_allocator, cpu_buffer, cpu_allocation);
		return false;
	}

	const VkBufferCopy buf_copy = {0u, 0u, size};
	fill_callback(cpu_ai.pMappedData);
	vmaFlushAllocation(m_allocator, cpu_allocation, 0, size);
	vkCmdCopyBuffer(GetCurrentInitCommandBuffer(), cpu_buffer, *gpu_buffer, 1, &buf_copy);
	DeferBufferDestruction(cpu_buffer, cpu_allocation);
	return true;
}


std::vector<GSAdapterInfo> GSDeviceVK::GetAdapterInfo()
{
	GPUList gpus = EnumerateGPUs();
	std::vector<GSAdapterInfo> ret;
	ret.reserve(gpus.size());
	for (auto& [physical_device, ai] : gpus)
		ret.push_back(std::move(ai));
	return ret;
}

bool GSDeviceVK::IsSuitableDefaultRenderer()
{
	GPUList gpus = EnumerateGPUs();
	if (gpus.empty())
	{
		// No adapters, not gonna be able to use VK.
		return false;
	}

	// Check the first GPU, should be enough.
	const std::string& name = gpus.front().second.name;
	INFO_LOG("Using Vulkan GPU '{}' for automatic renderer check.", name);

	// Any software rendering (LLVMpipe, SwiftShader).
	if (StringUtil::StartsWithNoCase(name, "llvmpipe") || StringUtil::StartsWithNoCase(name, "SwiftShader"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for software renderer.");
		return false;
	}

	// For Intel, OpenGL usually ends up faster on Linux, because of fbfetch.
	// Plus, the Ivy Bridge and Haswell drivers are incomplete.
	if (StringUtil::StartsWithNoCase(name, "Intel"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for Intel GPU.");
		return false;
	}

	Console.WriteLn(Color_StrongGreen, "Allowing Vulkan as default renderer.");
	return true;
}

RenderAPI GSDeviceVK::GetRenderAPI() const
{
	return RenderAPI::Vulkan;
}

bool GSDeviceVK::HasSurface() const
{
	return static_cast<bool>(m_swap_chain);
}

bool GSDeviceVK::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	if (!CreateDeviceAndSwapChain())
		return false;

	if (!CheckFeatures())
	{
		Host::ReportErrorAsync("GS", TRANSLATE_SV("GSDeviceVK", "Your GPU does not support the required Vulkan features."));
		return false;
	}

	if (!CreateNullTexture())
	{
		Host::ReportErrorAsync("GS", "Failed to create dummy texture");
		return false;
	}

	if ((m_null_framebuffer = GSTextureVK::CreateNullFramebuffer(m_max_framebuffer_width, m_max_framebuffer_height)) == VK_NULL_HANDLE)
	{
		Host::ReportErrorAsync("GS", "Failed to create dummy framebuffer");
		return false;
	}

	{
		std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/tfx.glsl");
		if (!shader.has_value())
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tfx.glsl.");
			return false;
		}

		m_tfx_source = std::move(*shader);
	}

	if (!CreatePipelineLayouts())
	{
		Host::ReportErrorAsync("GS", "Failed to create pipeline layouts");
		return false;
	}

	if (!CreateRenderPasses())
	{
		Host::ReportErrorAsync("GS", "Failed to create render passes");
		return false;
	}

	if (!CreateBuffers())
		return false;

	if (!CompileConvertPipelines() || !CompilePresentPipelines() || !CompileInterlacePipelines() ||
		!CompileMergePipelines() || !CompilePostProcessingPipelines() || !InitSpinResources())
	{
		Host::ReportErrorAsync("GS", "Failed to compile utility pipelines");
		return false;
	}

	if (!CreatePersistentDescriptorSets())
	{
		Host::ReportErrorAsync("GS", "Failed to create persistent descriptor sets");
		return false;
	}

	// CAS uses a compute pipeline; some Android drivers (older Adreno ES compilers — see the
	// Adreno-650 CAS crash) reject it. Don't fail device init over it — disable CAS and carry on,
	// mirroring how the GL backend gates CAS to desktop only.
	if (!CompileCASPipelines())
	{
		Console.Warning("VK: CAS pipeline compilation failed - disabling CAS sharpening.");
		m_features.cas_sharpening = false;
	}

	// Same non-fatal treatment as CAS above, and for the same reason: FSR1 is two more compute
	// pipelines, so a driver that chokes on CAS's will likely choke on these too. Leaving
	// Features().fsr1 false makes GSRenderer fall back to the plain bilinear present.
	if (!CompileSGSRPipeline())
	{
		Console.Warning("VK: SGSR pipeline compilation failed - disabling SGSR upscaling.");
		m_features.sgsr = false;
	}

	if (!CompileFSR1Pipelines())
	{
		Console.Warning("VK: FSR1 pipeline compilation failed - disabling FSR1 upscaling.");
		m_features.fsr1 = false;
	}

	if (!CompileImGuiPipeline())
		return false;

	InitializeState();
	return true;
}

void GSDeviceVK::Destroy()
{
	// Free the libretro backbuffers while the device is still alive; the
	// frontend stopped sampling at context_destroy.
	if (VKLibretro::Active)
	{
		WaitForGPUIdle();
		for (std::unique_ptr<GSTextureVK>& bb : s_libretro_bb)
			bb.reset();
		s_libretro_bb_retired.clear();
		s_libretro_bb_idx = 0;
		s_libretro_present_count = 0;
	}

	std::unique_lock lock(s_instance_mutex);

	GSDevice::Destroy();

	// Free the filter chain before the device goes away — it owns Vulkan objects created
	// against m_device, so tearing the device down first would leak/UB them.
	DestroyShaderChain();

	// Same reasoning for frame generation, which additionally holds AHardwareBuffers shared
	// with a second VkDevice it owns; its Shutdown idles both before releasing anything.
	GSLsfg::Shutdown();

	EndRenderPass();
	if (GetCurrentCommandBuffer() != VK_NULL_HANDLE)
	{
		ExecuteCommandBuffer(false);
		WaitForGPUIdle();
	}

	m_swap_chain.reset();

	if (m_device != VK_NULL_HANDLE)
	{
		DestroySpinResources();
		DestroyResources();
	}

	VKShaderCache::Destroy();

	// The negotiated device belongs to the libretro frontend (it made the
	// vkCreateDevice call) and it destroys it after context_destroy —
	// destroying it here would leave the frontend tearing down a dead device.
	if (m_device != VK_NULL_HANDLE)
		if (!(VKLibretro::Active && VKLibretro::Init.device == m_device))
			vkDestroyDevice(m_device, nullptr);

	if (m_debug_messenger_callback != VK_NULL_HANDLE)
		DisableDebugUtils();

	if (m_instance != VK_NULL_HANDLE)
		if (!(VKLibretro::Active && VKLibretro::Init.instance == m_instance))
			vkDestroyInstance(m_instance, nullptr);

	Vulkan::UnloadVulkanLibrary();
}

bool GSDeviceVK::UpdateWindow()
{
	DestroySurface();

	if (!AcquireWindow(false))
		return false;

	if (m_window_info.type == WindowInfo::Type::Surfaceless)
		return true;

	// make sure previous frames are presented
	ExecuteCommandBuffer(false);
	WaitForGPUIdle();

	// recreate surface in existing swap chain if it already exists
	if (m_swap_chain)
	{
		if (m_swap_chain->RecreateSurface(m_window_info))
		{
			m_window_info = m_swap_chain->GetWindowInfo();
			return true;
		}

		m_swap_chain.reset();
	}

	VkSurfaceKHR surface = VKSwapChain::CreateVulkanSurface(m_instance, m_physical_device, &m_window_info);
	if (surface == VK_NULL_HANDLE)
	{
		Console.Error("VK: Failed to create new surface for swap chain");
		return false;
	}

	VkPresentModeKHR present_mode;
	if (!VKSwapChain::SelectPresentMode(surface, &m_vsync_mode, &present_mode) ||
		!(m_swap_chain = VKSwapChain::Create(m_window_info, surface, present_mode,
			  Pcsx2Config::GSOptions::TriStateToOptionalBoolean(GSConfig.ExclusiveFullscreenControl))))
	{
		Console.Error("VK: Failed to create swap chain");
		VKSwapChain::DestroyVulkanSurface(m_instance, &m_window_info, surface);
		return false;
	}

	m_window_info = m_swap_chain->GetWindowInfo();
	RenderBlankFrame();
	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
	return true;
}

void GSDeviceVK::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	m_resize_requested = false;

	if (!m_swap_chain)
	{
		// Surfaceless (libretro): the "window" is the backbuffer rendered for
		// the frontend, so just adopt the new size — DoBeginPresent recreates
		// the backbuffer to match on the next frame.
		m_window_info.surface_width = new_window_width;
		m_window_info.surface_height = new_window_height;
		m_window_info.surface_scale = new_window_scale;
		return;
	}

	if (m_swap_chain->GetWidth() == new_window_width && m_swap_chain->GetHeight() == new_window_height)
	{
		// skip unnecessary resizes
		m_window_info.surface_scale = new_window_scale;
		return;
	}

	// make sure previous frames are presented
	WaitForGPUIdle();

	if (!m_swap_chain->ResizeSwapChain(new_window_width, new_window_height, new_window_scale))
	{
		// AcquireNextImage() will fail, and we'll recreate the surface.
		Console.Error("VK: Failed to resize swap chain. Next present will fail.");
		return;
	}

	m_window_info = m_swap_chain->GetWindowInfo();
}

bool GSDeviceVK::SupportsExclusiveFullscreen() const
{
	return false;
}

void GSDeviceVK::DestroySurface()
{
	WaitForGPUIdle();
	m_swap_chain.reset();
}

std::string GSDeviceVK::GetDriverInfo() const
{
	std::string ret;
	const u32 api_version = m_device_properties.apiVersion;
	const u32 driver_version = m_device_properties.driverVersion;
	if (m_optional_extensions.vk_khr_driver_properties)
	{
		const VkPhysicalDeviceDriverProperties& props = m_device_driver_properties;
		ret = StringUtil::StdStringFromFormat(
			"Driver %u.%u.%u\nVulkan %u.%u.%u\nConformance Version %u.%u.%u.%u\n%s\n%s\n%s",
			VK_VERSION_MAJOR(driver_version), VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version),
			VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version),
			props.conformanceVersion.major, props.conformanceVersion.minor, props.conformanceVersion.subminor,
			props.conformanceVersion.patch, props.driverInfo, props.driverName, m_device_properties.deviceName);
	}
	else
	{
		ret = StringUtil::StdStringFromFormat("Driver %u.%u.%u\nVulkan %u.%u.%u\n%s", VK_VERSION_MAJOR(driver_version),
			VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version), VK_API_VERSION_MAJOR(api_version),
			VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version), m_device_properties.deviceName);
	}

	return ret;
}

void GSDeviceVK::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_allow_present_throttle = allow_present_throttle;
	if (!m_swap_chain)
	{
		// For when it is re-created.
		m_vsync_mode = mode;
		return;
	}

	VkPresentModeKHR present_mode;
	if (!VKSwapChain::SelectPresentMode(m_swap_chain->GetSurface(), &mode, &present_mode))
	{
		ERROR_LOG("Ignoring vsync mode change.");
		return;
	}

	// Actually changed? If using a fallback, it might not have.
	if (m_vsync_mode == mode)
		return;

	m_vsync_mode = mode;

	// This swap chain should not be used by the current buffer, thus safe to destroy.
	WaitForGPUIdle();
	if (!m_swap_chain->SetPresentMode(present_mode))
	{
		pxFailRel("Failed to update swap chain present mode.");
		m_swap_chain.reset();
	}
}

GSDevice::PresentResult GSDeviceVK::DoBeginPresent(bool frame_skip)
{
	EndRenderPass();

	// Check if the device was lost.
	if (m_last_submit_failed)
		return PresentResult::DeviceLost;

	if (frame_skip)
		return PresentResult::FrameSkipped;

	// If we're running surfaceless, kick the command buffer so we don't run out of descriptors.
	if (!m_swap_chain)
	{
		// Libretro: run a REAL present targeting a dedicated backbuffer, so
		// the whole normal path (PresentRect aspect-correct draw, TV shaders,
		// FullscreenUI, ImGui OSD in EndPresent) works unchanged; EndPresent
		// then hands the finished image to the frontend. The backbuffers are
		// owned outside the texture pool because the frontend keeps sampling
		// the published view for cached-frame replays.
		if (VKLibretro::Active)
		{
			// Reclaim backbuffers retired long enough ago that the frontend's
			// replay window has moved past them (see kLibretroRetireFrames).
			const u64 now = ++s_libretro_present_count;
			std::erase_if(s_libretro_bb_retired, [now](const RetiredBackbuffer& r) {
				return now - r.retired_at >= kLibretroRetireFrames;
			});

			const GSVector2i pres = GetPresentationSize();
			std::unique_ptr<GSTextureVK>& bb = s_libretro_bb[s_libretro_bb_idx];
			if (!bb || bb->GetWidth() != pres.x || bb->GetHeight() != pres.y)
			{
				// The frontend may still replay the displaced image for a few
				// frames -- retire (freed later above), don't destroy now.
				if (bb)
					s_libretro_bb_retired.push_back({std::move(bb), now});
				bb = GSTextureVK::Create(GSTexture::RenderTarget, GSTexture::Format::Color,
					pres.x, pres.y, 1);
			}
			if (bb)
			{
				VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();
				if (!frame_skip && m_current)
					static_cast<GSTextureVK*>(m_current)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
				bb->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);

				const VkFramebuffer fb = bb->GetFramebuffer(false);
				if (fb != VK_NULL_HANDLE)
				{
					const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
						GetRenderPass(bb->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
							VK_ATTACHMENT_STORE_OP_STORE),
						fb, {{0, 0}, {static_cast<u32>(bb->GetWidth()), static_cast<u32>(bb->GetHeight())}},
						1u, &s_present_clear_color};
					vkCmdBeginRenderPass(cmdbuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

					const VkViewport vp{0.0f, 0.0f, static_cast<float>(bb->GetWidth()),
						static_cast<float>(bb->GetHeight()), 0.0f, 1.0f};
					const VkRect2D scissor{
						{0, 0}, {static_cast<u32>(bb->GetWidth()), static_cast<u32>(bb->GetHeight())}};
					vkCmdSetViewport(cmdbuffer, 0, 1, &vp);
					vkCmdSetScissor(cmdbuffer, 0, 1, &scissor);
					m_is_presenting = true;
					return PresentResult::OK;
				}
			}
		}

		ExecuteCommandBuffer(false);
		return PresentResult::FrameSkipped;
	}

	// End the pipeline statistics for this cmdbuffer before postprocessing.
	FrameResources& resources = m_frame_resources[m_current_frame];
	if (resources.pipeline_statistics_query == QueryState::Querying)
	{
		resources.pipeline_statistics_query = QueryState::Ready;
		vkCmdEndQuery(m_current_command_buffer, m_pipeline_statistics_query_pool, m_current_frame);
	}

	VkResult res = m_resize_requested ? VK_ERROR_OUT_OF_DATE_KHR : m_swap_chain->AcquireNextImage();
	if (res != VK_SUCCESS)
	{
		m_swap_chain->ReleaseCurrentImage();

		if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
		{
			ResizeWindow(0, 0, m_window_info.surface_scale);
			ImGuiManager::WindowResized();
			res = m_swap_chain->AcquireNextImage();
		}
		else if (res == VK_ERROR_SURFACE_LOST_KHR)
		{
			Console.Warning("VK: Surface lost, attempting to recreate");
			// Android: the surface dies when the activity is backgrounded or the
			// SurfaceView is torn down, and the handle cached in m_window_info is
			// stale — vkCreateAndroidSurfaceKHR on it crashes inside the loader.
			// Re-acquire the window first; if we're surfaceless now, drop the
			// swapchain and skip frames until the next surfaceChanged posts an
			// UpdateWindow with the new surface.
			if (!AcquireWindow(false) || m_window_info.type == WindowInfo::Type::Surfaceless)
			{
				Console.WriteLn("VK: Window is gone, dropping swap chain until it returns");
				DestroySurface();
				ExecuteCommandBuffer(false);
				return PresentResult::FrameSkipped;
			}
			if (!m_swap_chain->RecreateSurface(m_window_info))
			{
				Console.Error("VK: Failed to recreate surface after loss");
				// Do NOT keep the half-dead swap chain and retry the inline recreate:
				// after a surface loss the native window can still be held by the old
				// surface (stock Qualcomm Adreno returns NATIVE_WINDOW_IN_USE from
				// vkCreateSwapchainKHR on the same ANativeWindow), so RecreateSurface
				// fails again every frame and the game relaunch stays black forever
				// (#380 / #374; Turnip tolerates it and recovers, stock Adreno does
				// not). Drop the swap chain entirely so the next onNativeSurfaceChanged
				// -> MTGS::UpdateDisplayWindow -> UpdateWindow rebuilds from the genuinely
				// fresh surface instead of hammering the in-use one.
				DestroySurface();
				ExecuteCommandBuffer(false);
				return PresentResult::FrameSkipped;
			}

			res = m_swap_chain->AcquireNextImage();
		}
		else
			LOG_VULKAN_ERROR(res, "vkAcquireNextImageKHR() failed: ");

		// This can happen when multiple resize events happen in quick succession.
		// In this case, just wait until the next frame to try again.
		if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		{
			// Still submit the command buffer, otherwise we'll end up with several frames waiting.
			ExecuteCommandBuffer(false);
			return PresentResult::FrameSkipped;
		}
	}

	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();

	// Swap chain images start in undefined
	GSTextureVK* swap_chain_texture = m_swap_chain->GetCurrentTexture();
	swap_chain_texture->OverrideImageLayout(GSTextureVK::Layout::Undefined);
	swap_chain_texture->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);

	// Present render pass gets started out here, so we can't transition source textures in DoStretchRect
	// Make sure they're ready now
	if (!frame_skip && m_current)
		static_cast<GSTextureVK*>(m_current)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

	const VkFramebuffer fb = swap_chain_texture->GetFramebuffer(false);
	if (fb == VK_NULL_HANDLE)
		return GSDevice::PresentResult::FrameSkipped;

	const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
		GetRenderPass(swap_chain_texture->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE),
		fb,
		{{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}},
		1u, &s_present_clear_color};
	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);

	const VkViewport vp{0.0f, 0.0f, static_cast<float>(swap_chain_texture->GetWidth()),
		static_cast<float>(swap_chain_texture->GetHeight()), 0.0f, 1.0f};
	const VkRect2D scissor{
		{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}};
	vkCmdSetViewport(GetCurrentCommandBuffer(), 0, 1, &vp);
	vkCmdSetScissor(GetCurrentCommandBuffer(), 0, 1, &scissor);
	m_is_presenting = true;
	return PresentResult::OK;
}

void GSDeviceVK::EndPresent()
{
	RenderImGui();

	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();
	vkCmdEndRenderPass(cmdbuffer);
	m_is_presenting = false;

	if (VKLibretro::Active && !m_swap_chain)
	{
		// Libretro: finish the backbuffer, submit without any swapchain
		// semantics, and hand the image to the frontend.
		GSTextureVK* bb = s_libretro_bb[s_libretro_bb_idx].get();
		bb->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
		g_perfmon.Put(GSPerfMon::RenderPasses, 1);

		SubmitCommandBuffer(static_cast<VKSwapChain*>(nullptr));
		MoveToNextCommandBuffer();
		InvalidateCachedState();

		VKLibretro::Frame frame;
		frame.image = bb->GetImage();
		frame.view = bb->GetView();
		frame.format = bb->GetVkFormat();
		frame.width = static_cast<u32>(bb->GetWidth());
		frame.height = static_cast<u32>(bb->GetHeight());
		s_libretro_bb_idx = (s_libretro_bb_idx + 1) % kLibretroBackbuffers;
		VKLibretro::PublishFrame(frame);
		return;
	}

	m_swap_chain->GetCurrentTexture()->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::PresentSrc);
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	// Bring frame generation up or down to match the settings, once per frame and immediately
	// before the present it hooks. Both calls are cheap no-ops in the steady state; the work
	// happens only when the toggle, the multiplier, or the swapchain geometry actually changed.
	// The path goes in first because availability is partly a question about that file.
	GSLsfg::SetDllPath(GSConfig.LsfgDllPath);
	if (GSConfig.LsfgEnabled && GSLsfg::IsAvailable())
		GSLsfg::Initialize(m_swap_chain.get(), GSConfig.LsfgMultiplier);
	else if (GSLsfg::IsActive())
		GSLsfg::Shutdown();

	SubmitCommandBuffer(m_swap_chain.get());
	MoveToNextCommandBuffer();

	InvalidateCachedState();
}

bool GSDeviceVK::IsPresenting() const
{
	return m_is_presenting;
}

#ifdef ENABLE_OGL_DEBUG
static std::array<float, 3> Palette(float phase, const std::array<float, 3>& a, const std::array<float, 3>& b,
	const std::array<float, 3>& c, const std::array<float, 3>& d)
{
	std::array<float, 3> result;
	result[0] = a[0] + b[0] * std::cos(6.28318f * (c[0] * phase + d[0]));
	result[1] = a[1] + b[1] * std::cos(6.28318f * (c[1] * phase + d[1]));
	result[2] = a[2] + b[2] * std::cos(6.28318f * (c[2] * phase + d[2]));
	return result;
}
#endif

void GSDeviceVK::PushDebugGroup(const char* fmt, ...)
{
#ifdef ENABLE_OGL_DEBUG
	if (!vkCmdBeginDebugUtilsLabelEXT || !GSConfig.UseDebugDevice)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	const std::array<float, 3> color = Palette(
		++s_debug_scope_depth, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 0.5f}, {0.8f, 0.90f, 0.30f});

	const VkDebugUtilsLabelEXT label = {
		VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		nullptr,
		buf.c_str(),
		{color[0], color[1], color[2], 1.0f},
	};
	vkCmdBeginDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &label);
#endif
}

void GSDeviceVK::PushDrawLabel(const std::string_view label)
{
	// Compiled into every build; see the base-class comment. Requires only the
	// debug-utils entry points, which are present whenever the loader exposes the
	// extension -- notably without the validation layer UseDebugDevice would install.
	if (!vkCmdBeginDebugUtilsLabelEXT || !GSConfig.DebugLabels)
		return;

	const std::string buf(label);
	const VkDebugUtilsLabelEXT vk_label = {
		VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		nullptr,
		buf.c_str(),
		{0.6f, 0.8f, 1.0f, 1.0f},
	};
	vkCmdBeginDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &vk_label);
}

void GSDeviceVK::PopDrawLabel()
{
	if (!vkCmdEndDebugUtilsLabelEXT || !GSConfig.DebugLabels)
		return;

	vkCmdEndDebugUtilsLabelEXT(GetCurrentCommandBuffer());
}

void GSDeviceVK::PopDebugGroup()
{
#ifdef ENABLE_OGL_DEBUG
	if (!vkCmdEndDebugUtilsLabelEXT || !GSConfig.UseDebugDevice)
		return;

	s_debug_scope_depth = (s_debug_scope_depth == 0) ? 0 : (s_debug_scope_depth - 1u);

	vkCmdEndDebugUtilsLabelEXT(GetCurrentCommandBuffer());
#endif
}

void GSDeviceVK::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
#ifdef ENABLE_OGL_DEBUG
	if (!vkCmdInsertDebugUtilsLabelEXT || !GSConfig.UseDebugDevice)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	if (buf.empty())
		return;

	static constexpr float colors[][3] = {
		{0.1f, 0.1f, 0.0f}, // Cache
		{0.1f, 0.1f, 0.0f}, // Reg
		{0.5f, 0.0f, 0.5f}, // Debug
		{0.0f, 0.5f, 0.5f}, // Message
		{0.0f, 0.2f, 0.0f} // Performance
	};

	const VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr, buf.c_str(),
		{colors[static_cast<int>(category)][0], colors[static_cast<int>(category)][1],
			colors[static_cast<int>(category)][2], 1.0f}};
	vkCmdInsertDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &label);
#endif
}

bool GSDeviceVK::CreateDeviceAndSwapChain()
{
	std::unique_lock lock(s_instance_mutex);
	// The debug-utils instance extension is what makes vkCmdBeginDebugUtilsLabelEXT
	// resolvable, so per-draw labels need it. The validation layer stays tied to
	// UseDebugDevice alone -- that is the part that makes a capture useless for timing.
	bool enable_debug_utils = GSConfig.UseDebugDevice || GSConfig.DebugLabels;
	bool enable_validation_layer = GSConfig.UseDebugDevice;

	Error error;
	// The libretro frontend loads the library (and installs the negotiation
	// wraps) before GS opens; loading twice trips the loader's assert.
	if (!Vulkan::IsVulkanLibraryLoaded() && !Vulkan::LoadVulkanLibrary(&error))
	{
		Error::AddPrefix(&error, "Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?\nThe error was:\n");
		Host::ReportErrorAsync("Error", error.GetDescription());
		return false;
	}

	if (!AcquireWindow(true))
		return false;

	// Libretro context sharing: the VkInstance comes from the frontend's
	// negotiation interface. Function loading still goes through the wrapped
	// vkGetInstanceProcAddr so vkCreateDevice/vkQueueSubmit get intercepted.
	if (VKLibretro::Active && VKLibretro::Init.instance != VK_NULL_HANDLE)
		m_instance = VKLibretro::Init.instance;
	else
		m_instance = CreateVulkanInstance(m_window_info, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
	if (m_instance == VK_NULL_HANDLE)
	{
		if (enable_debug_utils || enable_validation_layer)
		{
			// Try again without the validation layer.
			enable_debug_utils = false;
			enable_validation_layer = false;
			m_instance = CreateVulkanInstance(m_window_info, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
			if (m_instance == VK_NULL_HANDLE)
			{
				Host::ReportErrorAsync("Error", "Failed to create Vulkan instance. Does your GPU and/or driver support Vulkan?");
				return false;
			}

			ERROR_LOG("VK: validation/debug layers requested but are unavailable. Creating non-debug device.");
		}
	}

	if (!Vulkan::LoadVulkanInstanceFunctions(m_instance))
	{
		ERROR_LOG("Failed to load Vulkan instance functions");
		return false;
	}

	GPUList gpus = EnumerateGPUs(m_instance);
	if (gpus.empty())
	{
		Host::ReportErrorAsync("Error", "No physical devices found. Does your GPU and/or driver support Vulkan?");
		return false;
	}

	if (VKLibretro::Active && VKLibretro::Init.gpu != VK_NULL_HANDLE)
	{
		// Frontend picked the physical device during negotiation.
		m_physical_device = VKLibretro::Init.gpu;
		vkGetPhysicalDeviceProperties(m_physical_device, &m_device_properties);
		m_name = m_device_properties.deviceName;
	}
	else
	{
	const bool is_default_gpu = GSConfig.Adapter == GetDefaultAdapter();
	if (!(GSConfig.Adapter.empty() || is_default_gpu))
	{
		u32 gpu_index = 0;
		for (; gpu_index < static_cast<u32>(gpus.size()); gpu_index++)
		{
			DEV_LOG("GPU {}: {}", gpu_index, gpus[gpu_index].second.name);
			if (gpus[gpu_index].second.name == GSConfig.Adapter)
			{
				m_physical_device = gpus[gpu_index].first;
				break;
			}
		}

		if (gpu_index == static_cast<u32>(gpus.size()))
		{
			WARNING_LOG("Requested GPU '{}' not found, using first ({})", GSConfig.Adapter, gpus[0].second.name);
			m_physical_device = gpus[0].first;
		}
	}
	else
	{
		INFO_LOG("{} GPU requested, using first ({})", is_default_gpu ? "Default" : "No", gpus[0].second.name);
		m_physical_device = gpus[0].first;
	}

	// Read device physical memory properties, we need it for allocating buffers
	vkGetPhysicalDeviceProperties(m_physical_device, &m_device_properties);

	// Stores the GPU name
	m_name = m_device_properties.deviceName;
	} // !VKLibretro gpu adoption

	// We need this to be at least 32 byte aligned for AVX2 stores.
	m_device_properties.limits.minUniformBufferOffsetAlignment =
		std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.minTexelBufferOffsetAlignment =
		std::max(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyOffsetAlignment =
		std::max(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyRowPitchAlignment = std::bit_ceil(
		std::max(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(32)));
	m_device_properties.limits.bufferImageGranularity =
		std::max(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(32));

	// Only the messenger, which is a debug-device concern. Draw labels need the
	// extension enabled above, not this.
	if (GSConfig.UseDebugDevice)
		EnableDebugUtils();

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ScopedGuard surface_cleanup = [this, &surface]() {
		if (surface != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(m_instance, surface, nullptr);
	};
	if (m_window_info.type != WindowInfo::Type::Surfaceless)
	{
		surface = VKSwapChain::CreateVulkanSurface(m_instance, m_physical_device, &m_window_info);
		if (surface == VK_NULL_HANDLE)
			return false;
	}

	// Attempt to create the device.
	if (!CreateDevice(surface, enable_validation_layer))
		return false;

	// And critical resources.
	if (!CreateAllocator() || !CreateGlobalDescriptorPool() || !CreateCommandBuffers())
		return false;

	VKShaderCache::Create();

	if (surface != VK_NULL_HANDLE)
	{
		VkPresentModeKHR present_mode;
		if (!VKSwapChain::SelectPresentMode(surface, &m_vsync_mode, &present_mode) ||
			!(m_swap_chain = VKSwapChain::Create(m_window_info, surface, present_mode,
				  Pcsx2Config::GSOptions::TriStateToOptionalBoolean(GSConfig.ExclusiveFullscreenControl))))
		{
			ERROR_LOG("Failed to create swap chain");
			return false;
		}

		// NOTE: This is assigned afterwards, because some platforms can modify the window info (e.g. Metal).
		m_window_info = m_swap_chain->GetWindowInfo();
	}

	surface_cleanup.Cancel();

	// Render a frame as soon as possible to clear out whatever was previously being displayed.
	if (m_window_info.type != WindowInfo::Type::Surfaceless)
		RenderBlankFrame();

	return true;
}

bool GSDeviceVK::CheckFeatures()
{
	const VkPhysicalDeviceLimits& limits = m_device_properties.limits;
	//const u32 vendorID = m_device_properties.vendorID;
	//const bool isAMD = (vendorID == 0x1002 || vendorID == 0x1022);
	//const bool isNVIDIA = (vendorID == 0x10DE);

	// NOTE (2026-07-12): we deliberately do NOT force the Mali runtime GPU profile here.
	// The old band-aid clamped blending accuracy up to Full on Mali (via the profile) to
	// dodge the broken HW dual-source unit — which is exactly why Mali used to "need
	// Blending=Max". sashkinbro/EmuCoreX replaced that with the correct, targeted fix:
	// m_features.dual_source_blend = dualSrcBlend (below), so GSRendererHW SW-blends ONLY
	// the specific SRC1/blend-mix/PABE draws instead of globally clamping. We already carry
	// that path, and the Vulkan renderer never reads the runtime profile anyway (only
	// GSDeviceOGL does), so the force was dead weight that diverged from his known-good tree.

	// Set when the user (or auto-detection) selects the Xclipse GPU profile; forces the
	// Xclipse fbfetch-off path below even if the 0x144D vendorID guess doesn't fire on
	// their driver. Declared outside the Android block so it stays a harmless false on
	// desktop. Populated from the resolved mobile profile just below.
	bool force_xclipse_profile = false;

	// The driver context feeds the driver-bug database (ported from EmuCoreX/sashkinbro with his
	// approval). Vulkan is the good case: VkPhysicalDeviceDriverProperties names the blob outright,
	// which is what the r44p1 DEVICE_LOST and 8-Elite push-descriptor fixes both learned the hard
	// way — gate on driverID, never on vendorID, or Turnip/PanVK inherit proprietary workarounds.
	// ProcessDeviceExtensions() has already filled m_device_driver_properties by the time we get
	// here (CreateDeviceAndSwapChain runs before CheckFeatures), so one resolve sees everything.
	//
	// Resolved on EVERY platform, not just Android: the database is keyed on the DRIVER, and the
	// same drivers ship off Android. Turnip on an ARM Linux handheld (Rocknix/Batocera) is the
	// same Mesa stack with the same defects as Turnip on a phone, and it was the #442 reproducer.
	// Gating this on __ANDROID__ made every rule silently dead on exactly the devices we test on.
	// Resolution is pure data — a rule only changes behaviour where something queries HasBug()/
	// UsesWorkaround(), and every such query is an explicit, per-defect decision.
	//
	// The MOBILE-SPECIFIC consequences below stay Android-only on purpose:
	//   - GS tuning / GPU identity: their only consumers are themselves __ANDROID__-gated, and
	//     changing desktop texture-pool sizing is not this code's business.
	// The runtime profile itself is NOT one of them (see below): the Vulkan resolve classifies
	// by device-name/driver match and answers Unknown for desktop GPUs, unlike the GL detector
	// (which calls every non-Mali GPU Adreno and genuinely must stay Android-only).
	MobileDriverContext driver_context;
	driver_context.api = MobileGpuApi::Vulkan;
	driver_context.vendor_id = m_device_properties.vendorID;
	driver_context.device_id = m_device_properties.deviceID;
	driver_context.driver_version = m_device_properties.driverVersion;
	driver_context.api_version = m_device_properties.apiVersion;
	driver_context.max_draw_indirect_count = m_device_properties.limits.maxDrawIndirectCount;
	if (m_optional_extensions.vk_khr_driver_properties)
	{
		driver_context.driver_id = static_cast<u32>(m_device_driver_properties.driverID);
		driver_context.driver_name = m_device_driver_properties.driverName;
		driver_context.driver_info = m_device_driver_properties.driverInfo;
	}
	const GpuProfileSelection mobile_profile = GpuProfileDetector::Resolve(
		GSConfig.AndroidGpuProfileOverride, std::string_view(), m_device_properties.deviceName,
		driver_context);
	SetMobileDriverProfile(mobile_profile.driver);
	// Published on every platform for the same reason the driver-DB resolve is: the ARM Linux
	// handhelds run the same Turnip/blob stacks as the phones, and an Android-only gate here
	// left GetRuntimeGPUProfile() answering Unknown for the entire Vulkan lifetime on exactly
	// those devices — the renderer-variant selection reads it, so Auto could never see a
	// tiler there. Desktop GPUs still resolve Unknown (name-keyed match, see above).
	SetRuntimeGPUProfile(mobile_profile.runtime_profile);
#if defined(__ANDROID__)
	// MediaTek (Dimensity/Helio) Mali Vulkan stacks return zero/stale destination color
	// through ROAA (black / missing textures) across GPU generations, so detect the SoC
	// here and disable fbfetch below. Ported from sashkinbro/EmuCoreX. Detection reads the
	// ro.soc.* props already folded into the profile hints (no new JNI needed).
	//
	// (The runtime profile itself is published above, unconditionally: it once lived only in
	// this Android block, which kept IsMaliGPUProfile()/IsAdrenoGPUProfile() answering the
	// default for the entire Vulkan lifetime — first noticed on Android where it killed the
	// Tekken 5 MediaTek override, then again on ARM Linux where it hid the GPU profile from
	// the renderer-variant selection.)
	SetMediaTekSoC(mobile_profile.is_mediatek_soc);
	force_xclipse_profile = (mobile_profile.override_mode == GpuProfileOverride::Xclipse) ||
		(mobile_profile.runtime_profile == RuntimeGpuProfile::Xclipse);
	// Per-vendor GS tuning (pool sizes/ages + constrained) — drives GSDevice pool sizing above.
	// This is what constrains texture/target caching on weaker Mali (e.g. G615). From EmuCoreX.
	SetMobileGPUIdentity(mobile_profile.gpu);
	SetMobileGSTuning(mobile_profile.gs_tuning);

	// Hand the resolved architecture to frame generation, which needs Adreno 7xx or newer. Done
	// here rather than asked for on demand so the settings screen can still say WHY the row is
	// unavailable after the game stops and the device is gone.
	{
		u32 adreno_generation = 0;
		switch (mobile_profile.gpu.architecture)
		{
			case MobileGpuArchitecture::Adreno7xx: adreno_generation = 7; break;
			case MobileGpuArchitecture::Adreno8xx: adreno_generation = 8; break;
			// Adreno X (X1-85 and up) postdates 7xx and carries the same feature set.
			case MobileGpuArchitecture::AdrenoX: adreno_generation = 9; break;
			default: break;
		}
		GSLsfg::NoteRendererCapability(true, adreno_generation);
	}
#endif
	Console.WriteLn("VK: GPU profile override='%s' resolved='%s' driver='%s' version=%u.%u.%u "
					"raw=%08x rules=%u bugs=%016llx workarounds=%016llx.",
		GpuProfileDetector::OverrideToConfigString(mobile_profile.override_mode),
		GpuProfileDetector::RuntimeProfileToString(mobile_profile.runtime_profile),
		GpuProfileDetector::DriverToString(mobile_profile.driver.driver),
		static_cast<unsigned>(mobile_profile.driver.version.major),
		static_cast<unsigned>(mobile_profile.driver.version.minor),
		static_cast<unsigned>(mobile_profile.driver.version.patch),
		static_cast<unsigned>(mobile_profile.driver.version.raw),
		static_cast<unsigned>(mobile_profile.driver.matched_rule_count),
		static_cast<unsigned long long>(mobile_profile.driver.bugs),
		static_cast<unsigned long long>(mobile_profile.driver.workarounds));
	DevCon.WriteLn("VK: GPU profile hints: %s", mobile_profile.hints.c_str());

	// BrokenSubpassFeedback + BrokenAttachmentFeedbackLoopLayout: on these drivers an in-pass
	// render-target self-read can silently drop the whole draw, in BOTH shapes — the subpassLoad
	// input attachment and the feedback-loop-layout texelFetch sampler. Reading a separate copy of
	// the target is the only reliable form.
	//
	// This was originally narrowed to replacement textures, on the evidence that Tales of the
	// Abyss + an HD pack lost its whole 2D text layer in-tile (at 1x as well as 4x, so not
	// tile-size related) while NFS Underground pushed 608 barrier draws per frame through the same
	// in-tile self-read with no pack and rendered correctly. That read the pattern backwards: the
	// self-read is not reliable for ordinary blending either, it just fails subtly enough there to
	// look fine. OutRun 2006 has no pack and renders its sea as high-contrast two-tone speckle -
	// 4.0% of the frame differs from the software renderer by more than 16 levels in-tile, and
	// 0.13% through the RT copy (Turnip/Adreno 650, water dump, 2026-08-02). Both in-pass shapes
	// are byte-identical wrong, which is what says driver rather than draw.
	//
	// So it applies to every draw on an affected driver now. The cost is real and scales with
	// upscale - measured on device, RT copy vs in-tile, median frame time over two runs each:
	//
	//              1x      3x      4x
	//   FlatOut 2  +7.5%  +10.5%  +24.6%   (copies/frame 23 -> 477, render passes 100 -> 538)
	//   OutRun    +20.5%   +9.9%   +0.5%
	//   GoW II     -3.2%   +2.9%
	//   RG lamps   -1.5%   +9.3%
	//
	// The old note recorded +38%/+40% at 3x/4x on NFSU. Nothing here reproduces that on the titles
	// available now; FlatOut 2 makes a bigger structural change (more copies, more render passes)
	// for a third of the cost at 3x. Treat the old figure as an upper bound on a build we can no
	// longer run, not as a contradiction.
	//
	// WHICH draws does it get wrong? All of them, on every affected title, and worse than it
	// looks. Scored per frame against the software renderer, in-tile (Turnip/Adreno 650,
	// 2026-08-02):
	//
	//   FlatOut 2   31.3% of the frame wrong by >16 levels   <- worst measured, once read as clean
	//   Katamari     7.5%
	//   OutRun       4.6%   (a separate scoring run from the 4.0% above, different frames)
	//   NFSU         0.00% by >16 levels, but ~40% of pixels off by 1-16
	//
	// NFSU is the whole lesson: corrupt everywhere and invisible, which is exactly what made
	// "only titles with a texture pack" look like a real gate. There is no title-level
	// discriminator to find. Nor a useful draw-level one - most corrupted draws never sample the
	// target, they read it for the destination-alpha test and the write mask, so "self-read"
	// understates what the workaround protects.
	//
	// Two symptoms, one behaviour (per-draw bisect + Turnip source reading, 2026-08-03): an
	// in-pass read returns render-pass-START content — stale across the pass's earlier DRAWS, and
	// stale across the earlier PRIMITIVES of its own draw (God of War II's ~10.9k-primitive
	// accumulation strip collapses to exactly one layer: error = -(k-1) on a pixel covered by k
	// layers, i.e. every fragment reads the pre-draw value). The explicit vkCmdPipelineBarrier
	// self-dependency changes nothing, and neither does forcing rasterization-order access on
	// every pipeline (TU_DEBUG=rast_order, byte-identical) — the read simply does not observe
	// unresolved tile writes.
	//
	// ⚠️ DO NOT ATTEMPT TO REPLACE THE COPY WITH A PASS BREAK. It was fully built and validated
	// (2026-08-03): break the pass before each one-barrier feedback draw and sample the live
	// attachment, so the stale read returns exactly the pre-draw snapshot the copy provides. It
	// IS byte-exact — but only in shapes that cost 2-5x whole-frame. The complete map, every cell
	// measured on the SD865:
	//
	//   - This workload's speed lives in Turnip's untiled sysmem NO_FLUSH mode: the bandwidth
	//     autotuner renders most of these small single-draw passes untiled, and the shipped COPY
	//     path depends on it too (TU_DEBUG=gmem: Katamari 4.2 -> 10.0 ms, NFSU 10.8 -> 48 ms).
	//   - A live self-read under sysmem NO_FLUSH is a data race: nondeterministic frames
	//     run-to-run, byte-compare passes by luck. Every fix abandons NO_FLUSH: declaring the
	//     loop via an input-attachment reference triggers feedback_invalidate (replayed per
	//     tile); the rasterization-order pipeline flag makes sysmem execution
	//     FLUSH_PER_OVERLAP_AND_OVERWRITE (per-overlap pipeline flush — these draws overlap
	//     heavily); pinning gmem pays the gmem tax directly. All land at 2x Katamari / 5x NFSU.
	//   - The copy is the unique shape that is correct, deterministic, AND compatible with
	//     sysmem NO_FLUSH — the shader reads a separate texture, so the driver owes it nothing.
	//     That is WHY the copy path is also the fastest: its measured "cost" (0.6 ms/frame on
	//     Katamari vs no read at all) cannot be recovered by removing the copy, because removing
	//     the copy removes the rendering mode.
	//
	// ⚠️ Reusing the clone ACROSS draws was also fully built and refuted (2026-08-05): a snapshot
	// cache keyed on "no pass end since the copy" with per-draw written-area tracking, verified
	// byte-exact on ten dumps — and it hit 0 times in ~4,800 feedback draws across the corpus.
	// The reads are byte-dependent on the writes: these draws read the RT at (or overlapping) the
	// destination pixels of the PREVIOUS feedback draw (blend/fbmask/tex-is-fb chains), so the
	// snapshot is stale by construction the moment it could be reused. That geometry is GS-state,
	// not GPU behaviour, so no driver revision changes it. Batching several copies into one pass
	// break fails on the same dependency: copy N is only valid after draw N-1 has executed. The
	// per-feedback-draw break+copy bracket is structural for this workload.
	//
	// OverrideTextureBarriers = 1 remains the documented way back to the in-tile path for A/B
	// work and for a future driver revision that fixes the read.
	const bool rt_self_read_is_broken =
		GetMobileDriverProfile().UsesWorkaround(DriverWorkaround::UseRenderTargetCopyForFeedback);

	// framebuffer_fetch: the tiler-native ordered Cd read (ROAA / subpassLoad in tile
	// memory). It lets DetermineBarriers() (GSRendererHW.cpp) drop every per-primitive
	// barrier and makes ROV (m_features.rov below) auto-disable — the fast, correct path
	// for blend-heavy games on a TBDR.
	//
	// MALI (0x13B5): ENABLED by default when ROAA is present. The Mali profile forces SW
	// blend, so fbfetch reads Cd in-shader and never touches Mali's broken HW dual-source
	// unit; without fbfetch the per-PRIMITIVE texture-barrier path tanks blend-heavy games
	// (GT4 = 10-20fps slideshow). No-op on any Mali lacking the extension.
	//
	// ADRENO / other non-Mali: opt-in via EnableAdrenoFramebufferFetch — but that is true only
	// where the Pcsx2Config default (false) actually holds, i.e. DESKTOP. The Android build ships
	// the key ON; see the vendor_allows_fbfetch note below before reasoning about who gets fbfetch.
	// ROV is the wrong primitive on a tiler (fragment_shader_interlock serializes same-pixel
	// fragments + bypasses tile memory), so on Adreno fbfetch is the way to make accurate
	// blending fast. Historically kept off because the Adreno-840 PROPRIETARY driver returned
	// STALE ROAA reads above Basic blending (alpha cutouts / invisible floors, A/B 2026-06-10);
	// that was never confirmed on other Adreno gens or on Turnip/Mesa, which is why it started
	// life as a ship-dark toggle to be A/B-verified per device+driver. Gated on ROAA presence, so
	// it is a no-op on any device that does not expose the extension.
	const bool is_mali_vk = (m_device_properties.vendorID == 0x13B5u);
	const bool is_adreno = IsDeviceAdreno();
	// Turnip/Mesa is the open Adreno driver and does NOT exhibit the proprietary
	// blob's stale-ROAA reads (the reason Adreno fbfetch shipped opt-in), so default
	// it ON there — the fast blend path on a tiler that drops the per-primitive
	// barriers spiking GS on transparency-heavy scenes. Proprietary Adreno is opt-in
	// via EnableAdrenoFramebufferFetch on desktop only (Android ships that key on);
	// DisableFramebufferFetch still overrides everywhere.
	//
	// ⚠️ In practice this is currently moot on Adreno: UseRenderTargetCopyForFeedback turns texture
	// barriers off below, and "fbfetch needs barriers" then clears framebuffer_fetch regardless of
	// what this resolves to. Framebuffer fetch IS the in-tile self-read, so a driver that cannot
	// do that read cannot have it. Kept as-is so a driver that stops carrying the bug recovers the
	// fast path for free.
	const bool is_turnip = (m_device_driver_properties.driverID == VK_DRIVER_ID_MESA_TURNIP);
	// Samsung Xclipse (Exynos AMD-RDNA2) has no working ROAA-based framebuffer fetch — force it off
	// there so we never route the fast-blend path into a broken unit. Inert if the 0x144D vendorID
	// guess is wrong (a real Xclipse tester must confirm IsDeviceXclipse() fires). The user
	// can also force it from Settings → Renderer → GPU Profile (force_xclipse_profile) for
	// drivers where the 0x144D vendorID doesn't report.
	const bool is_xclipse_vk = IsDeviceXclipse() || force_xclipse_profile;
	// MediaTek Mali + Mali-G57 expose ROAA but return zero/stale destination color from it
	// (black or intermittently missing textures); force those onto the texture-barrier path
	// instead of fbfetch. Ported from sashkinbro/EmuCoreX (MediaTek across GPU generations,
	// plus the older Mali-G57 case). deviceName is null-terminated by Vulkan.
	const bool is_mali_g57 = is_mali_vk &&
		(std::string_view(m_device_properties.deviceName).find("Mali-G57") != std::string_view::npos);
	const bool is_mediatek_mali_vk = is_mali_vk && IsMediaTekSoC();
	// ForceMaliFramebufferFetch (default OFF) lets a MediaTek/G57 user re-enable fbfetch to A/B
	// their own driver. The blanket disable above is a per-VENDOR guess ported wholesale from
	// EmuCoreX ("MediaTek across GPU generations"), not a per-driver fact, and it is expensive
	// on exactly this hardware: Mali reports dualSrcBlend=false, so GSRendererHW force-SW-blends
	// every SRC1/blend-mix/PABE draw, and with fbfetch off the only way to read Cd is the
	// per-PRIMITIVE texture barrier -- the "GT4 slideshow" path described above. Issue #339 is
	// that collision on a Dimensity 8350 + Mali-G615 (Shadow of the Colossus lost perf when SW
	// blend fixed its visuals). If a newer MediaTek driver reports ROAA honestly, this recovers
	// the fast blend path; if it still lies, the user sees the black/missing textures and turns
	// it back off -- which is why it must default OFF and stay a separate setting.
	//
	// Deliberately NOT reusing EnableAdrenoFramebufferFetch: that one is default-ON on Android
	// (Settings.kt adrenoFbFetch = true, plus a ConfigStore migration that flips old saves ON),
	// so keying off it would silently force fbfetch on for EVERY MediaTek Mali user -- the exact
	// breakage this block exists to prevent.
	//
	// Xclipse stays excluded even when forced: it has no working ROAA fbfetch at all, so honouring
	// the force there would route the fast path into a unit that cannot do it.
	// ANGLE is likewise no escape for the user -- it translates GLES onto this same Vulkan driver.
	// The working workaround remains the native GL renderer, whose fbfetch comes from
	// GL_ARM_shader_framebuffer_fetch (GSDeviceOGL) and never touches the Vulkan ROAA path.
	const bool unreliable_mali_fbfetch =
		(is_mediatek_mali_vk || is_mali_g57) && !GSConfig.ForceMaliFramebufferFetch;
	// is_adreno (not just is_turnip): the removed `if (is_adreno)` block used to force fbfetch on
	// for the whole vendor, so making it opt-in here would silently drop the proprietary blob onto
	// the per-primitive barrier path — a regression unrelated to #442. Keeping the vendor listed
	// preserves that default while letting DisableFramebufferFetch actually take effect, which the
	// old unconditional force ate (see feedback_adreno_fbfetch_ini_override_measurement_trap).
	//
	// ⚠️ The EnableAdrenoFramebufferFetch term is NOT a no-op, and the vendor terms below are NOT an
	// allow-list on Android. That key defaults to false only in Pcsx2Config.cpp (desktop, where this
	// really does restrict fbfetch to Mali+Adreno). The Android build ships it TRUE
	// (Settings.kt adrenoFbFetch = true) and force-flips existing saves to true via a one-time
	// ConfigStore migration, so there the disjunction is (is_mali_vk || is_adreno || true) == true
	// and the vendor terms restrict NOTHING: every GPU advertising ROAA takes the fbfetch path,
	// including PowerVR/Broadcom and any vendor not named here. Only the two negative terms still
	// bite — unreliable_mali_fbfetch and is_xclipse_vk.
	//
	// So the effective Android policy is a DENY-list (ROAA is trusted unless the vendor is known to
	// lie about it), not an allow-list. Do NOT "restore" the allow-list as a tidy-up: that would
	// REMOVE fbfetch from PowerVR et al. and drop them onto the ~3-4x-slower per-primitive barrier
	// path, on hardware nobody here can test. The deny-list shape is also the more future-proof one
	// — a new vendor with working ROAA gets the fast path instead of being stranded until someone
	// adds it to a list. If a non-Mali/non-Adreno vendor is ever REPORTED returning stale/empty
	// ROAA, add it alongside is_xclipse_vk rather than re-narrowing this.
	// 8 Elite (Adreno 8xx on the Qualcomm PROPRIETARY driver): that blob returns STALE ROAA reads
	// above Basic blending — invisible floors / alpha cutouts (A/B 2026-06-10, the "Adreno-840
	// proprietary" case in the note above). Never reproduced on 6xx/7xx or on Turnip/Mesa. So keep
	// the fast in-tile fbfetch path for every other Adreno, but route the 8xx proprietary blob onto
	// the correct texture-barrier path — restoring the historical 8-Elite exclusion. A hard gate like
	// is_xclipse_vk (the toggle can't force it back on), since it's a correctness bug, not a perf
	// trade; Turnip on 8xx (open driver, no stale reads) is unaffected and keeps the fast path.
	const bool is_adreno8xx_proprietary = is_adreno &&
		m_device_driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
		mobile_profile.gpu.architecture == MobileGpuArchitecture::Adreno8xx;
	const bool vendor_allows_fbfetch = !unreliable_mali_fbfetch && !is_adreno8xx_proprietary &&
		(is_mali_vk || is_adreno || GSConfig.EnableAdrenoFramebufferFetch) && !is_xclipse_vk;
	m_features.framebuffer_fetch = vendor_allows_fbfetch &&
		m_optional_extensions.vk_ext_rasterization_order_attachment_access && !GSConfig.DisableFramebufferFetch;
	m_features.texture_barrier = GSConfig.OverrideTextureBarriers != 0;
	// No working in-pass render-target self-read (ARMSX2 #442, Qualcomm/Turnip). Force the RT-COPY
	// path: with texture barriers off, GSRendererHW reads Cd from a separate copy of the target
	// (draw_rt_clone) instead of sampling the live attachment, and "fbfetch needs barriers" below
	// (framebuffer_fetch &= texture_barrier) drops the in-tile read too. Expensive — one RT copy
	// per feedback draw — but it is the only shape this driver renders correctly.
	//
	// tfx.glsl selects the read purely from two defines: DISABLE_TEXTURE_BARRIER (this path) or
	// HAS_FEEDBACK_LOOP_LAYOUT. Both compile texelFetch; the difference is whether the sampled
	// image is a copy or the live attachment, and only the copy works here. Turning
	// framebuffer_fetch off on its own does NOT change the variant — it leaves subpassLoad in
	// place, which is equally broken.
	//
	// Only applied when OverrideTextureBarriers is on auto (-1). An explicit 1 still wins, so the
	// in-tile path stays reachable for A/B-ing this workaround's cost and for a future driver
	// revision that fixes the read; an explicit 0 already lands here anyway.
	if (rt_self_read_is_broken && GSConfig.OverrideTextureBarriers < 0)
	{
		Console.WriteLn("VK: driver has an unreliable in-pass render-target self-read — forcing the "
						"RT-copy blend path.");
		m_features.texture_barrier = false;
	}
	// (Mali r44p1 used to get its own copy of the block above, testing driverInfo for "r44p1" and
	// clearing texture_barrier a second time. It is now rule vk-arm-r44p1-attachment-self-read in
	// the driver-bug database, so rt_self_read_is_broken already covers it and the duplicate is
	// gone. One difference, deliberate: the table-driven path respects OverrideTextureBarriers,
	// which the hand-rolled test ignored -- and the comment above documents forcing barriers on as
	// the way back to the in-tile path for A/B work, so honouring it is the intent.)
	m_features.multidraw_fb_copy = false;
	m_features.broken_point_sampler = false;

	// geometryShader is needed because gl_PrimitiveID is part of the Geometry SPIR-V Execution Model.
	m_features.primitive_id = m_device_features.geometryShader;

	m_features.prefer_new_textures = true;
#if defined(__ANDROID__)
	// Weak mobile parts would rather reuse than grow the pool, and full preloading blows
	// their texture budget. Both from the resolved GPU profile; sashkinbro/EmuCoreX.
	m_features.prefer_new_textures = GetMobileGSTuning().prefer_new_textures;
	// The profile no longer forces Texture Preloading down to Partial. It silently contradicted an
	// explicit user setting — our default is Full, and the downgrade fired for every "constrained"
	// profile, which includes the conservative fallback used by any GPU the table does not
	// recognise, so the UI said Full while the renderer ran Partial and only a log line said
	// otherwise. It was also order-dependent: GSConfig is a global reassigned wholesale on each
	// ApplySettings, while this override only re-runs when the device is recreated, so preloading
	// could differ between a fresh boot and a mid-session settings change. sashkinbro dropped it
	// too ("Restore fast mobile GS paths"); Full is upstream's default because it is usually the
	// faster path, and a pool-size heuristic is not a measurement of texture-memory pressure.
#endif
	m_features.provoking_vertex_last = m_optional_extensions.vk_ext_provoking_vertex;
	m_features.vs_expand = !GSConfig.DisableVertexShaderExpand;

	if (!m_features.texture_barrier)
		Console.Warning("VK: Texture buffers are disabled. This may break some graphical effects.");

	// Test for D32S8 support.
	{
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(m_physical_device, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
		m_features.stencil_buffer =
			((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
	}

	// Fbfetch is useless if we don't have barriers enabled.
	m_features.framebuffer_fetch &= m_features.texture_barrier;

	// The Vulkan spelling of framebuffer fetch *is* rasterization-order attachment access, whose
	// contract is that overlapping fragments in one draw observe each other in primitive order.
	// So the ordering a full barrier would provide is already guaranteed, and keeping the barrier
	// would only reintroduce the render-pass breaks this path exists to avoid.
	//
	// Derived AFTER every write to framebuffer_fetch above, including the RT-copy workaround's
	// texture_barrier mask. Deriving it beside the first assignment left the pair disagreeing on
	// Adreno — no fetch, but "fetch orders overlap" still true — which is inert only for as long as
	// every reader sits inside an `if (features.framebuffer_fetch)` gate, as DetermineBarriers
	// currently does. Keep this the last word on the bit rather than relying on that.
	//
	// ⚠️ Delivering that contract for an in-pass SELF-read is a separate question from ordering, and
	// Turnip answers the two differently depending on tiling. Read against mesa-26.1.2: the tiled
	// path sets GRAS_SC_CNTL.SINGLE_PRIM_MODE = FLUSH_PER_OVERLAP under rasterization-order access,
	// which the a6xx register docs define as waiting for any overlapping primitive prior to the
	// current one — the ordering really is requested. The stronger FLUSH_PER_OVERLAP_AND_OVERWRITE,
	// whose documented extra guarantee is that UCHE and CCU stay in sync "when fetching the previous
	// value for the current pixel", is only ever set on the UNTILED sysmem path (and there a feedback
	// loop alone is enough to get it). The device agrees: forcing ROAA on every pipeline changed
	// nothing while tiled, and the same draw came out correct under TU_DEBUG=sysmem. So what fails
	// on Adreno is read VISIBILITY while tiled — writes sit unresolved in GMEM while the fetch goes
	// out through UCHE — not primitive ordering. Adreno never reaches this line with fetch enabled,
	// so nothing here depends on it; a tiler whose fetch is a genuine tile-local read is a different
	// case and is not implicated by any of the above.
	m_features.framebuffer_fetch_orders_overlap = m_features.framebuffer_fetch;

	// Mali Vulkan stacks frequently report dualSrcBlend=false. When absent, GSRendererHW SW-blends
	// the specific draws that need SRC1 instead of relying on a global high blending-accuracy level
	// (which is why Mali no longer needs Blending=Max by hand). Ported from sashkinbro/EmuCoreX.
	m_features.dual_source_blend = m_device_features.dualSrcBlend;

	// Mali-G57 r13p0-class drivers can expose alternating/stale FastMAD history banks instead of the
	// reconstructed frame; GSRenderer::Merge falls those back to weave+blend. Ported from sashkinbro/EmuCoreX.
	m_features.broken_mad_deinterlace = is_mali_g57;

	// Concurrent depth test + depth-as-texture rides the same feedback-sync path as texture_barrier;
	// a driver with broken barriers has no chance doing GENERAL-layout depth feedback either.
	// Additionally, Adreno/turnip hangs the tiler sampling the live depth buffer while it is also the
	// depth attachment (tex == ds) — force it off there so tex == ds takes a depth copy instead of an
	// in-pass self-read.
	m_features.test_and_sample_depth = m_features.texture_barrier && !is_adreno;

	// Use D32F depth instead of D32S8 when we have framebuffer fetch.
	m_features.stencil_buffer &= !m_features.framebuffer_fetch;

	// @@MALI_TELEMETRY@@ One-line device/driver banner so Mali (and Adreno) field reports are
	// actionable: which GPU/driver, and — critically — which accurate-blend path was resolved:
	// in-tile framebuffer_fetch (cheap) vs the per-primitive barrier fallback (the tile-flush
	// slideshow). ROAA=yes but fbfetch=NO on Mali means the barrier path is active. See the
	// Mali driver-support deep dive.
	Console.WriteLn("VK: GPU '%s' vendor=0x%04X driver='%s' (%s) | ROAA=%s fbfetch=%s texbarrier=%s "
					"inpAttFB=%s dualSrc=%s testSampleDepth=%s madFallback=%s pushdesc=%s",
		m_device_properties.deviceName,
		m_device_properties.vendorID,
		m_device_driver_properties.driverName,
		m_device_driver_properties.driverInfo,
		m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "yes" : "NO",
		m_features.framebuffer_fetch ? "yes(in-tile)" : "NO(barrier-fallback)",
		m_features.texture_barrier ? "on" : "off",
		// inputAttachmentFeedback: the subpassInput/INPUT_ATTACHMENT descriptor path is active
		// when texture_barrier is on AND feedback_loop_layout is unavailable (Mali). This is the
		// path sashkinbro's stale-tile descriptor fix targets — the rainbow-blink suspect.
		(m_features.texture_barrier && !UseFeedbackLoopLayout()) ? "yes" : "NO",
		m_features.dual_source_blend ? "yes" : "NO(sw-blend-fallback)",
		m_features.test_and_sample_depth ? "on" : "off",
		m_features.broken_mad_deinterlace ? "weave+blend(G57)" : "motion-adaptive",
		m_use_push_descriptors ? "on" : "off");

	// Adreno colorWriteMask-with-depthtest bug (PPSSPP #10421 / thin3d_vulkan.cpp): on
	// Adreno 5xx and pre-0x801EA000 drivers the pipeline colorWriteMask is ignored while a
	// depth test is active, so masked RGBA channels get written. PS2 FBMASK relies on the
	// write mask; we emulate the one case Vulkan blend can express (RGB fully masked, alpha
	// independent) in CreateTFXPipeline. No user toggle; excludes Adreno 6xx/7xx/8xx.
	// The 0x801EA000 threshold is in the PROPRIETARY blob's driverVersion encoding; Turnip
	// reports Mesa's version (e.g. Mesa 26.1.2 -> 0x06801002), which is always below it and
	// made the workaround misfire on every Turnip device. The blob bug does not exist in
	// Mesa, so exclude Turnip outright.
	m_broken_colormask_with_depth = IsDeviceAdreno() && !is_turnip &&
		(m_device_properties.deviceID < 0x06000000u || m_device_properties.driverVersion < 0x801EA000u);
	if (m_broken_colormask_with_depth)
		Console.WriteLn("VK: Adreno colorWriteMask-with-depthtest workaround active (deviceID=0x%08X driver=0x%08X)",
			m_device_properties.deviceID, m_device_properties.driverVersion);

	// Adreno/turnip hangs the GPU (A6xx hangcheck) on any stencil-bearing D32S8 depth buffer. Force
	// stencil off so depth is created as plain D32_SFLOAT and no stencil attachment or stencil DATE
	// pre-pass is emitted; DATE falls back to the stencil-free paths (PrimID tracking, then Full, then Off).
	if (is_adreno)
		m_features.stencil_buffer = false;

	// On tiler GPUs, declaring gl_FragDepth (for PS2 32-bit Z quantization) emits
	// SPIR-V ExecutionMode DepthReplacing, which disables early-ZS for the entire
	// pipeline. Default-on for Mali; opt-out via INI for Z-precision-sensitive titles.
	//
	// Apple GPUs additionally miscompare. Depth stored through gl_FragDepth does not
	// bit-match the fixed-function interpolation that a later read-only pass tests
	// against, so a GEQUAL retest of the same geometry drops out along shared triangle
	// edges and whatever was drawn underneath shows through as pinpoints. The floor
	// only ever lowers the stored value, so it masks the mismatch rather than causing
	// it: on Black (SLUS-21376) a dark wall shows 7062 stray pixels with the depth
	// write on the shader path and the floor removed, 748 with the floor, and 0 with
	// the shader path skipped entirely. God of War II's Athena statue speckles the
	// same way. Biasing the stored value one PS2 Z unit down also clears it, which
	// puts the disagreement below a single Z unit.
	m_features.no_ps2_z_quantization =
		GSConfig.DisablePS2DepthQuantization || IsDeviceMali() || IsDeviceAppleGPU();

	// whether we can do point/line expand depends on the range of the device
	const float f_upscale = static_cast<float>(GSConfig.UpscaleMultiplier);
	m_features.point_expand = (m_device_features.largePoints && limits.pointSizeRange[0] <= f_upscale &&
							   limits.pointSizeRange[1] >= f_upscale);
	m_features.line_expand =
		(m_device_features.wideLines && limits.lineWidthRange[0] <= f_upscale && limits.lineWidthRange[1] >= f_upscale);

	// Mobile tile-native ordered depth feedback ("mobile ROV"), opt-in via HWROV. Reads the
	// depth buffer in-tile (subpassLoad on a depth input attachment) instead of copying it to a
	// colour RT (DoBeginDSAsRT), so SW-Z / DATE / alpha-test / AA1 depth passes fuse in-pass rather
	// than round-tripping. It needs an ordered in-tile depth read: ROAA on the depth aspect on the
	// framebuffer_fetch path (Mali-default / opt-in Adreno), or the render-pass self-dependency on
	// the texture_barrier path. Gated behind HWROV so toggle-off is byte-for-byte the well-tested
	// avoid/copy fallback (same as D3D11). Mali r44p1 excludes itself: texture_barrier is forced
	// off for it above, so framebuffer_fetch is off and the ordered read is unavailable.
	// Read at device init, so the depth half applies on game restart (HWROV's colour half is live).
	// Desktop keeps canonical feedback-loop behaviour.
#if defined(__ANDROID__)
	const bool depth_feedback_ordered =
		m_features.framebuffer_fetch ? m_optional_extensions.vk_ext_roaa_depth : m_features.texture_barrier;
	m_features.depth_feedback = GSConfig.HWROV && m_features.feedback_loops() && depth_feedback_ordered;
	Console.WriteLn("Mobile depth feedback (ROV): %s [HWROV=%s fbfetch=%s roaa_depth=%s texbarrier=%s]",
		m_features.depth_feedback ? "ENABLED" : "disabled", GSConfig.HWROV ? "on" : "off",
		m_features.framebuffer_fetch ? "yes" : "no", m_optional_extensions.vk_ext_roaa_depth ? "yes" : "no",
		m_features.texture_barrier ? "yes" : "no");
#else
	m_features.depth_feedback = m_features.feedback_loops();
#endif
	m_features.aa1 = GSConfig.HWAA1 && m_features.vs_expand && m_features.feedback_loops();

	DevCon.WriteLn("Optional features:%s%s%s%s%s%s", m_features.primitive_id ? " primitive_id" : "",
		m_features.texture_barrier ? " texture_barrier" : "", m_features.framebuffer_fetch ? " framebuffer_fetch" : "",
		m_features.provoking_vertex_last ? " provoking_vertex_last" : "", m_features.vs_expand ? " vs_expand" : "",
		m_features.no_ps2_z_quantization ? " no_ps2_z_quantization" : "");

	DevCon.WriteLn("Using %s for point expansion and %s for line expansion.",
		m_features.point_expand ? "hardware" : "vertex expanding",
		m_features.line_expand ? "hardware" : "vertex expanding");

	bool has_rov_storage_flags = true;

	// Check texture format support before we try to create them.
	for (u32 fmt = static_cast<u32>(GSTexture::Format::Color); fmt < static_cast<u32>(GSTexture::Format::PrimID); fmt++)
	{
		const VkFormat vkfmt = LookupNativeFormat(static_cast<GSTexture::Format>(fmt));
		VkFormatFeatureFlags bits = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

		if (static_cast<GSTexture::Format>(fmt) == GSTexture::Format::DepthStencil)
			bits |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
		else
			bits |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(m_physical_device, vkfmt, &props);
		if ((props.optimalTilingFeatures & bits) != bits)
		{
			// ColorClip (R16G16B16A16_UNORM) may not be supported as a render target on some GPUs
			// (e.g. Broadcom V3D). Fall back to ColorHDR (R16G16B16A16_SFLOAT) which provides
			// equivalent precision for color clamping emulation.
			if (static_cast<GSTexture::Format>(fmt) == GSTexture::Format::ColorClip)
			{
				Console.Warning("VK: ColorClip format (R16G16B16A16_UNORM) not supported as render target, falling back to ColorHDR (R16G16B16A16_SFLOAT).");
				m_colorclip_fallback_to_hdr = true;
				continue;
			}

			Host::ReportFormattedErrorAsync("VK: Renderer Unavailable",
				"Required format %u is missing bits, you may need to update your driver. (vk:%u, has:0x%x, needs:0x%x)",
				fmt, static_cast<unsigned>(vkfmt), props.optimalTilingFeatures, bits);
			return false;
		}

		if (GSTexture::IsShaderWriteFormat(static_cast<GSTexture::Format>(fmt)))
			has_rov_storage_flags &= ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0);
	}

	m_features.dxt_textures = m_device_features.textureCompressionBC;
	m_features.bptc_textures = m_device_features.textureCompressionBC;

	// ASTC LDR: the device feature must be enabled (SelectDeviceFeatures) AND every
	// footprint the loader accepts must be sampleable with optimal tiling. All-or-nothing,
	// because the loader is format-agnostic once a pack is installed.
	m_features.astc_textures = false;
	if (m_device_features.textureCompressionASTC_LDR)
	{
		static constexpr std::array<VkFormat, 14> s_astc_formats = {{
			VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
			VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
			VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
			VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
			VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
		}};

		bool all_ok = true;
		for (const VkFormat fmt : s_astc_formats)
		{
			VkFormatProperties props = {};
			vkGetPhysicalDeviceFormatProperties(m_physical_device, fmt, &props);
			if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
				!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
			{
				all_ok = false;
				break;
			}
		}
		m_features.astc_textures = all_ok;
	}

	DevCon.WriteLn("Vulkan: ASTC LDR texture replacements %s.",
		m_features.astc_textures ? "active" : "not supported by this device/driver");

	// The "no stencil buffer or texture barrier" warning is deliberately NOT shown on Android.
	// UseRenderTargetCopyForFeedback turns texture barriers off by design on the mobile drivers
	// this ships to (see the feedback notes above), so the condition is the NORMAL configuration
	// here rather than a fault — every affected user got a scary "this will break some graphical
	// effects" toast on every boot about a path we chose on purpose. It stays on desktop, where it
	// really does indicate a deficient driver.
#ifndef __ANDROID__
	if (!m_features.texture_barrier && !m_features.stencil_buffer)
	{
		Host::AddKeyedOSDMessage("GSDeviceVK_NoTextureBarrierOrStencilBuffer",
			TRANSLATE_STR("GS",
				"Stencil buffers and texture barriers are both unavailable, this will break some graphical effects."),
			Host::OSD_WARNING_DURATION);
	}
#endif

	m_max_texture_size = m_device_properties.limits.maxImageDimension2D;
	m_max_framebuffer_width = m_device_properties.limits.maxFramebufferWidth;
	m_max_framebuffer_height = m_device_properties.limits.maxFramebufferHeight;

#if defined(_WIN32)
	// ROV (fragment_shader_interlock) is only correct on immediate-mode desktop GPUs.
	// Gate it on a positive vendor allowlist rather than blocklisting known-bad tilers:
	// tilers order fragments per-tile, so pixel_interlock_ordered is either wrong or hangs
	// (Adreno/turnip A6xx hangcheck, PowerVR, Apple, Broadcom). WoA Adreno fails this for free.
	const bool rov_vendor_ok = IsDeviceNVIDIA() || IsDeviceAMD() || IsDeviceIntel();
	m_features.rov = m_optional_extensions.vk_ext_fragment_shader_interlock &&
	                 m_device_features.fragmentStoresAndAtomics &&
	                 has_rov_storage_flags &&
	                 !m_features.framebuffer_fetch &&
	                 rov_vendor_ok;
#else
	// Android and Linux (handheld/mobile targets) are tilers — ROV is never correct here.
	m_features.rov = false;
#endif

	return true;
}

void GSDeviceVK::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDraw(GetCurrentCommandBuffer(), m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive()
{
	DrawIndexedPrimitive(0, m_index.count);
}

void GSDeviceVK::DrawIndexedPrimitive(int offset, int count)
{
	pxAssert(offset + count <= (int)m_index.count);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDrawIndexed(GetCurrentCommandBuffer(), count, 1, m_index.start + offset, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitiveVSExpand(int offset, int count, bool vs_indexing, int vs_indexing_expansion)
{
	pxAssert(offset + count <= (int)m_index.count);

	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	if (vs_indexing)
	{
		SetVSPushConstants(m_vertex.start, m_index.start + offset);
		vkCmdDraw(GetCurrentCommandBuffer(), count * vs_indexing_expansion, 1, 0, 0);
	}
	else
	{
		SetVSPushConstants(m_vertex.start);
		vkCmdDrawIndexed(GetCurrentCommandBuffer(), count, 1, m_index.start + offset, 0, 0);
	}
}

void GSDeviceVK::Draw(const GSHWDrawConfig& config, int offset, int count)
{
	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		const bool vs_indexing = config.vs.UseVSExpandIndexBuffer();
		const u32 vs_indexing_expansion = GetExpansionFactor(config.vs.expand);
		DrawIndexedPrimitiveVSExpand(offset, count, vs_indexing, vs_indexing_expansion);
	}
	else
	{
		DrawIndexedPrimitive(offset, count);
	}
}

void GSDeviceVK::Draw(const GSHWDrawConfig& config)
{
	Draw(config, 0, m_index.count);
}

VkFormat GSDeviceVK::LookupNativeFormat(GSTexture::Format format) const
{
	static constexpr std::array<VkFormat, static_cast<int>(GSTexture::Format::Last) + 1> s_format_mapping = {{
		VK_FORMAT_UNDEFINED, // Invalid
		VK_FORMAT_R8G8B8A8_UNORM, // Color
		VK_FORMAT_A2B10G10R10_UNORM_PACK32, // ColorHQ
		VK_FORMAT_R16G16B16A16_SFLOAT, // ColorHDR
		VK_FORMAT_R16G16B16A16_UNORM, // ColorClip
		VK_FORMAT_D32_SFLOAT_S8_UINT, // DepthStencil
		VK_FORMAT_R32_SFLOAT, // DepthColor
		VK_FORMAT_R8_UNORM, // UNorm8
		VK_FORMAT_R16_UINT, // UInt16
		VK_FORMAT_R32_UINT, // UInt32
		VK_FORMAT_R32_SFLOAT, // PrimID
		VK_FORMAT_BC1_RGBA_UNORM_BLOCK, // BC1
		VK_FORMAT_BC2_UNORM_BLOCK, // BC2
		VK_FORMAT_BC3_UNORM_BLOCK, // BC3
		VK_FORMAT_BC7_UNORM_BLOCK, // BC7
		VK_FORMAT_ASTC_4x4_UNORM_BLOCK, // ASTC4x4
		VK_FORMAT_ASTC_5x4_UNORM_BLOCK, // ASTC5x4
		VK_FORMAT_ASTC_5x5_UNORM_BLOCK, // ASTC5x5
		VK_FORMAT_ASTC_6x5_UNORM_BLOCK, // ASTC6x5
		VK_FORMAT_ASTC_6x6_UNORM_BLOCK, // ASTC6x6
		VK_FORMAT_ASTC_8x5_UNORM_BLOCK, // ASTC8x5
		VK_FORMAT_ASTC_8x6_UNORM_BLOCK, // ASTC8x6
		VK_FORMAT_ASTC_8x8_UNORM_BLOCK, // ASTC8x8
		VK_FORMAT_ASTC_10x5_UNORM_BLOCK, // ASTC10x5
		VK_FORMAT_ASTC_10x6_UNORM_BLOCK, // ASTC10x6
		VK_FORMAT_ASTC_10x8_UNORM_BLOCK, // ASTC10x8
		VK_FORMAT_ASTC_10x10_UNORM_BLOCK, // ASTC10x10
		VK_FORMAT_ASTC_12x10_UNORM_BLOCK, // ASTC12x10
		VK_FORMAT_ASTC_12x12_UNORM_BLOCK, // ASTC12x12
	}};

	if (format == GSTexture::Format::ColorClip && m_colorclip_fallback_to_hdr)
		return VK_FORMAT_R16G16B16A16_SFLOAT;

	return (format != GSTexture::Format::DepthStencil || m_features.stencil_buffer) ?
		s_format_mapping[static_cast<int>(format)] :
		VK_FORMAT_D32_SFLOAT;
}

GSTexture* GSDeviceVK::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	std::unique_ptr<GSTexture> tex = GSTextureVK::Create(usage, format, width, height, levels);
	if (!tex)
	{
		// We're probably out of vram, try flushing the command buffer to release pending textures.
		PurgePool();
		ExecuteCommandBufferAndRestartRenderPass(true, "Couldn't allocate texture.");
		tex = GSTextureVK::Create(usage, format, width, height, levels);
	}

	return tex.release();
}

std::unique_ptr<GSDownloadTexture> GSDeviceVK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureVK::Create(width, height, format);
}

void GSDeviceVK::DoHintReadbackSource(GSTexture* tex)
{
	// MRU ring of 2 (see the member comment): per-frame readback patterns re-read the
	// same one or two targets, and the next draw into one of them predicts a readback.
	if (m_recent_readback_sources[0] == tex || m_recent_readback_sources[1] == tex)
		return;

	m_recent_readback_sources[1] = m_recent_readback_sources[0];
	m_recent_readback_sources[0] = tex;
}

// Two images moved at the same point go into ONE vkCmdPipelineBarrier, not one each. The image
// barriers are the same structs either way -- what merges is the submission. The call's stage masks
// become the union of the two, which is a STRONGER dependency than either alone and never a weaker
// one, so the merge cannot under-synchronise. Measured motivation: the TileGpu DATE snapshot records
// four barrier submissions a pass, 5,360 a stuntman frame, and they are two pairs at two points.
static void TransitionTwoImages(VkCommandBuffer cmd, GSTextureVK* a, GSTextureVK::Layout a_layout,
	GSTextureVK* b, GSTextureVK::Layout b_layout)
{
	// Either both moves batch or neither does. Batching one and falling back for the other would
	// record the two out of the order the caller asked for them in.
	if ((a && !a->LayoutTransitionIsBatchable(a_layout)) || (b && !b->LayoutTransitionIsBatchable(b_layout)))
	{
		if (a)
			a->TransitionToLayout(cmd, a_layout);
		if (b)
			b->TransitionToLayout(cmd, b_layout);
		return;
	}

	VkImageMemoryBarrier barriers[2];
	VkPipelineStageFlags src_stage = 0, dst_stage = 0;
	u32 count = 0;
	const auto add = [&](GSTextureVK* tex, GSTextureVK::Layout layout) {
		VkPipelineStageFlags s, d;
		// False means the image is already there and there is nothing to record -- which is also
		// what the source-is-the-destination copy hits on its second call.
		if (!tex || !tex->BuildBatchedLayoutTransition(barriers[count], s, d, layout))
			return;
		src_stage |= s;
		dst_stage |= d;
		count++;
	};
	add(a, a_layout);
	add(b, b_layout);
	if (count > 0)
		vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, count, barriers);
}

void GSDeviceVK::DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	// Empty rect, abort copy.
	if (r.rempty())
	{
		GL_INS("VK: DoCopyRect rect empty.");
		return;
	}

	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	const GSVector4i dst_rect(0, 0, dTexVK->GetWidth(), dTexVK->GetHeight());
	const bool full_draw_copy = dst_rect.eq(r);

	// Source is cleared, if destination is a render target, we can carry the clear forward.
	if (sTexVK->GetState() == GSTexture::State::Cleared)
	{
		if (dTexVK->IsRenderTargetOrDepthStencil())
		{
			if (ProcessClearsBeforeCopy(sTex, dTex, full_draw_copy))
				return;

			// Do an attachment clear.
			const bool depth = dTexVK->IsDepthStencil();
			OMSetRenderTargets(depth ? nullptr : dTexVK, depth ? dTexVK : nullptr, dst_rect);
			BeginRenderPassForStretchRect(
				dTexVK, dst_rect, GSVector4i(destX, destY, destX + r.width(), destY + r.height()));

			// so use an attachment clear. VkClearValue is a union, so only the aspect we are
			// actually clearing may be written -- filling both destroys the colour's red and
			// green with the depth and the stencil.
			VkClearAttachment ca;
			ca.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			ca.colorAttachment = 0;
			if (depth)
			{
				ca.clearValue.depthStencil.depth = sTexVK->GetClearDepth();
				ca.clearValue.depthStencil.stencil = 0;
			}
			else
			{
				GSVector4::store<false>(ca.clearValue.color.float32, sTexVK->GetClearForFormat());
			}

			// The clear rect is in framebuffer coordinates and the framebuffer is the whole
			// destination, so it has to carry the copy's destination offset.
			const VkClearRect cr = {{{static_cast<s32>(destX), static_cast<s32>(destY)},
										{static_cast<u32>(r.width()), static_cast<u32>(r.height())}},
				0u, 1u};
			vkCmdClearAttachments(GetCurrentCommandBuffer(), 1, &ca, 1, &cr);

			return;
		}

		// commit the clear to the source first, then do normal copy
		sTexVK->CommitClear();
	}

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	// if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
	// (the area outside of where we're copying to)
	if (dTexVK->GetState() == GSTexture::State::Cleared && !full_draw_copy)
		dTexVK->CommitClear();

	// *now* we can do a normal image copy.
	const VkImageAspectFlags src_aspect =
		(sTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageAspectFlags dst_aspect =
		(dTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageCopy ic = {{src_aspect, 0u, 0u, 1u}, {r.left, r.top, 0u}, {dst_aspect, 0u, 0u, 1u},
		{static_cast<s32>(destX), static_cast<s32>(destY), 0u},
		{static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u}};

	EndRenderPass();

	sTexVK->SetUseFenceCounter(GetCurrentFenceCounter());
	dTexVK->SetUseFenceCounter(GetCurrentFenceCounter());
	// One barrier submission for the pair, not one each: both images move at this point and nothing
	// is recorded between them.
	const bool copy_to_self = (dTexVK == sTexVK);
	TransitionTwoImages(GetCurrentCommandBuffer(), sTexVK,
		copy_to_self ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferSrc, dTexVK,
		copy_to_self ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferDst);

	vkCmdCopyImage(GetCurrentCommandBuffer(), sTexVK->GetImage(), sTexVK->GetVkLayout(), dTexVK->GetImage(),
		dTexVK->GetVkLayout(), 1, &ic);

	dTexVK->SetState(GSTexture::State::Dirty);
}

void GSDeviceVK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	pxAssert(dTex);
	filter = shader.SupportsBilinear() ? Nearest : filter; // Don't allow HW bilinear if SW bilinear is needed.
	const bool allow_discard = (shader.Mask() == 0xf);
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), filter, allow_discard);
}

void GSDeviceVK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
	PresentShader shader, Filter filter)
{
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, nullptr, dRect,
		m_present.at(static_cast<u32>(shader)), filter, true);
}

void GSDeviceVK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, dTex ? dTex->GetSize() : GetPresentationSize());
	cb.SetTime(shaderTime);
	SetUtilityPushConstants(&cb, sizeof(cb));

	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		m_present[static_cast<int>(shader)], filter, true);
}

void GSDeviceVK::DoDrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader)
{
	GSTexture* last_tex = rects[0].src;
	Filter last_filter = rects[0].filter;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	// Make sure all textures are in shader read only layout, so we don't need to break
	// the render pass to transition.
	for (u32 i = 0; i < num_rects; i++)
	{
		GSTextureVK* const stex = static_cast<GSTextureVK*>(rects[i].src);
		stex->CommitClear();
		if (stex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
		{
			EndRenderPass();
			stex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].filter == last_filter && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
		last_tex = rects[i].src;
		last_filter = rects[i].filter;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
}

void GSDeviceVK::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvertSelector shader)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	
	// Set up vertices first.
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
		{
			pxFailRel("Failed to reserve space for vertices");
		}
	}

	// Pain in the arse because the primitive topology for the pipelines is all triangle strips.
	// Don't use primitive restart here, it ends up slower on some drivers.
	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;

		const float inv_x = 2.0f / ds.x;
		const float inv_y = 2.0f / ds.y;

		const float left = dRect.x * inv_x - 1.0f;
		const float right = dRect.z * inv_x - 1.0f;
		const float top = 1.0f - dRect.y * inv_y;
		const float bottom = 1.0f - dRect.w * inv_y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(icount * sizeof(u16));
	SetIndexBuffer(m_index_stream_buffer.GetBuffer());

	// Even though we're batching, a cmdbuffer submit could've messed this up.
	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetUtilityTexture(rects[0].src, rects[0].filter == Biln ? m_linear_sampler : m_point_sampler);

	SetPipeline(GetConvertPipeline(shader.SetMask(rects[0].wmask.wrgba)));

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
}

void GSDeviceVK::BeginRenderPassForStretchRect(
	GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	pxAssert(dst_rc.x >= 0 && dst_rc.y >= 0 && dst_rc.z <= dTex->GetWidth() && dst_rc.w <= dTex->GetHeight());

	const VkAttachmentLoadOp load_op =
		(allow_discard && dst_rc.eq(dtex_rc)) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : GetLoadOpForTexture(dTex);
	dTex->SetState(GSTexture::State::Dirty);

	if (dTex->IsDepthStencil())
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_depth_render_pass_clear, dtex_rc, dTex->GetClearDepth(), 0);
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_depth_render_pass_discard :
			                                                               m_utility_depth_render_pass_load, dtex_rc);
	}
	else if (dTex->GetFormat() == GSTexture::Format::Color)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_color_render_pass_clear, dtex_rc, dTex->GetClearColor());
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_color_render_pass_discard :
			                                                               m_utility_color_render_pass_load, dtex_rc);
	}
	else
	{
		// integer formats, etc
		const VkRenderPass rp = GetRenderPass(dTex->GetVkFormat(), VK_FORMAT_UNDEFINED, load_op,
			VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		{
			BeginClearRenderPass(rp, dtex_rc, dTex->GetClearColor());
		}
		else
		{
			BeginRenderPass(rp, dtex_rc);
		}
	}
}

void GSDeviceVK::DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
	VkPipeline pipeline, Filter filter, bool allow_discard)
{
	if (sTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
	{
		// can't transition in a render pass
		EndRenderPass();
		sTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}

	SetUtilityTexture(sTex, filter == Biln ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->IsDepthStencil());
	const GSVector2i size(is_present ? GetPresentationSize() : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	// switch rts (which might not end the render pass), so check the bounds
	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
		if (InRenderPass() && dTex->GetState() == GSTexture::State::Cleared)
			EndRenderPass();
	}
	else
	{
		// this is for presenting, we don't want to screw with the viewport/scissor set by display
		m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !InRenderPass())
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

// Rotate a logical-NDC point (x, y) into physical-NDC by GSConfig.Rotation.
// Used by the present pass to map a quad laid out for a logically-rotated
// window onto the unrotated swapchain viewport.
//   Rot0:   (x,  y)
//   Rot90:  (y, -x)   (image rotates 90° CW on the panel)
//   Rot180: (-x,-y)
//   Rot270: (-y, x)   (image rotates 90° CCW on the panel)
static void RotateNDCForPresent(float& x, float& y)
{
	switch (GSConfig.Rotation)
	{
		case DisplayRotation::Rot90:
		{
			const float nx = y;
			const float ny = -x;
			x = nx;
			y = ny;
			break;
		}
		case DisplayRotation::Rot180:
			x = -x;
			y = -y;
			break;
		case DisplayRotation::Rot270:
		{
			const float nx = -y;
			const float ny = x;
			x = nx;
			y = ny;
			break;
		}
		case DisplayRotation::Rot0:
		default:
			break;
	}
}

void GSDeviceVK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	// ia
	const float inv_x = 2.0f / ds.x;
	const float inv_y = 2.0f / ds.y;

	float left = dRect.x * inv_x - 1.0f;
	float right = dRect.z * inv_x - 1.0f;
	float top = 1.0f - dRect.y * inv_y;
	float bottom = 1.0f - dRect.w * inv_y;

	// Present pass: map logical-NDC (computed against the rotated window) onto
	// the unrotated physical swapchain viewport. Non-present passes pass the
	// real dst-texture size in `ds` and must not rotate.
	if (m_is_presenting && GSConfig.Rotation != DisplayRotation::Rot0)
	{
		float tlx = left, tly = top;
		float trx = right, try_ = top;
		float blx = left, bly = bottom;
		float brx = right, bry = bottom;
		RotateNDCForPresent(tlx, tly);
		RotateNDCForPresent(trx, try_);
		RotateNDCForPresent(blx, bly);
		RotateNDCForPresent(brx, bry);

		const GSVertexPT1 vertices[] = {
			{GSVector4(tlx, tly, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
			{GSVector4(trx, try_, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
			{GSVector4(blx, bly, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
			{GSVector4(brx, bry, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
		};
		IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	}
	else
	{
		const GSVertexPT1 vertices[] = {
			{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
			{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
			{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
			{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
		};
		IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	}

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDeviceVK::BlitRect(GSTexture* sTex, const GSVector4i& sRect, u32 sLevel, GSTexture* dTex,
	const GSVector4i& dRect, u32 dLevel, Filter filter)
{
	GSTextureVK* sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* dTexVK = static_cast<GSTextureVK*>(dTex);

	EndRenderPass();

	sTexVK->TransitionToLayout(GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(GSTextureVK::Layout::TransferDst);

	// ensure we don't leave this bound later on
	if (m_tfx_textures[0] == sTexVK)
		PSSetShaderResource(0, nullptr, false);

	pxAssert(sTexVK->IsDepthStencil() == dTexVK->IsDepthStencil());
	const VkImageAspectFlags aspect =
		sTexVK->IsDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageBlit ib{{aspect, sLevel, 0u, 1u}, {{sRect.left, sRect.top, 0}, {sRect.right, sRect.bottom, 1}},
		{aspect, dLevel, 0u, 1u}, {{dRect.left, dRect.top, 0}, {dRect.right, dRect.bottom, 1}}};

	vkCmdBlitImage(GetCurrentCommandBuffer(), sTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib,
		filter == Biln ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void GSDeviceVK::DoUpdateCLUTTexture(
	GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	// Super annoying, but apparently NVIDIA doesn't like floats/ints packed together in the same vec4?
	struct alignas(16) Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};

	const Uniforms uniforms = {offsetX, offsetY, dOffset, 0, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), Nearest, true);
}

void GSDeviceVK::DoConvertToIndexedTexture(
	GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct alignas(16) Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 PSM;
		u32 pad1[1];
		float ScaleFactor;
		float pad2[3];
	};

	const Uniforms uniforms = {SBW, DBW, SPSM, {}, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ((SPSM & 0xE) == 0) ? ShaderConvert::RGBA_TO_8I : ShaderConvert::RGB5A1_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), Nearest, true);
}


void GSDeviceVK::DoFilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct alignas(16) Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float step_multiplier;
		float pad1[2];
	};

	const Uniforms uniforms = {
		clamp_min, static_cast<int>(downsample_factor), 0, static_cast<float>(downsample_factor * downsample_factor), (GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive) ? 2.0f : 1.0f};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	//const GSVector4 dRect = GSVector4(dTex->GetRect());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), Nearest, true);
}

void GSDeviceVK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	GL_PUSH("DoMerge");

	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4] = {EXTBUF.EMODA, EXTBUF.EMODC};
	const GSVector4 bg_color = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const VkSampler& sampler = filter == Biln ? m_linear_sampler : m_point_sampler;
	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	EndRenderPass();

	// transition everything before starting the new render pass
	const bool has_input_0 = (sTex[0] &&
		(sTex[0]->GetState() == GSTexture::State::Dirty || (sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
		(sTex[1]->GetState() == GSTexture::State::Dirty || (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
	{
		static_cast<GSTextureVK*>(sTex[0])->CommitClear();
		static_cast<GSTextureVK*>(sTex[0])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	if (has_input_1)
	{
		static_cast<GSTextureVK*>(sTex[1])->CommitClear();
		static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		if (sTex[1]->GetState() == GSTexture::State::Dirty)
		{
			static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			OMSetRenderTargets(dTex, nullptr, darea);
			SetUtilityTexture(sTex[1], sampler);
			BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
			SetPipeline(GetConvertPipeline(ShaderConvert::COPY));
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dTex->SetState(GSTexture::State::Dirty);
			dcleared = true;
		}
	}

	// Upload constant to select YUV algo
	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);
		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetPipeline(GetConvertPipeline(ShaderConvert::YUV));
			SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();

		if (sTex[0] == sTex[2])
		{
			// need a barrier here because of the render pass
			static_cast<GSTextureVK*>(sTex[2])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(m_utility_color_render_pass_load, darea);
	}

	if (sTex[0] && sTex[0]->GetState() == GSTexture::State::Dirty)
	{
		// 1st output is enabled. It must be blended
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(m_merge[PMODE.MMOD]);
		SetUtilityPushConstants(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		SetPipeline(GetConvertPipeline(ShaderConvert::YUV));
		SetUtilityTexture(dTex, sampler);
		SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(m_utility_color_render_pass_load, fbarea);
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTextureVK*>(dTex)->CommitClear();
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityTexture(sTex, filter == Biln ? m_linear_sampler : m_point_sampler);
	BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(m_interlace[static_cast<int>(shader)]);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	const GSVector4i dRect = dTex->GetRect();
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, dRect);
	SetUtilityTexture(sTex, m_point_sampler);
	BeginRenderPass(m_utility_color_render_pass_discard, dRect);
	dTex->SetState(GSTexture::State::Dirty);
	SetPipeline(m_shadeboost_pipeline);
	SetUtilityPushConstants(params, sizeof(float) * 4);
	DrawStretchRect(sRect, GSVector4(dRect), dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	const GSVector4i dRect = dTex->GetRect();
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, dRect);
	SetUtilityTexture(sTex, m_linear_sampler);
	BeginRenderPass(m_utility_color_render_pass_discard, dRect);
	dTex->SetState(GSTexture::State::Dirty);
	SetPipeline(m_fxaa_pipeline);
	DrawStretchRect(sRect, GSVector4(dRect), dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

#ifdef ARMSX2_HAS_LIBRASHADER

static void ReportShaderChainError(const char* what, libra_error_t err)
{
	char* msg = nullptr;
	if (libra_error_write(err, &msg) == 0 && msg)
	{
		Console.Error("(GS) librashader %s failed: %s", what, msg);
		libra_error_free_string(&msg);
	}
	else
	{
		Console.Error("(GS) librashader %s failed (errno %d)", what, static_cast<int>(libra_error_errno(err)));
	}
	libra_error_free(&err);
}

#endif

void GSDeviceVK::DestroyShaderChain()
{
#ifdef ARMSX2_HAS_LIBRASHADER
	if (m_shader_chain)
	{
		libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(m_shader_chain);
		libra_vk_filter_chain_free(&chain);
		m_shader_chain = nullptr;
	}
#endif
	m_shader_chain_preset.clear();
	m_shader_chain_failed = false;
	m_shader_frame_count = 0;
	m_shader_param_generation = 0;
}

void GSDeviceVK::ApplyShaderChainParams()
{
#ifdef ARMSX2_HAS_LIBRASHADER
	// Per-frame fast path: one atomic load. The lock and the copy only happen on the
	// frames where the user actually moved something.
	const u64 generation = GetShaderChainParamGeneration();
	if (generation == m_shader_param_generation)
		return;

	std::vector<std::pair<std::string, float>> params;
	if (GetShaderChainParams(m_shader_chain_preset, &params))
	{
		libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(m_shader_chain);
		for (const auto& [name, value] : params)
		{
			// A preset can be swapped under a stale override set, so an unknown parameter
			// name is a routine miss, not a fault: report nothing and keep going, since
			// the remaining names are still valid.
			if (libra_error_t err = libra_vk_filter_chain_set_param(&chain, name.c_str(), value))
				libra_error_free(&err);
		}
	}

	// Set even when the store held another preset's values or none at all — otherwise this
	// re-runs the lookup on every frame for as long as the generation stays ahead.
	m_shader_param_generation = generation;
#endif
}

bool GSDeviceVK::DoApplyShaderChain(GSTexture* sTex, GSTexture* dTex)
{
#ifndef ARMSX2_HAS_LIBRASHADER
	return false;
#else
	// A preset that fails to compile must not be retried every frame — that would run a
	// full slang compile 60x/sec. Latch the failure until the user picks another preset.
	if (m_shader_chain_failed && m_shader_chain_preset == GSConfig.ShaderChainPreset)
		return false;

	if (!m_shader_chain || m_shader_chain_preset != GSConfig.ShaderChainPreset)
	{
		DestroyShaderChain();
		m_shader_chain_preset = GSConfig.ShaderChainPreset;

		libra_shader_preset_t preset = nullptr;
		if (libra_error_t err = libra_preset_create(m_shader_chain_preset.c_str(), &preset))
		{
			ReportShaderChainError("preset load", err);
			m_shader_chain_failed = true;
			return false;
		}

		libra_device_vk_t vk = {};
		vk.physical_device = m_physical_device;
		vk.instance = m_instance;
		vk.device = m_device;
		vk.queue = m_graphics_queue;
		// librashader resolves Vulkan through this loader rather than linking it, which
		// is why it transparently rides a user's custom Turnip ICD.
		vk.entry = vkGetInstanceProcAddr;

		// create() invalidates `preset` unconditionally ("the shader preset is
		// immediately invalidated"), so it must NOT be freed afterwards on either path.
		libra_vk_filter_chain_t chain = nullptr;
		if (libra_error_t err = libra_vk_filter_chain_create(&preset, vk, nullptr, &chain))
		{
			ReportShaderChainError("chain create", err);
			m_shader_chain_failed = true;
			return false;
		}

		m_shader_chain = chain;
		m_shader_frame_count = 0;
		// The new chain sits at the preset's initial values, so whatever we last pushed is
		// gone with the old one — force ApplyShaderChainParams to feed it again.
		m_shader_param_generation = 0;
		Console.WriteLn("(GS) librashader: loaded preset '%s'", m_shader_chain_preset.c_str());
	}

	// GS thread, chain alive, before the frame call — the only place a set_param is safe.
	ApplyShaderChainParams();

	GSTextureVK* const src = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dst = static_cast<GSTextureVK*>(dTex);

	// The chain records its own render passes, so it must not run inside one of ours.
	EndRenderPass();

	// librashader's contract: source in SHADER_READ_ONLY_OPTIMAL, target in
	// COLOR_ATTACHMENT_OPTIMAL.
	src->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	dst->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const libra_image_vk_t in = {src->GetImage(), src->GetVkFormat(),
		static_cast<uint32_t>(src->GetWidth()), static_cast<uint32_t>(src->GetHeight())};
	const libra_image_vk_t out = {dst->GetImage(), dst->GetVkFormat(),
		static_cast<uint32_t>(dst->GetWidth()), static_cast<uint32_t>(dst->GetHeight())};
	const libra_viewport_t vp = {0.0f, 0.0f,
		static_cast<uint32_t>(dst->GetWidth()), static_cast<uint32_t>(dst->GetHeight())};

	// Every librashader entry point takes the chain handle by address, not by value.
	libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(m_shader_chain);
	if (libra_error_t err = libra_vk_filter_chain_frame(&chain, GetCurrentCommandBuffer(),
			m_shader_frame_count, in, out, &vp, nullptr, nullptr))
	{
		ReportShaderChainError("frame", err);
		m_shader_chain_failed = true;
		return false;
	}
	m_shader_frame_count++;

	// The chain left the target in COLOR_ATTACHMENT_OPTIMAL behind the tracker's back, so
	// resync it WITHOUT emitting a barrier (Override), then transition for real to the
	// ShaderReadOnly that DoFXAA/DoShadeBoost also leave behind and the presenter expects.
	// Skipping the Override would make the next barrier start from a stale layout.
	dst->OverrideImageLayout(GSTextureVK::Layout::ColorAttachment);
	dst->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	dst->SetState(GSTexture::State::Dirty);

	// librashader recycles its per-frame objects (VkImageView / VkFramebuffer / descriptor
	// sets) on its OWN internal frame counter, over a `frames_in_flight`-deep ring
	// (default 3): frame N destroys what frame N-3 recorded. That is only safe if every
	// frame() call is followed by a submit, so the fence for N-3 has been waited by the
	// time N recycles its slot.
	//
	// PCSX2 violates that. GSRenderer::VSync calls Merge() -- and therefore the chain --
	// BEFORE it decides whether to present, and a skipped present (SkipDuplicateFrames,
	// which is default-on, or the FIFO present throttle) returns early from DoBeginPresent
	// and never reaches EndPresent, so it never submits. MAX_SKIPPED_DUPLICATE_FRAMES is
	// 3 -- exactly the ring depth -- so three skipped frames in a row let librashader
	// destroy views that are still bound to the command buffer we are STILL recording.
	// Validation names it: VUID-vkDestroyImageView-imageView-01026, followed by the
	// buffer going invalid and the Adreno driver segfaulting as it walks it at submit.
	//
	// Kicking the buffer here keeps exactly one submit per chain frame, so librashader's
	// ring and our NUM_COMMAND_BUFFERS ring advance together. The GL backend is immune
	// because it executes immediately and has no recorded buffer to go stale.
	ExecuteCommandBuffer(false);
	return true;
#endif
}

void GSDeviceVK::IASetVertexBuffer(const void* vertex, size_t stride, size_t count, size_t align_multiplier)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride) * align_multiplier))
	{
		// While presenting, the swap-chain pass isn't tracked by m_current_render_pass, so the ordinary
		// render-pass restart would leave the command buffer with an open present pass. From EmuCoreX.
		if (m_is_presenting)
			ExecuteCommandBufferAndRestartPresent(false, "Uploading bytes to vertex buffer");
		else
			ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride) * align_multiplier))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceVK::UploadIndices(VKStreamBuffer& buffer, const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
	if (!buffer.ReserveMemory(size, sizeof(u16)))
	{
		if (m_is_presenting)
			ExecuteCommandBufferAndRestartPresent(false, "Uploading bytes to index buffer");
		else
			ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to index buffer");
		if (!buffer.ReserveMemory(size, sizeof(u16)))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_index.start = buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;

	std::memcpy(buffer.GetCurrentHostPointer(), index, size);
	buffer.CommitMemory(size);
}

void GSDeviceVK::IASetIndexBuffer(const void* index, size_t count)
{
	UploadIndices(m_index_stream_buffer, index, count);

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
}

void GSDeviceVK::VSSetIndexBuffer(const void* index, size_t count)
{
	UploadIndices(m_expand_index_stream_buffer, index, count);
}

void GSDeviceVK::OMSetRenderTargets(
	GSTexture* rt, GSTexture* ds, const GSVector4i& scissor, FeedbackLoopFlag feedback_loop,
	const GSVector2i& viewport_size)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);

	if (m_current_render_target != vkRt || m_current_depth_target != vkDs ||
		m_current_framebuffer_feedback_loop != feedback_loop ||
		m_current_framebuffer == VK_NULL_HANDLE)
	{
		// framebuffer change or feedback loop enabled/disabled
		EndRenderPass();

		if (vkRt)
		{
			m_current_framebuffer =
				vkRt->GetLinkedFramebuffer(vkDs,
					(feedback_loop & FeedbackLoopFlag_ReadAndWriteRT) != 0,
					(feedback_loop & (FeedbackLoopFlag_ReadAndWriteDepth | FeedbackLoopFlag_ReadDepth)) != 0,
					(feedback_loop & FeedbackLoopFlag_TileGpuSelfRead) != 0);
		}
		else if (vkDs)
		{
			pxAssert(!(feedback_loop & (FeedbackLoopFlag_ReadAndWriteRT | FeedbackLoopFlag_TileGpuSelfRead)));
			m_current_framebuffer = vkDs->GetLinkedFramebuffer(
				nullptr, false, (feedback_loop & (FeedbackLoopFlag_ReadAndWriteDepth | FeedbackLoopFlag_ReadDepth)) != 0);
		}
		else
		{
			m_current_framebuffer = m_null_framebuffer;
		}
	}
	else if (InRenderPass())
	{
		// Framebuffer unchanged, but check for clears
		// Use an attachment clear to wipe it out without restarting the render pass
		if (IsDeviceNVIDIA())
		{
			// Using vkCmdClearAttachments() within a render pass on NVIDIA seems to cause dependency issues
			// between draws that are testing depth which precede it. The result is flickering where Z tests
			// should be failing. Breaking/restarting the render pass isn't enough to work around the bug,
			// it needs an explicit pipeline barrier.
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkRt->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkRt->GetLayout(), vkRt->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkRt->SetState(GSTexture::State::Dirty);
				}
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkDs->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkDs->GetLayout(), vkDs->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkDs->SetState(GSTexture::State::Dirty);
				}
			}
		}
		else
		{
			std::array<VkClearAttachment, 2> cas;
			u32 num_ca = 0;
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					ca.colorAttachment = 0;
					GSVector4::store<false>(ca.clearValue.color.float32, vkRt->GetClearForFormat());
				}

				vkRt->SetState(GSTexture::State::Dirty);
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					ca.colorAttachment = 1;
					ca.clearValue.depthStencil = {vkDs->GetClearDepth()};
				}

				vkDs->SetState(GSTexture::State::Dirty);
			}

			if (num_ca > 0)
			{
				const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
				const VkClearRect cr = {{{0, 0}, {static_cast<u32>(size.x), static_cast<u32>(size.y)}}, 0u, 1u};
				vkCmdClearAttachments(GetCurrentCommandBuffer(), num_ca, cas.data(), 1, &cr);
			}
		}
	}

	m_current_render_target = vkRt;
	m_current_depth_target = vkDs;
	m_current_framebuffer_feedback_loop = feedback_loop;

	if (!InRenderPass())
	{
		if (vkRt)
		{
			// The declared in-pass read wants exactly what the feedback loop wants of the colour target:
			// the GENERAL layout, and the two Adreno quirks below (a self-read of a CLEAR or DONT_CARE
			// load op reads garbage there).
			if (feedback_loop & (FeedbackLoopFlag_ReadAndWriteRT | FeedbackLoopFlag_TileGpuSelfRead))
			{
				// ⚠️ A pass cannot read what it did not LOAD. On the declared-read road that is not a
				// vendor quirk but the contract: a DONT_CARE load op leaves the attachment undefined at
				// pass start, so the first fragment's read of it is undefined too, and a CLEAR load op
				// hands some drivers garbage instead of the clear value. Both were already answered here
				// for Classic's feedback loop, gated on the two vendors that were observed doing it;
				// TileGpu's declared read needs the answer on every device, so the gate widens for it.
				// Measured on a software rasterizer: with the target left Invalidated, the destination-alpha
				// test read undefined content and the SotC composite came out 56.3 mean levels from the
				// software golden against the snapshot road's 6.3.
				const bool declared = (feedback_loop & FeedbackLoopFlag_TileGpuSelfRead) != 0;
				if (vkRt->GetState() == GSTexture::State::Cleared && (declared || IsDeviceNVIDIA() || IsDeviceAdreno()))
					vkRt->CommitClear();
				else if (vkRt->GetState() == GSTexture::State::Invalidated && (declared || IsDeviceAdreno()))
					vkRt->SetState(GSTexture::State::Dirty);

				if (vkRt->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					// need to update descriptors to reflect the new layout
					m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_RT);
					if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkRt)
						m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE;
					vkRt->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else
			{
				vkRt->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);
			}
		}
		if (vkDs)
		{
			// need to update descriptors to reflect the new layout
			if (feedback_loop & FeedbackLoopFlag_ReadAndWriteDepth)
			{
				// NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
				// the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
				// Note: DS feedback loop was added later - we will assume that the same issue is relevant.
				// Adreno/turnip has the same garbage-on-CLEAR feedback read.
				if (vkDs->GetState() == GSTexture::State::Cleared && (IsDeviceNVIDIA() || IsDeviceAdreno()))
					vkDs->CommitClear();

				if (vkDs->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_DEPTH);
					if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkDs)
						m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE;
					vkDs->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else if (feedback_loop & FeedbackLoopFlag_ReadDepth)
			{
				const GSTextureVK::Layout layout = m_features.depth_feedback ?
					GSTextureVK::Layout::FeedbackLoop : GSTextureVK::Layout::General;
				if (vkDs->GetLayout() != layout)
				{
					m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE);
					if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkDs)
						m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE;
					vkDs->TransitionToLayout(layout);
				}
			}
			else
			{
				vkDs->TransitionToLayout(GSTextureVK::Layout::DepthStencilAttachment);
			}
		}
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = (vkRt ? vkRt->GetSize() : (vkDs ? vkDs->GetSize() : viewport_size));
	const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};

	SetViewport(vp);
	SetScissor(scissor);
}

VkSampler GSDeviceVK::GetSampler(GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	// See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
	// for the reasoning behind 0.25f here.
	const VkSamplerCreateInfo ci = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0,
		ss.IsMagFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // min
		ss.IsMinFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // mag
		ss.IsMipFilterLinear() ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, // mip
		static_cast<VkSamplerAddressMode>(
			ss.tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // u
		static_cast<VkSamplerAddressMode>(
			ss.tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // v
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // w
		0.0f, // lod bias
		VK_FALSE, // anisotropy enable
		1.0f, // anisotropy
		VK_FALSE, // compare enable
		VK_COMPARE_OP_ALWAYS, // compare op
		0.0f, // min lod
		(ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : VK_LOD_CLAMP_NONE, // max lod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // border
		VK_FALSE // unnormalized coordinates
	};
	VkSampler sampler = VK_NULL_HANDLE;
	VkResult res = vkCreateSampler(m_device, &ci, nullptr, &sampler);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkCreateSampler() failed: ");

	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

void GSDeviceVK::ClearSamplerCache()
{
	ExecuteCommandBuffer(true);
	for (const auto& it : m_samplers)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroySampler(m_device, it.second, nullptr);
	}
	m_samplers.clear();
	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_utility_sampler = m_point_sampler;
	m_tfx_sampler = m_point_sampler;
}

static void AddMacro(std::stringstream& ss, const char* name, int value)
{
	ss << "#define " << name << " " << value << "\n";
}

static void AddShaderHeader(std::stringstream& ss)
{
	const GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const GSDevice::FeatureSupport features = dev->Features();

	ss << "#version 460 core\n";
	ss << "#extension GL_EXT_samplerless_texture_functions : require\n";

	// Mali driver-bug shader gate (currently the EQUAL_WZ_CORRUPTS_DEPTH z-nudge in
	// tfx.glsl). 1 only on Mali; the guarded code compiles out on every other GPU.
	ss << "#define GPU_PROFILE_MALI " << (dev->IsDeviceMali() ? 1 : 0) << "\n";

	if (!features.texture_barrier)
		ss << "#define DISABLE_TEXTURE_BARRIER 1\n";
	if (features.texture_barrier && dev->UseFeedbackLoopLayout())
		ss << "#define HAS_FEEDBACK_LOOP_LAYOUT 1\n";
	if (features.rov)
	{
		ss << "#extension GL_ARB_fragment_shader_interlock : require\n";
		ss << "#extension GL_ARB_shader_image_load_store : require\n";
	}

	// Shader-compiler workarounds from the driver-bug database (ported from EmuCoreX/sashkinbro
	// with his approval). Both default to 0, so the generated SPIR-V is unchanged on any driver
	// the database has no rule for. Emitted after the #extension directives above because GLSL
	// wants those before any real code, and the wrapper bodies below are real code.
	AddMacro(ss, "DRIVER_SCALARIZE_VECTOR_BITWISE_AND",
		dev->UsesMobileDriverWorkaround(DriverWorkaround::ScalarizeVectorBitwiseAnd) ? 1 : 0);
	AddMacro(ss, "DRIVER_REWRITE_UNIFORM_INDEXING",
		dev->UsesMobileDriverWorkaround(DriverWorkaround::RewriteUniformIndexing) ? 1 : 0);
	// When no workaround is active these MUST expand to the bare operator, not to a function that
	// happens to return it. Overloads cost an OpFunctionCall in the SPIR-V at every call site --
	// including inside the texture loop in tfx.glsl and the region-clamp path -- and Qualcomm's
	// SPIR-V compiler segfaults building a TFX pipeline from that shape (LEGO Batman, Adreno 740,
	// driver 512.676.53: SIGSEGV inside CreateQGLCProgram, chained to SIGABRT on the GS thread).
	// OpenGL is unaffected because it hands GLSL straight to the driver and never goes through
	// SPIR-V, which is why the same build renders that game fine on the GL renderer.
	//
	// This also makes good on what the wrappers were introduced promising -- that a driver the
	// database has no rule for gets unchanged SPIR-V. It did not hold: the function wrapper was
	// emitted unconditionally, so EVERY driver got new shader structure to please the two that
	// needed it.
	ss << R"(
#if DRIVER_SCALARIZE_VECTOR_BITWISE_AND
uvec2 gpu_bitwise_and(uvec2 a, uvec2 b)
{
	return uvec2(a.x & b.x, a.y & b.y);
}

uvec3 gpu_bitwise_and(uvec3 a, uvec3 b)
{
	return uvec3(a.x & b.x, a.y & b.y, a.z & b.z);
}

uvec4 gpu_bitwise_and(uvec4 a, uvec4 b)
{
	return uvec4(a.x & b.x, a.y & b.y, a.z & b.z, a.w & b.w);
}

ivec3 gpu_bitwise_and(ivec3 a, ivec3 b)
{
	return ivec3(a.x & b.x, a.y & b.y, a.z & b.z);
}
#else
#define gpu_bitwise_and(a, b) ((a) & (b))
#endif

#if DRIVER_REWRITE_UNIFORM_INDEXING
float gpu_matrix_element(mat4 value, int column, int row)
{
	vec4 selected_column;
	if (column == 0)
		selected_column = value[0];
	else if (column == 1)
		selected_column = value[1];
	else if (column == 2)
		selected_column = value[2];
	else
		selected_column = value[3];

	if (row == 0)
		return selected_column[0];
	if (row == 1)
		return selected_column[1];
	if (row == 2)
		return selected_column[2];
	return selected_column[3];
}
#else
#define gpu_matrix_element(value, column, row) ((value)[(column)][(row)])
#endif
)";
}

static void AddShaderStageMacro(std::stringstream& ss, bool vs, bool gs, bool fs)
{
	if (vs)
		ss << "#define VERTEX_SHADER 1\n";
	else if (gs)
		ss << "#define GEOMETRY_SHADER 1\n";
	else if (fs)
		ss << "#define FRAGMENT_SHADER 1\n";
}

static void AddUtilityVertexAttributes(Vulkan::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexBuffer(0, sizeof(GSVertexPT1));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

static void SetPipelineProvokingVertex(const GSDevice::FeatureSupport& features, Vulkan::GraphicsPipelineBuilder& gpb)
{
	// We enable provoking vertex here anyway, in case it doesn't support multiple modes in the same pass.
	// Normally we wouldn't enable it on the present/swap chain, but apparently the rule is it applies to the last
	// pipeline bound before the render pass begun, and in this case, we can't bind null.
	if (features.provoking_vertex_last)
		gpb.SetProvokingVertex(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
}

VkShaderModule GSDeviceVK::GetUtilityVertexShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetVertexShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityFragmentShader(
	const std::string& source, const char* replace_main = nullptr, u32* out_spv_words = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetFragmentShader(ss.str(), out_spv_words);
}

bool GSDeviceVK::CreateNullTexture()
{
	m_null_texture = GSTextureVK::Create(GSTexture::ShaderWriteTarget, GSTexture::Format::Color, 1, 1, 1);
	if (!m_null_texture)
		return false;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	const VkClearColorValue ccv{};
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ClearDst);
	vkCmdClearColorImage(cmdbuf, m_null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::General);
	Vulkan::SetObjectName(m_device, m_null_texture->GetImage(), "Null texture");
	Vulkan::SetObjectName(m_device, m_null_texture->GetView(), "Null texture view");

	return true;
}

bool GSDeviceVK::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_features.vs_expand ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
			VERTEX_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate vertex buffer");
		return false;
	}

	if (!m_index_stream_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate index buffer");
		return false;
	}

	if (!m_expand_index_stream_buffer.Create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_features.aa1 ? INDEX_BUFFER_SIZE : 4))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate expansion index buffer (VS resource)");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate vertex uniform buffer");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate fragment uniform buffer");
		return false;
	}

	if (!m_texture_stream_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate texture upload buffer");
		return false;
	}

	if (!AllocatePreinitializedGPUBuffer(EXPAND_BUFFER_SIZE, &m_expand_index_buffer, &m_expand_index_buffer_allocation,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &GSDevice::GenerateExpansionIndexBuffer))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate expansion index buffer");
		return false;
	}

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
	return true;
}

bool GSDeviceVK::CreatePipelineLayouts()
{
	VkDevice dev = m_device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////

	if (m_use_push_descriptors)
		dslb.SetPushFlag();
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NUM_UTILITY_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_utility_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_utility_ds_layout, "Convert descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
	plb.AddDescriptorSet(m_utility_ds_layout);
	if ((m_utility_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_utility_pipeline_layout, "Convert pipeline layout");

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	dslb.AddBinding(
		0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (m_features.vs_expand)
	{
		// The fragment stage also reads this binding: the Tile depth walk streams
		// its per-primitive plane payload through the same buffer (PS_TILE_ZWALK).
		dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GSHWDrawConfig::VSPushConstants));
	}
	if (m_features.aa1)
		dslb.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_tfx_ubo_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_tfx_ubo_ds_layout, "TFX UBO descriptor layout");

	if (m_use_push_descriptors)
		dslb.SetPushFlag();
	dslb.AddBinding(TFX_TEXTURE_TEXTURE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_PALETTE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		// Mali needs real input-attachment descriptors for the subpass feedback path; its shader reads
	// Cd/Zd via subpassLoad. Adreno mishandles input-attachment descriptors here and produces
	// alternating stale destination colour (flicker) in accurate-blending draws, so keep Qualcomm on
	// the long-standing sampled-image descriptor — subpassLoad still reads the render-pass input
	// attachment (not vendor-scoped). Keep this condition identical to the writes in ApplyTFXState.
	// Vendor-scoped per sashkinbro/EmuCoreX 30b09c8 (Fix Adreno Vulkan accurate blending flicker).
	const VkDescriptorType feedback_descriptor_type =
		(m_features.texture_barrier && !UseFeedbackLoopLayout() && m_device_properties.vendorID == 0x13B5u) ?
			VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT :
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	dslb.AddBinding(TFX_TEXTURE_RT, feedback_descriptor_type, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_PRIMID, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_DEPTH, feedback_descriptor_type, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_RT_ROV, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_DEPTH_ROV, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_texture_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_tfx_texture_ds_layout, "TFX texture descriptor layout");

	plb.AddDescriptorSet(m_tfx_ubo_ds_layout);
	plb.AddDescriptorSet(m_tfx_texture_ds_layout);
	if ((m_tfx_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_tfx_pipeline_layout, "TFX pipeline layout");
	return true;
}

bool GSDeviceVK::CreateRenderPasses()
{
#define GET(dest, rt, depth, fbl, dsp, opa, opb, opc) \
	do \
	{ \
		dest = GetRenderPass( \
			(rt), (depth), ((rt) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* color load */ \
			((rt) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
											VK_ATTACHMENT_STORE_OP_DONT_CARE, /* color store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opb) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* depth load */ \
			((depth) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
											   VK_ATTACHMENT_STORE_OP_DONT_CARE, /* depth store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opc) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* stencil load */ \
			VK_ATTACHMENT_STORE_OP_DONT_CARE, /* stencil store */ \
			(fbl), /* feedback loop */ \
			(dsp) /* depth sampling */ \
		); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)

	const VkFormat rt_format = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat colclip_rt_format = LookupNativeFormat(GSTexture::Format::ColorClip);
	const VkFormat depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);

	for (u32 rt = 0; rt < 2; rt++)
	{
		for (u32 ds = 0; ds < 2; ds++)
		{
			for (u32 colclip = 0; colclip < 2; colclip++)
			{
				for (u32 stencil = 0; stencil < 2; stencil++)
				{
					for (u32 fbl = 0; fbl < 2; fbl++)
					{
						for (u32 dsp = 0; dsp < 2; dsp++)
						{
							for (u32 opa = VK_ATTACHMENT_LOAD_OP_LOAD; opa <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opa++)
							{
								for (u32 opb = VK_ATTACHMENT_LOAD_OP_LOAD; opb <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opb++)
								{
									const VkFormat rp_rt_format =
										(rt != 0) ? ((colclip != 0) ? colclip_rt_format : rt_format) : VK_FORMAT_UNDEFINED;
									const VkFormat rp_depth_format = (ds != 0) ? depth_format : VK_FORMAT_UNDEFINED;
									const VkAttachmentLoadOp opc = (!stencil || !m_features.stencil_buffer) ?
									                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE :
									                                   VK_ATTACHMENT_LOAD_OP_LOAD;
									GET(m_tfx_render_pass[rt][ds][colclip][stencil][fbl][dsp][opa][opb], rp_rt_format,
										rp_depth_format, (fbl != 0), (dsp != 0), static_cast<VkAttachmentLoadOp>(opa),
										static_cast<VkAttachmentLoadOp>(opb), static_cast<VkAttachmentLoadOp>(opc));
								}
							}
						}
					}
				}
			}
		}
	}

	GET(m_utility_color_render_pass_load, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_clear, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_discard, rt_format, VK_FORMAT_UNDEFINED, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_clear, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

	m_date_setup_render_pass = GetRenderPass(VK_FORMAT_UNDEFINED, depth_format, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		m_features.stencil_buffer ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		m_features.stencil_buffer ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
	if (m_date_setup_render_pass == VK_NULL_HANDLE)
		return false;

#undef GET

	return true;
}

bool GSDeviceVK::CompileConvertPipelines()
{
	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/convert.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/convert.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*source);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);

	m_convert.resize(ShaderConvertSelector::NUM_TOTAL_SHADERS);
	for (u32 i = 0; i < ShaderConvertSelector::NUM_TOTAL_SHADERS; i++)
	{
		const ShaderConvertSelector shader = ShaderConvertSelector::Get(i);

		VkRenderPass rp;
		if (shader.DATMConvertShader())
		{
			rp = m_date_setup_render_pass;
		}
		else if (shader.DepthOutput())
		{
			rp = GetRenderPass(
				LookupNativeFormat(GSTexture::Format::Invalid),
				LookupNativeFormat(GSTexture::Format::DepthStencil),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}
		else
		{
			rp = GetRenderPass(
				LookupNativeFormat(shader.OutputFormat()),
				LookupNativeFormat(GSTexture::Format::Invalid),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}

		if (!rp)
			return false;

		gpb.SetRenderPass(rp, 0);

		if (shader.DATMConvertShader())
		{
			const VkStencilOpState sos = {
				VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 1u, 1u, 1u};
			gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
			gpb.SetStencilState(true, sos, sos);
		}
		else
		{
			gpb.SetDepthState(shader.DepthOutput(), shader.DepthOutput(), VK_COMPARE_OP_ALWAYS);
			gpb.SetNoStencilState();
		}

		gpb.SetColorWriteMask(0, shader.Mask());

		std::string macro;
		macro += fmt::format("#define HAS_BILN {}\n", static_cast<int>(shader.Biln()));
		macro += fmt::format("#define HAS_STENCIL_OUTPUT {}\n", static_cast<int>(shader.StencilOutput()));
		macro += fmt::format("#define HAS_INTEGER_OUTPUT {}\n", static_cast<int>(shader.IntegerOutputBpp() != 0));
		macro += fmt::format("#define HAS_DEPTH_OUTPUT {}\n", static_cast<int>(shader.DepthOutput()));
		macro += fmt::format("#define HAS_FLOAT32_INPUT {}\n", static_cast<int>(shader.Float32Input()));
		macro += fmt::format("#define HAS_FLOAT32_OUTPUT {}\n", static_cast<int>(shader.Float32Output()));

		std::string shader_with_header = macro + *source;

		VkShaderModule ps = GetUtilityFragmentShader(shader_with_header, shader.EntryPoint());
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetFragmentShader(ps);

		VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
					
		if (!pipe)
			return false;

		m_convert[i] = pipe;

		Vulkan::SetObjectName(m_device, pipe, "Convert pipeline (%s, mask=%x, depth=%d, biln=%d)",
			shader.Name(), shader.Mask(), static_cast<int>(shader.DepthOutput()),
			static_cast<int>(shader.Biln()));

		if (shader.Shader() == ShaderConvert::COLCLIP_INIT || shader.Shader() == ShaderConvert::COLCLIP_RESOLVE)
		{
			const bool is_setup = shader.Shader() == ShaderConvert::COLCLIP_INIT;
			VkPipeline(&arr)[2][2] = *(is_setup ? &m_colclip_setup_pipelines : &m_colclip_finish_pipelines);
			for (u32 ds = 0; ds < 2; ds++)
			{
				for (u32 fbl = 0; fbl < 2; fbl++)
				{
					pxAssert(!arr[ds][fbl]);

					gpb.SetRenderPass(GetTFXRenderPass(true, ds != 0, is_setup, false, fbl != 0, false,
						VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE),
						0);
					// The feedback-loop (fbl) render pass carries the RASTERIZATION_ORDER_ATTACHMENT subpass flag
					// when framebuffer fetch is available; the pipeline bound in it must declare the matching
					// color-blend rasterization-order flag or the coherent self-read is undefined. Set (overwrite,
					// not OR, so fbl=0 stays unflagged).
					gpb.SetBlendFlags((fbl != 0 && m_features.framebuffer_fetch)
							? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT
							: 0);
					arr[ds][fbl] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
					if (!arr[ds][fbl])
						return false;

					Vulkan::SetObjectName(m_device, arr[ds][fbl], "ColorClip %s/copy pipeline (ds=%u, fbl=%u)",
						is_setup ? "setup" : "finish", i, ds, fbl);
				}
			}
			// gpb is reused for subsequent convert shaders; clear the rasterization-order flag we set above so it
			// does not leak onto non-feedback convert pipelines (invalid on a non-RASTER_ORDER render pass).
			gpb.SetBlendFlags(0);
		}
	}

	// date image setup
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 clear = 0; clear < 2; clear++)
		{
			m_primid_image_setup_render_passes[ds][clear] = GetRenderPass(LookupNativeFormat(GSTexture::Format::PrimID),
				ds ? LookupNativeFormat(GSTexture::Format::DepthStencil) : VK_FORMAT_UNDEFINED,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
				ds ? (clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD) :
					 VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				ds ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
		}
	}

	for (u32 datm = 0; datm < 4; datm++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_primid_image_init_%d", datm));
		VkShaderModule ps =
			GetUtilityFragmentShader(*source, entry_point.c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetPipelineLayout(m_utility_pipeline_layout);
		gpb.SetFragmentShader(ps);
		gpb.SetNoDepthTestState();
		gpb.SetNoStencilState();
		gpb.ClearBlendAttachments();
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);

		for (u32 ds = 0; ds < 2; ds++)
		{
			gpb.SetRenderPass(m_primid_image_setup_render_passes[ds][0], 0);
			m_primid_image_setup_pipelines[ds][datm] =
				gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_primid_image_setup_pipelines[ds][datm])
				return false;

			Vulkan::SetObjectName(m_device, m_primid_image_setup_pipelines[ds][datm],
				"DATE image clear pipeline (ds=%u, datm=%u)", ds, (datm == 1 || datm == 3));
		}
	}

	return true;
}

// The Tile renderer's reinterpretation of a render target as an indexed texture
// (tile_convert.glsl). Compiled on first use rather than with the convert
// pipelines: only the Tile renderer asks, and the fragment source is assembled at
// runtime — the swizzle forms are FITTED from GSTables.cpp and prepended as
// defines, so a table that stopped fitting disables the route here instead of
// addressing the wrong bytes.
const std::string& GSDeviceVK::TileFormDefines()
{
	if (!m_tile_forms_fitted)
	{
		m_tile_forms = GSTileSwizzleForms::Fit();
		m_tile_forms_fitted = true;
		m_tile_form_defines = GSTileSwizzleForms::ShaderDefines(m_tile_forms);
		if (!m_tile_forms.valid)
			Console.Error("GS/Tile: the page swizzle tables no longer fit closed forms; device-side reinterpretation is off.");
		if (!m_tile_forms.clut_valid)
			Console.Error("GS/Tile: the CLUT loaders' word order no longer fits a closed form; device-side CLUT gather is off.");
	}
	return m_tile_form_defines;
}

bool GSDeviceVK::TileSwizzleFormsFit(bool& clut_ok)
{
	TileFormDefines();
	clut_ok = m_tile_forms.clut_valid;
	return m_tile_forms.valid;
}

bool GSDeviceVK::CompileTileReinterpretPipelines()
{
	m_tile_reinterpret_tried = true;

	const std::string& defines = TileFormDefines();
	const GSTileSwizzleForms::FormSet& forms = m_tile_forms;
	if (!forms.valid)
		return false;

	const std::optional<std::string> vsource = ReadShaderSource("shaders/vulkan/convert.glsl");
	const std::optional<std::string> fsource = ReadShaderSource("shaders/vulkan/tile_convert.glsl");
	if (!vsource || !fsource)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tile_convert.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*vsource);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);
	gpb.SetRenderPass(GetRenderPass(LookupNativeFormat(GSTexture::Format::Color),
						  LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_DONT_CARE),
		0);
	gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetColorWriteMask(0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

	for (u32 i = 0; i < m_tile_reinterpret.size(); i++)
	{
		// 0..4: the index formats; 5 and 6: the CLUT gather at 16 and 256 entries. The
		// gather needs the CLUT loaders' word order to have fitted as well.
		const bool clut = i >= 5;
		if (clut && !forms.clut_valid)
			continue;
		const u32 fmt = clut ? 0 : i;
		const u32 entries = (i == 5) ? 16 : 256;
		const std::string source = defines +
								   fmt::format("#define TILE_IDX_FMT {}\n#define TILE_CLUT_ENTRIES {}\n", fmt, entries) +
								   *fsource;
		VkShaderModule ps = GetUtilityFragmentShader(source, clut ? "ps_tile_clut_from_target" : "ps_tile_reinterpret_index");
		if (ps == VK_NULL_HANDLE)
			return false;
		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetFragmentShader(ps);

		VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!pipe)
			return false;
		m_tile_reinterpret[i] = pipe;
		if (clut)
			Vulkan::SetObjectName(m_device, pipe, "Tile CLUT-gather pipeline (entries=%u)", entries);
		else
			Vulkan::SetObjectName(m_device, pipe, "Tile reinterpret-index pipeline (fmt=%u)", fmt);
	}
	return true;
}

bool GSDeviceVK::TileGpuExecutorAvailable()
{
	return m_optional_extensions.tilegpu_device_capable;
}

// The sampled-target array is a fixed-size array of combined samplers whose index is the draw's
// slot (see tilegpu.glsl's tilegpu_target_texel), so it needs one feature beyond the executor's
// own contract: dynamic indexing of a sampled-image array. NOT the non-uniform sub-feature -- the
// executor splits its indirect calls at a slot change, which makes the index wave-uniform by
// construction. CreateDevice decided that; answered here rather than assumed by the renderer,
// because this is the seam a device that cannot serve it refuses at -- and the shader compiled for
// such a device has no target array in it at all.
bool GSDeviceVK::TileGpuBindlessTargets()
{
	return m_optional_extensions.tilegpu_bindless_targets;
}

// The declared in-pass destination read. Asked once, before any target is allocated, because the
// answer changes how a colour target is CREATED: the read is an input attachment, so the image
// needs that usage bit from the start and no later discovery can add it.
bool GSDeviceVK::TileGpuDualSourceBlend()
{
	return m_optional_extensions.tilegpu_dual_source;
}

bool GSDeviceVK::TileGpuSelfRead()
{
	return m_optional_extensions.tilegpu_self_read;
}

// Adreno wants its passes depth-uniform, and it is the only architecture measured that does.
//
// Merging draws that differ only in the depth write-enable into one pass, with the executor
// rebinding the depth pipeline per indirect run, is a 3-7x win on a tiler (M2/Honeykrisp: Shadow of
// the Colossus 574.95 passes a frame down to 76.95, 35.0 ms down to 10.4) and a LOSS here. A
// three-arm run on an SD865 (Adreno 650, Turnip, 2026-08-23) put the merged arm slowest or
// near-slowest on all four dumps -- sotc 27.14 vs 25.91 ms, sotc-gate 30.69 vs 29.42, mgs3 29.49 vs
// 19.79, ac3 27.73 vs 18.68 -- while the pass-count deltas matched the planner's model exactly, so
// the passes really are saved and something else more than eats them.
//
// ⚠️ What that something is has NOT been established, and this comment deliberately names no
// mechanism. Early-Z rejection was the first hypothesis and it is refuted (TU_DEBUG=nolrz moves
// neither arm); a CPU-side cause is ruled out too. The investigation is open. What is not in doubt
// is the measurement, and this gate exists to keep each architecture on the arm that measured
// faster on it rather than on the arm a story predicts.
//
// Gated on the VENDOR rather than the driver, unlike the driver-bug workarounds elsewhere in this
// file, because nothing so far points at the driver: the merged arm loses by a similar margin on
// dumps whose Turnip-side behaviour is otherwise very different. The proprietary blob is therefore
// given the same treatment and is UNMEASURED -- if it turns out to behave like a tiler, this is
// where that gets narrowed.
bool GSDeviceVK::TileGpuPrefersDepthUniformPasses()
{
	return IsDeviceAdreno();
}

// Adreno charges for pass LENGTH, so a pass here holds at most 64 draws and the 65th opens another
// with the same key.
//
// Two independent instruments put the number in the same place, which is the reason it is 64 and not
// a round guess:
//
//  1. A pass-size sweep on an SD865 (Turnip, 2026-08-24) drove the cap from unset down to 1 draw and
//     read the frame time at each stop. The driver's occlusion-query autotune term -- worth +2.69 ms
//     on MGS3 and +9.05 ms on Armored Core 3 with no cap -- is ENTIRELY gone by 64, even though a
//     64-draw pass is far above the driver's own 5-draw instrumentation gate. So the cost was never
//     per-pass; it is concentrated in the handful of giant passes a frame has. Under shipping
//     conditions the cap wins ac3 27.26 -> 18.48 ms and mgs3 29.35 -> 26.58.
//  2. A CPU-side structural census of the depth-policy A/B, on a different machine with no device
//     access, found that the fraction of a frame's fill relocated into passes of >= 64 draws is what
//     ranks that A/B's per-title GPU outcome (rho +0.86, sign correct on both improving titles), and
//     swept the threshold: the correlation peaks at 64.
//
// ⚠️ CONSERVATIVE ON PURPOSE. Smaller caps are not better: below 8 draws the same sweep HARMS two of
// the three dumps it covered (ac3 +34.6%, sotc +9.1%), because each split costs a per-pass state
// re-emit of a few microseconds and a frame capped at 1 draw pays it thousands of times. MGS3 keeps
// falling all the way to N=1 (-39%) and nothing else does; that is something MGS3's passes contain,
// not a size law, and it is not what this number is set from.
//
// ⚠️ The mechanism behind the residual is UNNAMED and this comment names none. The autotune term is
// understood; what else concentrated fill costs is not. The cap deletes the term without explaining
// it, which is worth having and is not the same as understanding it.
//
// Adreno only, and by VENDOR rather than by driver, for the same reason the depth answer above is:
// nothing points at Turnip specifically. Every other vendor is uncapped -- a tiler has no measurement
// asking for one, and a cap it does not need is pure pass-boundary cost.
u32 GSDeviceVK::TileGpuMaxPassDraws()
{
	return IsDeviceAdreno() ? 64u : 0u;
}

// How many pipeline binds a plan may add for fragment specialization before the planner stops
// freezing state into the programs of the passes that add them.
//
// 300 sits inside a gap the SD865 round of 2026-08-24 left wide open. Measured added binds a frame,
// specialized against the same binary's unspecialized arm: Shadow of the Colossus 141 and Gran
// Turismo 4 150, both of which WIN (-20.5% and -7.6% frame time, GPU time falling); the two Ratchet
// & Clank scenes 525 and 859, both of which LOSE (+8.4% and +9.2%, GPU time RISING). Across the rest
// of the eighteen-dump corpus the highest is Gran Turismo 4's Online Public Beta at 72. So the
// budget has a 3.5x gap to sit in and a 4x margin over every dump that is not one of the two losers,
// which is what makes it a threshold rather than a fitted constant.
//
// ⚠️ What it is NOT: an instruction-cache size effect. That was the first hypothesis -- an Adreno
// fragment-program change is an instruction-RAM DMA and the a650's cache holds 127 instrlen units --
// and compiling every bound variant offline against the device's own Turnip refutes it outright. The
// Ratchet gameplay scene's busiest pass alternates among five programs totalling 32 units, a quarter
// of the cache, while Shadow of the Colossus runs 110-182-unit working sets and streams 2.6x the
// program bytes a frame, and it is Shadow of the Colossus that wins. The cost tracks how OFTEN the
// program changes, not how big the programs are.
//
// Adreno only, by vendor, like the two pass-boundary answers above. Everywhere else the guard is off,
// because giving up wave128 to save a bind is a trade that only pays where a bind is expensive, and
// nothing outside this round says it is.
u32 GSDeviceVK::TileGpuMaxSpecializationBinds()
{
	return IsDeviceAdreno() ? 300u : 0u;
}

// Adreno charges every draw in a declaring pass for the declaration, so the readers have to be
// alone in one.
//
// The measurement first: the suite's device round (SD865, Adreno 650, Turnip, 20260824-0814) found
// +49% to +156% frame time on EVERY TileGpu dump against the arm before the read landed, with the
// CPU flat, Classic flat and thermals controlled. The ranking tracks each dump's DECLARED-PASS
// share rather than its reader count, which is the tell: Ratchet & Clank's effects scene is the
// worst at +156% on six admitted alpha-test draws, because those six sit inside enormous effect
// passes and the whole pass pays.
//
// The mechanism is in the Turnip source (mesa 26.1.2) and it is three steps:
//   1. tu_render_pass_check_feedback_loop marks a subpass whose input attachment aliases its colour
//      attachment as a colour feedback loop. That is exactly the shape of a declaring pass -- our
//      set 3 binds the pass's own colour target as an input attachment.
//   2. Every pipeline built against that subpass inherits the feedback-loop flag, whether or not
//      its fragment stage reads anything (tu_pipeline).
//   3. On sysmem, a feedback-loop or raster-order pipeline sets GRAS_SC_CNTL.single_prim_mode to
//      FLUSH_PER_OVERLAP_AND_OVERWRITE -- a render-backend flush per overlapping primitive.
// Our workload is 100% sysmem with heavy overdraw, so step 3 is charged against every draw of the
// pass, per overlap.
//
// Segregating is not free: it costs a pass boundary on each side of every run of readers. That is
// the right trade only where declaring is expensive, which is why this is a device question and not
// the planner's. On Mali the opposite holds -- an in-pass read is free below stride-1 density
// (measured 2026-08-17) and there is no analogous whole-pass mode -- so it keeps the merged
// arrangement, and so does every vendor nobody has measured.
//
// ⚠️ Answering yes here does a SECOND thing, and it is here rather than behind a virtual of its own
// because it is the same fact: segregation moves the readers out of everybody else's pass, and it
// can do nothing at all for a title whose passes are reader-saturated. Xenosaga is entirely 16-bit,
// so almost every blended draw is admitted for result quantisation -- 2,409 admitted draws inside
// 1,930 declaring passes a frame, and those counters come out identical merged or segregated. The
// device measured it at 303 ms a frame with one kernel-level GPU fault in two runs. So under this
// bit that admission class is not taken at all, and those draws keep fixed-function blending
// without the quantise; see gsTileGpuAdmitsQuantisedBlend for what the accuracy costs.
bool GSDeviceVK::TileGpuSegregatesSelfRead()
{
	return IsDeviceAdreno();
}

// The writeback compute's workgroup edge: 16 unless this is Mali, which takes 8. Both architectures
// were measured on the same two builds and they disagree, so this is a device answer rather than the
// single constant it started as.
//
// What is being traded is the subgroup width against the barrier the shader's shared-memory stage
// needs. A 16x16 workgroup is 256 invocations:
//
//  - On Adreno that is two 128-lane waves, and the 8x8 shape it replaced was half a wave sitting
//    idle. The store-path A/B on an SD865 (2026-08-31) cut gow2 by 1.090 ms of gpu_ms p50, flatout2
//    by 2.197 and stuntman by 0.661, with Spider-Man 3 flat. That MISSED the round's pre-registered
//    3.0 ms bar on gow2 and the shape is kept anyway, because it is a win on three of four titles
//    and moves no pixel; what the missed bar killed was the claim that the store path holds the
//    rest of the writeback's bill, not this shape.
//  - On Mali it is SIXTEEN 16-lane warps under one barrier, and the same pair of builds LOST on an
//    RG477V (Dimensity 8300, Mali-G615, r44p1): gow2 12.671 -> 12.999 ms and Spider-Man 3 56.160 ->
//    58.283 ms of wall mean, four rounds each with the two arms' round values not overlapping at
//    all. 8 puts that device back on a 64-invocation workgroup, four warps under one barrier. That
//    part is a PRIMARY target, co-equal with the SD865, so this is not a concession to a low tier.
//
// ⚠️ The Mali arm is 8x8 with the LDS quad stage, which is NOT the shader that measured faster there
// -- that one was 8x8 storing a word per invocation. The stage is kept because the regression is
// attributable to the shape (the barrier's warp count is what 16 changes on a 16-lane part) and
// because the quads are the architecture-neutral half of the change. If an RG477V A/B against
// 600c0b2972 still reads slow at dim 8, the LDS stage is the remaining suspect and this is where to
// split it.
//
// Every other vendor takes 16: Apple and NVIDIA are 32-wide, where a 64-invocation workgroup is
// already two full SIMD groups and the M2 measured flat either way, so 16 costs them nothing and
// keeps one shape on everything that is not the one part measured to dislike it.
u32 GSDeviceVK::TileGpuWritebackGroupDim() const
{
	return IsDeviceMali() ? kGSTileWritebackGroupDimMali : kGSTileWritebackGroupDimDefault;
}

bool GSDeviceVK::CompileTileGpuPipeline()
{
	m_tilegpu_tried = true;

	// Every road out of this function ends in a renderer with no geometry pipelines, and the executor's
	// whole symptom for that is a frame whose passes clear and store and draw nothing -- deterministic
	// black, at a cost, with no other column moving. So each road names itself. This is the ONLY place
	// that knows which step failed: the caller sees one null handle whatever the cause.
	const auto fail = [](const char* what) {
		Console.Error("TileGpu: pipeline build failed at %s; the renderer has no geometry pipelines and "
					  "every frame will record its passes without draws.",
			what);
		return false;
	};

	// Per-draw state table (SSBO, binding 0) + the frame's ring (SSBO, binding 1) + their layout.
	// The VS reads its transform row from the state table by the indirect draw's first_instance and
	// forwards the row index, so both stages see binding 0; the FS reads texels out of the ring, so
	// it alone sees binding 1. The push range is shared with the seed pipeline (same layout, so one
	// descriptor bind serves both): the geometry pipeline pushes base_row / table_base / pal_base, the
	// seed pushes its five words. Eight words covers either.
	{
		Vulkan::DescriptorSetLayoutBuilder dslb;
		dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		if ((m_tilegpu_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
			return fail("the state descriptor-set layout");
		Vulkan::SetObjectName(m_device, m_tilegpu_ds_layout, "TileGpu state DS layout");

		// Set 1, bound per pass: binding 0 is the pass's snapshot, binding 1 the pass's rule-2
		// sampled targets. They share a set rather than taking one each because a pipeline layout
		// may carry at most ONE push-descriptor set (VUID-VkPipelineLayoutCreateInfo-pSetLayouts-
		// 00293), and a second set would have to be non-push -- which means the frame descriptor
		// pool, which only exists on devices that do NOT push. Per pass either way: a descriptor
		// carries the image layout it was written with, and these images move between colour
		// attachment and shader read as the frame's passes go by.
		Vulkan::DescriptorSetLayoutBuilder snap_dslb;
		if (m_use_push_descriptors)
			snap_dslb.SetPushFlag();
		snap_dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		snap_dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			GSTileGpuPassPlan::kMaxTexSourcesPerPass, VK_SHADER_STAGE_FRAGMENT_BIT);
		if ((m_tilegpu_snapshot_ds_layout = snap_dslb.Create(m_device)) == VK_NULL_HANDLE)
			return fail("the per-pass descriptor-set layout");
		Vulkan::SetObjectName(m_device, m_tilegpu_snapshot_ds_layout, "TileGpu per-pass DS layout");

		// Set 2, written once per plan: rule 3's frame-wide materialised sources. Never a push set --
		// set 1 already is the layout's one push set -- so it is always a pooled set, which is why it
		// gets a pool and a ring of its own below rather than riding the frame pool (that pool only
		// exists on the non-push path). The layout is created whatever the device can serve: a device
		// without the array-indexing feature simply compiles no module that names the set, and an
		// unused set costs nothing.
		Vulkan::DescriptorSetLayoutBuilder src_dslb;
		src_dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GSTileGpuPassPlan::kMaxSources,
			VK_SHADER_STAGE_FRAGMENT_BIT);
		if ((m_tilegpu_source_ds_layout = src_dslb.Create(m_device)) == VK_NULL_HANDLE)
			return fail("the source descriptor-set layout");
		Vulkan::SetObjectName(m_device, m_tilegpu_source_ds_layout, "TileGpu source DS layout");

		// Set 3, bound only by a pass that declares the in-pass destination read: the pass's own colour
		// attachment, as an input attachment. A set of its own rather than a binding on the per-pass
		// set 1, for two reasons that both bite. Set 1 is the layout's ONE push-descriptor set, and a
		// push layout's set cannot be allocated from a pool -- while an input attachment is precisely
		// the descriptor that would not stay validation-clean when pushed (measured on a software
		// rasterizer: the identical run reports the descriptor invalid through a push and is clean
		// through a pooled set). Bound at most once per pass, so a set of its own costs one command on
		// the passes that declare and nothing at all on the ones that do not.
		Vulkan::DescriptorSetLayoutBuilder dest_dslb;
		dest_dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		if ((m_tilegpu_dest_ds_layout = dest_dslb.Create(m_device)) == VK_NULL_HANDLE)
			return fail("the destination-read descriptor-set layout");
		Vulkan::SetObjectName(m_device, m_tilegpu_dest_ds_layout, "TileGpu destination-read DS layout");

		Vulkan::PipelineLayoutBuilder plb;
		plb.AddDescriptorSet(m_tilegpu_ds_layout);
		plb.AddDescriptorSet(m_tilegpu_snapshot_ds_layout);
		plb.AddDescriptorSet(m_tilegpu_source_ds_layout);
		plb.AddDescriptorSet(m_tilegpu_dest_ds_layout);
		plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32) * kTileGpuPushWords);
		if ((m_tilegpu_pipeline_layout = plb.Create(m_device)) == VK_NULL_HANDLE)
			return fail("the pipeline layout");
		Vulkan::SetObjectName(m_device, m_tilegpu_pipeline_layout, "TileGpu pipeline layout");
	}

	// The source set ring and its pool.
	//
	// ⚠️ The rationale that used to stand here -- "written once per plan, twice if a mid-plan flush
	// forces it, so a handful of sets covers everything in flight" -- was wrong, and wrong by two
	// orders of magnitude on half the corpus. A plan flush writes a set, and the flush rate runs
	// from 1.5 a frame on Katamari to 52 on GT4. Depth is set by gsTileGpuSourceSetRingDepth, which
	// carries the measurement. Allocated eagerly with the layout because a failure
	// here has to fall back to "rule 3 is not available" rather than surface mid-frame.
	{
		// Depth is the one thing about this pool that is a policy rather than a fact, so it is named
		// in the log with where the number came from: a run whose ring depth you cannot read off its
		// emulog is a run whose wait count means nothing.
		const u32 depth = gsTileGpuSourceSetRingDepth(GSConfig.TileGpuSourceSetRingDepth);
		const VkDescriptorPoolSize src_pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GSTileGpuPassPlan::kMaxSources * depth}};
		const VkDescriptorPoolCreateInfo src_pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
			depth, static_cast<u32>(std::size(src_pool_sizes)), src_pool_sizes};
		if (vkCreateDescriptorPool(m_device, &src_pool_info, nullptr, &m_tilegpu_source_pool) != VK_SUCCESS)
			return fail("the source descriptor pool");
		Vulkan::SetObjectName(m_device, m_tilegpu_source_pool, "TileGpu source descriptor pool");
		m_tilegpu_source_set_count = 0;
		for (u32 i = 0; i < depth; i++)
		{
			const VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
				m_tilegpu_source_pool, 1, &m_tilegpu_source_ds_layout};
			// A short ring is a slower ring, not a broken one, so a device that runs out of
			// descriptor memory part way keeps what it got rather than losing rule 3 entirely. Only
			// a ring below the minimum is a failure -- deepening the default must not be able to
			// turn a working machine into one with no source road.
			if (vkAllocateDescriptorSets(m_device, &ai, &m_tilegpu_source_sets[i]) != VK_SUCCESS)
			{
				if (i < kGSTileGpuSourceSetRingMin)
					return fail("the source set ring, short of its minimum depth");
				Console.Warning("TileGpu source set ring: only %u of %u sets allocated; running short.", i, depth);
				break;
			}
			m_tilegpu_source_set_epoch[i] = 0;
			m_tilegpu_source_set_count = i + 1;
		}
		Console.WriteLn("TileGpu source descriptor ring: %u sets (%s) -- built-in %u, "
						"EmuCore/GS/TileGpuSourceSetRingDepth = %d, %u descriptors a set.",
			m_tilegpu_source_set_count, (GSConfig.TileGpuSourceSetRingDepth > 0) ? "FORCED by settings" : "the default",
			kGSTileGpuSourceSetRingDefault, GSConfig.TileGpuSourceSetRingDepth, GSTileGpuPassPlan::kMaxSources);

		// The eight samplers rule 3 admits: wrap U x wrap V x filter. REGION_CLAMP and REGION_REPEAT
		// have no sampler spelling, so the renderer refuses them to the byte road and they need none.
		// triln stays 0, which is what pins max LOD at level 0 in GetSampler -- rule 3 samples level 0
		// exactly as every road does today, and choosing a GS level is M4's.
		for (u32 i = 0; i < kTileGpuSourceSamplers; i++)
		{
			GSHWDrawConfig::SamplerSelector ss;
			ss.tau = (i & 1u) ? 1 : 0; // REPEAT on U
			ss.tav = (i & 2u) ? 1 : 0; // REPEAT on V
			ss.biln = (i & 4u) ? 1 : 0;
			m_tilegpu_source_sampler[i] = GetSampler(ss);
			if (m_tilegpu_source_sampler[i] == VK_NULL_HANDLE)
				return fail("a source sampler");
		}
	}

	// Indirect + state + ring streams, allocated only now (first executor use), so a non-TileGpu
	// session pays nothing. Sized for the corpus's largest frames with ring headroom for frames in
	// flight; ReserveMemory waits on a fence rather than overrunning. The ring holds the frame's
	// page slots (only the pages the plan reads or reconciles -- a few hundred KB to low MB, not the
	// whole 4 MiB), its epoch page tables, page-entry lists and palettes; several frames' worth so a
	// frame in flight does not stall the next frame's staging.
	static constexpr u32 TILEGPU_INDIRECT_BUFFER_SIZE = 4 * 1024 * 1024;
	static constexpr u32 TILEGPU_STATE_BUFFER_SIZE = 4 * 1024 * 1024;
	static constexpr u32 TILEGPU_VRAM_BUFFER_SIZE = 32 * 1024 * 1024;
	if (!m_tilegpu_indirect_stream_buffer.Create(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, TILEGPU_INDIRECT_BUFFER_SIZE) ||
		!m_tilegpu_state_stream_buffer.Create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, TILEGPU_STATE_BUFFER_SIZE) ||
		// TRANSFER_DST because the CLUT gather's byte-road half copies palette blocks straight out of
		// an owner target into the frame's palette run of this buffer -- an image-to-buffer copy at a
		// pass head, which is what keeps a thousand CLUT loads a frame off the pass count.
		!m_tilegpu_vram_stream_buffer.Create(
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, TILEGPU_VRAM_BUFFER_SIZE))
	{
		return fail("the indirect, state and ring stream buffers");
	}

	// One persistent descriptor set over the whole state and ring buffers; the shader indexes rows
	// and words within them (base_row + first_instance; the page table + address), so it never needs
	// rewriting frame to frame.
	m_tilegpu_state_descriptor_set = AllocatePersistentDescriptorSet(m_tilegpu_ds_layout);
	if (m_tilegpu_state_descriptor_set == VK_NULL_HANDLE)
		return fail("the persistent state descriptor set");
	{
		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddBufferDescriptorWrite(m_tilegpu_state_descriptor_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_tilegpu_state_stream_buffer.GetBuffer(), 0, TILEGPU_STATE_BUFFER_SIZE);
		dsub.AddBufferDescriptorWrite(m_tilegpu_state_descriptor_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_tilegpu_vram_stream_buffer.GetBuffer(), 0, TILEGPU_VRAM_BUFFER_SIZE);
		dsub.Update(m_device);
	}
	Vulkan::SetObjectName(m_device, m_tilegpu_state_descriptor_set, "TileGpu state set");

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/tilegpu.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tilegpu.glsl.");
		return fail("reading shaders/vulkan/tilegpu.glsl");
	}

	// The shader tree is read from disk, so the file compiled here need not be the file this binary was
	// built beside. A row-size disagreement between the two is the one mismatch nothing else can see:
	// the executor's gate checks the PLAN's stride against the binary and still agrees, while the shader
	// indexes state_rows[] at its own stride and reads every row but the first from the wrong place --
	// no compile error, no counter, a wrong frame. So refuse to build rather than render it.
	const u32 shader_row_words = GSTileGpuShaderVariant::StateRowWordsIn(*source);
	if (shader_row_words != GSDevice::GSTileGpuPassPlan::kStateRowWords)
	{
		Console.Error("TileGpu: shaders/vulkan/tilegpu.glsl declares a %u-word StateRow and this build is "
					  "%u words. The shader tree is from another revision of the renderer.",
			shader_row_words, GSDevice::GSTileGpuPassPlan::kStateRowWords);
		return fail("the shader's StateRow layout");
	}

	// The byte sampling path compiles in only if the page-swizzle forms fitted closed forms — the
	// shader addresses guest memory with them, so an unfit table disables texturing rather than
	// sampling the wrong bytes (the state rows still set tex_enable, but TILEGPU_TEX 0 #ifdefs the
	// sampling out and every draw falls back to the vertex-colour path). The forms are prepended as
	// TILE_SWZ_* defines exactly as the Tile reinterpretation pipelines take them; TileFormDefines()
	// fits them as a side effect, so m_tile_forms.valid is read after the call.
	const std::string& form_defines = TileFormDefines();
	m_tilegpu_tex = m_tile_forms.valid;
	// Honeykrisp miscompiles the dynamic byte-extract shift on a word loaded from the vram
	// SSBO (the selector comes out as the word index's low bits, zeroing 3 of 4 paletted
	// texels in a 2x2 lattice); TILEGPU_STATIC_BYTE_SEL switches tilegpu_byte_sel to
	// constant shifts there. Gate on driverID, never vendorID, per the device-workaround
	// rule above -- other Mesa drivers and the proprietary ones keep the straight form.
	const bool static_byte_sel = TileGpuStaticByteSel();
	// The second shader-compiler workaround: the Mali proprietary compiler miscompiles a bitwise AND
	// whose operands are vectors, which in this shader is the fog colour unpack and the whole integer
	// byte tail -- blend coefficients, the COLCLAMP mask, the 16-bit quantise and the FBMSK merge.
	// TILEGPU_SCALARIZE_VECTOR_AND does them a component at a time there. Read out of the driver-bug
	// database, so this shader and the classic renderer's tfx.glsl act on one answer rather than two.
	const bool scalarize_vec_and = UsesMobileDriverWorkaround(DriverWorkaround::ScalarizeVectorBitwiseAnd);
	// Rule 2's tap compiles in only where the device can index the sampled-target array by the
	// draw's slot (TileGpuBindlessTargets, negotiated in CreateDevice). Elsewhere the array is not
	// declared at all and the tap has no target branch: the renderer already refuses to bind
	// targets there, but "never taken at runtime" is not the same as "not in the SPIR-V", and a
	// dynamic image-array index is only legal in a module the device supports it in.
	// Rule 3's array rides the same capability as rule 2's, for the same reason: the index is a value
	// the shader computes. It is injected as its own define so the shader reads as three roads rather
	// than two roads and a borrowed flag. The whole block is assembled by GSTileGpuShaderVariant so
	// the size gate compiles the same programs this device does, rather than its own idea of them.
	// The two settings in that block rather than capabilities: the merged CLUT arm and the per-draw
	// stride its page half needs. Latched HERE and nowhere else, because every module of the session
	// is compiled from this one string, and the renderer asks TileGpuClutMerge*Compiled() rather than
	// re-reading the settings -- a mid-session flip must not put a mode-3 state row in front of a
	// module that has no mode-3 arm, nor a 64-wide tile in front of an arm whose stride is 16.
	m_tilegpu_clut_merge = GSConfig.TileGpuClutMergeRegions;
	m_tilegpu_clut_merge_pages = m_tilegpu_clut_merge && GSConfig.TileGpuClutMergePages;
	const std::string defines =
		(m_tilegpu_tex ? form_defines : std::string()) +
		GSTileGpuShaderVariant::DeviceDefines(m_tilegpu_tex, static_byte_sel,
			m_optional_extensions.tilegpu_bindless_targets, m_optional_extensions.tilegpu_dual_source,
			scalarize_vec_and, m_tilegpu_clut_merge, m_tilegpu_clut_merge_pages);
	// Kept for the session: a pass's fragment module is compiled from this plus its variant defines,
	// on first use of that (road, texel-arm) pair. Re-reading the file instead would risk compiling
	// two variants from two different revisions of the shader, since the runtime shader tree is a
	// symlink into the source tree and an edit lands live.
	m_tilegpu_shader_source = defines + *source;

	VkShaderModule vs = GetUtilityVertexShader(m_tilegpu_shader_source);
	if (vs == VK_NULL_HANDLE)
		return fail("the vertex module");
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	// The FULL module: every road this device can serve, and every one of the byte road's texel arms.
	// It exists to be the fallback -- it backs the eager pipelines below and stands in wherever a
	// variant fails to compile, and being the superset of every variant it is bigger but never wrong.
	//
	// ⚠️ Bigger is not free here: on the Adreno 650 this program is roughly twice the instruction-size
	// threshold that swings the whole frame, so the fallback doctrine is that it must not RUN. The
	// alternatives were both worse. A narrower fallback is not a superset, so a pass whose arms it
	// lacks would decode nothing -- a wrong frame instead of a slow one. Per-arm eager tables would
	// multiply the twelve startup pipelines by five and still need this one for a mixed pass. So the
	// full program stays, and what changed is that GetTileGpuPipeline now sends a pass to the eager
	// table only when its masks really are the full set, which no ordinary pass is: 3.4 in 32,985
	// byte-road passes over the corpus. Compiling it costs startup time, not frame time; the cliff
	// rides on the size register written when a pipeline is BOUND. The vertex stage reads no texture
	// and takes no road, so it has no variants at all.
	const u32 full_roads = TileGpuRoadMask(GSDevice::kGSTileGpuRoadMaskAll);
	VkShaderModule fs = CompileTileGpuFragmentModule(
		full_roads, TileGpuTexelMask(full_roads, GSDevice::kGSTileGpuTexelMaskAll), 0, false, {});
	if (fs == VK_NULL_HANDLE)
		return fail("the full fragment module");
	ScopedGuard fs_guard([this, &fs]() { vkDestroyShaderModule(m_device, fs, nullptr); });

	// The modules stay alive: blend variants are built lazily per GS ALPHA equation as draws
	// first need them (CreateTileGpuPipeline), from the same two stages.
	m_tilegpu_vs = vs;
	m_tilegpu_fs = fs;
	vs_guard.Cancel();
	fs_guard.Cancel();

	// The no-blend pipelines, one per GSTileGpuTopology (triangle/line/point) times depth mode, at the
	// full masks. They stay eager rather than becoming twelve more lazy variants because they are also
	// the fallback every other lookup returns when a pipeline fails to build: something valid has to
	// exist before the first pass is planned. Every pass whose masks are narrower -- in practice all
	// of them -- takes the lazy road below and gets the smaller program.
	for (u32 t = 0; t < 3; t++)
	{
		for (u32 i = 0; i < GSDevice::kGSTileGpuDepthModes; i++)
		{
			VkPipeline& pipe = m_tilegpu_pipeline[t][i];
			pipe = CreateTileGpuPipeline(t, i, kTileGpuNoBlend, 0xFu, full_roads,
				TileGpuTexelMask(full_roads, GSDevice::kGSTileGpuTexelMaskAll), 0, false, false, false, 0, {});
			if (pipe == VK_NULL_HANDLE)
				return fail("an eager no-blend pipeline");
		}
	}
	return true;
}

// The plan asks for roads; the device decides which of them exist. Without the swizzle forms there
// is no texture road at all (TILEGPU_TEX 0 takes the whole block out, whatever a state row says), and
// without dynamic sampled-image-array indexing there is no rule-2 road -- the renderer already
// refuses to bind targets there, so the bit cannot arrive, but normalising here is what stops two
// masks that compile the identical program from becoming two modules and two pipelines.
u32 GSDeviceVK::TileGpuRoadMask(u32 plan_road_mask) const
{
	if (!m_tilegpu_tex)
		return 0;
	u32 mask = plan_road_mask & GSDevice::kGSTileGpuRoadMaskAll;
	if (!m_optional_extensions.tilegpu_bindless_targets)
		mask &= ~(GSDevice::kGSTileGpuRoadTarget | GSDevice::kGSTileGpuRoadSource);
	if (m_tilegpu_source_ds_layout == VK_NULL_HANDLE || m_tilegpu_source_pool == VK_NULL_HANDLE)
		mask &= ~GSDevice::kGSTileGpuRoadSource;
	return mask;
}

// The texel-arm mask, normalised against the road mask it rides with. No byte road means no arms, so
// the two masks that compile the identical program cannot become two modules; and a byte road that
// named no arm takes the whole set, because that combination is a planner bug and the two ways of
// being wrong are not equal -- a superset renders correctly and slowly, a subset decodes nothing and
// paints the frame black. The assert is where the bug gets reported; the widening is what a release
// build does about it.
u32 GSDeviceVK::TileGpuTexelMask(u32 road_mask, u32 plan_texel_mask)
{
	if ((road_mask & GSDevice::kGSTileGpuRoadByte) == 0)
		return 0;
	u32 mask = plan_texel_mask & GSDevice::kGSTileGpuTexelMaskAll;
	// The palette-order arms mean nothing without a paletted geometry to fetch through, and leaving one
	// set would make two masks that compile the identical program into two modules. Both of them: the
	// 32-bit order and the 16-bit one are alternatives, and neither survives with no palette to order.
	if ((mask & GSDevice::kGSTileGpuTexelPalettedMask) == 0)
		mask &= ~GSDevice::kGSTileGpuTexelPalGatherMask;
	pxAssertMsg((mask & GSDevice::kGSTileGpuTexelGeometryMask) != 0,
		"TileGpu pass takes the byte road but names no texel arm");
	return ((mask & GSDevice::kGSTileGpuTexelGeometryMask) != 0) ? mask : GSDevice::kGSTileGpuTexelMaskAll;
}

// Every stream a TileGpu plan stages into, carried onto the command buffer that is recording now.
//
// The stream ring's model of who is still reading a range is "the command buffer that was recording
// when it was committed", and that is right for the classic renderer, which stages and then draws
// without ever ending a buffer in between. The TileGpu executor does not work that way: it stages
// the WHOLE frame -- vertices, indices, the state table, the indirect commands, the ring pages --
// before it opens the first pass, and then records draws that reference those ranges for the rest of
// the plan. Anything that ends the command buffer in that window leaves the ranges tracked against a
// submission that retires FIRST, while the passes recorded after it are still reading them.
//
// What that costs is not a torn edge on one draw. UpdateGPUPosition treats "the tracked position
// caught up with the write position" as the ring being wholly idle and resets it to offset zero, so
// the next frame does not overwrite the tail of the live data -- it stages a new frame directly on
// top of all of it. The pixels are then whatever the two frames' timing happened to produce, which
// is how this presented: Ratchet & Clank drawing with other draws' textures, differently on every
// run.
//
// So this is called at every one of the executor's mid-plan cuts, not only at the ones where a range
// is known to be live. Two of them (the source-set take and the tile-expand descriptor refill) sit
// outside the staging window today and get nothing out of it; they call it anyway, because the rule
// that survives a later edit is "a cut retains", and the alternative is re-auditing all five sites
// every time a reservation moves. Retention only ever delays a reuse, never permits one.
void GSDeviceVK::RetainTileGpuStreamsForCurrentCommandBuffer()
{
	m_vertex_stream_buffer.RetainForCurrentCommandBuffer();
	m_index_stream_buffer.RetainForCurrentCommandBuffer();
	m_tilegpu_state_stream_buffer.RetainForCurrentCommandBuffer();
	m_tilegpu_indirect_stream_buffer.RetainForCurrentCommandBuffer();
	m_tilegpu_vram_stream_buffer.RetainForCurrentCommandBuffer();
}

// The source set the frame's draws sample through, taken from the ring. A set is reusable once the
// submission that last read it has completed; anything else would rewrite descriptors under an
// in-flight pass. One write per PLAN, and a plan is usually a frame -- but not always: every stall
// road flushes a plan mid-frame, and GT4's CLUT churn flushes far more than the ring is long. So the
// ring can wrap around to a set THIS command buffer has already recorded draws against, which no
// wait can resolve (the fence it would wait on is the one this thread has not submitted yet, and
// waiting on it asserts). Submitting is what resolves it, and it is legal here: this runs before any
// pass opens, outside a render pass, and the caller re-reads the command buffer after.
//
// ⚠️ The caller must therefore call this BEFORE it captures the command buffer it records the plan
// into. Nothing else in the plan may end the command buffer.
u32 GSDeviceVK::WriteTileGpuSourceSet(std::span<const GSTileGpuPassPlan::SourceBind> sources)
{
	if (m_tilegpu_source_pool == VK_NULL_HANDLE || m_tilegpu_source_set_count == 0)
		return kTileGpuSourceSetsMax;

	const u32 idx = m_tilegpu_source_next_set;
	m_tilegpu_source_next_set = (m_tilegpu_source_next_set + 1) % m_tilegpu_source_set_count;
	if (m_tilegpu_source_set_epoch[idx] >= GetCurrentFenceCounter())
	{
		// The ring came all the way round inside one command buffer: the set's last reader is still
		// being recorded. Close the buffer so it becomes a submission that can be waited on.
		ExecuteCommandBuffer(false, "TileGpu source descriptor ring wrapped inside one command buffer");
		RetainTileGpuStreamsForCurrentCommandBuffer();
	}
	if (m_tilegpu_source_set_epoch[idx] > GetCompletedSubmitEpoch())
		WaitForFenceCounter(m_tilegpu_source_set_epoch[idx], GpuWaitCause::SourceSet);

	// Every slot written, including the ones this frame does not use: the shader uses the array
	// statically, so an unwritten slot would need descriptorBindingPartiallyBound. The null texture
	// stands in, exactly as it does for set 1's unused target slots.
	std::array<VkDescriptorImageInfo, GSTileGpuPassPlan::kMaxSources> infos{};
	for (u32 s = 0; s < GSTileGpuPassPlan::kMaxSources; s++)
	{
		const bool live = s < sources.size() && sources[s].texture != nullptr;
		GSTextureVK* const tex = live ? static_cast<GSTextureVK*>(sources[s].texture) : m_null_texture.get();
		infos[s].sampler = m_tilegpu_source_sampler[live ? (sources[s].sampler & (kTileGpuSourceSamplers - 1)) : 0];
		infos[s].imageView = tex->GetView();
		// A source's layout is stated, not read: this set is written BEFORE the pass loop, and a
		// source built this frame is still a colour attachment at that moment. What the descriptor
		// promises is what every source is in by the time a pass samples it -- a materialise moves its
		// image to ShaderReadOnly as soon as its pass closes, and a cached source never left that
		// layout. The null filler is the exception, because it lives in General and stays there.
		infos[s].imageLayout = live ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : tex->GetVkLayout();
	}
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = m_tilegpu_source_sets[idx];
	w.dstBinding = 0;
	w.descriptorCount = GSTileGpuPassPlan::kMaxSources;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = infos.data();
	vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
	m_tilegpu_source_set_epoch[idx] = GetCurrentFenceCounter();
	return idx;
}

// One fragment module for one road mask. The mask arrives as #defines, so the roads it does not name
// are not in the SPIR-V at all -- which is the whole point on a tiler, where an instruction that is
// never executed still costs program size and can carry the whole frame over a cliff.
VkShaderModule GSDeviceVK::CompileTileGpuFragmentModule(
	u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, const GSDevice::GSTileGpuFragmentSpec& spec)
{
	const std::string source = GSTileGpuShaderVariant::VariantDefines(road_mask, texel_mask, self_mask, quantise) +
							   GSTileGpuShaderVariant::SpecDefines(spec) + m_tilegpu_shader_source;
	u32 spv_words = 0;
	VkShaderModule mod = GetUtilityFragmentShader(source, nullptr, &spv_words);
#ifdef PCSX2_DEVBUILD
	// The attribution line the instruction-size gate reads: a device stats line says how big a
	// program is, and this says which variant it was. Word count is the local proxy -- the real
	// number is the driver's instrlen, which only the device can report.
	Console.WriteLn("TileGpu fragment variant road=%u texel=%u self=%u q16=%u (%s%s): %u SPIR-V words%s", road_mask,
		texel_mask, self_mask, quantise ? 1u : 0u,
		GSTileGpuShaderVariant::VariantName(road_mask, texel_mask, self_mask, quantise).c_str(),
		GSTileGpuShaderVariant::SpecName(spec).c_str(), spv_words, (mod == VK_NULL_HANDLE) ? " -- FAILED" : "");
#endif
	return mod;
}

// The fragment module for a pass's (road mask, texel-arm mask), compiled on first sight of the pair.
// A pair that fails to compile falls back to the full module, which is a superset: bigger than the
// pass needs, never wrong.
VkShaderModule GSDeviceVK::GetTileGpuFragmentShader(
	u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, const GSDevice::GSTileGpuFragmentSpec& spec)
{
	const u32 full_roads = TileGpuRoadMask(GSDevice::kGSTileGpuRoadMaskAll);
	if (!spec.valid && self_mask == 0 && !quantise && road_mask == full_roads &&
		texel_mask == TileGpuTexelMask(full_roads, GSDevice::kGSTileGpuTexelMaskAll))
		return m_tilegpu_fs;
	const u32 key = TileGpuVariantKey(road_mask, texel_mask, self_mask, quantise, spec);
	const auto it = m_tilegpu_fs_variants.find(key);
	if (it != m_tilegpu_fs_variants.end())
		return (it->second != VK_NULL_HANDLE) ? it->second : m_tilegpu_fs;
	VkShaderModule mod = CompileTileGpuFragmentModule(road_mask, texel_mask, self_mask, quantise, spec);
	if (mod == VK_NULL_HANDLE)
	{
		// The fallback is a superset, so the pixels stay right and only the size is wrong -- but on the
		// Adreno 650 the full program is about twice the instruction-size threshold that swings the whole
		// frame, so "it still renders" is not the same as "nothing happened". The compile log carries the
		// FAILED marker only on a devbuild, which is exactly the build a device round is NOT.
		m_tilegpu_variant_compile_failures++;
		if (!m_tilegpu_variant_compile_warned)
		{
			m_tilegpu_variant_compile_warned = true;
			Console.Error("TileGpu: fragment variant road=%u texel=%u self=%u q16=%u failed to compile; its draws "
						  "run the full program instead -- correct, and past the size threshold that swings a "
						  "frame. Totals at teardown.",
				road_mask, texel_mask, self_mask, quantise ? 1u : 0u);
		}
	}
	m_tilegpu_fs_variants.emplace(key, mod);
	return (mod != VK_NULL_HANDLE) ? mod : m_tilegpu_fs;
}

// One geometry pipeline: topology (triangle/line/point) x depth mode x fixed-function blend.
// Only the primitive topology differs across the topology row: a GS point is one vertex, a
// line is two, and both render at their native footprint rather than being synthesised into
// triangles. PS2 depth grows towards the viewer, so a real ZTST test maps to GREATER_OR_EQUAL
// and a write-only (ZTST ALWAYS) draw to ALWAYS; depth WRITE follows the GS ZMSK independently
// of the test. The blend index is the GS ALPHA (A,B,C,D) equation mapped onto Vulkan factors by
// GSDevice::m_blendMap -- As arrives as the shader's dual-source output (SRC1), FIX as the
// blend constant (dynamic state), Ad as DST_ALPHA -- and kTileGpuNoBlend disables blending. The
// alpha channel is always written unblended (the GS stores the fragment alpha as-is). The road mask
// picks the fragment module: the pass's roads and no others.
//
// color_write_mask carries the draw's FRAME.FBMSK, one bit per channel, straight onto the
// attachment. The mask applies AFTER blending and after the depth/stencil stage, so it composes
// with everything above it: a masked channel of a blended draw leaves the destination alone, and a
// draw masking all four still tests and writes depth.
VkPipeline GSDeviceVK::CreateTileGpuPipeline(u32 topology, u32 depth_mode, u32 blend_index, u32 color_write_mask,
	u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, bool declares, bool reads, u32 dualsrc_road,
	const GSDevice::GSTileGpuFragmentSpec& spec)
{
	static constexpr VkPrimitiveTopology kTopology[3] = {
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // GSTileGpuTopology::Triangle
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST,     // GSTileGpuTopology::Line
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST,    // GSTileGpuTopology::Point
	};
	static constexpr const char* kTopologyName[3] = {"triangle", "line", "point"};
	struct DepthVariant
	{
		bool has_depth;
		bool test_enable;
		bool write_enable;
		VkCompareOp compare;
		const char* name;
	};
	static constexpr DepthVariant kDepthVariant[GSDevice::kGSTileGpuDepthModes] = {
		{false, false, false, VK_COMPARE_OP_ALWAYS, ""},                          // None
		{true, true, true, VK_COMPARE_OP_GREATER_OR_EQUAL, " (depth test+write)"}, // TestWrite
		{true, true, false, VK_COMPARE_OP_GREATER_OR_EQUAL, " (depth test)"},      // TestNoWrite
		{true, true, true, VK_COMPARE_OP_ALWAYS, " (depth write)"},                // WriteAlways
	};
	// clang-format off
	static constexpr std::array<VkBlendFactor, 16> kBlendFactors = { {
		VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
		VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
		VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO
	}};
	static constexpr std::array<VkBlendOp, 3> kBlendOps = {{VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT}};
	// clang-format on

	if (topology >= 3 || depth_mode >= GSDevice::kGSTileGpuDepthModes || m_tilegpu_vs == VK_NULL_HANDLE ||
		m_tilegpu_fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	// This pass's roads and texel arms and no others. A variant that failed to compile comes back as
	// the full module.
	const VkShaderModule fs = GetTileGpuFragmentShader(road_mask, texel_mask, self_mask, quantise, spec);
	if (fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	const DepthVariant& dv = kDepthVariant[depth_mode];
	const VkFormat color_fmt = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat depth_fmt = LookupNativeFormat(GSTexture::Format::DepthStencil);

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	gpb.AddVertexBuffer(0, sizeof(GSVertex));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R16G16_UINT, 16);    // XY (raw 12.4 fixed)
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32_UINT, 20);       // Z
	gpb.AddVertexAttribute(2, 0, VK_FORMAT_R8G8B8A8_UNORM, 8);  // colour (RGBAQ.RGBA)
	gpb.AddVertexAttribute(3, 0, VK_FORMAT_R32G32_SFLOAT, 0);   // ST texture coords
	gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_SFLOAT, 12);     // Q (RGBAQ.Q, the STQ divisor)
	gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24);    // UV texture coords (12.4 fixed)
	gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG (the factor F is the low byte)
	gpb.SetPrimitiveTopology(kTopology[topology]);
	gpb.SetPipelineLayout(m_tilegpu_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.SetNoCullRasterizationState();
	gpb.SetVertexShader(m_tilegpu_vs);
	gpb.SetFragmentShader(fs);
	// A pipeline is only usable inside a render pass COMPATIBLE with the one it was built against, and
	// an input-attachment reference is part of that compatibility -- so every pipeline a declaring pass
	// binds has to be built against the declaring render pass, whether or not that particular draw
	// reads. `reads` is the separate question, and it is the one the ROAA blend flag answers.
	gpb.SetRenderPass(
		GetRenderPass(color_fmt, dv.has_depth ? depth_fmt : LookupNativeFormat(GSTexture::Format::Invalid),
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			dv.has_depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			dv.has_depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, false, false, declares),
		0);
	// The declaration under test in the ROAA probe, and the reason a reading draw's destination read
	// is ordered against every earlier fragment of the pass. Only the READERS carry it: the probe put
	// an unflagged write-only pipeline in the same flagged subpass and the first flagged reader saw
	// its output, on both device tiers, so the mixed shape is proven rather than assumed -- and a
	// pipeline that does not read has no reason to pay whatever the flag costs.
	gpb.SetBlendFlags(reads ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0);
	gpb.SetDepthState(dv.test_enable, dv.write_enable, dv.compare);
	gpb.SetNoStencilState();
	// A colour target is RGBA8 with R holding the guest cell's byte 0, so the GS's per-byte FBMSK
	// and Vulkan's per-channel write mask are the same four bits in the same order.
	const VkColorComponentFlags channels =
		((color_write_mask & 0x1u) ? VK_COLOR_COMPONENT_R_BIT : 0) |
		((color_write_mask & 0x2u) ? VK_COLOR_COMPONENT_G_BIT : 0) |
		((color_write_mask & 0x4u) ? VK_COLOR_COMPONENT_B_BIT : 0) |
		((color_write_mask & 0x8u) ? VK_COLOR_COMPONENT_A_BIT : 0);
	if (blend_index == kTileGpuNoBlend || blend_index >= 3 * 3 * 3 * 3)
	{
		gpb.SetNoBlendingState();
		gpb.SetColorWriteMask(0, channels);
	}
	else
	{
		const HWBlend b = GSDevice::GetBlend(blend_index);
		// The As factor's road, decided per draw by the plan and carried in the blend key. On the
		// dual-source road the table's SRC1_* factors stand as they are. On the carrier road they do
		// not exist in this pipeline at all: o_color.a holds the factor, so SRC1_COLOR reads back as
		// SRC_ALPHA and ONE_MINUS_SRC1_COLOR as ONE_MINUS_SRC_ALPHA -- exactly equal, because the
		// index-1 output was a broadcast of that same scalar.
		//
		// The ALPHA equation is ONE/ZERO everywhere else -- the GS stores the fragment's own alpha
		// byte -- and CONSTANT_ALPHA/ZERO on the restore road, where the constant is 128/255 and
		// undoes the 255/128 the carrier applied. The executor sets that constant's alpha component;
		// no colour factor here reads it, because a row with a constant colour factor selects FIX,
		// which is C = 2 and never names As.
		const bool carrier = dualsrc_road == GSDevice::GSTileGpuPassPlan::kDualSrcCarrier ||
							 dualsrc_road == GSDevice::GSTileGpuPassPlan::kDualSrcCarrierRestore;
		const auto factor = [&](u8 f) -> VkBlendFactor {
			if (!IsDualSourceBlendFactor(f))
				return kBlendFactors[f];
			if (carrier)
			{
				return (f == SRC1_COLOR || f == SRC1_ALPHA) ? VK_BLEND_FACTOR_SRC_ALPHA :
															  VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			}
			// No road, so the factor stays what the table said -- which is legal only where the
			// device really has the feature. A pipeline that reaches here without one is not slow or
			// wrong, it fails to be created, and the frame goes on the eager fallback: worth an
			// assert rather than a silent black.
			pxAssertMsg(m_optional_extensions.tilegpu_dual_source,
				"TileGpu pipeline names a dual-source factor on a device with no dual-source blending");
			return kBlendFactors[f];
		};
		const bool restore = dualsrc_road == GSDevice::GSTileGpuPassPlan::kDualSrcCarrierRestore;
		gpb.SetBlendAttachment(0, true, factor(b.src), factor(b.dst), kBlendOps[b.op],
			restore ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			channels);
	}

	VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (pipe == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	// The written channels spelt out, so a capture names the FBMSK the draw carried.
	static constexpr const char* kMaskName[16] = {
		" (depth only)", " (writes r)", " (writes g)", " (writes rg)",
		" (writes b)", " (writes rb)", " (writes gb)", " (writes rgb)",
		" (writes a)", " (writes ra)", " (writes ga)", " (writes rga)",
		" (writes ba)", " (writes rba)", " (writes gba)", ""};
	const char* mask = kMaskName[color_write_mask & 0xFu];
	const std::string variant = GSTileGpuShaderVariant::VariantName(road_mask, texel_mask, self_mask, quantise) +
								GSTileGpuShaderVariant::SpecName(spec) +
								(reads ? " reading" : (declares ? " in-declared-pass" : ""));
	if (blend_index == kTileGpuNoBlend)
		Vulkan::SetObjectName(m_device, pipe, "TileGpu %s pipeline%s%s %s", kTopologyName[topology], dv.name, mask,
			variant.c_str());
	else
		Vulkan::SetObjectName(m_device, pipe, "TileGpu %s pipeline%s blend %u%s %s", kTopologyName[topology], dv.name,
			blend_index, mask, variant.c_str());
	return pipe;
}

// The pipeline for a (topology, depth mode, colour write mask, blend key, road mask) tuple;
// everything but the eager full-road no-blend table is cached on first use. A blend the table
// cannot express (or anything that failed to build) falls back to the eager pipeline of the same
// topology and depth mode -- wrong-fast, never a dropped draw, and a fallback carries the full
// shader, so it is always a superset of what the pass needs.
//
// ⚠️ The fallback writes all four channels, so a masked draw that lands on it paints channels the
// GS preserves. That is only reachable when pipeline creation itself fails; it is called out
// because for the write mask the degradation is a visible defect (the Beyond Good & Evil ratchet),
// not the cosmetic wrong-blend the fallback was written for.
VkPipeline GSDeviceVK::GetTileGpuPipeline(u32 topology, u32 depth_mode, u32 blend_key, u32 plan_road_mask,
	u32 plan_texel_mask, u32 self_mask, bool quantise, bool declares,
	const GSDevice::GSTileGpuFragmentSpec& plan_spec)
{
	// The key's colour field is the PRESERVE sense (see GSTileGpuPassPlan::kNoWriteMask), so a plan
	// that carries no blend keys at all still asks for all four channels.
	const u32 color_write_mask = (~(blend_key >> GSTileGpuPassPlan::kNoWriteShift)) & 0xFu;
	// A draw whose BLEND the fragment stage evaluates out of its state row must have the
	// fixed-function blend off -- applying both would blend twice. A draw that merely READS its
	// destination (for the alpha test, say) keeps it: reading is not blending. Either way the channel
	// write mask stays, because that half of FBMSK is exact and the shader only owns the bits inside a
	// channel the mask covers in part.
	const bool reads = (blend_key & GSTileGpuPassPlan::kSelfRead) != 0;
	const bool shader_blend = (blend_key & GSTileGpuPassPlan::kSelfBlend) != 0;
	const bool blend = !shader_blend && (blend_key & GSTileGpuPassPlan::kBlendEnable) != 0;
	// `declares` is the pass's and arrives from the caller; `self_mask` is this RUN's, and says how
	// much of the read machinery THESE draws need compiled in. A run that needs none of it inside a
	// declaring pass still has to be built against the declaring render pass -- that is what makes
	// the two separate arguments -- but its fragment program carries no destination read at all.
	pxAssertMsg(declares || self_mask == 0, "TileGpu run needs the destination read in a pass that declares none");
	pxAssertMsg(!reads || self_mask != 0, "TileGpu draw reads its destination through a variant that compiles none");
	const u32 road_mask = TileGpuRoadMask(plan_road_mask);
	const u32 texel_mask = TileGpuTexelMask(road_mask, plan_texel_mask);
	// ...and the frozen state narrowed the same way, against the road this DEVICE serves rather than
	// the one the plan asked for: a field the program cannot read must not be in the key, or two
	// character-identical programs become two modules and two pipelines. Then the driver's own
	// narrowing, for the axis Honeykrisp may not be trusted with. Both run BEFORE the spec reaches
	// the #define block or either cache key below, so the program and its key cannot disagree.
	GSDevice::GSTileGpuFragmentSpec spec = plan_spec;
	spec.NarrowToRoad(road_mask);
	spec.NarrowToDriver(TileGpuFreezeTexa());
	const u32 all_roads = TileGpuRoadMask(GSDevice::kGSTileGpuRoadMaskAll);
	// The eager table is the FULL program, which on Adreno sits past the instruction-size threshold --
	// so a pass reaches it only by genuinely being the full set, never as a convenience. It is also
	// built against the ordinary render pass, so a declaring pass may never reach it.
	const bool full_variant =
		road_mask == all_roads && texel_mask == TileGpuTexelMask(all_roads, GSDevice::kGSTileGpuTexelMaskAll);
	if (!spec.valid && !declares && self_mask == 0 && !quantise && full_variant && !blend &&
		color_write_mask == 0xFu)
		return m_tilegpu_pipeline[topology][depth_mode];
	const u32 blend_index = blend ? (blend_key & 0x7Fu) : kTileGpuNoBlend;
	// Which road the draw's As factor takes -- pipeline state, because it decides what the row's
	// SRC1_* factors become and whether the alpha channel is blended. Read only where there IS a
	// fixed-function blend, so a draw whose equation the shader owns cannot key two pipelines.
	const u32 dualsrc_road = blend ? (blend_key & GSTileGpuPassPlan::kDualSrcRoadMask) : 0u;
	// Bits 0-1 topology, 2-3 depth, 8-15 blend, 16-19 the colour write mask, 20-22 the road mask,
	// 23-29 the texel-arm mask, 32-34 the pass's self-read mask, 35 declares, 36 reads, 37 quantise,
	// 38-55 the run's frozen per-draw GS state (the plan key's spec half, shifted down to 38),
	// 56-57 the As factor's road.
	const u64 spec_bits = static_cast<u64>((GSTileGpuPassPlan::PackVariantKey(0, 0, 0, false, spec) &
											   GSTileGpuPassPlan::kVariantSpecMask) >>
										   14);
	const u64 key = static_cast<u64>(topology) | (static_cast<u64>(depth_mode) << 2) |
					(static_cast<u64>(blend ? (blend_key & 0x7Fu) : 0x80u) << 8) |
					(static_cast<u64>(color_write_mask) << 16) | (static_cast<u64>(road_mask) << 20) |
					(static_cast<u64>(texel_mask) << 23) | (static_cast<u64>(self_mask) << 32) |
					(static_cast<u64>(declares ? 1u : 0u) << 35) | (static_cast<u64>(reads ? 1u : 0u) << 36) |
					(static_cast<u64>(quantise ? 1u : 0u) << 37) | (spec_bits << 38) |
					(static_cast<u64>(dualsrc_road >> GSTileGpuPassPlan::kDualSrcRoadShift) << 56);
	const auto it = m_tilegpu_blend_pipelines.find(key);
	if (it != m_tilegpu_blend_pipelines.end())
		return (it->second != VK_NULL_HANDLE) ? it->second : TileGpuPipelineFallback(topology, depth_mode, declares);
	VkPipeline pipe = CreateTileGpuPipeline(topology, depth_mode, blend_index, color_write_mask, road_mask,
		texel_mask, self_mask, quantise, declares, reads, dualsrc_road, spec);
	m_tilegpu_blend_pipelines.emplace(key, pipe);
	return (pipe != VK_NULL_HANDLE) ? pipe : TileGpuPipelineFallback(topology, depth_mode, declares);
}

// What a failed build falls back to. Outside a declaring pass that is the eager full-road pipeline --
// wrong-fast, never a dropped draw. Inside one there is nothing to fall back TO: the eager table is
// built against the ordinary render pass and binding it in a declaring pass is not a wrong blend, it
// is an incompatible render pass. So the run is dropped and said out loud instead.
VkPipeline GSDeviceVK::TileGpuPipelineFallback(u32 topology, u32 depth_mode, bool declares)
{
	if (!declares)
	{
		// Counted, because this fallback is not the cosmetic wrong-blend it was written for. It writes
		// all four channels, so a masked draw that lands on it paints channels the GS preserves -- the
		// Beyond Good & Evil ratchet -- and it carries the full program, which on the Adreno 650 is past
		// the instruction-size threshold. Both are visible in a frame and neither says anything.
		m_tilegpu_pipeline_fallback_runs++;
		if (!m_tilegpu_pipeline_fallback_warned)
		{
			m_tilegpu_pipeline_fallback_warned = true;
			Console.Error("TileGpu: a pipeline failed to build and its run fell back to the eager full-road "
						  "pipeline, which writes all four channels whatever the draw's FBMSK. Totals at teardown.");
		}
		return m_tilegpu_pipeline[topology][depth_mode];
	}
	if (!m_tilegpu_declared_fallback_warned)
	{
		m_tilegpu_declared_fallback_warned = true;
		Console.Error("TileGpu: a pipeline for a pass that declares the in-pass destination read failed to build; "
					  "its draws are dropped (there is no render-pass-compatible fallback).");
	}
	return VK_NULL_HANDLE;
}

// The target -> bytes reconciliation: a compute pass that reswizzles a resident colour target's
// listed pages into the frame's ring slots (named through the epoch page table), block- and
// byte-masked, so a later draw sampling those pages through the flat road reads the target's
// pixels. Binding 0 = the target (combined sampler), 1 = the ring SSBO (read-modify-write). Only
// meaningful when the swizzle forms fitted (m_tilegpu_tex): the shader shares tilegpu.glsl's
// TILE_SWZ_* defines, so writer and reader cannot disagree about a constant.
bool GSDeviceVK::CompileTileGpuWritebackPipeline(u32 road_fmt)
{
	pxAssert(road_fmt < kGSTileByteRoadFormats);
	m_tilegpu_writeback_tried[road_fmt] = true;

	// No texturing means no byte road to reconcile; the forms also must have fitted for the defines
	// to exist. Leave everything null -- the executor skips the op (the read stays whatever the slot
	// was prefilled with).
	if (!m_tilegpu_tex)
		return false;

	// The workgroup shape is this device's answer (TileGpuWritebackGroupDim says which and why), and
	// Vulkan only GUARANTEES 128 invocations and a 128x128 group size, where 16x16 is 256. Every part
	// we ship to gives 512 or more, but a device that does not would refuse the pipeline and the
	// executor would then skip every writeback op and hand its readers unwritten ring bytes -- a wrong
	// pixel with no message. Say so here instead, where the cause is still in hand.
	const u32 wg_dim = TileGpuWritebackGroupDim();
	pxAssertMsg(gsTileWritebackGroupDimFits(wg_dim), "the writeback workgroup edge must tile a page in whole blocks");
	{
		const VkPhysicalDeviceLimits& lim = m_device_properties.limits;
		if (lim.maxComputeWorkGroupInvocations < wg_dim * wg_dim || lim.maxComputeWorkGroupSize[0] < wg_dim ||
			lim.maxComputeWorkGroupSize[1] < wg_dim)
		{
			Console.Error("TileGpu: this device takes at most %u compute invocations of %ux%u per workgroup and "
						  "the writeback wants %ux%u; its ops will be skipped. Lower TileGpuWritebackGroupDim.",
				lim.maxComputeWorkGroupInvocations, lim.maxComputeWorkGroupSize[0], lim.maxComputeWorkGroupSize[1],
				wg_dim, wg_dim);
			return false;
		}
	}

	// The set and pipeline layouts are shared by every format's program; only the first build makes
	// them.
	if (m_tilegpu_writeback_pipeline_layout == VK_NULL_HANDLE)
	{
		Vulkan::DescriptorSetLayoutBuilder dslb;
		if (m_use_push_descriptors)
			dslb.SetPushFlag();
		dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
		dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
		// The same buffer again, seen as uvec4s. The shader's whole-word arm stages a block through
		// shared memory and stores it four words at a time, and GLSL has no way to spell a 16-byte
		// store into an array declared as words. Two bindings, one VkBuffer, one range.
		dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
		if ((m_tilegpu_writeback_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
			return false;
		Vulkan::SetObjectName(m_device, m_tilegpu_writeback_ds_layout, "TileGpu writeback DS layout");

		Vulkan::PipelineLayoutBuilder plb;
		plb.AddDescriptorSet(m_tilegpu_writeback_ds_layout);
		plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32) * kTileGpuPushWords);
		if ((m_tilegpu_writeback_pipeline_layout = plb.Create(m_device)) == VK_NULL_HANDLE)
			return false;
		Vulkan::SetObjectName(m_device, m_tilegpu_writeback_pipeline_layout, "TileGpu writeback pipeline layout");
	}

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/tilegpu_writeback.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tilegpu_writeback.glsl.");
		return false;
	}

	// GetComputeShader compiles the source verbatim (no #version injected, unlike the utility path),
	// so the version line leads and the swizzle-form defines follow it before the body.
	const std::string full_source = "#version 460 core\n\n" + TileFormDefines() +
									fmt::format("#define TILEGPU_WB_FMT {}\n", road_fmt) +
									fmt::format("#define TILEGPU_WB_WG {}\n", wg_dim) + *source;
	VkShaderModule cs = g_vulkan_shader_cache->GetComputeShader(full_source);
	if (cs == VK_NULL_HANDLE)
		return false;
	ScopedGuard cs_guard([this, &cs]() { vkDestroyShaderModule(m_device, cs, nullptr); });

	Vulkan::ComputePipelineBuilder cpb;
	cpb.SetPipelineLayout(m_tilegpu_writeback_pipeline_layout);
	cpb.SetShader(cs, "main");
	m_tilegpu_writeback_pipeline[road_fmt] = cpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (m_tilegpu_writeback_pipeline[road_fmt] == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(m_device, m_tilegpu_writeback_pipeline[road_fmt], "TileGpu writeback pipeline");
	return true;
}

// The bytes -> target reconciliation: a fragment pass over a colour target that unswizzles the
// op's pages out of the ring into the target's pixel space, discarding outside the page set. It
// shares the geometry pipeline's layout and persistent descriptor set (binding 1 is the ring), so
// running it costs a pipeline bind and a push, no descriptor traffic. Compiled on first use, only
// when the swizzle forms fitted.
bool GSDeviceVK::CompileTileGpuSeedPipeline(u32 road_fmt)
{
	pxAssert(road_fmt < kGSTileByteRoadFormats);
	m_tilegpu_seed_tried[road_fmt] = true;
	if (!m_tilegpu_tex || m_tilegpu_pipeline_layout == VK_NULL_HANDLE)
		return false;

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/tilegpu_seed.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tilegpu_seed.glsl.");
		return false;
	}
	// TILEGPU_STATIC_BYTE_SEL rides along for the same reason the materialise takes it: the 16-bit
	// arm extracts a halfword from an SSBO-loaded word with a computed shift, which is the shape
	// Honeykrisp gets wrong.
	const std::string full_source = TileFormDefines() +
									fmt::format("#define TILEGPU_STATIC_BYTE_SEL {}\n", TileGpuStaticByteSel() ? 1 : 0) +
									fmt::format("#define TILEGPU_SEED_FMT {}\n", road_fmt) + *source;

	VkShaderModule vs = GetUtilityVertexShader(full_source);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
	VkShaderModule fs = GetUtilityFragmentShader(full_source);
	if (fs == VK_NULL_HANDLE)
		return false;
	ScopedGuard fs_guard([this, &fs]() { vkDestroyShaderModule(m_device, fs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetPipelineLayout(m_tilegpu_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetNoDepthTestState();
	gpb.SetNoStencilState();
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);
	gpb.SetRenderPass(GetRenderPass(LookupNativeFormat(GSTexture::Format::Color),
						  LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_LOAD,
						  VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE),
		0);
	gpb.SetColorWriteMask(0,
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	m_tilegpu_seed_pipeline[road_fmt] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (m_tilegpu_seed_pipeline[road_fmt] == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(m_device, m_tilegpu_seed_pipeline[road_fmt], "TileGpu seed pipeline");
	return true;
}

// The same pass with the depth attachment as its destination: no colour attachment, depth compare
// ALWAYS and depth write on (Vulkan writes depth only when the TEST is enabled, so "no test" here is
// spelled as a test that always passes). The one road that fills a Z buffer from guest bytes; there
// is still no road back.
bool GSDeviceVK::CompileTileGpuSeedDepthPipeline(u32 depth_fmt)
{
	pxAssert(depth_fmt < kGSTileDepthRoadFormats);
	m_tilegpu_seed_depth_tried[depth_fmt] = true;
	if (!m_tilegpu_tex || m_tilegpu_pipeline_layout == VK_NULL_HANDLE)
		return false;

	// Fits the swizzle forms if nothing has yet; the 16-bit guard below reads one of them.
	const std::string& defines = TileFormDefines();

	// The 16-bit arms fold the depth block XOR into the IN-PAGE block index, which is the same map
	// as GSOffset's absolute-block XOR only while the constant stays under 32. Fit() proves that for
	// the 32-bit constant and the writeback road depends on it; nothing proved it for the 16-bit one
	// because until now nothing used it, so it is checked here rather than widened into a FormSet
	// validity rule the Tile renderer would also inherit.
	const bool sixteen = depth_fmt == 2 || depth_fmt == 3 || depth_fmt == 6 || depth_fmt == 7;
	if (sixteen && m_tile_forms.z16_block_xor >= 32)
		return false;

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/tilegpu_seed.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tilegpu_seed.glsl.");
		return false;
	}
	const std::string full_source = defines +
									fmt::format("#define TILEGPU_STATIC_BYTE_SEL {}\n", TileGpuStaticByteSel() ? 1 : 0) +
									"#define TILEGPU_SEED_DEPTH 1\n" +
									fmt::format("#define TILEGPU_SEED_FMT {}\n", depth_fmt) + *source;

	VkShaderModule vs = GetUtilityVertexShader(full_source);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
	VkShaderModule fs = GetUtilityFragmentShader(full_source);
	if (fs == VK_NULL_HANDLE)
		return false;
	ScopedGuard fs_guard([this, &fs]() { vkDestroyShaderModule(m_device, fs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetPipelineLayout(m_tilegpu_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetDepthState(true, true, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);
	gpb.SetRenderPass(GetRenderPass(VK_FORMAT_UNDEFINED, LookupNativeFormat(GSTexture::Format::DepthStencil),
						  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
						  VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE),
		0);
	m_tilegpu_seed_depth_pipeline[depth_fmt] =
		gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (m_tilegpu_seed_depth_pipeline[depth_fmt] == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(m_device, m_tilegpu_seed_depth_pipeline[depth_fmt], "TileGpu depth seed pipeline");
	return true;
}

bool GSDeviceVK::TileGpuStaticByteSel() const
{
	return m_device_driver_properties.driverID == VK_DRIVER_ID_MESA_HONEYKRISP;
}

// The same driver, the same miscompile class, one axis further on. Freezing TEXA into the fragment
// program moves six pixels of one corpus frame on Honeykrisp -- green channel, |delta| <= 8, two 2x2
// quads of a bilinear byte-road draw -- and the source is not at fault: the frozen constants provably
// equal the state row's, every contractible float op already carries NoContraction, and the same
// binary renders all eighteen corpus dumps byte-identically under lavapipe. It needs five frozen axes
// interacting and any one of them read back off the row hides it again, which is a code-shape effect
// rather than a wrong value. So TEXA comes off the frozen set here and nowhere else; every other
// driver keeps all ten axes.
bool GSDeviceVK::TileGpuFreezeTexa() const
{
	return m_device_driver_properties.driverID != VK_DRIVER_ID_MESA_HONEYKRISP;
}

// Bytes -> a texture source: a fragment pass over the source image that unswizzles one texel per
// fragment out of the ring, so rule 3 can sample the window with the hardware sampler. Like the
// seed it shares the geometry pipeline's layout and persistent descriptor set (binding 1 is the
// ring), so it costs a pipeline bind and a push and nothing else. One pipeline per source format,
// compiled on first use of that format, only when the swizzle forms fitted.
u32 GSDeviceVK::TileGpuSrcFormatForPsm(u32 psm)
{
	switch (psm)
	{
		case PSMCT32: return 0;
		case PSMCT24: return 1;
		case PSMT8: return 2;
		case PSMT4: return 3;
		// The alpha-byte views. Their own pipelines rather than PSMCT32's plus a fixup, because the
		// materialise is one program per format by design -- the arithmetic differs and a runtime
		// branch on it would be paid per texel of every window the road ever builds.
		case PSMT8H: return 4;
		case PSMT4HL: return 5;
		case PSMT4HH: return 6;
		default: return kTileGpuSrcFormats;
	}
}

bool GSDeviceVK::CompileTileGpuMaterialisePipeline(u32 src_fmt)
{
	pxAssert(src_fmt < kTileGpuSrcFormats);
	m_tilegpu_materialise_tried[src_fmt] = true;
	if (!m_tilegpu_tex || m_tilegpu_pipeline_layout == VK_NULL_HANDLE)
		return false;

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/tilegpu_materialise.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tilegpu_materialise.glsl.");
		return false;
	}
	const std::string full_source = TileFormDefines() +
									fmt::format("#define TILEGPU_STATIC_BYTE_SEL {}\n", TileGpuStaticByteSel() ? 1 : 0) +
									fmt::format("#define TILEGPU_SRC_FMT {}\n", src_fmt) + *source;

	VkShaderModule vs = GetUtilityVertexShader(full_source);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
	VkShaderModule fs = GetUtilityFragmentShader(full_source);
	if (fs == VK_NULL_HANDLE)
		return false;
	ScopedGuard fs_guard([this, &fs]() { vkDestroyShaderModule(m_device, fs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetPipelineLayout(m_tilegpu_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetNoDepthTestState();
	gpb.SetNoStencilState();
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);
	gpb.SetRenderPass(GetRenderPass(LookupNativeFormat(GSTexture::Format::Color),
						  LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_LOAD,
						  VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE),
		0);
	gpb.SetColorWriteMask(0,
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (pipe == VK_NULL_HANDLE)
	{
		// Say it once per format: without this the sources are allocated and never filled, which
		// looks like a texture-cache bug at every level above here rather than a missing pipeline.
		Console.Error("VK: TileGpu materialise pipeline (source format %u) failed to build -- rule 3 "
					  "sources will not be filled.",
			src_fmt);
		return false;
	}
	m_tilegpu_materialise_pipeline[src_fmt] = pipe;
	Vulkan::SetObjectName(m_device, pipe, "TileGpu materialise pipeline (fmt %u)", src_fmt);
	return true;
}

// Indices + palette -> colour: rule 3's paletted second stage. Same arithmetic as
// ps_tile_expand_palette, recorded RAW rather than through TileExpandPalette -- see the header of
// tilegpu_expand.glsl for why that distinction is load-bearing and not a preference. Its own
// two-sampler descriptor layout, so it depends on no other road's compile; the shader takes no push
// constants (level 0 only) and the layout declares none.
bool GSDeviceVK::CompileTileGpuExpandPipeline()
{
	m_tilegpu_expand_tried = true;

	{
		Vulkan::DescriptorSetLayoutBuilder dslb;
		if (m_use_push_descriptors)
			dslb.SetPushFlag();
		dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		if ((m_tilegpu_expand_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
			return false;
		Vulkan::SetObjectName(m_device, m_tilegpu_expand_ds_layout, "TileGpu expand DS layout");

		Vulkan::PipelineLayoutBuilder plb;
		plb.AddDescriptorSet(m_tilegpu_expand_ds_layout);
		if ((m_tilegpu_expand_pipeline_layout = plb.Create(m_device)) == VK_NULL_HANDLE)
			return false;
		Vulkan::SetObjectName(m_device, m_tilegpu_expand_pipeline_layout, "TileGpu expand pipeline layout");
	}

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/tilegpu_expand.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tilegpu_expand.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*source);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
	VkShaderModule fs = GetUtilityFragmentShader(*source);
	if (fs == VK_NULL_HANDLE)
		return false;
	ScopedGuard fs_guard([this, &fs]() { vkDestroyShaderModule(m_device, fs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetPipelineLayout(m_tilegpu_expand_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetNoDepthTestState();
	gpb.SetNoStencilState();
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);
	gpb.SetRenderPass(GetRenderPass(LookupNativeFormat(GSTexture::Format::Color),
					  LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_LOAD,
					  VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE),
		0);
	gpb.SetColorWriteMask(0,
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (pipe == VK_NULL_HANDLE)
	{
		// Say it once: without this a paletted source is allocated and never filled, which reads as a
		// texture-cache bug at every level above here rather than as a missing pipeline.
		Console.Error("VK: TileGpu expand pipeline failed to build -- rule 3's paletted sources will not "
					  "be filled.");
		return false;
	}
	m_tilegpu_expand_pipeline = pipe;
	Vulkan::SetObjectName(m_device, pipe, "TileGpu expand pipeline");
	return true;
}

// A resident target -> an index image: rule 3's DONOR build, the road for a window whose pages one
// live target solely owns. Same arithmetic as ps_tile_reinterpret_index, recorded raw rather than
// through GSDevice::TileReinterpretIndex -- see tilegpu_reinterpret.glsl's header for why that
// distinction is load-bearing. Its own one-sampler descriptor layout and its own four-word push
// range, so it depends on no other road's compile.
// The one descriptor and pipeline layout the two read-a-target roads share: one combined image
// sampler (the owner) and four push words (the source layout and the owner's). Built once, by
// whichever road compiles first.
bool GSDeviceVK::CompileTileGpuReadTargetLayout()
{
	if (m_tilegpu_readtarget_pipeline_layout != VK_NULL_HANDLE)
		return true;

	Vulkan::DescriptorSetLayoutBuilder dslb;
	if (m_use_push_descriptors)
		dslb.SetPushFlag();
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tilegpu_readtarget_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(m_device, m_tilegpu_readtarget_ds_layout, "TileGpu read-target DS layout");

	Vulkan::PipelineLayoutBuilder plb;
	plb.AddDescriptorSet(m_tilegpu_readtarget_ds_layout);
	plb.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, kTileGpuDonorPushWords * sizeof(u32));
	if ((m_tilegpu_readtarget_pipeline_layout = plb.Create(m_device)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(m_device, m_tilegpu_readtarget_pipeline_layout, "TileGpu read-target pipeline layout");
	return true;
}

// One read-a-target pipeline, from a shader that takes the shared layout. `defines` carries the
// per-variant constants; `what` names the road for the one-shot failure message, which exists
// because a missing pipeline leaves a prep texture allocated and never filled -- and that reads as a
// texture-cache bug at every level above here.
VkPipeline GSDeviceVK::CompileTileGpuReadTargetPipeline(const char* shader, const std::string& defines,
	const char* what)
{
	// The arithmetic IS the fitted forms; without them there are no constants to compile.
	if (TileFormDefines().empty() || !CompileTileGpuReadTargetLayout())
		return VK_NULL_HANDLE;

	const std::optional<std::string> source = ReadShaderSource(shader);
	if (!source)
	{
		Host::ReportErrorAsync("GS", fmt::format("Failed to read {}.", shader));
		return VK_NULL_HANDLE;
	}
	const std::string full_source = TileFormDefines() + defines + *source;

	VkShaderModule vs = GetUtilityVertexShader(full_source);
	if (vs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
	VkShaderModule fs = GetUtilityFragmentShader(full_source);
	if (fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	ScopedGuard fs_guard([this, &fs]() { vkDestroyShaderModule(m_device, fs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetPipelineLayout(m_tilegpu_readtarget_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetNoDepthTestState();
	gpb.SetNoStencilState();
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);
	gpb.SetRenderPass(GetRenderPass(LookupNativeFormat(GSTexture::Format::Color),
					  LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_LOAD,
					  VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE),
		0);
	gpb.SetColorWriteMask(0,
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (pipe == VK_NULL_HANDLE)
		Console.Error("VK: TileGpu %s pipeline failed to build -- that road will not be served.", what);
	return pipe;
}

bool GSDeviceVK::CompileTileGpuDonorPipeline(u32 idx_fmt)
{
	pxAssert(idx_fmt < kTileGpuIdxFormats);
	m_tilegpu_donor_tried[idx_fmt] = true;
	VkPipeline pipe = CompileTileGpuReadTargetPipeline("shaders/vulkan/tilegpu_reinterpret.glsl",
		fmt::format("#define TILE_IDX_FMT {}\n", idx_fmt), "donor");
	if (pipe == VK_NULL_HANDLE)
		return false;
	m_tilegpu_donor_pipeline[idx_fmt] = pipe;
	Vulkan::SetObjectName(m_device, pipe, "TileGpu donor pipeline (idx fmt %u)", idx_fmt);
	return true;
}

bool GSDeviceVK::CompileTileGpuClutGatherPipeline(u32 size_idx)
{
	pxAssert(size_idx < kTileGpuClutSizes);
	m_tilegpu_clut_tried[size_idx] = true;
	// The entry -> word order is a fitted form of its own with its own validity bit: a device whose
	// CLUT forms did not fit must not gather, even where the block forms did. TileFormDefines()
	// performs the fit, so it is asked first.
	if (TileFormDefines().empty() || !m_tile_forms.clut_valid)
		return false;
	VkPipeline pipe = CompileTileGpuReadTargetPipeline("shaders/vulkan/tilegpu_clutgather.glsl",
		fmt::format("#define TILE_CLUT_ENTRIES {}\n", (size_idx == 0) ? 256 : 16), "CLUT gather");
	if (pipe == VK_NULL_HANDLE)
		return false;
	m_tilegpu_clut_pipeline[size_idx] = pipe;
	Vulkan::SetObjectName(m_device, pipe, "TileGpu CLUT gather pipeline (%u entries)", (size_idx == 0) ? 256 : 16);
	return true;
}

// EmuCore/GS/TileGpuPoisonAllocations: one loud colour per allocation class, so that a read of
// memory nothing ever wrote is deterministic AND self-identifying -- the colour names the class
// that was under-covered. The ring value is a raw dword because vkCmdFillBuffer takes one; RGBA8
// is little-endian on the byte road, so 0xFFFF00FF is bytes FF 00 FF FF = opaque magenta.
static constexpr u32 kTileGpuPoisonRingSlot = 0xFFFF00FFu;                                 // magenta
static constexpr VkClearColorValue kTileGpuPoisonSource = {{0.0f, 1.0f, 1.0f, 1.0f}};      // cyan
static constexpr VkClearColorValue kTileGpuPoisonSnapshot = {{1.0f, 1.0f, 0.0f, 1.0f}};    // yellow

bool GSDeviceVK::ExecuteTileGpuPassPlan(const GSTileGpuPassPlan& plan)
{
	if (!m_optional_extensions.tilegpu_device_capable)
		return false;

	// An empty plan is a valid frame the executor completes with nothing to record.
	if (plan.passes.empty())
		return true;

	// Record the frame's geometry as constant-cost indirect draws. The draw commands go into an
	// indirect buffer (GSTileGpuIndirectDraw is byte-identical to VkDrawIndexedIndirectCommand, so
	// this is a straight copy), the per-draw state into a state SSBO the VS/FS read by first_instance,
	// and each pass issues one vkCmdDrawIndexedIndirect per maximal same-topology run rather than one
	// vkCmdDrawIndexed per draw. The state row carries the transform, the texture block and the
	// per-draw tests; the scissor is a command, not a row field. The shader's StateRow is a fixed
	// 144-byte layout (kStateRowWords), so state_stride must match it exactly.
	if (!m_tilegpu_tried)
		CompileTileGpuPipeline();
	const bool pipelines_ok =
		m_tilegpu_pipeline[0][0] != VK_NULL_HANDLE && m_tilegpu_state_descriptor_set != VK_NULL_HANDLE;
	const bool have_geometry = !plan.draws.empty() && plan.topologies.size() == plan.draws.size() &&
							   plan.vertex_stride == sizeof(GSVertex) && !plan.vertices.empty() &&
							   plan.state_table != nullptr &&
							   plan.state_stride == sizeof(u32) * GSTileGpuPassPlan::kStateRowWords &&
							   plan.scissors.size() == plan.draws.size() && plan.state_count > 0;
	const bool can_draw = pipelines_ok && have_geometry;

	// A frame that fails either half records not one draw, and NOTHING else about it changes: the pass
	// loop below still opens every pass, still clears or loads it, still stores it, and the frame is
	// presented. That is a deterministic black frame at close to the full tile bill, and every column
	// outside the executor -- the planner census, the pass and draw counts, the pixel bill -- is built
	// before this point and reports the frame the planner intended. So the gate says which conjunct
	// failed and with what values, once, and the totals go out at teardown; a device that renders black
	// must never again be silent about why.
	//
	// ⚠️ ...but only for a plan that HAS geometry to lose. A plan can legitimately consist of nothing
	// but reconciliation: the display materialise and the upload merge each append a draw that renders
	// zero indices purely to carry a prep-op range, so a plan flushed with no ordinary draw in it has an
	// empty vertex stream, and refusing it costs exactly the nothing it would have drawn. Those are
	// ordinary -- one turned up in the first mgs3 spot run this counter existed for -- and counting them
	// would bury the frames that really did lose their geometry under the frames that never had any.
	if (!can_draw && !plan.vertices.empty())
	{
		m_tilegpu_undrawn_frames++;
		m_tilegpu_undrawn_draws += static_cast<u32>(plan.draws.size());
		if (!m_tilegpu_undrawn_warned)
		{
			m_tilegpu_undrawn_warned = true;
			Console.Error("TileGpu: a frame's %zu planned draws were not recorded -- pipelines=%s, "
						  "state set=%s, draws=%zu, topologies=%zu, scissors=%zu, vertices=%zu bytes, "
						  "vertex_stride=%u (want %zu), state_table=%s, state_stride=%u (want %zu), "
						  "state_count=%u. Its passes still clear and store, so the frame is black rather "
						  "than absent. Totals at teardown.",
				plan.draws.size(), (m_tilegpu_pipeline[0][0] != VK_NULL_HANDLE) ? "yes" : "no",
				(m_tilegpu_state_descriptor_set != VK_NULL_HANDLE) ? "yes" : "no", plan.draws.size(),
				plan.topologies.size(), plan.scissors.size(), plan.vertices.size(), plan.vertex_stride,
				sizeof(GSVertex), (plan.state_table != nullptr) ? "yes" : "no", plan.state_stride,
				sizeof(u32) * GSTileGpuPassPlan::kStateRowWords, plan.state_count);
		}
	}

	// The byte road rides along only when the frame carries ring pages AND the sampling path
	// compiled in (the swizzle forms fitted). Without it the state rows' tex_enable is still set but
	// the shader #ifdef'd the sampling out, so the ring is inert; the reconciliation ops are skipped.
	//
	// ⚠️ It does NOT require the plan to carry GEOMETRY, and that is not a relaxation for its own
	// sake. A plan can legitimately consist of nothing but reconciliation: the display materialise
	// and the upload merge both append a pending draw that renders zero indices purely to carry a
	// prep-op range, and a plan flushed with no ordinary draw in it then has an empty vertex stream.
	// Gating the byte road on the geometry stream made the executor silently DISCARD such a plan's
	// ops -- the renderer's model recorded the reconciliation as done and no bytes moved, which is
	// the exact shape of a wrong pixel nothing downstream can correct.
	const bool can_texture = pipelines_ok && m_tilegpu_tex && !plan.ring_pages.empty() && plan.epoch_count > 0;

	EndRenderPass();

	// Rule 3's frame-wide source set, TAKEN first -- ahead of every stream this plan stages, because
	// taking it can close the command buffer (WriteTileGpuSourceSet: the descriptor ring wraps inside
	// one buffer on the stall-heavy titles, and only a submission can free a set the buffer is still
	// recording against). Nothing staged yet means a submit here costs nothing and strands nothing;
	// after the staging it would strand the stream reservations the plan is about to reference.
	u32 source_set_index = kTileGpuSourceSetsMax;
	VkDescriptorSet source_set = VK_NULL_HANDLE;
	if (can_draw && !plan.sources.empty())
	{
		source_set_index = WriteTileGpuSourceSet(plan.sources);
		if (source_set_index < kTileGpuSourceSetsMax)
			source_set = m_tilegpu_source_sets[source_set_index];
	}

	// Stage the whole frame's streams before opening any pass: a ring wrap here can flush the
	// command buffer, which must not happen mid-render-pass. The indirect commands' vertex/index
	// offsets are frame-relative, so the streams get bound at their base (below) rather than rebased
	// per draw; base_row rebases first_instance into this frame's slice of the state ring.
	u32 vbase = 0, ibase = 0, state_base_rows = 0, indirect_base_bytes = 0;
	u32 table_base_words = 0, pal_base_words = 0, entries_base_words = 0, masks_base_words = 0;
	u32 keep_base_words = 0;
	if (can_draw)
	{
		IASetVertexBuffer(plan.vertices.data(), sizeof(GSVertex), plan.vertices.size() / sizeof(GSVertex), 1);
		IASetIndexBuffer(plan.indices.data(), plan.indices.size());
		vbase = m_vertex.start;
		ibase = m_index.start;

		const u32 state_bytes = plan.state_count * plan.state_stride;
		if (!m_tilegpu_state_stream_buffer.ReserveMemory(state_bytes, plan.state_stride))
		{
			ExecuteCommandBufferAndRestartRenderPass(false, "Uploading TileGpu state table");
			RetainTileGpuStreamsForCurrentCommandBuffer();
			if (!m_tilegpu_state_stream_buffer.ReserveMemory(state_bytes, plan.state_stride))
				pxFailRel("Failed to reserve TileGpu state table");
		}
		std::memcpy(m_tilegpu_state_stream_buffer.GetCurrentHostPointer(), plan.state_table, state_bytes);
		state_base_rows = m_tilegpu_state_stream_buffer.GetCurrentOffset() / plan.state_stride;
		m_tilegpu_state_stream_buffer.CommitMemory(state_bytes);

		const u32 indirect_bytes = static_cast<u32>(plan.draws.size() * sizeof(GSTileGpuIndirectDraw));
		if (!m_tilegpu_indirect_stream_buffer.ReserveMemory(indirect_bytes, sizeof(u32)))
		{
			ExecuteCommandBufferAndRestartRenderPass(false, "Uploading TileGpu draw commands");
			RetainTileGpuStreamsForCurrentCommandBuffer();
			if (!m_tilegpu_indirect_stream_buffer.ReserveMemory(indirect_bytes, sizeof(u32)))
				pxFailRel("Failed to reserve TileGpu draw commands");
		}
		std::memcpy(m_tilegpu_indirect_stream_buffer.GetCurrentHostPointer(), plan.draws.data(), indirect_bytes);
		indirect_base_bytes = m_tilegpu_indirect_stream_buffer.GetCurrentOffset();
		m_tilegpu_indirect_stream_buffer.CommitMemory(indirect_bytes);
	}

	// The allocation-poison lever, EmuCore/GS/TileGpuPoisonAllocations. Off is the shipped position
	// and records NOTHING: every site below is behind this flag, so the command stream at the
	// default is the one this executor emitted before the lever existed. Announced once per run,
	// not once per plan -- a plan is a frame, and the line exists so a device log can testify the
	// arm engaged, which one line does.
	const bool poison = GSConfig.TileGpuPoisonAllocations;
	if (poison && !m_tilegpu_poison_announced)
	{
		m_tilegpu_poison_announced = true;
		Console.WriteLn("TileGpu allocation poison ENGAGED (EmuCore/GS/TileGpuPoisonAllocations): every GPU "
						"allocation TileGpu owns is filled with a per-class sentinel as it enters service -- "
						"MAGENTA an un-composed byte-slab ring slot, CYAN a region of a materialised source "
						"image the build never covered, YELLOW a region of the per-pass snapshot scratch the "
						"copy never covered. A sentinel colour in a frame is a read of memory nothing wrote, "
						"and the colour names the class. The persistent target pool and the host-visible "
						"stream buffers are deliberately not poisoned. Diagnostic only.");
	}

	// The frame's byte road, staged in one reservation so its parts cannot straddle a ring wrap:
	//   [zero slot 8 KB][one 8 KB slot per ring page][epoch page tables][page entries]
	//   [seed page masks][seed per-page block masks][writeback keep masks][palettes]
	// Slots the renderer named a source for are memcpy'd from it (the CPU shadow's bytes, or a
	// version copy); the rest are left for the writeback compute to compose. Every table entry
	// starts at the zero slot and each ring page then claims its epoch range, so a page the plan
	// never staged for an epoch reads as zeros rather than as another page's bytes.
	if (can_texture)
	{
		static constexpr u32 kPageBytes = GS_PAGE_SIZE;
		static constexpr u32 kPageWords = kPageBytes / sizeof(u32);
		const u32 slot_bytes = (1 + static_cast<u32>(plan.ring_pages.size())) * kPageBytes;
		const u32 table_bytes = plan.epoch_count * GS_MAX_PAGES * sizeof(u32);
		const u32 entry_bytes = static_cast<u32>(plan.page_entries.size() * sizeof(GSTileGpuPageEntry));
		// One 512-bit page mask per seed op, built below; reserve for every op (writebacks skip theirs).
		const u32 mask_words = static_cast<u32>(plan.prep_ops.size()) * (GS_MAX_PAGES / 32);
		const u32 mask_bytes = mask_words * sizeof(u32);
		// ...and one 512-ENTRY block-mask table per seed op that carries a mask per page, which is
		// the upload merge's batched seed and nothing else. Indexed by guest page like the mask
		// above, so it is dense and 2 KB; counted rather than reserved for every op because a frame
		// carries hundreds of ordinary seeds whose mask is uniform and needs no table at all.
		u32 block_table_ops = 0;
		for (const GSTileGpuPrepOp& op : plan.prep_ops)
		{
			if ((op.kind == GSTileGpuPrepKind::Seed || op.kind == GSTileGpuPrepKind::SeedDepth) &&
				op.seed_blocks_per_page != 0)
			{
				block_table_ops++;
			}
		}
		const u32 block_words = block_table_ops * GS_MAX_PAGES;
		const u32 block_bytes = block_words * sizeof(u32);
		// The upload merge's per-page keep-mask tables, verbatim. Empty on every frame that
		// plans no merge, which is most of them.
		const u32 keep_bytes = static_cast<u32>(plan.writeback_keep_masks.size() * sizeof(u32));
		const u32 pal_bytes = static_cast<u32>(plan.palettes.size() * sizeof(u32));
		const u32 total = slot_bytes + table_bytes + entry_bytes + mask_bytes + block_bytes + keep_bytes + pal_bytes;
		if (!m_tilegpu_vram_stream_buffer.ReserveMemory(total, kPageBytes))
		{
			ExecuteCommandBufferAndRestartRenderPass(false, "Uploading TileGpu ring");
			RetainTileGpuStreamsForCurrentCommandBuffer();
			if (!m_tilegpu_vram_stream_buffer.ReserveMemory(total, kPageBytes))
				pxFailRel("Failed to reserve TileGpu ring");
		}
		u8* const base = static_cast<u8*>(m_tilegpu_vram_stream_buffer.GetCurrentHostPointer());
		const u32 base_words = m_tilegpu_vram_stream_buffer.GetCurrentOffset() / sizeof(u32);

		std::memset(base, 0, kPageBytes); // the zero slot
		u32* const tables = reinterpret_cast<u32*>(base + slot_bytes);
		for (u32 i = 0; i < plan.epoch_count * GS_MAX_PAGES; i++)
			tables[i] = base_words; // -> zero slot
		for (u32 i = 0; i < plan.ring_pages.size(); i++)
		{
			const GSTileGpuRingPage& rp = plan.ring_pages[i];
			u8* const slot = base + (1 + i) * kPageBytes;
			if (rp.src)
				std::memcpy(slot, rp.src, kPageBytes);
			const u32 slot_words = base_words + (1 + i) * kPageWords;
			const u32 e1 = std::min<u32>(rp.epoch_last, plan.epoch_count - 1);
			for (u32 e = rp.epoch_first; e <= e1; e++)
				tables[e * GS_MAX_PAGES + rp.page] = slot_words;
		}

		// Poison class A -- magenta over every slot this plan left for the writeback compute.
		//
		// A slot the renderer named a source for was memcpy'd whole just above, so it is covered by
		// construction and is skipped. An UNPREFILLED slot is covered only if the writebacks this
		// frame emits reach every BYTE of all 32 of its blocks, which GSRendererTileGpu::EnsureRingSlot
		// computes rather than knows; a byte no writeback reached reads whatever the previous tenant of
		// this ring offset left, because only the zero slot above is memset. Magenta turns that from a
		// stale texel into a colour that names its own cause.
		//
		// With the coverage rule honest about bytes, poison ON must be BYTE-IDENTICAL to poison OFF on
		// every title: that identity is the standing proof that no draw consumes an unwritten ring byte,
		// and this lever is the tripwire that keeps it.
		//
		// ⚠️ The fill MUST skip the prefilled slots. The prefill is a host write to mapped memory and
		// the fill is a GPU transfer; record order does not order those two, and a submission's host
		// writes are all visible before any of its commands execute -- so a fill over a prefilled
		// slot erases real bytes on the GPU timeline every time, not occasionally. The zero slot is
		// skipped for a different reason: it is deliberately zero, and a page the plan never staged
		// reads it on purpose, so poisoning it would report the design as a defect.
		if (poison && !plan.ring_pages.empty())
		{
			// Read here, not from `cmd`: that is declared below, and the reservation retry above can
			// have swapped the command buffer out from under an earlier read.
			const VkCommandBuffer pcmd = GetCurrentCommandBuffer();
			const VkBuffer ring = m_tilegpu_vram_stream_buffer.GetBuffer();
			const VkDeviceSize base_bytes = m_tilegpu_vram_stream_buffer.GetCurrentOffset();
			constexpr VkPipelineStageFlags kRingStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
														 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
														 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			// The pair the CLUT block copy further down records around its own transfer, for the same
			// reason: ring reads already recorded must finish before the fill overwrites any of it,
			// and the fill must be visible to every stage that reads the ring after it.
			const auto fill_barrier = [&](VkPipelineStageFlags src_stage, VkAccessFlags src_access,
										  VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
				VkBufferMemoryBarrier bmb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
				bmb.srcAccessMask = src_access;
				bmb.dstAccessMask = dst_access;
				bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bmb.buffer = ring;
				bmb.offset = 0;
				bmb.size = VK_WHOLE_SIZE;
				vkCmdPipelineBarrier(pcmd, src_stage, dst_stage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
			};

			// Adjacent unprefilled slots coalesce into one fill: a plan carries hundreds of ring
			// pages and most of them are unprefilled on the frames that matter.
			u32 run_first = 0, run_len = 0, filled_slots = 0;
			const auto flush_run = [&]() {
				if (run_len == 0)
					return;
				if (filled_slots == 0)
				{
					fill_barrier(kRingStages, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
						VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
				}
				vkCmdFillBuffer(pcmd, ring, base_bytes + static_cast<VkDeviceSize>(1 + run_first) * kPageBytes,
					static_cast<VkDeviceSize>(run_len) * kPageBytes, kTileGpuPoisonRingSlot);
				filled_slots += run_len;
				run_len = 0;
			};
			for (u32 i = 0; i < plan.ring_pages.size(); i++)
			{
				if (plan.ring_pages[i].src)
				{
					flush_run();
					continue;
				}
				if (run_len == 0)
					run_first = i;
				run_len++;
			}
			flush_run();
			if (filled_slots != 0)
			{
				fill_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, kRingStages,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			}
		}

		table_base_words = base_words + slot_bytes / sizeof(u32);

		u8* const entries = base + slot_bytes + table_bytes;
		if (entry_bytes)
			std::memcpy(entries, plan.page_entries.data(), entry_bytes);
		entries_base_words = table_base_words + table_bytes / sizeof(u32);

		u32* const masks = reinterpret_cast<u32*>(entries + entry_bytes);
		masks_base_words = entries_base_words + entry_bytes / sizeof(u32);
		u32* const blocks = masks + mask_words;
		const u32 blocks_base_words = masks_base_words + mask_words;
		m_tilegpu_seed_block_bases.assign(plan.prep_ops.size(), 0);
		u32 block_table = 0;
		for (u32 o = 0; o < plan.prep_ops.size(); o++)
		{
			u32* const m = masks + o * (GS_MAX_PAGES / 32);
			std::memset(m, 0, (GS_MAX_PAGES / 32) * sizeof(u32));
			const GSTileGpuPrepOp& op = plan.prep_ops[o];
			if (op.kind != GSTileGpuPrepKind::Seed && op.kind != GSTileGpuPrepKind::SeedDepth)
				continue;
			// A page the op does not name reads a zero block mask as well as a clear page-mask bit,
			// so the table is zeroed whole rather than only where the entries land: a stale mask
			// left by the previous tenant of this ring offset would seed a page nothing asked about.
			u32* const b = (op.seed_blocks_per_page != 0) ? (blocks + block_table * GS_MAX_PAGES) : nullptr;
			if (b)
			{
				std::memset(b, 0, GS_MAX_PAGES * sizeof(u32));
				m_tilegpu_seed_block_bases[o] = blocks_base_words + block_table * GS_MAX_PAGES;
				block_table++;
			}
			for (u32 k = 0; k < op.page_entry_count; k++)
			{
				const GSTileGpuPageEntry& e = plan.page_entries[op.first_page_entry + k];
				m[e.page >> 5] |= 1u << (e.page & 31);
				if (b)
					b[e.page] = e.block_mask;
			}
		}
		pxAssert(block_table == block_table_ops);

		u8* const keeps = reinterpret_cast<u8*>(blocks) + block_bytes;
		if (keep_bytes)
			std::memcpy(keeps, plan.writeback_keep_masks.data(), keep_bytes);
		keep_base_words = blocks_base_words + block_words;

		if (pal_bytes)
			std::memcpy(keeps + keep_bytes, plan.palettes.data(), pal_bytes);
		pal_base_words = keep_base_words + keep_bytes / sizeof(u32);

		m_tilegpu_vram_stream_buffer.CommitMemory(total);
	}

	// A multi-draw indirect covers at most this many commands; a longer topology run is split.
	const u32 max_indirect = std::max<u32>(1u, GetDeviceProperties().limits.maxDrawIndirectCount);

	// NOT const: from grade 2 the ordering lever below ends this command buffer mid-plan and
	// continues on the next one, and every command the executor records goes through this handle.
	// The lambdas that record barriers capture it by reference for the same reason.
	VkCommandBuffer cmd = GetCurrentCommandBuffer();

	// The ordering lever, EmuCore/GS/TileGpuSerializeOps. Grade 0 is the shipped position and
	// records NOTHING: every call site is behind the grade, so the command stream at 0 is the one
	// this executor emitted before the lever existed.
	//
	// It answers one question, and only on r44p1 Mali: TileGpu is run-to-run nondeterministic there
	// and stable everywhere else on the same binary, with sync validation clean, so the standing
	// theory is that the blob drops or weakens ordering edges inside a producer->consumer graph this
	// deep in one command buffer. Garbage that vanishes as the grade rises proves that on silicon;
	// garbage that survives grade 3 kills it.
	const u32 serialize = gsTileGpuSerializeOps(GSConfig.TileGpuSerializeOps);
	// ...and WHERE, EmuCore/GS/TileGpuSerializeMask: one bit per GSTileGpuSerializeSite, so a device
	// round can bisect which of the seven boundaries the blanket is papering over instead of re-running
	// blankets. The blanket WINS a contradiction rather than being merged with the mask: an arm whose
	// engagement has to be derived from a precedence rule is an arm nobody will believe, so the two
	// keys set together is an error that names itself and leaves the blanket standing.
	const u32 mask_key = gsTileGpuSerializeMask(GSConfig.TileGpuSerializeMask);
	const bool mask_refused = (serialize != 0 && mask_key != 0);
	const u32 serialize_sites = (serialize != 0) ? kGSTileGpuSerializeMaskAll : mask_key;
	if (serialize_sites != 0 && !m_tilegpu_serialize_announced)
	{
		m_tilegpu_serialize_announced = true;
		if (serialize != 0)
		{
			Console.WriteLn("TileGpu op serialization: grade %u ENGAGED (EmuCore/GS/TileGpuSerializeOps = %d) -- %s "
							"after every op recorded outside a pass and after every pass. Diagnostic only.",
				serialize, GSConfig.TileGpuSerializeOps,
				(serialize == 1) ? "full barrier" :
				(serialize == 2) ? "full barrier + submit" :
								   "full barrier + submit + fence wait");
		}
		else
		{
			Console.WriteLn("TileGpu op serialization: sites 0x%02X ENGAGED (EmuCore/GS/TileGpuSerializeMask = %d) -- "
							"a full barrier at expand=%c donor=%c clutcopy=%c materialise=%c byteroad=%c snapshot=%c "
							"passtail=%c. Diagnostic only.",
				serialize_sites, GSConfig.TileGpuSerializeMask,
				(serialize_sites & (1u << kGSTileGpuSerializeSiteExpand)) ? 'Y' : 'n',
				(serialize_sites & (1u << kGSTileGpuSerializeSiteDonor)) ? 'Y' : 'n',
				(serialize_sites & (1u << kGSTileGpuSerializeSiteClutCopy)) ? 'Y' : 'n',
				(serialize_sites & (1u << kGSTileGpuSerializeSiteMaterialise)) ? 'Y' : 'n',
				(serialize_sites & (1u << kGSTileGpuSerializeSiteByteRoad)) ? 'Y' : 'n',
				(serialize_sites & (1u << kGSTileGpuSerializeSiteSnapshot)) ? 'Y' : 'n',
				(serialize_sites & (1u << kGSTileGpuSerializeSitePassTail)) ? 'Y' : 'n');
		}
	}
	if (mask_refused && !m_tilegpu_serialize_mask_refused)
	{
		m_tilegpu_serialize_mask_refused = true;
		Console.Error("TileGpu: TileGpuSerializeOps (%d) and TileGpuSerializeMask (%d) are BOTH set; the mask is "
					  "REFUSED and the blanket grade stands at every site. Clear one of them -- a per-site round "
					  "run under the blanket measures the blanket.",
			GSConfig.TileGpuSerializeOps, GSConfig.TileGpuSerializeMask);
	}

	// The mid-frame kick, EmuCore/GS/TileGpuKickReadbackFrames. Announced once per run like the
	// levers above, and only when it is set: a lever whose whole effect is submission TIMING leaves
	// no other trace in a log, and "was it on, and at what cadence?" has to be answerable from one.
	if (GSConfig.TileGpuKickReadbackFrames && !m_tilegpu_kick_announced)
	{
		m_tilegpu_kick_announced = true;
		const u32 cadence = gsTileGpuKickPassCadence(GSConfig.TileGpuKickPassCadence);
		Console.WriteLn("TileGpu mid-frame kick ENGAGED (EmuCore/GS/TileGpuKickReadbackFrames) -- the plan's "
						"recorded work is submitted at a pass boundary rather than held until a pull drains it. "
						"Two triggers: near a readback, every %u render passes; and a pass cadence "
						"(EmuCore/GS/TileGpuKickPassCadence) of %u, which is %s. Ring is %u command buffers "
						"deep. Never blocks: it fires only when the next command buffer has already retired. "
						"Counts at teardown.",
			kGSTileGpuKickReadbackThreshold, cadence,
			cadence ? "additive -- it fires on any frame, readback or not"
					: "OFF, leaving the near-readback trigger standing alone: the arm this shipped as",
			static_cast<u32>(NUM_COMMAND_BUFFERS));
	}

	// One op boundary, at the strength the grade asks for, at the sites the mask names. The barrier is
	// deliberately the bluntest one Vulkan can express: this is a discriminator, not a fix, so it must
	// not be able to miss an edge. The cut from grade 2 takes the road the source-ring wrap already
	// takes -- submit, retain the plan's stream reservations, re-read the command buffer -- which is
	// sound here because MoveToNextCommandBuffer invalidates the framebuffer cache and every pass
	// re-establishes its own vertex/index binds, sets, push constants and dynamic state before it draws.
	const auto serialize_boundary = [&](GSTileGpuSerializeSite site) {
		if (!(serialize_sites & (1u << site)))
			return;
		pxAssertMsg(m_current_render_pass == VK_NULL_HANDLE,
			"TileGpu serialize boundary reached inside a render pass");
		VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
			0, nullptr, 0, nullptr);
		if (serialize < 2)
			return;
		// The counter this submission will carry, read before the submit moves it on.
		const u64 submitted = GetCurrentFenceCounter();
		ExecuteCommandBuffer(false);
		RetainTileGpuStreamsForCurrentCommandBuffer();
		if (serialize >= 3)
			WaitForFenceCounter(submitted, GpuWaitCause::Sync);
		cmd = GetCurrentCommandBuffer();
	};

	// The mid-frame kick (EmuCore/GS/TileGpuKickReadbackFrames), ported from the Classic renderer's
	// DoRenderHW. Called at the PASS TAIL below -- after the pass's closing EndRenderPass and after
	// its snapshot recycle -- for three reasons: no render pass is open there (a submit inside one
	// would force a tile flush, and ExecuteCommandBuffer(WaitType) does not end one for you); the
	// recycle has already stamped the scratch texture against the submission that actually read it;
	// and it is the one point in this loop that the grade-2 ordering lever ALREADY submits at, so
	// the state audit is the one written above serialize_boundary rather than a new one.
	//
	// It takes that same road verbatim -- submit, retain the plan's stream reservations, re-read the
	// command buffer -- and it must, because everything the loop carries across a pass either
	// survives a submit or is re-established by the next pass:
	//   - the frame's vertex/index/state/indirect/ring stream reservations, held by
	//     RetainTileGpuStreamsForCurrentCommandBuffer so the next frame cannot stage over data this
	//     one has recorded but not yet submitted;
	//   - the framebuffer, pipeline and descriptor caches, cleared by MoveToNextCommandBuffer's
	//     InvalidateCachedState, which is what makes the next pass's OMSetRenderTargets rebuild
	//     rather than early-out;
	//   - every per-pass bind -- viewport, scissor, vertex and index buffers at this frame's base,
	//     sets 0..3 in ascending order, push constants, pipeline -- re-issued inside the pass;
	//   - rule 3's frame-wide source set, which lives in a persistent ring rather than the frame
	//     pool and so survives a flush; only its reuse epoch has to name the LAST command buffer,
	//     which the re-stamp at the end of this function already does;
	//   - the frame-pool descriptor sets (expand, read-target, writeback, snapshot, dest), every one
	//     of which is allocated and consumed inside a single pass, so none spans this boundary;
	//   - the CLUT block-copy run, which never crosses a pass and is closed well before the tail.
	//
	// TWO triggers decide whether to offer, and gsTileGpuKickWantsSubmit is the whole of that
	// decision. Classic's near-a-readback gate is one of them, unchanged. The other is a pass
	// CADENCE (EmuCore/GS/TileGpuKickPassCadence, 32) that fires on any frame, and it is here
	// because the readback gate cannot serve the case that costs the most: TileGpu records a whole
	// frame and submits it once, so a title with no readbacks leaves the GPU idle for as long as
	// the GS thread records -- SD865 Stuntman, 35.99 ms of idle in a 90.50 ms drawn frame -- with
	// this kick, the machinery that would fill it, firing zero times because nothing reads back.
	//
	// Neither trigger is a submit budget. They set how often we OFFER, and the fence check below is
	// what decides, by a wide margin. ⚠️ The kick must never block: submitting cycles to the next
	// command buffer, and ActivateCommandBuffer fence-waits if that buffer's previous submission is
	// still executing, which is a hidden GPU sync worse than the backlog the kick drains. So it
	// fires only when the next buffer is verifiably complete; otherwise the counter keeps the gate
	// open and the next pass tail retries. Classic's comment at DoRenderHW carries the measurement
	// behind that rule, and NUM_COMMAND_BUFFERS carries why the ring is eight deep rather than
	// three: at three, that guard refuses most of the cadence's offers on a short frame.
	const u32 kick_cadence = gsTileGpuKickPassCadence(GSConfig.TileGpuKickPassCadence);
	const auto readback_kick = [&](GSTexture* pass_rt, GSTexture* pass_ds) {
		if (!GSConfig.TileGpuKickReadbackFrames)
			return;
		constexpr u32 readback_window_frames = 3;
		// A pass that wrote a recent readback source is (almost certainly) producing the data for
		// the next pull, which follows immediately -- kick regardless of the threshold so the
		// backlog is already draining when it asks. Both attachments, unlike Classic's colour-only
		// test: a TileGpu pull takes a depth owner as readily as a colour one.
		const auto recent = [this](GSTexture* t) {
			return t && (t == m_recent_readback_sources[0] || t == m_recent_readback_sources[1]);
		};
		const bool produces_readback_data = recent(pass_rt) || recent(pass_ds);
		const bool near_readback =
			m_tilegpu_readback_frame != ~0u && (m_frame - m_tilegpu_readback_frame) <= readback_window_frames;
		if (InRenderPass())
			return;
		// Two triggers, OR'd: the near-readback one this kick landed with, and the pass CADENCE,
		// which fires on any frame and is what serves a title that never reads back. See
		// gsTileGpuKickWantsSubmit -- the whole composition lives there so a unit test can hold it.
		if (!gsTileGpuKickWantsSubmit(
				m_render_passes_since_submit, kick_cadence, near_readback, produces_readback_data))
			return;

		m_tilegpu_kicks_offered++;
		ScanForCommandBufferCompletion();
		const u32 next_buffer = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		if (m_frame_resources[next_buffer].fence_counter > m_completed_fence_counter)
			return;

		m_tilegpu_kicks_taken++;
		ExecuteCommandBuffer(WaitType::None);
		RetainTileGpuStreamsForCurrentCommandBuffer();
		cmd = GetCurrentCommandBuffer();
	};

	// Poison classes B and C -- one hand-rolled clear, because GSTextureVK::CommitClear pxFailRel's
	// on a plain sampled texture and the snapshot scratch is one. Records nothing when the lever is
	// off, and must be called with no render pass open: a clear is a transfer, not an attachment op.
	//
	// The state stamp is not bookkeeping. GetLoadOpForTexture turns State::Invalidated into
	// LOAD_OP_DONT_CARE, and a DONT_CARE load discards this clear before the draw that was supposed
	// to replace it -- so a fresh image would be poisoned and then un-poisoned in the same op. Dirty
	// makes the build pass LOAD instead, which is what leaves the sentinel standing over exactly the
	// region no draw covered. That is the whole question the lever asks.
	const auto poison_clear = [&](GSTexture* tex, const VkClearColorValue& colour) {
		if (!poison || !tex)
			return;
		pxAssertMsg(m_current_render_pass == VK_NULL_HANDLE, "TileGpu poison clear inside a render pass");
		GSTextureVK* const vtex = static_cast<GSTextureVK*>(tex);
		vtex->StampGpuTouch(GetCurrentFenceCounter());
		vtex->TransitionToLayout(cmd, GSTextureVK::Layout::ClearDst);
		static constexpr VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
		vkCmdClearColorImage(cmd, vtex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &srr);
		vtex->SetState(GSTexture::State::Dirty);
	};

	// The plan's mid-flight exits. Five sites below cannot get a render pass object and return from the
	// executor, which skips every pass after them -- and the renderer discards this function's return
	// value, having already recorded the whole plan as done. So the frame is wrong from the exit point
	// on AND the next frame's byte model believes bytes moved that never did, with nothing said. Each
	// exit names itself here.
	const auto abandon = [this](const char* what) {
		m_tilegpu_abandoned_plans++;
		if (!m_tilegpu_abandoned_warned)
		{
			m_tilegpu_abandoned_warned = true;
			Console.Error("TileGpu: a plan was abandoned mid-frame at %s. Every pass after it is skipped and the "
						  "renderer still records the whole plan as done, so the byte model is now ahead of the "
						  "GPU. Totals at teardown.",
				what);
		}
		return false;
	};

	// Every source into shader-read layout, which is what the descriptors just written promise.
	if (source_set != VK_NULL_HANDLE)
	{
		for (const GSTileGpuPassPlan::SourceBind& sb : plan.sources)
		{
			if (sb.texture)
				static_cast<GSTextureVK*>(sb.texture)->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	// Rule 3's frame-wide source array, written ONCE for the whole plan and bound per pass. It is
	// frame-scoped because a materialised source belongs to a texture window, not to a target pair,
	// so the same image serves draws in any number of passes and a state row's tex_source is an index
	// into this one list. The set comes from a persistent ring rather than the frame pool: set 2 can
	// never be a push set (set 1 is the layout's one), and the frame pool only exists on the non-push
	// path. A mid-plan command-buffer flush loses the BINDING but not the set, and the binding is
	// re-issued per pass below -- so unlike a frame-pool set this one needs no rewrite after a flush;
	// it only needs its epoch re-stamped at the end, against the submission that actually read it.
	//
	// Every source is put in shader-read layout HERE, before any pass, and the descriptors are written
	// promising exactly that. It has to be here and not at each build: a source materialised at pass
	// 40 would otherwise still be in its allocation layout while passes 1..39 record, and a descriptor
	// must identify the layout its image is in for the whole time the set is bound -- the validation
	// layer cannot prove which array element a draw indexes, and neither can the driver. A build then
	// borrows its image back as a colour attachment for the length of its own pass head and returns it
	// (the transition after the materialise draw), so outside that window every slot is shader-read.
	// A first transition out of UNDEFINED discards contents, which is exactly right for an image whose
	// build is about to write every texel of it.

	// The ring is written by the writeback compute and read by the seed and geometry fragment
	// stages; every op boundary orders both directions so composed slots are complete before
	// anything samples them, and a slot is never overwritten under a pass still reading it.
	const auto ring_barrier = [&](VkPipelineStageFlags src_stage, VkAccessFlags src_access,
								  VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
		VkBufferMemoryBarrier bmb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		bmb.srcAccessMask = src_access;
		bmb.dstAccessMask = dst_access;
		bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bmb.buffer = m_tilegpu_vram_stream_buffer.GetBuffer();
		bmb.offset = 0;
		bmb.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
	};

	// ONE ring hazard pair per contiguous run of ClutBlockCopy ops, not one per copy. The ring
	// barrier is whole-buffer against every shader stage, so on a tiler it is a cache flush plus
	// invalidate plus a drain; measured on Adreno 650, a 1 KB CLUT copy inside the bracket costs
	// 14.3 us against 1.4 us outside it, and gt4opb records 1,251 of them a frame in runs of eight.
	//
	// The interior pairs carry no hazard. Consecutive copies write DISJOINT slots -- every record's
	// words are reserved by a bump allocation in CaptureClutRecordWords (stream_offset = the palette
	// vector's size, then a resize; an offset is never handed out twice in a plan, and a record
	// already in this plan's stream emits no second op), and within one op the regions tile that
	// one reservation exactly, region b at b * copy_w * copy_h words -- and nothing between them
	// reads or writes the ring, because every other op kind closes the run below before it
	// records anything.
	//
	// The same disjointness merges the run's COMMANDS: consecutive ops that pull from one source
	// image compose into a single vkCmdCopyImageToBuffer carrying all their regions. That is the
	// rest of the per-command fixed cost -- gt4opb's 1,251 copy commands become 283 -- and it is
	// sound for exactly the same reason the hoist is. Regions inside one copy command may be
	// executed in any order, which is why disjointness is the load-bearing clause and not a
	// convenience.
	static constexpr u32 kClutCopyBatchRegions = 64; // 16 ops' worth; the observed run is 8
	GSTextureVK* clut_copy_owner = nullptr;
	std::array<VkBufferImageCopy, kClutCopyBatchRegions> clut_copy_regions{};
	u32 clut_copy_region_count = 0;
	const auto flush_clut_copy_batch = [&]() {
		if (clut_copy_region_count == 0)
			return;
		// The owner is still in TransferSrc: only this arm transitions it, only ever to TransferSrc,
		// and a change of owner flushes the batch BEFORE the new owner's transition is recorded.
		vkCmdCopyImageToBuffer(cmd, clut_copy_owner->GetImage(), clut_copy_owner->GetVkLayout(),
			m_tilegpu_vram_stream_buffer.GetBuffer(), clut_copy_region_count, clut_copy_regions.data());
		clut_copy_region_count = 0;
		clut_copy_owner = nullptr;
	};
	bool clut_copy_run_open = false;
	const auto open_clut_copy_run = [&]() {
		if (clut_copy_run_open)
			return;
		clut_copy_run_open = true;
		// Ring reads already recorded must finish before the run's first copy overwrites any of it.
		ring_barrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
						 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT);
	};
	const auto close_clut_copy_run = [&]() {
		// Whatever the run staged is recorded before the barrier that publishes it, so the two can
		// never be reordered by a caller getting the sequence wrong.
		flush_clut_copy_batch();
		if (!clut_copy_run_open)
			return;
		clut_copy_run_open = false;
		// ...and the run's copies must be visible to every stage that reads the ring after it.
		ring_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	};

	for (const GSTileGpuPass& pass : plan.passes)
	{
		if (pass.target_pair_count == 0)
			continue;

		const GSTileGpuTargetPair& tp = plan.target_pairs[pass.first_target_pair];
		GSTexture* rt =
			(tp.frame_target != GSTileGpuPassPlan::kNoTarget) ? plan.targets[tp.frame_target] : nullptr;
		GSTexture* ds =
			(tp.zbuf_target != GSTileGpuPassPlan::kNoTarget) ? plan.targets[tp.zbuf_target] : nullptr;
		if (!rt && !ds)
			continue;

		// Reconciliation before the pass, in the renderer's order: writebacks compose ring slots
		// out of finished targets (compute), seeds fill targets out of ring slots (a fragment pass
		// each). Both are the byte model's traffic between the resident images and the byte store,
		// and both are gated on the byte road being live.
		//
		// The DONOR build is not: it reads an owner target's texture and writes a source image,
		// touching no ring slot and no page table, so a frame that composes nothing must still run
		// it or the draws it serves sample an image nobody wrote. That is why the gate below is a
		// union and every other kind re-checks `can_texture` inside the loop -- which leaves the
		// byte-road kinds behaving exactly as they did.
		const u32 op_end = std::min<u32>(pass.first_prep_op + pass.prep_op_count,
			static_cast<u32>(plan.prep_ops.size()));
		bool pass_has_donor = false;
		for (u32 o = pass.first_prep_op; o < op_end; o++)
		{
			const GSTileGpuPrepKind k = plan.prep_ops[o].kind;
			pass_has_donor |= (k == GSTileGpuPrepKind::Donor || k == GSTileGpuPrepKind::ClutGather);
		}

		if ((can_texture || pass_has_donor) && pass.prep_op_count > 0)
		{
			if (can_texture)
			{
				// One writeback and one seed program per colour swizzle universe, compiled on the
				// first op that names one -- a title with no 16-bit target never builds those two.
				// The depth seed has its own eight-format list, on the same terms.
				for (u32 o = pass.first_prep_op; o < op_end; o++)
				{
					const GSTileGpuPrepOp& op = plan.prep_ops[o];
					if (op.kind == GSTileGpuPrepKind::SeedDepth)
					{
						const u32 df = gsTileDepthRoadFormat(op.psm);
						if (df < kGSTileDepthRoadFormats && !m_tilegpu_seed_depth_tried[df])
							CompileTileGpuSeedDepthPipeline(df);
						continue;
					}
					if (op.kind != GSTileGpuPrepKind::Writeback && op.kind != GSTileGpuPrepKind::Seed)
						continue;
					const u32 rf = gsTileByteRoadFormat(op.psm);
					if (rf >= kGSTileByteRoadFormats)
						continue;
					if (op.kind == GSTileGpuPrepKind::Writeback && !m_tilegpu_writeback_tried[rf])
						CompileTileGpuWritebackPipeline(rf);
					if (op.kind == GSTileGpuPrepKind::Seed && !m_tilegpu_seed_tried[rf])
						CompileTileGpuSeedPipeline(rf);
				}
			}

			for (u32 o = pass.first_prep_op; o < op_end; o++)
			{
				const GSTileGpuPrepOp& op = plan.prep_ops[o];
				// Any other kind ends the copy run, ahead of the first command it records -- including
				// the ones that record nothing and the ones that abandon the plan, so no arm below can
				// reach the ring, or return, with the run's writes still unpublished.
				if (op.kind != GSTileGpuPrepKind::ClutBlockCopy)
					close_clut_copy_run();
				if (op.kind != GSTileGpuPrepKind::Donor && op.kind != GSTileGpuPrepKind::ClutGather && !can_texture)
					continue;

				// Indices + palette -> the colour image a paletted draw samples. It names three prep
				// textures and no target, and it MUST follow the materialise that filled its index --
				// which array order gives, because the renderer queues the pair in that order for the
				// same draw and this loop walks the range in order. Recorded raw, like the materialise
				// and the seed: see tilegpu_expand.glsl's header for why the device's own
				// TileExpandPalette may not be called from inside this recording.
				if (op.kind == GSTileGpuPrepKind::Expand)
				{
					if (op.target >= plan.prep_textures.size() || op.index_texture >= plan.prep_textures.size() ||
						op.palette_texture >= plan.prep_textures.size())
						continue;
					GSTexture* const edst = plan.prep_textures[op.target];
					GSTexture* const eidx = plan.prep_textures[op.index_texture];
					GSTexture* const epal = plan.prep_textures[op.palette_texture];
					if (!edst || !eidx || !epal)
						continue;
					if (!m_tilegpu_expand_tried)
						CompileTileGpuExpandPipeline();
					if (m_tilegpu_expand_pipeline == VK_NULL_HANDLE)
						continue;

					GSTextureVK* const dst = static_cast<GSTextureVK*>(edst);
					EndRenderPass();
					// The two reads first: a transition cannot happen inside a render pass, and the
					// index's is also the dependency that orders its materialise ahead of this.
					static_cast<GSTextureVK*>(eidx)->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
					static_cast<GSTextureVK*>(epal)->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);

					// Poison class B, at the head of the build that is about to fill this image. On a
					// build that RUNS the full-screen triangle below overwrites all of it; what this
					// catches is the build that does not, and this arm has such a road -- the
					// descriptor-pool guard further down leaves "a source nobody wrote" in its own
					// words. Today that source is the pool's previous tenant; with the lever on it is
					// cyan.
					poison_clear(dst, kTileGpuPoisonSource);

					// The whole image, one fragment per texel -- an expansion is exactly its index image.
					const GSVector2i esize = dst->GetSize();
					const GSVector4i earea = GSVector4i::loadh(esize);
					OMSetRenderTargets(dst, nullptr, earea);
					const VkAttachmentLoadOp eload = GetLoadOpForTexture(dst);
					const VkRenderPass erp = GetRenderPass(LookupNativeFormat(dst->GetFormat()), VK_FORMAT_UNDEFINED,
						eload, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						VK_ATTACHMENT_STORE_OP_DONT_CARE);
					if (erp == VK_NULL_HANDLE)
						return abandon("a palette-expand op's render pass");
					if (eload == VK_ATTACHMENT_LOAD_OP_CLEAR)
					{
						VkClearValue cv = {};
						cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
						BeginClearRenderPass(erp, earea, &cv, 1);
					}
					else
					{
						BeginRenderPass(erp, earea);
					}

					const VkViewport evp{
						0.0f, 0.0f, static_cast<float>(esize.x), static_cast<float>(esize.y), 0.0f, 1.0f};
					vkCmdSetViewport(cmd, 0, 1, &evp);
					const VkRect2D esc{{0, 0}, {static_cast<u32>(esize.x), static_cast<u32>(esize.y)}};
					vkCmdSetScissor(cmd, 0, 1, &esc);
					InvalidateCachedViewportScissor();
					{
						// Bound on THIS road's own layout, and the passes around it re-establish their
						// own sets before their draws -- which is the whole reason this is recorded here
						// rather than handed to the device's utility path.
						Vulkan::DescriptorSetUpdateBuilder dsub;
						if (m_use_push_descriptors)
						{
							dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, 0,
								static_cast<GSTextureVK*>(eidx)->GetView(), m_point_sampler,
								static_cast<GSTextureVK*>(eidx)->GetVkLayout());
							dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, 1,
								static_cast<GSTextureVK*>(epal)->GetView(), m_point_sampler,
								static_cast<GSTextureVK*>(epal)->GetVkLayout());
							dsub.PushUpdate(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_expand_pipeline_layout, 0,
								false);
						}
						else
						{
							VkDescriptorSet eset = AllocateDescriptorSetFromFramePool(m_tilegpu_expand_ds_layout);
							if (eset == VK_NULL_HANDLE) [[unlikely]]
							{
								// A few hundred a frame at most; exhaustion is implausible and the fallback is
								// a source nobody wrote, so leave it unbuilt rather than draw with no set.
								EndRenderPass();
								continue;
							}
							dsub.AddCombinedImageSamplerDescriptorWrite(eset, 0,
								static_cast<GSTextureVK*>(eidx)->GetView(), m_point_sampler,
								static_cast<GSTextureVK*>(eidx)->GetVkLayout());
							dsub.AddCombinedImageSamplerDescriptorWrite(eset, 1,
								static_cast<GSTextureVK*>(epal)->GetView(), m_point_sampler,
								static_cast<GSTextureVK*>(epal)->GetVkLayout());
							dsub.Update(m_device);
							vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
								m_tilegpu_expand_pipeline_layout, 0, 1, &eset, 0, nullptr);
						}
					}
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_expand_pipeline);
					vkCmdDraw(cmd, 3, 1, 0, 0);
					EndRenderPass();
					dst->SetState(GSTexture::State::Dirty);
					// Back to shader-read, which is what set 2's descriptors were written promising
					// (see the layout note above the set write); the transition is also the execution
					// dependency ordering this build ahead of every pass that samples it.
					dst->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
					serialize_boundary(kGSTileGpuSerializeSiteExpand);
					continue;
				}

				// The two roads that read a resident target through the GS swizzle: the DONOR build
				// (a texture source reinterpreted out of its owner) and the CLUT GATHER (a palette
				// gathered out of the target the game rendered it into). Same shape, same layout,
				// same hoist hazard -- only the pipeline and the destination geometry differ -- so
				// they run one arm. Two lists at once: the DESTINATION is a prep texture (neither is
				// anything's render target) and the OWNER is one of the frame's targets, which is
				// why the op carries both indices. Recorded raw for the same reason the expand is;
				// see tilegpu_reinterpret.glsl.
				if (op.kind == GSTileGpuPrepKind::Donor || op.kind == GSTileGpuPrepKind::ClutGather)
				{
					if (op.target >= plan.prep_textures.size() || !plan.prep_textures[op.target] ||
						op.donor_target >= plan.targets.size() || !plan.targets[op.donor_target])
						continue;
					GSTextureVK* const dst = static_cast<GSTextureVK*>(plan.prep_textures[op.target]);
					VkPipeline pipe = VK_NULL_HANDLE;
					if (op.kind == GSTileGpuPrepKind::Donor)
					{
						const int idx_fmt = GSTileSwizzleForms::IndexFormatFor(op.psm);
						if (idx_fmt < 0 || static_cast<u32>(idx_fmt) >= kTileGpuIdxFormats)
							continue;
						if (!m_tilegpu_donor_tried[idx_fmt])
							CompileTileGpuDonorPipeline(static_cast<u32>(idx_fmt));
						pipe = m_tilegpu_donor_pipeline[idx_fmt];
					}
					else
					{
						// The palette's entry count is the destination's width by construction, so
						// the op needs no field for it and the two cannot disagree.
						const u32 size_idx = (dst->GetWidth() >= 256) ? 0u : 1u;
						if (!m_tilegpu_clut_tried[size_idx])
							CompileTileGpuClutGatherPipeline(size_idx);
						pipe = m_tilegpu_clut_pipeline[size_idx];
					}
					if (pipe == VK_NULL_HANDLE)
						continue;

					GSTextureVK* const owner = static_cast<GSTextureVK*>(plan.targets[op.donor_target]);
					// A DONOR reads a texture window the draw is about to sample, so reading the pass's own
					// attachment would be the in-pass feedback road and the planner excludes it. A CLUT GATHER
					// is a different question and legitimately reads one: a palette the game rendered into its
					// own frame buffer is loaded BEFORE the draws that use it, and this op runs at the pass head
					// -- ahead of every draw in the pass and behind every draw before it, which IS the load's own
					// sequence point. What must not happen is a draw ALREADY IN this pass having overwritten
					// those pixels, and that is the renderer's DonorHoistCollides test, not this one.
					pxAssertMsg(op.kind != GSTileGpuPrepKind::Donor || (owner != rt && owner != ds),
						"TileGpu donor build reads this pass's own attachment -- the planner must have "
						"broken the pass");
					EndRenderPass();
					// The read first, and its transition is also the execution dependency that orders
					// whatever earlier pass rendered into the owner ahead of this build.
					owner->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);

					// Poison class B, for both kinds this arm serves. Same reasoning as the expand:
					// the draw covers the whole destination, so the cyan only ever survives where a
					// build did not happen -- and this arm's descriptor-pool guard is one such road.
					poison_clear(dst, kTileGpuPoisonSource);

					// The whole image, one fragment per texel: a source is exactly its window.
					const GSVector2i dsize = dst->GetSize();
					const GSVector4i darea = GSVector4i::loadh(dsize);
					OMSetRenderTargets(dst, nullptr, darea);
					const VkAttachmentLoadOp dload = GetLoadOpForTexture(dst);
					const VkRenderPass drp = GetRenderPass(LookupNativeFormat(dst->GetFormat()), VK_FORMAT_UNDEFINED,
						dload, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						VK_ATTACHMENT_STORE_OP_DONT_CARE);
					if (drp == VK_NULL_HANDLE)
						return abandon("a donor or CLUT-gather build's render pass");
					if (dload == VK_ATTACHMENT_LOAD_OP_CLEAR)
					{
						VkClearValue cv = {};
						cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
						BeginClearRenderPass(drp, darea, &cv, 1);
					}
					else
					{
						BeginRenderPass(drp, darea);
					}

					const VkViewport dvp{
						0.0f, 0.0f, static_cast<float>(dsize.x), static_cast<float>(dsize.y), 0.0f, 1.0f};
					vkCmdSetViewport(cmd, 0, 1, &dvp);
					const VkRect2D dsc{{0, 0}, {static_cast<u32>(dsize.x), static_cast<u32>(dsize.y)}};
					vkCmdSetScissor(cmd, 0, 1, &dsc);
					InvalidateCachedViewportScissor();
					{
						// This road's own layout, and the passes around it re-establish their own sets
						// before their draws -- the whole reason it is recorded here.
						Vulkan::DescriptorSetUpdateBuilder dsub;
						if (m_use_push_descriptors)
						{
							dsub.AddCombinedImageSamplerDescriptorWrite(
								VK_NULL_HANDLE, 0, owner->GetView(), m_point_sampler, owner->GetVkLayout());
							dsub.PushUpdate(
								cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_readtarget_pipeline_layout, 0, false);
						}
						else
						{
							VkDescriptorSet dset = AllocateDescriptorSetFromFramePool(m_tilegpu_readtarget_ds_layout);
							if (dset == VK_NULL_HANDLE) [[unlikely]]
							{
								// A few dozen a frame at most; exhaustion is implausible and the fallback
								// is a source nobody wrote, so leave it unbuilt rather than draw with no set.
								EndRenderPass();
								continue;
							}
							dsub.AddCombinedImageSamplerDescriptorWrite(
								dset, 0, owner->GetView(), m_point_sampler, owner->GetVkLayout());
							dsub.Update(m_device);
							vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
								m_tilegpu_readtarget_pipeline_layout, 0, 1, &dset, 0, nullptr);
						}
					}
					const u32 dpush[kTileGpuDonorPushWords] = {op.bp, op.bw, op.owner_bp, op.owner_bwpg};
					vkCmdPushConstants(cmd, m_tilegpu_readtarget_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
						sizeof(dpush), dpush);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
					vkCmdDraw(cmd, 3, 1, 0, 0);
					EndRenderPass();
					dst->SetState(GSTexture::State::Dirty);
					// Back to shader-read, which is what set 2's descriptors were written promising, and
					// the dependency that orders this build ahead of every pass that samples it.
					dst->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
					serialize_boundary(kGSTileGpuSerializeSiteDonor);
					continue;
				}

				// The CLUT gather's byte-road half: the palette's blocks copied verbatim out of the
				// owner target into the frame's palette stream, one image-to-buffer copy for the whole
				// palette. NOT a gather pass -- at six hundred to twelve hundred CLUT loads a frame a
				// render pass each would double the frame's pass count. It lands texels row-major
				// rather than in CSM1 entry order, which is why the fragment shader has a
				// palette-from-copied-block mode instead of the two sides agreeing on an order here.
				//
				// The copy is cheap; the SYNCHRONISATION around it was not. Adreno 650 / Turnip,
				// gt4opb frame 2: 1.4-1.5 us for a copy standing alone, 14.3 us for the same copy
				// inside a whole-buffer ring-barrier pair, against 5.1 us for a ClutGather pass. So
				// "a copy at a pass head costs none" was true of the copy and false of the op, and
				// the run bracketing above is what makes the road cost what it claims to.
				if (op.kind == GSTileGpuPrepKind::ClutBlockCopy)
				{
					if (op.donor_target >= plan.targets.size() || !plan.targets[op.donor_target] ||
						op.copy_count == 0 || op.copy_count > 4)
						continue;
					GSTextureVK* const owner = static_cast<GSTextureVK*>(plan.targets[op.donor_target]);
					const int ow = owner->GetWidth();
					const int oh = owner->GetHeight();

					VkBufferImageCopy regions[4] = {};
					u32 count = 0;
					for (u32 b = 0; b < op.copy_count; b++)
					{
						// The renderer proved the palette's pages resident and texel-filled, so every
						// rect is inside the image; the bound is a guard against a clause that stops
						// holding, not a semantic -- a partial copy would be a palette with holes, so
						// the whole op drops instead.
						if (static_cast<int>(op.copy_x[b] + op.copy_w) > ow ||
							static_cast<int>(op.copy_y[b] + op.copy_h) > oh)
						{
							count = 0;
							break;
						}
						VkBufferImageCopy& rc = regions[count++];
						// Where region b lands in the palette stream. The op says it outright when its
						// destination is a TILE the regions are placed in -- the 16-bit road's four
						// scattered blocks are the four quadrants of one 32x16 tile -- and otherwise the
						// regions simply follow one another, which is what every 32-bit copy produces and
						// what this arithmetic has always been. Same words either way for those.
						rc.bufferOffset =
							static_cast<VkDeviceSize>(pal_base_words + op.bp + op.CopyRegionOffset(b)) *
							sizeof(u32);
						rc.bufferRowLength = op.CopyRowLength();
						rc.bufferImageHeight = op.copy_h;
						rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
						rc.imageOffset = {static_cast<s32>(op.copy_x[b]), static_cast<s32>(op.copy_y[b]), 0};
						rc.imageExtent = {op.copy_w, op.copy_h, 1};
					}
					if (count == 0)
						continue;

					EndRenderPass();
					// A batch names one source image, and the flush must precede the next owner's
					// transition so the pending regions are recorded while their own owner still holds
					// the layout the flush names.
					if (clut_copy_owner != owner ||
						clut_copy_region_count + count > kClutCopyBatchRegions)
						flush_clut_copy_batch();
					// The transition is also the execution dependency that orders whatever rendered the
					// palette into the owner ahead of this copy. It is an IMAGE hazard and independent
					// of the ring, so a mid-run owner change needs no ring barrier of its own.
					owner->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
					// Opens the run on the first copy after a non-copy op; records nothing on the rest.
					// A dropped op (count == 0 above) leaves it closed -- it writes nothing.
					open_clut_copy_run();
					clut_copy_owner = owner;
					for (u32 b = 0; b < count; b++)
						clut_copy_regions[clut_copy_region_count++] = regions[b];
					// The lever's boundary has to fall AFTER this op's copy, not before it, so an
					// engaged site puts the road back at one copy command per op.
					if (serialize_sites & (1u << kGSTileGpuSerializeSiteClutCopy))
						flush_clut_copy_batch();
					serialize_boundary(kGSTileGpuSerializeSiteClutCopy);
					continue;
				}

				// A materialise names a source image, not one of the frame's targets, and carries no
				// page list -- its addresses come from the window's layout and the epoch page table.
				if (op.kind == GSTileGpuPrepKind::Materialise)
				{
					if (op.target >= plan.prep_textures.size() || !plan.prep_textures[op.target])
						continue;
					const u32 src_fmt = TileGpuSrcFormatForPsm(op.psm);
					if (src_fmt >= kTileGpuSrcFormats)
						continue;
					if (!m_tilegpu_materialise_tried[src_fmt])
						CompileTileGpuMaterialisePipeline(src_fmt);
					if (m_tilegpu_materialise_pipeline[src_fmt] == VK_NULL_HANDLE)
						continue;

					GSTextureVK* const dst = static_cast<GSTextureVK*>(plan.prep_textures[op.target]);
					EndRenderPass();

					// Poison class B. The materialise is the arm with no mid-build bail-out, so on this
					// road the cyan is a check on the OTHER side of the contract: an image the source
					// cache hands to a draw in a frame that queued no build for it at all.
					poison_clear(dst, kTileGpuPoisonSource);

					// The whole image, one fragment per texel: no scissor to trim to, because a
					// source is exactly its window.
					const GSVector2i size = dst->GetSize();
					const GSVector4i area = GSVector4i::loadh(size);
					OMSetRenderTargets(dst, nullptr, area);
					const VkAttachmentLoadOp op_load = GetLoadOpForTexture(dst);
					const VkRenderPass rp = GetRenderPass(LookupNativeFormat(dst->GetFormat()), VK_FORMAT_UNDEFINED,
						op_load, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						VK_ATTACHMENT_STORE_OP_DONT_CARE);
					if (rp == VK_NULL_HANDLE)
						return abandon("a source materialise's render pass");
					if (op_load == VK_ATTACHMENT_LOAD_OP_CLEAR)
					{
						VkClearValue cv = {};
						cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
						BeginClearRenderPass(rp, area, &cv, 1);
					}
					else
					{
						BeginRenderPass(rp, area);
					}

					const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};
					vkCmdSetViewport(cmd, 0, 1, &vp);
					const VkRect2D sc{{0, 0}, {static_cast<u32>(size.x), static_cast<u32>(size.y)}};
					vkCmdSetScissor(cmd, 0, 1, &sc);
					InvalidateCachedViewportScissor();
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 0, 1,
						&m_tilegpu_state_descriptor_set, 0, nullptr);
					const u32 mpush[kTileGpuPushWords] = {table_base_words, op.epoch, op.bp, op.bw, op.texa, 0, 0, 0};
					vkCmdPushConstants(cmd, m_tilegpu_pipeline_layout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mpush), mpush);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_materialise_pipeline[src_fmt]);
					vkCmdDraw(cmd, 3, 1, 0, 0);
					EndRenderPass();
					dst->SetState(GSTexture::State::Dirty);
					// Straight to shader-read, and it stays there for the rest of the image's life in
					// the cache. Set 2's descriptors were written promising this layout before any of
					// these ops ran (a source is a colour attachment only for the length of its own
					// build), and this transition is also the execution dependency that orders the
					// build ahead of every pass that samples it.
					dst->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
					serialize_boundary(kGSTileGpuSerializeSiteMaterialise);
					continue;
				}

				if (op.target >= plan.targets.size() || !plan.targets[op.target] || op.page_entry_count == 0)
					continue;
				GSTextureVK* const tex = static_cast<GSTextureVK*>(plan.targets[op.target]);

				// The swizzle universe decides the program AND the page geometry the road addresses the
				// surface with. TWO enumerations, because a Z buffer reaches formats the colour road does
				// not carry (PSMZ16/PSMZ16S, and both families -- GSState swaps ZBUF.PSM's 0x30 bit when
				// FRAME's PSM is a depth format): a depth seed asks gsTileDepthRoadFormat, the writeback
				// and the colour seed gsTileByteRoadFormat. Out of range means the renderer emitted an op
				// for a layout that road does not serve, which its own admission test forbids -- skip
				// rather than run some other format's program over these bytes.
				const bool is_depth_seed = op.kind == GSTileGpuPrepKind::SeedDepth;
				u32 road_fmt;
				if (is_depth_seed)
				{
					road_fmt = gsTileDepthRoadFormat(op.psm);
					if (road_fmt >= kGSTileDepthRoadFormats)
						continue;
				}
				else
				{
					road_fmt = gsTileByteRoadFormat(op.psm);
					if (road_fmt >= kGSTileByteRoadFormats)
						continue;
				}
				const u32 page_h = gsTilePageHeight(op.psm);

				if (op.kind == GSTileGpuPrepKind::Writeback)
				{
					if (m_tilegpu_writeback_pipeline[road_fmt] == VK_NULL_HANDLE)
						continue;
					EndRenderPass();
					// Prior shader reads and compute writes of the ring must finish before this
					// dispatch writes it (a slot may be re-composed after a pass consumed it, or two
					// owners of one page write disjoint bytes of the same words).
					ring_barrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
									 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
					// ComputeReadOnly, not ShaderReadOnly: the dispatch below samples this target from the
					// COMPUTE stage, and ShaderReadOnly's barriers name the FRAGMENT stage on both sides. Under
					// it the pass that filled this target is not ordered ahead of the read and its writes are
					// not made visible to it, and the read is not ordered ahead of whatever binds the target as
					// an attachment next -- so the slot this composes can hold the pixels from either side of
					// the boundary, decided by how the device happened to overlap two kinds of job. The VkImage
					// layout is the same SHADER_READ_ONLY_OPTIMAL, so no descriptor and no other reader moves.
					tex->TransitionToLayout(cmd, GSTextureVK::Layout::ComputeReadOnly);

					Vulkan::DescriptorSetUpdateBuilder dsub;
					if (m_use_push_descriptors)
					{
						dsub.AddCombinedImageSamplerDescriptorWrite(
							VK_NULL_HANDLE, 0, tex->GetView(), m_point_sampler, tex->GetVkLayout());
						dsub.AddBufferDescriptorWrite(VK_NULL_HANDLE, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
							m_tilegpu_vram_stream_buffer.GetBuffer(), 0, m_tilegpu_vram_stream_buffer.GetCurrentSize());
						dsub.AddBufferDescriptorWrite(VK_NULL_HANDLE, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
							m_tilegpu_vram_stream_buffer.GetBuffer(), 0, m_tilegpu_vram_stream_buffer.GetCurrentSize());
						dsub.PushUpdate(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tilegpu_writeback_pipeline_layout, 0, false);
					}
					else
					{
						VkDescriptorSet wbset = AllocateDescriptorSetFromFramePool(m_tilegpu_writeback_ds_layout);
						if (wbset == VK_NULL_HANDLE) [[unlikely]]
						{
							// Out of descriptor memory outright. The op is skipped and the ring slot keeps
							// whatever it was prefilled with, so a later draw samples the target's PREVIOUS
							// bytes -- a wrong pixel the renderer's model has already recorded as composed.
							// A device that does not push descriptors takes this road for every writeback of
							// every frame, so the total is how a run testifies it never fired.
							m_tilegpu_writeback_pool_dropped_ops++;
							if (!m_tilegpu_writeback_pool_warned)
							{
								m_tilegpu_writeback_pool_warned = true;
								Console.Error("TileGpu: the device would give no descriptor set for a writeback; the "
											  "op is skipped and its ring slot keeps the bytes it was prefilled "
											  "with. Totals at teardown.");
							}
							continue;
						}
						dsub.AddCombinedImageSamplerDescriptorWrite(
							wbset, 0, tex->GetView(), m_point_sampler, tex->GetVkLayout());
						dsub.AddBufferDescriptorWrite(wbset, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
							m_tilegpu_vram_stream_buffer.GetBuffer(), 0, m_tilegpu_vram_stream_buffer.GetCurrentSize());
						dsub.AddBufferDescriptorWrite(wbset, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
							m_tilegpu_vram_stream_buffer.GetBuffer(), 0, m_tilegpu_vram_stream_buffer.GetCurrentSize());
						dsub.Update(m_device);
						vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
							m_tilegpu_writeback_pipeline_layout, 0, 1, &wbset, 0, nullptr);
					}

					const u32 wpush[kTileGpuPushWords] = {table_base_words, op.epoch, op.bp, op.bw, op.byte_mask,
						entries_base_words, op.first_page_entry, op.page_entry_count, keep_base_words};
					vkCmdPushConstants(cmd, m_tilegpu_writeback_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
						sizeof(wpush), wpush);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tilegpu_writeback_pipeline[road_fmt]);
					// Groups of TileGpuWritebackGroupDim square covering one page's WORDS: 64x32
					// for a CT32 page, one texel per invocation; 32x64 for a 64x64 16-bit page,
					// whose invocations each pack the two texels that share a word. 2048
					// invocations either way whatever the dim, one page per z -- and it is the same
					// call the pipeline's local_size was baked from, so the two cannot disagree.
					{
						const GSTileDispatch2D groups = gsTileWritebackGroups(op.psm, TileGpuWritebackGroupDim());
						vkCmdDispatch(cmd, groups.x, groups.y, op.page_entry_count);
					}
					// The composed slots feed the seeds and passes that follow.
					ring_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
						VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
							VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
				}
				else // Seed / SeedDepth
				{
					// One pass shape, two destinations. The addressing, the page mask, the scissor and the
					// push constants are identical -- what differs is which attachment the fragments land on
					// and therefore which pipeline and render pass carry them.
					const VkPipeline seed_pipe = is_depth_seed ? m_tilegpu_seed_depth_pipeline[road_fmt] :
																 m_tilegpu_seed_pipeline[road_fmt];
					if (seed_pipe == VK_NULL_HANDLE)
						continue;
					pxAssertMsg(is_depth_seed == tex->IsDepthStencil(),
						"TileGpu seed kind disagrees with the target it names");
					if (is_depth_seed != tex->IsDepthStencil())
						continue;
					EndRenderPass();

					// Full-target render area (a first-bind clear must cover the whole attachment);
					// the scissor trims to the page rows/columns the op touches. Everything else
					// discards in the shader.
					const GSVector2i size = tex->GetSize();
					const u32 base_page = (op.bp >> 5) & (GS_MAX_PAGES - 1);
					int min_col = INT32_MAX, max_col = -1, min_row = INT32_MAX, max_row = -1;
					for (u32 k = 0; k < op.page_entry_count; k++)
					{
						const u32 rel = (plan.page_entries[op.first_page_entry + k].page + GS_MAX_PAGES - base_page) & (GS_MAX_PAGES - 1);
						const int col = static_cast<int>(rel % std::max<u32>(op.bw, 1));
						const int row = static_cast<int>(rel / std::max<u32>(op.bw, 1));
						min_col = std::min(min_col, col);
						max_col = std::max(max_col, col);
						min_row = std::min(min_row, row);
						max_row = std::max(max_row, row);
					}
					const GSVector4i sc_rect =
						GSVector4i(min_col * 64, min_row * static_cast<int>(page_h), (max_col + 1) * 64,
							(max_row + 1) * static_cast<int>(page_h))
							.rintersect(GSVector4i(0, 0, size.x, size.y));
					if (sc_rect.rempty())
						continue;

					const GSVector4i area = GSVector4i::loadh(size);
					OMSetRenderTargets(is_depth_seed ? nullptr : tex, is_depth_seed ? tex : nullptr, area);
					const VkAttachmentLoadOp op_load = GetLoadOpForTexture(tex);
					const VkFormat native_fmt = LookupNativeFormat(tex->GetFormat());
					const VkRenderPass rp = is_depth_seed ?
												GetRenderPass(VK_FORMAT_UNDEFINED, native_fmt, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
													VK_ATTACHMENT_STORE_OP_DONT_CARE, op_load, VK_ATTACHMENT_STORE_OP_STORE,
													VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE) :
												GetRenderPass(native_fmt, VK_FORMAT_UNDEFINED, op_load, VK_ATTACHMENT_STORE_OP_STORE,
													VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
													VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
					if (rp == VK_NULL_HANDLE)
						return abandon("a seed op's render pass");
					if (op_load == VK_ATTACHMENT_LOAD_OP_CLEAR)
					{
						// A pool texture on its first bind. The clear covers the WHOLE attachment, not the
						// scissor: the pages outside it would otherwise stay uninitialized and a later pass
						// that LOADs them reads garbage.
						VkClearValue cv = {};
						if (is_depth_seed)
							cv.depthStencil = {tex->GetClearDepth(), 0};
						else
							cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
						BeginClearRenderPass(rp, area, &cv, 1);
					}
					else
					{
						BeginRenderPass(rp, area);
					}
					// Counted at the pass, not off the op array: everything above this line can decline a
					// seed, and a declined op costs no pass. This is the only place a seed's render passes
					// can be counted at all -- the renderer's census counts PLAN passes, and a seed runs at
					// a pass head, inside no plan pass.
					m_tilegpu_seed_render_passes++;
					m_tilegpu_merge_seed_render_passes += (op.seed_from_merge != 0) ? 1 : 0;

					const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};
					vkCmdSetViewport(cmd, 0, 1, &vp);
					const VkRect2D sc{{sc_rect.x, sc_rect.y},
						{static_cast<u32>(sc_rect.width()), static_cast<u32>(sc_rect.height())}};
					vkCmdSetScissor(cmd, 0, 1, &sc);
					// The seed's page rectangle is narrower than the target, and the device's cache still
					// names the whole one from the OMSetRenderTargets above -- so this is the site where a
					// stale cache costs a later draw its scissor.
					InvalidateCachedViewportScissor();
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 0, 1,
						&m_tilegpu_state_descriptor_set, 0, nullptr);
					// Read only where the OP declares a table, and bounds-checked on top of that. Both
					// clauses are locally provable rather than provable at a distance: the table is
					// staged under `can_texture` and a seed op cannot reach this line with it false
					// (the kind filter at the head of the loop drops it), but a base that came from
					// anywhere but this op's own declaration would hand the shader a per-page table for
					// an op whose mask is uniform -- which is not a crash, it is a seed reading some
					// other plan's block masks.
					const u32 block_base = (op.seed_blocks_per_page != 0 && o < m_tilegpu_seed_block_bases.size()) ?
											   m_tilegpu_seed_block_bases[o] :
											   0u;
					const u32 spush[kTileGpuPushWords] = {table_base_words, op.epoch, op.bp, op.bw,
						masks_base_words + o * (GS_MAX_PAGES / 32), op.seed_blocks, block_base, 0};
					vkCmdPushConstants(cmd, m_tilegpu_pipeline_layout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(spush), spush);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, seed_pipe);
					vkCmdDraw(cmd, 3, 1, 0, 0);
					EndRenderPass();
					tex->SetState(GSTexture::State::Dirty);
				}

				// The writeback dispatch or the seed pass just above; the four kinds that `continue`
				// out of this loop carry their own boundary at their own tail.
				serialize_boundary(kGSTileGpuSerializeSiteByteRoad);
			}
		}

		// A run never crosses a pass: the pass about to be recorded samples the ring, so whatever
		// prep ended in copies publishes them here. Which also leaves the flag false at every other
		// road out of this loop body.
		close_clut_copy_run();

		// The pass snapshot: a copy of the colour target as it stands before the pass opens, for
		// the draws that read their own destination (DATE today). Taken here, outside any render
		// pass, into a scratch texture recycled after the pass; the queue orders it behind every
		// earlier pass on the target.
		GSTexture* snapshot = nullptr;
		if (rt && pass.snapshot_count > 0 && pass.first_snapshot < plan.snapshots.size() && !pass.declares_self_read)
		{
			const GSTileGpuSnapshotCopy& sc = plan.snapshots[pass.first_snapshot];
			const GSVector2i ssz = rt->GetSize();
			snapshot = CreateTexture(ssz.x, ssz.y, 1, GSTexture::Format::Color, true);
			if (snapshot)
			{
				EndRenderPass();
				// Poison class C, at the fetch and before the copy. The scratch comes out of the
				// texture pool carrying its previous tenant, and the copy covers only src_rect
				// intersected with the target -- so anything outside that rect is the tenant, sampled
				// by the pass as if it were the target's own pixels. Yellow says so out loud.
				poison_clear(snapshot, kTileGpuPoisonSnapshot);
				const GSVector4i copy_rect = sc.src_rect.rintersect(GSVector4i(0, 0, ssz.x, ssz.y));
				CopyRect(rt, snapshot, copy_rect, copy_rect.x, copy_rect.y);
				// And one on the way out. The scratch goes to shader-read for the pass that samples
				// it, and the target goes back to colour-attachment HERE rather than inside
				// OMSetRenderTargets below -- which then finds it already there and records nothing.
				// Nothing between the two points touches the target: the pass's rule-2 sampled
				// sources are asserted not to be its own attachment, and a pass that declares the
				// in-pass read takes no snapshot at all, so this is never the feedback-loop layout's
				// road.
				TransitionTwoImages(cmd, static_cast<GSTextureVK*>(snapshot), GSTextureVK::Layout::ShaderReadOnly,
					static_cast<GSTextureVK*>(rt), GSTextureVK::Layout::ColorAttachment);
				// A copy recorded outside a pass whose result the pass about to open samples: the
				// same producer->consumer shape the lever is interrogating.
				serialize_boundary(kGSTileGpuSerializeSiteSnapshot);
			}
		}

		// The pass's rule-2 sampled targets, moved to shader-read before anything binds an
		// attachment. Their descriptors carry the layout they are transitioned to here, and a
		// transition may not happen inside a render pass, so this is the last thing done before
		// the framebuffer is set up. The barrier the transition emits is also the execution
		// dependency that orders whatever earlier pass rendered into the target ahead of this
		// pass's reads.
		u32 tex_source_count = 0;
		std::array<GSTextureVK*, GSTileGpuPassPlan::kMaxTexSourcesPerPass> tex_sources{};
		if (pass.tex_source_count > 0 && pass.first_tex_source + pass.tex_source_count <= plan.tex_sources.size())
		{
			EndRenderPass();
			for (u32 s = 0; s < pass.tex_source_count && s < GSTileGpuPassPlan::kMaxTexSourcesPerPass; s++)
			{
				const u32 ti = plan.tex_sources[pass.first_tex_source + s];
				if (ti >= plan.targets.size() || !plan.targets[ti])
					continue;
				GSTextureVK* const src = static_cast<GSTextureVK*>(plan.targets[ti]);
				pxAssertMsg(src != rt && src != ds,
					"TileGpu pass samples its own attachment -- that is the feedback road, not a target bind");
				src->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
				tex_sources[tex_source_count++] = src;
			}
		}

		// The render area has to fit inside the smaller of the pair, not just the colour target:
		// this planner does pair a big colour with a small depth, and the framebuffer built for
		// that pair is clamped to match. The viewport is a separate question -- see
		// gsTileGpuPassGeometry.
		const GSTileGpuPassGeometry geom = gsTileGpuPassGeometry(rt != nullptr, rt ? rt->GetWidth() : 0,
			rt ? rt->GetHeight() : 0, ds != nullptr, ds ? ds->GetWidth() : 0, ds ? ds->GetHeight() : 0);
		const GSVector2i size(geom.area_width, geom.area_height);
		const GSVector4i area = GSVector4i::loadh(size);
		// A declaring pass wants its colour target in GENERAL with the feedback framebuffer, which is
		// exactly what the feedback-loop flag arranges -- including the Adreno quirks around a CLEAR or
		// DONT_CARE load op under a self-read, which are already answered there. The declared render
		// pass built below is compatible with that framebuffer's: same attachment formats, same single
		// input-attachment reference.
		const bool declares = pass.declares_self_read && rt != nullptr;
		bool declared_set_exhausted = false;
		// The same outcome for the per-pass set every drawing pass needs, on the road that allocates it
		// from a pool rather than pushing it. A flag of its own so neither road can silence the other.
		bool pass_set_exhausted = false;
		pxAssertMsg(pass.declares_self_read == (pass.self_mask != 0),
			"TileGpu pass declares the self-read without saying what for, or the other way round");
		OMSetRenderTargets(rt, ds, area, declares ? FeedbackLoopFlag_TileGpuSelfRead : FeedbackLoopFlag_None);

		// Targets persist across frames: a texture the pool has just allocated clears on its
		// first bind (State::Cleared), everything else loads and accumulates -- a seed pass above
		// has already marked a freshly seeded target Dirty, so its pages load.
		const VkAttachmentLoadOp rt_op =
			rt ? GetLoadOpForTexture(static_cast<GSTextureVK*>(rt)) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		const VkAttachmentLoadOp ds_op =
			ds ? GetLoadOpForTexture(static_cast<GSTextureVK*>(ds)) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		const VkFormat color_fmt = rt ? LookupNativeFormat(rt->GetFormat()) : VK_FORMAT_UNDEFINED;
		const VkFormat depth_fmt = ds ? LookupNativeFormat(ds->GetFormat()) : VK_FORMAT_UNDEFINED;
		const VkRenderPass rp = GetRenderPass(color_fmt, depth_fmt, rt_op, VK_ATTACHMENT_STORE_OP_STORE, ds_op,
			VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, false,
			false, declares);
		if (rp == VK_NULL_HANDLE)
			return abandon("a geometry pass's render pass");

		// The pass's RENDER AREA: what its draws touch, not the whole attachment pair. On a tiler a
		// pass costs a tile load and a tile store per pixel of it whether anything drew there or not,
		// and the pass-structure census measured what the whole attachment costs a pass-heavy title --
		// 134.8 Mpx a frame on Dirge of Cerberus, which alternates draw by draw between a 35x35 scratch
		// surface and its 640x448 framebuffer, against a few percent of it touched.
		//
		// Restricted to a pass every attachment of which LOADs, and that is the whole of what makes it
		// byte-inert. Outside the render area a LOAD/STORE attachment is neither loaded nor stored, so
		// its memory keeps what it held; a CLEAR would leave the region outside uninitialized and a
		// DONT_CARE undefined, and both of those are the attachment's FIRST bind -- a handful of passes
		// a frame, against the hundreds that load. So a pass that does either keeps the full area and
		// the backstop below stays exactly as true as it was.
		//
		// The union comes off the same gsTileGpuScissorRect the draw loop sets before each indirect
		// call, so every draw is contained in it by construction. Nothing else in the pass renders: the
		// prep ops, the snapshot copy and the seeds all closed their own passes before this one opened,
		// and the declared in-pass read is a subpassLoad at the fragment's own coordinate.
		const bool may_clamp =
			gsTileGpuMayClampPassArea(rt != nullptr, rt_op == VK_ATTACHMENT_LOAD_OP_LOAD, ds != nullptr,
				ds_op == VK_ATTACHMENT_LOAD_OP_LOAD) &&
			// A pass naming draws the plan does not carry is malformed: take the whole attachment
			// rather than an area built out of a range that is not there.
			(!can_draw || (static_cast<size_t>(pass.first_draw) + pass.draw_count) <= plan.scissors.size());
		GSVector4i pass_area = area;
		if (may_clamp)
		{
			const VkExtent2D gran = GetRenderAreaGranularity(rp);
			const GSTileGpuScissorRect pa = gsTileGpuPassArea(
				can_draw ? plan.scissors.subspan(pass.first_draw, pass.draw_count) : std::span<const GSVector4i>(),
				size.x, size.y, static_cast<int>(gran.width), static_cast<int>(gran.height));
			pass_area = GSVector4i(pa.x, pa.y, pa.x + pa.width, pa.y + pa.height);
		}

		if (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		{
			// Backstop: the clear only covers `area` = min(colour, depth). An attachment cleared on
			// its first bind whose own size exceeds `area` keeps the region outside `area`
			// uninitialized, and any later pass that LOADs it there reads garbage. The SotC dump
			// never hits this -- the oversized target of a mismatched pair gets its full-size clear
			// from an earlier full-extent pass, so the clamped pass LOADs it -- but a draw order that
			// first-binds the big target through a clamped pass would break silently, so guard it.
			if (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR && rt &&
				(rt->GetSize().x > size.x || rt->GetSize().y > size.y))
			{
				Console.Error("TileGpu: colour %dx%d first-cleared over only %dx%d -- outside stays uninitialized",
					rt->GetSize().x, rt->GetSize().y, size.x, size.y);
				pxAssertMsg(false, "TileGpu clear area smaller than first-bind colour attachment");
			}
			if (ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR && ds &&
				(ds->GetSize().x > size.x || ds->GetSize().y > size.y))
			{
				Console.Error("TileGpu: depth %dx%d first-cleared over only %dx%d -- outside stays uninitialized",
					ds->GetSize().x, ds->GetSize().y, size.x, size.y);
				pxAssertMsg(false, "TileGpu clear area smaller than first-bind depth attachment");
			}

			VkClearValue cv[2] = {};
			u32 n = 0;
			if (rt)
				cv[n++].color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // an empty framebuffer background
			if (ds)
				cv[n++].depthStencil = {0.0f, 0}; // farthest under GEQUAL
			BeginClearRenderPass(rp, pass_area, cv, n);
		}
		else
		{
			BeginRenderPass(rp, pass_area);
		}

		if (can_draw)
		{
			const VkViewport vp{0.0f, 0.0f, static_cast<float>(geom.viewport_width),
				static_cast<float>(geom.viewport_height), 0.0f, 1.0f};
			vkCmdSetViewport(cmd, 0, 1, &vp);
			// The render area, as the opening value only: every indirect call below replaces it with
			// that call's own rectangle, and no draw runs under this one. It is the render area rather
			// than the attachment so that "nothing renders outside the render area" holds for whatever
			// is issued here, not only for the calls that set their own.
			const VkRect2D sc{{pass_area.x, pass_area.y},
				{static_cast<u32>(pass_area.width()), static_cast<u32>(pass_area.height())}};
			vkCmdSetScissor(cmd, 0, 1, &sc);
			InvalidateCachedViewportScissor();

			// Bind the vertex/index streams at this frame's base, so the indirect commands'
			// frame-relative offsets are correct without any per-draw rebase; the state SSBO the VS
			// reads by first_instance; and this frame's base row into the state ring.
			const VkDeviceSize voff = static_cast<VkDeviceSize>(vbase) * sizeof(GSVertex);
			vkCmdBindVertexBuffers(cmd, 0, 1, m_vertex_stream_buffer.GetBufferPtr(), &voff);
			vkCmdBindIndexBuffer(cmd, m_index_stream_buffer.GetBuffer(),
				static_cast<VkDeviceSize>(ibase) * sizeof(u16), VK_INDEX_TYPE_UINT16);
			// The pass's sets are established in ASCENDING set order -- 0, then 1, then 2, then 3 -- and
			// that order is load-bearing. Binding or pushing at set N leaves the HIGHER-numbered sets
			// bound only if set N's PREVIOUS binding used a layout compatible for set N; otherwise they
			// are disturbed. Set 1 is this layout's push set, and what last touched set 1 before this pass
			// may be another layout's push at a different set number -- the utility blit pushes at set 0,
			// which disturbs set 1 -- so writing set 1 is precisely the bind whose predecessor is not this
			// layout's, and whatever was bound above it beforehand goes with it. Bound the other way round,
			// set 2 was the casualty in every pass that followed a blit into the same command buffer.
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 0, 1,
				&m_tilegpu_state_descriptor_set, 0, nullptr);
			{
				// Set 1, both bindings at once. Binding 0 is the snapshot (or the null texture -- the
				// shader only reads it under a per-draw flag, but the binding must be valid
				// regardless). Binding 1 is the pass's rule-2 sampled targets, EVERY slot written:
				// the shader statically uses the array, so leaving a slot unwritten would need
				// descriptorBindingPartiallyBound, which a push-descriptor set may not carry. Unused
				// slots repeat slot 0; a pass that samples nothing gets the null texture throughout.
				// Written by hand rather than through DescriptorSetUpdateBuilder, whose image-info
				// pool is smaller than one full array.
				GSTextureVK* const snap_tex = snapshot ? static_cast<GSTextureVK*>(snapshot) : m_null_texture.get();
				VkDescriptorImageInfo snap_info = {
					m_point_sampler, snap_tex->GetView(), snap_tex->GetVkLayout()};
				std::array<VkDescriptorImageInfo, GSTileGpuPassPlan::kMaxTexSourcesPerPass> src_infos{};
				for (u32 s = 0; s < GSTileGpuPassPlan::kMaxTexSourcesPerPass; s++)
				{
					GSTextureVK* const src =
						(tex_source_count > 0) ? tex_sources[std::min(s, tex_source_count - 1)] : m_null_texture.get();
					src_infos[s].sampler = m_point_sampler;
					src_infos[s].imageView = src->GetView();
					src_infos[s].imageLayout = src->GetVkLayout();
				}
				VkWriteDescriptorSet w[2] = {
					{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
				const u32 nw = 2;
				w[0].dstBinding = 0;
				w[0].descriptorCount = 1;
				w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w[0].pImageInfo = &snap_info;
				w[1].dstBinding = 1;
				w[1].descriptorCount = GSTileGpuPassPlan::kMaxTexSourcesPerPass;
				w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w[1].pImageInfo = src_infos.data();
				if (m_use_push_descriptors)
				{
					vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 1, nw, w);
				}
				else
				{
					const VkDescriptorSet sset = AllocateDescriptorSetFromFramePool(m_tilegpu_snapshot_ds_layout);
					if (sset != VK_NULL_HANDLE)
					{
						for (u32 i = 0; i < nw; i++)
							w[i].dstSet = sset;
						vkUpdateDescriptorSets(m_device, nw, w, 0, nullptr);
						vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 1, 1,
							&sset, 0, nullptr);
					}
					else
					{
						// No set means set 1 keeps whatever the LAST pass bound, and every draw below samples
						// that pass's snapshot and targets instead of this one's -- a wrong pixel, not a slow
						// one, and one nothing downstream corrects. So drop the pass's draws and say so, the
						// way the declared read below already does. The pool grows on demand inside the
						// allocator, so reaching here means the device refused more descriptor memory outright.
						//
						// A device that does not push descriptors takes this road for EVERY pass of every frame
						// (Mali is gated off push descriptors), which is why the outcome is counted and not only
						// warned: the totals at teardown are how a device run testifies that it never fired.
						pass_set_exhausted = true;
					}
				}

				// Set 2, the same frame-wide source array for every pass. Bound per pass because a
				// mid-plan flush drops every binding, and because it costs one command.
				if (source_set != VK_NULL_HANDLE)
				{
					vkCmdBindDescriptorSets(
						cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 2, 1, &source_set, 0, nullptr);
				}

				// Set 3: this pass's colour attachment as an input attachment, in the layout the render
				// pass keeps it in (GENERAL). Only a declaring pass binds it, and only a declaring pass's
				// fragment module names it.
				if (declares)
				{
					GSTextureVK* const vk_rt = static_cast<GSTextureVK*>(rt);
					const VkDescriptorImageInfo dest_info = {
						VK_NULL_HANDLE, vk_rt->GetView(), vk_rt->GetVkLayout()};
					VkWriteDescriptorSet dw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					dw.dstBinding = 0;
					dw.descriptorCount = 1;
					dw.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
					dw.pImageInfo = &dest_info;
					const VkDescriptorSet dset = AllocateDescriptorSetFromFramePool(m_tilegpu_dest_ds_layout);
					if (dset != VK_NULL_HANDLE)
					{
						dw.dstSet = dset;
						vkUpdateDescriptorSets(m_device, 1, &dw, 0, nullptr);
						vkCmdBindDescriptorSets(
							cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tilegpu_pipeline_layout, 3, 1, &dset, 0, nullptr);
					}
					else
					{
						// A pass whose fragment stage READS through this set cannot proceed without it: the
						// binding would be whatever an earlier pass left, which is a wrong pixel rather than
						// a slow one. Drop the pass's draws and say so. The pool grows on demand, so this
						// is no longer a capacity cliff -- reaching here means the device would give us no
						// more descriptor memory at all.
						declared_set_exhausted = true;
					}
				}
			}
			// base_row (VS), table_base + pal_base + epoch_count (FS). epoch_count is the number of
			// page tables actually staged above, which is what the fragment stage clamps a state
			// row's epoch against; zero when the byte road is not live this frame.
			const u32 push[kTileGpuPushWords] = {
				state_base_rows, table_base_words, pal_base_words, can_texture ? plan.epoch_count : 0u, 0, 0, 0, 0};
			vkCmdPushConstants(cmd, m_tilegpu_pipeline_layout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);

			// One vkCmdDrawIndexedIndirect per maximal run of draws sharing pipeline state -- the run
			// key: topology, blend key, depth mode, fragment variant -- and, within a run, per stretch
			// sharing a sampled-target slot. In draw order throughout: commands are laid out in draw
			// order, each run is a contiguous slice, and runs issue in order -- so overdraw is identical
			// to the per-draw path, at one submission per run instead of one per draw. A pipeline change
			// (and, for a FIX-factor blend, a blend-constant set) is the only per-run cost; a slot change
			// costs the call alone. This is the constant-cost submission the design bets on.
			//
			// The FRAGMENT VARIANT is in that key rather than on the pass because the fragment program
			// is not attachment state. Keyed per pass it was the union of everything the pass's draws
			// needed, so a merge widened the program every draw of the pass ran -- on the corpus, 99.6%
			// of Bloodrayne 2's source-road draws were executing a byte decoder they never entered, and
			// merging pushed untextured draws off the Adreno's 4-register wave128 program onto a
			// 12-register wave64 one. Keyed per run, each draw gets the smallest program that serves it.
			//
			// The sampled-binding key ends an indirect call too, but for a different reason: it is
			// not pipeline state, it is the pair of indices the fragment shader reads the per-pass
			// target array and the frame's source array at, and a descriptor array index has to be
			// dynamically uniform. Two keys in one indirect call would put fragments of two draws in
			// one wave under one descriptor. So the run keeps its pipeline and only the CALL is cut --
			// see GSTileGpuPassPlan::bind_keys, and tilegpu.glsl's tilegpu_target_texel and
			// tilegpu_source_sample, both undecorated on the strength of this.
			if (declared_set_exhausted)
			{
				// Counted, not just warned: the warning fires once and the totals go out at teardown,
				// so a run that loses draws says so in its text output rather than only in its frames.
				// A flag of its own, because the pipeline-build road below has one too and sharing it
				// let whichever failed first silence the other.
				m_tilegpu_declared_pool_dropped_passes++;
				m_tilegpu_declared_pool_dropped_draws += pass.draw_count;
				if (!m_tilegpu_declared_pool_warned)
				{
					m_tilegpu_declared_pool_warned = true;
					Console.Error("TileGpu: the device would give no descriptor set to a pass that declares the "
								  "in-pass destination read; its draws are dropped. Totals at teardown.");
				}
			}
			if (pass_set_exhausted)
			{
				m_tilegpu_pass_pool_dropped_passes++;
				m_tilegpu_pass_pool_dropped_draws += pass.draw_count;
				if (!m_tilegpu_pass_pool_warned)
				{
					m_tilegpu_pass_pool_warned = true;
					Console.Error("TileGpu: the device would give no descriptor set for a pass's snapshot and "
								  "sampled targets; its draws are dropped rather than sampling the previous pass's. "
								  "Totals at teardown.");
				}
			}
			const bool have_slots = plan.bind_keys.size() == plan.draws.size();
			// A plan that carries no variant stream leaves every run on its pass's union masks, which
			// is what this loop did before the variant joined the run key.
			const bool have_variants = plan.variant_keys.size() == plan.draws.size();
			const u32 end = pass.first_draw + pass.draw_count;
			u32 d = pass.first_draw;
			while (d < end)
			{
				const GSTileGpuPassPlan::GSTileGpuRunKey rkey = plan.RunKeyAt(d);
				const u32 bkey = rkey.blend_key;
				const u32 run_end = plan.RunEndAt(d, end);
				// This run's fragment variant: its own draws' roads, decode arms, destination-read uses
				// and 16-bit quantise -- never the pass's union, unless the plan carries no stream.
				const u32 run_road =
					have_variants ? GSTileGpuPassPlan::VariantRoadMask(rkey.variant) : pass.road_mask;
				const u32 run_texel =
					have_variants ? GSTileGpuPassPlan::VariantTexelMask(rkey.variant) : pass.texel_mask;
				const u32 run_self =
					have_variants ? GSTileGpuPassPlan::VariantSelfMask(rkey.variant) : pass.self_mask;
				const bool run_quantise =
					have_variants ? GSTileGpuPassPlan::VariantQuantises(rkey.variant) : pass.quantises_frame;
				// ...and the frozen per-draw GS state (alpha test, fog, the texture function, the wrap
				// modes, the filter, the coordinate kind, DATE, TEXA). Unlike the four masks above it has
				// no pass union to be a subset of, and deliberately so: those four are ORed over the pass
				// at grouping because the pass's DESCRIPTORS depend on them, while nothing about a pass
				// depends on which alpha comparison its draws run. It is run-only, which is why the
				// subset assert below does not mention it.
				const GSDevice::GSTileGpuFragmentSpec run_spec =
					have_variants ? GSTileGpuPassPlan::VariantSpec(rkey.variant) :
									GSDevice::GSTileGpuFragmentSpec();
				pxAssertMsg((run_road & ~pass.road_mask) == 0 && (run_texel & ~pass.texel_mask) == 0 &&
								(run_self & ~pass.self_mask) == 0 && (!run_quantise || pass.quantises_frame),
					"TileGpu run asks for a fragment variant its pass's union does not cover");

				// The depth ATTACHMENT is uniform across the pass whatever the device's grouping policy --
				// a render pass cannot gain or lose one -- so a run's None-ness and this pass's depth
				// binding are the same fact. The three depth-carrying variants are per run.
				pxAssertMsg((rkey.depth_mode == GSTileGpuDepthMode::None) == (ds == nullptr),
					"TileGpu draw's depth mode disagrees with its pass's depth attachment");
				const VkPipeline run_pipe = (declared_set_exhausted || pass_set_exhausted) ?
												VK_NULL_HANDLE :
												GetTileGpuPipeline(static_cast<u32>(rkey.topology),
													static_cast<u32>(rkey.depth_mode), bkey, run_road, run_texel, run_self,
													run_quantise, declares, run_spec);
				if (run_pipe == VK_NULL_HANDLE)
				{
					// Either a pass whose descriptor sets the pool would not serve -- the two exhausted
					// flags above, which counted their own draws at the pass -- or a declaring pass whose
					// pipeline failed to build, where the eager fallback would be an incompatible render
					// pass rather than a wrong blend. Only the second is uncounted here, and it drops a
					// RUN rather than a pass, so it is the run's draws that go.
					if (!declared_set_exhausted && !pass_set_exhausted)
						m_tilegpu_declared_build_dropped_draws += run_end - d;
					d = run_end;
					continue;
				}
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, run_pipe);
				if ((bkey & GSTileGpuPassPlan::kBlendEnable) && !(bkey & GSTileGpuPassPlan::kSelfBlend))
				{
					// FIX rides as the blend constant in the GS's 0x80 = 1.0 convention.
					const float fix = std::min(static_cast<float>((bkey >> 8) & 0xFFu) * (1.0f / 128.0f), 1.0f);
					// ...and its ALPHA component is the carrier's undo factor, which the restore road
					// asks for as CONSTANT_ALPHA. Nothing else reads it: the alpha equation is
					// ONE/ZERO on every other road, and a colour factor that reads the constant
					// belongs to a C = FIX row, which never names As.
					const bool restore = (bkey & GSTileGpuPassPlan::kDualSrcRoadMask) ==
										 GSTileGpuPassPlan::kDualSrcCarrierRestore;
					const float consts[4] = {fix, fix, fix, restore ? GSDevice::kGSTileGpuAlphaRestore : fix};
					vkCmdSetBlendConstants(cmd, consts);
				}

				for (u32 first = d; first < run_end;)
				{
					u32 count = std::min(run_end - first, max_indirect);
					if (have_slots)
					{
						const u32 slot = plan.bind_keys[first];
						u32 n = 1;
						while (n < count && plan.bind_keys[first + n] == slot)
							n++;
						count = n;
					}
					// The GS scissor ends an indirect call the way the sampled-binding key does --
					// same shape, different reason. A slot has to be dynamically uniform; a scissor
					// is not shader state at all, it is one command, and one command cannot say two
					// rectangles. The cut is the whole cost: same pipeline, same fragments, one more
					// vkCmdDrawIndexedIndirect and one vkCmdSetScissor wherever the rectangle
					// changes. Measured at about 0.054 ms a frame on the Adreno 650.
					{
						const GSVector4i& want = plan.scissors[first];
						u32 n = 1;
						while (n < count && plan.scissors[first + n].eq(want))
							n++;
						count = n;
						const GSTileGpuScissorRect r =
							gsTileGpuScissorRect(want.x, want.y, want.z, want.w, size.x, size.y);
						// Rendering outside the render area is undefined, and the area was built out of
						// exactly these rectangles -- so this can only fire if the two walks stopped
						// seeing the same draws, which is invisible in a frame until a driver acts on it.
						pxAssertMsg(r.width <= 0 || r.height <= 0 ||
										(r.x >= pass_area.x && r.y >= pass_area.y && (r.x + r.width) <= pass_area.z &&
											(r.y + r.height) <= pass_area.w),
							"TileGpu draw's scissor reaches outside its pass's render area");
						const VkRect2D dsc{{r.x, r.y}, {static_cast<u32>(r.width), static_cast<u32>(r.height)}};
						vkCmdSetScissor(cmd, 0, 1, &dsc);
						InvalidateCachedViewportScissor();
					}
					const VkDeviceSize offset = static_cast<VkDeviceSize>(indirect_base_bytes) +
												static_cast<VkDeviceSize>(first) * sizeof(GSTileGpuIndirectDraw);
					vkCmdDrawIndexedIndirect(cmd, m_tilegpu_indirect_stream_buffer.GetBuffer(), offset, count,
						sizeof(GSTileGpuIndirectDraw));
					// One submission per indirect call, not per draw: the DrawCalls metric now reads
					// the collapsed count (≈ pipeline-state runs per frame), which is the A/B against the
					// per-draw baseline the design is measured on.
					g_perfmon.Put(GSPerfMon::DrawCalls, 1);
					first += count;
				}
				d = run_end;
			}
		}

		EndRenderPass();

		if (rt)
			rt->SetState(GSTexture::State::Dirty);
		if (ds)
			ds->SetState(GSTexture::State::Dirty);
		if (snapshot)
			Recycle(snapshot);

		// Between consecutive passes -- after the recycle, so the scratch texture's reuse epoch is
		// the submission that actually read it and not the one after.
		serialize_boundary(kGSTileGpuSerializeSitePassTail);

		// ...and the readback kick, at the same boundary and for the same reason, after it: at a
		// serialize grade that already submitted, m_render_passes_since_submit is zero and the gate
		// below simply does not open. The diagnostic lever wins where the two overlap.
		readback_kick(rt, ds);
	}

	// These passes recorded raw, bypassing the device's state cache; force the present path and
	// the next frame to re-apply everything. One binding has no dirty flag to force it: the device
	// binds its vertex stream buffer at offset 0 once per command buffer (SetInitialState) and
	// every later draw relies on that, while the passes above bound the same buffer at this
	// frame's offset -- restore it, or the merge and present that follow in this command buffer
	// read their quads from the wrong place (the presented frame goes dark until a submit happens
	// to intervene).
	if (can_draw)
	{
		constexpr VkDeviceSize zero_offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, m_vertex_stream_buffer.GetBufferPtr(), &zero_offset);
	}

	// The source set's epoch, re-stamped against the command buffer that ends up carrying the LAST
	// pass rather than the one that was current when it was written. Those differ whenever a pass
	// forced a mid-plan flush, and taking the earlier of the two would let the ring recycle the set
	// while a later submission still reads it.
	if (source_set_index < kTileGpuSourceSetsMax)
		m_tilegpu_source_set_epoch[source_set_index] = GetCurrentFenceCounter();

	InvalidateCachedState();
	return true;
}

bool GSDeviceVK::TileClutFromTarget(GSTexture* owner, GSTexture* dst, const TileClutGatherParams& p)
{
	if (!m_tile_reinterpret_tried && !CompileTileReinterpretPipelines())
	{
		for (VkPipeline& pipe : m_tile_reinterpret)
		{
			if (pipe != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, pipe, nullptr);
			pipe = VK_NULL_HANDLE;
		}
	}
	const u32 which = (p.entries == 16) ? 5 : (p.entries == 256) ? 6 : 0xFFFFFFFFu;
	if (which == 0xFFFFFFFFu || m_tile_reinterpret[which] == VK_NULL_HANDLE)
		return false;

	struct alignas(16) Uniforms
	{
		u32 src_bp;
		u32 src_bwpg;
		u32 dst_bp;
		u32 dst_bwpg;
	};
	const Uniforms uniforms = {p.cbp, 0, p.dst_bp, p.dst_bwpg};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, static_cast<int>(p.entries), 1);
	DoStretchRect(static_cast<GSTextureVK*>(owner), GSVector4::zero(), static_cast<GSTextureVK*>(dst), dRect,
		m_tile_reinterpret[which], Nearest, true);
	return true;
}

bool GSDeviceVK::TileReinterpretIndex(GSTexture* owner, GSTexture* dst, const TileReinterpretParams& p)
{
	if (!m_tile_reinterpret_tried && !CompileTileReinterpretPipelines())
	{
		// One failed compile is permanent for the session; anything half-built stays
		// null and the route below refuses.
		for (VkPipeline& pipe : m_tile_reinterpret)
		{
			if (pipe != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, pipe, nullptr);
			pipe = VK_NULL_HANDLE;
		}
	}
	if (p.fmt >= 5 || m_tile_reinterpret[p.fmt] == VK_NULL_HANDLE)
		return false;

	struct alignas(16) Uniforms
	{
		u32 src_bp;
		u32 src_bwpg;
		u32 dst_bp;
		u32 dst_bwpg;
	};
	const Uniforms uniforms = {p.src_bp, p.src_bwpg, p.dst_bp, p.dst_bwpg};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dst->GetWidth(), dst->GetHeight());
	DoStretchRect(static_cast<GSTextureVK*>(owner), GSVector4::zero(), static_cast<GSTextureVK*>(dst), dRect,
		m_tile_reinterpret[p.fmt], Nearest, true);
	return true;
}

// The Tile renderer's palette expansion (ps_tile_expand_palette): two source
// textures where the utility layout binds one, so the pass carries its own
// descriptor and pipeline layouts. The push-constant range is the utility one's
// exactly — ranges identical means push constants recorded through either layout
// satisfy both, so SetUtilityPushConstants and its command-buffer-restart replay
// serve this pipeline unchanged.
bool GSDeviceVK::CompileTileExpandPipeline()
{
	m_tile_expand_tried = true;

	{
		Vulkan::DescriptorSetLayoutBuilder dslb;
		if (m_use_push_descriptors)
			dslb.SetPushFlag();
		dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		if ((m_tile_expand_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
			return false;
		Vulkan::SetObjectName(m_device, m_tile_expand_ds_layout, "Tile expand descriptor layout");

		Vulkan::PipelineLayoutBuilder plb;
		plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
		plb.AddDescriptorSet(m_tile_expand_ds_layout);
		if ((m_tile_expand_pipeline_layout = plb.Create(m_device)) == VK_NULL_HANDLE)
			return false;
		Vulkan::SetObjectName(m_device, m_tile_expand_pipeline_layout, "Tile expand pipeline layout");
	}

	const std::optional<std::string> vsource = ReadShaderSource("shaders/vulkan/convert.glsl");
	const std::optional<std::string> fsource = ReadShaderSource("shaders/vulkan/tile_convert.glsl");
	if (!vsource || !fsource)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tile_convert.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*vsource);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	// No swizzle-form defines: the entry-point macro alone selects the expand pass,
	// which compiles without the forms (see tile_convert.glsl) — expansion has no
	// address arithmetic and must not be hostage to the forms fitting.
	VkShaderModule ps = GetUtilityFragmentShader(*fsource, "ps_tile_expand_palette");
	if (ps == VK_NULL_HANDLE)
		return false;
	ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_tile_expand_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(ps);
	gpb.SetRenderPass(GetRenderPass(LookupNativeFormat(GSTexture::Format::Color),
						  LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_DONT_CARE),
		0);
	gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetColorWriteMask(0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

	m_tile_expand_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (m_tile_expand_pipeline == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(m_device, m_tile_expand_pipeline, "Tile palette-expand pipeline");
	return true;
}

// Descriptor binding for the expansion draw. Rebound on every call: the pass runs
// a handful of times per frame with different sources each time, so there is
// nothing to cache — and m_tile_expand_textures is only ever dereferenced here,
// inside TileExpandPalette's call window, which is why InvalidateCachedState
// leaves it alone (nulling it there would make the descriptor-exhaustion retry
// below bind the null texture, not the caller's).
bool GSDeviceVK::ApplyTileExpandState(bool already_execed)
{
	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	const u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_BASE_STATE;

	m_current_pipeline_layout = PipelineLayout::TileExpand;

	Vulkan::DescriptorSetUpdateBuilder dsub;
	if (m_use_push_descriptors)
	{
		dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, 0, m_tile_expand_textures[0]->GetView(),
			m_point_sampler, m_tile_expand_textures[0]->GetVkLayout());
		dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, 1, m_tile_expand_textures[1]->GetView(),
			m_point_sampler, m_tile_expand_textures[1]->GetVkLayout());
		dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tile_expand_pipeline_layout, 0, false);
	}
	else
	{
		VkDescriptorSet ds = AllocateDescriptorSetFromFramePool(m_tile_expand_ds_layout);
		if (ds == VK_NULL_HANDLE) [[unlikely]]
		{
			if (already_execed)
			{
				Console.Error("VK: Failed to allocate tile-expand descriptor set");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Out of tile-expand descriptors");
			RetainTileGpuStreamsForCurrentCommandBuffer();
			return ApplyTileExpandState(true);
		}
		dsub.AddCombinedImageSamplerDescriptorWrite(ds, 0, m_tile_expand_textures[0]->GetView(), m_point_sampler,
			m_tile_expand_textures[0]->GetVkLayout());
		dsub.AddCombinedImageSamplerDescriptorWrite(ds, 1, m_tile_expand_textures[1]->GetView(), m_point_sampler,
			m_tile_expand_textures[1]->GetVkLayout());
		dsub.Update(m_device);
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tile_expand_pipeline_layout, 0, 1, &ds, 0, nullptr);
	}

	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::TileExpandPalette(GSTexture* index, GSTexture* palette, GSTexture* dst, u32 src_level, u32 dst_level)
{
	// One failed compile is permanent for the session: the tried flag stays set, the
	// pipeline stays null, and every later call refuses here (half-created layouts
	// are destroyed with the device).
	if (!m_tile_expand_tried)
		CompileTileExpandPipeline();
	if (m_tile_expand_pipeline == VK_NULL_HANDLE)
		return false;

	GSTextureVK* idxVK = static_cast<GSTextureVK*>(index);
	GSTextureVK* palVK = static_cast<GSTextureVK*>(palette);

	// A render-target dst at level 0 takes the expansion draw directly; a plain
	// mipmapped texture (or any level above 0) is not renderable, so the draw goes
	// to a scratch target and a 1:1 blit carries it into the level.
	const bool direct = dst->IsRenderTarget() && dst_level == 0;
	const int lw = std::max(dst->GetWidth() >> dst_level, 1);
	const int lh = std::max(dst->GetHeight() >> dst_level, 1);

	GSTexture* scratch = nullptr;
	GSTextureVK* rtVK = static_cast<GSTextureVK*>(dst);
	if (!direct)
	{
		scratch = CreateRenderTarget(lw, lh, GSTexture::Format::Color, false, true);
		if (!scratch)
			return false;
		rtVK = static_cast<GSTextureVK*>(scratch);
	}

	if (idxVK->GetLayout() != GSTextureVK::Layout::ShaderReadOnly ||
		palVK->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
	{
		// can't transition in a render pass
		EndRenderPass();
		idxVK->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		palVK->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}

	SetPipeline(m_tile_expand_pipeline);
	m_tile_expand_textures[0] = idxVK;
	m_tile_expand_textures[1] = palVK;

	struct alignas(16) Uniforms
	{
		u32 src_level;
	};
	const Uniforms uniforms = {src_level};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4i dst_rc(0, 0, lw, lh);
	OMSetRenderTargets(rtVK, nullptr, dst_rc);
	if (InRenderPass() && rtVK->GetState() == GSTexture::State::Cleared)
		EndRenderPass();
	if (!InRenderPass())
		BeginRenderPassForStretchRect(rtVK, dst_rc, dst_rc, true);

	// DrawStretchRect's full-rect quad (v_tex is unread — the pass fetches by
	// fragment coordinate), drawn through the tile-expand state apply instead of
	// the utility one.
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	const GSVertexPT1 vertices[] = {
		{GSVector4(-1.0f, 1.0f, 0.5f, 1.0f), GSVector2(0.0f, 0.0f)},
		{GSVector4(1.0f, 1.0f, 0.5f, 1.0f), GSVector2(1.0f, 0.0f)},
		{GSVector4(-1.0f, -1.0f, 0.5f, 1.0f), GSVector2(0.0f, 1.0f)},
		{GSVector4(1.0f, -1.0f, 0.5f, 1.0f), GSVector2(1.0f, 1.0f)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	const bool drawn = ApplyTileExpandState();
	if (drawn)
		DrawPrimitive();

	if (!direct)
	{
		// Never blit an unwritten scratch over the caller's texture.
		if (drawn)
			BlitRect(scratch, dst_rc, 0, dst, dst_rc, dst_level, Nearest);
		Recycle(scratch);
	}
	return drawn;
}

bool GSDeviceVK::CompilePresentPipelines()
{
	// we may not have a swap chain if running in headless mode.
	m_swap_chain_render_pass =
		GetRenderPass(m_swap_chain ? m_swap_chain->GetTextureFormat() : VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED);
	if (m_swap_chain_render_pass == VK_NULL_HANDLE)
		return false;

	const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/present.glsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/present.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);
	gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetRenderPass(m_swap_chain_render_pass, 0);

	for (PresentShader i = PresentShader::COPY; i < PresentShader::Count; i = static_cast<PresentShader>(static_cast<int>(i) + 1))
	{
		const int index = static_cast<int>(i);

		VkShaderModule ps = GetUtilityFragmentShader(*shader, ShaderEntryPoint(i));
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetFragmentShader(ps);

		m_present[index] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_present[index])
			return false;

		Vulkan::SetObjectName(m_device, m_present[index], "Present pipeline %d", i);
	}

	return true;
}


bool GSDeviceVK::CompileInterlacePipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/interlace.glsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/interlace.glsl.");
		return false;
	}

	VkRenderPass rp =
		GetRenderPass(LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_interlace.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);

		m_interlace[i] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		vkDestroyShaderModule(m_device, ps, nullptr);
		if (!m_interlace[i])
			return false;

		Vulkan::SetObjectName(m_device, m_interlace[i], "Interlace pipeline %d", i);
	}

	return true;
}

bool GSDeviceVK::CompileMergePipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/merge.glsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/merge.glsl.");
		return false;
	}

	VkRenderPass rp =
		GetRenderPass(LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_merge.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);

		m_merge[i] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		vkDestroyShaderModule(m_device, ps, nullptr);
		if (!m_merge[i])
			return false;

		Vulkan::SetObjectName(m_device, m_merge[i], "Merge pipeline %d", i);
	}

	return true;
}

bool GSDeviceVK::CompilePostProcessingPipelines()
{
	VkRenderPass rp =
		GetRenderPass(LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);

	{
		const std::optional<std::string> vshader = ReadShaderSource("shaders/vulkan/convert.glsl");
		if (!vshader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/convert.glsl.");
			return false;
		}

		const std::optional<std::string> pshader = ReadShaderSource("shaders/common/fxaa.fx");
		if (!pshader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/common/fxaa.fx.");
			return false;
		}

		const std::string psource = "#define FXAA_GLSL_VK 1\n" + *pshader;

		VkShaderModule vs = GetUtilityVertexShader(*vshader);
		if (vs == VK_NULL_HANDLE)
			return false;
		ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
		VkShaderModule ps = GetUtilityFragmentShader(psource, "ps_main");
		if (ps == VK_NULL_HANDLE)
			return false;
		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetVertexShader(vs);
		gpb.SetFragmentShader(ps);

		m_fxaa_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_fxaa_pipeline)
			return false;

		Vulkan::SetObjectName(m_device, m_fxaa_pipeline, "FXAA pipeline");
	}

	{
		const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/shadeboost.glsl");
		if (!shader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/shadeboost.glsl.");
			return false;
		}

		VkShaderModule vs = GetUtilityVertexShader(*shader);
		ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
		if (vs == VK_NULL_HANDLE)
			return false;

		VkShaderModule ps = GetUtilityFragmentShader(*shader);
		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetVertexShader(vs);
		gpb.SetFragmentShader(ps);

		m_shadeboost_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_shadeboost_pipeline)
			return false;

		Vulkan::SetObjectName(m_device, m_shadeboost_pipeline, "Shadeboost pipeline");
	}

	return true;
}

bool GSDeviceVK::CompileCASPipelines()
{
	VkDevice dev = m_device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	if (m_use_push_descriptors)
		dslb.SetPushFlag();
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	if ((m_cas_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_cas_ds_layout, "CAS descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, NUM_CAS_CONSTANTS * sizeof(u32));
	plb.AddDescriptorSet(m_cas_ds_layout);
	if ((m_cas_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_cas_pipeline_layout, "CAS pipeline layout");

	// we use specialization constants to avoid compiling it twice
	std::optional<std::string> cas_source = ReadShaderSource("shaders/vulkan/cas.glsl");
	if (!cas_source.has_value() || !GetCASShaderSource(&cas_source.value()))
		return false;

	VkShaderModule mod = g_vulkan_shader_cache->GetComputeShader(cas_source->c_str());
	ScopedGuard mod_guard = [this, &mod]() { vkDestroyShaderModule(m_device, mod, nullptr); };
	if (mod == VK_NULL_HANDLE)
		return false;

	for (u8 sharpen_only = 0; sharpen_only < 2; sharpen_only++)
	{
		Vulkan::ComputePipelineBuilder cpb;
		cpb.SetPipelineLayout(m_cas_pipeline_layout);
		cpb.SetShader(mod, "main");
		cpb.SetSpecializationBool(0, sharpen_only != 0);
		m_cas_pipelines[sharpen_only] = cpb.Create(dev, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_cas_pipelines[sharpen_only])
			return false;
	}

	m_features.cas_sharpening = true;
	return true;
}

bool GSDeviceVK::CompileFSR1Pipelines()
{
	VkDevice dev = m_device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	if (m_use_push_descriptors)
		dslb.SetPushFlag();
	// Combined image sampler, not SAMPLED_IMAGE as CAS uses: EASU reads through textureGather,
	// which needs a sampler bound to the image.
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	if ((m_fsr1_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_fsr1_ds_layout, "FSR1 descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, NUM_FSR1_CONSTANTS * sizeof(u32));
	plb.AddDescriptorSet(m_fsr1_ds_layout);
	if ((m_fsr1_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_fsr1_pipeline_layout, "FSR1 pipeline layout");

	// Two modules from two differently-#define'd copies of the same file, where CAS gets away
	// with one module and a specialization constant: FSR_EASU_F/FSR_RCAS_F decide which function
	// bodies ffx_fsr1.h emits, so a specialization constant would leave both calls unresolved.
	for (u8 easu_pass = 0; easu_pass < NUM_FSR1_PIPELINES; easu_pass++)
	{
		std::optional<std::string> fsr1_source = ReadShaderSource("shaders/vulkan/fsr1.glsl");
		if (!fsr1_source.has_value() || !GetFSR1ShaderSource(&fsr1_source.value(), easu_pass != 0))
			return false;

		VkShaderModule mod = g_vulkan_shader_cache->GetComputeShader(fsr1_source->c_str());
		if (mod == VK_NULL_HANDLE)
			return false;
		ScopedGuard mod_guard = [this, &mod]() { vkDestroyShaderModule(m_device, mod, nullptr); };

		Vulkan::ComputePipelineBuilder cpb;
		cpb.SetPipelineLayout(m_fsr1_pipeline_layout);
		cpb.SetShader(mod, "main");
		m_fsr1_pipelines[easu_pass] = cpb.Create(dev, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_fsr1_pipelines[easu_pass])
			return false;
	}

	m_features.fsr1 = true;
	return true;
}

bool GSDeviceVK::CompileSGSRPipeline()
{
	VkDevice dev = m_device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	if (m_use_push_descriptors)
		dslb.SetPushFlag();
	// Combined image sampler for the same reason FSR1 needs one: SGSR reads through
	// textureGather, which has to have a sampler bound to the image.
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	if ((m_sgsr_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_sgsr_ds_layout, "SGSR descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, NUM_SGSR_CONSTANTS * sizeof(u32));
	plb.AddDescriptorSet(m_sgsr_ds_layout);
	if ((m_sgsr_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_sgsr_pipeline_layout, "SGSR pipeline layout");

	// Two modules from two differently-#define'd copies of one file, the same shape FSR1 uses:
	// the variant decides which filter body the preprocessor emits at all, so it cannot be a
	// specialization constant.
	for (u8 edge = 0; edge < NUM_SGSR_PIPELINES; edge++)
	{
		std::optional<std::string> sgsr_source = ReadShaderSource("shaders/vulkan/sgsr.glsl");
		if (!sgsr_source.has_value())
			return false;
		sgsr_source->insert(0, edge ? "#version 460 core\n#define SGSR_EDGE_DIRECTION 1\n"
									: "#version 460 core\n#define SGSR_EDGE_DIRECTION 0\n");

		VkShaderModule mod = g_vulkan_shader_cache->GetComputeShader(sgsr_source->c_str());
		if (mod == VK_NULL_HANDLE)
			return false;
		ScopedGuard mod_guard = [this, &mod]() { vkDestroyShaderModule(m_device, mod, nullptr); };

		Vulkan::ComputePipelineBuilder cpb;
		cpb.SetPipelineLayout(m_sgsr_pipeline_layout);
		cpb.SetShader(mod, "main");
		m_sgsr_pipelines[edge] = cpb.Create(dev, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_sgsr_pipelines[edge])
			return false;
	}

	m_features.sgsr = true;
	return true;
}

bool GSDeviceVK::CompileImGuiPipeline()
{
	const std::optional<std::string> glsl = ReadShaderSource("shaders/vulkan/imgui.glsl");
	if (!glsl.has_value())
	{
		Console.Error("VK: Failed to read imgui.glsl");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(glsl.value(), "vs_main");
	if (vs == VK_NULL_HANDLE)
	{
		Console.Error("VK: Failed to compile ImGui vertex shader");
		return false;
	}
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	VkShaderModule ps = GetUtilityFragmentShader(glsl.value(), "ps_main");
	if (ps == VK_NULL_HANDLE)
	{
		Console.Error("VK: Failed to compile ImGui pixel shader");
		return false;
	}
	ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetRenderPass(m_swap_chain_render_pass, 0);
	gpb.AddVertexBuffer(0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX);
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos));
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv));
	gpb.AddVertexAttribute(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col));
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(ps);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
		VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);

	m_imgui_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(), false);
	if (!m_imgui_pipeline)
	{
		Console.Error("VK: Failed to compile ImGui pipeline");
		return false;
	}

	Vulkan::SetObjectName(m_device, m_imgui_pipeline, "ImGui pipeline");
	return true;
}

void GSDeviceVK::RenderImGui()
{
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (draw_data->CmdListsCount == 0)
		return;

	UpdateImGuiTextures();

	// ImGui's vertex Position and ClipRect both come in *logical* pixel coords
	// (against io.DisplaySize, which we set to the rotated presentation size).
	// uScale/uTranslate map pixel coords directly to NDC. Rotation transforms
	// logical pixels into the physical pixel space of the swapchain viewport,
	// so uScale must always be in physical units (not logical) for both the
	// rotated and unrotated paths.
	const float phys_w = static_cast<float>(m_window_info.surface_width);
	const float phys_h = static_cast<float>(m_window_info.surface_height);
	const GSVector4 uniforms(2.0f / phys_w, 2.0f / phys_h, -1.0f, -1.0f);

	SetUtilityPushConstants(&uniforms, sizeof(uniforms));
	SetPipeline(m_imgui_pipeline);

	if (m_utility_sampler != m_linear_sampler)
	{
		m_utility_sampler = m_linear_sampler;
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}

	// this is for presenting, we don't want to screw with the viewport/scissor set by display
	m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);

	// Logical/physical coords differ for Rot90/Rot270; the rotation transform
	// maps a logical pixel (lx, ly) on a (lw, lh) logical surface to a
	// physical pixel on the (phys_w, phys_h) swapchain. Rotation is around
	// the geometric centre of each surface.
	const bool rotate = (GSConfig.Rotation != DisplayRotation::Rot0);
	const GSVector2i pres = GetPresentationSize();
	const float lw = static_cast<float>(pres.x);
	const float lh = static_cast<float>(pres.y);
	const auto rotate_pixel = [&](float lx, float ly, float& px, float& py) {
		const float lcx = lx - lw * 0.5f;
		const float lcy = ly - lh * 0.5f;
		float pcx = lcx;
		float pcy = lcy;
		switch (GSConfig.Rotation)
		{
			case DisplayRotation::Rot90:
				pcx = lcy;
				pcy = -lcx;
				break;
			case DisplayRotation::Rot180:
				pcx = -lcx;
				pcy = -lcy;
				break;
			case DisplayRotation::Rot270:
				pcx = -lcy;
				pcy = lcx;
				break;
			default:
				break;
		}
		px = pcx + phys_w * 0.5f;
		py = pcy + phys_h * 0.5f;
	};

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];

		u32 vertex_offset;
		{
			const u32 size = sizeof(ImDrawVert) * static_cast<u32>(cmd_list->VtxBuffer.Size);
			if (!m_vertex_stream_buffer.ReserveMemory(size, sizeof(ImDrawVert)))
			{
				Console.Warning("VK: Skipping ImGui draw because of no vertex buffer space");
				return;
			}

			vertex_offset = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(ImDrawVert);
			if (!rotate)
			{
				std::memcpy(m_vertex_stream_buffer.GetCurrentHostPointer(),
					cmd_list->VtxBuffer.Data, size);
			}
			else
			{
				ImDrawVert* dst = reinterpret_cast<ImDrawVert*>(
					m_vertex_stream_buffer.GetCurrentHostPointer());
				const ImDrawVert* src = cmd_list->VtxBuffer.Data;
				for (int i = 0; i < cmd_list->VtxBuffer.Size; i++)
				{
					dst[i] = src[i];
					rotate_pixel(src[i].pos.x, src[i].pos.y, dst[i].pos.x, dst[i].pos.y);
				}
			}
			m_vertex_stream_buffer.CommitMemory(size);
		}

		static_assert(sizeof(ImDrawIdx) == sizeof(u16));
		IASetIndexBuffer(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size);

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			pxAssert(!pcmd->UserCallback);

			GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			if (rotate)
			{
				// Rotate the four corners of the logical clip rect into
				// physical space, then take their axis-aligned bounding box.
				// (90/270 rotations preserve axis-alignment.)
				float x0 = clip.x, y0 = clip.y, x1 = clip.z, y1 = clip.w;
				float c0x, c0y, c1x, c1y, c2x, c2y, c3x, c3y;
				rotate_pixel(x0, y0, c0x, c0y);
				rotate_pixel(x1, y0, c1x, c1y);
				rotate_pixel(x0, y1, c2x, c2y);
				rotate_pixel(x1, y1, c3x, c3y);
				const float xmin = std::min(std::min(c0x, c1x), std::min(c2x, c3x));
				const float xmax = std::max(std::max(c0x, c1x), std::max(c2x, c3x));
				const float ymin = std::min(std::min(c0y, c1y), std::min(c2y, c3y));
				const float ymax = std::max(std::max(c0y, c1y), std::max(c2y, c3y));
				clip = GSVector4(xmin, ymin, xmax, ymax);
			}

			SetScissor(GSVector4i(clip).max_i32(GSVector4i::zero()));

			// Since we don't have the GSTexture...
			GSTextureVK* tex = reinterpret_cast<GSTextureVK*>(pcmd->GetTexID());
			if (tex)
				SetUtilityTexture(tex, m_linear_sampler);

			if (ApplyUtilityState())
			{
				vkCmdDrawIndexed(GetCurrentCommandBuffer(), pcmd->ElemCount, 1, m_index.start + pcmd->IdxOffset,
					vertex_offset + pcmd->VtxOffset, 0);
			}
		}

		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}
}

void GSDeviceVK::RenderBlankFrame()
{
	VkResult res = m_swap_chain->AcquireNextImage();
	if (res != VK_SUCCESS)
	{
		Console.Error("VK: Failed to acquire image for blank frame present");
		return;
	}

	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();
	GSTextureVK* sctex = m_swap_chain->GetCurrentTexture();
	sctex->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferDst);

	constexpr VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(
		cmdbuffer, sctex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &s_present_clear_color.color, 1, &srr);

	m_swap_chain->GetCurrentTexture()->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::PresentSrc);
	SubmitCommandBuffer(m_swap_chain.get());
	ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
}

bool GSDeviceVK::DoCAS(
	GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	EndRenderPass();

	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

	// The sharpen reads the source from the COMPUTE stage, so it needs a compute-scoped transition for
	// the same reason the TileGpu writeback does -- ShaderReadOnly orders the fragment stage only, and
	// the pass that rendered this source is a colour-attachment write the dispatch would not be
	// waiting on. Same VkImage layout, so the descriptor written below is unaffected.
	sTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ComputeReadOnly);
	dTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ComputeReadWriteImage);

	// only happening once a frame, so the update isn't a huge deal.
	Vulkan::DescriptorSetUpdateBuilder dsub;
	if (m_use_push_descriptors)
	{
		dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, 0, sTexVK->GetView(), sTexVK->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(VK_NULL_HANDLE, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
		dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_cas_pipeline_layout, 0, false);
	}
	else
	{
		VkDescriptorSet ds = AllocateDescriptorSetFromFramePool(m_cas_ds_layout);
		if (ds == VK_NULL_HANDLE) [[unlikely]]
			return false; // single alloc per frame after EndRenderPass — exhaustion implausible; skip the sharpen pass
		dsub.AddImageDescriptorWrite(ds, 0, sTexVK->GetView(), sTexVK->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(ds, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
		dsub.Update(m_device);
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_cas_pipeline_layout, 0, 1, &ds, 0, nullptr);
	}

	// the actual meat and potatoes! only four commands.
	static const int threadGroupWorkRegionDim = 16;
	const int dispatchX = (dTex->GetWidth() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int dispatchY = (dTex->GetHeight() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

	vkCmdPushConstants(cmdbuf, m_cas_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, NUM_CAS_CONSTANTS * sizeof(u32),
		constants.data());
	vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_cas_pipelines[static_cast<u8>(sharpen_only)]);
	vkCmdDispatch(cmdbuf, dispatchX, dispatchY, 1);

	dTexVK->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

	// all done!
	return true;
}

bool GSDeviceVK::DoFSR1EASU(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_FSR1_CONSTANTS>& constants)
{
	return DoFSR1Pass(sTex, dTex, true, constants);
}

bool GSDeviceVK::DoFSR1RCAS(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_FSR1_CONSTANTS>& constants)
{
	return DoFSR1Pass(sTex, dTex, false, constants);
}

bool GSDeviceVK::DoFSR1Pass(
	GSTexture* sTex, GSTexture* dTex, bool easu_pass, const std::array<u32, NUM_FSR1_CONSTANTS>& constants)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	EndRenderPass();

	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

	// The EASU intermediate is handed to RCAS in compute and so never leaves GENERAL, which
	// defeats both of the backend's usual tools: Layout::ShaderReadOnly's barrier targets
	// VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT and would not make compute stores visible to a
	// compute read, and TransitionToLayout() early-outs when the layout already matches, so
	// re-requesting ComputeReadWriteImage emits nothing at all. Anything already in GENERAL
	// therefore needs its compute<->compute dependency stated by hand.
	const auto compute_barrier = [cmdbuf](GSTextureVK* tex, VkAccessFlags src_access, VkAccessFlags dst_access) {
		const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, src_access, dst_access,
			VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			tex->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
		vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &barrier);
	};

	if (sTexVK->GetLayout() == GSTextureVK::Layout::ComputeReadWriteImage)
	{
		// RCAS reading EASU's output. Without this it reads undefined data.
		compute_barrier(sTexVK, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	else
	{
		// EASU's input is the merged display texture, arriving from a colour-attachment write
		// exactly as CAS's does.
		sTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ShaderReadOnly);
	}

	if (dTexVK->GetLayout() == GSTextureVK::Layout::ComputeReadWriteImage)
	{
		// Every frame after the first: the intermediate was left in GENERAL for RCAS to read, so
		// this is what orders EASU's writes against the previous frame's RCAS reads of it.
		compute_barrier(dTexVK, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
	}
	else
	{
		dTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ComputeReadWriteImage);
	}

	// EASU gathers with normalised coordinates, so it needs the linear/clamp-to-edge sampler;
	// RCAS only texelFetches and ignores the sampler entirely.
	const VkSampler sampler = easu_pass ? m_linear_sampler : m_point_sampler;

	// only happening once a frame, so the update isn't a huge deal.
	Vulkan::DescriptorSetUpdateBuilder dsub;
	if (m_use_push_descriptors)
	{
		dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, 0, sTexVK->GetView(), sampler, sTexVK->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(VK_NULL_HANDLE, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
		dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_fsr1_pipeline_layout, 0, false);
	}
	else
	{
		VkDescriptorSet ds = AllocateDescriptorSetFromFramePool(m_fsr1_ds_layout);
		if (ds == VK_NULL_HANDLE) [[unlikely]]
			return false; // two allocs per frame after EndRenderPass - exhaustion implausible; skip the pass
		dsub.AddCombinedImageSamplerDescriptorWrite(ds, 0, sTexVK->GetView(), sampler, sTexVK->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(ds, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
		dsub.Update(m_device);
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_fsr1_pipeline_layout, 0, 1, &ds, 0, nullptr);
	}

	static const int threadGroupWorkRegionDim = 16;
	const int dispatchX = (dTex->GetWidth() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int dispatchY = (dTex->GetHeight() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

	// Full 80 bytes for both passes. RCAS only reads Const0, but the shared block puts `Sample`
	// at byte 64 either way, and it is read unconditionally - a 32-byte push leaves it undefined
	// and the shader squares the image.
	vkCmdPushConstants(cmdbuf, m_fsr1_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
		NUM_FSR1_CONSTANTS * sizeof(u32), constants.data());
	vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_fsr1_pipelines[static_cast<u8>(easu_pass)]);
	vkCmdDispatch(cmdbuf, dispatchX, dispatchY, 1);

	// The EASU target goes straight into RCAS in compute, so leave it in GENERAL and let the
	// barrier at the top of the next pass order the two dispatches. Only RCAS's output is handed
	// to the present pass, which samples it from the fragment stage.
	if (!easu_pass)
		dTexVK->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

	return true;
}

bool GSDeviceVK::DoSGSR(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_SGSR_CONSTANTS>& constants,
	bool edge_direction)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	EndRenderPass();

	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

	// Input arrives from a colour-attachment write, exactly as FSR1's EASU input does. There is
	// no compute->compute case here because SGSR has no intermediate.
	sTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ShaderReadOnly);

	if (dTexVK->GetLayout() == GSTextureVK::Layout::ComputeReadWriteImage)
	{
		// Every frame after the first: order this dispatch's writes against the previous frame's
		// reads of the same texture by the present pass. TransitionToLayout early-outs when the
		// layout already matches, so a same-layout dependency has to be stated by hand.
		const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			dTexVK->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
		vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
	}
	else
	{
		dTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ComputeReadWriteImage);
	}

	// Normalised coordinates and gathers, so linear/clamp-to-edge, like EASU.
	const VkSampler sampler = m_linear_sampler;

	Vulkan::DescriptorSetUpdateBuilder dsub;
	if (m_use_push_descriptors)
	{
		dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, 0, sTexVK->GetView(), sampler, sTexVK->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(VK_NULL_HANDLE, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
		dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_sgsr_pipeline_layout, 0, false);
	}
	else
	{
		VkDescriptorSet ds = AllocateDescriptorSetFromFramePool(m_sgsr_ds_layout);
		if (ds == VK_NULL_HANDLE) [[unlikely]]
			return false;
		dsub.AddCombinedImageSamplerDescriptorWrite(ds, 0, sTexVK->GetView(), sampler, sTexVK->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(ds, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
		dsub.Update(m_device);
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_sgsr_pipeline_layout, 0, 1, &ds, 0, nullptr);
	}

	// 8x8 local size, one pixel per invocation — not FSR1's 16, which comes from its 64 threads
	// each writing four pixels.
	static const int threadGroupWorkRegionDim = 8;
	const int dispatchX = (dTex->GetWidth() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int dispatchY = (dTex->GetHeight() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

	vkCmdPushConstants(cmdbuf, m_sgsr_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
		NUM_SGSR_CONSTANTS * sizeof(u32), constants.data());
	vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_sgsr_pipelines[edge_direction ? 1 : 0]);
	vkCmdDispatch(cmdbuf, dispatchX, dispatchY, 1);

	// Handed straight to the present pass, which samples it from the fragment stage.
	dTexVK->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

	return true;
}

void GSDeviceVK::DestroyResources()
{
	if (m_tfx_ubo_descriptor_set != VK_NULL_HANDLE)
		FreePersistentDescriptorSet(m_tfx_ubo_descriptor_set);

	for (auto& it : m_tfx_pipelines)
		vkDestroyPipeline(m_device, it.second, nullptr);
	for (auto& it : m_tfx_fragment_shaders)
		vkDestroyShaderModule(m_device, it.second, nullptr);
	for (auto& it : m_tfx_vertex_shaders)
		vkDestroyShaderModule(m_device, it.second, nullptr);
	for (VkPipeline it : m_interlace)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	for (VkPipeline it : m_merge)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	for (VkPipeline it : m_present)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	for (const auto& pipe : m_convert)
	{
		if (pipe != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, pipe, nullptr);
	}
	for (VkPipeline& pipe : m_tile_reinterpret)
	{
		if (pipe != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, pipe, nullptr);
		pipe = VK_NULL_HANDLE;
	}
	m_tile_reinterpret_tried = false;
	if (m_tile_expand_pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_device, m_tile_expand_pipeline, nullptr);
		m_tile_expand_pipeline = VK_NULL_HANDLE;
	}
	if (m_tile_expand_pipeline_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_tile_expand_pipeline_layout, nullptr);
		m_tile_expand_pipeline_layout = VK_NULL_HANDLE;
	}
	if (m_tile_expand_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tile_expand_ds_layout, nullptr);
		m_tile_expand_ds_layout = VK_NULL_HANDLE;
	}
	m_tile_expand_tried = false;

	if (m_tilegpu_state_descriptor_set != VK_NULL_HANDLE)
	{
		FreePersistentDescriptorSet(m_tilegpu_state_descriptor_set);
		m_tilegpu_state_descriptor_set = VK_NULL_HANDLE;
	}
	for (auto& row : m_tilegpu_pipeline)
	{
		for (VkPipeline& pipe : row)
		{
			if (pipe != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, pipe, nullptr);
			pipe = VK_NULL_HANDLE;
		}
	}
#ifdef PCSX2_DEVBUILD
	// The variant population, watched rather than feared. The doctrine that gets TileGpu under the
	// Adreno instruction-size threshold takes shader axes on need and lets the object count grow, so
	// the object count is a number somebody has to be able to read -- this is where it is read. The
	// twelve eager pipelines are not in either figure.
	if (!m_tilegpu_blend_pipelines.empty() || !m_tilegpu_fs_variants.empty())
	{
		Console.WriteLn("TileGpu variant population: %zu fragment modules, %zu pipelines (+12 eager)",
			m_tilegpu_fs_variants.size(), m_tilegpu_blend_pipelines.size());
	}
#endif
	for (auto& [key, pipe] : m_tilegpu_blend_pipelines)
	{
		if (pipe != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, pipe, nullptr);
	}
	m_tilegpu_blend_pipelines.clear();
	if (m_tilegpu_vs != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_device, m_tilegpu_vs, nullptr);
		m_tilegpu_vs = VK_NULL_HANDLE;
	}
	if (m_tilegpu_fs != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_device, m_tilegpu_fs, nullptr);
		m_tilegpu_fs = VK_NULL_HANDLE;
	}
	// The variants are separate modules; the full one above is not among them.
	for (auto& [variant_key, mod] : m_tilegpu_fs_variants)
	{
		if (mod != VK_NULL_HANDLE)
			vkDestroyShaderModule(m_device, mod, nullptr);
	}
	m_tilegpu_fs_variants.clear();
	m_tilegpu_shader_source = {};
	if (m_tilegpu_pipeline_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_tilegpu_pipeline_layout, nullptr);
		m_tilegpu_pipeline_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_ds_layout, nullptr);
		m_tilegpu_ds_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_dest_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_dest_ds_layout, nullptr);
		m_tilegpu_dest_ds_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_snapshot_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_snapshot_ds_layout, nullptr);
		m_tilegpu_snapshot_ds_layout = VK_NULL_HANDLE;
	}
	// The source sets come out of their own pool, so destroying it frees all of them at once; the
	// samplers belong to the device's sampler cache and are not ours to destroy.
	if (m_tilegpu_source_pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(m_device, m_tilegpu_source_pool, nullptr);
		m_tilegpu_source_pool = VK_NULL_HANDLE;
	}
	m_tilegpu_source_sets.fill(VK_NULL_HANDLE);
	m_tilegpu_source_set_epoch.fill(0);
	m_tilegpu_source_sampler.fill(VK_NULL_HANDLE);
	m_tilegpu_source_next_set = 0;
	m_tilegpu_source_set_count = 0;
	if (m_tilegpu_source_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_source_ds_layout, nullptr);
		m_tilegpu_source_ds_layout = VK_NULL_HANDLE;
	}
	for (u32 f = 0; f < kGSTileByteRoadFormats; f++)
	{
		if (m_tilegpu_seed_pipeline[f] != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, m_tilegpu_seed_pipeline[f], nullptr);
		m_tilegpu_seed_pipeline[f] = VK_NULL_HANDLE;
		m_tilegpu_seed_tried[f] = false;
	}
	for (u32 f = 0; f < kGSTileDepthRoadFormats; f++)
	{
		if (m_tilegpu_seed_depth_pipeline[f] != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, m_tilegpu_seed_depth_pipeline[f], nullptr);
		m_tilegpu_seed_depth_pipeline[f] = VK_NULL_HANDLE;
		m_tilegpu_seed_depth_tried[f] = false;
	}
	for (u32 f = 0; f < kTileGpuSrcFormats; f++)
	{
		if (m_tilegpu_materialise_pipeline[f] != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, m_tilegpu_materialise_pipeline[f], nullptr);
		m_tilegpu_materialise_pipeline[f] = VK_NULL_HANDLE;
		m_tilegpu_materialise_tried[f] = false;
	}
	if (m_tilegpu_expand_pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_device, m_tilegpu_expand_pipeline, nullptr);
		m_tilegpu_expand_pipeline = VK_NULL_HANDLE;
	}
	if (m_tilegpu_expand_pipeline_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_tilegpu_expand_pipeline_layout, nullptr);
		m_tilegpu_expand_pipeline_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_expand_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_expand_ds_layout, nullptr);
		m_tilegpu_expand_ds_layout = VK_NULL_HANDLE;
	}
	m_tilegpu_expand_tried = false;
	for (u32 f = 0; f < kTileGpuIdxFormats; f++)
	{
		if (m_tilegpu_donor_pipeline[f] != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, m_tilegpu_donor_pipeline[f], nullptr);
		m_tilegpu_donor_pipeline[f] = VK_NULL_HANDLE;
		m_tilegpu_donor_tried[f] = false;
	}
	for (u32 f = 0; f < kTileGpuClutSizes; f++)
	{
		if (m_tilegpu_clut_pipeline[f] != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, m_tilegpu_clut_pipeline[f], nullptr);
		m_tilegpu_clut_pipeline[f] = VK_NULL_HANDLE;
		m_tilegpu_clut_tried[f] = false;
	}
	if (m_tilegpu_readtarget_pipeline_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_tilegpu_readtarget_pipeline_layout, nullptr);
		m_tilegpu_readtarget_pipeline_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_readtarget_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_readtarget_ds_layout, nullptr);
		m_tilegpu_readtarget_ds_layout = VK_NULL_HANDLE;
	}
	for (u32 f = 0; f < kGSTileByteRoadFormats; f++)
	{
		if (m_tilegpu_writeback_pipeline[f] != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, m_tilegpu_writeback_pipeline[f], nullptr);
		m_tilegpu_writeback_pipeline[f] = VK_NULL_HANDLE;
		m_tilegpu_writeback_tried[f] = false;
	}
	if (m_tilegpu_writeback_pipeline_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_tilegpu_writeback_pipeline_layout, nullptr);
		m_tilegpu_writeback_pipeline_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_writeback_ds_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_device, m_tilegpu_writeback_ds_layout, nullptr);
		m_tilegpu_writeback_ds_layout = VK_NULL_HANDLE;
	}
	if (m_tilegpu_indirect_stream_buffer.IsValid())
		m_tilegpu_indirect_stream_buffer.Destroy(false);
	if (m_tilegpu_state_stream_buffer.IsValid())
		m_tilegpu_state_stream_buffer.Destroy(false);
	if (m_tilegpu_vram_stream_buffer.IsValid())
		m_tilegpu_vram_stream_buffer.Destroy(false);
	m_tilegpu_tried = false;
	m_tilegpu_tex = false;

	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 fbl = 0; fbl < 2; fbl++)
		{
			if (m_colclip_setup_pipelines[ds][fbl] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_colclip_setup_pipelines[ds][fbl], nullptr);
			if (m_colclip_finish_pipelines[ds][fbl] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_colclip_finish_pipelines[ds][fbl], nullptr);
		}
	}
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 datm = 0; datm < 4; datm++)
		{
			if (m_primid_image_setup_pipelines[ds][datm] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_primid_image_setup_pipelines[ds][datm], nullptr);
		}
	}
	if (m_fxaa_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_device, m_fxaa_pipeline, nullptr);
	if (m_shadeboost_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_device, m_shadeboost_pipeline, nullptr);

	for (VkPipeline it : m_cas_pipelines)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	if (m_cas_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_cas_pipeline_layout, nullptr);
	if (m_cas_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_cas_ds_layout, nullptr);

	for (VkPipeline it : m_fsr1_pipelines)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	if (m_fsr1_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_fsr1_pipeline_layout, nullptr);
	if (m_fsr1_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_fsr1_ds_layout, nullptr);

	for (VkPipeline it : m_sgsr_pipelines)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	if (m_sgsr_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_sgsr_pipeline_layout, nullptr);
	if (m_sgsr_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_sgsr_ds_layout, nullptr);

	if (m_imgui_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_device, m_imgui_pipeline, nullptr);

	for (const auto& it : m_samplers)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroySampler(m_device, it.second, nullptr);
	}
	m_samplers.clear();

	m_texture_stream_buffer.Destroy(false);
	m_fragment_uniform_stream_buffer.Destroy(false);
	m_vertex_uniform_stream_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);
	m_expand_index_stream_buffer.Destroy(false);
	if (m_expand_index_buffer != VK_NULL_HANDLE)
		vmaDestroyBuffer(m_allocator, m_expand_index_buffer, m_expand_index_buffer_allocation);

	if (m_tfx_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_tfx_pipeline_layout, nullptr);
	if (m_tfx_texture_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_tfx_texture_ds_layout, nullptr);
	if (m_tfx_ubo_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_tfx_ubo_ds_layout, nullptr);
	if (m_utility_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_utility_pipeline_layout, nullptr);
	if (m_utility_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_utility_ds_layout, nullptr);

	if (m_null_framebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(m_device, m_null_framebuffer, nullptr);
	}

	if (m_null_texture)
	{
		m_null_texture->Destroy(false);
		m_null_texture.reset();
	}

	if (m_oob.fence != VK_NULL_HANDLE)
		vkDestroyFence(m_device, m_oob.fence, nullptr);
	if (m_oob.command_buffer != VK_NULL_HANDLE)
		vkFreeCommandBuffers(m_device, m_oob.command_pool, 1, &m_oob.command_buffer);
	if (m_oob.command_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(m_device, m_oob.command_pool, nullptr);
	m_oob = {};

	// Say out loud what a run lost, so a recurrence shows up in a log rather than only in a frame.
	// The per-occurrence error is printed once; this is the total.
	if (m_tilegpu_declared_pool_dropped_draws != 0)
	{
		Console.Error("TileGpu: %u draws in %u passes were dropped this session because the frame descriptor pool "
					  "could not serve the in-pass destination read.",
			m_tilegpu_declared_pool_dropped_draws, m_tilegpu_declared_pool_dropped_passes);
	}
	if (m_tilegpu_pass_pool_dropped_draws != 0)
	{
		Console.Error("TileGpu: %u draws in %u passes were dropped this session because the frame descriptor pool "
					  "could not serve a pass's snapshot and sampled-target set.",
			m_tilegpu_pass_pool_dropped_draws, m_tilegpu_pass_pool_dropped_passes);
	}
	if (m_tilegpu_undrawn_frames != 0)
	{
		Console.Error("TileGpu: %u frames carrying %u draws recorded no geometry at all this session; their passes "
					  "cleared and stored, so they rendered black.",
			m_tilegpu_undrawn_frames, m_tilegpu_undrawn_draws);
	}
	if (m_tilegpu_abandoned_plans != 0)
	{
		Console.Error("TileGpu: %u plans were abandoned mid-frame this session; the passes after each were never "
					  "recorded, and the renderer's byte model counted them as done.",
			m_tilegpu_abandoned_plans);
	}
	if (m_tilegpu_variant_compile_failures != 0)
	{
		Console.Error("TileGpu: %u fragment variants failed to compile this session; their draws ran the full "
					  "program, which is correct and past the size threshold that swings a frame.",
			m_tilegpu_variant_compile_failures);
	}
	if (m_tilegpu_pipeline_fallback_runs != 0)
	{
		Console.Error("TileGpu: %u draw runs fell back to the eager full-road pipeline this session because their "
					  "own pipeline would not build; that pipeline writes all four channels.",
			m_tilegpu_pipeline_fallback_runs);
	}
	if (m_tilegpu_declared_build_dropped_draws != 0)
	{
		Console.Error("TileGpu: %u draws were dropped this session because a pipeline for a pass declaring the "
					  "in-pass destination read would not build, and no fallback is render-pass compatible.",
			m_tilegpu_declared_build_dropped_draws);
	}
	if (m_tilegpu_writeback_pool_dropped_ops != 0)
	{
		Console.Error("TileGpu: %u writebacks were skipped this session because the frame descriptor pool could not "
					  "serve them; their ring slots kept the bytes they were prefilled with.",
			m_tilegpu_writeback_pool_dropped_ops);
	}

	for (FrameResources& resources : m_frame_resources)
	{
		for (auto& it : resources.cleanup_resources)
			it();
		resources.cleanup_resources.clear();

		if (resources.fence != VK_NULL_HANDLE)
			vkDestroyFence(m_device, resources.fence, nullptr);
		if (resources.command_buffers[0] != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(m_device, resources.command_pool, static_cast<u32>(resources.command_buffers.size()),
				resources.command_buffers.data());
		}
		if (resources.command_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(m_device, resources.command_pool, nullptr);
		for (const VkDescriptorPool pool : resources.descriptor_pools)
			vkDestroyDescriptorPool(m_device, pool, nullptr);
		resources.descriptor_pools.clear();
	}

	if (m_timestamp_query_pool != VK_NULL_HANDLE)
		vkDestroyQueryPool(m_device, m_timestamp_query_pool, nullptr);

	if (m_pipeline_statistics_query_pool != VK_NULL_HANDLE)
		vkDestroyQueryPool(m_device, m_pipeline_statistics_query_pool, nullptr);

	if (m_global_descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_device, m_global_descriptor_pool, nullptr);

	for (auto& it : m_render_pass_cache)
		vkDestroyRenderPass(m_device, it.second, nullptr);
	m_render_pass_cache.clear();
	m_render_area_granularity.clear();

	if (m_allocator != VK_NULL_HANDLE)
		vmaDestroyAllocator(m_allocator);
}

VkShaderModule GSDeviceVK::GetTFXVertexShader(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	AddMacro(ss, "VS_TME", sel.tme);
	AddMacro(ss, "VS_FST", sel.fst);
	AddMacro(ss, "VS_IIP", sel.iip);
	AddMacro(ss, "VS_POINT_SIZE", sel.point_size);
	AddMacro(ss, "VS_EXPAND", static_cast<int>(sel.expand));
	AddMacro(ss, "VS_TILE_PRIM_ORD", sel.tile_prim_ord);
	AddMacro(ss, "VS_PROVOKING_VERTEX_LAST", static_cast<int>(m_features.provoking_vertex_last));
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetVertexShader(ss.str());
	if (mod)
		Vulkan::SetObjectName(m_device, mod, "TFX Vertex %08X", sel.key);

	m_tfx_vertex_shaders.emplace(sel.key, mod);
	return mod;
}

VkShaderModule GSDeviceVK::GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fragment_shaders.find(sel);
	if (it != m_tfx_fragment_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	AddMacro(ss, "PS_FST", sel.fst);
	AddMacro(ss, "PS_WMS", sel.wms);
	AddMacro(ss, "PS_WMT", sel.wmt);
	AddMacro(ss, "PS_ADJS", sel.adjs);
	AddMacro(ss, "PS_ADJT", sel.adjt);
	AddMacro(ss, "PS_AEM_FMT", sel.aem_fmt);
	AddMacro(ss, "PS_PAL_FMT", sel.pal_fmt);
	AddMacro(ss, "PS_DST_FMT", sel.dst_fmt);
	AddMacro(ss, "PS_DEPTH_FMT", sel.depth_fmt);
	AddMacro(ss, "PS_CHANNEL_FETCH", sel.channel);
	AddMacro(ss, "PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	AddMacro(ss, "PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	AddMacro(ss, "PS_AEM", sel.aem);
	AddMacro(ss, "PS_TFX", sel.tfx);
	AddMacro(ss, "PS_TCC", sel.tcc);
	AddMacro(ss, "PS_ATST", static_cast<u32>(sel.atst));
	AddMacro(ss, "PS_AFAIL", static_cast<u32>(sel.afail));
	AddMacro(ss, "PS_FOG", sel.fog);
	AddMacro(ss, "PS_BLEND_HW", sel.blend_hw);
	AddMacro(ss, "PS_A_MASKED", sel.a_masked);
	AddMacro(ss, "PS_FBA", sel.fba);
	AddMacro(ss, "PS_LTF", sel.ltf);
	AddMacro(ss, "PS_TILE_SNAP", sel.tile_snap);
	AddMacro(ss, "PS_TILE_LTF", sel.tile_ltf);
	AddMacro(ss, "PS_TILE_NN", sel.tile_nn);
	AddMacro(ss, "PS_TILE_MIP", sel.tile_mip);
	AddMacro(ss, "PS_TILE_LCM", sel.tile_lcm);
	AddMacro(ss, "PS_TILE_LTFX", sel.tile_ltfx);
	AddMacro(ss, "PS_TILE_VCOLOR", sel.tile_vcolor);
	AddMacro(ss, "PS_TILE_FOG", sel.tile_fog);
	AddMacro(ss, "PS_TILE_ZWALK", sel.tile_zwalk);
	AddMacro(ss, "PS_TILE_TWALK", sel.tile_twalk);
	AddMacro(ss, "PS_TILE_TWALK_FST", sel.tile_twalk_fst);
	AddMacro(ss, "PS_TILE_CWALK", sel.tile_cwalk);
	AddMacro(ss, "PS_TILE_TCLAG", sel.tile_tclag);
	AddMacro(ss, "PS_TILE_BLEND_MIX", sel.tile_blend_mix);
	AddMacro(ss, "PS_TILE_BLEND", sel.tile_blend);
	AddMacro(ss, "PS_TILE_DIRECT_IDX", sel.tile_direct_idx);
	AddMacro(ss, "PS_TILE_DIRECT_PAL", sel.tile_direct_pal);
	// The direct legs address a colour target through the GS swizzle: the forms
	// fitted from the tree's tables ride along as defines (GSTileSwizzleForms).
	if (sel.tile_direct_idx || sel.tile_direct_pal)
		ss << TileFormDefines();
	AddMacro(ss, "PS_AUTOMATIC_LOD", sel.automatic_lod);
	AddMacro(ss, "PS_MANUAL_LOD", sel.manual_lod);
	AddMacro(ss, "PS_COLCLIP", sel.colclip);
	AddMacro(ss, "PS_DATE", sel.date);
	AddMacro(ss, "PS_TCOFFSETHACK", sel.tcoffsethack);
	AddMacro(ss, "PS_REGION_RECT", sel.region_rect);
	AddMacro(ss, "PS_BLEND_A", sel.blend_a);
	AddMacro(ss, "PS_BLEND_B", sel.blend_b);
	AddMacro(ss, "PS_BLEND_C", sel.blend_c);
	AddMacro(ss, "PS_BLEND_D", sel.blend_d);
	AddMacro(ss, "PS_BLEND_MIX", sel.blend_mix);
	AddMacro(ss, "PS_ROUND_INV", sel.round_inv);
	AddMacro(ss, "PS_FIXED_ONE_A", sel.fixed_one_a);
	AddMacro(ss, "PS_IIP", sel.iip);
	AddMacro(ss, "PS_SHUFFLE", sel.shuffle);
	AddMacro(ss, "PS_SHUFFLE_SAME", sel.shuffle_same);
	AddMacro(ss, "PS_PROCESS_BA", sel.process_ba);
	AddMacro(ss, "PS_PROCESS_RG", sel.process_rg);
	AddMacro(ss, "PS_SHUFFLE_ACROSS", sel.shuffle_across);
	AddMacro(ss, "PS_READ16_SRC", sel.real16src);
	AddMacro(ss, "PS_WRITE_RG", sel.write_rg);
	AddMacro(ss, "PS_FBMASK", sel.fbmask);
	AddMacro(ss, "PS_COLCLIP_HW", sel.colclip_hw);
	AddMacro(ss, "PS_RTA_CORRECTION", sel.rta_correction);
	AddMacro(ss, "PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	AddMacro(ss, "PS_DITHER", sel.dither);
	AddMacro(ss, "PS_DITHER_ADJUST", sel.dither_adjust);
	AddMacro(ss, "PS_ZCLAMP", sel.zclamp);
	AddMacro(ss, "PS_ZFLOOR", sel.zfloor);
	AddMacro(ss, "PS_PABE", sel.pabe);
	AddMacro(ss, "PS_SCANMSK", sel.scanmsk);
	AddMacro(ss, "PS_TEX_IS_FB", sel.tex_is_fb);
	AddMacro(ss, "PS_NO_COLOR", sel.no_color);
	AddMacro(ss, "PS_NO_COLOR1", sel.no_color1);
	AddMacro(ss, "PS_BLEND_FACTOR_IN_ALPHA", sel.blend_factor_in_alpha);
	AddMacro(ss, "PS_ZTST", sel.ztst);
	AddMacro(ss, "PS_AA1", static_cast<u32>(sel.aa1));
	AddMacro(ss, "PS_ABE", sel.abe);
	AddMacro(ss, "PS_ANISOTROPIC_FILTERING", sel.sw_aniso);
	AddMacro(ss, "PS_ROV_COLOR", sel.rov_color);
	AddMacro(ss, "PS_ROV_DEPTH", static_cast<u32>(sel.rov_depth));
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetFragmentShader(ss.str());
	if (mod)
		Vulkan::SetObjectName(m_device, mod, "TFX Fragment %016" PRIX64 "_%016" PRIX64, sel.key_hi, sel.key_lo);

	m_tfx_fragment_shaders.emplace(sel, mod);
	return mod;
}

VkPipeline GSDeviceVK::CreateTFXPipeline(const PipelineSelector& p)
{
	static constexpr std::array<VkPrimitiveTopology, 3> topology_lookup = {{
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST, // Point
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST, // Line
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // Triangle
	}};

	GSHWDrawConfig::BlendState pbs{p.bs};
	GSHWDrawConfig::PSSelector pps{p.ps};
	if (!p.bs.IsEffective(p.cms))
	{
		// disable blending when colours are masked
		pbs = {};
		pps.no_color1 = true;
	}

	VkShaderModule vs = GetTFXVertexShader(p.vs);
	VkShaderModule fs = GetTFXFragmentShader(pps);
	if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);

	// Common state
	gpb.SetPipelineLayout(m_tfx_pipeline_layout);
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// DATE image prepass
		gpb.SetRenderPass(m_primid_image_setup_render_passes[p.ds][0], 0);
	}
	else
	{
		gpb.SetRenderPass(
			GetTFXRenderPass(p.rt, p.ds, p.ps.colclip_hw, p.dss.date,
				p.IsRTFeedbackLoop(), p.IsTestingAndSamplingDepth(),
				p.rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				p.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			0);
	}

	// Declare the feedback loops on the PIPELINE, not just on the image layout and render pass.
	//
	// We put attachments into VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT (see
	// UseFeedbackLoopLayout()) but never set the matching pipeline create flags. The spec requires
	// them on any pipeline used while its attachments are in that layout, so omitting them is
	// undefined behaviour rather than a missing optimisation — and strict mobile drivers (Adreno)
	// are exactly where undefined shows up as intermittently stale attachment reads.
	// Ported from sashkinbro/EmuCoreX ("Fix Vulkan attachment feedback pipelines").
	if (UseFeedbackLoopLayout())
	{
		if (p.IsRTFeedbackLoop())
			gpb.AddPipelineFlags(VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
		if (p.IsTestingAndSamplingDepth())
			gpb.AddPipelineFlags(VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
	}

	gpb.SetPrimitiveTopology(topology_lookup[p.topology]);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	if (m_optional_extensions.vk_ext_line_rasterization &&
		p.topology == static_cast<u8>(GSHWDrawConfig::Topology::Line))
	{
		gpb.SetLineRasterizationMode(VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
	}
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);

	// Shaders
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);

	// IA
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		gpb.AddVertexBuffer(0, sizeof(GSVertex));
		gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
		gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
		gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
		gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
		gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
		gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
		gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG
	}

	// DepthStencil
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
	if (p.dss.date)
	{
		const VkStencilOpState sos{VK_STENCIL_OP_KEEP, p.dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP,
			VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(true, sos, sos);
	}

	// Blending
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// image DATE prepass
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MIN, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
	}
	else if (pbs.enable)
	{
		// clang-format off
		static constexpr std::array<VkBlendFactor, 16> vk_blend_factors = { {
			VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
			VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
			VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO
		}};
		static constexpr std::array<VkBlendOp, 3> vk_blend_ops = {{
				VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT
		}};
		// clang-format on

		gpb.SetBlendAttachment(0, true, vk_blend_factors[pbs.src_factor], vk_blend_factors[pbs.dst_factor],
			vk_blend_ops[pbs.op], vk_blend_factors[pbs.src_factor_alpha], vk_blend_factors[pbs.dst_factor_alpha],
			VK_BLEND_OP_ADD, p.cms.wrgba);
	}
	else if (m_broken_colormask_with_depth && (p.cms.wrgba & 0x7u) == 0 &&
			 (p.dss.ztst != ZTST_ALWAYS || p.dss.zwe))
	{
		// Adreno colorWriteMask-with-depthtest bug (PPSSPP #10421): with a depth test
		// active the pipeline write mask is ignored, so a masked-RGB draw (FBMASK RGB=off)
		// would wrongly write colour. Only alpha can differ here (RGB is fully masked),
		// which Vulkan blend CAN express: open the write mask so the broken HW mask can't
		// misfire, keep old RGB via (src=ZERO,dst=ONE), gate alpha on wa. Arbitrary
		// per-channel RGB masks are not emulable this way (would corrupt), so they fall
		// through to the normal path below.
		const bool write_alpha = (p.cms.wrgba & 0x8u) != 0;
		gpb.SetBlendAttachment(0, true,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
			write_alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
			write_alpha ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
			VK_BLEND_OP_ADD, 0xFu);
	}
	else
	{
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.cms.wrgba);
	}

	// Tests have shown that it's faster to just enable rast order on the entire pass, rather than alternating
	// between turning it on and off for different draws, and adding the required barrier between non-rast-order
	// and rast-order draws.
	if (m_features.framebuffer_fetch && p.IsRTFeedbackLoop())
		gpb.AddBlendFlags(VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);

	// Mobile ordered depth feedback: the render pass declares ordered depth access (subpass flag
	// above) whenever depth is sampled on the fbfetch path with the toggle on — the pipeline bound
	// in that pass must carry the matching depth-stencil rasterization-order flag or it is invalid.
	// Condition mirrors the subpass flag exactly (key.depth_sampling == p.IsTestingAndSamplingDepth()).
	if (m_features.depth_feedback && m_features.framebuffer_fetch && p.IsTestingAndSamplingDepth() &&
		m_optional_extensions.vk_ext_roaa_depth)
		gpb.AddDepthStencilFlags(VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT);

	VkPipeline pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true));
	if (pipeline)
	{
		Vulkan::SetObjectName(
			m_device, pipeline, "TFX Pipeline %08X/%016" PRIX64 "_%016" PRIX64, p.vs.key, p.ps.key_hi, p.ps.key_lo);
	}

	return pipeline;
}

VkPipeline GSDeviceVK::GetTFXPipeline(const PipelineSelector& p)
{
	const auto it = m_tfx_pipelines.find(p);
	if (it != m_tfx_pipelines.end())
		return it->second;

	// A cache miss compiles SYNCHRONOUSLY on the GS thread, freezing the picture for as long as the
	// driver takes. Normally invisible (a few ms, spread out), but fast-forward runs through content
	// at 4x+ and hits a burst of new variants at once — which is the other half of "turn fast-forward
	// off and it hangs for a few seconds". Timed so the stall is measurable instead of inferred;
	// only slow compiles are logged, so this costs nothing in the common case.
	const Common::Timer::Value tfx_compile_start = Common::Timer::GetCurrentValue();
	VkPipeline pipeline = CreateTFXPipeline(p);
	const double tfx_compile_ms =
		Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - tfx_compile_start);
	if (tfx_compile_ms >= 20.0)
	{
		Console.Warning("@@ANDROID_TFXCOMPILE@@ %.1f ms for one pipeline (n=%u since last cache flush)",
			tfx_compile_ms, m_tfx_pipeline_compile_counter + 1);
	}
	m_tfx_pipelines.emplace(p, pipeline);

	// Persist the pipeline cache every N new compiles so an Android OOM-kill
	// or crash mid-session doesn't throw away pipelines that compiled after
	// the last onPause flush. Android gets a larger threshold because the log
	// showed repeated synchronous disk writes during normal gameplay in heavy
	// scenes; onPause still performs a final flush.
#ifdef __ANDROID__
	static constexpr u32 PIPELINE_CACHE_FLUSH_THRESHOLD = 256;
#else
	static constexpr u32 PIPELINE_CACHE_FLUSH_THRESHOLD = 32;
#endif
	if (g_vulkan_shader_cache && ++m_tfx_pipeline_compile_counter >= PIPELINE_CACHE_FLUSH_THRESHOLD)
	{
		m_tfx_pipeline_compile_counter = 0;
		g_vulkan_shader_cache->FlushPipelineCache();
	}
	return pipeline;
}

bool GSDeviceVK::BindDrawPipeline(const PipelineSelector& p)
{
	VkPipeline pipeline = GetTFXPipeline(p);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDeviceVK::InitializeState()
{
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_pass = VK_NULL_HANDLE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();

	m_utility_texture = m_null_texture.get();

	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	if (m_point_sampler)
		Vulkan::SetObjectName(m_device, m_point_sampler, "Point sampler");
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	if (m_linear_sampler)
		Vulkan::SetObjectName(m_device, m_linear_sampler, "Linear sampler");

	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_tfx_sampler = m_point_sampler;

	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
}

bool GSDeviceVK::CreatePersistentDescriptorSets()
{
	const VkDevice dev = m_device;
	Vulkan::DescriptorSetUpdateBuilder dsub;

	// Allocate UBO descriptor sets for TFX.
	m_tfx_ubo_descriptor_set = AllocatePersistentDescriptorSet(m_tfx_ubo_ds_layout);
	if (m_tfx_ubo_descriptor_set == VK_NULL_HANDLE)
		return false;
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_vertex_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::VSConstantBuffer));
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_fragment_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::PSConstantBuffer));
	if (m_features.vs_expand)
	{
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_vertex_stream_buffer.GetBuffer(), 0, VERTEX_BUFFER_SIZE);
	}
	if (m_features.aa1)
	{
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_expand_index_stream_buffer.GetBuffer(), 0, INDEX_BUFFER_SIZE);
	}
	dsub.Update(dev);
	Vulkan::SetObjectName(dev, m_tfx_ubo_descriptor_set, "Persistent TFX UBO set");
	return true;
}

GSDeviceVK::WaitType GSDeviceVK::GetWaitType(bool wait, bool spin)
{
	if (!wait)
		return WaitType::None;
	if (spin)
		return WaitType::Spin;
	else
		return WaitType::Sleep;
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion)
{
	EndRenderPass();
	ExecuteCommandBuffer(GetWaitType(wait_for_completion, GSConfig.HWSpinCPUForReadbacks));
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("VK: Executing command buffer due to '%s'", reason_str.c_str());
	ExecuteCommandBuffer(wait_for_completion);
}

void GSDeviceVK::ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion, const char* reason)
{
	Console.Warning("VK: Executing command buffer due to '%s'", reason);

	const VkRenderPass render_pass = m_current_render_pass;
	const GSVector4i render_pass_area = m_current_render_pass_area;
	const GSVector4i scissor = m_scissor;
	GSTexture* const current_rt = m_current_render_target;
	GSTexture* const current_ds = m_current_depth_target;
	const FeedbackLoopFlag current_feedback_loop = m_current_framebuffer_feedback_loop;

	EndRenderPass();
	ExecuteCommandBuffer(GetWaitType(wait_for_completion, GSConfig.HWSpinCPUForReadbacks));

	if (render_pass != VK_NULL_HANDLE)
	{
		// rebind framebuffer
		OMSetRenderTargets(current_rt, current_ds, scissor, current_feedback_loop);

		// restart render pass
		BeginRenderPass(GetRenderPassForRestarting(render_pass), render_pass_area);
	}

	// Push constants are command-buffer state. Utility draws often upload them before reserving their
	// vertices, so a stream-buffer rollover can happen after the upload — restore the cached block so
	// strict mobile drivers don't draw post-process/interlace with stale coordinates. From EmuCoreX.
	if (m_utility_push_constants_size > 0)
	{
		vkCmdPushConstants(GetCurrentCommandBuffer(), m_utility_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			m_utility_push_constants_size, m_utility_push_constants.data());
	}
}

void GSDeviceVK::ExecuteCommandBufferAndRestartPresent(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("VK: Executing command buffer due to '%s'", reason_str.c_str());

	pxAssert(m_is_presenting);
	vkCmdEndRenderPass(GetCurrentCommandBuffer());
	ExecuteCommandBuffer(wait_for_completion);

	GSTextureVK* swap_chain_texture = m_swap_chain->GetCurrentTexture();

	const VkFramebuffer fb = swap_chain_texture->GetFramebuffer(false);
	pxAssert(fb);

	const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
		GetRenderPass(swap_chain_texture->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE),
		fb,
		{{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}},
		0u, nullptr};
	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);

	// Dynamic viewport/scissor and push-constant state do not survive a command-buffer submission.
	// DoBeginPresent() normally emits them; this rare restart path must do the same, or strict mobile
	// Vulkan drivers present with undefined viewport/coordinates. Ported from sashkinbro/EmuCoreX.
	const VkViewport present_vp{0.0f, 0.0f, static_cast<float>(swap_chain_texture->GetWidth()),
		static_cast<float>(swap_chain_texture->GetHeight()), 0.0f, 1.0f};
	const VkRect2D present_scissor{
		{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}};
	vkCmdSetViewport(GetCurrentCommandBuffer(), 0, 1, &present_vp);
	vkCmdSetScissor(GetCurrentCommandBuffer(), 0, 1, &present_scissor);

	if (m_utility_push_constants_size > 0)
	{
		vkCmdPushConstants(GetCurrentCommandBuffer(), m_utility_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			m_utility_push_constants_size, m_utility_push_constants.data());
	}
}

void GSDeviceVK::ExecuteCommandBufferForReadback()
{
	m_readback_frame = m_frame;
	m_tilegpu_readback_frame = m_frame;
	ExecuteCommandBuffer(true);
	if (m_spinning_supported && GSConfig.HWSpinGPUForReadbacks)
	{
		m_spin_timer = 30;
		m_spin_manager.ReadbackRequested();

		if (!m_optional_extensions.vk_ext_calibrated_timestamps && !m_warned_slow_spin)
		{
			m_warned_slow_spin = true;
			Host::AddKeyedOSDMessage("GSDeviceVK_NoCalibratedTimestamps",
				TRANSLATE_STR("GS", "Spin GPU During Readbacks is enabled, but calibrated timestamps are unavailable.  "
									"This might be really slow."),
				Host::OSD_WARNING_DURATION);
		}
	}
}

void GSDeviceVK::InvalidateCachedState()
{
	m_dirty_flags = ALL_DIRTY_STATE;
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();
	m_utility_texture = m_null_texture.get();
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_target = nullptr;
	m_current_depth_target = nullptr;
	m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;

	m_current_pipeline_layout = PipelineLayout::Undefined;
	m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	m_tfx_rt_descriptor_set = VK_NULL_HANDLE;
	m_utility_descriptor_set = VK_NULL_HANDLE;
}

void GSDeviceVK::SetIndexBuffer(VkBuffer buffer)
{
	if (m_index_buffer == buffer)
		return;

	m_index_buffer = buffer;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceVK::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceVK::SetLineWidth(float width)
{
	if (m_current_line_width == width)
		return;

	m_current_line_width = width;
	m_dirty_flags |= DIRTY_FLAG_LINE_WIDTH;
}

void GSDeviceVK::PSSetROVs(GSTexture* rt, GSTexture* ds, bool write_rt, bool write_ds)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);
	GSTextureVK* oldVkRt = m_tfx_textures[TFX_TEXTURE_RT_ROV] != m_null_texture.get() ?
	                       m_tfx_textures[TFX_TEXTURE_RT_ROV] : nullptr;
	GSTextureVK* oldVkDs = m_tfx_textures[TFX_TEXTURE_DEPTH_ROV] != m_null_texture.get() ?
	                       m_tfx_textures[TFX_TEXTURE_DEPTH_ROV] : nullptr;

	if (!(vkRt || vkDs || oldVkRt || oldVkDs))
		return;

	pxAssert(!(vkRt || vkDs) || m_features.rov);

	if (vkRt)
	{
		PSSetShaderResource(TFX_TEXTURE_RT_ROV, vkRt, true, ResourceType::UAV);

		// Unbind conflicting RT texture
		PSSetShaderResource(TFX_TEXTURE_RT, nullptr, false);

		// Unbind conflicting source texture
		if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkRt)
		{
			PSSetShaderResource(TFX_TEXTURE_TEXTURE, nullptr, false);
		}

		if (write_rt)
		{
			vkRt->SetState(GSTexture::State::Dirty);
		}
	}
	else
	{
		// Unbind to avoid conflicts with OM targets.
		PSSetShaderResource(TFX_TEXTURE_RT_ROV, nullptr, false);
	}

	if (vkDs)
	{
		PSSetShaderResource(TFX_TEXTURE_DEPTH_ROV, vkDs, true, ResourceType::UAV);

		// Unbind conflicting depth texture
		PSSetShaderResource(TFX_TEXTURE_DEPTH, nullptr, false);

		// Unbind conflicting source texture
		if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkDs)
		{
			PSSetShaderResource(TFX_TEXTURE_TEXTURE, nullptr, false);
		}

		if (write_ds)
		{
			vkDs->SetState(GSTexture::State::Dirty);
		}
	}
	else
	{
		// Unbind to avoid conflicts with OM targets.
		PSSetShaderResource(TFX_TEXTURE_DEPTH_ROV, nullptr, false);
	}

	if (IsDeviceNVIDIA() && InRenderPass())
	{
		// Nvidia doesn't like switching ROV targets mid-render pass, doing so causes flickering or missing geometry.
		// End the render pass to avoid such issues.
		if (vkRt != oldVkRt || vkDs != oldVkDs)
		{
			GL_INS("VK: Ending render pass due to UAV switch");
			EndRenderPass();
		}
	}

	if (GSConfig.HWROVBarriersVK)
	{
		// This is to fix issues with some systems that seem to require barriers even with FSI.
		// Adds a barriers to the UAVs before every draw. Can result in a large performance hit.
		if ((vkRt || vkDs) && InRenderPass())
		{
			GL_INS("Ending render pass due to UAV barrier");
			EndRenderPass();
		}
		for (GSTextureVK* tex : std::array{ vkRt, vkDs })
		{
			if (tex)
			{
				if (InRenderPass())
				{
					GL_INS("Ending render pass due to UAV barrier");
					EndRenderPass();
				}
				// The subresources version does the barrier even when the before/after layout
				// are the same since we want a memory barrier here, not just a layout change.
				tex->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1, tex->GetLayout(), tex->GetLayout());
			}
		}
	}
}

void GSDeviceVK::PSSetShaderResource(int i, GSTexture* sr, bool check_state, ResourceType type)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(sr);
	bool update_layout = false;
	if (vkTex)
	{
		if (check_state)
		{
			const GSTextureVK::Layout layout = GetResourceLayout(type);

			vkTex->CommitClear();

			if (vkTex->GetLayout() != layout)
			{
				update_layout = true;
				if (InRenderPass())
				{
					GL_INS("Ending render pass due to resource transition");
					EndRenderPass();
				}
				vkTex->TransitionToLayout(layout);
			}
		}
		vkTex->SetUseFenceCounter(GetCurrentFenceCounter());
	}
	else
	{
		vkTex = m_null_texture.get();
	}

	if (m_tfx_textures[i] == vkTex && !update_layout)
		return;

	m_tfx_textures[i] = vkTex;
	m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << i);
}

void GSDeviceVK::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	m_tfx_sampler_sel = sel.key;
	m_tfx_sampler = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0;
}

void GSDeviceVK::SetUtilityTexture(GSTexture* tex, VkSampler sampler)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(tex);
	if (vkTex)
	{
		vkTex->CommitClear();
		vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		vkTex->SetUseFenceCounter(GetCurrentFenceCounter());
	}
	else
	{
		vkTex = m_null_texture.get();
	}

	if (m_utility_texture == vkTex && m_utility_sampler == sampler)
		return;

	m_utility_texture = vkTex;
	m_utility_sampler = sampler;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceVK::SetUtilityPushConstants(const void* data, u32 size)
{
	// Cache so a mid-utility-draw command-buffer rollover can replay these. Ported from sashkinbro/EmuCoreX.
	pxAssert(size <= m_utility_push_constants.size());
	std::memcpy(m_utility_push_constants.data(), data, size);
	m_utility_push_constants_size = size;
	vkCmdPushConstants(GetCurrentCommandBuffer(), m_utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void GSDeviceVK::UnbindTexture(GSTextureVK* tex)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex)
		{
			m_tfx_textures[i] = m_null_texture.get();
			m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << i);
		}
	}
	if (m_utility_texture == tex)
	{
		m_utility_texture = m_null_texture.get();
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (m_current_render_target == tex || m_current_depth_target == tex)
	{
		EndRenderPass();
		m_current_framebuffer = VK_NULL_HANDLE;
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;
	}
}

bool GSDeviceVK::InRenderPass()
{
	return m_current_render_pass != VK_NULL_HANDLE;
}

void GSDeviceVK::BeginRenderPass(VkRenderPass rp, const GSVector4i& rect)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;
	StampRenderTargetTouch();
	CountRenderPassArea(rect);

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}}, 0,
		nullptr};

	m_command_buffer_render_passes++;
	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::CountRenderPassArea(const GSVector4i& rect)
{
	// Counted where the area is handed to vkCmdBeginRenderPass, which is the only site that cannot
	// disagree with what was submitted -- a pass that clamps its area and a counter that predicts the
	// clamp would be two derivations of one number, and the reason to have this at all is that the
	// pass-structure census had to be assembled by hand.
	g_perfmon.Put(GSPerfMon::RenderPassAreaPixels,
		static_cast<double>(rect.width()) * static_cast<double>(rect.height()));
}

void GSDeviceVK::StampRenderTargetTouch()
{
	// A pass on an attachment already in its attachment layout records no transition,
	// and its draws are what change the image's bytes; the out-of-band readback path
	// must see the pass as a touch.
	const u64 counter = GetCurrentFenceCounter();
	if (m_current_render_target)
		m_current_render_target->StampGpuTouch(counter);
	if (m_current_depth_target)
		m_current_depth_target->StampGpuTouch(counter);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;
	StampRenderTargetTouch();
	CountRenderPassArea(rect);

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		cv_count, cv};

	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color)
{
	alignas(16) VkClearValue cv;
	GSVector4::store<true>((void*)cv.color.float32, GSVector4::unorm8(clear_color));
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil)
{
	VkClearValue cv;
	cv.depthStencil.depth = depth;
	cv.depthStencil.stencil = stencil;
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::EndRenderPass()
{
	if (m_current_render_pass == VK_NULL_HANDLE)
		return;

	m_current_render_pass = VK_NULL_HANDLE;
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);
	m_render_passes_since_submit++;

	vkCmdEndRenderPass(GetCurrentCommandBuffer());
}

void GSDeviceVK::SetViewport(const VkViewport& viewport)
{
	if (std::memcmp(&viewport, &m_viewport, sizeof(VkViewport)) == 0)
		return;

	std::memcpy(&m_viewport, &viewport, sizeof(VkViewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceVK::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::InvalidateCachedViewportScissor()
{
	// Say that the viewport and scissor were written straight into the command buffer, bypassing
	// SetViewport/SetScissor. Without it the cache still names the rectangle the cached road last
	// asked for AND still reads as clean, so the next draw through ApplyBaseState dedupes against
	// a value the command buffer no longer holds, emits nothing, and inherits the raw rectangle.
	//
	// The cached values themselves are deliberately left alone rather than overwritten with what
	// was emitted. They are what the cached road asked for, and something reads them back:
	// ExecuteCommandBufferAndRestartRenderPass replays m_scissor into the reopened pass, which
	// should be the drawing road's rectangle and not some executor internal. Marking both dirty
	// costs one redundant vkCmdSetViewport/vkCmdSetScissor pair at the next cached draw and cannot
	// lose a set that was pending.
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::SetPipeline(VkPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

void GSDeviceVK::SetInitialState(VkCommandBuffer cmdbuf)
{
	VkBuffer buffer = *m_vertex_stream_buffer.GetBufferPtr();
	if (buffer != VK_NULL_HANDLE)
	{
		constexpr VkDeviceSize buffer_offset = 0;
		vkCmdBindVertexBuffers(cmdbuf, 0, 1, &buffer, &buffer_offset);
	}
}

__ri void GSDeviceVK::ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, m_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	if (flags & DIRTY_FLAG_PIPELINE)
	{
		// Counted here, at the emitted bind, rather than in SetPipeline: SetPipeline
		// dedupes same-pipeline requests, and the cost being tracked is the command
		// actually reaching the driver.
		g_perfmon.Put(GSPerfMon::PipelineSwitches, 1);
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline);
	}

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor{
			{m_scissor.x, m_scissor.y}, {static_cast<u32>(m_scissor.width()), static_cast<u32>(m_scissor.height())}};
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const GSVector4 col(static_cast<float>(m_blend_constant_color) / 128.0f);
		vkCmdSetBlendConstants(cmdbuf, col.v);
	}

	if (flags & DIRTY_FLAG_LINE_WIDTH)
		vkCmdSetLineWidth(cmdbuf, m_current_line_width);
}

bool GSDeviceVK::ApplyTFXState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0)
		return true;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE | DIRTY_FLAG_TFX_UBO);

	// do cbuffer first, because it's the most likely to cause an exec
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(
			sizeof(m_vs_cb_cache), static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment)))
		{
			if (already_execed)
			{
				Console.Error("VK: Failed to reserve vertex uniform space");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of vertex uniform space");
			return ApplyTFXState(true);
		}

		std::memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(
				sizeof(m_ps_cb_cache), static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment)))
		{
			if (already_execed)
			{
				Console.Error("VK: Failed to reserve pixel uniform space");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of pixel uniform space");
			return ApplyTFXState(true);
		}

		std::memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
		m_fragment_uniform_stream_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	Vulkan::DescriptorSetUpdateBuilder dsub;
	if (m_current_pipeline_layout != PipelineLayout::TFX)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;
		flags |= DIRTY_FLAG_TFX_UBO | DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_VS_PUSH_CONSTANTS;

		// Clear out the RT/DS binding if feedback loop isn't on, because it'll be in the wrong state and make
		// the validation layer cranky. Not a big deal since we need to write it anyway.
		std::array<TFX_TEXTURES, 2> texture_types = { TFX_TEXTURE_RT, TFX_TEXTURE_DEPTH };
		for (u32 texture_type : texture_types)
		{
			const GSTextureVK::Layout tex_layout = m_tfx_textures[texture_type]->GetLayout();
			if (tex_layout != GSTextureVK::Layout::FeedbackLoop && tex_layout != GSTextureVK::Layout::ShaderReadOnly)
				m_tfx_textures[texture_type] = m_null_texture.get();
		}
	}

	if (flags & DIRTY_FLAG_TFX_UBO)
	{
		// Still need to bind the UBO descriptor set.
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, 0, 1,
			&m_tfx_ubo_descriptor_set, NUM_TFX_DYNAMIC_OFFSETS, m_tfx_dynamic_offsets.data());
	}

	if (m_features.vs_expand && (flags & DIRTY_FLAG_VS_PUSH_CONSTANTS))
		SetVSPushConstants(m_vs_pc_cache.base_vertex, m_vs_pc_cache.base_index, true);

	if (flags & DIRTY_FLAG_TFX_TEXTURES)
	{
		VkDescriptorSet ds = VK_NULL_HANDLE;
		// Non-push path allocates a fresh (empty) descriptor set, so every binding the
		// shader may read must be written — not just the dirty ones (push descriptors
		// persist the rest in command-buffer state; allocated sets do not). Force all
		// texture sub-flags on; all m_tfx_textures[] slots are always valid (null slots
		// hold m_null_texture), so this is safe.
		if (!m_use_push_descriptors)
		{
			flags |= DIRTY_FLAG_TFX_TEXTURES;
			ds = AllocateDescriptorSetFromFramePool(m_tfx_texture_ds_layout);
			if (ds == VK_NULL_HANDLE) [[unlikely]]
			{
				if (already_execed)
				{
					Console.Error("VK: Failed to allocate TFX texture descriptor set");
					return false;
				}

				// Frame descriptor pool exhausted — flush to reset it, then restart
				// the render pass and re-apply all state on the fresh command buffer.
				ExecuteCommandBufferAndRestartRenderPass(false, "Out of TFX texture descriptors");
				return ApplyTFXState(true);
			}
		}

		if (flags & DIRTY_FLAG_TFX_TEXTURE_TEX)
		{
			dsub.AddCombinedImageSamplerDescriptorWrite(ds, TFX_TEXTURE_TEXTURE,
				m_tfx_textures[TFX_TEXTURE_TEXTURE]->GetView(), m_tfx_sampler,
				m_tfx_textures[TFX_TEXTURE_TEXTURE]->GetVkLayout());
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_PALETTE)
		{
			dsub.AddImageDescriptorWrite(ds, TFX_TEXTURE_PALETTE,
				m_tfx_textures[TFX_TEXTURE_PALETTE]->GetView(), m_tfx_textures[TFX_TEXTURE_PALETTE]->GetVkLayout());
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_RT)
		{
			if (m_features.texture_barrier && !UseFeedbackLoopLayout() && m_device_properties.vendorID == 0x13B5u)
			{
				dsub.AddInputAttachmentDescriptorWrite(
					ds, TFX_TEXTURE_RT, m_tfx_textures[TFX_TEXTURE_RT]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
			}
			else
			{
				dsub.AddImageDescriptorWrite(ds, TFX_TEXTURE_RT, m_tfx_textures[TFX_TEXTURE_RT]->GetView(),
					m_tfx_textures[TFX_TEXTURE_RT]->GetVkLayout());
			}
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_PRIMID)
		{
			dsub.AddImageDescriptorWrite(ds, TFX_TEXTURE_PRIMID,
				m_tfx_textures[TFX_TEXTURE_PRIMID]->GetView(), m_tfx_textures[TFX_TEXTURE_PRIMID]->GetVkLayout());
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_DEPTH)
		{
			if (m_features.texture_barrier && !UseFeedbackLoopLayout() && m_device_properties.vendorID == 0x13B5u)
			{
				dsub.AddInputAttachmentDescriptorWrite(
					ds, TFX_TEXTURE_DEPTH, m_tfx_textures[TFX_TEXTURE_DEPTH]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
			}
			else
			{
				dsub.AddImageDescriptorWrite(ds, TFX_TEXTURE_DEPTH, m_tfx_textures[TFX_TEXTURE_DEPTH]->GetView(),
					m_tfx_textures[TFX_TEXTURE_DEPTH]->GetVkLayout());
			}
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_RT_ROV)
		{
			dsub.AddImageDescriptorWrite(ds, TFX_TEXTURE_RT_ROV, m_tfx_textures[TFX_TEXTURE_RT_ROV]->GetView(),
				m_tfx_textures[TFX_TEXTURE_RT_ROV]->GetVkLayout(), true);
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_DEPTH_ROV)
		{
			dsub.AddImageDescriptorWrite(ds, TFX_TEXTURE_DEPTH_ROV, m_tfx_textures[TFX_TEXTURE_DEPTH_ROV]->GetView(),
				m_tfx_textures[TFX_TEXTURE_DEPTH_ROV]->GetVkLayout(), true);
		}

		if (m_use_push_descriptors)
		{
			dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, TFX_DESCRIPTOR_SET_TEXTURES);
		}
		else
		{
			dsub.Update(m_device);
			vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout,
				TFX_DESCRIPTOR_SET_TEXTURES, 1, &ds, 0, nullptr);
		}
	}

	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::ApplyUtilityState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_UTILITY_STATE;

	if (m_current_pipeline_layout != PipelineLayout::Utility || flags & DIRTY_FLAG_UTILITY_TEXTURE)
	{
		m_current_pipeline_layout = PipelineLayout::Utility;

		Vulkan::DescriptorSetUpdateBuilder dsub;
		if (m_use_push_descriptors)
		{
			dsub.AddCombinedImageSamplerDescriptorWrite(
				VK_NULL_HANDLE, 0, m_utility_texture->GetView(), m_utility_sampler, m_utility_texture->GetVkLayout());
			dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout, 0, false);
		}
		else
		{
			VkDescriptorSet ds = AllocateDescriptorSetFromFramePool(m_utility_ds_layout);
			if (ds == VK_NULL_HANDLE) [[unlikely]]
			{
				if (already_execed)
				{
					Console.Error("VK: Failed to allocate utility descriptor set");
					return false;
				}

				// Frame descriptor pool exhausted — flush to reset it, then restart
				// the render pass and re-apply all state on the fresh command buffer.
				ExecuteCommandBufferAndRestartRenderPass(false, "Out of utility descriptors");
				return ApplyUtilityState(true);
			}
			dsub.AddCombinedImageSamplerDescriptorWrite(
				ds, 0, m_utility_texture->GetView(), m_utility_sampler, m_utility_texture->GetVkLayout());
			dsub.Update(m_device);
			vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout, 0, 1, &ds, 0, nullptr);
		}
	}


	ApplyBaseState(flags, cmdbuf);
	return true;
}

void GSDeviceVK::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetVSPushConstants(u32 base_vertex, u32 base_index, bool force_update)
{
	GSHWDrawConfig::VSPushConstants pc;
	pc.base_vertex = base_vertex;
	pc.base_index = base_index;

	if (m_vs_pc_cache.Update(pc) || force_update)
	{
		vkCmdPushConstants(GetCurrentCommandBuffer(), m_tfx_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	}
}

void GSDeviceVK::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	GL_PUSH("SetupDATE {%d,%d} %dx%d", bbox.left, bbox.top, bbox.width(), bbox.height());

	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(GetConvertPipeline(SetDATMShader(datm)));
	BeginClearRenderPass(m_date_setup_render_pass, bbox, 0.0f, 0);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTextureVK* GSDeviceVK::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	// How this is done:
	// - can't put a barrier for the image in the middle of the normal render pass, so that's out
	// - so, instead of just filling the int texture with INT_MAX, we sample the RT and use -1 for failing values
	// - then, instead of sampling the RT with DATE=1/2, we just do a min() without it, the -1 gets preserved
	// - then, the DATE=3 draw is done as normal
	GL_INS("Setup DATE Primitive ID Image for {%d,%d}-{%d,%d}", config.drawarea.left, config.drawarea.top,
		config.drawarea.right, config.drawarea.bottom);

	const GSVector2i rtsize(config.rt->GetSize());
	GSTextureVK* image =
		static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	// setup the fill quad to prefill with existing alpha values
	SetUtilityTexture(config.rt, m_point_sampler);
	OMSetRenderTargets(image, config.ds, config.drawarea);

	// if the depth target has been cleared, we need to preserve that clear
	const VkAttachmentLoadOp ds_load_op = GetLoadOpForTexture(static_cast<GSTextureVK*>(config.ds));
	const u32 ds = (config.ds ? 1 : 0);

	if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
	{
		VkClearValue cv[2] = {};
		cv[1].depthStencil.depth = static_cast<GSTextureVK*>(config.ds)->GetClearDepth();
		cv[1].depthStencil.stencil = 1;
		BeginClearRenderPass(m_primid_image_setup_render_passes[ds][1], GSVector4i::loadh(rtsize), cv, 2);
	}
	else
	{
		BeginRenderPass(m_primid_image_setup_render_passes[ds][0], config.drawarea);
	}

	// draw the quad to prefill the image
	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	const VkPipeline pipeline = m_primid_image_setup_pipelines[ds][static_cast<u8>(config.datm)];
	SetPipeline(pipeline);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	// image is now filled with either -1 or INT_MAX, so now we can do the prepass
	UploadHWDrawVerticesAndIndices(config);

	// primid texture will get re-bound, so clear it since we're using push descriptors
	PSSetShaderResource(3, m_null_texture.get(), false);

	// cut down the configuration for the prepass, we don't need blending or any feedback loop
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	pipe.dss.zwe = false;
	pipe.cms.wrgba = 0;
	pipe.bs = {};
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	pipe.rt = true;
	pipe.ps.blend_a = pipe.ps.blend_b = pipe.ps.blend_c = pipe.ps.blend_d = false;
	pipe.ps.no_color = false;
	pipe.ps.no_color1 = true;
	if (BindDrawPipeline(pipe))
		Draw(config);

	// image is initialized/prepass is done, so finish up and get ready to do the "real" draw
	EndRenderPass();

	// .. by setting it to DATE=3
	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	// and bind the image to the primitive sampler
	image->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	PSSetShaderResource(3, image, false);
	return image;
}

void GSDeviceVK::DoRenderHW(GSHWDrawConfig& config)
{
	// Mid-frame kick (see m_render_passes_since_submit in the header): while a
	// readback-prone frame is recording, submit accumulated work at a render-pass
	// boundary so the GPU executes concurrently with GS-thread recording instead of
	// only starting when the readback fence-waits on it. Draw entry is the safe spot:
	// nothing is staged yet, and every binding below re-applies via dirty flags.
	// Gated to outside-a-render-pass (no forced tile flush on tilers) and to frames
	// near an actual readback (games that never read back see zero change).
	//
	// Do not read the threshold as a submit budget. It sets how often we *offer* to kick;
	// what we get is decided by the fence gate below, and that gate binds by a wide margin.
	// With three command buffers only two submissions can be in flight, so a third kick
	// needs the first to have retired. Measured on Rogue Galaxy (M2/Honeykrisp, 60 frames,
	// ~116 RPs/frame): the arithmetic "RPs-per-frame / threshold" predicts ~14 kicks/frame
	// and the real number is 2, because ~3300 of ~3400 offers find the next command buffer
	// still executing. So raising the threshold buys far less than it looks like it should,
	// and lowering it buys nothing. Sweeping it 8->16 measured -2% total GPU stall on Rogue
	// Galaxy and +12% on OutRun 2006, i.e. no free lunch in either direction.
	constexpr u32 kick_threshold = 8;
	constexpr u32 readback_window_frames = 3;
	// A draw into a recent readback source is (almost certainly) producing the data for
	// the next readback, which follows immediately — kick regardless of the threshold so
	// the backlog drains during this pass's recording and the readback waits only on the
	// pass itself plus the copy (see m_recent_readback_sources).
	const bool produces_readback_data =
		config.rt && (config.rt == m_recent_readback_sources[0] || config.rt == m_recent_readback_sources[1]);
	const bool near_readback = m_readback_frame != ~0u && (m_frame - m_readback_frame) <= readback_window_frames;
	if (near_readback &&
		(m_render_passes_since_submit >= kick_threshold ||
			(produces_readback_data && m_render_passes_since_submit > 0)) &&
		!InRenderPass())
	{
		// The kick must never block: submitting cycles to the next command buffer, and
		// ActivateCommandBuffer fence-waits if that buffer's previous submission is still
		// executing — a hidden GPU-sync worse than the backlog the kick drains. Only kick
		// when the next buffer is verifiably complete; otherwise keep recording and retry
		// at the next draw (the counter keeps the gate open).
		ScanForCommandBufferCompletion();
		const u32 next_buffer = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		if (m_frame_resources[next_buffer].fence_counter <= m_completed_fence_counter)
			ExecuteCommandBuffer(WaitType::None);
	}

	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());
	GSTextureVK* draw_rt = config.ps.HasColorROV() ? nullptr : static_cast<GSTextureVK*>(config.rt);
	GSTextureVK* draw_ds = config.ps.HasDepthROV() ? nullptr : static_cast<GSTextureVK*>(config.ds);
	GSTextureVK* draw_rt_rov = config.ps.HasColorROV() ? static_cast<GSTextureVK*>(config.rt) : nullptr;
	GSTextureVK* draw_ds_rov = config.ps.HasDepthROV() ? static_cast<GSTextureVK*>(config.ds) : nullptr;
	GSTextureVK* draw_rt_clone = nullptr;
	GSTextureVK* colclip_rt = static_cast<GSTextureVK*>(g_gs_device->GetColorClipTexture());

	// stream buffer in first, in case we need to exec
	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	// bind textures before checking the render pass, in case we need to transition them
	if (config.tex)
	{
		PSSetShaderResource(TFX_TEXTURE_TEXTURE, config.tex, config.tex != config.rt && config.tex != config.ds);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(TFX_TEXTURE_PALETTE, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	if (config.topology == GSHWDrawConfig::Topology::Line)
		SetLineWidth(config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f);

	// Primitive ID tracking DATE setup.
	// Needs to be done before
	GSTextureVK* date_image = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		// If we have a colclip in progress, we need to use the colclip texture, but we can't check this later as there's a chicken/egg problem with the pipe setup.
		GSTexture* backup_rt = config.rt;

		if (colclip_rt)
			config.rt = colclip_rt;

		date_image = SetupPrimitiveTrackingDATE(config);
		if (!date_image)
		{
			Console.Warning("VK: Failed to allocate DATE image, aborting draw.");
			return;
		}

		config.rt = backup_rt;
	}

	// figure out the pipeline
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);

	// now blit the colclip texture back to the original target
	if (colclip_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			GL_PUSH("Blit ColorClip back to RT");

			EndRenderPass();
			colclip_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

			draw_rt = static_cast<GSTextureVK*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, GSVector4i::loadh(rtsize), static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				alignas(16) VkClearValue cvs[2];
				u32 cv_count = 0;
				GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetClearForFormat());
				if (draw_ds)
					cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

				BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
										 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
										 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect(), cvs, cv_count);
				draw_rt->SetState(GSTexture::State::Dirty);
			}
			else
			{
				BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
									pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect());
			}

			const GSVector4 drawareaf = GSVector4(config.colclip_update_area);
			const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
			SetPipeline(m_colclip_finish_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
			SetUtilityTexture(colclip_rt, m_point_sampler);
			DrawStretchRect(sRect, drawareaf, rtsize);

			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);

			colclip_rt = nullptr;
			draw_rt = config.ps.HasColorROV() ? nullptr : static_cast<GSTextureVK*>(config.rt);
		}
		else
		{
			pipe.ps.colclip_hw = 1;
			draw_rt = colclip_rt;
		}
	}

	// Destination Alpha Setup
	const bool need_barrier = config.require_one_barrier || (config.require_full_barrier && m_features.texture_barrier);
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::Full: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking: // Setup is done below
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne: // setup is done below
		{
			// we only need to do the setup here if we don't have barriers, in which case do full DATE.
			if (!need_barrier)
			{
				SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
				config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
			}
		}
		break;

		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
			break;
	}

	// Switch to colclip target for colclip hw rendering
	if (pipe.ps.colclip_hw)
	{
		if (!colclip_rt)
		{
			config.colclip_update_area = config.drawarea;
			EndRenderPass();
			colclip_rt = static_cast<GSTextureVK*>(CreateFeedbackTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!colclip_rt)
			{
				Console.Warning("VK: Failed to allocate ColorClip render target, aborting draw.");

				if (date_image)
					Recycle(date_image);

				GL_POP();
				return;
			}
			g_gs_device->SetColorClipTexture(static_cast<GSTexture*>(colclip_rt));

			// propagate clear value through if the colclip render is the first
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				colclip_rt->SetState(GSTexture::State::Cleared);
				colclip_rt->SetClearColor(draw_rt->GetClearColor());

				// If depth is cleared, we need to commit it, because we're only going to draw to the active part of the FB.
				if (draw_ds && draw_ds->GetState() == GSTexture::State::Cleared && !config.drawarea.eq(GSVector4i::loadh(rtsize)))
					draw_ds->CommitClear(m_current_command_buffer);
			}
			else if (draw_rt->GetState() == GSTexture::State::Dirty)
			{
				GL_PUSH_("ColorClip Render Target Setup");
				draw_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			}

			// we're not drawing to the RT, so we can use it as a source
			if (config.require_one_barrier && !m_features.texture_barrier)
				PSSetShaderResource(2, draw_rt, true);
		}
		draw_rt = colclip_rt;
	}

	// clear texture binding when it's bound to RT or DS.
	if (!config.tex && ((config.rt && static_cast<GSTextureVK*>(config.rt) == m_tfx_textures[0]) ||
						   (config.ds && static_cast<GSTextureVK*>(config.ds) == m_tfx_textures[0])))
	{
		PSSetShaderResource(0, nullptr, false);
	}

	// render pass restart optimizations
	if (colclip_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly))
	{
		// colclip hw requires blitting.
		EndRenderPass();
	}
	else if (InRenderPass() &&
		((draw_rt && m_current_render_target == draw_rt) ||
		(draw_ds && m_current_depth_target == draw_ds)))
	{
		// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
		// keep the depth even if doing colclip hw draws, because the next draw will probably re-enable depth.
		if (!(draw_rt || draw_rt_rov) && m_current_render_target && config.tex != m_current_render_target &&
			draw_ds && m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			m_pipeline_selector.rt = true;
		}
		else if (!(draw_ds || draw_ds_rov) && m_current_depth_target && config.tex != m_current_depth_target &&
			draw_rt && m_current_depth_target->GetSize() == draw_rt->GetSize())
		{
			draw_ds = m_current_depth_target;
			m_pipeline_selector.ds = true;
		}

		// Everything EXCEPT Broadcom keeps feedback-loop state draw-local: carrying it over
		// can leave later draws in the previous feedback render pass/layout and cause
		// Vulkan-only flicker. That matches sashkinbro/EmuCoreX, which removes the carry
		// globally. A vendor-scoped carry was tried once before and reverted — do NOT widen
		// this past Broadcom without re-testing Adreno/Mali.
		//
		// Broadcom/V3D (Raspberry Pi, via the Linux arm64 build) is tile-based and pays
		// heavily to close and reopen a tile render pass. Carrying the flags keeps the
		// feedback_loop passed to OMSetRenderTargets equal to m_current_framebuffer_feedback_loop,
		// so the render pass is NOT restarted for an otherwise-identical attachment set.
		//
		// Gated PER TARGET, not on the enclosing condition — that only requires ONE of rt/ds
		// to match, so a draw keeping the RT but swapping the depth target would otherwise
		// inherit a stale depth feedback layout: precisely the flicker mode described above.
		if (IsDeviceBroadcom())
		{
			if (draw_rt && m_current_render_target == draw_rt)
				pipe.feedback_loop_flags |= m_current_framebuffer_feedback_loop & FeedbackLoopFlag_ReadAndWriteRT;
			if (draw_ds && m_current_depth_target == draw_ds)
			{
				pipe.feedback_loop_flags |= (m_current_framebuffer_feedback_loop &
					(FeedbackLoopFlag_ReadAndWriteDepth | FeedbackLoopFlag_ReadDepth));
			}
		}
	}

	if (draw_rt && ((config.require_one_barrier && (config.IsFeedbackLoopRT(config.ps) || config.IsFeedbackLoopRT(config.alpha_second_pass.ps)))) && !m_features.texture_barrier)
	{
		// Requires a copy of the RT.
		draw_rt_clone = static_cast<GSTextureVK*>(CreateTexture(rtsize.x, rtsize.y, 1, draw_rt->GetFormat(), true));
		if (draw_rt_clone)
		{
			GL_PUSH("VK: Copy RT to temp texture {%d,%d %dx%d}",
				config.drawarea.left, config.drawarea.top,
				config.drawarea.width(), config.drawarea.height());
			EndRenderPass();
			if (config.tex_hazard)
			{
				const GSVector4i union_rect = config.drawarea.runion(config.samplearea);
				const u32 size_union = union_rect.width() * union_rect.height();
				const u32 size_indiv = config.drawarea.width() * config.drawarea.height() +
				                       config.samplearea.width() * config.samplearea.height();

				// Do an individual copy if the union is larger than the sum of individual areas.
				if (size_union > size_indiv)
				{
					const GSVector4i snapped_drawarea = ProcessCopyArea(GSVector4i(0, 0, rtsize.x, rtsize.y), config.drawarea);
					const GSVector4i snapped_samplearea = ProcessCopyArea(GSVector4i(0, 0, rtsize.x, rtsize.y), config.samplearea);
					DoCopyRect(draw_rt, draw_rt_clone, snapped_drawarea, snapped_drawarea.left, snapped_drawarea.top);
					DoCopyRect(draw_rt, draw_rt_clone, snapped_samplearea, snapped_samplearea.left, snapped_samplearea.top);
				}
				else
				{
					DoCopyRect(draw_rt, draw_rt_clone, union_rect, union_rect.left, union_rect.top);
				}
			}
			else
			{
				DoCopyRect(draw_rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
			}

			if (config.require_one_barrier)
				PSSetShaderResource(2, draw_rt_clone, true);
			if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT)
				PSSetShaderResource(0, draw_rt_clone, true);
		}
		else
			Console.Warning("VK: Failed to allocate temp texture for RT copy.");
	}

	if (pipe.IsRTFeedbackLoop() && draw_rt)
	{
		pxAssertMsg(m_features.texture_barrier, "Texture barriers enabled");
		PSSetShaderResource(TFX_TEXTURE_RT, draw_rt, false);

		if (m_tfx_textures[TFX_TEXTURE_RT_ROV] == draw_rt)
		{
			// Unbind to avoid conflicts.
			PSSetShaderResource(TFX_TEXTURE_RT_ROV, nullptr, false);
			m_dirty_flags |= static_cast<u32>(DIRTY_FLAG_TFX_TEXTURE_RT_ROV);
		}
	}
	else if (!draw_rt_clone)
	{
		// Unbind to avoid conflicts with framebuffer
		PSSetShaderResource(TFX_TEXTURE_RT, nullptr, false);
	}
	
	if (pipe.IsDepthFeedbackLoop() && draw_ds)
	{
		pxAssertMsg(m_features.texture_barrier, "Texture barriers enabled");
		PSSetShaderResource(TFX_TEXTURE_DEPTH, draw_ds, false);

		if (m_tfx_textures[TFX_TEXTURE_DEPTH_ROV] == draw_ds)
		{
			// Unbind to avoid conflicts.
			PSSetShaderResource(TFX_TEXTURE_DEPTH_ROV, nullptr, false);
			m_dirty_flags |= static_cast<u32>(DIRTY_FLAG_TFX_TEXTURE_DEPTH_ROV);
		}
	}
	else
	{
		// Unbind to avoid conflicts with framebuffer
		PSSetShaderResource(TFX_TEXTURE_DEPTH, nullptr, false);
	}
	
	PSSetROVs(draw_rt_rov, draw_ds_rov, config.ps.HasColorOutput(), config.ps.HasDepthROVWrite());

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags), rtsize);

	// Begin render pass if new target or out of the area.
	if (!InRenderPass())
	{
		VkAttachmentLoadOp rt_op = GetLoadOpForTexture(draw_rt);
		VkAttachmentLoadOp ds_op = GetLoadOpForTexture(draw_ds);
		// A feedback-loop draw reads the attachment via subpassLoad; a DONT_CARE load op leaves it
		// uninitialized, so the coherent read returns undefined (tile) memory. Force LOAD so the read
		// sees real content - you cannot coherently read what you did not load.
		if (pipe.IsRTFeedbackLoop() && rt_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			rt_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (pipe.IsDepthFeedbackLoop() && ds_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			ds_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		const VkRenderPass rp = GetTFXRenderPass(pipe.rt, pipe.ds, pipe.ps.colclip_hw,
			config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil, pipe.IsRTFeedbackLoop(),
			pipe.IsTestingAndSamplingDepth(), rt_op, ds_op);
		const bool is_clearing_rt = (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR);

		// Only draw to the active area of the colclip hw target. Except when depth is cleared, we need to use the full
		// buffer size, otherwise it'll only clear the draw part of the depth buffer.
		const GSVector4i render_area = (pipe.ps.colclip_hw && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && ds_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
		                             ? config.drawarea
		                             : GSVector4i::loadh(rtsize);

		if (is_clearing_rt)
		{
			// when we're clearing, we set the draw area to the whole fb, otherwise part of it will be undefined
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			if (draw_rt)
			{
				GSVector4 clear_color = draw_rt->GetClearForFormat();
				if (pipe.ps.colclip_hw)
				{
					// Denormalize clear color for hw colclip.
					clear_color *= GSVector4::cxpr(255.0f / 65535.0f, 255.0f / 65535.0f, 255.0f / 65535.0f, 1.0f);
				}
				GSVector4::store<true>(&cvs[cv_count++].color, clear_color);
			}
			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 0};

			BeginClearRenderPass(rp, render_area, cvs, cv_count);
		}
		else
		{
			BeginRenderPass(rp, render_area);
		}
	}

	// Guard on stencil_buffer: devices without a stencil attachment (e.g. Adreno, forced D32F) have no
	// stencil aspect to clear.
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne && m_features.stencil_buffer)
	{
		const VkClearAttachment ca = {VK_IMAGE_ASPECT_STENCIL_BIT, 0u, {.depthStencil = {0.0f, 1u}}};
		const VkClearRect rc = {{{config.drawarea.left, config.drawarea.top},
									{static_cast<u32>(config.drawarea.width()), static_cast<u32>(config.drawarea.height())}},
			0u, 1u};
		vkCmdClearAttachments(m_current_command_buffer, 1, &ca, 1, &rc);
	}

	// rt -> colclip hw blit if enabled
	if (colclip_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && config.rt->GetState() == GSTexture::State::Dirty)
	{
		OMSetRenderTargets(draw_rt, draw_ds, GSVector4i::loadh(rtsize), static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
		SetUtilityTexture(static_cast<GSTextureVK*>(config.rt), m_point_sampler);
		SetPipeline(m_colclip_setup_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);

		const GSVector4 drawareaf = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		DrawStretchRect(sRect, drawareaf, rtsize);

		GL_POP();
		OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
	}

	// VB/IB upload, if we did DATE setup and it's not colclip hw this has already been done
	if (!date_image || colclip_rt)
		UploadHWDrawVerticesAndIndices(config);

	// now we can do the actual draw
	if (BindDrawPipeline(pipe))
		SendHWDraw(config, pipe.IsRTFeedbackLoop() ? draw_rt : nullptr, pipe.IsDepthFeedbackLoop() ? draw_ds : nullptr,
			config.require_one_barrier, config.require_full_barrier);

	// blend second pass
	if (config.blend_multi_pass.enable)
	{
		if (config.blend_multi_pass.blend.constant_enable)
			SetBlendConstants(config.blend_multi_pass.blend.constant);

		pipe.bs = config.blend_multi_pass.blend;
		pipe.ps.no_color1 = config.blend_multi_pass.no_color1;
		pipe.ps.blend_hw = config.blend_multi_pass.blend_hw;
		pipe.ps.dither = config.blend_multi_pass.dither;
		if (BindDrawPipeline(pipe))
		{
			// TODO: This probably should have barriers, in case we want to use it conditionally.
			Draw(config);
		}
	}

	// and the alpha pass
	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.cms = config.alpha_second_pass.colormask;
		pipe.dss = config.alpha_second_pass.depth;
		pipe.bs = config.blend;
		if (BindDrawPipeline(pipe))
		{
			SendHWDraw(config, pipe.IsRTFeedbackLoop() ? draw_rt : nullptr, pipe.IsDepthFeedbackLoop() ? draw_ds : nullptr,
				config.alpha_second_pass.require_one_barrier, config.alpha_second_pass.require_full_barrier);
		}
	}

	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (date_image)
		Recycle(date_image);

	// now blit the colclip texture back to the original target
	if (colclip_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if ((config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve))
		{
			GL_PUSH("Blit ColorClip back to RT");

			EndRenderPass();

			colclip_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

			draw_rt = static_cast<GSTextureVK*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly) ? GSVector4i::loadh(rtsize) : config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				alignas(16) VkClearValue cvs[2];
				u32 cv_count = 0;
				GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetClearForFormat());
				if (draw_ds)
					cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

				BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
										 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
										 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect(), cvs, cv_count);
				draw_rt->SetState(GSTexture::State::Dirty);
			}
			else
			{
				BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
									pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect());
			}

			const GSVector4 drawareaf = GSVector4(config.colclip_update_area);
			const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
			SetPipeline(m_colclip_finish_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
			SetUtilityTexture(colclip_rt, m_point_sampler);
			DrawStretchRect(sRect, drawareaf, rtsize);

			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
	}

	config.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
}

void GSDeviceVK::UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	pipe.vs.key = config.vs.key;
	pipe.ps.key_hi = config.ps.key_hi;
	pipe.ps.key_lo = config.ps.key_lo;
	pipe.dss.key = config.ps.HasDepthROV() ? GSHWDrawConfig::DepthStencilSelector::NoDepth().key : config.depth.key;
	pipe.bs.key = config.ps.HasColorROV() ? GSHWDrawConfig::BlendState().key : config.blend.key;
	pipe.bs.constant = 0; // don't dupe states with different alpha values
	pipe.cms.key = config.ps.HasColorROV() ? GSHWDrawConfig::ColorMaskSelector().key : config.colormask.key;
	pipe.topology = static_cast<u32>(config.topology);
	pipe.rt = config.rt != nullptr && !config.ps.HasColorROV();
	pipe.ds = config.ds != nullptr && !config.ps.HasDepthROV();
	pipe.line_width = config.line_expand;
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	if (m_features.texture_barrier)
	{
		if (config.IsFeedbackLoopRT(config.ps))
			pipe.feedback_loop_flags |= FeedbackLoopFlag_ReadAndWriteRT;

		if (config.IsFeedbackLoopDepth(config.ps))
			pipe.feedback_loop_flags |= FeedbackLoopFlag_ReadAndWriteDepth;
	}
	// With framebuffer fetch, an RT-reading shader (IsFeedbackLoopRT: tex_is_fb / fbmask / date >= 5 /
	// sw_blend) reads the render target via subpassLoad, which requires it bound as an input attachment -
	// i.e. an RT feedback loop. DetermineBarriers clears the barrier flags for framebuffer fetch (the read
	// is coherent), so the barrier-gated block above skips these draws and the input attachment is never
	// bound. Wire the RT feedback loop here so IsRTFeedbackLoop() drives the input-attachment binding,
	// feedback render pass and rasterization-order blend flag. Only the subpassLoad path needs this; the
	// feedback-loop-layout path samples a texture and is handled elsewhere.
	if (m_features.framebuffer_fetch && !UseFeedbackLoopLayout() && config.IsFeedbackLoopRT(config.ps))
		pipe.feedback_loop_flags |= FeedbackLoopFlag_ReadAndWriteRT;
	if (pipe.ds && !(pipe.feedback_loop_flags & FeedbackLoopFlag_ReadAndWriteDepth))
	{
		pipe.feedback_loop_flags |= (config.tex && config.tex == config.ds) ? FeedbackLoopFlag_ReadDepth : FeedbackLoopFlag_None;
	}

	// enable point size in the vertex shader if we're rendering points regardless of upscaling.
	pipe.vs.point_size |= (config.topology == GSHWDrawConfig::Topology::Point);
}

void GSDeviceVK::UploadHWDrawVerticesAndIndices(GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts, GetVertexAlignment(config.vs.expand));

	if (config.vs.UseFixedExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer);
	}
	else if (config.vs.UseVSExpandIndexBuffer())
	{
		VSSetIndexBuffer(config.indices, config.nindices);
		SetVSConstantBuffer(config.cb_vs);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}

	if (config.vs.tile_prim_ord && config.tile_payload_size > 0)
	{
		// Tile per-primitive planes: stream the payload into the same buffer the
		// expansion path reads as a storage buffer (set 0, binding 2). The two
		// stages declare different blocks over it, which is legal because they are
		// separate modules — a sprite draw has the vertex stage reading vertices
		// there and the fragment stage reading planes, off one descriptor.
		//
		// The descriptor spans the whole buffer, so only the uvec4 element base
		// travels — through the PS constant buffer, re-cached here because the
		// caller stamped its copy before this offset existed. The vertex shader
		// derives the primitive ordinal from gl_VertexIndex, which includes the
		// draw's base vertex, so that is pushed the way the expansion path does.
		const u32 size = config.tile_payload_size * 16;
		if (!m_vertex_stream_buffer.ReserveMemory(size, 16))
		{
			if (m_is_presenting)
				ExecuteCommandBufferAndRestartPresent(false, "Uploading tile plane payload");
			else
				ExecuteCommandBufferAndRestartRenderPass(false, "Uploading tile plane payload");
			if (!m_vertex_stream_buffer.ReserveMemory(size, 16))
				pxFailRel("Failed to reserve space for tile plane payload");
		}
		const u32 base = m_vertex_stream_buffer.GetCurrentOffset() / 16;
		std::memcpy(m_vertex_stream_buffer.GetCurrentHostPointer(), config.tile_payload, size);
		m_vertex_stream_buffer.CommitMemory(size);
		if (config.tile_zwalk_at != 0xFFFFFFFFu)
		{
			const u32 zbase = base + config.tile_zwalk_at;
			std::memcpy(&config.cb_ps.TileZBase, &zbase, sizeof(zbase));
		}
		if (config.tile_twalk_at != 0xFFFFFFFFu)
		{
			const u32 sbase = base + config.tile_twalk_at;
			std::memcpy(&config.cb_ps.TileTWBase, &sbase, sizeof(sbase));
		}
		if (config.tile_cwalk_at != 0xFFFFFFFFu)
		{
			const u32 cbase = base + config.tile_cwalk_at;
			std::memcpy(&config.cb_ps.TileCBase, &cbase, sizeof(cbase));
		}
		SetPSConstantBuffer(config.cb_ps);
		SetVSPushConstants(static_cast<u32>(m_vertex.start));
	}
}

VkImageMemoryBarrier GSDeviceVK::GetColorBufferFeedbackBarrier(GSTextureVK* rt) const
{
	const VkImageLayout layout =
		UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
	const VkAccessFlags dst_access =
		UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, dst_access, layout, layout,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rt->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
}

VkImageMemoryBarrier GSDeviceVK::GetDepthStencilBufferFeedbackBarrier(GSTextureVK* ds) const
{
	const VkImageLayout layout =
		UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
	const VkAccessFlags dst_access =
		UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, dst_access, layout, layout,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ds->GetImage(),
		{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u}};
}

VkDependencyFlags GSDeviceVK::GetFeedbackBarrierDependencyFlags() const
{
	return UseFeedbackLoopLayout() ? (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
	                                 VK_DEPENDENCY_BY_REGION_BIT;
}

void GSDeviceVK::SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, GSTextureVK* draw_ds,
	bool one_barrier, bool full_barrier)
{
	if (!m_features.texture_barrier) [[unlikely]]
	{
		Draw(config);
		return;
	}

#ifdef PCSX2_DEVBUILD
	if ((one_barrier || full_barrier) && !(config.IsFeedbackLoopRT(m_pipeline_selector.ps) || config.IsFeedbackLoopDepth(m_pipeline_selector.ps))) [[unlikely]]
		Console.Warning("VK: Possible unnecessary barrier detected.");
#endif
	VkDependencyFlags barrier_flags = GetFeedbackBarrierDependencyFlags();

	std::array<VkImageMemoryBarrier, 2> barriers;
	u32 n_barriers = 0;
	if (full_barrier || one_barrier)
	{
		if (draw_rt)
		{
			barriers[0] = GetColorBufferFeedbackBarrier(draw_rt);
			n_barriers++;
		}
		if (draw_ds)
		{
			barriers[1] = GetDepthStencilBufferFeedbackBarrier(draw_ds);
			n_barriers++;
		}
	}

	const auto IssueBarriers = [&]() {
		if (draw_rt)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, barrier_flags, 0, nullptr, 0, nullptr, 1, &barriers[0]);
		}
		if (draw_ds)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, barrier_flags, 0, nullptr, 0, nullptr, 1, &barriers[1]);
		}
	};

	if (full_barrier)
	{
		pxAssert(config.drawlist && !config.drawlist->empty());

		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());

		GL_PUSH("Split the draw");
		g_perfmon.Put(GSPerfMon::Barriers, n_barriers * static_cast<u32>(draw_list_size));

		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
			IssueBarriers();

			const u32 count = config.drawlist->at(n) * indices_per_prim;
			Draw(config, p, count);
			p += count;
		}

		return;
	}

	if (one_barrier)
	{
		g_perfmon.Put(GSPerfMon::Barriers, n_barriers);
		IssueBarriers();
	}

	Draw(config);

	if (config.ps.HasColorROV() || config.ps.HasDepthROV())
		g_perfmon.Put(GSPerfMon::DrawCallsROV, 1);
}
