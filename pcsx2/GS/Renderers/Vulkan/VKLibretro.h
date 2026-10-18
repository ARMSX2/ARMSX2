// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Libretro Vulkan context sharing (the lrps2-libretro pattern).
//
// When a libretro frontend drives the GS, the VkInstance (and optionally the
// VkPhysicalDevice) come from the frontend's context-negotiation interface,
// and the VkDevice we create must (a) enable every extension/layer/feature
// the frontend requires and (b) serialise vkQueueSubmit against the
// frontend's own use of the shared graphics queue. Rather than teach
// GSDeviceVK about any of this, the loader's global vkGetInstanceProcAddr is
// swapped for a wrapper that transparently intercepts vkCreateDevice (to
// merge the frontend requirements) and vkQueueSubmit (to lock the queue via
// the retro_hw_render_interface_vulkan). GSDeviceVK only needs two explicit
// branches: adopt the provided instance/gpu instead of creating its own, and
// skip surface/swapchain creation (presentation is handed off through
// set_image from the frontend side).

#pragma once

#include "VKLoader.h"

#include "common/Pcsx2Types.h"

namespace VKLibretro
{
	// True when the libretro frontend owns the display; checked by
	// GSDeviceVK during init/present.
	extern bool Active;

	// Largest output canvas the core will produce: 4x-upscaled PAL expanded
	// to 4:3. Advertised to the frontend as retro_game_geometry
	// max_width/max_height and enforced when the present path sizes the
	// canvas to the merged frame.
	static constexpr u32 kMaxCanvasWidth = 2732;
	static constexpr u32 kMaxCanvasHeight = 2048;

	struct InitInfo
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice gpu = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE; // filled by the wrapped vkCreateDevice
		PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
		const char** required_device_extensions = nullptr;
		unsigned num_required_device_extensions = 0;
		const char** required_device_layers = nullptr;
		unsigned num_required_device_layers = 0;
		const VkPhysicalDeviceFeatures* required_features = nullptr;
	};
	extern InitInfo Init;

	// Swap the loader's global vkGetInstanceProcAddr for the intercepting
	// wrapper. Call after Vulkan::LoadVulkanLibrary(), before GS opens.
	void InstallWraps();

	// The retro_hw_render_interface_vulkan acquired after context_reset
	// (stored as void* so this header stays free of libretro headers).
	void SetHWRenderInterface(void* iface);
	void* GetHWRenderInterface();

	// GS-thread -> retro_run frame handoff. The GS present path publishes the
	// current display texture; retro_run consumes it (at most one per call)
	// and forwards it to the frontend via set_image + video_cb.
	struct Frame
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		u32 width = 0;
		u32 height = 0;

		// Volumetric (GSVolumetric.h, ENABLE_VOLUMETRIC): depth of the displayed picture, when there is one. An
		// R32F image in SHADER_READ_ONLY_OPTIMAL holding GS Z / 2^32 (larger is nearer); times
		// depth_scale it is 0..1 of the Z format's range. depth_src_uv is the region of it that
		// was presented, at depth_draw_rect (x0, y0, x1, y1 pixels) in the image above.
		VkImage depth_image = VK_NULL_HANDLE;
		VkImageView depth_view = VK_NULL_HANDLE;
		u32 depth_width = 0;
		u32 depth_height = 0;
		float depth_scale = 0.0f;
		float depth_src_uv[4] = {};
		float depth_draw_rect[4] = {};
		// RGBA8 image in the depth image's space and layout: the colour when the depth was taken.
		// Where the presented picture differs from it, something was drawn after the 3D (HUD).
		VkImage prehud_image = VK_NULL_HANDLE;
		VkImageView prehud_view = VK_NULL_HANDLE;
		// R32F like the depth image: the depth target at vsync when the depth above is a snapshot
		// from this frame. A HUD that writes Z after the snapshot is at depth_scale * value >= 1.
		VkImage live_depth_image = VK_NULL_HANDLE;
		VkImageView live_depth_view = VK_NULL_HANDLE;
		// Geometric stereo prototype: 0 = this frame was drawn for the left eye, 1 = right, -1 = mono.
		int stereo_eye = -1;
		// Multiview stereo: image above is the left eye, this one (same size, format and layout) the
		// right. Null without multiview stereo. The depth image then has a layer per eye too.
		VkImage right_image = VK_NULL_HANDLE;
		VkImageView right_view = VK_NULL_HANDLE;
		// Where the game's picture lies in the image (x0, y0, x1, y1 pixels), with or without depth:
		// the rest is border. Light guns aim in this space.
		float draw_rect[4] = {};
		// Volumetric present copy: the ids of the armed targets this frame was copied into (image to
		// the left target, right_image to the right one), 0 for none.
		u64 copied_left = 0;
		u64 copied_right = 0;
	};
	void PublishFrame(const Frame& frame);
	bool ConsumeFrame(Frame* out_frame); // true if a new frame arrived since the last consume

#ifdef PCSX2_VOLUMETRIC
	// Volumetric present copy (ENABLE_VOLUMETRIC). The frontend hands over images it wants the next presented frame copied
	// into (XR swapchain images it acquired), one per eye. The GS records the copies at the end of the
	// present's command buffer, so they run right after the frame on the GPU instead of queueing
	// behind the GS's later work, which on a shared queue made the frontend's frames late.
	struct PresentCopyTarget
	{
		VkImage image = VK_NULL_HANDLE; // RGBA8-sized texels, any layout; left in COLOR_ATTACHMENT_OPTIMAL
		u32 width = 0, height = 0; // copied into only by a frame of exactly this size
		u64 id = 0; // non-zero, reported back in Frame::copied_left/right
		// Left in SHADER_READ_ONLY_OPTIMAL instead, for the frontend to sample before it moves the
		// image to COLOR_ATTACHMENT_OPTIMAL and releases it.
		bool readable = false;
		// The GS may draw straight into it through an RGBA8 UNORM view (the image is mutable-format
		// or UNORM); otherwise it only copies into it.
		bool drawable = false;
	};
	// Frontend: arms the target for an eye (0 left, 1 right), replacing one not yet taken.
	void ArmPresentCopy(int eye, const PresentCopyTarget& target);
	// Frontend: waits until the GS is not between taking targets and submitting their copies, then
	// disarms the eye (-1: both). Afterwards the frontend may write, release or destroy the images.
	void DisarmPresentCopy(int eye = -1);
	// GS: takes the armed targets (empty image where none); true if there was any. The GS then calls
	// FinishPresentCopy once the command buffer with the copies is submitted.
	bool TakePresentCopy(PresentCopyTarget* left, PresentCopyTarget* right);
	void FinishPresentCopy();
#endif

	// Frame pacing: when enabled, PublishFrame blocks the GS thread until
	// retro_run consumes the frame — the frontend's retro_run cadence becomes
	// the emulation's vsync. Abort before shutdown so the GS thread can't be
	// left parked.
	void SetPacing(bool enabled);
	void AbortPacing();

	void Shutdown();
} // namespace VKLibretro
