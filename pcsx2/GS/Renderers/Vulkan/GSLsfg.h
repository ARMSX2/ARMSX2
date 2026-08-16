// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"
#include "common/Pcsx2Defs.h"

#include <string>

class VKSwapChain;

/// LSFG — Lossless Scaling frame generation, driven from our own Vulkan present path.
///
/// The upstream consumer app captures the screen with MediaProjection and composites over the
/// target process, because Android 12+ forbids injecting code into a non-debuggable app. That
/// constraint is not ours: ARMSX2 owns its swapchain, so we hand the library our own images
/// through its AHardwareBuffer entry points. No screen capture, no overlay, no accessibility
/// service, and the interpolated frames never leave our process.
///
/// NOTHING PROPRIETARY IS BUNDLED. The interpolation shaders are read at runtime out of the
/// user's own legitimately purchased Lossless.dll, which they supply through the Storage Access
/// Framework exactly as they supply a PS2 BIOS. Absent that file the feature stays off.
///
/// Every entry point is a no-op when ARMSX2_HAS_LSFG is undefined (the play flavour, and any
/// build whose fetch did not produce the library), so call sites need no #ifdef of their own.
namespace GSLsfg
{
	/// Why frame generation is unavailable, for the UI to explain rather than just grey a row out.
	/// Ordered by how early it is decided; the first failing reason is the one reported.
	enum class Unavailable : u8
	{
		Available = 0,   ///< usable right now
		NotCompiledIn,   ///< play flavour, or the library was not fetched
		NotVulkan,       ///< the OpenGL backend has no AHardwareBuffer path
		GpuUnsupported,  ///< needs Adreno 7xx or newer
		NoDll,           ///< the user has not supplied Lossless.dll
		DllUnreadable,   ///< supplied, but not a PE we can read shaders out of
		InitFailed,      ///< the library refused to initialise on this device
	};

	/// Why the feature cannot run, evaluated in the order above. Safe to call at any time and
	/// from any thread; it reads a cached capability verdict rather than touching Vulkan.
	Unavailable GetUnavailableReason();

	/// True only when GetUnavailableReason() == Available.
	bool IsAvailable();

	/// One-line explanation of the current reason, for the log and the settings row.
	const char* GetUnavailableReasonString();

	/// One line for the performance overlay, and the only place frame generation ever admits it
	/// is not running. EMPTY ONLY when the user has not enabled it — every other state says
	/// something, because the alternative is the setting being on, no line appearing, and no way
	/// to tell whether it is working, broken, or unsupported on this device.
	///
	/// "LSFG: unavailable" / "failed" / "no shaders" / "starting" / "LSFG: 118.80".
	std::string GetStatusText();

	/// Frames reaching the DISPLAY per second — real plus generated — over the last one-second
	/// window. Deliberately not the emulator's FPS, which frame generation does not change: what
	/// changes is how many frames get presented, and that is only visible if something counts it.
	/// 0 before the first window closes and after Shutdown.
	float GetDisplayFPS();

	/// Record what the renderer that just came up can do. Called once from GSDeviceVK::Create so
	/// the frontend can answer "is this supported" without a device, and so the verdict survives
	/// into the settings screen after the game stops. `adreno_generation` is 7 for Adreno 7xx, 8
	/// for 8xx, 0 when the GPU is not an Adreno or was not recognised.
	void NoteRendererCapability(bool is_vulkan, u32 adreno_generation);

	/// Absolute path to the user-supplied Lossless.dll, or empty when unset. The frontend copies
	/// the SAF pick into app storage first — the PE is read with ordinary file IO, so a
	/// content:// URI will not do.
	void SetDllPath(std::string path);
	const std::string& GetDllPath();

	/// Cheap structural check that a file is a PE ARMSX2 could read shaders out of, so the UI can
	/// reject a wrong pick at import time instead of failing later inside a frame. Does not
	/// validate that it IS Lossless Scaling — only that it is a readable PE image.
	bool LooksLikeLosslessDll(const std::string& path);

	/// Bring the interpolator up against this swapchain. Idempotent: a second call with the same
	/// swapchain geometry and multiplier is free, and a changed one tears down and rebuilds.
	/// Returns false and leaves the feature disabled on any error — a failure here must never
	/// take the renderer down with it.
	bool Initialize(VKSwapChain* swap_chain, u32 multiplier);

	/// Release everything. Safe to call when never initialised. MUST be called before the
	/// swapchain it was initialised against is destroyed.
	void Shutdown();

	/// True while a successful Initialize() is in effect.
	bool IsActive();

	/// Frames displayed per rendered frame. 1 = passthrough, 2..4 = the multiplier in effect.
	u32 GetMultiplier();

	/// Present `multiplier - 1` interpolated frames followed by the real one, in place of the
	/// caller's own vkQueuePresentKHR.
	///
	/// `render_finished` is the semaphore the frame's rendering signals; it is consumed here, so
	/// the caller must NOT also wait on it. The swapchain's current image index is read at entry
	/// and used for the final, real present — the extra images this acquires move the swapchain's
	/// notion of "current", which is harmless because the caller re-acquires straight afterwards.
	///
	/// `frame_has_new_content` must be false when this present carries no freshly rendered game
	/// frame — a pause-menu repaint of the previous one, a blank the GS produced nothing for, a
	/// boot screen before any output exists. Interpolating those invents motion between two
	/// frames the game never drew, and it costs the same GPU time as a real one. The frame
	/// history is dropped when it is false, so the pair either side of the gap is never stitched
	/// together into one bogus in-between frame.
	///
	/// Returns false if generation could not run this frame, in which case NOTHING was presented
	/// or consumed and the caller must fall through to its ordinary present path.
	bool PresentWithGeneration(VkQueue present_queue, VKSwapChain* swap_chain, VkSemaphore render_finished,
		bool frame_has_new_content);
} // namespace GSLsfg
