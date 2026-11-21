// ControllerUIThemePreset+TextOverrides.swift — per-theme text colour overrides
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension ControllerUIThemePreset {
    var textOverrides: ControllerUIThemeTextOverrides? {
        switch self {
        case .defaultTheme, .purpleMilk, .henyBlue:
            let blue = rgb(0x57B8F9)
            return text(
                // Body and card copy remain white while selection and value
                // accents retain Heny Blue.
                normal: .white,
                secondary: .white.opacity(0.72),
                focused: blue,
                title: .white
            )
        case .neon:
            return text(
                normal: Color(red: 0.48, green: 0.80, blue: 1.00),
                secondary: Color(red: 0.72, green: 0.90, blue: 1.00),
                focused: Color(red: 0.36, green: 0.74, blue: 1.00),
                title: Color(red: 0.62, green: 0.86, blue: 1.00)
            )
        case .gold:
            return text(
                normal: Color(red: 1.00, green: 0.82, blue: 0.30),
                secondary: Color(red: 1.00, green: 0.92, blue: 0.62),
                focused: Color(red: 1.00, green: 0.74, blue: 0.12),
                title: Color(red: 1.00, green: 0.88, blue: 0.42)
            )
        case .cyberGreen:
            return text(
                normal: Color(red: 0.42, green: 1.00, blue: 0.12),
                secondary: Color(red: 0.72, green: 1.00, blue: 0.52),
                focused: Color(red: 0.28, green: 1.00, blue: 0.05),
                title: Color(red: 0.58, green: 1.00, blue: 0.30)
            )
        case .ocean:
            return text(
                normal: Color(red: 0.34, green: 0.84, blue: 1.00),
                secondary: Color(red: 0.66, green: 0.93, blue: 1.00),
                focused: Color(red: 0.24, green: 0.78, blue: 1.00),
                title: Color(red: 0.50, green: 0.89, blue: 1.00)
            )
        case .roseGold:
            return text(
                normal: Color(red: 1.00, green: 0.68, blue: 0.78),
                secondary: Color(red: 1.00, green: 0.86, blue: 0.89),
                focused: Color(red: 1.00, green: 0.52, blue: 0.70),
                title: Color(red: 1.00, green: 0.74, blue: 0.82)
            )
        case .classicPS1:
            return text(
                normal: Color(red: 0.22, green: 0.21, blue: 0.19),
                secondary: Color(red: 0.34, green: 0.32, blue: 0.29),
                focused: Color(red: 0.12, green: 0.12, blue: 0.11),
                title: Color(red: 0.17, green: 0.16, blue: 0.15)
            )
        case .electricLime:
            return text(
                normal: Color(red: 0.76, green: 1.00, blue: 0.08),
                secondary: Color(red: 0.90, green: 1.00, blue: 0.56),
                focused: Color(red: 0.68, green: 1.00, blue: 0.02),
                title: Color(red: 0.84, green: 1.00, blue: 0.28)
            )
        case .scanline:
            return text(
                normal: Color(red: 0.54, green: 0.96, blue: 0.66),
                secondary: Color(red: 0.74, green: 0.94, blue: 0.78),
                focused: Color(red: 0.20, green: 1.00, blue: 0.50),
                title: Color(red: 0.62, green: 1.00, blue: 0.72)
            )
        case .pastelLavender:
            return text(
                normal: Color(red: 0.82, green: 0.72, blue: 1.00),
                secondary: Color(red: 0.92, green: 0.87, blue: 1.00),
                focused: Color(red: 0.75, green: 0.60, blue: 1.00),
                title: Color(red: 0.87, green: 0.80, blue: 1.00)
            )
        case .pastelOcean:
            return text(
                normal: Color(red: 0.45, green: 0.86, blue: 1.00),
                secondary: Color(red: 0.72, green: 0.94, blue: 1.00),
                focused: Color(red: 0.30, green: 0.78, blue: 1.00),
                title: Color(red: 0.58, green: 0.90, blue: 1.00)
            )
        case .minimalGraphite:
            return text(
                normal: Color(white: 0.78),
                secondary: Color(white: 0.90),
                focused: Color(white: 0.98),
                title: Color(white: 0.86)
            )
        case .minimalSand:
            return text(
                normal: Color(red: 0.92, green: 0.80, blue: 0.62),
                secondary: Color(red: 0.98, green: 0.90, blue: 0.76),
                focused: Color(red: 0.92, green: 0.62, blue: 0.30),
                title: Color(red: 0.96, green: 0.84, blue: 0.66)
            )
        case .minimalOcean:
            return text(
                normal: Color(red: 0.60, green: 0.86, blue: 0.92),
                secondary: Color(red: 0.78, green: 0.94, blue: 0.96),
                focused: Color(red: 0.30, green: 0.82, blue: 0.96),
                title: Color(red: 0.66, green: 0.90, blue: 0.96)
            )
        case .spatialRetroGrid:
            return text(
                normal: Color(red: 0.68, green: 0.86, blue: 1.00),
                secondary: Color(red: 0.84, green: 0.93, blue: 1.00),
                focused: Color(red: 0.42, green: 0.78, blue: 1.00),
                title: Color(red: 0.72, green: 0.88, blue: 1.00)
            )
        case .orbitalTowers, .faceButtonDream:
            return text(
                normal: Color(red: 0.42, green: 0.80, blue: 1.00),
                secondary: Color(red: 0.72, green: 0.91, blue: 1.00),
                focused: Color(red: 0.28, green: 0.72, blue: 1.00),
                title: Color(red: 0.56, green: 0.85, blue: 1.00)
            )
        case .luminousRibbons:
            return text(
                normal: Color(red: 1.00, green: 0.86, blue: 0.44),
                secondary: Color(red: 1.00, green: 0.94, blue: 0.72),
                focused: Color(red: 1.00, green: 0.78, blue: 0.20),
                title: Color(red: 1.00, green: 0.90, blue: 0.52)
            )
        default:
            return nil
        }
    }

    private func text(
        normal: Color,
        secondary: Color,
        focused: Color,
        title: Color
    ) -> ControllerUIThemeTextOverrides {
        ControllerUIThemeTextOverrides(
            normal: normal,
            secondary: secondary,
            focused: focused,
            title: title
        )
    }
}
