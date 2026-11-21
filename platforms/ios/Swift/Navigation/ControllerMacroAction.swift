// ControllerMacroAction.swift — controller macro buttons, bindings and actions
// SPDX-License-Identifier: GPL-3.0+

@preconcurrency import GameController

enum ControllerMacroButton: String, CaseIterable, Identifiable, Sendable {
    case select
    case pause
    case start
    case l3
    case r3
    case l1
    case r1
    case l2
    case r2
    case cross
    case circle
    case square
    case triangle
    case dpadUp
    case dpadDown
    case dpadLeft
    case dpadRight

    var id: String { rawValue }

    var title: String {
        switch self {
        case .select: "Select"
        case .pause: "Pause"
        case .start: "Start"
        case .l3: "L3"
        case .r3: "R3"
        case .l1: "L1"
        case .r1: "R1"
        case .l2: "L2"
        case .r2: "R2"
        case .cross: "Cross"
        case .circle: "Circle"
        case .square: "Square"
        case .triangle: "Triangle"
        case .dpadUp: "D-Pad Up"
        case .dpadDown: "D-Pad Down"
        case .dpadLeft: "D-Pad Left"
        case .dpadRight: "D-Pad Right"
        }
    }

    /// Bit shared with the SDL gameplay-input gate. Pause and Start are the
    /// same physical `buttonMenu` control on Apple's extended profile.
    var gameplayInputMask: UInt32 {
        switch self {
        case .dpadUp: 1 << 0
        case .dpadDown: 1 << 1
        case .dpadLeft: 1 << 2
        case .dpadRight: 1 << 3
        case .cross: 1 << 4
        case .circle: 1 << 5
        case .square: 1 << 6
        case .triangle: 1 << 7
        case .l1: 1 << 8
        case .r1: 1 << 9
        case .l2: 1 << 10
        case .r2: 1 << 11
        case .pause, .start: 1 << 12
        case .select: 1 << 13
        case .l3: 1 << 14
        case .r3: 1 << 15
        }
    }

    fileprivate func isPressed(on gamepad: GCExtendedGamepad) -> Bool {
        switch self {
        case .select:
            gamepad.buttonOptions?.isPressed == true
        case .pause:
            gamepad.buttonMenu.isPressed
        case .start:
            gamepad.buttonMenu.isPressed
        case .l3:
            gamepad.leftThumbstickButton?.isPressed == true
        case .r3:
            gamepad.rightThumbstickButton?.isPressed == true
        case .l1:
            gamepad.leftShoulder.isPressed
        case .r1:
            gamepad.rightShoulder.isPressed
        case .l2:
            gamepad.leftTrigger.isPressed
        case .r2:
            gamepad.rightTrigger.isPressed
        case .cross:
            gamepad.buttonA.isPressed
        case .circle:
            gamepad.buttonB.isPressed
        case .square:
            gamepad.buttonX.isPressed
        case .triangle:
            gamepad.buttonY.isPressed
        case .dpadUp:
            gamepad.dpad.up.isPressed
        case .dpadDown:
            gamepad.dpad.down.isPressed
        case .dpadLeft:
            gamepad.dpad.left.isPressed
        case .dpadRight:
            gamepad.dpad.right.isPressed
        }
    }
}

struct ControllerMacroBinding: RawRepresentable, Hashable, Sendable {
    let first: ControllerMacroButton
    let second: ControllerMacroButton

    init(first: ControllerMacroButton, second: ControllerMacroButton) {
        self.first = first
        self.second = second
    }

    init?(rawValue: String) {
        let components = rawValue.split(separator: "+", omittingEmptySubsequences: true)
        guard components.count == 2,
              let first = ControllerMacroButton(rawValue: String(components[0])),
              let second = ControllerMacroButton(rawValue: String(components[1])),
              first != second else { return nil }
        self.init(first: first, second: second)
    }

    var rawValue: String { "\(first.rawValue)+\(second.rawValue)" }
    var title: String { "\(first.title) + \(second.title)" }
    var gameplayInputMask: UInt32 {
        first.gameplayInputMask | second.gameplayInputMask
    }
    /// Start/Pause is the only gameplay input allowed to wait for chord resolution. Every other
    /// button keeps the direct SDL-to-emulated-pad delivery used by master when pressed alone.
    /// Custom chords without Start still trigger their action, but do not add latency to either
    /// constituent input.
    var gameplayModifierMask: UInt32 {
        let startMask = ControllerMacroButton.start.gameplayInputMask
        return gameplayInputMask & startMask
    }

    func isPressed(on gamepad: GCExtendedGamepad) -> Bool {
        first.isPressed(on: gamepad) && second.isPressed(on: gamepad)
    }
}

enum ControllerMacroAction: String, CaseIterable, Identifiable, Sendable {
    case quickMenu
    case saveGameState
    case loadGameState
    case increaseSpeed
    case decreaseSpeed
    case enableFastForward
    case disableFastForward
    case undoSaveState

    var id: String { rawValue }

    var title: String {
        switch self {
        case .quickMenu: "Quick Menu"
        case .saveGameState: "Save Game State"
        case .loadGameState: "Load Game State"
        case .increaseSpeed: "Increase Emulation Speed"
        case .decreaseSpeed: "Decrease Emulation Speed"
        case .enableFastForward: "Enable Fast Forward (1000%)"
        case .disableFastForward: "Disable Fast Forward (100%)"
        case .undoSaveState: "Undo Save State"
        }
    }

    var detail: String {
        switch self {
        case .quickMenu:
            "Opens the Quick Menu while a game is running."
        case .saveGameState:
            "Saves to Quick Save."
        case .loadGameState:
            "Loads Quick Save."
        case .increaseSpeed:
            "Adds 25% while pressed or held."
        case .decreaseSpeed:
            "Subtracts 25% while pressed or held."
        case .enableFastForward:
            "Enables Fast Forward and sets speed to 1000%."
        case .disableFastForward:
            "Disables Fast Forward and restores speed to 100%."
        case .undoSaveState:
            "Undoes the last load, delete or save-over while its notice shows. Hold the first button, then press the second."
        }
    }

    var defaultBinding: ControllerMacroBinding {
        switch self {
        case .quickMenu:
            ControllerMacroBinding(first: .select, second: .pause)
        case .saveGameState:
            ControllerMacroBinding(first: .start, second: .r2)
        case .loadGameState:
            ControllerMacroBinding(first: .start, second: .l2)
        case .increaseSpeed:
            ControllerMacroBinding(first: .start, second: .dpadRight)
        case .decreaseSpeed:
            ControllerMacroBinding(first: .start, second: .dpadLeft)
        case .enableFastForward:
            ControllerMacroBinding(first: .start, second: .dpadUp)
        case .disableFastForward:
            ControllerMacroBinding(first: .start, second: .dpadDown)
        case .undoSaveState:
            ControllerMacroBinding(first: .start, second: .circle)
        }
    }

    var emulationShortcut: EmulationControllerShortcut? {
        switch self {
        case .quickMenu: nil
        case .saveGameState: .saveLastState
        case .loadGameState: .loadLastState
        case .increaseSpeed: .increaseSpeed
        case .decreaseSpeed: .decreaseSpeed
        case .enableFastForward: .enableFastForward
        case .disableFastForward: .disableFastForward
        case .undoSaveState: .undoSaveState
        }
    }

    var repeatsWhileHeld: Bool {
        self == .increaseSpeed || self == .decreaseSpeed
    }
}

enum EmulationControllerShortcut: Hashable, Sendable {
    case saveLastState
    case loadLastState
    case increaseSpeed
    case decreaseSpeed
    case enableFastForward
    case disableFastForward
    case undoSaveState
}

struct EmulationControllerShortcutRequest: Equatable, Sendable {
    let sequence: UInt64
    let shortcut: EmulationControllerShortcut
}
