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
#include <mutex>

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
		// Guards s_dll_path, s_dll_checked and s_dll_ok: SetDllPath() runs on the UI and the GS
		// thread, and GetUnavailableReason() reads from both.
		std::mutex s_dll_mutex;
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
		// fopen/fread/fseek/fread/fclose on the present path every single frame. Cleared by
		// SetDllPath() on a path change, and by InvalidateDllVerdict() when the file itself was
		// rewritten under an unchanged path.
		bool s_dll_checked = false;
		bool s_dll_ok = false;

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
		std::unique_lock lock(s_dll_mutex);
		if (s_dll_path == path)
			return;
		s_dll_path = std::move(path);
		s_dll_checked = false;
		// A new DLL deserves a fresh attempt; the previous failure may have been this file.
		s_init_failed.store(false, std::memory_order_relaxed);
		s_no_shaders.store(false, std::memory_order_relaxed);
	}

	void InvalidateDllVerdict()
	{
		std::unique_lock lock(s_dll_mutex);
		s_dll_checked = false;
	}

	// By value: a reference would outlive the lock.
	std::string GetDllPath()
	{
		std::unique_lock lock(s_dll_mutex);
		return s_dll_path;
	}

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

		{
			std::unique_lock lock(s_dll_mutex);
			if (s_dll_path.empty())
				return Unavailable::NoDll;
			if (!s_dll_checked)
			{
				s_dll_ok = LooksLikeLosslessDll(s_dll_path);
				s_dll_checked = true;
			}
			if (!s_dll_ok)
				return Unavailable::DllUnreadable;
		}
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

		/// Interpolated frames are generated into images WE own, then copied into the acquired
		/// swap chain image.
		///
		/// ★ The obvious shortcut — dispatch straight into the swap chain image through a storage
		/// view — is what crashed Turnip inside vkQueueSubmit, while the stock Qualcomm driver
		/// tolerated it. A WSI image is not an ordinary image: on Adreno it is typically
		/// UBWC-compressed, and a driver can advertise STORAGE_IMAGE on the format while its
		/// swap chain images cannot actually serve as storage targets. Recording succeeded and
		/// the fault only appeared when the driver walked the command stream at submit.
		///
		/// Neither Eden nor the previous implementation did that. Eden generates into its own
		/// frame pool and blits; the old AHardwareBuffer path generated into its own images and
		/// copied. This does the same, which costs one image copy per generated frame — the same
		/// copy the old path already paid — and in exchange asks nothing unusual of the WSI.
		struct GenImage
		{
			Vulkan::vk::Image image;
			VkImageView view = VK_NULL_HANDLE;
		};
		std::vector<GenImage> s_gen_images;

		/// Per-frame resources, ring-buffered.
		///
		/// ★ A SINGLE command buffer here is undefined behaviour, and it is what crashed the
		/// driver at vkQueueSubmit on the first frame that actually generated. Resetting a
		/// command buffer while it is still executing, and resubmitting one that is still
		/// pending, are both illegal — as is signalling a binary semaphore that the previous
		/// present has not consumed yet.
		///
		/// The old AHardwareBuffer implementation had exactly the same single-slot arrangement
		/// and got away with it, because it called vkQueueWaitIdle twice per frame. Removing
		/// those idles is the entire point of this port, and it also removed the accidental
		/// serialisation that made reuse safe. So the synchronisation has to be explicit now:
		/// one slot per swap chain image, each with its own fence, and a slot is not touched
		/// until its fence says the GPU is finished with it.
		struct FrameSlot
		{
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			/// ★ A FENCE, not a semaphore, for the extra acquires.
			///
			/// Waiting on an acquire semaphore inside our submit is the one thing that
			/// distinguishes a generating frame (waits=2) from a working one (waits=1), and it is
			/// where Turnip segfaults — after the storage-view theory was disproved by generating
			/// into our own images and copying, which changed nothing. Acquiring with a fence and
			/// blocking on it before the submit removes that wait, at the cost of a short CPU
			/// stall per generated frame. That is still far cheaper than the two full device
			/// idles per frame the old implementation paid.
			std::array<VkFence, VideoCore::FrameGen::MAX_GENERATIONS> acquire_fences = {};
			std::array<VkSemaphore, VideoCore::FrameGen::MAX_GENERATIONS + 1> done_sems = {};
			bool submitted = false; ///< false until the fence has ever been signalled
		};
		std::vector<FrameSlot> s_slots;
		VkCommandPool s_cmd_pool = VK_NULL_HANDLE;

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

			for (GenImage& g : s_gen_images)
			{
				if (g.view != VK_NULL_HANDLE)
					vkDestroyImageView(s_vk_device, g.view, nullptr);
				g.image = Vulkan::vk::Image(); // releases the VMA allocation
			}
			s_gen_images.clear();

			for (FrameSlot& slot : s_slots)
			{
				for (VkFence f : slot.acquire_fences)
				{
					if (f != VK_NULL_HANDLE)
						vkDestroyFence(s_vk_device, f, nullptr);
				}
				for (VkSemaphore sem : slot.done_sems)
				{
					if (sem != VK_NULL_HANDLE)
						vkDestroySemaphore(s_vk_device, sem, nullptr);
				}
				if (slot.fence != VK_NULL_HANDLE)
					vkDestroyFence(s_vk_device, slot.fence, nullptr);
			}
			s_slots.clear();

			if (s_cmd_pool != VK_NULL_HANDLE)
			{
				// Frees every command buffer allocated from it.
				vkDestroyCommandPool(s_vk_device, s_cmd_pool, nullptr);
				s_cmd_pool = VK_NULL_HANDLE;
			}
		}

		bool CreateResources(GSDeviceVK* dev, VKSwapChain* swap_chain)
		{
			const VkCommandPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
				VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, dev->GetGraphicsQueueFamilyIndex()};
			if (vkCreateCommandPool(s_vk_device, &pool_ci, nullptr, &s_cmd_pool) != VK_SUCCESS)
				return false;

			// One slot per swap chain image: that is the depth at which the presentation engine
			// can already be holding work, so it is the depth at which reuse becomes safe.
			const u32 count = swap_chain->GetImageCount();
			s_slots.resize(count);

			VkSemaphoreCreateInfo sem_ci = {};
			sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			VkFenceCreateInfo fence_ci = {};
			fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			for (FrameSlot& slot : s_slots)
			{
				const VkCommandBufferAllocateInfo cmd_ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					nullptr, s_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
				if (vkAllocateCommandBuffers(s_vk_device, &cmd_ai, &slot.cmd) != VK_SUCCESS)
					return false;
				// Created UNSIGNALLED, with `submitted` false — the first use must not wait on a
				// fence that has never been submitted, which would block forever.
				if (vkCreateFence(s_vk_device, &fence_ci, nullptr, &slot.fence) != VK_SUCCESS)
					return false;
				for (VkFence& f : slot.acquire_fences)
				{
					if (vkCreateFence(s_vk_device, &fence_ci, nullptr, &f) != VK_SUCCESS)
						return false;
				}
				for (VkSemaphore& sem : slot.done_sems)
				{
					if (vkCreateSemaphore(s_vk_device, &sem_ci, nullptr, &sem) != VK_SUCCESS)
						return false;
				}
			}

			// One generation image per slot, per interpolated frame. Per-SLOT is what makes reuse
			// safe: the fence ring already proves slot N's command buffer has finished before
			// slot N is touched again, and these are only ever referenced by that slot's buffer.
			const u32 per_frame = std::max<u32>(s_multiplier, 2u) - 1u;
			s_gen_images.resize(static_cast<size_t>(count) * per_frame);
			for (GenImage& g : s_gen_images)
			{
				VkImageCreateInfo ici = {};
				ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
				ici.imageType = VK_IMAGE_TYPE_2D;
				ici.format = s_format;
				ici.extent = {s_extent.width, s_extent.height, 1};
				ici.mipLevels = 1;
				ici.arrayLayers = 1;
				ici.samples = VK_SAMPLE_COUNT_1_BIT;
				ici.tiling = VK_IMAGE_TILING_OPTIMAL;
				// STORAGE to be dispatched into, TRANSFER_SRC to be copied out of. Ordinary
				// device-local images, which is the entire point — nothing here is a WSI image.
				ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

				g.image = s_allocator->CreateImage(ici);
				if (!g.image)
					return false;

				VkImageViewCreateInfo vci = {};
				vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				vci.image = *g.image;
				vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				vci.format = s_format;
				vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				if (vkCreateImageView(s_vk_device, &vci, nullptr, &g.view) != VK_SUCCESS)
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

		s_vk_device = dev->GetDevice();
		s_extent = extent;
		s_format = format;
		s_multiplier = multiplier;
		s_flow_scale_percent = flow_scale_percent;
		s_performance_requested = GSConfig.LsfgPerformance;

		// ★ ORDER MATTERS, and getting it wrong here is not a compile error.
		//
		// CreateResources allocates the generation images through s_allocator, so the allocator
		// and the device adapter must exist BEFORE it runs. They used to be constructed after it,
		// which dereferenced an empty std::optional and took the GS thread down during boot —
		// long before frame generation would ever have produced a frame, so it looked like an
		// entirely different bug from the one it followed.
		s_device.emplace(dev);

		// Both features are required by the interpolation shaders and neither is core in Vulkan
		// 1.1, so GSDeviceVK requests them as extensions. It also clears the flag when a driver
		// advertises one without really supporting it, which is why these are asked of the DEVICE
		// rather than of vkGetPhysicalDeviceFeatures2 — see the note in LsfgVkCompat.cpp.
		//
		// Checked before anything is allocated: there is no point building a chain for a device
		// that cannot run the shaders.
		if (!s_device->IsVulkanMemoryModelSupported() || !s_device->HasNullDescriptor())
		{
			Console.Warning("LSFG: device lacks the Vulkan memory model or nullDescriptor — "
							"frame generation unavailable.");
			s_device.reset();
			s_init_failed.store(true, std::memory_order_relaxed);
			s_vk_device = VK_NULL_HANDLE;
			return false;
		}

		s_allocator.emplace(dev->GetAllocator());

		if (!CreateResources(dev, swap_chain))
		{
			Console.Error("LSFG: failed to create frame-generation resources.");
			DestroyResources();
			s_allocator.reset();
			s_device.reset();
			s_init_failed.store(true, std::memory_order_relaxed);
			s_vk_device = VK_NULL_HANDLE;
			return false;
		}

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
		if (s_gen_images.empty())
		{
			NoteFramesDisplayed(1, 0);
			return false;
		}
		const VkImage real_image = swap_chain->GetImage(real_index);

		// 1. Record the chain work for this frame, then ask how many frames to interpolate.
		//    WantedGenerations drives the PACER, which is the whole reason a game bouncing
		//    between 60 and 30fps does not judder here: the count varies to hold the presented
		//    rate steady rather than blindly multiplying whatever arrived.
		// ★ Take this frame's slot and WAIT for the GPU to be done with it before touching
		// anything in it. Without this, the reset below hits a command buffer that may still be
		// executing and the submit below resubmits one that is still pending — both undefined,
		// and both were reliably fatal on the first frame that recorded a real dispatch chain.
		FrameSlot& slot = s_slots[s_frame_index % s_slots.size()];
		if (slot.submitted)
		{
			// Bounded rather than UINT64_MAX: a lost surface must not wedge the GS thread. If it
			// does expire, decline the frame — the caller still presents normally.
			static constexpr u64 kSlotTimeoutNs = 200ull * 1000 * 1000;
			if (vkWaitForFences(s_vk_device, 1, &slot.fence, VK_TRUE, kSlotTimeoutNs) != VK_SUCCESS)
			{
				NoteFramesDisplayed(1, 0);
				return false;
			}
		}
		vkResetFences(s_vk_device, 1, &slot.fence);

		const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkResetCommandBuffer(slot.cmd, 0);
		if (vkBeginCommandBuffer(slot.cmd, &begin) != VK_SUCCESS)
			return false;

		// The ported passes expect a presentable image in GENERAL; PCSX2 hands it over in
		// PRESENT_SRC_KHR and needs it back that way.
		TransitionImage(slot.cmd, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

		const Vulkan::vk::CommandBuffer cmdbuf{slot.cmd};
		// Process only null-checks the view (Eden uses it as a capability probe) — the presented
		// frame is COPIED into the chain, never written through a view. Hand it one of ours.
		s_frame_gen->Process(*s_device, cmdbuf, real_image, s_gen_images[0].view, s_extent, s_format,
			s_extent);

		// ★ The budget is what Vulkan allows us to HOLD, not the image count. Using
		// GetImageCount() - 1 here is what crashed the driver: with min=3 and 3 images the real
		// budget is zero, and acquiring anyway is undefined behaviour rather than a failed call.
		const size_t acquire_budget = swap_chain->GetExtraAcquirableImages();
		const size_t per_frame_images = std::max<u32>(s_multiplier, 2u) - 1u;
		static constexpr u64 kGeneratedAcquireTimeoutNs = 50ull * 1000 * 1000;
		const size_t wanted = s_frame_gen->WantedGenerations(
			std::min<size_t>(s_multiplier - 1u, acquire_budget));
		const size_t available = s_frame_gen->GeneratedFrameCount();
		const size_t generations = std::min(wanted, available);

		// 2. Acquire a swap chain image per generated frame and record its generation pass. The
		//    acquires happen BEFORE the single submit below because that submit has to wait on
		//    every acquire semaphore at once.
		u32 acquired_index[VideoCore::FrameGen::MAX_GENERATIONS] = {};
		size_t acquired = 0;
		for (size_t i = 0; i < generations && i < acquire_budget && i < VideoCore::FrameGen::MAX_GENERATIONS; i++)
		{
			// ★ Bounded, but NOT zero. A zero timeout looks right — an interpolated frame is a
			// bonus, so why stall for one — and it silently disables the entire feature: under
			// FIFO the presentation engine hands an image back at a vblank, so at steady state
			// nothing is EVER free instantly, every acquire returns VK_NOT_READY, and every
			// generated frame is dropped. Observed exactly that on an Adreno 740. Waiting for a
			// display slot IS the mechanism: presenting two frames per rendered frame means
			// waiting for the second slot.
			u32 image_index = 0;
			const VkResult acq = vkAcquireNextImageKHR(s_vk_device, swap_chain->GetSwapChain(),
				kGeneratedAcquireTimeoutNs, VK_NULL_HANDLE, slot.acquire_fences[i], &image_index);
			if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
				break; // nothing free, out of date, or lost — still present the real frame
			if (image_index >= swap_chain->GetImageCount())
				break;

			// Block until the image is genuinely ours before recording anything that touches it.
			// This is what the semaphore wait in the submit used to do.
			if (vkWaitForFences(s_vk_device, 1, &slot.acquire_fences[i], VK_TRUE,
					kGeneratedAcquireTimeoutNs) != VK_SUCCESS)
				break;
			vkResetFences(s_vk_device, 1, &slot.acquire_fences[i]);

			GenImage& gen = s_gen_images[(s_frame_index % s_slots.size()) * per_frame_images + i];
			// Generate into OUR image, not the swap chain's — see the note on s_gen_images.
			s_frame_gen->GenerateInto(*s_device, cmdbuf, *gen.image, gen.view, i);
			// The pass leaves our image in GENERAL. Copy it into the acquired swap chain image,
			// then hand that back to the presentation engine.
			VkImage dst = swap_chain->GetImage(image_index);
			TransitionImage(slot.cmd, *gen.image, VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

			VkImageCopy region = {};
			region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.extent = {s_extent.width, s_extent.height, 1};
			vkCmdCopyImage(slot.cmd, *gen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);

			acquired_index[acquired++] = image_index;
		}

		TransitionImage(slot.cmd, real_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);

		if (vkEndCommandBuffer(slot.cmd) != VK_SUCCESS)
			return false;

		// 3. ONE submit. It waits on the caller's render-finished semaphore plus every acquire,
		//    and signals one semaphore per present that follows — see the note by s_cmd_pool for
		//    why this is not a submit per frame.
		VkSemaphore wait_sems[1 + VideoCore::FrameGen::MAX_GENERATIONS] = {};
		VkPipelineStageFlags wait_stages[1 + VideoCore::FrameGen::MAX_GENERATIONS] = {};
		u32 wait_count = 0;
		wait_sems[wait_count] = render_finished;
		wait_stages[wait_count++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		const u32 signal_count = static_cast<u32>(acquired) + 1u;
		VkSubmitInfo submit = {};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.waitSemaphoreCount = wait_count;
		submit.pWaitSemaphores = wait_sems;
		submit.pWaitDstStageMask = wait_stages;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &slot.cmd;
		submit.signalSemaphoreCount = signal_count;
		submit.pSignalSemaphores = slot.done_sems.data();

		// The fence is what lets this slot be reused safely N frames from now.
		if (vkQueueSubmit(GSDeviceVK::GetInstance()->GetGraphicsQueue(), 1, &submit, slot.fence) != VK_SUCCESS)
			return false;
		slot.submitted = true;

		s_frame_index++;

		// 4. Generated frames first — they sit between the previous real frame and this one, so
		//    they display first. Presents issued on one queue are processed in call order, which
		//    is what puts them on screen in that order without a semaphore between them.
		u32 presented_generated = 0;
		for (size_t i = 0; i < acquired; i++)
		{
			const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &slot.done_sems[i], 1,
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
			&slot.done_sems[acquired], 1, swap_chain->GetSwapChainPtr(), &real_index, nullptr};
		swap_chain->ResetImageAcquireResult();
		vkQueuePresentKHR(present_queue, &present);
		NoteFramesDisplayed(1, presented_generated);
		return true;
	}
} // namespace GSLsfg

#endif // ARMSX2_HAS_LSFG
