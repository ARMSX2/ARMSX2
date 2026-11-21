// ControllerUIThemePreset+Readability.swift — per-theme readability overrides
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension ControllerUIThemePreset {
    /// Preset-specific contrast for surfaces that do not share the same
    /// luminance. Presets explicitly approved by the user are intentionally
    /// absent so their existing appearance remains unchanged.
    var readabilityOverrides: ControllerUIThemeReadabilityOverrides? {
        switch self {
        // Bright library backgrounds: retain dark library ink and use a
        // separate light foreground on the graphite/glass overlay surfaces.
        case .diamond:
            return lightReadability(
                primary: rgb(0x102B3A), secondary: rgb(0x365966),
                focused: rgb(0x0A5774),
                overlayPrimary: rgb(0x102B3A), overlaySecondary: rgb(0x365966),
                replacesWhiteTabTitle: true
            )
        case .pastelMint:
            return lightReadability(
                primary: rgb(0x163D33), secondary: rgb(0x45685E),
                overlayPrimary: rgb(0xF1FFF9), overlaySecondary: rgb(0xB9D9CD),
                replacesWhiteTabTitle: true
            )
        case .minimalSnow:
            return lightReadability(
                primary: rgb(0x20262D), secondary: rgb(0x4C5863),
                focused: rgb(0x35566D),
                overlayPrimary: rgb(0x20262D), overlaySecondary: rgb(0x4C5863),
                replacesWhiteTabTitle: true
            )
        case .portableGlass:
            return lightReadability(
                primary: rgb(0x102B3C), secondary: rgb(0x3B5C6B),
                focused: rgb(0x075C7D),
                overlayPrimary: rgb(0x102B3C), overlaySecondary: rgb(0x3B5C6B),
                replacesWhiteTabTitle: true
            )

        // These presets already have the intended primary treatment, but
        // their old shared secondary role was too faint on both surfaces.
        case .liveSilver:
            return secondaryReadability(
                secondary: rgb(0x414C57),
                overlayPrimary: rgb(0xF4F6FA), overlaySecondary: rgb(0xB3BDC8)
            )
        case .arctic:
            return lightReadability(
                primary: rgb(0x163747), secondary: rgb(0x315363),
                focused: rgb(0x0E607D),
                overlayPrimary: rgb(0x163747), overlaySecondary: rgb(0x315363),
                replacesWhiteTabTitle: true
            )
        case .synthwave:
            return darkReadability(
                primary: rgb(0xFFF0FB), secondary: rgb(0xD9BCEB)
            )
        case .pastelSunrise:
            return secondaryReadability(
                secondary: rgb(0x6B493D),
                overlayPrimary: rgb(0xFFF7ED), overlaySecondary: rgb(0xDDBEAD)
            )

        // Dark and saturated backgrounds need a light primary card role and
        // a visibly quieter, but still readable, secondary role.
        case .galaxy:
            return darkReadability(
                primary: rgb(0xF5F0FF), secondary: rgb(0xCBBCE6)
            )
        case .blackHole:
            return darkReadability(
                primary: rgb(0xF3F5FA), secondary: rgb(0xB9C1CE)
            )
        case .cherry:
            return darkReadability(
                primary: rgb(0xFFF0F6), secondary: rgb(0xE7B5CE)
            )
        case .bloodDragon, .redBlood:
            return darkReadability(
                primary: rgb(0xFFF1F1), secondary: rgb(0xE0B4B8)
            )
        case .plasma:
            return darkReadability(
                primary: rgb(0xFFF3E8), secondary: rgb(0xE5C1A2)
            )
        case .ultraviolet:
            return darkReadability(
                primary: rgb(0xF6EDFF), secondary: rgb(0xCDB8E8)
            )
        case .obsidian:
            return darkReadability(
                primary: rgb(0xF3F5FA), secondary: rgb(0xBAC2CF)
            )
        case .copper:
            return darkReadability(
                primary: rgb(0xFFF0E2), secondary: rgb(0xDDBB9C)
            )
        case .classicPS1:
            return darkReadability(
                primary: rgb(0xF4F1E9), secondary: rgb(0xCEC7BB)
            )
        case .classicPS2:
            return darkReadability(
                primary: rgb(0xF3E9DC), secondary: rgb(0xC9B8A3)
            )
        case .sunset:
            return darkReadability(
                primary: rgb(0xFFF3E8), secondary: rgb(0xE8C0B1)
            )
        case .midnight, .minimalMidnight:
            return darkReadability(
                primary: rgb(0xEEF4FF), secondary: rgb(0xB6C7E6)
            )
        case .royal:
            return darkReadability(
                primary: rgb(0xFFF5DC), secondary: rgb(0xD7C4E8)
            )
        case .eclipse:
            return darkReadability(
                primary: rgb(0xF5EEFF), secondary: rgb(0xC8B7DA)
            )
        case .rainbow, .ambientSpectrum:
            return darkReadability(
                primary: rgb(0xFFFFFF), secondary: rgb(0xD9E2EC)
            )
        case .comet, .lightSpeedNebula:
            return darkReadability(
                primary: rgb(0xF2FBFF), secondary: rgb(0xB9DDEB)
            )
        case .splineFlow:
            return darkReadability(
                primary: rgb(0xF5EFFF), secondary: rgb(0xCDBBE5)
            )
        case .particleWave, .oceanWave:
            return darkReadability(
                primary: rgb(0xEDFBFF), secondary: rgb(0xAED8E5)
            )
        case .aurora:
            return darkReadability(
                primary: rgb(0xF0FFFB), secondary: rgb(0xB8E5DB)
            )

        // Pastel fields need dark library ink, but their overlay glass still
        // needs the light foreground used by the other command decks.
        case .sakura, .pastelSakura:
            return lightReadability(
                primary: rgb(0x452536), secondary: rgb(0x704A5C),
                overlayPrimary: rgb(0xFFF0F5), overlaySecondary: rgb(0xE3B6C8)
            )
        case .pastelLavender:
            return lightReadability(
                primary: rgb(0x302643), secondary: rgb(0x5D4D73),
                focused: rgb(0x67498E),
                overlayPrimary: rgb(0x302643), overlaySecondary: rgb(0x5D4D73),
                replacesWhiteTabTitle: true
            )

        // These pale silver surfaces were previously considered complete,
        // but system-primary labels still resolved to white. Give every text
        // role an explicit graphite value so native and custom labels agree.
        case .minimal:
            return lightReadability(
                primary: rgb(0x252B31), secondary: rgb(0x505B65),
                focused: rgb(0x315D78),
                overlayPrimary: rgb(0x252B31), overlaySecondary: rgb(0x505B65),
                replacesWhiteTabTitle: true
            )

        // Only overlay surfaces were reported as problematic for these.
        case .emerald:
            return overlayReadability(
                primary: rgb(0xF0FFF7), secondary: rgb(0xB5D9C6)
            )
        case .frostedPearl:
            return lightReadability(
                primary: rgb(0x252B33), secondary: rgb(0x505B66),
                focused: rgb(0x315A6F),
                overlayPrimary: rgb(0x252B33), overlaySecondary: rgb(0x505B66),
                replacesWhiteTabTitle: true
            )

        // The Y2K family owns a complete, pre-audited text hierarchy instead
        // of inheriting whichever palette stop happens to be brightest.
        case .frutigerAero, .aquaWeb:
            return lightReadability(
                primary: rgb(0x07345F), secondary: rgb(0x38657A),
                focused: rgb(0x0066A8),
                overlayPrimary: rgb(0x07345F), overlaySecondary: rgb(0x38657A),
                replacesWhiteTabTitle: true
            )
        case .frutigerEco, .digitalMeadow, .glassGarden:
            return lightReadability(
                primary: rgb(0x123A2A), secondary: rgb(0x436B59),
                focused: rgb(0x087A45),
                overlayPrimary: rgb(0x123A2A), overlaySecondary: rgb(0x436B59),
                replacesWhiteTabTitle: true
            )
        case .darkAero:
            return darkReadability(
                primary: rgb(0xE8FCFF), secondary: rgb(0xA7DDE5)
            )
        case .technozen, .zenGlass:
            return lightReadability(
                primary: rgb(0x173B3C), secondary: rgb(0x4A6B69),
                focused: rgb(0x167777),
                overlayPrimary: rgb(0x173B3C), overlaySecondary: rgb(0x4A6B69),
                replacesWhiteTabTitle: true
            )
        case .dorfic:
            return lightReadability(
                primary: rgb(0x432B1E), secondary: rgb(0x745644),
                focused: rgb(0x8A461E),
                overlayPrimary: rgb(0x432B1E), overlaySecondary: rgb(0x745644),
                replacesWhiteTabTitle: true
            )
        case .frutigerMetro:
            return lightReadability(
                primary: rgb(0x172E58), secondary: rgb(0x4D5F79),
                focused: rgb(0x005EB8),
                overlayPrimary: rgb(0x172E58), overlaySecondary: rgb(0x4D5F79),
                replacesWhiteTabTitle: true
            )
        case .mcBling, .bubblegumTech:
            return lightReadability(
                primary: rgb(0x4B1739), secondary: rgb(0x7D4C6C),
                focused: rgb(0xB01872),
                overlayPrimary: rgb(0x4B1739), overlaySecondary: rgb(0x7D4C6C),
                replacesWhiteTabTitle: true
            )
        case .vectorbloom:
            return lightReadability(
                primary: rgb(0x3E2440), secondary: rgb(0x68556B),
                focused: rgb(0x8B2F78),
                overlayPrimary: rgb(0x3E2440), overlaySecondary: rgb(0x68556B),
                replacesWhiteTabTitle: true
            )
        case .skeuomorphicAqua, .y2kChrome:
            return lightReadability(
                primary: rgb(0x182F3F), secondary: rgb(0x4A6370),
                focused: rgb(0x086B97),
                overlayPrimary: rgb(0x182F3F), overlaySecondary: rgb(0x4A6370),
                replacesWhiteTabTitle: true
            )
        case .metroNight:
            return darkReadability(
                primary: rgb(0xF2F7FF), secondary: rgb(0xB9C9E4)
            )
        case .defaultTheme, .purpleMilk, .henyBlue:
            return darkReadability(
                primary: .white,
                secondary: .white.opacity(0.72)
            )

        // Keep the explicitly approved presets byte-for-byte equivalent in
        // their semantic text selection.
        case .custom, .enderPearlPul, .neon, .gold, .cyberGreen, .ocean,
             .roseGold, .electricLime, .scanline, .pastelOcean,
             .minimalGraphite, .minimalSand, .minimalOcean,
             .spatialRetroGrid, .orbitalTowers, .faceButtonDream,
             .luminousRibbons:
            return nil
        }
    }

    private func lightReadability(
        primary: Color,
        secondary: Color,
        focused: Color? = nil,
        overlayPrimary: Color,
        overlaySecondary: Color,
        replacesWhiteTabTitle: Bool = false
    ) -> ControllerUIThemeReadabilityOverrides {
        ControllerUIThemeReadabilityOverrides(
            normal: primary,
            secondary: secondary,
            title: primary,
            unselected: secondary,
            focused: focused ?? primary,
            tabTitle: replacesWhiteTabTitle ? primary : nil,
            tabSubtitle: secondary,
            cardTitle: primary,
            cardSubtitle: secondary,
            contextPrimary: overlayPrimary,
            contextSecondary: overlaySecondary,
            quickMenuPrimary: overlayPrimary,
            quickMenuSecondary: overlaySecondary,
            quickMenuFocused: focused ?? overlayPrimary
        )
    }

    private func secondaryReadability(
        secondary: Color,
        overlayPrimary: Color,
        overlaySecondary: Color
    ) -> ControllerUIThemeReadabilityOverrides {
        ControllerUIThemeReadabilityOverrides(
            secondary: secondary,
            tabSubtitle: secondary,
            contextPrimary: overlayPrimary,
            contextSecondary: overlaySecondary,
            quickMenuPrimary: overlayPrimary,
            quickMenuSecondary: overlaySecondary,
            quickMenuFocused: overlayPrimary
        )
    }

    private func darkReadability(
        primary: Color,
        secondary: Color
    ) -> ControllerUIThemeReadabilityOverrides {
        ControllerUIThemeReadabilityOverrides(
            secondary: secondary,
            tabSubtitle: secondary,
            cardTitle: primary,
            cardSubtitle: secondary,
            contextPrimary: primary,
            contextSecondary: secondary,
            quickMenuPrimary: primary,
            quickMenuSecondary: secondary,
            quickMenuFocused: primary
        )
    }

    private func overlayReadability(
        primary: Color,
        secondary: Color
    ) -> ControllerUIThemeReadabilityOverrides {
        ControllerUIThemeReadabilityOverrides(
            contextPrimary: primary,
            contextSecondary: secondary,
            quickMenuPrimary: primary,
            quickMenuSecondary: secondary,
            quickMenuFocused: primary
        )
    }
}
