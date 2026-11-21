// ControllerToolbarFocusVisualModifier.swift — toolbar focus visuals
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ControllerToolbarFocusVisualModifier: ViewModifier {
    let isFocused: Bool
    let focusID: String?
    var baseHorizontalPadding: CGFloat = 0
    var minimumWidth: CGFloat = 36
    var foregroundColour: Color? = nil
    @Environment(\.uiToolbarColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    @ViewBuilder
    func body(content: Content) -> some View {
        let visuallyFocused = isFocused
            && controllerInput?.isControllerNavigationEnabled != false
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        let focusedLabel = content
            .focusEffectDisabled()
            // Toolbar buttons are actions, not body copy. Match master by
            // keeping Boot BIOS and every toolbar symbol in the semantic
            // accent in both focused and unfocused states.
            .foregroundStyle(foregroundColour ?? accentColour)
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
        let visual = focusedLabel
            .padding(.horizontal, baseHorizontalPadding)
            .frame(minWidth: minimumWidth, minHeight: 36)
            .background {
                Capsule(style: .continuous)
                    .fill(Color.black.opacity(visuallyFocused ? 0.48 : 0))
            }
            .controllerFocusBoxPresentation(
                isVisible: visuallyFocused,
                cornerRadius: 10_000
            )
            .animation(.linear(duration: 0.08), value: visuallyFocused)

        if let focusID {
            visual.controllerNavigationOrbTarget(
                id: focusID,
                isActive: visuallyFocused,
                palette: .green,
                inset: 2,
                orbScale: 0.72,
                priority: 20
            )
        } else { visual }
    }
}

private struct ControllerAccessibilityToolbarFocusVisualModifier: ViewModifier {
    let baseHorizontalPadding: CGFloat
    let minimumWidth: CGFloat
    let fallbackHorizontalPadding: CGFloat
    let fallbackMinimumHeight: CGFloat
    @Environment(\.controllerAccessibilityNavigationActive) private var navigationActive
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused
    @Environment(\.controllerAccessibilityUsesWindowFocusVisual)
    private var usesWindowFocusVisual
    @Environment(\.uiToolbarColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    @ViewBuilder
    func body(content: Content) -> some View {
        let visuallyFocused = isFocused
            && controllerInput?.isControllerNavigationEnabled != false
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        if navigationActive, usesWindowFocusVisual {
            let focusedLabel = content
                .focusEffectDisabled()
                .foregroundStyle(accentColour)
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
            focusedLabel
                .padding(.horizontal, baseHorizontalPadding)
                .frame(minWidth: minimumWidth, minHeight: 36)
                .background {
                    Capsule(style: .continuous)
                        .fill(Color.black.opacity(visuallyFocused ? 0.48 : 0))
                }
                .animation(.linear(duration: 0.08), value: visuallyFocused)
        } else if navigationActive {
            content.modifier(
                ControllerToolbarFocusVisualModifier(
                    isFocused: visuallyFocused,
                    focusID: nil,
                    baseHorizontalPadding: baseHorizontalPadding,
                    minimumWidth: minimumWidth
                )
            )
        } else {
            content
                .foregroundStyle(accentColour)
                .shadow(
                    color: textAppearance.normalShadowColor.opacity(
                        textAppearance.normalShadowStrength
                    ),
                    radius: CGFloat(
                        5 * textAppearance.normalShadowStrength
                    ),
                    y: CGFloat(2 * textAppearance.normalShadowStrength)
                )
                .padding(.horizontal, fallbackHorizontalPadding)
                .frame(minHeight: fallbackMinimumHeight)
        }
    }
}

extension View {
    func controllerAccessibilityToolbarFocusVisual(
        baseHorizontalPadding: CGFloat = 0,
        minimumWidth: CGFloat = 36,
        fallbackHorizontalPadding: CGFloat = 0,
        fallbackMinimumHeight: CGFloat = 0
    ) -> some View {
        modifier(
            ControllerAccessibilityToolbarFocusVisualModifier(
                baseHorizontalPadding: baseHorizontalPadding,
                minimumWidth: minimumWidth,
                fallbackHorizontalPadding: fallbackHorizontalPadding,
                fallbackMinimumHeight: fallbackMinimumHeight
            )
        )
    }
}
