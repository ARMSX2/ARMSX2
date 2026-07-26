// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// RenderDoc capture driven from inside gsrunner, rather than by RenderDoc itself.
//
// The usual F12 trigger cannot work here. RenderDoc only polls X11/xcb for the
// capture key (its Wayland input backend sits behind an off-by-default build
// option), so on a Wayland surface PlatformHasKeyInput() is false and the key is
// never seen. Triggering over target control does not help either: gsrunner opens
// EGL through eglGetPlatformDisplayEXT / eglCreatePlatformWindowSurfaceEXT, and
// RenderDoc hooks only the core entry points, so it never records a native window
// for the surface and has nothing to attach a capture to.
//
// The in-application API sidesteps both. StartFrameCapture(nullptr, nullptr)
// captures the active device whatever windowing system the swapchain came from,
// and needs no keypress, so a headless dump replay can capture unattended.
namespace RenderDocCapture
{
	/// Binds the RenderDoc API and arms a capture of dump frames
	/// [start_frame, start_frame + num_frames). Must be called before the GS device
	/// exists, since RenderDoc installs its hooks when its library loads. Returns
	/// false (and disables capture) if RenderDoc is unavailable.
	bool Initialize(const std::string& path_template, u32 start_frame, u32 num_frames);

	/// Runs on the GS thread at each present boundary, where `frame_number` is the
	/// dump frame whose GS work has just been submitted. Opens and closes captures
	/// so that each armed frame lands in its own .rdc.
	void OnPresentFrame(u32 frame_number);

	/// Reports whether the armed range completed. Does not close an in-flight
	/// capture: EndFrameCapture must run on the GS thread, and this is called from
	/// the CPU thread after the GS thread is gone.
	void Shutdown();
} // namespace RenderDocCapture
