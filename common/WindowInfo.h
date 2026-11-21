// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "Pcsx2Defs.h"

#include <optional>

/// Contains the information required to create a graphics context in a window.
struct WindowInfo
{
	enum class Type
	{
		Surfaceless,
		Win32,
		X11,
		Wayland,
		MacOS,
		Android,

		// Vulkan VK_KHR_display direct-to-monitor (no compositor / no GBM
		// intermediate). Frontend supplies no native window handle; the
		// renderer enumerates displays itself. surface_width/surface_height
		// carry the requested mode (0 = pick the display's preferred mode).
		VulkanDirect
	};

	/// The type of the surface. Surfaceless indicates there is no surface to display on now.
	Type type = Type::Surfaceless;

	/// For a Surfaceless window whose surface is only gone for now (an Android app between
	/// surfaces, or the Qt game list shown over a running game), the type the window has when the
	/// surface returns. A renderer opened in that state
	/// still sets up support for that window system, so it can present once the surface is back.
	/// Surfaceless for a run that never displays anything (runners, libretro, tests).
	Type pending_type = Type::Surfaceless;

	/// The window system this window presents to, now or once its surface returns.
	/// Surfaceless if it never presents.
	Type GetPresentType() const { return (type == Type::Surfaceless) ? pending_type : type; }

	/// Connection to the display server. On most platforms except X11/Wayland, this is implicit and null.
	void* display_connection = nullptr;

	/// Abstract handle to the window. This depends on the surface type.
	void* window_handle = nullptr;

	/// For platforms where a separate surface/layer handle is needed, it is stored here (e.g. MacOS).
	void* surface_handle = nullptr;

	/// Width of the surface in pixels.
	u32 surface_width = 0;

	/// Height of the surface in pixels.
	u32 surface_height = 0;

	/// DPI scale for the surface.
	float surface_scale = 1.0f;

	/// Refresh rate of the surface, if available.
	float surface_refresh_rate = 0.0f;

	/// Rotation the window system applies to show this window, in quarter turns counted the way
	/// Vulkan's currentTransform counts them (1 = ROTATE_90). Filled in where the graphics API does
	/// not report it, so the renderer can pre-rotate: on Wayland the compositor's rotation, on
	/// VulkanDirect the DRM panel orientation. 0 if none or unknown.
	u8 surface_rotation = 0;

	/// Returns the host's refresh rate for the given window, if available.
	static std::optional<float> QueryRefreshRateForWindow(const WindowInfo& wi);
};
