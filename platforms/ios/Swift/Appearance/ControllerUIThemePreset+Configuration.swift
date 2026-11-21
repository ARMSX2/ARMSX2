// ControllerUIThemePreset+Configuration.swift — per-theme focus box and palette configuration
// SPDX-License-Identifier: GPL-3.0+

extension ControllerUIThemePreset {
    var configuration: ControllerUIThemeConfiguration? {
        switch self {
        case .custom:
            return nil
        case .defaultTheme:
            // Default mirrors Heny Blue's complete interface configuration;
            // only its dynamic wallpaper differs.
            return theme(.henyBlue, .neonBlue, .henyBlue, .henyBlue, 1, 1)
        case .neon:
            return theme(.blue, .neonBlue, .blue, .blue, 1, 1)
        case .purpleMilk:
            return theme(
                .henyBlue,
                .animatedWaves,
                .aurora,
                .platinumGrey,
                0.5,
                0.85
            )
        case .enderPearlPul:
            // The bundled transfer document is authoritative. This fallback
            // keeps the preset usable if its resource cannot be decoded.
            return theme(
                .xmbPurpleEclipse,
                .glowingCorners,
                .aurora,
                .aurora,
                0.7,
                1.45
            )
        case .henyBlue:
            return theme(.henyBlue, .neonBlue, .henyBlue, .henyBlue, 1, 1)
        case .liveSilver:
            return theme(.silver, .softGlow, .platinumGrey, .silver, 0.8, 1.2)
        case .gold:
            return theme(.gold, .doubleNeon, .gold, .gold, 0.9, 1.3)
        case .diamond:
            return theme(.arcticIce, .prism, .platinumGrey, .arcticIce, 0.8, 1.45)
        case .galaxy:
            return theme(.violet, .animatedGradient, .multicolor, .xmbAmethystGlow, 0.7, 1.35)
        case .blackHole:
            return theme(.obsidian, .scanline, .obsidian, .xmbLunarGraphite, 0.55, 1.55)
        case .sakura:
            return theme(.pink, .pinkSakura, .xmbSakuraBloom, .xmbSakuraBloom, 0.75, 1.15)
        case .cherry:
            return theme(.hotMagenta, .dottedPulse, .xmbWinterCoral, .hotMagenta, 1.05, 1.25)
        case .bloodDragon:
            return theme(.crimson, .redBlood, .xmbCinderRed, .xmbCinderRed, 1.2, 1.55)
        case .redBlood:
            return theme(.crimson, .redBlood, .burgundy, .crimson, 0.9, 1.4)
        case .cyberGreen:
            return theme(.neonGreen, .scanline, .neonGreen, .electricLime, 1.25, 1.45)
        case .ocean:
            return theme(.cyan, .oceanBlue, .xmbOceanMidnight, .xmbTurquoiseLagoon, 0.7, 1.2)
        case .arctic:
            return theme(.arcticIce, .stillWaves, .arcticIce, .platinumGrey, 0.65, 1.15)
        case .aurora:
            return theme(.aurora, .animatedGradient, .aurora, .aurora, 0.75, 1.35)
        case .synthwave:
            return theme(.hotMagenta, .animatedWaves, .multicolor, .violet, 1.15, 1.4)
        case .plasma:
            return theme(.plasmaOrange, .cometTrail, .plasmaOrange, .copper, 1.3, 1.45)
        case .ultraviolet:
            return theme(.violet, .rainbowNeon, .xmbAmethystGlow, .xmbPurpleEclipse, 0.85, 1.3)
        case .roseGold:
            return theme(.pink, .doubleNeon, .xmbSakuraBloom, .gold, 0.8, 1.25)
        case .emerald:
            return theme(.emerald, .stillWaves, .emerald, .deepTeal, 0.8, 1.2)
        case .obsidian:
            return theme(.gunmetalGrey, .softGlow, .obsidian, .gunmetalGrey, 0.6, 1.35)
        case .copper:
            return theme(.copper, .classicFrame, .copper, .desertSand, 0.7, 1.15)
        case .classicPS1:
            return theme(.ps1Grey, .minimal, .ps1Grey, .platinumGrey, 0.5, 0.85)
        case .classicPS2:
            return theme(.ps2Brown, .classicFrame, .ps2Brown, .gunmetalGrey, 0.5, 0.95)
        case .sunset:
            return theme(.sunset, .animatedGradient, .sunset, .plasmaOrange, 0.75, 1.25)
        case .midnight:
            return theme(.midnight, .softGlow, .midnight, .navy, 0.65, 1.3)
        case .royal:
            return theme(.violet, .doubleNeon, .violet, .gold, 0.8, 1.35)
        case .electricLime:
            return theme(.electricLime, .dottedPulse, .electricLime, .neonGreen, 1.1, 1.3)
        case .frostedPearl:
            return theme(.xmbFrostedPearl, .prism, .xmbFrostedPearl, .silver, 0.65, 1.1)
        case .eclipse:
            return theme(.xmbPurpleEclipse, .glowingCorners, .xmbPurpleEclipse, .obsidian, 0.7, 1.45)
        case .rainbow:
            return theme(.cyan, .animatedGlowingRainbowNeon, .multicolor, .multicolor, 0.9, 1.4)
        case .comet:
            return theme(.arcticIce, .cometTrail, .aurora, .arcticIce, 1.25, 1.35)
        case .scanline:
            return theme(.emerald, .scanline, .graphite, .neonGreen, 1, 1.1)
        case .minimal:
            return theme(.silver, .minimal, .silver, .silver, 0.5, 0.65)
        case .pastelSakura:
            return theme(.xmbSakuraBloom, .softGlow, .pink, .xmbSakuraBloom, 0.55, 0.82)
        case .pastelLavender:
            return theme(.lavender, .softGlow, .xmbOrchidHaze, .lavender, 0.52, 0.8)
        case .pastelMint:
            return theme(.xmbSpringMeadow, .stillWaves, .emerald, .xmbSpringMeadow, 0.5, 0.76)
        case .pastelOcean:
            return theme(.xmbTurquoiseLagoon, .stillWaves, .arcticIce, .cyan, 0.55, 0.82)
        case .pastelSunrise:
            return theme(.desertSand, .gradient, .xmbWinterCoral, .gold, 0.5, 0.78)
        case .minimalSnow:
            return theme(.xmbFrostedPearl, .minimal, .silver, .xmbFrostedPearl, 0.38, 0.52)
        case .minimalGraphite:
            return theme(.graphite, .minimal, .platinumGrey, .graphite, 0.4, 0.5)
        case .minimalMidnight:
            return theme(.midnight, .minimal, .blue, .navy, 0.38, 0.54)
        case .minimalSand:
            return theme(.desertSand, .minimal, .copper, .desertSand, 0.36, 0.5)
        case .minimalOcean:
            return theme(.deepTeal, .minimal, .cyan, .navy, 0.4, 0.54)
        case .ambientSpectrum:
            return theme(.violet, .animatedGradient, .multicolor, .aurora, 0.82, 1.15)
        case .lightSpeedNebula:
            return theme(.cyan, .cometTrail, .aurora, .arcticIce, 1.2, 1.28)
        case .spatialRetroGrid:
            return theme(.ps1Grey, .classicFrame, .platinumGrey, .ps1Grey, 0.62, 0.82)
        case .orbitalTowers:
            return theme(.blue, .doubleNeon, .cyan, .blue, 0.72, 1.08)
        case .faceButtonDream:
            return theme(.blue, .dottedPulse, .cyan, .blue, 0.88, 1.08)
        case .portableGlass:
            return theme(.arcticIce, .stillWaves, .xmbFrostedPearl, .cyan, 0.56, 0.82)
        case .splineFlow:
            return theme(.violet, .animatedWaves, .xmbAmethystGlow, .violet, 0.72, 1.04)
        case .particleWave:
            return theme(.blue, .particles, .cyan, .arcticIce, 0.88, 1.12)
        case .oceanWave:
            return theme(.cyan, .oceanBlue, .xmbTurquoiseLagoon, .blue, 0.68, 1.0)
        case .luminousRibbons:
            return theme(.gold, .animatedGradient, .xmbGildedSun, .gold, 0.7, 1.08)
        case .frutigerAero:
            return theme(.cyan, .aquaBubble, .arcticIce, .xmbSpringMeadow, 0.72, 1.18)
        case .frutigerEco:
            return theme(.emerald, .ecoPulse, .neonGreen, .xmbSpringMeadow, 0.62, 1.02)
        case .darkAero:
            return theme(.cyan, .darkAeroOrbit, .xmbTurquoiseLagoon, .midnight, 0.68, 1.30)
        case .technozen:
            return theme(.deepTeal, .zenBreath, .xmbSpringMeadow, .silver, 0.42, 0.78)
        case .dorfic:
            return theme(.copper, .rusticStitch, .desertSand, .ps2Brown, 0.48, 0.82)
        case .frutigerMetro:
            return theme(.blue, .metroSweep, .hotMagenta, .gold, 0.88, 1.08)
        case .mcBling:
            return theme(.hotMagenta, .rhinestone, .pink, .gold, 0.92, 1.24)
        case .vectorbloom:
            return theme(.pink, .vectorBloom, .xmbSpringMeadow, .hotMagenta, 0.68, 1.02)
        case .skeuomorphicAqua:
            return theme(.arcticIce, .skeuomorphicBevel, .cyan, .blue, 0.50, 0.96)
        case .y2kChrome:
            return theme(.silver, .skeuomorphicBevel, .platinumGrey, .arcticIce, 0.44, 0.92)
        case .aquaWeb:
            return theme(.xmbAzureHorizon, .aquaBubble, .cyan, .blue, 0.82, 1.12)
        case .bubblegumTech:
            return theme(.pink, .rhinestone, .hotMagenta, .xmbSakuraBloom, 0.84, 1.16)
        case .digitalMeadow:
            return theme(.xmbSpringMeadow, .ecoPulse, .emerald, .electricLime, 0.74, 1.08)
        case .glassGarden:
            return theme(.emerald, .vectorBloom, .xmbSpringMeadow, .cyan, 0.56, 0.98)
        case .zenGlass:
            return theme(.silver, .zenBreath, .deepTeal, .arcticIce, 0.36, 0.70)
        case .metroNight:
            return theme(.violet, .metroSweep, .cyan, .hotMagenta, 0.96, 1.18)
        }
    }

    private func theme(
        _ accent: ThemePalette,
        _ style: ControllerFocusBoxStyle,
        _ focusBox: ThemePalette,
        _ orbs: ThemePalette,
        _ speed: Double,
        _ glow: Double
    ) -> ControllerUIThemeConfiguration {
        let usesDarkText = accent.prefersDarkInterfaceText
        return ControllerUIThemeConfiguration(
            accentPalette: accent,
            focusBoxStyle: style,
            focusBoxPalette: focusBox,
            orbPalette: orbs,
            textPalette: accent,
            secondaryTextPalette: accent.prefersDarkInterfaceText
                ? accent
                : .silver,
            focusedTextPalette: focusBox,
            criticalTextPalette: criticalTextPalette,
            textShadowPalette: usesDarkText ? .silver : .obsidian,
            focusedTextShadowPalette:
                focusBox.prefersDarkInterfaceText ? .silver : .obsidian,
            textShadowStrength: 0.2,
            focusedTextShadowStrength: 0.35,
            animationSpeed: speed,
            glowIntensity: glow
        )
    }

    /// Critical actions remain semantically distinct from each preset's
    /// ordinary text. Red-heavy presets use gold so warnings do not disappear
    /// into their background; every other preset, including Cyber Green, uses
    /// a true red critical role.
    private var criticalTextPalette: ThemePalette {
        switch self {
        case .bloodDragon, .redBlood, .cherry:
            return .gold
        default:
            return .crimson
        }
    }
}
