// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSLsfg.h"

#include "Config.h"
#include "GS/GS.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef ARMSX2_HAS_LSFG
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKSwapChain.h"

#include "GS/Renderers/Vulkan/FrameGen/FrameGen.h"
#include "GS/Renderers/Vulkan/FrameGen/LosslessDll.h"
#include "GS/Renderers/Vulkan/FrameGen/LsfgVkCompat.h"

#include "common/Timer.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>
#endif

namespace GSLsfg
{
	namespace
	{
		std::string s_dll_path;

		// Written once from the GS thread at device creation, read from the UI thread whenever
		// the settings screen asks why the row is greyed out. Atomic rather than mutex'd because
		// the UI only needs a recent value, never a synchronised one.
		std::atomic<bool> s_caps_known{false};
		std::atomic<bool> s_is_vulkan{false};
		std::atomic<u32> s_adreno_generation{0};

		// Sticky: a device that failed to initialise once will fail the same way every frame,
		// and retrying inside the present path would turn one bad init into a per-frame stall.
		std::atomic<bool> s_init_failed{false};

		// The structural PE check reads the file, and GetUnavailableReason() runs once per frame
		// from EndPresent while the feature is on — so without this the GS thread did an
		// fopen/fread/fseek/fread/fclose on the present path every single frame. The verdict can
		// only change when the path does, which is exactly when SetDllPath() clears it.
		std::atomic<bool> s_dll_checked{false};
		std::atomic<bool> s_dll_ok{false};

		// What the overlay reports. Written from the GS thread in the present path, read from
		// whichever thread draws the OSD, so both are atomic rather than mutex'd — a recent
		// value is all a status line needs.
		//
		// s_no_shaders separates "your DLL has no usable shader family" from every other way
		// initialisation can fail. Both are InitFailed to the settings screen, but they are
		// different problems: one is fixed by updating Lossless Scaling, the other is not.
		std::atomic<float> s_display_fps{0.0f};
		std::atomic<bool> s_no_shaders{false};
	} // namespace

	void NoteRendererCapability(bool is_vulkan, u32 adreno_generation)
	{
		s_is_vulkan.store(is_vulkan, std::memory_order_relaxed);
		s_adreno_generation.store(adreno_generation, std::memory_order_relaxed);
		s_caps_known.store(true, std::memory_order_release);
	}

	void SetDllPath(std::string path)
	{
		if (s_dll_path == path)
			return;
		s_dll_path = std::move(path);
		// A new DLL deserves a fresh attempt; the previous failure may have been this file.
		s_init_failed.store(false, std::memory_order_relaxed);
		s_dll_checked.store(false, std::memory_order_relaxed);
		s_no_shaders.store(false, std::memory_order_relaxed);
	}

	const std::string& GetDllPath() { return s_dll_path; }

	bool LooksLikeLosslessDll(const std::string& path)
	{
		// Structural only: "MZ" at 0 and a PE signature where the DOS header points. The point
		// is to reject an obviously-wrong pick at import time — a .txt, a truncated download,
		// the wrong DLL — not to authenticate Lossless Scaling. A file that passes this and is
		// still not the real thing fails later with a missing-shader error, which is the
		// message the user needs anyway.
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
		if (!fp)
			return false;

		u8 dos[0x40] = {};
		if (std::fread(dos, sizeof(dos), 1, fp.get()) != 1 || dos[0] != 'M' || dos[1] != 'Z')
			return false;

		// e_lfanew at 0x3C is the offset of the PE header.
		const u32 pe_off = static_cast<u32>(dos[0x3C]) | (static_cast<u32>(dos[0x3D]) << 8) |
		                   (static_cast<u32>(dos[0x3E]) << 16) | (static_cast<u32>(dos[0x3F]) << 24);
		if (pe_off < sizeof(dos) || pe_off > (64u * 1024u * 1024u))
			return false;

		if (FileSystem::FSeek64(fp.get(), static_cast<s64>(pe_off), SEEK_SET) != 0)
			return false;
		u8 sig[4] = {};
		if (std::fread(sig, sizeof(sig), 1, fp.get()) != 1)
			return false;
		return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
	}

	Unavailable GetUnavailableReason()
	{
#ifndef ARMSX2_HAS_LSFG
		return Unavailable::NotCompiledIn;
#else
		// Before any renderer has come up there is nothing to ask, so the two hardware gates are
		// skipped rather than guessed. Reporting GpuUnsupported from a cold start would tell a
		// perfectly capable device it is not supported, purely because no game had booted yet.
		if (s_caps_known.load(std::memory_order_acquire))
		{
			// Vulkan only: the library shares images as AHardwareBuffers imported into its own
			// VkDevice, and there is no equivalent path for the GLES backend.
			if (!s_is_vulkan.load(std::memory_order_relaxed))
				return Unavailable::NotVulkan;
			// Adreno 7xx or newer, per upstream. Asked of the resolved architecture rather than
			// a GL_RENDERER substring search, for the same reason the Mali workarounds moved
			// into the driver database: a parsed generation can say "7xx and up", a substring
			// cannot.
			if (s_adreno_generation.load(std::memory_order_relaxed) < 7)
				return Unavailable::GpuUnsupported;
		}

		if (s_dll_path.empty())
			return Unavailable::NoDll;
		if (!s_dll_checked.load(std::memory_order_acquire))
		{
			s_dll_ok.store(LooksLikeLosslessDll(s_dll_path), std::memory_order_relaxed);
			s_dll_checked.store(true, std::memory_order_release);
		}
		if (!s_dll_ok.load(std::memory_order_relaxed))
			return Unavailable::DllUnreadable;
		if (s_init_failed.load(std::memory_order_relaxed))
			return Unavailable::InitFailed;
		return Unavailable::Available;
#endif
	}

	bool IsAvailable() { return GetUnavailableReason() == Unavailable::Available; }

	const char* GetUnavailableReasonString()
	{
		switch (GetUnavailableReason())
		{
			case Unavailable::Available: return "available";
			case Unavailable::NotCompiledIn: return "not included in this build";
			case Unavailable::NotVulkan: return "requires the Vulkan renderer";
			case Unavailable::GpuUnsupported: return "requires an Adreno 7xx or newer GPU";
			case Unavailable::NoDll: return "no Lossless.dll selected";
			case Unavailable::DllUnreadable: return "the selected file is not a readable DLL";
			case Unavailable::InitFailed: return "frame generation failed to start on this device";
			default: return "unavailable";
		}
	}

	float GetDisplayFPS() { return s_display_fps.load(std::memory_order_relaxed); }

	std::string GetStatusText()
	{
		// Nothing at all when the user has not asked for frame generation — an overlay line for a
		// feature nobody switched on is just clutter. Every OTHER state says something, including
		// the ones where nothing is wrong yet, because "on but silent" is indistinguishable from
		// "on and broken" and that is precisely the failure this exists to prevent.
		if (!GSConfig.LsfgEnabled)
			return {};

		switch (GetUnavailableReason())
		{
			case Unavailable::Available:
				break;
			case Unavailable::InitFailed:
				// Split out because the two have different fixes: "no shaders" means update
				// Lossless Scaling, "failed" means this device or driver refused.
				return s_no_shaders.load(std::memory_order_relaxed) ? "LSFG: no shaders" : "LSFG: failed";
			default:
				return "LSFG: unavailable";
		}

		// Available but no window has closed yet: bring-up, or the first second of a session.
		const float fps = s_display_fps.load(std::memory_order_relaxed);
		if (fps <= 0.0f)
			return "LSFG: starting";
		return fmt::format("LSFG: {:.2f}", fps);
	}
} // namespace GSLsfg

#ifndef ARMSX2_HAS_LSFG

// Play flavour, or a build whose fetch produced no library. The state queries above still work
// (and always answer NotCompiledIn), so only the parts that would need the library are stubbed.
namespace GSLsfg
{
	bool Initialize(VKSwapChain*, u32) { return false; }
	void Shutdown() {}
	bool IsActive() { return false; }
	u32 GetMultiplier() { return 1; }
	bool PresentWithGeneration(VkQueue, VKSwapChain*, VkSemaphore, bool) { return false; }
} // namespace GSLsfg

#else

namespace GSLsfg
{
	namespace
	{
		// --- state ----------------------------------------------------------------------------
		// Frame generation now runs as ordinary compute on the emulator's OWN device (ported from
		// Eden, PR #4263). The previous implementation drove a separate library on a second
		// VkDevice and shared images through AHardwareBuffer, which forced a full device idle
		// twice per frame because Android offers no cross-device semaphore. None of that is here.
		std::optional<Vulkan::Device> s_device;
		std::optional<Vulkan::MemoryAllocator> s_allocator;
		std::optional<Vulkan::FrameGen> s_frame_gen;

		bool s_active = false;
		u32 s_multiplier = 1;
		u8 s_flow_scale_percent = 100;
		bool s_performance_requested = false;
		VkExtent2D s_extent = {};
		VkFormat s_format = VK_FORMAT_UNDEFINED;
		VkDevice s_vk_device = VK_NULL_HANDLE;

		/// One storage view per swap chain image. Generated frames are written straight into a
		/// swap chain image through these, which is why the swap chain has to carry
		/// VK_IMAGE_USAGE_STORAGE_BIT — see VKSwapChain::IsStorageUsageAvailable.
		std::vector<VkImageView> s_storage_views;

		/// One command buffer for the whole frame's generation work, and one semaphore per
		/// present that will be issued from it.
		///
		/// ★ Everything is recorded into ONE buffer and submitted ONCE, signalling N+1
		/// semaphores. The obvious alternative — a submit per generated frame — reintroduces the
		/// binary-semaphore hazard the old implementation was bitten by, because the real
		/// present and the first generated present would both want to wait on the semaphore that
		/// says "the source copy has landed". A binary semaphore may be waited exactly once.
		VkCommandPool s_cmd_pool = VK_NULL_HANDLE;
		VkCommandBuffer s_cmd = VK_NULL_HANDLE;
		std::array<VkSemaphore, VideoCore::FrameGen::MAX_GENERATIONS> s_acquire_sems = {};
		std::array<VkSemaphore, VideoCore::FrameGen::MAX_GENERATIONS + 1> s_done_sems = {};

		u64 s_frame_index = 0;

		// The one-second display-rate window. Reset with everything else in Shutdown so a stale
		// number cannot outlive the session it came from.
		u64 s_fps_window_start = 0;
		u32 s_fps_real = 0;
		u32 s_fps_generated = 0;

		/// Book frames as they reach the presentation engine and republish the rate once a
		/// second. Called on the declined paths too, with nothing generated: a real frame still
		/// went out, and a counter that stops updating whenever generation is skipped would sit
		/// on its last value through an entire pause menu.
		void NoteFramesDisplayed(u32 real, u32 generated)
		{
			s_fps_real += real;
			s_fps_generated += generated;

			const u64 now = Common::Timer::GetCurrentValue();
			if (s_fps_window_start == 0)
			{
				s_fps_window_start = now;
				return;
			}
			const double secs = Common::Timer::ConvertValueToSeconds(now - s_fps_window_start);
			if (secs < 1.0)
				return;

			s_display_fps.store(static_cast<float>((s_fps_real + s_fps_generated) / secs), std::memory_order_relaxed);
			s_fps_window_start = now;
			s_fps_real = 0;
			s_fps_generated = 0;
		}

		/// A plain layout transition on a whole colour image.
		///
		/// The ported passes handle their own barriers, but they speak Eden's convention where a
		/// presentable image lives in GENERAL. PCSX2's swap chain images are in PRESENT_SRC_KHR
		/// when we get them and must be back in it to be presented, so these bracket the calls.
		void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
			VkAccessFlags src_access, VkAccessFlags dst_access)
		{
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.srcAccessMask = src_access;
			barrier.dstAccessMask = dst_access;
			barrier.oldLayout = from;
			barrier.newLayout = to;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		void DestroyResources()
		{
			if (s_vk_device == VK_NULL_HANDLE)
				return;

			for (VkImageView view : s_storage_views)
			{
				if (view != VK_NULL_HANDLE)
					vkDestroyImageView(s_vk_device, view, nullptr);
			}
			s_storage_views.clear();

			for (VkSemaphore& sem : s_acquire_sems)
			{
				if (sem != VK_NULL_HANDLE)
					vkDestroySemaphore(s_vk_device, sem, nullptr);
				sem = VK_NULL_HANDLE;
			}
			for (VkSemaphore& sem : s_done_sems)
			{
				if (sem != VK_NULL_HANDLE)
					vkDestroySemaphore(s_vk_device, sem, nullptr);
				sem = VK_NULL_HANDLE;
			}
			if (s_cmd_pool != VK_NULL_HANDLE)
			{
				// Frees s_cmd with it.
				vkDestroyCommandPool(s_vk_device, s_cmd_pool, nullptr);
				s_cmd_pool = VK_NULL_HANDLE;
				s_cmd = VK_NULL_HANDLE;
			}
		}

		bool CreateResources(GSDeviceVK* dev, VKSwapChain* swap_chain)
		{
			const VkCommandPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
				VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, dev->GetGraphicsQueueFamilyIndex()};
			if (vkCreateCommandPool(s_vk_device, &pool_ci, nullptr, &s_cmd_pool) != VK_SUCCESS)
				return false;

			const VkCommandBufferAllocateInfo cmd_ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
				s_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
			if (vkAllocateCommandBuffers(s_vk_device, &cmd_ai, &s_cmd) != VK_SUCCESS)
				return false;

			VkSemaphoreCreateInfo sem_ci = {};
			sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			for (VkSemaphore& sem : s_acquire_sems)
			{
				if (vkCreateSemaphore(s_vk_device, &sem_ci, nullptr, &sem) != VK_SUCCESS)
					return false;
			}
			for (VkSemaphore& sem : s_done_sems)
			{
				if (vkCreateSemaphore(s_vk_device, &sem_ci, nullptr, &sem) != VK_SUCCESS)
					return false;
			}

			// A storage view per swap chain image. The swap chain's own views are created for the
			// colour-attachment format, which for an sRGB surface cannot be a storage image at all,
			// so these are separate rather than borrowed.
			const u32 count = swap_chain->GetImageCount();
			s_storage_views.assign(count, VK_NULL_HANDLE);
			for (u32 i = 0; i < count; i++)
			{
				VkImageViewCreateInfo ci = {};
				ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				ci.image = swap_chain->GetImage(i);
				ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				ci.format = s_format;
				ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				if (vkCreateImageView(s_vk_device, &ci, nullptr, &s_storage_views[i]) != VK_SUCCESS)
					return false;
			}
			return true;
		}
	} // namespace

	bool IsActive() { return s_active; }

	u32 GetMultiplier() { return s_active ? s_multiplier : 1u; }

	void Shutdown()
	{
		if (!s_active && s_cmd_pool == VK_NULL_HANDLE && !s_frame_gen)
			return;

		// Everything below is destroyed immediately rather than through PCSX2's deferred path, so
		// the device has to be idle first. FrameGen's destructor idles as well; this covers our
		// own command buffer and semaphores, which it knows nothing about.
		if (s_vk_device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(s_vk_device);

		s_frame_gen.reset();
		s_allocator.reset();
		s_device.reset();
		DestroyResources();

		s_active = false;
		s_multiplier = 1;
		s_performance_requested = false;
		s_flow_scale_percent = 100;
		s_extent = {};
		s_format = VK_FORMAT_UNDEFINED;
		s_frame_index = 0;
		s_vk_device = VK_NULL_HANDLE;

		s_display_fps.store(0.0f, std::memory_order_relaxed);
		s_fps_window_start = 0;
		s_fps_real = 0;
		s_fps_generated = 0;
	}

	bool Initialize(VKSwapChain* swap_chain, u32 multiplier)
	{
		if (!swap_chain || !g_gs_device || !IsAvailable())
			return false;

		GSDeviceVK* dev = GSDeviceVK::GetInstance();
		if (!dev)
			return false;

		multiplier = std::clamp<u32>(multiplier, 2, 4);
		const u8 flow_scale_percent = std::clamp<u8>(GSConfig.LsfgFlowScale, 25, 100);
		const VkExtent2D extent = {swap_chain->GetWidth(), swap_chain->GetHeight()};
		const VkFormat format = swap_chain->GetTextureFormat();

		if (s_active && extent.width == s_extent.width && extent.height == s_extent.height &&
			format == s_format && multiplier == s_multiplier &&
			GSConfig.LsfgPerformance == s_performance_requested && flow_scale_percent == s_flow_scale_percent)
		{
			return true; // idempotent; nothing changed
		}

		Shutdown();

		// The one hard prerequisite that cannot be recovered from here. The swap chain decides its
		// image usage at creation, and it only asks for STORAGE when frame generation was already
		// enabled — so switching the feature on mid-session needs the swap chain to be recreated
		// before this can succeed. Reported rather than retried, because recreating a swap chain
		// from inside a present is how you get a black screen.
		if (!swap_chain->IsStorageUsageAvailable())
		{
			Console.Warning("LSFG: swap chain has no STORAGE usage — frame generation needs a "
							"renderer restart to take effect.");
			s_init_failed.store(true, std::memory_order_relaxed);
			return false;
		}

		s_vk_device = dev->GetDevice();
		s_extent = extent;
		s_format = format;
		s_multiplier = multiplier;
		s_flow_scale_percent = flow_scale_percent;
		s_performance_requested = GSConfig.LsfgPerformance;

		if (!CreateResources(dev, swap_chain))
		{
			Console.Error("LSFG: failed to create frame-generation resources.");
			DestroyResources();
			s_init_failed.store(true, std::memory_order_relaxed);
			s_vk_device = VK_NULL_HANDLE;
			return false;
		}

		s_device.emplace(dev);
		// Both are required by the interpolation shaders and neither is core in Vulkan 1.1, so
		// GSDeviceVK requests them as extensions. It also clears the flag when a driver advertises
		// one without really supporting it, which is why these are asked of the DEVICE rather than
		// of vkGetPhysicalDeviceFeatures2 — see the note in LsfgVkCompat.cpp.
		if (!s_device->IsVulkanMemoryModelSupported() || !s_device->HasNullDescriptor())
		{
			Console.Warning("LSFG: device lacks the Vulkan memory model or nullDescriptor — "
							"frame generation unavailable.");
			s_device.reset();
			DestroyResources();
			s_init_failed.store(true, std::memory_order_relaxed);
			s_vk_device = VK_NULL_HANDLE;
			return false;
		}

		s_allocator.emplace(dev->GetAllocator());
		s_frame_gen.emplace(*s_allocator, dev);

		s_frame_index = 0;
		s_active = true;
		s_init_failed.store(false, std::memory_order_relaxed);
		Console.WriteLn("LSFG: frame generation active (%ux, %ux%u).", multiplier, extent.width, extent.height);
		return true;
	}

	bool PresentWithGeneration(
		VkQueue present_queue, VKSwapChain* swap_chain, VkSemaphore render_finished, bool frame_has_new_content)
	{
		if (!s_active || !swap_chain || !s_frame_gen)
			return false;

		// Nothing new to interpolate between. Pause menus, boot screens before the GS has any
		// output, and the blank frames a fade produces all land here — inventing motion across
		// them is wrong AND costs a full generation pass per frame to do it.
		if (!frame_has_new_content)
		{
			s_frame_index = 0;
			NoteFramesDisplayed(1, 0);
			return false;
		}

		// A resize between Initialize and here would have us reading mismatched extents. Decline
		// the frame; the caller presents normally and the next Initialize picks up the new size.
		if (swap_chain->GetWidth() != s_extent.width || swap_chain->GetHeight() != s_extent.height)
		{
			NoteFramesDisplayed(1, 0);
			return false;
		}

		const u32 real_index = swap_chain->GetCurrentImageIndex();
		if (real_index >= s_storage_views.size())
		{
			NoteFramesDisplayed(1, 0);
			return false;
		}
		const VkImage real_image = swap_chain->GetImage(real_index);

		// 1. Record the chain work for this frame, then ask how many frames to interpolate.
		//    WantedGenerations drives the PACER, which is the whole reason a game bouncing
		//    between 60 and 30fps does not judder here: the count varies to hold the presented
		//    rate steady rather than blindly multiplying whatever arrived.
		const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkResetCommandBuffer(s_cmd, 0);
		if (vkBeginCommandBuffer(s_cmd, &begin) != VK_SUCCESS)
			return false;

		// The ported passes expect a presentable image in GENERAL; PCSX2 hands it over in
		// PRESENT_SRC_KHR and needs it back that way.
		TransitionImage(s_cmd, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

		const Vulkan::vk::CommandBuffer cmdbuf{s_cmd};
		s_frame_gen->Process(*s_device, cmdbuf, real_image, s_storage_views[real_index], s_extent, s_format,
			s_extent);

		const size_t wanted = s_frame_gen->WantedGenerations(
			std::min<size_t>(s_multiplier - 1u, swap_chain->GetImageCount() - 1u));
		const size_t available = s_frame_gen->GeneratedFrameCount();
		const size_t generations = std::min(wanted, available);

		// 2. Acquire a swap chain image per generated frame and record its generation pass. The
		//    acquires happen BEFORE the single submit below because that submit has to wait on
		//    every acquire semaphore at once.
		u32 acquired_index[VideoCore::FrameGen::MAX_GENERATIONS] = {};
		size_t acquired = 0;
		for (size_t i = 0; i < generations && i < VideoCore::FrameGen::MAX_GENERATIONS; i++)
		{
			// ★ Bounded, but NOT zero. A zero timeout looks right — an interpolated frame is a
			// bonus, so why stall for one — and it silently disables the entire feature: under
			// FIFO the presentation engine hands an image back at a vblank, so at steady state
			// nothing is EVER free instantly, every acquire returns VK_NOT_READY, and every
			// generated frame is dropped. Observed exactly that on an Adreno 740. Waiting for a
			// display slot IS the mechanism: presenting two frames per rendered frame means
			// waiting for the second slot.
			static constexpr u64 kGeneratedAcquireTimeoutNs = 50ull * 1000 * 1000;
			u32 image_index = 0;
			const VkResult acq = vkAcquireNextImageKHR(s_vk_device, swap_chain->GetSwapChain(),
				kGeneratedAcquireTimeoutNs, s_acquire_sems[i], VK_NULL_HANDLE, &image_index);
			if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
				break; // nothing free, out of date, or lost — still present the real frame
			if (image_index >= s_storage_views.size())
				break;

			s_frame_gen->GenerateInto(*s_device, cmdbuf, swap_chain->GetImage(image_index),
				s_storage_views[image_index], i);
			// The generation pass leaves its target in GENERAL.
			TransitionImage(s_cmd, swap_chain->GetImage(image_index), VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);

			acquired_index[acquired++] = image_index;
		}

		TransitionImage(s_cmd, real_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);

		if (vkEndCommandBuffer(s_cmd) != VK_SUCCESS)
			return false;

		// 3. ONE submit. It waits on the caller's render-finished semaphore plus every acquire,
		//    and signals one semaphore per present that follows — see the note by s_cmd_pool for
		//    why this is not a submit per frame.
		VkSemaphore wait_sems[1 + VideoCore::FrameGen::MAX_GENERATIONS] = {};
		VkPipelineStageFlags wait_stages[1 + VideoCore::FrameGen::MAX_GENERATIONS] = {};
		u32 wait_count = 0;
		wait_sems[wait_count] = render_finished;
		wait_stages[wait_count++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		for (size_t i = 0; i < acquired; i++)
		{
			wait_sems[wait_count] = s_acquire_sems[i];
			wait_stages[wait_count++] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		}

		const u32 signal_count = static_cast<u32>(acquired) + 1u;
		VkSubmitInfo submit = {};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.waitSemaphoreCount = wait_count;
		submit.pWaitSemaphores = wait_sems;
		submit.pWaitDstStageMask = wait_stages;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &s_cmd;
		submit.signalSemaphoreCount = signal_count;
		submit.pSignalSemaphores = s_done_sems.data();

		if (vkQueueSubmit(GSDeviceVK::GetInstance()->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
			return false;

		s_frame_index++;

		// 4. Generated frames first — they sit between the previous real frame and this one, so
		//    they display first. Presents issued on one queue are processed in call order, which
		//    is what puts them on screen in that order without a semaphore between them.
		u32 presented_generated = 0;
		for (size_t i = 0; i < acquired; i++)
		{
			const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &s_done_sems[i], 1,
				swap_chain->GetSwapChainPtr(), &acquired_index[i], nullptr};
			// ★ VK_SUBOPTIMAL_KHR IS A SUCCESS CODE — the frame WAS presented. Treating it as
			// failure broke nothing visible and made the overlay lie: the generated frame reached
			// the screen, we stopped counting, and the display rate read exactly the real rate
			// forever. Suboptimal is routine on Android (rotation, insets, a driver preferring a
			// different transform), so it fired every frame.
			const VkResult pres = vkQueuePresentKHR(present_queue, &present);
			if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
				break;
			presented_generated++;
		}

		// 5. The real frame goes out last, after whatever generated frames made it.
		const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1,
			&s_done_sems[acquired], 1, swap_chain->GetSwapChainPtr(), &real_index, nullptr};
		swap_chain->ResetImageAcquireResult();
		vkQueuePresentKHR(present_queue, &present);
		NoteFramesDisplayed(1, presented_generated);
		return true;
	}
} // namespace GSLsfg

#endif // ARMSX2_HAS_LSFG
