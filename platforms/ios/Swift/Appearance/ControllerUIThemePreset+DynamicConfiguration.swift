// ControllerUIThemePreset+DynamicConfiguration.swift — per-theme dynamic background configuration
// SPDX-License-Identifier: GPL-3.0+

extension ControllerUIThemePreset {
    var dynamicConfiguration: ControllerUIDynamicThemeConfiguration? {
        switch self {
        case .custom:
            return nil
        case .defaultTheme:
            // Keep the stock preset on the standard Blue/Cyan dynamic
            // palettes. Heny Blue remains the opt-in custom-gradient variant.
            return dynamicTheme(
                .playStation3XMBByMart,
                .blue,
                .cyan,
                dark: 0.28,
                particles: true,
                style: .xmb3,
                amount: 0.8,
                speed: 1.1,
                brightness: 0.8,
                motion: 1.05,
                armsx2Logo: true
            )
        case .neon:
            return dynamicTheme(.playStation2Menu, .blue, .cyan, dark: 0.85, style: .xmb3, amount: 0.8, speed: 1.1, brightness: 0.8, motion: 1.05)
        case .purpleMilk:
            // The bundled complete snapshot is authoritative. This compact
            // fallback is used only if that resource cannot be decoded.
            return dynamicTheme(
                .playStation3XMBByMart,
                .lavender,
                .xmbOrchidHaze,
                ribbonMulti: [
                    .xmbOrchidHaze,
                    .violet,
                    .lavender,
                    .xmbAzureHorizon,
                ],
                animated: false,
                dark: 0,
                particles: false,
                style: .ps1Dust,
                amount: 0.45,
                speed: 0.4,
                brightness: 0.7,
                motion: 0.55
            )
        case .enderPearlPul:
            // The bundled complete snapshot is authoritative.
            return dynamicTheme(
                .playStation3XMBByMart,
                .xmbPurpleEclipse,
                .obsidian,
                sharedMulti: [.xmbPurpleEclipse, .obsidian, .violet],
                ribbonMulti: [.violet, .obsidian],
                animated: true,
                dark: 0,
                particles: true,
                style: .mixed,
                amount: 1,
                speed: 0.55,
                brightness: 0.65,
                motion: 0.58
            )
        case .henyBlue:
            return dynamicTheme(
                .playStation2Menu,
                .henyBlue,
                .cyan,
                sharedGradientHexes: [0x146BFF, 0x29E0EB, 0xBD33F0],
                dark: 0.28,
                particles: true,
                style: .xmb3,
                amount: 0.8,
                speed: 1.1,
                brightness: 0.8,
                motion: 1.05,
                tuning: ControllerUIDynamicThemeTuning(
                    ps2FramesPerSecond: 15
                )
            )
        case .liveSilver:
            return dynamicTheme(.playStation3XMBByMart, .silver, .platinumGrey, dark: 0.55, style: .xmbMart, amount: 0.65, speed: 0.75, brightness: 0.92, motion: 0.8)
        case .gold:
            return dynamicTheme(.playStation2Menu, .gold, .xmbHarvestGold, dark: 0.75, style: .ps4Glow, amount: 0.8, speed: 0.8, brightness: 0.9, motion: 0.85)
        case .diamond:
            return dynamicTheme(.playStation3XMBByMart, .arcticIce, .xmbFrostedPearl, sharedMulti: [.arcticIce, .silver, .cyan], ribbonMulti: [.xmbFrostedPearl, .platinumGrey], animated: true, dark: 0.35, style: .xmbMart, amount: 0.9, speed: 0.7, brightness: 1.0, motion: 0.72)
        case .galaxy:
            return dynamicTheme(.playStation3XMBByMart, .violet, .xmbAmethystGlow, sharedMulti: [.midnight, .violet, .xmbPurpleEclipse, .cyan], ribbonMulti: [.violet, .hotMagenta, .cyan], animated: true, dark: 1.35, style: .mixed, amount: 1.3, speed: 0.65, brightness: 0.88, motion: 0.68)
        case .blackHole:
            return dynamicTheme(.playStation2Menu, .obsidian, .xmbLunarGraphite, dark: 1.8, style: .ps1Dust, amount: 0.7, speed: 0.45, brightness: 0.48, motion: 0.5)
        case .sakura:
            return dynamicTheme(.playStation3XMBByMart, .xmbSakuraBloom, .pink, sharedMulti: [.xmbSakuraBloom, .pink, .xmbRoseTwilight], ribbonMulti: [.pink, .silver], animated: true, dark: 0.55, style: .xmbMart, amount: 0.8, speed: 0.65, brightness: 0.95, motion: 0.72)
        case .cherry:
            return dynamicTheme(.playStation2Menu, .xmbWinterCoral, .hotMagenta, dark: 0.9, style: .ps4Glow, amount: 0.95, speed: 0.95, brightness: 0.9, motion: 0.95)
        case .bloodDragon:
            return dynamicTheme(.playStation2Menu, .xmbCinderRed, .crimson, sharedMulti: [.xmbCinderRed, .crimson, .obsidian], ribbonMulti: [.crimson, .burgundy], animated: true, dark: 1.55, style: .mixed, amount: 1.2, speed: 1.15, brightness: 0.72, motion: 1.15)
        case .redBlood:
            return dynamicTheme(.playStation2Menu, .burgundy, .crimson, dark: 1.35, style: .ps4Glow, amount: 1.0, speed: 0.8, brightness: 0.7, motion: 0.82)
        case .cyberGreen:
            return dynamicTheme(.playStation2Menu, .neonGreen, .electricLime, dark: 1.0, style: .xmb3, amount: 1.1, speed: 1.25, brightness: 0.95, motion: 1.2)
        case .ocean:
            return dynamicTheme(.playStation3XMBByMart, .xmbOceanMidnight, .xmbTurquoiseLagoon, sharedMulti: [.xmbOceanMidnight, .blue, .cyan], ribbonMulti: [.xmbTurquoiseLagoon, .cyan], animated: true, dark: 1.0, style: .xmbMart, amount: 0.8, speed: 0.55, brightness: 0.82, motion: 0.62)
        case .arctic:
            return dynamicTheme(.playStation3XMBByMart, .arcticIce, .platinumGrey, dark: 0.25, style: .xmbMart, amount: 0.65, speed: 0.45, brightness: 1.0, motion: 0.55)
        case .aurora:
            return dynamicTheme(.playStation3XMBByMart, .aurora, .cyan, sharedMulti: [.emerald, .cyan, .violet, .hotMagenta], ribbonMulti: [.cyan, .violet, .emerald], animated: true, dark: 0.75, style: .mixed, amount: 1.1, speed: 0.72, brightness: 0.9, motion: 0.75)
        case .synthwave:
            return dynamicTheme(.playStation2Menu, .hotMagenta, .violet, sharedMulti: [.hotMagenta, .violet, .cyan], ribbonMulti: [.cyan, .hotMagenta], animated: true, dark: 1.1, style: .xmb3, amount: 1.15, speed: 1.1, brightness: 0.92, motion: 1.15)
        case .plasma:
            return dynamicTheme(.playStation2Menu, .plasmaOrange, .copper, sharedMulti: [.plasmaOrange, .gold, .crimson], ribbonMulti: [.plasmaOrange, .gold], animated: true, dark: 0.8, style: .ps4Glow, amount: 1.25, speed: 1.35, brightness: 1.0, motion: 1.3)
        case .ultraviolet:
            return dynamicTheme(.playStation3XMBByMart, .xmbAmethystGlow, .xmbPurpleEclipse, dark: 1.2, style: .xmbMart, amount: 0.9, speed: 0.72, brightness: 0.86, motion: 0.7)
        case .roseGold:
            return dynamicTheme(.playStation3XMBByMart, .xmbSakuraBloom, .gold, sharedMulti: [.xmbSakuraBloom, .pink, .gold], ribbonMulti: [.gold, .pink], animated: false, dark: 0.55, style: .ps5Drift, amount: 0.75, speed: 0.62, brightness: 0.94, motion: 0.7)
        case .emerald:
            return dynamicTheme(.playStation3XMBByMart, .emerald, .deepTeal, dark: 0.85, style: .xmbMart, amount: 0.8, speed: 0.68, brightness: 0.85, motion: 0.68)
        case .obsidian:
            return dynamicTheme(.playStation2Menu, .obsidian, .gunmetalGrey, dark: 1.7, style: .ps1Dust, amount: 0.55, speed: 0.42, brightness: 0.5, motion: 0.48)
        case .copper:
            return dynamicTheme(.playStation2Menu, .copper, .desertSand, dark: 0.95, style: .ps1Dust, amount: 0.7, speed: 0.58, brightness: 0.75, motion: 0.62)
        case .classicPS1:
            return dynamicTheme(.playStation3XMBByMart, .ps1Grey, .platinumGrey, dark: 0.65, particles: false, style: .ps1Dust, amount: 0.45, speed: 0.4, brightness: 0.7, motion: 0.55)
        case .classicPS2:
            return dynamicTheme(.playStation2Menu, .ps2Brown, .gunmetalGrey, dark: 1.2, particles: false, style: .ps1Dust, amount: 0.5, speed: 0.5, brightness: 0.65, motion: 0.6)
        case .sunset:
            return dynamicTheme(.playStation2Menu, .sunset, .plasmaOrange, sharedMulti: [.violet, .hotMagenta, .plasmaOrange, .gold], ribbonMulti: [.plasmaOrange, .pink], animated: true, dark: 0.7, style: .ps5Drift, amount: 0.9, speed: 0.72, brightness: 0.92, motion: 0.76)
        case .midnight:
            return dynamicTheme(.playStation3XMBByMart, .midnight, .navy, dark: 1.55, style: .xmbMart, amount: 0.7, speed: 0.45, brightness: 0.58, motion: 0.5)
        case .royal:
            return dynamicTheme(.playStation2Menu, .violet, .gold, sharedMulti: [.violet, .midnight, .gold], ribbonMulti: [.gold, .violet], animated: false, dark: 1.05, style: .ps5Drift, amount: 0.8, speed: 0.72, brightness: 0.86, motion: 0.72)
        case .electricLime:
            return dynamicTheme(.playStation2Menu, .electricLime, .neonGreen, dark: 0.85, style: .xmb3, amount: 1.0, speed: 1.15, brightness: 0.95, motion: 1.1)
        case .frostedPearl:
            return dynamicTheme(.playStation3XMBByMart, .xmbFrostedPearl, .silver, dark: 0.2, style: .xmbMart, amount: 0.55, speed: 0.4, brightness: 1.0, motion: 0.5)
        case .eclipse:
            return dynamicTheme(.playStation3XMBByMart, .xmbPurpleEclipse, .obsidian, sharedMulti: [.xmbPurpleEclipse, .obsidian, .violet], ribbonMulti: [.violet, .obsidian], animated: true, dark: 1.65, style: .mixed, amount: 1.0, speed: 0.55, brightness: 0.65, motion: 0.58)
        case .rainbow:
            return dynamicTheme(.playStation3XMBByMart, .multicolor, .multicolor, sharedMulti: [.crimson, .gold, .neonGreen, .cyan, .blue, .violet, .hotMagenta], ribbonMulti: [.cyan, .violet, .pink, .gold], animated: true, dark: 0.55, style: .mixed, amount: 1.2, speed: 0.95, brightness: 1.0, motion: 0.9)
        case .comet:
            return dynamicTheme(.playStation3XMBByMart, .aurora, .arcticIce, sharedMulti: [.midnight, .violet, .cyan, .arcticIce], ribbonMulti: [.arcticIce, .cyan], animated: true, dark: 1.0, style: .ps5Drift, amount: 1.3, speed: 1.35, brightness: 1.0, motion: 1.2)
        case .scanline:
            return dynamicTheme(.playStation2Menu, .graphite, .neonGreen, dark: 1.25, particles: false, style: .xmb3, amount: 0.4, speed: 0.55, brightness: 0.72, motion: 0.68)
        case .minimal:
            return dynamicTheme(.playStation3XMBByMart, .silver, .silver, dark: 0.4, particles: false, style: .xmbMart, amount: 0.4, speed: 0.35, brightness: 0.82, motion: 0.45)
        case .pastelSakura:
            return dynamicTheme(.multicolorAmbient, .xmbSakuraBloom, .pink, sharedMulti: [.xmbSakuraBloom, .lavender, .pink], ribbonMulti: [.pink, .xmbFrostedPearl], animated: true, dark: 0.28, particles: false, style: .ps5Drift, amount: 0.42, speed: 0.38, brightness: 0.9, motion: 0.42)
        case .pastelLavender:
            return dynamicTheme(.playStationPortableBlur, .lavender, .xmbOrchidHaze, sharedMulti: [.lavender, .xmbOrchidHaze, .xmbFrostedPearl], ribbonMulti: [.lavender, .silver], animated: false, dark: 0.3, particles: false, style: .xmb3, amount: 0.38, speed: 0.34, brightness: 0.88, motion: 0.4)
        case .pastelMint:
            return dynamicTheme(.playStation4Waves, .xmbSpringMeadow, .xmbTurquoiseLagoon, sharedMulti: [.xmbSpringMeadow, .arcticIce], ribbonMulti: [.xmbTurquoiseLagoon, .xmbFrostedPearl], animated: false, dark: 0.32, particles: false, style: .ps4Glow, amount: 0.4, speed: 0.36, brightness: 0.9, motion: 0.42)
        case .pastelOcean:
            return dynamicTheme(.playStation3Splines, .arcticIce, .xmbTurquoiseLagoon, sharedMulti: [.arcticIce, .xmbAzureHorizon], ribbonMulti: [.xmbTurquoiseLagoon, .cyan], animated: false, dark: 0.3, particles: false, style: .xmb3, amount: 0.42, speed: 0.34, brightness: 0.92, motion: 0.4)
        case .pastelSunrise:
            return dynamicTheme(.playStationRibbons, .desertSand, .xmbWinterCoral, sharedMulti: [.desertSand, .xmbSakuraBloom], ribbonMulti: [.xmbWinterCoral, .gold], animated: false, dark: 0.3, particles: false, style: .ps5Drift, amount: 0.4, speed: 0.34, brightness: 0.9, motion: 0.4)
        case .minimalSnow:
            return dynamicTheme(.playStationPortableBlur, .xmbFrostedPearl, .silver, dark: 0.18, particles: false, style: .xmb3, amount: 0.24, speed: 0.25, brightness: 0.78, motion: 0.3)
        case .minimalGraphite:
            return dynamicTheme(.spatialRetro, .graphite, .platinumGrey, dark: 1.05, particles: false, style: .ps1Dust, amount: 0.2, speed: 0.24, brightness: 0.55, motion: 0.28)
        case .minimalMidnight:
            return dynamicTheme(.playStation3Splines, .midnight, .blue, dark: 1.35, particles: false, style: .xmb3, amount: 0.22, speed: 0.24, brightness: 0.52, motion: 0.28)
        case .minimalSand:
            return dynamicTheme(.towersOrbs, .ps2Brown, .desertSand, dark: 1.0, particles: false, style: .ps1Dust, amount: 0.2, speed: 0.22, brightness: 0.56, motion: 0.26)
        case .minimalOcean:
            return dynamicTheme(.playStation4Waves, .navy, .deepTeal, dark: 1.2, particles: false, style: .ps4Glow, amount: 0.22, speed: 0.24, brightness: 0.58, motion: 0.28)
        case .ambientSpectrum:
            return dynamicTheme(.multicolorAmbient, .aurora, .multicolor, sharedMulti: [.violet, .cyan, .hotMagenta, .emerald], ribbonMulti: [.cyan, .pink, .gold], animated: true, dark: 0.62, style: .mixed, amount: 0.75, speed: 0.68, brightness: 0.86, motion: 0.72)
        case .lightSpeedNebula:
            return dynamicTheme(.lightSpeed, .midnight, .aurora, sharedMulti: [.midnight, .violet, .cyan], ribbonMulti: [.arcticIce, .cyan, .violet], animated: true, dark: 1.2, particles: false, style: .ps5Drift, amount: 1.0, speed: 1.15, brightness: 0.94, motion: 1.2)
        case .spatialRetroGrid:
            return dynamicTheme(.spatialRetro, .ps1Grey, .platinumGrey, dark: 0.86, particles: false, style: .ps1Dust, amount: 0.5, speed: 0.48, brightness: 0.7, motion: 0.58)
        case .orbitalTowers:
            return dynamicTheme(.towersOrbs, .midnight, .cyan, sharedMulti: [.midnight, .blue, .violet], ribbonMulti: [.cyan, .blue], animated: true, dark: 1.1, particles: false, style: .ps4Glow, amount: 0.58, speed: 0.55, brightness: 0.8, motion: 0.62)
        case .faceButtonDream:
            return dynamicTheme(.faceButtons, .blue, .cyan, sharedMulti: [.blue, .navy, .cyan], ribbonMulti: [.blue, .cyan, .arcticIce], animated: true, dark: 0.86, particles: false, style: .mixed, amount: 0.75, speed: 0.7, brightness: 0.86, motion: 0.76)
        case .portableGlass:
            return dynamicTheme(.playStationPortableBlur, .xmbAzureHorizon, .xmbFrostedPearl, sharedMulti: [.xmbAzureHorizon, .arcticIce], ribbonMulti: [.xmbFrostedPearl, .cyan], animated: false, dark: 0.55, particles: false, style: .xmb3, amount: 0.48, speed: 0.42, brightness: 0.88, motion: 0.5)
        case .splineFlow:
            return dynamicTheme(.playStation3Splines, .xmbPurpleEclipse, .xmbAmethystGlow, sharedMulti: [.xmbPurpleEclipse, .violet], ribbonMulti: [.xmbAmethystGlow, .cyan], animated: true, dark: 1.15, particles: false, style: .xmb3, amount: 0.7, speed: 0.58, brightness: 0.84, motion: 0.66)
        case .particleWave:
            return dynamicTheme(.playStation4Particles, .navy, .cyan, sharedMulti: [.navy, .blue], ribbonMulti: [.cyan, .arcticIce], animated: false, dark: 1.0, particles: false, style: .ps4Glow, amount: 0.82, speed: 0.72, brightness: 0.9, motion: 0.76)
        case .oceanWave:
            return dynamicTheme(.playStation4Waves, .xmbOceanMidnight, .xmbTurquoiseLagoon, sharedMulti: [.xmbOceanMidnight, .deepTeal], ribbonMulti: [.xmbTurquoiseLagoon, .cyan], animated: true, dark: 0.9, particles: false, style: .ps4Glow, amount: 0.62, speed: 0.48, brightness: 0.86, motion: 0.56)
        case .luminousRibbons:
            return dynamicTheme(.playStationRibbons, .xmbAntiqueDusk, .xmbGildedSun, sharedMulti: [.xmbAntiqueDusk, .ps2Brown], ribbonMulti: [.xmbGildedSun, .gold], animated: true, dark: 0.92, particles: false, style: .ps5Drift, amount: 0.7, speed: 0.55, brightness: 0.88, motion: 0.64)
        case .frutigerAero:
            return dynamicTheme(
                .playStation3XMBByMart, .arcticIce, .xmbSpringMeadow,
                sharedGradientHexes: [0x58BFFF, 0xBDEFFF, 0x72DD67, 0xF7F5A7],
                ribbonGradientHexes: [0xFFFFFF, 0x5CE49A, 0x2A91FF],
                dark: 0.12, style: .ps4Glow, amount: 0.58, speed: 0.62,
                brightness: 1.0, motion: 0.72,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -12, gradientOffsetY: -0.04, gradientWidth: 1.25,
                    gradientCurvature: 0.25, xmbParticleCount: 720,
                    xmbParticleOpacity: 0.62, xmbParticleSize: 8, xmbFlowSpeed: 0.13,
                    xmbParticleFlowSpeed: 0.11, xmbTension: 0.10, xmbDamping: 0.00015,
                    xmbBandAmplitude: 0.24, xmbBandSecondaryFrequency: 6,
                    xmbBandSecondaryAmplitude: 0.035, xmbTravelSpeed1: 0.20,
                    xmbTravelAmplitude1: 0.018, xmbTravelSpeed2: 0.12,
                    xmbTravelAmplitude2: 0.010, xmbOpacity: 0.76, xmbBrightness: 0.48
                )
            )
        case .frutigerEco:
            return dynamicTheme(
                .playStation2Menu, .xmbSpringMeadow, .emerald,
                sharedMulti: [.xmbSpringMeadow, .emerald, .electricLime],
                ribbonMulti: [.emerald, .neonGreen], animated: true,
                dark: 0.16, style: .ps5Drift, amount: 0.48, speed: 0.48,
                brightness: 0.92, motion: 0.58,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -10, gradientOffsetY: 0.08, gradientWidth: 1.30,
                    gradientCurvature: 0.25, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 1.1, ps2RollSpeed: 2.2, ps2OrbitDegrees: 18,
                    ps2ColorsTowers: true
                )
            )
        case .darkAero:
            return dynamicTheme(
                .playStation3XMBByMart, .midnight, .xmbTurquoiseLagoon,
                sharedGradientHexes: [0x020A1C, 0x052A43, 0x075B69, 0x02040E],
                ribbonGradientHexes: [0x39D7E8, 0x86F6FF, 0x296A8F],
                dark: 1.35, style: .xmb3, amount: 0.72, speed: 0.54,
                brightness: 0.82, motion: 0.64,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 18, gradientOffsetX: 0.08, gradientWidth: 1.15,
                    gradientCurvature: -0.18, xmbParticleCount: 520,
                    xmbParticleOpacity: 0.68, xmbParticleSize: 9, xmbFlowSpeed: 0.11,
                    xmbParticleFlowSpeed: 0.09, xmbTension: 0.16,
                    xmbBandAmplitude: 0.16, xmbOpacity: 0.62, xmbBrightness: 0.30
                )
            )
        case .technozen:
            return dynamicTheme(
                .playStation3XMBByMart, .arcticIce, .deepTeal,
                sharedGradientHexes: [0xD7F3ED, 0xA8DAD1, 0x6FA7A2, 0xEAF7F3],
                ribbonGradientHexes: [0x2E817E, 0x78C9BB, 0xE7FCF6],
                dark: 0.22, particles: false, style: .ps5Drift, amount: 0.30,
                speed: 0.34, brightness: 0.82, motion: 0.40,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -4, gradientOffsetY: 0.03, gradientWidth: 1.40,
                    gradientCurvature: 0.36, xmbParticleCount: 240,
                    xmbParticleOpacity: 0.34, xmbParticleSize: 7, xmbFlowSpeed: 0.06,
                    xmbParticleFlowSpeed: 0.05, xmbTension: 0.07,
                    xmbBandAmplitude: 0.11, xmbOpacity: 0.50, xmbBrightness: 0.38
                )
            )
        case .dorfic:
            return dynamicTheme(
                .playStation2Menu, .desertSand, .copper,
                sharedGradientHexes: [0xE5C99B, 0xB98A58, 0x73503A, 0xDDBF8A],
                ribbonGradientHexes: [0x6E8C4A, 0xC6773B, 0xF0D59C],
                dark: 0.34, particles: false, style: .ps1Dust, amount: 0.32,
                speed: 0.34, brightness: 0.76, motion: 0.42,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 12, gradientOffsetX: -0.06, gradientWidth: 1.15,
                    gradientCurvature: 0.10, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 0.6, ps2RollSpeed: 1.2, ps2OrbitDegrees: 12,
                    ps2ColorsTowers: true
                )
            )
        case .frutigerMetro:
            return dynamicTheme(
                .playStation2Menu, .blue, .hotMagenta,
                sharedMulti: [.blue, .cyan, .hotMagenta, .gold],
                ribbonMulti: [.hotMagenta, .gold, .cyan], animated: true,
                dark: 0.24, particles: false, style: .mixed, amount: 0.38,
                speed: 0.72, brightness: 0.94, motion: 0.78,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 25, gradientOffsetX: 0.10, gradientWidth: 0.90,
                    gradientCurvature: 0.45, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 2.8, ps2RollSpeed: 5.2, ps2OrbitDegrees: 30,
                    ps2ColorsTowers: true
                )
            )
        case .mcBling:
            return dynamicTheme(
                .playStation3XMBByMart, .pink, .gold,
                sharedGradientHexes: [0xFFD0EA, 0xF173B5, 0xB6227A, 0xFFE0A8],
                ribbonGradientHexes: [0xFFF5CF, 0xFF8DD2, 0xC7E9FF],
                animated: true, dark: 0.20, style: .ps4Glow, amount: 0.76,
                speed: 0.82, brightness: 1.0, motion: 0.86,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 26, gradientOffsetY: -0.06, gradientWidth: 0.90,
                    gradientCurvature: 0.42, xmbParticleCount: 860,
                    xmbParticleOpacity: 0.82, xmbParticleSize: 11, xmbFlowSpeed: 0.22,
                    xmbParticleFlowSpeed: 0.20, xmbBandAmplitude: 0.27,
                    xmbOpacity: 0.82, xmbBrightness: 0.58
                )
            )
        case .vectorbloom:
            return dynamicTheme(
                .playStation2Menu, .xmbSpringMeadow, .hotMagenta,
                sharedMulti: [.xmbSpringMeadow, .pink, .arcticIce],
                ribbonMulti: [.hotMagenta, .pink, .emerald], animated: true,
                dark: 0.18, particles: false, style: .ps5Drift, amount: 0.36,
                speed: 0.44, brightness: 0.92, motion: 0.52,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -22, gradientOffsetX: -0.12, gradientWidth: 1.25,
                    gradientCurvature: 0.50, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 1.6, ps2RollSpeed: 3.4, ps2OrbitDegrees: 24,
                    ps2ColorsTowers: true
                )
            )
        case .skeuomorphicAqua:
            return dynamicTheme(
                .playStation3XMBByMart, .arcticIce, .cyan,
                sharedGradientHexes: [0xDFF8FF, 0x77D6F2, 0x198EC5, 0xD4F5FF],
                ribbonGradientHexes: [0xFFFFFF, 0x5FCDF1, 0x006BA6],
                dark: 0.16, particles: false, style: .ps4Glow, amount: 0.30,
                speed: 0.36, brightness: 0.92, motion: 0.44,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -16, gradientOffsetY: -0.05, gradientWidth: 1.15,
                    gradientCurvature: 0.20, xmbParticleCount: 460,
                    xmbParticleOpacity: 0.45, xmbParticleSize: 12, xmbFlowSpeed: 0.10,
                    xmbParticleFlowSpeed: 0.08, xmbBandAmplitude: 0.22,
                    xmbOpacity: 0.78, xmbBrightness: 0.52
                )
            )
        case .y2kChrome:
            return dynamicTheme(
                .playStation3XMBByMart, .silver, .arcticIce,
                sharedGradientHexes: [0xF4F7FA, 0xAEBBC6, 0x637480, 0xE9F4FA],
                ribbonGradientHexes: [0xFFFFFF, 0x8BDCF4, 0x536F82],
                animated: true, dark: 0.22, particles: false, style: .xmbMart,
                amount: 0.32, speed: 0.40, brightness: 0.96, motion: 0.46,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 0, gradientOffsetX: 0.04, gradientWidth: 0.82,
                    gradientCurvature: -0.28, xmbParticleCount: 300,
                    xmbParticleOpacity: 0.42, xmbParticleSize: 6, xmbFlowSpeed: 0.08,
                    xmbParticleFlowSpeed: 0.07, xmbBandAmplitude: 0.18,
                    xmbOpacity: 0.82, xmbBrightness: 0.58
                )
            )
        case .aquaWeb:
            return dynamicTheme(
                .playStation2Menu, .xmbAzureHorizon, .cyan,
                sharedMulti: [.xmbAzureHorizon, .cyan, .arcticIce],
                ribbonMulti: [.cyan, .blue], animated: true,
                dark: 0.18, style: .xmb3, amount: 0.56, speed: 0.68,
                brightness: 0.96, motion: 0.76,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -18, gradientOffsetY: -0.08, gradientWidth: 1.0,
                    gradientCurvature: 0.30, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 2.2, ps2RollSpeed: 4.8, ps2OrbitDegrees: 28,
                    ps2ColorsTowers: true
                )
            )
        case .bubblegumTech:
            return dynamicTheme(
                .playStation3XMBByMart, .xmbSakuraBloom, .hotMagenta,
                sharedMulti: [.xmbSakuraBloom, .pink, .lavender],
                ribbonMulti: [.hotMagenta, .gold], animated: true,
                dark: 0.18, style: .ps4Glow, amount: 0.68, speed: 0.70,
                brightness: 0.98, motion: 0.74,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 22, gradientOffsetX: 0.08, gradientWidth: 1.05,
                    gradientCurvature: 0.34, xmbParticleCount: 760,
                    xmbParticleOpacity: 0.74, xmbParticleSize: 10, xmbFlowSpeed: 0.18,
                    xmbParticleFlowSpeed: 0.17, xmbBandAmplitude: 0.25,
                    xmbOpacity: 0.78, xmbBrightness: 0.54
                )
            )
        case .digitalMeadow:
            return dynamicTheme(
                .playStation2Menu, .emerald, .electricLime,
                sharedMulti: [.emerald, .xmbSpringMeadow, .electricLime],
                ribbonMulti: [.electricLime, .cyan], animated: true,
                dark: 0.20, style: .ps5Drift, amount: 0.46, speed: 0.54,
                brightness: 0.94, motion: 0.62,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -8, gradientOffsetX: -0.08, gradientWidth: 1.35,
                    gradientCurvature: 0.38, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 1.3, ps2RollSpeed: 2.6, ps2OrbitDegrees: 20,
                    ps2ColorsTowers: true
                )
            )
        case .glassGarden:
            return dynamicTheme(
                .playStation3XMBByMart, .xmbSpringMeadow, .cyan,
                sharedGradientHexes: [0xDFF7E8, 0x80C99A, 0x3D8D68, 0xBEE9F2],
                ribbonGradientHexes: [0xF7FFFF, 0x69D7B3, 0x4BAEE5],
                animated: true, dark: 0.14, particles: false, style: .ps5Drift,
                amount: 0.34, speed: 0.42, brightness: 0.94, motion: 0.48,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: -24, gradientOffsetY: 0.05, gradientWidth: 1.35,
                    gradientCurvature: 0.55, xmbParticleCount: 400,
                    xmbParticleOpacity: 0.44, xmbParticleSize: 8, xmbFlowSpeed: 0.09,
                    xmbParticleFlowSpeed: 0.08, xmbBandAmplitude: 0.19,
                    xmbOpacity: 0.68, xmbBrightness: 0.46
                )
            )
        case .zenGlass:
            return dynamicTheme(
                .playStation3XMBByMart, .silver, .deepTeal,
                sharedGradientHexes: [0xE8F1EE, 0xBACDC8, 0x73958E, 0xF5FAF8],
                ribbonGradientHexes: [0xFFFFFF, 0x78B8B2, 0x395E60],
                dark: 0.18, particles: false, style: .xmbMart, amount: 0.24,
                speed: 0.28, brightness: 0.80, motion: 0.34,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 3, gradientOffsetY: 0.02, gradientWidth: 1.50,
                    gradientCurvature: 0.15, xmbParticleCount: 180,
                    xmbParticleOpacity: 0.25, xmbParticleSize: 6, xmbFlowSpeed: 0.045,
                    xmbParticleFlowSpeed: 0.04, xmbTension: 0.06,
                    xmbBandAmplitude: 0.08, xmbOpacity: 0.46, xmbBrightness: 0.34
                )
            )
        case .metroNight:
            return dynamicTheme(
                .playStation2Menu, .midnight, .hotMagenta,
                sharedMulti: [.midnight, .violet, .blue],
                ribbonMulti: [.hotMagenta, .cyan, .gold], animated: true,
                dark: 1.28, particles: false, style: .mixed, amount: 0.42,
                speed: 0.74, brightness: 0.86, motion: 0.82,
                tuning: ControllerUIDynamicThemeTuning(
                    gradientTilt: 32, gradientOffsetX: 0.12, gradientWidth: 0.85,
                    gradientCurvature: -0.35, ps2FramesPerSecond: 15,
                    ps2YawSpeed: 3.2, ps2RollSpeed: 6.5, ps2OrbitDegrees: 34,
                    ps2ColorsTowers: true
                )
            )
        }
    }

    private func dynamicTheme(
        _ background: DynamicBackgroundStyle,
        _ shared: ThemePalette,
        _ ribbons: ThemePalette,
        sharedMulti: [ThemePalette] = [],
        ribbonMulti: [ThemePalette] = [],
        sharedGradientHexes: [UInt32] = [],
        ribbonGradientHexes: [UInt32] = [],
        animated: Bool = false,
        dark: Double,
        particles: Bool = true,
        style: DynamicParticleStyle,
        amount: Double,
        speed: Double,
        brightness: Double,
        motion: Double,
        armsx2Logo: Bool = false,
        tuning: ControllerUIDynamicThemeTuning = ControllerUIDynamicThemeTuning()
    ) -> ControllerUIDynamicThemeConfiguration {
        var particleSettings = DynamicParticleSettings()
        particleSettings.isEnabled = particles
        particleSettings.style = style
        particleSettings.amount = amount
        particleSettings.speed = speed
        particleSettings.brightness = brightness
        particleSettings.paletteDarkEffectIntensity = dark
        particleSettings.sharedPaletteGradientTilt = tuning.gradientTilt
        particleSettings.sharedPaletteGradientOffsetX = tuning.gradientOffsetX
        particleSettings.sharedPaletteGradientOffsetY = tuning.gradientOffsetY
        particleSettings.sharedPaletteGradientWidth = tuning.gradientWidth
        particleSettings.sharedPaletteGradientCurvature = tuning.gradientCurvature
        particleSettings.multiColorAnimationSpeed = motion
        particleSettings.armsx2LogoEnabled = armsx2Logo
        particleSettings.playStation3XMB.gradientPreset = .theme
        particleSettings.playStation3XMB.flowSpeed = tuning.xmbFlowSpeed ?? (0.18 * motion)
        particleSettings.playStation3XMB.particleFlowSpeed =
            tuning.xmbParticleFlowSpeed ?? (0.18 * motion)
        particleSettings.backgrounds.multicolorAmbientSpeed = motion
        particleSettings.backgrounds.lightSpeedMotionSpeed = motion
        particleSettings.backgrounds.spatialRetroSpeed = motion
        particleSettings.backgrounds.towersOrbsSpeed = motion
        particleSettings.backgrounds.playStation2MenuSceneSpeed = motion
        particleSettings.backgrounds.faceButtonsSpeed = motion
        particleSettings.backgrounds.playStationPortableBlurSpeed = motion
        particleSettings.backgrounds.playStation3SplinesSpeed = motion
        particleSettings.backgrounds.playStation4ParticlesSpeed = motion
        particleSettings.backgrounds.playStation4WavesSpeed = motion
        particleSettings.backgrounds.playStationRibbonsSpeed = motion

        if background == .playStation2Menu {
            // All built-in themes share the same low-cost PS2 renderer cadence.
            // Theme-specific motion still comes from scene/yaw/roll tuning.
            particleSettings.backgrounds.playStation2MenuFramesPerSecond = 15
        } else if let value = tuning.ps2FramesPerSecond {
            particleSettings.backgrounds.playStation2MenuFramesPerSecond = value
        }
        if let value = tuning.ps2YawSpeed {
            particleSettings.backgrounds.playStation2MenuYawSpeed = value
        }
        if let value = tuning.ps2RollSpeed {
            particleSettings.backgrounds.playStation2MenuRollSpeed = value
        }
        if let value = tuning.ps2OrbitDegrees {
            particleSettings.backgrounds.playStation2MenuOrbitDegrees = value
        }
        if let value = tuning.ps2ColorsTowers {
            particleSettings.backgrounds.playStation2MenuColorsTowers = value
        }
        if let value = tuning.xmbParticleCount {
            particleSettings.playStation3XMB.particleCount = value
        }
        if let value = tuning.xmbParticleOpacity {
            particleSettings.playStation3XMB.particleOpacity = value
        }
        if let value = tuning.xmbParticleSize {
            particleSettings.playStation3XMB.particleSizeBase = value
        }
        if let value = tuning.xmbTension {
            particleSettings.playStation3XMB.tension = value
        }
        if let value = tuning.xmbDamping {
            particleSettings.playStation3XMB.damping = value
        }
        if let value = tuning.xmbBandAmplitude {
            particleSettings.playStation3XMB.bandAmplitude = value
        }
        if let value = tuning.xmbBandSecondaryFrequency {
            particleSettings.playStation3XMB.bandSecondaryFrequency = value
        }
        if let value = tuning.xmbBandSecondaryAmplitude {
            particleSettings.playStation3XMB.bandSecondaryAmplitude = value
        }
        if let value = tuning.xmbTravelSpeed1 {
            particleSettings.playStation3XMB.travelSpeed1 = value
        }
        if let value = tuning.xmbTravelAmplitude1 {
            particleSettings.playStation3XMB.travelAmplitude1 = value
        }
        if let value = tuning.xmbTravelSpeed2 {
            particleSettings.playStation3XMB.travelSpeed2 = value
        }
        if let value = tuning.xmbTravelAmplitude2 {
            particleSettings.playStation3XMB.travelAmplitude2 = value
        }
        if let value = tuning.xmbOpacity {
            particleSettings.playStation3XMB.opacity = value
        }
        if let value = tuning.xmbBrightness {
            particleSettings.playStation3XMB.brightness = value
        }

        return ControllerUIDynamicThemeConfiguration(
            background: background,
            sharedPalette: shared,
            sharedCustomColor: savedGradient(sharedGradientHexes),
            ribbonPalette: ribbons,
            ribbonCustomColor: savedGradient(ribbonGradientHexes),
            sharedMultiColorPalettes: sharedMulti,
            ribbonMultiColorPalettes: ribbonMulti,
            animatesMultiColor: animated,
            particleSettings: particleSettings
        )
    }

    private func savedGradient(_ hexes: [UInt32]) -> SavedPaletteColor? {
        guard hexes.count > 1 else { return nil }
        return SavedPaletteColor(colors: hexes.map(rgb))
    }
}
