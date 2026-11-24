// SceneDelegate.mm — iOS scene delegate: SDL/PCSX2 bootstrap, window setup,
// SwiftUI menu attachment and BIOS discovery.

#define SDL_MAIN_HANDLED

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/CDVD/CDVD.h"          // cdvdSaveNVRAM
#include "pcsx2/ps2/BiosTools.h"      // BiosPath
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/Config.h"             // EmuConfig, GSConfig
#include "pcsx2/Counters.h"           // g_FrameCount
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Memory.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/R5900.h"              // cpuRegs
#include "pcsx2/VMManager.h"
#include "pcsx2/vtlb.h"               // vtlb_FastmemAreaUnavailable
#include "pcsx2/SIO/Memcard/MemoryCardFile.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "IconsFontAwesome.h"

#include "common/Darwin/DarwinMisc.h"

#import <UIKit/UIKit.h>
#import <SwiftUI/SwiftUI.h>
#import <GameController/GameController.h>

// Xcode names the generated Swift bridge header after the Swift module.
#import <MetalKit/MetalKit.h>
#import "ARMSX2iOS-Swift.h"

#include "IOSRuntime.h"
#import "ARMSX2GameView.h"
#import "PCSX2SceneDelegate.h"
#import "SceneDelegateInternal.h"
#import "ARMSX2Bridge.h"

std::atomic<bool> s_loadLastSaveStateOnBoot{false};

@implementation PCSX2SceneDelegate

#pragma mark - Scene connection & bootstrap
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;

    // Let ValidateJITAlive see VM/worker state before any of its callers can
    // run (they are all scene-driven, so this is always first).
    DarwinMisc::SetJITActivityQuery(&ARMSX2JITWorkerBusy);

    UIWindowScene *windowScene = (UIWindowScene *)scene;
    
    static bool s_initialized = false;
    if (!s_initialized) {
        SDL_SetMainReady();
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            NSLog(@"SDL_Init failed: %s", SDL_GetError());
            return;
        }
        s_initialized = true;
    }
    ARMSX2InstallNativeGamepadDpadObserversOnMain();
    
    [self loadSettingsOnce];
    ARMSX2_PostRetroAchievementsStateChanged();
    
    Host::g_sdl_window = SDL_CreateWindow("PCSX2 iOS", 1280, 720, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE);
    if (!Host::g_sdl_window) {
        Console.Error("Failed to create SDL window: %s", SDL_GetError());
        return;
    }
    
    UIWindow *uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(Host::g_sdl_window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (uiWindow) {
        uiWindow.windowScene = windowScene;
        self.window = uiWindow;
        self.window.backgroundColor = [UIColor systemGroupedBackgroundColor];
        // SDL creates the original render controller. Keep it as a child of a
        // GCEventViewController so UIKit can own menu focus without changing the
        // CAMetalLayer host used by emulation.
        UIViewController *sdlRootVC = uiWindow.rootViewController;
        if (sdlRootVC) {
            s_sdlRootVC = sdlRootVC;
            UIViewController *eventHost =
                [SwiftUIHost createControllerEventHostWithContentController:sdlRootVC];
            if (eventHost) {
                uiWindow.rootViewController = eventHost;
                s_rootVC = eventHost;
            }
        }
        // Before the window is on screen, so there is no wrong first frame.
        [self syncRootViewToWindow];
        [self.window makeKeyAndVisible];

        // ProMotion only needs CADisableMinimumFrameDurationOnPhone in Info.plist. Pinning
        // preferredFrameRateRange via -[UIUpdateLink initWithWindowScene:] crashes: no such selector.

        // A reconnect keeps the view a running GS renders into; the new menu embeds it again.
        if (!g_gameRenderView) {
            g_gameRenderView = [[ARMSX2GameView alloc] initWithFrame:CGRectZero];
            g_gameRenderView.backgroundColor = [UIColor blackColor];
            g_gameRenderView.clipsToBounds = YES;
        }
        // Do NOT addSubview here — SwiftUI's MetalGameView handles view hierarchy
    }
    
    {
        UIViewController *rootVC = self.window.rootViewController;
        if (rootVC) {
            rootVC.view.backgroundColor = [UIColor systemGroupedBackgroundColor];

            if (connectionOptions.URLContexts.count > 0 || getenv("ARMSX2_BOOT_ELF") ||
                getenv("ARMSX2_AUTO_BOOT")) {
                [SwiftUIHost suppressAutomaticGameStartup];
            }
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

            [self syncRootViewToWindow];
            Console.WriteLn("[UI] SwiftUI menu attached (screen: %.0fx%.0f)",
                rootVC.view.bounds.size.width, rootVC.view.bounds.size.height);

}
    }

    // queue:nil = synchronous delivery, so background colors are set BEFORE
    // SwiftUI re-renders the game overlay (avoids gray flash)
    [[NSNotificationCenter defaultCenter] addObserverForName:ARMSX2iOSRequestVMBoot
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        std::string bootISO;
        if (s_settings_interface)
            bootISO = Host::GetBaseStringSettingValue("GameISO", "BootISO", "");
        const std::string biosPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
        std::fprintf(stderr, "@@BOOT_NOTIFY@@ received=1 has_bios=%d bios=\"%s\" boot_iso=\"%s\"\n",
            (!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(biosPath.c_str())) ? 1 : 0,
            EmuConfig.BaseFilenames.Bios.c_str(), bootISO.c_str());
        std::fflush(stderr);
        NSNumber* loadState = note.userInfo[@"loadLastSaveState"];
        s_loadLastSaveStateOnBoot.store(loadState ? loadState.boolValue : true);
        ARMSX2ApplyIOSMultitapConfig("boot-request");
        if (s_rootVC) s_rootVC.view.backgroundColor = [UIColor blackColor];
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif
    }];

    // RootView paints the menu background; rootVC stays black so a VM restart has no white flash.

    [[NSNotificationCenter defaultCenter] addObserverForName:ARMSX2iOSVMDidShutdown
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:ARMSX2iOSReturnToMenu
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:ARMSX2iOSEnterGameScreen
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
            [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSRequestVMBoot object:nil];
            [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSAutoBootDidStart object:nil];
        });
    }
#endif // DEBUG || TARGET_OS_SIMULATOR — AUTO_BOOT
    // ps2autotests: auto-boot VM when ELF env var is set (always enabled)
    if (getenv("ARMSX2_BOOT_ELF")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter] postNotificationName:ARMSX2iOSRequestVMBoot object:nil];
        });
    }

    NSSet<UIOpenURLContext *> *launchURLContexts = connectionOptions.URLContexts;
    if (launchURLContexts.count > 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            for (UIOpenURLContext *ctx in launchURLContexts) {
                NSLog(@"[ARMSX2 iOS DeepLink] cold-launch host=%@", ctx.URL.host);
                [DeepLinkBridge handle:ctx.URL];
            }
        });
    }
}

- (void)loadSettingsOnce {
    // Process-wide state that every boot reloads on the VM thread; a reconnect must not rewrite it.
    static bool s_loaded = false;
    if (s_loaded) return;
    s_loaded = true;

    std::string dataRoot = [[ARMSX2Bridge documentsDirectory] UTF8String];
    
    EmuFolders::DataRoot = dataRoot;
    EmuFolders::Bios = dataRoot + "/bios";

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

            s_settings_interface->SetStringValue("SPU2/Output", "Backend", "SDL");

            s_settings_interface->SetIntValue("EmuCore/GS", "VsyncQueueSize", 8);

            // Normal console-speed frame limiter. Do not save reduced nominal
            // speed here; that is not a safe FPS cap for iOS.
            s_settings_interface->SetFloatValue("Framerate", "NominalScalar", 1.0f);
            s_settings_interface->SetBoolValue("GameISO", "FastBoot", false);
            s_settings_interface->SetBoolValue("EmuCore", "EnableFastBoot", false);

            s_settings_interface->SetBoolValue("EmuCore/Speedhacks", "vuThread", ARMSX2ShouldEnableMTVUByDefault(nullptr));

            s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
            s_settings_interface->SetBoolValue("Achievements", "ChallengeMode", false);

            s_settings_interface->SetIntValue("ARMSX2iOS/Gamepad", "MultitapMode", 0);

            s_settings_interface->Save();
        }
        Host::Internal::SetBaseSettingsLayer(s_settings_interface);
        std::string secretsPath = dataRoot + "/PCSX2-iOS-secrets.ini";
        s_secrets_settings_interface = new INISettingsInterface(secretsPath);
        s_secrets_settings_interface->Load();
        Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface);
	// Load gamepad button mapping from INI
        for (int i = 0; i < 16; i++) {
            char key[32]; snprintf(key, sizeof(key), "Button%d", i);
            int val = s_settings_interface->GetIntValue("ARMSX2iOS/GamepadMapping", key, s_defaultMap[i]);
            s_buttonMap[i] = val;
        }
    }
    {
        auto lock = Host::GetSettingsLock(); // not recursive: no Host accessor in this scope
        ARMSX2RepairIOSARM64JITSettings(s_settings_interface, "scene-connect");
        ARMSX2MigrateJITScriptProtocolForIOS(s_settings_interface, "scene-connect");
    }
    ARMSX2MigratePerGameDeinterlaceBlend(s_settings_interface);
    // One-time migration for existing INI (runs once, then conditions are false)
    if (!Host::ContainsBaseSettingValue("SPU2/Output", "Backend")) {
        Host::SetBaseStringSettingValue("SPU2/Output", "Backend", "SDL");
    }
    if (!Host::ContainsBaseSettingValue("EmuCore/CPU", "ExtraMemory")) {
        Host::SetBaseBoolSettingValue("EmuCore/CPU", "ExtraMemory", false);
    }
    if (!Host::ContainsBaseSettingValue("EmuCore/CPU/Recompiler", "EnableFastmem")) {
        Host::SetBaseBoolSettingValue("EmuCore/CPU/Recompiler", "EnableFastmem", true);
    }
    if (!Host::ContainsBaseSettingValue("Achievements", "Enabled")) {
        Host::SetBaseBoolSettingValue("Achievements", "Enabled", false);
    }
    if (!Host::ContainsBaseSettingValue("Achievements", "ChallengeMode")) {
        Host::SetBaseBoolSettingValue("Achievements", "ChallengeMode", false);
    }
    if (!ARMSX2IOSRetroAchievementsHardcoreAvailable) {
        auto lock = Host::GetSettingsLock();
        ARMSX2DisableRetroAchievementsHardcoreForIOS(s_settings_interface, "scene-connect");
    }
    if (!Host::ContainsBaseSettingValue("ARMSX2iOS/Gamepad", "MultitapMode")) {
        Host::SetBaseIntSettingValue("ARMSX2iOS/Gamepad", "MultitapMode", 0);
    }
    {
        auto lock = Host::GetSettingsLock();
        ARMSX2IOSSanitizeFolderSettings(s_settings_interface, dataRoot, "scene-connect");
        ARMSX2EnsureIOSSpeedhackDefaults(s_settings_interface, "scene-connect");
    }
    ARMSX2SanitizeFrameLimiterConfig("scene-connect");
    ARMSX2ApplyIOSMultitapConfig("scene-connect");
    Host::CommitBaseSettingChanges();
    [self checkAndConfigureBIOS];

    EmuConfig.GS.Renderer = GSRendererType::Metal;
    Host::CommitBaseSettingChanges();

    VMManager::Internal::LoadStartupSettings();
    if (!ARMSX2IOSRetroAchievementsHardcoreAvailable) {
        EmuConfig.Achievements.HardcoreMode = false;
    }
    ARMSX2SanitizeFrameLimiterConfig("after-startup-settings");
    ARMSX2ApplyIOSOsdPresetFromConfig("after-startup-settings");
    {
        auto lock = Host::GetSettingsLock();
        ARMSX2IOSApplyRetroAchievementsOverlayDefaults(s_settings_interface, "after-startup-settings");
    }
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { VMManager::ApplySettings(); }, false);
}

#pragma mark - Deep-link handling
- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts {
    for (UIOpenURLContext *context in URLContexts) {
        NSURL *url = context.URL;
        [DeepLinkBridge handle:url];
    }
}

#pragma mark - BIOS discovery
- (void)checkAndConfigureBIOS {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *docs = [ARMSX2Bridge documentsDirectory];
    NSString *biosDir = [docs stringByAppendingPathComponent:@"bios"];
    [fm createDirectoryAtPath:biosDir withIntermediateDirectories:YES attributes:nil error:nil];

    // 0. Check Env Var Override (ARMSX2_BIOS_PATH)
    const char* envBios = getenv("ARMSX2_BIOS_PATH");
    if (envBios && envBios[0]) {
        NSString *envPath = [NSString stringWithUTF8String:envBios];
        BOOL exists = [fm fileExistsAtPath:envPath];
        Console.WriteLn("@@BIOS_ENV@@ exists=%d", exists ? 1 : 0);
        if (exists) {
            NSString *fileName = envPath.lastPathComponent;
            NSString *destPath = [biosDir stringByAppendingPathComponent:fileName];
            if (![envPath isEqualToString:destPath]) {
                [fm removeItemAtPath:destPath error:nil];
                [fm copyItemAtPath:envPath toPath:destPath error:nil];
            }
            [ARMSX2Bridge setDefaultBIOS:fileName];
            Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=env", fileName.UTF8String);
            return;
        }
    }

    // 1. Check existing config
    NSString *currentBios = [ARMSX2Bridge defaultBIOSName];
    if (currentBios.length > 0 && [fm fileExistsAtPath:[biosDir stringByAppendingPathComponent:currentBios]]) {
        Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=config", currentBios.UTF8String);
        return;
    }

    // 1b. Auto-move BIOS files from Documents/ root to bios/ subfolder.
    // availableISOs treats a .bin over 50 MB as a game image, so the size range
    // keeps a game left in Documents from being moved out of the library.
    for (NSString *file in [fm contentsOfDirectoryAtPath:docs error:nil]) {
        NSString *ext = file.pathExtension.lowercaseString;
        if ([ext isEqualToString:@"bin"] || [ext isEqualToString:@"rom"]) {
            NSString *src = [docs stringByAppendingPathComponent:file];
            const unsigned long long size = [[fm attributesOfItemAtPath:src error:nil] fileSize];
            if (size < 1024 * 1024 || size > 50 * 1024 * 1024)
                continue;
            NSString *dst = [biosDir stringByAppendingPathComponent:file];
            if (![fm fileExistsAtPath:dst]) {
                [fm moveItemAtPath:src toPath:dst error:nil];
            }
        }
    }

    // 2. Scan available BIOSes
    NSArray<ARMSX2BIOSInfo *> *bioses = [ARMSX2Bridge availableBIOSInfos];
    // availableBIOSInfos sorts valid entries first, and also lists companion
    // ROMs like ROM1.BIN, which cannot be booted.
    if (bioses.firstObject.valid) {
        NSString *picked = bioses.firstObject.fileName;
        [ARMSX2Bridge setDefaultBIOS:picked];
        Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=scan", picked.UTF8String);
        return;
    }
    Console.WriteLn("@@BIOS_SCAN@@ found=%d", (int)bioses.count);
    Console.WriteLn("@@BIOS_PICK@@ result=\"(none)\" source=none");

    // 3. Check Bundle Resources (Fallback)
    NSString *bundleBios = [[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:@"BiosFiles"];
    NSArray<NSString *> *bundleFiles = [fm contentsOfDirectoryAtPath:bundleBios error:nil];
    for (NSString *file in bundleFiles) {
        if ([file.pathExtension caseInsensitiveCompare:@"bin"] == NSOrderedSame) {
            NSString *src = [bundleBios stringByAppendingPathComponent:file];
            NSString *dst = [biosDir stringByAppendingPathComponent:file];
            [fm removeItemAtPath:dst error:nil];
            if ([fm copyItemAtPath:src toPath:dst error:nil]) {
                [ARMSX2Bridge setDefaultBIOS:file];
                return;
            }
        }
    }

    Console.Warning("No BIOS found automatically.");
    EmuConfig.BaseFilenames.Bios.clear();
}

#pragma mark - Scene lifecycle
- (void)sceneDidDisconnect:(UIScene *)scene {
}

/// A host container connects the scene while the app is still backgrounded, where
/// UIKit has no interface orientation to resolve, so the window takes the real
/// landscape bounds while its root view controller starts portrait. The menu is
/// pinned to that view and inherits the mismatch. No-op when the two agree.
- (void)syncRootViewToWindow {
    UIViewController *rootVC = s_rootVC ?: self.window.rootViewController;
    UIWindow *win = self.window;
    if (rootVC == nil || win == nil) return;
    if (CGSizeEqualToSize(rootVC.view.bounds.size, win.bounds.size)) return;

    rootVC.view.frame = win.bounds;
    [rootVC.view setNeedsLayout];
    [rootVC.view layoutIfNeeded];
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    [self syncRootViewToWindow];

    // Prepare the persistent CPU/JIT worker while the launch-time JIT grant is
    // fresh, but leave it waiting without a VM boot request. Running this from
    // scene activation also retries after returning from a JIT-enabler app.
#if !TARGET_OS_SIMULATOR
    [self prepareVMThreadForIdle];
#endif
}

- (void)sceneWillResignActive:(UIScene *)scene {
// NVM save when app loses focus
    if (s_vmThreadActive.load(std::memory_order_relaxed) && !BiosPath.empty()) {
        cdvdSaveNVRAM();
        Console.WriteLn("[NVM] NVM saved on sceneWillResignActive");
    }
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
    // Runs before the first frame of the return, unlike sceneDidBecomeActive.
    [self syncRootViewToWindow];
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
    // cdvdSaveNVRAM otherwise only runs at VM shutdown, which never comes when iOS kills the
    // app, so BIOS settings (language, date) would be lost on every restart.
    if (s_vmThreadActive.load(std::memory_order_relaxed) && !BiosPath.empty()) {
        cdvdSaveNVRAM();
        Console.WriteLn("[NVM] NVM saved on sceneDidEnterBackground");
    }
    // A folder card writes its last sectors two frames after the game stops writing, and a game in
    // the background gets no more frames, so a kill there would lose the end of an in-game save.
    if (VMManager::HasValidVM()) {
        Host::RunOnCPUThread([]() {
            if (!VMManager::HasValidVM())
                return;
            FileMcd_Flush();
            Console.WriteLn("[MCD] Memory cards flushed on sceneDidEnterBackground");
        }, false);
    }
}

@end
