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
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKSwapChain.h"

#include "imgui.h"

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
		/// Set when the swap chain cannot spare an image for generated frames. Distinct from
		/// "failed": everything initialised, there is simply nowhere to put a generated frame.
		std::atomic<bool> s_no_headroom{false};
		std::atomic<bool> s_no_shaders{false};
		/// The half-precision shaders are the ones running, for the overlay: "LSFG: 60.00 fp16".
		std::atomic<bool> s_fp16_active{false};
		/// Frame generation's own GPU time per frame, ms, averaged over a second; 0 when unmeasured.
		std::atomic<float> s_gpu_ms{0.0f};
		// Why the shaders failed, in a few words, for the overlay; set with s_no_shaders.
		std::mutex s_failure_mutex;
		std::string s_failure_reason;
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
		std::unique_lock failure_lock(s_failure_mutex);
		s_failure_reason.clear();
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
			// Adreno 7xx or newer, or a 6xx on Turnip (GSDeviceVK reports 6 only for that
			// pairing). Asked of the resolved architecture rather than a GL_RENDERER substring
			// search, for the same reason the Mali workarounds moved into the driver database: a
			// parsed generation can say "7xx and up", a substring cannot.
			if (s_adreno_generation.load(std::memory_order_relaxed) < 6)
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
			case Unavailable::GpuUnsupported: return "requires an Adreno 7xx or newer GPU, or an Adreno 6xx on Turnip";
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
				// Lossless Scaling (or says what else stopped them), "failed" means this device or
				// driver refused.
				if (s_no_shaders.load(std::memory_order_relaxed))
				{
					std::unique_lock lock(s_failure_mutex);
					return s_failure_reason.empty() ? "LSFG: no shaders" : fmt::format("LSFG: no shaders ({})", s_failure_reason);
				}
				return "LSFG: failed";
			default:
				return "LSFG: unavailable";
		}

		if (s_no_headroom.load(std::memory_order_relaxed))
			return "LSFG: no display headroom";

		// Available but no window has closed yet: bring-up, or the first second of a session.
		const float fps = s_display_fps.load(std::memory_order_relaxed);
		if (fps <= 0.0f)
			return "LSFG: starting";
		// "LSFG: 60.00 fp16 · 1.23 ms": the display rate, the shader family when it is fp16, and
		// frame generation's own GPU time per frame.
		std::string text = fmt::format("LSFG: {:.2f}", fps);
		if (s_fp16_active.load(std::memory_order_relaxed))
			text += " fp16";
		if (const float gpu_ms = s_gpu_ms.load(std::memory_order_relaxed); gpu_ms > 0.0f)
			text += fmt::format(" \u00b7 {:.2f} ms", gpu_ms);
		return text;
	}

	bool StatusIsProblem()
	{
		return GSConfig.LsfgEnabled &&
			   (GetUnavailableReason() != Unavailable::Available || s_no_headroom.load(std::memory_order_relaxed));
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
	void CaptureGameFrame(VkCommandBuffer, const GameFrame*) {}
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
		bool s_fp16_requested = false;
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

		/// Frame generation on the game's own image (CaptureGameFrame). The game frame is copied
		/// into s_capture, screen-sized so it never has to be replaced as the game's resolution
		/// changes: the copy uses its top-left s_capture_extent. That size follows the BIGGEST frame
		/// seen, so a game switching resolution back and forth doesn't rebuild the passes every
		/// time; a smaller frame is stretched to it, and the present stretches it back. s_captured
		/// says this frame's copy is recorded, with where and how the present drew the frame, for
		/// PresentWithGeneration to repeat on the generated ones.
		GenImage s_capture;
		VkExtent2D s_capture_extent = {};
		VkExtent2D s_peak_game = {};
		bool s_captured = false;
		float s_draw_rect[4] = {};
		bool s_draw_linear = false;

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
			bool timed = false; ///< its timestamps were written by the last submit, to be read
			/// This frame's ImGui overlay, copied for drawing onto its generated frames: the GS
			/// reclaims its own copy when ITS frame is done, which can be before this slot is.
			Vulkan::vk::Buffer overlay_vertices;
			Vulkan::vk::Buffer overlay_indices;
			VkDeviceSize overlay_vertex_bytes = 0;
			VkDeviceSize overlay_index_bytes = 0;
			/// Descriptor sets for the overlay's textures, when the device does not push them.
			VkDescriptorPool overlay_pool = VK_NULL_HANDLE;
		};
		std::vector<FrameSlot> s_slots;
		VkCommandPool s_cmd_pool = VK_NULL_HANDLE;
		/// For drawing the ImGui overlay onto a generated frame: the GS's own non-clearing pass for
		/// swap chain images, the one its ImGui pipeline and the images' framebuffers were made with.
		/// The GS owns it.
		VkRenderPass s_overlay_pass = VK_NULL_HANDLE;
		/// One ImGui draw of this frame's overlay, prepared once (PrepareOverlay) and recorded onto
		/// each generated frame (DrawOverlay).
		struct OverlayDraw
		{
			VkRect2D scissor;
			VkImageView view; ///< pushed, when the device pushes descriptors
			VkImageLayout layout;
			VkDescriptorSet set; ///< bound, when it doesn't
			u32 index_count;
			u32 first_index;
			s32 vertex_offset;
		};
		std::vector<OverlayDraw> s_overlay_draws;
		/// Descriptor sets per slot for the overlay's textures, one per change of texture between
		/// draws. The OSD's text is all one font texture; running out only means that frame
		/// generates from the finished screen instead.
		constexpr u32 kOverlaySets = 256;
		/// What frame generation last ran on, for the log: the game's image, or the finished screen.
		VkExtent2D s_last_run_extent = {};
		bool s_last_run_game = false;
		/// LSFG's own GPU time: a timestamp at each end of a frame's command buffer (everything it
		/// does for that frame, the passes over the real frame and every generated one with its
		/// copy), read back once the slot's fence has signalled, so it never waits for the GPU.
		/// Averaged over the display-rate window, for the overlay and the log.
		VkQueryPool s_query_pool = VK_NULL_HANDLE;
		double s_ns_per_tick = 0.0;
		u64 s_tick_mask = 0;
		double s_gpu_ns_sum = 0.0;
		u32 s_gpu_samples = 0;
		u32 s_gpu_log_windows = 0;

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
			const float gpu_ms = s_gpu_samples ? static_cast<float>(s_gpu_ns_sum / s_gpu_samples / 1e6) : 0.0f;
			s_gpu_ms.store(gpu_ms, std::memory_order_relaxed);
			s_gpu_ns_sum = 0.0;
			s_gpu_samples = 0;
			// In the log every ten seconds, so a session can be measured without the overlay.
			if (gpu_ms > 0.0f && ++s_gpu_log_windows >= 10)
			{
				s_gpu_log_windows = 0;
				Console.WriteLn("LSFG GPU: %.2f ms per frame (%ux%u %s%s).", gpu_ms, s_last_run_extent.width,
					s_last_run_extent.height, s_last_run_game ? "game image" : "screen",
					s_fp16_active.load(std::memory_order_relaxed) ? ", fp16" : "");
			}
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

		/// Room for this frame's overlay in one of a slot's buffers, grown in steps so a busier
		/// overlay doesn't allocate every frame. Only called once the slot's fence has signalled, so
		/// a buffer it replaces is no longer read.
		bool ReserveOverlayBuffer(Vulkan::vk::Buffer& buffer, VkDeviceSize& capacity, VkDeviceSize needed,
			VkBufferUsageFlags usage)
		{
			if (buffer && capacity >= needed)
				return true;
			VkDeviceSize size = 64 * 1024;
			while (size < needed)
				size *= 2;
			VkBufferCreateInfo ci = {};
			ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			ci.size = size;
			ci.usage = usage;
			ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			buffer = s_allocator->CreateBuffer(ci, Vulkan::MemoryUsage::Upload);
			capacity = (buffer && buffer.Mapped().data()) ? size : 0;
			return capacity != 0;
		}

		/// This frame's ImGui overlay, for the generated frames: the game image they are made from has
		/// none, so each gets it drawn on top afresh, exactly as GSDeviceVK::RenderImGui drew it on the
		/// real frame. Nothing is stepped: the draw data ImGui::Render made for this frame is valid until
		/// the next ImGuiManager::NewFrame, which comes after this present. It is copied into the slot's
		/// own buffers, because the GS reclaims its stream buffers when ITS frame is done, which can be
		/// before this slot is. False when there is something to draw and it can't be, and frame
		/// generation then works on the finished screen, where the GS drew the overlay itself.
		bool PrepareOverlay(FrameSlot& slot)
		{
			s_overlay_draws.clear();
			if (!ImGui::GetCurrentContext())
				return true;
			const ImDrawData* const data = ImGui::GetDrawData();
			if (!data || !data->Valid || data->CmdLists.Size == 0 || data->TotalIdxCount <= 0)
				return true;
			if (s_overlay_pass == VK_NULL_HANDLE)
				return false;

			static_assert(sizeof(ImDrawIdx) == sizeof(u16));
			if (!ReserveOverlayBuffer(slot.overlay_vertices, slot.overlay_vertex_bytes,
					sizeof(ImDrawVert) * static_cast<VkDeviceSize>(data->TotalVtxCount), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
				!ReserveOverlayBuffer(slot.overlay_indices, slot.overlay_index_bytes,
					sizeof(ImDrawIdx) * static_cast<VkDeviceSize>(data->TotalIdxCount), VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
			{
				return false;
			}

			GSDeviceVK* const dev = GSDeviceVK::GetInstance();
			const bool push = dev->UsesPushDescriptors();
			if (!push)
				vkResetDescriptorPool(s_vk_device, slot.overlay_pool, 0);

			u8* const vertices = slot.overlay_vertices.Mapped().data();
			u8* const indices = slot.overlay_indices.Mapped().data();
			u32 vertex_base = 0;
			u32 index_base = 0;
			// Like RenderImGui, a draw without a texture keeps the one before it.
			VkImageView view = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkDescriptorSet set = VK_NULL_HANDLE;
			for (int n = 0; n < data->CmdLists.Size; n++)
			{
				const ImDrawList* const list = data->CmdLists[n];
				std::memcpy(vertices + vertex_base * sizeof(ImDrawVert), list->VtxBuffer.Data,
					sizeof(ImDrawVert) * static_cast<size_t>(list->VtxBuffer.Size));
				std::memcpy(indices + index_base * sizeof(ImDrawIdx), list->IdxBuffer.Data,
					sizeof(ImDrawIdx) * static_cast<size_t>(list->IdxBuffer.Size));

				for (int cmd_i = 0; cmd_i < list->CmdBuffer.Size; cmd_i++)
				{
					const ImDrawCmd* const pcmd = &list->CmdBuffer[cmd_i];
					if (pcmd->UserCallback)
						continue;

					const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
					if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
						continue;

					if (const GSTextureVK* const tex = reinterpret_cast<const GSTextureVK*>(pcmd->GetTexID()))
					{
						if (tex->GetView() != view)
						{
							view = tex->GetView();
							layout = tex->GetVkLayout();
							if (!push)
							{
								const VkDescriptorSetLayout ds_layout = dev->GetUtilityDescriptorSetLayout();
								const VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
									nullptr, slot.overlay_pool, 1, &ds_layout};
								if (vkAllocateDescriptorSets(s_vk_device, &ai, &set) != VK_SUCCESS)
								{
									s_overlay_draws.clear();
									return false;
								}
								const VkDescriptorImageInfo info = {dev->GetLinearSampler(), view, layout};
								const VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0,
									1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &info, nullptr, nullptr};
								vkUpdateDescriptorSets(s_vk_device, 1, &write, 0, nullptr);
							}
						}
					}
					if (view == VK_NULL_HANDLE)
						continue;

					const GSVector4i scissor = GSVector4i(clip).max_i32(GSVector4i::zero());
					OverlayDraw& draw = s_overlay_draws.emplace_back();
					draw.scissor = {{scissor.left, scissor.top},
						{static_cast<u32>(scissor.width()), static_cast<u32>(scissor.height())}};
					draw.view = view;
					draw.layout = layout;
					draw.set = set;
					draw.index_count = pcmd->ElemCount;
					draw.first_index = index_base + pcmd->IdxOffset;
					draw.vertex_offset = static_cast<s32>(vertex_base + pcmd->VtxOffset);
				}

				vertex_base += static_cast<u32>(list->VtxBuffer.Size);
				index_base += static_cast<u32>(list->IdxBuffer.Size);
			}
			slot.overlay_vertices.Flush();
			slot.overlay_indices.Flush();
			return true;
		}

		/// Draws the prepared overlay onto swap chain image `index`, which must be in
		/// COLOR_ATTACHMENT_OPTIMAL, and leaves it there. The state RenderImGui draws in: the present
		/// pass's full-screen viewport, a scissor per draw, the linear sampler.
		void DrawOverlay(VkCommandBuffer cmd, VkFramebuffer framebuffer, const FrameSlot& slot)
		{
			GSDeviceVK* const dev = GSDeviceVK::GetInstance();
			const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, s_overlay_pass,
				framebuffer, {{0, 0}, s_extent}, 0, nullptr};
			vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

			const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(s_extent.width),
				static_cast<float>(s_extent.height), 0.0f, 1.0f};
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			// Dynamic in the ImGui pipeline, so they have to be set, though its blend never reads them.
			const float blend_constants[4] = {};
			vkCmdSetBlendConstants(cmd, blend_constants);
			vkCmdSetLineWidth(cmd, 1.0f);

			const VkPipelineLayout layout = dev->GetUtilityPipelineLayout();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->GetImGuiPipeline());
			const float uniforms[4] = {2.0f / static_cast<float>(s_extent.width),
				2.0f / static_cast<float>(s_extent.height), -1.0f, -1.0f};
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				sizeof(uniforms), uniforms);
			const VkBuffer vertex_buffer = *slot.overlay_vertices;
			const VkDeviceSize vertex_offset = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &vertex_offset);
			vkCmdBindIndexBuffer(cmd, *slot.overlay_indices, 0, VK_INDEX_TYPE_UINT16);

			const bool push = dev->UsesPushDescriptors();
			VkImageView bound = VK_NULL_HANDLE;
			for (const OverlayDraw& draw : s_overlay_draws)
			{
				vkCmdSetScissor(cmd, 0, 1, &draw.scissor);
				if (draw.view != bound)
				{
					if (push)
					{
						const VkDescriptorImageInfo info = {dev->GetLinearSampler(), draw.view, draw.layout};
						const VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE,
							0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &info, nullptr, nullptr};
						vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
					}
					else
					{
						vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &draw.set, 0, nullptr);
					}
					bound = draw.view;
				}
				vkCmdDrawIndexed(cmd, draw.index_count, 1, draw.first_index, draw.vertex_offset, 0);
			}

			vkCmdEndRenderPass(cmd);
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
			s_capture.image = Vulkan::vk::Image();
			s_capture_extent = {};
			s_captured = false;

			for (FrameSlot& slot : s_slots)
			{
				if (slot.overlay_pool != VK_NULL_HANDLE)
					vkDestroyDescriptorPool(s_vk_device, slot.overlay_pool, nullptr);
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
			s_overlay_pass = VK_NULL_HANDLE;
			s_overlay_draws.clear();

			if (s_query_pool != VK_NULL_HANDLE)
			{
				vkDestroyQueryPool(s_vk_device, s_query_pool, nullptr);
				s_query_pool = VK_NULL_HANDLE;
			}
			s_gpu_ns_sum = 0.0;
			s_gpu_samples = 0;
			s_gpu_ms.store(0.0f, std::memory_order_relaxed);

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

			// The game frame is copied into this (CaptureGameFrame), and frame generation copies it out
			// again at the start of its passes. Not fatal: without it frame generation works on the
			// finished screen, as it always did.
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
				ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				s_capture.image = s_allocator->CreateImage(ici);
				if (!s_capture.image)
					Console.Warning("LSFG: no image for the game frame; generating from the finished screen.");
			}

			// The ImGui overlay on generated frames, drawn with the GS's ImGui pipeline in the GS's own
			// pass for swap chain images (the same key GSTextureVK::GetFramebuffer builds with). Not
			// fatal either: without it the game's own image is not used while the overlay has
			// something on screen, and generation works on the finished screen then.
			s_overlay_pass = dev->GetImGuiPipeline() != VK_NULL_HANDLE ?
								 dev->GetRenderPass(s_format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
									 VK_ATTACHMENT_STORE_OP_STORE) :
								 VK_NULL_HANDLE;
			if (s_overlay_pass != VK_NULL_HANDLE && !dev->UsesPushDescriptors())
			{
				const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kOverlaySets};
				const VkDescriptorPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
					kOverlaySets, 1, &size};
				for (FrameSlot& slot : s_slots)
				{
					if (vkCreateDescriptorPool(s_vk_device, &pool_ci, nullptr, &slot.overlay_pool) != VK_SUCCESS)
					{
						s_overlay_pass = VK_NULL_HANDLE;
						break;
					}
				}
			}
			if (s_overlay_pass == VK_NULL_HANDLE)
				Console.Warning("LSFG: can't draw the overlay on generated frames; the finished screen is used while it is up.");

			// Two timestamps per slot for frame generation's own GPU time, where the graphics queue
			// keeps them (the same test GSDeviceVK makes for its GPU timing). Without, there is
			// simply no number on the overlay.
			const VkPhysicalDeviceLimits& limits = dev->GetDeviceProperties().limits;
			u32 family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(dev->GetPhysicalDevice(), &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(dev->GetPhysicalDevice(), &family_count, families.data());
			const u32 family = dev->GetGraphicsQueueFamilyIndex();
			const u32 valid_bits = family < family_count ? families[family].timestampValidBits : 0;
			if (limits.timestampComputeAndGraphics && limits.timestampPeriod > 0.0f && valid_bits > 0)
			{
				const VkQueryPoolCreateInfo query_ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
					VK_QUERY_TYPE_TIMESTAMP, 2 * count, 0};
				if (vkCreateQueryPool(s_vk_device, &query_ci, nullptr, &s_query_pool) == VK_SUCCESS)
				{
					s_ns_per_tick = limits.timestampPeriod;
					s_tick_mask = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1);
				}
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
		s_fp16_requested = false;
		s_fp16_active.store(false, std::memory_order_relaxed);
		s_flow_scale_percent = 100;
		s_extent = {};
		s_format = VK_FORMAT_UNDEFINED;
		s_frame_index = 0;
		s_vk_device = VK_NULL_HANDLE;
		s_peak_game = {};
		s_captured = false;

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
			GSConfig.LsfgPerformance == s_performance_requested && GSConfig.LsfgFp16 == s_fp16_requested &&
			flow_scale_percent == s_flow_scale_percent)
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
		s_fp16_requested = GSConfig.LsfgFp16;

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

		// ★ The swap chain only asks for the extra images frame generation needs when
		// GSConfig.LsfgEnabled was true AT CREATION (see VKSwapChain::CreateSwapChain). Enabling
		// LSFG per-game turns it on LONG after that -- observed 17 seconds after the swap chain
		// was built -- so the chain was sized without the extra image, GetExtraAcquirableImages()
		// is 0, and every generation is silently skipped while this function still reports
		// "active". Display rate then reads exactly the real rate forever, with nothing anywhere
		// admitting why.
		//
		// Rebuild it now that the setting is actually on. Same size, so this is only about the
		// image count. If the driver still will not give us headroom, say so rather than claiming
		// to be running.
		//
		// And rebuild it when it is not FIFO. CreateSwapChain forces FIFO for frame generation, but
		// again only when it was on AT CREATION. With vsync off the chain is MAILBOX with three
		// images; on a driver whose minimum is two that already leaves one spare, so the check
		// above never fired and generation ran in MAILBOX, where the real frame presented right
		// after each generated one replaces it: every generated frame paid for, none displayed.
		// (ARMSX3 hit the same in its a909a0d3a.)
		if (swap_chain->GetExtraAcquirableImages() == 0 || swap_chain->GetPresentMode() != VK_PRESENT_MODE_FIFO_KHR)
		{
			Console.WriteLn("LSFG: swap chain has no spare images or is not FIFO; rebuilding it.");
			// Scale passed through explicitly: ResizeSwapChain defaults it to 1.0, which would
			// silently drop a non-default surface scale while we are only after the image count.
			if (!swap_chain->ResizeSwapChain(swap_chain->GetWidth(), swap_chain->GetHeight(),
					swap_chain->GetScale()) ||
				swap_chain->GetExtraAcquirableImages() == 0)
			{
				Console.Error("LSFG: the display cannot spare an image for generated frames.");
				s_no_headroom.store(true, std::memory_order_relaxed);
				s_active = false;
				DestroyResources();
				s_frame_gen.reset();
				s_allocator.reset();
				s_device.reset();
				s_vk_device = VK_NULL_HANDLE;
				return false;
			}
		}
		s_no_headroom.store(false, std::memory_order_relaxed);

		Console.WriteLn("LSFG: frame generation active (%ux, %ux%u).", multiplier, extent.width, extent.height);
		return true;
	}

	void CaptureGameFrame(VkCommandBuffer cmd, const GameFrame* frame)
	{
		s_captured = false;
		if (!frame || !frame->image || !s_active || !s_capture.image || s_extent.width == 0 || s_extent.height == 0)
			return;
		const s32 game_w = frame->src[2] - frame->src[0];
		const s32 game_h = frame->src[3] - frame->src[1];
		if (game_w <= 0 || game_h <= 0)
			return;

		// The biggest frame seen, shrunk to fit the screen: past the screen's size there is nothing
		// to gain, and following the biggest rather than the current one keeps a game that switches
		// resolution from rebuilding the passes every time it does.
		s_peak_game.width = std::max(s_peak_game.width, static_cast<u32>(game_w));
		s_peak_game.height = std::max(s_peak_game.height, static_cast<u32>(game_h));
		const float fit = std::min({1.0f, static_cast<float>(s_extent.width) / static_cast<float>(s_peak_game.width),
			static_cast<float>(s_extent.height) / static_cast<float>(s_peak_game.height)});
		const VkExtent2D extent = {std::max(1u, static_cast<u32>(static_cast<float>(s_peak_game.width) * fit)),
			std::max(1u, static_cast<u32>(static_cast<float>(s_peak_game.height) * fit))};

		// The last reader of this image was frame generation, in an earlier submission; a barrier's
		// first scope takes in everything earlier in submission order, which is what orders this
		// write after that read. The old contents are not wanted.
		TransitionImage(cmd, *s_capture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
			VK_ACCESS_TRANSFER_WRITE_BIT);
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {frame->src[0], frame->src[1], 0};
		blit.srcOffsets[1] = {frame->src[2], frame->src[3], 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstOffsets[0] = {0, 0, 0};
		blit.dstOffsets[1] = {static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1};
		// A straight copy stays exact; only a frame that has to be scaled is filtered.
		const bool same_size = extent.width == static_cast<u32>(game_w) && extent.height == static_cast<u32>(game_h);
		vkCmdBlitImage(cmd, frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *s_capture.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, same_size ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		// GENERAL, which frame generation's copy at the start of its passes expects.
		TransitionImage(cmd, *s_capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

		s_capture_extent = extent;
		std::copy(std::begin(frame->draw), std::end(frame->draw), s_draw_rect);
		s_draw_linear = frame->linear;
		s_captured = true;
	}

	bool PresentWithGeneration(
		VkQueue present_queue, VKSwapChain* swap_chain, VkSemaphore render_finished, bool frame_has_new_content)
	{
		// Consumed on every call, generating or not, so a copy made for a frame that ends up declined
		// can never be read as a later frame's.
		const bool captured = std::exchange(s_captured, false);

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
		const u32 slot_index = static_cast<u32>(s_frame_index % s_slots.size());
		FrameSlot& slot = s_slots[slot_index];
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
		// The slot's last frame is finished, so its timestamps are ready: no wait.
		if (slot.timed)
		{
			u64 ticks[2] = {};
			if (vkGetQueryPoolResults(s_vk_device, s_query_pool, slot_index * 2, 2, sizeof(ticks), ticks,
					sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			{
				s_gpu_ns_sum += static_cast<double>((ticks[1] - ticks[0]) & s_tick_mask) * s_ns_per_tick;
				s_gpu_samples++;
			}
			slot.timed = false;
		}

		// The game's own image, when this frame's overlay can be drawn onto each generated frame; the
		// finished screen, which has the overlay, when it can't.
		const bool game_res = captured && PrepareOverlay(slot);

		const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkResetCommandBuffer(slot.cmd, 0);
		if (vkBeginCommandBuffer(slot.cmd, &begin) != VK_SUCCESS)
			return false;
		if (s_query_pool != VK_NULL_HANDLE)
		{
			vkCmdResetQueryPool(slot.cmd, s_query_pool, slot_index * 2, 2);
			vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_query_pool, slot_index * 2);
		}

		const Vulkan::vk::CommandBuffer cmdbuf{slot.cmd};
		if (game_res)
		{
			// The game's own frame, copied by CaptureGameFrame in this frame's command buffer. This
			// barrier extends the render-finished wait, which covers only colour-attachment output,
			// to the copy frame generation begins with, and makes the captured frame visible to it.
			TransitionImage(slot.cmd, *s_capture.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			s_frame_gen->Process(*s_device, cmdbuf, *s_capture.image, s_gen_images[0].view, s_capture_extent,
				s_format, s_capture_extent);
			s_last_run_extent = s_capture_extent;
		}
		else
		{
			// The ported passes expect a presentable image in GENERAL; PCSX2 hands it over in
			// PRESENT_SRC_KHR and needs it back that way.
			TransitionImage(slot.cmd, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

			// Process only null-checks the view (Eden uses it as a capability probe) — the presented
			// frame is COPIED into the chain, never written through a view. Hand it one of ours.
			//
			// The last argument is the size the game was really drawn at, which the automatic flow scale
			// (the default, 100%) compares with the screen, so the optical flow runs at about the detail
			// the frame has. It was passed the screen size, the ratio was always 1, and the default was
			// the most expensive setting there is. The frame just presented is the GS device's current.
			VkExtent2D guest_extent = s_extent;
			if (const GSTexture* const current = g_gs_device->GetCurrent())
				guest_extent = {static_cast<u32>(current->GetWidth()), static_cast<u32>(current->GetHeight())};
			s_frame_gen->Process(*s_device, cmdbuf, real_image, s_gen_images[0].view, s_extent, s_format,
				guest_extent);
			s_last_run_extent = s_extent;
		}
		s_last_run_game = game_res;
		s_fp16_active.store(s_frame_gen->UsingFp16(), std::memory_order_relaxed);

		// Frame generation turned itself off: its shaders could not be loaded or built. That used
		// to be silent, the overlay reading the real rate as if all were well (#626, an Adreno 650
		// under Turnip). Say why, once, on the overlay and in the log, and let the settings screen
		// report it as the failure it is.
		if (s_frame_gen->IsUnavailable() && !s_no_shaders.load(std::memory_order_relaxed))
		{
			const char* why = s_frame_gen->UnavailableReason();
			Console.Error("LSFG: frame generation turned off: %s.", why);
			{
				std::unique_lock lock(s_failure_mutex);
				s_failure_reason = why;
			}
			s_no_shaders.store(true, std::memory_order_relaxed);
			s_init_failed.store(true, std::memory_order_relaxed);
		}

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

			if (game_res)
			{
				// Generated at the game's size: put it on screen the way the real frame went, the same
				// scaled blit into the same rectangle, on black where the frame doesn't reach (the
				// acquired image holds whatever it last showed).
				const auto edge = [](float v, u32 limit) {
					return static_cast<s32>(std::clamp<long>(std::lround(v), 0, static_cast<long>(limit)));
				};
				const s32 x0 = edge(s_draw_rect[0], s_extent.width), y0 = edge(s_draw_rect[1], s_extent.height);
				const s32 x1 = edge(s_draw_rect[2], s_extent.width), y1 = edge(s_draw_rect[3], s_extent.height);
				if (x0 > 0 || y0 > 0 || x1 < static_cast<s32>(s_extent.width) || y1 < static_cast<s32>(s_extent.height))
				{
					const VkClearColorValue black = {};
					const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
					vkCmdClearColorImage(slot.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
					TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
				}
				if (x1 > x0 && y1 > y0)
				{
					VkImageBlit blit = {};
					blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
					blit.srcOffsets[1] = {static_cast<s32>(s_capture_extent.width), static_cast<s32>(s_capture_extent.height), 1};
					blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
					blit.dstOffsets[0] = {x0, y0, 0};
					blit.dstOffsets[1] = {x1, y1, 1};
					vkCmdBlitImage(slot.cmd, *gen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, s_draw_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
				}
			}
			else
			{
				VkImageCopy region = {};
				region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.extent = {s_extent.width, s_extent.height, 1};
				vkCmdCopyImage(slot.cmd, *gen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			}

			// The overlay on top, afresh: the game image had none.
			const VkFramebuffer overlay_fb = (game_res && !s_overlay_draws.empty()) ?
												 swap_chain->GetTexture(image_index)->GetFramebuffer(false) :
												 VK_NULL_HANDLE;
			if (overlay_fb != VK_NULL_HANDLE)
			{
				TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
				DrawOverlay(slot.cmd, overlay_fb, slot);
				TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
			}
			else
			{
				TransitionImage(slot.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
			}

			acquired_index[acquired++] = image_index;
		}

		// Only the finished-screen path borrowed the real image.
		if (!game_res)
		{
			TransitionImage(slot.cmd, real_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
		}

		if (s_query_pool != VK_NULL_HANDLE)
			vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_query_pool, slot_index * 2 + 1);
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
		slot.timed = s_query_pool != VK_NULL_HANDLE;

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
