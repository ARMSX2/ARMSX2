// SettingsStore.swift — INI-backed settings for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

@MainActor
@Observable
final class SettingsStore {
    static let shared = SettingsStore()

    @ObservationIgnored private(set) var suppressINIWrites = false
    @ObservationIgnored var isProgrammaticOsdFlagChange = false
    @ObservationIgnored var isAutoMarkingCustom = false
    @ObservationIgnored var isProgrammaticFramePacingFlagChange = false
    @ObservationIgnored var isAutoMarkingFramePacingCustom = false
    @ObservationIgnored var graphicsApplyWorkItem: DispatchWorkItem?
    @ObservationIgnored var visualSliderDragCount = 0
    @ObservationIgnored var graphicsApplyDeferred = false
    @ObservationIgnored var visualSliderWatchdog: DispatchWorkItem?

    // ── Emulator / CPU ──
    var eeCoreType: Int {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIInt("EmuCore/CPU", key: "CoreType", value: Int32(eeCoreType))
            ARMSX2Bridge.setINIBool("EmuCore/CPU", key: "UseArm64Dynarec", value: eeCoreType == 2)
        }
    }
    var iopRecompiler: Bool = true { didSet { commit(Self._iopRecompilerConfig, iopRecompiler) } }
    var vu0Recompiler: Bool = true { didSet { commit(Self._vu0RecompilerConfig, vu0Recompiler) } }
    var vu1Recompiler: Bool = true { didSet { commit(Self._vu1RecompilerConfig, vu1Recompiler) } }
    var fastBoot: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("GameISO", key: "FastBoot", value: fastBoot)
            ARMSX2Bridge.setINIBool("EmuCore", key: "EnableFastBoot", value: fastBoot)
        }
    }
    var automaticLoadLastSaveState = false {
        didSet { commit(Self._automaticLoadLastSaveStateConfig, automaticLoadLastSaveState) }
    }
    var autoSaveEnabled = true {
        didSet { commit(Self._autoSaveEnabledConfig, autoSaveEnabled) }
    }
    var autoSaveIntervalMinutes = 10 {
        didSet { commit(Self._autoSaveIntervalConfig, autoSaveIntervalMinutes) }
    }
    var autoSaveOnLeave = true {
        didSet { commit(Self._autoSaveOnLeaveConfig, autoSaveOnLeave) }
    }
    var autoSaveOnLowBattery = true {
        didSet { commit(Self._autoSaveOnLowBatteryConfig, autoSaveOnLowBattery) }
    }
    var undoSeconds = 5 {
        didSet { commit(Self._undoSecondsConfig, undoSeconds) }
    }
    var automaticLoadLastGame = false {
        didSet { commit(Self._automaticLoadLastGameConfig, automaticLoadLastGame) }
    }
    var fastmem: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/Speedhacks", key: "ManualFastmem", value: true)
            ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", value: fastmem)
        }
    }
    var emulationOnlyModeEnabled: Bool = false { didSet { commit(Self._emulationOnlyModeConfig, emulationOnlyModeEnabled) } }
    var emulationOnlyDisablePatches: Bool = true {
        didSet { commit(Self._emulationOnlyDisablePatchesConfig, emulationOnlyDisablePatches) }
    }
    // Discord presence is always released by Emulation-Only Mode.
    let emulationOnlyDisableDiscordPresence = true
    var emulationOnlyDisablePINE: Bool = true {
        didSet { commit(Self._emulationOnlyDisablePINEConfig, emulationOnlyDisablePINE) }
    }
    var emulationOnlyDisableRetroAchievements: Bool = true {
        didSet { commit(Self._emulationOnlyDisableRetroAchievementsConfig, emulationOnlyDisableRetroAchievements) }
    }
    var emulationOnlyDisableInputRecording: Bool = true {
        didSet { commit(Self._emulationOnlyDisableInputRecordingConfig, emulationOnlyDisableInputRecording) }
    }
    var emulationOnlyDisableOSD: Bool = true {
        didSet { commit(Self._emulationOnlyDisableOSDConfig, emulationOnlyDisableOSD) }
    }
    var emulationOnlyDisableFramePacing: Bool = true {
        didSet { commit(Self._emulationOnlyDisableFramePacingConfig, emulationOnlyDisableFramePacing) }
    }
    var emulationOnlyDisableVirtualControls: Bool = true {
        didSet { commit(Self._emulationOnlyDisableVirtualControlsConfig, emulationOnlyDisableVirtualControls) }
    }
    var emulationOnlyDisableQuickMenu: Bool = true {
        didSet { commit(Self._emulationOnlyDisableQuickMenuConfig, emulationOnlyDisableQuickMenu) }
    }
    var emulationOnlyClearNetworkCache: Bool = true {
        didSet { commit(Self._emulationOnlyClearNetworkCacheConfig, emulationOnlyClearNetworkCache) }
    }
    var emulationOnlyModeDelaySeconds = SettingsStore.defaultEmulationOnlyModeDelaySeconds { didSet {
        let clamped = Self.clamped(emulationOnlyModeDelaySeconds, to: Self.emulationOnlyModeDelayRange)
        guard emulationOnlyModeDelaySeconds == clamped else {
            emulationOnlyModeDelaySeconds = clamped
            return
        }
        commit(Self._emulationOnlyModeDelayConfig, emulationOnlyModeDelaySeconds)
    }}

    // ── CPU Rounding & Clamping ──
    var eeFpuRoundMode: Int = 3 { didSet { commit(Self._eeFpuRoundModeConfig, eeFpuRoundMode) } }
    var vu0RoundMode: Int = 3 { didSet { commit(Self._vu0RoundModeConfig, vu0RoundMode) } }
    var vu1RoundMode: Int = 3 { didSet { commit(Self._vu1RoundModeConfig, vu1RoundMode) } }
    var eeClampMode: Int {
        didSet {
            guard !suppressINIWrites else { return }
            Self.applyEEClampMode(Self.clamped(eeClampMode, to: 0...3))
        }
    }
    var vuClampMode: Int {
        didSet {
            guard !suppressINIWrites else { return }
            Self.applyVUClampMode(Self.clamped(vuClampMode, to: 0...3))
        }
    }
    var frameLimiterEnabled: Bool {
        didSet {
            applyFrameLimiterSettings()
            if frameLimiterEnabled != oldValue { markFramePacingCustom() }
        }
    }
    var fastForwardRuntimeEnabled = false
    var targetFPS: Float {
        didSet {
            let normalized = Self.clampedTargetFPS(targetFPS)
            guard abs(targetFPS - normalized) <= 0.001 else {
                targetFPS = normalized
                return
            }
            applyFrameLimiterSettings()
            // Compare against the clamped old value: this didSet re-enters after clamping, so
            // oldValue is the unclamped intermediate, and clampedTargetFPS rounds.
            if abs(targetFPS - Self.clampedTargetFPS(oldValue)) > 0.001 { markFramePacingCustom() }
        }
    }
    var fastForwardScalar: Float {
        didSet {
            let normalized = Self.clampedSpeedScalar(fastForwardScalar)
            guard abs(fastForwardScalar - normalized) <= 0.001 else {
                fastForwardScalar = normalized
                return
            }
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("Framerate", key: "TurboScalar", value: fastForwardScalar)
        }
    }
    var emulatorVolumePercent: Int {
        didSet {
            let normalized = Self.clampedEmulatorVolumePercent(emulatorVolumePercent)
            guard emulatorVolumePercent == normalized else {
                emulatorVolumePercent = normalized
                return
            }
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setEmulatorVolumePercent(Int32(normalized))
        }
    }

    // ── Audio Output (SPU2/Output) ── applied live by the SPU2 stream.
    var audioTimeStretch: Bool = true { didSet { commit(Self._audioTimeStretchConfig, audioTimeStretch) } }
    var audioBufferMs: Int = 50 { didSet {
        commit(Self._audioBufferMsConfig, audioBufferMs)
        if audioBufferMs != oldValue { markFramePacingCustom() }
    }}
    var audioOutputLatencyMs: Int = 20 { didSet {
        commit(Self._audioOutputLatencyMsConfig, audioOutputLatencyMs)
        if audioOutputLatencyMs != oldValue { markFramePacingCustom() }
    }}
    var audioFastForwardVolume: Int = 100 { didSet { commit(Self._audioFastForwardVolumeConfig, audioFastForwardVolume) } }
    var audioSwapChannels: Bool = false { didSet { commit(Self._audioSwapChannelsConfig, audioSwapChannels) } }
    var ntscFramerate: Float {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "FramerateNTSC", value: ntscFramerate)
            applyFrameLimiterSettings()
            requestGraphicsApplyGuarded()
        }
    }
    var palFramerate: Float = 50.0 { didSet { commit(Self._palFramerateConfig, palFramerate) } }

    var fastCDVD: Bool = false { didSet { commit(Self._fastCDVDConfig, fastCDVD) } }

    // ── Advanced Speedhacks ──
    var eeCycleRate: Int = 0 { didSet { commit(Self._eeCycleRateConfig, eeCycleRate) } }
    var vu1Instant: Bool = true { didSet { commit(Self._vu1InstantConfig, vu1Instant) } }
    var mtvu: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/Speedhacks", key: "ManualMTVU", value: true)
            ARMSX2Bridge.setINIInt("ARMSX2iOS/Speedhacks", key: "ManualMTVUVersion", value: 3)
            ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vuThread", value: mtvu)
        }
    }
    var waitLoop: Bool = true { didSet { commit(Self._waitLoopConfig, waitLoop) } }
    var intcStat: Bool = true { didSet { commit(Self._intcStatConfig, intcStat) } }
    var eeCycleSkip: Int = 0 { didSet { commit(Self._eeCycleSkipConfig, eeCycleSkip) } }
    var vuFlagHack: Bool = true { didSet { commit(Self._vuFlagHackConfig, vuFlagHack) } }
    var enableCheats: Bool = false { didSet { commit(Self._enableCheatsConfig, enableCheats) } }
    var enablePatches: Bool = true { didSet { commit(Self._enablePatchesConfig, enablePatches) } }
    var enableGameFixes: Bool = true { didSet { commit(Self._enableGameFixesConfig, enableGameFixes) } }
    var enableGameDBHardwareFixes: Bool = true {
        didSet { commit(Self._enableGameDBHardwareFixesConfig, enableGameDBHardwareFixes) }
    }
    var enableWidescreenPatches: Bool = false {
        didSet { commit(Self._enableWidescreenPatchesConfig, enableWidescreenPatches) }
    }
    var enableNoInterlacingPatches: Bool = false {
        didSet { commit(Self._enableNoInterlacingPatchesConfig, enableNoInterlacingPatches) }
    }
    var hostFilesystem: Bool = false { didSet { commit(Self._hostFilesystemConfig, hostFilesystem) } }
    var autoTune: Bool = false { didSet { commit(Self._autoTuneConfig, autoTune) } }

    // ── Manual Game Fixes (EmuCore/Gamefixes/<key>) ──
    // Dictionary-backed because the 17 fixes are alike; each writes only its own INI key,
    // and they apply only while GameDB Core Fixes (enableGameFixes) is on.
    var gameFixes: [String: Bool] = [:]

    var renderer: Int = 17 { didSet { commit(Self._rendererConfig, renderer) } }
    var upscaleMultiplier: Float = 1.0 { didSet { commit(Self._upscaleMultiplierConfig, upscaleMultiplier) } }
    var vsyncQueueSize: Int = 8 { didSet {
        commit(Self._vsyncQueueSizeConfig, vsyncQueueSize)
        if vsyncQueueSize != oldValue { markFramePacingCustom() }
    }}
    var textureFiltering: Int = 2 { didSet { commit(Self._textureFilteringConfig, textureFiltering) } }
    var backThreadMode: Int = 0 { didSet { commit(Self._backThreadModeConfig, backThreadMode) } }
    var hardwareMipmapping: Bool = true { didSet { commit(Self._hardwareMipmappingConfig, hardwareMipmapping) } }
    var fxaa: Bool = false { didSet { commit(Self._fxaaConfig, fxaa) } }
    var casMode: Int = 0 { didSet { commit(Self._casModeConfig, casMode) } }
    var casSharpness: Int = 50 { didSet { commit(Self._casSharpnessConfig, casSharpness) } }
    var shaderChainEnabled: Bool = false { didSet {
        commit(Self._shaderChainEnabledConfig, shaderChainEnabled)
        ARMSX2Bridge.retryShaderChain()
    }}
    var shaderChainPresetRef: String = "" { didSet {
        commit(Self._shaderChainPresetRefConfig, shaderChainPresetRef)
        applyShaderChainSelection()
    }}
    var interlaceMode: Int = 0 { didSet { commit(Self._interlaceModeConfig, interlaceMode) } }
    var aspectRatio: Int = 1 { didSet { commit(Self._aspectRatioConfig, aspectRatio) } }
    var blendingAccuracy: Int = 1 { didSet { commit(Self._blendingAccuracyConfig, blendingAccuracy) } }
    var dithering: Int = 2 { didSet { commit(Self._ditheringConfig, dithering) } }
    var trilinearFiltering: Int = -1 { didSet { commit(Self._trilinearFilteringConfig, trilinearFiltering) } }
    var halfPixelOffset: Int = 0 { didSet { commit(Self._halfPixelOffsetConfig, halfPixelOffset) } }
    var roundSprite: Int = 0 { didSet { commit(Self._roundSpriteConfig, roundSprite) } }
    var alignSprite: Bool = false { didSet { commit(Self._alignSpriteConfig, alignSprite) } }
    var mergeSprite: Bool = false { didSet { commit(Self._mergeSpriteConfig, mergeSprite) } }
    var wildArmsOffset: Bool = false { didSet { commit(Self._wildArmsOffsetConfig, wildArmsOffset) } }
    var textureOffsetX: Int {
        didSet {
            let normalized = Self.clampedTextureOffset(textureOffsetX)
            guard textureOffsetX == normalized else {
                textureOffsetX = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_TCOffsetX", value: Int32(textureOffsetX))
            requestGraphicsApplyGuarded()
        }
    }
    var textureOffsetY: Int {
        didSet {
            let normalized = Self.clampedTextureOffset(textureOffsetY)
            guard textureOffsetY == normalized else {
                textureOffsetY = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_TCOffsetY", value: Int32(textureOffsetY))
            requestGraphicsApplyGuarded()
        }
    }
    var skipDrawStart: Int {
        didSet {
            let normalized = Self.clampedSkipDraw(skipDrawStart)
            guard skipDrawStart == normalized else {
                skipDrawStart = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_Start", value: Int32(skipDrawStart))
            requestGraphicsApplyGuarded()
        }
    }
    var skipDrawEnd: Int {
        didSet {
            let normalized = Self.clampedSkipDraw(skipDrawEnd)
            guard skipDrawEnd == normalized else {
                skipDrawEnd = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_End", value: Int32(skipDrawEnd))
            requestGraphicsApplyGuarded()
        }
    }
    var loadTextureReplacements: Bool = false {
        didSet { commit(Self._loadTextureReplacementsConfig, loadTextureReplacements) }
    }
    var loadTextureReplacementsAsync: Bool = true {
        didSet { commit(Self._loadTextureReplacementsAsyncConfig, loadTextureReplacementsAsync) }
    }
    var precacheTextureReplacements: Bool = false {
        didSet { commit(Self._precacheTextureReplacementsConfig, precacheTextureReplacements) }
    }
    var texturePreloading: Int = 2 { didSet { commit(Self._texturePreloadingConfig, texturePreloading) } }
    var dumpReplaceableTextures: Bool = false {
        didSet { commit(Self._dumpReplaceableTexturesConfig, dumpReplaceableTextures) }
    }
    var dumpReplaceableMipmaps: Bool = false {
        didSet { commit(Self._dumpReplaceableMipmapsConfig, dumpReplaceableMipmaps) }
    }
    var dumpTexturesWithFMVActive: Bool = false {
        didSet { commit(Self._dumpTexturesWithFMVActiveConfig, dumpTexturesWithFMVActive) }
    }
    var dumpDirectTextures: Bool = true { didSet { commit(Self._dumpDirectTexturesConfig, dumpDirectTextures) } }
    var dumpPaletteTextures: Bool = true { didSet { commit(Self._dumpPaletteTexturesConfig, dumpPaletteTextures) } }

    // ── GS Hardware Fixes (EmuCore/GS) ──
    var hwAccurateAlphaTest: Bool = false { didSet { commit(Self._hwAccurateAlphaTestConfig, hwAccurateAlphaTest) } }
    var textureInsideRt: Int = 0 { didSet { commit(Self._textureInsideRtConfig, textureInsideRt) } }
    var limit24BitDepth: Int = 0 { didSet { commit(Self._limit24BitDepthConfig, limit24BitDepth) } }
    var nativeScaling: Int = 0 { didSet { commit(Self._nativeScalingConfig, nativeScaling) } }
    var cpuClutRender: Int = 0 { didSet { commit(Self._cpuClutRenderConfig, cpuClutRender) } }
    var cpuSpriteRenderBw: Int = 0 { didSet { commit(Self._cpuSpriteRenderBwConfig, cpuSpriteRenderBw) } }
    var cpuSpriteRenderLevel: Int = 0 { didSet { commit(Self._cpuSpriteRenderLevelConfig, cpuSpriteRenderLevel) } }
    var gpuTargetClut: Int = 0 { didSet { commit(Self._gpuTargetClutConfig, gpuTargetClut) } }
    var bilinearUpscaleHack: Int = 0 { didSet { commit(Self._bilinearUpscaleHackConfig, bilinearUpscaleHack) } }
    var maxAnisotropy: Int = 0 { didSet { commit(Self._maxAnisotropyConfig, maxAnisotropy) } }
    var hardwareDownloadMode: Int = 0 { didSet { commit(Self._hardwareDownloadModeConfig, hardwareDownloadMode) } }
    var tvShader: Int = 0 { didSet { commit(Self._tvShaderConfig, tvShader) } }
    var upscaler: Int = 0 { didSet { commit(Self._upscalerConfig, upscaler) } }

    /// What the running game really has, keyed by INI key. The INI is what the player
    /// asked for; between the two sit the two mask passes and the GameDB, and only the
    /// core can see the result. Empty until a game is running.
    var graphicsHackStatus: [String: GraphicsHackStatus] = [:]

    var gsBoolHacks: [String: Bool] = [:]

    // ── Screen / PCRTC (EmuCore/GS) ── display-output options, applied live.
    var pcrtcOffsets: Bool = false { didSet { commit(Self._pcrtcOffsetsConfig, pcrtcOffsets) } }
    var pcrtcOverscan: Bool = false { didSet { commit(Self._pcrtcOverscanConfig, pcrtcOverscan) } }
    var pcrtcAntiBlur: Bool = true { didSet { commit(Self._pcrtcAntiBlurConfig, pcrtcAntiBlur) } }
    var disableInterlaceOffset: Bool = false {
        didSet { commit(Self._disableInterlaceOffsetConfig, disableInterlaceOffset) }
    }
    var skipDuplicateFrames: Bool = true { didSet { commit(Self._skipDuplicateFramesConfig, skipDuplicateFrames) } }
    var syncToHostRefresh: Bool = false { didSet {
        commit(Self._syncToHostRefreshConfig, syncToHostRefresh)
        if syncToHostRefresh != oldValue { markFramePacingCustom() }
    }}
    var integerScaling: Bool = false { didSet { commit(Self._integerScalingConfig, integerScaling) } }

    // ── Shade Boost (EmuCore/GS) ── post-process color adjustment, applied live.
    var shadeBoost: Bool = false { didSet { commit(Self._shadeBoostConfig, shadeBoost) } }
    var shadeBoostBrightness: Int = 50 { didSet { commit(Self._shadeBoostBrightnessConfig, shadeBoostBrightness) } }
    var shadeBoostContrast: Int = 50 { didSet { commit(Self._shadeBoostContrastConfig, shadeBoostContrast) } }
    var shadeBoostSaturation: Int = 50 { didSet { commit(Self._shadeBoostSaturationConfig, shadeBoostSaturation) } }
    var shadeBoostGamma: Int = 50 { didSet { commit(Self._shadeBoostGammaConfig, shadeBoostGamma) } }

    // ── OSD Overlay ──
    var osdPreset: OsdPreset {
        didSet {
            // Only a user change cascades into the OSD flags, so a bulk reload keeps them.
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIInt("ARMSX2iOS/UI", key: "OsdPreset", value: Int32(osdPreset.rawValue))
            if osdPreset == .off {
                if oldValue != .off {
                    lastActiveOsdPreset = oldValue
                }
            } else {
                lastActiveOsdPreset = osdPreset
            }
            if osdPreset == .custom {
                if !isAutoMarkingCustom {
                    restoreCustomOsd()
                }
            } else {
                applyOsdPreset(osdPreset)
            }
        }
    }
    // Frame Pacing — consolidated EmuCore/GS + SPU2/Output + Framerate surface.
    var framePacingPreset: FramePacingPreset = .optimal {
        didSet {
            // Only a user change cascades into the pacing keys, so a bulk reload keeps them.
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIInt("ARMSX2iOS/FramePacing", key: "Preset", value: Int32(framePacingPreset.rawValue))
            if framePacingPreset == .custom {
                if !isAutoMarkingFramePacingCustom {
                    restoreCustomFramePacing()
                }
            } else {
                applyFramePacingPreset(framePacingPreset)
            }
        }
    }
    var adaptiveResolutionEnabled: Bool = false { didSet {
        commit(Self._adaptiveResolutionEnabledConfig, adaptiveResolutionEnabled)
        // Not while the INI is loading: setEnabled reads SettingsStore.shared, and we are inside
        // that very initializer. init starts the controller itself once it has finished.
        guard !suppressINIWrites else { return }
        FrameTimeDynamicResolutionController.shared.setEnabled(adaptiveResolutionEnabled)
    }}
    var lastActiveOsdPreset: OsdPreset = .simple { didSet { commit(Self._lastActiveOsdPresetConfig, lastActiveOsdPreset) } }
    var osdPerformancePosition = SettingsStore.defaultOsdPerformancePosition {
        didSet { commit(Self._osdPerformancePositionConfig, osdPerformancePosition) }
    }
    var osdShowMessages: Bool = true { didSet { commit(Self._osdShowMessagesConfig, osdShowMessages) } }
    var osdShowFPS: Bool = false { didSet {
        commit(Self._osdShowFPSConfig, osdShowFPS)
        markOsdCustom()
    }}
    var osdShowVPS: Bool = false { didSet {
        commit(Self._osdShowVPSConfig, osdShowVPS)
        markOsdCustom()
    }}
    var osdShowSpeed: Bool = false { didSet {
        commit(Self._osdShowSpeedConfig, osdShowSpeed)
        markOsdCustom()
    }}
    var osdShowCPU: Bool = false { didSet {
        commit(Self._osdShowCPUConfig, osdShowCPU)
        markOsdCustom()
    }}
    var osdShowGPU: Bool = false { didSet {
        commit(Self._osdShowGPUConfig, osdShowGPU)
        markOsdCustom()
    }}
    var osdShowResolution: Bool = false { didSet {
        commit(Self._osdShowResolutionConfig, osdShowResolution)
        markOsdCustom()
    }}
    var osdShowGSStats: Bool = false { didSet {
        commit(Self._osdShowGSStatsConfig, osdShowGSStats)
        markOsdCustom()
    }}
    var osdShowIndicators: Bool = false { didSet {
        commit(Self._osdShowIndicatorsConfig, osdShowIndicators)
        markOsdCustom()
    }}
    var osdShowSettings: Bool = false { didSet {
        commit(Self._osdShowSettingsConfig, osdShowSettings)
        markOsdCustom()
    }}
    var osdShowInputs: Bool = false { didSet {
        commit(Self._osdShowInputsConfig, osdShowInputs)
        markOsdCustom()
    }}
    var osdShowFrameTimes: Bool = false { didSet {
        commit(Self._osdShowFrameTimesConfig, osdShowFrameTimes)
        markOsdCustom()
    }}
    var osdShowVersion: Bool = false { didSet {
        commit(Self._osdShowVersionConfig, osdShowVersion)
        markOsdCustom()
    }}
    var osdShowHardwareInfo: Bool = false { didSet {
        commit(Self._osdShowHardwareInfoConfig, osdShowHardwareInfo)
        markOsdCustom()
    }}
    var osdShowTextureReplacements: Bool = false {
        didSet { commit(Self._osdShowTextureReplacementsConfig, osdShowTextureReplacements) }
    }
    var osdShowDeviceStats: Bool = false { didSet {
        commit(Self._osdShowDeviceStatsConfig, osdShowDeviceStats)
        markOsdCustom()
    }}

    let controller: Controller
    var appLanguage: AppLanguage = .system { didSet { commit(Self._appLanguageConfig, appLanguage) } }

    var autoOpenStikDebug: Bool = false { didSet { commit(Self._autoOpenStikDebugConfig, autoOpenStikDebug) } }
    var jitScriptProtocol = JITScriptProtocol.defaultValue {
        didSet { commit(Self._jitScriptProtocolConfig, jitScriptProtocol) }
    }

    let network: Network

    let appearance: Appearance

    private init() {
        Self.Appearance.initializeAutomaticJITAppearanceState()
        // An assignment here skips didSet only while a stored property without a default
        // is still unset, so the last load must be one of those. In a helper every load
        // would fire didSet, and each non-suppressible setting would rewrite itself.
        suppressINIWrites = true
        defer {
            suppressINIWrites = false
            controller.synchronizeControllerMacroGameplayInput()
            writeFrameLimiterSettings()
        }

        // Runs before any stored property is read, and writes the INI directly because
        // init can't touch SettingsStore.shared (swift_once deadlock).
        Self.migrateFramePacingOptimalDefaultV1()
        // The phone rumble slider changed meaning without changing key.
        Self.migratePhoneRumbleStrengthRescaleV1()
        // These two run on every launch, because every install moves the container.
        ShaderPresetLibrary.repairSavedReferences()
        Self.migrateShaderChainSelectionV1()

        eeCoreType = Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "CoreType", defaultValue: 2))
        iopRecompiler = Self._iopRecompilerConfig.load()
        vu0Recompiler = Self._vu0RecompilerConfig.load()
        vu1Recompiler = Self._vu1RecompilerConfig.load()
        fastBoot = Self.loadedFastBoot()
        automaticLoadLastSaveState = Self._automaticLoadLastSaveStateConfig.load()
        autoSaveEnabled = Self._autoSaveEnabledConfig.load()
        autoSaveIntervalMinutes = Self._autoSaveIntervalConfig.load()
        autoSaveOnLeave = Self._autoSaveOnLeaveConfig.load()
        autoSaveOnLowBattery = Self._autoSaveOnLowBatteryConfig.load()
        undoSeconds = Self._undoSecondsConfig.load()
        automaticLoadLastGame = Self._automaticLoadLastGameConfig.load()
        fastmem = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: true)
        emulationOnlyModeEnabled = Self._emulationOnlyModeConfig.load()
        emulationOnlyDisablePatches = Self._emulationOnlyDisablePatchesConfig.load()
        emulationOnlyDisablePINE = Self._emulationOnlyDisablePINEConfig.load()
        emulationOnlyDisableRetroAchievements = Self._emulationOnlyDisableRetroAchievementsConfig.load()
        emulationOnlyDisableInputRecording = Self._emulationOnlyDisableInputRecordingConfig.load()
        emulationOnlyDisableOSD = Self._emulationOnlyDisableOSDConfig.load()
        emulationOnlyDisableFramePacing = Self._emulationOnlyDisableFramePacingConfig.load()
        emulationOnlyDisableVirtualControls = Self._emulationOnlyDisableVirtualControlsConfig.load()
        emulationOnlyDisableQuickMenu = Self._emulationOnlyDisableQuickMenuConfig.load()
        emulationOnlyClearNetworkCache = Self._emulationOnlyClearNetworkCacheConfig.load()
        emulationOnlyModeDelaySeconds = Self._emulationOnlyModeDelayConfig.load()
        eeFpuRoundMode = Self._eeFpuRoundModeConfig.load()
        vu0RoundMode = Self._vu0RoundModeConfig.load()
        vu1RoundMode = Self._vu1RoundModeConfig.load()
        eeClampMode = Self.eeClampModeFromBools(
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuOverflow", defaultValue: true),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuExtraOverflow", defaultValue: false),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuFullMode", defaultValue: false))
        vuClampMode = Self.vuClampModeFromBools(
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0Overflow", defaultValue: true),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0ExtraOverflow", defaultValue: false),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0SignOverflow", defaultValue: false))
        let loadedNTSCFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FramerateNTSC", defaultValue: 59.94)
        ntscFramerate = loadedNTSCFramerate
        palFramerate = Self._palFramerateConfig.load()
        let nominalScalar = ARMSX2Bridge.getINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0)
        frameLimiterEnabled = Self.frameLimiterEnabled(fromNominalScalar: nominalScalar)
        targetFPS = Self.loadedTargetFPS(fromNominalScalar: nominalScalar, baseFramerate: loadedNTSCFramerate)
        Self.sanitizeNominalScalarIfNeeded(nominalScalar)
        fastForwardScalar = Self.clampedSpeedScalar(ARMSX2Bridge.getINIFloat("Framerate", key: "TurboScalar", defaultValue: Self.defaultFastForwardScalar))
        emulatorVolumePercent = Self.clampedEmulatorVolumePercent(Int(ARMSX2Bridge.emulatorVolumePercent()))
        audioTimeStretch = Self._audioTimeStretchConfig.load()
        audioBufferMs = Self._audioBufferMsConfig.load()
        audioOutputLatencyMs = Self._audioOutputLatencyMsConfig.load()
        audioFastForwardVolume = Self._audioFastForwardVolumeConfig.load()
        audioSwapChannels = Self._audioSwapChannelsConfig.load()
        fastCDVD = Self._fastCDVDConfig.load()
        eeCycleRate = Self._eeCycleRateConfig.load()
        vu1Instant = Self._vu1InstantConfig.load()
        mtvu = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vuThread", defaultValue: true)
        waitLoop = Self._waitLoopConfig.load()
        intcStat = Self._intcStatConfig.load()
        eeCycleSkip = Self._eeCycleSkipConfig.load()
        vuFlagHack = Self._vuFlagHackConfig.load()
        enableCheats = Self._enableCheatsConfig.load()
        enablePatches = Self._enablePatchesConfig.load()
        enableGameFixes = Self._enableGameFixesConfig.load()
        enableGameDBHardwareFixes = Self._enableGameDBHardwareFixesConfig.load()
        enableWidescreenPatches = Self._enableWidescreenPatchesConfig.load()
        enableNoInterlacingPatches = Self._enableNoInterlacingPatchesConfig.load()
        hostFilesystem = Self._hostFilesystemConfig.load()
        autoTune = Self._autoTuneConfig.load()
        gameFixes = Self.loadGameFixes()
        // Not load(): the INI can name a desktop renderer, so we correct it on disk too.
#if targetEnvironment(macCatalyst)
        renderer = 17
        Self._rendererConfig.write(17)
#else
        let initialRenderer = Self.supportedIOSRenderer(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17)))
        renderer = initialRenderer
        Self._rendererConfig.write(initialRenderer)
#endif
        upscaleMultiplier = Self._upscaleMultiplierConfig.load()
        vsyncQueueSize = Self._vsyncQueueSizeConfig.load()
        textureFiltering = Self._textureFilteringConfig.load()
        backThreadMode = Self._backThreadModeConfig.load()
        hardwareMipmapping = Self._hardwareMipmappingConfig.load()
        fxaa = Self._fxaaConfig.load()
        casMode = Self._casModeConfig.load()
        casSharpness = Self._casSharpnessConfig.load()
        shaderChainEnabled = Self._shaderChainEnabledConfig.load()
        shaderChainPresetRef = Self._shaderChainPresetRefConfig.load()
        interlaceMode = Self._interlaceModeConfig.load()
        aspectRatio = Self._aspectRatioConfig.load()
        blendingAccuracy = Self._blendingAccuracyConfig.load()
        dithering = Self._ditheringConfig.load()
        trilinearFiltering = Self._trilinearFilteringConfig.load()
        halfPixelOffset = Self._halfPixelOffsetConfig.load()
        roundSprite = Self._roundSpriteConfig.load()
        alignSprite = Self._alignSpriteConfig.load()
        mergeSprite = Self._mergeSpriteConfig.load()
        wildArmsOffset = Self._wildArmsOffsetConfig.load()
        textureOffsetX = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetX", defaultValue: 0)))
        textureOffsetY = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetY", defaultValue: 0)))
        let loadedSkipDrawStart = Self.clampedSkipDraw(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_Start", defaultValue: 0)))
        skipDrawStart = loadedSkipDrawStart
        skipDrawEnd = Self.normalizedSkipDrawEnd(
            start: loadedSkipDrawStart,
            end: Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_End", defaultValue: 0))
        )
        loadTextureReplacements = Self._loadTextureReplacementsConfig.load()
        loadTextureReplacementsAsync = Self._loadTextureReplacementsAsyncConfig.load()
        precacheTextureReplacements = Self._precacheTextureReplacementsConfig.load()
        texturePreloading = Self._texturePreloadingConfig.load()
        dumpReplaceableTextures = Self._dumpReplaceableTexturesConfig.load()
        dumpReplaceableMipmaps = Self._dumpReplaceableMipmapsConfig.load()
        dumpTexturesWithFMVActive = Self._dumpTexturesWithFMVActiveConfig.load()
        dumpDirectTextures = Self._dumpDirectTexturesConfig.load()
        dumpPaletteTextures = Self._dumpPaletteTexturesConfig.load()
        hwAccurateAlphaTest = Self._hwAccurateAlphaTestConfig.load()
        textureInsideRt = Self._textureInsideRtConfig.load()
        limit24BitDepth = Self._limit24BitDepthConfig.load()
        nativeScaling = Self._nativeScalingConfig.load()
        cpuClutRender = Self._cpuClutRenderConfig.load()
        cpuSpriteRenderBw = Self._cpuSpriteRenderBwConfig.load()
        cpuSpriteRenderLevel = Self._cpuSpriteRenderLevelConfig.load()
        gpuTargetClut = Self._gpuTargetClutConfig.load()
        bilinearUpscaleHack = Self._bilinearUpscaleHackConfig.load()
        maxAnisotropy = Self._maxAnisotropyConfig.load()
        hardwareDownloadMode = Self._hardwareDownloadModeConfig.load()
        tvShader = Self._tvShaderConfig.load()
        upscaler = Self._upscalerConfig.load()
        gsBoolHacks = Self.loadGSBoolHacks()
        pcrtcOffsets = Self._pcrtcOffsetsConfig.load()
        pcrtcOverscan = Self._pcrtcOverscanConfig.load()
        pcrtcAntiBlur = Self._pcrtcAntiBlurConfig.load()
        disableInterlaceOffset = Self._disableInterlaceOffsetConfig.load()
        skipDuplicateFrames = Self._skipDuplicateFramesConfig.load()
        syncToHostRefresh = Self._syncToHostRefreshConfig.load()
        integerScaling = Self._integerScalingConfig.load()
        shadeBoost = Self._shadeBoostConfig.load()
        shadeBoostBrightness = Self._shadeBoostBrightnessConfig.load()
        shadeBoostContrast = Self._shadeBoostContrastConfig.load()
        shadeBoostSaturation = Self._shadeBoostSaturationConfig.load()
        shadeBoostGamma = Self._shadeBoostGammaConfig.load()
        let loadedOsdPreset = OsdPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "OsdPreset", defaultValue: 0))) ?? .off
        osdPreset = loadedOsdPreset
        // Not load(): -1 means never set, and then the preset above decides.
        let loadedLastActiveOsdPresetRaw = ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "LastActiveOsdPreset", defaultValue: -1)
        if loadedLastActiveOsdPresetRaw >= 0 {
            lastActiveOsdPreset = OsdPreset(rawValue: Int(loadedLastActiveOsdPresetRaw)) ?? .simple
        } else {
            lastActiveOsdPreset = loadedOsdPreset != .off ? loadedOsdPreset : .simple
        }
        // Not load(): old INIs hold positions this build no longer offers.
        osdPerformancePosition = Self.normalizedOsdPerformancePosition(
            Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "OsdPerformancePos", defaultValue: Int32(Self.defaultOsdPerformancePosition)))
        )
        osdShowMessages = Self._osdShowMessagesConfig.load()
        osdShowFPS = Self._osdShowFPSConfig.load()
        osdShowVPS = Self._osdShowVPSConfig.load()
        osdShowSpeed = Self._osdShowSpeedConfig.load()
        osdShowCPU = Self._osdShowCPUConfig.load()
        osdShowGPU = Self._osdShowGPUConfig.load()
        osdShowResolution = Self._osdShowResolutionConfig.load()
        osdShowGSStats = Self._osdShowGSStatsConfig.load()
        osdShowIndicators = Self._osdShowIndicatorsConfig.load()
        osdShowSettings = Self._osdShowSettingsConfig.load()
        osdShowInputs = Self._osdShowInputsConfig.load()
        osdShowFrameTimes = Self._osdShowFrameTimesConfig.load()
        osdShowVersion = Self._osdShowVersionConfig.load()
        osdShowHardwareInfo = Self._osdShowHardwareInfoConfig.load()
        osdShowTextureReplacements = Self._osdShowTextureReplacementsConfig.load()
        // Fresh installs follow the OSD preset rather than the descriptor default.
        osdShowDeviceStats = Self._osdShowDeviceStatsConfig.load(default: loadedOsdPreset != .off)
        controller = Controller()
        appLanguage = Self._appLanguageConfig.load()
        autoOpenStikDebug = Self._autoOpenStikDebugConfig.load()
        // Not load(): older builds wrote names this enum no longer spells that way.
        jitScriptProtocol = Self.loadedJITScriptProtocol()
        // The migration above already wrote these INI values.
        framePacingPreset = FramePacingPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/FramePacing", key: "Preset", defaultValue: Int32(FramePacingPreset.optimal.rawValue)))) ?? .optimal
        adaptiveResolutionEnabled = Self._adaptiveResolutionEnabledConfig.load()
        network = Network()
        appearance = Appearance()
        network.normalizeDEV9Settings()
        VPadSkinLibraryStore.shared.adoptLegacySelection(controller.virtualPadSkin)
        Self._aspectRatioConfig.write(aspectRatio)
        // The saved per-item flags are the source of truth, so applying the preset here
        // would rewrite every one of them. Seed the Custom snapshot once instead, or
        // cycling to Custom shows an empty overlay.
        if !ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "OsdCustomSeeded", defaultValue: false) {
            snapshotCustomOsd()
            ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "OsdCustomSeeded", value: true)
        }
        // Next run loop: setEnabled reads SettingsStore.shared.upscaleMultiplier, which
        // would re-enter this init's swift_once and deadlock dispatch_once.
        DispatchQueue.main.async { [self] in
            FrameTimeDynamicResolutionController.shared.setEnabled(self.adaptiveResolutionEnabled)
        }
    }


}
