// SettingsStoreController.swift — touch pad, controller, rumble and macro settings
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

extension SettingsStore {
    @MainActor
    @Observable
    final class Controller {
        @ObservationIgnored private(set) var suppressINIWrites = false

        // ── Gamepad / UI ──
        var padOpacity: Float = 0.6 { didSet { commit(Self._padOpacityConfig, padOpacity) } }
        var phoneRumbleStrength: Float = 0.25 { didSet { commit(Self._phoneRumbleStrengthConfig, phoneRumbleStrength) } }
        var increaseRumbleDurationAndInterpolation: Bool = true {
            didSet { commit(Self._increaseRumbleDurationAndInterpolationConfig, increaseRumbleDurationAndInterpolation) }
        }
        var hapticFeedback: Bool = true {
            didSet {
                commit(Self._hapticFeedbackConfig, hapticFeedback)
                ARMSX2Bridge.setRuntimeHapticFeedback(hapticFeedback)
            }
        }
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
        var analogStickScale: Float {
            didSet {
                let clamped = SettingsStore.clampedAnalogStickScale(analogStickScale)
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

        init() {
            suppressINIWrites = true
            defer { suppressINIWrites = false }
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
            gameControllerDeadZone = Self._gameControllerDeadZoneConfig.load()
            gameControllerLeftInstantDeadzoneEnabled = Self._gameControllerLeftInstantDeadzoneConfig.load()
            gameControllerLeftNegativeDeadzone = Self._gameControllerLeftNegativeDeadzoneConfig.load()
            gameControllerRightInstantDeadzoneEnabled = Self._gameControllerRightInstantDeadzoneConfig.load()
            gameControllerRightNegativeDeadzone = Self._gameControllerRightNegativeDeadzoneConfig.load()
            invertLeftStickX = Self._invertLeftStickXConfig.load()
            invertLeftStickY = Self._invertLeftStickYConfig.load()
            invertRightStickX = Self._invertRightStickXConfig.load()
            invertRightStickY = Self._invertRightStickYConfig.load()
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
            analogStickScale = SettingsStore.clampedAnalogStickScale(ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", defaultValue: 1.0))
            ARMSX2Bridge.setRuntimeHapticFeedback(hapticFeedback)
        }

        private func commit<T>(_ setting: Setting<T>, _ value: T) {
            guard !(setting.suppressible && suppressINIWrites) else { return }
            setting.codec.write(setting.section, setting.key, value)
        }
    }
}
