// ControllerPrompt.swift — prompt any screen can ask, pad glyphs and hint line
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Observation
import GameController

/// A question any screen can ask through RootView's prompt window. SwiftUI's own
/// `.confirmationDialog` is a UIAlertController, which a controller cannot reach.
@MainActor
@Observable
final class ControllerPrompt {
    struct Action {
        let title: String
        var isDestructive = false
        var isCancel = false
        var run: @MainActor () -> Void = {}
    }

    struct Request {
        let id = UUID()
        let title: String
        let message: String
        let actions: [Action]
    }

    static let shared = ControllerPrompt()
    private(set) var request: Request?

    func ask(_ title: String, message: String, actions: [Action]) {
        request = Request(title: title, message: message, actions: actions)
    }

    /// Runs the chosen action, or the cancel action for nil (Circle, or a tap outside).
    func answer(_ index: Int?) {
        guard let request else { return }
        self.request = nil
        let chosen = index.flatMap { request.actions.indices.contains($0) ? request.actions[$0] : nil }
        (chosen ?? request.actions.first(where: \.isCancel))?.run()
    }
}

extension ControllerPrompt.Action {
    @MainActor static var cancel: Self { .init(title: SettingsStore.shared.localized("Cancel"), isCancel: true) }
    @MainActor static var ok: Self { .init(title: SettingsStore.shared.localized("OK"), isCancel: true) }
}

extension View {
    /// Use in place of `.alert` and `.confirmationDialog`: the question goes to RootView's prompt
    /// window, which touch and the controller can both answer. Any answer clears `isPresented`.
    func controllerPrompt(
        _ title: String,
        isPresented: Binding<Bool>,
        message: String = "",
        actions: [ControllerPrompt.Action]
    ) -> some View {
        onChange(of: isPresented.wrappedValue, initial: true) { _, presented in
            guard presented else { return }
            ControllerPrompt.shared.ask(title, message: message, actions: actions.map { action in
                var action = action
                let run = action.run
                action.run = {
                    run()
                    isPresented.wrappedValue = false
                }
                return action
            })
        }
    }
}

/// A pad button the way the connected controller draws it: □ on a DualSense, X on an Xbox pad.
/// Start and Select stay words in a chip, which read better than their icons.
struct ControllerButtonGlyph: View {
    let button: ControllerMacroButton
    let tint: Color
    var symbolFont: Font = .caption.weight(.semibold)
    var chipFont: Font = .caption2.weight(.semibold)

    /// `current` follows the controller the player used last, so the artwork can't come from a
    /// second, idle one.
    static var connectedGamepad: GCExtendedGamepad? {
        if let current = GCController.current?.extendedGamepad { return current }
        return GCController.controllers().lazy.compactMap(\.extendedGamepad).first
    }

    var body: some View {
        if let title = chipTitle {
            Text(title)
                .font(chipFont)
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(tint.opacity(0.10), in: Capsule(style: .continuous))
                .overlay {
                    Capsule(style: .continuous)
                        .stroke(tint.opacity(0.22), lineWidth: 0.65)
                }
                .accessibilityLabel(title)
        } else if let element, let symbol = element.sfSymbolsName, !symbol.isEmpty {
            Image(systemName: symbol)
                .font(symbolFont)
                .symbolRenderingMode(.hierarchical)
                .accessibilityLabel(element.localizedName ?? button.title)
        } else {
            Text(shortTitle)
                .font(chipFont.bold())
                .accessibilityLabel(button.title)
        }
    }

    private var chipTitle: String? {
        switch button {
        case .select: SettingsStore.shared.localized("Select")
        case .pause, .start: SettingsStore.shared.localized("Start")
        default: nil
        }
    }

    private var element: GCControllerElement? {
        guard let gamepad = Self.connectedGamepad else { return nil }
        switch button {
        case .select, .pause, .start: return nil
        case .l3: return gamepad.leftThumbstickButton
        case .r3: return gamepad.rightThumbstickButton
        case .l1: return gamepad.leftShoulder
        case .r1: return gamepad.rightShoulder
        case .l2: return gamepad.leftTrigger
        case .r2: return gamepad.rightTrigger
        case .cross: return gamepad.buttonA
        case .circle: return gamepad.buttonB
        case .square: return gamepad.buttonX
        case .triangle: return gamepad.buttonY
        case .dpadUp: return gamepad.dpad.up
        case .dpadDown: return gamepad.dpad.down
        case .dpadLeft: return gamepad.dpad.left
        case .dpadRight: return gamepad.dpad.right
        }
    }

    private var shortTitle: String {
        switch button {
        case .cross: "✕"
        case .circle: "○"
        case .square: "□"
        case .triangle: "△"
        case .dpadUp: "↑"
        case .dpadDown: "↓"
        case .dpadLeft: "←"
        case .dpadRight: "→"
        default: button.title
        }
    }
}

/// Controls and what they do, in one line: "□ Rename, lock, delete", "Start + R2 Save · Start + L2 Load".
struct ControllerHintLine: View {
    struct Part {
        let buttons: [ControllerMacroButton]
        let label: String

        init(_ binding: ControllerMacroBinding, _ label: String) {
            buttons = [binding.first, binding.second]
            self.label = label
        }

        init(_ button: ControllerMacroButton, _ label: String) {
            buttons = [button]
            self.label = label
        }
    }

    let parts: [Part]

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        HStack(spacing: 4) {
            ForEach(parts.indices, id: \.self) { index in
                if index > 0 { Text("·") }
                ForEach(parts[index].buttons.indices, id: \.self) { position in
                    if position > 0 {
                        Image(systemName: "plus").imageScale(.small)
                    }
                    ControllerButtonGlyph(button: parts[index].buttons[position], tint: accentColour)
                        .foregroundStyle(accentColour)
                }
                Text(parts[index].label)
            }
        }
        .lineLimit(1)
        .accessibilityElement(children: .combine)
    }
}
