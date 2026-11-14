// ShaderPassReorderButton.swift — chevron pair that moves a shader layer up or down
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderPassReorderButton: View {
    let canMoveUp: Bool
    let canMoveDown: Bool
    let label: String
    let targetID: String
    let controllerInput: MenuControllerInputRouter?
    let onMoveUp: () -> Void
    let onMoveDown: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        HStack(spacing: 0) {
            Button {
                performTouchAction(onMoveUp)
            } label: {
                Image(systemName: "chevron.up")
                    .frame(width: 22, height: 32)
            }
            .disabled(!canMoveUp)

            Button {
                performTouchAction(onMoveDown)
            } label: {
                Image(systemName: "chevron.down")
                    .frame(width: 22, height: 32)
            }
            .disabled(!canMoveDown)
        }
        .buttonStyle(.borderless)
        .foregroundStyle(accentColour)
        .accessibilityElement(children: .ignore)
        .controllerAccessibilityAdjustableTarget(
            id: targetID,
            label: label,
            value: nil,
            onActivate: {
                if canMoveDown { onMoveDown() }
                else if canMoveUp { onMoveUp() }
            },
            onIncrement: { if canMoveDown { onMoveDown() } },
            onDecrement: { if canMoveUp { onMoveUp() } }
        )
    }

    private func performTouchAction(_ action: () -> Void) {
        action()
        MenuAudioPackManager.shared.play(.activate)
        controllerInput?.playTouchHaptics(.activate)
    }
}
