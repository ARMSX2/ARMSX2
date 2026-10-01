// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/WindowInfo.h"

#include <array>
#include <memory>
#include <vector>

class Error;

class GLContext
{
public:
	GLContext(const WindowInfo& wi);
	virtual ~GLContext();

	enum class Profile
	{
		NoProfile,
		Core,
		ES
	};

	struct Version
	{
		Profile profile;
		int major_version;
		int minor_version;
	};

	__fi const WindowInfo& GetWindowInfo() const { return m_wi; }
	__fi bool IsGLES() const { return (m_version.profile == Profile::ES); }
	__fi u32 GetSurfaceWidth() const { return m_wi.surface_width; }
	__fi u32 GetSurfaceHeight() const { return m_wi.surface_height; }

	virtual void* GetProcAddress(const char* name) = 0;
	virtual bool ChangeSurface(const WindowInfo& new_wi) = 0;
	virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) = 0;
	virtual bool SwapBuffers() = 0;
	virtual bool IsCurrent() = 0;
	virtual bool MakeCurrent() = 0;
	virtual bool DoneCurrent() = 0;
	virtual bool SupportsNegativeSwapInterval() const = 0;
	virtual bool SetSwapInterval(s32 interval) = 0;
	virtual std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) = 0;

	/// The framebuffer a finished frame belongs in. Zero - the window's own -
	/// for every context that owns its surface; a libretro frontend hands the
	/// core an FBO of its own instead.
	virtual u32 GetDefaultFramebuffer() const { return 0; }

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, Error* error);

	/// Set by a headless frontend (the GS dump runner) before the GS opens. With it
	/// set, a Surfaceless window gets an EGL context on Mesa's surfaceless platform,
	/// with no default framebuffer; the device draws into textures and skips present.
	/// Left false, Surfaceless gets no context, as before. The Qt app passes
	/// Surfaceless while it shows the game list and when it probes adapter info, and
	/// a context from the surfaceless platform could not later attach to its window.
	static bool AllowHeadless;

	// Libretro: the frontend is about to throw away the context this one shares
	// with, and on EGL that takes the whole display - driver state included -
	// with it, leaving these handles pointing at freed memory. Called while
	// they are still valid, this gives the context up without destroying it:
	// afterwards nothing here touches the platform again, so the handles leak
	// rather than crash. They belong to a display that is being torn down
	// anyway, so there is nothing left to leak.
	virtual void Abandon() { m_abandoned = true; }
	__fi bool IsAbandoned() const { return m_abandoned; }

	// Unbind whatever this thread has current, on the way to abandoning it,
	// knowing the platform state behind the context may already be gone.
	// Returns false when the platform offers no way to do that safely - the
	// binding then stays, and no context can ever be made current on this
	// thread again.
	virtual bool ReleaseThread() { return false; }

protected:
	WindowInfo m_wi;
	Version m_version = {};
	bool m_abandoned = false;
};
