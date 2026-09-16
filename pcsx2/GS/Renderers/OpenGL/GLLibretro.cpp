// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLLibretro.h"

#include "common/Console.h"
#include "common/Error.h"

#if defined(_WIN32)
#include "GS/Renderers/OpenGL/GLContextWGL.h"
#else
#include "GS/Renderers/OpenGL/GLContextEGL.h"
#endif

#include <condition_variable>
#include <mutex>
#include <vector>

namespace GLLibretro
{
	bool Active = false;

	namespace
	{
		enum class Backend
		{
			None,
			EGL,
			WGL,
		};

		// Written by the frontend thread (context_reset/context_destroy), read
		// by the GS thread when it builds its context. The two are ordered by
		// the core: the CPU thread does not boot the VM - and so the GS thread
		// does not exist - until context_reset has run.
		Backend s_backend = Backend::None;
		void* s_display = nullptr;
		void* s_share_context = nullptr;
		// Whether the frontend handed us a GL ES context. Objects can only be
		// shared between contexts of the same client API, so ours has to match.
		bool s_share_is_gles = false;
	} // namespace

	bool CaptureFrontendContext(Error* error)
	{
		ReleaseFrontendContext();

#if defined(_WIN32)
		void* const rc = GLContextWGL::CaptureCurrentContext();
		if (rc)
		{
			s_backend = Backend::WGL;
			s_share_context = rc;
			Console.WriteLn("GL: Captured the frontend's WGL context.");
			return true;
		}
#else
		// EGL is the only one this fork has: its GL contexts are EGL contexts on
		// every platform but Windows. Asking it about a thread that is really
		// on GLX is harmless - it answers that nothing is current.
		EGLDisplay egl_display = EGL_NO_DISPLAY;
		EGLContext egl_context = EGL_NO_CONTEXT;
		bool egl_is_gles = false;
		if (GLContextEGL::CaptureCurrentContext(&egl_display, &egl_context, &egl_is_gles))
		{
			s_backend = Backend::EGL;
			s_display = egl_display;
			s_share_context = egl_context;
			s_share_is_gles = egl_is_gles;
			Console.WriteLnFmt("GL: Captured the frontend's EGL context ({}).",
				egl_is_gles ? "OpenGL ES" : "desktop OpenGL");
			return true;
		}

#endif

		Error::SetStringView(error,
			"No GL context was current on the frontend thread. An X11 frontend hands out a GLX "
			"context, which this core has no way to share with - try the Vulkan renderer, or a "
			"frontend on Wayland or Android, where the context is EGL.");
		return false;
	}

	void ReleaseFrontendContext()
	{
		s_backend = Backend::None;
		s_display = nullptr;
		s_share_context = nullptr;
		s_share_is_gles = false;
	}

	bool HasFrontendContext()
	{
		return s_backend != Backend::None;
	}

	std::unique_ptr<GLContext> CreateSharedContext(
		const WindowInfo& wi, std::span<const GLContext::Version> versions_to_try, Error* error)
	{
		// Keep only the profile the frontend's context uses. Sharing across
		// client APIs is not a thing, and trying the wrong ones first just
		// spends a failed eglCreateContext on each.
		const auto wanted =
			s_share_is_gles ? GLContext::Profile::ES : GLContext::Profile::Core;
		std::vector<GLContext::Version> matching;
		for (const GLContext::Version& v : versions_to_try)
		{
			if (v.profile == wanted)
				matching.push_back(v);
		}
		if (matching.empty())
		{
			Error::SetStringView(error, "No context versions match the frontend's client API");
			return nullptr;
		}
		versions_to_try = matching;

		switch (s_backend)
		{
#if defined(_WIN32)
			case Backend::WGL:
				return GLContextWGL::CreateShared(
					wi, static_cast<HGLRC>(s_share_context), versions_to_try, error);
#else
			case Backend::EGL:
				return GLContextEGL::CreateShared(wi, s_display, s_share_context, versions_to_try, error);
#endif

			default:
				Error::SetStringView(error, "The frontend's GL context was never captured");
				return nullptr;
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// GS thread -> retro_run frame handoff
	//////////////////////////////////////////////////////////////////////////

	namespace
	{
		std::mutex s_frame_mutex;
		std::condition_variable s_frame_consumed_cv;
		Frame s_frame;
		u64 s_frame_serial = 0;
		u64 s_frame_consumed = 0;
		bool s_pacing = false;
	} // namespace

	void PublishFrame(const Frame& frame)
	{
		std::unique_lock<std::mutex> lock(s_frame_mutex);

		// A frame nobody picked up (pacing off, or the frontend tore its
		// context down mid-flight) still owns a fence. Retire it here, on the
		// thread that made it, rather than leaking one per dropped frame.
		if (s_frame.fence && s_frame_serial != s_frame_consumed)
			glDeleteSync(s_frame.fence);

		s_frame = frame;
		s_frame_serial++;

		// Block the GS thread until retro_run picks the frame up (or pacing
		// gets aborted for shutdown): one presented frame per retro_run.
		s_frame_consumed_cv.wait(
			lock, []() { return !s_pacing || s_frame_consumed == s_frame_serial; });
	}

	bool ConsumeFrame(Frame* out_frame)
	{
		std::lock_guard<std::mutex> lock(s_frame_mutex);
		if (s_frame_serial == s_frame_consumed || s_frame.texture == 0)
			return false;

		s_frame_consumed = s_frame_serial;
		*out_frame = s_frame;
		// The caller owns the fence now; clearing it here keeps PublishFrame
		// from also retiring it.
		s_frame.fence = nullptr;
		s_frame_consumed_cv.notify_all();
		return true;
	}

	bool HasFrame()
	{
		std::lock_guard<std::mutex> lock(s_frame_mutex);
		return s_frame_serial != s_frame_consumed && s_frame.texture != 0;
	}

	void SetPacing(bool enabled)
	{
		std::lock_guard<std::mutex> lock(s_frame_mutex);
		s_pacing = enabled;
		if (!enabled)
			s_frame_consumed_cv.notify_all();
	}

	void AbortPacing()
	{
		SetPacing(false);
	}

	void Shutdown()
	{
		ReleaseFrontendContext();

		std::lock_guard<std::mutex> lock(s_frame_mutex);
		// Not deleted: by the time a session shuts down the context that owns
		// this fence may already be gone, and glDeleteSync on a dead share
		// group is undefined. Dropping the handle leaks nothing that outlives
		// the context itself.
		s_frame = Frame();
		s_frame_serial = 0;
		s_frame_consumed = 0;
		s_pacing = false;
		s_frame_consumed_cv.notify_all();
	}
} // namespace GLLibretro
