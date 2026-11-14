// PerGameSettingsDraft.swift — the values the per-game panel edits, loaded from the INI
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor
@Observable
final class PerGameSettingsDraft {
    private static let useGlobalSentinel = -1
    private static let upscaleUseGlobalSentinel: Float = -1.0
    private static let aspectUseGlobalSentinel = ""
    static let trilinearUseGlobalSentinel = Int(Int32.min)
    static let eeCycleRateUseGlobalSentinel = Int(Int32.min)
    static let fastBootUseGlobalSentinel = -1
    static let fastBootOff = 0
    static let fastBootOn = 1

    /// A single snapshot of the game's INI. Its presence, including an empty
    /// dictionary, means the bridge already resolved the serial/CRC and loaded
    /// the file. Missing keys therefore mean "Use Global" and must not fall
    /// back to another ISO scan.
    private struct PerGameINISnapshot {
        let values: [String: Any]

        init?(_ info: [String: Any]) {
            guard let values = info["perGameINI"] as? [String: Any] else {
                return nil
            }
            self.values = values
        }

        private func key(_ section: String, _ name: String) -> String {
            "\(section)\n\(name)"
        }

        func number(_ section: String, _ name: String) -> NSNumber? {
            values[key(section, name)] as? NSNumber
        }

    }

    let game: ISOEntry
    let savesToRunningGame: Bool
    let settings = SettingsStore.shared

    var enabled: Bool
    var upscaleMultiplier: Float
    var aspectRatio: String
    var textureFiltering: Int
    /// Tri-state: useGlobalSentinel, 0 off, 1 on. Was a Bool, which had no room to say "inherit".
    var hardwareMipmapping: Int
    var blendingAccuracy: Int
    var interlaceMode: Int
    var trilinearFiltering: Int
    var halfPixelOffset: Int
    var roundSprite: Int
    var alignSprite: Int
    var mergeSprite: Int
    var wildArmsOffset: Int
    var textureOffsetXOverride: Bool
    var textureOffsetX: Int
    var textureOffsetYOverride: Bool
    var textureOffsetY: Int
    var skipDrawStartOverride: Bool
    var skipDrawStart: Int
    var skipDrawEndOverride: Bool
    var skipDrawEnd: Int
    let globalVolumePercent: Int
    var volumeOverride: Bool
    var volumePercent: Int
    var padLayoutIdentity: PadLayoutGameIdentity?
    var eeCoreType: Int
    var mtvu: Bool
    let globalEECycleRate: Int
    var eeCycleRate: Int
    let globalEECycleSkip: Int
    var eeCycleSkip: Int
    let globalFastBoot: Bool
    var fastBoot: Int
    var hasGameSettingsIdentity: Bool
    var enableCheats: Bool
    var enablePatches: Bool
    var enableGameFixes: Bool
    var enableGameDBHardwareFixes: Bool
    // Per-game compatibility overrides (-1 = use global). Driven by the generic
    // per-game INI helper; -1 clears the per-game key so the global value applies.
    var perGameFixes: [String: Int]
    var perGameAAT: Int
    var perGameTextureInsideRt: Int
    var perGameDisableDepth: Int
    var perGameRenderer: Int
    var perGameFXAA: Int
    var perGameUpscaler: Int
    var perGameShadeBoost: Int
    var perGameTVShader: Int
    var perGameCASMode: Int
    var perGameMaxAnisotropy: Int
    var perGameCASSharpness: Int
    var perGamePCRTCOffsets: Int
    var perGameIntegerScaling: Int
    var perGameSkipDupFrames: Int
    var perGamePCRTCOverscan: Int
    var perGamePCRTCAntiBlur: Int
    var perGameDisableInterlaceOffset: Int
    var perGameWidescreen: Int
    var perGameNoInterlace: Int
    var perGameShadeBoostBrightness: Int
    var perGameShadeBoostContrast: Int
    var perGameShadeBoostSaturation: Int
    var perGameShadeBoostGamma: Int
    var perGameShaderChain: Int
    var perGameShaderPresetRef: String
    var perGameDithering: Int
    var perGameFastForwardVolume: Int
    var perGameIOP: Int
    var perGameVU0: Int
    var perGameVU1: Int
    var perGameEEFpuRound: Int
    var perGameVU0Round: Int
    var perGameVU1Round: Int
    var perGameEEClamp: Int
    var perGameVUClamp: Int
    let globalEEFpuRound: Int
    let globalVU0Round: Int
    let globalVU1Round: Int
    let globalEEClamp: Int
    let globalVUClamp: Int
    var perGameHWDownloadMode: Int
    var perGameCPUCLUT: Int
    var perGameGPUTargetCLUT: Int
    var perGameVsyncQueue: Int
    var perGameLoadTextureReplacements: Int
    var perGameLoadTextureReplacementsAsync: Int
    var perGamePrecacheTextureReplacements: Int
    var perGameSyncToHostRefresh: Int
    var perGameBufferMS: Int
    var perGameOutputLatencyMS: Int
    // Frame Pacing per-game overrides (-1 = use global). Limiter state remains
    // in Framerate/NominalScalar while presentation cadence has its own key.
    var perGameFramePacingPreset: Int
    var perGameFrameLimiter: Int
    var perGameTargetFPS: Float
    var statusMessage: String?
    var savedFingerprint = 0
    var shaderParameterPreviewSequence: UInt64 = 0
    var raEnabledOverride: Int
    var raHardcoreOverride: Int

    init(
        game: ISOEntry,
        preloadedSettings: [String: Any]?,
        savesToRunningGame: Bool,
        initiallySelectsGameController: Bool,
        initiallySelectsShaders: Bool
    ) {
        self.game = game
        self.savesToRunningGame = savesToRunningGame
        // In game the caller passes settings loaded through a VM-safe path, so this never re-scans the disc.
        let info = preloadedSettings ?? ARMSX2Bridge.gameSettings(forISO: game.bootName)
        let iniSnapshot = PerGameINISnapshot(info)
        enabled = initiallySelectsGameController
                || initiallySelectsShaders
                || Self.boolValue(info["enabled"], defaultValue: false)
        let inheritedVolume = SettingsStore.clampedEmulatorVolumePercent(Self.intValue(info["globalVolumePercent"], defaultValue: SettingsStore.defaultEmulatorVolumePercent))
        let loadedVolume = SettingsStore.clampedEmulatorVolumePercent(Self.intValue(info["volumePercent"], defaultValue: inheritedVolume))
        globalVolumePercent = inheritedVolume
        volumeOverride = Self.boolValue(info["hasVolumeOverride"], defaultValue: false)
        volumePercent = loadedVolume
        padLayoutIdentity = PadLayoutGameIdentity(
            serial: (info["serial"] as? String) ?? game.metadata["serial"],
            crc: (info["crc"] as? String) ?? game.metadata["crc"]
        )
        hasGameSettingsIdentity = !PadLayoutGameIdentity.normalizedCRC((info["crc"] as? String) ?? game.metadata["crc"]).isEmpty
        // Sentinel unless the file actually carries the key, so opening and saving the panel
        // cannot invent an override.
        upscaleMultiplier = Self.boolValue(info["hasUpscaleMultiplierOverride"], defaultValue: false) ? Self.floatValue(info["upscaleMultiplier"], defaultValue: 1.0) : Self.upscaleUseGlobalSentinel
        aspectRatio = Self.boolValue(info["hasAspectRatioOverride"], defaultValue: false) ? Self.normalizedAspect(info["aspectRatio"] as? String) : Self.aspectUseGlobalSentinel
        textureFiltering = Self.boolValue(info["hasTextureFilteringOverride"], defaultValue: false) ? Self.intValue(info["textureFiltering"], defaultValue: 2) : Self.useGlobalSentinel
        hardwareMipmapping = Self.boolValue(info["hasHardwareMipmappingOverride"], defaultValue: false) ? (Self.boolValue(info["hardwareMipmapping"], defaultValue: true) ? 1 : 0) : Self.useGlobalSentinel
        blendingAccuracy = Self.boolValue(info["hasBlendingAccuracyOverride"], defaultValue: false) ? Self.intValue(info["blendingAccuracy"], defaultValue: 1) : Self.useGlobalSentinel
        interlaceMode = Self.boolValue(info["hasInterlaceModeOverride"], defaultValue: false) ? Self.intValue(info["interlaceMode"], defaultValue: 0) : Self.useGlobalSentinel
        trilinearFiltering = Self.boolValue(info["hasTrilinearFilteringOverride"], defaultValue: false) ? Self.intValue(info["trilinearFiltering"], defaultValue: -1) : Self.trilinearUseGlobalSentinel
        halfPixelOffset = Self.boolValue(info["hasHalfPixelOffsetOverride"], defaultValue: false) ? Self.intValue(info["halfPixelOffset"], defaultValue: 0) : Self.useGlobalSentinel
        roundSprite = Self.boolValue(info["hasRoundSpriteOverride"], defaultValue: false) ? Self.intValue(info["roundSprite"], defaultValue: 0) : Self.useGlobalSentinel
        alignSprite = Self.boolValue(info["hasAlignSpriteOverride"], defaultValue: false) ? (Self.boolValue(info["alignSprite"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel
        mergeSprite = Self.boolValue(info["hasMergeSpriteOverride"], defaultValue: false) ? (Self.boolValue(info["mergeSprite"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel
        wildArmsOffset = Self.boolValue(info["hasWildArmsOffsetOverride"], defaultValue: false) ? (Self.boolValue(info["wildArmsOffset"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel
        textureOffsetXOverride = Self.boolValue(info["hasTextureOffsetXOverride"], defaultValue: false)
        textureOffsetX = SettingsStore.clampedTextureOffset(Self.intValue(info["textureOffsetX"], defaultValue: 0))
        textureOffsetYOverride = Self.boolValue(info["hasTextureOffsetYOverride"], defaultValue: false)
        textureOffsetY = SettingsStore.clampedTextureOffset(Self.intValue(info["textureOffsetY"], defaultValue: 0))
        let hasSkipDrawStartOverride = Self.boolValue(info["hasSkipDrawStartOverride"], defaultValue: false)
        let hasSkipDrawEndOverride = Self.boolValue(info["hasSkipDrawEndOverride"], defaultValue: false)
        let initialSkipDrawStart = SettingsStore.clampedSkipDraw(Self.intValue(info["skipDrawStart"], defaultValue: 0))
        let initialSkipDrawEnd = SettingsStore.normalizedSkipDrawEnd(
            start: initialSkipDrawStart,
            end: Self.intValue(info["skipDrawEnd"], defaultValue: 0),
            startOverride: hasSkipDrawStartOverride,
            endOverride: hasSkipDrawEndOverride
        )
        skipDrawStartOverride = hasSkipDrawStartOverride
        skipDrawStart = initialSkipDrawStart
        skipDrawEndOverride = hasSkipDrawEndOverride
        skipDrawEnd = initialSkipDrawEnd
        eeCoreType = Self.intValue(info["eeCoreType"], defaultValue: 2)
        mtvu = Self.boolValue(info["mtvu"], defaultValue: true)
        let inheritedEECycleRate = SettingsStore.clampedEECycleRate(Self.intValue(info["globalEECycleRate"], defaultValue: 0))
        globalEECycleRate = inheritedEECycleRate
        eeCycleRate = Self.boolValue(info["hasEECycleRateOverride"], defaultValue: false) ? SettingsStore.clampedEECycleRate(Self.intValue(info["eeCycleRate"], defaultValue: inheritedEECycleRate)) : Self.eeCycleRateUseGlobalSentinel
        let inheritedEECycleSkip = SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleSkip", defaultValue: 0)), to: 0...3)
        globalEECycleSkip = inheritedEECycleSkip
        let inheritedFastBoot = Self.boolValue(info["globalFastBoot"], defaultValue: false)
        globalFastBoot = inheritedFastBoot
        fastBoot = Self.boolValue(info["hasFastBootOverride"], defaultValue: false) ? (Self.boolValue(info["fastBoot"], defaultValue: inheritedFastBoot) ? Self.fastBootOn : Self.fastBootOff) : Self.fastBootUseGlobalSentinel
        enableCheats = Self.boolValue(info["enableCheats"], defaultValue: false)
        enablePatches = Self.boolValue(info["enablePatches"], defaultValue: true)
        enableGameFixes = Self.boolValue(info["enableGameFixes"], defaultValue: true)
        enableGameDBHardwareFixes = Self.boolValue(info["enableGameDBHardwareFixes"], defaultValue: true)

        // The bridge preloads the compatibility overrides in one INI read instead of one parse per key.
        var loadedFixes: [String: Int] = [:]
        let preloadedFixes = info["perGameFixes"] as? [String: Any] ?? [:]
        for option in SettingsStore.gameFixOptions {
            if let value = preloadedFixes[option.key] {
                loadedFixes[option.key] = Self.intValue(value, defaultValue: 0)
            } else {
                loadedFixes[option.key] = -1
            }
        }
        perGameFixes = loadedFixes
        let hasPerGameAAT = Self.boolValue(info["hasPerGameAAT"], defaultValue: false)
        perGameAAT = hasPerGameAAT ? Self.intValue(info["perGameAAT"], defaultValue: 0) : -1
        let hasPerGameTextureInsideRt = Self.boolValue(info["hasPerGameTextureInsideRt"], defaultValue: false)
        perGameTextureInsideRt = hasPerGameTextureInsideRt ? Self.intValue(info["perGameTextureInsideRt"], defaultValue: 0) : -1
        perGameDisableDepth = Self.boolValue(info["hasPerGameDisableDepth"], defaultValue: false) ? (Self.boolValue(info["perGameDisableDepth"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel
        let hasPerGameRenderer = Self.boolValue(info["hasPerGameRenderer"], defaultValue: false)
        perGameRenderer = hasPerGameRenderer ? Self.intValue(info["perGameRenderer"], defaultValue: 17) : -1
        let hasPerGameFXAA = Self.boolValue(info["hasPerGameFXAA"], defaultValue: false)
        perGameFXAA = hasPerGameFXAA ? Self.intValue(info["perGameFXAA"], defaultValue: 0) : -1
        let hasPerGameUpscaler = Self.boolValue(info["hasPerGameUpscaler"], defaultValue: false)
        perGameUpscaler = hasPerGameUpscaler ? Self.intValue(info["perGameUpscaler"], defaultValue: 0) : -1
        let hasPerGameShadeBoost = Self.boolValue(info["hasPerGameShadeBoost"], defaultValue: false)
        perGameShadeBoost = hasPerGameShadeBoost ? Self.intValue(info["perGameShadeBoost"], defaultValue: 0) : -1
        let hasPerGameTVShader = Self.boolValue(info["hasPerGameTVShader"], defaultValue: false)
        perGameTVShader = hasPerGameTVShader ? Self.intValue(info["perGameTVShader"], defaultValue: 0) : -1
        let hasPerGameCASMode = Self.boolValue(info["hasPerGameCASMode"], defaultValue: false)
        perGameCASMode = hasPerGameCASMode ? Self.intValue(info["perGameCASMode"], defaultValue: 0) : -1
        let hasPerGameMaxAnisotropy = Self.boolValue(info["hasPerGameMaxAnisotropy"], defaultValue: false)
        perGameMaxAnisotropy = hasPerGameMaxAnisotropy ? Self.intValue(info["perGameMaxAnisotropy"], defaultValue: 0) : -1
        let hasPerGameCASSharpness = Self.boolValue(info["hasPerGameCASSharpness"], defaultValue: false)
        perGameCASSharpness = hasPerGameCASSharpness ? SettingsStore.clamped(Self.intValue(info["perGameCASSharpness"], defaultValue: 50), to: SettingsStore.casSharpnessRange) : -1
        let hasPerGamePCRTCOffsets = Self.boolValue(info["hasPerGamePCRTCOffsets"], defaultValue: false)
        perGamePCRTCOffsets = hasPerGamePCRTCOffsets ? Self.intValue(info["perGamePCRTCOffsets"], defaultValue: 0) : -1
        let hasPerGameIntegerScaling = Self.boolValue(info["hasPerGameIntegerScaling"], defaultValue: false)
        perGameIntegerScaling = hasPerGameIntegerScaling ? Self.intValue(info["perGameIntegerScaling"], defaultValue: 0) : -1
        let hasPerGameSkipDupFrames = Self.boolValue(info["hasPerGameSkipDupFrames"], defaultValue: false)
        perGameSkipDupFrames = hasPerGameSkipDupFrames ? Self.intValue(info["perGameSkipDupFrames"], defaultValue: 1) : -1

        let hasPerGamePCRTCOverscan = Self.boolValue(info["hasPerGamePCRTCOverscan"], defaultValue: false)
        perGamePCRTCOverscan = hasPerGamePCRTCOverscan ? Self.intValue(info["perGamePCRTCOverscan"], defaultValue: 0) : -1

        let hasPerGamePCRTCAntiBlur = Self.boolValue(info["hasPerGamePCRTCAntiBlur"], defaultValue: false)
        perGamePCRTCAntiBlur = hasPerGamePCRTCAntiBlur ? Self.intValue(info["perGamePCRTCAntiBlur"], defaultValue: 1) : -1

        let hasPerGameDisableInterlaceOffset = Self.boolValue(info["hasPerGameDisableInterlaceOffset"], defaultValue: false)
        perGameDisableInterlaceOffset = hasPerGameDisableInterlaceOffset ? Self.intValue(info["perGameDisableInterlaceOffset"], defaultValue: 0) : -1
        let perGameISO = game.bootName
        let useCurrent = savesToRunningGame
        eeCycleSkip = Self.loadedPerGameInt("EmuCore/Speedhacks", "EECycleSkip", globalDefault: Int32(inheritedEECycleSkip), useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameWidescreen = Self.loadedPerGameBool("EmuCore", "EnableWideScreenPatches", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameNoInterlace = Self.loadedPerGameBool("EmuCore", "EnableNoInterlacingPatches", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameShadeBoostBrightness = Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Brightness", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameShadeBoostContrast = Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Contrast", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameShadeBoostSaturation = Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Saturation", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameShadeBoostGamma = Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Gamma", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameShaderChain = PerGameShaderSelection.loadedChain(useCurrent: useCurrent, iso: perGameISO)
        perGameShaderPresetRef = PerGameShaderSelection.loadedPresetRef(useCurrent: useCurrent, iso: perGameISO)
        perGameDithering = Self.loadedPerGameInt("EmuCore/GS", "dithering_ps2", globalDefault: 2, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameFastForwardVolume = Self.clampedPerGameInt(Self.loadedPerGameInt("SPU2/Output", "FastForwardVolume", globalDefault: 100, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.fastForwardVolumeRange)
        perGameIOP = Self.loadedPerGameBool("EmuCore/CPU/Recompiler", "EnableIOP", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameVU0 = Self.loadedPerGameBool("EmuCore/CPU/Recompiler", "EnableVU0", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameVU1 = Self.loadedPerGameBool("EmuCore/CPU/Recompiler", "EnableVU1", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameEEFpuRound = Self.loadedPerGameInt("EmuCore/CPU", "FPU.Roundmode", globalDefault: 3, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameVU0Round = Self.loadedPerGameInt("EmuCore/CPU", "VU0.Roundmode", globalDefault: 3, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameVU1Round = Self.loadedPerGameInt("EmuCore/CPU", "VU1.Roundmode", globalDefault: 3, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameEEClamp = Self.loadedPerGameEEClamp(useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameVUClamp = Self.loadedPerGameVUClamp(useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        globalEEFpuRound = SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "FPU.Roundmode", defaultValue: 3)), to: 0...3)
        globalVU0Round = SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "VU0.Roundmode", defaultValue: 3)), to: 0...3)
        globalVU1Round = SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "VU1.Roundmode", defaultValue: 3)), to: 0...3)
        globalEEClamp = SettingsStore.eeClampModeFromBools(
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuOverflow", defaultValue: true),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuExtraOverflow", defaultValue: false),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuFullMode", defaultValue: false))
        globalVUClamp = SettingsStore.vuClampModeFromBools(
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0Overflow", defaultValue: true),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0ExtraOverflow", defaultValue: false),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0SignOverflow", defaultValue: false))
        perGameHWDownloadMode = Self.loadedPerGameInt("EmuCore/GS", "HWDownloadMode", globalDefault: 0, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameCPUCLUT = Self.loadedPerGameInt("EmuCore/GS", "UserHacks_CPUCLUTRender", globalDefault: 0, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameGPUTargetCLUT = Self.loadedPerGameInt("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", globalDefault: 0, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameVsyncQueue = Self.clampedPerGameInt(Self.loadedPerGameInt("EmuCore/GS", "VsyncQueueSize", globalDefault: 8, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.vsyncQueueRange)
        perGameLoadTextureReplacements = Self.loadedPerGameBool("EmuCore/GS", "LoadTextureReplacements", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameLoadTextureReplacementsAsync = Self.loadedPerGameBool("EmuCore/GS", "LoadTextureReplacementsAsync", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGamePrecacheTextureReplacements = Self.loadedPerGameBool("EmuCore/GS", "PrecacheTextureReplacements", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameSyncToHostRefresh = Self.loadedPerGameBool("EmuCore/GS", "SyncToHostRefreshRate", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameBufferMS = Self.clampedPerGameInt(Self.loadedPerGameInt("SPU2/Output", "BufferMS", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.audioBufferMsRange)
        perGameOutputLatencyMS = Self.clampedPerGameInt(Self.loadedPerGameInt("SPU2/Output", "OutputLatencyMS", globalDefault: 20, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.audioOutputLatencyMsRange)
        let _globalFramePacingPreset = Int32(SettingsStore.shared.framePacingPreset.rawValue)
        perGameFramePacingPreset = Self.loadedPerGameInt("ARMSX2iOS/FramePacing", "Preset", globalDefault: _globalFramePacingPreset, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        let _fpLimiter = Self.loadedPerGameFrameLimiter(useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        perGameFrameLimiter = _fpLimiter.limiter
        perGameTargetFPS = _fpLimiter.fps
        raEnabledOverride = Self.loadedPerGameBool("Achievements", "Enabled", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        raHardcoreOverride = Self.loadedPerGameBool("Achievements", "ChallengeMode", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        let initialFingerprint = perGameFingerprint()
        savedFingerprint = initialFingerprint
    }

    /// Encodes the current editable per-game state so Save can be gated on real changes.
    /// Virtual Pad values are left out on purpose: that tab writes as you edit it, so
    /// there is never anything of its own left for Save to commit.
    func perGameFingerprint() -> Int {
        var hasher = Hasher()
        hasher.combine(enabled)
        hasher.combine(textureOffsetXOverride)
        hasher.combine(textureOffsetYOverride)
        hasher.combine(skipDrawStartOverride)
        hasher.combine(skipDrawEndOverride)
        hasher.combine(volumeOverride)
        hasher.combine(perGameLivePreviewFingerprint())
        return hasher.finalize()
    }

    /// The override master switch changes what Save persists, but it is not a
    /// visual/emulation value which should interrupt gameplay for a preview.
    func perGameLivePreviewFingerprint() -> Int {
        var hasher = Hasher()
        hasher.combine(upscaleMultiplier)
        hasher.combine(aspectRatio)
        hasher.combine(textureFiltering)
        hasher.combine(hardwareMipmapping)
        hasher.combine(blendingAccuracy)
        hasher.combine(interlaceMode)
        hasher.combine(trilinearFiltering)
        hasher.combine(halfPixelOffset)
        hasher.combine(roundSprite)
        hasher.combine(alignSprite)
        hasher.combine(mergeSprite)
        hasher.combine(wildArmsOffset)
        hasher.combine(textureOffsetX)
        hasher.combine(textureOffsetY)
        hasher.combine(skipDrawStart)
        hasher.combine(skipDrawEnd)
        hasher.combine(volumePercent)
        hasher.combine(eeCoreType)
        hasher.combine(mtvu)
        hasher.combine(eeCycleRate)
        hasher.combine(eeCycleSkip)
        hasher.combine(fastBoot)
        hasher.combine(enableCheats)
        hasher.combine(enablePatches)
        hasher.combine(enableGameFixes)
        hasher.combine(enableGameDBHardwareFixes)
        hasher.combine(perGameAAT)
        hasher.combine(perGameTextureInsideRt)
        hasher.combine(perGameDisableDepth)
        hasher.combine(perGameRenderer)
        hasher.combine(perGameFXAA)
        hasher.combine(perGameUpscaler)
        hasher.combine(perGameShadeBoost)
        hasher.combine(perGameTVShader)
        hasher.combine(perGameCASMode)
        hasher.combine(perGameMaxAnisotropy)
        hasher.combine(perGameCASSharpness)
        hasher.combine(perGamePCRTCOffsets)
        hasher.combine(perGameIntegerScaling)
        hasher.combine(perGameSkipDupFrames)
        hasher.combine(perGamePCRTCOverscan)
        hasher.combine(perGamePCRTCAntiBlur)
        hasher.combine(perGameDisableInterlaceOffset)
        hasher.combine(perGameWidescreen)
        hasher.combine(perGameNoInterlace)
        hasher.combine(perGameShadeBoostBrightness)
        hasher.combine(perGameShadeBoostContrast)
        hasher.combine(perGameShadeBoostSaturation)
        hasher.combine(perGameShadeBoostGamma)
        hasher.combine(perGameShaderChain)
        hasher.combine(perGameShaderPresetRef)
        hasher.combine(shaderParameterPreviewSequence)
        hasher.combine(perGameDithering)
        hasher.combine(perGameFastForwardVolume)
        hasher.combine(perGameIOP)
        hasher.combine(perGameVU0)
        hasher.combine(perGameVU1)
        hasher.combine(perGameHWDownloadMode)
        hasher.combine(perGameCPUCLUT)
        hasher.combine(perGameGPUTargetCLUT)
        hasher.combine(perGameVsyncQueue)
        hasher.combine(perGameLoadTextureReplacements)
        hasher.combine(perGameLoadTextureReplacementsAsync)
        hasher.combine(perGamePrecacheTextureReplacements)
        hasher.combine(perGameSyncToHostRefresh)
        hasher.combine(perGameBufferMS)
        hasher.combine(perGameOutputLatencyMS)
        hasher.combine(perGameEEFpuRound)
        hasher.combine(perGameVU0Round)
        hasher.combine(perGameVU1Round)
        hasher.combine(perGameEEClamp)
        hasher.combine(perGameVUClamp)
        hasher.combine(raEnabledOverride)
        hasher.combine(raHardcoreOverride)
        hasher.combine(perGameFramePacingPreset)
        hasher.combine(perGameFrameLimiter)
        hasher.combine(perGameTargetFPS)
        for option in SettingsStore.gameFixOptions {
            hasher.combine(perGameFixes[option.key] ?? -1)
        }
        return hasher.finalize()
    }

    var hasPendingChanges: Bool {
        perGameFingerprint() != savedFingerprint
    }

    var displayName: String {
        let name = ((game.name as NSString).deletingPathExtension as String).trimmingCharacters(in: .whitespacesAndNewlines)
        let serial = game.metadata["serial"]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""

        if name.isEmpty {
            return serial.isEmpty ? settings.localized("Current Game") : serial
        }
        if serial.isEmpty {
            return name
        }
        return "\(name) - \(serial)"
    }

    // MARK: - Per-game INI helpers
    // `useCurrent` picks the VM-safe current-game accessor, which live-applies; otherwise
    // the ISO variant writes the per-game file without applying.

    static func targetISO(useCurrent: Bool, iso: String) -> String? {
        useCurrent ? nil : iso
    }

    /// Reads a per-game int override; returns -1 ("use global") when no per-game key is set.
    private static func loadedPerGameInt(
        _ section: String,
        _ key: String,
        globalDefault: Int32,
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        if let snapshot {
            return snapshot.number(section, key)?.intValue
                ?? useGlobalSentinel
        }
        let target = targetISO(useCurrent: useCurrent, iso: iso)
        guard ARMSX2Bridge.hasPerGameINIValue(section, key: key, forISO: target) else { return -1 }
        return Int(ARMSX2Bridge.getPerGameINIInt(section, key: key, defaultValue: globalDefault, forISO: target))
    }

    /// Pin a loaded value to what its control can show. The sentinel is not a value, so it is
    /// left alone; anything else has to fit or the stepper and the stored number disagree.
    private static func clampedPerGameInt(_ value: Int, to range: ClosedRange<Int>) -> Int {
        value == useGlobalSentinel ? value : SettingsStore.clamped(value, to: range)
    }

    /// Reads a per-game bool override; returns -1 ("use global"), 0 (off), or 1 (on).
    private static func loadedPerGameBool(
        _ section: String,
        _ key: String,
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        if let snapshot {
            guard let value = snapshot.number(section, key) else {
                return useGlobalSentinel
            }
            return value.boolValue ? 1 : 0
        }
        let target = targetISO(useCurrent: useCurrent, iso: iso)
        guard ARMSX2Bridge.hasPerGameINIValue(section, key: key, forISO: target) else { return -1 }
        return ARMSX2Bridge.getPerGameINIBool(section, key: key, defaultValue: false, forISO: target) ? 1 : 0
    }

    /// Reads independent per-game limiter and presentation-cadence overrides.
    /// Legacy files are interpreted correctly until the native runtime or Save
    /// migrates their encoded target into the dedicated cadence key.
    private static func loadedPerGameFrameLimiter(
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> (limiter: Int, fps: Float) {
        let scalarPresent: Bool
        let scalar: Float
        let targetPresent: Bool
        let storedTarget: Float
        if let snapshot {
            let scalarNumber = snapshot.number("Framerate", "NominalScalar")
            scalarPresent = scalarNumber != nil
            scalar = scalarNumber?.floatValue ?? 1.0
            let targetNumber = snapshot.number(
                "ARMSX2iOS/FramePacing",
                "TargetFPS"
            )
            targetPresent = targetNumber != nil
            storedTarget = targetNumber?.floatValue
                ?? SettingsStore.defaultTargetFPS
        } else {
            let target = targetISO(useCurrent: useCurrent, iso: iso)
            scalarPresent = ARMSX2Bridge.hasPerGameINIValue("Framerate", key: "NominalScalar", forISO: target)
            scalar = scalarPresent ? ARMSX2Bridge.getPerGameINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0, forISO: target) : 1.0
            targetPresent = ARMSX2Bridge.hasPerGameINIValue("ARMSX2iOS/FramePacing", key: "TargetFPS", forISO: target)
            storedTarget = targetPresent ? ARMSX2Bridge.getPerGameINIFloat("ARMSX2iOS/FramePacing", key: "TargetFPS", defaultValue: SettingsStore.defaultTargetFPS, forISO: target) : SettingsStore.defaultTargetFPS
        }

        let limiter = scalarPresent ? (SettingsStore.frameLimiterEnabled(fromNominalScalar: scalar) ? 1 : 0) : -1
        if targetPresent {
            let fps = min(
                max((storedTarget * 1_000.0).rounded() / 1_000.0, SettingsStore.minTargetFPS),
                SettingsStore.maxTargetFPS)
            return (limiter, fps)
        }

        // Older per-game files stored target/base directly in NominalScalar.
        if scalarPresent, limiter == 1, abs(scalar - 1.0) >= 0.002 {
            let legacyFPS = SettingsStore.targetFPS(
                fromNominalScalar: scalar,
                baseFramerate: SettingsStore.shared.ntscFramerate)
            return (limiter, max(legacyFPS, SettingsStore.minTargetFPS))
        }
        return (limiter, -1.0)
    }

    private static func loadedPerGameEEClamp(
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        let overflow = loadedPerGameBool("EmuCore/CPU/Recompiler", "fpuOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot)
        guard overflow != -1 else { return -1 }
        let extra = loadedPerGameBool("EmuCore/CPU/Recompiler", "fpuExtraOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        let full = loadedPerGameBool("EmuCore/CPU/Recompiler", "fpuFullMode", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        return SettingsStore.eeClampModeFromBools(overflow == 1, extra, full)
    }

    private static func loadedPerGameVUClamp(
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        let overflow = loadedPerGameBool("EmuCore/CPU/Recompiler", "vu0Overflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot)
        guard overflow != -1 else { return -1 }
        let extra = loadedPerGameBool("EmuCore/CPU/Recompiler", "vu0ExtraOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        let sign = loadedPerGameBool("EmuCore/CPU/Recompiler", "vu0SignOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        return SettingsStore.vuClampModeFromBools(overflow == 1, extra, sign)
    }

    private static func normalizedAspect(_ value: String?) -> String {
        switch value {
        case "Stretch", "4:3", "16:9", "10:7":
            return value ?? "Auto 4:3/3:2"
        default:
            return "Auto 4:3/3:2"
        }
    }

    private static func boolValue(_ value: Any?, defaultValue: Bool) -> Bool {
        if let bool = value as? Bool {
            return bool
        }
        if let number = value as? NSNumber {
            return number.boolValue
        }
        return defaultValue
    }

    private static func intValue(_ value: Any?, defaultValue: Int) -> Int {
        if let number = value as? NSNumber {
            return number.intValue
        }
        return defaultValue
    }

    private static func floatValue(_ value: Any?, defaultValue: Float) -> Float {
        if let number = value as? NSNumber {
            return number.floatValue
        }
        return defaultValue
    }
}
