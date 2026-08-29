// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

/// The GL context a libretro frontend already owns.
///
/// Nothing is created or destroyed here: RetroArch makes a context current on
/// its video thread and hands the core two callbacks - one to resolve GL
/// entry points, one to ask which framebuffer the frame should be drawn into,
/// since a libretro core renders into the frontend's FBO rather than into the
/// window's default framebuffer. Everything else (swap interval, buffer swap,
/// shared contexts) belongs to the frontend and is a no-op here.
class GLContextLibretro final : public GLContext
{
public:
	using GetProcAddressCallback = void* (*)(const char* name);
	using GetFramebufferCallback = u32 (*)();

	~GLContextLibretro() override;

	/// Installs the frontend's callbacks. Called from context_reset, before
	/// anything asks for a device. Passing nullptrs uninstalls them again,
	/// which is what context_destroy does.
	static void SetCallbacks(GetProcAddressCallback get_proc_address, GetFramebufferCallback get_framebuffer);

	/// True when a frontend context is available, i.e. when GLContext::Create
	/// should hand out one of these instead of opening its own.
	static bool IsAvailable();

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, Error* error);

	void* GetProcAddress(const char* name) override;
	bool ChangeSurface(const WindowInfo& new_wi) override;
	void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
	bool SwapBuffers() override;
	bool IsCurrent() override;
	bool MakeCurrent() override;
	bool DoneCurrent() override;
	bool SupportsNegativeSwapInterval() const override;
	bool SetSwapInterval(s32 interval) override;
	std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

	u32 GetDefaultFramebuffer() const override;

private:
	explicit GLContextLibretro(const WindowInfo& wi);
};
