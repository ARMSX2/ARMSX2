// PerGameSettingsDraft+Save.swift — writes the per-game draft back to the INI
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension PerGameSettingsDraft {
    /// Status shown after Save. In-game most settings apply immediately via the
    /// live-apply bridge path; a few (renderer, recompiler toggles, MTVU) need a
    /// reset. From the library nothing is running, so the next boot is the earliest.
    var postSaveMessage: String {
        if !enabled {
            return settings.localized("Per-game overrides cleared.")
        }
        let serial = game.metadata["serial"] ?? game.name
        let suffix = savesToRunningGame
            ? settings.localized("Saved — changes apply now. Renderer and recompiler settings need a reset.")
            : settings.localized("Reset or relaunch the game to apply.")
        return String(format: settings.localized("Saved for %1$@. %2$@"), serial, suffix)
    }

    func clearStickInversionOverrides() {
        for key in SettingsStore.stickInversionKeys {
            Self.clearPerGameValue("ARMSX2iOS/UI", key, useCurrent: savesToRunningGame, iso: game.bootName)
        }
        if savesToRunningGame {
            settings.reloadStickInversionOverrides()
        }
    }

    func clearFramePacingOverrides() {
        perGameFramePacingPreset = -1
        perGameFrameLimiter = -1
        perGameTargetFPS = -1
        perGameVsyncQueue = -1
        perGameSyncToHostRefresh = -1
        perGameBufferMS = -1
        perGameOutputLatencyMS = -1
    }

    /// Writes the editor snapshot through the existing bridge. During a live preview
    /// the native transaction puts the committed INI back once the VM has consumed
    /// this snapshot, so this method does not itself mark anything saved.
    @discardableResult
    func writeCurrentSettings(
        normalizesEditableValues: Bool
    ) -> Bool {
        guard hasGameSettingsIdentity else {
            statusMessage = "Start this game once before saving its settings."
            return false
        }
        let normalizedSkipDraw = normalizedSkipDrawValues()
        if normalizesEditableValues,
           skipDrawStart != normalizedSkipDraw.start {
            skipDrawStart = normalizedSkipDraw.start
        }
        if normalizesEditableValues,
           skipDrawEnd != normalizedSkipDraw.end {
            skipDrawEnd = normalizedSkipDraw.end
        }

        let settingsDict: [String: Any] = [
            "enabled": enabled,
            "upscaleMultiplier": upscaleMultiplier,
            "aspectRatio": aspectRatio,
            "textureFiltering": Int32(textureFiltering),
            "hardwareMipmapping": Int32(hardwareMipmapping),
            "blendingAccuracy": Int32(blendingAccuracy),
            "interlaceMode": Int32(interlaceMode),
            "trilinearFiltering": Int32(trilinearFiltering),
            "halfPixelOffset": Int32(halfPixelOffset),
            "roundSprite": Int32(roundSprite),
            "alignSprite": Int32(alignSprite),
            "mergeSprite": Int32(mergeSprite),
            "wildArmsOffset": Int32(wildArmsOffset),
            "hasTextureOffsetXOverride": textureOffsetXOverride,
            "textureOffsetX": Int32(textureOffsetX),
            "hasTextureOffsetYOverride": textureOffsetYOverride,
            "textureOffsetY": Int32(textureOffsetY),
            "hasSkipDrawStartOverride": skipDrawStartOverride,
            "skipDrawStart": Int32(normalizedSkipDraw.start),
            "hasSkipDrawEndOverride": skipDrawEndOverride,
            "skipDrawEnd": Int32(normalizedSkipDraw.end),
            "hasVolumeOverride": enabled && volumeOverride,
            "volumePercent": Int32(volumePercent),
            "eeCoreType": enabled ? Int32(eeCoreType) : 0,
            "mtvu": enabled && mtvu,
            "hasEECycleRateOverride": enabled && eeCycleRate != Self.eeCycleRateUseGlobalSentinel,
            "eeCycleRate": Int32(SettingsStore.clampedEECycleRate(eeCycleRate == Self.eeCycleRateUseGlobalSentinel ? globalEECycleRate : eeCycleRate)),
            "hasFastBootOverride": enabled && fastBoot != Self.fastBootUseGlobalSentinel,
            "fastBoot": fastBoot == Self.fastBootOn,
            "enableCheats": enabled && enableCheats,
            "enablePatches": enabled && enablePatches,
            "enableGameFixes": enabled && enableGameFixes,
            "enableGameDBHardwareFixes": enabled && enableGameDBHardwareFixes
        ]
        ARMSX2Bridge.setGameSettings(settingsDict, forISO: savesToRunningGame ? nil : game.bootName)
        savePerGameCompatibility()
        return true
    }

    /// Write the per-game compatibility overrides (game fixes, accurate alpha test,
    /// texture-inside-RT) via the generic per-game INI helper. "Use global" (-1) and
    /// a disabled master toggle both clear the per-game key so the global value wins.
    private func savePerGameCompatibility() {
        let useCurrent = savesToRunningGame
        let iso = game.bootName
        for option in SettingsStore.gameFixOptions {
            let state = perGameFixes[option.key] ?? -1
            if enabled && state != -1 {
                Self.setPerGameBoolValue("EmuCore/Gamefixes", option.key, state == 1, useCurrent: useCurrent, iso: iso)
            } else {
                Self.clearPerGameValue("EmuCore/Gamefixes", option.key, useCurrent: useCurrent, iso: iso)
            }
        }
        if enabled && perGameAAT != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "HWAccurateAlphaTest", perGameAAT == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "HWAccurateAlphaTest", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameTextureInsideRt != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", perGameTextureInsideRt, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_TextureInsideRt", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameDisableDepth != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", perGameDisableDepth == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_DisableDepthSupport", useCurrent: useCurrent, iso: iso)
        }
        // The core keeps a running game on its booted renderer, so this applies next boot.
        if enabled && perGameRenderer != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "Renderer", perGameRenderer, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "Renderer", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameFXAA != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "fxaa", perGameFXAA == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "fxaa", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameUpscaler != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "Upscaler", perGameUpscaler, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "Upscaler", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoost != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "ShadeBoost", perGameShadeBoost == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameTVShader != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "TVShader", perGameTVShader, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "TVShader", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameCASMode != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "CASMode", perGameCASMode, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "CASMode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameMaxAnisotropy != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "MaxAnisotropy", perGameMaxAnisotropy, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "MaxAnisotropy", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameCASSharpness != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "CASSharpness", perGameCASSharpness, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "CASSharpness", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePCRTCOffsets != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "pcrtc_offsets", perGamePCRTCOffsets == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "pcrtc_offsets", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameIntegerScaling != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "IntegerScaling", perGameIntegerScaling == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "IntegerScaling", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameSkipDupFrames != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "SkipDuplicateFrames", perGameSkipDupFrames == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "SkipDuplicateFrames", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePCRTCOverscan != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "pcrtc_overscan", perGamePCRTCOverscan == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "pcrtc_overscan", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePCRTCAntiBlur != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "pcrtc_antiblur", perGamePCRTCAntiBlur == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "pcrtc_antiblur", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameDisableInterlaceOffset != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "disable_interlace_offset", perGameDisableInterlaceOffset == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "disable_interlace_offset", useCurrent: useCurrent, iso: iso)
        }
        // High-value per-game overrides added via the generic helper path.
        if enabled && perGameWidescreen != -1 {
            Self.setPerGameBoolValue("EmuCore", "EnableWideScreenPatches", perGameWidescreen == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore", "EnableWideScreenPatches", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameNoInterlace != -1 {
            Self.setPerGameBoolValue("EmuCore", "EnableNoInterlacingPatches", perGameNoInterlace == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore", "EnableNoInterlacingPatches", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostBrightness != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Brightness", perGameShadeBoostBrightness, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Brightness", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostContrast != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Contrast", perGameShadeBoostContrast, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Contrast", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostSaturation != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Saturation", perGameShadeBoostSaturation, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Saturation", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostGamma != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Gamma", perGameShadeBoostGamma, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Gamma", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameDithering != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "dithering_ps2", perGameDithering, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "dithering_ps2", useCurrent: useCurrent, iso: iso)
        }
        // PerGameShaderSelection writes or clears all three shader keys together.
        if enabled && perGameShaderChain != -1 {
            PerGameShaderSelection.write(chain: perGameShaderChain, presetRef: perGameShaderPresetRef, useCurrent: useCurrent, iso: iso)
        } else {
            PerGameShaderSelection.clear(useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameFastForwardVolume != -1 {
            Self.setPerGameIntValue("SPU2/Output", "FastForwardVolume", perGameFastForwardVolume, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("SPU2/Output", "FastForwardVolume", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameIOP != -1 {
            Self.setPerGameBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", perGameIOP == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU/Recompiler", "EnableIOP", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU0 != -1 {
            Self.setPerGameBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", perGameVU0 == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU/Recompiler", "EnableVU0", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU1 != -1 {
            Self.setPerGameBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", perGameVU1 == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU/Recompiler", "EnableVU1", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameHWDownloadMode != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "HWDownloadMode", perGameHWDownloadMode, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "HWDownloadMode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameCPUCLUT != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "UserHacks_CPUCLUTRender", perGameCPUCLUT, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_CPUCLUTRender", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameGPUTargetCLUT != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", perGameGPUTargetCLUT, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVsyncQueue != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "VsyncQueueSize", perGameVsyncQueue, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "VsyncQueueSize", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameLoadTextureReplacements != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "LoadTextureReplacements", perGameLoadTextureReplacements == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "LoadTextureReplacements", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameLoadTextureReplacementsAsync != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "LoadTextureReplacementsAsync", perGameLoadTextureReplacementsAsync == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "LoadTextureReplacementsAsync", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePrecacheTextureReplacements != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "PrecacheTextureReplacements", perGamePrecacheTextureReplacements == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "PrecacheTextureReplacements", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameSyncToHostRefresh != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "SyncToHostRefreshRate", perGameSyncToHostRefresh == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "SyncToHostRefreshRate", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameBufferMS != -1 {
            Self.setPerGameIntValue("SPU2/Output", "BufferMS", perGameBufferMS, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("SPU2/Output", "BufferMS", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameOutputLatencyMS != -1 {
            Self.setPerGameIntValue("SPU2/Output", "OutputLatencyMS", perGameOutputLatencyMS, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("SPU2/Output", "OutputLatencyMS", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameFramePacingPreset != -1 {
            Self.setPerGameIntValue("ARMSX2iOS/FramePacing", "Preset", perGameFramePacingPreset, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("ARMSX2iOS/FramePacing", "Preset", useCurrent: useCurrent, iso: iso)
        }
        // Cascade a named preset's own keys. Use Global and .custom both miss the
        // table and fall through. After the individual writes on purpose, so the
        // preset's profile wins over stale per-game picker state.
        if enabled, let preset = FramePacingPreset(rawValue: perGameFramePacingPreset),
           let values = SettingsStore.framePacingPresetTable[preset] {
            Self.setPerGameIntValue("EmuCore/GS", "VsyncQueueSize", values.vsyncQueueSize, useCurrent: useCurrent, iso: iso)
            Self.setPerGameIntValue("SPU2/Output", "OutputLatencyMS", values.audioOutputLatencyMs, useCurrent: useCurrent, iso: iso)
            Self.setPerGameIntValue("SPU2/Output", "BufferMS", values.audioBufferMs, useCurrent: useCurrent, iso: iso)
            Self.setPerGameBoolValue("EmuCore/GS", "SyncToHostRefreshRate", values.syncToHostRefresh, useCurrent: useCurrent, iso: iso)
        }
        // Limiter state and presentation cadence are independent per-game overrides.
        Self.savePerGameFrameLimiter(preset: perGameFramePacingPreset, limiter: perGameFrameLimiter, targetFPS: perGameTargetFPS, enabled: enabled, useCurrent: useCurrent, iso: iso)
        if enabled && eeCycleSkip != -1 {
            Self.setPerGameIntValue("EmuCore/Speedhacks", "EECycleSkip", eeCycleSkip, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/Speedhacks", "EECycleSkip", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameEEFpuRound != -1 {
            Self.setPerGameIntValue("EmuCore/CPU", "FPU.Roundmode", perGameEEFpuRound, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU", "FPU.Roundmode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU0Round != -1 {
            Self.setPerGameIntValue("EmuCore/CPU", "VU0.Roundmode", perGameVU0Round, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU", "VU0.Roundmode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU1Round != -1 {
            Self.setPerGameIntValue("EmuCore/CPU", "VU1.Roundmode", perGameVU1Round, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU", "VU1.Roundmode", useCurrent: useCurrent, iso: iso)
        }
        Self.savePerGameEEClamp(perGameEEClamp, enabled: enabled, useCurrent: useCurrent, iso: iso)
        Self.savePerGameVUClamp(perGameVUClamp, enabled: enabled, useCurrent: useCurrent, iso: iso)
        if enabled && raEnabledOverride != -1 {
            Self.setPerGameBoolValue("Achievements", "Enabled", raEnabledOverride == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("Achievements", "Enabled", useCurrent: useCurrent, iso: iso)
        }
        if enabled && raHardcoreOverride != -1 {
            Self.setPerGameBoolValue("Achievements", "ChallengeMode", raHardcoreOverride == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("Achievements", "ChallengeMode", useCurrent: useCurrent, iso: iso)
        }
    }

    private func normalizedSkipDrawValues() -> (start: Int, end: Int) {
        let start = SettingsStore.clampedSkipDraw(skipDrawStart)
        let end = SettingsStore.normalizedSkipDrawEnd(
            start: start,
            end: skipDrawEnd,
            startOverride: skipDrawStartOverride,
            endOverride: skipDrawEndOverride
        )
        return (start, end)
    }

    private static func setPerGameBoolValue(_ section: String, _ key: String, _ value: Bool, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIBool(section, key: key, value: value, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func setPerGameIntValue(_ section: String, _ key: String, _ value: Int, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIInt(section, key: key, value: Int32(value), forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func setPerGameFloatValue(_ section: String, _ key: String, _ value: Float, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIFloat(section, key: key, value: value, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    /// Writes limiter state and presentation cadence independently so either
    /// picker can continue inheriting its global value.
    private static func savePerGameFrameLimiter(preset: Int, limiter: Int, targetFPS: Float, enabled: Bool, useCurrent: Bool, iso: String) {
        if enabled, let named = FramePacingPreset(rawValue: preset), let v = SettingsStore.framePacingPresetTable[named] {
            setPerGameFloatValue("Framerate", "NominalScalar", SettingsStore.nominalScalarForFrameLimiter(enabled: v.frameLimiterEnabled), useCurrent: useCurrent, iso: iso)
            setPerGameFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", Float(v.targetFPS), useCurrent: useCurrent, iso: iso)
            return
        }
        if enabled, limiter != -1 {
            setPerGameFloatValue("Framerate", "NominalScalar", SettingsStore.nominalScalarForFrameLimiter(enabled: limiter == 1), useCurrent: useCurrent, iso: iso)
        } else {
            clearPerGameValue("Framerate", "NominalScalar", useCurrent: useCurrent, iso: iso)
        }
        if enabled, targetFPS >= SettingsStore.minTargetFPS {
            setPerGameFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", targetFPS, useCurrent: useCurrent, iso: iso)
        } else {
            clearPerGameValue("ARMSX2iOS/FramePacing", "TargetFPS", useCurrent: useCurrent, iso: iso)
        }
    }

    private static func clearPerGameValue(_ section: String, _ key: String, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.deletePerGameINIValue(section, key: key, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func savePerGameEEClamp(_ mode: Int, enabled: Bool, useCurrent: Bool, iso: String) {
        if enabled && mode != -1 {
            setPerGameBoolValue("EmuCore/CPU/Recompiler", "fpuOverflow", mode >= 1, useCurrent: useCurrent, iso: iso)
            setPerGameBoolValue("EmuCore/CPU/Recompiler", "fpuExtraOverflow", mode >= 2, useCurrent: useCurrent, iso: iso)
            setPerGameBoolValue("EmuCore/CPU/Recompiler", "fpuFullMode", mode >= 3, useCurrent: useCurrent, iso: iso)
        } else {
            for key in ["fpuOverflow", "fpuExtraOverflow", "fpuFullMode"] {
                clearPerGameValue("EmuCore/CPU/Recompiler", key, useCurrent: useCurrent, iso: iso)
            }
        }
    }

    private static func savePerGameVUClamp(_ mode: Int, enabled: Bool, useCurrent: Bool, iso: String) {
        if enabled && mode != -1 {
            for prefix in ["vu0", "vu1"] {
                setPerGameBoolValue("EmuCore/CPU/Recompiler", "\(prefix)Overflow", mode >= 1, useCurrent: useCurrent, iso: iso)
                setPerGameBoolValue("EmuCore/CPU/Recompiler", "\(prefix)ExtraOverflow", mode >= 2, useCurrent: useCurrent, iso: iso)
                setPerGameBoolValue("EmuCore/CPU/Recompiler", "\(prefix)SignOverflow", mode >= 3, useCurrent: useCurrent, iso: iso)
            }
        } else {
            for prefix in ["vu0", "vu1"] {
                for suffix in ["Overflow", "ExtraOverflow", "SignOverflow"] {
                    clearPerGameValue("EmuCore/CPU/Recompiler", "\(prefix)\(suffix)", useCurrent: useCurrent, iso: iso)
                }
            }
        }
    }
}
