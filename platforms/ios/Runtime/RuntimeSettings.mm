// RuntimeSettings.mm — repair, migrate and sanitize helpers for the base settings
// SPDX-License-Identifier: GPL-3.0+

#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Darwin/DarwinMisc.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"          // EmuConfig, GSConfig
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/SIO/Memcard/MemoryCardFile.h"
#include "pcsx2/VMManager.h"

#include "IOSRuntime.h"

bool ARMSX2ShouldEnableMTVUByDefault(u32* physical_cores)
{
    u32 total_physical_cores = 0;
    for (const DarwinMisc::CPUClass& cpu_class : DarwinMisc::GetCPUClasses())
        total_physical_cores += cpu_class.num_physical;

    if (physical_cores)
        *physical_cores = total_physical_cores;

    return total_physical_cores >= 3;
}

void ARMSX2EnsureIOSSpeedhackDefaults(SettingsInterface* si, const char* reason)
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
        std::fflush(stderr);
        changed = true;
    }

    if (has_legacy_mtvu) {
        si->DeleteValue("EmuCore/Speedhacks", "MTVU");
        changed = true;
    }

    if (changed)
        si->Save();
}

bool ARMSX2RepairIOSARM64JITSettings(SettingsInterface* si, const char* reason)
{
    if (!si)
        return false;
    if (DarwinMisc::iPSX2_FORCE_EE_INTERP) {
        std::fprintf(stderr,
            "@@IOS_JIT_POLICY@@ reason=%s forced_interp=1 action=skip_recompiler_repair\n",
            reason ? reason : "unknown");
        std::fflush(stderr);
        return false;
    }

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

bool ARMSX2GetConfiguredFastBoot()
{
    if (!s_settings_interface)
        return false;

    return Host::GetBaseBoolSettingValue(
        "GameISO", "FastBoot",
        Host::GetBaseBoolSettingValue("EmuCore", "EnableFastBoot", false));
}

// Resolves fast boot for an ISO that is about to boot. A per-game EmuCore/EnableFastBoot in the
// game's settings INI takes precedence, otherwise the configured global value is used. Global
// settings are never mutated, so a per-game override cannot leak into the global configuration.
bool ARMSX2ResolveFastBootForISO(const std::string& isoPath)
{
    const bool globalFastBoot = ARMSX2GetConfiguredFastBoot();
    if (isoPath.empty())
        return globalFastBoot;

    GameList::Entry entry;
    if (!GameList::PopulateEntryFromPath(isoPath, &entry) || entry.crc == 0)
        return globalFastBoot;

    const std::string serial = (entry.type == GameList::EntryType::ELF) ? std::string() : entry.serial;
    INISettingsInterface si(VMManager::GetGameSettingsPath(serial, entry.crc));
    if (!si.Load())
        return globalFastBoot;

    if (si.ContainsValue("EmuCore", "EnableFastBoot"))
        return si.GetBoolValue("EmuCore", "EnableFastBoot", globalFastBoot);

    return globalFastBoot;
}

int ARMSX2GetIOSMajorVersion()
{
    NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    return static_cast<int>(version.majorVersion);
}

const char* ARMSX2DefaultJITScriptProtocol()
{
    return ARMSX2GetIOSMajorVersion() >= 26 ? "universal" : "legacy";
}

std::string ARMSX2NormalizeJITScriptProtocol(std::string jitProtocol)
{
    std::transform(jitProtocol.begin(), jitProtocol.end(), jitProtocol.begin(), ::tolower);
    if (jitProtocol == "utm-dolphin" || jitProtocol == "utm_dolphin")
        return "legacy";
    if (jitProtocol == "legacy" || jitProtocol == "universal")
        return jitProtocol;
    return {};
}

void ARMSX2MigrateJITScriptProtocolForIOS(SettingsInterface* si, const char* reason)
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

std::string ARMSX2ResolveJITScriptProtocol()
{
    std::string jitProtocol = ARMSX2DefaultJITScriptProtocol();
    if (s_settings_interface)
    {
        jitProtocol = ARMSX2NormalizeJITScriptProtocol(
            Host::GetBaseStringSettingValue("ARMSX2iOS/JIT", "ScriptProtocol", ARMSX2DefaultJITScriptProtocol()));
        if (jitProtocol != "legacy" && jitProtocol != "universal")
            jitProtocol = Host::GetBaseBoolSettingValue(
                "ARMSX2iOS/JIT", "UseUniversalJITScript", ARMSX2GetIOSMajorVersion() >= 26) ? "universal" : "legacy";
    }
    return jitProtocol;
}

void ARMSX2ApplyJITScriptProtocol(const char* reason)
{
    const std::string jitProtocol = ARMSX2ResolveJITScriptProtocol();
    setenv("ARMSX2_JIT_PROTOCOL", jitProtocol.c_str(), 1);
    std::fprintf(stderr, "@@JIT_PROTOCOL_SELECTED@@ reason=%s ios_major=%d protocol=%s\n",
        reason ? reason : "unknown", ARMSX2GetIOSMajorVersion(), jitProtocol.c_str());
    std::fflush(stderr);
}

void ARMSX2DisableRetroAchievementsHardcoreForIOS(SettingsInterface* si, const char* reason)
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

void ARMSX2IOSApplyRetroAchievementsOverlayDefaults(SettingsInterface* si, const char* reason)
{
    if (!si)
        return;

    // iOS overlays share the screen with touch controls and the perf OSD. Keep
    // gameplay trackers left, but show achievement notifications at top-center.
    si->SetBoolValue("Achievements", "Overlays", true);
    si->SetBoolValue("Achievements", "LBOverlays", true);
    si->SetBoolValue("Achievements", "Notifications", true);
    si->SetBoolValue("Achievements", "LeaderboardNotifications", true);
    si->SetIntValue("Achievements", "OverlayPosition", static_cast<int>(AchievementOverlayPosition::TopLeft));
    si->SetIntValue("Achievements", "NotificationPosition", static_cast<int>(OsdOverlayPos::TopCenter));

    EmuConfig.Achievements.Overlays = true;
    EmuConfig.Achievements.LBOverlays = true;
    EmuConfig.Achievements.Notifications = true;
    EmuConfig.Achievements.LeaderboardNotifications = true;
    EmuConfig.Achievements.OverlayPosition = AchievementOverlayPosition::TopLeft;
    EmuConfig.Achievements.NotificationPosition = OsdOverlayPos::TopCenter;
}

bool ARMSX2IOSPathIsInsideRoot(const std::string& path, const std::string& root)
{
    return path == root || path.starts_with(root + "/");
}

bool ARMSX2IOSPathContainsContainerFragment(const std::string& path)
{
    return path.find("Data/Application/") != std::string::npos;
}

std::string ARMSX2IOSResolveFolderPath(const std::string& root, const std::string& value)
{
    return Path::IsAbsolute(value) ? value : Path::Combine(root, value);
}

NSInteger ARMSX2IOSCopyDirectoryContentsIfPresent(const std::string& source, const std::string& destination)
{
    if (source.empty() || destination.empty() || source == destination)
        return 0;

    @autoreleasepool {
        NSFileManager* fm = [NSFileManager defaultManager];
        NSString* sourcePath = [NSString stringWithUTF8String:source.c_str()];
        NSString* destinationPath = [NSString stringWithUTF8String:destination.c_str()];
        if (!sourcePath || !destinationPath)
            return 0;

        BOOL sourceIsDirectory = NO;
        if (![fm fileExistsAtPath:sourcePath isDirectory:&sourceIsDirectory] || !sourceIsDirectory)
            return 0;

        NSError* createError = nil;
        [fm createDirectoryAtPath:destinationPath withIntermediateDirectories:YES attributes:nil error:&createError];
        if (createError)
            NSLog(@"[ARMSX2 iOS FolderSanitize] destination create failed: %@ -> %@ error=%@",
                  sourcePath, destinationPath, createError.localizedDescription ?: @"unknown");

        NSInteger copied = 0;
        NSDirectoryEnumerator<NSString*>* enumerator = [fm enumeratorAtPath:sourcePath];
        for (NSString* relativePath in enumerator) {
            NSString* itemSource = [sourcePath stringByAppendingPathComponent:relativePath];
            NSString* itemDestination = [destinationPath stringByAppendingPathComponent:relativePath];

            BOOL itemIsDirectory = NO;
            if (![fm fileExistsAtPath:itemSource isDirectory:&itemIsDirectory])
                continue;

            if (itemIsDirectory) {
                [fm createDirectoryAtPath:itemDestination withIntermediateDirectories:YES attributes:nil error:nil];
                continue;
            }

            if ([fm fileExistsAtPath:itemDestination])
                continue;

            [fm createDirectoryAtPath:itemDestination.stringByDeletingLastPathComponent
          withIntermediateDirectories:YES
                           attributes:nil
                                error:nil];

            NSError* copyError = nil;
            if ([fm copyItemAtPath:itemSource toPath:itemDestination error:&copyError]) {
                copied++;
            } else {
                NSLog(@"[ARMSX2 iOS FolderSanitize] copy failed: %@ -> %@ error=%@",
                      itemSource, itemDestination, copyError.localizedDescription ?: @"unknown");
            }
        }

        return copied;
    }
}

void ARMSX2IOSSanitizeFolderSettings(SettingsInterface* si, const std::string& dataRoot, const char* reason)
{
    if (!si)
        return;

    struct FolderDefault
    {
        const char* key;
        const char* value;
        bool settingsRoot;
    };

    static constexpr FolderDefault defaults[] = {
        {"Bios", "bios", false},
        {"Snapshots", "snaps", false},
        {"Savestates", "sstates", false},
        {"MemoryCards", "memcards", false},
        {"Logs", "logs", false},
        {"Cheats", "cheats", false},
        {"Patches", "patches", false},
        {"Covers", "covers", false},
        {"GameSettings", "gamesettings", false},
        {"UserResources", "resources", false},
        {"Cache", "cache", false},
        {"Textures", "textures", false},
        {"InputProfiles", "inputprofiles", false},
        {"DebuggerLayouts", "debuggerlayouts", true},
        {"DebuggerSettings", "debuggersettings", true},
    };

    const std::string settingsRoot = Path::Combine(dataRoot, "inis");
    bool changed = false;

    for (const FolderDefault& entry : defaults) {
        std::string rawValue;
        if (!si->GetStringValue("Folders", entry.key, &rawValue) || rawValue.empty())
            continue;

        const std::string& root = entry.settingsRoot ? settingsRoot : dataRoot;
        const std::string resolved = ARMSX2IOSResolveFolderPath(root, rawValue);
        const bool rawAbsolute = Path::IsAbsolute(rawValue);
        const bool containerPath = ARMSX2IOSPathContainsContainerFragment(rawValue);
        const bool stale = (!rawAbsolute && containerPath) ||
            (rawAbsolute && containerPath && !ARMSX2IOSPathIsInsideRoot(resolved, root));

        if (!stale)
            continue;

        const std::string migratedPath = Path::Combine(root, entry.value);
        const NSInteger copied = ARMSX2IOSCopyDirectoryContentsIfPresent(resolved, migratedPath);
        si->SetStringValue("Folders", entry.key, entry.value);
        changed = true;

        std::fprintf(stderr,
            "@@IOS_FOLDER_SANITIZE@@ reason=%s key=%s old=\"%s\" resolved=\"%s\" default=\"%s\" copied=%ld\n",
            reason ? reason : "unknown", entry.key, rawValue.c_str(), resolved.c_str(),
            entry.value, static_cast<long>(copied));
        std::fflush(stderr);
    }

    if (changed)
        si->Save();
}

void ARMSX2IOSLogMemoryCardConfig(const char* reason)
{
    std::fprintf(stderr, "@@IOS_MEMCARD_DIR@@ reason=%s path=\"%s\"\n",
        reason ? reason : "unknown", EmuFolders::MemoryCards.c_str());

    constexpr size_t numMemoryCardSlots = sizeof(EmuConfig.Mcd) / sizeof(EmuConfig.Mcd[0]);
    for (size_t slot = 0; slot < numMemoryCardSlots; slot++) {
        const auto& card = EmuConfig.Mcd[slot];
        const std::string path = card.Filename.empty() ? std::string() : EmuConfig.FullpathToMcd(static_cast<uint>(slot));
        const bool existsFile = !path.empty() && FileSystem::FileExists(path.c_str());
        const bool existsDirectory = !path.empty() && FileSystem::DirectoryExists(path.c_str());
        const s64 size = existsFile ? FileSystem::GetPathFileSize(path.c_str()) : -1;
        const bool formatted = existsFile ? FileMcd_IsMemoryCardFormatted(path) : false;

        std::fprintf(stderr,
            "@@IOS_MEMCARD_SLOT@@ reason=%s slot=%zu enabled=%d type=%d name=\"%s\" path=\"%s\" file=%d dir=%d size=%lld formatted=%d hardcore_pref=%d hardcore_active=%d\n",
            reason ? reason : "unknown",
            slot + 1,
            card.Enabled ? 1 : 0,
            static_cast<int>(card.Type),
            card.Filename.c_str(),
            path.c_str(),
            existsFile ? 1 : 0,
            existsDirectory ? 1 : 0,
            static_cast<long long>(size),
            formatted ? 1 : 0,
            EmuConfig.Achievements.HardcoreMode ? 1 : 0,
            Achievements::IsHardcoreModeActive() ? 1 : 0);
    }

    std::fflush(stderr);
}

float ARMSX2SanitizedNominalScalar(float scalar)
{
    if (!std::isfinite(scalar))
        return 1.0f;

    return std::clamp(scalar, 0.05f, 10.0f);
}

void ARMSX2SanitizeFrameLimiterConfig(const char* reason)
{
    if (!s_settings_interface)
        return;

    const float raw = Host::GetBaseFloatSettingValue("Framerate", "NominalScalar", 1.0f);
    const float sanitized = ARMSX2SanitizedNominalScalar(raw);
    if (std::fabs(raw - sanitized) > 0.001f) {
        Console.Warning("@@FRAMELIMIT@@ clamping unsupported NominalScalar %.3f -> %.3f reason=%s",
            raw, sanitized, reason ? reason : "unknown");
        Host::SetBaseFloatSettingValue("Framerate", "NominalScalar", sanitized);
        Host::CommitBaseSettingChanges();
    }

    EmuConfig.EmulationSpeed.NominalScalar = sanitized;
}

void ARMSX2SetIOSOsdFlags(bool show_fps, bool show_vps, bool show_speed, bool show_cpu,
    bool show_gpu, bool show_resolution, bool show_gs_stats, bool show_indicators,
    bool show_settings, bool show_inputs, bool show_frame_times, bool show_version,
    bool show_hardware_info)
{
    // EmuConfig only: GSConfig belongs to the GS thread, its bitfields share a word, and this
    // runs on the caller's thread (UIKit at scene connect), so a write there races the GS thread.
    // Nothing is lost: ImGuiOverlays copies EmuConfig.GS into GSConfig every frame before it draws.
    EmuConfig.GS.OsdShowFPS = show_fps;
    EmuConfig.GS.OsdShowVPS = show_vps;
    EmuConfig.GS.OsdShowSpeed = show_speed;
    EmuConfig.GS.OsdShowCPU = show_cpu;
    EmuConfig.GS.OsdShowGPU = show_gpu;
    EmuConfig.GS.OsdShowResolution = show_resolution;
    EmuConfig.GS.OsdShowGSStats = show_gs_stats;
    EmuConfig.GS.OsdShowIndicators = show_indicators;
    EmuConfig.GS.OsdShowSettings = show_settings;
    EmuConfig.GS.OsdShowInputs = show_inputs;
    EmuConfig.GS.OsdShowFrameTimes = show_frame_times;
    EmuConfig.GS.OsdShowVersion = show_version;
    EmuConfig.GS.OsdShowHardwareInfo = show_hardware_info;
    EmuConfig.GS.OsdShowInputRec = false;
}

void ARMSX2ApplyIOSOsdPresetFromConfig(const char* reason)
{
    if (!s_settings_interface)
        return;

    ARMSX2SetIOSOsdFlags(
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowFPS", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowVPS", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowSpeed", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowCPU", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowGPU", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowResolution", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowGSStats", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowIndicators", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowSettings", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowInputs", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowFrameTimes", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowVersion", false),
        Host::GetBaseBoolSettingValue("EmuCore/GS", "OsdShowHardwareInfo", false));

    int position = Host::GetBaseIntSettingValue("EmuCore/GS", "OsdPerformancePos", static_cast<int>(OsdOverlayPos::TopRight));
    if (position == static_cast<int>(OsdOverlayPos::TopCenter))
        position = static_cast<int>(OsdOverlayPos::TopRight);
    // Not mirrored per frame like the flags above, so it has to be pushed, but not from here: this
    // has no thread hop and one caller is the UIKit thread, where MTGS::RunOnGSThread asserts. The
    // only call site that runs after the GS opens pushes it there.
    EmuConfig.GS.OsdPerformancePos = static_cast<OsdOverlayPos>(position);
}
