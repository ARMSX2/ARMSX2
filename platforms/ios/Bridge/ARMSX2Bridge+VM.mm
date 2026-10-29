// ARMSX2Bridge+VM.mm — render view, VM control, OSD, speed and DEV9
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/PerformanceMetrics.h"
#include "GS/GS.h"
#include "common/Darwin/DarwinMisc.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Error.h"

#import "ARMSX2GameView.h"
#include "IOSRuntime.h"

#include <algorithm>
#include <cmath>
#include <ifaddrs.h>
#include <net/if.h>

@implementation ARMSX2Bridge (VM)

+ (UIView *)gameRenderView {
    return g_gameRenderView;
}

+ (void)prepareGameRenderViewForCurrentRenderer {
    ARMSX2_PrepareGameRenderViewForCurrentRenderer("swift_preboot");
}

+ (void)requestVMStop {
    s_requestVMStop.store(true);
    NSLog(@"[ARMSX2Bridge] VM stop requested");
}

+ (void)setVMPaused:(BOOL)paused {
    if (!VMManager::HasValidVM())
        return;

    Host::RunOnCPUThread([paused]() {
        if (!VMManager::HasValidVM())
            return;

        const VMState state = VMManager::GetState();
        if (paused && state == VMState::Running) {
            VMManager::SetPaused(true);
            Console.WriteLn("@@IOS_VM_PAUSE@@ paused=1 reason=swiftui-menu");
        } else if (!paused && state == VMState::Paused) {
            VMManager::SetPaused(false);
            Console.WriteLn("@@IOS_VM_PAUSE@@ paused=0 reason=swiftui-menu");
        }
    }, false);
}

+ (void)setFullScreen:(BOOL)enabled {
    ARMSX2_SetSDLFullscreen(enabled ? true : false);
}

+ (BOOL)isSDLFullscreen {
    return ARMSX2_IsSDLFullscreen() ? YES : NO;
}

+ (nonnull NSString *)buildVersion {
    NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    return [NSString stringWithFormat:@"ARMSX2 iOS v%@", ver];
}

+ (BOOL)isJITAvailable {
    return DarwinMisc::IsJITAvailable();
}

+ (BOOL)isNoJITFallbackActive {
    return DarwinMisc::iPSX2_FORCE_EE_INTERP != 0;
}

+ (BOOL)isIdleVMPrewarmResolved {
    return ARMSX2_IsIdleVMPrewarmResolved() ? YES : NO;
}

// Toggle overlay visibility via position (None vs TopRight).
// Individual OSD flags are controlled by preset in SettingsStore, not here.
+ (void)setPerformanceOverlayVisible:(BOOL)visible {
    // Hidden in the config means the user never picked a corner, so give them one.
    OsdOverlayPos pos = OsdOverlayPos::None;
    if (visible) {
        pos = EmuConfig.GS.OsdPerformancePos;
        if (pos == OsdOverlayPos::None)
            pos = OsdOverlayPos::TopRight;
    }

    if (ARMSX2HasBaseSettings()) {
        Host::SetBaseIntSettingValue("EmuCore/GS", "OsdPerformancePos", static_cast<int>(pos));
        Host::CommitBaseSettingChanges();
    }

    // GSConfig is the GS thread's and EmuConfig the CPU thread's, both bitfields, so a UI write
    // can drop a neighbouring flag, even one deciding whether GSUpdateConfig tears the device
    // down. Write EmuConfig on its thread and push, since ImGuiOverlays does not re-copy this one.
    Host::RunOnCPUThread([pos]() {
        EmuConfig.GS.OsdPerformancePos = pos;
        if (MTGS::IsOpen())
            MTGS::ApplySettings();
    });
}

+ (nonnull NSDictionary<NSString *, id> *)deviceStatsForAccessibility {
    int battery = -1, severity = 0;
    double ramGB = 0.0;
    bool lowPower = false;
    ARMSX2_iOSCopyDeviceStats(&battery, &severity, &ramGB, &lowPower);
    NSString* thermal;
    switch (severity) {
        case 2:  thermal = @"Serious"; break;
        case 1:  thermal = @"Fair"; break;
        default: thermal = @"Nominal"; break;
    }
    return @{
        @"battery": @(battery),
        @"thermalState": thermal,
        @"ramGB": @(ramGB),
        @"lowPower": @(lowPower),
    };
}

+ (void)releaseNonEmulationResources:(NSUInteger)releaseFlags {
    if (releaseFlags & VMManager::EMULATION_ONLY_RELEASE_ACHIEVEMENTS) {
        void (^clearPendingNotification)(void) = ^{
            s_pendingRetroAchievementsNotification = nil;
        };
        if ([NSThread isMainThread])
            clearPendingNotification();
        else
            dispatch_async(dispatch_get_main_queue(), clearPendingNotification);
    }

    Host::RunOnCPUThread([releaseFlags]() {
        if (!VMManager::HasValidVM())
            return;

        VMManager::ReleaseNonEssentialRuntimeResources(static_cast<u32>(releaseFlags));
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter]
                postNotificationName:@"ARMSX2iOSEmulationOnlyResourcesReleased"
                object:nil];
        });
    }, false);
}

+ (BOOL)isEmulationOnlyModeActive {
    return VMManager::IsEmulationOnlyMode();
}

// Apply OSD preset — sets ALL GSConfig flags to match the preset
+ (void)applyOsdPreset:(int)preset {
    // 1 simple: clean player readout, plus the device stats line the overlay draws.
    // 2 detail: performance and renderer diagnostics.
    // 3 full: closest to Android's full stats section. 0 is off.
    const bool simple = (preset == 1);
    const bool detail = (preset == 2);
    const bool full = (preset == 3);

    const bool fps = simple || detail || full;
    const bool vps = detail || full;
    const bool speed = simple || detail || full;
    const bool cpu = simple || detail || full;
    const bool gpu = detail || full;
    const bool resolution = detail || full;
    const bool indicators = detail || full;
    const bool version = simple || detail || full;
    const bool gsStats = full;
    const bool frameTimes = full;
    const bool hardwareInfo = full;
    const bool settings = full;
    const bool inputs = full;

    // Same bitfield ownership problem as setPerformanceOverlayVisible, so only the CPU thread's
    // copy is written. No push, deliberately: ImGuiOverlays re-copies all of these from
    // EmuConfig.GS each frame, so a push would only add a queue drain per preset tap.
    Host::RunOnCPUThread([=]() {
        EmuConfig.GS.OsdShowFPS = fps;
        EmuConfig.GS.OsdShowVPS = vps;
        EmuConfig.GS.OsdShowSpeed = speed;
        EmuConfig.GS.OsdShowCPU = cpu;
        EmuConfig.GS.OsdShowGPU = gpu;
        EmuConfig.GS.OsdShowResolution = resolution;
        EmuConfig.GS.OsdShowGSStats = gsStats;
        EmuConfig.GS.OsdShowFrameTimes = frameTimes;
        EmuConfig.GS.OsdShowVersion = version;
        EmuConfig.GS.OsdShowHardwareInfo = hardwareInfo;
        EmuConfig.GS.OsdShowIndicators = indicators;
        EmuConfig.GS.OsdShowSettings = settings;
        EmuConfig.GS.OsdShowInputs = inputs;
        EmuConfig.GS.OsdShowInputRec = false;
    });
}

#pragma mark - Frame-time history

// Returns the 150-sample PerformanceMetrics frame-time history as contiguous float data.
+ (nonnull NSData *)frameTimeHistory {
    const PerformanceMetrics::FrameTimeHistory& history = PerformanceMetrics::GetFrameTimeHistory();
    return [NSData dataWithBytes:history.data() length:sizeof(history)];
}

// Current write cursor inside the ring buffer, so callers can read the most
// recent N samples (those just before the cursor) rather than treating the
// array as a linear window.
+ (NSUInteger)frameTimeHistoryPos {
    return (NSUInteger)PerformanceMetrics::GetFrameTimeHistoryPos();
}

+ (int)limiterMode
{
    if (!VMManager::HasValidVM())
        return static_cast<int>(LimiterModeType::Nominal);

    return static_cast<int>(VMManager::GetLimiterMode());
}

+ (void)setRuntimeFastForwardEnabled:(BOOL)enabled speedPercent:(int)percent
{
    const int normalizedPercent = std::clamp(percent, 125, 1000);
    const float turboScalar = static_cast<float>(normalizedPercent) / 100.0f;
    if (!VMManager::HasValidVM())
        return;

    // Apply the scalar and limiter as one CPU-thread transaction. Separate INI
    // and limiter updates can otherwise briefly restore the previous mode.
    Host::RunOnCPUThread([enabled, turboScalar]() {
        if (!VMManager::HasValidVM())
            return;

        EmuConfig.EmulationSpeed.TurboScalar = turboScalar;
        if (!enabled)
            EmuConfig.EmulationSpeed.NominalScalar = 1.0f;
        VMManager::SetLimiterMode(
            enabled ? LimiterModeType::Turbo : LimiterModeType::Nominal);
        const LimiterModeType appliedMode = VMManager::GetLimiterMode();
        GSSetPresentCapSuspended(
            appliedMode == LimiterModeType::Turbo && GSGetMaxPresentInterval() != 0);
        VMManager::UpdateTargetSpeed();
    }, false);
}

+ (void)setRuntimeEmulationSpeedPercent:(int)percent
{
    int normalizedPercent = std::clamp(percent, 25, 1000);
    if (ARMSX2RetroAchievementsHardcoreActive() && normalizedPercent < 100) {
        ARMSX2LogRetroAchievementsHardcoreBlock("slowdown_runtime_shortcut");
        normalizedPercent = 100;
    }
    const float scalar = static_cast<float>(normalizedPercent) / 100.0f;

    // Session-only: the persisted frame-pacing configuration is left alone.
    Host::RunOnCPUThread([scalar]() {
        if (!VMManager::HasValidVM())
            return;
        EmuConfig.EmulationSpeed.NominalScalar = scalar;
        VMManager::UpdateTargetSpeed();
    }, false);
}

static void ARMSX2SetPresentFPSCapValue(double fps)
{
    const double requestedFPS = std::isfinite(fps) ? std::clamp(static_cast<double>(fps), 0.0, 1000.0) : 0.0;
    const u32 requestedMilliFPS = requestedFPS > 0.0 ?
        static_cast<u32>(std::llround(requestedFPS * 1000.0)) : 0u;

    // 60 FPS and up take the native path: a zero interval keeps master's original VM pacing,
    // rendering and submission. Fractional rates below 60, such as 59.970, stay explicit caps.
    const bool customCapActive = requestedMilliFPS != 0 && requestedMilliFPS < 60000;
    const u32 displayFPS = customCapActive ? static_cast<u32>(std::lround(requestedFPS)) : 0u;
    const u32 milliFPS = customCapActive ? requestedMilliFPS : 0u;
    const u64 interval = customCapActive
        ? static_cast<u64>(std::llround(static_cast<double>(GetTickFrequency()) / requestedFPS))
        : 0u;

    GSSetMaxPresentFps(displayFPS, interval, milliFPS);
    GSSetPresentCapRenderSkip(customCapActive);
    GSSetPresentCapSuspended(customCapActive && VMManager::HasValidVM() &&
        VMManager::GetLimiterMode() == LimiterModeType::Turbo);
}

// Older iOS per-game profiles kept their presentation target in Framerate/NominalScalar.
// Convert the active profile once so it no longer slows CPU and audio timing but keeps the
// display cadence. Runs on the CPU thread before the effective cap is read.
static bool ARMSX2MigrateLegacyPerGamePresentFPSCap()
{
    if (!VMManager::HasValidVM())
        return false;

    bool migrated = false;
    {
        auto lock = Host::GetSettingsLock();
        SettingsInterface* const game_layer = Host::Internal::GetGameSettingsLayer();
        if (!game_layer ||
            !game_layer->ContainsValue("Framerate", "NominalScalar") ||
            game_layer->ContainsValue("ARMSX2iOS/FramePacing", "TargetFPS"))
        {
            return false;
        }

        const float scalar = game_layer->GetFloatValue("Framerate", "NominalScalar", 1.0f);
        if (!std::isfinite(scalar) || scalar >= 5.0f || std::abs(scalar - 1.0f) < 0.002f)
            return false;

        SettingsInterface* const layered = Host::GetSettingsInterface();
        const float base_fps = layered ?
            layered->GetFloatValue("EmuCore/GS", "FramerateNTSC", 59.94f) : 59.94f;
        const float target_fps = std::clamp(scalar * std::max(base_fps, 1.0f), 15.0f, 120.0f);

        game_layer->SetFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", target_fps);
        game_layer->SetFloatValue("Framerate", "NominalScalar", 1.0f);
        Error error;
        if (!game_layer->Save(&error))
        {
            Console.Error("Failed to migrate per-game presentation FPS cap: %s", error.GetDescription().c_str());
            return false;
        }
        migrated = true;
    }

    if (migrated)
        VMManager::ReloadGameSettings();
    return migrated;
}

extern "C" void ARMSX2_ApplyEffectivePresentFPSCap(void)
{
    ARMSX2MigrateLegacyPerGamePresentFPSCap();

    float nominalScalar = 1.0f;
    float targetFPS = 60.0f;
    {
        auto lock = Host::GetSettingsLock();
        SettingsInterface* const si = Host::GetSettingsInterface();
        if (si)
        {
            nominalScalar = si->GetFloatValue("Framerate", "NominalScalar", 1.0f);
            targetFPS = si->GetFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", 60.0f);
        }
    }

    const bool limiterEnabled = !std::isfinite(nominalScalar) || nominalScalar < 5.0f;
    ARMSX2SetPresentFPSCapValue(limiterEnabled ? (std::isfinite(targetFPS) ? targetFPS : 60.0f) : 0.0f);
    if (!VMManager::HasValidVM())
        GSSetPresentCapSuspended(false);
}

+ (void)setPresentFPSCap:(float)fps
{
    // With a running VM the layered settings interface is authoritative: a
    // per-game TargetFPS must continue to win when the global value changes.
    if (VMManager::HasValidVM())
        Host::RunOnCPUThread([]() { ARMSX2_ApplyEffectivePresentFPSCap(); }, false);
    else
        ARMSX2SetPresentFPSCapValue(fps);
}

#pragma mark - VM lifecycle

+ (BOOL)isVMRunning {
    VMState st = VMManager::GetState();
    return st == VMState::Running || st == VMState::Paused;
}

+ (BOOL)hasBIOS {
    if (EmuConfig.BaseFilenames.Bios.empty()) return NO;
    std::string fullPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
    return FileSystem::FileExists(fullPath.c_str());
}

+ (void)requestVMBoot {
    [self requestVMBootLoadingLastSaveState:YES];
}

+ (void)requestVMBootLoadingLastSaveState:(BOOL)loadLastSaveState {
	std::string bootISO;
	if (ARMSX2HasBaseSettings())
		bootISO = Host::GetBaseStringSettingValue("GameISO", "BootISO", "");
	const std::string biosPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
	std::fprintf(stderr, "@@BOOT_REQUEST@@ posted=1 has_bios=%d bios=\"%s\" boot_iso=\"%s\"\n",
		(!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(biosPath.c_str())) ? 1 : 0,
		EmuConfig.BaseFilenames.Bios.c_str(), bootISO.c_str());
	std::fflush(stderr);
    [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMBoot"
        object:nil userInfo:@{@"loadLastSaveState": @(loadLastSaveState)}];
}

#pragma mark - DEV9 / Network

+ (nonnull NSArray<NSString *> *)dev9NetworkAdapters {
    NSMutableOrderedSet<NSString *> *adapters = [NSMutableOrderedSet orderedSetWithObject:@"Auto"];

    struct ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs *ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr)
                continue;

            const sa_family_t family = ifa->ifa_addr->sa_family;
            if (family != AF_INET)
                continue;

            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
                continue;

            [adapters addObject:[NSString stringWithUTF8String:ifa->ifa_name]];
        }
        freeifaddrs(interfaces);
    }

    return adapters.array;
}

@end
