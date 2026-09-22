// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

class Error;

#define VK_NO_PROTOTYPES

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR

// vulkan.h pulls in windows.h on Windows, so we need to include our replacement header first
#include "common/RedtapeWindows.h"
#endif

#if defined(X11_API)
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#if defined(WAYLAND_API)
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#endif

#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include "vulkan/vulkan.h"

#if defined(X11_API)

// This breaks a bunch of our code. They shouldn't be #defines in the first place.
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#ifdef CursorShape
#undef CursorShape
#endif
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef FocusIn
#undef FocusIn
#endif
#ifdef FocusOut
#undef FocusOut
#endif
#ifdef FontChange
#undef FontChange
#endif
#ifdef Expose
#undef Expose
#endif
#ifdef Unsorted
#undef Unsorted
#endif
#ifdef Bool
#undef Bool
#endif

#endif

#include "VKEntryPoints.h"

// We include vk_mem_alloc globally, so we don't accidentally include it before the vulkan header somewhere.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_STATS_STRING_ENABLED 0
#include "vk_mem_alloc.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace Vulkan
{
	bool IsVulkanLibraryLoaded();
	bool LoadVulkanLibrary(Error* error);
	bool LoadVulkanInstanceFunctions(VkInstance instance);
	bool LoadVulkanDeviceFunctions(VkDevice device);
	void UnloadVulkanLibrary();
	void ResetVulkanLibraryFunctionPointers();

	// The definition in VKLoader.cpp is guarded on ARMSX2_USE_ADRENOTOOLS, so the
	// declaration has to be too. It used to say __ANDROID__, which is a wider set:
	// every Android build that does not link adrenotools saw a declaration with no
	// definition behind it, and only the fact that the single caller lived in the one
	// CMake tree that force-sets ARMSX2_USE_ADRENOTOOLS kept that from being a link
	// error. Adding a second caller (pcsx2-gsrunner) is what makes it matter.
#if defined(ARMSX2_USE_ADRENOTOOLS)
	/// Configures a custom Vulkan driver (e.g. Mesa Turnip) to load via
	/// libadrenotools instead of the system loader. Must be called BEFORE
	/// LoadVulkanLibrary — the first MTGS::Open triggers enumerate which
	/// is the first load, so the setter has to run before VM start.
	/// Pass empty strings to revert to the system loader on next load.
	/// `required` decides what happens when the driver cannot be opened: false falls
	/// through to the system loader so the boot proceeds (what the app wants — the user
	/// gets a picture), true fails LoadVulkanLibrary (what a measurement run wants — a
	/// run on the vendor driver that claims to be on a pack is worse than no run).
	void SetCustomDriverPath(const char* driver_dir, const char* driver_name,
		const char* redirect_dir, const char* hook_lib_dir, bool required);
#endif
} // namespace Vulkan
