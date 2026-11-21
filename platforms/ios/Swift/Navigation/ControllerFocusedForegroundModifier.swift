// ControllerFocusedForegroundModifier.swift — focused text colour
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ControllerFocusedForegroundModifier: ViewModifier {
    let normal: Color
    let allowsFocusedBlue: Bool
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    func body(content: Content) -> some View {
        let controllerOwnsAppearance =
            controllerInput?.isControllerNavigationEnabled != false
        let visuallyFocused = isFocused && controllerOwnsAppearance
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        content
            .foregroundStyle(
                visuallyFocused && allowsFocusedBlue
                    ? textAppearance.focusedColor
                    : (allowsFocusedBlue
                        ? (textAppearance.normalColor ?? normal)
                        : normal)
            )
            .shadow(
                color: (visuallyFocused
                    ? textAppearance.focusedShadowColor
                    : textAppearance.normalShadowColor
                ).opacity(shadowStrength),
                radius: CGFloat(5 * shadowStrength),
                y: CGFloat(2 * shadowStrength)
            )
            .animation(
                ControllerFocusVisualAnimation.textFade,
                value: visuallyFocused
            )
    }
}

extension View {
    func controllerFocusedTextColor(
        normal: Color = .primary,
        allowsFocusedBlue: Bool = true
    ) -> some View {
        modifier(
            ControllerFocusedForegroundModifier(
                normal: normal,
                allowsFocusedBlue: allowsFocusedBlue
            )
        )
    }
}
