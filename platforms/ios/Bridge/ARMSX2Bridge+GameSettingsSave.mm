// ARMSX2Bridge+GameSettingsSave.mm — per-game settings, write side
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Error.h"

#include "IOSRuntime.h"

#include <cstdio>
#include <limits>

static constexpr int ARMSX2UseGlobalIntSentinel = -1;
// "Use global" markers. Out of band for their ranges: upscale is positive, the int keys start
// at 0, AspectRatio uses an empty string.
static constexpr float ARMSX2UseGlobalFloatSentinel = -1.0f;
static constexpr int ARMSX2TriFilterUseGlobalSentinel = std::numeric_limits<int>::min();

// Older builds stamped deinterlace_mode 7 into every per-game file with overrides on. Nobody
// picked that deliberately, Blend BFF was offered as 6, so drop it and let Automatic apply again.
// Exact 7 only.
void ARMSX2MigratePerGameDeinterlaceBlend(SettingsInterface* si)
{
    // si is Host's base layer, kept for the null check; the flag goes through the locked accessors.
    if (!si || Host::GetBaseBoolSettingValue("ARMSX2iOS/Migrations", "PerGameDeinterlaceBlendV1", false))
        return;

    FileSystem::FindResultsArray files;
    FileSystem::FindFiles(EmuFolders::GameSettings.c_str(), "*.ini",
        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);

    u32 repaired = 0;
    for (const FILESYSTEM_FIND_DATA& fd : files) {
        INISettingsInterface game_si(fd.FileName);
        if (!game_si.Load())
            continue;
        if (!game_si.ContainsValue("EmuCore/GS", "deinterlace_mode") ||
            game_si.GetIntValue("EmuCore/GS", "deinterlace_mode", 0) != 7)
            continue;

        game_si.DeleteValue("EmuCore/GS", "deinterlace_mode");
        game_si.RemoveEmptySections();
        if (game_si.Save())
            repaired++;
    }

    Host::SetBaseBoolSettingValue("ARMSX2iOS/Migrations", "PerGameDeinterlaceBlendV1", true);
    Host::CommitBaseSettingChanges();
    std::fprintf(stderr, "@@IOS_DEINTERLACE_MIGRATION@@ scanned=%zu repaired=%u\n", files.size(), repaired);
    std::fflush(stderr);
}

static void ARMSX2WriteGameSettingsForIdentity(const std::string& serial,
                                                u32 crc,
                                                NSDictionary<NSString*, id>* s)
{
    BOOL enabled = [s[@"enabled"] boolValue];
    float upscaleMultiplier = s[@"upscaleMultiplier"] ? [s[@"upscaleMultiplier"] floatValue] : ARMSX2UseGlobalFloatSentinel;
    NSString* aspectRatio = [s[@"aspectRatio"] isKindOfClass:NSString.class] ? s[@"aspectRatio"] : @"";
    int textureFiltering = s[@"textureFiltering"] ? [s[@"textureFiltering"] intValue] : ARMSX2UseGlobalIntSentinel;
    int hardwareMipmapping = s[@"hardwareMipmapping"] ? [s[@"hardwareMipmapping"] intValue] : ARMSX2UseGlobalIntSentinel;
    int blendingAccuracy = s[@"blendingAccuracy"] ? [s[@"blendingAccuracy"] intValue] : ARMSX2UseGlobalIntSentinel;
    int interlaceMode = s[@"interlaceMode"] ? [s[@"interlaceMode"] intValue] : ARMSX2UseGlobalIntSentinel;
    int trilinearFiltering = s[@"trilinearFiltering"] ? [s[@"trilinearFiltering"] intValue] : ARMSX2TriFilterUseGlobalSentinel;
    int halfPixelOffset = s[@"halfPixelOffset"] ? [s[@"halfPixelOffset"] intValue] : ARMSX2UseGlobalIntSentinel;
    int roundSprite = s[@"roundSprite"] ? [s[@"roundSprite"] intValue] : ARMSX2UseGlobalIntSentinel;
    int alignSprite = s[@"alignSprite"] ? [s[@"alignSprite"] intValue] : ARMSX2UseGlobalIntSentinel;
    int mergeSprite = s[@"mergeSprite"] ? [s[@"mergeSprite"] intValue] : ARMSX2UseGlobalIntSentinel;
    int wildArmsOffset = s[@"wildArmsOffset"] ? [s[@"wildArmsOffset"] intValue] : ARMSX2UseGlobalIntSentinel;
    BOOL textureOffsetXOverride = [s[@"hasTextureOffsetXOverride"] boolValue];
    int textureOffsetX = [s[@"textureOffsetX"] intValue];
    BOOL textureOffsetYOverride = [s[@"hasTextureOffsetYOverride"] boolValue];
    int textureOffsetY = [s[@"textureOffsetY"] intValue];
    BOOL skipDrawStartOverride = [s[@"hasSkipDrawStartOverride"] boolValue];
    int skipDrawStart = [s[@"skipDrawStart"] intValue];
    BOOL skipDrawEndOverride = [s[@"hasSkipDrawEndOverride"] boolValue];
    int skipDrawEnd = [s[@"skipDrawEnd"] intValue];
    BOOL volumeOverride = [s[@"hasVolumeOverride"] boolValue];
    int volumePercent = s[@"volumePercent"] ? [s[@"volumePercent"] intValue] : ARMSX2DefaultAudioVolumePercent;
    int eeCoreType = s[@"eeCoreType"] ? [s[@"eeCoreType"] intValue] : 2;
    BOOL mtvu = s[@"mtvu"] ? [s[@"mtvu"] boolValue] : YES;
    BOOL eeCycleRateOverride = [s[@"hasEECycleRateOverride"] boolValue];
    int eeCycleRate = [s[@"eeCycleRate"] intValue];
    BOOL fastBootOverride = [s[@"hasFastBootOverride"] boolValue];
    BOOL fastBoot = [s[@"fastBoot"] boolValue];
    BOOL enableCheats = [s[@"enableCheats"] boolValue];
    BOOL enablePatches = s[@"enablePatches"] ? [s[@"enablePatches"] boolValue] : YES;
    BOOL enableGameFixes = s[@"enableGameFixes"] ? [s[@"enableGameFixes"] boolValue] : YES;
    BOOL enableGameDBHardwareFixes = s[@"enableGameDBHardwareFixes"] ? [s[@"enableGameDBHardwareFixes"] boolValue] : YES;

    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);
    if (enableCheats && (ARMSX2RetroAchievementsHardcoreActive() || EmuConfig.Achievements.HardcoreMode)) {
        ARMSX2LogRetroAchievementsHardcoreBlock("per_game_enable_cheats");
        enableCheats = NO;
    }

    const std::string settingsPath = VMManager::GetGameSettingsPath(serial, crc);
    INISettingsInterface si(settingsPath);
    si.Load();

    if (enabled) {
        si.SetBoolValue("ARMSX2iOS/PerGame", "Enabled", true);
        // Only write what was actually overridden. Copying the global in froze it against later
        // edits, and for deinterlace_mode invented a value the global INI never held.
        if (upscaleMultiplier <= ARMSX2UseGlobalFloatSentinel)
            si.DeleteValue("EmuCore/GS", "upscale_multiplier");
        else
            si.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscaleMultiplier);
        if (aspectRatio.length == 0)
            si.DeleteValue("EmuCore/GS", "AspectRatio");
        else
            si.SetStringValue("EmuCore/GS", "AspectRatio", aspectRatio.UTF8String);
        if (textureFiltering == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "filter");
        else
            si.SetIntValue("EmuCore/GS", "filter", textureFiltering);
        if (hardwareMipmapping == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "hw_mipmap");
        else
            si.SetBoolValue("EmuCore/GS", "hw_mipmap", hardwareMipmapping != 0);
        if (blendingAccuracy == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "accurate_blending_unit");
        else
            si.SetIntValue("EmuCore/GS", "accurate_blending_unit", blendingAccuracy);
        if (interlaceMode == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "deinterlace_mode");
        else
            si.SetIntValue("EmuCore/GS", "deinterlace_mode", interlaceMode);
        if (trilinearFiltering == ARMSX2TriFilterUseGlobalSentinel)
            si.DeleteValue("EmuCore/GS", "TriFilter");
        else
            si.SetIntValue("EmuCore/GS", "TriFilter", ARMSX2ClampInt(trilinearFiltering, -1, 2));

        if (halfPixelOffset == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_HalfPixelOffset");
        else
            si.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", ARMSX2ClampInt(halfPixelOffset, 0, 5));

        if (roundSprite == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_round_sprite_offset");
        else
            si.SetIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", ARMSX2ClampInt(roundSprite, 0, 2));

        if (alignSprite == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_align_sprite_X");
        else
            si.SetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", alignSprite != 0);

        if (mergeSprite == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_merge_pp_sprite");
        else
            si.SetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", mergeSprite != 0);

        if (wildArmsOffset == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition");
        else
            si.SetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", wildArmsOffset != 0);

        if (textureOffsetXOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_TCOffsetX", ARMSX2ClampInt(textureOffsetX, -4096, 4096));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetX");

        if (textureOffsetYOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_TCOffsetY", ARMSX2ClampInt(textureOffsetY, -4096, 4096));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetY");

        // Derived from the keys just written, so the GameDB stops writing them for
        // this game. The core folds the global claims back in at load.
        ARMSX2StoreDerivedPerGameHackClaims(si);

        if (skipDrawStartOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", ARMSX2ClampInt(skipDrawStart, 0, 5000));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_Start");

        if (skipDrawEndOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", ARMSX2ClampInt(skipDrawEnd, 0, 5000));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_End");

        // StandardVolume only. The audio tab owns FastForwardVolume and writes it later in the
        // same save, so touching it here just meant last writer won.
        if (volumeOverride)
            si.SetIntValue("SPU2/Output", "StandardVolume", ARMSX2ClampInt(volumePercent, 0, ARMSX2DefaultAudioVolumePercent));
        else
            si.DeleteValue("SPU2/Output", "StandardVolume");

        // No use-global marker on these, so compare against the global and write nothing when
        // they agree, same as vuThread below.
        const auto writeIfDifferent = [&si](const char* section, const char* key, bool value, bool global_value) {
            if (value == global_value)
                si.DeleteValue(section, key);
            else
                si.SetBoolValue(section, key, value);
        };
        const bool globalEnableCheats = ARMSX2HasBaseSettings() ?
            Host::GetBaseBoolSettingValue("EmuCore", "EnableCheats", false) : false;
        const bool globalEnablePatches = ARMSX2HasBaseSettings() ?
            Host::GetBaseBoolSettingValue("EmuCore", "EnablePatches", true) : true;
        const bool globalEnableGameFixes = ARMSX2HasBaseSettings() ?
            Host::GetBaseBoolSettingValue("EmuCore", "EnableGameFixes", true) : true;
        const bool globalUserHacks = ARMSX2HasBaseSettings() ?
            Host::GetBaseBoolSettingValue("EmuCore/GS", "UserHacks", false) : false;
        const int globalEECoreType = ARMSX2HasBaseSettings() ?
            Host::GetBaseIntSettingValue("EmuCore/CPU", "CoreType", 2) : 2;
        writeIfDifferent("EmuCore", "EnableCheats", enableCheats, globalEnableCheats);
        writeIfDifferent("EmuCore", "EnablePatches", enablePatches, globalEnablePatches);
        writeIfDifferent("EmuCore", "EnableGameFixes", enableGameFixes, globalEnableGameFixes);
        writeIfDifferent("EmuCore/GS", "UserHacks", !enableGameDBHardwareFixes, globalUserHacks);
        if (eeCoreType == globalEECoreType) {
            si.DeleteValue("EmuCore/CPU", "CoreType");
            si.DeleteValue("EmuCore/CPU", "UseArm64Dynarec");
        } else {
            si.SetIntValue("EmuCore/CPU", "CoreType", eeCoreType);
            si.SetBoolValue("EmuCore/CPU", "UseArm64Dynarec", eeCoreType == 2);
        }
        const bool globalMTVU = ARMSX2HasBaseSettings() ?
            Host::GetBaseBoolSettingValue("EmuCore/Speedhacks", "vuThread", true) : true;
        if (mtvu == globalMTVU) {
            si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVU");
            si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVUVersion");
            si.DeleteValue("EmuCore/Speedhacks", "vuThread");
        } else {
            si.SetBoolValue("ARMSX2iOS/PerGame", "ManualMTVU", true);
            si.SetIntValue("ARMSX2iOS/PerGame", "ManualMTVUVersion", 3);
            si.SetBoolValue("EmuCore/Speedhacks", "vuThread", mtvu);
        }

        if (eeCycleRateOverride) {
            int clampedEECycleRate = ARMSX2ClampInt(eeCycleRate, -3, 3);
            if (ARMSX2RetroAchievementsHardcoreActive() && clampedEECycleRate < 0) {
                ARMSX2LogRetroAchievementsHardcoreBlock("per_game_ee_underclock");
                clampedEECycleRate = 0;
            }
            si.SetIntValue("EmuCore/Speedhacks", "EECycleRate", clampedEECycleRate);
        } else {
            si.DeleteValue("EmuCore/Speedhacks", "EECycleRate");
        }

        if (fastBootOverride)
            si.SetBoolValue("EmuCore", "EnableFastBoot", fastBoot);
        else
            si.DeleteValue("EmuCore", "EnableFastBoot");
    } else {
        si.DeleteValue("ARMSX2iOS/PerGame", "Enabled");
        si.DeleteValue("EmuCore/GS", "upscale_multiplier");
        si.DeleteValue("EmuCore/GS", "AspectRatio");
        si.DeleteValue("EmuCore/GS", "filter");
        si.DeleteValue("EmuCore/GS", "hw_mipmap");
        si.DeleteValue("EmuCore/GS", "accurate_blending_unit");
        si.DeleteValue("EmuCore/GS", "deinterlace_mode");
        si.DeleteValue("EmuCore/GS", "TriFilter");
        si.DeleteValue("EmuCore/GS", "UserHacks_HalfPixelOffset");
        si.DeleteValue("EmuCore/GS", "UserHacks_round_sprite_offset");
        si.DeleteValue("EmuCore/GS", "UserHacks_align_sprite_X");
        si.DeleteValue("EmuCore/GS", "UserHacks_merge_pp_sprite");
        si.DeleteValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition");
        si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetX");
        si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetY");
        si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_Start");
        si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_End");
        si.DeleteValue("EmuCore", "EnableCheats");
        si.DeleteValue("EmuCore", "EnablePatches");
        si.DeleteValue("EmuCore", "EnableGameFixes");
        si.DeleteValue("EmuCore/GS", "UserHacks");
        si.DeleteValue("EmuCore/GS", "UserHackOverrides");
        si.DeleteValue("EmuCore/CPU", "CoreType");
        si.DeleteValue("EmuCore/CPU", "UseArm64Dynarec");
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVU");
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVUVersion");
        si.DeleteValue("EmuCore/Speedhacks", "vuThread");
        si.DeleteValue("EmuCore/Speedhacks", "EECycleRate");
        si.DeleteValue("EmuCore", "EnableFastBoot");
        si.DeleteValue("SPU2/Output", "StandardVolume");
        si.DeleteValue("SPU2/Output", "FastForwardVolume");
        // hasKnownOverride counts these and the pad tab writes them without consulting the master
        // toggle, so not clearing them let a flipped stick latch overrides on.
        si.DeleteValue("ARMSX2iOS/UI", "InvertLeftStickX");
        si.DeleteValue("ARMSX2iOS/UI", "InvertLeftStickY");
        si.DeleteValue("ARMSX2iOS/UI", "InvertRightStickX");
        si.DeleteValue("ARMSX2iOS/UI", "InvertRightStickY");
        si.RemoveEmptySections();
    }

    Error error;
    const bool saved = si.Save(&error);
    NSLog(@"[ARMSX2Bridge] Game settings %@ serial=%@ crc=%08X path=%@ result=%d",
          enabled ? @"saved" : @"cleared", ARMSX2NSStringFromStdString(serial),
          crc, ARMSX2NSStringFromStdString(settingsPath), saved ? 1 : 0);
    if (!saved)
        NSLog(@"[ARMSX2Bridge] Game settings save error: %@", ARMSX2NSStringFromStdString(error.GetDescription()));
}

@implementation ARMSX2Bridge (GameSettingsSave)

+ (void)setGameSettings:(nonnull NSDictionary<NSString *, id> *)settings forISO:(nullable NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc)) {
        NSLog(@"[ARMSX2Bridge] Game settings save rejected for %@", isoName ?: @"current game");
        return;
    }

    ARMSX2WriteGameSettingsForIdentity(serial, crc, settings);

    if (isoName.length == 0 || ARMSX2PathIsRunningDisc(ARMSX2ResolveISOPath(isoName))) {
        ARMSX2RequestPerGameSettingsReload();
    }
}

@end
