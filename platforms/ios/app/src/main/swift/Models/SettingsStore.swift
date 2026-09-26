// SettingsStore.swift — INI-backed settings for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

/// [P51] OSD preset levels
enum OsdPreset: Int, CaseIterable {
    case off = 0
    case simple = 1    // FPS + speed + CPU usage + device stats
    case detail = 2    // All except frame times graph
    case full = 3      // Everything
    case custom = 4    // User-defined toggle set (not derived from a preset table)

    var label: String {
        switch self {
        case .off: return "OFF"
        case .simple: return "Simple"
        case .detail: return "Detail"
        case .full: return "Full"
        case .custom: return "Custom"
        }
    }
}

/// Frame Pacing presets. Mirrors the OsdPreset shape so the consolidated
/// Settings panel and the per-game tab can drive a single picker that fans
/// out to the underlying EmuCore/GS + SPU2/Output + Framerate keys.
enum FramePacingPreset: Int, CaseIterable, Identifiable {
    case optimal = 0       // ARMSX2-tuned default for fresh installs
    case smooth = 1        // larger queues / buffers for visual stability
    case lowLatency = 2    // tight queues + low audio latency for input feel
    case batterySaver = 3  // 45 fps cap + larger audio buffer
    case custom = 4        // user-tweaked; not derived from the table

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .optimal: return "Optimal"
        case .smooth: return "Smooth"
        case .lowLatency: return "Low Latency"
        case .batterySaver: return "Battery Saver"
        case .custom: return "Custom"
        }
    }
}

enum JITScriptProtocol: String, CaseIterable, Identifiable {
    case universal
    case legacy

    var id: String { rawValue }

    var label: String {
        switch self {
        case .universal:
            return "Universal"
        case .legacy:
            return "Legacy"
        }
    }

    var subtitle: String {
        switch self {
        case .universal:
            return "Uses brk #0xf00d prepare + detach."
        case .legacy:
            return "Uses the iOS 17/18 scriptless/legacy JIT path."
        }
    }

    static var defaultValue: JITScriptProtocol {
        ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 26 ? .universal : .legacy
    }

    static func normalized(_ rawValue: String) -> JITScriptProtocol {
        switch rawValue.lowercased() {
        case "legacy", "utm-dolphin", "utm_dolphin":
            return .legacy
        default:
            return .universal
        }
    }
}

/// A manual per-fix toggle under EmuCore/Gamefixes. The `key` is the exact PCSX2
/// config key; `label` is the localized user-facing name.
struct GameFixOption: Identifiable, Hashable {
    let key: String
    let label: String
    var id: String { key }
}

/// Which analog stick an axis-inversion setting applies to.
enum StickSide: String, CaseIterable, Identifiable {
    case left, right
    var id: String { rawValue }
}

/// Every numeric setting a screen can show, described once next to the ranges below. A
/// call site names the setting and hands over its binding, so there is nothing left for
/// it to spell differently.
///
/// Stops sit where the useful values are rather than spread evenly, and every list holds
/// the default so the reset arrow lands on one. Typing reaches anything in between.
@MainActor
extension NumberSetting {
    static let targetFPS = NumberSetting(
        "FPS Target",
        in: Double(SettingsStore.minTargetFPS)...Double(SettingsStore.maxTargetFPS),
        format: .framesPerSecond.compactDecimals(3),
        detents: [15, 23.976, 29.94, 30, 45, 59.97, 60, 90, 120],
        default: Double(SettingsStore.defaultTargetFPS))

    static let fastForwardSpeed = NumberSetting(
        "Fast Forward Speed",
        in: Double(SettingsStore.minFastForwardScalar)...Double(SettingsStore.maxFastForwardScalar),
        format: .unitPercent, detents: [1.25, 1.5, 2, 3, 5, 10],
        default: Double(SettingsStore.defaultFastForwardScalar))

    /// A stepper: fifteen contiguous values, and people come here wanting an exact one rather than
    /// somewhere in the region of one.
    static let vsyncQueueSize = NumberSetting(
        "Queue Size", in: SettingsStore.vsyncQueueRange, style: .stepper, default: 8)

    static let audioBufferMs = NumberSetting(
        "Buffer Size", in: SettingsStore.audioBufferMsRange, format: .milliseconds,
        detents: [10, 20, 30, 50, 75, 100, 150, 200], default: 50)

    static let audioOutputLatencyMs = NumberSetting(
        "Output Latency", in: SettingsStore.audioOutputLatencyMsRange, format: .milliseconds,
        detents: [5, 10, 15, 20, 30, 50, 100, 200], default: 20)

    static let emulatorVolume = NumberSetting(
        "Emulator Volume", in: SettingsStore.emulatorVolumeRange, format: .percent,
        detents: [0, 25, 50, 75, 100, 125, 150],
        default: SettingsStore.defaultEmulatorVolumePercent,
        hint: "Adjusts emulator game audio without changing iOS system volume or other apps.")

    static let fastForwardVolume = NumberSetting(
        "Fast-Forward Volume", in: SettingsStore.fastForwardVolumeRange, format: .percent,
        detents: [0, 25, 50, 75, 100, 150, 200], default: 100)

    /// Stored as a percentage. Driving it from a 0-to-1 slider truncated it on every drag tick.
    static let casSharpness = NumberSetting(
        "CAS Sharpness", in: SettingsStore.casSharpnessRange, format: .percent,
        detents: [0, 25, 50, 75, 100], default: 50)

    static let shadeBoostBrightness = shadeBoost("Brightness")
    static let shadeBoostContrast = shadeBoost("Contrast")
    static let shadeBoostSaturation = shadeBoost("Saturation")
    static let shadeBoostGamma = shadeBoost("Gamma")

    /// Four settings that differ only in name. Both screens that show them put them under a Shade
    /// Boost header, which is what lets the names stay this short.
    private static func shadeBoost(_ title: String) -> NumberSetting {
        NumberSetting(title, in: SettingsStore.shadeBoostRange, format: .percent,
                      detents: [1, 25, 50, 75, 100], default: 50)
    }

    static let emulationOnlyModeTimer = NumberSetting(
        "Emulation-Only Mode Timer", in: SettingsStore.emulationOnlyModeDelayRange,
        format: .seconds.decimals(0), detents: [0, 2, 5, 10, 15],
        default: SettingsStore.defaultEmulationOnlyModeDelaySeconds)

    /// Tenths rather than quarters like its neighbours: this is the one control you set by eye
    /// against your own wallpaper, and a quarter of the way is a huge jump in how dark the thing is.
    static let backgroundDim = NumberSetting(
        "Background Dim", in: 0...1, format: .unitPercent,
        detents: [0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1],
        default: 0, icon: "circle.lefthalf.filled")

    static let padOpacity = NumberSetting(
        "Opacity", in: 0.1...1, format: .unitPercent,
        detents: [0.1, 0.25, 0.4, 0.6, 0.8, 1], default: 0.6)

    static let analogStickSize = NumberSetting(
        "Analog Stick Size", in: 0.8...1.6, format: .unitPercent,
        detents: [0.8, 0.9, 1, 1.2, 1.4, 1.6], default: 1)

    static let phoneRumbleStrength = NumberSetting(
        "Phone Rumble Strength", in: 0...1, format: .unitPercent,
        detents: [0, 0.25, 0.5, 0.75, 1], default: 0.25)

    static let gameRumbleStrength = NumberSetting(
        "Game Rumble Strength", in: 0...2, format: .unitPercent,
        step: 0.01, default: 1,
        hint: "100% preserves the original game rumble. 200% applies 2x strength.")

    static let gameControllerDeadZone = NumberSetting(
        "Dead Zone", in: 0...0.25, format: .unitPercent,
        step: 0.01, default: 0.15,
        hint: "Ignores stick movement inside this range.")

    static let gameControllerLeftNegativeDeadzone = NumberSetting(
        "Left Negative Deadzone", in: -0.25...0, format: .unitPercent,
        step: 0.01, default: -0.08,
        hint: "Sets the left stick's minimum output after it leaves the dead zone.")

    static let gameControllerRightNegativeDeadzone = NumberSetting(
        "Right Negative Deadzone", in: -0.25...0, format: .unitPercent,
        step: 0.01, default: -0.08,
        hint: "Sets the right stick's minimum output after it leaves the dead zone.")

    static let uiRumbleStrength = NumberSetting(
        "UI Rumble Strength", in: 0...2, format: .unitPercent,
        step: 0.01, default: 1,
        hint: "100% preserves the original UI rumble. 200% applies up to 2x gain.")

    // Typed fields, not sliders: these get copied verbatim off a compatibility list, so reaching
    // an exact number matters and dragging towards one does not.
    static let textureOffsetX = NumberSetting(
        "Texture Offset X", in: SettingsStore.textureOffsetRange, style: .field)
    static let textureOffsetY = NumberSetting(
        "Texture Offset Y", in: SettingsStore.textureOffsetRange, style: .field)
    static let skipDrawStart = NumberSetting(
        "Skipdraw Start", in: SettingsStore.skipDrawRange, style: .field)
    static let skipDrawEnd = NumberSetting(
        "Skipdraw End", in: SettingsStore.skipDrawRange, style: .field)
    static let cpuSpriteRenderBw = NumberSetting(
        "CPU Sprite Render BW", in: SettingsStore.cpuSpriteRenderBwRange, style: .field)
}

@MainActor
@Observable
final class SettingsStore {
    static let shared = SettingsStore()
    static let minTargetFPS: Float = 15.0
    static let maxTargetFPS: Float = 120.0
    static let defaultTargetFPS: Float = 60.0
    static let minFastForwardScalar: Float = 1.25
    static let maxFastForwardScalar: Float = 10.0
    static let defaultFastForwardScalar: Float = 2.0
    static let defaultEmulatorVolumePercent = 100
    static let textureOffsetRange = -4096...4096
    static let skipDrawRange = 0...5000
    // Named so the stepper, the per-game panel and the global codec all agree.
    static let vsyncQueueRange = 2...16
    static let audioBufferMsRange = 10...200
    static let audioOutputLatencyMsRange = 5...200
    static let fastForwardVolumeRange = 0...200
    static let emulatorVolumeRange = 0...150
    static let shadeBoostRange = 1...100
    static let casSharpnessRange = 0...100
    static let cpuSpriteRenderBwRange = 0...10
    static let defaultOsdPerformancePosition = 3
    static let emulationOnlyModeDelayRange = 0...15
    static let defaultEmulationOnlyModeDelaySeconds = 5

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
    let _iopRecompilerConfig = Setting<Bool>(
        section: "EmuCore/CPU/Recompiler", key: "EnableIOP", default: true,
        codec: .bool)
    var iopRecompiler: Bool = true { didSet { commit(_iopRecompilerConfig, iopRecompiler) } }
    let _vu0RecompilerConfig = Setting<Bool>(
        section: "EmuCore/CPU/Recompiler", key: "EnableVU0", default: true,
        codec: .bool)
    var vu0Recompiler: Bool = true { didSet { commit(_vu0RecompilerConfig, vu0Recompiler) } }
    let _vu1RecompilerConfig = Setting<Bool>(
        section: "EmuCore/CPU/Recompiler", key: "EnableVU1", default: true,
        codec: .bool)
    var vu1Recompiler: Bool = true { didSet { commit(_vu1RecompilerConfig, vu1Recompiler) } }
    // writes GameISO/FastBoot + EmuCore/EnableFastBoot
    var fastBoot: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("GameISO", key: "FastBoot", value: fastBoot)
            ARMSX2Bridge.setINIBool("EmuCore", key: "EnableFastBoot", value: fastBoot)
        }
    }
    let _automaticLoadLastSaveStateConfig = Setting<Bool>(
        section: "ARMSX2iOS/Boot", key: "AutomaticLoadLastSaveState", default: false,
        codec: .bool)
    var automaticLoadLastSaveState = false {
        didSet { commit(_automaticLoadLastSaveStateConfig, automaticLoadLastSaveState) }
    }
    let _automaticLoadLastGameConfig = Setting<Bool>(
        section: "ARMSX2iOS/Boot", key: "AutomaticLoadLastGame", default: false,
        codec: .bool)
    var automaticLoadLastGame = false {
        didSet { commit(_automaticLoadLastGameConfig, automaticLoadLastGame) }
    }
    // writes ManualFastmem + EnableFastmem
    var fastmem: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/Speedhacks", key: "ManualFastmem", value: true)
            ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", value: fastmem)
        }
    }
    let _emulationOnlyModeConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyMode", default: false,
        codec: .bool)
    var emulationOnlyModeEnabled: Bool = false { didSet { commit(_emulationOnlyModeConfig, emulationOnlyModeEnabled) } }
    let _emulationOnlyDisablePatchesConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisablePatches", default: true,
        codec: .bool)
    var emulationOnlyDisablePatches: Bool = true {
        didSet { commit(_emulationOnlyDisablePatchesConfig, emulationOnlyDisablePatches) }
    }
    // Discord presence is always released by Emulation-Only Mode.
    let emulationOnlyDisableDiscordPresence = true
    let _emulationOnlyDisablePINEConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisablePINE", default: true,
        codec: .bool)
    var emulationOnlyDisablePINE: Bool = true {
        didSet { commit(_emulationOnlyDisablePINEConfig, emulationOnlyDisablePINE) }
    }
    let _emulationOnlyDisableRetroAchievementsConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableRetroAchievements", default: true,
        codec: .bool)
    var emulationOnlyDisableRetroAchievements: Bool = true {
        didSet { commit(_emulationOnlyDisableRetroAchievementsConfig, emulationOnlyDisableRetroAchievements) }
    }
    let _emulationOnlyDisableInputRecordingConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableInputRecording", default: true,
        codec: .bool)
    var emulationOnlyDisableInputRecording: Bool = true {
        didSet { commit(_emulationOnlyDisableInputRecordingConfig, emulationOnlyDisableInputRecording) }
    }
    let _emulationOnlyDisableOSDConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableOSD", default: true,
        codec: .bool)
    var emulationOnlyDisableOSD: Bool = true {
        didSet { commit(_emulationOnlyDisableOSDConfig, emulationOnlyDisableOSD) }
    }
    let _emulationOnlyDisableFramePacingConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableFramePacing", default: true,
        codec: .bool)
    var emulationOnlyDisableFramePacing: Bool = true {
        didSet { commit(_emulationOnlyDisableFramePacingConfig, emulationOnlyDisableFramePacing) }
    }
    let _emulationOnlyDisableVirtualControlsConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableVirtualControls", default: true,
        codec: .bool)
    var emulationOnlyDisableVirtualControls: Bool = true {
        didSet { commit(_emulationOnlyDisableVirtualControlsConfig, emulationOnlyDisableVirtualControls) }
    }
    let _emulationOnlyDisableQuickMenuConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableQuickMenu", default: true,
        codec: .bool)
    var emulationOnlyDisableQuickMenu: Bool = true {
        didSet { commit(_emulationOnlyDisableQuickMenuConfig, emulationOnlyDisableQuickMenu) }
    }
    let _emulationOnlyClearNetworkCacheConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyClearNetworkCache", default: true,
        codec: .bool)
    var emulationOnlyClearNetworkCache: Bool = true {
        didSet { commit(_emulationOnlyClearNetworkCacheConfig, emulationOnlyClearNetworkCache) }
    }
    let _emulationOnlyModeDelayConfig = Setting<Int>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyModeDelaySeconds",
        default: SettingsStore.defaultEmulationOnlyModeDelaySeconds,
        codec: .int(in: SettingsStore.emulationOnlyModeDelayRange))
    var emulationOnlyModeDelaySeconds = SettingsStore.defaultEmulationOnlyModeDelaySeconds { didSet {
        let clamped = Self.clamped(emulationOnlyModeDelaySeconds, to: Self.emulationOnlyModeDelayRange)
        guard emulationOnlyModeDelaySeconds == clamped else {
            emulationOnlyModeDelaySeconds = clamped
            return
        }
        commit(_emulationOnlyModeDelayConfig, emulationOnlyModeDelaySeconds)
    }}

    // ── CPU Rounding & Clamping ──
    // Clamp modes are one 0-3 level here, unpacked to the three (EE) or six (VU0+VU1)
    // boolean keys the recompiler reads. Changes take effect on next boot.
    let _eeFpuRoundModeConfig = Setting<Int>(
        section: "EmuCore/CPU", key: "FPU.Roundmode", default: 3,
        codec: .roundMode)
    var eeFpuRoundMode: Int = 3 { didSet { commit(_eeFpuRoundModeConfig, eeFpuRoundMode) } }
    let _vu0RoundModeConfig = Setting<Int>(
        section: "EmuCore/CPU", key: "VU0.Roundmode", default: 3,
        codec: .roundMode)
    var vu0RoundMode: Int = 3 { didSet { commit(_vu0RoundModeConfig, vu0RoundMode) } }
    let _vu1RoundModeConfig = Setting<Int>(
        section: "EmuCore/CPU", key: "VU1.Roundmode", default: 3,
        codec: .roundMode)
    var vu1RoundMode: Int = 3 { didSet { commit(_vu1RoundModeConfig, vu1RoundMode) } }
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
    let _audioTimeStretchConfig = Setting<Bool>(
        section: "SPU2/Output", key: "SyncMode", default: true,
        codec: .timeStretch)
    var audioTimeStretch: Bool = true { didSet { commit(_audioTimeStretchConfig, audioTimeStretch) } }
    let _audioBufferMsConfig = Setting<Int>(
        section: "SPU2/Output", key: "BufferMS", default: 50,
        codec: .int(in: SettingsStore.audioBufferMsRange))
    var audioBufferMs: Int = 50 { didSet {
        commit(_audioBufferMsConfig, audioBufferMs)
        if audioBufferMs != oldValue { markFramePacingCustom() }
    }}
    let _audioOutputLatencyMsConfig = Setting<Int>(
        section: "SPU2/Output", key: "OutputLatencyMS", default: 20,
        codec: .int(in: SettingsStore.audioOutputLatencyMsRange))
    var audioOutputLatencyMs: Int = 20 { didSet {
        commit(_audioOutputLatencyMsConfig, audioOutputLatencyMs)
        if audioOutputLatencyMs != oldValue { markFramePacingCustom() }
    }}
    let _audioFastForwardVolumeConfig = Setting<Int>(
        section: "SPU2/Output", key: "FastForwardVolume", default: 100,
        codec: .int(in: SettingsStore.fastForwardVolumeRange))
    var audioFastForwardVolume: Int = 100 { didSet { commit(_audioFastForwardVolumeConfig, audioFastForwardVolume) } }
    let _audioSwapChannelsConfig = Setting<Bool>(
        section: "SPU2/Output", key: "SwapChannels", default: false,
        codec: .bool)
    var audioSwapChannels: Bool = false { didSet { commit(_audioSwapChannelsConfig, audioSwapChannels) } }
    var ntscFramerate: Float {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "FramerateNTSC", value: ntscFramerate)
            applyFrameLimiterSettings()
            requestGraphicsApplyGuarded()
        }
    }
    let _palFramerateConfig = Setting<Float>(
        section: "EmuCore/GS", key: "FrameratePAL", default: 50.0,
        codec: .float)
    var palFramerate: Float = 50.0 { didSet { commit(_palFramerateConfig, palFramerate) } }

    // ── Boot ──
    let _fastCDVDConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "fastCDVD", default: false,
        codec: .bool)
    var fastCDVD: Bool = false { didSet { commit(_fastCDVDConfig, fastCDVD) } }

    // ── Advanced Speedhacks ──
    let _eeCycleRateConfig = Setting<Int>(
        section: "EmuCore/Speedhacks", key: "EECycleRate", default: 0,
        codec: .int)
    var eeCycleRate: Int = 0 { didSet { commit(_eeCycleRateConfig, eeCycleRate) } }
    let _vu1InstantConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "vu1Instant", default: true,
        codec: .bool)
    var vu1Instant: Bool = true { didSet { commit(_vu1InstantConfig, vu1Instant) } }
    // writes ManualMTVU + ManualMTVUVersion + vuThread
    var mtvu: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/Speedhacks", key: "ManualMTVU", value: true)
            ARMSX2Bridge.setINIInt("ARMSX2iOS/Speedhacks", key: "ManualMTVUVersion", value: 3)
            ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vuThread", value: mtvu)
        }
    }
    let _waitLoopConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "WaitLoop", default: true,
        codec: .bool)
    var waitLoop: Bool = true { didSet { commit(_waitLoopConfig, waitLoop) } }
    let _intcStatConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "IntcStat", default: true,
        codec: .bool)
    var intcStat: Bool = true { didSet { commit(_intcStatConfig, intcStat) } }
    let _eeCycleSkipConfig = Setting<Int>(
        section: "EmuCore/Speedhacks", key: "EECycleSkip", default: 0,
        codec: .cycleSkip)
    var eeCycleSkip: Int = 0 { didSet { commit(_eeCycleSkipConfig, eeCycleSkip) } }
    let _vuFlagHackConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "vuFlagHack", default: true,
        codec: .bool)
    var vuFlagHack: Bool = true { didSet { commit(_vuFlagHackConfig, vuFlagHack) } }
    let _enableCheatsConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableCheats", default: false,
        suppressible: false,
        codec: .bool)
    var enableCheats: Bool = false { didSet { commit(_enableCheatsConfig, enableCheats) } }
    let _enablePatchesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnablePatches", default: true,
        suppressible: false,
        codec: .bool)
    var enablePatches: Bool = true { didSet { commit(_enablePatchesConfig, enablePatches) } }
    let _enableGameFixesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableGameFixes", default: true,
        suppressible: false,
        codec: .bool)
    var enableGameFixes: Bool = true { didSet { commit(_enableGameFixesConfig, enableGameFixes) } }
    let _enableGameDBHardwareFixesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks", default: true,
        suppressible: false,
        codec: .inverted)
    var enableGameDBHardwareFixes: Bool = true {
        didSet { commit(_enableGameDBHardwareFixesConfig, enableGameDBHardwareFixes) }
    }
    let _enableWidescreenPatchesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableWideScreenPatches", default: false,
        suppressible: false,
        codec: .bool)
    var enableWidescreenPatches: Bool = false {
        didSet { commit(_enableWidescreenPatchesConfig, enableWidescreenPatches) }
    }
    let _enableNoInterlacingPatchesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableNoInterlacingPatches", default: false,
        suppressible: false,
        codec: .bool)
    var enableNoInterlacingPatches: Bool = false {
        didSet { commit(_enableNoInterlacingPatchesConfig, enableNoInterlacingPatches) }
    }
    let _hostFilesystemConfig = Setting<Bool>(
        section: "EmuCore", key: "HostFs", default: false,
        suppressible: false,
        codec: .bool)
    var hostFilesystem: Bool = false { didSet { commit(_hostFilesystemConfig, hostFilesystem) } }

    // ── Manual Game Fixes (EmuCore/Gamefixes/<key>) ──
    // Dictionary-backed because the 17 fixes are homogeneous toggles. Effective only
    // while GameDB Core Fixes (enableGameFixes) is on. Toggling one writes only its
    // own INI key.
    var gameFixes: [String: Bool] = [:]

    var isMetalFXAvailable: Bool {
        ARMSX2Bridge.isMetalFXSupported()
    }

    // Boot-only on purpose: a live switch is a restart option, so applying it
    // would send GSUpdateConfig down GSreopen and tear the Metal device down
    // under the running game. The picker says "Requires restart".
    let _rendererConfig = Setting<Int>(
        section: "EmuCore/GS", key: "Renderer", default: 17,
        suppressible: false,
        bootOnly: true,
        codec: .int)
    var renderer: Int = 17 { didSet { commit(_rendererConfig, renderer) } }
    let _upscaleMultiplierConfig = Setting<Float>(
        section: "EmuCore/GS", key: "upscale_multiplier", default: 1.0,
        suppressible: false,
        codec: .float)
    var upscaleMultiplier: Float = 1.0 { didSet { commit(_upscaleMultiplierConfig, upscaleMultiplier) } }
    let _vsyncQueueSizeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "VsyncQueueSize", default: 8,
        suppressible: false,
        codec: .int(in: SettingsStore.vsyncQueueRange))
    var vsyncQueueSize: Int = 8 { didSet {
        commit(_vsyncQueueSizeConfig, vsyncQueueSize)
        if vsyncQueueSize != oldValue { markFramePacingCustom() }
    }}
    let _textureFilteringConfig = Setting<Int>(
        section: "EmuCore/GS", key: "filter", default: 2,
        suppressible: false,
        codec: .int)
    var textureFiltering: Int = 2 { didSet { commit(_textureFilteringConfig, textureFiltering) } }
    // Boot-only for the same reason as the renderer above: the core counts this in
    // RestartOptionsAreEqual, so applying it live goes down GSreopen and tears the
    // Metal device down under the running game. The picker says "Requires restart".
    let _backThreadModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "GSBackThreadMode", default: 0,
        suppressible: false,
        bootOnly: true,
        codec: .int)
    var backThreadMode: Int = 0 { didSet { commit(_backThreadModeConfig, backThreadMode) } }
    let _hardwareMipmappingConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "hw_mipmap", default: true,
        suppressible: false,
        codec: .bool)
    var hardwareMipmapping: Bool = true { didSet { commit(_hardwareMipmappingConfig, hardwareMipmapping) } }
    let _fxaaConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "fxaa", default: false,
        suppressible: false,
        codec: .bool)
    var fxaa: Bool = false { didSet { commit(_fxaaConfig, fxaa) } }
    let _casModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "CASMode", default: 0,
        suppressible: false,
        codec: .int)
    var casMode: Int = 0 { didSet { commit(_casModeConfig, casMode) } }
    let _casSharpnessConfig = Setting<Int>(
        section: "EmuCore/GS", key: "CASSharpness", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.casSharpnessRange))
    var casSharpness: Int = 50 { didSet { commit(_casSharpnessConfig, casSharpness) } }
    let _shaderChainEnabledConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "ShaderChainEnabled", default: false,
        suppressible: false,
        codec: .bool)
    var shaderChainEnabled: Bool = false { didSet {
        commit(_shaderChainEnabledConfig, shaderChainEnabled)
        ARMSX2Bridge.retryShaderChain()
    }}
    // Stored as a ShaderPresetLibrary token, since the container path changes on every install;
    // applyShaderChainSelection writes the absolute ShaderChainPreset that core reads.
    let _shaderChainPresetRefConfig = Setting<String>(
        section: "EmuCore/GS", key: "ShaderChainPresetRef", default: "",
        suppressible: false,
        codec: .string)
    var shaderChainPresetRef: String = "" { didSet {
        commit(_shaderChainPresetRefConfig, shaderChainPresetRef)
        applyShaderChainSelection()
    }}
    let _interlaceModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "deinterlace_mode", default: 0,
        suppressible: false,
        codec: .int)
    var interlaceMode: Int = 0 { didSet { commit(_interlaceModeConfig, interlaceMode) } }
    let _aspectRatioConfig = Setting<Int>(
        section: "EmuCore/GS", key: "AspectRatio", default: 1,
        suppressible: false,
        codec: .aspectRatio)
    var aspectRatio: Int = 1 { didSet { commit(_aspectRatioConfig, aspectRatio) } }
    let _blendingAccuracyConfig = Setting<Int>(
        section: "EmuCore/GS", key: "accurate_blending_unit", default: 1,
        suppressible: false,
        codec: .int)
    var blendingAccuracy: Int = 1 { didSet { commit(_blendingAccuracyConfig, blendingAccuracy) } }
    let _ditheringConfig = Setting<Int>(
        section: "EmuCore/GS", key: "dithering_ps2", default: 2,
        suppressible: false,
        codec: .int)
    var dithering: Int = 2 { didSet { commit(_ditheringConfig, dithering) } }
    let _trilinearFilteringConfig = Setting<Int>(
        section: "EmuCore/GS", key: "TriFilter", default: -1,
        suppressible: false,
        codec: .int)
    var trilinearFiltering: Int = -1 { didSet { commit(_trilinearFilteringConfig, trilinearFiltering) } }
    let _halfPixelOffsetConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_HalfPixelOffset", default: 0,
        suppressible: false,
        codec: .int)
    var halfPixelOffset: Int = 0 { didSet { commit(_halfPixelOffsetConfig, halfPixelOffset) } }
    let _roundSpriteConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_round_sprite_offset", default: 0,
        suppressible: false,
        codec: .int)
    var roundSprite: Int = 0 { didSet { commit(_roundSpriteConfig, roundSprite) } }
    let _alignSpriteConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks_align_sprite_X", default: false,
        suppressible: false,
        codec: .bool)
    var alignSprite: Bool = false { didSet { commit(_alignSpriteConfig, alignSprite) } }
    let _mergeSpriteConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks_merge_pp_sprite", default: false,
        suppressible: false,
        codec: .bool)
    var mergeSprite: Bool = false { didSet { commit(_mergeSpriteConfig, mergeSprite) } }
    let _wildArmsOffsetConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks_ForceEvenSpritePosition", default: false,
        suppressible: false,
        codec: .bool)
    var wildArmsOffset: Bool = false { didSet { commit(_wildArmsOffsetConfig, wildArmsOffset) } }
    // clamps to -4096...4096
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
    // clamps to -4096...4096
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
    // clamps to 0...5000
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
    // clamps to 0...5000
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
    let _loadTextureReplacementsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "LoadTextureReplacements", default: false,
        suppressible: false,
        codec: .bool)
    var loadTextureReplacements: Bool = false {
        didSet { commit(_loadTextureReplacementsConfig, loadTextureReplacements) }
    }
    let _loadTextureReplacementsAsyncConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "LoadTextureReplacementsAsync", default: true,
        suppressible: false,
        codec: .bool)
    var loadTextureReplacementsAsync: Bool = true {
        didSet { commit(_loadTextureReplacementsAsyncConfig, loadTextureReplacementsAsync) }
    }
    let _precacheTextureReplacementsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "PrecacheTextureReplacements", default: false,
        suppressible: false,
        codec: .bool)
    var precacheTextureReplacements: Bool = false {
        didSet { commit(_precacheTextureReplacementsConfig, precacheTextureReplacements) }
    }
    let _texturePreloadingConfig = Setting<Int>(
        section: "EmuCore/GS", key: "texture_preloading", default: 2,
        suppressible: false,
        codec: .int)
    var texturePreloading: Int = 2 { didSet { commit(_texturePreloadingConfig, texturePreloading) } }
    let _dumpReplaceableTexturesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpReplaceableTextures", default: false,
        suppressible: false,
        codec: .bool)
    var dumpReplaceableTextures: Bool = false {
        didSet { commit(_dumpReplaceableTexturesConfig, dumpReplaceableTextures) }
    }
    let _dumpReplaceableMipmapsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpReplaceableMipmaps", default: false,
        suppressible: false,
        codec: .bool)
    var dumpReplaceableMipmaps: Bool = false {
        didSet { commit(_dumpReplaceableMipmapsConfig, dumpReplaceableMipmaps) }
    }
    let _dumpTexturesWithFMVActiveConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpTexturesWithFMVActive", default: false,
        suppressible: false,
        codec: .bool)
    var dumpTexturesWithFMVActive: Bool = false {
        didSet { commit(_dumpTexturesWithFMVActiveConfig, dumpTexturesWithFMVActive) }
    }
    let _dumpDirectTexturesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpDirectTextures", default: true,
        suppressible: false,
        codec: .bool)
    var dumpDirectTextures: Bool = true { didSet { commit(_dumpDirectTexturesConfig, dumpDirectTextures) } }
    let _dumpPaletteTexturesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpPaletteTextures", default: true,
        suppressible: false,
        codec: .bool)
    var dumpPaletteTextures: Bool = true { didSet { commit(_dumpPaletteTexturesConfig, dumpPaletteTextures) } }

    // ── GS Hardware Fixes (EmuCore/GS) ──
    // Compatibility-oriented hardware-renderer fixes. AAT (HWAccurateAlphaTest) and
    // Texture Inside RT close the GameDB advisory gap added in 2.3.2. Applied live by
    // the GS thread (no VM restart); some may require GameDB Graphics Fixes off.
    let _hwAccurateAlphaTestConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "HWAccurateAlphaTest", default: false,
        suppressible: false,
        codec: .bool)
    var hwAccurateAlphaTest: Bool = false { didSet { commit(_hwAccurateAlphaTestConfig, hwAccurateAlphaTest) } }
    let _textureInsideRtConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_TextureInsideRt", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    var textureInsideRt: Int = 0 { didSet { commit(_textureInsideRtConfig, textureInsideRt) } }
    let _limit24BitDepthConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_Limit24BitDepth", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    var limit24BitDepth: Int = 0 { didSet { commit(_limit24BitDepthConfig, limit24BitDepth) } }
    let _nativeScalingConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_native_scaling", default: 0,
        suppressible: false,
        codec: .int(in: 0...4))
    var nativeScaling: Int = 0 { didSet { commit(_nativeScalingConfig, nativeScaling) } }
    let _cpuClutRenderConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_CPUCLUTRender", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    var cpuClutRender: Int = 0 { didSet { commit(_cpuClutRenderConfig, cpuClutRender) } }
    let _cpuSpriteRenderBwConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_CPUSpriteRenderBW", default: 0,
        suppressible: false,
        codec: .int(in: SettingsStore.cpuSpriteRenderBwRange))
    var cpuSpriteRenderBw: Int = 0 { didSet { commit(_cpuSpriteRenderBwConfig, cpuSpriteRenderBw) } }
    let _cpuSpriteRenderLevelConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_CPUSpriteRenderLevel", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    var cpuSpriteRenderLevel: Int = 0 { didSet { commit(_cpuSpriteRenderLevelConfig, cpuSpriteRenderLevel) } }
    let _gpuTargetClutConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_GPUTargetCLUTMode", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    var gpuTargetClut: Int = 0 { didSet { commit(_gpuTargetClutConfig, gpuTargetClut) } }
    let _bilinearUpscaleHackConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_BilinearHack", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    var bilinearUpscaleHack: Int = 0 { didSet { commit(_bilinearUpscaleHackConfig, bilinearUpscaleHack) } }
    let _maxAnisotropyConfig = Setting<Int>(
        section: "EmuCore/GS", key: "MaxAnisotropy", default: 0,
        suppressible: false,
        codec: .int(in: 0...16))
    var maxAnisotropy: Int = 0 { didSet { commit(_maxAnisotropyConfig, maxAnisotropy) } }
    let _hardwareDownloadModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "HWDownloadMode", default: 0,
        suppressible: false,
        codec: .int(in: 0...4))
    var hardwareDownloadMode: Int = 0 { didSet { commit(_hardwareDownloadModeConfig, hardwareDownloadMode) } }
    let _tvShaderConfig = Setting<Int>(
        section: "EmuCore/GS", key: "TVShader", default: 0,
        suppressible: false,
        codec: .int(in: 0...7))
    var tvShader: Int = 0 { didSet { commit(_tvShaderConfig, tvShader) } }
    // MetalFX Spatial upscaler (0 = Off / bilinear, 1 = MetalFX Spatial).
    // Hidden in the UI when isMetalFXAvailable is false; default is Off.
    let _upscalerConfig = Setting<Int>(
        section: "EmuCore/GS", key: "Upscaler", default: 0,
        suppressible: false,
        codec: .int)
    var upscaler: Int = 0 { didSet { commit(_upscalerConfig, upscaler) } }

    /// Why an upscaling hack isn't doing what the row says. Mirrors the enum in
    /// ARMSX2Bridge.mm; the values cross as ints.
    enum GraphicsHackReason: Int {
        case applied = 0
        case needsManualHacks
        case needsUpscaling
        case fromGameDatabase
        case noGame
        case perGame
    }

    struct GraphicsHackStatus {
        var effective: Int
        var reason: GraphicsHackReason
        var pinned: Bool
    }

    /// What the running game really has, keyed by INI key. The INI is what the player
    /// asked for; between the two sit the two mask passes and the GameDB, and only the
    /// core can see the result. Empty until a game is running.
    var graphicsHackStatus: [String: GraphicsHackStatus] = [:]

    var gsBoolHacks: [String: Bool] = [:]

    // ── Screen / PCRTC (EmuCore/GS) ── display-output options, applied live.
    let _pcrtcOffsetsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "pcrtc_offsets", default: false,
        suppressible: false,
        codec: .bool)
    var pcrtcOffsets: Bool = false { didSet { commit(_pcrtcOffsetsConfig, pcrtcOffsets) } }
    let _pcrtcOverscanConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "pcrtc_overscan", default: false,
        suppressible: false,
        codec: .bool)
    var pcrtcOverscan: Bool = false { didSet { commit(_pcrtcOverscanConfig, pcrtcOverscan) } }
    let _pcrtcAntiBlurConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "pcrtc_antiblur", default: true,
        suppressible: false,
        codec: .bool)
    var pcrtcAntiBlur: Bool = true { didSet { commit(_pcrtcAntiBlurConfig, pcrtcAntiBlur) } }
    let _disableInterlaceOffsetConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "disable_interlace_offset", default: false,
        suppressible: false,
        codec: .bool)
    var disableInterlaceOffset: Bool = false {
        didSet { commit(_disableInterlaceOffsetConfig, disableInterlaceOffset) }
    }
    let _skipDuplicateFramesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "SkipDuplicateFrames", default: true,
        suppressible: false,
        codec: .bool)
    var skipDuplicateFrames: Bool = true { didSet { commit(_skipDuplicateFramesConfig, skipDuplicateFrames) } }
    let _syncToHostRefreshConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "SyncToHostRefreshRate", default: false,
        suppressible: false,
        codec: .bool)
    var syncToHostRefresh: Bool = false { didSet {
        commit(_syncToHostRefreshConfig, syncToHostRefresh)
        if syncToHostRefresh != oldValue { markFramePacingCustom() }
    }}
    let _integerScalingConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "IntegerScaling", default: false,
        suppressible: false,
        codec: .bool)
    var integerScaling: Bool = false { didSet { commit(_integerScalingConfig, integerScaling) } }

    // ── Shade Boost (EmuCore/GS) ── post-process color adjustment, applied live.
    let _shadeBoostConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "ShadeBoost", default: false,
        suppressible: false,
        codec: .bool)
    var shadeBoost: Bool = false { didSet { commit(_shadeBoostConfig, shadeBoost) } }
    let _shadeBoostBrightnessConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Brightness", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    var shadeBoostBrightness: Int = 50 { didSet { commit(_shadeBoostBrightnessConfig, shadeBoostBrightness) } }
    let _shadeBoostContrastConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Contrast", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    var shadeBoostContrast: Int = 50 { didSet { commit(_shadeBoostContrastConfig, shadeBoostContrast) } }
    let _shadeBoostSaturationConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Saturation", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    var shadeBoostSaturation: Int = 50 { didSet { commit(_shadeBoostSaturationConfig, shadeBoostSaturation) } }
    let _shadeBoostGammaConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Gamma", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    var shadeBoostGamma: Int = 50 { didSet { commit(_shadeBoostGammaConfig, shadeBoostGamma) } }

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
    // Adaptive Resolution — opt-in frame-time-driven dynamic internal
    // resolution, off by default. The didSet writes the INI key and starts or
    // stops the controller so its lifecycle tracks the user's toggle.
    let _adaptiveResolutionEnabledConfig = Setting<Bool>(
        section: "ARMSX2iOS/FramePacing", key: "DynamicResolution", default: false,
        suppressible: false,
        codec: .bool)
    var adaptiveResolutionEnabled: Bool = false { didSet {
        commit(_adaptiveResolutionEnabledConfig, adaptiveResolutionEnabled)
        // Not while the INI is loading: setEnabled reads SettingsStore.shared, and we are inside
        // that very initializer. init starts the controller itself once it has finished.
        guard !suppressINIWrites else { return }
        FrameTimeDynamicResolutionController.shared.setEnabled(adaptiveResolutionEnabled)
    }}
    let _lastActiveOsdPresetConfig = Setting<OsdPreset>(
        section: "ARMSX2iOS/UI", key: "LastActiveOsdPreset", default: .simple,
        suppressible: false,
        codec: .rawInt)
    var lastActiveOsdPreset: OsdPreset = .simple { didSet { commit(_lastActiveOsdPresetConfig, lastActiveOsdPreset) } }
    let _osdPerformancePositionConfig = Setting<Int>(
        section: "EmuCore/GS", key: "OsdPerformancePos",
        default: SettingsStore.defaultOsdPerformancePosition,
        suppressible: false,
        codec: .int)
    var osdPerformancePosition = SettingsStore.defaultOsdPerformancePosition {
        didSet { commit(_osdPerformancePositionConfig, osdPerformancePosition) }
    }
    /// Suppresses transient on-screen messages (shader compilation, save state,
    /// settings-applied). Critical SwiftUI alerts are unaffected. Backed by the
    /// core's OsdMessagesPos (1 = TopLeft default, 0 = None).
    let _osdShowMessagesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdMessagesPos", default: true,
        suppressible: false,
        codec: .boolAsInt)
    var osdShowMessages: Bool = true { didSet { commit(_osdShowMessagesConfig, osdShowMessages) } }
    let _osdShowFPSConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowFPS", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowFPS: Bool = false { didSet {
        commit(_osdShowFPSConfig, osdShowFPS)
        markOsdCustom()
    }}
    let _osdShowVPSConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowVPS", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowVPS: Bool = false { didSet {
        commit(_osdShowVPSConfig, osdShowVPS)
        markOsdCustom()
    }}
    let _osdShowSpeedConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowSpeed", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowSpeed: Bool = false { didSet {
        commit(_osdShowSpeedConfig, osdShowSpeed)
        markOsdCustom()
    }}
    let _osdShowCPUConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowCPU", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowCPU: Bool = false { didSet {
        commit(_osdShowCPUConfig, osdShowCPU)
        markOsdCustom()
    }}
    let _osdShowGPUConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowGPU", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowGPU: Bool = false { didSet {
        commit(_osdShowGPUConfig, osdShowGPU)
        markOsdCustom()
    }}
    let _osdShowResolutionConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowResolution", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowResolution: Bool = false { didSet {
        commit(_osdShowResolutionConfig, osdShowResolution)
        markOsdCustom()
    }}
    let _osdShowGSStatsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowGSStats", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowGSStats: Bool = false { didSet {
        commit(_osdShowGSStatsConfig, osdShowGSStats)
        markOsdCustom()
    }}
    let _osdShowIndicatorsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowIndicators", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowIndicators: Bool = false { didSet {
        commit(_osdShowIndicatorsConfig, osdShowIndicators)
        markOsdCustom()
    }}
    let _osdShowSettingsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowSettings", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowSettings: Bool = false { didSet {
        commit(_osdShowSettingsConfig, osdShowSettings)
        markOsdCustom()
    }}
    let _osdShowInputsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowInputs", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowInputs: Bool = false { didSet {
        commit(_osdShowInputsConfig, osdShowInputs)
        markOsdCustom()
    }}
    let _osdShowFrameTimesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowFrameTimes", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowFrameTimes: Bool = false { didSet {
        commit(_osdShowFrameTimesConfig, osdShowFrameTimes)
        markOsdCustom()
    }}
    let _osdShowVersionConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowVersion", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowVersion: Bool = false { didSet {
        commit(_osdShowVersionConfig, osdShowVersion)
        markOsdCustom()
    }}
    let _osdShowHardwareInfoConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowHardwareInfo", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowHardwareInfo: Bool = false { didSet {
        commit(_osdShowHardwareInfoConfig, osdShowHardwareInfo)
        markOsdCustom()
    }}
    let _osdShowTextureReplacementsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "OsdShowTextureReplacements", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowTextureReplacements: Bool = false {
        didSet { commit(_osdShowTextureReplacementsConfig, osdShowTextureReplacements) }
    }
    let _osdShowDeviceStatsConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "OsdShowDeviceStats", default: false,
        suppressible: false,
        codec: .bool)
    var osdShowDeviceStats: Bool = false { didSet {
        commit(_osdShowDeviceStatsConfig, osdShowDeviceStats)
        markOsdCustom()
    }}

    // ── Gamepad / UI ──
    let _padOpacityConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "PadOpacity", default: 0.6,
        suppressible: false,
        codec: .float)
    var padOpacity: Float = 0.6 { didSet { commit(_padOpacityConfig, padOpacity) } }
    let _phoneRumbleStrengthConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "PhoneRumbleStrength", default: 0.25,
        suppressible: false,
        codec: .float)
    var phoneRumbleStrength: Float = 0.25 { didSet { commit(_phoneRumbleStrengthConfig, phoneRumbleStrength) } }
    let _increaseRumbleDurationAndInterpolationConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "IncreaseRumbleDurationAndInterpolation", default: true,
        suppressible: false,
        codec: .bool)
    var increaseRumbleDurationAndInterpolation: Bool = true {
        didSet { commit(_increaseRumbleDurationAndInterpolationConfig, increaseRumbleDurationAndInterpolation) }
    }
    let _hapticFeedbackConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "HapticFeedback", default: true,
        suppressible: false,
        codec: .bool)
    var hapticFeedback: Bool = true { didSet { commit(_hapticFeedbackConfig, hapticFeedback) } }
    let _gameRumbleStrengthConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "GameRumbleStrength", default: 1,
        suppressible: false,
        codec: .float)
    var gameRumbleStrength: Float = 1 {
        didSet {
            let clamped = min(max(gameRumbleStrength, 0), 2)
            guard clamped == gameRumbleStrength else {
                gameRumbleStrength = clamped
                return
            }
            commit(_gameRumbleStrengthConfig, gameRumbleStrength)
        }
    }
    let _uiRumbleStrengthConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "UIRumbleStrength", default: 0.5,
        suppressible: false,
        codec: .float)
    var uiRumbleStrength: Float = 0.5 {
        didSet {
            let clamped = min(max(uiRumbleStrength, 0), 1)
            guard clamped == uiRumbleStrength else {
                uiRumbleStrength = clamped
                return
            }
            commit(_uiRumbleStrengthConfig, uiRumbleStrength)
        }
    }
    let _dpadDiagonalsEnabledConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "DpadDiagonalsEnabled", default: true,
        suppressible: false,
        codec: .bool)
    var dpadDiagonalsEnabled: Bool = true { didSet { commit(_dpadDiagonalsEnabledConfig, dpadDiagonalsEnabled) } }
    let _faceComboZonesEnabledConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "FaceComboZonesEnabled", default: true,
        suppressible: false,
        codec: .bool)
    var faceComboZonesEnabled: Bool = true { didSet { commit(_faceComboZonesEnabledConfig, faceComboZonesEnabled) } }
    let _virtualPadSkinConfig = Setting<VirtualPadSkin>(
        section: "ARMSX2iOS/UI", key: "VirtualPadSkin", default: .armsx2Refresh,
        suppressible: false,
        codec: .rawInt)
    var virtualPadSkin: VirtualPadSkin = .armsx2Refresh { didSet { commit(_virtualPadSkinConfig, virtualPadSkin) } }
    let _automaticDownloadCustomSkinConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "AutomaticDownloadCustomSkin", default: false,
        codec: .bool)
    var automaticDownloadCustomSkin: Bool = false {
        didSet { commit(_automaticDownloadCustomSkinConfig, automaticDownloadCustomSkin) }
    }
    let _autoHideVirtualPadWhenControllerConnectedConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "AutoHideVirtualPadWhenControllerConnected", default: true,
        codec: .bool)
    var autoHideVirtualPadWhenControllerConnected: Bool = true {
        didSet { commit(_autoHideVirtualPadWhenControllerConnectedConfig, autoHideVirtualPadWhenControllerConnected) }
    }
    let _autoFullscreenConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "AutoFullscreen", default: true,
        codec: .bool)
    var autoFullscreen: Bool = true { didSet { commit(_autoFullscreenConfig, autoFullscreen) } }
    let _hideMenuButtonConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "HideMenuButton", default: false,
        codec: .bool)
    var hideMenuButton: Bool = false { didSet { commit(_hideMenuButtonConfig, hideMenuButton) } }
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
    let _gameControllerDeadZoneConfig = Setting<Float>(
        section: "ARMSX2iOS/Gamepad", key: "StickDeadZone", default: 0.15,
        codec: .float)
    var gameControllerDeadZone: Float = 0.15 {
        didSet {
            let clamped = min(max(gameControllerDeadZone, 0), 0.25)
            guard clamped == gameControllerDeadZone else {
                gameControllerDeadZone = clamped
                return
            }
            commit(_gameControllerDeadZoneConfig, gameControllerDeadZone)
        }
    }
    let _gameControllerLeftInstantDeadzoneConfig = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "LeftInstantDeadzoneEnabled", default: false,
        codec: .bool)
    var gameControllerLeftInstantDeadzoneEnabled: Bool = false {
        didSet { commit(_gameControllerLeftInstantDeadzoneConfig, gameControllerLeftInstantDeadzoneEnabled) }
    }
    let _gameControllerLeftNegativeDeadzoneConfig = Setting<Float>(
        section: "ARMSX2iOS/Gamepad", key: "LeftNegativeDeadzone", default: -0.08,
        codec: .float)
    var gameControllerLeftNegativeDeadzone: Float = -0.08 {
        didSet {
            let clamped = min(max(gameControllerLeftNegativeDeadzone, -0.25), 0)
            guard clamped == gameControllerLeftNegativeDeadzone else {
                gameControllerLeftNegativeDeadzone = clamped
                return
            }
            commit(_gameControllerLeftNegativeDeadzoneConfig, gameControllerLeftNegativeDeadzone)
        }
    }
    let _gameControllerRightInstantDeadzoneConfig = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "RightInstantDeadzoneEnabled", default: false,
        codec: .bool)
    var gameControllerRightInstantDeadzoneEnabled: Bool = false {
        didSet { commit(_gameControllerRightInstantDeadzoneConfig, gameControllerRightInstantDeadzoneEnabled) }
    }
    let _gameControllerRightNegativeDeadzoneConfig = Setting<Float>(
        section: "ARMSX2iOS/Gamepad", key: "RightNegativeDeadzone", default: -0.08,
        codec: .float)
    var gameControllerRightNegativeDeadzone: Float = -0.08 {
        didSet {
            let clamped = min(max(gameControllerRightNegativeDeadzone, -0.25), 0)
            guard clamped == gameControllerRightNegativeDeadzone else {
                gameControllerRightNegativeDeadzone = clamped
                return
            }
            commit(_gameControllerRightNegativeDeadzoneConfig, gameControllerRightNegativeDeadzone)
        }
    }
    private static let stickInversionSection = "ARMSX2iOS/UI"
    let _invertLeftStickXConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertLeftStickX", default: false,
        codec: .bool)
    var invertLeftStickX: Bool = false { didSet { commit(_invertLeftStickXConfig, invertLeftStickX) } }
    let _invertLeftStickYConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertLeftStickY", default: false,
        codec: .bool)
    var invertLeftStickY: Bool = false { didSet { commit(_invertLeftStickYConfig, invertLeftStickY) } }
    let _invertRightStickXConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertRightStickX", default: false,
        codec: .bool)
    var invertRightStickX: Bool = false { didSet { commit(_invertRightStickXConfig, invertRightStickX) } }
    let _invertRightStickYConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertRightStickY", default: false,
        codec: .bool)
    var invertRightStickY: Bool = false { didSet { commit(_invertRightStickYConfig, invertRightStickY) } }

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
    let _appLanguageConfig = Setting<AppLanguage>(
        section: "ARMSX2iOS/UI", key: "AppLanguage", default: .system,
        suppressible: false,
        codec: .rawString)
    var appLanguage: AppLanguage = .system { didSet { commit(_appLanguageConfig, appLanguage) } }
    let _controllerMultitapModeConfig = Setting<Int>(
        section: "ARMSX2iOS/Gamepad", key: "MultitapMode", default: 0,
        suppressible: false,
        codec: .int)
    var controllerMultitapMode: Int = 0 { didSet { commit(_controllerMultitapModeConfig, controllerMultitapMode) } }
    let _controllerMacroQuickMenuConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroQuickMenu",
        default: ControllerMacroAction.quickMenu.defaultBinding,
        suppressible: false, codec: .rawString)
    let _controllerMacroQuickMenuSelectPauseMigration = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "MacroQuickMenuSelectPauseMigrated",
        default: false, suppressible: false, codec: .bool)
    let _controllerMacroStartDefaultsMigration = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "MacroStartDefaultsMigrated",
        default: false, suppressible: false, codec: .bool)
    var controllerMacroQuickMenu = ControllerMacroAction.quickMenu.defaultBinding {
        didSet { commit(_controllerMacroQuickMenuConfig, controllerMacroQuickMenu) }
    }
    let _controllerMacroSaveGameStateConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroSaveGameState",
        default: ControllerMacroAction.saveGameState.defaultBinding,
        suppressible: false, codec: .rawString)
    var controllerMacroSaveGameState = ControllerMacroAction.saveGameState.defaultBinding {
        didSet { commit(_controllerMacroSaveGameStateConfig, controllerMacroSaveGameState) }
    }
    let _controllerMacroLoadGameStateConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroLoadGameState",
        default: ControllerMacroAction.loadGameState.defaultBinding,
        suppressible: false, codec: .rawString)
    var controllerMacroLoadGameState = ControllerMacroAction.loadGameState.defaultBinding {
        didSet { commit(_controllerMacroLoadGameStateConfig, controllerMacroLoadGameState) }
    }
    let _controllerMacroIncreaseSpeedConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroIncreaseSpeed",
        default: ControllerMacroAction.increaseSpeed.defaultBinding,
        suppressible: false, codec: .rawString)
    var controllerMacroIncreaseSpeed = ControllerMacroAction.increaseSpeed.defaultBinding {
        didSet { commit(_controllerMacroIncreaseSpeedConfig, controllerMacroIncreaseSpeed) }
    }
    let _controllerMacroDecreaseSpeedConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroDecreaseSpeed",
        default: ControllerMacroAction.decreaseSpeed.defaultBinding,
        suppressible: false, codec: .rawString)
    var controllerMacroDecreaseSpeed = ControllerMacroAction.decreaseSpeed.defaultBinding {
        didSet { commit(_controllerMacroDecreaseSpeedConfig, controllerMacroDecreaseSpeed) }
    }
    let _controllerMacroEnableFastForwardConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroEnableFastForward",
        default: ControllerMacroAction.enableFastForward.defaultBinding,
        suppressible: false, codec: .rawString)
    var controllerMacroEnableFastForward = ControllerMacroAction.enableFastForward.defaultBinding {
        didSet { commit(_controllerMacroEnableFastForwardConfig, controllerMacroEnableFastForward) }
    }
    let _controllerMacroDisableFastForwardConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroDisableFastForward",
        default: ControllerMacroAction.disableFastForward.defaultBinding,
        suppressible: false, codec: .rawString)
    var controllerMacroDisableFastForward = ControllerMacroAction.disableFastForward.defaultBinding {
        didSet { commit(_controllerMacroDisableFastForwardConfig, controllerMacroDisableFastForward) }
    }

    let _autoOpenStikDebugConfig = Setting<Bool>(
        section: "ARMSX2iOS/JIT", key: "AutoOpenStikDebug", default: false,
        codec: .bool)
    var autoOpenStikDebug: Bool = false { didSet { commit(_autoOpenStikDebugConfig, autoOpenStikDebug) } }
    let _jitScriptProtocolConfig = Setting<JITScriptProtocol>(
        section: "ARMSX2iOS/JIT", key: "ScriptProtocol",
        // Which one is right depends on the iOS version, so ask rather than assume.
        default: JITScriptProtocol.defaultValue,
        codec: .rawString)
    var jitScriptProtocol = JITScriptProtocol.defaultValue {
        didSet { commit(_jitScriptProtocolConfig, jitScriptProtocol) }
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
    let _dev9HddFileConfig = Setting<String>(
        section: "DEV9/Hdd", key: "HddFile", default: "DEV9hdd.raw",
        codec: .string)
    var dev9HddFile: String = "DEV9hdd.raw" { didSet { commit(_dev9HddFileConfig, dev9HddFile) } }
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
    let _dev9InterceptDHCPConfig = Setting<Bool>(
        section: "DEV9/Eth", key: "InterceptDHCP", default: false,
        codec: .bool)
    var dev9InterceptDHCP: Bool = false { didSet { commit(_dev9InterceptDHCPConfig, dev9InterceptDHCP) } }
    let _dev9EthLogDHCPConfig = Setting<Bool>(
        section: "DEV9/Eth", key: "EthLogDHCP", default: false,
        codec: .bool)
    var dev9EthLogDHCP: Bool = false { didSet { commit(_dev9EthLogDHCPConfig, dev9EthLogDHCP) } }
    let _dev9EthLogDNSConfig = Setting<Bool>(
        section: "DEV9/Eth", key: "EthLogDNS", default: false,
        codec: .bool)
    var dev9EthLogDNS: Bool = false { didSet { commit(_dev9EthLogDNSConfig, dev9EthLogDNS) } }
    let _dev9DNS1ModeConfig = Setting<String>(
        section: "DEV9/Eth", key: "ModeDNS1", default: "Auto",
        codec: .string)
    var dev9DNS1Mode: String = "Auto" { didSet { commit(_dev9DNS1ModeConfig, dev9DNS1Mode) } }
    let _dev9DNS1Config = Setting<String>(
        section: "DEV9/Eth", key: "DNS1", default: "0.0.0.0",
        codec: .string)
    var dev9DNS1: String = "0.0.0.0" { didSet { commit(_dev9DNS1Config, dev9DNS1) } }
    let _dev9DNS2ModeConfig = Setting<String>(
        section: "DEV9/Eth", key: "ModeDNS2", default: "Auto",
        codec: .string)
    var dev9DNS2Mode: String = "Auto" { didSet { commit(_dev9DNS2ModeConfig, dev9DNS2Mode) } }
    let _dev9DNS2Config = Setting<String>(
        section: "DEV9/Eth", key: "DNS2", default: "0.0.0.0",
        codec: .string)
    var dev9DNS2: String = "0.0.0.0" { didSet { commit(_dev9DNS2Config, dev9DNS2) } }

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
    var hideMenuStatusBar: Bool = true {
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

    var hasCustomBackground: Bool {
        dynamicBackgroundsEnabled
            || backgroundPrimaryAsset != nil
            || backgroundLandscapeAsset != nil
    }

    // aspectRatioName / aspectRatioValue — see SettingsStore+Graphics.swift.
    // loadedFastBoot / loadedJITScriptProtocol — see SettingsStore+Speedhacks.swift.

    // ── Init from INI ──
    private init() {
        Self.initializeAutomaticJITAppearanceState()
        // Assignments in init skip didSet, so loading the INI writes nothing back. Moved
        // into a helper they would fire, and every non-suppressible setting would rewrite
        // itself each launch.
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
        iopRecompiler = _iopRecompilerConfig.load()
        vu0Recompiler = _vu0RecompilerConfig.load()
        vu1Recompiler = _vu1RecompilerConfig.load()
        fastBoot = Self.loadedFastBoot()
        automaticLoadLastSaveState = _automaticLoadLastSaveStateConfig.load()
        automaticLoadLastGame = _automaticLoadLastGameConfig.load()
        fastmem = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: true)
        emulationOnlyModeEnabled = _emulationOnlyModeConfig.load()
        emulationOnlyDisablePatches = _emulationOnlyDisablePatchesConfig.load()
        emulationOnlyDisablePINE = _emulationOnlyDisablePINEConfig.load()
        emulationOnlyDisableRetroAchievements = _emulationOnlyDisableRetroAchievementsConfig.load()
        emulationOnlyDisableInputRecording = _emulationOnlyDisableInputRecordingConfig.load()
        emulationOnlyDisableOSD = _emulationOnlyDisableOSDConfig.load()
        emulationOnlyDisableFramePacing = _emulationOnlyDisableFramePacingConfig.load()
        emulationOnlyDisableVirtualControls = _emulationOnlyDisableVirtualControlsConfig.load()
        emulationOnlyDisableQuickMenu = _emulationOnlyDisableQuickMenuConfig.load()
        emulationOnlyClearNetworkCache = _emulationOnlyClearNetworkCacheConfig.load()
        emulationOnlyModeDelaySeconds = _emulationOnlyModeDelayConfig.load()
        // CPU rounding & clamping
        eeFpuRoundMode = _eeFpuRoundModeConfig.load()
        vu0RoundMode = _vu0RoundModeConfig.load()
        vu1RoundMode = _vu1RoundModeConfig.load()
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
        palFramerate = _palFramerateConfig.load()
        let nominalScalar = ARMSX2Bridge.getINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0)
        frameLimiterEnabled = Self.frameLimiterEnabled(fromNominalScalar: nominalScalar)
        targetFPS = Self.loadedTargetFPS(fromNominalScalar: nominalScalar, baseFramerate: loadedNTSCFramerate)
        Self.sanitizeNominalScalarIfNeeded(nominalScalar)
        fastForwardScalar = Self.clampedSpeedScalar(ARMSX2Bridge.getINIFloat("Framerate", key: "TurboScalar", defaultValue: Self.defaultFastForwardScalar))
        emulatorVolumePercent = Self.clampedEmulatorVolumePercent(Int(ARMSX2Bridge.emulatorVolumePercent()))
        audioTimeStretch = _audioTimeStretchConfig.load()
        audioBufferMs = _audioBufferMsConfig.load()
        audioOutputLatencyMs = _audioOutputLatencyMsConfig.load()
        audioFastForwardVolume = _audioFastForwardVolumeConfig.load()
        audioSwapChannels = _audioSwapChannelsConfig.load()
        // Boot
        fastCDVD = _fastCDVDConfig.load()
        // Advanced Speedhacks
        eeCycleRate = _eeCycleRateConfig.load()
        vu1Instant = _vu1InstantConfig.load()
        mtvu = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vuThread", defaultValue: true)
        waitLoop = _waitLoopConfig.load()
        intcStat = _intcStatConfig.load()
        eeCycleSkip = _eeCycleSkipConfig.load()
        vuFlagHack = _vuFlagHackConfig.load()
        enableCheats = _enableCheatsConfig.load()
        enablePatches = _enablePatchesConfig.load()
        enableGameFixes = _enableGameFixesConfig.load()
        enableGameDBHardwareFixes = _enableGameDBHardwareFixesConfig.load()
        enableWidescreenPatches = _enableWidescreenPatchesConfig.load()
        enableNoInterlacingPatches = _enableNoInterlacingPatchesConfig.load()
        hostFilesystem = _hostFilesystemConfig.load()
        gameFixes = Self.loadGameFixes()
        // Graphics
        // Not load(): the INI can name a desktop renderer, so we correct it on disk too.
#if targetEnvironment(macCatalyst)
        renderer = 17
        _rendererConfig.write(17)
#else
        let initialRenderer = Self.supportedIOSRenderer(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17)))
        renderer = initialRenderer
        _rendererConfig.write(initialRenderer)
#endif
        upscaleMultiplier = _upscaleMultiplierConfig.load()
        vsyncQueueSize = _vsyncQueueSizeConfig.load()
        textureFiltering = _textureFilteringConfig.load()
        backThreadMode = _backThreadModeConfig.load()
        hardwareMipmapping = _hardwareMipmappingConfig.load()
        fxaa = _fxaaConfig.load()
        casMode = _casModeConfig.load()
        casSharpness = _casSharpnessConfig.load()
        shaderChainEnabled = _shaderChainEnabledConfig.load()
        shaderChainPresetRef = _shaderChainPresetRefConfig.load()
        interlaceMode = _interlaceModeConfig.load()
        aspectRatio = _aspectRatioConfig.load()
        blendingAccuracy = _blendingAccuracyConfig.load()
        dithering = _ditheringConfig.load()
        trilinearFiltering = _trilinearFilteringConfig.load()
        halfPixelOffset = _halfPixelOffsetConfig.load()
        roundSprite = _roundSpriteConfig.load()
        alignSprite = _alignSpriteConfig.load()
        mergeSprite = _mergeSpriteConfig.load()
        wildArmsOffset = _wildArmsOffsetConfig.load()
        textureOffsetX = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetX", defaultValue: 0)))
        textureOffsetY = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetY", defaultValue: 0)))
        let loadedSkipDrawStart = Self.clampedSkipDraw(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_Start", defaultValue: 0)))
        skipDrawStart = loadedSkipDrawStart
        skipDrawEnd = Self.normalizedSkipDrawEnd(
            start: loadedSkipDrawStart,
            end: Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_End", defaultValue: 0))
        )
        loadTextureReplacements = _loadTextureReplacementsConfig.load()
        loadTextureReplacementsAsync = _loadTextureReplacementsAsyncConfig.load()
        precacheTextureReplacements = _precacheTextureReplacementsConfig.load()
        texturePreloading = _texturePreloadingConfig.load()
        dumpReplaceableTextures = _dumpReplaceableTexturesConfig.load()
        dumpReplaceableMipmaps = _dumpReplaceableMipmapsConfig.load()
        dumpTexturesWithFMVActive = _dumpTexturesWithFMVActiveConfig.load()
        dumpDirectTextures = _dumpDirectTexturesConfig.load()
        dumpPaletteTextures = _dumpPaletteTexturesConfig.load()
        // GS Hardware Fixes
        hwAccurateAlphaTest = _hwAccurateAlphaTestConfig.load()
        textureInsideRt = _textureInsideRtConfig.load()
        limit24BitDepth = _limit24BitDepthConfig.load()
        nativeScaling = _nativeScalingConfig.load()
        cpuClutRender = _cpuClutRenderConfig.load()
        cpuSpriteRenderBw = _cpuSpriteRenderBwConfig.load()
        cpuSpriteRenderLevel = _cpuSpriteRenderLevelConfig.load()
        gpuTargetClut = _gpuTargetClutConfig.load()
        bilinearUpscaleHack = _bilinearUpscaleHackConfig.load()
        maxAnisotropy = _maxAnisotropyConfig.load()
        hardwareDownloadMode = _hardwareDownloadModeConfig.load()
        tvShader = _tvShaderConfig.load()
        upscaler = _upscalerConfig.load()
        gsBoolHacks = Self.loadGSBoolHacks()
        // Screen / PCRTC
        pcrtcOffsets = _pcrtcOffsetsConfig.load()
        pcrtcOverscan = _pcrtcOverscanConfig.load()
        pcrtcAntiBlur = _pcrtcAntiBlurConfig.load()
        disableInterlaceOffset = _disableInterlaceOffsetConfig.load()
        skipDuplicateFrames = _skipDuplicateFramesConfig.load()
        syncToHostRefresh = _syncToHostRefreshConfig.load()
        integerScaling = _integerScalingConfig.load()
        // Shade Boost
        shadeBoost = _shadeBoostConfig.load()
        shadeBoostBrightness = _shadeBoostBrightnessConfig.load()
        shadeBoostContrast = _shadeBoostContrastConfig.load()
        shadeBoostSaturation = _shadeBoostSaturationConfig.load()
        shadeBoostGamma = _shadeBoostGammaConfig.load()
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
        osdShowMessages = _osdShowMessagesConfig.load()
        osdShowFPS = _osdShowFPSConfig.load()
        osdShowVPS = _osdShowVPSConfig.load()
        osdShowSpeed = _osdShowSpeedConfig.load()
        osdShowCPU = _osdShowCPUConfig.load()
        osdShowGPU = _osdShowGPUConfig.load()
        osdShowResolution = _osdShowResolutionConfig.load()
        osdShowGSStats = _osdShowGSStatsConfig.load()
        osdShowIndicators = _osdShowIndicatorsConfig.load()
        osdShowSettings = _osdShowSettingsConfig.load()
        osdShowInputs = _osdShowInputsConfig.load()
        osdShowFrameTimes = _osdShowFrameTimesConfig.load()
        osdShowVersion = _osdShowVersionConfig.load()
        osdShowHardwareInfo = _osdShowHardwareInfoConfig.load()
        osdShowTextureReplacements = _osdShowTextureReplacementsConfig.load()
        // Fresh installs follow the OSD preset rather than the descriptor default.
        osdShowDeviceStats = _osdShowDeviceStatsConfig.load(default: loadedOsdPreset != .off)
        // UI
        padOpacity = _padOpacityConfig.load()
        hapticFeedback = _hapticFeedbackConfig.load()
        gameRumbleStrength = _gameRumbleStrengthConfig.load()
        uiRumbleStrength = _uiRumbleStrengthConfig.load()
        phoneRumbleStrength = _phoneRumbleStrengthConfig.load()
        increaseRumbleDurationAndInterpolation = _increaseRumbleDurationAndInterpolationConfig.load()
        dpadDiagonalsEnabled = _dpadDiagonalsEnabledConfig.load()
        faceComboZonesEnabled = _faceComboZonesEnabledConfig.load()
        virtualPadSkin = _virtualPadSkinConfig.load()
        automaticDownloadCustomSkin = _automaticDownloadCustomSkinConfig.load()
        autoHideVirtualPadWhenControllerConnected = _autoHideVirtualPadWhenControllerConnectedConfig.load()
        autoFullscreen = _autoFullscreenConfig.load()
        hideMenuButton = _hideMenuButtonConfig.load()
        analogStickScale = Self.clampedAnalogStickScale(ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", defaultValue: 1.0))
        gameControllerDeadZone = _gameControllerDeadZoneConfig.load()
        gameControllerLeftInstantDeadzoneEnabled = _gameControllerLeftInstantDeadzoneConfig.load()
        gameControllerLeftNegativeDeadzone = _gameControllerLeftNegativeDeadzoneConfig.load()
        gameControllerRightInstantDeadzoneEnabled = _gameControllerRightInstantDeadzoneConfig.load()
        gameControllerRightNegativeDeadzone = _gameControllerRightNegativeDeadzoneConfig.load()
        invertLeftStickX = _invertLeftStickXConfig.load()
        invertLeftStickY = _invertLeftStickYConfig.load()
        invertRightStickX = _invertRightStickXConfig.load()
        invertRightStickY = _invertRightStickYConfig.load()
        appLanguage = _appLanguageConfig.load()
        controllerMultitapMode = _controllerMultitapModeConfig.load()
        let loadedQuickMenuMacro = _controllerMacroQuickMenuConfig.load()
        if !_controllerMacroQuickMenuSelectPauseMigration.load() {
            let previousDefault = ControllerMacroBinding(first: .l3, second: .r3)
            let resolvedQuickMenuMacro = loadedQuickMenuMacro == previousDefault
                ? ControllerMacroAction.quickMenu.defaultBinding
                : loadedQuickMenuMacro
            controllerMacroQuickMenu = resolvedQuickMenuMacro
            _controllerMacroQuickMenuConfig.write(resolvedQuickMenuMacro)
            _controllerMacroQuickMenuSelectPauseMigration.write(true)
        } else {
            controllerMacroQuickMenu = loadedQuickMenuMacro
        }
        let loadedSaveStateMacro = _controllerMacroSaveGameStateConfig.load()
        let loadedLoadStateMacro = _controllerMacroLoadGameStateConfig.load()
        let loadedIncreaseSpeedMacro = _controllerMacroIncreaseSpeedConfig.load()
        let loadedDecreaseSpeedMacro = _controllerMacroDecreaseSpeedConfig.load()
        let loadedEnableFastForwardMacro = _controllerMacroEnableFastForwardConfig.load()
        let loadedDisableFastForwardMacro = _controllerMacroDisableFastForwardConfig.load()
        if !_controllerMacroStartDefaultsMigration.load() {
            let previousSaveStateDefault = ControllerMacroBinding(first: .r3, second: .circle)
            let previousLoadStateDefault = ControllerMacroBinding(first: .r3, second: .triangle)
            let previousIncreaseSpeedDefault = ControllerMacroBinding(first: .l3, second: .dpadRight)
            let previousDecreaseSpeedDefault = ControllerMacroBinding(first: .l3, second: .dpadLeft)
            let previousEnableFastForwardDefault = ControllerMacroBinding(first: .l3, second: .dpadUp)
            let previousDisableFastForwardDefault = ControllerMacroBinding(first: .l3, second: .dpadDown)

            let resolvedSaveStateMacro = loadedSaveStateMacro == previousSaveStateDefault
                ? ControllerMacroAction.saveGameState.defaultBinding : loadedSaveStateMacro
            let resolvedLoadStateMacro = loadedLoadStateMacro == previousLoadStateDefault
                ? ControllerMacroAction.loadGameState.defaultBinding : loadedLoadStateMacro
            let resolvedIncreaseSpeedMacro = loadedIncreaseSpeedMacro == previousIncreaseSpeedDefault
                ? ControllerMacroAction.increaseSpeed.defaultBinding : loadedIncreaseSpeedMacro
            let resolvedDecreaseSpeedMacro = loadedDecreaseSpeedMacro == previousDecreaseSpeedDefault
                ? ControllerMacroAction.decreaseSpeed.defaultBinding : loadedDecreaseSpeedMacro
            let resolvedEnableFastForwardMacro = loadedEnableFastForwardMacro == previousEnableFastForwardDefault
                ? ControllerMacroAction.enableFastForward.defaultBinding : loadedEnableFastForwardMacro
            let resolvedDisableFastForwardMacro = loadedDisableFastForwardMacro == previousDisableFastForwardDefault
                ? ControllerMacroAction.disableFastForward.defaultBinding : loadedDisableFastForwardMacro

            controllerMacroSaveGameState = resolvedSaveStateMacro
            controllerMacroLoadGameState = resolvedLoadStateMacro
            controllerMacroIncreaseSpeed = resolvedIncreaseSpeedMacro
            controllerMacroDecreaseSpeed = resolvedDecreaseSpeedMacro
            controllerMacroEnableFastForward = resolvedEnableFastForwardMacro
            controllerMacroDisableFastForward = resolvedDisableFastForwardMacro

            _controllerMacroSaveGameStateConfig.write(resolvedSaveStateMacro)
            _controllerMacroLoadGameStateConfig.write(resolvedLoadStateMacro)
            _controllerMacroIncreaseSpeedConfig.write(resolvedIncreaseSpeedMacro)
            _controllerMacroDecreaseSpeedConfig.write(resolvedDecreaseSpeedMacro)
            _controllerMacroEnableFastForwardConfig.write(resolvedEnableFastForwardMacro)
            _controllerMacroDisableFastForwardConfig.write(resolvedDisableFastForwardMacro)
            _controllerMacroStartDefaultsMigration.write(true)
        } else {
            controllerMacroSaveGameState = loadedSaveStateMacro
            controllerMacroLoadGameState = loadedLoadStateMacro
            controllerMacroIncreaseSpeed = loadedIncreaseSpeedMacro
            controllerMacroDecreaseSpeed = loadedDecreaseSpeedMacro
            controllerMacroEnableFastForward = loadedEnableFastForwardMacro
            controllerMacroDisableFastForward = loadedDisableFastForwardMacro
        }
        autoOpenStikDebug = _autoOpenStikDebugConfig.load()
        // Not load(): older builds wrote names this enum no longer spells that way.
        jitScriptProtocol = Self.loadedJITScriptProtocol()
        // The migration above already wrote the post-migration INI values;
        // these reads pick them up.
        framePacingPreset = FramePacingPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/FramePacing", key: "Preset", defaultValue: Int32(FramePacingPreset.optimal.rawValue)))) ?? .optimal
        adaptiveResolutionEnabled = _adaptiveResolutionEnabledConfig.load()
        dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
        dev9HddFile = _dev9HddFileConfig.load()
        dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
        dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        dev9InterceptDHCP = _dev9InterceptDHCPConfig.load()
        dev9EthLogDHCP = _dev9EthLogDHCPConfig.load()
        dev9EthLogDNS = _dev9EthLogDNSConfig.load()
        dev9DNS1Mode = _dev9DNS1ModeConfig.load()
        dev9DNS1 = _dev9DNS1Config.load()
        dev9DNS2Mode = _dev9DNS2ModeConfig.load()
        dev9DNS2 = _dev9DNS2Config.load()
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
        clearLiquidGlassUISubSettings = UserDefaults.standard.object(
            forKey: "ARMSX2iOSClearLiquidGlassUISubSettings"
        ) as? Bool ?? true
        clearLiquidGlassUI = UserDefaults.standard.object(
            forKey: "ARMSX2iOSClearLiquidGlassUI"
        ) as? Bool ?? true
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
        hideGameplayStatusBar = UserDefaults.standard.object(
            forKey: "ARMSX2iOSHideGameplayStatusBar"
        ) as? Bool ?? true
        hideIntroStatusBar = UserDefaults.standard.object(
            forKey: "ARMSX2iOSHideIntroStatusBar"
        ) as? Bool ?? true
        hideMenuStatusBar = UserDefaults.standard.object(
            forKey: "ARMSX2iOSHideMenuStatusBar"
        ) as? Bool ?? true
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
        backgroundDim = Self.clampedBackgroundDim(UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundDim") as? Double ?? 0.0)
        backgroundEnabledInBIOS = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInBIOS") as? Bool ?? true
        backgroundEnabledInHelp = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInHelp") as? Bool ?? true
        backgroundEnabledInSettings = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInSettings") as? Bool ?? true
        normalizeDEV9Settings()
        VPadSkinLibraryStore.shared.adoptLegacySelection(virtualPadSkin)
        _aspectRatioConfig.write(aspectRatio)
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
