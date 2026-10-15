// SPDX-FileCopyrightText: 2026 ZeroFG authors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSZeroFG.h"

#include "GS/GS.h"

#include "common/Console.h"

#include <mutex>

#ifdef ARMSX2_HAS_ZEROFG

#include "common/Timer.h"

#include "fmt/format.h"

#include "zerofg/zerofg.h"

#if defined(ARMSX2_USE_ADRENOTOOLS)
#include "adrenotools/driver.h"
#endif

// The presenter side (the standalone's portable host kit), when the engine checkout carries it:
// the build gives this file the kit's include directories then.
#if __has_include("zerofg_host/host.h")
#define ZEROFG_WITH_HOST_KIT 1
#include "zerofg_host/host.h"
#include "xenia/base/frame_stats.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"
#include "xenia/ui/vulkan/zerofg_completion_owner.h"
#include "xenia/ui/vulkan/zerofg_config.h"
#include "xenia/ui/vulkan/zerofg_device_handoff.h"
#include "xenia/ui/vulkan/zerofg_independent_presenter.h"

#include "GS/Renderers/Vulkan/GSZeroFGPostShaders.h"

#include "imgui.h"

#include <android/native_window.h>
#else
#define ZEROFG_WITH_HOST_KIT 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GSZeroFG
{
	namespace
	{
		// The size the engine is first sized for at startup, to prove it builds on this driver
		// before any game frame: a PS2 frame, which the engine's 64-cell grid does not divide, so
		// its padded path is proven too. Game frames resize it to the generation size.
		constexpr u32 PROBE_WIDTH = 640;
		constexpr u32 PROBE_HEIGHT = 448;
		constexpr VkFormat PROBE_FORMAT = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		constexpr u32 FRAME_CONTEXTS = 3;
		// The queues device B asks for on its graphics/compute family: one for generation, one
		// for the egress to present from, as the presenter prefers. Fewer when the family has fewer.
		constexpr u32 WANTED_QUEUES = 2;

		// Device B's entry points, resolved through its own instance and device. The VKLoader
		// globals were loaded for the GS instance and device and are not used for these.
		struct InstanceFunctions
		{
			PFN_vkDestroyInstance DestroyInstance = nullptr;
			PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
			PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
			PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
			PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
			PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
			PFN_vkCreateDevice CreateDevice = nullptr;
			PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
		};

		struct DeviceFunctions
		{
			PFN_vkDestroyDevice DestroyDevice = nullptr;
			PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
			PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
		};

#if ZEROFG_WITH_HOST_KIT
		using xe::ui::vulkan::VulkanDevice;
		using xe::ui::vulkan::VulkanInstance;
		using xe::ui::vulkan::ZeroFGCompletionOwner;
		using xe::ui::vulkan::ZeroFGCompletionOwnerStats;
		using xe::ui::vulkan::ZeroFGDeviceHandoff;
		using xe::ui::vulkan::ZeroFGIndependentPresenter;

		// The game frame's format and layout across the handoff, the presenter and the engine: the
		// guest output convention the presenter assumes.
		constexpr VkFormat FRAME_FORMAT = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		constexpr VkImageLayout FRAME_LAYOUT = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		// The Synthetic cost the presenter assumes before it has measured one (its reference seed).
		constexpr uint64_t SYNTHETIC_COST_SEED_NS = 8000000ull;
		// Presenters replaced after giving the window back, per device, before the GS device keeps it.
		constexpr u32 MAX_FAIL_OPENS = 3;
		// ImGui textures the Post can hold at once (the font atlas, and the atlas it grows into).
		constexpr u32 OVERLAY_TEXTURES = 16;

		uint64_t NowNs()
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
											 .count());
		}

		// Device B's entry points the Post draws with, which the presenter's table does not carry.
		struct PostFunctions
		{
			PFN_vkCreateRenderPass CreateRenderPass = nullptr;
			PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
			PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
			PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
			PFN_vkCreateShaderModule CreateShaderModule = nullptr;
			PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
			PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
			PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
			PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
			PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
			PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
			PFN_vkDestroyPipeline DestroyPipeline = nullptr;
			PFN_vkCreateSampler CreateSampler = nullptr;
			PFN_vkDestroySampler DestroySampler = nullptr;
			PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
			PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
			PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
			PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
			PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
			PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
			PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
			PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
			PFN_vkCmdSetViewport CmdSetViewport = nullptr;
			PFN_vkCmdSetScissor CmdSetScissor = nullptr;
			PFN_vkCmdDraw CmdDraw = nullptr;
			// The overlay (ImGui) draw.
			PFN_vkFreeDescriptorSets FreeDescriptorSets = nullptr;
			PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
			PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
			PFN_vkCmdPushConstants CmdPushConstants = nullptr;
			PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
		};

		/// An image with its own memory and a 2D colour view, on device A or B.
		struct OwnedImage
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkExtent2D extent = {};

			bool Create(const VulkanDevice* device, VkExtent2D size, VkFormat format, VkImageUsageFlags usage)
			{
				VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
				info.imageType = VK_IMAGE_TYPE_2D;
				info.format = format;
				info.extent = {size.width, size.height, 1};
				info.mipLevels = 1;
				info.arrayLayers = 1;
				info.samples = VK_SAMPLE_COUNT_1_BIT;
				info.tiling = VK_IMAGE_TILING_OPTIMAL;
				info.usage = usage;
				info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				if (!xe::ui::vulkan::util::CreateDedicatedAllocationImage(
						device, info, xe::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image, memory))
					return false;
				VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				view_info.image = image;
				view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
				view_info.format = format;
				view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				if (device->functions().vkCreateImageView(device->device(), &view_info, nullptr, &view) != VK_SUCCESS)
				{
					Destroy(device);
					return false;
				}
				extent = size;
				return true;
			}

			void Destroy(const VulkanDevice* device)
			{
				const VulkanDevice::Functions& f = device->functions();
				if (view != VK_NULL_HANDLE)
					f.vkDestroyImageView(device->device(), view, nullptr);
				if (image != VK_NULL_HANDLE)
					f.vkDestroyImage(device->device(), image, nullptr);
				if (memory != VK_NULL_HANDLE)
					f.vkFreeMemory(device->device(), memory, nullptr);
				*this = {};
			}
		};

		/// The Synthetic pool and the command contexts that generate into it, one per engine frame
		/// context. Presenter threads only.
		struct GenerationContext
		{
			static constexpr u32 COUNT = ZeroFGIndependentPresenter::kSyntheticPoolSize;
			struct Command
			{
				VkCommandPool pool = VK_NULL_HANDLE;
				VkCommandBuffer buffer = VK_NULL_HANDLE;
				ZeroFGCompletionOwner completion;
				bool submitted = false;
				uint64_t signal_value = 0;
				uint64_t submit_time_ns = 0;
			};
			std::array<Command, COUNT> commands;
			std::array<OwnedImage, COUNT> outputs;
			std::array<bool, COUNT> output_busy = {};
			std::array<bool, COUNT> output_initialized = {};
			VkImageUsageFlags output_usage = 0;
			VkExtent2D extent = {};
			u32 pool_high_water = 0;
			ZeroFGCompletionOwnerStats owner_stats;
		};

		/// The Post: one job per FinalOutput slot, each copying its frame into an input it samples
		/// into the draw rectangle of the FinalOutput. Presenter threads only.
		struct PostContext
		{
			static constexpr u32 COUNT = ZeroFGIndependentPresenter::kFinalOutputPoolSize;
			struct Job
			{
				VkCommandPool pool = VK_NULL_HANDLE;
				VkCommandBuffer buffer = VK_NULL_HANDLE;
				// Signalled with the job's timeline; exported as the sync file the egress waits on.
				VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
				ZeroFGCompletionOwner completion;
				OwnedImage input;
				bool input_written = false;
				VkDescriptorSet set = VK_NULL_HANDLE;
				VkFramebuffer framebuffer = VK_NULL_HANDLE;
				VkImageView framebuffer_view = VK_NULL_HANDLE;
				VkExtent2D framebuffer_extent = {};
				bool submitted = false;
				uint64_t signal_value = 0;
				// The overlay's vertices, indices and texture uploads for this job, mapped. Rewritten
				// only when the job is recorded again, after its last submission completed.
				VkBuffer overlay_buffer = VK_NULL_HANDLE;
				VkDeviceMemory overlay_memory = VK_NULL_HANDLE;
				uint32_t overlay_memory_type = 0;
				VkDeviceSize overlay_size = 0;
				void* overlay_mapped = nullptr;
			};
			/// One of ImGui's textures on device B, with the descriptor set that samples it.
			struct OverlayImage
			{
				OwnedImage image;
				VkDescriptorSet set = VK_NULL_HANDLE;
				u64 version = 0;
			};
			/// An overlay image no job will sample again, kept until the jobs in flight when it went
			/// (their timeline values here) have completed.
			struct RetiredOverlayImage
			{
				OverlayImage image;
				std::array<uint64_t, COUNT> in_flight = {};
			};
			std::array<Job, COUNT> jobs;
			VkFormat final_format = VK_FORMAT_UNDEFINED;
			VkRenderPass render_pass = VK_NULL_HANDLE;
			VkShaderModule vertex = VK_NULL_HANDLE;
			VkShaderModule fragment = VK_NULL_HANDLE;
			VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
			VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkSampler linear = VK_NULL_HANDLE;
			VkSampler nearest = VK_NULL_HANDLE;
			VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
			// The overlay: ImGui's pipeline over the frame, and its textures. Without them (they
			// failed to build) the Post draws the frame alone.
			VkShaderModule overlay_vertex = VK_NULL_HANDLE;
			VkShaderModule overlay_fragment = VK_NULL_HANDLE;
			VkPipelineLayout overlay_layout = VK_NULL_HANDLE;
			VkPipeline overlay_pipeline = VK_NULL_HANDLE;
			VkDescriptorPool overlay_pool = VK_NULL_HANDLE;
			std::unordered_map<int, OverlayImage> overlay_images;
			std::vector<RetiredOverlayImage> overlay_retired;
			ZeroFGCompletionOwnerStats owner_stats;
		};

		/// One of ImGui's textures as the GS thread copied it, immutable once copied.
		struct OverlayTexture
		{
			int id = 0; ///< ImTextureData::UniqueID
			u64 version = 0; ///< new for every copy
			u32 width = 0;
			u32 height = 0;
			std::vector<u32> pixels; ///< RGBA8, tightly packed
		};

		/// ImGui's draw data of one present (the OSD), for the Post to draw over every output while
		/// device B presents, as the GS present draws it over its own. Immutable once published.
		struct OverlaySnapshot
		{
			struct Command
			{
				float clip[4] = {}; ///< in output pixels: left, top, right, bottom
				int texture = 0; ///< OverlayTexture::id
				u32 first_index = 0;
				s32 vertex_offset = 0;
				u32 index_count = 0;
			};
			std::vector<ImDrawVert> vertices;
			std::vector<ImDrawIdx> indices;
			std::vector<Command> commands;
			std::vector<std::shared_ptr<const OverlayTexture>> textures;
			float origin[2] = {}; ///< ImDrawData::DisplayPos
		};

		/// The game frame on device A in the form the handoff copies from. GS thread only.
		struct SourceStage
		{
			OwnedImage staging;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			bool captured = false;
			u32 mailbox = 0;
			u32 aspect_x = 0;
			u32 aspect_y = 0;
		};
#endif

		struct State
		{
			VkInstance instance = VK_NULL_HANDLE;
			VkPhysicalDevice physical_device = VK_NULL_HANDLE;
			VkDevice device = VK_NULL_HANDLE;
			InstanceFunctions ifn;
			DeviceFunctions dfn;
			// Device B's queue family, its queues in index order, and what the presenter reads of it.
			u32 queue_family = 0;
			std::vector<VkQueue> queues;
			bool display_timing = false;
			VkPhysicalDeviceLimits limits = {};
			std::unique_ptr<zerofg::Interpolator> engine;
			// What the engine was built with: the capabilities enabled on device B, and the mode
			// (GSConfig.ZeroFGMode), so a mode switched in game rebuilds it on the same device.
			zerofg::Capabilities caps;
			zerofg::Mode engine_mode = zerofg::Mode::kZero;
#if ZEROFG_WITH_HOST_KIT
			// Devices A and B as the presenter and the handoff see them. They wrap, never own.
			std::unique_ptr<xe::ui::vulkan::VulkanInstance> kit_instance_a;
			std::unique_ptr<xe::ui::vulkan::VulkanInstance> kit_instance_b;
			std::unique_ptr<xe::ui::vulkan::VulkanDevice> kit_device_a;
			std::unique_ptr<xe::ui::vulkan::VulkanDevice> kit_device_b;
			PostFunctions pfn;
			std::unique_ptr<ZeroFGDeviceHandoff> handoff;
			std::unique_ptr<ZeroFGIndependentPresenter> presenter;
			std::unique_ptr<GenerationContext> generation;
			std::unique_ptr<PostContext> post;
			// Device B's surface on the game's window while the presenter is connected to it.
			VkSurfaceKHR surface = VK_NULL_HANDLE;
			ANativeWindow* window = nullptr;
			// The GS device dropped its swap chain for the presenter on this connection.
			bool released_by_a = false;
			// The presenter gave the window back after a failure. A presenter that failed stays off the
			// window (its Main Surface Authority session refusal), so a new one replaces it, up to
			// MAX_FAIL_OPENS times per device; past that the GS device keeps the window.
			u32 fail_opens = 0;
			bool failed_open = false;
			// The size frames are generated at, fixed for a connection: the presenter treats any change
			// of it as a structural break and fails open. Zero until the first frame of the connection.
			VkExtent2D generation_extent = {};
			float draw_w = 0.0f;
			float draw_h = 0.0f;
			SourceStage source;
#endif
			bool handoff_qualified = false;
			std::string status;
		};

		std::mutex s_mutex;
		State s_state;
		// Set by GSDeviceVK::SelectDeviceExtensions for the device it is creating.
		std::atomic<bool> s_source_handoff{false};
#if ZEROFG_WITH_HOST_KIT
		// The output settings the Post reads, written by the GS thread with each captured frame.
		std::mutex s_paint_mutex;
		xe::ui::Presenter::GuestOutputPaintConfig s_paint_config;
		// The FinalOutput size the presenter was connected with.
		VkExtent2D s_output_extent = {};
		// The overlay the Post draws, the GS thread's latest (CaptureOverlay), and that thread's copies
		// of ImGui's textures by id, kept while ZeroFG is up so a texture made earlier is known.
		std::mutex s_overlay_mutex;
		std::shared_ptr<const OverlaySnapshot> s_overlay;
		std::unordered_map<int, std::shared_ptr<const OverlayTexture>> s_overlay_textures;
		// The ids whose last copy was of a change the GS device had not applied yet.
		std::unordered_set<int> s_overlay_pending;
		u64 s_overlay_version = 0;
		// The outputs the Post sent to the window, Real and Synthetic (presenter thread), for the
		// overlay's ZeroFG line (GetOverlayText, GS thread).
		std::atomic<u64> s_outputs_real{0};
		std::atomic<u64> s_outputs_synthetic{0};
#endif

		template <typename T>
		T InstanceProc(VkInstance instance, const char* name)
		{
			return reinterpret_cast<T>(vkGetInstanceProcAddr(instance, name));
		}

		std::string VersionString(u32 version)
		{
			return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
				VK_API_VERSION_PATCH(version));
		}

		const char* StatusName(zerofg::Status status)
		{
			switch (status)
			{
				case zerofg::Status::kSuccess: return "success";
				case zerofg::Status::kInvalidArgument: return "invalid argument";
				case zerofg::Status::kUnsupported: return "unsupported";
				case zerofg::Status::kOutOfMemory: return "out of memory";
				case zerofg::Status::kVulkanError: return "Vulkan error";
				default: return "unknown";
			}
		}

		bool HasExtension(const std::vector<VkExtensionProperties>& extensions, std::string_view name)
		{
			return std::any_of(extensions.begin(), extensions.end(),
				[name](const VkExtensionProperties& e) { return name == e.extensionName; });
		}

#if ZEROFG_WITH_HOST_KIT
		void TeardownHostLocked();
#endif

		void DestroyLocked()
		{
			State& s = s_state;
#if ZEROFG_WITH_HOST_KIT
			// The presenter, its contexts, the handoff and the staging on device A go first: all of
			// them use the devices below.
			TeardownHostLocked();
			s.fail_opens = 0;
			{
				std::unique_lock overlay_lock(s_overlay_mutex);
				s_overlay.reset();
			}
			// A new device starts ImGui's textures over.
			s_overlay_textures.clear();
			s_overlay_pending.clear();
			s.failed_open = false;
			s.kit_device_a.reset();
			s.kit_device_b.reset();
			s.kit_instance_a.reset();
			s.kit_instance_b.reset();
#endif
			s.handoff_qualified = false;
			s.queues.clear();
			if (s.device != VK_NULL_HANDLE)
			{
				// The engine's resources belong to device B: nothing of it may be in flight.
				s.dfn.DeviceWaitIdle(s.device);
				s.engine.reset();
				s.dfn.DestroyDevice(s.device, nullptr);
			}
			s.engine.reset();
			// The instance is the GS device's (UseSourceInstanceLocked): it stays.
			s.instance = VK_NULL_HANDLE;
			s.physical_device = VK_NULL_HANDLE;
			s.device = VK_NULL_HANDLE;
			s.ifn = {};
			s.dfn = {};
		}

		/// Device B is created from the GS device's instance and GPU: it presents
		/// on the GS device's surface, and Android connects a window to one surface at a time, so a
		/// second instance could never reach the window. GSDeviceVK created that instance at 1.3 for it.
		bool UseSourceInstanceLocked(const SourceDevice& source, std::string* why)
		{
			State& s = s_state;
			if (!Vulkan::IsVulkanLibraryLoaded() || !vkGetInstanceProcAddr || source.instance == VK_NULL_HANDLE ||
				source.physical_device == VK_NULL_HANDLE)
			{
				*why = "no Vulkan instance";
				return false;
			}
			if (source.instance_api_version < VK_API_VERSION_1_3)
			{
				*why = fmt::format("the Vulkan instance is {}, ZeroFG needs 1.3", VersionString(source.instance_api_version));
				return false;
			}
			s.instance = source.instance;
			s.physical_device = source.physical_device;

			InstanceFunctions& f = s.ifn;
#define ZEROFG_INSTANCE_PROC(member, name) f.member = InstanceProc<PFN_##name>(s.instance, #name)
			ZEROFG_INSTANCE_PROC(DestroyInstance, vkDestroyInstance);
			ZEROFG_INSTANCE_PROC(EnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
			ZEROFG_INSTANCE_PROC(GetPhysicalDeviceProperties2, vkGetPhysicalDeviceProperties2);
			ZEROFG_INSTANCE_PROC(GetPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2);
			ZEROFG_INSTANCE_PROC(GetPhysicalDeviceQueueFamilyProperties, vkGetPhysicalDeviceQueueFamilyProperties);
			ZEROFG_INSTANCE_PROC(EnumerateDeviceExtensionProperties, vkEnumerateDeviceExtensionProperties);
			ZEROFG_INSTANCE_PROC(CreateDevice, vkCreateDevice);
			ZEROFG_INSTANCE_PROC(GetDeviceProcAddr, vkGetDeviceProcAddr);
#undef ZEROFG_INSTANCE_PROC
			if (!f.GetPhysicalDeviceProperties2 || !f.GetPhysicalDeviceFeatures2 ||
				!f.GetPhysicalDeviceQueueFamilyProperties || !f.EnumerateDeviceExtensionProperties || !f.CreateDevice ||
				!f.GetDeviceProcAddr)
			{
				*why = "the instance lacks a core entry point";
				return false;
			}
			return true;
		}

		bool CreateDeviceLocked(zerofg::Capabilities* caps, std::string* summary, std::string* why)
		{
			State& s = s_state;

			VkPhysicalDeviceVulkan13Properties properties13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
			VkPhysicalDeviceVulkan12Properties properties12 = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &properties13};
			VkPhysicalDeviceVulkan11Properties properties11 = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &properties12};
			VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &properties11};
			s.ifn.GetPhysicalDeviceProperties2(s.physical_device, &properties);
			const u32 api_version = properties.properties.apiVersion;
			if (api_version < VK_API_VERSION_1_3)
			{
				*why = fmt::format("the GPU's driver is Vulkan {}, ZeroFG needs 1.3", VersionString(api_version));
				return false;
			}

			VkPhysicalDeviceVulkan13Features supported13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
			VkPhysicalDeviceVulkan12Features supported12 = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &supported13};
			VkPhysicalDeviceVulkan11Features supported11 = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &supported12};
			VkPhysicalDeviceFeatures2 supported = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported11};
			s.ifn.GetPhysicalDeviceFeatures2(s.physical_device, &supported);

			// The floor: synchronization2 for the engine, timeline semaphores for the presenter's
			// completion tracking, and storage images of the formats the engine writes.
			if (!supported13.synchronization2 || !supported12.timelineSemaphore ||
				!supported.features.shaderStorageImageExtendedFormats)
			{
				*why = fmt::format("the GPU lacks a required feature (synchronization2={} timelineSemaphore={} "
								   "shaderStorageImageExtendedFormats={})",
					supported13.synchronization2, supported12.timelineSemaphore,
					supported.features.shaderStorageImageExtendedFormats);
				return false;
			}

			std::vector<VkExtensionProperties> extensions;
			u32 count = 0;
			if (s.ifn.EnumerateDeviceExtensionProperties(s.physical_device, nullptr, &count, nullptr) == VK_SUCCESS)
			{
				extensions.resize(count);
				if (s.ifn.EnumerateDeviceExtensionProperties(s.physical_device, nullptr, &count, extensions.data()) !=
					VK_SUCCESS)
					extensions.clear();
				extensions.resize(std::min<size_t>(count, extensions.size()));
			}

			// Required: presenting on the game's surface, and receiving the game's frames as
			// AHardwareBuffers with sync files.
			std::vector<const char*> enabled_extensions;
			for (const char* name : {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
					 VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
					 VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})
			{
				if (!HasExtension(extensions, name))
				{
					*why = fmt::format("the GPU has no {}", name);
					return false;
				}
				enabled_extensions.push_back(name);
			}
			// Optional: the foreign queue family the AHardwareBuffer import names, the display's
			// own record of when frames reached it, and the hardware cubic filter the engine's
			// Modern form uses for its final colour.
			const bool queue_family_foreign = HasExtension(extensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
			if (queue_family_foreign)
				enabled_extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
			const bool display_timing = HasExtension(extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
			if (display_timing)
				enabled_extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
			bool filter_cubic = false;
			if (HasExtension(extensions, VK_EXT_FILTER_CUBIC_EXTENSION_NAME))
			{
				enabled_extensions.push_back(VK_EXT_FILTER_CUBIC_EXTENSION_NAME);
				filter_cubic = true;
			}
			else if (HasExtension(extensions, VK_IMG_FILTER_CUBIC_EXTENSION_NAME))
			{
				enabled_extensions.push_back(VK_IMG_FILTER_CUBIC_EXTENSION_NAME);
				filter_cubic = true;
			}
			// Device B's work must not wait behind the game's on a saturated GPU: its queues ask the
			// driver for a high global priority (the GS device's stay at the default), for drivers
			// that honour it. Turnip's KGSL backend does not; the adrenotools hook below covers it.
			const char* global_priority = nullptr;
			if (HasExtension(extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME))
				global_priority = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
			else if (HasExtension(extensions, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME))
				global_priority = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
			if (global_priority)
				enabled_extensions.push_back(global_priority);

			u32 family_count = 0;
			s.ifn.GetPhysicalDeviceQueueFamilyProperties(s.physical_device, &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			s.ifn.GetPhysicalDeviceQueueFamilyProperties(s.physical_device, &family_count, families.data());
			u32 family = family_count;
			for (u32 i = 0; i < family_count; i++)
			{
				constexpr VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
				if ((families[i].queueFlags & wanted) == wanted && families[i].queueCount > 0)
				{
					family = i;
					break;
				}
			}
			if (family == family_count)
			{
				*why = "the GPU has no graphics and compute queue";
				return false;
			}
			const u32 queue_count = std::min(WANTED_QUEUES, families[family].queueCount);
			const std::array<float, WANTED_QUEUES> priorities = {1.0f, 1.0f};
			VkDeviceQueueGlobalPriorityCreateInfoKHR global_priority_info = {
				VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
			global_priority_info.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
			VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			queue_info.pNext = global_priority ? &global_priority_info : nullptr;
			queue_info.queueFamilyIndex = family;
			queue_info.queueCount = queue_count;
			queue_info.pQueuePriorities = priorities.data();

			// Everything the engine and the presenter use that the GPU has, and nothing else.
			VkPhysicalDeviceVulkan13Features enabled13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
			enabled13.synchronization2 = VK_TRUE;
			enabled13.subgroupSizeControl = supported13.subgroupSizeControl;
			enabled13.computeFullSubgroups = supported13.computeFullSubgroups;
			enabled13.maintenance4 = supported13.maintenance4;
			VkPhysicalDeviceVulkan12Features enabled12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &enabled13};
			enabled12.timelineSemaphore = VK_TRUE;
			enabled12.shaderFloat16 = supported12.shaderFloat16;
			VkPhysicalDeviceVulkan11Features enabled11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &enabled12};
			enabled11.shaderDrawParameters = supported11.shaderDrawParameters;
			VkPhysicalDeviceFeatures2 enabled = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &enabled11};
			enabled.features.shaderStorageImageExtendedFormats = VK_TRUE;
			enabled.features.shaderInt16 = supported.features.shaderInt16;

			VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
			device_info.queueCreateInfoCount = 1;
			device_info.pQueueCreateInfos = &queue_info;
			device_info.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
			device_info.ppEnabledExtensionNames = enabled_extensions.data();
#if defined(ARMSX2_USE_ADRENOTOOLS)
			// Device B asks KGSL for a higher priority than the game's (KGSL's default is 8): on a
			// saturated GPU its small work then runs first instead of waiting behind the game's frames
			// and completing in clumps (a copy of 8 ms took 40-170 ms in another emulator). Turnip
			// creates its KGSL draw contexts without one, so the adrenotools hook (loaded with the
			// custom driver while ZeroFG is on, VKLoader) sets it during this call. 4 is the second of
			// KGSL's four levels: above the game, below the top level.
			constexpr u32 KGSL_CONTEXT_PRIORITY = 4;
			const u32 kgsl_raised_before = adrenotools_context_priority_raised();
			adrenotools_set_context_priority(KGSL_CONTEXT_PRIORITY);
#endif
			VkResult result = s.ifn.CreateDevice(s.physical_device, &device_info, nullptr, &s.device);
			const char* global_priority_state = global_priority ? "high" : "unsupported";
			if (global_priority && (result == VK_ERROR_NOT_PERMITTED_KHR || result == VK_ERROR_INITIALIZATION_FAILED))
			{
				// A driver may refuse a high priority to an application: device B at the default
				// priority is still device B.
				global_priority_state = "refused";
				queue_info.pNext = nullptr;
				result = s.ifn.CreateDevice(s.physical_device, &device_info, nullptr, &s.device);
			}
#if defined(ARMSX2_USE_ADRENOTOOLS)
			adrenotools_set_context_priority(0);
			const u32 kgsl_raised = adrenotools_context_priority_raised() - kgsl_raised_before;
#else
			const u32 kgsl_raised = 0;
#endif
			if (result != VK_SUCCESS)
			{
				s.device = VK_NULL_HANDLE;
				*why = fmt::format("vkCreateDevice failed ({})", static_cast<int>(result));
				return false;
			}
			s.dfn.DestroyDevice =
				reinterpret_cast<PFN_vkDestroyDevice>(s.ifn.GetDeviceProcAddr(s.device, "vkDestroyDevice"));
			s.dfn.DeviceWaitIdle =
				reinterpret_cast<PFN_vkDeviceWaitIdle>(s.ifn.GetDeviceProcAddr(s.device, "vkDeviceWaitIdle"));
			s.dfn.GetDeviceQueue =
				reinterpret_cast<PFN_vkGetDeviceQueue>(s.ifn.GetDeviceProcAddr(s.device, "vkGetDeviceQueue"));
			if (!s.dfn.DestroyDevice || !s.dfn.DeviceWaitIdle || !s.dfn.GetDeviceQueue)
			{
				*why = "the device lacks a core entry point";
				if (s.dfn.DestroyDevice)
					s.dfn.DestroyDevice(s.device, nullptr);
				s.device = VK_NULL_HANDLE;
				return false;
			}
			s.queue_family = family;
			for (u32 i = 0; i < queue_count; i++)
			{
				VkQueue queue = VK_NULL_HANDLE;
				s.dfn.GetDeviceQueue(s.device, family, i, &queue);
				s.queues.push_back(queue);
			}
			s.display_timing = display_timing;
			s.limits = properties.properties.limits;

			// What is enabled on device B, which is all the engine may use.
			caps->effective_api_version = VK_API_VERSION_1_3;
			caps->synchronization2_enabled = true;
			caps->shader_storage_image_extended_formats_enabled = true;
			caps->shader_float16_supported = supported12.shaderFloat16;
			caps->shader_float16_enabled = enabled12.shaderFloat16;
			caps->shader_int16_supported = supported.features.shaderInt16;
			caps->shader_int16_enabled = enabled.features.shaderInt16;
			caps->subgroup_size = properties11.subgroupSize;
			caps->subgroup_min_size = properties13.minSubgroupSize;
			caps->subgroup_max_size = properties13.maxSubgroupSize;
			caps->max_compute_workgroup_subgroups = properties13.maxComputeWorkgroupSubgroups;
			caps->subgroup_supported_stages = properties11.subgroupSupportedStages;
			caps->subgroup_supported_operations = properties11.subgroupSupportedOperations;
			caps->subgroup_size_control_supported = supported13.subgroupSizeControl;
			caps->subgroup_size_control_enabled = enabled13.subgroupSizeControl;
			caps->compute_full_subgroups_supported = supported13.computeFullSubgroups;
			caps->compute_full_subgroups_enabled = enabled13.computeFullSubgroups;
			caps->required_subgroup_size_stages = properties13.requiredSubgroupSizeStages;
			caps->maintenance4_supported = supported13.maintenance4;
			caps->maintenance4_enabled = enabled13.maintenance4;
			caps->filter_cubic_supported = filter_cubic;
			caps->filter_cubic_enabled = filter_cubic;

			*summary = fmt::format("'{}' Vulkan {} driver '{}' ({}), {} queue(s) on family {}, float16={} int16={} "
								   "subgroup={} ({}..{}) cubic={} display_timing={} queue_family_foreign={} "
								   "global_priority={} kgsl_contexts_raised={}",
				properties.properties.deviceName, VersionString(api_version), properties12.driverName,
				properties12.driverInfo, queue_count, family, caps->shader_float16_enabled, caps->shader_int16_enabled,
				caps->subgroup_size, caps->subgroup_min_size, caps->subgroup_max_size, filter_cubic, display_timing,
				queue_family_foreign, global_priority_state, kgsl_raised);
			return true;
		}

		/// The engine on device B, built for a PS2-sized frame. Auto takes the fast paths device B
		/// has enabled; when it is refused, the portable Compat form gets one try, as the
		/// integration guide says.
		/// The engine the user picked (GSConfig.ZeroFGMode).
		zerofg::Mode ConfiguredMode()
		{
			return GSConfig.ZeroFGMode == 1 ? zerofg::Mode::kReallyZero : zerofg::Mode::kZero;
		}

		const char* ModeName(zerofg::Mode mode)
		{
			return mode == zerofg::Mode::kReallyZero ? "ReallyZero" : "Zero";
		}

		bool CreateEngineLocked(const zerofg::Capabilities& caps, std::string* backend, std::string* why)
		{
			State& s = s_state;
			zerofg::CreateInfo info;
			info.vulkan.instance = s.instance;
			info.vulkan.physical_device = s.physical_device;
			info.vulkan.device = s.device;
			info.vulkan.get_instance_proc_addr = vkGetInstanceProcAddr;
			info.mode = ConfiguredMode();
			info.capabilities = caps;
			s.caps = caps;
			s.engine_mode = info.mode;
			info.frame_context_count = FRAME_CONTEXTS;

			zerofg::Status status = zerofg::Status::kUnsupported;
			for (const zerofg::Backend attempt : {zerofg::Backend::kAuto, zerofg::Backend::kCompat})
			{
				info.backend = attempt;
				s.engine = zerofg::Interpolator::Create(info, &status);
				if (s.engine)
					status = s.engine->Resize(PROBE_WIDTH, PROBE_HEIGHT, PROBE_FORMAT, PROBE_FORMAT);
				if (s.engine && status == zerofg::Status::kSuccess)
				{
					*backend = attempt == zerofg::Backend::kAuto ? "auto" : "compat";
					return true;
				}
				// The engine may hold pipelines device B is still building against.
				s.dfn.DeviceWaitIdle(s.device);
				s.engine.reset();
				WARNING_LOG("ZeroFG: the {} engine failed: {}", attempt == zerofg::Backend::kAuto ? "auto" : "compat",
					StatusName(status));
			}
			*why = fmt::format("the engine failed to start ({})", StatusName(status));
			return false;
		}

#if ZEROFG_WITH_HOST_KIT
		/// The presenter's log lines, into the emulator's log.
		void KitLog(zerofg_host::LogLevel level, std::string_view message)
		{
			switch (level)
			{
				case zerofg_host::LogLevel::kError: ERROR_LOG("{}", message); break;
				case zerofg_host::LogLevel::kWarning: WARNING_LOG("{}", message); break;
				// Its periodic summaries are for whoever debugs it: a development build or verbose
				// logging. Its warnings and errors always reach the log.
				default: DEV_LOG("{}", message); break;
			}
		}

		/// Devices A and B as the presenter and the handoff see them, then the handoff's startup
		/// self-test: a pattern copied on device A into an AHardwareBuffer, read back on device B,
		/// checked texel for texel, at two sizes. Runs on the GS thread before it submits anything,
		/// so nothing else is using device A's queue meanwhile.
		bool QualifyHandoffLocked(const SourceDevice& source, std::string* why)
		{
			using xe::ui::vulkan::VulkanDevice;
			using xe::ui::vulkan::VulkanInstance;
			State& s = s_state;
			if (!s_source_handoff.load(std::memory_order_acquire))
			{
				*why = "the GS device lacks the handoff extensions";
				return false;
			}
			zerofg_host::SetLogSink(KitLog);

			s.kit_instance_b = VulkanInstance::Wrap(s.instance, vkGetInstanceProcAddr, VK_API_VERSION_1_3, false);
			VulkanDevice::Description b;
			b.instance = s.kit_instance_b.get();
			b.physical_device = s.physical_device;
			b.device = s.device;
			b.queue_family = s.queue_family;
			b.queues = s.queues;
			b.properties.synchronization2 = true;
			b.properties.timelineSemaphore = true;
			b.properties.maxImageDimension2D = s.limits.maxImageDimension2D;
			b.properties.maxFramebufferWidth = s.limits.maxFramebufferWidth;
			b.properties.maxFramebufferHeight = s.limits.maxFramebufferHeight;
			b.extensions.ext_KHR_swapchain = true;
			b.extensions.ext_ANDROID_external_memory_android_hardware_buffer = true;
			b.extensions.ext_KHR_external_semaphore_fd = true;
			b.extensions.ext_GOOGLE_display_timing = s.display_timing;
			if (s.kit_instance_b)
				s.kit_device_b = VulkanDevice::Wrap(b);

			// The GS instance is 1.1; synchronization2 is the extension GSDeviceVK enabled for this.
			s.kit_instance_a =
				VulkanInstance::Wrap(source.instance, vkGetInstanceProcAddr, VK_API_VERSION_1_1, false);
			VulkanDevice::Description a;
			a.instance = s.kit_instance_a.get();
			a.physical_device = source.physical_device;
			a.device = source.device;
			a.queue_family = source.queue_family;
			a.queues = {source.queue};
			a.properties.synchronization2 = true;
			a.extensions.ext_KHR_swapchain = true;
			a.extensions.ext_ANDROID_external_memory_android_hardware_buffer = true;
			a.extensions.ext_KHR_external_semaphore_fd = true;
			if (s.kit_instance_a)
				s.kit_device_a = VulkanDevice::Wrap(a);

			if (!s.kit_device_a || !s.kit_device_b)
			{
				*why = "a device does not have what the presenter needs (see the lines above)";
				return false;
			}

			// The handoff lives on: the presenter consumes it once a window is connected and it is armed.
			s.handoff = std::make_unique<ZeroFGDeviceHandoff>(s.kit_device_a.get(), s.kit_device_b.get());
			if (!s.handoff->SelfTest())
			{
				s.kit_device_a->vkDeviceWaitIdle()(source.device);
				s.dfn.DeviceWaitIdle(s.device);
				s.handoff->DestroyAfterIdle();
				s.handoff.reset();
				*why = "the handoff self-test failed (see the ZeroFGDeviceSplit lines above)";
				return false;
			}
			// The GS thread runs a frame or two ahead of the GS device's GPU. With the handoff's default
			// depth of one waiting publication, every Publish held it to the GPU's completion of the
			// previous frame's copy; two give it back the frame of lead it has without ZeroFG.
			s.handoff->SetPublishDepth(2);
			return true;
		}

		// ---- Generation: the engine on device B, between two Reals the presenter holds --------------

		void DestroyGeneration()
		{
			State& s = s_state;
			if (!s.generation || !s.kit_device_b)
				return;
			GenerationContext& context = *s.generation;
			const VulkanDevice* const device = s.kit_device_b.get();
			for (OwnedImage& output : context.outputs)
				output.Destroy(device);
			for (GenerationContext::Command& command : context.commands)
			{
				if (command.pool != VK_NULL_HANDLE)
					device->functions().vkDestroyCommandPool(device->device(), command.pool, nullptr);
				command.completion.DestroySyncFd(device);
				command.completion.DestroyTimeline(device);
			}
			s.generation.reset();
		}

		/// The command contexts, each with its own completion timeline and sync file. The Synthetic
		/// images come with the first request, which says their size.
		bool PrepareGeneration()
		{
			State& s = s_state;
			if (s.generation)
				return true;
			const VulkanDevice* const device = s.kit_device_b.get();
			const VulkanDevice::Functions& f = device->functions();
			auto context = std::make_unique<GenerationContext>();
			for (GenerationContext::Command& command : context->commands)
			{
				VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
				pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
				pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
				VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				allocate.commandBufferCount = 1;
				if (f.vkCreateCommandPool(device->device(), &pool_info, nullptr, &command.pool) != VK_SUCCESS ||
					(allocate.commandPool = command.pool,
						f.vkAllocateCommandBuffers(device->device(), &allocate, &command.buffer) != VK_SUCCESS) ||
					command.completion.CreateSyncFd(device, "ZeroFG Generation sync_fd") != VK_SUCCESS ||
					command.completion.CreateTimeline(device, "ZeroFG Generation completion") != VK_SUCCESS)
				{
					s.generation = std::move(context);
					DestroyGeneration();
					return false;
				}
			}
			// The engine writes its output directly when the format can be a storage image here, and
			// blits it in otherwise; both need these usages.
			VkFormatProperties properties = {};
			device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
				device->physical_device(), FRAME_FORMAT, &properties);
			context->output_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
									VK_IMAGE_USAGE_SAMPLED_BIT;
			if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
				context->output_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			s.generation = std::move(context);
			return true;
		}

		bool ProcessGeneration(const ZeroFGIndependentPresenter::GenerationRequest& request,
			ZeroFGIndependentPresenter::GenerationResult& result)
		{
			using FailureStage = ZeroFGIndependentPresenter::GenerationFailureStage;
			State& s = s_state;
			result = {};
			const uint64_t begin_ns = NowNs();
			const auto fail = [&result, begin_ns](FailureStage stage) {
				result.failure_stage = stage;
				result.total_ns = NowNs() - begin_ns;
				return false;
			};
			const auto fail_vk = [&result, begin_ns](FailureStage stage, VkResult vk_result) {
				if (vk_result == VK_ERROR_DEVICE_LOST)
				{
					s_state.kit_device_b->SetLost();
					ERROR_LOG("ZeroFGDeviceB lost operation=Generation stage={}", static_cast<u32>(stage));
				}
				result.failure_stage = stage;
				result.vk_result = vk_result;
				result.vk_result_valid = true;
				result.total_ns = NowNs() - begin_ns;
				return false;
			};

			bool waits_valid = request.input_wait_count && request.input_wait_count <= request.input_wait_semaphores.size();
			for (u32 i = 0; waits_valid && i < request.input_wait_count; i++)
				waits_valid = request.input_wait_semaphores[i] != VK_NULL_HANDLE && request.input_wait_values[i];
			if (waits_valid && request.input_wait_count == 2)
				waits_valid = request.input_wait_semaphores[0] != request.input_wait_semaphores[1];
			if (request.previous_image == VK_NULL_HANDLE || request.previous_view == VK_NULL_HANDLE ||
				request.current_image == VK_NULL_HANDLE || request.current_view == VK_NULL_HANDLE ||
				!request.extent.width || !request.extent.height || !request.source_physical_extent.width ||
				!request.source_physical_extent.height ||
				request.source_physical_extent.width < request.extent.width ||
				request.source_physical_extent.height < request.extent.height || !waits_valid ||
				!request.previous_source_id || request.current_source_id <= request.previous_source_id || !s.engine)
				return fail(FailureStage::kRequestValidation);

			const VulkanDevice* const device = s.kit_device_b.get();
			const VulkanDevice::Functions& f = device->functions();
			if (!s.generation)
			{
				// Built on the presenter's first request; this one is declined while that happens.
				if (!PrepareGeneration())
					return fail(FailureStage::kAdapter);
				return fail(FailureStage::kPreparationYield);
			}
			GenerationContext& context = *s.generation;

			// A new picture size: the engine and the Synthetic pool follow it, between Generations.
			if (context.extent.width != request.extent.width || context.extent.height != request.extent.height)
			{
				if (std::any_of(context.output_busy.begin(), context.output_busy.end(), [](bool b) { return b; }))
					return fail(FailureStage::kResize);
				const zerofg::Status status =
					s.engine->Resize(request.extent.width, request.extent.height, FRAME_FORMAT, FRAME_FORMAT);
				INFO_LOG("ZeroFGGeneration resize picture={}x{} status={}", request.extent.width,
					request.extent.height, static_cast<u32>(status));
				if (status != zerofg::Status::kSuccess)
				{
					result.zerofg_status = static_cast<u32>(status);
					return fail(FailureStage::kResize);
				}
				for (OwnedImage& output : context.outputs)
				{
					output.Destroy(device);
					if (!output.Create(device, request.extent, FRAME_FORMAT, context.output_usage))
					{
						for (OwnedImage& o : context.outputs)
							o.Destroy(device);
						return fail(FailureStage::kResize);
					}
				}
				context.output_initialized = {};
				context.extent = request.extent;
				return fail(FailureStage::kPreparationYield);
			}

			u32 selected = GenerationContext::COUNT;
			for (u32 i = 0; i < GenerationContext::COUNT; i++)
			{
				if (!context.output_busy[i])
				{
					context.output_busy[i] = true;
					selected = i;
					break;
				}
			}
			if (selected == GenerationContext::COUNT)
				return fail(FailureStage::kSyntheticPool);
			const auto release_before_submit = [&context, selected]() { context.output_busy[selected] = false; };
			u32 occupancy = 0;
			for (bool busy : context.output_busy)
				occupancy += busy;
			context.pool_high_water = std::max(context.pool_high_water, occupancy);
			result.pool_occupancy = occupancy;
			result.pool_high_water = context.pool_high_water;

			GenerationContext::Command& command = context.commands[selected];
			VkResult vk_result = f.vkResetCommandPool(device->device(), command.pool, 0);
			if (vk_result != VK_SUCCESS)
			{
				release_before_submit();
				return fail_vk(FailureStage::kCommandReset, vk_result);
			}
			VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vk_result = f.vkBeginCommandBuffer(command.buffer, &begin);
			if (vk_result != VK_SUCCESS)
			{
				release_before_submit();
				return fail_vk(FailureStage::kCommandBegin, vk_result);
			}

			// The two Reals are only read, in their own layouts, behind the presenter's timeline waits.
			const VkImageLayout real_layouts[2] = {request.previous_layout, request.current_layout};
			const VkImage real_images[2] = {request.previous_image, request.current_image};
			VkImageMemoryBarrier reals[2] = {};
			for (u32 i = 0; i < 2; i++)
			{
				reals[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				reals[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				reals[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				reals[i].oldLayout = real_layouts[i];
				reals[i].newLayout = real_layouts[i];
				reals[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				reals[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				reals[i].image = real_images[i];
				reals[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			}
			f.vkCmdPipelineBarrier(command.buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, reals);
			if (!context.output_initialized[selected])
			{
				VkImageMemoryBarrier initialize = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				initialize.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				initialize.newLayout = FRAME_LAYOUT;
				initialize.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				initialize.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				initialize.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				initialize.image = context.outputs[selected].image;
				initialize.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				f.vkCmdPipelineBarrier(command.buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &initialize);
			}

			const auto wrap_real = [&request](VkImage image, VkImageView view, VkImageLayout layout, uint64_t sequence) {
				zerofg::Image wrapped = {};
				wrapped.image = image;
				wrapped.view = view;
				wrapped.layout = layout;
				wrapped.sequence = sequence;
				wrapped.format = FRAME_FORMAT;
				wrapped.width = request.source_physical_extent.width;
				wrapped.height = request.source_physical_extent.height;
				wrapped.usage =
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
				wrapped.creation_facts_known = true;
				wrapped.active_rect = {0, 0, request.extent.width, request.extent.height};
				return wrapped;
			};
			zerofg::Image output = {};
			output.image = context.outputs[selected].image;
			output.view = context.outputs[selected].view;
			output.layout = FRAME_LAYOUT;
			output.format = FRAME_FORMAT;
			output.width = request.extent.width;
			output.height = request.extent.height;
			output.usage = context.output_usage;
			output.creation_facts_known = true;
			output.active_rect = {0, 0, request.extent.width, request.extent.height};
			const zerofg::Status status = s.engine->Interpolate(command.buffer, selected,
				wrap_real(request.previous_image, request.previous_view, request.previous_layout,
					request.previous_source_id),
				wrap_real(request.current_image, request.current_view, request.current_layout, request.current_source_id),
				0.5f, output);
			if (status != zerofg::Status::kSuccess)
			{
				f.vkResetCommandPool(device->device(), command.pool, 0);
				release_before_submit();
				result.zerofg_status = static_cast<u32>(status);
				return fail(FailureStage::kRecord);
			}
			f.vkCmdPipelineBarrier(command.buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, reals);
			vk_result = f.vkEndCommandBuffer(command.buffer);
			if (vk_result != VK_SUCCESS)
			{
				release_before_submit();
				return fail_vk(FailureStage::kCommandEnd, vk_result);
			}

			// Completion: the context's own timeline, plus a sync file the presenter polls.
			const VkSemaphore timeline = command.completion.timeline();
			const uint64_t signal_value = command.completion.ClaimTimelineValue();
			if (command.completion.occupied())
				++context.owner_stats.reuse_before_retire;
			if (command.completion.sync_fd_recreate_pending())
				command.completion.RecreateSyncFd(device, "ZeroFG Generation sync_fd");
			const bool signal_sync_fd = command.completion.can_signal_sync_fd();
			const VkSemaphore signal_semaphores[2] = {timeline, command.completion.sync_semaphore()};
			const uint64_t signal_values[2] = {signal_value, 0};
			const VkPipelineStageFlags wait_stages[2] = {
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
			VkTimelineSemaphoreSubmitInfo timeline_submit = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
			timeline_submit.waitSemaphoreValueCount = request.input_wait_count;
			timeline_submit.pWaitSemaphoreValues = request.input_wait_values.data();
			timeline_submit.signalSemaphoreValueCount = signal_sync_fd ? 2 : 1;
			timeline_submit.pSignalSemaphoreValues = signal_values;
			VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline_submit};
			submit.waitSemaphoreCount = request.input_wait_count;
			submit.pWaitSemaphores = request.input_wait_semaphores.data();
			submit.pWaitDstStageMask = wait_stages;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &command.buffer;
			submit.signalSemaphoreCount = signal_sync_fd ? 2 : 1;
			submit.pSignalSemaphores = signal_semaphores;
			const uint64_t queue_request_ns = NowNs();
			{
				const u32 family = device->queue_family_graphics_compute();
				const u32 index = device->queue_index_zerofg_presenter();
				std::optional<VulkanDevice::Queue::Acquisition> queue = device->TryAcquireQueue(family, index);
				while (!queue && request.queue_commit_allowed && request.queue_commit_allowed())
				{
					std::this_thread::yield();
					queue = device->TryAcquireQueue(family, index);
				}
				if (queue && request.queue_commit_allowed && !request.queue_commit_allowed())
					queue.reset();
				if (!queue || s.kit_device_b->RejectZeroFGSubmitAfterTeardownIdle())
				{
					result.queue_wait_ns = NowNs() - queue_request_ns;
					f.vkResetCommandPool(device->device(), command.pool, 0);
					release_before_submit();
					return fail(FailureStage::kQueueBusy);
				}
				const uint64_t submit_begin_ns = NowNs();
				result.queue_wait_ns = submit_begin_ns - queue_request_ns;
				vk_result = f.vkQueueSubmit(queue->queue(), 1, &submit, VK_NULL_HANDLE);
				result.submit_host_ns = NowNs() - submit_begin_ns;
			}
			if (vk_result != VK_SUCCESS)
			{
				release_before_submit();
				return fail_vk(FailureStage::kQueueSubmit, vk_result);
			}
			result.submission_accepted = true;
			command.signal_value = signal_value;
			command.submit_time_ns = NowNs();
			command.submitted = true;
			command.completion.MarkSubmitted(timeline);
			context.owner_stats.CountSubmit(true, result.submit_host_ns);
			command.completion.CloseSyncFd();
			if (signal_sync_fd)
			{
				result.completion_fd_export_attempted = true;
				result.completion_fd_exported = command.completion.ExportSyncFd(device);
				result.completion_fd_fallback = !result.completion_fd_exported;
			}
			else
			{
				command.completion.SetSyncFdFallback();
				result.completion_fd_fallback = true;
			}
			context.output_initialized[selected] = true;
			result.image = context.outputs[selected].image;
			result.view = context.outputs[selected].view;
			result.extent = context.extent;
			result.synthetic_index = selected;
			result.signal_value = signal_value;
			result.completion_semaphore = timeline;
			result.completion_per_context = true;
			result.completion_owner_stats = context.owner_stats;
			result.submit_time_ns = command.submit_time_ns;
			result.total_ns = NowNs() - begin_ns;
			return true;
		}

		/// Whether a Generation finished, by its sync file: never a wait, never a timeline query on
		/// device B (a zero-timeout wait can block forever on KGSL).
		bool PollGeneration(uint32_t index, ZeroFGIndependentPresenter::GenerationResult& result, bool& ready)
		{
			using FailureStage = ZeroFGIndependentPresenter::GenerationFailureStage;
			State& s = s_state;
			result = {};
			ready = false;
			if (!s.generation || index >= GenerationContext::COUNT)
			{
				result.failure_stage = FailureStage::kRequestValidation;
				return false;
			}
			GenerationContext& context = *s.generation;
			GenerationContext::Command& command = context.commands[index];
			if (!command.submitted || !command.signal_value)
			{
				result.failure_stage = FailureStage::kRequestValidation;
				return false;
			}
			bool complete = false;
			bool observed = false;
			if (s.kit_device_b->zerofg_teardown_idle())
			{
				complete = observed = true;
			}
			else if (command.completion.sync_fd_observable())
			{
				const ZeroFGCompletionOwner::SyncFdPoll poll = command.completion.PollSyncFd();
				if (poll == ZeroFGCompletionOwner::SyncFdPoll::kError)
				{
					++context.owner_stats.sync_fd_errors;
					command.completion.AbandonSyncFd();
				}
				else
				{
					result.completion_fd_polled = true;
					observed = true;
					complete = poll == ZeroFGCompletionOwner::SyncFdPoll::kSignaled;
				}
			}
			if (!observed)
			{
				// The fail-open drain owns retirement then.
				result.failure_stage = FailureStage::kTimelineQuery;
				result.submission_accepted = true;
				return false;
			}
			result.completion_owner_stats = context.owner_stats;
			if (!complete)
				return true;
			command.completion.CloseSyncFd();
			command.completion.MarkRetired();
			ready = true;
			result.submission_accepted = true;
			result.signal_value = command.signal_value;
			result.submit_time_ns = command.submit_time_ns;
			result.submit_to_ready_ns = NowNs() - command.submit_time_ns;
			result.gpu_completion_ns = result.submit_to_ready_ns;
			result.image = context.outputs[index].image;
			result.view = context.outputs[index].view;
			result.extent = context.extent;
			result.synthetic_index = index;
			command.submitted = false;
			return true;
		}

		void ReleaseSynthetic(uint32_t index)
		{
			State& s = s_state;
			if (!s.generation || index >= GenerationContext::COUNT)
				return;
			GenerationContext& context = *s.generation;
			GenerationContext::Command& command = context.commands[index];
			command.submitted = false;
			if (command.completion.occupied() && command.completion.sync_fd_observable())
			{
				++context.owner_stats.release_checks;
				const ZeroFGCompletionOwner::SyncFdPoll poll = command.completion.PollSyncFd();
				if (poll == ZeroFGCompletionOwner::SyncFdPoll::kPending)
					++context.owner_stats.release_unretired;
				else if (poll == ZeroFGCompletionOwner::SyncFdPoll::kError)
				{
					++context.owner_stats.sync_fd_errors;
					command.completion.AbandonSyncFd();
				}
			}
			command.completion.RetireSyncFd();
			command.completion.MarkRetired();
			context.output_busy[index] = false;
		}

		// ---- Post: a frame into the draw rectangle of a FinalOutput, on device B ---------------------

		void DestroyOverlayImage(PostContext& context, PostContext::OverlayImage& image)
		{
			State& s = s_state;
			const VulkanDevice* const device = s.kit_device_b.get();
			if (image.set != VK_NULL_HANDLE && context.overlay_pool != VK_NULL_HANDLE)
				s.pfn.FreeDescriptorSets(device->device(), context.overlay_pool, 1, &image.set);
			image.image.Destroy(device);
			image = {};
		}

		void DestroyOverlayBuffer(PostContext::Job& job)
		{
			const VulkanDevice* const device = s_state.kit_device_b.get();
			const VulkanDevice::Functions& f = device->functions();
			if (job.overlay_mapped)
				f.vkUnmapMemory(device->device(), job.overlay_memory);
			if (job.overlay_buffer != VK_NULL_HANDLE)
				f.vkDestroyBuffer(device->device(), job.overlay_buffer, nullptr);
			if (job.overlay_memory != VK_NULL_HANDLE)
				f.vkFreeMemory(device->device(), job.overlay_memory, nullptr);
			job.overlay_buffer = VK_NULL_HANDLE;
			job.overlay_memory = VK_NULL_HANDLE;
			job.overlay_mapped = nullptr;
			job.overlay_size = 0;
		}

		void DestroyPost()
		{
			State& s = s_state;
			if (!s.post || !s.kit_device_b)
				return;
			PostContext& context = *s.post;
			const VulkanDevice* const device = s.kit_device_b.get();
			const VkDevice d = device->device();
			const VulkanDevice::Functions& f = device->functions();
			const PostFunctions& p = s.pfn;
			// The device drained before the Post goes (DisconnectSurfaceLocked, TeardownHostLocked).
			for (auto& [id, image] : context.overlay_images)
				DestroyOverlayImage(context, image);
			for (PostContext::RetiredOverlayImage& retired : context.overlay_retired)
				DestroyOverlayImage(context, retired.image);
			if (context.overlay_pipeline != VK_NULL_HANDLE)
				p.DestroyPipeline(d, context.overlay_pipeline, nullptr);
			if (context.overlay_layout != VK_NULL_HANDLE)
				p.DestroyPipelineLayout(d, context.overlay_layout, nullptr);
			if (context.overlay_pool != VK_NULL_HANDLE)
				p.DestroyDescriptorPool(d, context.overlay_pool, nullptr);
			if (context.overlay_vertex != VK_NULL_HANDLE)
				p.DestroyShaderModule(d, context.overlay_vertex, nullptr);
			if (context.overlay_fragment != VK_NULL_HANDLE)
				p.DestroyShaderModule(d, context.overlay_fragment, nullptr);
			for (PostContext::Job& job : context.jobs)
			{
				if (job.framebuffer != VK_NULL_HANDLE)
					p.DestroyFramebuffer(d, job.framebuffer, nullptr);
				DestroyOverlayBuffer(job);
				job.input.Destroy(device);
				if (job.acquire_semaphore != VK_NULL_HANDLE)
					f.vkDestroySemaphore(d, job.acquire_semaphore, nullptr);
				job.completion.DestroyTimeline(device);
				if (job.pool != VK_NULL_HANDLE)
					f.vkDestroyCommandPool(d, job.pool, nullptr);
			}
			if (context.pipeline != VK_NULL_HANDLE)
				p.DestroyPipeline(d, context.pipeline, nullptr);
			if (context.pipeline_layout != VK_NULL_HANDLE)
				p.DestroyPipelineLayout(d, context.pipeline_layout, nullptr);
			if (context.descriptor_pool != VK_NULL_HANDLE)
				p.DestroyDescriptorPool(d, context.descriptor_pool, nullptr);
			if (context.set_layout != VK_NULL_HANDLE)
				p.DestroyDescriptorSetLayout(d, context.set_layout, nullptr);
			if (context.linear != VK_NULL_HANDLE)
				p.DestroySampler(d, context.linear, nullptr);
			if (context.nearest != VK_NULL_HANDLE)
				p.DestroySampler(d, context.nearest, nullptr);
			if (context.vertex != VK_NULL_HANDLE)
				p.DestroyShaderModule(d, context.vertex, nullptr);
			if (context.fragment != VK_NULL_HANDLE)
				p.DestroyShaderModule(d, context.fragment, nullptr);
			if (context.render_pass != VK_NULL_HANDLE)
				p.DestroyRenderPass(d, context.render_pass, nullptr);
			s.post.reset();
		}

		bool PrepareOverlay(PostContext& context);

		/// The render pass (cleared, ending ready for the egress copy), the pipeline drawing one
		/// triangle over the viewport, the samplers and one descriptor set and command context per job.
		bool PreparePost(VkFormat final_format)
		{
			State& s = s_state;
			if (s.post && s.post->final_format == final_format)
				return true;
			DestroyPost();
			const VulkanDevice* const device = s.kit_device_b.get();
			const VkDevice d = device->device();
			const VulkanDevice::Functions& f = device->functions();
			const PostFunctions& p = s.pfn;
			s.post = std::make_unique<PostContext>();
			PostContext& context = *s.post;
			context.final_format = final_format;

			VkAttachmentDescription attachment = {};
			attachment.format = final_format;
			attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			// The egress copies the FinalOutput into the swap chain image.
			attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			VkSubpassDescription subpass = {};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &color;
			VkSubpassDependency dependencies[2] = {};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			VkRenderPassCreateInfo render_pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
			render_pass_info.attachmentCount = 1;
			render_pass_info.pAttachments = &attachment;
			render_pass_info.subpassCount = 1;
			render_pass_info.pSubpasses = &subpass;
			render_pass_info.dependencyCount = 2;
			render_pass_info.pDependencies = dependencies;

			VkShaderModuleCreateInfo vertex_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			vertex_info.codeSize = sizeof(zerofg_post_vert_spv);
			vertex_info.pCode = zerofg_post_vert_spv;
			VkShaderModuleCreateInfo fragment_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			fragment_info.codeSize = sizeof(zerofg_post_frag_spv);
			fragment_info.pCode = zerofg_post_frag_spv;

			const VkDescriptorSetLayoutBinding binding = {
				0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo set_layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			set_layout_info.bindingCount = 1;
			set_layout_info.pBindings = &binding;

			VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sampler_info.maxLod = 0.0f;

			const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PostContext::COUNT};
			VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool_info.maxSets = PostContext::COUNT;
			pool_info.poolSizeCount = 1;
			pool_info.pPoolSizes = &pool_size;

			bool ok = p.CreateRenderPass(d, &render_pass_info, nullptr, &context.render_pass) == VK_SUCCESS &&
					  p.CreateShaderModule(d, &vertex_info, nullptr, &context.vertex) == VK_SUCCESS &&
					  p.CreateShaderModule(d, &fragment_info, nullptr, &context.fragment) == VK_SUCCESS &&
					  p.CreateDescriptorSetLayout(d, &set_layout_info, nullptr, &context.set_layout) == VK_SUCCESS;
			if (ok)
			{
				sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
				ok = p.CreateSampler(d, &sampler_info, nullptr, &context.linear) == VK_SUCCESS;
				sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
				ok = ok && p.CreateSampler(d, &sampler_info, nullptr, &context.nearest) == VK_SUCCESS;
			}
			if (ok)
			{
				VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
				layout_info.setLayoutCount = 1;
				layout_info.pSetLayouts = &context.set_layout;
				ok = p.CreatePipelineLayout(d, &layout_info, nullptr, &context.pipeline_layout) == VK_SUCCESS &&
					 p.CreateDescriptorPool(d, &pool_info, nullptr, &context.descriptor_pool) == VK_SUCCESS;
			}
			if (ok)
			{
				VkPipelineShaderStageCreateInfo stages[2] = {};
				stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
				stages[0].module = context.vertex;
				stages[0].pName = "main";
				stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				stages[1].module = context.fragment;
				stages[1].pName = "main";
				VkPipelineVertexInputStateCreateInfo vertex_input = {
					VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
				VkPipelineInputAssemblyStateCreateInfo input_assembly = {
					VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
				input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
				viewport.viewportCount = 1;
				viewport.scissorCount = 1;
				VkPipelineRasterizationStateCreateInfo rasterization = {
					VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
				rasterization.polygonMode = VK_POLYGON_MODE_FILL;
				rasterization.cullMode = VK_CULL_MODE_NONE;
				rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
				rasterization.lineWidth = 1.0f;
				VkPipelineMultisampleStateCreateInfo multisample = {
					VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
				multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
				VkPipelineColorBlendAttachmentState blend_attachment = {};
				blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
												  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
				VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
				blend.attachmentCount = 1;
				blend.pAttachments = &blend_attachment;
				const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
				VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
				dynamic.dynamicStateCount = 2;
				dynamic.pDynamicStates = dynamic_states;
				VkGraphicsPipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
				pipeline_info.stageCount = 2;
				pipeline_info.pStages = stages;
				pipeline_info.pVertexInputState = &vertex_input;
				pipeline_info.pInputAssemblyState = &input_assembly;
				pipeline_info.pViewportState = &viewport;
				pipeline_info.pRasterizationState = &rasterization;
				pipeline_info.pMultisampleState = &multisample;
				pipeline_info.pColorBlendState = &blend;
				pipeline_info.pDynamicState = &dynamic;
				pipeline_info.layout = context.pipeline_layout;
				pipeline_info.renderPass = context.render_pass;
				ok = p.CreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &context.pipeline) ==
					 VK_SUCCESS;
			}
			for (PostContext::Job& job : context.jobs)
			{
				if (!ok)
					break;
				VkCommandPoolCreateInfo command_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
				command_pool_info.flags =
					VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
				command_pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
				VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				allocate.commandBufferCount = 1;
				VkExportSemaphoreCreateInfo export_info = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
				export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
				VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &export_info};
				VkDescriptorSetAllocateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
				set_info.descriptorPool = context.descriptor_pool;
				set_info.descriptorSetCount = 1;
				set_info.pSetLayouts = &context.set_layout;
				ok = f.vkCreateCommandPool(d, &command_pool_info, nullptr, &job.pool) == VK_SUCCESS &&
					 (allocate.commandPool = job.pool,
						 f.vkAllocateCommandBuffers(d, &allocate, &job.buffer) == VK_SUCCESS) &&
					 f.vkCreateSemaphore(d, &semaphore_info, nullptr, &job.acquire_semaphore) == VK_SUCCESS &&
					 job.completion.CreateTimeline(device, "ZeroFG Post completion") == VK_SUCCESS &&
					 p.AllocateDescriptorSets(d, &set_info, &job.set) == VK_SUCCESS;
			}
			if (!ok)
			{
				ERROR_LOG("ZeroFG: the Post could not be built for format {}", static_cast<int>(final_format));
				DestroyPost();
				return false;
			}
			const bool overlay = PrepareOverlay(context);
			INFO_LOG("ZeroFG: Post built for format {}{}", static_cast<int>(final_format),
				overlay ? ", with the overlay" : ", without the overlay (it could not be built)");
			return true;
		}

		/// ImGui's pipeline in the Post's render pass: PCSX2's imgui.glsl and blend, its textures
		/// sampled through the Post's set layout, the scale and translation as push constants.
		bool PrepareOverlay(PostContext& context)
		{
			State& s = s_state;
			const VkDevice d = s.kit_device_b->device();
			const PostFunctions& p = s.pfn;
			VkShaderModuleCreateInfo vertex_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			vertex_info.codeSize = sizeof(zerofg_overlay_vert_spv);
			vertex_info.pCode = zerofg_overlay_vert_spv;
			VkShaderModuleCreateInfo fragment_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			fragment_info.codeSize = sizeof(zerofg_overlay_frag_spv);
			fragment_info.pCode = zerofg_overlay_frag_spv;
			const VkPushConstantRange push_range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
			VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout_info.setLayoutCount = 1;
			layout_info.pSetLayouts = &context.set_layout;
			layout_info.pushConstantRangeCount = 1;
			layout_info.pPushConstantRanges = &push_range;
			const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, OVERLAY_TEXTURES};
			VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			pool_info.maxSets = OVERLAY_TEXTURES;
			pool_info.poolSizeCount = 1;
			pool_info.pPoolSizes = &pool_size;
			if (p.CreateShaderModule(d, &vertex_info, nullptr, &context.overlay_vertex) != VK_SUCCESS ||
				p.CreateShaderModule(d, &fragment_info, nullptr, &context.overlay_fragment) != VK_SUCCESS ||
				p.CreatePipelineLayout(d, &layout_info, nullptr, &context.overlay_layout) != VK_SUCCESS ||
				p.CreateDescriptorPool(d, &pool_info, nullptr, &context.overlay_pool) != VK_SUCCESS)
				return false;

			VkPipelineShaderStageCreateInfo stages[2] = {};
			stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			stages[0].module = context.overlay_vertex;
			stages[0].pName = "main";
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[1].module = context.overlay_fragment;
			stages[1].pName = "main";
			const VkVertexInputBindingDescription binding = {0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
			const VkVertexInputAttributeDescription attributes[3] = {
				{0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(ImDrawVert, pos))},
				{1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(ImDrawVert, uv))},
				{2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<u32>(offsetof(ImDrawVert, col))},
			};
			VkPipelineVertexInputStateCreateInfo vertex_input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
			vertex_input.vertexBindingDescriptionCount = 1;
			vertex_input.pVertexBindingDescriptions = &binding;
			vertex_input.vertexAttributeDescriptionCount = 3;
			vertex_input.pVertexAttributeDescriptions = attributes;
			VkPipelineInputAssemblyStateCreateInfo input_assembly = {
				VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
			input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
			viewport.viewportCount = 1;
			viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo rasterization = {
				VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
			rasterization.polygonMode = VK_POLYGON_MODE_FILL;
			rasterization.cullMode = VK_CULL_MODE_NONE;
			rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			rasterization.lineWidth = 1.0f;
			VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
			multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			// The GS device's ImGui blend (GSDeviceVK::CreateImGuiPipeline).
			VkPipelineColorBlendAttachmentState blend_attachment = {};
			blend_attachment.blendEnable = VK_TRUE;
			blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
			blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
			blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
											  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
			blend.attachmentCount = 1;
			blend.pAttachments = &blend_attachment;
			const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
			VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
			dynamic.dynamicStateCount = 2;
			dynamic.pDynamicStates = dynamic_states;
			VkGraphicsPipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			pipeline_info.stageCount = 2;
			pipeline_info.pStages = stages;
			pipeline_info.pVertexInputState = &vertex_input;
			pipeline_info.pInputAssemblyState = &input_assembly;
			pipeline_info.pViewportState = &viewport;
			pipeline_info.pRasterizationState = &rasterization;
			pipeline_info.pMultisampleState = &multisample;
			pipeline_info.pColorBlendState = &blend;
			pipeline_info.pDynamicState = &dynamic;
			pipeline_info.layout = context.overlay_layout;
			pipeline_info.renderPass = context.render_pass;
			return p.CreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &context.overlay_pipeline) ==
				   VK_SUCCESS;
		}

		/// Destroys the retired overlay images whose last users have completed. [current] is the job
		/// being recorded: its previous submission is complete.
		void RetireOverlayImages(PostContext& context, u32 current)
		{
			std::erase_if(context.overlay_retired, [&context, current](PostContext::RetiredOverlayImage& retired) {
				for (u32 i = 0; i < PostContext::COUNT; ++i)
				{
					const PostContext::Job& job = context.jobs[i];
					if (i != current && retired.in_flight[i] && job.submitted && job.signal_value == retired.in_flight[i])
						return false;
				}
				DestroyOverlayImage(context, retired.image);
				return true;
			});
		}

		void RetireOverlayImage(PostContext& context, PostContext::OverlayImage&& image, u32 current)
		{
			PostContext::RetiredOverlayImage retired;
			retired.image = std::move(image);
			for (u32 i = 0; i < PostContext::COUNT; ++i)
			{
				const PostContext::Job& job = context.jobs[i];
				retired.in_flight[i] = i != current && job.submitted ? job.signal_value : 0;
			}
			context.overlay_retired.push_back(std::move(retired));
		}

		/// Writes this job's overlay into its buffer: the vertices, the indices, and the pixels of the
		/// textures B lacks or has older copies of, whose uploads it records into [cmd] (outside any
		/// render pass). False draws no overlay on this output. Presenter thread.
		bool StageOverlay(PostContext& context, u32 index, VkCommandBuffer cmd, const OverlaySnapshot& overlay,
			VkDeviceSize* index_offset)
		{
			State& s = s_state;
			const VulkanDevice* const device = s.kit_device_b.get();
			const VkDevice d = device->device();
			const VulkanDevice::Functions& f = device->functions();
			const PostFunctions& p = s.pfn;
			PostContext::Job& job = context.jobs[index];
			RetireOverlayImages(context, index);

			// Images of textures ImGui let go of are retired; the ones that changed, re-uploaded.
			for (auto it = context.overlay_images.begin(); it != context.overlay_images.end();)
			{
				const bool kept = std::any_of(overlay.textures.begin(), overlay.textures.end(),
					[id = it->first](const auto& texture) { return texture->id == id; });
				if (kept)
				{
					++it;
					continue;
				}
				RetireOverlayImage(context, std::move(it->second), index);
				it = context.overlay_images.erase(it);
			}
			std::vector<const OverlayTexture*> uploads;
			for (const auto& texture : overlay.textures)
			{
				auto it = context.overlay_images.find(texture->id);
				if (it != context.overlay_images.end() && it->second.version == texture->version)
					continue;
				if (it != context.overlay_images.end() && (it->second.image.extent.width != texture->width ||
															  it->second.image.extent.height != texture->height))
				{
					// A new size: the old image may still be sampled by jobs in flight.
					RetireOverlayImage(context, std::move(it->second), index);
					context.overlay_images.erase(it);
					it = context.overlay_images.end();
				}
				if (it == context.overlay_images.end())
				{
					PostContext::OverlayImage image;
					VkDescriptorSetAllocateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
					set_info.descriptorPool = context.overlay_pool;
					set_info.descriptorSetCount = 1;
					set_info.pSetLayouts = &context.set_layout;
					if (!image.image.Create(device, {texture->width, texture->height}, VK_FORMAT_R8G8B8A8_UNORM,
							VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
						p.AllocateDescriptorSets(d, &set_info, &image.set) != VK_SUCCESS)
					{
						DestroyOverlayImage(context, image);
						continue;
					}
					VkDescriptorImageInfo image_info = {};
					image_info.sampler = context.linear;
					image_info.imageView = image.image.view;
					image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					write.dstSet = image.set;
					write.descriptorCount = 1;
					write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					write.pImageInfo = &image_info;
					p.UpdateDescriptorSets(d, 1, &write, 0, nullptr);
					it = context.overlay_images.emplace(texture->id, std::move(image)).first;
				}
				uploads.push_back(texture.get());
			}

			// The buffer: vertices, indices, then the pixels to upload.
			const VkDeviceSize vertex_bytes = overlay.vertices.size() * sizeof(ImDrawVert);
			const VkDeviceSize index_bytes = overlay.indices.size() * sizeof(ImDrawIdx);
			*index_offset = vertex_bytes;
			VkDeviceSize size = (vertex_bytes + index_bytes + 15) & ~VkDeviceSize(15);
			const VkDeviceSize pixels_offset = size;
			for (const OverlayTexture* texture : uploads)
				size += texture->pixels.size() * sizeof(u32);
			if (job.overlay_size < size)
			{
				DestroyOverlayBuffer(job);
				const VkDeviceSize allocate = std::max<VkDeviceSize>(size + size / 2, 64 * 1024);
				if (!xe::ui::vulkan::util::CreateDedicatedAllocationBuffer(device, allocate,
						VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
							VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
						xe::ui::vulkan::util::MemoryPurpose::kUpload, job.overlay_buffer, job.overlay_memory,
						&job.overlay_memory_type) ||
					f.vkMapMemory(d, job.overlay_memory, 0, VK_WHOLE_SIZE, 0, &job.overlay_mapped) != VK_SUCCESS)
				{
					DestroyOverlayBuffer(job);
					return false;
				}
				job.overlay_size = allocate;
			}
			u8* const mapped = static_cast<u8*>(job.overlay_mapped);
			std::memcpy(mapped, overlay.vertices.data(), vertex_bytes);
			std::memcpy(mapped + vertex_bytes, overlay.indices.data(), index_bytes);
			VkDeviceSize offset = pixels_offset;
			for (const OverlayTexture* texture : uploads)
			{
				std::memcpy(mapped + offset, texture->pixels.data(), texture->pixels.size() * sizeof(u32));
				offset += texture->pixels.size() * sizeof(u32);
			}
			xe::ui::vulkan::util::FlushMappedMemoryRange(device, job.overlay_memory, job.overlay_memory_type);

			// The uploads, whole textures. Same queue as every Post: the barrier waits for the earlier
			// Posts still sampling the image, and makes the copy visible to the later ones.
			offset = pixels_offset;
			for (const OverlayTexture* texture : uploads)
			{
				PostContext::OverlayImage& image = context.overlay_images[texture->id];
				VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = image.image.image;
				barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
					nullptr, 0, nullptr, 1, &barrier);
				VkBufferImageCopy copy = {};
				copy.bufferOffset = offset;
				copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				copy.imageExtent = {texture->width, texture->height, 1};
				f.vkCmdCopyBufferToImage(
					cmd, job.overlay_buffer, image.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
					nullptr, 0, nullptr, 1, &barrier);
				image.version = texture->version;
				offset += texture->pixels.size() * sizeof(u32);
			}
			return true;
		}

		/// Draws the staged overlay over the whole output, inside the Post's render pass.
		void DrawOverlay(const PostContext& context, const PostContext::Job& job, VkCommandBuffer cmd,
			const OverlaySnapshot& overlay, VkDeviceSize index_offset, VkExtent2D extent)
		{
			const PostFunctions& p = s_state.pfn;
			const float out_w = static_cast<float>(extent.width);
			const float out_h = static_cast<float>(extent.height);
			const VkViewport viewport = {0.0f, 0.0f, out_w, out_h, 0.0f, 1.0f};
			p.CmdSetViewport(cmd, 0, 1, &viewport);
			p.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.overlay_pipeline);
			const VkDeviceSize vertex_offset = 0;
			p.CmdBindVertexBuffers(cmd, 0, 1, &job.overlay_buffer, &vertex_offset);
			p.CmdBindIndexBuffer(cmd, job.overlay_buffer, index_offset,
				sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
			// ImGui's pixels are the output's (the GS device's swap chain is the window's size, as the
			// FinalOutput is), offset by the display origin.
			const float transform[4] = {2.0f / out_w, 2.0f / out_h, -1.0f - overlay.origin[0] * 2.0f / out_w,
				-1.0f - overlay.origin[1] * 2.0f / out_h};
			p.CmdPushConstants(cmd, context.overlay_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), transform);
			VkDescriptorSet bound = VK_NULL_HANDLE;
			for (const OverlaySnapshot::Command& command : overlay.commands)
			{
				const auto it = context.overlay_images.find(command.texture);
				if (it == context.overlay_images.end() || !it->second.version)
					continue;
				const float x0 = std::clamp(command.clip[0], 0.0f, out_w);
				const float y0 = std::clamp(command.clip[1], 0.0f, out_h);
				const float x1 = std::clamp(command.clip[2], 0.0f, out_w);
				const float y1 = std::clamp(command.clip[3], 0.0f, out_h);
				if (x1 <= x0 || y1 <= y0)
					continue;
				const VkRect2D scissor = {{static_cast<s32>(x0), static_cast<s32>(y0)},
					{static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}};
				p.CmdSetScissor(cmd, 0, 1, &scissor);
				if (it->second.set != bound)
				{
					bound = it->second.set;
					p.CmdBindDescriptorSets(
						cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.overlay_layout, 0, 1, &bound, 0, nullptr);
				}
				p.CmdDrawIndexed(cmd, command.index_count, 1, command.first_index, command.vertex_offset, 0);
			}
		}

		bool ProcessPost(const ZeroFGIndependentPresenter::PostProcessRequest& request,
			ZeroFGIndependentPresenter::PostProcessResult& result)
		{
			using Stage = ZeroFGIndependentPresenter::PostFailureStage;
			State& s = s_state;
			result = {};
			const auto fail = [&result](Stage stage) {
				result.failure_stage = stage;
				return false;
			};
			const auto fail_vk = [&result](Stage stage, VkResult vk_result) {
				if (vk_result == VK_ERROR_DEVICE_LOST)
				{
					s_state.kit_device_b->SetLost();
					ERROR_LOG("ZeroFGDeviceB lost operation=Post stage={}", static_cast<u32>(stage));
				}
				result.failure_stage = stage;
				result.vk_result = vk_result;
				result.vk_result_valid = true;
				return false;
			};
			if (request.candidate_image == VK_NULL_HANDLE || request.candidate_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
				request.final_output_image == VK_NULL_HANDLE || request.final_output_view == VK_NULL_HANDLE ||
				!request.final_output_extent.width || !request.final_output_extent.height ||
				request.candidate_image == request.final_output_image ||
				request.final_output_index >= PostContext::COUNT ||
				((request.candidate_wait_semaphore == VK_NULL_HANDLE) != (request.candidate_wait_value == 0)) ||
				request.final_output_format == VK_FORMAT_UNDEFINED)
				return fail(Stage::kRequestValidation);
			if (!PreparePost(request.final_output_format))
				return fail(Stage::kFinalPipeline);

			const VulkanDevice* const device = s.kit_device_b.get();
			const VkDevice d = device->device();
			const VulkanDevice::Functions& f = device->functions();
			const PostFunctions& p = s.pfn;
			PostContext& context = *s.post;
			PostContext::Job& job = context.jobs[request.final_output_index];

			// The frame's own pixels: the content of the candidate, which may sit in a larger image.
			VkExtent2D content = {request.frontbuffer_width, request.frontbuffer_height};
			if (!content.width || !content.height)
				content = request.candidate_storage_extent;
			content.width = std::min(content.width, request.candidate_storage_extent.width);
			content.height = std::min(content.height, request.candidate_storage_extent.height);
			if (!content.width || !content.height)
				return fail(Stage::kLogicalInput);
			if (job.input.extent.width != content.width || job.input.extent.height != content.height)
			{
				job.input.Destroy(device);
				job.input_written = false;
				if (!job.input.Create(device, content, FRAME_FORMAT,
						VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
					return fail(Stage::kIntermediateResource);
			}
			if (job.framebuffer_view != request.final_output_view ||
				job.framebuffer_extent.width != request.final_output_extent.width ||
				job.framebuffer_extent.height != request.final_output_extent.height)
			{
				if (job.framebuffer != VK_NULL_HANDLE)
					p.DestroyFramebuffer(d, job.framebuffer, nullptr);
				job.framebuffer = VK_NULL_HANDLE;
				job.framebuffer_view = VK_NULL_HANDLE;
				VkFramebufferCreateInfo framebuffer_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
				framebuffer_info.renderPass = context.render_pass;
				framebuffer_info.attachmentCount = 1;
				framebuffer_info.pAttachments = &request.final_output_view;
				framebuffer_info.width = request.final_output_extent.width;
				framebuffer_info.height = request.final_output_extent.height;
				framebuffer_info.layers = 1;
				const VkResult framebuffer_result = p.CreateFramebuffer(d, &framebuffer_info, nullptr, &job.framebuffer);
				if (framebuffer_result != VK_SUCCESS)
					return fail_vk(Stage::kFinalFramebuffer, framebuffer_result);
				job.framebuffer_view = request.final_output_view;
				job.framebuffer_extent = request.final_output_extent;
			}

			// Where the frame goes: the draw rectangle the GS present uses, in FinalOutput pixels.
			const std::array<float, 4> rect = request.config.draw_rect;
			const float out_w = static_cast<float>(request.final_output_extent.width);
			const float out_h = static_cast<float>(request.final_output_extent.height);
			float x0 = std::clamp(rect[0], 0.0f, out_w);
			float y0 = std::clamp(rect[1], 0.0f, out_h);
			float x1 = std::clamp(rect[2], 0.0f, out_w);
			float y1 = std::clamp(rect[3], 0.0f, out_h);
			if (x1 - x0 < 1.0f || y1 - y0 < 1.0f)
			{
				x0 = 0.0f;
				y0 = 0.0f;
				x1 = out_w;
				y1 = out_h;
			}

			VkDescriptorImageInfo image_info = {};
			image_info.sampler = request.config.linear_filter ? context.linear : context.nearest;
			image_info.imageView = job.input.view;
			image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = job.set;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &image_info;
			p.UpdateDescriptorSets(d, 1, &write, 0, nullptr);

			VkResult vk_result = f.vkResetCommandPool(d, job.pool, 0);
			if (vk_result != VK_SUCCESS)
				return fail_vk(Stage::kCommandReset, vk_result);
			VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vk_result = f.vkBeginCommandBuffer(job.buffer, &begin);
			if (vk_result != VK_SUCCESS)
				return fail_vk(Stage::kCommandBegin, vk_result);

			// The candidate is copied out (behind the submit's wait on it), the input takes the copy.
			VkImageMemoryBarrier acquire[2] = {};
			for (VkImageMemoryBarrier& barrier : acquire)
			{
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			}
			acquire[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			acquire[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			acquire[0].oldLayout = request.candidate_layout;
			acquire[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			acquire[0].image = request.candidate_image;
			acquire[1].srcAccessMask = 0;
			acquire[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			acquire[1].oldLayout =
				job.input_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
			acquire[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			acquire[1].image = job.input.image;
			f.vkCmdPipelineBarrier(job.buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
				nullptr, 0, nullptr, 2, acquire);
			VkImageCopy copy = {};
			copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.extent = {content.width, content.height, 1};
			f.vkCmdCopyImage(job.buffer, request.candidate_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, job.input.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			VkImageMemoryBarrier ready[2] = {};
			for (VkImageMemoryBarrier& barrier : ready)
			{
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			}
			ready[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			ready[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			ready[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			ready[0].newLayout = request.candidate_layout;
			ready[0].image = request.candidate_image;
			ready[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			ready[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			ready[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			ready[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			ready[1].image = job.input.image;
			f.vkCmdPipelineBarrier(job.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2,
				ready);

			// The overlay of the latest GS present, the same over Real and Synthetic outputs.
			std::shared_ptr<const OverlaySnapshot> overlay;
			if (context.overlay_pipeline != VK_NULL_HANDLE)
			{
				std::unique_lock overlay_lock(s_overlay_mutex);
				overlay = s_overlay;
			}
			VkDeviceSize overlay_index_offset = 0;
			if (overlay && (overlay->commands.empty() ||
							   !StageOverlay(context, request.final_output_index, job.buffer, *overlay, &overlay_index_offset)))
				overlay.reset();

			// The bars around the draw rectangle are black, as the GS present leaves them.
			VkClearValue black = {};
			black.color.float32[3] = 1.0f;
			VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			pass.renderPass = context.render_pass;
			pass.framebuffer = job.framebuffer;
			pass.renderArea = {{0, 0}, request.final_output_extent};
			pass.clearValueCount = 1;
			pass.pClearValues = &black;
			p.CmdBeginRenderPass(job.buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
			const VkViewport viewport = {x0, y0, x1 - x0, y1 - y0, 0.0f, 1.0f};
			const s32 sx0 = static_cast<s32>(std::floor(x0));
			const s32 sy0 = static_cast<s32>(std::floor(y0));
			const VkRect2D scissor = {{sx0, sy0},
				{static_cast<u32>(std::ceil(x1)) - static_cast<u32>(sx0),
					static_cast<u32>(std::ceil(y1)) - static_cast<u32>(sy0)}};
			p.CmdSetViewport(job.buffer, 0, 1, &viewport);
			p.CmdSetScissor(job.buffer, 0, 1, &scissor);
			p.CmdBindPipeline(job.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, context.pipeline);
			p.CmdBindDescriptorSets(
				job.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, context.pipeline_layout, 0, 1, &job.set, 0, nullptr);
			p.CmdDraw(job.buffer, 3, 1, 0, 0);
			if (overlay)
				DrawOverlay(context, job, job.buffer, *overlay, overlay_index_offset, request.final_output_extent);
			p.CmdEndRenderPass(job.buffer);
			vk_result = f.vkEndCommandBuffer(job.buffer);
			if (vk_result != VK_SUCCESS)
				return fail_vk(Stage::kCommandEnd, vk_result);

			const VkSemaphore timeline = job.completion.timeline();
			const uint64_t signal_value = job.completion.ClaimTimelineValue();
			if (job.completion.occupied())
				++context.owner_stats.reuse_before_retire;
			const uint64_t wait_values[1] = {request.candidate_wait_value};
			const uint64_t signal_values[2] = {signal_value, 0};
			VkTimelineSemaphoreSubmitInfo timeline_submit = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
			timeline_submit.waitSemaphoreValueCount = request.candidate_wait_semaphore != VK_NULL_HANDLE ? 1 : 0;
			timeline_submit.pWaitSemaphoreValues = wait_values;
			timeline_submit.signalSemaphoreValueCount = 2;
			timeline_submit.pSignalSemaphoreValues = signal_values;
			const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			const VkSemaphore signal_semaphores[2] = {timeline, job.acquire_semaphore};
			VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline_submit};
			submit.waitSemaphoreCount = request.candidate_wait_semaphore != VK_NULL_HANDLE ? 1 : 0;
			submit.pWaitSemaphores = &request.candidate_wait_semaphore;
			submit.pWaitDstStageMask = &wait_stage;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &job.buffer;
			submit.signalSemaphoreCount = 2;
			submit.pSignalSemaphores = signal_semaphores;
			result.input_has_wait = request.candidate_wait_semaphore != VK_NULL_HANDLE;
			if (!result.input_has_wait)
			{
				result.input_readiness_known = true;
				result.input_complete_at_submit = true;
			}
			const uint64_t queue_request_ns = NowNs();
			result.submit_request_ns = queue_request_ns;
			{
				const VulkanDevice::Queue::Acquisition queue =
					device->AcquireQueue(device->queue_family_graphics_compute(), device->queue_index_zerofg_presenter());
				if (s.kit_device_b->RejectZeroFGSubmitAfterTeardownIdle())
				{
					f.vkResetCommandPool(d, job.pool, 0);
					return fail(Stage::kQueueSubmit);
				}
				const uint64_t submit_begin_ns = NowNs();
				result.queue_wait_ns = submit_begin_ns - queue_request_ns;
				vk_result = f.vkQueueSubmit(queue.queue(), 1, &submit, VK_NULL_HANDLE);
				result.submit_host_ns = NowNs() - submit_begin_ns;
			}
			if (vk_result != VK_SUCCESS)
				return fail_vk(Stage::kQueueSubmit, vk_result);
			result.submission_accepted = true;
			(request.candidate_is_synthetic ? s_outputs_synthetic : s_outputs_real).fetch_add(1, std::memory_order_relaxed);
			result.effect_count = 1;
			job.signal_value = signal_value;
			job.submitted = true;
			job.input_written = true;
			job.completion.MarkSubmitted(timeline);
			context.owner_stats.CountSubmit(true, result.submit_host_ns);
			result.signal_value = signal_value;
			result.completion_semaphore = timeline;
			result.completion_per_slot = true;
			result.completion_owner_stats = context.owner_stats;
			result.submit_time_ns = NowNs();
			VkSemaphoreGetFdInfoKHR fd_info = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
			fd_info.semaphore = job.acquire_semaphore;
			fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			const uint64_t export_begin_ns = NowNs();
			const VkResult export_result = device->vkGetSemaphoreFdKHR()(d, &fd_info, &result.acquire_fence_fd);
			result.export_host_ns = NowNs() - export_begin_ns;
			if (export_result != VK_SUCCESS)
				return fail_vk(Stage::kAcquireFenceExport, export_result);
			result.acquire_fence_exported = true;
			return true;
		}

		void ReleasePost(uint32_t index)
		{
			State& s = s_state;
			if (!s.post || index >= PostContext::COUNT)
				return;
			PostContext::Job& job = s.post->jobs[index];
			job.submitted = false;
			job.signal_value = 0;
			// The presenter observed this Post complete before releasing the slot.
			job.completion.MarkRetired();
		}

		// ---- The presenter, its window, and the frames the GS publishes to it -----------------------

		bool LoadPostFunctionsLocked()
		{
			State& s = s_state;
			PostFunctions& p = s.pfn;
#define ZEROFG_POST_PROC(member, name) \
	p.member = reinterpret_cast<PFN_##name>(s.ifn.GetDeviceProcAddr(s.device, #name))
			ZEROFG_POST_PROC(CreateRenderPass, vkCreateRenderPass);
			ZEROFG_POST_PROC(DestroyRenderPass, vkDestroyRenderPass);
			ZEROFG_POST_PROC(CreateFramebuffer, vkCreateFramebuffer);
			ZEROFG_POST_PROC(DestroyFramebuffer, vkDestroyFramebuffer);
			ZEROFG_POST_PROC(CreateShaderModule, vkCreateShaderModule);
			ZEROFG_POST_PROC(DestroyShaderModule, vkDestroyShaderModule);
			ZEROFG_POST_PROC(CreateDescriptorSetLayout, vkCreateDescriptorSetLayout);
			ZEROFG_POST_PROC(DestroyDescriptorSetLayout, vkDestroyDescriptorSetLayout);
			ZEROFG_POST_PROC(CreatePipelineLayout, vkCreatePipelineLayout);
			ZEROFG_POST_PROC(DestroyPipelineLayout, vkDestroyPipelineLayout);
			ZEROFG_POST_PROC(CreateGraphicsPipelines, vkCreateGraphicsPipelines);
			ZEROFG_POST_PROC(DestroyPipeline, vkDestroyPipeline);
			ZEROFG_POST_PROC(CreateSampler, vkCreateSampler);
			ZEROFG_POST_PROC(DestroySampler, vkDestroySampler);
			ZEROFG_POST_PROC(CreateDescriptorPool, vkCreateDescriptorPool);
			ZEROFG_POST_PROC(DestroyDescriptorPool, vkDestroyDescriptorPool);
			ZEROFG_POST_PROC(AllocateDescriptorSets, vkAllocateDescriptorSets);
			ZEROFG_POST_PROC(UpdateDescriptorSets, vkUpdateDescriptorSets);
			ZEROFG_POST_PROC(CmdBeginRenderPass, vkCmdBeginRenderPass);
			ZEROFG_POST_PROC(CmdEndRenderPass, vkCmdEndRenderPass);
			ZEROFG_POST_PROC(CmdBindPipeline, vkCmdBindPipeline);
			ZEROFG_POST_PROC(CmdBindDescriptorSets, vkCmdBindDescriptorSets);
			ZEROFG_POST_PROC(CmdSetViewport, vkCmdSetViewport);
			ZEROFG_POST_PROC(CmdSetScissor, vkCmdSetScissor);
			ZEROFG_POST_PROC(CmdDraw, vkCmdDraw);
			ZEROFG_POST_PROC(FreeDescriptorSets, vkFreeDescriptorSets);
			ZEROFG_POST_PROC(CmdBindVertexBuffers, vkCmdBindVertexBuffers);
			ZEROFG_POST_PROC(CmdBindIndexBuffer, vkCmdBindIndexBuffer);
			ZEROFG_POST_PROC(CmdPushConstants, vkCmdPushConstants);
			ZEROFG_POST_PROC(CmdDrawIndexed, vkCmdDrawIndexed);
#undef ZEROFG_POST_PROC
			return p.CreateRenderPass && p.DestroyRenderPass && p.CreateFramebuffer && p.DestroyFramebuffer &&
				   p.CreateShaderModule && p.DestroyShaderModule && p.CreateDescriptorSetLayout &&
				   p.DestroyDescriptorSetLayout && p.CreatePipelineLayout && p.DestroyPipelineLayout &&
				   p.CreateGraphicsPipelines && p.DestroyPipeline && p.CreateSampler && p.DestroySampler &&
				   p.CreateDescriptorPool && p.DestroyDescriptorPool && p.AllocateDescriptorSets &&
				   p.UpdateDescriptorSets && p.CmdBeginRenderPass && p.CmdEndRenderPass && p.CmdBindPipeline &&
				   p.CmdBindDescriptorSets && p.CmdSetViewport && p.CmdSetScissor && p.CmdDraw && p.FreeDescriptorSets &&
				   p.CmdBindVertexBuffers && p.CmdBindIndexBuffer && p.CmdPushConstants && p.CmdDrawIndexed;
		}

		/// The presenter on device B, with the host's half: the ingress from the handoff, the output
		/// settings, Generation, Post and the drain. Created once per device; connected per window.
		bool CreatePresenterLocked(std::string* why)
		{
			State& s = s_state;
			if (!LoadPostFunctionsLocked())
			{
				*why = "device B lacks an entry point the Post draws with";
				return false;
			}
			// The presenter's mode switch, which it only names in its log; the engine is already built.
			cvars::zerofg_mode = s.engine_mode == zerofg::Mode::kReallyZero ? "reallyzero" : "zero";
			s.presenter = ZeroFGIndependentPresenter::Create(
				s.kit_device_b.get(), true, SYNTHETIC_COST_SEED_NS,
				[](ZeroFGIndependentPresenter::IngressSourcePublication& publication) {
					State& state = s_state;
					publication = {};
					if (!state.handoff)
						return false;
					if (state.handoff->failed() || state.kit_device_b->IsLost())
					{
						publication.external_failure = true;
						return true;
					}
					const bool acquired = state.handoff->Acquire(publication);
					// Acquire marks a reporting window under the pool lock; log it outside.
					state.handoff->MaybeLog();
					return acquired;
				},
				[]() {
					std::unique_lock paint_lock(s_paint_mutex);
					return s_paint_config;
				},
				[](const ZeroFGIndependentPresenter::GenerationRequest& request,
					ZeroFGIndependentPresenter::GenerationResult& result) { return ProcessGeneration(request, result); },
				[](uint32_t index, ZeroFGIndependentPresenter::GenerationResult& result, bool& ready) {
					return PollGeneration(index, result, ready);
				},
				[](uint32_t index) { ReleaseSynthetic(index); }, []() { DestroyGeneration(); },
				[](const ZeroFGIndependentPresenter::PostProcessRequest& request,
					ZeroFGIndependentPresenter::PostProcessResult& result) { return ProcessPost(request, result); },
				[](uint32_t index) { ReleasePost(index); }, []() { DestroyPost(); },
				[]() { return s_state.kit_device_b->DrainZeroFGForTeardown(); });
			if (!s.presenter)
			{
				*why = "the presenter refused device B (see the ZeroFGC0 lines above)";
				return false;
			}
			// The GS presents every frame and checks the surface's hand-over there, so the presenter's
			// requests for a paint need nothing more.
			s.presenter->SetMainSurfaceUIRequest([]() {});
			return true;
		}

		/// Takes the presenter off the window: the handoff stops taking frames, the presenter stops
		/// producing, the GS device's work that feeds the handoff finishes, then device B's surface
		/// resources, contexts and handoff pool go (the presenter's disconnect order).
		void DisconnectSurfaceLocked()
		{
			State& s = s_state;
			if (!s.presenter || s.surface == VK_NULL_HANDLE)
				return;
			if (s.handoff)
				s.handoff->DisarmAndQuiesceSource();
			s.presenter->BeginSurfaceDisconnect();
			s.kit_device_a->vkDeviceWaitIdle()(s.kit_device_a->device());
			s.presenter->DestroySurfaceResourcesAfterSourceIdle();
			if (!s.kit_device_b->DrainZeroFGForTeardown())
				WARNING_LOG("ZeroFG: device B did not drain before its contexts were destroyed");
			DestroyGeneration();
			DestroyPost();
			if (s.handoff)
				s.handoff->DestroyAfterIdle();
			// The surface is the GS device's; only device B's swap chain on it went (above).
			s.surface = VK_NULL_HANDLE;
			s.window = nullptr;
			s.released_by_a = false;
			{
				std::unique_lock overlay_lock(s_overlay_mutex);
				s_overlay.reset();
			}
			// The window's lifecycle is the GS device's again.
			s.presenter->NoteMainSurfaceProducedByA();
			INFO_LOG("ZeroFG: presenter disconnected from the window.");
		}

		void TeardownHostLocked()
		{
			State& s = s_state;
			if (s.presenter)
			{
				DisconnectSurfaceLocked();
				s.presenter.reset();
			}
			DestroyGeneration();
			DestroyPost();
			if (s.handoff)
			{
				s.handoff->DisarmAndQuiesceSource();
				if (s.kit_device_a)
					s.kit_device_a->vkDeviceWaitIdle()(s.kit_device_a->device());
				s.handoff->DestroyAfterIdle();
				s.handoff.reset();
			}
			if (s.kit_device_a && s.source.staging.image != VK_NULL_HANDLE)
			{
				s.kit_device_a->vkDeviceWaitIdle()(s.kit_device_a->device());
				s.source.staging.Destroy(s.kit_device_a.get());
			}
			s.source = {};
		}

		/// The size frames are generated at: the picture's footprint on the screen, fitted down to the
		/// engine's grid, 64 cells across and an even number of rows, where it runs its economical path
		/// (20-35% cheaper on the Adreno 840 than a size it pads). From the footprint alone, so the
		/// game's own resolution (512 or 640 wide, interlaced or not) never enters it, and never past
		/// it, so it always fits the window the presenter was connected with. Taken from the first
		/// frame of a connection and kept (State::generation_extent). The Post scales back to the draw
		/// rectangle, so the aspect change of the fit, or of a later footprint, never shows.
		VkExtent2D GenerationExtent(float draw_w, float draw_h)
		{
			const u32 cell = std::max(1u, static_cast<u32>(draw_w) / 64u);
			u32 rows = static_cast<u32>(draw_h) / cell;
			rows = std::max(2u, rows & ~1u);
			return {cell * 64u, rows * cell};
		}
#endif
	} // namespace

	bool WantsSourceHandoff()
	{
		// Without the presenter in the build nothing would use the handoff.
		return ZEROFG_WITH_HOST_KIT && GSConfig.ZeroFGEnabled;
	}

	void NoteSourceHandoff(bool enabled)
	{
		s_source_handoff.store(enabled, std::memory_order_release);
	}

	bool SourceHandoffEnabled()
	{
		return s_source_handoff.load(std::memory_order_acquire);
	}

	u32 InstanceApiVersion(u32 base)
	{
		if (!WantsSourceHandoff() || !Vulkan::IsVulkanLibraryLoaded() || !vkGetInstanceProcAddr)
			return base;
		const auto enumerate_version =
			InstanceProc<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
		u32 version = VK_API_VERSION_1_0;
		if (!enumerate_version || enumerate_version(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3)
			return base;
		return std::max(base, VK_API_VERSION_1_3);
	}

	void OnDeviceCreated(const SourceDevice& source)
	{
		std::unique_lock lock(s_mutex);
		// A recreated GS device brings device B up again from nothing.
		DestroyLocked();

		if (!GSConfig.ZeroFGEnabled)
		{
			s_state.status = "ZeroFG: off";
			return;
		}

		Common::Timer timer;
		zerofg::Capabilities caps;
		std::string summary;
		std::string backend;
		std::string why;
		if (!UseSourceInstanceLocked(source, &why) || !CreateDeviceLocked(&caps, &summary, &why) ||
			!CreateEngineLocked(caps, &backend, &why))
		{
			DestroyLocked();
			s_state.status = fmt::format("ZeroFG: unavailable, {}", why);
			WARNING_LOG("{}", s_state.status);
			return;
		}
		INFO_LOG("ZeroFG: device B ready in {:.1f} ms: {}; engine {} ({}) built for {}x{}.", timer.GetTimeMilliseconds(),
			summary, ModeName(s_state.engine_mode), backend, PROBE_WIDTH, PROBE_HEIGHT);

#if ZEROFG_WITH_HOST_KIT
		Common::Timer handoff_timer;
		s_state.handoff_qualified = QualifyHandoffLocked(source, &why);
		if (!s_state.handoff_qualified)
		{
			s_state.status = fmt::format("ZeroFG: device B ready ({}), handoff unavailable: {}", backend, why);
			WARNING_LOG("{}", s_state.status);
			return;
		}
		INFO_LOG("ZeroFG: the handoff qualified in {:.1f} ms.", handoff_timer.GetTimeMilliseconds());
		if (!CreatePresenterLocked(&why))
		{
			s_state.status = fmt::format("ZeroFG: device B ready ({}), presenter unavailable: {}", backend, why);
			WARNING_LOG("{}", s_state.status);
			return;
		}
		s_state.status = fmt::format("ZeroFG: ready ({}), waiting for the window", backend);
		INFO_LOG("ZeroFG: presenter created.");
#else
		s_state.status = fmt::format("ZeroFG: device B ready ({}), no presenter in this build", backend);
#endif
	}

	void Shutdown()
	{
		std::unique_lock lock(s_mutex);
		DestroyLocked();
	}

	bool IsActive()
	{
		std::unique_lock lock(s_mutex);
		return static_cast<bool>(s_state.engine);
	}

	std::string GetStatusText()
	{
		std::unique_lock lock(s_mutex);
		return s_state.status;
	}

#if ZEROFG_WITH_HOST_KIT
	void ConnectSurface(const OutputSurface& output)
	{
		std::unique_lock lock(s_mutex);
		State& s = s_state;
		if (!GSConfig.ZeroFGEnabled || !s.presenter || s.failed_open || !output.window ||
			output.surface == VK_NULL_HANDLE || !output.width || !output.height)
			return;
		DisconnectSurfaceLocked();
		if (s.engine_mode != ConfiguredMode())
		{
			// The mode was switched while ZeroFG was off the window (GSDeviceVK took it off for
			// this): the engine is rebuilt on the same device B, and a new presenter with it.
			std::string backend;
			std::string why;
			s.presenter.reset();
			s.dfn.DeviceWaitIdle(s.device);
			s.engine.reset();
			if (!CreateEngineLocked(s.caps, &backend, &why) || !CreatePresenterLocked(&why))
			{
				s.presenter.reset();
				s.status = fmt::format("ZeroFG: unavailable after the mode switch, {}", why);
				WARNING_LOG("{}", s.status);
				return;
			}
			INFO_LOG("ZeroFG: engine switched to {} ({}).", ModeName(s.engine_mode), backend);
		}

		// Device B borrows the GS device's surface (same instance).
		ANativeWindow* const window = static_cast<ANativeWindow*>(output.window);
		s.surface = output.surface;
		// A new connection may generate at a new size (the window may have changed).
		s.generation_extent = {};
		s.draw_w = s.draw_h = 0.0f;
		{
			std::unique_lock paint_lock(s_paint_mutex);
			s_output_extent = {output.width, output.height};
			s_paint_config = {};
			s_paint_config.draw_rect = {0.0f, 0.0f, static_cast<float>(output.width), static_cast<float>(output.height)};
		}
		// The GS device's swap chain is on the window now: it is the producer until the presenter
		// takes the window over (OwnsOutput / MainSurfaceHandoffPending).
		s.presenter->NoteMainSurfaceProducedByA();
		if (!s.presenter->Connect(window, {output.width, output.height}, output.format, s.surface))
		{
			// The presenter's refusals last (its MSA session), so it is not offered the window again.
			s.surface = VK_NULL_HANDLE;
			s.failed_open = true;
			s.status = "ZeroFG: the presenter refused the window, the GS presents alone";
			WARNING_LOG("ZeroFG: the presenter refused the window; the GS presents alone.");
			return;
		}
		s.window = window;
		s.handoff->Arm();
		s.status = "ZeroFG: connected";
		INFO_LOG("ZeroFG: presenter connected to the window ({}x{}, format {}).", output.width, output.height,
			static_cast<int>(output.format));
	}

	void DisconnectSurface()
	{
		std::unique_lock lock(s_mutex);
		DisconnectSurfaceLocked();
	}

	// The per-frame questions below are asked by the GS thread, the only thread that creates or
	// destroys the presenter, so they need no lock.

	bool Connected()
	{
		const State& s = s_state;
		return s.presenter && s.surface != VK_NULL_HANDLE;
	}

	bool CanConnect()
	{
		const State& s = s_state;
		return s.presenter && !s.failed_open && s.surface == VK_NULL_HANDLE;
	}

	bool ModeChanged()
	{
		const State& s = s_state;
		return s.presenter && s.engine_mode != ConfiguredMode();
	}

	bool OwnsOutput()
	{
		const State& s = s_state;
		return s.presenter && s.surface != VK_NULL_HANDLE && s.presenter->owns_final_output();
	}

	bool MainSurfaceHandoffPending()
	{
		const State& s = s_state;
		return s.presenter && s.surface != VK_NULL_HANDLE && s.presenter->main_surface_handoff_pending();
	}

	void MainSurfaceReleasedByA()
	{
		State& s = s_state;
		if (s.presenter)
		{
			s.presenter->MainSurfaceReleasedByA();
			s.released_by_a = true;
			s.status = "ZeroFG: presenting";
			INFO_LOG("ZeroFG: the GS device released the window; device B presents.");
		}
	}

	bool ReconnectRequested()
	{
		// Two ways back to the GS device: the surface went out of date under device B, or the
		// presenter failed and handed the window back (it no longer owns the output, and the GS
		// device has no swap chain since it released the window).
		const State& s = s_state;
		return s.presenter && s.surface != VK_NULL_HANDLE &&
			   (s.presenter->main_surface_reconnect_requested() ||
				   (s.released_by_a && !s.presenter->owns_final_output()));
	}

	void FailOpen()
	{
		std::unique_lock lock(s_mutex);
		State& s = s_state;
		DisconnectSurfaceLocked();
		// The presenter that handed the window back refuses it from now on: a new one takes the next
		// connection, until the device has failed open too often.
		s.presenter.reset();
		std::string why;
		if (++s.fail_opens > MAX_FAIL_OPENS || !CreatePresenterLocked(&why))
		{
			s.failed_open = true;
			s.status = "ZeroFG: failed open, the GS presents alone";
			WARNING_LOG("ZeroFG: the presenter gave the window back (fail-open {}, limit {}{}); the GS device presents "
						"for the rest of this session.",
				s.fail_opens, MAX_FAIL_OPENS, why.empty() ? std::string() : fmt::format(", {}", why));
			return;
		}
		s.status = "ZeroFG: failed open, rearmed";
		WARNING_LOG("ZeroFG: the presenter gave the window back (fail-open {}, limit {}); a new one takes the next "
					"connection.",
			s.fail_opens, MAX_FAIL_OPENS);
	}

	bool WantsFrames()
	{
		const State& s = s_state;
		return s.presenter && s.surface != VK_NULL_HANDLE && s.handoff && !s.handoff->failed() &&
			   s.presenter->accepting();
	}

	std::string GetOverlayText()
	{
		// The rates over half-second windows, so the line holds still between updates.
		static u64 last_ns = 0;
		static u64 last_real = 0;
		static u64 last_synthetic = 0;
		static float real_fps = 0.0f;
		static float synthetic_fps = 0.0f;
		const State& s = s_state;
		if (!GSConfig.ZeroFGEnabled || !s.presenter)
			return {};
		if (s.failed_open)
			return "ZeroFG: off (it failed; see the log)";
		if (!OwnsOutput())
		{
			last_ns = 0;
			return s.surface != VK_NULL_HANDLE ? "ZeroFG: starting" : std::string();
		}
		const u64 now = NowNs();
		const u64 real = s_outputs_real.load(std::memory_order_relaxed);
		const u64 synthetic = s_outputs_synthetic.load(std::memory_order_relaxed);
		if (!last_ns)
		{
			last_ns = now;
			last_real = real;
			last_synthetic = synthetic;
			real_fps = synthetic_fps = 0.0f;
		}
		else if (now - last_ns >= 500000000ull)
		{
			const float seconds = static_cast<float>(now - last_ns) / 1e9f;
			real_fps = static_cast<float>(real - last_real) / seconds;
			synthetic_fps = static_cast<float>(synthetic - last_synthetic) / seconds;
			last_ns = now;
			last_real = real;
			last_synthetic = synthetic;
		}
		return fmt::format("ZeroFG ({}): {:.1f} FPS ({:.1f} real + {:.1f} generated)", ModeName(s.engine_mode),
			real_fps + synthetic_fps, real_fps,
			synthetic_fps);
	}

	void CaptureOverlay(const ImDrawData* draw_data)
	{
		State& s = s_state;
		if (!s.presenter || !draw_data)
			return;
		// ImGui's textures, copied the first time they are seen and whenever they change, all along
		// while ZeroFG is up: one made while the GS device presented is still needed once B does.
		if (draw_data->Textures)
		{
			for (ImTextureData* texture : *draw_data->Textures)
			{
				if (texture->Status == ImTextureStatus_WantDestroy || texture->Status == ImTextureStatus_Destroyed)
				{
					s_overlay_textures.erase(texture->UniqueID);
					s_overlay_pending.erase(texture->UniqueID);
					continue;
				}
				if (!texture->GetPixels() || texture->Width <= 0 || texture->Height <= 0)
					continue;
				// A change is copied as soon as it is seen and once more when the GS device has
				// applied it (the status is OK again): ImGui may add to a pending change, and the GS
				// device applies it only on a present that draws something.
				const int id = texture->UniqueID;
				const bool pending =
					texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates;
				const bool known = s_overlay_textures.contains(id);
				const bool was_pending = s_overlay_pending.contains(id);
				if (known && pending == was_pending)
					continue;
				if (pending)
					s_overlay_pending.insert(id);
				else
					s_overlay_pending.erase(id);
				auto copy = std::make_shared<OverlayTexture>();
				copy->id = id;
				copy->version = ++s_overlay_version;
				copy->width = static_cast<u32>(texture->Width);
				copy->height = static_cast<u32>(texture->Height);
				const size_t count = static_cast<size_t>(copy->width) * copy->height;
				copy->pixels.resize(count);
				if (texture->Format == ImTextureFormat_RGBA32)
				{
					std::memcpy(copy->pixels.data(), texture->GetPixels(), count * sizeof(u32));
				}
				else
				{
					const u8* const alpha = static_cast<const u8*>(texture->GetPixels());
					for (size_t i = 0; i < count; ++i)
						copy->pixels[i] = 0x00FFFFFFu | (static_cast<u32>(alpha[i]) << 24);
				}
				s_overlay_textures[id] = std::move(copy);
			}
		}

		// The draw lists, while device B presents. A rotated display is left without: the GS present
		// rotates ImGui's vertices as it copies them, and the Post does not.
		std::shared_ptr<OverlaySnapshot> overlay;
		if (OwnsOutput() && GSConfig.Rotation == DisplayRotation::Rot0)
		{
			overlay = std::make_shared<OverlaySnapshot>();
			overlay->origin[0] = draw_data->DisplayPos.x;
			overlay->origin[1] = draw_data->DisplayPos.y;
			for (const auto& [id, texture] : s_overlay_textures)
				overlay->textures.push_back(texture);
			overlay->vertices.reserve(static_cast<size_t>(draw_data->TotalVtxCount));
			overlay->indices.reserve(static_cast<size_t>(draw_data->TotalIdxCount));
			for (const ImDrawList* list : draw_data->CmdLists)
			{
				const s32 base_vertex = static_cast<s32>(overlay->vertices.size());
				const u32 base_index = static_cast<u32>(overlay->indices.size());
				overlay->vertices.insert(overlay->vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
				overlay->indices.insert(overlay->indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
				for (const ImDrawCmd& command : list->CmdBuffer)
				{
					// A texture of the GS device's own (an icon, a cover) has no copy on device B.
					const ImTextureData* const texture = command.TexRef._TexData;
					if (command.UserCallback || !command.ElemCount || !texture)
						continue;
					OverlaySnapshot::Command& out = overlay->commands.emplace_back();
					out.clip[0] = command.ClipRect.x - draw_data->DisplayPos.x;
					out.clip[1] = command.ClipRect.y - draw_data->DisplayPos.y;
					out.clip[2] = command.ClipRect.z - draw_data->DisplayPos.x;
					out.clip[3] = command.ClipRect.w - draw_data->DisplayPos.y;
					out.texture = texture->UniqueID;
					out.first_index = base_index + command.IdxOffset;
					out.vertex_offset = base_vertex + static_cast<s32>(command.VtxOffset);
					out.index_count = command.ElemCount;
				}
			}
		}
		std::unique_lock lock(s_overlay_mutex);
		s_overlay = std::move(overlay);
	}

	void CaptureGameFrame(VkCommandBuffer cmd, const GameFrame& frame)
	{
		State& s = s_state;
		SourceStage& source = s.source;
		source.captured = false;
		if (!WantsFrames() || frame.image == VK_NULL_HANDLE || !frame.output_width || !frame.output_height)
			return;
		const s32 content_w = frame.src[2] - frame.src[0];
		const s32 content_h = frame.src[3] - frame.src[1];
		const float draw_w = frame.draw[2] - frame.draw[0];
		const float draw_h = frame.draw[3] - frame.draw[1];
		if (content_w <= 0 || content_h <= 0 || draw_w < 1.0f || draw_h < 1.0f)
			return;

		const VulkanDevice* const device = s.kit_device_a.get();
		const VulkanDevice::Functions& f = device->functions();
		// The first frame of the connection sets the generation size; later frames are scaled into it
		// wherever the game draws them (GoW2 widens its picture when progressive scan goes on).
		if (!s.generation_extent.width)
			s.generation_extent = GenerationExtent(draw_w, draw_h);
		const VkExtent2D extent = s.generation_extent;
		if (draw_w != s.draw_w || draw_h != s.draw_h)
		{
			if (s.draw_w != 0.0f && source.staging.extent.width == extent.width &&
				source.staging.extent.height == extent.height)
			{
				INFO_LOG("ZeroFG: the game's {}x{} now drawn at {:.0f}x{:.0f}; frames stay {}x{}", content_w, content_h,
					draw_w, draw_h, extent.width, extent.height);
			}
			s.draw_w = draw_w;
			s.draw_h = draw_h;
		}
		if (source.staging.extent.width != extent.width || source.staging.extent.height != extent.height)
		{
			// A new picture size. The handoff may still be copying out of the old image.
			if (source.staging.image != VK_NULL_HANDLE)
			{
				f.vkDeviceWaitIdle(device->device());
				source.staging.Destroy(device);
			}
			if (!source.staging.Create(device, extent, FRAME_FORMAT,
					VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
			{
				ERROR_LOG("ZeroFG: no {}x{} frame image on the GS device", extent.width, extent.height);
				return;
			}
			source.layout = VK_IMAGE_LAYOUT_UNDEFINED;
			INFO_LOG("ZeroFG: frames {}x{} from the game's {}x{} drawn at {:.0f}x{:.0f}", extent.width,
				extent.height, content_w, content_h, draw_w, draw_h);
		}

		// The game's picture into the frame image, scaled to the generation size, then left the way the
		// handoff reads it: shader-read-only, last touched in the fragment stage.
		VkImageMemoryBarrier to_write = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		to_write.srcAccessMask = 0;
		to_write.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_write.oldLayout = source.layout;
		to_write.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_write.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_write.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_write.image = source.staging.image;
		to_write.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_write);
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {frame.src[0], frame.src[1], 0};
		blit.srcOffsets[1] = {frame.src[2], frame.src[3], 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstOffsets[1] = {static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1};
		const bool same_size = extent.width == static_cast<u32>(content_w) && extent.height == static_cast<u32>(content_h);
		f.vkCmdBlitImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.staging.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, same_size ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		VkImageMemoryBarrier to_read = to_write;
		to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_read.newLayout = FRAME_LAYOUT;
		f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &to_read);
		source.layout = FRAME_LAYOUT;

		// Where the Post puts it: the GS present's draw rectangle, in the FinalOutput's pixels.
		{
			std::unique_lock paint_lock(s_paint_mutex);
			const float sx = static_cast<float>(s_output_extent.width) / static_cast<float>(frame.output_width);
			const float sy = static_cast<float>(s_output_extent.height) / static_cast<float>(frame.output_height);
			s_paint_config.draw_rect = {frame.draw[0] * sx, frame.draw[1] * sy, frame.draw[2] * sx, frame.draw[3] * sy};
			s_paint_config.linear_filter = frame.linear;
		}
		source.aspect_x = static_cast<u32>(std::lround(draw_w));
		source.aspect_y = static_cast<u32>(std::lround(draw_h));
		source.captured = true;
	}

	void NoteSourceFrame()
	{
		// The Source issue, one per new game frame, on the thread that also publishes: the presenter
		// qualifies the game's native cadence from these before it takes any frame (activation).
		State& s = s_state;
		if (!s.presenter)
			return;
		xe::RecordGuestPresent();
		xe::RecordSourcePresent();
		// The host asks for the activation, once per frame
		// (IsGuestOutputPresentationBackendActive). It qualifies after this frame's capture, so the
		// next frame is the first ZeroFG takes.
		if (s.surface != VK_NULL_HANDLE && !s.presenter->accepting())
			s.presenter->TryActivateFromNativeSourceCadence();
	}

	void PublishCapturedFrame()
	{
		State& s = s_state;
		SourceStage& source = s.source;
		if (!source.captured)
			return;
		source.captured = false;
		if (!s.presenter || !s.handoff || !s.presenter->accepting())
			return;
		const u32 mailbox = source.mailbox;
		source.mailbox = (source.mailbox + 1) % ZeroFGIndependentPresenter::kMailboxCount;

		// The copy into the handoff's buffer, after the frame on the same queue. May wait for a free
		// buffer: that wait is ZeroFG's Source backpressure, lossless by design.
		const VkSubmitInfo* const submit = s.handoff->PrepareSource(mailbox, source.staging.image, source.staging.extent);
		if (submit)
		{
			const uint64_t queue_begin_ns = NowNs();
			VkResult result;
			uint64_t submit_begin_ns;
			{
				const VulkanDevice::Queue::Acquisition queue =
					s.kit_device_a->AcquireQueue(s.kit_device_a->queue_family_graphics_compute(), 0);
				submit_begin_ns = NowNs();
				result = s.kit_device_a->functions().vkQueueSubmit(queue.queue(), 1, submit, VK_NULL_HANDLE);
			}
			s.handoff->SourceSubmitted(result, submit_begin_ns - queue_begin_ns, NowNs() - submit_begin_ns);
		}

		const xe::SourcePublicationTelemetry publication = xe::PrepareSourcePublicationTelemetry();
		xe::CommitSourcePublicationTelemetry(publication, false);
		ZeroFGIndependentPresenter::IngressSourcePublication metadata;
		metadata.mailbox_index = mailbox;
		metadata.content_width = source.staging.extent.width;
		metadata.content_height = source.staging.extent.height;
		metadata.display_aspect_ratio_x = source.aspect_x;
		metadata.display_aspect_ratio_y = source.aspect_y;
		metadata.is_8bpc = false;
		metadata.publication = publication;
		s.handoff->Publish(mailbox, metadata, s.presenter.get());
		s.presenter->ClaimPublicationAuthority();
		s.presenter->PublicationCommitted();
	}
#else
	void ConnectSurface(const OutputSurface&) {}
	void DisconnectSurface() {}
	bool Connected() { return false; }
	bool CanConnect() { return false; }
	bool ModeChanged() { return false; }
	bool OwnsOutput() { return false; }
	bool MainSurfaceHandoffPending() { return false; }
	void MainSurfaceReleasedByA() {}
	bool ReconnectRequested() { return false; }
	void FailOpen() {}
	bool WantsFrames() { return false; }
	void CaptureGameFrame(VkCommandBuffer, const GameFrame&) {}
	void PublishCapturedFrame() {}
	void CaptureOverlay(const ImDrawData*) {}
	std::string GetOverlayText() { return {}; }
	void NoteSourceFrame() {}
#endif
} // namespace GSZeroFG

#else

namespace GSZeroFG
{
	bool WantsSourceHandoff() { return false; }
	void NoteSourceHandoff(bool) {}
	bool SourceHandoffEnabled() { return false; }
	u32 InstanceApiVersion(u32 base) { return base; }
	void OnDeviceCreated(const SourceDevice&) {}
	void Shutdown() {}
	bool IsActive() { return false; }
	std::string GetStatusText() { return {}; }
	void ConnectSurface(const OutputSurface&) {}
	void DisconnectSurface() {}
	bool Connected() { return false; }
	bool CanConnect() { return false; }
	bool ModeChanged() { return false; }
	bool OwnsOutput() { return false; }
	bool MainSurfaceHandoffPending() { return false; }
	void MainSurfaceReleasedByA() {}
	bool ReconnectRequested() { return false; }
	void FailOpen() {}
	bool WantsFrames() { return false; }
	void CaptureGameFrame(VkCommandBuffer, const GameFrame&) {}
	void PublishCapturedFrame() {}
	void CaptureOverlay(const ImDrawData*) {}
	std::string GetOverlayText() { return {}; }
	void NoteSourceFrame() {}
} // namespace GSZeroFG

#endif
