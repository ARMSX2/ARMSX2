// CheatsControllerFocus.swift — Controller focus look for the Cheats & Patches rows
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct CheatsControllerFocusModifier: ViewModifier {
    let targetID: String
    let isFocused: Bool
    let cornerRadius: CGFloat

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.uiAccentColour) private var accentColour

    func body(content: Content) -> some View {
        content
            .focusEffectDisabled()
            .padding(.horizontal, 7)
            .foregroundStyle(isFocused ? accentColour : Color.primary)
            .background {
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(isFocused ? Color.black.opacity(0.2) : .clear)
            }
            .controllerFocusBoxPresentation(
                isVisible: isFocused,
                cornerRadius: cornerRadius,
                performanceOptimized: true
            )
            .controllerNavigationOrbTarget(
                id: "cheats.\(targetID)",
                isActive: isFocused,
                palette: .blue,
                style: .plain,
                inset: 2,
                orbScale: 0.7,
                priority: 10
            )
            .scaleEffect(isFocused && !reduceMotion ? 1.01 : 1)
            .shadow(
                color: .black.opacity(isFocused ? 0.1 : 0),
                radius: isFocused ? 4 : 0,
                y: isFocused ? 2 : 0
            )
            .animation(
                reduceMotion ? .linear(duration: 0.1) : .smooth(duration: 0.2),
                value: isFocused
            )
    }
}

@MainActor
func cheatsControllerFocus(
    _ target: CheatsPatchesManagerView.ControllerTarget,
    controllerInput: MenuControllerInputRouter?,
    controllerTarget: CheatsPatchesManagerView.ControllerTarget
) -> CheatsControllerFocusModifier {
    CheatsControllerFocusModifier(
        targetID: String(describing: target),
        isFocused:
            controllerInput?.isControllerNavigationEnabled == true
                && controllerTarget == target,
        cornerRadius: 12
    )
}
