// ControllerTextAppearance.swift — themed text colours and their environment value
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ControllerTextAppearance {
    var normalColor: Color?
    var secondaryColor: Color?
    var titleColor: Color
    var contentColor: Color?
    var unselectedColor: Color
    var focusedColor: Color
    var normalShadowColor: Color
    var focusedShadowColor: Color
    var normalShadowStrength: Double
    var focusedShadowStrength: Double

    static let standard = ControllerTextAppearance(
        normalColor: nil,
        secondaryColor: nil,
        titleColor: .primary,
        contentColor: nil,
        unselectedColor: .secondary,
        focusedColor: .blue,
        normalShadowColor: .black,
        focusedShadowColor: .black,
        normalShadowStrength: 0,
        focusedShadowStrength: 0.1
    )
}

private struct ControllerTextAppearanceEnvironmentKey: EnvironmentKey {
    static let defaultValue = ControllerTextAppearance.standard
}

extension EnvironmentValues {
    var controllerTextAppearance: ControllerTextAppearance {
        get { self[ControllerTextAppearanceEnvironmentKey.self] }
        set { self[ControllerTextAppearanceEnvironmentKey.self] = newValue }
    }
}
