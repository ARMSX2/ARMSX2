// ControllerFocusBoxParticleOutline.swift — the particle focus box outlines
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ControllerFocusBoxParticleOutline: View {
    let preset: ParticlePreset
    let colors: [Color]
    let cornerRadius: CGFloat
    let phase: Double
    let reducedGlow: Double
    let performanceOptimized: Bool

    private var closedColors: [Color] {
        guard let first = colors.first else { return [.blue, .cyan, .blue] }
        return colors + [first]
    }

    private var primaryGlow: Color { colors.first ?? .blue }
    private var secondaryGlow: Color { colors.dropFirst().first ?? primaryGlow }

    private var shape: RoundedRectangle {
        RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
    }

    var body: some View {
        particlePresetOutline(preset)
    }

    enum ParticlePreset: Equatable {
        case blood
        case ocean
        case sakura
        case xmb

        var particleCount: Int {
            switch self {
            case .blood: return 8
            case .ocean: return 12
            case .sakura: return 14
            case .xmb: return 22
            }
        }
    }

    private func particlePresetOutline(_ preset: ParticlePreset) -> some View {
        ZStack {
            presetBorder(preset)

            Canvas { context, size in
                drawPresetParticles(
                    preset,
                    in: &context,
                    size: size
                )
            }
        }
        .shadow(
            color: primaryGlow.opacity(0.76 * reducedGlow),
            radius: CGFloat(6 * reducedGlow)
        )
        .shadow(
            color: secondaryGlow.opacity(0.54 * reducedGlow),
            radius: performanceOptimized
                ? 0
                : CGFloat(12 * reducedGlow)
        )
    }

    @ViewBuilder
    private func presetBorder(_ preset: ParticlePreset) -> some View {
        switch preset {
        case .blood:
            shape.strokeBorder(
                LinearGradient(
                    colors: closedColors,
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                ),
                style: StrokeStyle(
                    lineWidth: 2.8,
                    lineCap: .round,
                    dash: [14, 2, 5, 2],
                    dashPhase: CGFloat(
                        wrapped(phase * -12, period: 23)
                    )
                )
            )
        case .ocean:
            shape.strokeBorder(
                AngularGradient(
                    colors: closedColors + closedColors,
                    center: .center,
                    startAngle: .degrees(
                        wrapped(phase * 24 - 90, period: 360)
                    ),
                    endAngle: .degrees(
                        wrapped(phase * 24 - 90, period: 360) + 360
                    )
                ),
                style: StrokeStyle(
                    lineWidth: 2.6,
                    lineCap: .round,
                    dash: [10, 3, 3, 3],
                    dashPhase: CGFloat(
                        wrapped(phase * -18, period: 19)
                    )
                )
            )
        case .sakura:
            shape.strokeBorder(
                LinearGradient(
                    colors: closedColors,
                    startPoint: .leading,
                    endPoint: .trailing
                ),
                style: StrokeStyle(
                    lineWidth: 2.4,
                    lineCap: .round,
                    dash: [2, 4],
                    dashPhase: CGFloat(
                        wrapped(phase * -10, period: 6)
                    )
                )
            )
        case .xmb:
            shape.strokeBorder(
                LinearGradient(
                    colors: colors.map { $0.opacity(0.72) },
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                ),
                lineWidth: 1.35
            )
        }
    }

    private func drawPresetParticles(
        _ preset: ParticlePreset,
        in context: inout GraphicsContext,
        size: CGSize
    ) {
        // The performance-optimized card path halves the particle workload.
        let requestedCount = performanceOptimized
            ? max(6, preset.particleCount / 2)
            : preset.particleCount
        guard requestedCount > 0, size.width > 1, size.height > 1 else { return }

        for index in 0..<requestedCount {
            let seed = Double(index) * 0.618_033_988_75
            let rate = 0.055 + Double(index % 5) * 0.012
            let travel = normalized(phase * rate + seed)
            let wobble = sin(
                wrapped(
                    phase * (0.65 + Double(index % 3) * 0.18) + seed * 9,
                    period: .pi * 2
                )
            )
            let baseX = normalized(seed * 1.73 + travel * 0.16)
            let x = min(
                max(
                    size.width * CGFloat(baseX) + CGFloat(wobble) * 5,
                    1
                ),
                size.width - 1
            )

            let yProgress: Double
            switch preset {
            case .blood, .sakura:
                yProgress = travel
            case .ocean, .xmb:
                yProgress = 1 - travel
            }
            let y = size.height * CGFloat(yProgress)
            let baseRadius = 1.1 + CGFloat(index % 4) * 0.45
            let radius: CGFloat
            switch preset {
            case .blood: radius = baseRadius * 0.85
            case .ocean: radius = baseRadius * 0.72
            case .sakura: radius = baseRadius * 1.05
            case .xmb: radius = baseRadius * 0.62
            }
            let color = colors[index % colors.count]
            let opacity = 0.42 + Double(index % 4) * 0.13
            let particleRect: CGRect
            if preset == .sakura {
                particleRect = CGRect(
                    x: x - radius * 1.5,
                    y: y - radius * 0.7,
                    width: radius * 3,
                    height: radius * 1.4
                )
            } else if preset == .blood {
                particleRect = CGRect(
                    x: x - radius * 0.65,
                    y: y - radius * 1.7,
                    width: radius * 1.3,
                    height: radius * 3.4
                )
            } else {
                particleRect = CGRect(
                    x: x - radius,
                    y: y - radius,
                    width: radius * 2,
                    height: radius * 2
                )
            }
            context.fill(
                Path(ellipseIn: particleRect),
                with: .color(color.opacity(opacity))
            )
        }
    }

    private func normalized(_ value: Double) -> Double {
        wrapped(value, period: 1)
    }

    private func wrapped(_ value: Double, period: Double) -> Double {
        guard period.isFinite, period > 0, value.isFinite else { return 0 }
        let remainder = value.truncatingRemainder(dividingBy: period)
        return remainder < 0 ? remainder + period : remainder
    }
}
