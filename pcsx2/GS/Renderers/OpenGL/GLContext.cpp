// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContext.h"
#ifdef __ANDROID__
#include "GS/Renderers/OpenGL/GLContextEGLAndroid.h"
#endif

#if defined(_WIN32)
#include "GS/Renderers/OpenGL/GLContextWGL.h"
#else // Linux
#ifdef X11_API
#include "GS/Renderers/OpenGL/GLContextEGLX11.h"
#endif
#ifdef WAYLAND_API
#include "GS/Renderers/OpenGL/GLContextEGLWayland.h"
#endif
#ifndef __ANDROID__
#include "GS/Renderers/OpenGL/GLContextEGL.h"
#endif
#endif

#include "common/Console.h"
#include "common/Error.h"

#include "glad/gl.h"

#if defined(ENABLE_LIBRETRO)
#include "GS/Renderers/OpenGL/GLLibretro.h"
#endif

#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_set>

static bool ShouldPreferESContext()
{
	const char* value = std::getenv("PREFER_GLES_CONTEXT");
	return (value && std::strcmp(value, "1") == 0);
}

// GLAD assigns only the entry points the new context's version and extensions provide and leaves
// the rest as they were, so a pointer from an earlier context in the process survives into one
// that lacks the function. The null tests that pick a fallback (glColorMaski,
// glDrawElementsBaseVertex) then skip it and the call goes into the old driver. The Android app
// keeps one process across games, so a native GLES session followed by ANGLE hits this.
//
// Cleared after the load rather than before: under libretro the frontend thread presents through
// these pointers while the GS thread can be creating a context, so the ones the new context
// provides must not be null even briefly.
static void ClearGLADFunctionsNotAssigned(const std::unordered_set<std::string_view>& assigned)
{
#define GLAD_FUNCTION(name) \
	if (!assigned.contains(#name)) \
		glad_##name = nullptr;
#include "GS/Renderers/OpenGL/GLADFunctions.inl"
#undef GLAD_FUNCTION
}

bool GLContext::AllowHeadless = false;

GLContext::GLContext(const WindowInfo& wi)
	: m_wi(wi)
{
}

GLContext::~GLContext() = default;

std::unique_ptr<GLContext> GLContext::Create(const WindowInfo& wi, Error* error)
{
	static constexpr Version vlist[] = {
		{Profile::Core, 4, 6},
		{Profile::Core, 4, 5},
		{Profile::Core, 4, 4},
		{Profile::Core, 4, 3},
		{Profile::Core, 4, 2},
		{Profile::Core, 4, 1},
		{Profile::Core, 4, 0},
		{Profile::Core, 3, 3},
		{Profile::ES, 3, 2},
		{Profile::ES, 3, 1},
		{Profile::ES, 3, 0},
		{Profile::ES, 2, 0},
	};
	static constexpr size_t num_versions = std::size(vlist);

	const Version* versions_to_try = vlist;
	size_t num_versions_to_try = num_versions;

	Version reordered[num_versions];
	if (ShouldPreferESContext())
	{
		size_t count = 0;
		for (size_t i = 0; i < num_versions_to_try; i++)
		{
			if (versions_to_try[i].profile == Profile::ES)
				reordered[count++] = versions_to_try[i];
		}
		for (size_t i = 0; i < num_versions_to_try; i++)
		{
			if (versions_to_try[i].profile != Profile::ES)
				reordered[count++] = versions_to_try[i];
		}
		versions_to_try = reordered;
	}

	std::unique_ptr<GLContext> context;
	// Libretro: the frontend owns the only context that can reach the screen,
	// and it is current on the frontend's own thread - so this thread, the GS
	// thread, gets one that shares its objects rather than one of its own. See
	// GLLibretro. Nothing below could help here anyway: there is no window for
	// a platform context to attach to.
#if defined(ENABLE_LIBRETRO)
	if (GLLibretro::Active)
	{
		context = GLLibretro::CreateSharedContext(
			wi, std::span<const Version>(versions_to_try, num_versions_to_try), error);
		if (!context)
			return nullptr;
	}
#endif

#ifdef __ANDROID__
	if (!context && wi.type == WindowInfo::Type::Android)
		context = GLContextEGLAndroid::Create(wi, versions_to_try, num_versions_to_try);
#endif
#if defined(_WIN32)
	if (!context)
		context = GLContextWGL::Create(wi, std::span<const Version>(versions_to_try, num_versions_to_try), error);
#else
#ifdef X11_API
	if (!context && wi.type == WindowInfo::Type::X11)
		context = GLContextEGLX11::Create(wi, std::span<const Version>(versions_to_try, num_versions_to_try), error);
#endif
#ifdef WAYLAND_API
	if (!context && wi.type == WindowInfo::Type::Wayland)
		context = GLContextEGLWayland::Create(wi, std::span<const Version>(versions_to_try, num_versions_to_try), error);
#endif
// GLContextEGL.cpp is built only alongside a window system or libretro (pcsx2/CMakeLists.txt).
#if !defined(__ANDROID__) && (defined(X11_API) || defined(WAYLAND_API) || defined(ENABLE_LIBRETRO))
	if (!context && wi.type == WindowInfo::Type::Surfaceless && AllowHeadless)
		context = GLContextEGL::Create(wi, std::span<const Version>(versions_to_try, num_versions_to_try), error);
#endif
#endif

	if (!context)
		return nullptr;

	// NOTE: Not thread-safe. But this is okay, since we're not going to be creating more than one context at a time.
	static GLContext* context_being_created;
	static std::unordered_set<std::string_view>* names_being_assigned;
	std::unordered_set<std::string_view> assigned;
	context_being_created = context.get();
	names_being_assigned = &assigned;
	const GLADloadfunc load = [](const char* name) {
		names_being_assigned->insert(name);
		return reinterpret_cast<GLADapiproc>(context_being_created->GetProcAddress(name));
	};

	if (!context->IsGLES())
	{
		const int gl_version = gladLoadGL(load);
		ClearGLADFunctionsNotAssigned(assigned);
		if (!gl_version)
		{
			Error::SetStringView(error, "Failed to load GL functions for GLAD");
			return nullptr;
		}
	}
	else
	{
		const int gles_version = gladLoadGLES2(load);
		ClearGLADFunctionsNotAssigned(assigned);
		if (!gles_version)
		{
			Error::SetStringView(error, "Failed to load GLES functions for GLAD");
			return nullptr;
		}

		// The renderer needs ES 3.0 or newer. On an ES 2.0 context GLAD loads none of the 3.x
		// entry points, so the first call to one (glInvalidateFramebuffer in RenderBlankFrame)
		// jumps to null. The 2.0 entry in vlist stays: a context is how we find out what the
		// driver offers. ANGLE on old Vulkan 1.1 drivers is the usual case.
		if (GLAD_VERSION_MAJOR(gles_version) < 3)
		{
			const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
			const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
			Console.ErrorFmt("GL: refusing an OpenGL ES {}.{} context. GL_RENDERER: {} GL_VERSION: {}",
				GLAD_VERSION_MAJOR(gles_version), GLAD_VERSION_MINOR(gles_version), renderer ? renderer : "",
				version ? version : "");
			if (renderer && std::strstr(renderer, "ANGLE"))
			{
				Error::SetStringFmt(error,
					"ANGLE on this device offers only OpenGL ES {}.{}, which ARMSX2 cannot use. "
					"Use the Vulkan renderer with a custom GPU driver instead.",
					GLAD_VERSION_MAJOR(gles_version), GLAD_VERSION_MINOR(gles_version));
			}
			else
			{
				Error::SetStringFmt(error,
					"This GPU driver offers only OpenGL ES {}.{}, which ARMSX2 cannot use: the OpenGL "
					"renderer needs OpenGL ES 3.0 or newer. Use the Vulkan renderer instead.",
					GLAD_VERSION_MAJOR(gles_version), GLAD_VERSION_MINOR(gles_version));
			}
			return nullptr;
		}
	}

	context_being_created = nullptr;

	return context;
}
