// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Controller macro behavior is separate from persisted storage
// declarations so future macro edits avoid recompiling the main settings file.
extension SettingsStore.Controller {
    func controllerMacroBinding(for action: ControllerMacroAction) -> ControllerMacroBinding {
        switch action {
        case .quickMenu: controllerMacroQuickMenu
        case .saveGameState: controllerMacroSaveGameState
        case .loadGameState: controllerMacroLoadGameState
        case .increaseSpeed: controllerMacroIncreaseSpeed
        case .decreaseSpeed: controllerMacroDecreaseSpeed
        case .enableFastForward: controllerMacroEnableFastForward
        case .disableFastForward: controllerMacroDisableFastForward
        case .undoSaveState: controllerMacroUndoSaveState
        }
    }

    func setControllerMacroBinding(
        _ binding: ControllerMacroBinding,
        for action: ControllerMacroAction
    ) {
        switch action {
        case .quickMenu: controllerMacroQuickMenu = binding
        case .saveGameState: controllerMacroSaveGameState = binding
        case .loadGameState: controllerMacroLoadGameState = binding
        case .increaseSpeed: controllerMacroIncreaseSpeed = binding
        case .decreaseSpeed: controllerMacroDecreaseSpeed = binding
        case .enableFastForward: controllerMacroEnableFastForward = binding
        case .disableFastForward: controllerMacroDisableFastForward = binding
        case .undoSaveState: controllerMacroUndoSaveState = binding
        }
        synchronizeControllerMacroGameplayInput()
    }

    /// SDL owns emulated-pad delivery, so it needs the union of configured chord buttons for
    /// consumption. Only Start/Pause is published as a delayed modifier; every other gameplay
    /// input retains master's direct delivery when pressed without Start.
    func synchronizeControllerMacroGameplayInput() {
        var inputMask: UInt32 = 0
        var modifierMask: UInt32 = 0
        for action in ControllerMacroAction.allCases {
            let binding = controllerMacroBinding(for: action)
            inputMask |= binding.gameplayInputMask
            modifierMask |= binding.gameplayModifierMask
        }
        ARMSX2Bridge.configureControllerMacroInput(
            mask: inputMask,
            modifierMask: modifierMask
        )
    }

    func resetControllerMacros() {
        for action in ControllerMacroAction.allCases {
            setControllerMacroBinding(action.defaultBinding, for: action)
        }
    }
}

extension SettingsStore.Controller {
    static let _padOpacityConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "PadOpacity", default: 0.6,
        suppressible: false,
        codec: .float)
    static let _phoneRumbleStrengthConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "PhoneRumbleStrength", default: 0.25,
        suppressible: false,
        codec: .float)
    static let _increaseRumbleDurationAndInterpolationConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "IncreaseRumbleDurationAndInterpolation", default: true,
        suppressible: false,
        codec: .bool)
    static let _hapticFeedbackConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "HapticFeedback", default: true,
        suppressible: false,
        codec: .bool)
    static let _gameRumbleStrengthConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "GameRumbleStrength", default: 1,
        suppressible: false,
        codec: .float)
    static let _uiRumbleStrengthConfig = Setting<Float>(
        section: "ARMSX2iOS/UI", key: "UIRumbleStrength", default: 0.5,
        suppressible: false,
        codec: .float)
    static let _dpadDiagonalsEnabledConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "DpadDiagonalsEnabled", default: true,
        suppressible: false,
        codec: .bool)
    static let _faceComboZonesEnabledConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "FaceComboZonesEnabled", default: true,
        suppressible: false,
        codec: .bool)
    static let _virtualPadSkinConfig = Setting<VirtualPadSkin>(
        section: "ARMSX2iOS/UI", key: "VirtualPadSkin", default: .armsx2Refresh,
        suppressible: false,
        codec: .rawInt)
    static let _automaticDownloadCustomSkinConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "AutomaticDownloadCustomSkin", default: false,
        codec: .bool)
    static let _autoHideVirtualPadWhenControllerConnectedConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "AutoHideVirtualPadWhenControllerConnected", default: true,
        codec: .bool)
    static let _autoFullscreenConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "AutoFullscreen", default: true,
        codec: .bool)
    static let _hideMenuButtonConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "HideMenuButton", default: false,
        codec: .bool)
    static let _gameControllerDeadZoneConfig = Setting<Float>(
        section: "ARMSX2iOS/Gamepad", key: "StickDeadZone", default: 0.15,
        codec: .float)
    static let _gameControllerLeftInstantDeadzoneConfig = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "LeftInstantDeadzoneEnabled", default: false,
        codec: .bool)
    static let _gameControllerLeftNegativeDeadzoneConfig = Setting<Float>(
        section: "ARMSX2iOS/Gamepad", key: "LeftNegativeDeadzone", default: -0.08,
        codec: .float)
    static let _gameControllerRightInstantDeadzoneConfig = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "RightInstantDeadzoneEnabled", default: false,
        codec: .bool)
    static let _gameControllerRightNegativeDeadzoneConfig = Setting<Float>(
        section: "ARMSX2iOS/Gamepad", key: "RightNegativeDeadzone", default: -0.08,
        codec: .float)
    static let _invertLeftStickXConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertLeftStickX", default: false,
        codec: .bool)
    static let _invertLeftStickYConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertLeftStickY", default: false,
        codec: .bool)
    static let _invertRightStickXConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertRightStickX", default: false,
        codec: .bool)
    static let _invertRightStickYConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "InvertRightStickY", default: false,
        codec: .bool)
    static let _controllerMultitapModeConfig = Setting<Int>(
        section: "ARMSX2iOS/Gamepad", key: "MultitapMode", default: 0,
        suppressible: false,
        codec: .int)
    static let _controllerMacroQuickMenuConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroQuickMenu",
        default: ControllerMacroAction.quickMenu.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroQuickMenuSelectPauseMigration = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "MacroQuickMenuSelectPauseMigrated",
        default: false, suppressible: false, codec: .bool)
    static let _controllerMacroStartDefaultsMigration = Setting<Bool>(
        section: "ARMSX2iOS/Gamepad", key: "MacroStartDefaultsMigrated",
        default: false, suppressible: false, codec: .bool)
    static let _controllerMacroSaveGameStateConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroSaveGameState",
        default: ControllerMacroAction.saveGameState.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroLoadGameStateConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroLoadGameState",
        default: ControllerMacroAction.loadGameState.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroIncreaseSpeedConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroIncreaseSpeed",
        default: ControllerMacroAction.increaseSpeed.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroDecreaseSpeedConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroDecreaseSpeed",
        default: ControllerMacroAction.decreaseSpeed.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroEnableFastForwardConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroEnableFastForward",
        default: ControllerMacroAction.enableFastForward.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroDisableFastForwardConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroDisableFastForward",
        default: ControllerMacroAction.disableFastForward.defaultBinding,
        suppressible: false, codec: .rawString)
    static let _controllerMacroUndoSaveStateConfig = Setting<ControllerMacroBinding>(
        section: "ARMSX2iOS/Gamepad", key: "MacroUndoSaveState",
        default: ControllerMacroAction.undoSaveState.defaultBinding,
        suppressible: false, codec: .rawString)
}
