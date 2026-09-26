// GameLibraryScreenOrbField.swift - full-screen free-roaming game library orbs
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// One half of the Games library's depth-split orb field. The two instances
/// are placed on opposite sides of the card content and evaluate the same
/// deterministic motion program, so an orb can pass behind a card and emerge
/// above it without changing position or restarting its animation.
struct GameLibraryScreenOrbField: View {
    enum DepthLayer {
        case behindCards
        case aboveCards
    }

    let depthLayer: DepthLayer
    let isActive: Bool
    let controllerInput: MenuControllerInputRouter?

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var settings = SettingsStore.shared

    private let particles = GameLibraryScreenOrbParticle.standard

    var body: some View {
        GeometryReader { proxy in
            AdaptiveAnimationTimeline(
                domain: .controllerEffects,
                paused: reduceMotion || !isActive
            ) { timeline in
                let time = reduceMotion
                    ? 0
                    : timeline.date.timeIntervalSinceReferenceDate

                ZStack {
                    Canvas { context, size in
                        drawTrails(
                            in: &context,
                            size: size,
                            time: time
                        )
                    }

                    orbLayer(time: time, size: proxy.size)
                }
                .frame(width: proxy.size.width, height: proxy.size.height)
            }
        }
        .opacity(isActive ? 1 : 0)
        .animation(.easeOut(duration: 0.16), value: isActive)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    @ViewBuilder
    private func orbLayer(time: TimeInterval, size: CGSize) -> some View {
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: 3) {
                particlesLayer(time: time, size: size, usesGlass: true)
            }
        } else {
            particlesLayer(time: time, size: size, usesGlass: false)
        }
    }

    private func particlesLayer(
        time: TimeInterval,
        size: CGSize,
        usesGlass: Bool
    ) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(particles) { particle in
                let depth = particleDepth(particle, at: time)
                let opacity = layerOpacity(for: depth)
                let position = particlePosition(
                    particle,
                    at: time,
                    in: size
                )
                let depthScale = 0.88 + CGFloat((depth + 1) * 0.14)

                GameLibraryScreenGlassOrb(
                    color: particleColor(particle.colorIndex),
                    diameter: particle.diameter * depthScale,
                    usesGlass: usesGlass
                )
                .position(position)
                .opacity(opacity)
            }
        }
        .frame(width: size.width, height: size.height)
        .animation(.easeOut(duration: 0.12), value: pressedColor)
    }

    private func drawTrails(
        in context: inout GraphicsContext,
        size: CGSize,
        time: TimeInterval
    ) {
        guard !reduceMotion else { return }

        let sampleInterval: TimeInterval = 0.072
        let sampleCount = 11

        for particle in particles {
            let color = particleColor(particle.colorIndex)
            var samples: [(point: CGPoint, opacity: Double)] = []
            samples.reserveCapacity(sampleCount)

            for index in 0..<sampleCount {
                let sampleTime = time - Double(index) * sampleInterval
                let depth = particleDepth(particle, at: sampleTime)
                samples.append((
                    point: particlePosition(
                        particle,
                        at: sampleTime,
                        in: size
                    ),
                    opacity: layerOpacity(for: depth)
                ))
            }

            for index in 0..<(samples.count - 1) {
                let start = samples[index]
                let end = samples[index + 1]
                let depthOpacity = min(start.opacity, end.opacity)
                guard depthOpacity > 0.01 else { continue }

                let fade = 1 - Double(index) / Double(samples.count - 1)
                var segment = Path()
                segment.move(to: start.point)
                segment.addLine(to: end.point)

                context.stroke(
                    segment,
                    with: .color(
                        color.opacity(fade * depthOpacity * 0.14)
                    ),
                    style: StrokeStyle(lineWidth: 5, lineCap: .round)
                )
                context.stroke(
                    segment,
                    with: .color(
                        color.opacity(fade * depthOpacity * 0.82)
                    ),
                    style: StrokeStyle(
                        lineWidth: 0.55 + fade,
                        lineCap: .round
                    )
                )
            }
        }
    }

    private func particlePosition(
        _ particle: GameLibraryScreenOrbParticle,
        at time: TimeInterval,
        in size: CGSize
    ) -> CGPoint {
        guard size.width > 0, size.height > 0 else { return .zero }

        let phaseTime = time / particle.travelDuration + particle.phase
        let segment = floor(phaseTime)
        let progress = smoothStep(phaseTime - segment)
        let start = randomWaypoint(for: particle, segment: segment)
        let end = randomWaypoint(for: particle, segment: segment + 1)
        let normalizedX = interpolate(start.x, end.x, progress)
        let normalizedY = interpolate(start.y, end.y, progress)
        let edgeInset = max(16, particle.diameter * 0.75)
        let availableWidth = max(0, size.width - edgeInset * 2)
        let availableHeight = max(0, size.height - edgeInset * 2)

        return CGPoint(
            x: edgeInset + normalizedX * availableWidth,
            y: edgeInset + normalizedY * availableHeight
        )
    }

    private func randomWaypoint(
        for particle: GameLibraryScreenOrbParticle,
        segment: Double
    ) -> CGPoint {
        CGPoint(
            x: CGFloat(stableRandom(
                particle.seed + segment * 37.17 + 11.3
            )),
            y: CGFloat(stableRandom(
                particle.seed + segment * 53.91 + 79.7
            ))
        )
    }

    private func particleDepth(
        _ particle: GameLibraryScreenOrbParticle,
        at time: TimeInterval
    ) -> Double {
        let phaseTime = time / particle.depthDuration + particle.depthPhase
        let segment = floor(phaseTime)
        let progress = smoothStep(phaseTime - segment)
        let start = depthWaypoint(for: particle, segment: segment)
        let end = depthWaypoint(for: particle, segment: segment + 1)
        return interpolate(start, end, progress)
    }

    private func depthWaypoint(
        for particle: GameLibraryScreenOrbParticle,
        segment: Double
    ) -> Double {
        // Alternating planes guarantees that every orb visits both sides of
        // the cards; the independently seeded magnitude and duration keep the
        // depth changes from appearing synchronized.
        let integralSegment = Int(segment.rounded(.down))
        let direction = (integralSegment + particle.id).isMultiple(of: 2)
            ? -1.0
            : 1.0
        let magnitude = 0.32 + stableRandom(
            particle.seed + segment * 29.41 + 151.9
        ) * 0.68
        return direction * magnitude
    }

    private func layerOpacity(for depth: Double) -> Double {
        let aboveCardsOpacity = smoothStep((depth + 0.18) / 0.36)
        switch depthLayer {
        case .behindCards:
            return 1 - aboveCardsOpacity
        case .aboveCards:
            return aboveCardsOpacity
        }
    }

    private var pressedColor: Color? {
        switch controllerInput?.pressedFaceButton {
        case .cross: return .green
        case .circle: return .red
        case .triangle: return .orange
        case nil: return nil
        }
    }

    private func particleColor(_ index: Int) -> Color {
        if let pressedColor { return pressedColor }
        let colors = settings.controllerOrbColors
        return colors.isEmpty ? .blue : colors[index % colors.count]
    }

    private func stableRandom(_ seed: Double) -> Double {
        let rawValue = sin(seed * 12.9898) * 43_758.5453
        return rawValue - floor(rawValue)
    }

    private func smoothStep(_ value: Double) -> Double {
        let progress = min(max(value, 0), 1)
        return progress * progress * (3 - 2 * progress)
    }

    private func interpolate(
        _ start: Double,
        _ end: Double,
        _ progress: Double
    ) -> Double {
        start + (end - start) * progress
    }

    private func interpolate(
        _ start: CGFloat,
        _ end: CGFloat,
        _ progress: Double
    ) -> CGFloat {
        start + (end - start) * CGFloat(progress)
    }
}

private struct GameLibraryScreenGlassOrb: View {
    let color: Color
    let diameter: CGFloat
    let usesGlass: Bool

    private var core: some View {
        Circle()
            .fill(.white.opacity(0.08))
            .frame(width: diameter, height: diameter)
            .overlay {
                Circle()
                    .stroke(.white.opacity(0.62), lineWidth: 0.65)
            }
            .overlay(alignment: .topLeading) {
                Circle()
                    .fill(.white.opacity(0.38))
                    .frame(
                        width: diameter * 0.24,
                        height: diameter * 0.24
                    )
                    .padding(diameter * 0.18)
            }
            .shadow(color: color.opacity(0.72), radius: diameter * 0.85)
    }

    @ViewBuilder
    var body: some View {
        if #available(iOS 26.0, *), usesGlass {
            core
                .glassEffect(
                    .regular.tint(color.opacity(0.34)),
                    in: .circle
                )
                .opacity(0.78)
        } else {
            core
                .background(.ultraThinMaterial, in: Circle())
                .overlay {
                    Circle().fill(color.opacity(0.2))
                }
                .opacity(0.75)
        }
    }
}

private struct GameLibraryScreenOrbParticle: Identifiable {
    let id: Int
    let seed: Double
    let phase: Double
    let depthPhase: Double
    let travelDuration: TimeInterval
    let depthDuration: TimeInterval
    let diameter: CGFloat
    let colorIndex: Int

    static let standard: [GameLibraryScreenOrbParticle] = [
        .init(
            id: 0, seed: 3.17, phase: 0.14, depthPhase: 0.62,
            travelDuration: 4.1, depthDuration: 3.7,
            diameter: 9, colorIndex: 0
        ),
        .init(
            id: 1, seed: 17.83, phase: 0.71, depthPhase: 0.18,
            travelDuration: 5.3, depthDuration: 4.6,
            diameter: 12, colorIndex: 1
        ),
        .init(
            id: 2, seed: 41.29, phase: 0.38, depthPhase: 0.83,
            travelDuration: 6.2, depthDuration: 5.1,
            diameter: 8, colorIndex: 2
        ),
        .init(
            id: 3, seed: 73.61, phase: 0.89, depthPhase: 0.37,
            travelDuration: 4.7, depthDuration: 4.2,
            diameter: 10, colorIndex: 3
        ),
    ]
}
