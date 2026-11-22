// IOSRuntime.mm — settings statics, the VM thread's CPU task queue and the render view glue
// SPDX-License-Identifier: GPL-3.0+

// Set SDL_MAIN_HANDLED to prevent SDL from redefining main()
#define SDL_MAIN_HANDLED


#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>
#import <GameController/GameController.h>

// SwiftUI integration — Xcode names the generated header after the Swift module.
#import <MetalKit/MetalKit.h>
#import "ARMSX2iOS-Swift.h"
#import <SwiftUI/SwiftUI.h>

// ... other includes ...
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include <chrono>
#include <cstdio>

#include "common/ProgressCallback.h"
#include "common/Error.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/Counters.h" // g_FrameCount
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/R5900.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVDdiscReader.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/GameList.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Config.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/ps2/BiosTools.h"
#include "pcsx2/SIO/Memcard/MemoryCardFile.h"

#include "pcsx2/DEV9/pcap_io.h"
#include "pcsx2/DEV9/net.h"

#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStreamTypes.h"
#include "pcsx2/INISettingsInterface.h"

#include "common/WindowInfo.h"
#include "common/HTTPDownloader.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <sys/stat.h> // For mkdir

struct rc_client_event_t;
struct rc_client_t;

// iOS specific headers
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#import <AVFoundation/AVFoundation.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include "common/Darwin/DarwinMisc.h"

#include "IOSRuntime.h"
#import "ARMSX2BridgeKeys.h"
#import "ARMSX2GameView.h"

// Game render view — frame-based layout, portrait=50% / landscape=full safe area
// Game render view with CAMetalLayer as backing layer (like MTKView)
// Game render view — backing layer (CAMetalLayer), manual landscape toggle
#include "pcsx2/MTGS.h"
#include "pcsx2/GS/GS.h"

#pragma mark - Init helpers
static std::vector<u8> s_imguiStandardFontData;

void ARMSX2ConfigureImGuiFonts(const char* reason)
{
    const std::string fontPath =
        EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
    std::optional<std::vector<u8>> fontData = FileSystem::ReadBinaryFile(fontPath.c_str());
    if (!fontData.has_value()) {
        std::fprintf(stderr, "@@IMGUI_FONT@@ ok=0 reason=%s path=\"%s\"\n", reason, fontPath.c_str());
        std::fflush(stderr);
        ImGuiManager::SetFonts({});
        return;
    }

    s_imguiStandardFontData = std::move(fontData.value());
    std::vector<ImGuiManager::FontInfo> fonts;
    fonts.push_back({
        std::span<const u8>(s_imguiStandardFontData.data(), s_imguiStandardFontData.size()),
        std::span<const u32>(),
        nullptr,
        false,
    });
    ImGuiManager::SetFonts(std::move(fonts));
}

#pragma mark - Statics & device stats
ARMSX2GameView* g_gameRenderView = nil;  // non-static: accessed from the bridge
INISettingsInterface* s_settings_interface = nullptr;
INISettingsInterface* s_secrets_settings_interface = nullptr;


#pragma mark - VM thread & CPU task queue
// Touch pad state
bool g_touchPadState[64] = {};

// Persistent VM thread lifecycle
std::atomic<bool> s_vmThreadActive{false};   // true while VM is executing
std::atomic<bool> s_requestVMStop{false};     // signal VM to stop from UI (extern for ARMSX2Bridge)
std::atomic<bool> s_requestVMBoot{false};     // signal VM thread to boot
std::mutex s_vmMutex;
std::condition_variable s_vmCV;
bool s_vmThreadCreated = false;               // guarded by s_vmMutex

std::thread::id s_cpuThreadId;
std::atomic<unsigned long long> s_cpuTaskNextId{1};
std::mutex s_cpuTaskMutex;
std::deque<std::shared_ptr<CPUThreadTask>> s_cpuTasks;

void ARMSX2DrainCPUThreadTasks()
{
    for (;;) {
        std::shared_ptr<CPUThreadTask> task;
        {
            std::lock_guard<std::mutex> lock(s_cpuTaskMutex);
            if (s_cpuTasks.empty())
                break;

            task = std::move(s_cpuTasks.front());
            s_cpuTasks.pop_front();
        }

        bool runs = false;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            runs = task->function && !task->cancelled;
            task->started = runs;
        }
        if (runs)
            task->function();

        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->complete = true;
        }
        task->cv.notify_all();
    }
}

// A task left over from a VM that ended must not run in the next one.
void ARMSX2DiscardCPUThreadTasks()
{
    std::deque<std::shared_ptr<CPUThreadTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(s_cpuTaskMutex);
        tasks.swap(s_cpuTasks);
    }
    for (const auto& task : tasks) {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->cancelled = true;
            task->complete = true;
        }
        task->cv.notify_all();
    }
}

extern "C" void ARMSX2_PostRuntimeMenuStateChanged(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSRuntimeMenuStateChanged object:nil];
    });
}

extern "C" void ARMSX2_PostEmulationOnlyStartupReady(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (VMManager::IsEmulationOnlyMode())
            return;
        [[NSNotificationCenter defaultCenter]
            postNotificationName:ARMSX2iOSEmulationOnlyStartupReady
            object:nil];
    });
}

// Gamepad button mapping — 16 PS2 buttons → SDL_GamepadButton
std::atomic<bool> s_captureMode{false};
std::atomic<int>  s_capturedButton{-1};

// Default mapping: PS2 index → SDL_GamepadButton
int s_buttonMap[16] = {
    SDL_GAMEPAD_BUTTON_DPAD_UP,        // 0  PAD_UP
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,      // 1  PAD_DOWN
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,      // 2  PAD_LEFT
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     // 3  PAD_RIGHT
    SDL_GAMEPAD_BUTTON_SOUTH,          // 4  PAD_CROSS
    SDL_GAMEPAD_BUTTON_EAST,           // 5  PAD_CIRCLE
    SDL_GAMEPAD_BUTTON_WEST,           // 6  PAD_SQUARE
    SDL_GAMEPAD_BUTTON_NORTH,          // 7  PAD_TRIANGLE
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  // 8  PAD_L1
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, // 9  PAD_R1
    -1,                                // 10 PAD_L2 (analog trigger)
    -1,                                // 11 PAD_R2 (analog trigger)
    SDL_GAMEPAD_BUTTON_START,          // 12 PAD_START
    SDL_GAMEPAD_BUTTON_BACK,           // 13 PAD_SELECT
    SDL_GAMEPAD_BUTTON_LEFT_STICK,     // 14 PAD_L3
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    // 15 PAD_R3
};
const int s_defaultMap[16] = {
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    -1, -1,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
};

#pragma mark - View controllers & render-view glue
// View controller references for background color switching. They stay
// __unsafe_unretained because UIKit and SwiftUI own the controllers, which
// outlive these references.
UIViewController* __unsafe_unretained s_menuVC = nil;
UIViewController* __unsafe_unretained s_rootVC = nil;
UIViewController* __unsafe_unretained s_sdlRootVC = nil;

static void ARMSX2EnsureGameRenderViewOnMain(const char* reason) {
    if (g_gameRenderView)
        return;

    g_gameRenderView = [[ARMSX2GameView alloc] initWithFrame:CGRectZero];
    g_gameRenderView.backgroundColor = [UIColor blackColor];
    g_gameRenderView.clipsToBounds = YES;
    [g_gameRenderView setNeedsLayout];
    Console.WriteLn("[Layout] Game render view prepared for SwiftUI (reason=%s)",
        reason ? reason : "unknown");
}

extern "C" void ARMSX2_PrepareGameRenderViewForCurrentRenderer(const char* reason) {
    if ([NSThread isMainThread]) {
        ARMSX2EnsureGameRenderViewOnMain(reason);
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{
            ARMSX2EnsureGameRenderViewOnMain(reason);
        });
    }
}
