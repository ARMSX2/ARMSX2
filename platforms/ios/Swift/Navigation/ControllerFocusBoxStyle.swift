// ControllerFocusBoxStyle.swift — focus box styles
// SPDX-License-Identifier: GPL-3.0+

enum ControllerFocusBoxStyle: String, CaseIterable, Identifiable, Codable {
    case neonBlue
    case doubleNeon
    case softGlow
    case glowingCorners
    case rainbowNeon
    case animatedGlowingRainbowNeon
    case stillWaves
    case animatedWaves
    case gradient
    case animatedGradient
    case dottedPulse
    case cometTrail
    case prism
    case scanline
    case classicFrame
    case minimal
    case redBlood
    case oceanBlue
    case pinkSakura
    case particles
    case aquaBubble
    case ecoPulse
    case darkAeroOrbit
    case zenBreath
    case rusticStitch
    case metroSweep
    case rhinestone
    case vectorBloom
    case skeuomorphicBevel

    var id: String { rawValue }

    var title: String {
        switch self {
        case .neonBlue: return "Neon"
        case .doubleNeon: return "Double Neon"
        case .softGlow: return "Soft Glow"
        case .glowingCorners: return "Glowing Corners"
        case .rainbowNeon: return "Rainbow Neon"
        case .animatedGlowingRainbowNeon:
            return "Animated Glowing Rainbow Neon"
        case .stillWaves: return "Still Waves"
        case .animatedWaves: return "Animated Waves"
        case .gradient: return "Gradient"
        case .animatedGradient: return "Animated Gradient"
        case .dottedPulse: return "Dotted Pulse"
        case .cometTrail: return "Comet Trail"
        case .prism: return "Prism"
        case .scanline: return "Scanline"
        case .classicFrame: return "Classic Console Frame"
        case .minimal: return "Minimal"
        case .redBlood: return "Red Blood"
        case .oceanBlue: return "Ocean Blue"
        case .pinkSakura: return "Pink Sakura"
        case .particles: return "Particles"
        case .aquaBubble: return "Aqua Bubble"
        case .ecoPulse: return "Eco Pulse"
        case .darkAeroOrbit: return "Dark Aero Orbit"
        case .zenBreath: return "Zen Breath"
        case .rusticStitch: return "Rustic Stitch"
        case .metroSweep: return "Metro Sweep"
        case .rhinestone: return "Rhinestone Sparkle"
        case .vectorBloom: return "Vector Bloom"
        case .skeuomorphicBevel: return "Skeuomorphic Bevel"
        }
    }

    var usesPalette: Bool {
        true
    }

    var isAnimated: Bool {
        switch self {
        case .animatedGlowingRainbowNeon, .animatedWaves, .animatedGradient,
             .dottedPulse, .cometTrail, .scanline, .redBlood, .oceanBlue,
             .pinkSakura, .particles, .aquaBubble, .ecoPulse,
             .darkAeroOrbit, .zenBreath, .metroSweep, .rhinestone,
             .vectorBloom:
            return true
        case .neonBlue, .doubleNeon, .softGlow, .glowingCorners,
             .rainbowNeon, .stillWaves, .gradient, .prism, .classicFrame,
             .minimal, .rusticStitch, .skeuomorphicBevel:
            return false
        }
    }
}
