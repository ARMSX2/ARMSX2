// ControllerNavigationOrbField.swift - OrbitKeys-inspired controller focus field
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ControllerNavigationDepthEffectEnabledEnvironmentKey:
    EnvironmentKey {
    static let defaultValue = true
}

private struct ControllerFocusDepthHandledByAncestorEnvironmentKey:
    EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var controllerNavigationDepthEffectEnabled: Bool {
        get { self[ControllerNavigationDepthEffectEnabledEnvironmentKey.self] }
        set { self[ControllerNavigationDepthEffectEnabledEnvironmentKey.self] = newValue }
    }

    var controllerFocusDepthHandledByAncestor: Bool {
        get { self[ControllerFocusDepthHandledByAncestorEnvironmentKey.self] }
        set { self[ControllerFocusDepthHandledByAncestorEnvironmentKey.self] = newValue }
    }
}

struct ControllerNavigationOrbInteraction: Equatable, Sendable {
    let sequence: UInt64
    let command: MenuControllerCommand
    let timestamp: TimeInterval
}

enum ControllerNavigationOrbStyle: Equatable {
    case liquidGlass
    case plain
}

enum ControllerNavigationOrbPalette: Equatable {
    case blue
    case green
    case red
    case orange
    case purple
    case pink

    var primaryColor: Color {
        switch self {
        case .blue: return .blue
        case .green: return .green
        case .red: return .red
        case .orange: return .orange
        case .purple: return .purple
        case .pink: return .pink
        }
    }
}

enum ControllerNavigationOrbReactionKind: Equatable {
    case activate
    case back
    case contextMenu
    case favorite
    case tab
}

struct ControllerNavigationOrbReaction: Equatable {
    let sequence: UInt64
    let kind: ControllerNavigationOrbReactionKind
}

enum ControllerNavigationOrbPathShape {
    case ellipse
    case roundedRectangle
}

/// Returns a constant-direction point on a rounded rectangle centred at zero.
/// Game-card focus uses this perimeter instead of the generic elliptical orbit,
/// keeping the particles aligned with the card's actual silhouette.
func controllerNavigationRoundedRectangleOffset(
    angle: Double,
    radiusX: CGFloat,
    radiusY: CGFloat
) -> CGSize {
    let halfWidth = max(2, radiusX)
    let halfHeight = max(2, radiusY)
    let cornerRadius = min(
        min(halfWidth, halfHeight) * 0.34,
        24
    )
    let horizontalLength = max(0, 2 * (halfWidth - cornerRadius))
    let verticalLength = max(0, 2 * (halfHeight - cornerRadius))
    let cornerLength = .pi * cornerRadius / 2
    let perimeter = 2 * horizontalLength
        + 2 * verticalLength
        + 4 * cornerLength
    guard perimeter > 0 else { return .zero }

    var turn = (angle / (.pi * 2)).truncatingRemainder(dividingBy: 1)
    if turn < 0 { turn += 1 }
    var distance = CGFloat(turn) * perimeter

    func arc(
        centerX: CGFloat,
        centerY: CGFloat,
        startAngle: Double,
        distance: CGFloat
    ) -> CGSize {
        let progress = cornerLength > 0 ? distance / cornerLength : 0
        let currentAngle = startAngle + Double(progress) * .pi / 2
        return CGSize(
            width: centerX + CGFloat(cos(currentAngle)) * cornerRadius,
            height: centerY + CGFloat(sin(currentAngle)) * cornerRadius
        )
    }

    if distance <= horizontalLength {
        return CGSize(
            width: -halfWidth + cornerRadius + distance,
            height: -halfHeight
        )
    }
    distance -= horizontalLength
    if distance <= cornerLength {
        return arc(
            centerX: halfWidth - cornerRadius,
            centerY: -halfHeight + cornerRadius,
            startAngle: -.pi / 2,
            distance: distance
        )
    }
    distance -= cornerLength
    if distance <= verticalLength {
        return CGSize(
            width: halfWidth,
            height: -halfHeight + cornerRadius + distance
        )
    }
    distance -= verticalLength
    if distance <= cornerLength {
        return arc(
            centerX: halfWidth - cornerRadius,
            centerY: halfHeight - cornerRadius,
            startAngle: 0,
            distance: distance
        )
    }
    distance -= cornerLength
    if distance <= horizontalLength {
        return CGSize(
            width: halfWidth - cornerRadius - distance,
            height: halfHeight
        )
    }
    distance -= horizontalLength
    if distance <= cornerLength {
        return arc(
            centerX: -halfWidth + cornerRadius,
            centerY: halfHeight - cornerRadius,
            startAngle: .pi / 2,
            distance: distance
        )
    }
    distance -= cornerLength
    if distance <= verticalLength {
        return CGSize(
            width: -halfWidth,
            height: halfHeight - cornerRadius - distance
        )
    }
    distance -= verticalLength
    return arc(
        centerX: -halfWidth + cornerRadius,
        centerY: -halfHeight + cornerRadius,
        startAngle: .pi,
        distance: min(distance, cornerLength)
    )
}

/// A render-only controller focus treatment adapted from OrbitKeys' orbit field.
/// One presentation-level instance survives focus changes, so its timeline does
/// not invalidate the surrounding navigation layout.
struct ControllerNavigationOrbField: View {
    let primaryColor: Color
    let accentColor: Color
    var paletteColors: [Color] = []
    let pressedColor: Color?
    let style: ControllerNavigationOrbStyle
    var inset: CGFloat = 2
    var orbScale: CGFloat = 1
    var speedMultiplier: Double = 1
    var dispersion: CGFloat = 1
    var reaction: ControllerNavigationOrbReaction? = nil
    var renderTimeOverride: TimeInterval? = nil
    var phaseTimeOverride: TimeInterval? = nil
    var dispersionOverride: CGFloat? = nil
    var reactionStateOverride: ControllerNavigationOrbReactionState? = nil
    var showsTrails = true
    var pathShape: ControllerNavigationOrbPathShape = .ellipse

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var speedPrograms: [ControllerNavigationOrbSpeedProgram] = []
    @State private var dispersionPrograms: [ControllerNavigationOrbDispersionProgram] = []
    @State private var reactionPrograms: [ControllerNavigationOrbReactionProgram] = []

    private let particles = ControllerNavigationOrbParticle.standard

    var body: some View {
        GeometryReader { proxy in
            AdaptiveAnimationTimeline(
                domain: .controllerEffects,
                paused: reduceMotion || renderTimeOverride != nil
            ) { timeline in
                let time = reduceMotion
                    ? 0
                    : (renderTimeOverride
                        ?? timeline.date.timeIntervalSinceReferenceDate)

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
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onAppear {
            let now = Date.timeIntervalSinceReferenceDate
            speedPrograms = [
                ControllerNavigationOrbSpeedProgram(
                    startTime: now,
                    initialState: ControllerNavigationOrbSpeedState(
                        phaseTime: now,
                        multiplier: speedMultiplier
                    ),
                    targetMultiplier: speedMultiplier,
                    duration: 0
                )
            ]
            dispersionPrograms = [
                ControllerNavigationOrbDispersionProgram(
                    startTime: now,
                    initialValue: dispersion,
                    targetValue: dispersion,
                    duration: 0
                )
            ]
        }
        .onChange(of: speedMultiplier) { _, newValue in
            guard phaseTimeOverride == nil else { return }
            let now = Date.timeIntervalSinceReferenceDate
            speedPrograms.append(
                ControllerNavigationOrbSpeedProgram(
                    startTime: now,
                    initialState: speedState(at: now),
                    targetMultiplier: newValue,
                    duration: reduceMotion ? 0 : 0.44
                )
            )
            trimPrograms(&speedPrograms, limit: 32)
        }
        .onChange(of: dispersion) { _, newValue in
            guard dispersionOverride == nil else { return }
            let now = Date.timeIntervalSinceReferenceDate
            dispersionPrograms.append(
                ControllerNavigationOrbDispersionProgram(
                    startTime: now,
                    initialValue: dispersionValue(at: now),
                    targetValue: newValue,
                    duration: reduceMotion ? 0 : 0.68
                )
            )
            trimPrograms(&dispersionPrograms, limit: 32)
        }
        .onChange(of: reaction) { _, newValue in
            guard reactionStateOverride == nil,
                  let newValue,
                  !reduceMotion else { return }
            let now = Date.timeIntervalSinceReferenceDate
            let current = reactionState(at: now)
            reactionPrograms.append(
                ControllerNavigationOrbReactionProgram(
                    reaction: newValue,
                    startTime: now,
                    initialPhaseTurns: current.phaseTurns,
                    initialDispersion: current.dispersion,
                    initialScale: current.scale
                )
            )
            trimPrograms(&reactionPrograms, limit: 32)
        }
    }

    @ViewBuilder
    private func orbLayer(time: TimeInterval, size: CGSize) -> some View {
        if #available(iOS 26.0, *), style == .liquidGlass {
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
        ZStack {
            ForEach(particles) { particle in
                let reactionState = resolvedReactionState(at: time)
                let offset = orbOffset(
                    particle,
                    time: time,
                    size: size
                )
                ControllerNavigationGlassOrb(
                    color: particleColor(particle.colorIndex),
                    diameter: particle.diameter * orbScale * reactionState.scale,
                    usesGlass: usesGlass,
                    isPlain: style == .plain
                )
                .offset(offset)
            }
        }
        .frame(width: size.width, height: size.height)
        .animation(.easeInOut(duration: 0.16), value: pressedColor)
        .animation(.easeInOut(duration: 0.22), value: primaryColor)
        .animation(.easeInOut(duration: 0.22), value: accentColor)
    }

    private func drawTrails(
        in context: inout GraphicsContext,
        size: CGSize,
        time: TimeInterval
    ) {
        guard !reduceMotion, showsTrails else { return }

        let reactionState = resolvedReactionState(at: time)
        let sampleCount = 9 + Int((reactionState.trailBoost * 5).rounded())

        for particle in particles {
            let color = particleColor(particle.colorIndex)
            let points = trailPoints(
                for: particle,
                time: time,
                size: size,
                sampleCount: sampleCount
            )
            guard points.count > 1 else { continue }

            for index in 0..<(points.count - 1) {
                let fade = 1 - Double(index) / Double(points.count - 1)
                let segment = ControllerNavigationOrbTrail.segment(
                    points: points,
                    index: index,
                    maximumTangentLength: 18 * orbScale
                )

                context.stroke(
                    segment,
                    with: .color(color.opacity(fade * 0.14)),
                    style: StrokeStyle(
                        lineWidth: (4.5 + reactionState.trailBoost * 2.5) * orbScale,
                        lineCap: .round
                    )
                )
                context.stroke(
                    segment,
                    with: .color(color.opacity(fade * 0.88)),
                    style: StrokeStyle(
                        lineWidth: (0.55 + fade * 1.05) * orbScale,
                        lineCap: .round
                    )
                )
            }
        }
    }

    private func trailPoints(
        for particle: ControllerNavigationOrbParticle,
        time: TimeInterval,
        size: CGSize,
        sampleCount: Int
    ) -> [CGPoint] {
        var points: [CGPoint] = []
        for index in 0..<sampleCount {
            let sampleTime = time
                - Double(index) * controllerNavigationOrbTrailSampleInterval
            let offset = orbOffset(
                particle,
                time: sampleTime,
                size: size
            )
            points.append(CGPoint(
                x: size.width / 2 + offset.width,
                y: size.height / 2 + offset.height
            ))
        }
        return points
    }

    private func orbOffset(
        _ particle: ControllerNavigationOrbParticle,
        time: TimeInterval,
        size: CGSize
    ) -> CGSize {
        let reactionState = resolvedReactionState(at: time)
        let diameter = particle.diameter * orbScale * reactionState.scale
        let resolvedDispersion = dispersionOverride
            ?? dispersionValue(at: time)
            * reactionState.dispersion
        let radiusX = max(2, size.width / 2 - diameter / 2 - inset)
            * particle.radiusX * resolvedDispersion
        let radiusY = max(2, size.height / 2 - diameter / 2 - inset)
            * particle.radiusY * resolvedDispersion
        let animationTime = reduceMotion
            ? 0
            : (phaseTimeOverride
                ?? speedState(at: time).phaseTime)
        let angle = animationTime * particle.speed * particle.direction
            + particle.phase
            + reactionState.phaseTurns * .pi * 2 * particle.direction
        let wobble = particle.wobble * orbScale

        switch pathShape {
        case .ellipse:
            return CGSize(
                width: cos(angle) * radiusX
                    + sin(angle * particle.wobbleFrequency + particle.phase) * wobble,
                height: sin(angle) * radiusY
                    + cos(angle * (particle.wobbleFrequency + 0.31)) * wobble
            )
        case .roundedRectangle:
            return controllerNavigationRoundedRectangleOffset(
                angle: angle,
                radiusX: radiusX,
                radiusY: radiusY
            )
        }
    }

    private func particleColor(_ index: Int) -> Color {
        if let pressedColor {
            return pressedColor
        }
        if !paletteColors.isEmpty {
            return paletteColors[index % paletteColors.count]
        }
        switch index % 4 {
        case 0: return primaryColor
        case 1: return accentColor
        case 2: return .cyan
        default: return .white
        }
    }

    private func speedState(at time: TimeInterval) -> ControllerNavigationOrbSpeedState {
        for program in speedPrograms.reversed() where time >= program.startTime {
            return program.state(at: time)
        }
        return ControllerNavigationOrbSpeedState(
            phaseTime: time,
            multiplier: speedMultiplier
        )
    }

    private func dispersionValue(at time: TimeInterval) -> CGFloat {
        for program in dispersionPrograms.reversed() where time >= program.startTime {
            return program.value(at: time)
        }
        return dispersion
    }

    private func reactionState(at time: TimeInterval) -> ControllerNavigationOrbReactionState {
        for program in reactionPrograms.reversed() where time >= program.startTime {
            return program.state(at: time)
        }
        return .idle
    }

    private func resolvedReactionState(
        at time: TimeInterval
    ) -> ControllerNavigationOrbReactionState {
        reactionStateOverride ?? reactionState(at: time)
    }

    private func trimPrograms<T>(_ programs: inout [T], limit: Int) {
        if programs.count > limit {
            programs.removeFirst(programs.count - limit)
        }
    }
}

private struct ControllerNavigationGlassOrb: View {
    let color: Color
    let diameter: CGFloat
    let usesGlass: Bool
    let isPlain: Bool

    private var core: some View {
        Circle()
            .fill(.white.opacity(isPlain ? 0.16 : 0.08))
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
        } else if isPlain {
            core
                .overlay {
                    Circle().fill(color.opacity(0.32))
                }
                .opacity(0.9)
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

struct ControllerNavigationOrbParticle: Identifiable, Sendable {
    let id: Int
    let radiusX: CGFloat
    let radiusY: CGFloat
    let speed: Double
    let direction: Double
    let phase: Double
    let wobble: CGFloat
    let wobbleFrequency: Double
    let diameter: CGFloat
    let colorIndex: Int

    /// Deterministic equivalents of OrbitKeys' four randomly seeded particles.
    /// Stable values prevent visible reseeding when focus enters a lazy card.
    static let standard: [ControllerNavigationOrbParticle] = [
        .init(
            id: 0, radiusX: 0.96, radiusY: 0.9, speed: 1.24,
            direction: 1, phase: 0.18, wobble: 1.8,
            wobbleFrequency: 1.46, diameter: 8, colorIndex: 0
        ),
        .init(
            id: 1, radiusX: 0.88, radiusY: 1, speed: 1.58,
            direction: -1, phase: 1.72, wobble: 2.6,
            wobbleFrequency: 1.88, diameter: 11, colorIndex: 1
        ),
        .init(
            id: 2, radiusX: 1, radiusY: 0.84, speed: 1.06,
            direction: 1, phase: 3.42, wobble: 2.1,
            wobbleFrequency: 2.22, diameter: 7, colorIndex: 2
        ),
        .init(
            id: 3, radiusX: 0.92, radiusY: 0.94, speed: 1.76,
            direction: -1, phase: 5.08, wobble: 1.5,
            wobbleFrequency: 1.31, diameter: 9, colorIndex: 3
        ),
    ]
}

enum ControllerNavigationOrbTrail {
    static func segment(
        points: [CGPoint],
        index: Int,
        maximumTangentLength: CGFloat
    ) -> Path {
        let start = points[index]
        let end = points[index + 1]
        let startTangent = tangent(
            points: points,
            index: index,
            maximumLength: maximumTangentLength
        )
        let endTangent = tangent(
            points: points,
            index: index + 1,
            maximumLength: maximumTangentLength
        )

        var path = Path()
        path.move(to: start)
        path.addCurve(
            to: end,
            control1: CGPoint(
                x: start.x + startTangent.dx / 3,
                y: start.y + startTangent.dy / 3
            ),
            control2: CGPoint(
                x: end.x - endTangent.dx / 3,
                y: end.y - endTangent.dy / 3
            )
        )
        return path
    }

    private static func tangent(
        points: [CGPoint],
        index: Int,
        maximumLength: CGFloat
    ) -> CGVector {
        let previous = points[max(0, index - 1)]
        let following = points[min(points.count - 1, index + 1)]
        let raw = CGVector(
            dx: following.x - previous.x,
            dy: following.y - previous.y
        )
        let rawLength = hypot(raw.dx, raw.dy)
        guard rawLength > 0.001 else { return .zero }

        let scale = min(1, maximumLength / rawLength)
        return CGVector(dx: raw.dx * scale, dy: raw.dy * scale)
    }
}
