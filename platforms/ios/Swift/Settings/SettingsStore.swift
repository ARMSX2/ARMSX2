// SettingsStore.swift — INI-backed settings for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

@MainActor
@Observable
final class SettingsStore {
    static let shared = SettingsStore()

    /// Manual EmuCore/Gamefixes toggles — see SettingsStore+GameFixes.swift.

    @ObservationIgnored var suppressINIWrites = false
    @ObservationIgnored var isProgrammaticOsdFlagChange = false
    @ObservationIgnored var isAutoMarkingCustom = false
    @ObservationIgnored var isProgrammaticFramePacingFlagChange = false
    @ObservationIgnored var isAutoMarkingFramePacingCustom = false
    @ObservationIgnored var graphicsApplyWorkItem: DispatchWorkItem?
    @ObservationIgnored var visualSliderDragCount = 0
    @ObservationIgnored var graphicsApplyDeferred = false
    @ObservationIgnored var visualSliderWatchdog: DispatchWorkItem?

    // ── Emulator / CPU ──
    // writes CoreType + UseArm64Dynarec
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
    // writes GameISO/FastBoot + EmuCore/EnableFastBoot
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
    // writes ManualFastmem + EnableFastmem
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
    // clamps to 15...120
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
    // clamps to 1.25...10.0
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
    // clamps to 0...150
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

    // ── Boot ──
    var fastCDVD: Bool = false { didSet { commit(Self._fastCDVDConfig, fastCDVD) } }

    // ── Advanced Speedhacks ──
    var eeCycleRate: Int = 0 { didSet { commit(Self._eeCycleRateConfig, eeCycleRate) } }
    var vu1Instant: Bool = true { didSet { commit(Self._vu1InstantConfig, vu1Instant) } }
    // writes ManualMTVU + ManualMTVUVersion + vuThread
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
    // Dictionary-backed because the 17 fixes are homogeneous toggles. Effective only
    // while GameDB Core Fixes (enableGameFixes) is on. Toggling one writes only its
    // own INI key.
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
            // Only an explicit user change should cascade the preset into the
            // individual OSD flags. During a bulk reload (suppressINIWrites), skip
            // so applyOsdPreset() can't overwrite the user OSD settings.
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
            // Only an explicit user change cascades the preset into the
            // individual pacing keys; skip during a bulk reload so restored
            // values are not overwritten.
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

    // ── Gamepad / UI ──
    var padOpacity: Float = 0.6 { didSet { commit(Self._padOpacityConfig, padOpacity) } }
    var phoneRumbleStrength: Float = 0.25 { didSet { commit(Self._phoneRumbleStrengthConfig, phoneRumbleStrength) } }
    var increaseRumbleDurationAndInterpolation: Bool = true {
        didSet { commit(Self._increaseRumbleDurationAndInterpolationConfig, increaseRumbleDurationAndInterpolation) }
    }
    var hapticFeedback: Bool = true { didSet { commit(Self._hapticFeedbackConfig, hapticFeedback) } }
    var gameRumbleStrength: Float = 1 {
        didSet {
            let clamped = min(max(gameRumbleStrength, 0), 2)
            guard clamped == gameRumbleStrength else {
                gameRumbleStrength = clamped
                return
            }
            commit(Self._gameRumbleStrengthConfig, gameRumbleStrength)
        }
    }
    var uiRumbleStrength: Float = 0.5 {
        didSet {
            let clamped = min(max(uiRumbleStrength, 0), 1)
            guard clamped == uiRumbleStrength else {
                uiRumbleStrength = clamped
                return
            }
            commit(Self._uiRumbleStrengthConfig, uiRumbleStrength)
        }
    }
    var dpadDiagonalsEnabled: Bool = true { didSet { commit(Self._dpadDiagonalsEnabledConfig, dpadDiagonalsEnabled) } }
    var faceComboZonesEnabled: Bool = true { didSet { commit(Self._faceComboZonesEnabledConfig, faceComboZonesEnabled) } }
    var virtualPadSkin: VirtualPadSkin = .armsx2Refresh { didSet { commit(Self._virtualPadSkinConfig, virtualPadSkin) } }
    var automaticDownloadCustomSkin: Bool = false {
        didSet { commit(Self._automaticDownloadCustomSkinConfig, automaticDownloadCustomSkin) }
    }
    var autoHideVirtualPadWhenControllerConnected: Bool = true {
        didSet { commit(Self._autoHideVirtualPadWhenControllerConnectedConfig, autoHideVirtualPadWhenControllerConnected) }
    }
    var autoFullscreen: Bool = true { didSet { commit(Self._autoFullscreenConfig, autoFullscreen) } }
    var hideMenuButton: Bool = false { didSet { commit(Self._hideMenuButtonConfig, hideMenuButton) } }
    // clamps to 0.8...1.6
    var analogStickScale: Float {
        didSet {
            let clamped = Self.clampedAnalogStickScale(analogStickScale)
            guard abs(analogStickScale - clamped) <= 0.001 else {
                analogStickScale = clamped
                return
            }
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", value: analogStickScale)
        }
    }
    var gameControllerDeadZone: Float = 0.15 {
        didSet {
            let clamped = min(max(gameControllerDeadZone, 0), 0.25)
            guard clamped == gameControllerDeadZone else {
                gameControllerDeadZone = clamped
                return
            }
            commit(Self._gameControllerDeadZoneConfig, gameControllerDeadZone)
        }
    }
    var gameControllerLeftInstantDeadzoneEnabled: Bool = false {
        didSet { commit(Self._gameControllerLeftInstantDeadzoneConfig, gameControllerLeftInstantDeadzoneEnabled) }
    }
    var gameControllerLeftNegativeDeadzone: Float = -0.08 {
        didSet {
            let clamped = min(max(gameControllerLeftNegativeDeadzone, -0.25), 0)
            guard clamped == gameControllerLeftNegativeDeadzone else {
                gameControllerLeftNegativeDeadzone = clamped
                return
            }
            commit(Self._gameControllerLeftNegativeDeadzoneConfig, gameControllerLeftNegativeDeadzone)
        }
    }
    var gameControllerRightInstantDeadzoneEnabled: Bool = false {
        didSet { commit(Self._gameControllerRightInstantDeadzoneConfig, gameControllerRightInstantDeadzoneEnabled) }
    }
    var gameControllerRightNegativeDeadzone: Float = -0.08 {
        didSet {
            let clamped = min(max(gameControllerRightNegativeDeadzone, -0.25), 0)
            guard clamped == gameControllerRightNegativeDeadzone else {
                gameControllerRightNegativeDeadzone = clamped
                return
            }
            commit(Self._gameControllerRightNegativeDeadzoneConfig, gameControllerRightNegativeDeadzone)
        }
    }
    private static let stickInversionSection = "ARMSX2iOS/UI"
    var invertLeftStickX: Bool = false { didSet { commit(Self._invertLeftStickXConfig, invertLeftStickX) } }
    var invertLeftStickY: Bool = false { didSet { commit(Self._invertLeftStickYConfig, invertLeftStickY) } }
    var invertRightStickX: Bool = false { didSet { commit(Self._invertRightStickXConfig, invertRightStickX) } }
    var invertRightStickY: Bool = false { didSet { commit(Self._invertRightStickYConfig, invertRightStickY) } }

    static let stickInversionKeys = ["InvertLeftStickX", "InvertLeftStickY", "InvertRightStickX", "InvertRightStickY"]
    /// Per-game overrides for the game the cache was built from. Only the overridden keys
    /// are stored, so a change to a global still takes effect without rebuilding.
    @ObservationIgnored private var stickInversionOverrides: [String: Bool] = [:]
    @ObservationIgnored private var stickInversionOverridesGame = ""

    /// Effective axis inversion for a stick: the per-game override if there is one, else the
    /// global. Called once per stick sample, so it must not touch the filesystem — the
    /// per-game INI is read only when the running game changes or an override is written.
    func stickInversion(for side: StickSide) -> (x: Bool, y: Bool) {
        let game = ARMSX2Bridge.perGameIdentityKeyForCurrentGame()
        if game != stickInversionOverridesGame {
            stickInversionOverridesGame = game
            stickInversionOverrides = game.isEmpty ? [:] : Self.loadedStickInversionOverrides()
        }
        switch side {
        case .left:
            return (stickInversionOverrides["InvertLeftStickX"] ?? invertLeftStickX,
                    stickInversionOverrides["InvertLeftStickY"] ?? invertLeftStickY)
        case .right:
            return (stickInversionOverrides["InvertRightStickX"] ?? invertRightStickX,
                    stickInversionOverrides["InvertRightStickY"] ?? invertRightStickY)
        }
    }

    /// Call after writing or clearing a per-game inversion key so the next sample picks it up.
    func reloadStickInversionOverrides() {
        stickInversionOverridesGame = ARMSX2Bridge.perGameIdentityKeyForCurrentGame()
        stickInversionOverrides = stickInversionOverridesGame.isEmpty ? [:] : Self.loadedStickInversionOverrides()
    }

    private static func loadedStickInversionOverrides() -> [String: Bool] {
        var overrides: [String: Bool] = [:]
        for key in stickInversionKeys where ARMSX2Bridge.hasPerGameINIValueForCurrentGame(stickInversionSection, key: key) {
            overrides[key] = ARMSX2Bridge.getPerGameINIBoolForCurrentGame(stickInversionSection, key: key, defaultValue: false)
        }
        return overrides
    }
    var appLanguage: AppLanguage = .system { didSet { commit(Self._appLanguageConfig, appLanguage) } }
    var controllerMultitapMode: Int = 0 { didSet { commit(Self._controllerMultitapModeConfig, controllerMultitapMode) } }
    var controllerMacroQuickMenu = ControllerMacroAction.quickMenu.defaultBinding {
        didSet { commit(Self._controllerMacroQuickMenuConfig, controllerMacroQuickMenu) }
    }
    var controllerMacroSaveGameState = ControllerMacroAction.saveGameState.defaultBinding {
        didSet { commit(Self._controllerMacroSaveGameStateConfig, controllerMacroSaveGameState) }
    }
    var controllerMacroLoadGameState = ControllerMacroAction.loadGameState.defaultBinding {
        didSet { commit(Self._controllerMacroLoadGameStateConfig, controllerMacroLoadGameState) }
    }
    var controllerMacroIncreaseSpeed = ControllerMacroAction.increaseSpeed.defaultBinding {
        didSet { commit(Self._controllerMacroIncreaseSpeedConfig, controllerMacroIncreaseSpeed) }
    }
    var controllerMacroDecreaseSpeed = ControllerMacroAction.decreaseSpeed.defaultBinding {
        didSet { commit(Self._controllerMacroDecreaseSpeedConfig, controllerMacroDecreaseSpeed) }
    }
    var controllerMacroEnableFastForward = ControllerMacroAction.enableFastForward.defaultBinding {
        didSet { commit(Self._controllerMacroEnableFastForwardConfig, controllerMacroEnableFastForward) }
    }
    var controllerMacroDisableFastForward = ControllerMacroAction.disableFastForward.defaultBinding {
        didSet { commit(Self._controllerMacroDisableFastForwardConfig, controllerMacroDisableFastForward) }
    }
    var controllerMacroUndoSaveState = ControllerMacroAction.undoSaveState.defaultBinding {
        didSet { commit(Self._controllerMacroUndoSaveStateConfig, controllerMacroUndoSaveState) }
    }

    var autoOpenStikDebug: Bool = false { didSet { commit(Self._autoOpenStikDebugConfig, autoOpenStikDebug) } }
    var jitScriptProtocol = JITScriptProtocol.defaultValue {
        didSet { commit(Self._jitScriptProtocolConfig, jitScriptProtocol) }
    }

    // DEV9 / Network
    // writes HddEnable + HddFile (+ excludes HDD image from backup on enable)
    var dev9HddEnabled: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Hdd", key: "HddEnable", value: dev9HddEnabled)
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile)
            if dev9HddEnabled {
                // A large HDD image should never ride along in iCloud/iTunes
                // backups, so mark it excluded as soon as the feature is on.
                excludeHddImageFromBackup()
            }
        }
    }
    var dev9HddFile: String = "DEV9hdd.raw" { didSet { commit(Self._dev9HddFileConfig, dev9HddFile) } }
    // writes EthEnable + EthApi + EthDevice
    var dev9EthernetEnabled: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthEnable", value: dev9EthernetEnabled)
            if dev9EthernetEnabled {
                ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
                ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
            }
        }
    }
    // writes EthApi + EthDevice
    var dev9EthDevice: String {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
        }
    }
    var dev9InterceptDHCP: Bool = false { didSet { commit(Self._dev9InterceptDHCPConfig, dev9InterceptDHCP) } }
    var dev9EthLogDHCP: Bool = false { didSet { commit(Self._dev9EthLogDHCPConfig, dev9EthLogDHCP) } }
    var dev9EthLogDNS: Bool = false { didSet { commit(Self._dev9EthLogDNSConfig, dev9EthLogDNS) } }
    var dev9DNS1Mode: String = "Auto" { didSet { commit(Self._dev9DNS1ModeConfig, dev9DNS1Mode) } }
    var dev9DNS1: String = "0.0.0.0" { didSet { commit(Self._dev9DNS1Config, dev9DNS1) } }
    var dev9DNS2Mode: String = "Auto" { didSet { commit(Self._dev9DNS2ModeConfig, dev9DNS2Mode) } }
    var dev9DNS2: String = "0.0.0.0" { didSet { commit(Self._dev9DNS2Config, dev9DNS2) } }

    // ── Library Background ──
    var dynamicBackgroundsEnabled: Bool = true {
        didSet {
            UserDefaults.standard.set(
                dynamicBackgroundsEnabled,
                forKey: "ARMSX2iOSDynamicBackgroundsEnabled"
            )
        }
    }
    var dynamicAppearancePreferences: DynamicAppearancePreferences = .standard {
        didSet { dynamicAppearancePreferences.save() }
    }
    var clearLiquidGlassUI: Bool = true {
        didSet {
            UserDefaults.standard.set(clearLiquidGlassUI, forKey: "ARMSX2iOSClearLiquidGlassUI")
            if !clearLiquidGlassUI && clearLiquidGlassUISubSettings {
                clearLiquidGlassUISubSettings = false
            }
        }
    }
    var clearLiquidGlassUISubSettings: Bool = true {
        didSet {
            UserDefaults.standard.set(
                clearLiquidGlassUISubSettings,
                forKey: "ARMSX2iOSClearLiquidGlassUISubSettings"
            )
        }
    }
    var clearLiquidGlassUIQuickMenu: Bool = false {
        didSet {
            UserDefaults.standard.set(
                clearLiquidGlassUIQuickMenu,
                forKey: "ARMSX2iOSClearLiquidGlassUIQuickMenu"
            )
        }
    }
    var clearLiquidGlassUIPerGameSettingsLibrary: Bool = false {
        didSet {
            UserDefaults.standard.set(
                clearLiquidGlassUIPerGameSettingsLibrary,
                forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsLibrary"
            )
        }
    }
    var clearLiquidGlassUIPerGameSettingsEmulation: Bool = false {
        didSet {
            UserDefaults.standard.set(
                clearLiquidGlassUIPerGameSettingsEmulation,
                forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsEmulation"
            )
        }
    }
    static let perGameLivePreviewDurationRange: ClosedRange<Double> = 2...10
    static let perGameBeforeChangesPreviewDurationRange: ClosedRange<Double> = 0...10
    var temporalSaveStateToLivePreviewChanges: Bool = true {
        didSet {
            UserDefaults.standard.set(
                temporalSaveStateToLivePreviewChanges,
                forKey: "ARMSX2iOSTemporalSaveStateToLivePreviewChanges"
            )
        }
    }
    var perGameLivePreviewDuration: Double = 2 {
        didSet {
            UserDefaults.standard.set(
                perGameLivePreviewDuration,
                forKey: "ARMSX2iOSPerGameLivePreviewDuration"
            )
        }
    }
    var perGameBeforeChangesPreviewDuration: Double = 0 {
        didSet {
            UserDefaults.standard.set(
                perGameBeforeChangesPreviewDuration,
                forKey: "ARMSX2iOSPerGameBeforeChangesPreviewDuration"
            )
        }
    }
    var perGameLivePreviewStopsWithCircle: Bool = true {
        didSet {
            UserDefaults.standard.set(
                perGameLivePreviewStopsWithCircle,
                forKey: "ARMSX2iOSPerGameLivePreviewStopsWithCircle"
            )
        }
    }
    var gameCardZoomAnimationEnabled: Bool = true {
        didSet {
            UserDefaults.standard.set(
                gameCardZoomAnimationEnabled,
                forKey: "ARMSX2iOSGameCardZoomAnimationEnabled"
            )
        }
    }
    var favoriteGlowingEffectEnabled: Bool = false {
        didSet {
            UserDefaults.standard.set(
                favoriteGlowingEffectEnabled,
                forKey: "ARMSX2iOSFavoriteGlowingEffectEnabled"
            )
        }
    }
    var gameMenuCoverColoursEnabled: Bool = true {
        didSet {
            UserDefaults.standard.set(
                gameMenuCoverColoursEnabled,
                forKey: "ARMSX2iOSGameMenuCoverColoursEnabled"
            )
        }
    }
    var hideGameplayStatusBar: Bool = true {
        didSet {
            UserDefaults.standard.set(
                hideGameplayStatusBar,
                forKey: "ARMSX2iOSHideGameplayStatusBar"
            )
        }
    }
    var hideIntroStatusBar: Bool = true {
        didSet {
            UserDefaults.standard.set(
                hideIntroStatusBar,
                forKey: "ARMSX2iOSHideIntroStatusBar"
            )
        }
    }
    var hideMenuStatusBar: Bool = false {
        didSet {
            UserDefaults.standard.set(
                hideMenuStatusBar,
                forKey: "ARMSX2iOSHideMenuStatusBar"
            )
        }
    }
    var focusOrbsEnabled: Bool = false {
        didSet {
            UserDefaults.standard.set(
                focusOrbsEnabled,
                forKey: "ARMSX2iOSFocusOrbsEnabled"
            )
        }
    }
    // Retain the preset's readable semantic defaults while editing Custom.
    var controllerCustomThemeBase: ControllerUIThemePreset = ControllerUIThemePreset(
        rawValue: UserDefaults.standard.string(forKey: "ARMSX2iOSCustomThemeBase") ?? ""
    ) ?? .defaultTheme {
        didSet {
            UserDefaults.standard.set(controllerCustomThemeBase.rawValue, forKey: "ARMSX2iOSCustomThemeBase")
        }
    }
    var controllerRoleCustomColors: [String: SavedPaletteColor] = {
        guard let data = UserDefaults.standard.data(forKey: "ARMSX2iOSRoleCustomColors") else { return [:] }
        return (try? JSONDecoder().decode([String: SavedPaletteColor].self, from: data)) ?? [:]
    }() {
        didSet {
            if let data = try? JSONEncoder().encode(controllerRoleCustomColors) {
                UserDefaults.standard.set(data, forKey: "ARMSX2iOSRoleCustomColors")
            }
        }
    }
    var controllerUIThemePreset: ControllerUIThemePreset = .defaultTheme {
        didSet {
            UserDefaults.standard.set(
                controllerUIThemePreset.rawValue,
                forKey: "ARMSX2iOSControllerUIThemePreset"
            )
        }
    }
    var controllerNavigationDepthEffectEnabled: Bool = true {
        didSet {
            UserDefaults.standard.set(
                controllerNavigationDepthEffectEnabled,
                forKey: "ARMSX2iOSControllerNavigationDepthEffectEnabled"
            )
        }
    }
    var controllerFocusBoxStyle: ControllerFocusBoxStyle = .neonBlue {
        didSet {
            UserDefaults.standard.set(
                controllerFocusBoxStyle.rawValue,
                forKey: "ARMSX2iOSControllerFocusBoxStyle"
            )
        }
    }
    var controllerNavigationFocusAnimation: ControllerNavigationFocusTravelStyle = .easeInOut {
        didSet {
            UserDefaults.standard.set(
                controllerNavigationFocusAnimation.rawValue,
                forKey: "ARMSX2iOSControllerNavigationFocusAnimation"
            )
        }
    }
    var controllerFocusBoxPalette: ThemePalette = .multicolor {
        didSet {
            UserDefaults.standard.set(
                controllerFocusBoxPalette.rawValue,
                forKey: "ARMSX2iOSControllerFocusBoxPalette"
            )
        }
    }
    var controllerFocusBoxCustomColor: SavedPaletteColor? {
        didSet {
            Self.persistSavedPaletteColor(
                controllerFocusBoxCustomColor,
                forKey: "ARMSX2iOSControllerFocusBoxCustomColor"
            )
        }
    }
    var controllerOrbPalette: ThemePalette = .blue {
        didSet {
            UserDefaults.standard.set(
                controllerOrbPalette.rawValue,
                forKey: "ARMSX2iOSControllerOrbPalette"
            )
        }
    }
    var controllerNavigationAccentPalette: ThemePalette = .henyBlue {
        didSet {
            UserDefaults.standard.set(
                controllerNavigationAccentPalette.rawValue,
                forKey: "ARMSX2iOSControllerNavigationAccentPalette"
            )
        }
    }
    var controllerNavigationCustomAccentColor: SavedPaletteColor? {
        didSet {
            Self.persistSavedPaletteColor(
                controllerNavigationCustomAccentColor,
                forKey: "ARMSX2iOSControllerNavigationCustomAccentColor"
            )
        }
    }
    var controllerTextPalette: ThemePalette? = nil {
        didSet {
            if let controllerTextPalette {
                UserDefaults.standard.set(
                    controllerTextPalette.rawValue,
                    forKey: "ARMSX2iOSControllerTextPalette"
                )
            } else {
                UserDefaults.standard.removeObject(
                    forKey: "ARMSX2iOSControllerTextPalette"
                )
            }
        }
    }
    var controllerTextCustomColor: SavedPaletteColor? {
        didSet {
            Self.persistSavedPaletteColor(
                controllerTextCustomColor,
                forKey: "ARMSX2iOSControllerTextCustomColor"
            )
        }
    }
    var controllerSecondaryTextPalette: ThemePalette? = nil {
        didSet {
            if let controllerSecondaryTextPalette {
                UserDefaults.standard.set(
                    controllerSecondaryTextPalette.rawValue,
                    forKey: "ARMSX2iOSControllerSecondaryTextPalette"
                )
            } else {
                UserDefaults.standard.removeObject(
                    forKey: "ARMSX2iOSControllerSecondaryTextPalette"
                )
            }
        }
    }
    var controllerCriticalTextPalette: ThemePalette = .crimson {
        didSet {
            UserDefaults.standard.set(
                controllerCriticalTextPalette.rawValue,
                forKey: "ARMSX2iOSControllerCriticalTextPalette"
            )
        }
    }
    var controllerTabTitlePalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerTabTitlePalette,
                forKey: "ARMSX2iOSControllerTabTitlePalette"
            )
        }
    }
    var controllerTabSubtitlePalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerTabSubtitlePalette,
                forKey: "ARMSX2iOSControllerTabSubtitlePalette"
            )
        }
    }
    var controllerBottomTabBarPalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerBottomTabBarPalette,
                forKey: "ARMSX2iOSControllerBottomTabBarPalette"
            )
        }
    }
    var controllerBottomTabBarUnselectedPalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerBottomTabBarUnselectedPalette,
                forKey: "ARMSX2iOSControllerBottomTabBarUnselectedPalette"
            )
        }
    }
    var controllerCardTitlePalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerCardTitlePalette,
                forKey: "ARMSX2iOSControllerCardTitlePalette"
            )
        }
    }
    var controllerContextMenuPalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerContextMenuPalette,
                forKey: "ARMSX2iOSControllerContextMenuPalette"
            )
        }
    }
    var controllerImportActionPalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerImportActionPalette,
                forKey: "ARMSX2iOSControllerImportActionPalette"
            )
        }
    }
    var controllerToolbarPalette: ThemePalette? = nil {
        didSet {
            Self.persistOptionalThemePalette(
                controllerToolbarPalette,
                forKey: "ARMSX2iOSControllerToolbarPalette"
            )
        }
    }
    var controllerFocusedTextPalette: ThemePalette = .blue {
        didSet {
            UserDefaults.standard.set(
                controllerFocusedTextPalette.rawValue,
                forKey: "ARMSX2iOSControllerFocusedTextPalette"
            )
        }
    }
    var controllerFocusedTextCustomColor: SavedPaletteColor? {
        didSet {
            Self.persistSavedPaletteColor(
                controllerFocusedTextCustomColor,
                forKey: "ARMSX2iOSControllerFocusedTextCustomColor"
            )
        }
    }
    var controllerTextShadowStrength: Double = 0 {
        didSet {
            UserDefaults.standard.set(
                controllerTextShadowStrength,
                forKey: "ARMSX2iOSControllerTextShadowStrength"
            )
        }
    }
    var controllerTextShadowPalette: ThemePalette = .obsidian {
        didSet {
            UserDefaults.standard.set(
                controllerTextShadowPalette.rawValue,
                forKey: "ARMSX2iOSControllerTextShadowPalette"
            )
        }
    }
    var controllerTextShadowCustomColor: SavedPaletteColor? {
        didSet {
            Self.persistSavedPaletteColor(
                controllerTextShadowCustomColor,
                forKey: "ARMSX2iOSControllerTextShadowCustomColor"
            )
        }
    }
    var controllerFocusedTextShadowStrength: Double = 0.1 {
        didSet {
            UserDefaults.standard.set(
                controllerFocusedTextShadowStrength,
                forKey: "ARMSX2iOSControllerFocusedTextShadowStrength"
            )
        }
    }
    var controllerFocusedTextShadowPalette: ThemePalette = .obsidian {
        didSet {
            UserDefaults.standard.set(
                controllerFocusedTextShadowPalette.rawValue,
                forKey: "ARMSX2iOSControllerFocusedTextShadowPalette"
            )
        }
    }
    var controllerFocusedTextShadowCustomColor: SavedPaletteColor? {
        didSet {
            Self.persistSavedPaletteColor(
                controllerFocusedTextShadowCustomColor,
                forKey: "ARMSX2iOSControllerFocusedTextShadowCustomColor"
            )
        }
    }
    var controllerFocusBoxAnimationSpeed: Double = 1 {
        didSet {
            UserDefaults.standard.set(
                controllerFocusBoxAnimationSpeed,
                forKey: "ARMSX2iOSControllerFocusBoxAnimationSpeed"
            )
        }
    }
    var controllerFocusBoxGlowIntensity: Double = 1 {
        didSet {
            UserDefaults.standard.set(
                controllerFocusBoxGlowIntensity,
                forKey: "ARMSX2iOSControllerFocusBoxGlowIntensity"
            )
        }
    }
    var backgroundPrimaryAsset: BackgroundAsset? {
        didSet {
            if let asset = backgroundPrimaryAsset {
                UserDefaults.standard.set(try? JSONEncoder().encode(asset), forKey: "ARMSX2iOSBackgroundPrimaryAsset")
            } else {
                UserDefaults.standard.removeObject(forKey: "ARMSX2iOSBackgroundPrimaryAsset")
            }
        }
    }
    var backgroundLandscapeAsset: BackgroundAsset? {
        didSet {
            if let asset = backgroundLandscapeAsset {
                UserDefaults.standard.set(try? JSONEncoder().encode(asset), forKey: "ARMSX2iOSBackgroundLandscapeAsset")
            } else {
                UserDefaults.standard.removeObject(forKey: "ARMSX2iOSBackgroundLandscapeAsset")
            }
        }
    }
    var backgroundFitMode: BackgroundFitMode {
        didSet { UserDefaults.standard.set(backgroundFitMode.rawValue, forKey: "ARMSX2iOSBackgroundFitMode") }
    }
    var backgroundLandscapeFitMode: BackgroundFitMode = .fill {
        didSet { UserDefaults.standard.set(backgroundLandscapeFitMode.rawValue, forKey: "ARMSX2iOSBackgroundLandscapeFitMode") }
    }
    var backgroundVideoMuted: Bool {
        didSet { UserDefaults.standard.set(backgroundVideoMuted, forKey: "ARMSX2iOSBackgroundVideoMuted") }
    }
    var backgroundDim: Double {
        didSet {
            let clamped = Self.clampedBackgroundDim(backgroundDim)
            guard backgroundDim == clamped else { backgroundDim = clamped; return }
            UserDefaults.standard.set(backgroundDim, forKey: "ARMSX2iOSBackgroundDim")
        }
    }
    var backgroundEnabledInBIOS: Bool = true {
        didSet {
            UserDefaults.standard.set(backgroundEnabledInBIOS, forKey: "ARMSX2iOSBackgroundEnabledInBIOS")
        }
    }
    var backgroundEnabledInHelp: Bool = true {
        didSet {
            UserDefaults.standard.set(backgroundEnabledInHelp, forKey: "ARMSX2iOSBackgroundEnabledInHelp")
        }
    }
    var backgroundEnabledInSettings: Bool = true {
        didSet {
            UserDefaults.standard.set(backgroundEnabledInSettings, forKey: "ARMSX2iOSBackgroundEnabledInSettings")
        }
    }

    // aspectRatioName / aspectRatioValue — see SettingsStore+Graphics.swift.
    // loadedFastBoot / loadedJITScriptProtocol — see SettingsStore+Speedhacks.swift.

    // ── Init from INI ──
    private init() {
        Self.initializeAutomaticJITAppearanceState()
        // An assignment here skips didSet only while a stored property without a default
        // is still unset, so the last load must be one of those. In a helper every load
        // would fire didSet, and each non-suppressible setting would rewrite itself.
        suppressINIWrites = true
        defer {
            suppressINIWrites = false
            synchronizeControllerMacroGameplayInput()
            applyFrameLimiterSettings()
        }

        // Runs before any stored property is read, and writes the INI directly because
        // init can't touch SettingsStore.shared (swift_once deadlock).
        Self.migrateFramePacingOptimalDefaultV1()
        // The phone rumble slider changed meaning without changing key.
        Self.migratePhoneRumbleStrengthRescaleV1()
        // These two run on every launch, because every install moves the container.
        ShaderPresetLibrary.repairSavedReferences()
        Self.migrateShaderChainSelectionV1()

        // CPU
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
        // CPU rounding & clamping
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
        // Boot
        fastCDVD = Self._fastCDVDConfig.load()
        // Advanced Speedhacks
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
        // Graphics
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
        // GS Hardware Fixes
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
        // Screen / PCRTC
        pcrtcOffsets = Self._pcrtcOffsetsConfig.load()
        pcrtcOverscan = Self._pcrtcOverscanConfig.load()
        pcrtcAntiBlur = Self._pcrtcAntiBlurConfig.load()
        disableInterlaceOffset = Self._disableInterlaceOffsetConfig.load()
        skipDuplicateFrames = Self._skipDuplicateFramesConfig.load()
        syncToHostRefresh = Self._syncToHostRefreshConfig.load()
        integerScaling = Self._integerScalingConfig.load()
        // Shade Boost
        shadeBoost = Self._shadeBoostConfig.load()
        shadeBoostBrightness = Self._shadeBoostBrightnessConfig.load()
        shadeBoostContrast = Self._shadeBoostContrastConfig.load()
        shadeBoostSaturation = Self._shadeBoostSaturationConfig.load()
        shadeBoostGamma = Self._shadeBoostGammaConfig.load()
        // OSD
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
        // UI
        padOpacity = Self._padOpacityConfig.load()
        hapticFeedback = Self._hapticFeedbackConfig.load()
        gameRumbleStrength = Self._gameRumbleStrengthConfig.load()
        uiRumbleStrength = Self._uiRumbleStrengthConfig.load()
        phoneRumbleStrength = Self._phoneRumbleStrengthConfig.load()
        increaseRumbleDurationAndInterpolation = Self._increaseRumbleDurationAndInterpolationConfig.load()
        dpadDiagonalsEnabled = Self._dpadDiagonalsEnabledConfig.load()
        faceComboZonesEnabled = Self._faceComboZonesEnabledConfig.load()
        virtualPadSkin = Self._virtualPadSkinConfig.load()
        automaticDownloadCustomSkin = Self._automaticDownloadCustomSkinConfig.load()
        autoHideVirtualPadWhenControllerConnected = Self._autoHideVirtualPadWhenControllerConnectedConfig.load()
        autoFullscreen = Self._autoFullscreenConfig.load()
        hideMenuButton = Self._hideMenuButtonConfig.load()
        analogStickScale = Self.clampedAnalogStickScale(ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", defaultValue: 1.0))
        gameControllerDeadZone = Self._gameControllerDeadZoneConfig.load()
        gameControllerLeftInstantDeadzoneEnabled = Self._gameControllerLeftInstantDeadzoneConfig.load()
        gameControllerLeftNegativeDeadzone = Self._gameControllerLeftNegativeDeadzoneConfig.load()
        gameControllerRightInstantDeadzoneEnabled = Self._gameControllerRightInstantDeadzoneConfig.load()
        gameControllerRightNegativeDeadzone = Self._gameControllerRightNegativeDeadzoneConfig.load()
        invertLeftStickX = Self._invertLeftStickXConfig.load()
        invertLeftStickY = Self._invertLeftStickYConfig.load()
        invertRightStickX = Self._invertRightStickXConfig.load()
        invertRightStickY = Self._invertRightStickYConfig.load()
        appLanguage = Self._appLanguageConfig.load()
        controllerMultitapMode = Self._controllerMultitapModeConfig.load()
        if !Self._controllerMacroQuickMenuSelectPauseMigration.load() {
            if Self._controllerMacroQuickMenuConfig.load() == ControllerMacroBinding(first: .l3, second: .r3) {
                Self._controllerMacroQuickMenuConfig.write(ControllerMacroAction.quickMenu.defaultBinding)
            }
            Self._controllerMacroQuickMenuSelectPauseMigration.write(true)
        }
        if !Self._controllerMacroStartDefaultsMigration.load() {
            let previousStartDefaults: [(Setting<ControllerMacroBinding>, ControllerMacroBinding, ControllerMacroAction)] = [
                (Self._controllerMacroSaveGameStateConfig, ControllerMacroBinding(first: .r3, second: .circle), .saveGameState),
                (Self._controllerMacroLoadGameStateConfig, ControllerMacroBinding(first: .r3, second: .triangle), .loadGameState),
                (Self._controllerMacroIncreaseSpeedConfig, ControllerMacroBinding(first: .l3, second: .dpadRight), .increaseSpeed),
                (Self._controllerMacroDecreaseSpeedConfig, ControllerMacroBinding(first: .l3, second: .dpadLeft), .decreaseSpeed),
                (Self._controllerMacroEnableFastForwardConfig, ControllerMacroBinding(first: .l3, second: .dpadUp), .enableFastForward),
                (Self._controllerMacroDisableFastForwardConfig, ControllerMacroBinding(first: .l3, second: .dpadDown), .disableFastForward),
            ]
            for (setting, previousDefault, action) in previousStartDefaults where setting.load() == previousDefault {
                setting.write(action.defaultBinding)
            }
            Self._controllerMacroStartDefaultsMigration.write(true)
        }
        controllerMacroQuickMenu = Self._controllerMacroQuickMenuConfig.load()
        controllerMacroSaveGameState = Self._controllerMacroSaveGameStateConfig.load()
        controllerMacroLoadGameState = Self._controllerMacroLoadGameStateConfig.load()
        controllerMacroIncreaseSpeed = Self._controllerMacroIncreaseSpeedConfig.load()
        controllerMacroDecreaseSpeed = Self._controllerMacroDecreaseSpeedConfig.load()
        controllerMacroEnableFastForward = Self._controllerMacroEnableFastForwardConfig.load()
        controllerMacroDisableFastForward = Self._controllerMacroDisableFastForwardConfig.load()
        controllerMacroUndoSaveState = Self._controllerMacroUndoSaveStateConfig.load()
        autoOpenStikDebug = Self._autoOpenStikDebugConfig.load()
        // Not load(): older builds wrote names this enum no longer spells that way.
        jitScriptProtocol = Self.loadedJITScriptProtocol()
        // The migration above already wrote the post-migration INI values;
        // these reads pick them up.
        framePacingPreset = FramePacingPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/FramePacing", key: "Preset", defaultValue: Int32(FramePacingPreset.optimal.rawValue)))) ?? .optimal
        adaptiveResolutionEnabled = Self._adaptiveResolutionEnabledConfig.load()
        dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
        dev9HddFile = Self._dev9HddFileConfig.load()
        dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
        dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        dev9InterceptDHCP = Self._dev9InterceptDHCPConfig.load()
        dev9EthLogDHCP = Self._dev9EthLogDHCPConfig.load()
        dev9EthLogDNS = Self._dev9EthLogDNSConfig.load()
        dev9DNS1Mode = Self._dev9DNS1ModeConfig.load()
        dev9DNS1 = Self._dev9DNS1Config.load()
        dev9DNS2Mode = Self._dev9DNS2ModeConfig.load()
        dev9DNS2 = Self._dev9DNS2Config.load()
        BackgroundStorage.migrateLegacyBackgroundsIfNeeded()
        dynamicBackgroundsEnabled = UserDefaults.standard.object(
            forKey: "ARMSX2iOSDynamicBackgroundsEnabled"
        ) as? Bool ?? true
        var loadedDynamicAppearance =
            DynamicAppearancePreferences.load() ?? .standard
        let storedThemePreset = ControllerUIThemePreset(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerUIThemePreset"
            ) ?? ""
        )
        if storedThemePreset == .defaultTheme,
           loadedDynamicAppearance.particleSettings.armsx2LogoEnabled == nil {
            // The logo option was added after existing Default-theme payloads
            // had already been persisted. Upgrade only the missing value;
            // an explicit user-off selection remains respected.
            loadedDynamicAppearance.particleSettings.armsx2LogoEnabled = true
            loadedDynamicAppearance.save()
        }
        dynamicAppearancePreferences = loadedDynamicAppearance
        let storedClearGlass = UserDefaults.standard.object(
            forKey: "ARMSX2iOSClearLiquidGlassUI"
        ) as? Bool ?? true
        clearLiquidGlassUI = storedClearGlass
        // New in this release, so an upgrade starts from the main toggle.
        clearLiquidGlassUISubSettings = UserDefaults.standard.object(
            forKey: "ARMSX2iOSClearLiquidGlassUISubSettings"
        ) as? Bool ?? storedClearGlass
        clearLiquidGlassUIQuickMenu = UserDefaults.standard.object(
            forKey: "ARMSX2iOSClearLiquidGlassUIQuickMenu"
        ) as? Bool ?? false
        let perGameRegularGlassMigrationKey =
            "ARMSX2iOSPerGameSettingsRegularGlassMigrationV1"
        if !UserDefaults.standard.bool(forKey: perGameRegularGlassMigrationKey) {
            // Restore the earlier regular (non-clear) Liquid Glass presentation
            // once for existing installs as well as new ones. Users can still
            // opt into Clear independently afterward.
            clearLiquidGlassUIPerGameSettingsLibrary = false
            clearLiquidGlassUIPerGameSettingsEmulation = false
            UserDefaults.standard.set(true, forKey: perGameRegularGlassMigrationKey)
        } else {
            clearLiquidGlassUIPerGameSettingsLibrary = UserDefaults.standard.object(
                forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsLibrary"
            ) as? Bool ?? false
            clearLiquidGlassUIPerGameSettingsEmulation = UserDefaults.standard.object(
                forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsEmulation"
            ) as? Bool ?? false
        }
        temporalSaveStateToLivePreviewChanges = UserDefaults.standard.object(
            forKey: "ARMSX2iOSTemporalSaveStateToLivePreviewChanges"
        ) as? Bool ?? true
        perGameLivePreviewDuration = min(
            max(
                UserDefaults.standard.object(
                    forKey: "ARMSX2iOSPerGameLivePreviewDuration"
                ) as? Double ?? 2,
                Self.perGameLivePreviewDurationRange.lowerBound
            ),
            Self.perGameLivePreviewDurationRange.upperBound
        )
        perGameBeforeChangesPreviewDuration = min(
            max(
                UserDefaults.standard.double(
                    forKey: "ARMSX2iOSPerGameBeforeChangesPreviewDuration"
                ),
                Self.perGameBeforeChangesPreviewDurationRange.lowerBound
            ),
            Self.perGameBeforeChangesPreviewDurationRange.upperBound
        )
        perGameLivePreviewStopsWithCircle = UserDefaults.standard.object(
            forKey: "ARMSX2iOSPerGameLivePreviewStopsWithCircle"
        ) as? Bool ?? true
        gameCardZoomAnimationEnabled = UserDefaults.standard.object(
            forKey: "ARMSX2iOSGameCardZoomAnimationEnabled"
        ) as? Bool ?? true
        favoriteGlowingEffectEnabled = UserDefaults.standard.object(
            forKey: "ARMSX2iOSFavoriteGlowingEffectEnabled"
        ) as? Bool ?? false
        gameMenuCoverColoursEnabled = UserDefaults.standard.object(
            forKey: "ARMSX2iOSGameMenuCoverColoursEnabled"
        ) as? Bool ?? true
        hideGameplayStatusBar = UserDefaults.standard.object(
            forKey: "ARMSX2iOSHideGameplayStatusBar"
        ) as? Bool ?? true
        hideIntroStatusBar = UserDefaults.standard.object(
            forKey: "ARMSX2iOSHideIntroStatusBar"
        ) as? Bool ?? true
        hideMenuStatusBar = UserDefaults.standard.object(
            forKey: "ARMSX2iOSHideMenuStatusBar"
        ) as? Bool ?? false
        focusOrbsEnabled = UserDefaults.standard.object(
            forKey: "ARMSX2iOSFocusOrbsEnabled"
        ) as? Bool ?? false
        let storedControllerUIThemePreset = ControllerUIThemePreset(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerUIThemePreset"
            ) ?? ""
        ) ?? .defaultTheme
        controllerUIThemePreset = storedControllerUIThemePreset
        controllerNavigationDepthEffectEnabled = UserDefaults.standard.object(
            forKey: "ARMSX2iOSControllerNavigationDepthEffectEnabled"
        ) as? Bool ?? true
        controllerFocusBoxStyle = ControllerFocusBoxStyle(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerFocusBoxStyle"
            ) ?? ""
        ) ?? .neonBlue
        controllerNavigationFocusAnimation = ControllerNavigationFocusTravelStyle(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerNavigationFocusAnimation"
            ) ?? ""
        ) ?? .easeInOut
        controllerFocusBoxPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerFocusBoxPalette"
            ) ?? ""
        ) ?? .multicolor
        controllerFocusBoxCustomColor = Self.loadSavedPaletteColor(
            forKey: "ARMSX2iOSControllerFocusBoxCustomColor"
        )
        controllerOrbPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerOrbPalette"
            ) ?? ""
        ) ?? .blue
        controllerNavigationAccentPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerNavigationAccentPalette"
            ) ?? ""
        ) ?? .henyBlue
        controllerNavigationCustomAccentColor = Self.loadSavedPaletteColor(
            forKey: "ARMSX2iOSControllerNavigationCustomAccentColor"
        )
        controllerTextPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerTextPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerTextCustomColor = Self.loadSavedPaletteColor(
            forKey: "ARMSX2iOSControllerTextCustomColor"
        )
        controllerSecondaryTextPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerSecondaryTextPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerCriticalTextPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerCriticalTextPalette"
            ) ?? ""
        ) ?? .crimson
        controllerTabTitlePalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerTabTitlePalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerTabSubtitlePalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerTabSubtitlePalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerBottomTabBarPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerBottomTabBarPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerBottomTabBarUnselectedPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerBottomTabBarUnselectedPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerCardTitlePalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerCardTitlePalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerContextMenuPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerContextMenuPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerImportActionPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerImportActionPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerToolbarPalette = UserDefaults.standard.string(
            forKey: "ARMSX2iOSControllerToolbarPalette"
        ).flatMap(ThemePalette.init(rawValue:))
        controllerFocusedTextPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerFocusedTextPalette"
            ) ?? ""
        ) ?? .blue
        controllerFocusedTextCustomColor = Self.loadSavedPaletteColor(
            forKey: "ARMSX2iOSControllerFocusedTextCustomColor"
        )
        controllerTextShadowStrength = min(
            max(
                UserDefaults.standard.object(
                    forKey: "ARMSX2iOSControllerTextShadowStrength"
                ) as? Double ?? 0,
                0
            ),
            1
        )
        controllerTextShadowPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerTextShadowPalette"
            ) ?? ""
        ) ?? .obsidian
        controllerTextShadowCustomColor = Self.loadSavedPaletteColor(
            forKey: "ARMSX2iOSControllerTextShadowCustomColor"
        )
        controllerFocusedTextShadowStrength = min(
            max(
                UserDefaults.standard.object(
                    forKey: "ARMSX2iOSControllerFocusedTextShadowStrength"
                ) as? Double ?? 0.1,
                0
            ),
            1
        )
        controllerFocusedTextShadowPalette = ThemePalette(
            rawValue: UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerFocusedTextShadowPalette"
            ) ?? ""
        ) ?? .obsidian
        controllerFocusedTextShadowCustomColor = Self.loadSavedPaletteColor(
            forKey: "ARMSX2iOSControllerFocusedTextShadowCustomColor"
        )
        controllerFocusBoxAnimationSpeed = min(
            max(
                UserDefaults.standard.object(
                    forKey: "ARMSX2iOSControllerFocusBoxAnimationSpeed"
                ) as? Double ?? 1,
                0.25
            ),
            2
        )
        controllerFocusBoxGlowIntensity = min(
            max(
                UserDefaults.standard.object(
                    forKey: "ARMSX2iOSControllerFocusBoxGlowIntensity"
                ) as? Double ?? 1,
                0
            ),
            2
        )
        backgroundPrimaryAsset = Self.loadBackgroundAsset(forKey: "ARMSX2iOSBackgroundPrimaryAsset")
        backgroundLandscapeAsset = Self.loadBackgroundAsset(forKey: "ARMSX2iOSBackgroundLandscapeAsset")
        backgroundFitMode = BackgroundFitMode(rawValue: UserDefaults.standard.string(forKey: "ARMSX2iOSBackgroundFitMode") ?? "") ?? .fill
        backgroundLandscapeFitMode = BackgroundFitMode(rawValue: UserDefaults.standard.string(forKey: "ARMSX2iOSBackgroundLandscapeFitMode") ?? "") ?? .fill
        backgroundVideoMuted = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundVideoMuted") as? Bool ?? true
        backgroundEnabledInBIOS = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInBIOS") as? Bool ?? true
        backgroundEnabledInHelp = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInHelp") as? Bool ?? true
        backgroundEnabledInSettings = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInSettings") as? Bool ?? true
        backgroundDim = Self.clampedBackgroundDim(UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundDim") as? Double ?? 0.0)
        normalizeDEV9Settings()
        VPadSkinLibraryStore.shared.adoptLegacySelection(virtualPadSkin)
        Self._aspectRatioConfig.write(aspectRatio)
        // The saved per-item flags are the source of truth, so applying the preset here
        // would rewrite every one of them. Seed the Custom snapshot once instead, or
        // cycling to Custom shows an empty overlay.
        if !ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "OsdCustomSeeded", defaultValue: false) {
            snapshotCustomOsd()
            ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "OsdCustomSeeded", value: true)
        }
        // Defer starting the adaptive controller to the next run loop: setEnabled
        // reads SettingsStore.shared.upscaleMultiplier, which would re-enter this
        // init's swift_once and deadlock dispatch_once.
        DispatchQueue.main.async { [self] in
            FrameTimeDynamicResolutionController.shared.setEnabled(self.adaptiveResolutionEnabled)
        }
    }


}
