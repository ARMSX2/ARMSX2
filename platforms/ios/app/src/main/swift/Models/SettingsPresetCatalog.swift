// SettingsPresetCatalog.swift — Built-in settings and Dynamic Control presets
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum BuiltInSettingsPreset: String, CaseIterable, Identifiable {
    case defaultPreset = "Default"
    case ultraQuality = "Ultra Quality"
    case highQuality = "High Quality"
    case highQuality30FPS = "High Quality 30 FPS"
    case performance = "Performance"
    case ultraPerformance = "Ultra Performance"

    var id: String { rawValue }

    var summary: String {
        switch self {
        case .defaultPreset:
            return "Restores the graphics and emulation options managed by quality presets to the ARMSX2 defaults."
        case .ultraQuality:
            return "Uses 2x internal resolution with FXAA and CAS Sharpening for maximum image quality."
        case .highQuality:
            return "Uses native internal resolution with FXAA and CAS Sharpening."
        case .highQuality30FPS:
            return "Uses the High Quality configuration and enables OPH Flag Hack for demanding 30 FPS games."
        case .performance:
            return "Uses native internal resolution without FXAA or CAS Sharpening to reduce GPU load."
        case .ultraPerformance:
            return "Uses the Performance configuration with Emulation-Only Mode. Intended for low-end devices."
        }
    }

    var detail: String {
        switch self {
        case .defaultPreset:
            return "Default disables Fast Boot, PNACH Cheats, Widescreen Patches, Fast CDVD, FXAA, CAS Sharpening, OPH Flag Hack, Emulation-Only Mode, and the background in Settings; restores Internal Resolution to 1x Native and Queue Size to 8; and selects the White Colored Virtual Pad skin."
        case .ultraQuality:
            return "Ultra Quality enables Fast Boot, PNACH Cheats, Widescreen Patches, Fast CDVD, FXAA, CAS Sharpening, and the background in Settings; sets Internal Resolution to 2x (1024x896) and Queue Size to 2. OPH Flag Hack and Emulation-Only Mode are disabled. The selected Virtual Pad skin is preserved."
        case .highQuality:
            return "High Quality uses the Ultra Quality settings, including the background in Settings, with Internal Resolution set to 1x Native (512x448). The selected Virtual Pad skin is preserved."
        case .highQuality30FPS:
            return "High Quality 30 FPS uses the High Quality graphics and emulation settings, enables OPH Flag Hack, and disables the background in Settings. It does not change the frame-limiter target or the selected Virtual Pad skin."
        case .performance:
            return "Performance uses the High Quality settings with FXAA, CAS Sharpening, and the background in Settings disabled. The selected Virtual Pad skin is preserved."
        case .ultraPerformance:
            return "Ultra Performance uses the Performance settings, disables the background in Settings, and enables Emulation-Only Mode. This preset is intended for low-end devices and requires an external controller when Virtual Control Layout unloading is enabled. The selected Virtual Pad skin is preserved."
        }
    }

    var preservesVirtualPadSkin: Bool {
        self != .defaultPreset
    }

    @MainActor
    func isActive(
        settings: SettingsStore = .shared,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) -> Bool {
        let configuration = self.configuration
        let settingsMatch = settings.fastBoot == configuration.fastBoot &&
            settings.enableCheats == configuration.enableCheats &&
            settings.enableWidescreenPatches == configuration.enableWidescreenPatches &&
            settings.fastCDVD == configuration.fastCDVD &&
            settings.fxaa == configuration.fxaa &&
            (settings.casMode > 0) == configuration.casSharpening &&
            settings.vsyncQueueSize == configuration.vsyncQueueSize &&
            settings.upscaleMultiplier == configuration.upscaleMultiplier &&
            settings.gameFixEnabled("OPHFlagHack") == configuration.ophFlagHack &&
            settings.emulationOnlyModeEnabled == configuration.emulationOnlyMode &&
            settings.backgroundEnabledInSettings == configuration.showBackgroundInSettings
        guard settingsMatch else { return false }
        guard self == .defaultPreset else { return true }
        return skinLibrary.selectedSkinID == VirtualPadSkin.armsx2Refresh.descriptorID &&
            settings.virtualPadSkin == .armsx2Refresh
    }

    @MainActor
    func apply(
        settings: SettingsStore = .shared,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) {
        let configuration = self.configuration
        settings.fastBoot = configuration.fastBoot
        settings.enableCheats = configuration.enableCheats
        settings.enableWidescreenPatches = configuration.enableWidescreenPatches
        settings.fastCDVD = configuration.fastCDVD
        settings.fxaa = configuration.fxaa
        settings.casMode = configuration.casSharpening ? 1 : 0
        settings.vsyncQueueSize = configuration.vsyncQueueSize
        settings.upscaleMultiplier = configuration.upscaleMultiplier
        settings.setGameFix("OPHFlagHack", configuration.ophFlagHack)
        settings.emulationOnlyModeEnabled = configuration.emulationOnlyMode
        settings.backgroundEnabledInSettings = configuration.showBackgroundInSettings
        if self == .defaultPreset {
            skinLibrary.selectSkin(id: VirtualPadSkin.armsx2Refresh.descriptorID)
            settings.virtualPadSkin = .armsx2Refresh
        }
    }

    private var configuration: Configuration {
        switch self {
        case .defaultPreset:
            return Configuration(
                fastBoot: false,
                enableCheats: false,
                enableWidescreenPatches: false,
                fastCDVD: false,
                fxaa: false,
                casSharpening: false,
                vsyncQueueSize: 8,
                upscaleMultiplier: 1,
                ophFlagHack: false,
                emulationOnlyMode: false
            )
        case .ultraQuality:
            var configuration = qualityConfiguration(upscaleMultiplier: 2)
            configuration.showBackgroundInSettings = true
            return configuration
        case .highQuality:
            var configuration = qualityConfiguration(upscaleMultiplier: 1)
            configuration.showBackgroundInSettings = true
            return configuration
        case .highQuality30FPS:
            var configuration = qualityConfiguration(upscaleMultiplier: 1)
            configuration.ophFlagHack = true
            return configuration
        case .performance:
            var configuration = qualityConfiguration(upscaleMultiplier: 1)
            configuration.fxaa = false
            configuration.casSharpening = false
            return configuration
        case .ultraPerformance:
            var configuration = configurationForPerformance
            configuration.emulationOnlyMode = true
            return configuration
        }
    }

    private var configurationForPerformance: Configuration {
        var configuration = qualityConfiguration(upscaleMultiplier: 1)
        configuration.fxaa = false
        configuration.casSharpening = false
        return configuration
    }

    private func qualityConfiguration(upscaleMultiplier: Float) -> Configuration {
        Configuration(
            fastBoot: true,
            enableCheats: true,
            enableWidescreenPatches: true,
            fastCDVD: true,
            fxaa: true,
            casSharpening: true,
            vsyncQueueSize: 2,
            upscaleMultiplier: upscaleMultiplier,
            ophFlagHack: false,
            emulationOnlyMode: false
        )
    }

    private struct Configuration {
        var fastBoot: Bool
        var enableCheats: Bool
        var enableWidescreenPatches: Bool
        var fastCDVD: Bool
        var fxaa: Bool
        var casSharpening: Bool
        var vsyncQueueSize: Int
        var upscaleMultiplier: Float
        var ophFlagHack: Bool
        var emulationOnlyMode: Bool
        var showBackgroundInSettings = false
    }
}

enum BuiltInDynamicControlPreset: String, CaseIterable, Identifiable {
    case defaultPreset = "Default"
    case mgs3 = "MGS 3"
    case classicFirstPersonShooter = "Classic First Person Shooter"

    var id: String { rawValue }

    var summary: String {
        switch self {
        case .defaultPreset:
            return "Restores the Dynamic Control switches managed by presets while preserving sensitivities and button assignments."
        case .mgs3:
            return "Enables Dynamic Thumbsticks, Swipe Camera, right-thumbstick actions, the aiming crosshair, and Double Tap to Hold Aim."
        case .classicFirstPersonShooter:
            return "Configures responsive swipe aiming, adaptive swipe-to-stick conversion, right-thumbstick actions, automatic fire, and the reactive aiming crosshair for classic first-person shooters."
        }
    }

    @MainActor
    func isActive(settings: DynamicThumbstickSettings = .shared) -> Bool {
        func matches(_ lhs: Double, _ rhs: Double) -> Bool {
            abs(lhs - rhs) < 0.0001
        }

        switch self {
        case .defaultPreset:
            return settings.legacyThumbsticks &&
                !settings.dynamicThumbsticks &&
                !settings.swipeCamera &&
                !settings.rightThumbstickActionsEnabled &&
                !settings.dynamicCrosshairEnabled &&
                !settings.doubleTapToHoldAim
        case .mgs3:
            return settings.dynamicThumbsticks &&
                settings.swipeCamera &&
                settings.rightThumbstickActionsEnabled &&
                settings.dynamicCrosshairEnabled &&
                settings.doubleTapToHoldAim
        case .classicFirstPersonShooter:
            return !settings.legacyThumbsticks &&
                settings.dynamicThumbsticks &&
                settings.swipeCamera &&
                !settings.gyroscopeCamera &&
                settings.leftInstantDeadzoneEnabled &&
                matches(settings.leftNegativeDeadzone, -0.08) &&
                settings.rightInstantDeadzoneEnabled &&
                matches(settings.rightNegativeDeadzone, -0.14) &&
                matches(settings.leftThumbstickAreaScale, 1) &&
                matches(settings.rightThumbstickAreaScale, 1.75) &&
                settings.convertSwipeToDynamicJoystick &&
                matches(settings.convertIntoDynamicThumbstickThreshold, 1) &&
                matches(settings.pullingBackDistance, 0.05) &&
                matches(settings.movementSensitivity, 1) &&
                matches(settings.lookSensitivity, 1) &&
                matches(settings.swipeSensitivity, 0.28) &&
                matches(settings.swipeHorizontalSensitivity, 1) &&
                matches(settings.swipeVerticalSensitivity, 1) &&
                !settings.swipeSensitivityWhileAimingEnabled &&
                matches(settings.swipeSensitivityWhileAiming, 0.28) &&
                matches(settings.swipeHorizontalSensitivityWhileAiming, 1) &&
                matches(settings.swipeVerticalSensitivityWhileAiming, 1) &&
                settings.swipeSensitivityWhileNotAimingEnabled &&
                matches(settings.swipeSensitivityWhileNotAiming, 0.75) &&
                matches(settings.swipeHorizontalSensitivityWhileNotAiming, 1) &&
                matches(settings.swipeVerticalSensitivityWhileNotAiming, 1) &&
                !settings.leftThumbstickActionsEnabled &&
                settings.rightThumbstickActionsEnabled &&
                settings.rightAimButton == .leftShoulder &&
                settings.rightFireButton == .rightShoulder &&
                settings.rightHoldFireButton == .rightShoulder &&
                settings.dynamicCrosshairEnabled &&
                settings.showCrosshairWhileHoldingSwipe &&
                matches(settings.swipeCrosshairHideDelay, 0.5) &&
                settings.triggerButtonWhenUnholdingSwipe == -1 &&
                matches(settings.dynamicCrosshairSize, 32) &&
                matches(settings.dynamicCrosshairOpacity, 0.70) &&
                settings.dynamicCrosshairType == .fourBoxes &&
                settings.dynamicCrosshairAnimation == .reactive &&
                matches(settings.thumbstickRadius, 52) &&
                matches(settings.deadZone, 0.08) &&
                matches(settings.thumbstickOpacity, 0.72) &&
                matches(settings.baseOpacity, 0.20) &&
                matches(settings.trailOpacity, 0.10) &&
                settings.activationHaptics &&
                !settings.holdAimWhileSwipe &&
                !settings.doubleTapToHoldAim &&
                settings.singleTapActionOnNonAimMode &&
                settings.actionsOnNonAimMode &&
                matches(settings.aimReleaseDelay, 1.25) &&
                matches(settings.doubleTapWindow, 0.28) &&
                settings.tapToFire &&
                matches(settings.tapMaximumDuration, 0.18) &&
                matches(settings.tapTravelTolerance, 12) &&
                settings.rapidTapFireEnabled &&
                matches(settings.rapidTapWindow, 0.28) &&
                settings.rapidTapActivationCount == 2 &&
                matches(settings.automaticFireInterval, 0.12) &&
                settings.extendFireWhileDragging &&
                settings.releaseFireWhenTouchEnds &&
                matches(settings.fireReleaseDelay, 0)
        }
    }

    @MainActor
    func apply(settings: DynamicThumbstickSettings = .shared) {
        switch self {
        case .defaultPreset:
            settings.setLegacyThumbsticks(true)
            settings.swipeCamera = false
            settings.rightThumbstickActionsEnabled = false
            settings.dynamicCrosshairEnabled = false
            settings.setDoubleTapToHoldAim(false)
        case .mgs3:
            settings.setDynamicThumbsticks(true)
            settings.swipeCamera = true
            settings.rightThumbstickActionsEnabled = true
            settings.dynamicCrosshairEnabled = true
            settings.setDoubleTapToHoldAim(true)
        case .classicFirstPersonShooter:
            settings.setDynamicThumbsticks(true)
            settings.swipeCamera = true
            settings.gyroscopeCamera = false

            settings.leftInstantDeadzoneEnabled = true
            settings.leftNegativeDeadzone = -0.08
            settings.rightInstantDeadzoneEnabled = true
            settings.rightNegativeDeadzone = -0.14
            settings.leftThumbstickAreaScale = 1
            settings.rightThumbstickAreaScale = 1.75
            settings.convertSwipeToDynamicJoystick = true
            settings.setConvertIntoDynamicThumbstickThreshold(1)
            settings.setPullingBackDistance(0.05)

            settings.movementSensitivity = 1
            settings.lookSensitivity = 1
            settings.swipeSensitivity = 0.28
            settings.swipeHorizontalSensitivity = 1
            settings.swipeVerticalSensitivity = 1
            settings.swipeSensitivityWhileAimingEnabled = false
            settings.swipeSensitivityWhileAiming = 0.28
            settings.swipeHorizontalSensitivityWhileAiming = 1
            settings.swipeVerticalSensitivityWhileAiming = 1
            settings.swipeSensitivityWhileNotAimingEnabled = true
            settings.swipeSensitivityWhileNotAiming = 0.75
            settings.swipeHorizontalSensitivityWhileNotAiming = 1
            settings.swipeVerticalSensitivityWhileNotAiming = 1

            settings.leftThumbstickActionsEnabled = false
            settings.rightThumbstickActionsEnabled = true
            settings.rightAimButton = .leftShoulder
            settings.rightFireButton = .rightShoulder
            settings.rightHoldFireButton = .rightShoulder

            settings.dynamicCrosshairEnabled = true
            settings.showCrosshairWhileHoldingSwipe = true
            settings.swipeCrosshairHideDelay = 0.5
            settings.triggerButtonWhenUnholdingSwipe = -1
            settings.dynamicCrosshairSize = 32
            settings.dynamicCrosshairOpacity = 0.70
            settings.dynamicCrosshairType = .fourBoxes
            settings.dynamicCrosshairAnimation = .reactive

            settings.thumbstickRadius = 52
            settings.deadZone = 0.08
            settings.thumbstickOpacity = 0.72
            settings.baseOpacity = 0.20
            settings.trailOpacity = 0.10
            settings.activationHaptics = true

            settings.setHoldAimWhileSwipe(false)
            settings.setDoubleTapToHoldAim(false)
            settings.singleTapActionOnNonAimMode = true
            settings.setActionsOnNonAimMode(true)
            settings.aimReleaseDelay = 1.25
            settings.doubleTapWindow = 0.28
            settings.tapToFire = true
            settings.tapMaximumDuration = 0.18
            settings.tapTravelTolerance = 12
            settings.rapidTapFireEnabled = true
            settings.rapidTapWindow = 0.28
            settings.rapidTapActivationCount = 2
            settings.automaticFireInterval = 0.12
            settings.extendFireWhileDragging = true
            settings.releaseFireWhenTouchEnds = true
            settings.fireReleaseDelay = 0
        }
    }
}
