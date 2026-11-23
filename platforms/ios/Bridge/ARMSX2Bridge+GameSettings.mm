// ARMSX2Bridge+GameSettings.mm — per-game settings, read side
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "PerGameOverrides.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/Error.h"

#include "IOSRuntime.h"

// Builds the per-game settings dictionary seeded with the current global values.
static NSMutableDictionary<NSString*, id>* ARMSX2BuildGlobalGameSettingsResult()
{
    float globalUpscale = 1.0f;
    std::string globalAspect = "Auto 4:3/3:2";
    int globalTextureFiltering = 2;
    bool globalHardwareMipmapping = true;
    int globalBlendingAccuracy = 1;
    int globalInterlaceMode = 0;
    int globalTrilinearFiltering = -1;
    int globalHalfPixelOffset = 0;
    int globalRoundSprite = 0;
    bool globalAlignSprite = false;
    bool globalMergeSprite = false;
    bool globalWildArmsOffset = false;
    int globalTextureOffsetX = 0;
    int globalTextureOffsetY = 0;
    int globalSkipDrawStart = 0;
    int globalSkipDrawEnd = 0;
    bool globalEnableCheats = false;
    bool globalEnablePatches = true;
    bool globalEnableGameFixes = true;
    bool globalEnableGameDBHardwareFixes = true;
    int globalEECoreType = 2;
    bool globalMTVU = true;
    int globalEECycleRate = 0;
    bool globalFastBoot = false;
    int globalVolumePercent = ARMSX2DefaultAudioVolumePercent;
    ARMSX2EditBaseSettings([&](SettingsInterface& si) {
        globalUpscale = si.GetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f);
        globalAspect = si.GetStringValue("EmuCore/GS", "AspectRatio", "Auto 4:3/3:2");
        globalTextureFiltering = si.GetIntValue("EmuCore/GS", "filter", 2);
        globalHardwareMipmapping = si.GetBoolValue("EmuCore/GS", "hw_mipmap", true);
        globalBlendingAccuracy = si.GetIntValue("EmuCore/GS", "accurate_blending_unit", 1);
        // 0 is GSInterlaceMode::Automatic. Not 7, whatever the old picker labelled it.
        globalInterlaceMode = si.GetIntValue("EmuCore/GS", "deinterlace_mode", 0);
        globalTrilinearFiltering = si.GetIntValue("EmuCore/GS", "TriFilter", -1);
        globalHalfPixelOffset = si.GetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", 0);
        globalRoundSprite = si.GetIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", 0);
        globalAlignSprite = si.GetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", false);
        globalMergeSprite = si.GetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", false);
        globalWildArmsOffset = si.GetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", false);
        globalTextureOffsetX = si.GetIntValue("EmuCore/GS", "UserHacks_TCOffsetX", 0);
        globalTextureOffsetY = si.GetIntValue("EmuCore/GS", "UserHacks_TCOffsetY", 0);
        globalSkipDrawStart = si.GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", 0);
        globalSkipDrawEnd = si.GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", 0);
        globalEnableCheats = si.GetBoolValue("EmuCore", "EnableCheats", false);
        globalEnablePatches = si.GetBoolValue("EmuCore", "EnablePatches", true);
        globalEnableGameFixes = si.GetBoolValue("EmuCore", "EnableGameFixes", true);
        globalEnableGameDBHardwareFixes = !si.GetBoolValue("EmuCore/GS", "UserHacks", false);
        globalEECoreType = si.GetIntValue("EmuCore/CPU", "CoreType", 2);
        globalMTVU = si.GetBoolValue("EmuCore/Speedhacks", "vuThread", true);
        globalEECycleRate = ARMSX2ClampInt(si.GetIntValue("EmuCore/Speedhacks", "EECycleRate", 0), -3, 3);
        globalFastBoot = si.GetBoolValue("EmuCore", "EnableFastBoot", si.GetBoolValue("GameISO", "FastBoot", false));
        globalVolumePercent = ARMSX2ClampInt(
            si.GetIntValue("SPU2/Output", "StandardVolume", ARMSX2DefaultAudioVolumePercent),
            0,
            ARMSX2DefaultAudioVolumePercent);
    });
    return [@{
        ARMSX2PerGameKeyEnabled: @NO,
        ARMSX2PerGameKeyPath: @"",
        ARMSX2PerGameKeySerial: @"",
        ARMSX2PerGameKeyCrc: @"",
        // has*Override so an untouched setting writes nothing.
        ARMSX2PerGameKeyUpscaleMultiplier: @(globalUpscale),
        ARMSX2PerGameKeyHasUpscaleMultiplierOverride: @NO,
        ARMSX2PerGameKeyAspectRatio: ARMSX2NSStringFromStdString(globalAspect),
        ARMSX2PerGameKeyHasAspectRatioOverride: @NO,
        ARMSX2PerGameKeyTextureFiltering: @(globalTextureFiltering),
        ARMSX2PerGameKeyHasTextureFilteringOverride: @NO,
        ARMSX2PerGameKeyHardwareMipmapping: @(globalHardwareMipmapping),
        ARMSX2PerGameKeyHasHardwareMipmappingOverride: @NO,
        ARMSX2PerGameKeyBlendingAccuracy: @(globalBlendingAccuracy),
        ARMSX2PerGameKeyHasBlendingAccuracyOverride: @NO,
        ARMSX2PerGameKeyInterlaceMode: @(globalInterlaceMode),
        ARMSX2PerGameKeyHasInterlaceModeOverride: @NO,
        ARMSX2PerGameKeyTrilinearFiltering: @(globalTrilinearFiltering),
        ARMSX2PerGameKeyHasTrilinearFilteringOverride: @NO,
        ARMSX2PerGameKeyHalfPixelOffset: @(globalHalfPixelOffset),
        ARMSX2PerGameKeyHasHalfPixelOffsetOverride: @NO,
        ARMSX2PerGameKeyRoundSprite: @(globalRoundSprite),
        ARMSX2PerGameKeyHasRoundSpriteOverride: @NO,
        ARMSX2PerGameKeyAlignSprite: @(globalAlignSprite),
        ARMSX2PerGameKeyHasAlignSpriteOverride: @NO,
        ARMSX2PerGameKeyMergeSprite: @(globalMergeSprite),
        ARMSX2PerGameKeyHasMergeSpriteOverride: @NO,
        ARMSX2PerGameKeyWildArmsOffset: @(globalWildArmsOffset),
        ARMSX2PerGameKeyHasWildArmsOffsetOverride: @NO,
        ARMSX2PerGameKeyTextureOffsetX: @(ARMSX2ClampInt(globalTextureOffsetX, -4096, 4096)),
        ARMSX2PerGameKeyHasTextureOffsetXOverride: @NO,
        ARMSX2PerGameKeyTextureOffsetY: @(ARMSX2ClampInt(globalTextureOffsetY, -4096, 4096)),
        ARMSX2PerGameKeyHasTextureOffsetYOverride: @NO,
        ARMSX2PerGameKeySkipDrawStart: @(ARMSX2ClampInt(globalSkipDrawStart, 0, 5000)),
        ARMSX2PerGameKeyHasSkipDrawStartOverride: @NO,
        ARMSX2PerGameKeySkipDrawEnd: @(ARMSX2ClampInt(globalSkipDrawEnd, 0, 5000)),
        ARMSX2PerGameKeyHasSkipDrawEndOverride: @NO,
        ARMSX2PerGameKeyEnableCheats: @(globalEnableCheats),
        ARMSX2PerGameKeyEnablePatches: @(globalEnablePatches),
        ARMSX2PerGameKeyEnableGameFixes: @(globalEnableGameFixes),
        ARMSX2PerGameKeyEnableGameDBHardwareFixes: @(globalEnableGameDBHardwareFixes),
        ARMSX2PerGameKeyEeCoreType: @(globalEECoreType),
        ARMSX2PerGameKeyMtvu: @(globalMTVU),
        ARMSX2PerGameKeyGlobalEECycleRate: @(globalEECycleRate),
        ARMSX2PerGameKeyEeCycleRate: @(globalEECycleRate),
        ARMSX2PerGameKeyHasEECycleRateOverride: @NO,
        ARMSX2PerGameKeyGlobalFastBoot: @(globalFastBoot),
        ARMSX2PerGameKeyFastBoot: @(globalFastBoot),
        ARMSX2PerGameKeyHasFastBootOverride: @NO,
        ARMSX2PerGameKeyGlobalVolumePercent: @(globalVolumePercent),
        ARMSX2PerGameKeyVolumePercent: @(globalVolumePercent),
        ARMSX2PerGameKeyHasVolumeOverride: @NO,
        // An empty snapshot is still authoritative: no identity/INI was
        // available, so Swift must not retry the same disc probe per row.
        ARMSX2PerGameKeyPerGameINI: @{},
    } mutableCopy];
}

// Overlays per-game INI overrides for the given serial/crc onto a globals-seeded result.
// Sourcing serial/crc from the caller avoids re-scanning the disc image (which is unsafe
// while the VM is actively reading the same disc).
static void ARMSX2ApplyPerGameSettingsOverrides(NSMutableDictionary<NSString*, id>* result, const std::string& serial, u32 crc)
{
    const std::string settingsPath = VMManager::GetGameSettingsPath(serial, crc);
    result[ARMSX2PerGameKeyPath] = ARMSX2NSStringFromStdString(settingsPath);
    result[ARMSX2PerGameKeySerial] = ARMSX2NSStringFromStdString(serial);
    result[ARMSX2PerGameKeyCrc] = [NSString stringWithFormat:@"%08X", crc];

    // Publish one typed snapshot from the already-open INI, so every row shares the same game
    // identity and each caller does a single file read.
    NSMutableDictionary<NSString*, id>* prefetchedINI = [NSMutableDictionary dictionary];
    result[ARMSX2PerGameKeyPerGameINI] = prefetchedINI;

    INISettingsInterface si(settingsPath);
    if (!si.Load())
        return;

    const auto snapshotKey = [](const char* section, const char* key) {
        return [NSString stringWithFormat:@"%s\n%s", section, key];
    };
    const auto snapshotBool = [&](const char* section, const char* key) {
        if (si.ContainsValue(section, key))
            prefetchedINI[snapshotKey(section, key)] = @(si.GetBoolValue(section, key, false));
    };
    const auto snapshotInt = [&](const char* section, const char* key) {
        if (si.ContainsValue(section, key))
            prefetchedINI[snapshotKey(section, key)] = @(si.GetIntValue(section, key, 0));
    };
    const auto snapshotFloat = [&](const char* section, const char* key) {
        if (si.ContainsValue(section, key))
            prefetchedINI[snapshotKey(section, key)] = @(si.GetFloatValue(section, key, 0.0f));
    };
    const auto snapshotString = [&](const char* section, const char* key) {
        if (si.ContainsValue(section, key))
            prefetchedINI[snapshotKey(section, key)] = ARMSX2NSStringFromStdString(
                si.GetStringValue(section, key, ""));
    };

    static constexpr const char* kBoolSnapshotKeys[][2] = {
        {"EmuCore", "EnableWideScreenPatches"},
        {"EmuCore", "EnableNoInterlacingPatches"},
        {"EmuCore/GS", "ShaderChainEnabled"},
        {"EmuCore/CPU/Recompiler", "EnableIOP"},
        {"EmuCore/CPU/Recompiler", "EnableVU0"},
        {"EmuCore/CPU/Recompiler", "EnableVU1"},
        {"EmuCore/CPU/Recompiler", "fpuOverflow"},
        {"EmuCore/CPU/Recompiler", "fpuExtraOverflow"},
        {"EmuCore/CPU/Recompiler", "fpuFullMode"},
        {"EmuCore/CPU/Recompiler", "vu0Overflow"},
        {"EmuCore/CPU/Recompiler", "vu0ExtraOverflow"},
        {"EmuCore/CPU/Recompiler", "vu0SignOverflow"},
        {"EmuCore/GS", "LoadTextureReplacements"},
        {"EmuCore/GS", "LoadTextureReplacementsAsync"},
        {"EmuCore/GS", "PrecacheTextureReplacements"},
        {"EmuCore/GS", "SyncToHostRefreshRate"},
        {"Achievements", "Enabled"},
        {"Achievements", "ChallengeMode"},
    };
    static constexpr const char* kIntSnapshotKeys[][2] = {
        {"EmuCore/Speedhacks", "EECycleSkip"},
        {"EmuCore/GS", "ShadeBoost_Brightness"},
        {"EmuCore/GS", "ShadeBoost_Contrast"},
        {"EmuCore/GS", "ShadeBoost_Saturation"},
        {"EmuCore/GS", "ShadeBoost_Gamma"},
        {"EmuCore/GS", "dithering_ps2"},
        {"SPU2/Output", "FastForwardVolume"},
        {"EmuCore/CPU", "FPU.Roundmode"},
        {"EmuCore/CPU", "VU0.Roundmode"},
        {"EmuCore/CPU", "VU1.Roundmode"},
        {"EmuCore/GS", "HWDownloadMode"},
        {"EmuCore/GS", "UserHacks_CPUCLUTRender"},
        {"EmuCore/GS", "UserHacks_GPUTargetCLUTMode"},
        {"EmuCore/GS", "VsyncQueueSize"},
        {"SPU2/Output", "BufferMS"},
        {"SPU2/Output", "OutputLatencyMS"},
        {"ARMSX2iOS/FramePacing", "Preset"},
    };
    static constexpr const char* kFloatSnapshotKeys[][2] = {
        {"Framerate", "NominalScalar"},
        {"ARMSX2iOS/FramePacing", "TargetFPS"},
    };

    for (const auto& entry : kBoolSnapshotKeys)
        snapshotBool(entry[0], entry[1]);
    for (const auto& entry : kIntSnapshotKeys)
        snapshotInt(entry[0], entry[1]);
    for (const auto& entry : kFloatSnapshotKeys)
        snapshotFloat(entry[0], entry[1]);
    snapshotString("EmuCore/GS", "ShaderChainPresetRef");

    if (si.ContainsValue("EmuCore/Speedhacks", "vuThread") &&
        !si.GetBoolValue("EmuCore/Speedhacks", "vuThread", true) &&
        (!si.GetBoolValue("ARMSX2iOS/PerGame", "ManualMTVU", false) ||
            si.GetIntValue("ARMSX2iOS/PerGame", "ManualMTVUVersion", 0) < 3)) {
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVU");
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVUVersion");
        si.DeleteValue("EmuCore/Speedhacks", "vuThread");
        Error saveError;
        const bool saved = si.Save(&saveError);
        std::fprintf(stderr, "@@IOS_PERGAME_MTVU_REPAIR@@ file=\"%s\" ui_read=1 removed_stale_false=1 saved=%d error=\"%s\"\n",
            settingsPath.c_str(), saved ? 1 : 0, saveError.GetDescription().c_str());
        std::fflush(stderr);
    }

    // Older saves froze the global claim mask into this file, where it went stale.
    const u32 derivedClaims = ARMSX2DerivePerGameHackClaims(si);
    const u32 storedClaims = static_cast<u32>(si.GetIntValue("EmuCore/GS", "UserHackOverrides", 0));
    if (storedClaims != derivedClaims) {
        ARMSX2StoreDerivedPerGameHackClaims(si);
        si.RemoveEmptySections();
        Error claimError;
        const bool saved = si.Save(&claimError);
        std::fprintf(stderr, "@@IOS_PERGAME_CLAIM_REPAIR@@ file=\"%s\" stored=%u derived=%u saved=%d\n",
            settingsPath.c_str(), storedClaims, derivedClaims, saved ? 1 : 0);
        std::fflush(stderr);
    }

    const bool hasKnownOverride =
        si.GetBoolValue("ARMSX2iOS/PerGame", "Enabled", false) ||
        si.ContainsValue("EmuCore/GS", "upscale_multiplier") ||
        si.ContainsValue("EmuCore/GS", "AspectRatio") ||
        si.ContainsValue("EmuCore/GS", "filter") ||
        si.ContainsValue("EmuCore/GS", "hw_mipmap") ||
        si.ContainsValue("EmuCore/GS", "accurate_blending_unit") ||
        si.ContainsValue("EmuCore/GS", "deinterlace_mode") ||
        si.ContainsValue("EmuCore/GS", "TriFilter") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_HalfPixelOffset") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_round_sprite_offset") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_align_sprite_X") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_merge_pp_sprite") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_DisableDepthSupport") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetX") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetY") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_Start") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_End") ||
        si.ContainsValue("EmuCore", "EnableCheats") ||
        si.ContainsValue("EmuCore", "EnablePatches") ||
        si.ContainsValue("EmuCore", "EnableGameFixes") ||
        si.ContainsValue("EmuCore/GS", "UserHacks") ||
        si.ContainsValue("EmuCore/CPU", "CoreType") ||
        si.ContainsValue("EmuCore/CPU", "UseArm64Dynarec") ||
        si.ContainsValue("EmuCore/Speedhacks", "vuThread") ||
        si.ContainsValue("EmuCore/Speedhacks", "EECycleRate") ||
        si.ContainsValue("EmuCore", "EnableFastBoot") ||
        si.ContainsValue("SPU2/Output", "StandardVolume") ||
        si.ContainsValue("SPU2/Output", "FastForwardVolume") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertLeftStickX") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertLeftStickY") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertRightStickX") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertRightStickY");

    result[ARMSX2PerGameKeyEnabled] = @(hasKnownOverride);
    // StandardVolume only. Falling back to FastForwardVolume made the main slider show an
    // override nobody set.
    const bool hasVolumeOverride = si.ContainsValue("SPU2/Output", "StandardVolume");
    const int inheritedVolumePercent = [result[ARMSX2PerGameKeyVolumePercent] intValue];
    const int volumePercent = hasVolumeOverride ?
        si.GetIntValue("SPU2/Output", "StandardVolume", inheritedVolumePercent) :
        inheritedVolumePercent;
    result[ARMSX2PerGameKeyHasVolumeOverride] = @(hasVolumeOverride);
    result[ARMSX2PerGameKeyVolumePercent] = @(ARMSX2ClampInt(volumePercent, 0, ARMSX2DefaultAudioVolumePercent));
    NSString* currentAspect = [result[ARMSX2PerGameKeyAspectRatio] isKindOfClass:NSString.class] ? result[ARMSX2PerGameKeyAspectRatio] : @"Auto 4:3/3:2";
    result[ARMSX2PerGameKeyHasUpscaleMultiplierOverride] = @(si.ContainsValue("EmuCore/GS", "upscale_multiplier"));
    result[ARMSX2PerGameKeyUpscaleMultiplier] = @(si.GetFloatValue("EmuCore/GS", "upscale_multiplier", [result[ARMSX2PerGameKeyUpscaleMultiplier] floatValue]));
    result[ARMSX2PerGameKeyHasAspectRatioOverride] = @(si.ContainsValue("EmuCore/GS", "AspectRatio"));
    result[ARMSX2PerGameKeyAspectRatio] = ARMSX2NSStringFromStdString(si.GetStringValue("EmuCore/GS", "AspectRatio", currentAspect.UTF8String));
    result[ARMSX2PerGameKeyHasTextureFilteringOverride] = @(si.ContainsValue("EmuCore/GS", "filter"));
    result[ARMSX2PerGameKeyTextureFiltering] = @(si.GetIntValue("EmuCore/GS", "filter", [result[ARMSX2PerGameKeyTextureFiltering] intValue]));
    result[ARMSX2PerGameKeyHasHardwareMipmappingOverride] = @(si.ContainsValue("EmuCore/GS", "hw_mipmap"));
    result[ARMSX2PerGameKeyHardwareMipmapping] = @(si.GetBoolValue("EmuCore/GS", "hw_mipmap", [result[ARMSX2PerGameKeyHardwareMipmapping] boolValue]));
    result[ARMSX2PerGameKeyHasBlendingAccuracyOverride] = @(si.ContainsValue("EmuCore/GS", "accurate_blending_unit"));
    result[ARMSX2PerGameKeyBlendingAccuracy] = @(si.GetIntValue("EmuCore/GS", "accurate_blending_unit", [result[ARMSX2PerGameKeyBlendingAccuracy] intValue]));
    result[ARMSX2PerGameKeyHasInterlaceModeOverride] = @(si.ContainsValue("EmuCore/GS", "deinterlace_mode"));
    result[ARMSX2PerGameKeyInterlaceMode] = @(si.GetIntValue("EmuCore/GS", "deinterlace_mode", [result[ARMSX2PerGameKeyInterlaceMode] intValue]));
    result[ARMSX2PerGameKeyHasTrilinearFilteringOverride] = @(si.ContainsValue("EmuCore/GS", "TriFilter"));
    result[ARMSX2PerGameKeyTrilinearFiltering] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "TriFilter", [result[ARMSX2PerGameKeyTrilinearFiltering] intValue]), -1, 2));
    result[ARMSX2PerGameKeyHasHalfPixelOffsetOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_HalfPixelOffset"));
    result[ARMSX2PerGameKeyHalfPixelOffset] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", [result[ARMSX2PerGameKeyHalfPixelOffset] intValue]), 0, 5));
    result[ARMSX2PerGameKeyHasRoundSpriteOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_round_sprite_offset"));
    result[ARMSX2PerGameKeyRoundSprite] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", [result[ARMSX2PerGameKeyRoundSprite] intValue]), 0, 2));
    result[ARMSX2PerGameKeyHasAlignSpriteOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_align_sprite_X"));
    result[ARMSX2PerGameKeyAlignSprite] = @(si.GetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", [result[ARMSX2PerGameKeyAlignSprite] boolValue]));
    result[ARMSX2PerGameKeyHasMergeSpriteOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_merge_pp_sprite"));
    result[ARMSX2PerGameKeyMergeSprite] = @(si.GetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", [result[ARMSX2PerGameKeyMergeSprite] boolValue]));
    result[ARMSX2PerGameKeyHasWildArmsOffsetOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition"));
    result[ARMSX2PerGameKeyWildArmsOffset] = @(si.GetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", [result[ARMSX2PerGameKeyWildArmsOffset] boolValue]));
    result[ARMSX2PerGameKeyHasTextureOffsetXOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetX"));
    result[ARMSX2PerGameKeyTextureOffsetX] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_TCOffsetX", [result[ARMSX2PerGameKeyTextureOffsetX] intValue]), -4096, 4096));
    result[ARMSX2PerGameKeyHasTextureOffsetYOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetY"));
    result[ARMSX2PerGameKeyTextureOffsetY] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_TCOffsetY", [result[ARMSX2PerGameKeyTextureOffsetY] intValue]), -4096, 4096));
    result[ARMSX2PerGameKeyHasSkipDrawStartOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_Start"));
    result[ARMSX2PerGameKeySkipDrawStart] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", [result[ARMSX2PerGameKeySkipDrawStart] intValue]), 0, 5000));
    result[ARMSX2PerGameKeyHasSkipDrawEndOverride] = @(si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_End"));
    result[ARMSX2PerGameKeySkipDrawEnd] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", [result[ARMSX2PerGameKeySkipDrawEnd] intValue]), 0, 5000));
    result[ARMSX2PerGameKeyEnableCheats] = @(si.GetBoolValue("EmuCore", "EnableCheats", [result[ARMSX2PerGameKeyEnableCheats] boolValue]));
    result[ARMSX2PerGameKeyEnablePatches] = @(si.GetBoolValue("EmuCore", "EnablePatches", [result[ARMSX2PerGameKeyEnablePatches] boolValue]));
    result[ARMSX2PerGameKeyEnableGameFixes] = @(si.GetBoolValue("EmuCore", "EnableGameFixes", [result[ARMSX2PerGameKeyEnableGameFixes] boolValue]));
    result[ARMSX2PerGameKeyEnableGameDBHardwareFixes] = @(!si.GetBoolValue("EmuCore/GS", "UserHacks", ![result[ARMSX2PerGameKeyEnableGameDBHardwareFixes] boolValue]));
    result[ARMSX2PerGameKeyEeCoreType] = @(si.GetIntValue("EmuCore/CPU", "CoreType", [result[ARMSX2PerGameKeyEeCoreType] intValue]));
    result[ARMSX2PerGameKeyMtvu] = @(si.GetBoolValue("EmuCore/Speedhacks", "vuThread", [result[ARMSX2PerGameKeyMtvu] boolValue]));
    result[ARMSX2PerGameKeyHasEECycleRateOverride] = @(si.ContainsValue("EmuCore/Speedhacks", "EECycleRate"));
    result[ARMSX2PerGameKeyEeCycleRate] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/Speedhacks", "EECycleRate", [result[ARMSX2PerGameKeyEeCycleRate] intValue]), -3, 3));
    result[ARMSX2PerGameKeyHasFastBootOverride] = @(si.ContainsValue("EmuCore", "EnableFastBoot"));
    result[ARMSX2PerGameKeyFastBoot] = @(si.GetBoolValue("EmuCore", "EnableFastBoot", [result[ARMSX2PerGameKeyFastBoot] boolValue]));

    // Per-game compatibility overrides, read from the INI already loaded here so the
    // panel opens without re-parsing it per key. Only present overrides are included;
    // absent keys fall back to the global value on the caller side.
    {
        NSMutableDictionary<NSString*, NSNumber*>* perGameFixes = [NSMutableDictionary dictionary];
        static constexpr const char* kARMSX2GameFixKeys[] = {
            "VuAddSubHack", "XgKickHack", "EETimingHack", "InstantDMAHack",
            "SoftwareRendererFMVHack", "SkipMPEGHack", "OPHFlagHack", "DMABusyHack",
            "VIF1StallHack", "GIFFIFOHack", "GoemonTlbHack", "IbitHack", "VUSyncHack",
            "VUOverflowHack", "BlitInternalFPSHack", "FullVU0SyncHack"
        };
        for (const char* gameFixKey : kARMSX2GameFixKeys)
        {
            if (si.ContainsValue("EmuCore/Gamefixes", gameFixKey))
            {
                perGameFixes[[NSString stringWithUTF8String:gameFixKey]] =
                    @(si.GetBoolValue("EmuCore/Gamefixes", gameFixKey, false) ? 1 : 0);
            }
        }
        result[ARMSX2PerGameKeyPerGameFixes] = perGameFixes;

        const bool hasPerGameAAT = si.ContainsValue("EmuCore/GS", "HWAccurateAlphaTest");
        result[ARMSX2PerGameKeyHasPerGameAAT] = @(hasPerGameAAT);
        result[ARMSX2PerGameKeyPerGameAAT] = @((hasPerGameAAT && si.GetBoolValue("EmuCore/GS", "HWAccurateAlphaTest", false)) ? 1 : 0);

        const bool hasPerGameTextureInsideRt = si.ContainsValue("EmuCore/GS", "UserHacks_TextureInsideRt");
        result[ARMSX2PerGameKeyHasPerGameTextureInsideRt] = @(hasPerGameTextureInsideRt);
        result[ARMSX2PerGameKeyPerGameTextureInsideRt] =
            @(hasPerGameTextureInsideRt ? si.GetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 0) : 0);

        const bool hasPerGameDisableDepth = si.ContainsValue("EmuCore/GS", "UserHacks_DisableDepthSupport");
        result[ARMSX2PerGameKeyHasPerGameDisableDepth] = @(hasPerGameDisableDepth);
        result[ARMSX2PerGameKeyPerGameDisableDepth] =
            @(hasPerGameDisableDepth ? si.GetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", false) : NO);

        const bool hasPerGameRenderer = si.ContainsValue("EmuCore/GS", "Renderer");
        result[ARMSX2PerGameKeyHasPerGameRenderer] = @(hasPerGameRenderer);
        result[ARMSX2PerGameKeyPerGameRenderer] = @(hasPerGameRenderer ? si.GetIntValue("EmuCore/GS", "Renderer", 17) : 17);

        const bool hasPerGameFXAA = si.ContainsValue("EmuCore/GS", "fxaa");
        result[ARMSX2PerGameKeyHasPerGameFXAA] = @(hasPerGameFXAA);
        result[ARMSX2PerGameKeyPerGameFXAA] = @((hasPerGameFXAA && si.GetBoolValue("EmuCore/GS", "fxaa", false)) ? 1 : 0);

        const bool hasPerGameShadeBoost = si.ContainsValue("EmuCore/GS", "ShadeBoost");
        result[ARMSX2PerGameKeyHasPerGameShadeBoost] = @(hasPerGameShadeBoost);
        result[ARMSX2PerGameKeyPerGameShadeBoost] = @((hasPerGameShadeBoost && si.GetBoolValue("EmuCore/GS", "ShadeBoost", false)) ? 1 : 0);

        const bool hasPerGameTVShader = si.ContainsValue("EmuCore/GS", "TVShader");
        result[ARMSX2PerGameKeyHasPerGameTVShader] = @(hasPerGameTVShader);
        result[ARMSX2PerGameKeyPerGameTVShader] = @(hasPerGameTVShader ? si.GetIntValue("EmuCore/GS", "TVShader", 0) : 0);

        const bool hasPerGameCASMode = si.ContainsValue("EmuCore/GS", "CASMode");
        result[ARMSX2PerGameKeyHasPerGameCASMode] = @(hasPerGameCASMode);
        result[ARMSX2PerGameKeyPerGameCASMode] = @(hasPerGameCASMode ? si.GetIntValue("EmuCore/GS", "CASMode", 0) : 0);

        // MetalFX Spatial upscaler (Off = 0, MetalFXSpatial = 1).
        const bool hasPerGameUpscaler = si.ContainsValue("EmuCore/GS", "Upscaler");
        result[ARMSX2PerGameKeyHasPerGameUpscaler] = @(hasPerGameUpscaler);
        result[ARMSX2PerGameKeyPerGameUpscaler] = @(hasPerGameUpscaler ? si.GetIntValue("EmuCore/GS", "Upscaler", 0) : 0);

        const bool hasPerGameMaxAnisotropy = si.ContainsValue("EmuCore/GS", "MaxAnisotropy");
        result[ARMSX2PerGameKeyHasPerGameMaxAnisotropy] = @(hasPerGameMaxAnisotropy);
        result[ARMSX2PerGameKeyPerGameMaxAnisotropy] = @(hasPerGameMaxAnisotropy ? si.GetIntValue("EmuCore/GS", "MaxAnisotropy", 0) : 0);

        const bool hasPerGameCASSharpness = si.ContainsValue("EmuCore/GS", "CASSharpness");
        result[ARMSX2PerGameKeyHasPerGameCASSharpness] = @(hasPerGameCASSharpness);
        result[ARMSX2PerGameKeyPerGameCASSharpness] = @(hasPerGameCASSharpness ? si.GetIntValue("EmuCore/GS", "CASSharpness", 50) : 50);

        const bool hasPerGamePCRTCOffsets = si.ContainsValue("EmuCore/GS", "pcrtc_offsets");
        result[ARMSX2PerGameKeyHasPerGamePCRTCOffsets] = @(hasPerGamePCRTCOffsets);
        result[ARMSX2PerGameKeyPerGamePCRTCOffsets] = @((hasPerGamePCRTCOffsets && si.GetBoolValue("EmuCore/GS", "pcrtc_offsets", false)) ? 1 : 0);

        const bool hasPerGameIntegerScaling = si.ContainsValue("EmuCore/GS", "IntegerScaling");
        result[ARMSX2PerGameKeyHasPerGameIntegerScaling] = @(hasPerGameIntegerScaling);
        result[ARMSX2PerGameKeyPerGameIntegerScaling] = @((hasPerGameIntegerScaling && si.GetBoolValue("EmuCore/GS", "IntegerScaling", false)) ? 1 : 0);

        const bool hasPerGameSkipDupFrames = si.ContainsValue("EmuCore/GS", "SkipDuplicateFrames");
        result[ARMSX2PerGameKeyHasPerGameSkipDupFrames] = @(hasPerGameSkipDupFrames);
        result[ARMSX2PerGameKeyPerGameSkipDupFrames] = @((hasPerGameSkipDupFrames && si.GetBoolValue("EmuCore/GS", "SkipDuplicateFrames", true)) ? 1 : 0);

        const bool hasPerGamePCRTCOverscan = si.ContainsValue("EmuCore/GS", "pcrtc_overscan");
        result[ARMSX2PerGameKeyHasPerGamePCRTCOverscan] = @(hasPerGamePCRTCOverscan);
        result[ARMSX2PerGameKeyPerGamePCRTCOverscan] = @((hasPerGamePCRTCOverscan && si.GetBoolValue("EmuCore/GS", "pcrtc_overscan", false)) ? 1 : 0);

        const bool hasPerGamePCRTCAntiBlur = si.ContainsValue("EmuCore/GS", "pcrtc_antiblur");
        result[ARMSX2PerGameKeyHasPerGamePCRTCAntiBlur] = @(hasPerGamePCRTCAntiBlur);
        result[ARMSX2PerGameKeyPerGamePCRTCAntiBlur] = @((hasPerGamePCRTCAntiBlur && si.GetBoolValue("EmuCore/GS", "pcrtc_antiblur", true)) ? 1 : 0);

        const bool hasPerGameDisableInterlaceOffset = si.ContainsValue("EmuCore/GS", "disable_interlace_offset");
        result[ARMSX2PerGameKeyHasPerGameDisableInterlaceOffset] = @(hasPerGameDisableInterlaceOffset);
        result[ARMSX2PerGameKeyPerGameDisableInterlaceOffset] = @((hasPerGameDisableInterlaceOffset && si.GetBoolValue("EmuCore/GS", "disable_interlace_offset", false)) ? 1 : 0);
    }
}

@implementation ARMSX2Bridge (GameSettings)

+ (nonnull NSDictionary<ARMSX2PerGameKey, id> *)gameSettingsForISO:(nullable NSString *)isoName {
    NSMutableDictionary<NSString*, id>* result = ARMSX2BuildGlobalGameSettingsResult();

    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc)) {
        NSLog(@"[ARMSX2Bridge] Game settings unavailable for %@", isoName ?: @"current game");
        return result;
    }

    ARMSX2ApplyPerGameSettingsOverrides(result, serial, crc);
    return result;
}

// The library already identified every game while building its card model. Reuse that
// serial/CRC rather than probing the disc image again when SwiftUI presents settings.
+ (nonnull NSDictionary<ARMSX2PerGameKey, id> *)gameSettingsForSerial:(nullable NSString *)serial
                                                                  crc:(nullable NSString *)crcString {
    NSMutableDictionary<NSString*, id>* result = ARMSX2BuildGlobalGameSettingsResult();
    if (crcString.length == 0)
        return result;

    unsigned int parsedCRC = 0;
    NSScanner* scanner = [NSScanner scannerWithString:crcString];
    if (![scanner scanHexInt:&parsedCRC] || parsedCRC == 0)
        return result;

    const std::string settingsSerial = serial.length > 0
        ? std::string(serial.UTF8String)
        : std::string();
    ARMSX2ApplyPerGameSettingsOverrides(result, settingsSerial, static_cast<u32>(parsedCRC));
    return result;
}

+ (nullable NSDictionary<ARMSX2PerGameKey, id> *)gameSettingsForCurrentGame {
    if (!VMManager::HasValidVM())
        return nil;
    return [self gameSettingsForISO:nil];
}

@end
