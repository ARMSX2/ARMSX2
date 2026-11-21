// ControllerFocusBoxArtwork.swift — animated focus box artwork
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ControllerFocusBoxArtwork: View {
    let style: ControllerFocusBoxStyle
    let palette: ThemePalette
    var customColors: [Color]? = nil
    let animationSpeed: Double
    let glowIntensity: Double
    let cornerRadius: CGFloat
    let time: TimeInterval
    var performanceOptimized = false

    private var colors: [Color] {
        if let customColors, !customColors.isEmpty {
            return customColors
        }
        let paletteColors = palette.colors
        return paletteColors.isEmpty ? [.blue, .cyan] : paletteColors
    }

    private var closedColors: [Color] {
        guard let first = colors.first else { return [.blue, .cyan, .blue] }
        return colors + [first]
    }

    private var primaryGlow: Color { colors.first ?? .blue }
    private var secondaryGlow: Color { colors.dropFirst().first ?? primaryGlow }
    private var speed: Double { max(0.1, animationSpeed) }
    private var glow: Double { min(max(glowIntensity, 0), 2) }
    private var reducedGlow: Double {
        performanceOptimized ? glow * 0.58 : glow
    }
    private var phase: Double { time * speed }

    private var shape: RoundedRectangle {
        RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
    }

    @ViewBuilder
    var body: some View {
        switch style {
        case .neonBlue:
            shape
                .strokeBorder(primaryGlow.opacity(0.98), lineWidth: 2)
                .shadow(
                    color: primaryGlow.opacity(0.95 * reducedGlow),
                    radius: CGFloat(5 * reducedGlow)
                )
                .shadow(
                    color: secondaryGlow.opacity(0.68 * reducedGlow),
                    radius: performanceOptimized
                        ? 0
                        : CGFloat(11 * reducedGlow)
                )

        case .doubleNeon:
            shape
                .strokeBorder(
                    LinearGradient(
                        colors: closedColors,
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    ),
                    lineWidth: 3.5
                )
                .overlay {
                    shape.strokeBorder(
                        secondaryGlow.opacity(0.95),
                        lineWidth: 1
                    )
                    .padding(3.5)
                }
                .shadow(
                    color: primaryGlow.opacity(0.82 * reducedGlow),
                    radius: CGFloat(6 * reducedGlow)
                )
                .shadow(
                    color: secondaryGlow.opacity(0.6 * reducedGlow),
                    radius: performanceOptimized
                        ? 0
                        : CGFloat(13 * reducedGlow)
                )

        case .softGlow:
            shape
                .strokeBorder(
                    LinearGradient(
                        colors: colors,
                        startPoint: .leading,
                        endPoint: .trailing
                    ),
                    lineWidth: 1.6
                )
                .shadow(
                    color: primaryGlow.opacity(0.65 * reducedGlow),
                    radius: CGFloat(9 * reducedGlow)
                )
                .shadow(
                    color: secondaryGlow.opacity(0.42 * reducedGlow),
                    radius: performanceOptimized
                        ? 0
                        : CGFloat(18 * reducedGlow)
                )

        case .glowingCorners:
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors,
                        center: .center
                    ),
                    style: StrokeStyle(
                        lineWidth: 3,
                        lineCap: .round,
                        dash: [18, 13]
                    )
                )
                .shadow(
                    color: primaryGlow.opacity(0.9 * reducedGlow),
                    radius: CGFloat(7 * reducedGlow)
                )

        case .rainbowNeon:
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors,
                        center: .center,
                        startAngle: .degrees(-90),
                        endAngle: .degrees(270)
                    ),
                    lineWidth: 2.25
                )
                .shadow(
                    color: primaryGlow.opacity(0.82 * reducedGlow),
                    radius: CGFloat(5 * reducedGlow)
                )
                .shadow(
                    color: secondaryGlow.opacity(0.62 * reducedGlow),
                    radius: performanceOptimized
                        ? 0
                        : CGFloat(10 * reducedGlow)
                )

        case .animatedGlowingRainbowNeon:
            let rotation = wrapped(phase * 88, period: 360)
            let pulse = 0.82 + 0.18 * sin(
                wrapped(phase * 3.2, period: .pi * 2)
            )
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors,
                        center: .center,
                        startAngle: .degrees(rotation),
                        endAngle: .degrees(rotation + 360)
                    ),
                    lineWidth: CGFloat(2.3 + pulse * 0.7)
                )
                .shadow(
                    color: primaryGlow.opacity(0.9 * reducedGlow),
                    radius: CGFloat((5 + pulse * 3) * reducedGlow)
                )
                .shadow(
                    color: secondaryGlow.opacity(0.72 * reducedGlow),
                    radius: performanceOptimized
                        ? 0
                        : CGFloat((10 + pulse * 5) * reducedGlow)
                )

        case .stillWaves:
            waveOutline(rotation: -90, dashPhase: 0, glowPulse: 1)

        case .animatedWaves:
            waveOutline(
                rotation: wrapped(phase * 54 - 90, period: 360),
                // Wrapped to one dash program: absolute uptime made a huge
                // dash offset and could stall Core Graphics on this style.
                dashPhase: wrapped(phase * -34, period: 20),
                glowPulse: 0.82 + 0.18 * sin(
                    wrapped(phase * 4, period: .pi * 2)
                )
            )

        case .gradient:
            gradientOutline(
                startPoint: .topLeading,
                endPoint: .bottomTrailing,
                glowPulse: 1
            )

        case .animatedGradient:
            let angle = wrapped(phase * 1.55, period: .pi * 2)
            gradientOutline(
                startPoint: UnitPoint(
                    x: CGFloat(0.5 + cos(angle) * 0.5),
                    y: CGFloat(0.5 + sin(angle) * 0.5)
                ),
                endPoint: UnitPoint(
                    x: CGFloat(0.5 - cos(angle) * 0.5),
                    y: CGFloat(0.5 - sin(angle) * 0.5)
                ),
                glowPulse: 0.86 + 0.14 * sin(
                    wrapped(phase * 3, period: .pi * 2)
                )
            )

        case .dottedPulse:
            let pulse = 0.72 + 0.28 * sin(
                wrapped(phase * 4.4, period: .pi * 2)
            )
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors,
                        center: .center,
                        startAngle: .degrees(-90),
                        endAngle: .degrees(270)
                    ),
                    style: StrokeStyle(
                        lineWidth: CGFloat(2 + pulse),
                        lineCap: .round,
                        dash: [1, 6],
                        dashPhase: CGFloat(
                            wrapped(phase * -22, period: 7)
                        )
                    )
                )
                .shadow(
                    color: primaryGlow.opacity(0.8 * reducedGlow),
                    radius: CGFloat((4 + pulse * 4) * reducedGlow)
                )

        case .cometTrail:
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: colors.map { $0.opacity(0.18) }
                            + closedColors,
                        center: .center,
                        startAngle: .degrees(
                            wrapped(phase * 105 - 90, period: 360)
                        ),
                        endAngle: .degrees(
                            wrapped(phase * 105 - 90, period: 360) + 360
                        )
                    ),
                    style: StrokeStyle(
                        lineWidth: 3,
                        lineCap: .round,
                        dash: [54, 18],
                        dashPhase: CGFloat(
                            wrapped(phase * -46, period: 72)
                        )
                    )
                )
                .shadow(
                    color: primaryGlow.opacity(0.88 * reducedGlow),
                    radius: CGFloat(8 * reducedGlow)
                )

        case .prism:
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors + Array(closedColors.reversed()),
                        center: .center,
                        startAngle: .degrees(-90),
                        endAngle: .degrees(270)
                    ),
                    lineWidth: 3
                )
                .overlay {
                    shape.strokeBorder(.white.opacity(0.46), lineWidth: 0.7)
                }
                .shadow(
                    color: primaryGlow.opacity(0.74 * reducedGlow),
                    radius: CGFloat(7 * reducedGlow)
                )

        case .scanline:
            scanlineOutline()

        case .classicFrame:
            shape
                .strokeBorder(primaryGlow.opacity(0.98), lineWidth: 3)
                .overlay {
                    shape
                        .strokeBorder(secondaryGlow.opacity(0.85), lineWidth: 1)
                        .padding(4)
                }
                .overlay {
                    shape
                        .strokeBorder(colors.last?.opacity(0.72) ?? .clear, lineWidth: 1)
                        .padding(7)
                }
                .shadow(
                    color: primaryGlow.opacity(0.52 * reducedGlow),
                    radius: CGFloat(4 * reducedGlow)
                )

        case .minimal:
            shape.strokeBorder(primaryGlow.opacity(0.96), lineWidth: 1.5)

        case .redBlood:
            particlePresetOutline(.blood)

        case .oceanBlue:
            particlePresetOutline(.ocean)

        case .pinkSakura:
            particlePresetOutline(.sakura)

        case .particles:
            particlePresetOutline(.xmb)

        case .aquaBubble:
            let pulse = 0.82 + 0.18 * sin(
                wrapped(phase * 2.2, period: .pi * 2)
            )
            shape
                .strokeBorder(
                    LinearGradient(
                        colors: [.white.opacity(0.92)] + closedColors,
                        startPoint: .top,
                        endPoint: .bottom
                    ),
                    lineWidth: CGFloat(2.2 + pulse * 0.8)
                )
                .overlay(alignment: .top) {
                    Capsule()
                        .fill(.white.opacity(0.62))
                        .frame(height: 1.2)
                        .padding(.horizontal, max(8, cornerRadius * 0.8))
                        .padding(.top, 2)
                }
                .shadow(
                    color: primaryGlow.opacity(0.82 * reducedGlow),
                    radius: CGFloat((6 + pulse * 5) * reducedGlow)
                )

        case .ecoPulse:
            let pulse = 0.78 + 0.22 * sin(
                wrapped(phase * 2.8, period: .pi * 2)
            )
            shape
                .strokeBorder(
                    LinearGradient(
                        colors: closedColors,
                        startPoint: .leading,
                        endPoint: .trailing
                    ),
                    style: StrokeStyle(
                        lineWidth: CGFloat(2 + pulse),
                        lineCap: .round,
                        dash: [12, 3, 3, 3],
                        dashPhase: CGFloat(wrapped(phase * -13, period: 21))
                    )
                )
                .shadow(
                    color: secondaryGlow.opacity(0.74 * reducedGlow),
                    radius: CGFloat((5 + pulse * 4) * reducedGlow)
                )

        case .darkAeroOrbit:
            let rotation = wrapped(phase * 62, period: 360)
            shape
                .strokeBorder(primaryGlow.opacity(0.42), lineWidth: 1.2)
                .overlay {
                    shape.strokeBorder(
                        AngularGradient(
                            colors: [.clear, primaryGlow, secondaryGlow, .clear],
                            center: .center,
                            startAngle: .degrees(rotation),
                            endAngle: .degrees(rotation + 360)
                        ),
                        style: StrokeStyle(
                            lineWidth: 3.2,
                            lineCap: .round,
                            dash: [42, 20],
                            dashPhase: CGFloat(wrapped(phase * -24, period: 62))
                        )
                    )
                }
                .shadow(
                    color: primaryGlow.opacity(0.88 * reducedGlow),
                    radius: CGFloat(9 * reducedGlow)
                )

        case .zenBreath:
            let breath = 0.56 + 0.44 * (
                sin(wrapped(phase * 1.25, period: .pi * 2)) * 0.5 + 0.5
            )
            shape
                .strokeBorder(primaryGlow.opacity(0.48 + breath * 0.44), lineWidth: 1.4)
                .overlay {
                    shape
                        .strokeBorder(secondaryGlow.opacity(0.34 + breath * 0.38), lineWidth: 0.9)
                        .padding(4 + CGFloat(breath) * 1.5)
                }
                .shadow(
                    color: primaryGlow.opacity(0.34 * reducedGlow),
                    radius: CGFloat((5 + breath * 6) * reducedGlow)
                )

        case .rusticStitch:
            shape
                .strokeBorder(
                    LinearGradient(
                        colors: closedColors,
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    ),
                    style: StrokeStyle(
                        lineWidth: 2.4,
                        lineCap: .round,
                        dash: [5, 3]
                    )
                )
                .overlay {
                    shape.strokeBorder(.white.opacity(0.24), lineWidth: 0.8)
                        .padding(4)
                }
                .shadow(
                    color: primaryGlow.opacity(0.46 * reducedGlow),
                    radius: CGFloat(4 * reducedGlow)
                )

        case .metroSweep:
            let rotation = wrapped(phase * 48 - 90, period: 360)
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors + closedColors,
                        center: .center,
                        startAngle: .degrees(rotation),
                        endAngle: .degrees(rotation + 360)
                    ),
                    style: StrokeStyle(
                        lineWidth: 3.8,
                        lineCap: .square,
                        dash: [28, 5, 9, 5],
                        dashPhase: CGFloat(wrapped(phase * -19, period: 47))
                    )
                )
                .shadow(
                    color: secondaryGlow.opacity(0.64 * reducedGlow),
                    radius: CGFloat(7 * reducedGlow)
                )

        case .rhinestone:
            let twinkle = 0.72 + 0.28 * sin(
                wrapped(phase * 5.2, period: .pi * 2)
            )
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: [.white] + closedColors + [.white],
                        center: .center,
                        startAngle: .degrees(wrapped(phase * 75, period: 360)),
                        endAngle: .degrees(wrapped(phase * 75, period: 360) + 360)
                    ),
                    style: StrokeStyle(
                        lineWidth: CGFloat(2.6 + twinkle),
                        lineCap: .round,
                        dash: [1, 5],
                        dashPhase: CGFloat(wrapped(phase * -28, period: 6))
                    )
                )
                .shadow(
                    color: .white.opacity(0.72 * reducedGlow),
                    radius: CGFloat((4 + twinkle * 5) * reducedGlow)
                )

        case .vectorBloom:
            let rotation = wrapped(phase * 32, period: 360)
            shape
                .strokeBorder(
                    AngularGradient(
                        colors: closedColors,
                        center: .center,
                        startAngle: .degrees(rotation),
                        endAngle: .degrees(rotation + 360)
                    ),
                    style: StrokeStyle(
                        lineWidth: 3,
                        lineCap: .round,
                        dash: [3, 2, 11, 2],
                        dashPhase: CGFloat(wrapped(phase * -11, period: 18))
                    )
                )
                .overlay {
                    shape
                        .strokeBorder(secondaryGlow.opacity(0.42), lineWidth: 1)
                        .padding(4)
                }
                .shadow(
                    color: primaryGlow.opacity(0.66 * reducedGlow),
                    radius: CGFloat(8 * reducedGlow)
                )

        case .skeuomorphicBevel:
            shape
                .strokeBorder(
                    LinearGradient(
                        colors: [.white.opacity(0.92), primaryGlow, secondaryGlow],
                        startPoint: .top,
                        endPoint: .bottom
                    ),
                    lineWidth: 3.4
                )
                .overlay {
                    shape
                        .strokeBorder(.black.opacity(0.34), lineWidth: 1.1)
                        .padding(3.8)
                }
                .shadow(color: .white.opacity(0.36), radius: 1, y: -1)
                .shadow(
                    color: primaryGlow.opacity(0.58 * reducedGlow),
                    radius: CGFloat(7 * reducedGlow),
                    y: 3
                )
        }
    }

    private func scanlineOutline() -> some View {
        let progress = normalized(phase * 0.32)
        return ZStack {
            shape.strokeBorder(
                LinearGradient(
                    colors: closedColors,
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                ),
                lineWidth: 2
            )

            GeometryReader { proxy in
                Rectangle()
                    .fill(
                        LinearGradient(
                            colors: [
                                .clear,
                                primaryGlow.opacity(0.92),
                                secondaryGlow.opacity(0.72),
                                .clear,
                            ],
                            startPoint: .leading,
                            endPoint: .trailing
                        )
                    )
                    .frame(height: 1.5)
                    .offset(
                        y: max(0, proxy.size.height - 1.5)
                            * CGFloat(progress)
                    )
            }
            .clipShape(shape)
        }
        .shadow(
            color: primaryGlow.opacity(0.72 * reducedGlow),
            radius: CGFloat(6 * reducedGlow)
        )
    }

    private func particlePresetOutline(
        _ preset: ControllerFocusBoxParticleOutline.ParticlePreset
    ) -> some View {
        ControllerFocusBoxParticleOutline(
            preset: preset,
            colors: colors,
            cornerRadius: cornerRadius,
            phase: phase,
            reducedGlow: reducedGlow,
            performanceOptimized: performanceOptimized
        )
    }

    private func normalized(_ value: Double) -> Double {
        wrapped(value, period: 1)
    }

    private func wrapped(_ value: Double, period: Double) -> Double {
        guard period.isFinite, period > 0, value.isFinite else { return 0 }
        let remainder = value.truncatingRemainder(dividingBy: period)
        return remainder < 0 ? remainder + period : remainder
    }

    private func waveOutline(
        rotation: Double,
        dashPhase: Double,
        glowPulse: Double
    ) -> some View {
        shape
            .strokeBorder(
                AngularGradient(
                    colors: closedColors + closedColors,
                    center: .center,
                    startAngle: .degrees(rotation),
                    endAngle: .degrees(rotation + 360)
                ),
                style: StrokeStyle(
                    lineWidth: 2.6,
                    lineCap: .round,
                    lineJoin: .round,
                    dash: [9, 4, 3, 4],
                    dashPhase: CGFloat(dashPhase)
                )
            )
            .overlay {
                shape.strokeBorder(
                    LinearGradient(
                        colors: closedColors,
                        startPoint: .leading,
                        endPoint: .trailing
                    ),
                    style: StrokeStyle(
                        lineWidth: 0.85,
                        dash: [3, 7],
                        dashPhase: CGFloat(-dashPhase * 0.55)
                    )
                )
            }
            .shadow(
                color: primaryGlow.opacity(0.76 * reducedGlow),
                radius: CGFloat((5 + glowPulse * 3) * reducedGlow)
            )
            .shadow(
                color: secondaryGlow.opacity(0.54 * reducedGlow),
                radius: performanceOptimized
                    ? 0
                    : CGFloat(10 * reducedGlow)
            )
    }

    private func gradientOutline(
        startPoint: UnitPoint,
        endPoint: UnitPoint,
        glowPulse: Double
    ) -> some View {
        shape
            .strokeBorder(
                LinearGradient(
                    colors: closedColors,
                    startPoint: startPoint,
                    endPoint: endPoint
                ),
                lineWidth: 2.5
            )
            .overlay {
                shape.strokeBorder(
                    LinearGradient(
                        colors: Array(closedColors.reversed()),
                        startPoint: endPoint,
                        endPoint: startPoint
                    ),
                    lineWidth: 0.75
                )
            }
            .shadow(
                color: primaryGlow.opacity(0.8 * reducedGlow),
                radius: CGFloat((5 + glowPulse * 2) * reducedGlow)
            )
            .shadow(
                color: secondaryGlow.opacity(0.58 * reducedGlow),
                radius: performanceOptimized
                    ? 0
                    : CGFloat(11 * reducedGlow)
            )
    }
}
