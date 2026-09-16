// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Libretro OpenGL context sharing and frame handoff.
//
// The Vulkan path can hand the frontend an image rendered on the GS thread
// because Vulkan objects are not bound to a thread. GL contexts are: the
// frontend's context is current on the thread that calls retro_run, and the GS
// runs on its own thread, so the two cannot be the same context.
//
// What they can be is the same *share group*. The frontend's context_reset
// runs on the frontend thread with its context current, which is the one
// moment the core can see that context at all - libretro has no GL equivalent
// of retro_hw_render_interface_vulkan - so that is where we grab it, straight
// from the platform (EGL, or WGL on Windows). The GS thread then creates a
// context that shares its object space, renders as usual, and publishes the
// finished texture; retro_run waits on a fence and blits that texture into the
// frontend's framebuffer. Textures and sync objects are shared, so nothing
// crosses the CPU.
//
// GLX is not covered: this fork builds its GL contexts on EGL everywhere but
// Windows, so an X11 frontend that hands out a GLX context finds nothing to
// capture and the core says so rather than drawing nothing.
//
// Everything here fails soft: if the frontend's context cannot be captured or
// shared, the core says why and falls back to the null renderer rather than
// running a GS thread whose every GL call goes nowhere.

#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

#include "common/Pcsx2Types.h"

#include "glad/gl.h"

#include <memory>
#include <span>

class Error;

#ifndef ENABLE_LIBRETRO

// Not a libretro build: the same names, and nothing behind them. The GL device
// asks about this path in a handful of places, and an inert header keeps those
// call sites free of a conditional each while leaving a Qt or SDL build with
// exactly the code it had before.
namespace GLLibretro
{
	inline constexpr bool Active = false;

	struct Frame
	{
		GLuint texture = 0;
		GLsync fence = nullptr;
		u32 width = 0;
		u32 height = 0;
	};

	inline void PublishFrame(const Frame&) {}
	inline bool ConsumeFrame(Frame*) { return false; }
	inline bool HasFrame() { return false; }
	inline void SetPacing(bool) {}
	inline void AbortPacing() {}
	inline void Shutdown() {}
} // namespace GLLibretro

#else

namespace GLLibretro
{
	// True when the GL device is presenting through a libretro frontend;
	// checked by GSDeviceOGL during init and present.
	extern bool Active;

	// Grabs the GL context the frontend has current on the calling thread.
	// Must be called from the frontend's context_reset, on its thread.
	bool CaptureFrontendContext(Error* error);

	// Drops the captured handles (the frontend is tearing its context down).
	void ReleaseFrontendContext();
	bool HasFrontendContext();

	// Creates a context sharing the captured context's object space, for the
	// GS thread to make current. Null when nothing was captured, or when the
	// platform refused to share.
	std::unique_ptr<GLContext> CreateSharedContext(
		const WindowInfo& wi, std::span<const GLContext::Version> versions_to_try, Error* error);

	// GS-thread -> retro_run frame handoff, mirroring VKLibretro. The texture
	// lives in the shared object space, so the frontend can read it directly;
	// the fence tells it when the GS thread's rendering has actually landed.
	struct Frame
	{
		GLuint texture = 0;
		GLsync fence = nullptr;
		u32 width = 0;
		u32 height = 0;
	};
	void PublishFrame(const Frame& frame);
	bool ConsumeFrame(Frame* out_frame); // true if a new frame arrived since the last consume
	bool HasFrame();                     // same, without consuming it

	// Frame pacing: when enabled, PublishFrame blocks the GS thread until
	// retro_run consumes the frame - the frontend's retro_run cadence becomes
	// the emulation's vsync. Abort before shutdown so the GS thread can't be
	// left parked.
	void SetPacing(bool enabled);
	void AbortPacing();

	void Shutdown();
} // namespace GLLibretro

#endif // ENABLE_LIBRETRO
