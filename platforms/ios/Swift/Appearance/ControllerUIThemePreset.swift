// ControllerUIThemePreset.swift — built-in interface themes
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ControllerUIThemeConfiguration {
    let accentPalette: ThemePalette
    let focusBoxStyle: ControllerFocusBoxStyle
    let focusBoxPalette: ThemePalette
    let orbPalette: ThemePalette
    let textPalette: ThemePalette?
    let secondaryTextPalette: ThemePalette?
    let focusedTextPalette: ThemePalette
    let criticalTextPalette: ThemePalette
    let textShadowPalette: ThemePalette
    let focusedTextShadowPalette: ThemePalette
    let textShadowStrength: Double
    let focusedTextShadowStrength: Double
    let animationSpeed: Double
    let glowIntensity: Double
}

struct ControllerUIDynamicThemeConfiguration {
    let background: DynamicBackgroundStyle
    let sharedPalette: ThemePalette
    let sharedCustomColor: SavedPaletteColor?
    let ribbonPalette: ThemePalette
    let ribbonCustomColor: SavedPaletteColor?
    let sharedMultiColorPalettes: [ThemePalette]
    let ribbonMultiColorPalettes: [ThemePalette]
    let animatesMultiColor: Bool
    let particleSettings: DynamicParticleSettings
}

/// Renderer-specific values carried by a theme preset. Optional fields leave
/// the established PS2/PS3 defaults intact, while each Y2K preset can still
/// shape the native scene without introducing another background engine.
struct ControllerUIDynamicThemeTuning {
    var gradientTilt: Double = 0
    var gradientOffsetX: Double = 0
    var gradientOffsetY: Double = 0
    var gradientWidth: Double = 1
    var gradientCurvature: Double = 0
    var ps2FramesPerSecond: Double? = nil
    var ps2YawSpeed: Double? = nil
    var ps2RollSpeed: Double? = nil
    var ps2OrbitDegrees: Double? = nil
    var ps2ColorsTowers: Bool? = nil
    var xmbParticleCount: Double? = nil
    var xmbParticleOpacity: Double? = nil
    var xmbParticleSize: Double? = nil
    var xmbFlowSpeed: Double? = nil
    var xmbParticleFlowSpeed: Double? = nil
    var xmbTension: Double? = nil
    var xmbDamping: Double? = nil
    var xmbBandAmplitude: Double? = nil
    var xmbBandSecondaryFrequency: Double? = nil
    var xmbBandSecondaryAmplitude: Double? = nil
    var xmbTravelSpeed1: Double? = nil
    var xmbTravelAmplitude1: Double? = nil
    var xmbTravelSpeed2: Double? = nil
    var xmbTravelAmplitude2: Double? = nil
    var xmbOpacity: Double? = nil
    var xmbBrightness: Double? = nil
}

/// Readability overrides for presets whose vivid palette is rendered over a
/// dark animated background. These are semantic UI roles, not one flat tint:
/// titles, ordinary content, secondary chrome, and focused labels retain
/// distinct contrast while staying in the preset's colour family.
struct ControllerUIThemeTextOverrides {
    let normal: Color
    let secondary: Color
    let focused: Color
    let title: Color
}

/// Foregrounds whose contrast depends on the surface underneath them. Library
/// cards sit over the preset's dynamic background, while Context Menu and
/// Quick Menu labels sit over Liquid Glass. Keeping these roles separate
/// prevents a colour chosen for a bright background from being reused on a
/// dark overlay (and vice versa).
struct ControllerUIThemeReadabilityOverrides {
    var normal: Color? = nil
    var secondary: Color? = nil
    var title: Color? = nil
    var unselected: Color? = nil
    var focused: Color? = nil
    var tabTitle: Color? = nil
    var tabSubtitle: Color? = nil
    var cardTitle: Color? = nil
    var cardSubtitle: Color? = nil
    var contextPrimary: Color
    var contextSecondary: Color
    var quickMenuPrimary: Color
    var quickMenuSecondary: Color
    var quickMenuFocused: Color
}

enum ControllerUIThemePreset: String, CaseIterable, Identifiable, Codable {
    case custom
    case defaultTheme
    case neon
    case purpleMilk
    case enderPearlPul
    case henyBlue
    case liveSilver
    case gold
    case diamond
    case galaxy
    case blackHole
    case sakura
    case cherry
    case bloodDragon
    case redBlood
    case cyberGreen
    case ocean
    case arctic
    case aurora
    case synthwave
    case plasma
    case ultraviolet
    case roseGold
    case emerald
    case obsidian
    case copper
    case classicPS1
    case classicPS2
    case sunset
    case midnight
    case royal
    case electricLime
    case frostedPearl
    case eclipse
    case rainbow
    case comet
    case scanline
    case minimal
    case pastelSakura
    case pastelLavender
    case pastelMint
    case pastelOcean
    case pastelSunrise
    case minimalSnow
    case minimalGraphite
    case minimalMidnight
    case minimalSand
    case minimalOcean
    case ambientSpectrum
    case lightSpeedNebula
    case spatialRetroGrid
    case orbitalTowers
    case faceButtonDream
    case portableGlass
    case splineFlow
    case particleWave
    case oceanWave
    case luminousRibbons
    case frutigerAero
    case frutigerEco
    case darkAero
    case technozen
    case dorfic
    case frutigerMetro
    case mcBling
    case vectorbloom
    case skeuomorphicAqua
    case y2kChrome
    case aquaWeb
    case bubblegumTech
    case digitalMeadow
    case glassGarden
    case zenGlass
    case metroNight

    var id: String { rawValue }

    var title: String {
        switch self {
        case .custom: return "Custom"
        case .defaultTheme: return "Default"
        case .neon: return "Neon"
        case .purpleMilk: return "Purple Milk"
        case .enderPearlPul: return "Ender Pearl-Pul"
        case .henyBlue: return "Heny Blue"
        case .liveSilver: return "Live Silver"
        case .gold: return "Gold"
        case .diamond: return "Diamond"
        case .galaxy: return "Galaxy"
        case .blackHole: return "Black Hole"
        case .sakura: return "Sakura"
        case .cherry: return "Cherry"
        case .bloodDragon: return "Blood Dragon"
        case .redBlood: return "Red Blood"
        case .cyberGreen: return "Cyber Green"
        case .ocean: return "Ocean"
        case .arctic: return "Arctic"
        case .aurora: return "Aurora"
        case .synthwave: return "Synthwave"
        case .plasma: return "Plasma"
        case .ultraviolet: return "Ultraviolet"
        case .roseGold: return "Rose Gold"
        case .emerald: return "Emerald"
        case .obsidian: return "Obsidian"
        case .copper: return "Copper"
        case .classicPS1: return "Classic Grey"
        case .classicPS2: return "Classic Brown"
        case .sunset: return "Sunset"
        case .midnight: return "Midnight"
        case .royal: return "Royal"
        case .electricLime: return "Electric Lime"
        case .frostedPearl: return "Frosted Pearl"
        case .eclipse: return "Eclipse"
        case .rainbow: return "Rainbow"
        case .comet: return "Comet"
        case .scanline: return "Retro Scanline"
        case .minimal: return "Minimal"
        case .pastelSakura: return "Pastel Sakura"
        case .pastelLavender: return "Pastel Lavender"
        case .pastelMint: return "Pastel Mint"
        case .pastelOcean: return "Pastel Ocean"
        case .pastelSunrise: return "Pastel Sunrise"
        case .minimalSnow: return "Minimalist Snow"
        case .minimalGraphite: return "Minimalist Graphite"
        case .minimalMidnight: return "Minimalist Midnight"
        case .minimalSand: return "Minimalist Sand"
        case .minimalOcean: return "Minimalist Ocean"
        case .ambientSpectrum: return "Ambient Spectrum"
        case .lightSpeedNebula: return "Light-Speed Nebula"
        case .spatialRetroGrid: return "Spatial Retro Grid"
        case .orbitalTowers: return "Orbital Towers"
        case .faceButtonDream: return "Face Button Dream"
        case .portableGlass: return "Portable Glass"
        case .splineFlow: return "Spline Flow"
        case .particleWave: return "Particle Wave"
        case .oceanWave: return "Ocean Wave"
        case .luminousRibbons: return "Luminous Ribbons"
        case .frutigerAero: return "Frutiger Aero"
        case .frutigerEco: return "Frutiger Eco"
        case .darkAero: return "Dark Aero"
        case .technozen: return "Technozen"
        case .dorfic: return "DORFic"
        case .frutigerMetro: return "Frutiger Metro"
        case .mcBling: return "McBling"
        case .vectorbloom: return "Vectorbloom"
        case .skeuomorphicAqua: return "Skeuomorphic Aqua"
        case .y2kChrome: return "Y2K Chrome"
        case .aquaWeb: return "Aqua Web"
        case .bubblegumTech: return "Bubblegum Tech"
        case .digitalMeadow: return "Digital Meadow"
        case .glassGarden: return "Glass Garden"
        case .zenGlass: return "Zen Glass"
        case .metroNight: return "Metro Night"
        }
    }

    func rgb(_ hex: UInt32) -> Color {
        Color(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255
        )
    }
}
