// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Controller macro behavior is separate from persisted storage
// declarations so future macro edits avoid recompiling the main settings file.
extension SettingsStore {
    func controllerMacroBinding(for action: ControllerMacroAction) -> ControllerMacroBinding {
        switch action {
        case .quickMenu: controllerMacroQuickMenu
        case .saveGameState: controllerMacroSaveGameState
        case .loadGameState: controllerMacroLoadGameState
        case .increaseSpeed: controllerMacroIncreaseSpeed
        case .decreaseSpeed: controllerMacroDecreaseSpeed
        case .enableFastForward: controllerMacroEnableFastForward
        case .disableFastForward: controllerMacroDisableFastForward
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
