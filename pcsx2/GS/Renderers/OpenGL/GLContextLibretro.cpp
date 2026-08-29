// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextLibretro.h"

#include "common/Error.h"

namespace
{
	GLContextLibretro::GetProcAddressCallback s_get_proc_address = nullptr;
	GLContextLibretro::GetFramebufferCallback s_get_framebuffer = nullptr;
} // namespace

GLContextLibretro::GLContextLibretro(const WindowInfo& wi)
	: GLContext(wi)
{
}

GLContextLibretro::~GLContextLibretro() = default;

void GLContextLibretro::SetCallbacks(GetProcAddressCallback get_proc_address, GetFramebufferCallback get_framebuffer)
{
	s_get_proc_address = get_proc_address;
	s_get_framebuffer = get_framebuffer;
}

bool GLContextLibretro::IsAvailable()
{
	return (s_get_proc_address != nullptr);
}

std::unique_ptr<GLContext> GLContextLibretro::Create(const WindowInfo& wi, Error* error)
{
	if (!IsAvailable())
	{
		Error::SetStringView(error, "No libretro GL context is installed.");
		return nullptr;
	}

	return std::unique_ptr<GLContext>(new GLContextLibretro(wi));
}

void* GLContextLibretro::GetProcAddress(const char* name)
{
	return s_get_proc_address ? s_get_proc_address(name) : nullptr;
}

bool GLContextLibretro::ChangeSurface(const WindowInfo& new_wi)
{
	m_wi = new_wi;
	return true;
}

void GLContextLibretro::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
	m_wi.surface_width = new_surface_width;
	m_wi.surface_height = new_surface_height;
}

bool GLContextLibretro::SwapBuffers()
{
	// The frontend presents the framebuffer we drew into; there is nothing to
	// swap from here.
	return true;
}

bool GLContextLibretro::IsCurrent()
{
	// The frontend guarantees its context is current while the core runs.
	return true;
}

bool GLContextLibretro::MakeCurrent()
{
	return true;
}

bool GLContextLibretro::DoneCurrent()
{
	return true;
}

bool GLContextLibretro::SupportsNegativeSwapInterval() const
{
	return false;
}

bool GLContextLibretro::SetSwapInterval(s32 interval)
{
	// Frame pacing is the frontend's job.
	return false;
}

std::unique_ptr<GLContext> GLContextLibretro::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	Error::SetStringView(error, "Shared contexts are not available through libretro.");
	return nullptr;
}

u32 GLContextLibretro::GetDefaultFramebuffer() const
{
	// Asked for every frame rather than cached: the frontend is free to hand
	// back a different FBO each time, and does when it recreates its target.
	return s_get_framebuffer ? s_get_framebuffer() : 0;
}
