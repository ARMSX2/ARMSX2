// NumberSettings.swift — every numeric setting a screen can show
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// Every numeric setting a screen can show, described once so no call site can spell one
/// differently. Stops sit where the useful values are and every list holds the default, so
/// the reset arrow lands on one; typing reaches anything in between.
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

    /// A stepper: fifteen contiguous values, and people come here wanting an exact one.
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

    /// Short names, because both screens show these four under a Shade Boost header.
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
