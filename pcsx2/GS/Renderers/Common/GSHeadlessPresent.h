// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>
#include <vector>

class Error;
class GSTexture;

// Headless present: a host with no window (`armsx2-sdl --headless WxH`) still gets a real present.
//
// A Surfaceless GSDevice normally skips every present. When the host has called SetEnabled(true)
// before the GS opens, the Vulkan and OpenGL backends instead draw the whole present into an
// offscreen texture the size of the Surfaceless WindowInfo: the game rect, the TV shaders,
// FullscreenUI and the OSD, exactly as they would land in a swapchain image. The backend then
// calls NotePresented() with that texture, and the readback functions below turn it into pixels.
//
// Threading: SetEnabled is called once by the host before the GS thread starts. Everything else
// is GS-thread only, except PresentCount(), which any thread may read.
namespace GSHeadless
{
	/// The pace of the no-game idle present, and the time every ImGui frame is told has passed
	/// (ImGuiManager::NewFrame), so animations depend on how many frames were drawn rather than
	/// on how fast this machine drew them. The host should report kFrameRate as the window's
	/// refresh rate.
	constexpr float kFrameRate = 30.0f;
	constexpr float kFrameTime = 1.0f / kFrameRate;

	/// Host switch. Not a setting: the window the host gave us is what decides, and a host that
	/// wants a screenshot-only GS knows it at startup.
	void SetEnabled(bool enabled);
	bool IsEnabled();

	/// A backend's EndPresent, after the finished frame is in `backbuffer` and submitted.
	/// The pointer stays valid until the backend resizes or destroys its backbuffer, so it is only
	/// good for CaptureFreshFrame(), which presents first.
	void NotePresented(GSTexture* backbuffer);

	/// GSDevice::Destroy: drops the pointer NotePresented() was given.
	void ForgetPresented();

	/// Frames presented so far. Any thread.
	u64 PresentCount();

	struct Frame
	{
		u32 width = 0;
		u32 height = 0;
		/// RGBA8, row 0 at the top, alpha forced to 0xFF.
		std::vector<u32> pixels;
	};

	/// GS thread. Presents the current frame once more (so the pixels cannot predate a window
	/// resize or a state change made since the last idle present), then reads that frame back.
	/// Fails with a reason in `error` if headless present is off, the renderer has no offscreen
	/// present (Metal, Direct3D), or the present was skipped.
	bool CaptureFreshFrame(Frame* out, Error* error);

	/// CaptureFreshFrame() written to `path` as a PNG. The file appears whole: it is written
	/// beside the target and renamed over it.
	bool CaptureFreshFrameToPNG(const std::string& path, u32* width, u32* height, Error* error);
} // namespace GSHeadless
