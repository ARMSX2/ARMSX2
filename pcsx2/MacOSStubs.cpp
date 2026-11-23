// iOS and tvOS stubs: CMake builds this file only for them, and neither has CocoaTools.mm.

// On iOS, CocoaTools.mm is excluded from the build. Provide stubs for the
// functions referenced by iOS-compiled core code (DynamicLibrary, WindowInfo,
// Pcsx2Config, etc.). iOS uses UIKit/Foundation, not Cocoa/AppKit.
//
// CreateMetalLayer, DestroyMetalLayer and GetBundlePath are deliberately absent:
// the app's HostImpls.mm implements them for real. Defining them here too gave the
// linker two of each and it picked one on its own.
#include "common/CocoaTools.h"
#include "common/WindowInfo.h"
#include <optional>
#include <string>

namespace CocoaTools
{
	std::optional<float> GetViewRefreshRate(const WindowInfo& wi) { return std::nullopt; }
	void MarkHelpMenu(void* menu) {}
	std::optional<std::string> GetNonTranslocatedBundlePath() { return std::nullopt; }
	std::optional<std::string> MoveToTrash(std::string_view file) { return std::nullopt; }
	bool DelayedLaunch(std::string_view file) { return false; }
	bool ShowInFinder(std::string_view file) { return false; }
	std::optional<std::string> GetResourcePath() { return std::nullopt; }
	void* CreateWindow(std::string_view title, uint32_t width, uint32_t height) { return nullptr; }
	void DestroyWindow(void* window) {}
	void GetWindowInfoFromWindow(WindowInfo* wi, void* window) {}
	void RunCocoaEventLoop(bool wait_forever) {}
	void StopMainThreadEventLoop() {}
}

// --- Discord Register stubs (discord_register_osx.m excluded on iOS) ---
extern "C" {
void Discord_Register(const char* applicationId, const char* command) {}
void Discord_RegisterSteamGame(const char* applicationId, const char* steamId) {}
}

// --- Host capture + hotkey stubs (frontend callbacks not yet wired) ---
#include "Host.h"
#include "GS/GS.h"
#include "Input/InputManager.h"

// g_host_hotkeys - normally defined in pcsx2-qt, empty on iOS. Not in a
// libretro core, which defines its own in pcsx2-libretro/Main.cpp: this is a
// frontend definition, and the core is the frontend there. Two of them is one
// duplicate symbol at link, which is what the first iOS core build stopped on.
#ifndef ENABLE_LIBRETRO
BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
#endif
