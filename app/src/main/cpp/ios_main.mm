// Set SDL_MAIN_HANDLED to prevent SDL from redefining main()
#define SDL_MAIN_HANDLED

#if defined(iPSX2_MACOS)
// ============================================================================
// [P63] macOS native build — full PCSX2 initialization + SDL Metal window
// ============================================================================
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#import <Cocoa/Cocoa.h>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/WindowInfo.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/CDVD/CDVDcommon.h"

// --- Global state ---
static SDL_Window* s_macos_window = nullptr;
static INISettingsInterface* s_macos_settings = nullptr;
static std::atomic<bool> s_vm_running{false};
static std::atomic<bool> s_vm_quit{false};

// Host::AcquireRenderWindow — real implementation for macOS Metal
namespace Host {
    SDL_Window* g_sdl_window = nullptr;
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window) {
    if (!g_sdl_window) {
        Console.Error("[P63] AcquireRenderWindow: g_sdl_window is NULL");
        return std::nullopt;
    }
    WindowInfo wi = {};
    wi.type = WindowInfo::Type::MacOS;

    SDL_PropertiesID props = SDL_GetWindowProperties(g_sdl_window);
    NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!nswin) {
        Console.Error("[P63] AcquireRenderWindow: no NSWindow from SDL");
        return std::nullopt;
    }
    wi.window_handle = (__bridge void*)[nswin contentView];

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_sdl_window, &w, &h);
    wi.surface_width = (u32)w;
    wi.surface_height = (u32)h;
    wi.surface_scale = SDL_GetWindowDisplayScale(g_sdl_window);

    SDL_DisplayID display = SDL_GetDisplayForWindow(g_sdl_window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    wi.surface_refresh_rate = mode ? mode->refresh_rate : 60.0f;

    Console.WriteLn("[P63] AcquireRenderWindow: %ux%u scale=%.2f",
        wi.surface_width, wi.surface_height, wi.surface_scale);
    return wi;
}

// --- Helper: setup directories ---
static void SetupDirectories(const std::string& dataRoot) {
    const char* dirs[] = {"bios", "iso", "logs", "memcards", "savestates",
                          "snaps", "cheats", "patches", "cache", "covers",
                          "gamesettings", "textures", "inputprofiles", "videos",
                          "inis", "resources"};
    mkdir(dataRoot.c_str(), 0755);
    for (auto d : dirs)
        mkdir((dataRoot + "/" + d).c_str(), 0755);

    EmuFolders::DataRoot = dataRoot;
    EmuFolders::AppRoot = dataRoot;
    EmuFolders::Resources = dataRoot;
    EmuFolders::Settings = dataRoot + "/inis";
    EmuFolders::Bios = dataRoot + "/bios";
    EmuFolders::Logs = dataRoot + "/logs";
    EmuFolders::Savestates = dataRoot + "/savestates";
    EmuFolders::MemoryCards = dataRoot + "/memcards";
    EmuFolders::Snapshots = dataRoot + "/snaps";
    EmuFolders::Cheats = dataRoot + "/cheats";
    EmuFolders::Patches = dataRoot + "/patches";
    EmuFolders::Cache = dataRoot + "/cache";
    EmuFolders::Covers = dataRoot + "/covers";
    EmuFolders::GameSettings = dataRoot + "/gamesettings";
    EmuFolders::Textures = dataRoot + "/textures";
    EmuFolders::InputProfiles = dataRoot + "/inputprofiles";
    EmuFolders::Videos = dataRoot + "/videos";
    EmuFolders::UserResources = dataRoot + "/resources";
}

// --- Helper: find BIOS in bios/ directory ---
static bool FindAndConfigureBIOS(const std::string& biosDir) {
    // Check existing config first
    if (!EmuConfig.BaseFilenames.Bios.empty()) {
        std::string path = Path::Combine(biosDir, EmuConfig.BaseFilenames.Bios);
        if (FileSystem::FileExists(path.c_str())) {
            Console.WriteLn("[P63] BIOS from config: %s", EmuConfig.BaseFilenames.Bios.c_str());
            return true;
        }
    }
    // Auto-scan for .bin files
    FileSystem::FindResultsArray results;
    if (FileSystem::FindFiles(biosDir.c_str(), "*", FILESYSTEM_FIND_FILES, &results)) {
        for (const auto& fd : results) {
            if (fd.Size >= 1024*1024 && fd.Size <= 50*1024*1024) {
                std::string fn(Path::GetFileName(fd.FileName));
                std::string ext = fn.size() >= 4 ? fn.substr(fn.size() - 4) : "";
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".bin" || ext == ".rom") {
                    EmuConfig.BaseFilenames.Bios = fn;
                    if (s_macos_settings) {
                        s_macos_settings->SetStringValue("Filenames", "BIOS", fn.c_str());
                        s_macos_settings->Save();
                    }
                    Console.WriteLn("[P63] BIOS auto-detected: %s", fn.c_str());
                    return true;
                }
            }
        }
    }
    return false;
}

// --- Helper: initialize settings ---
static void InitSettings(const std::string& dataRoot) {
    std::string iniPath = dataRoot + "/PCSX2-macOS.ini";
    s_macos_settings = new INISettingsInterface(iniPath);
    if (!s_macos_settings->Load()) {
        Console.WriteLn("[P63] Creating new config: %s", iniPath.c_str());
        s_macos_settings->SetIntValue("EmuCore/CPU", "CoreType", 0);
        s_macos_settings->SetBoolValue("EmuCore/CPU", "UseArm64Dynarec", false);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU", "EnableSparseMemory", true);
        s_macos_settings->SetStringValue("SPU2/Output", "Backend", "SDL");
        s_macos_settings->SetIntValue("EmuCore/GS", "VsyncQueueSize", 8);
        s_macos_settings->SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
        s_macos_settings->Save();
    }
    Host::Internal::SetBaseSettingsLayer(s_macos_settings);
}

// --- OSD title bar update ---
#include "pcsx2/PerformanceMetrics.h"
static std::string s_titlebar_game;

static void UpdateWindowTitle() {
    if (!s_macos_window) return;
    char buf[256];
    int fps = (int)PerformanceMetrics::GetFPS();
    int speed = (int)PerformanceMetrics::GetSpeed();
    if (!s_titlebar_game.empty())
        snprintf(buf, sizeof(buf), "iPSX2 — %s | %d FPS | %d%%", s_titlebar_game.c_str(), fps, speed);
    else
        snprintf(buf, sizeof(buf), "iPSX2 — BIOS | %d FPS | %d%%", fps, speed);
    SDL_SetWindowTitle(s_macos_window, buf);
}

// --- Build boot parameters from INI ---
static VMBootParameters BuildBootParams() {
    VMBootParameters bp;
    bp.fast_boot = false;
    std::string isoDir = EmuFolders::DataRoot + "/iso";
    std::string isoFilename = s_macos_settings->GetStringValue("GameISO", "BootISO", "");
    bool fastBoot = s_macos_settings->GetBoolValue("GameISO", "FastBoot", false);
    if (!isoFilename.empty()) {
        std::string isoPath = (!isoFilename.empty() && isoFilename.front() == '/') ? isoFilename : (isoDir + "/" + isoFilename);
        if (isoFilename.front() != '/' && !FileSystem::FileExists(isoPath.c_str())) {
            std::string rootPath = EmuFolders::DataRoot + "/" + isoFilename;
            if (FileSystem::FileExists(rootPath.c_str()))
                isoPath = rootPath;
        }
        if (FileSystem::FileExists(isoPath.c_str())) {
            bp.filename = isoPath;
            bp.source_type = CDVD_SourceType::Iso;
            bp.fast_boot = fastBoot;
            Console.WriteLn("[P63] ISO: %s fast_boot=%d", isoPath.c_str(), fastBoot);
        }
    }
    return bp;
}

// --- VM thread (persistent, with restart support) ---
static void VMThreadFunc() {
    Console.WriteLn("[P63] VM thread: CPUThreadInitialize...");
    if (!VMManager::Internal::CPUThreadInitialize()) {
        Console.Error("[P63] CPUThreadInitialize failed!");
        s_vm_running.store(false);
        return;
    }

    // Persistent boot loop — handles VM restart after PS2LOGO→game transitions
    while (!s_vm_quit.load()) {
        if (EmuConfig.BaseFilenames.Bios.empty() ||
            !FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
            Console.Error("[P63] BIOS not found!");
            break;
        }

        VMBootParameters boot_params = BuildBootParams();
        Console.WriteLn("[P63] Booting with BIOS=%s", EmuConfig.BaseFilenames.Bios.c_str());

        if (VMManager::Initialize(boot_params) == VMBootResult::StartupSuccess) {
            Console.WriteLn("[P63] VM initialized, entering run loop...");
            VMManager::SetState(VMState::Running);

            // Update title with game name
            s_titlebar_game = s_macos_settings->GetStringValue("GameISO", "BootISO", "");

            while (!s_vm_quit.load()) {
                VMState state = VMManager::GetState();
                if (state == VMState::Stopping || state == VMState::Shutdown)
                    break;
                if (state == VMState::Running)
                    VMManager::Execute();
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            Console.WriteLn("[P63] VM run loop exited (state=%d, quit=%d)",
                (int)VMManager::GetState(), s_vm_quit.load() ? 1 : 0);

            if (!s_vm_quit.load()) {
                // VM stopped but app still running — restart (PS2LOGO→game transition)
                Console.WriteLn("[P63] VM restarting...");
                VMManager::Shutdown(false);
                continue;
            }

            VMManager::Shutdown(false);
        } else {
            Console.Error("[P63] VMManager::Initialize failed!");
        }
        break;
    }

    VMManager::Internal::CPUThreadShutdown();
    s_vm_running.store(false);
    Console.WriteLn("[P63] VM thread exited.");
}

// --- macOS main ---
int main(int argc, char* argv[]) {
    @autoreleasepool {
        // 1. Data root
        const char* home = getenv("HOME");
        std::string dataRoot = std::string(home ? home : "/tmp") + "/Documents/iPSX2";
        SetupDirectories(dataRoot);

        // 2. Logging
        std::string logPath = dataRoot + "/pcsx2_log.txt";
        freopen(logPath.c_str(), "w", stderr);
        dup2(fileno(stderr), fileno(stdout));
        setvbuf(stderr, NULL, _IONBF, 0);
        setvbuf(stdout, NULL, _IONBF, 0);

        fprintf(stderr, "[P63] iPSX2 macOS native build starting...\n");
        fprintf(stderr, "[P63] DataRoot: %s\n", dataRoot.c_str());
        fprintf(stderr, "[P63] Log: %s\n", logPath.c_str());

        // 3. Settings
        InitSettings(dataRoot);
        VMManager::Internal::LoadStartupSettings();
        VMManager::ApplySettings();

        // 4. BIOS
        if (!FindAndConfigureBIOS(EmuFolders::Bios)) {
            fprintf(stderr, "[P63] ERROR: No BIOS found in %s/bios/\n", dataRoot.c_str());
            fprintf(stderr, "[P63] Copy a PS2 BIOS .bin file there and restart.\n");
            // Still open window to show the user something
        }

        // 5. SDL + Window
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) < 0) {
            fprintf(stderr, "[P63] SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        s_macos_window = SDL_CreateWindow("iPSX2 macOS",
            1280, 720, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE);
        if (!s_macos_window) {
            fprintf(stderr, "[P63] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        Host::g_sdl_window = s_macos_window;
        Console.WriteLn("[P63] SDL window created.");

        // 6. Setup macOS native menu bar
        @autoreleasepool {
            NSMenu* menuBar = [[NSMenu alloc] init];

            // App menu
            NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
            NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"iPSX2"];
            [appMenu addItemWithTitle:@"About iPSX2" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:@"Quit iPSX2" action:@selector(terminate:) keyEquivalent:@"q"];
            [appMenuItem setSubmenu:appMenu];
            [menuBar addItem:appMenuItem];

            // Emulation menu
            NSMenuItem* emuMenuItem = [[NSMenuItem alloc] init];
            NSMenu* emuMenu = [[NSMenu alloc] initWithTitle:@"Emulation"];
            [emuMenu addItemWithTitle:@"Pause / Resume" action:nil keyEquivalent:@"p"];
            [emuMenu addItemWithTitle:@"Reset" action:nil keyEquivalent:@"r"];
            [emuMenu addItem:[NSMenuItem separatorItem]];
            // CPU settings submenu
            NSMenuItem* cpuItem = [[NSMenuItem alloc] init];
            NSMenu* cpuMenu = [[NSMenu alloc] initWithTitle:@"CPU"];
            [cpuMenu addItemWithTitle:@"EE: JIT Recompiler" action:nil keyEquivalent:@""];
            [cpuMenu addItemWithTitle:@"EE: Interpreter" action:nil keyEquivalent:@""];
            [cpuMenu addItem:[NSMenuItem separatorItem]];
            [cpuMenu addItemWithTitle:@"IOP Recompiler" action:nil keyEquivalent:@""];
            [cpuMenu addItemWithTitle:@"VU0 Recompiler" action:nil keyEquivalent:@""];
            [cpuMenu addItemWithTitle:@"VU1 Recompiler" action:nil keyEquivalent:@""];
            [cpuItem setSubmenu:cpuMenu];
            [emuMenu addItem:cpuItem];
            // Speedhacks submenu
            NSMenuItem* hackItem = [[NSMenuItem alloc] init];
            NSMenu* hackMenu = [[NSMenu alloc] initWithTitle:@"Speed Hacks"];
            [hackMenu addItemWithTitle:@"MTVU (Multi-Threaded VU1)" action:nil keyEquivalent:@""];
            [hackMenu addItemWithTitle:@"Instant VU1" action:nil keyEquivalent:@""];
            [hackMenu addItemWithTitle:@"Fastmem" action:nil keyEquivalent:@""];
            [hackItem setSubmenu:hackMenu];
            [emuMenu addItem:hackItem];
            [emuMenuItem setSubmenu:emuMenu];
            [menuBar addItem:emuMenuItem];

            // GS menu
            NSMenuItem* gsMenuItem = [[NSMenuItem alloc] init];
            NSMenu* gsMenu = [[NSMenu alloc] initWithTitle:@"Graphics"];
            [gsMenu addItemWithTitle:@"Renderer: Metal" action:nil keyEquivalent:@""];
            [gsMenu addItem:[NSMenuItem separatorItem]];
            [gsMenu addItemWithTitle:@"Show FPS" action:nil keyEquivalent:@""];
            [gsMenu addItemWithTitle:@"Show CPU Usage" action:nil keyEquivalent:@""];
            [gsMenuItem setSubmenu:gsMenu];
            [menuBar addItem:gsMenuItem];

            // Window menu
            NSMenuItem* winMenuItem = [[NSMenuItem alloc] init];
            NSMenu* winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
            [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
            [winMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
            [winMenu addItem:[NSMenuItem separatorItem]];
            [winMenu addItemWithTitle:@"Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            [winMenuItem setSubmenu:winMenu];
            [menuBar addItem:winMenuItem];

            [NSApp setMainMenu:menuBar];
        }

        // 7. Launch VM thread (if BIOS found)
        std::thread vmThread;
        if (!EmuConfig.BaseFilenames.Bios.empty()) {
            s_vm_running.store(true);
            vmThread = std::thread(VMThreadFunc);
        }

        // 8. Main event loop (must run on main thread for Cocoa/Metal)
        bool running = true;
        uint32_t titleUpdateCounter = 0;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_RESIZED: {
                        int w = event.window.data1, h = event.window.data2;
                        Console.WriteLn("[P63] Window resized: %dx%d", w, h);
                        break;
                    }
                    default:
                        break;
                }
            }

            // Update window title bar with OSD info (~1Hz)
            if (++titleUpdateCounter >= 60) {
                titleUpdateCounter = 0;
                UpdateWindowTitle();
            }

            // Check if VM thread has exited
            if (!s_vm_running.load() && vmThread.joinable()) {
                // VM finished — don't quit the app, let user close window
            }

            SDL_Delay(16);
        }

        // 9. Shutdown
        Console.WriteLn("[P63] Quit requested, shutting down...");
        s_vm_quit.store(true);
        if (vmThread.joinable())
            vmThread.join();

        SDL_DestroyWindow(s_macos_window);
        SDL_Quit();
        Console.WriteLn("[P63] Clean exit.");
        return 0;
    }
}
#else // TARGET_OS_IPHONE — original iOS code below

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>

// SwiftUI integration — Xcode names the generated header after the Swift module.
#if __has_include("ARMSX2iOS-Swift.h")
#import "ARMSX2iOS-Swift.h"
#define ARMSX2_HAS_SWIFTUI 1
#elif __has_include("ARMSX2-Swift.h")
#import "ARMSX2-Swift.h"
#define ARMSX2_HAS_SWIFTUI 1
#else
#define ARMSX2_HAS_SWIFTUI 0
#endif
#import <SwiftUI/SwiftUI.h>

// ... other includes ...
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include <chrono>

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
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Config.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/ps2/BiosTools.h"

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
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include "common/Darwin/DarwinMisc.h"

// Global Log View
static UITextView* g_logView = nil;

// Game render view — frame-based layout, portrait=50% / landscape=full safe area
// Game render view with CAMetalLayer as backing layer (like MTKView)
// Game render view — backing layer (CAMetalLayer), manual landscape toggle
#include "pcsx2/MTGS.h"
extern void GSResizeDisplayWindow(u32 width, u32 height, float scale);

static std::vector<u8> s_imguiStandardFontData;

static bool ARMSX2ShouldEnableMTVUByDefault(u32* physical_cores)
{
    u32 total_physical_cores = 0;
    for (const DarwinMisc::CPUClass& cpu_class : DarwinMisc::GetCPUClasses())
        total_physical_cores += cpu_class.num_physical;

    if (physical_cores)
        *physical_cores = total_physical_cores;

    return total_physical_cores >= 3;
}

static void ARMSX2EnsureIOSSpeedhackDefaults(SettingsInterface* si, const char* reason)
{
    if (!si)
        return;

    bool changed = false;
    if (!si->ContainsValue("EmuCore/Speedhacks", "WaitLoop")) {
        si->SetBoolValue("EmuCore/Speedhacks", "WaitLoop", true);
        changed = true;
    }
    if (!si->ContainsValue("EmuCore/Speedhacks", "IntcStat")) {
        si->SetBoolValue("EmuCore/Speedhacks", "IntcStat", true);
        changed = true;
    }
    if (!si->ContainsValue("EmuCore/Speedhacks", "vuFlagHack")) {
        si->SetBoolValue("EmuCore/Speedhacks", "vuFlagHack", true);
        changed = true;
    }
    if (!si->ContainsValue("EmuCore/Speedhacks", "vu1Instant")) {
        si->SetBoolValue("EmuCore/Speedhacks", "vu1Instant", true);
        changed = true;
    }

    u32 physical_cores = 0;
    const bool default_mtvu = ARMSX2ShouldEnableMTVUByDefault(&physical_cores);
    const bool has_vu_thread = si->ContainsValue("EmuCore/Speedhacks", "vuThread");
    const bool has_legacy_mtvu = si->ContainsValue("EmuCore/Speedhacks", "MTVU");
    const bool migrated = si->GetBoolValue("ARMSX2iOS/Migrations", "SpeedhackDefaultsV2", false);

    if (!migrated) {
        const bool current_vu_thread = si->GetBoolValue("EmuCore/Speedhacks", "vuThread", default_mtvu);
        const bool legacy_false_default =
            has_legacy_mtvu && !si->GetBoolValue("EmuCore/Speedhacks", "MTVU", false) && !current_vu_thread;
        const bool mtvu_value = (!has_vu_thread || legacy_false_default) ? default_mtvu : current_vu_thread;
        si->SetBoolValue("EmuCore/Speedhacks", "vuThread", mtvu_value);
        si->SetBoolValue("ARMSX2iOS/Migrations", "SpeedhackDefaultsV2", true);
        std::fprintf(stderr,
            "@@MTVU_DEFAULT@@ reason=%s physical=%u default=%d had_vuThread=%d had_legacy=%d legacy_false=%d value=%d\n",
            reason, physical_cores, default_mtvu ? 1 : 0, has_vu_thread ? 1 : 0, has_legacy_mtvu ? 1 : 0,
            legacy_false_default ? 1 : 0, mtvu_value ? 1 : 0);
        changed = true;
    } else {
        const bool current_vu_thread = si->GetBoolValue("EmuCore/Speedhacks", "vuThread", default_mtvu);
        std::fprintf(stderr,
            "@@MTVU_DEFAULT@@ reason=%s physical=%u default=%d migrated=1 value=%d\n",
            reason, physical_cores, default_mtvu ? 1 : 0, current_vu_thread ? 1 : 0);
    }
    std::fflush(stderr);

    if (has_legacy_mtvu) {
        si->DeleteValue("EmuCore/Speedhacks", "MTVU");
        changed = true;
    }

    if (changed)
        si->Save();
}

static bool ARMSX2RepairIOSARM64JITSettings(SettingsInterface* si, const char* reason)
{
    if (!si)
        return false;

    const int coreType = si->GetIntValue("EmuCore/CPU", "CoreType", 2);
    const bool useArm64 = si->GetBoolValue("EmuCore/CPU", "UseArm64Dynarec", coreType == 2);
    const bool enableEE = si->GetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
    const bool enableIOP = si->GetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
    const bool enableVU0 = si->GetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
    const bool enableVU1 = si->GetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
    const bool enableFastmem = si->GetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", true);
    u32 physicalCores = 0;
    const bool defaultMTVU = ARMSX2ShouldEnableMTVUByDefault(&physicalCores);
    const bool manualFastmem = si->GetBoolValue("ARMSX2iOS/Speedhacks", "ManualFastmem", false);
    const bool manualMTVU = si->GetBoolValue("ARMSX2iOS/Speedhacks", "ManualMTVU", false);
    const int manualMTVUVersion = si->GetIntValue("ARMSX2iOS/Speedhacks", "ManualMTVUVersion", 0);
    const bool mtvu = si->GetBoolValue("EmuCore/Speedhacks", "vuThread", defaultMTVU);
    const bool staleManualMTVUOff = defaultMTVU && manualMTVU && !mtvu && manualMTVUVersion < 3;
#if TARGET_OS_SIMULATOR
    const bool jitAvailable = false;
#else
    const bool jitAvailable = DarwinMisc::IsJITAvailable();
#endif
    NSString* systemVersion = [[UIDevice currentDevice] systemVersion] ?: @"unknown";
    NSString* deviceModel = [[UIDevice currentDevice] model] ?: @"unknown";
    std::fprintf(stderr,
        "@@IOS_JIT_POLICY@@ reason=%s ios=\"%s\" device=\"%s\" jit_probe=%d core=%d use_arm64=%d ee=%d iop=%d vu0=%d vu1=%d fastmem=%d manual_fastmem=%d mtvu=%d manual_mtvu=%d manual_mtvu_version=%d stale_manual_mtvu=%d physical=%u\n",
        reason ? reason : "unknown", systemVersion.UTF8String, deviceModel.UTF8String, jitAvailable ? 1 : 0,
        coreType, useArm64 ? 1 : 0, enableEE ? 1 : 0, enableIOP ? 1 : 0,
        enableVU0 ? 1 : 0, enableVU1 ? 1 : 0, enableFastmem ? 1 : 0,
        manualFastmem ? 1 : 0, mtvu ? 1 : 0, manualMTVU ? 1 : 0, manualMTVUVersion,
        staleManualMTVUOff ? 1 : 0, physicalCores);
    std::fflush(stderr);

    if (!jitAvailable || coreType == 1)
        return false;

    bool changed = false;
    auto setBoolIfNeeded = [&](const char* section, const char* key, bool value) {
        if (si->GetBoolValue(section, key, !value) != value) {
            si->SetBoolValue(section, key, value);
            changed = true;
        }
    };
    auto setIntIfNeeded = [&](const char* section, const char* key, int value) {
        if (si->GetIntValue(section, key, value == 2 ? 0 : 2) != value) {
            si->SetIntValue(section, key, value);
            changed = true;
        }
    };

    setIntIfNeeded("EmuCore/CPU", "CoreType", 2);
    setBoolIfNeeded("EmuCore/CPU", "UseArm64Dynarec", true);
    setBoolIfNeeded("EmuCore/CPU/Recompiler", "EnableEE", true);
    setBoolIfNeeded("EmuCore/CPU/Recompiler", "EnableIOP", true);
    setBoolIfNeeded("EmuCore/CPU/Recompiler", "EnableVU0", true);
    setBoolIfNeeded("EmuCore/CPU/Recompiler", "EnableVU1", true);
    if (!manualFastmem)
        setBoolIfNeeded("EmuCore/CPU/Recompiler", "EnableFastmem", true);
    if (staleManualMTVUOff) {
        si->DeleteValue("ARMSX2iOS/Speedhacks", "ManualMTVU");
        si->DeleteValue("ARMSX2iOS/Speedhacks", "ManualMTVUVersion");
        changed = true;
        std::fprintf(stderr, "@@IOS_STALE_MTVU_REPAIR@@ reason=%s old_mtvu=0 manual_version=%d action=enable_default\n",
            reason ? reason : "unknown", manualMTVUVersion);
        std::fflush(stderr);
    }
    if (defaultMTVU && (!manualMTVU || staleManualMTVUOff))
        setBoolIfNeeded("EmuCore/Speedhacks", "vuThread", true);

    if (changed) {
        si->Save();
        std::fprintf(stderr,
            "@@CPU_DEFAULT_FIX@@ reason=%s old_core=%d old_arm64=%d old_ee=%d old_iop=%d old_vu0=%d old_vu1=%d old_fastmem=%d old_mtvu=%d new_core=2 new_arm64=1 fastmem=%d mtvu=%d\n",
            reason ? reason : "unknown", coreType, useArm64 ? 1 : 0, enableEE ? 1 : 0, enableIOP ? 1 : 0,
            enableVU0 ? 1 : 0, enableVU1 ? 1 : 0, enableFastmem ? 1 : 0, mtvu ? 1 : 0,
            si->GetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", true) ? 1 : 0,
            si->GetBoolValue("EmuCore/Speedhacks", "vuThread", defaultMTVU) ? 1 : 0);
        std::fflush(stderr);
    }
    return changed;
}

static void ARMSX2ConfigureImGuiFonts(const char* reason)
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

    std::fprintf(stderr, "@@IMGUI_FONT@@ ok=1 reason=%s path=\"%s\" size=%zu\n",
        reason, fontPath.c_str(), s_imguiStandardFontData.size());
    std::fflush(stderr);
}

@interface ARMSX2GameView : UIView
@end
@implementation ARMSX2GameView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self)
        [self armsx2ApplyNativeContentScale];
    return self;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self armsx2ApplyNativeContentScale];
    [self setNeedsLayout];
}
- (CGFloat)armsx2NativeContentScale {
    UIScreen* screen = self.window.screen ?: UIScreen.mainScreen;
    CGFloat scale = screen.nativeScale > 0.0 ? screen.nativeScale : screen.scale;
    return scale > 0.0 ? scale : 1.0;
}
- (void)armsx2ApplyNativeContentScale {
    const CGFloat scale = [self armsx2NativeContentScale];
    self.contentScaleFactor = scale;
    self.layer.contentsScale = scale;
    ((CAMetalLayer*)self.layer).contentsScale = scale;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    [self armsx2ApplyNativeContentScale];
    if (self.bounds.size.width <= 0.0 || self.bounds.size.height <= 0.0)
        return;

    CGFloat scale = self.contentScaleFactor;
    CAMetalLayer *mtl = (CAMetalLayer *)self.layer;
    mtl.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                   self.bounds.size.height * scale);
    int w = std::max(1, (int)(self.bounds.size.width * scale + 0.5));
    int h = std::max(1, (int)(self.bounds.size.height * scale + 0.5));
    float s = (float)scale;
    MTGS::RunOnGSThread([w, h, s]() {
        GSResizeDisplayWindow(w, h, s);
    });
}
@end
ARMSX2GameView* g_gameRenderView = nil;  // non-static: accessed from ARMSX2Bridge.mm
static INISettingsInterface* s_settings_interface = nullptr;
static INISettingsInterface* s_secrets_settings_interface = nullptr;

static bool ARMSX2GetConfiguredFastBoot()
{
    if (!s_settings_interface)
        return false;

    return s_settings_interface->GetBoolValue(
        "GameISO", "FastBoot",
        s_settings_interface->GetBoolValue("EmuCore", "EnableFastBoot", false));
}

static int ARMSX2GetIOSMajorVersion()
{
    NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    return static_cast<int>(version.majorVersion);
}

static const char* ARMSX2DefaultJITScriptProtocol()
{
    // iOS 26+ previously defaulted to universal, but StikDebug's brk #0xf00d prepare path
    // can hang on large code regions. Keep legacy as the safe default until universal is stable.
    return "legacy";
}

static std::string ARMSX2NormalizeJITScriptProtocol(std::string jitProtocol)
{
    std::transform(jitProtocol.begin(), jitProtocol.end(), jitProtocol.begin(), ::tolower);
    if (jitProtocol == "utm-dolphin" || jitProtocol == "utm_dolphin")
        return "legacy";
    if (jitProtocol == "legacy" || jitProtocol == "universal")
        return jitProtocol;
    return {};
}

static void ARMSX2MigrateJITScriptProtocolForIOS(SettingsInterface* si, const char* reason)
{
    if (!si)
        return;

    const int iosMajor = ARMSX2GetIOSMajorVersion();
    const char* defaultProtocol = ARMSX2DefaultJITScriptProtocol();
    const bool hadProtocol = si->ContainsValue("ARMSX2iOS/JIT", "ScriptProtocol");
    const bool migrated = si->GetBoolValue("ARMSX2iOS/Migrations", "JITScriptProtocolByOSV1", false);
    const std::string currentProtocol = ARMSX2NormalizeJITScriptProtocol(
        si->GetStringValue("ARMSX2iOS/JIT", "ScriptProtocol", defaultProtocol));

    if (!hadProtocol || (!migrated && iosMajor > 0 && iosMajor < 26 && currentProtocol == "universal")) {
        si->SetStringValue("ARMSX2iOS/JIT", "ScriptProtocol", defaultProtocol);
        si->SetBoolValue("ARMSX2iOS/Migrations", "JITScriptProtocolByOSV1", true);
        si->Save();
        std::fprintf(stderr,
            "@@JIT_PROTOCOL_MIGRATE@@ reason=%s ios_major=%d had=%d from=%s to=%s\n",
            reason ? reason : "unknown", iosMajor, hadProtocol ? 1 : 0, currentProtocol.c_str(), defaultProtocol);
        std::fflush(stderr);
        return;
    }

    if (!migrated) {
        si->SetBoolValue("ARMSX2iOS/Migrations", "JITScriptProtocolByOSV1", true);
        si->Save();
    }
}

static std::string ARMSX2ResolveJITScriptProtocol()
{
    std::string jitProtocol = ARMSX2DefaultJITScriptProtocol();
    if (s_settings_interface)
    {
        jitProtocol = ARMSX2NormalizeJITScriptProtocol(
            s_settings_interface->GetStringValue("ARMSX2iOS/JIT", "ScriptProtocol", ARMSX2DefaultJITScriptProtocol()));
        if (jitProtocol != "legacy" && jitProtocol != "universal")
            jitProtocol = s_settings_interface->GetBoolValue(
                "ARMSX2iOS/JIT", "UseUniversalJITScript", ARMSX2GetIOSMajorVersion() >= 26) ? "universal" : "legacy";
    }
    return jitProtocol;
}

static void ARMSX2ApplyJITScriptProtocol(const char* reason)
{
    const std::string jitProtocol = ARMSX2ResolveJITScriptProtocol();
    setenv("ARMSX2_JIT_PROTOCOL", jitProtocol.c_str(), 1);
    std::fprintf(stderr, "@@JIT_PROTOCOL_SELECTED@@ reason=%s ios_major=%d protocol=%s\n",
        reason ? reason : "unknown", ARMSX2GetIOSMajorVersion(), jitProtocol.c_str());
    std::fflush(stderr);
}

static bool ARMSX2IOSRuntimeTelemetryEnabled()
{
    static std::atomic<int> s_enabled{-1};
    int enabled = s_enabled.load(std::memory_order_acquire);
    if (enabled >= 0)
        return enabled == 1;

    const char* value = std::getenv("iPSX2_IOS_PERF_TELEMETRY");
    if (!value || !value[0])
        value = std::getenv("SIMCTL_CHILD_iPSX2_IOS_PERF_TELEMETRY");

    enabled = (value && std::strcmp(value, "1") == 0) ? 1 : 0;
    int expected = -1;
    if (!s_enabled.compare_exchange_strong(expected, enabled, std::memory_order_acq_rel))
        enabled = expected;
    return enabled == 1;
}

static double ARMSX2IOSGetAppRAMGB()
{
    task_vm_info_data_t vm_info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) != KERN_SUCCESS)
        return 0.0;

    return static_cast<double>(vm_info.phys_footprint) / (1024.0 * 1024.0 * 1024.0);
}

static const char* ARMSX2IOSHeatStateName(NSProcessInfoThermalState state)
{
    switch (state) {
    case NSProcessInfoThermalStateNominal:
        return "OK";
    case NSProcessInfoThermalStateFair:
        return "Warm";
    case NSProcessInfoThermalStateSerious:
        return "Hot";
    case NSProcessInfoThermalStateCritical:
        return "Critical";
    default:
        return "Unknown";
    }
}

static constexpr bool ARMSX2IOSRetroAchievementsHardcoreAvailable = false;

static void ARMSX2DisableRetroAchievementsHardcoreForIOS(SettingsInterface* si, const char* reason)
{
    if (!si)
        return;

    const bool was_hardcore = si->GetBoolValue("Achievements", "ChallengeMode", false);
    si->SetBoolValue("Achievements", "ChallengeMode", false);

    if (was_hardcore) {
        Console.Warning("@@RA_IOS_HARDCORE_DISABLED@@ reason=%s",
            reason ? reason : "unknown");
    }
}

struct ARMSX2IOSDeviceStatsCache
{
    bool show = true;
    int severity = 0;
    std::string line;
    std::chrono::steady_clock::time_point last_update;
};

static std::mutex s_device_stats_mutex;
static ARMSX2IOSDeviceStatsCache s_device_stats_cache;

static const ARMSX2IOSDeviceStatsCache& ARMSX2IOSRefreshDeviceStatsCacheLocked()
{
    const auto now = std::chrono::steady_clock::now();
    if (!s_device_stats_cache.line.empty() &&
        (now - s_device_stats_cache.last_update) < std::chrono::seconds(1))
    {
        return s_device_stats_cache;
    }

    s_device_stats_cache.show = s_settings_interface ?
        s_settings_interface->GetBoolValue("ARMSX2iOS/UI", "OsdShowDeviceStats", true) : true;

    @autoreleasepool {
        UIDevice* device = [UIDevice currentDevice];
        const float battery = [device batteryLevel];
        const int battery_percent = (battery >= 0.0f) ? static_cast<int>(std::round(battery * 100.0f)) : -1;
        const NSProcessInfoThermalState thermal_state = [[NSProcessInfo processInfo] thermalState];
        const double app_ram_gb = ARMSX2IOSGetAppRAMGB();
        const bool low_power = [[NSProcessInfo processInfo] isLowPowerModeEnabled];

        if (thermal_state >= NSProcessInfoThermalStateSerious || (battery >= 0.0f && battery <= 0.15f))
            s_device_stats_cache.severity = 2;
        else if (thermal_state == NSProcessInfoThermalStateFair || (battery >= 0.0f && battery <= 0.30f))
            s_device_stats_cache.severity = 1;
        else
            s_device_stats_cache.severity = 0;

        char buffer[192];
        if (battery_percent >= 0) {
            std::snprintf(buffer, sizeof(buffer), "Battery: %d%% | Heat: %s | RAM: %.1f GB%s",
                battery_percent, ARMSX2IOSHeatStateName(thermal_state), app_ram_gb, low_power ? " | Low Power" : "");
        } else {
            std::snprintf(buffer, sizeof(buffer), "Battery: -- | Heat: %s | RAM: %.1f GB%s",
                ARMSX2IOSHeatStateName(thermal_state), app_ram_gb, low_power ? " | Low Power" : "");
        }

        s_device_stats_cache.line = buffer;
    }

    s_device_stats_cache.last_update = now;
    return s_device_stats_cache;
}

extern "C" bool ARMSX2_iOSShouldShowDeviceStatsOverlay()
{
    std::lock_guard<std::mutex> lock(s_device_stats_mutex);
    return ARMSX2IOSRefreshDeviceStatsCacheLocked().show;
}

extern "C" int ARMSX2_iOSGetDeviceStatsOverlaySeverity()
{
    std::lock_guard<std::mutex> lock(s_device_stats_mutex);
    return ARMSX2IOSRefreshDeviceStatsCacheLocked().severity;
}

extern "C" const char* ARMSX2_iOSGetDeviceStatsOverlayLine()
{
    std::lock_guard<std::mutex> lock(s_device_stats_mutex);
    return ARMSX2IOSRefreshDeviceStatsCacheLocked().line.c_str();
}

static float ARMSX2SanitizedNominalScalar(float scalar)
{
    if (!std::isfinite(scalar))
        return 1.0f;

    return std::clamp(scalar, 0.05f, 10.0f);
}

static void ARMSX2SanitizeFrameLimiterConfig(const char* reason)
{
    if (!s_settings_interface)
        return;

    const float raw = s_settings_interface->GetFloatValue("Framerate", "NominalScalar", 1.0f);
    const float sanitized = ARMSX2SanitizedNominalScalar(raw);
    if (std::fabs(raw - sanitized) > 0.001f) {
        Console.Warning("@@FRAMELIMIT@@ clamping unsupported NominalScalar %.3f -> %.3f reason=%s",
            raw, sanitized, reason ? reason : "unknown");
        s_settings_interface->SetFloatValue("Framerate", "NominalScalar", sanitized);
        s_settings_interface->Save();
    }

    EmuConfig.EmulationSpeed.NominalScalar = sanitized;
}

static void ARMSX2SetIOSOsdFlags(bool show_fps, bool show_vps, bool show_speed, bool show_cpu,
    bool show_gpu, bool show_resolution, bool show_gs_stats, bool show_indicators,
    bool show_settings, bool show_inputs, bool show_frame_times, bool show_version,
    bool show_hardware_info)
{
    EmuConfig.GS.OsdShowFPS = show_fps;
    GSConfig.OsdShowFPS = show_fps;
    EmuConfig.GS.OsdShowVPS = show_vps;
    GSConfig.OsdShowVPS = show_vps;
    EmuConfig.GS.OsdShowSpeed = show_speed;
    GSConfig.OsdShowSpeed = show_speed;
    EmuConfig.GS.OsdShowCPU = show_cpu;
    GSConfig.OsdShowCPU = show_cpu;
    EmuConfig.GS.OsdShowGPU = show_gpu;
    GSConfig.OsdShowGPU = show_gpu;
    EmuConfig.GS.OsdShowResolution = show_resolution;
    GSConfig.OsdShowResolution = show_resolution;
    EmuConfig.GS.OsdShowGSStats = show_gs_stats;
    GSConfig.OsdShowGSStats = show_gs_stats;
    EmuConfig.GS.OsdShowIndicators = show_indicators;
    GSConfig.OsdShowIndicators = show_indicators;
    EmuConfig.GS.OsdShowSettings = show_settings;
    GSConfig.OsdShowSettings = show_settings;
    EmuConfig.GS.OsdShowInputs = show_inputs;
    GSConfig.OsdShowInputs = show_inputs;
    EmuConfig.GS.OsdShowFrameTimes = show_frame_times;
    GSConfig.OsdShowFrameTimes = show_frame_times;
    EmuConfig.GS.OsdShowVersion = show_version;
    GSConfig.OsdShowVersion = show_version;
    EmuConfig.GS.OsdShowHardwareInfo = show_hardware_info;
    GSConfig.OsdShowHardwareInfo = show_hardware_info;
    EmuConfig.GS.OsdShowVideoCapture = false;
    GSConfig.OsdShowVideoCapture = false;
    EmuConfig.GS.OsdShowInputRec = false;
    GSConfig.OsdShowInputRec = false;
}

static void ARMSX2WriteIOSOsdFlagsToSettings()
{
    if (!s_settings_interface)
        return;

    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowFPS", EmuConfig.GS.OsdShowFPS);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowVPS", EmuConfig.GS.OsdShowVPS);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowSpeed", EmuConfig.GS.OsdShowSpeed);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowCPU", EmuConfig.GS.OsdShowCPU);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowGPU", EmuConfig.GS.OsdShowGPU);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowResolution", EmuConfig.GS.OsdShowResolution);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowGSStats", EmuConfig.GS.OsdShowGSStats);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowIndicators", EmuConfig.GS.OsdShowIndicators);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowSettings", EmuConfig.GS.OsdShowSettings);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowInputs", EmuConfig.GS.OsdShowInputs);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowFrameTimes", EmuConfig.GS.OsdShowFrameTimes);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowVersion", EmuConfig.GS.OsdShowVersion);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowHardwareInfo", EmuConfig.GS.OsdShowHardwareInfo);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowVideoCapture", false);
    s_settings_interface->SetBoolValue("EmuCore/GS", "OsdShowInputRec", false);
}

static void ARMSX2ApplyIOSOsdPresetFromConfig(const char* reason)
{
    if (!s_settings_interface)
        return;

    const int preset = std::clamp(s_settings_interface->GetIntValue("ARMSX2iOS/UI", "OsdPreset", 0), 0, 3);
    int position = s_settings_interface->GetIntValue("EmuCore/GS", "OsdPerformancePos", static_cast<int>(OsdOverlayPos::TopRight));
    if (position == static_cast<int>(OsdOverlayPos::TopCenter))
        position = static_cast<int>(OsdOverlayPos::TopRight);

    switch (preset) {
    case 1:
        ARMSX2SetIOSOsdFlags(true, false, true, true, true, false, false, true, false, false, false, true, false);
        break;
    case 2:
        ARMSX2SetIOSOsdFlags(true, true, true, true, true, true, false, true, false, false, false, true, false);
        break;
    case 3:
        ARMSX2SetIOSOsdFlags(true, true, true, true, true, true, true, true, true, true, true, true, true);
        break;
    default:
        ARMSX2SetIOSOsdFlags(false, false, false, false, false, false, false, false, false, false, false, false, false);
        position = 0;
        break;
    }

    if (preset != 0 && (position < static_cast<int>(OsdOverlayPos::TopLeft) || position > static_cast<int>(OsdOverlayPos::TopRight)))
        position = static_cast<int>(OsdOverlayPos::TopRight);

    EmuConfig.GS.OsdPerformancePos = static_cast<OsdOverlayPos>(position);
    GSConfig.OsdPerformancePos = static_cast<OsdOverlayPos>(position);
    ARMSX2WriteIOSOsdFlagsToSettings();
    s_settings_interface->SetIntValue("ARMSX2iOS/UI", "OsdPreset", preset);
    s_settings_interface->SetIntValue("EmuCore/GS", "OsdPerformancePos", position);
    s_settings_interface->Save();

    Console.WriteLn("@@OSD@@ preset=%d position=%d reason=%s fps=%d vps=%d speed=%d gpu=%d device_stats=%d frame_times=%d version=%d hardware=%d",
        preset, position, reason ? reason : "unknown",
        EmuConfig.GS.OsdShowFPS ? 1 : 0,
        EmuConfig.GS.OsdShowVPS ? 1 : 0,
        EmuConfig.GS.OsdShowSpeed ? 1 : 0,
        EmuConfig.GS.OsdShowGPU ? 1 : 0,
        ARMSX2_iOSShouldShowDeviceStatsOverlay() ? 1 : 0,
        EmuConfig.GS.OsdShowFrameTimes ? 1 : 0,
        EmuConfig.GS.OsdShowVersion ? 1 : 0,
        EmuConfig.GS.OsdShowHardwareInfo ? 1 : 0);
}

// Touch pad state
bool g_touchPadState[64] = {};

// Persistent VM thread lifecycle
static std::atomic<bool> s_vmThreadActive{false};   // true while VM is executing
static std::atomic<unsigned int> s_vmHeartbeatGeneration{0};
std::atomic<bool> s_requestVMStop{false};     // signal VM to stop from UI (extern for ARMSX2Bridge)
static std::atomic<bool> s_requestVMBoot{false};     // signal VM thread to boot
static std::mutex s_vmMutex;
static std::condition_variable s_vmCV;
static bool s_vmThreadCreated = false;               // guarded by s_vmMutex
static std::atomic<bool> s_vmThreadInitComplete{false};

static void ARMSX2SurfaceVMInitFailure(const char* reason, const char* message)
{
    std::fprintf(stderr, "@@BOOT_FAIL@@ reason=%s stage=cpu_thread_initialize\n", reason ? reason : "unknown");
    std::fflush(stderr);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_rootVC)
            s_rootVC.view.backgroundColor = [UIColor systemGroupedBackgroundColor];
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSReturnToMenu" object:nil];
    });
    Host::ReportErrorAsync("Startup Error", message ? message :
        "VM initialization failed. Try JIT Script: Legacy (UTM-Dolphin) in Settings, or restart ARMSX2 via StikDebug.");
}

static void ARMSX2ScheduleVMInitWatchdog()
{
    const int timeout_ms = []() {
        const char* env = std::getenv("ARMSX2_VM_INIT_TIMEOUT_MS");
        if (env && env[0]) {
            const int v = std::atoi(env);
            if (v > 0)
                return v;
        }
        return 15000;
    }();

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout_ms) * NSEC_PER_MSEC),
        dispatch_get_main_queue(), ^{
            if (s_vmThreadInitComplete.load(std::memory_order_acquire))
                return;

            std::fprintf(stderr, "@@BOOT_FAIL@@ reason=vm_thread_init_timeout timeout_ms=%d\n", timeout_ms);
            std::fflush(stderr);
            ARMSX2SurfaceVMInitFailure("vm_thread_init_timeout",
                "VM initialization timed out during iOS JIT memory setup. Try JIT Script: Legacy (UTM-Dolphin) in Settings, or restart ARMSX2 via StikDebug.");
        });
}

struct CPUThreadTask
{
    unsigned long long id = 0;
    std::function<void()> function;
    std::mutex mutex;
    std::condition_variable cv;
    bool complete = false;
};

static std::thread::id s_cpuThreadId;
static std::atomic<unsigned long long> s_cpuTaskNextId{1};
static std::mutex s_cpuTaskMutex;
static std::deque<std::shared_ptr<CPUThreadTask>> s_cpuTasks;

static void ARMSX2DrainCPUThreadTasks()
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

        if (task && task->function) {
            std::fprintf(stderr, "@@CPU_TASK_RUN@@ id=%llu\n", task->id);
            std::fflush(stderr);
            task->function();
        }

        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->complete = true;
        }
        task->cv.notify_all();
    }
}

extern "C" void ARMSX2_PostRetroAchievementsStateChanged(void);
extern "C" bool ARMSX2_StartExternalGameDirectoryAccess(const char* path);
extern "C" void ARMSX2_PostRuntimeMenuStateChanged(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRuntimeMenuStateChanged" object:nil];
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

static constexpr u32 ARMSX2_MAX_IOS_GAMEPADS = 4;
static SDL_Gamepad* s_gamepads[ARMSX2_MAX_IOS_GAMEPADS] = {};
static std::atomic<u32> s_pendingGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u32> s_gamepadRumbleStopGeneration[ARMSX2_MAX_IOS_GAMEPADS];
static u32 s_appliedGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS] = {};
static bool s_appliedGamepadRumbleValid[ARMSX2_MAX_IOS_GAMEPADS] = {};
static bool s_loggedGamepadRumbleFailure = false;
static bool s_loggedSDLGamepadRumble = false;
static bool s_loggedSDLGamepadRumbleForceStop = false;
static std::atomic<u32> s_loggedPadRumbleCommandCount{0};
static std::atomic<u32> s_loggedIgnoredPadRumbleCount{0};
static bool s_loggedMultitapRestartNeeded = false;
static constexpr u32 ARMSX2_GAMEPAD_RUMBLE_DURATION_MS = 220;
static constexpr double ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS = 0.30;
static constexpr u16 ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY = 0x7000;

static CHHapticEngine* s_nativePulseHapticEngine[ARMSX2_MAX_IOS_GAMEPADS] = {};
static std::atomic<u32> s_nativePulseHapticStopGeneration[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u32> s_loggedNativePulseHapticEvents{0};
static GCController* s_nativeHapticController = nil;
static CHHapticEngine* s_nativeHapticEngine = nil;
static id<CHHapticAdvancedPatternPlayer> s_nativeHapticPlayer = nil;
static u32 s_nativeAppliedGamepadRumble = 0;
static bool s_nativeAppliedGamepadRumbleValid = false;
static bool s_loggedNativeGamepadRumbleReady = false;
static bool s_loggedNativeGamepadRumbleUnavailable = false;
static std::atomic<u8> s_nativeGamepadDpadMask[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u8> s_nativeGamepadDpadLatchedMask[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u8> s_nativeGamepadAnyDpadMask{0};
static std::atomic<u8> s_nativeGamepadAnyDpadLatchedMask{0};
static std::atomic<u32> s_loggedNativeGamepadDpadEvents{0};
static std::atomic<u32> s_loggedNativeGamepadDpadApplyEvents{0};
static std::atomic<u32> s_loggedJoyConRumbleSkipped{0};
static id s_nativeGamepadConnectObserver = nil;
static id s_nativeGamepadDisconnectObserver = nil;

enum : u8
{
    ARMSX2_NATIVE_DPAD_UP = 1 << 0,
    ARMSX2_NATIVE_DPAD_DOWN = 1 << 1,
    ARMSX2_NATIVE_DPAD_LEFT = 1 << 2,
    ARMSX2_NATIVE_DPAD_RIGHT = 1 << 3,
};

static u8 ARMSX2NativeDpadBitForPS2Button(u32 ps2_button)
{
    switch (ps2_button)
    {
        case PadDualshock2::Inputs::PAD_UP:
            return ARMSX2_NATIVE_DPAD_UP;
        case PadDualshock2::Inputs::PAD_DOWN:
            return ARMSX2_NATIVE_DPAD_DOWN;
        case PadDualshock2::Inputs::PAD_LEFT:
            return ARMSX2_NATIVE_DPAD_LEFT;
        case PadDualshock2::Inputs::PAD_RIGHT:
            return ARMSX2_NATIVE_DPAD_RIGHT;
        default:
            return 0;
    }
}

static void ARMSX2RecomputeNativeGamepadAnyDpadMask()
{
    u8 any_mask = 0;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        any_mask |= s_nativeGamepadDpadMask[slot].load(std::memory_order_relaxed);

    s_nativeGamepadAnyDpadMask.store(any_mask, std::memory_order_relaxed);
}

static void ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask()
{
    u8 any_mask = 0;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        any_mask |= s_nativeGamepadDpadLatchedMask[slot].load(std::memory_order_relaxed);

    s_nativeGamepadAnyDpadLatchedMask.store(any_mask, std::memory_order_relaxed);
}

static u8 ARMSX2NativeDpadMaskForDirectionPad(GCControllerDirectionPad* dpad)
{
    if (!dpad)
        return 0;

    u8 mask = 0;
    if (dpad.up.pressed || dpad.yAxis.value > 0.35f)
        mask |= ARMSX2_NATIVE_DPAD_UP;
    if (dpad.down.pressed || dpad.yAxis.value < -0.35f)
        mask |= ARMSX2_NATIVE_DPAD_DOWN;
    if (dpad.left.pressed || dpad.xAxis.value < -0.35f)
        mask |= ARMSX2_NATIVE_DPAD_LEFT;
    if (dpad.right.pressed || dpad.xAxis.value > 0.35f)
        mask |= ARMSX2_NATIVE_DPAD_RIGHT;

    return mask;
}

static GCControllerDirectionPad* ARMSX2NativeDpadForController(GCController* controller)
{
    if (!controller)
        return nil;

    GCExtendedGamepad* extended = controller.extendedGamepad;
    if (extended && extended.dpad)
        return extended.dpad;

    GCPhysicalInputProfile* profile = controller.physicalInputProfile;
    if (profile && [profile respondsToSelector:@selector(dpads)]) {
        NSDictionary<NSString*, GCControllerDirectionPad*>* dpads = profile.dpads;
        for (NSString* key in dpads) {
            GCControllerDirectionPad* dpad = dpads[key];
            if (dpad)
                return dpad;
        }
    }

    return nil;
}

static void ARMSX2SetNativeGamepadDpadBit(u32 slot, u8 bit, bool pressed, const char* direction)
{
    if (slot >= ARMSX2_MAX_IOS_GAMEPADS || bit == 0)
        return;

    if (pressed) {
        s_nativeGamepadDpadLatchedMask[slot].fetch_or(bit, std::memory_order_relaxed);
        ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
    }

    const u8 old_mask = s_nativeGamepadDpadMask[slot].load(std::memory_order_relaxed);
    const u8 new_mask = pressed ? (old_mask | bit) : (old_mask & ~bit);
    if (new_mask == old_mask)
        return;

    s_nativeGamepadDpadMask[slot].store(new_mask, std::memory_order_relaxed);
    ARMSX2RecomputeNativeGamepadAnyDpadMask();
    const u32 log_index = s_loggedNativeGamepadDpadEvents.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 24) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad slot=%u dir=%s pressed=%u mask=0x%02x",
            slot + 1, direction ? direction : "unknown", pressed ? 1 : 0, new_mask);
    }
}

static void ARMSX2PollNativeGamepadDpadMasks(const char* reason)
{
    NSArray<GCController*>* controllers = [GCController controllers];
    u8 any_mask = 0;
    u32 slot = 0;
    for (GCController* controller in controllers) {
        if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
            break;

        const u8 mask = ARMSX2NativeDpadMaskForDirectionPad(ARMSX2NativeDpadForController(controller));
        s_nativeGamepadDpadMask[slot].store(mask, std::memory_order_relaxed);
        if (mask != 0)
            s_nativeGamepadDpadLatchedMask[slot].fetch_or(mask, std::memory_order_relaxed);
        any_mask |= mask;
        slot++;
    }

    for (; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        s_nativeGamepadDpadMask[slot].store(0, std::memory_order_relaxed);
        s_nativeGamepadDpadLatchedMask[slot].store(0, std::memory_order_relaxed);
    }

    s_nativeGamepadAnyDpadMask.store(any_mask, std::memory_order_relaxed);
    ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();

    static std::atomic<u32> s_loggedNativeGamepadPolls{0};
    const u32 log_index = s_loggedNativeGamepadPolls.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 8) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad poll reason=%s controllers=%u any=0x%02x",
            reason ? reason : "poll", static_cast<unsigned>(controllers.count), any_mask);
    }
}

static void ARMSX2RefreshNativeGamepadDpadHandlersOnMain(const char* reason)
{
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        s_nativeGamepadDpadMask[slot].store(0, std::memory_order_relaxed);
        s_nativeGamepadDpadLatchedMask[slot].store(0, std::memory_order_relaxed);
    }
    s_nativeGamepadAnyDpadMask.store(0, std::memory_order_relaxed);
    s_nativeGamepadAnyDpadLatchedMask.store(0, std::memory_order_relaxed);

    NSArray<GCController*>* controllers = [GCController controllers];
    u32 slot = 0;
    for (GCController* controller in controllers) {
        if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
            break;

        GCControllerDirectionPad* dpad = ARMSX2NativeDpadForController(controller);
        if (!dpad) {
            slot++;
            continue;
        }

        const u32 controller_slot = slot;
        const u8 initial_mask = ARMSX2NativeDpadMaskForDirectionPad(dpad);
        s_nativeGamepadDpadMask[controller_slot].store(initial_mask, std::memory_order_relaxed);
        if (initial_mask != 0)
            s_nativeGamepadDpadLatchedMask[controller_slot].store(initial_mask, std::memory_order_relaxed);

        dpad.up.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_UP, pressed, "up");
        };
        dpad.down.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_DOWN, pressed, "down");
        };
        dpad.left.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_LEFT, pressed, "left");
        };
        dpad.right.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_RIGHT, pressed, "right");
        };
        dpad.valueChangedHandler = ^(GCControllerDirectionPad* directionPad, float xValue, float yValue) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_UP, yValue > 0.35f, "up-axis");
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_DOWN, yValue < -0.35f, "down-axis");
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_LEFT, xValue < -0.35f, "left-axis");
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_RIGHT, xValue > 0.35f, "right-axis");
        };

        NSString* vendor = controller.vendorName ?: @"unknown";
        NSString* product = @"";
        if ([controller respondsToSelector:@selector(productCategory)])
            product = controller.productCategory ?: @"";
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad fallback slot=%u vendor=%s category=%s reason=%s",
            controller_slot + 1, vendor.UTF8String, product.UTF8String, reason ? reason : "refresh");

        slot++;
    }

    ARMSX2RecomputeNativeGamepadAnyDpadMask();
    ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
}

static void ARMSX2InstallNativeGamepadDpadObserversOnMain()
{
    if (s_nativeGamepadConnectObserver)
        return;

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    s_nativeGamepadConnectObserver = [center addObserverForName:GCControllerDidConnectNotification
                                                         object:nil
                                                          queue:[NSOperationQueue mainQueue]
                                                     usingBlock:^(NSNotification* notification) {
        ARMSX2RefreshNativeGamepadDpadHandlersOnMain("native-connect");
    }];
    s_nativeGamepadDisconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
                                                            object:nil
                                                             queue:[NSOperationQueue mainQueue]
                                                        usingBlock:^(NSNotification* notification) {
        ARMSX2RefreshNativeGamepadDpadHandlersOnMain("native-disconnect");
    }];
    ARMSX2RefreshNativeGamepadDpadHandlersOnMain("observer-install");
}

enum class ARMSX2IOSMultitapMode : int
{
    Auto = 0,
    Disabled = 1,
    Port1 = 2,
    Port2 = 3,
    Both = 4,
};

static ARMSX2IOSMultitapMode ARMSX2GetIOSMultitapMode()
{
    if (!s_settings_interface)
        return ARMSX2IOSMultitapMode::Auto;

    const int value = s_settings_interface->GetIntValue("ARMSX2iOS/Gamepad", "MultitapMode", 0);
    switch (value)
    {
        case 1:
            return ARMSX2IOSMultitapMode::Disabled;
        case 2:
            return ARMSX2IOSMultitapMode::Port1;
        case 3:
            return ARMSX2IOSMultitapMode::Port2;
        case 4:
            return ARMSX2IOSMultitapMode::Both;
        default:
            return ARMSX2IOSMultitapMode::Auto;
    }
}

static const char* ARMSX2IOSMultitapModeName(ARMSX2IOSMultitapMode mode)
{
    switch (mode)
    {
        case ARMSX2IOSMultitapMode::Disabled:
            return "Disabled";
        case ARMSX2IOSMultitapMode::Port1:
            return "Port 1";
        case ARMSX2IOSMultitapMode::Port2:
            return "Port 2";
        case ARMSX2IOSMultitapMode::Both:
            return "Port 1 + Port 2";
        case ARMSX2IOSMultitapMode::Auto:
        default:
            return "Auto";
    }
}

static bool ARMSX2IOSMultitapUsesPort1(ARMSX2IOSMultitapMode mode, u32 detected_controllers)
{
    switch (mode)
    {
        case ARMSX2IOSMultitapMode::Auto:
            return detected_controllers > 2;
        case ARMSX2IOSMultitapMode::Port1:
        case ARMSX2IOSMultitapMode::Both:
            return true;
        default:
            return false;
    }
}

static bool ARMSX2IOSMultitapUsesPort2(ARMSX2IOSMultitapMode mode)
{
    return mode == ARMSX2IOSMultitapMode::Port2 || mode == ARMSX2IOSMultitapMode::Both;
}

static bool ARMSX2IOSMapsPort1Multitap(ARMSX2IOSMultitapMode mode)
{
    if (mode == ARMSX2IOSMultitapMode::Auto)
        return EmuConfig.Pad.MultitapPort0_Enabled;

    return mode == ARMSX2IOSMultitapMode::Port1 || mode == ARMSX2IOSMultitapMode::Both;
}

static bool ARMSX2IOSMapsPort2Multitap(ARMSX2IOSMultitapMode mode)
{
    if (mode == ARMSX2IOSMultitapMode::Auto)
        return false;

    return ARMSX2IOSMultitapUsesPort2(mode);
}

static void ARMSX2EnsureIOSPadType(u32 unified_slot)
{
    if (!s_settings_interface || unified_slot >= Pad::NUM_CONTROLLER_PORTS)
        return;

    const std::string section = Pad::GetConfigSection(unified_slot);
    const std::string type = s_settings_interface->GetStringValue(section.c_str(), "Type", "");
    if (type.empty() || type == "None" || type == "NotConnected")
        s_settings_interface->SetStringValue(section.c_str(), "Type", "DualShock2");
}

static u32 ARMSX2DetectedSDLGamepadCount()
{
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_free(ids);
    return static_cast<u32>(std::max(count, 0));
}

static void ARMSX2ApplyIOSMultitapConfig(const char* reason)
{
    if (!s_settings_interface)
        return;

    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();
    const u32 detected = ARMSX2DetectedSDLGamepadCount();
    const bool port1 = ARMSX2IOSMultitapUsesPort1(mode, detected);
    const bool port2 = ARMSX2IOSMultitapUsesPort2(mode);

    s_settings_interface->SetBoolValue("Pad", "MultitapPort1", port1);
    s_settings_interface->SetBoolValue("Pad", "MultitapPort2", port2);
    EmuConfig.Pad.MultitapPort0_Enabled = port1;
    EmuConfig.Pad.MultitapPort1_Enabled = port2;
    s_loggedMultitapRestartNeeded = false;

    for (u32 controller = 0; controller < std::min<u32>(detected, ARMSX2_MAX_IOS_GAMEPADS); controller++) {
        u32 unified_slot = controller;
        if (port1) {
            unified_slot = (controller == 0) ? 0 : controller + 1;
        } else if (port2) {
            unified_slot = (controller <= 1) ? controller : controller + 3;
        } else if (controller > 1) {
            continue;
        }

        ARMSX2EnsureIOSPadType(unified_slot);
    }

    s_settings_interface->Save();
    Console.WriteLn("[ARMSX2 iOS Gamepad] Multitap mode=%s detected=%u port1=%d port2=%d reason=%s",
        ARMSX2IOSMultitapModeName(mode), detected, port1 ? 1 : 0, port2 ? 1 : 0, reason ? reason : "unknown");
}

static u32 ARMSX2PackGamepadRumble(float large_intensity, float small_intensity)
{
    const u32 large = static_cast<u32>(std::clamp(large_intensity, 0.0f, 1.0f) * 65535.0f);
    const u32 small = static_cast<u32>(std::clamp(small_intensity, 0.0f, 1.0f) * 65535.0f);
    return ((large & 0xffffu) << 16) | (small & 0xffffu);
}

static float ARMSX2RumbleLargeIntensity(u32 packed)
{
    return static_cast<float>((packed >> 16) & 0xffffu) / 65535.0f;
}

static float ARMSX2RumbleSmallIntensity(u32 packed)
{
    return static_cast<float>(packed & 0xffffu) / 65535.0f;
}

static u32 ARMSX2ConnectedGamepadCount()
{
    u32 count = 0;
    for (SDL_Gamepad* gamepad : s_gamepads)
    {
        if (gamepad && SDL_GamepadConnected(gamepad))
            count++;
    }
    return count;
}

static u32 ARMSX2PadSlotForGamepadIndex(u32 gamepad_index)
{
    if (gamepad_index == 0)
        return 0;

    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();

    // Two controllers should behave like normal PS2 ports 1/2. Three or four
    // controllers default to Port 1 multitap, which maps to 1A/1B/1C/1D.
    if (ARMSX2IOSMapsPort1Multitap(mode))
        return gamepad_index + 1;

    // Port 2 multitap is an escape hatch for games that look there instead:
    // controller 2 remains 2A, controller 3/4 become 2B/2C.
    if (ARMSX2IOSMapsPort2Multitap(mode)) {
        if (gamepad_index == 1)
            return 1;
        return gamepad_index + 3;
    }

    return (gamepad_index <= 1) ? gamepad_index : 0xffffffffu;
}

static int ARMSX2GamepadIndexForPadSlot(u32 pad_index)
{
    if (pad_index == 0)
        return 0;

    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();
    if (ARMSX2IOSMapsPort1Multitap(mode)) {
        if (pad_index >= 2 && pad_index <= 4)
            return static_cast<int>(pad_index - 1);
        return -1;
    }

    if (ARMSX2IOSMapsPort2Multitap(mode)) {
        if (pad_index == 1)
            return 1;
        if (pad_index >= 5 && pad_index <= 6)
            return static_cast<int>(pad_index - 3);
        return -1;
    }

    return (pad_index == 1) ? 1 : -1;
}

extern "C" void ARMSX2_iOSUpdatePadVibration(u32 pad_index, float large_intensity, float small_intensity)
{
    const int gamepad_index = ARMSX2GamepadIndexForPadSlot(pad_index);
    if (gamepad_index < 0 || static_cast<u32>(gamepad_index) >= ARMSX2_MAX_IOS_GAMEPADS) {
        const u32 count = s_loggedIgnoredPadRumbleCount.fetch_add(1, std::memory_order_relaxed);
        if (count < 4)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Ignoring rumble for unmapped pad=%u large=%.3f small=%.3f", pad_index, large_intensity, small_intensity);
        return;
    }

    const u32 packed = ARMSX2PackGamepadRumble(large_intensity, small_intensity);
    if (packed != 0) {
        const u32 count = s_loggedPadRumbleCommandCount.fetch_add(1, std::memory_order_relaxed);
        if (count < 12)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Queued rumble pad=%u controller=%d large=%.3f small=%.3f",
                pad_index, gamepad_index + 1, large_intensity, small_intensity);
    }

    s_pendingGamepadRumble[gamepad_index].store(packed, std::memory_order_relaxed);
}

static void ARMSX2StopNativeGamepadRumbleOnMain()
{
    if (s_nativeHapticPlayer) {
        NSError* error = nil;
        [s_nativeHapticPlayer stopAtTime:CHHapticTimeImmediate error:&error];
        if (error)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native rumble stop failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        s_nativeHapticPlayer = nil;
    }
}

static void ARMSX2ResetNativeGamepadRumbleOnMain()
{
    ARMSX2StopNativeGamepadRumbleOnMain();
    if (s_nativeHapticEngine) {
        [s_nativeHapticEngine stopWithCompletionHandler:nil];
        s_nativeHapticEngine = nil;
    }
    s_nativeHapticController = nil;
    s_nativeAppliedGamepadRumble = 0;
    s_nativeAppliedGamepadRumbleValid = false;
    s_loggedNativeGamepadRumbleReady = false;
}

static GCController* ARMSX2FindNativeHapticController()
{
    for (GCController* controller in [GCController controllers]) {
        if (controller.haptics)
            return controller;
    }

    return nil;
}

static bool ARMSX2EnsureNativeGamepadRumbleOnMain(float intensity, float sharpness)
{
    if (@available(iOS 14.0, *)) {
    } else {
        return false;
    }

    GCController* controller = ARMSX2FindNativeHapticController();
    if (!controller) {
        if (!s_loggedNativeGamepadRumbleUnavailable) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native controller haptics unavailable");
            s_loggedNativeGamepadRumbleUnavailable = true;
        }
        ARMSX2ResetNativeGamepadRumbleOnMain();
        return false;
    }

    if (s_nativeHapticController != controller) {
        ARMSX2ResetNativeGamepadRumbleOnMain();
        s_nativeHapticController = controller;
    }

    if (!s_nativeHapticEngine) {
        s_nativeHapticEngine = [controller.haptics createEngineWithLocality:GCHapticsLocalityAll];
        if (!s_nativeHapticEngine) {
            if (!s_loggedNativeGamepadRumbleUnavailable) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic engine creation failed");
                s_loggedNativeGamepadRumbleUnavailable = true;
            }
            return false;
        }

        s_nativeHapticEngine.playsHapticsOnly = YES;
        s_nativeHapticEngine.autoShutdownEnabled = YES;
        s_nativeHapticEngine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
            s_nativeHapticPlayer = nil;
            s_nativeAppliedGamepadRumble = 0;
            s_nativeAppliedGamepadRumbleValid = false;
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic engine stopped reason=%ld", static_cast<long>(reason));
        };
        s_nativeHapticEngine.resetHandler = ^{
            s_nativeHapticPlayer = nil;
            s_nativeAppliedGamepadRumble = 0;
            s_nativeAppliedGamepadRumbleValid = false;
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic engine reset");
        };
    }

    NSError* error = nil;
    if (![s_nativeHapticEngine startAndReturnError:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic engine start failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }

    if (!s_nativeHapticPlayer) {
        NSArray<CHHapticEventParameter*>* params = @[
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity],
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness]
        ];
        CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                              parameters:params
                                                            relativeTime:0.0
                                                                 duration:1.0];
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
        if (!pattern) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pattern failed: %s", error.localizedDescription.UTF8String ?: "unknown");
            return false;
        }

        s_nativeHapticPlayer = [s_nativeHapticEngine createAdvancedPlayerWithPattern:pattern error:&error];
        if (!s_nativeHapticPlayer) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic player failed: %s", error.localizedDescription.UTF8String ?: "unknown");
            return false;
        }

        s_nativeHapticPlayer.loopEnabled = YES;
        s_nativeHapticPlayer.loopEnd = 1.0;
        if (![s_nativeHapticPlayer startAtTime:CHHapticTimeImmediate error:&error]) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic player start failed: %s", error.localizedDescription.UTF8String ?: "unknown");
            s_nativeHapticPlayer = nil;
            return false;
        }

        if (!s_loggedNativeGamepadRumbleReady) {
            NSString* vendor = controller.vendorName ?: @"unknown controller";
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native controller rumble active: %s", vendor.UTF8String);
            s_loggedNativeGamepadRumbleReady = true;
        }
    }

    const float sharpnessControl = std::clamp((sharpness * 2.0f) - 1.0f, -1.0f, 1.0f);
    NSArray<CHHapticDynamicParameter*>* dynamicParams = @[
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0.0],
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:sharpnessControl relativeTime:0.0]
    ];

    if (![s_nativeHapticPlayer sendParameters:dynamicParams atTime:CHHapticTimeImmediate error:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic update failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        ARMSX2StopNativeGamepadRumbleOnMain();
        return false;
    }

    return true;
}

static void ARMSX2ApplyNativeGamepadRumbleOnMain(u32 packed)
{
	if (s_nativeAppliedGamepadRumbleValid && packed == s_nativeAppliedGamepadRumble)
		return;

    const float large = ARMSX2RumbleLargeIntensity(packed);
    const float small = ARMSX2RumbleSmallIntensity(packed);
    const float intensity = std::clamp(std::max(large, small), 0.0f, 1.0f);
    if (intensity <= 0.01f) {
        ARMSX2StopNativeGamepadRumbleOnMain();
        s_nativeAppliedGamepadRumble = packed;
        s_nativeAppliedGamepadRumbleValid = true;
        return;
    }

    const float sharpness = std::clamp(0.20f + (small * 0.65f) - (large * 0.10f), 0.0f, 1.0f);
    if (ARMSX2EnsureNativeGamepadRumbleOnMain(intensity, sharpness)) {
        s_nativeAppliedGamepadRumble = packed;
        s_nativeAppliedGamepadRumbleValid = true;
	}
}

static void ARMSX2StopNativeGamepadRumblePulseOnMain(u32 slot)
{
	if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
		return;

	s_nativePulseHapticStopGeneration[slot].fetch_add(1, std::memory_order_relaxed);
	if (s_nativePulseHapticEngine[slot]) {
		@try {
			[s_nativePulseHapticEngine[slot] stopWithCompletionHandler:nil];
		} @catch (NSException* exception) {
			Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic stop exception slot=%u name=%s reason=%s",
				slot + 1,
				exception.name.UTF8String ?: "unknown",
				exception.reason.UTF8String ?: "unknown");
		}
		s_nativePulseHapticEngine[slot] = nil;
	}
}

static GCController* ARMSX2FindNativeHapticControllerForSlot(u32 slot)
{
	NSArray<GCController*>* controllers = [GCController controllers];
	if (slot < controllers.count) {
		GCController* controller = controllers[slot];
		if (controller.haptics)
			return controller;
	}

	if (controllers.count == 1) {
		GCController* controller = controllers.firstObject;
		if (controller.haptics)
			return controller;
	}

	for (GCController* controller in controllers) {
		if (controller.haptics)
			return controller;
	}

	return nil;
}

static GCController* ARMSX2FindNativeControllerForSlot(u32 slot)
{
	NSArray<GCController*>* controllers = [GCController controllers];
	if (slot < controllers.count)
		return controllers[slot];

	if (controllers.count == 1)
		return controllers.firstObject;

	return nil;
}

static bool ARMSX2NativeControllerLooksLikeJoyCon(GCController* controller)
{
	if (!controller)
		return false;

	NSString* vendor = controller.vendorName ?: @"";
	NSString* category = @"";
	if ([controller respondsToSelector:@selector(productCategory)])
		category = controller.productCategory ?: @"";

	NSString* descriptor = [[NSString stringWithFormat:@"%@ %@", vendor, category] lowercaseString];
	return [descriptor containsString:@"joy-con"] ||
	       [descriptor containsString:@"joycon"] ||
	       [descriptor containsString:@"joy con"];
}

static bool ARMSX2CStringLooksLikeJoyCon(const char* value)
{
	if (!value || !*value)
		return false;

	NSString* descriptor = [NSString stringWithUTF8String:value];
	if (!descriptor)
		return false;

	descriptor = descriptor.lowercaseString;
	return [descriptor containsString:@"joy-con"] ||
	       [descriptor containsString:@"joycon"] ||
	       [descriptor containsString:@"joy con"];
}

static bool ARMSX2SDLGamepadLooksLikeJoyCon(SDL_Gamepad* gamepad)
{
	return gamepad && ARMSX2CStringLooksLikeJoyCon(SDL_GetGamepadName(gamepad));
}

static bool ARMSX2NativeControllerSlotLooksLikeJoyCon(u32 slot)
{
	return ARMSX2NativeControllerLooksLikeJoyCon(ARMSX2FindNativeControllerForSlot(slot));
}

static bool ARMSX2GamepadSlotLooksLikeJoyCon(u32 slot)
{
	if (ARMSX2NativeControllerSlotLooksLikeJoyCon(slot))
		return true;

	return slot < ARMSX2_MAX_IOS_GAMEPADS && ARMSX2SDLGamepadLooksLikeJoyCon(s_gamepads[slot]);
}

static bool ARMSX2ApplyNativeGamepadRumblePulseOnMain(u32 slot, u32 packed, const char* reason)
{
	if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
		return false;

	if (@available(iOS 14.0, *)) {
	} else {
		return false;
	}

	if (ARMSX2GamepadSlotLooksLikeJoyCon(slot)) {
		const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 16)
			Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con native rumble hard-disabled slot=%u reason=%s",
				slot + 1, reason ? reason : "unknown");
		return false;
	}

	const float large = ARMSX2RumbleLargeIntensity(packed);
	const float small = ARMSX2RumbleSmallIntensity(packed);
	const float raw_intensity = std::max(large, small);
	if (raw_intensity <= 0.01f) {
		ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
		return true;
	}

	GCController* controller = ARMSX2FindNativeHapticControllerForSlot(slot);
	if (ARMSX2NativeControllerLooksLikeJoyCon(controller)) {
		const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 16) {
			NSString* vendor = controller.vendorName ?: @"unknown";
			NSString* product = @"";
			if ([controller respondsToSelector:@selector(productCategory)])
				product = controller.productCategory ?: @"";
			Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con native rumble skipped slot=%u controller=%s category=%s reason=%s",
				slot + 1, vendor.UTF8String, product.UTF8String, reason ? reason : "unknown");
		}
		return false;
	}

	if (!controller || !controller.haptics) {
		const u32 log_index = s_loggedNativePulseHapticEvents.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 16) {
			Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse unavailable slot=%u reason=%s controllers=%u",
				slot + 1, reason ? reason : "unknown", static_cast<unsigned>([GCController controllers].count));
		}
		return false;
	}

	ARMSX2StopNativeGamepadRumblePulseOnMain(slot);

	GCHapticsLocality locality = GCHapticsLocalityDefault;
	NSSet<GCHapticsLocality>* localities = controller.haptics.supportedLocalities;
	if ([localities containsObject:GCHapticsLocalityAll])
		locality = GCHapticsLocalityAll;

	CHHapticEngine* engine = [controller.haptics createEngineWithLocality:locality];
	if (!engine) {
		const u32 log_index = s_loggedNativePulseHapticEvents.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 16) {
			NSString* vendor = controller.vendorName ?: @"unknown";
			Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse engine failed slot=%u controller=%s locality=%s reason=%s",
				slot + 1, vendor.UTF8String, locality.UTF8String, reason ? reason : "unknown");
		}
		return false;
	}

	engine.playsHapticsOnly = YES;
	engine.autoShutdownEnabled = YES;

	NSError* error = nil;
	if (![engine startAndReturnError:&error]) {
		NSString* vendor = controller.vendorName ?: @"unknown";
		Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse start failed slot=%u controller=%s: %s",
			slot + 1, vendor.UTF8String, error.localizedDescription.UTF8String ?: "unknown");
		return false;
	}

	const float intensity = std::clamp(raw_intensity, 0.10f, 0.55f);
	const float sharpness = std::clamp(0.25f + (small * 0.45f), 0.20f, 0.65f);
	NSArray<CHHapticEventParameter*>* params = @[
		[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity],
		[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness]
	];
	CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
	                                                     parameters:params
	                                                   relativeTime:0.0
	                                                        duration:0.18];
	CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
	if (!pattern) {
		Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse pattern failed slot=%u: %s",
			slot + 1, error.localizedDescription.UTF8String ?: "unknown");
		[engine stopWithCompletionHandler:nil];
		return false;
	}

	id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
	if (!player || ![player startAtTime:CHHapticTimeImmediate error:&error]) {
		Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse player failed slot=%u: %s",
			slot + 1, error.localizedDescription.UTF8String ?: "unknown");
		[engine stopWithCompletionHandler:nil];
		return false;
	}

	s_nativePulseHapticEngine[slot] = engine;
	const u32 stop_generation = s_nativePulseHapticStopGeneration[slot].fetch_add(1, std::memory_order_relaxed) + 1;
	const u32 log_index = s_loggedNativePulseHapticEvents.fetch_add(1, std::memory_order_relaxed);
	if (log_index < 16) {
		NSString* vendor = controller.vendorName ?: @"unknown";
		NSString* product = @"";
		if ([controller respondsToSelector:@selector(productCategory)])
			product = controller.productCategory ?: @"";
		Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse accepted slot=%u controller=%s category=%s locality=%s reason=%s intensity=%.2f",
			slot + 1, vendor.UTF8String, product.UTF8String, locality.UTF8String, reason ? reason : "unknown", intensity);
	}

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		if (slot >= ARMSX2_MAX_IOS_GAMEPADS ||
			s_nativePulseHapticStopGeneration[slot].load(std::memory_order_relaxed) != stop_generation)
			return;

		if (s_nativePulseHapticEngine[slot]) {
			@try {
				[s_nativePulseHapticEngine[slot] stopWithCompletionHandler:nil];
			} @catch (NSException* exception) {
				Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic delayed stop exception slot=%u name=%s reason=%s",
					slot + 1,
					exception.name.UTF8String ?: "unknown",
					exception.reason.UTF8String ?: "unknown");
			}
			s_nativePulseHapticEngine[slot] = nil;
		}
	});

	return true;
}

static bool ARMSX2ApplyNativeGamepadRumblePulseForJoyConOnMain(u32 slot, u32 packed, const char* reason)
{
	GCController* controller = ARMSX2FindNativeControllerForSlot(slot);
	if (!ARMSX2NativeControllerLooksLikeJoyCon(controller))
		return false;

	const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
	if (log_index < 16) {
		NSString* vendor = controller.vendorName ?: @"unknown";
		NSString* product = @"";
		if ([controller respondsToSelector:@selector(productCategory)])
			product = controller.productCategory ?: @"";
		Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con rumble skipped slot=%u controller=%s category=%s reason=%s",
			slot + 1, vendor.UTF8String, product.UTF8String, reason ? reason : "unknown");
	}
	return false;
}

static void ARMSX2ApplyPendingGamepadRumble(u32 gamepad_index)
{
	if (gamepad_index >= ARMSX2_MAX_IOS_GAMEPADS)
		return;

    const u32 packed = s_pendingGamepadRumble[gamepad_index].load(std::memory_order_relaxed);
    if (s_appliedGamepadRumbleValid[gamepad_index] && packed == s_appliedGamepadRumble[gamepad_index])
        return;

    const u16 large = std::min<u16>(static_cast<u16>((packed >> 16) & 0xffffu), ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY);
    const u16 small = std::min<u16>(static_cast<u16>(packed & 0xffffu), ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY);
    const bool wants_rumble = (large != 0 || small != 0);
    const u32 stop_generation = s_gamepadRumbleStopGeneration[gamepad_index].fetch_add(1, std::memory_order_relaxed) + 1;

    if (!wants_rumble) {
        const u32 slot = gamepad_index;
        dispatch_async(dispatch_get_main_queue(), ^{
            ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
        });
    }

	if (wants_rumble && ARMSX2NativeControllerSlotLooksLikeJoyCon(gamepad_index)) {
		const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 16)
			Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con rumble request ignored safely slot=%u", gamepad_index + 1);
		s_appliedGamepadRumble[gamepad_index] = packed;
		s_appliedGamepadRumbleValid[gamepad_index] = true;
		return;
	}

	if (wants_rumble && ARMSX2GamepadSlotLooksLikeJoyCon(gamepad_index)) {
		const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 16)
			Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con SDL rumble request ignored safely slot=%u name=%s",
				gamepad_index + 1,
				s_gamepads[gamepad_index] ? (SDL_GetGamepadName(s_gamepads[gamepad_index]) ?: "unknown") : "unknown");
		s_appliedGamepadRumble[gamepad_index] = packed;
		s_appliedGamepadRumbleValid[gamepad_index] = true;
		return;
	}

    if (s_gamepads[gamepad_index]) {
        if (SDL_RumbleGamepad(s_gamepads[gamepad_index], large, small, ARMSX2_GAMEPAD_RUMBLE_DURATION_MS)) {
            if (!s_loggedSDLGamepadRumble) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] SDL controller rumble accepted");
                s_loggedSDLGamepadRumble = true;
            }
            if (wants_rumble) {
                const u32 slot = gamepad_index;
                const u32 native_packed = packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumblePulseForJoyConOnMain(slot, native_packed, "joycon-sdl-mirror");
                });
            }
            if (wants_rumble) {
                const u32 slot = gamepad_index;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                    if (slot >= ARMSX2_MAX_IOS_GAMEPADS ||
                        s_gamepadRumbleStopGeneration[slot].load(std::memory_order_relaxed) != stop_generation)
                        return;

                    if (s_gamepads[slot]) {
                        SDL_RumbleGamepad(s_gamepads[slot], 0, 0, 0);
                        SDL_RumbleGamepadTriggers(s_gamepads[slot], 0, 0, 0);
                    }
                    ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
                    if (!s_loggedSDLGamepadRumbleForceStop) {
                        Console.WriteLn("[ARMSX2 iOS Gamepad] SDL controller rumble force-stopped");
                        s_loggedSDLGamepadRumbleForceStop = true;
                    }
                });
            }
        } else {
            if (!s_loggedGamepadRumbleFailure) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] SDL controller %u rumble unavailable: %s", gamepad_index + 1, SDL_GetError());
                s_loggedGamepadRumbleFailure = true;
            }
            if (wants_rumble) {
                const u32 slot = gamepad_index;
                const u32 native_packed = packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumblePulseOnMain(slot, native_packed, "sdl-fallback");
                });
            }
        }
    } else if (wants_rumble) {
        const u32 slot = gamepad_index;
        const u32 native_packed = packed;
        dispatch_async(dispatch_get_main_queue(), ^{
            ARMSX2ApplyNativeGamepadRumblePulseOnMain(slot, native_packed, "no-sdl-gamepad");
        });
    }

    s_appliedGamepadRumble[gamepad_index] = packed;
    s_appliedGamepadRumbleValid[gamepad_index] = true;
}

extern "C" void ARMSX2_iOSTestGamepadRumble(void)
{
    Console.WriteLn("[ARMSX2 iOS Gamepad] Test controller rumble requested");

    SDL_PumpEvents();
    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL detected=%d", count);
    if (ids) {
        for (int id_index = 0; id_index < count; id_index++) {
            bool already_open = false;
            for (SDL_Gamepad* gamepad : s_gamepads) {
                if (gamepad && SDL_GetGamepadID(gamepad) == ids[id_index]) {
                    already_open = true;
                    break;
                }
            }
            if (already_open)
                continue;

            for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
                if (!s_gamepads[slot]) {
                    s_gamepads[slot] = SDL_OpenGamepad(ids[id_index]);
                    Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL open slot=%u id=%d result=%s",
                        slot + 1, ids[id_index],
                        s_gamepads[slot] ? (SDL_GetGamepadName(s_gamepads[slot]) ?: "unknown") : SDL_GetError());
                    break;
                }
            }
        }
        SDL_free(ids);
    }

    bool anySDLGamepad = false;
    bool anyNativeFallback = false;
    const u32 test_packed = ARMSX2PackGamepadRumble(0.55f, 0.55f);
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        if (!s_gamepads[slot])
            continue;

		if (ARMSX2GamepadSlotLooksLikeJoyCon(slot)) {
			const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
			if (log_index < 16)
				Console.WriteLn("[ARMSX2 iOS Gamepad] Test Joy-Con rumble hard-disabled slot=%u name=%s",
					slot + 1, SDL_GetGamepadName(s_gamepads[slot]) ?: "unknown");
			continue;
		}

        anySDLGamepad = true;
        const bool ok = SDL_RumbleGamepad(s_gamepads[slot], ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY, ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY, 250);
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL controller %u rumble %s%s%s",
            slot + 1, ok ? "accepted" : "failed", ok ? "" : ": ", ok ? "" : SDL_GetError());
        if (!ok) {
            const u32 native_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2ApplyNativeGamepadRumblePulseOnMain(native_slot, test_packed, "test-sdl-fallback");
            });
            anyNativeFallback = true;
        }
    }
    if (!anySDLGamepad) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL rumble skipped: no SDL gamepad open");
        for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
            if (ARMSX2NativeControllerSlotLooksLikeJoyCon(slot)) {
                const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
                if (log_index < 16)
                    Console.WriteLn("[ARMSX2 iOS Gamepad] Test native Joy-Con rumble skipped slot=%u", slot + 1);
                continue;
            }
            const u32 native_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2ApplyNativeGamepadRumblePulseOnMain(native_slot, test_packed, "test-no-sdl-gamepad");
            });
        }
        anyNativeFallback = true;
    }

    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        s_pendingGamepadRumble[slot].store(0, std::memory_order_relaxed);
        s_appliedGamepadRumble[slot] = 0;
        s_appliedGamepadRumbleValid[slot] = false;
    }

    Console.WriteLn("[ARMSX2 iOS Gamepad] Native CoreHaptics pulse fallback %s", anyNativeFallback ? "queued when needed" : "not queued");

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.30 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
            if (s_gamepads[slot]) {
                SDL_RumbleGamepad(s_gamepads[slot], 0, 0, 0);
                SDL_RumbleGamepadTriggers(s_gamepads[slot], 0, 0, 0);
            }
            ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
            s_pendingGamepadRumble[slot].store(0, std::memory_order_relaxed);
            s_appliedGamepadRumble[slot] = 0;
            s_appliedGamepadRumbleValid[slot] = false;
        }
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test controller rumble stopped");
    });
}

static void ARMSX2RefreshIOSGamepads()
{
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    ARMSX2PollNativeGamepadDpadMasks("sdl-refresh");

    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        SDL_Gamepad* gamepad = s_gamepads[slot];
        if (!gamepad)
            continue;

        if (!SDL_GamepadConnected(gamepad)) {
            Console.WriteLn("[Files] MFi gamepad %u disconnected", slot + 1);
            s_pendingGamepadRumble[slot].store(0, std::memory_order_relaxed);
            s_gamepadRumbleStopGeneration[slot].fetch_add(1, std::memory_order_relaxed);
            s_appliedGamepadRumble[slot] = 0;
            s_appliedGamepadRumbleValid[slot] = false;
            s_nativeGamepadDpadMask[slot].store(0, std::memory_order_relaxed);
            s_nativeGamepadDpadLatchedMask[slot].store(0, std::memory_order_relaxed);
            ARMSX2RecomputeNativeGamepadAnyDpadMask();
            ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
            const u32 disconnected_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2StopNativeGamepadRumblePulseOnMain(disconnected_slot);
            });
            if (slot == 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ResetNativeGamepadRumbleOnMain();
                });
            }
            SDL_CloseGamepad(gamepad);
            s_gamepads[slot] = nullptr;
        }
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids)
        return;

    for (int id_index = 0; id_index < count; id_index++) {
        bool already_open = false;
        for (SDL_Gamepad* gamepad : s_gamepads) {
            if (gamepad && SDL_GetGamepadID(gamepad) == ids[id_index]) {
                already_open = true;
                break;
            }
        }
        if (already_open)
            continue;

        for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
            if (s_gamepads[slot])
                continue;

            s_gamepads[slot] = SDL_OpenGamepad(ids[id_index]);
            if (s_gamepads[slot]) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2RefreshNativeGamepadDpadHandlersOnMain("sdl-open");
                });
                const u32 pad_slot = ARMSX2PadSlotForGamepadIndex(slot);
                if (pad_slot == 0xffffffffu || pad_slot >= Pad::NUM_CONTROLLER_PORTS) {
                    Console.WriteLn("[Files] MFi gamepad %u connected but ignored by current multitap mode: %s",
                        slot + 1, SDL_GetGamepadName(s_gamepads[slot]));
                } else {
                    Console.WriteLn("[Files] MFi gamepad %u connected to PS2 pad slot %u: %s",
                        slot + 1, pad_slot + 1, SDL_GetGamepadName(s_gamepads[slot]));
                }
                if (!s_loggedMultitapRestartNeeded && s_vmThreadActive.load() &&
                    ARMSX2GetIOSMultitapMode() == ARMSX2IOSMultitapMode::Auto &&
                    ARMSX2ConnectedGamepadCount() > 2 && !EmuConfig.Pad.MultitapPort0_Enabled) {
                    Console.Warning("[ARMSX2 iOS Gamepad] 3+ controllers connected after boot; restart/reset with controllers connected to enable multitap.");
                    s_loggedMultitapRestartNeeded = true;
                }
            }
            break;
        }
    }

    SDL_free(ids);
}

static bool ARMSX2ShouldPreserveTouchState(u32 ps2_button, bool preserve_touch)
{
    return preserve_touch && ps2_button < (sizeof(g_touchPadState) / sizeof(g_touchPadState[0])) && g_touchPadState[ps2_button];
}

static void ARMSX2ApplyIOSGamepadInput(u32 gamepad_index, SDL_Gamepad* gamepad, PadBase* pad, bool preserve_touch)
{
    if (!gamepad || !pad)
        return;

    if (s_captureMode.load()) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
            if (SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(b))) {
                s_capturedButton.store(b);
                break;
            }
        }
    }

    static const u32 ps2Buttons[] = {
        PadDualshock2::Inputs::PAD_UP, PadDualshock2::Inputs::PAD_DOWN,
        PadDualshock2::Inputs::PAD_LEFT, PadDualshock2::Inputs::PAD_RIGHT,
        PadDualshock2::Inputs::PAD_CROSS, PadDualshock2::Inputs::PAD_CIRCLE,
        PadDualshock2::Inputs::PAD_SQUARE, PadDualshock2::Inputs::PAD_TRIANGLE,
        PadDualshock2::Inputs::PAD_L1, PadDualshock2::Inputs::PAD_R1,
        0, 0, // L2/R2 handled as analog
        PadDualshock2::Inputs::PAD_START, PadDualshock2::Inputs::PAD_SELECT,
        PadDualshock2::Inputs::PAD_L3, PadDualshock2::Inputs::PAD_R3,
    };

    for (int i = 0; i < 16; i++) {
        const int sdlBtn = s_buttonMap[i];
        if (sdlBtn < 0)
            continue;

        const u32 ps2Button = ps2Buttons[i];
        if (ps2Button == 0)
            continue;

        if (ARMSX2NativeDpadBitForPS2Button(ps2Button) != 0)
            continue;

        bool pressed = SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(sdlBtn));

        if (pressed)
            pad->Set(ps2Button, 1.0f);
        else if (!ARMSX2ShouldPreserveTouchState(ps2Button, preserve_touch))
            pad->Set(ps2Button, 0.0f);
    }

    struct DpadBinding
    {
        int map_index;
        u32 ps2_button;
        u8 native_bit;
    };
    static constexpr DpadBinding dpad_bindings[] = {
        {0, PadDualshock2::Inputs::PAD_UP, ARMSX2_NATIVE_DPAD_UP},
        {1, PadDualshock2::Inputs::PAD_DOWN, ARMSX2_NATIVE_DPAD_DOWN},
        {2, PadDualshock2::Inputs::PAD_LEFT, ARMSX2_NATIVE_DPAD_LEFT},
        {3, PadDualshock2::Inputs::PAD_RIGHT, ARMSX2_NATIVE_DPAD_RIGHT},
    };

    u8 native_dpad_mask = 0;
    u8 slot_latched_mask = 0;
    u8 any_latched_mask = 0;
    if (gamepad_index < ARMSX2_MAX_IOS_GAMEPADS) {
        slot_latched_mask = s_nativeGamepadDpadLatchedMask[gamepad_index].exchange(0, std::memory_order_relaxed);
        native_dpad_mask = s_nativeGamepadDpadMask[gamepad_index].load(std::memory_order_relaxed) | slot_latched_mask;
        if (gamepad_index == 0 && ARMSX2ConnectedGamepadCount() <= 1) {
            any_latched_mask = s_nativeGamepadAnyDpadLatchedMask.exchange(0, std::memory_order_relaxed);
            native_dpad_mask |= s_nativeGamepadAnyDpadMask.load(std::memory_order_relaxed) | any_latched_mask;
        }
        ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
    }

    for (const DpadBinding& binding : dpad_bindings) {
        bool pressed = false;
        const int sdlBtn = s_buttonMap[binding.map_index];
        if (sdlBtn >= 0)
            pressed = SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(sdlBtn));

        const bool native_pressed = ((native_dpad_mask & binding.native_bit) != 0);
        pressed = pressed || native_pressed;

        if (native_pressed) {
            const u32 log_index = s_loggedNativeGamepadDpadApplyEvents.fetch_add(1, std::memory_order_relaxed);
            if (log_index < 48) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad applied gamepad=%u ps2=0x%08x slot_mask=0x%02x slot_latched=0x%02x any_mask=0x%02x any_latched=0x%02x",
                    gamepad_index + 1, binding.ps2_button,
                    s_nativeGamepadDpadMask[gamepad_index].load(std::memory_order_relaxed),
                    slot_latched_mask,
                    s_nativeGamepadAnyDpadMask.load(std::memory_order_relaxed),
                    any_latched_mask);
            }
        }

        if (pressed)
            pad->Set(binding.ps2_button, 1.0f);
        else if (!ARMSX2ShouldPreserveTouchState(binding.ps2_button, preserve_touch))
            pad->Set(binding.ps2_button, 0.0f);
    }

    const float l2 = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    const float r2 = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    if (l2 > 0.1f || !ARMSX2ShouldPreserveTouchState(PadDualshock2::Inputs::PAD_L2, preserve_touch))
        pad->Set(PadDualshock2::Inputs::PAD_L2, l2 > 0.1f ? l2 : 0.0f);
    if (r2 > 0.1f || !ARMSX2ShouldPreserveTouchState(PadDualshock2::Inputs::PAD_R2, preserve_touch))
        pad->Set(PadDualshock2::Inputs::PAD_R2, r2 > 0.1f ? r2 : 0.0f);

    auto axis = [&](SDL_GamepadAxis a) -> float {
        const float v = SDL_GetGamepadAxis(gamepad, a) / 32767.0f;
        return (v > 0.15f || v < -0.15f) ? v : 0.0f;
    };
    const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    const float ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    const float ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    auto set_axis = [&](u32 input, float value) {
        if (value > 0.0f || !ARMSX2ShouldPreserveTouchState(input, preserve_touch))
            pad->Set(input, value);
    };
    set_axis(PadDualshock2::Inputs::PAD_L_RIGHT, lx > 0 ? lx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_L_LEFT,  lx < 0 ? -lx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_L_DOWN,  ly > 0 ? ly : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_L_UP,    ly < 0 ? -ly : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_RIGHT, rx > 0 ? rx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_LEFT,  rx < 0 ? -rx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_DOWN,  ry > 0 ? ry : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_UP,    ry < 0 ? -ry : 0.0f);
}

// View controller references for background color switching
static UIViewController* __unsafe_unretained s_menuVC = nil;
static UIViewController* __unsafe_unretained s_rootVC = nil;

// Helper to log to screen (thread safe)
void LogToScreen(const char* str) {
    if (!str) return;
    NSString *msg = [NSString stringWithUTF8String:str];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_logView) {
            g_logView.text = [g_logView.text stringByAppendingString:msg];
            if (g_logView.text.length > 20000) {
                 g_logView.text = [g_logView.text substringFromIndex:g_logView.text.length - 20000];
            }
            [g_logView scrollRangeToVisible:NSMakeRange(g_logView.text.length, 0)];
        }
    });
}

// ... Host Stubs (Keep existing ones) ...

// We need to forward declare the Host stubs here or ensure they are present.
// For brevity, I will include the critical parts and stubs.

// -- Host Implementation Start --

namespace Host
{
    SDL_Window* g_sdl_window = nullptr;

    void RequestShutdown() {
        SDL_Event event;
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
    
    // ... [Include all other Host stubs from previous ios_main.mm] ...
    // Note: I will paste the stubs in the actual write call to ensure compilation.
    

    
    void RunOnMainThread(std::function<void()> func, bool wait) {
        if (wait) {
            dispatch_sync(dispatch_get_main_queue(), ^{ func(); });
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{ func(); });
        }
    }

    // Only needed stubs for linking
    bool CopyTextToClipboard(const std::string_view text)
    {
        NSString* nsText = [[NSString alloc] initWithBytes:text.data()
                                                    length:text.size()
                                                  encoding:NSUTF8StringEncoding];
        if (!nsText)
            return false;

        void (^copyBlock)(void) = ^{
            UIPasteboard.generalPasteboard.string = nsText;
        };

        if ([NSThread isMainThread])
            copyBlock();
        else
            dispatch_sync(dispatch_get_main_queue(), copyBlock);

        return true;
    }

    std::string GetTextFromClipboard()
    {
        __block NSString* nsText = nil;
        void (^pasteBlock)(void) = ^{
            nsText = UIPasteboard.generalPasteboard.string;
        };

        if ([NSThread isMainThread])
            pasteBlock();
        else
            dispatch_sync(dispatch_get_main_queue(), pasteBlock);

        const char* utf8 = nsText ? [nsText UTF8String] : nullptr;
        return utf8 ? std::string(utf8) : std::string();
    }

    void OnOSDMessage(const std::string&, float, u32) {}
    void ReportError(const char*, const char*) {}
    bool ConfirmAction(const char*, const char*, const char*) { return true; }
    std::optional<std::string> OpenFileSelectionDialog(const char*, const char*, const char*, const char*) { return std::nullopt; }
    std::optional<std::string> OpenDirectorySelectionDialog(const char*, const char*) { return std::nullopt; }
    void SysLog(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    void LoadSettings(SettingsInterface&, std::unique_lock<std::mutex>&) {} 
    void RequestResetSettings(bool) {} 
    const char* GetTranslatedStringImpl(const char* key) { return key; }
    u32 GetDisplayRefreshRate() { return 60; }
    std::optional<WindowInfo> AcquireRenderWindow(bool recreate_window) {
        Console.WriteLn("Host::AcquireRenderWindow(recreate=%d) called.", recreate_window);
        if (!g_sdl_window) {
            Console.Error("Host::AcquireRenderWindow: g_sdl_window is NULL");
            return std::nullopt;
        }
        
        __block WindowInfo wi = {};
        wi.type = WindowInfo::Type::MacOS;
        
        // SDL calls that interact with UIKit must run on the main thread
        dispatch_sync(dispatch_get_main_queue(), ^{
            // SDL3 properties for UIKit
            SDL_PropertiesID props = SDL_GetWindowProperties(g_sdl_window);
            UIWindow* window = (__bridge UIWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
            
            if (window) {
                // Use dedicated game render view if available (sized for portrait).
                if (g_gameRenderView) {
                    wi.window_handle = (__bridge void*)g_gameRenderView;
                } else {
                    wi.window_handle = (__bridge void*)[window rootViewController].view;
                }
            }

            if (!wi.window_handle) {
                 Console.Error("Host::AcquireRenderWindow: Failed to get UIKit View (UIWindow=%p)", window);
                 // Last resort: some older SDL versions might put the view in the window property or vice versa
                 if (!wi.window_handle) wi.window_handle = (__bridge void*)window;
            }

            // Get render size from the actual render view.
            UIView* renderView = (__bridge UIView*)wi.window_handle;
            CGFloat scale = renderView.contentScaleFactor > 0.0 ? renderView.contentScaleFactor : UIScreen.mainScreen.nativeScale;
            if (scale <= 0.0)
                scale = 1.0;
            wi.surface_width = static_cast<u32>(std::max<CGFloat>(1.0, renderView.bounds.size.width * scale));
            wi.surface_height = static_cast<u32>(std::max<CGFloat>(1.0, renderView.bounds.size.height * scale));
            wi.surface_scale = static_cast<float>(scale);
            
            SDL_DisplayID display = SDL_GetDisplayForWindow(g_sdl_window);
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
            if (mode)
                wi.surface_refresh_rate = mode->refresh_rate;
            else
                wi.surface_refresh_rate = 60.0f;
        });

        Console.WriteLn("Host::AcquireRenderWindow: Returning WindowInfo (Type=%d, View=%p, Size=%ux%u, Scale=%.2f)",
            (int)wi.type, wi.window_handle, wi.surface_width, wi.surface_height, wi.surface_scale);

        return wi;
    }
    void ReleaseRenderWindow() {}
    bool InNoGUIMode() { return false; }
    void OnVMPaused() {}
    void OnVMResumed() {}
    void OnVMStarted() {}
    void OnVMStarting() {}
    void EndTextInput() {}
    bool IsFullscreen() { return true; }
    void SetMouseMode(bool, bool) {}
    void OnGameChanged(const std::string&, const std::string&, const std::string&, const std::string&, unsigned int, unsigned int)
    {
        ARMSX2_PostRuntimeMenuStateChanged();
    }
    void OnVMDestroyed() {}
    void SetFullscreen(bool) {}
    void BeginTextInput() {}
    bool ConfirmMessage(std::string_view, std::string_view) { return true; }
    void RunOnCPUThread(std::function<void()> function, bool block)
    {
        if (!function)
            return;

        if (std::this_thread::get_id() == s_cpuThreadId) {
            function();
            return;
        }

        if (!s_vmThreadActive.load()) {
            Console.Warning("[ARMSX2 iOS] CPU-thread task requested while VM is inactive; running inline");
            function();
            return;
        }

        auto task = std::make_shared<CPUThreadTask>();
        task->id = s_cpuTaskNextId.fetch_add(1, std::memory_order_relaxed);
        task->function = std::move(function);

        {
            std::lock_guard<std::mutex> lock(s_cpuTaskMutex);
            s_cpuTasks.push_back(task);
        }

        if (!block)
            return;

        std::unique_lock<std::mutex> lock(task->mutex);
        std::fprintf(stderr, "@@CPU_TASK_WAIT@@ id=%llu state=%d\n",
            task->id, static_cast<int>(VMManager::GetState()));
        std::fflush(stderr);
        if (!task->cv.wait_for(lock, std::chrono::seconds(1), [&task] { return task->complete; })) {
            std::fprintf(stderr, "@@CPU_TASK_TIMEOUT@@ id=%llu state=%d queued=1\n",
                task->id, static_cast<int>(VMManager::GetState()));
            std::fflush(stderr);
            return;
        }
        std::fprintf(stderr, "@@CPU_TASK_WAIT_OK@@ id=%llu\n", task->id);
        std::fflush(stderr);
    }
    void ReportInfoAsync(std::string_view, std::string_view) {}
    void ReportErrorAsync(std::string_view title, std::string_view msg) {
        Console.Error("Host::ReportErrorAsync: %s - %s", std::string(title).c_str(), std::string(msg).c_str());
    }
    void OnSaveStateSaved(std::string_view) { ARMSX2_PostRuntimeMenuStateChanged(); }
    void OnSaveStateLoaded(std::string_view, bool) { ARMSX2_PostRuntimeMenuStateChanged(); }
    void BeginPresentFrame() {}
    void OnSaveStateLoading(std::string_view) {}
    bool LocaleCircleConfirm() { return false; }

    void RefreshGameListAsync(bool) {}
    bool RequestResetSettings(bool, bool, bool, bool, bool) { return true; }
    void CancelGameListRefresh() {}
    void RequestVMShutdown(bool, bool, bool) {}
    void RequestExitBigPicture() {}
    void OnInputDeviceConnected(std::string_view, std::string_view) {}
    void RequestExitApplication(bool) {}
    void CheckForSettingsChanges(const Pcsx2Config&) {}
    void OnAchievementsRefreshed() { ARMSX2_PostRetroAchievementsStateChanged(); }
    void PumpMessagesOnCPUThread()
    {
        ARMSX2DrainCPUThreadTasks();

// Check for VM shutdown request (safe: runs on CPU thread)
        if (s_requestVMStop.load()) {
            Console.WriteLn("[UI] PumpMessages: setting VM state to Stopping");
            VMManager::SetState(VMState::Stopping);
            return;
        }

        PadBase* pad = Pad::GetPad(0, 0);
        if (!pad) return;

#if TARGET_OS_SIMULATOR
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!keys) return;

        static const struct { SDL_Scancode sc; u32 idx; } mapping[] = {
            { SDL_SCANCODE_UP,     PadDualshock2::Inputs::PAD_UP },
            { SDL_SCANCODE_DOWN,   PadDualshock2::Inputs::PAD_DOWN },
            { SDL_SCANCODE_LEFT,   PadDualshock2::Inputs::PAD_LEFT },
            { SDL_SCANCODE_RIGHT,  PadDualshock2::Inputs::PAD_RIGHT },
            { SDL_SCANCODE_Z,      PadDualshock2::Inputs::PAD_CIRCLE },
            { SDL_SCANCODE_X,      PadDualshock2::Inputs::PAD_CROSS },
            { SDL_SCANCODE_A,      PadDualshock2::Inputs::PAD_SQUARE },
            { SDL_SCANCODE_S,      PadDualshock2::Inputs::PAD_TRIANGLE },
            { SDL_SCANCODE_Q,      PadDualshock2::Inputs::PAD_L1 },
            { SDL_SCANCODE_W,      PadDualshock2::Inputs::PAD_R1 },
            { SDL_SCANCODE_1,      PadDualshock2::Inputs::PAD_L2 },
            { SDL_SCANCODE_2,      PadDualshock2::Inputs::PAD_R2 },
            { SDL_SCANCODE_RETURN, PadDualshock2::Inputs::PAD_START },
            { SDL_SCANCODE_SPACE,  PadDualshock2::Inputs::PAD_SELECT },
        };

        // Merge keyboard + touch input: only override with keyboard if
        // touch is not currently pressing the same button.
        for (const auto& m : mapping) {
            if (keys[m.sc])
                pad->Set(m.idx, 1.0f);
            else if (!g_touchPadState[m.idx])
                pad->Set(m.idx, 0.0f);
            // If touch is holding the button (g_touchPadState), don't reset it
        }
#endif // TARGET_OS_SIMULATOR — keyboard mapping

        // MFi / External gamepad support via SDL3
        {
            ARMSX2RefreshIOSGamepads();
            for (u32 gamepad_index = 0; gamepad_index < ARMSX2_MAX_IOS_GAMEPADS; gamepad_index++) {
                SDL_Gamepad* gamepad = s_gamepads[gamepad_index];
                if (!gamepad)
                    continue;

                const u32 pad_slot = ARMSX2PadSlotForGamepadIndex(gamepad_index);
                if (pad_slot == 0xffffffffu || pad_slot >= Pad::NUM_CONTROLLER_PORTS)
                    continue;

                PadBase* gamepad_pad = Pad::GetPad(static_cast<u8>(pad_slot));
                if (!gamepad_pad)
                    continue;

                ARMSX2ApplyPendingGamepadRumble(gamepad_index);
                ARMSX2ApplyIOSGamepadInput(gamepad_index, gamepad, gamepad_pad, gamepad_index == 0);
            }
        }

        // [BIOS_NAV] Auto-navigate BIOS — debug only
#if DEBUG
        if (const char* nav = getenv("ARMSX2_BIOS_NAV"); nav && atoi(nav))
        {
            unsigned int fc = ::g_FrameCount;
            auto press = [&](u32 btn, unsigned int at) {
                if (fc >= at && fc <= at + 1) pad->Set(btn, 1.0f);
                else if (fc == at + 2) pad->Set(btn, 0.0f);
            };
            // BIOS nav: ↓ → ○ → ○ → ○ → ← → ○ → ○...
            // The exact screen order varies. Try multiple ←+○ combos.
            press(PadDualshock2::Inputs::PAD_DOWN, 600);
            press(PadDualshock2::Inputs::PAD_CIRCLE, 750);
            // After entering System Configuration, each screen needs ○ to advance.
            // The "initialization" dialog needs ← first to select "Yes".
            // Try ← before each ○ to handle wherever the dialog appears.
            unsigned int seq[] = {
                950,  0,  // ○ language
                1150, 0,  // ○ clock
                1350, 1,  // ← then ○ (init dialog attempt 1)
                1550, 1,  // ← then ○ (init dialog attempt 2)
                1750, 0,  // ○
                1950, 0,  // ○
                2150, 1,  // ← then ○ (attempt 3)
                2350, 0,  // ○
                2550, 0, 2750, 0, 2950, 0, 3150, 0, 3350, 0, 3550, 0,
            };
            for (int i = 0; i < (int)(sizeof(seq)/sizeof(seq[0])); i += 2) {
                unsigned int t = seq[i];
                if (seq[i+1]) // needs LEFT first
                    press(PadDualshock2::Inputs::PAD_LEFT, t);
                press(PadDualshock2::Inputs::PAD_CIRCLE, t + (seq[i+1] ? 100 : 0));
            }

            // Log after each step
            static const unsigned int cps[] = {650, 770, 950, 1130, 1300, 1500, 1800, 2100, 2400, 2700, 3000};
            for (auto cp : cps) {
                if (fc == cp) {
                    Console.WriteLn(Color_Yellow, "[BIOS_NAV] checkpoint f=%u", fc);
                }
            }
        }
#endif // DEBUG — BIOS_NAV
    }
    std::string TranslatePluralToString(const char*, const char*, const char*, int) { return ""; }
    void CommitBaseSettingChanges()
    {
        if (s_settings_interface)
            s_settings_interface->Save();
        if (s_secrets_settings_interface)
            s_secrets_settings_interface->Save();
    }
    void OnInputDeviceDisconnected(InputBindingKey, std::string_view) {}
    void OpenHostFileSelectorAsync(std::string_view, bool, std::function<void(const std::string&)>, std::vector<std::string>, std::string_view) {}
    std::unique_ptr<ProgressCallback> CreateHostProgressCallback() { return nullptr; }
    void OnAchievementsLoginSuccess(char const*, u32, u32, u32) { ARMSX2_PostRetroAchievementsStateChanged(); }
    void OnPerformanceMetricsUpdated()
    {
        if (!ARMSX2IOSRuntimeTelemetryEnabled())
            return;

        static std::atomic<uint> s_last_metrics_frame{0};
        const uint frame = ::g_FrameCount;
        const float fps = PerformanceMetrics::GetFPS();
        const float internal_fps = PerformanceMetrics::GetInternalFPS();
        const float speed = PerformanceMetrics::GetSpeed();
        const float cpu_usage = PerformanceMetrics::GetCPUThreadUsage();
        const float vu_usage = PerformanceMetrics::GetVUThreadUsage();
        const float gs_usage = PerformanceMetrics::GetGSThreadUsage();
        const float gpu_usage = PerformanceMetrics::GetGPUUsage();
        const bool hot_sample =
            (frame > 300 && fps < 58.0f) ||
            cpu_usage >= 85.0f ||
            vu_usage >= 35.0f ||
            gs_usage >= 25.0f ||
            gpu_usage >= 25.0f;
        const uint min_frame_delta = hot_sample ? 60 : 300;
        uint last = s_last_metrics_frame.load(std::memory_order_relaxed);
        if (last != 0 && frame < last + min_frame_delta)
            return;
        if (!s_last_metrics_frame.compare_exchange_strong(last, frame, std::memory_order_relaxed))
            return;

        std::fprintf(stderr,
            "@@PERF@@ frame=%u pm_frame=%llu fps=%.2f internal_fps=%.2f speed=%.2f cpu=%.2f vu=%.2f gs=%.2f gpu=%.2f state=%d\n",
            frame,
            static_cast<unsigned long long>(PerformanceMetrics::GetFrameNumber()),
            fps,
            internal_fps,
            speed,
            cpu_usage,
            vu_usage,
            gs_usage,
            gpu_usage,
            static_cast<int>(VMManager::GetState()));
    }
    void OnAchievementsLoginRequested(Achievements::LoginRequestReason) { ARMSX2_PostRetroAchievementsStateChanged(); }
    bool ShouldPreferHostFileSelector() { return false; }
    void OnCoverDownloaderOpenRequested() {}
    void OnCreateMemoryCardOpenRequested() {}
    void OnAchievementsHardcoreModeChanged(bool) { ARMSX2_PostRetroAchievementsStateChanged(); }
    void SetMouseLock(bool) {}
    int LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs) { return lhs.compare(rhs); }
    void OpenURL(std::string_view) {}
}

namespace Host::Internal
{
    s32 GetTranslatedStringImpl(const std::string_view, const std::string_view, char*, size_t) { return 0; }
}

// Called from ARMSX2Bridge to toggle SDL fullscreen (controls status bar visibility)
extern "C" void ARMSX2_SetSDLFullscreen(bool enabled) {
    if (Host::g_sdl_window)
        SDL_SetWindowFullscreen(Host::g_sdl_window, enabled);
}

extern "C" bool ARMSX2_IsSDLFullscreen() {
    if (!Host::g_sdl_window)
        return false;
    return (SDL_GetWindowFlags(Host::g_sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

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

namespace Common {
    bool PlaySoundAsync(const char*) { return false; }
}

// IOCtlSrc Stubs
IOCtlSrc::IOCtlSrc(std::string filename) : m_filename(std::move(filename)) {}
IOCtlSrc::~IOCtlSrc() {}
bool IOCtlSrc::Reopen(Error*) { return false; }
u32 IOCtlSrc::GetSectorCount() const { return 0; }
const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const { static std::vector<toc_entry> empty; return empty; }
bool IOCtlSrc::ReadSectors2048(u32, u32, u8*) const { return false; }
bool IOCtlSrc::ReadSectors2352(u32, u32, u8*) const { return false; }
bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ*) const { return false; }
u32 IOCtlSrc::GetLayerBreakAddress() const { return 0; }
s32 IOCtlSrc::GetMediaType() const { return 0; }
void IOCtlSrc::SetSpindleSpeed(bool) const {}
bool IOCtlSrc::DiscReady() { return false; }

// ... InputManager Stubs ...
namespace InputManager {
    void Initialize() {}
    void Shutdown() {}
    void Update() {}
    void SetRumble(int, u8, u8) {}
    const char* ConvertHostKeyboardCodeToIcon(unsigned int) { return ""; }
    std::optional<std::string> ConvertHostKeyboardCodeToString(unsigned int) { return std::nullopt; }
    std::optional<unsigned int> ConvertHostKeyboardStringToCode(std::string_view) { return std::nullopt; }
}

// ... HTTP ...
class IOSHTTPDownloader final : public HTTPDownloader
{
public:
    explicit IOSHTTPDownloader(std::string user_agent)
        : m_user_agent(std::move(user_agent))
    {
        @autoreleasepool {
            NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            configuration.URLCache = nil;
            configuration.HTTPShouldSetCookies = NO;
            NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];
#if __has_feature(objc_arc)
            m_session = (__bridge_retained void*)session;
#else
            m_session = (void*)[session retain];
#endif
            NSLog(@"[ARMSX2 iOS HTTP] NSURLSession created: %@", session);
        }
    }

    ~IOSHTTPDownloader() override
    {
        if (m_session)
        {
#if __has_feature(objc_arc)
            NSURLSession* session = (__bridge_transfer NSURLSession*)m_session;
#else
            NSURLSession* session = (NSURLSession*)m_session;
#endif
            m_session = nullptr;
            [session invalidateAndCancel];
#if !__has_feature(objc_arc)
            [session release];
#endif
        }
    }

protected:
    struct IOSRequest final : Request
    {
        NSURLSessionDataTask* task = nil;
        std::mutex completion_mutex;
        bool completion_ready = false;
        s32 completed_status_code = HTTP_STATUS_ERROR;
        u32 completed_content_length = 0;
        std::string completed_content_type;
        Request::Data completed_data;

#if !__has_feature(objc_arc)
        ~IOSRequest()
        {
            [task release];
        }
#endif
    };

    Request* InternalCreateRequest() override
    {
        return new IOSRequest();
    }

    void InternalPollRequests() override
    {
        for (Request* request : m_pending_http_requests)
        {
            IOSRequest* native_request = static_cast<IOSRequest*>(request);
            std::lock_guard<std::mutex> completion_lock(native_request->completion_mutex);
            if (!native_request->completion_ready)
                continue;

            native_request->status_code = native_request->completed_status_code;
            native_request->content_length = native_request->completed_content_length;
            native_request->content_type = std::move(native_request->completed_content_type);
            native_request->data = std::move(native_request->completed_data);
            native_request->completion_ready = false;
            native_request->state.store(Request::State::Complete, std::memory_order_release);
        }
    }

    bool StartRequest(Request* request) override
    {
        IOSRequest* native_request = static_cast<IOSRequest*>(request);

        @autoreleasepool {
            NSString* url_string = [NSString stringWithUTF8String:request->url.c_str()];
            NSURL* url = url_string ? [NSURL URLWithString:url_string] : nil;
            if (!url)
            {
                request->status_code = HTTP_STATUS_ERROR;
                request->state.store(Request::State::Complete);
                NSLog(@"[ARMSX2 iOS HTTP] Invalid URL: %@", url_string ?: @"<nil>");
                return true;
            }

            NSMutableURLRequest* url_request = [NSMutableURLRequest requestWithURL:url];
            url_request.timeoutInterval = m_timeout;
            url_request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;

            NSString* user_agent = [NSString stringWithUTF8String:m_user_agent.c_str()];
            if (user_agent.length > 0)
                [url_request setValue:user_agent forHTTPHeaderField:@"User-Agent"];

            if (request->type == Request::Type::Post)
            {
                url_request.HTTPMethod = @"POST";
                url_request.HTTPBody = [NSData dataWithBytes:request->post_data.data() length:request->post_data.size()];
                [url_request setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
            }
            else
            {
                url_request.HTTPMethod = @"GET";
            }

            NSString* debug_url = url.absoluteString ?: @"";
#if __has_feature(objc_arc)
            NSURLSession* session = (__bridge NSURLSession*)m_session;
#else
            NSURLSession* session = (NSURLSession*)m_session;
#endif
            if (!session || ![session respondsToSelector:@selector(dataTaskWithRequest:completionHandler:)])
            {
                request->status_code = HTTP_STATUS_ERROR;
                request->state.store(Request::State::Complete);
                NSLog(@"[ARMSX2 iOS HTTP] Invalid NSURLSession while starting %@", debug_url);
                return true;
            }
            request->state.store(Request::State::Started);

            NSURLSessionDataTask* task = [session dataTaskWithRequest:url_request completionHandler:
                ^(NSData* data, NSURLResponse* response, NSError* error) {
                    s32 status_code = HTTP_STATUS_ERROR;
                    u32 content_length = 0;
                    std::string content_type;
                    Request::Data response_data;

                    if (error)
                    {
                        if (error.code == NSURLErrorCancelled)
                            status_code = HTTP_STATUS_CANCELLED;
                        else if (error.code == NSURLErrorTimedOut)
                            status_code = HTTP_STATUS_TIMEOUT;

                        NSLog(@"[ARMSX2 iOS HTTP] %@ failed: %@", debug_url, error.localizedDescription);
                    }
                    else
                    {
                        NSHTTPURLResponse* http_response = [response isKindOfClass:[NSHTTPURLResponse class]] ?
                            (NSHTTPURLResponse*)response : nil;
                        status_code = http_response ? static_cast<s32>(http_response.statusCode) : HTTP_STATUS_ERROR;

                        NSString* mime_type = response.MIMEType;
                        if (mime_type.length > 0)
                            content_type = mime_type.UTF8String;

                        const long long expected_length = response.expectedContentLength;
                        if (expected_length > 0)
                            content_length = static_cast<u32>(std::min<long long>(expected_length, UINT32_MAX));

                        if (data.length > 0)
                        {
                            response_data.resize(data.length);
                            std::memcpy(response_data.data(), data.bytes, data.length);
                        }

                        NSLog(@"[ARMSX2 iOS HTTP] %@ -> %d (%lu bytes)", debug_url, status_code,
                            static_cast<unsigned long>(data.length));
                    }

                    {
                        std::lock_guard<std::mutex> completion_lock(native_request->completion_mutex);
                        native_request->completed_status_code = status_code;
                        native_request->completed_content_length = content_length;
                        native_request->completed_content_type = std::move(content_type);
                        native_request->completed_data = std::move(response_data);
                        native_request->completion_ready = true;
                    }
                }];

#if __has_feature(objc_arc)
            native_request->task = task;
#else
            native_request->task = [task retain];
#endif
            [native_request->task resume];
        }

        return true;
    }

    void CloseRequest(Request* request) override
    {
        IOSRequest* native_request = static_cast<IOSRequest*>(request);
        const Request::State state = native_request->state.load();

        if (state == Request::State::Complete)
        {
            delete native_request;
            return;
        }

        // NSURLSession can still deliver a completion after cancellation. Keep the tiny
        // request object alive in that rare timeout/cancel path to avoid a use-after-free.
        [native_request->task cancel];
#if !__has_feature(objc_arc)
        [native_request->task release];
#endif
        native_request->task = nil;
    }

private:
    std::string m_user_agent;
    void* m_session = nullptr;
};

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
    return std::make_unique<IOSHTTPDownloader>(std::move(user_agent));
}

// Global stubs for DISCopen
void GetValidDrive(std::string&) {  }
std::vector<std::string> GetOpticalDriveList() { return {}; }

namespace FileSystem {
    int OpenFDFileContent(const char*) { return -1; } // Added overload
    bool OpenFDFileContent(const std::string&, int, s64, s64) { return false; }
    std::string GetValidDrive(const std::string&) { return ""; }
    std::vector<std::string> GetOpticalDriveList() { return {}; }
}


// ... CocoaTools Stub ...
namespace CocoaTools {
    void InhibitAppNap(const std::string&) {}
    void UninhibitAppNap() {}
    std::string GetBundlePath() { return [[NSBundle mainBundle].bundlePath UTF8String]; }
    
    void* CreateMetalLayer(WindowInfo* wi) {
        if (!Host::g_sdl_window) return nullptr;
        
        // Return existing layer if we already have it
        if (wi->surface_handle) {
            return SDL_Metal_GetLayer((SDL_MetalView)wi->surface_handle);
        }
        
        // Create the Metal view
        SDL_MetalView view = SDL_Metal_CreateView(Host::g_sdl_window);
        if (!view) {
            Console.Error("SDL_Metal_CreateView failed: %s", SDL_GetError());
            return nullptr;
        }
        
        void* layer = SDL_Metal_GetLayer(view);
        wi->surface_handle = view; // Store view handle to destroy later
        Console.WriteLn("Created Metal Layer: %p from View: %p", layer, view);
        return layer;
    }
    
    void DestroyMetalLayer(WindowInfo* wi) {
        if (wi->surface_handle) {
            Console.WriteLn("Destroying Metal View: %p", wi->surface_handle);
            SDL_Metal_DestroyView((SDL_MetalView)wi->surface_handle);
            wi->surface_handle = nullptr;
        }
    }
}

// ... AudioStream Stub ...
#include "pcsx2/Host/AudioStream.h"
// ... PCAP Stub ...
PCAPAdapter::PCAPAdapter() {}
PCAPAdapter::~PCAPAdapter() {}
bool PCAPAdapter::blocks() { return false; }
bool PCAPAdapter::isInitialised() { return false; }
bool PCAPAdapter::recv(NetPacket*) { return false; }
bool PCAPAdapter::send(NetPacket*) { return false; }
void PCAPAdapter::reloadSettings() {}
std::vector<AdapterEntry> PCAPAdapter::GetAdapters() { return {}; }
AdapterOptions PCAPAdapter::GetAdapterOptions() { return {}; }
bool PCAPAdapter::InitPCAP(const std::string&, bool) { return false; }
bool PCAPAdapter::SetMACSwitchedFilter(PacketReader::MAC_Address) { return false; }
void PCAPAdapter::SetMACBridgedRecv(NetPacket*) {}
void PCAPAdapter::SetMACBridgedSend(NetPacket*) {}
void PCAPAdapter::HandleFrameCheckSequence(NetPacket*) {}
bool PCAPAdapter::ValidateEtherFrame(NetPacket*) { return false; }


// ... FileSystem Stub ...

// -- End Host Stubs --

// Settings Interface
#include "3rdparty/simpleini/include/SimpleIni.h"
#include "pcsx2/INISettingsInterface.h"

// Expose to ARMSX2Bridge.mm via extern
INISettingsInterface* g_p44_settings_interface = nullptr;

//
// -- IOS AppDelegate & SceneDelegate --
//

@interface PCSX2SceneDelegate : UIResponder <UIWindowSceneDelegate, UIDocumentPickerDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong, nonatomic) UIButton *startBiosButton;
@end

@implementation PCSX2SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    
    // --- SDL Initialization ---
    static bool s_initialized = false;
    if (!s_initialized) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) < 0) {
            NSLog(@"SDL_Init failed: %s", SDL_GetError());
            return;
        }
        s_initialized = true;
    }
    ARMSX2InstallNativeGamepadDpadObserversOnMain();
    
    // --- Setup PCSX2 Environment ---
    // (Moved to AppDelegate)
    // We still need local variables if used below, but EmuFolders are global.
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    std::string dataRoot = [documentsDirectory UTF8String];
    
    // Re-ensure EmuFolders (idempotent)
    EmuFolders::DataRoot = dataRoot;
    EmuFolders::Bios = dataRoot + "/bios";
    // ...
    
    Console.WriteLn("PCSX2 iOS: Initializing logic in SceneDelegate...");
    
    // Settings Initialization
    if (!s_settings_interface) {
        std::string iniPath = dataRoot + "/PCSX2-iOS.ini";
        s_settings_interface = new INISettingsInterface(iniPath);
        if (!static_cast<INISettingsInterface*>(s_settings_interface)->Load()) {
            Console.WriteLn("Creating new config at %s", iniPath.c_str());
            
            // [iPSX2] Standard Defaults: JIT Enabled (if supported), EE/IOP/VU Recompilers ON
            s_settings_interface->SetIntValue("EmuCore/CPU", "CoreType", 2); // ARM64 JIT in the Swift UI model
            s_settings_interface->SetBoolValue("EmuCore/CPU", "UseArm64Dynarec", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU", "EnableSparseMemory", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU", "ExtraMemory", false);

            // Audio
            s_settings_interface->SetStringValue("SPU2/Output", "Backend", "SDL");

            // GS
            s_settings_interface->SetIntValue("EmuCore/GS", "VsyncQueueSize", 8);

            // Normal console-speed frame limiter. Do not save reduced nominal
            // speed here; that is not a safe FPS cap for iOS.
            s_settings_interface->SetFloatValue("Framerate", "NominalScalar", 1.0f);
            s_settings_interface->SetBoolValue("GameISO", "FastBoot", false);
            s_settings_interface->SetBoolValue("EmuCore", "EnableFastBoot", false);

            // Speedhacks
            s_settings_interface->SetBoolValue("EmuCore/Speedhacks", "vuThread", ARMSX2ShouldEnableMTVUByDefault(nullptr));

            // RetroAchievements
            s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
            s_settings_interface->SetBoolValue("Achievements", "ChallengeMode", false);

            // iOS controller defaults.
            s_settings_interface->SetIntValue("ARMSX2iOS/Gamepad", "MultitapMode", 0);
            
            Console.WriteLn("@@CFG_DEFAULTS@@ created=1 CoreType=2 UseArm64Dynarec=true EnableEE=1 FastBoot=1");
            s_settings_interface->Save();
        }
        Host::Internal::SetBaseSettingsLayer(s_settings_interface);
        std::string secretsPath = dataRoot + "/PCSX2-iOS-secrets.ini";
        s_secrets_settings_interface = new INISettingsInterface(secretsPath);
        s_secrets_settings_interface->Load();
        Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface);
        g_p44_settings_interface = s_settings_interface; // expose to Bridge
	// Load gamepad button mapping from INI
        for (int i = 0; i < 16; i++) {
            char key[32]; snprintf(key, sizeof(key), "Button%d", i);
            int val = s_settings_interface->GetIntValue("ARMSX2iOS/GamepadMapping", key, s_defaultMap[i]);
            s_buttonMap[i] = val;
        }
    }
    ARMSX2RepairIOSARM64JITSettings(s_settings_interface, "scene-connect");
    ARMSX2MigrateJITScriptProtocolForIOS(s_settings_interface, "scene-connect");
    // One-time migration for existing INI (runs once, then conditions are false)
    if (!s_settings_interface->ContainsValue("SPU2/Output", "Backend")) {
        s_settings_interface->SetStringValue("SPU2/Output", "Backend", "SDL");
    }
    if (!s_settings_interface->ContainsValue("EmuCore/CPU", "ExtraMemory")) {
        s_settings_interface->SetBoolValue("EmuCore/CPU", "ExtraMemory", false);
    }
    if (!s_settings_interface->ContainsValue("EmuCore/CPU/Recompiler", "EnableFastmem")) {
        s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", true);
    }
    if (!s_settings_interface->ContainsValue("Achievements", "Enabled")) {
        s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
    }
    if (!s_settings_interface->ContainsValue("Achievements", "ChallengeMode")) {
        s_settings_interface->SetBoolValue("Achievements", "ChallengeMode", false);
    }
    if (!ARMSX2IOSRetroAchievementsHardcoreAvailable) {
        ARMSX2DisableRetroAchievementsHardcoreForIOS(s_settings_interface, "scene-connect");
    }
    if (!s_settings_interface->ContainsValue("ARMSX2iOS/Gamepad", "MultitapMode")) {
        s_settings_interface->SetIntValue("ARMSX2iOS/Gamepad", "MultitapMode", 0);
    }
    ARMSX2EnsureIOSSpeedhackDefaults(s_settings_interface, "scene-connect");
    ARMSX2SanitizeFrameLimiterConfig("scene-connect");
    ARMSX2ApplyIOSMultitapConfig("scene-connect");
    s_settings_interface->Save();
    [self checkAndConfigureBIOS];

    // GS Renderer: Metal fixed on iOS. Only override if not already Metal.
#if DEBUG
    if (const char* null_gs_env = getenv("ARMSX2_NULL_GS"); null_gs_env && atoi(null_gs_env)) {
        EmuConfig.GS.Renderer = GSRendererType::Null;
        Console.WriteLn("@@CFG@@ GS Renderer: Null (DEBUG)");
    } else
#endif
    {
        EmuConfig.GS.Renderer = GSRendererType::Metal;
    }
    s_settings_interface->Save();

    VMManager::Internal::LoadStartupSettings();
    if (!ARMSX2IOSRetroAchievementsHardcoreAvailable) {
        EmuConfig.Achievements.HardcoreMode = false;
    }
    ARMSX2SanitizeFrameLimiterConfig("after-startup-settings");
    ARMSX2ApplyIOSOsdPresetFromConfig("after-startup-settings");
    VMManager::ApplySettings();
    if (EmuConfig.Achievements.Enabled)
        Console.WriteLn("@@RA_INIT_DEFER@@ reason=scene-connect enabled=1 active=%d", Achievements::IsActive() ? 1 : 0);
    ARMSX2_PostRetroAchievementsStateChanged();
    
    // --- Create SDL Window ---
    Host::g_sdl_window = SDL_CreateWindow("PCSX2 iOS", 1280, 720, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE);
    if (!Host::g_sdl_window) {
        Console.Error("Failed to create SDL window: %s", SDL_GetError());
        return;
    }
    
    // --- Attach UIWindow ---
    UIWindow *uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(Host::g_sdl_window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (uiWindow) {
        Console.WriteLn("Attaching UIWindow to Scene...");
        uiWindow.windowScene = windowScene;
        self.window = uiWindow;
        self.window.backgroundColor = [UIColor systemGroupedBackgroundColor];
        [self.window makeKeyAndVisible];

// Create game render view — SwiftUI MetalGameView (UIViewRepresentable) manages placement
        g_gameRenderView = [[ARMSX2GameView alloc] initWithFrame:CGRectZero];
        g_gameRenderView.backgroundColor = [UIColor blackColor];
        g_gameRenderView.clipsToBounds = YES;
        // Do NOT addSubview here — SwiftUI's MetalGameView handles view hierarchy
        Console.WriteLn("[Layout] Game render view created (SwiftUI-managed)");
        
// Debug-only UI elements
#if DEBUG
        if (rootVC) {
            g_logView = [[UITextView alloc] initWithFrame:CGRectMake(10, 50, 600, 300)];
            g_logView.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.5];
            g_logView.textColor = [UIColor whiteColor];
            g_logView.font = [UIFont fontWithName:@"Courier" size:10];
            g_logView.editable = NO;
            g_logView.hidden = YES;
            [rootVC.view addSubview:g_logView];
        }
#endif
    }
    
    // --- [UI] Startup Logic: Show menu first, boot on user action ---
#if ARMSX2_HAS_SWIFTUI
    {
        UIViewController *rootVC = self.window.rootViewController;
        if (rootVC) {
            rootVC.view.backgroundColor = [UIColor systemGroupedBackgroundColor];

            UIViewController *menuVC = [SwiftUIHost createMenuController];
            menuVC.view.translatesAutoresizingMaskIntoConstraints = NO;
            menuVC.view.userInteractionEnabled = YES;
// Keep hosting controller always clear — SwiftUI RootView handles its own background
            menuVC.view.backgroundColor = [UIColor clearColor];
            [rootVC.view addSubview:menuVC.view];
            [NSLayoutConstraint activateConstraints:@[
                [menuVC.view.topAnchor constraintEqualToAnchor:rootVC.view.topAnchor],
                [menuVC.view.bottomAnchor constraintEqualToAnchor:rootVC.view.bottomAnchor],
                [menuVC.view.leadingAnchor constraintEqualToAnchor:rootVC.view.leadingAnchor],
                [menuVC.view.trailingAnchor constraintEqualToAnchor:rootVC.view.trailingAnchor],
            ]];
            [rootVC addChildViewController:menuVC];
            [menuVC didMoveToParentViewController:rootVC];
            s_menuVC = menuVC;
            s_rootVC = rootVC;

            if (g_logView) {
                g_logView.hidden = YES;
                g_logView.userInteractionEnabled = NO;
            }
            Console.WriteLn("[UI] SwiftUI menu attached (screen: %.0fx%.0f)",
                rootVC.view.bounds.size.width, rootVC.view.bounds.size.height);

}
    }

    // Listen for VM boot request from SwiftUI
    // queue:nil = synchronous delivery, so background colors are set BEFORE
    // SwiftUI re-renders the game overlay (avoids gray flash)
    [[NSNotificationCenter defaultCenter] addObserverForName:@"ARMSX2iOSRequestVMBoot"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        std::string bootISO;
        if (s_settings_interface)
            bootISO = s_settings_interface->GetStringValue("GameISO", "BootISO", "");
        const std::string biosPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
        std::fprintf(stderr, "@@BOOT_NOTIFY@@ received=1 has_bios=%d bios=\"%s\" boot_iso=\"%s\"\n",
            (!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(biosPath.c_str())) ? 1 : 0,
            EmuConfig.BaseFilenames.Bios.c_str(), bootISO.c_str());
        std::fflush(stderr);
        Console.WriteLn("[UI] VM boot requested from UI (rootVC=%p)", s_rootVC);
        ARMSX2ApplyIOSMultitapConfig("boot-request");
        if (s_rootVC) s_rootVC.view.backgroundColor = [UIColor blackColor];
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:@"ARMSX2iOSRequestVMShutdown"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        Console.WriteLn("[UI] VM shutdown requested from UI");
        s_requestVMStop.store(true);
    }];

    // ARMSX2iOSVMDidShutdown / ARMSX2iOSReturnToMenu: no rootVC background change needed.
    // SwiftUI RootView handles menu background via Color(systemGroupedBackground).ignoresSafeArea().
    // rootVC stays black after first boot — eliminates white flash during VM restart.

    [[NSNotificationCenter defaultCenter] addObserverForName:@"ARMSX2iOSVMDidShutdown"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        // No rootVC background change — SwiftUI handles menu bg
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:@"ARMSX2iOSReturnToMenu"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        // No rootVC background change — SwiftUI handles menu bg
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:@"ARMSX2iOSEnterGameScreen"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        if (s_rootVC) s_rootVC.view.backgroundColor = [UIColor blackColor];
    }];

// Auto-boot — debug/simulator only
#if DEBUG || TARGET_OS_SIMULATOR
    if (getenv("ARMSX2_AUTO_BOOT") && atoi(getenv("ARMSX2_AUTO_BOOT")) == 1) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            Console.WriteLn("[AutoBoot] @@AUTO_BOOT@@ posting ARMSX2iOSRequestVMBoot + AutoBootDidStart");
            [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMBoot" object:nil];
            [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSAutoBootDidStart" object:nil];
        });
    }
#endif // DEBUG || TARGET_OS_SIMULATOR — AUTO_BOOT
    // ps2autotests: auto-boot VM when ELF env var is set (always enabled)
    if (getenv("ARMSX2_BOOT_ELF")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMBoot" object:nil];
        });
    }

    // Cold-launch deep link: iOS delivers the launch URL through connectionOptions,
    NSSet<UIOpenURLContext *> *launchURLContexts = connectionOptions.URLContexts;
    if (launchURLContexts.count > 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            for (UIOpenURLContext *ctx in launchURLContexts) {
                NSLog(@"[ARMSX2 iOS DeepLink] cold-launch url=%@", ctx.URL.absoluteString);
                [DeepLinkBridge handle:ctx.URL];
            }
        });
    }
#else
    // Fallback: no SwiftUI — auto-boot like before
    if (!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif
    } else {
        Console.Warning("No valid BIOS found. Showing selection UI.");
        if (self.startBiosButton) {
            self.startBiosButton.hidden = NO;
            [self.window bringSubviewToFront:self.startBiosButton];
        }
    }
#endif
}

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts {
    for (UIOpenURLContext *context in URLContexts) {
        NSURL *url = context.URL;
        [DeepLinkBridge handle:url];
    }
}

- (void)checkAndConfigureBIOS {
    std::string dataRoot = EmuFolders::DataRoot;
    std::string biosDir = dataRoot + "/bios";
    
    // 0. [iPSX2] Check Env Var Override (ARMSX2_BIOS_PATH)
    const char* envBios = getenv("ARMSX2_BIOS_PATH");
    // Simulator: use ARMSX2_BIOS_PATH env var or auto-scan Documents/bios/
    // Real device: BIOS must be placed in Documents/bios/ via Files app
    Console.WriteLn("@@BIOS_DIR@@ path=\"%s\"", biosDir.c_str());
    
    if (envBios) {
        bool exists = FileSystem::FileExists(envBios);
        Console.WriteLn("@@BIOS_ENV@@ exists=%d", exists ? 1 : 0);
        
        if (exists) {
            // Copy to EmuFolders::Bios to ensure sandbox compliance
            struct stat st = {0};
            if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);
            
            std::string fileName(Path::GetFileName(envBios));
            std::string destPath = Path::Combine(biosDir, fileName);
            
            // Only copy if source != dest
            if (std::string(envBios) != destPath) {
                FILE *src = fopen(envBios, "rb");
                FILE *dst = fopen(destPath.c_str(), "wb");
                if (src && dst) {
                     char buffer[4096];
                     size_t bytes;
                     while ((bytes = fread(buffer, 1, 4096, src)) > 0) fwrite(buffer, 1, bytes, dst);
                     fclose(src); fclose(dst);
                     Console.WriteLn("Copied env-var BIOS to: %s", destPath.c_str());
                } else {
                     Console.Error("Failed to copy env-var BIOS. src=%p dst=%p", src, dst);
                     if (src) fclose(src);
                     if (dst) fclose(dst);
                }
            }
            
            EmuConfig.BaseFilenames.Bios = fileName;
            if (s_settings_interface) {
                s_settings_interface->SetStringValue("Filenames", "BIOS", EmuConfig.BaseFilenames.Bios.c_str());
                s_settings_interface->Save();
            }
            Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=env", EmuConfig.BaseFilenames.Bios.c_str());
            return;
        }
    } else {
        Console.WriteLn("@@BIOS_ENV@@ exists=0");
    }

    // 1. Check existing config
    if (!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
        Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=config", EmuConfig.BaseFilenames.Bios.c_str());
        return;
    }

    // 1b. Auto-move BIOS files from Documents/ root to bios/ subfolder
    {
        FileSystem::FindResultsArray rootResults;
        if (FileSystem::FindFiles(dataRoot.c_str(), "*", FILESYSTEM_FIND_FILES, &rootResults)) {
            for (const auto& fd : rootResults) {
                if (fd.Size >= 1024*1024 && fd.Size <= 50*1024*1024) {
                    std::string fn = std::string(Path::GetFileName(fd.FileName));
                    std::string ext = fn.size() >= 4 ? fn.substr(fn.size() - 4) : "";
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".bin" || ext == ".rom") {
                        std::string src = Path::Combine(dataRoot, fn);
                        std::string dst = Path::Combine(biosDir, fn);
                        if (!FileSystem::FileExists(dst.c_str())) {
                            if (rename(src.c_str(), dst.c_str()) == 0)
                                Console.WriteLn("[Files] Moved BIOS to bios/: %s", fn.c_str());
                            else
                                Console.WriteLn("[Files] Failed to move BIOS: %s (errno=%d)", fn.c_str(), errno);
                        }
                    }
                }
            }
        }
    }

    // 2. Scan Documents/bios
    FileSystem::FindResultsArray results;
    int foundCount = 0;
    if (FileSystem::FindFiles(biosDir.c_str(), "*", FILESYSTEM_FIND_FILES, &results)) {
        for (const auto& fd : results) {
            foundCount++;
            if (fd.Size >= 1024*1024 && (fd.FileName.find(".bin") != std::string::npos || fd.FileName.find(".BIN") != std::string::npos)) {
                // Found a candidate
                std::string currentName = std::string(Path::GetFileName(fd.FileName));
                EmuConfig.BaseFilenames.Bios = currentName;
                Console.WriteLn("Auto-detected BIOS (name only): %s", EmuConfig.BaseFilenames.Bios.c_str());
                if (s_settings_interface) {
                    s_settings_interface->SetStringValue("Filenames", "BIOS", EmuConfig.BaseFilenames.Bios.c_str());
                    s_settings_interface->Save();
                }
                Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=scan", EmuConfig.BaseFilenames.Bios.c_str());
                return;
            }
        }
    }
    Console.WriteLn("@@BIOS_SCAN@@ found=%d", foundCount);
    Console.WriteLn("@@BIOS_PICK@@ result=\"(none)\" source=none");

    // 3. Check Bundle Resources (Fallback)
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    std::string bundleDir = [resourcePath UTF8String];
    FileSystem::FindResultsArray bundleResults;

    // [iPSX2] Support "BiosFiles" folder reference
    std::string bfDir = bundleDir + "/BiosFiles";
    if (FileSystem::FindFiles(bfDir.c_str(), "*", FILESYSTEM_FIND_FILES, &bundleResults)) {
        for (const auto& fd : bundleResults) {
             if (fd.Size >= 1024*1024 && (fd.FileName.find(".bin") != std::string::npos || fd.FileName.find(".BIN") != std::string::npos)) {
                 Console.WriteLn("Found BIOS in BiosFiles: %s", fd.FileName.c_str());
                 struct stat st = {0};
                 if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);

                 std::string src = bfDir + "/" + fd.FileName;
                 std::string dst = biosDir + "/" + fd.FileName;
                 FILE *s=fopen(src.c_str(),"rb"), *d=fopen(dst.c_str(),"wb");
                 if(s && d) { char b[4096]; size_t n; while((n=fread(b,1,4096,s))>0) fwrite(b,1,n,d); }
                 if(s) fclose(s); if(d) fclose(d);
                 EmuConfig.BaseFilenames.Bios = fd.FileName;
                 return;
             }
        }
    }
    if (FileSystem::FindFiles(bundleDir.c_str(), "*", FILESYSTEM_FIND_FILES, &bundleResults)) {
        for (const auto& fd : bundleResults) {
             if (fd.Size >= 1024*1024 && (fd.FileName.find(".bin") != std::string::npos || fd.FileName.find(".BIN") != std::string::npos)) {
                 Console.WriteLn("Found BIOS in Bundle: %s. Copying...", fd.FileName.c_str());
                 std::string srcPath = bundleDir + "/" + fd.FileName;
                 std::string destPath = biosDir + "/" + fd.FileName;
                 
                 struct stat st = {0};
                 if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);

                 FILE *src = fopen(srcPath.c_str(), "rb");
                 FILE *dst = fopen(destPath.c_str(), "wb");
                 if (src && dst) {
                     char buffer[4096];
                     size_t bytes;
                     while ((bytes = fread(buffer, 1, 4096, src)) > 0) fwrite(buffer, 1, bytes, dst);
                     fclose(src); fclose(dst);
                     EmuConfig.BaseFilenames.Bios = fd.FileName;
                     Console.WriteLn("Copy and set successful.");
                     return;
                 }
                 if(src) fclose(src);
                 if(dst) fclose(dst);
             }
        }
    }
    
    Console.Warning("No BIOS found automatically.");
    EmuConfig.BaseFilenames.Bios.clear();
}

- (void)showBiosPicker {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIDocumentPickerViewController *documentPicker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
    documentPicker.delegate = self;
    documentPicker.allowsMultipleSelection = NO;
    [self.window.rootViewController presentViewController:documentPicker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (urls.count == 0) return;
    
    NSURL *url = urls.firstObject;
    Console.WriteLn("User picked file: %s", [[url path] UTF8String]);
    
    // Copy to Documents/bios
    std::string biosDir = EmuFolders::DataRoot + "/bios";
    struct stat st = {0};
    if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);
    
    NSString *destPath = [NSString stringWithFormat:@"%s/%@", biosDir.c_str(), [url lastPathComponent]];
    NSError *error = nil;
    
    // Remove if exists
    [[NSFileManager defaultManager] removeItemAtPath:destPath error:nil];
    
    if ([[NSFileManager defaultManager] copyItemAtURL:url toURL:[NSURL fileURLWithPath:destPath] error:&error]) {
        Console.WriteLn("Imported BIOS to: %s", [destPath UTF8String]);
        
        std::string fileName = [[destPath lastPathComponent] UTF8String];
        EmuConfig.BaseFilenames.Bios = fileName;
        
        // Hide button and start VM
        dispatch_async(dispatch_get_main_queue(), ^{
            self.startBiosButton.hidden = YES;
        });
        
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif

    } else {
        Console.Error("Failed to import BIOS: %s", [[error localizedDescription] UTF8String]);
        Host::ReportErrorAsync("Import Failed", [[error localizedDescription] UTF8String]);
    }
}

// JIT availability check for real devices. The allocation path can use MAP_JIT,
// dual-map, or legacy mprotect depending on what iOS/LiveContainer permits.
- (void)checkJITAndStartVM {
#if !TARGET_OS_SIMULATOR
    ARMSX2ApplyJITScriptProtocol("jit-gate");
    if (DarwinMisc::IsJITAvailable()) {
        std::fprintf(stderr, "@@BOOT_JIT_GATE@@ available=1 mode=jit_alloc\n");
        std::fflush(stderr);
        Console.WriteLn("@@JIT_GATE@@ JIT channel available; starting VM");
        [self startVMThread];
        return;
    }

    std::fprintf(stderr, "@@BOOT_JIT_GATE@@ available=0 mode=blocked reason=no_debug_jit_channel\n");
    std::fflush(stderr);
    Console.Error("@@JIT_GATE@@ No debug/JIT channel; VM boot blocked because this build requires JIT.");
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_rootVC)
            s_rootVC.view.backgroundColor = [UIColor systemGroupedBackgroundColor];
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSReturnToMenu" object:nil];
    });
    Host::ReportErrorAsync("JIT Unavailable", "Launch through the debugger/JIT enabler so iOS marks this process as debugged.");
#else
    [self startVMThread];
#endif
}

- (void)startVMThread {
    ARMSX2ApplyJITScriptProtocol("start-vm-thread");
    {
        std::lock_guard<std::mutex> lk(s_vmMutex);
        if (s_vmThreadActive.load()) {
            std::fprintf(stderr, "@@BOOT_START_THREAD@@ active=1 action=ignored\n");
            std::fflush(stderr);
            Console.WriteLn("[VM] startVMThread: VM already active, ignoring");
            return;
        }

        // Signal the persistent thread to boot
        s_requestVMBoot.store(true);
        s_requestVMStop.store(false);

        if (s_vmThreadCreated) {
            std::fprintf(stderr, "@@BOOT_START_THREAD@@ active=0 created=1 action=signal\n");
            std::fflush(stderr);
            Console.WriteLn("[VM] startVMThread: signaling existing VM thread");
            s_vmCV.notify_one();
            return;
        }

        // First call: create the persistent thread
        s_vmThreadCreated = true;
    }

    std::fprintf(stderr, "@@BOOT_START_THREAD@@ active=0 created=0 action=create\n");
    std::fflush(stderr);
    Console.WriteLn("[VM] Creating persistent VM thread...");
    s_vmThreadInitComplete.store(false, std::memory_order_release);
    ARMSX2ScheduleVMInitWatchdog();

    std::thread vmThread([]() {
        // === ONE-TIME INIT (runs once per app lifetime) ===
        s_cpuThreadId = std::this_thread::get_id();
        std::fprintf(stderr, "@@BOOT_THREAD_INIT@@ begin=1\n");
        std::fflush(stderr);
        ARMSX2ConfigureImGuiFonts("vm-thread");
        Console.WriteLn("[VM] VM Thread: CPUThreadInitialize (once)...");
        if (!VMManager::Internal::CPUThreadInitialize()) {
            std::fprintf(stderr, "@@BOOT_THREAD_INIT@@ ok=0\n");
            std::fflush(stderr);
            Console.Error("VM Thread: CPUThreadInitialize failed.");
            ARMSX2SurfaceVMInitFailure("cpu_thread_initialize_failed",
                "VM initialization failed during memory setup. Try JIT Script: Legacy (UTM-Dolphin) in Settings, or restart ARMSX2 via StikDebug.");
            std::lock_guard<std::mutex> lk(s_vmMutex);
            s_vmThreadCreated = false;
            return;
        }
        s_vmThreadInitComplete.store(true, std::memory_order_release);
        std::fprintf(stderr, "@@BOOT_THREAD_INIT@@ ok=1\n");
        std::fflush(stderr);

        // === PERSISTENT BOOT LOOP ===
        bool auto_boot_first = (getenv("ARMSX2_AUTO_BOOT") && atoi(getenv("ARMSX2_AUTO_BOOT")) == 1)
                            || (getenv("ARMSX2_BOOT_ELF") != nullptr);
        while (true) {
            // Wait for boot signal (or auto-boot on first iteration)
            {
                std::unique_lock<std::mutex> lk(s_vmMutex);
                if (auto_boot_first) {
                    Console.WriteLn("[AutoBoot] @@AUTO_BOOT@@ skipping UI wait, auto-boot enabled");
                    auto_boot_first = false;
                } else {
                    std::fprintf(stderr, "@@BOOT_THREAD_WAIT@@ waiting=1\n");
                    std::fflush(stderr);
                    Console.WriteLn("[VM] VM Thread: waiting for boot request...");
                    s_vmCV.wait(lk, [] { return s_requestVMBoot.load(); });
                }
                s_requestVMBoot.store(false);
            }

            std::fprintf(stderr, "@@BOOT_THREAD_SIGNAL@@ received=1\n");
            std::fflush(stderr);
            Console.WriteLn("[VM] VM Thread: boot signal received, preparing boot params...");
            s_vmThreadActive.store(true);
            const unsigned int heartbeat_generation =
                s_vmHeartbeatGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

            // --- Build boot parameters from INI ---
            VMBootParameters boot_params;
            boot_params.fast_boot = false;
            {
                std::string isoDir = EmuFolders::DataRoot + "/iso";
                std::string defaultISO = "";
                std::string isoFilename = s_settings_interface->GetStringValue("GameISO", "BootISO", defaultISO.c_str());
                bool fastBoot = ARMSX2GetConfiguredFastBoot();
                s_settings_interface->SetStringValue("GameISO", "BootISO", isoFilename.c_str());
                s_settings_interface->SetBoolValue("GameISO", "FastBoot", fastBoot);
                s_settings_interface->SetBoolValue("EmuCore", "EnableFastBoot", fastBoot);
                s_settings_interface->Save();
                std::fprintf(stderr, "@@BOOT_FASTBOOT_READ@@ selected=%d\n", fastBoot ? 1 : 0);
                std::fflush(stderr);
                std::string isoPath = (!isoFilename.empty() && isoFilename.front() == '/') ? isoFilename : (isoDir + "/" + isoFilename);
                // Fallback: check Documents/ root if not found in iso/.
                if (!isoFilename.empty() && isoFilename.front() != '/' && !FileSystem::FileExists(isoPath.c_str())) {
                    std::string rootPath = EmuFolders::DataRoot + "/" + isoFilename;
                    if (FileSystem::FileExists(rootPath.c_str())) {
                        isoPath = rootPath;
                        Console.WriteLn("ISO found in Documents/ root: %s", isoPath.c_str());
                    }
                }
                const bool isoExists = !isoFilename.empty() && FileSystem::FileExists(isoPath.c_str());
                std::fprintf(stderr, "@@BOOT_PARAMS@@ ini_iso=\"%s\" resolved=\"%s\" exists=%d fast_boot=%d\n",
                    isoFilename.c_str(), isoPath.c_str(), isoExists ? 1 : 0, fastBoot ? 1 : 0);
                std::fflush(stderr);
                if (isoExists) {
                    std::string suffix = isoFilename.size() >= 4 ? isoFilename.substr(isoFilename.size() - 4) : "";
                    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
                    bool isElf = (suffix == ".elf");
                    if (isElf) {
                        boot_params.elf_override = isoPath;
                        boot_params.source_type = CDVD_SourceType::NoDisc;
                        boot_params.fast_boot = true;
                        std::fprintf(stderr, "@@ISO_BOOT@@ path=%s fast_boot=1 mode=ELF INI=\"%s\"\n",
                            isoPath.c_str(), isoFilename.c_str());
                        std::fflush(stderr);
                        Console.WriteLn("@@ISO_BOOT@@ path=%s fast_boot=1 mode=ELF (INI: %s)", isoPath.c_str(), isoFilename.c_str());
                    } else {
                        boot_params.filename = isoPath;
                        boot_params.source_type = CDVD_SourceType::Iso;
                        boot_params.fast_boot = fastBoot;
                        std::fprintf(stderr, "@@ISO_BOOT@@ path=%s fast_boot=%d mode=ISO INI=\"%s\"\n",
                            isoPath.c_str(), fastBoot ? 1 : 0, isoFilename.c_str());
                        std::fflush(stderr);
                        Console.WriteLn("@@ISO_BOOT@@ path=%s fast_boot=%d (INI: %s)", isoPath.c_str(), fastBoot ? 1 : 0, isoFilename.c_str());
                    }
                } else {
                    std::fprintf(stderr, "@@ISO_BOOT_MISSING@@ ini_iso=\"%s\" attempted=\"%s\"\n",
                        isoFilename.c_str(), isoPath.c_str());
                    std::fflush(stderr);
                    Console.WriteLn("@@ISO_BOOT@@ no ISO='%s', falling back to BIOS only", isoFilename.c_str());
                }
            }

            if (getenv("ARMSX2_AUTO_BOOT_BIOS")) {
                Console.WriteLn("@@AUTO_BOOT_BIOS@@ enabled=1 action=triggered");
                boot_params.fast_boot = false;
            }
            // ps2autotests: boot ELF directly via env var
            if (const char* testElf = getenv("ARMSX2_BOOT_ELF")) {
                boot_params.elf_override = testElf;
                boot_params.source_type = CDVD_SourceType::NoDisc;
                boot_params.fast_boot = true;
                Console.WriteLn("@@BOOT_ELF@@ elf=%s", testElf);
            }

            // BIOS sanity check
            const std::string biosPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
            const bool biosExists = !EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(biosPath.c_str());
            std::fprintf(stderr, "@@BOOT_BIOS_CHECK@@ bios=\"%s\" path=\"%s\" exists=%d\n",
                EmuConfig.BaseFilenames.Bios.c_str(), biosPath.c_str(), biosExists ? 1 : 0);
            std::fflush(stderr);
            if (!biosExists) {
                std::fprintf(stderr, "@@BOOT_BIOS_FAIL@@ action=abort_to_menu\n");
                std::fflush(stderr);
                Console.Error("CRITICAL: BIOS verification failed inside VM thread.");
                Host::ReportErrorAsync("BIOS Error", "Validation failed.");
                s_vmThreadActive.store(false);
                s_vmHeartbeatGeneration.fetch_add(1, std::memory_order_acq_rel);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSVMDidShutdown" object:nil];
                });
                continue; // back to wait loop
            }

            ARMSX2SanitizeFrameLimiterConfig("pre-vm-initialize");
            ARMSX2EnsureIOSSpeedhackDefaults(s_settings_interface, "pre-vm-initialize");
            ARMSX2RepairIOSARM64JITSettings(s_settings_interface, "pre-vm-initialize");
            ARMSX2MigrateJITScriptProtocolForIOS(s_settings_interface, "pre-vm-initialize");
            VMManager::Internal::LoadStartupSettings();
            ARMSX2ApplyIOSOsdPresetFromConfig("pre-vm-initialize");
            EmuConfig.Speedhacks.vuThread =
                s_settings_interface->GetBoolValue("EmuCore/Speedhacks", "vuThread", EmuConfig.Speedhacks.vuThread);
            EmuConfig.Cpu.Recompiler.EnableFastmem =
                s_settings_interface->GetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", EmuConfig.Cpu.Recompiler.EnableFastmem);
            std::fprintf(stderr, "@@IOS_PREVM_SYNC_SETTINGS@@ mtvu=%d fastmem=%d\n",
                EmuConfig.Speedhacks.vuThread ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableFastmem ? 1 : 0);
            std::fflush(stderr);
            const int configuredCoreType = s_settings_interface->GetIntValue("EmuCore/CPU", "CoreType", 2);
            const bool configuredUseArm64 = s_settings_interface->GetBoolValue("EmuCore/CPU", "UseArm64Dynarec", configuredCoreType == 2);
            std::fprintf(stderr,
                "@@CPU_CONFIG@@ core=%d use_arm64=%d ee=%d iop=%d vu0=%d vu1=%d fastmem=%d mtvu=%d forced_interp=%d\n",
                configuredCoreType, configuredUseArm64 ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableEE ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableIOP ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableVU0 ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableVU1 ? 1 : 0,
                EmuConfig.Cpu.Recompiler.EnableFastmem ? 1 : 0,
                EmuConfig.Speedhacks.vuThread ? 1 : 0,
                DarwinMisc::iPSX2_FORCE_EE_INTERP ? 1 : 0);
            std::fflush(stderr);
            Console.WriteLn("@@FRAMELIMIT@@ boot nominal=%.3f turbo=%.3f slomo=%.3f ntsc=%.3f pal=%.3f",
                EmuConfig.EmulationSpeed.NominalScalar,
                EmuConfig.EmulationSpeed.TurboScalar,
                EmuConfig.EmulationSpeed.SlomoScalar,
                EmuConfig.GS.FramerateNTSC,
                EmuConfig.GS.FrameratePAL);

            ARMSX2ApplyJITScriptProtocol("pre-vm-initialize");

            // --- Initialize & Execute VM ---
            Error bootError;
            const VMBootResult bootResult = VMManager::Initialize(boot_params, &bootError);
            const std::string bootErrorText = bootError.GetDescription();
            std::fprintf(stderr, "@@BOOT_VM_INIT@@ result=%d success=%d error=\"%s\"\n",
                static_cast<int>(bootResult), bootResult == VMBootResult::StartupSuccess ? 1 : 0,
                bootErrorText.c_str());
            std::fflush(stderr);
            if (bootResult == VMBootResult::StartupSuccess) {
                std::fprintf(stderr, "@@BOOT_POST_INIT@@ stage=before_osd state=%d frame=%u\n",
                    static_cast<int>(VMManager::GetState()), ::g_FrameCount);
                std::fflush(stderr);
                ARMSX2ApplyIOSOsdPresetFromConfig("post-vm-initialize");
                std::fprintf(stderr, "@@BOOT_POST_INIT@@ stage=after_osd state=%d frame=%u\n",
                    static_cast<int>(VMManager::GetState()), ::g_FrameCount);
                std::fflush(stderr);
                Console.WriteLn("[VM] VM initialized successfully");
                VMManager::SetState(VMState::Running);
                std::fprintf(stderr, "@@BOOT_POST_INIT@@ stage=after_set_running state=%d frame=%u\n",
                    static_cast<int>(VMManager::GetState()), ::g_FrameCount);
                std::fflush(stderr);

                if (ARMSX2IOSRuntimeTelemetryEnabled()) {
                    std::thread([heartbeat_generation]() {
                        for (int sec = 1; sec <= 180; sec++) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            if (!s_vmThreadActive.load(std::memory_order_relaxed) ||
                                s_vmHeartbeatGeneration.load(std::memory_order_acquire) != heartbeat_generation)
                                break;
                            if (sec != 1 && (sec % 5) != 0)
                                continue;
                            std::fprintf(stderr,
                                "@@VM_HEARTBEAT@@ sec=%d state=%d frame=%u pm_frame=%llu fps=%.2f internal_fps=%.2f speed=%.2f cpu=%.2f vu=%.2f gs=%.2f gpu=%.2f ee_pc=0x%08x ee_cycle=%lld ee_next=%lld\n",
                                sec,
                                static_cast<int>(VMManager::GetState()),
                                ::g_FrameCount,
                                static_cast<unsigned long long>(PerformanceMetrics::GetFrameNumber()),
                                PerformanceMetrics::GetFPS(),
                                PerformanceMetrics::GetInternalFPS(),
                                PerformanceMetrics::GetSpeed(),
                                PerformanceMetrics::GetCPUThreadUsage(),
                                PerformanceMetrics::GetVUThreadUsage(),
                                PerformanceMetrics::GetGSThreadUsage(),
                                PerformanceMetrics::GetGPUUsage(),
                                cpuRegs.pc,
                                static_cast<long long>(cpuRegs.cycle),
                                static_cast<long long>(cpuRegs.nextEventCycle));
                        }
                    }).detach();
                }

                while (true) {
                    Host::PumpMessagesOnCPUThread();

                    if (s_requestVMStop.load()) {
                        Console.WriteLn("[VM] VM Thread: stop requested from UI.");
                        break;
                    }
                    VMState state = VMManager::GetState();
                    if (state == VMState::Stopping || state == VMState::Shutdown) {
                        Console.WriteLn("[VM] VM Thread: shutdown signal received.");
                        break;
                    } else if (state == VMState::Running) {
                        std::fprintf(stderr, "@@BOOT_EXEC_ENTER@@ state=%d frame=%u\n",
                            static_cast<int>(state), ::g_FrameCount);
                        std::fflush(stderr);
                        VMManager::Execute();
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }

                Console.WriteLn("[VM] VM Thread: shutting down VM...");
                VMManager::Shutdown(false);
            } else {
                Console.Error("VM Thread: VMManager::Initialize failed!");
                Host::ReportErrorAsync("Startup Error", "VM Initialization Failed.");
            }

            // --- Post-shutdown: reset state, notify UI ---
            s_vmThreadActive.store(false);
            s_vmHeartbeatGeneration.fetch_add(1, std::memory_order_acq_rel);
            s_requestVMStop.store(false);
            Console.WriteLn("[VM] VM Thread: shutdown complete, posting notification");
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSVMDidShutdown" object:nil];
            });
        } // end while(true) boot loop

        // Note: CPUThreadShutdown() is never reached because the thread persists.
        // It would only be needed if we added an app-termination signal.
    });
    vmThread.detach();
}

- (void)sceneDidDisconnect:(UIScene *)scene {
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
}

- (void)sceneWillResignActive:(UIScene *)scene {
// NVM save when app loses focus
    if (s_vmThreadActive.load(std::memory_order_relaxed) && !BiosPath.empty()) {
        extern void cdvdSaveNVRAM();
        cdvdSaveNVRAM();
        Console.WriteLn("[NVM] NVM saved on sceneWillResignActive");
    } else {
        Console.WriteLn("[NVM] Skipped save on sceneWillResignActive active=%d biosPath=%d",
            s_vmThreadActive.load(std::memory_order_relaxed) ? 1 : 0, BiosPath.empty() ? 0 : 1);
    }
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
// Save NVM + memory cards when app goes to background.
    // Without this, BIOS settings (language/date) are lost on every restart
    // because cdvdSaveNVRAM() is only called at VM shutdown, which never
    // happens when iOS terminates the app via SIGTERM.
    if (s_vmThreadActive.load(std::memory_order_relaxed) && !BiosPath.empty()) {
        extern void cdvdSaveNVRAM();
        cdvdSaveNVRAM();
        Console.WriteLn("[NVM] NVM saved on sceneDidEnterBackground");
    } else {
        Console.WriteLn("[NVM] Skipped save on sceneDidEnterBackground active=%d biosPath=%d",
            s_vmThreadActive.load(std::memory_order_relaxed) ? 1 : 0, BiosPath.empty() ? 0 : 1);
    }
}

@end

static void SetupIOSDirectories(const std::string& dataRoot)
{
    const char* dirs[] = {"bios", "iso", "logs", "memcards", "savestates",
                          "snaps", "cheats", "patches", "cache", "covers",
                          "gamesettings", "textures", "inputprofiles", "videos",
                          "inis", "resources"};
    mkdir(dataRoot.c_str(), 0755);
    for (auto d : dirs)
        mkdir((dataRoot + "/" + d).c_str(), 0755);

    EmuFolders::DataRoot = dataRoot;
    EmuFolders::Settings = dataRoot + "/inis";
    EmuFolders::Bios = dataRoot + "/bios";
    EmuFolders::Logs = dataRoot + "/logs";
    EmuFolders::Savestates = dataRoot + "/savestates";
    EmuFolders::MemoryCards = dataRoot + "/memcards";
    EmuFolders::Snapshots = dataRoot + "/snaps";
    EmuFolders::Cheats = dataRoot + "/cheats";
    EmuFolders::Patches = dataRoot + "/patches";
    EmuFolders::Cache = dataRoot + "/cache";
    EmuFolders::Covers = dataRoot + "/covers";
    EmuFolders::GameSettings = dataRoot + "/gamesettings";
    EmuFolders::Textures = dataRoot + "/textures";
    EmuFolders::InputProfiles = dataRoot + "/inputprofiles";
    EmuFolders::Videos = dataRoot + "/videos";
    EmuFolders::UserResources = dataRoot + "/resources";
}


@interface PCSX2AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation PCSX2AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // Override point for customization after application launch.
    [UIDevice currentDevice].batteryMonitoringEnabled = YES;
    
    // --- Setup PCSX2 Environment (Moved from SceneDelegate) ---
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    
    std::string dataRoot = [documentsDirectory UTF8String];
    SetupIOSDirectories(dataRoot);
    EmuFolders::AppRoot = [resourcePath UTF8String];
    EmuFolders::Resources = [resourcePath UTF8String];

    // --- Unified Logging Redirection ---
    // Force stderr and stdout to pcsx2_log.txt
    std::string logPath = dataRoot + "/pcsx2_log.txt";
    
    // Redirect stderr to file
    if (freopen(logPath.c_str(), "w", stderr) == NULL) { // "w" clears old logs
        printf("Reopen stderr failed\n");
    }
    
    // Redirect stdout to stderr
    if (dup2(fileno(stderr), fileno(stdout)) == -1) {
        fprintf(stderr, "Redirection of stdout failed\n");
    }
    
    // Disable buffering
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    
    // [iPSX2] Register File Descriptor for Signal Handler
    // We use the raw file descriptor of stderr (which is now our log file)
    DarwinMisc::SetCrashLogFD(fileno(stderr));
    
    // Log Proof Tag
    fprintf(stderr, "@@LOG_SINK@@ unified=1 path=%s pid=%d\n", logPath.c_str(), getpid());
    NSString* bundleID = [[NSBundle mainBundle] bundleIdentifier];
    fprintf(stderr, "@@BUNDLE_ID@@ %s\n", bundleID ? [bundleID UTF8String] : "(null)");
#ifndef ARMSX2_VERSION_STR
#define ARMSX2_VERSION_STR "dev"
#endif
#ifndef ARMSX2_GIT_HASH
#define ARMSX2_GIT_HASH "unknown"
#endif
#ifndef ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
#define ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS 0
#endif
    fprintf(stderr, "@@BUILD_ID@@ ARMSX2_iOS v%s %s %s %s\n",
        ARMSX2_VERSION_STR, ARMSX2_GIT_HASH, __DATE__, __TIME__);
    fprintf(stderr, "@@TEST_MARKER@@ armsx2_ios_32_pr189_pr190_ra_login_recover_v1\n");
    fprintf(stderr, "@@FF_FIX@@ offspeed_present_skip=1 present_cap60=1 adaptive_backoff=1 drawable_wait_probe=1 vm_pace_probe=1 turbo_only_toggle=1\n");
    fprintf(stderr, "@@DIAG_MODE@@ ee_hotpath=%d\n", ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS);
    
    // [iPSX2] Unification Validation
    // @@BIOS_GATE@@ build_id=2026-01-14_13-30-00 bundle=(from_nsbundle)
    NSString* bID = [[NSBundle mainBundle] bundleIdentifier];
    const char* cBundle = bID ? [bID UTF8String] : "(null)";
    fprintf(stderr, "@@BIOS_GATE@@ build_id=2026-01-17_PROBE bundle=%s\n", cBundle);
    fprintf(stderr, "@@LOG_UNIFIED@@ pcsx2_log.txt includes emulog output; emulog.txt disabled=1\n");
    ARMSX2ConfigureImGuiFonts("app-launch");
    
// DYLD Map — debug builds only
#if DEBUG
    {
        fprintf(stderr, "@@CFG@@ iPSX2_CRASH_DIAG=1 (DYLD Dump Enabled)\n");
        uint32_t count = _dyld_image_count();
        for (uint32_t i = 0; i < count; i++) {
            const char* name = _dyld_get_image_name(i);
            intptr_t slide = _dyld_get_image_vmaddr_slide(i);
            const struct mach_header* hdr = _dyld_get_image_header(i);
            fprintf(stderr, "@@DYLD_MAP@@ idx=%u addr=%p slide=%p path=%s\n", i, hdr, (void*)slide, name);
        }
    }
#endif
    fflush(stderr);

    // Enable PCSX2 Console Output only (std::cout/cerr will now go to file)
    Log::SetConsoleOutputLevel(LOGLEVEL::LOGLEVEL_INFO);
    
    Console.WriteLn("PCSX2 iOS: AppDelegate didFinishLaunching.");
    
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession options:(UISceneConnectionOptions *)options {
    // Called when a new scene session is being created.
    // Use this method to select a configuration to create the new scene with.
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default Configuration" sessionRole:connectingSceneSession.role];
    config.delegateClass = [PCSX2SceneDelegate class];
    return config;
}

- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions {
}

@end


//
// -- Main Entry Point --
//

// Signal Handler removed to allow PCSX2's internal handlers to work
#import <UIKit/UIKit.h>
#include <mach-o/dyld.h>
#include <cstdio>

int main(int argc, char * argv[]) {
    @autoreleasepool {
        // SDL_MAIN_HANDLED is set, so we use standard main()
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([PCSX2AppDelegate class]));
    }
}
#endif // iPSX2_MACOS
