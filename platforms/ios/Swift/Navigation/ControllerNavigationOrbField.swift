// ControllerNavigationOrbField.swift - OrbitKeys-inspired controller focus field
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct UIAccentColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.blue
}

private struct UISecondaryTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UITitleTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIContentTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UITabTitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UITabSubtitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UIBottomTabBarColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.blue
}

private struct UIBottomTabBarUnselectedColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UICardTitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UICardSubtitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UIContextMenuColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIContextMenuSecondaryColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UIContextMenuFocusedColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIImportActionColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIToolbarColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.blue
}

private struct UICriticalTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.red
}

private struct ControllerNavigationDepthEffectEnabledEnvironmentKey:
    EnvironmentKey {
    static let defaultValue = true
}

private struct ControllerFocusDepthHandledByAncestorEnvironmentKey:
    EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var uiAccentColour: Color {
        get { self[UIAccentColourEnvironmentKey.self] }
        set { self[UIAccentColourEnvironmentKey.self] = newValue }
    }

    var uiSecondaryTextColour: Color {
        get { self[UISecondaryTextColourEnvironmentKey.self] }
        set { self[UISecondaryTextColourEnvironmentKey.self] = newValue }
    }

    var uiTitleTextColour: Color {
        get { self[UITitleTextColourEnvironmentKey.self] }
        set { self[UITitleTextColourEnvironmentKey.self] = newValue }
    }

    var uiContentTextColour: Color {
        get { self[UIContentTextColourEnvironmentKey.self] }
        set { self[UIContentTextColourEnvironmentKey.self] = newValue }
    }

    var uiTabTitleColour: Color {
        get { self[UITabTitleColourEnvironmentKey.self] }
        set { self[UITabTitleColourEnvironmentKey.self] = newValue }
    }

    var uiTabSubtitleColour: Color {
        get { self[UITabSubtitleColourEnvironmentKey.self] }
        set { self[UITabSubtitleColourEnvironmentKey.self] = newValue }
    }

    var uiBottomTabBarColour: Color {
        get { self[UIBottomTabBarColourEnvironmentKey.self] }
        set { self[UIBottomTabBarColourEnvironmentKey.self] = newValue }
    }

    var uiBottomTabBarUnselectedColour: Color {
        get { self[UIBottomTabBarUnselectedColourEnvironmentKey.self] }
        set { self[UIBottomTabBarUnselectedColourEnvironmentKey.self] = newValue }
    }

    var uiCardTitleColour: Color {
        get { self[UICardTitleColourEnvironmentKey.self] }
        set { self[UICardTitleColourEnvironmentKey.self] = newValue }
    }

    var uiCardSubtitleColour: Color {
        get { self[UICardSubtitleColourEnvironmentKey.self] }
        set { self[UICardSubtitleColourEnvironmentKey.self] = newValue }
    }

    var uiContextMenuColour: Color {
        get { self[UIContextMenuColourEnvironmentKey.self] }
        set { self[UIContextMenuColourEnvironmentKey.self] = newValue }
    }

    var uiContextMenuSecondaryColour: Color {
        get { self[UIContextMenuSecondaryColourEnvironmentKey.self] }
        set { self[UIContextMenuSecondaryColourEnvironmentKey.self] = newValue }
    }

    var uiContextMenuFocusedColour: Color {
        get { self[UIContextMenuFocusedColourEnvironmentKey.self] }
        set { self[UIContextMenuFocusedColourEnvironmentKey.self] = newValue }
    }

    var uiImportActionColour: Color {
        get { self[UIImportActionColourEnvironmentKey.self] }
        set { self[UIImportActionColourEnvironmentKey.self] = newValue }
    }

    var uiToolbarColour: Color {
        get { self[UIToolbarColourEnvironmentKey.self] }
        set { self[UIToolbarColourEnvironmentKey.self] = newValue }
    }

    var uiCriticalTextColour: Color {
        get { self[UICriticalTextColourEnvironmentKey.self] }
        set { self[UICriticalTextColourEnvironmentKey.self] = newValue }
    }

    var controllerNavigationDepthEffectEnabled: Bool {
        get { self[ControllerNavigationDepthEffectEnabledEnvironmentKey.self] }
        set { self[ControllerNavigationDepthEffectEnabledEnvironmentKey.self] = newValue }
    }

    var controllerFocusDepthHandledByAncestor: Bool {
        get { self[ControllerFocusDepthHandledByAncestorEnvironmentKey.self] }
        set { self[ControllerFocusDepthHandledByAncestorEnvironmentKey.self] = newValue }
    }
}

/// Preserves the master UI's semantic split: label copy inherits its normal
/// primary/secondary colour while symbols use the active theme accent.
struct UIAccentIconLabelStyle: LabelStyle {
    @Environment(\.uiAccentColour) private var accentColour

    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 8) {
            configuration.icon
                .symbolRenderingMode(.monochrome)
                .foregroundStyle(accentColour)
            configuration.title
        }
    }
}

private struct UICriticalForegroundStyleModifier: ViewModifier {
    @Environment(\.uiCriticalTextColour) private var criticalTextColour

    func body(content: Content) -> some View {
        content.foregroundStyle(criticalTextColour)
    }
}

extension View {
    /// Keeps destructive, error, and other critical labels independent from
    /// the user's ordinary and secondary text palettes.
    func uiCriticalForegroundStyle() -> some View {
        modifier(UICriticalForegroundStyleModifier())
    }
}

struct ControllerTextAppearance {
    var normalColor: Color?
    var secondaryColor: Color?
    var titleColor: Color
    var contentColor: Color?
    var unselectedColor: Color
    var focusedColor: Color
    var normalShadowColor: Color
    var focusedShadowColor: Color
    var normalShadowStrength: Double
    var focusedShadowStrength: Double

    static let standard = ControllerTextAppearance(
        normalColor: nil,
        secondaryColor: nil,
        titleColor: .primary,
        contentColor: nil,
        unselectedColor: .secondary,
        focusedColor: .blue,
        normalShadowColor: .black,
        focusedShadowColor: .black,
        normalShadowStrength: 0,
        focusedShadowStrength: 0.1
    )
}

private struct UISecondaryForegroundStyleModifier: ViewModifier {
    let color: Color?

    func body(content: Content) -> some View {
        // Keep one view identity when a theme changes between the system
        // foreground and an explicit palette colour. A conditional branch here
        // used to replace RootView's complete subtree; the retiring branch then
        // ran its lifecycle cleanup after the replacement appeared and stopped
        // controller delivery while the menu was still visible.
        content.foregroundColor(color)
    }
}

extension View {
    /// Colours interface labels and symbols that do not own an accent or
    /// controller-focus style. Keeping this optional preserves system defaults
    /// when Secondary Text Colour is disabled.
    func uiSecondaryForegroundStyle(_ color: Color?) -> some View {
        modifier(UISecondaryForegroundStyleModifier(color: color))
    }
}

private struct ControllerTextAppearanceEnvironmentKey: EnvironmentKey {
    static let defaultValue = ControllerTextAppearance.standard
}

extension EnvironmentValues {
    var controllerTextAppearance: ControllerTextAppearance {
        get { self[ControllerTextAppearanceEnvironmentKey.self] }
        set { self[ControllerTextAppearanceEnvironmentKey.self] = newValue }
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

private struct ControllerNavigationOrbSpeedState {
    let phaseTime: TimeInterval
    let multiplier: Double
}

private struct ControllerNavigationOrbSpeedProgram {
    let startTime: TimeInterval
    let initialState: ControllerNavigationOrbSpeedState
    let targetMultiplier: Double
    let duration: TimeInterval

    func state(at time: TimeInterval) -> ControllerNavigationOrbSpeedState {
        let elapsed = max(0, time - startTime)
        guard duration > 0 else {
            return ControllerNavigationOrbSpeedState(
                phaseTime: initialState.phaseTime + targetMultiplier * elapsed,
                multiplier: targetMultiplier
            )
        }

        let transitionElapsed = min(elapsed, duration)
        let progress = transitionElapsed / duration
        let curve = ControllerNavigationOrbMotionCurve.value(
            progress,
            control1: 0.1,
            control2: 0.9
        )
        let integratedCurve = ControllerNavigationOrbMotionCurve.integral(
            progress,
            control1: 0.1,
            control2: 0.9
        )
        let multiplierDelta = targetMultiplier - initialState.multiplier
        var phaseTime = initialState.phaseTime
            + duration * (
                initialState.multiplier * progress
                    + multiplierDelta * integratedCurve
            )
        if elapsed > duration {
            phaseTime += targetMultiplier * (elapsed - duration)
        }
        return ControllerNavigationOrbSpeedState(
            phaseTime: phaseTime,
            multiplier: initialState.multiplier + multiplierDelta * curve
        )
    }
}

private struct ControllerNavigationOrbDispersionProgram {
    let startTime: TimeInterval
    let initialValue: CGFloat
    let targetValue: CGFloat
    let duration: TimeInterval

    func value(at time: TimeInterval) -> CGFloat {
        guard duration > 0 else { return targetValue }
        let linearProgress = min(max((time - startTime) / duration, 0), 1)
        let progress = ControllerNavigationOrbMotionCurve.value(
            linearProgress,
            control1: 0.06,
            control2: 0.88
        )
        return initialValue
            + (targetValue - initialValue) * CGFloat(progress)
    }
}

struct ControllerNavigationOrbReactionState {
    let phaseTurns: Double
    let dispersion: CGFloat
    let scale: CGFloat
    let trailBoost: CGFloat

    static let idle = ControllerNavigationOrbReactionState(
        phaseTurns: 0,
        dispersion: 1,
        scale: 1,
        trailBoost: 0
    )
}

private struct ControllerNavigationOrbReactionProgram {
    let reaction: ControllerNavigationOrbReaction
    let startTime: TimeInterval
    let initialPhaseTurns: Double
    let initialDispersion: CGFloat
    let initialScale: CGFloat

    func state(at time: TimeInterval) -> ControllerNavigationOrbReactionState {
        let profile = profile
        let progress = min(max((time - startTime) / profile.duration, 0), 1)
        let phaseProgress = ControllerNavigationOrbMotionCurve.value(
            progress,
            control1: 0.08,
            control2: 0.92
        )
        let peakProgress = 0.32
        let dispersion: CGFloat
        let scale: CGFloat
        if progress < peakProgress {
            let rise = ControllerNavigationOrbMotionCurve.value(
                progress / peakProgress,
                control1: 0.08,
                control2: 0.9
            )
            dispersion = interpolate(
                initialDispersion,
                profile.peakDispersion,
                rise
            )
            scale = interpolate(initialScale, profile.peakScale, rise)
        } else {
            let fall = ControllerNavigationOrbMotionCurve.value(
                (progress - peakProgress) / (1 - peakProgress),
                control1: 0.08,
                control2: 0.88
            )
            dispersion = interpolate(profile.peakDispersion, 1, fall)
            scale = interpolate(profile.peakScale, 1, fall)
        }
        return ControllerNavigationOrbReactionState(
            phaseTurns: initialPhaseTurns + profile.turns * phaseProgress,
            dispersion: dispersion,
            scale: scale,
            trailBoost: CGFloat(sin(.pi * progress)) * profile.trailBoost
        )
    }

    private var profile: Profile {
        switch reaction.kind {
        case .activate:
            return Profile(
                duration: 0.68,
                turns: 0.78,
                peakDispersion: 0.62,
                peakScale: 1.34,
                trailBoost: 1
            )
        case .back:
            return Profile(
                duration: 0.64,
                turns: -0.58,
                peakDispersion: 1.42,
                peakScale: 0.88,
                trailBoost: 0.88
            )
        case .contextMenu:
            return Profile(
                duration: 0.82,
                turns: 1.12,
                peakDispersion: 1.56,
                peakScale: 1.16,
                trailBoost: 1.15
            )
        case .favorite:
            return Profile(
                duration: 0.86,
                turns: 1.34,
                peakDispersion: 1.38,
                peakScale: 1.28,
                trailBoost: 1.1
            )
        case .tab:
            return Profile(
                duration: 0.58,
                turns: 0.46,
                peakDispersion: 1.2,
                peakScale: 1.08,
                trailBoost: 0.72
            )
        }
    }

    private func interpolate(
        _ start: CGFloat,
        _ end: CGFloat,
        _ progress: Double
    ) -> CGFloat {
        start + (end - start) * CGFloat(progress)
    }

    private struct Profile {
        let duration: TimeInterval
        let turns: Double
        let peakDispersion: CGFloat
        let peakScale: CGFloat
        let trailBoost: CGFloat
    }
}

/// Preserve the long OrbitKeys trail while sampling it sparsely enough for the
/// particle field to remain decorative rather than consuming the focus budget.
private let controllerNavigationOrbTrailLengthMultiplier = 3
private let controllerNavigationOrbTrailSampleInterval = 0.028
    * Double(controllerNavigationOrbTrailLengthMultiplier)
private let controllerNavigationOrbTransitionHistoryDuration: TimeInterval = 1.5

private enum ControllerNavigationOrbMotionCurve {
    static func value(
        _ progress: Double,
        control1: Double,
        control2: Double
    ) -> Double {
        let value = min(max(progress, 0), 1)
        let inverse = 1 - value
        return 3 * inverse * inverse * value * control1
            + 3 * inverse * value * value * control2
            + value * value * value
    }

    static func integral(
        _ progress: Double,
        control1: Double,
        control2: Double
    ) -> Double {
        let value = min(max(progress, 0), 1)
        return 1.5 * control1 * value * value
            + (-2 * control1 + control2) * value * value * value
            + (0.75 * control1 - 0.75 * control2 + 0.25)
                * value * value * value * value
    }
}

enum ControllerNavigationOrbPathShape {
    case ellipse
    case roundedRectangle
}

/// Returns a constant-direction point on a rounded rectangle centred at zero.
/// Game-card focus uses this perimeter instead of the generic elliptical orbit,
/// keeping the particles aligned with the card's actual silhouette.
private func controllerNavigationRoundedRectangleOffset(
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

private struct ControllerNavigationOrbAnchorTarget {
    let id: String
    let bounds: Anchor<CGRect>
    let palette: ControllerNavigationOrbPalette
    let style: ControllerNavigationOrbStyle
    let inset: CGFloat
    let orbScale: CGFloat
    let segmentIndex: Int?
    let segmentCount: Int?
    let priority: Int
}

private struct ControllerNavigationOrbAnchorPreferenceKey: PreferenceKey {
    static let defaultValue: ControllerNavigationOrbAnchorTarget? = nil

    static func reduce(
        value: inout ControllerNavigationOrbAnchorTarget?,
        nextValue: () -> ControllerNavigationOrbAnchorTarget?
    ) {
        guard let next = nextValue() else { return }
        if value == nil || next.priority >= (value?.priority ?? .min) {
            value = next
        }
    }
}

private struct ControllerNavigationResolvedOrbTarget: Equatable {
    let id: String
    let frame: CGRect
    let palette: ControllerNavigationOrbPalette
    let style: ControllerNavigationOrbStyle
    let inset: CGFloat
    let orbScale: CGFloat
}

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
    let focusOrbsEnabled: Bool
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
    var cardFocused: Color? = nil
    var contextPrimary: Color
    var contextSecondary: Color
    var contextFocused: Color
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
                secondary: .white.opacity(0.72),
                cardFocused: rgb(0x57B8F9)
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
            cardFocused: focused ?? primary,
            contextPrimary: overlayPrimary,
            contextSecondary: overlaySecondary,
            contextFocused: focused ?? overlayPrimary,
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
            contextFocused: overlayPrimary,
            quickMenuPrimary: overlayPrimary,
            quickMenuSecondary: overlaySecondary,
            quickMenuFocused: overlayPrimary
        )
    }

    private func darkReadability(
        primary: Color,
        secondary: Color,
        cardFocused: Color? = nil
    ) -> ControllerUIThemeReadabilityOverrides {
        ControllerUIThemeReadabilityOverrides(
            secondary: secondary,
            tabSubtitle: secondary,
            cardTitle: primary,
            cardSubtitle: secondary,
            cardFocused: cardFocused ?? primary,
            contextPrimary: primary,
            contextSecondary: secondary,
            contextFocused: primary,
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
            contextFocused: primary,
            quickMenuPrimary: primary,
            quickMenuSecondary: secondary,
            quickMenuFocused: primary
        )
    }

    private func rgb(_ hex: UInt32) -> Color {
        Color(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255
        )
    }

    var configuration: ControllerUIThemeConfiguration? {
        switch self {
        case .custom:
            return nil
        case .defaultTheme:
            // Default mirrors Heny Blue's complete interface configuration;
            // only its dynamic wallpaper differs.
            return theme(.henyBlue, .neonBlue, .henyBlue, .henyBlue, 1, 1, true)
        case .neon:
            return theme(.blue, .neonBlue, .blue, .blue, 1, 1, true)
        case .purpleMilk:
            return theme(
                .henyBlue,
                .animatedWaves,
                .aurora,
                .platinumGrey,
                0.5,
                0.85,
                false
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
                1.45,
                false
            )
        case .henyBlue:
            return theme(.henyBlue, .neonBlue, .henyBlue, .henyBlue, 1, 1, true)
        case .liveSilver:
            return theme(.silver, .softGlow, .platinumGrey, .silver, 0.8, 1.2, true)
        case .gold:
            return theme(.gold, .doubleNeon, .gold, .gold, 0.9, 1.3, true)
        case .diamond:
            return theme(.arcticIce, .prism, .platinumGrey, .arcticIce, 0.8, 1.45, true)
        case .galaxy:
            return theme(.violet, .animatedGradient, .multicolor, .xmbAmethystGlow, 0.7, 1.35, true)
        case .blackHole:
            return theme(.obsidian, .scanline, .obsidian, .xmbLunarGraphite, 0.55, 1.55, true)
        case .sakura:
            return theme(.pink, .pinkSakura, .xmbSakuraBloom, .xmbSakuraBloom, 0.75, 1.15, true)
        case .cherry:
            return theme(.hotMagenta, .dottedPulse, .xmbWinterCoral, .hotMagenta, 1.05, 1.25, true)
        case .bloodDragon:
            return theme(.crimson, .redBlood, .xmbCinderRed, .xmbCinderRed, 1.2, 1.55, true)
        case .redBlood:
            return theme(.crimson, .redBlood, .burgundy, .crimson, 0.9, 1.4, true)
        case .cyberGreen:
            return theme(.neonGreen, .scanline, .neonGreen, .electricLime, 1.25, 1.45, true)
        case .ocean:
            return theme(.cyan, .oceanBlue, .xmbOceanMidnight, .xmbTurquoiseLagoon, 0.7, 1.2, true)
        case .arctic:
            return theme(.arcticIce, .stillWaves, .arcticIce, .platinumGrey, 0.65, 1.15, true)
        case .aurora:
            return theme(.aurora, .animatedGradient, .aurora, .aurora, 0.75, 1.35, true)
        case .synthwave:
            return theme(.hotMagenta, .animatedWaves, .multicolor, .violet, 1.15, 1.4, true)
        case .plasma:
            return theme(.plasmaOrange, .cometTrail, .plasmaOrange, .copper, 1.3, 1.45, true)
        case .ultraviolet:
            return theme(.violet, .rainbowNeon, .xmbAmethystGlow, .xmbPurpleEclipse, 0.85, 1.3, true)
        case .roseGold:
            return theme(.pink, .doubleNeon, .xmbSakuraBloom, .gold, 0.8, 1.25, true)
        case .emerald:
            return theme(.emerald, .stillWaves, .emerald, .deepTeal, 0.8, 1.2, true)
        case .obsidian:
            return theme(.gunmetalGrey, .softGlow, .obsidian, .gunmetalGrey, 0.6, 1.35, true)
        case .copper:
            return theme(.copper, .classicFrame, .copper, .desertSand, 0.7, 1.15, true)
        case .classicPS1:
            return theme(.ps1Grey, .minimal, .ps1Grey, .platinumGrey, 0.5, 0.85, false)
        case .classicPS2:
            return theme(.ps2Brown, .classicFrame, .ps2Brown, .gunmetalGrey, 0.5, 0.95, false)
        case .sunset:
            return theme(.sunset, .animatedGradient, .sunset, .plasmaOrange, 0.75, 1.25, true)
        case .midnight:
            return theme(.midnight, .softGlow, .midnight, .navy, 0.65, 1.3, true)
        case .royal:
            return theme(.violet, .doubleNeon, .violet, .gold, 0.8, 1.35, true)
        case .electricLime:
            return theme(.electricLime, .dottedPulse, .electricLime, .neonGreen, 1.1, 1.3, true)
        case .frostedPearl:
            return theme(.xmbFrostedPearl, .prism, .xmbFrostedPearl, .silver, 0.65, 1.1, true)
        case .eclipse:
            return theme(.xmbPurpleEclipse, .glowingCorners, .xmbPurpleEclipse, .obsidian, 0.7, 1.45, true)
        case .rainbow:
            return theme(.cyan, .animatedGlowingRainbowNeon, .multicolor, .multicolor, 0.9, 1.4, true)
        case .comet:
            return theme(.arcticIce, .cometTrail, .aurora, .arcticIce, 1.25, 1.35, true)
        case .scanline:
            return theme(.emerald, .scanline, .graphite, .neonGreen, 1, 1.1, false)
        case .minimal:
            return theme(.silver, .minimal, .silver, .silver, 0.5, 0.65, false)
        case .pastelSakura:
            return theme(.xmbSakuraBloom, .softGlow, .pink, .xmbSakuraBloom, 0.55, 0.82, false)
        case .pastelLavender:
            return theme(.lavender, .softGlow, .xmbOrchidHaze, .lavender, 0.52, 0.8, false)
        case .pastelMint:
            return theme(.xmbSpringMeadow, .stillWaves, .emerald, .xmbSpringMeadow, 0.5, 0.76, false)
        case .pastelOcean:
            return theme(.xmbTurquoiseLagoon, .stillWaves, .arcticIce, .cyan, 0.55, 0.82, false)
        case .pastelSunrise:
            return theme(.desertSand, .gradient, .xmbWinterCoral, .gold, 0.5, 0.78, false)
        case .minimalSnow:
            return theme(.xmbFrostedPearl, .minimal, .silver, .xmbFrostedPearl, 0.38, 0.52, false)
        case .minimalGraphite:
            return theme(.graphite, .minimal, .platinumGrey, .graphite, 0.4, 0.5, false)
        case .minimalMidnight:
            return theme(.midnight, .minimal, .blue, .navy, 0.38, 0.54, false)
        case .minimalSand:
            return theme(.desertSand, .minimal, .copper, .desertSand, 0.36, 0.5, false)
        case .minimalOcean:
            return theme(.deepTeal, .minimal, .cyan, .navy, 0.4, 0.54, false)
        case .ambientSpectrum:
            return theme(.violet, .animatedGradient, .multicolor, .aurora, 0.82, 1.15, false)
        case .lightSpeedNebula:
            return theme(.cyan, .cometTrail, .aurora, .arcticIce, 1.2, 1.28, false)
        case .spatialRetroGrid:
            return theme(.ps1Grey, .classicFrame, .platinumGrey, .ps1Grey, 0.62, 0.82, false)
        case .orbitalTowers:
            return theme(.blue, .doubleNeon, .cyan, .blue, 0.72, 1.08, false)
        case .faceButtonDream:
            return theme(.blue, .dottedPulse, .cyan, .blue, 0.88, 1.08, false)
        case .portableGlass:
            return theme(.arcticIce, .stillWaves, .xmbFrostedPearl, .cyan, 0.56, 0.82, false)
        case .splineFlow:
            return theme(.violet, .animatedWaves, .xmbAmethystGlow, .violet, 0.72, 1.04, false)
        case .particleWave:
            return theme(.blue, .particles, .cyan, .arcticIce, 0.88, 1.12, false)
        case .oceanWave:
            return theme(.cyan, .oceanBlue, .xmbTurquoiseLagoon, .blue, 0.68, 1.0, false)
        case .luminousRibbons:
            return theme(.gold, .animatedGradient, .xmbGildedSun, .gold, 0.7, 1.08, false)
        case .frutigerAero:
            return theme(.cyan, .aquaBubble, .arcticIce, .xmbSpringMeadow, 0.72, 1.18, false)
        case .frutigerEco:
            return theme(.emerald, .ecoPulse, .neonGreen, .xmbSpringMeadow, 0.62, 1.02, false)
        case .darkAero:
            return theme(.cyan, .darkAeroOrbit, .xmbTurquoiseLagoon, .midnight, 0.68, 1.30, false)
        case .technozen:
            return theme(.deepTeal, .zenBreath, .xmbSpringMeadow, .silver, 0.42, 0.78, false)
        case .dorfic:
            return theme(.copper, .rusticStitch, .desertSand, .ps2Brown, 0.48, 0.82, false)
        case .frutigerMetro:
            return theme(.blue, .metroSweep, .hotMagenta, .gold, 0.88, 1.08, false)
        case .mcBling:
            return theme(.hotMagenta, .rhinestone, .pink, .gold, 0.92, 1.24, false)
        case .vectorbloom:
            return theme(.pink, .vectorBloom, .xmbSpringMeadow, .hotMagenta, 0.68, 1.02, false)
        case .skeuomorphicAqua:
            return theme(.arcticIce, .skeuomorphicBevel, .cyan, .blue, 0.50, 0.96, false)
        case .y2kChrome:
            return theme(.silver, .skeuomorphicBevel, .platinumGrey, .arcticIce, 0.44, 0.92, false)
        case .aquaWeb:
            return theme(.xmbAzureHorizon, .aquaBubble, .cyan, .blue, 0.82, 1.12, false)
        case .bubblegumTech:
            return theme(.pink, .rhinestone, .hotMagenta, .xmbSakuraBloom, 0.84, 1.16, false)
        case .digitalMeadow:
            return theme(.xmbSpringMeadow, .ecoPulse, .emerald, .electricLime, 0.74, 1.08, false)
        case .glassGarden:
            return theme(.emerald, .vectorBloom, .xmbSpringMeadow, .cyan, 0.56, 0.98, false)
        case .zenGlass:
            return theme(.silver, .zenBreath, .deepTeal, .arcticIce, 0.36, 0.70, false)
        case .metroNight:
            return theme(.violet, .metroSweep, .cyan, .hotMagenta, 0.96, 1.18, false)
        }
    }

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

    private func theme(
        _ accent: ThemePalette,
        _ style: ControllerFocusBoxStyle,
        _ focusBox: ThemePalette,
        _ orbs: ThemePalette,
        _ speed: Double,
        _ glow: Double,
        _ showsOrbs: Bool
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
            glowIntensity: glow,
            // Presets may choose orb colours, but visibility is controlled
            // only by the manual Focus Orbs toggle.
            focusOrbsEnabled: false
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

/// One visual implementation for controller focus across Forms, libraries,
/// alerts, toolbars, and the bottom navigation bar. Animated styles own only
/// this lightweight outline timeline; they never invalidate the focused row.
enum ControllerFocusBoxShape {
    case roundedRectangle
    case pill
}

private func controllerFocusCornerRadius(
    size: CGSize,
    baseline: CGFloat,
    shape: ControllerFocusBoxShape
) -> CGFloat {
    guard size.width > 0, size.height > 0 else { return baseline }
    let halfHeight = size.height / 2
    switch shape {
    case .roundedRectangle:
        // Explicit rectangular surfaces retain a stable continuous radius.
        return min(halfHeight, max(6, baseline))
    case .pill:
        if size.width >= size.height * 1.6 {
            return halfHeight
        }
        // Tall cards must never become vertical ovals. They retain a softened
        // continuous rectangle while horizontal controls become true pills.
        return min(
            halfHeight,
            max(baseline, min(22, size.height * 0.28))
        )
    }
}

struct ControllerFocusBox: View {
    var cornerRadius: CGFloat = 12
    var performanceOptimized = false
    var animatesArtwork = true
    var shape: ControllerFocusBoxShape = .pill

    @State private var settings = SettingsStore.shared
    @State private var gameCoverThemePreview = GameCoverThemePreviewStore.shared
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        GeometryReader { proxy in
            AdaptiveAnimationTimeline(
                domain: .controllerEffects,
                paused: reduceMotion
                    || !animatesArtwork
                    || !settings.controllerFocusBoxStyle.isAnimated
            ) { timeline in
                ControllerFocusBoxArtwork(
                    style: settings.controllerFocusBoxStyle,
                    palette: settings.controllerFocusBoxPalette,
                    customColors: gameCoverThemePreview.preview.map {
                        $0.backgroundPalette.gradientColors
                    } ?? settings.controllerFocusBoxCustomColor.map { [$0.color] },
                    animationSpeed: settings.controllerFocusBoxAnimationSpeed,
                    glowIntensity: settings.controllerFocusBoxGlowIntensity,
                    cornerRadius: controllerFocusCornerRadius(
                        size: proxy.size,
                        baseline: cornerRadius,
                        shape: shape
                    ),
                    time: reduceMotion || !animatesArtwork
                        ? 0
                        : timeline.date.timeIntervalSinceReferenceDate,
                    performanceOptimized: performanceOptimized
                )
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// The persistent bottom navigation bar intentionally uses a restrained focus
/// treatment. Theme artwork such as waves, particles, blood, or filled
/// gradients can obscure the compact tab labels, so this variant keeps only a
/// palette-aware neon outline and its glow. Other focus surfaces continue to
/// use `ControllerFocusBox` and therefore retain the selected artwork style.
struct ControllerBottomNavigationFocusGlow: View {
    var cornerRadius: CGFloat = 18

    @State private var settings = SettingsStore.shared
    @State private var gameCoverThemePreview = GameCoverThemePreviewStore.shared

    var body: some View {
        GeometryReader { proxy in
            let paletteColors = gameCoverThemePreview.preview.map {
                $0.backgroundPalette.gradientColors
            } ?? settings.controllerFocusBoxCustomColor.map { [$0.color] }
                ?? settings.controllerFocusBoxPalette.colors
            let primary = paletteColors.first ?? .blue
            let secondary = paletteColors.dropFirst().first ?? primary
            let intensity = min(
                max(settings.controllerFocusBoxGlowIntensity, 0),
                2
            )
            let shape = RoundedRectangle(
                cornerRadius: controllerFocusCornerRadius(
                    size: proxy.size,
                    baseline: cornerRadius,
                    shape: .pill
                ),
                style: .continuous
            )

            shape
                .strokeBorder(
                    LinearGradient(
                        colors: [primary, secondary, primary],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    ),
                    lineWidth: 2
                )
                .shadow(
                    color: primary.opacity(0.82 * intensity),
                    radius: CGFloat(5 * intensity)
                )
                .shadow(
                    color: secondary.opacity(0.52 * intensity),
                    radius: CGFloat(10 * intensity)
                )
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

private struct ControllerFocusBoxDepthPresentationModifier: ViewModifier {
    let isVisible: Bool
    let cornerRadius: CGFloat
    let performanceOptimized: Bool
    @Environment(\.controllerNavigationDepthEffectEnabled)
    private var depthEffectEnabled

    func body(content: Content) -> some View {
        content
            .background {
                if isVisible && depthEffectEnabled {
                    ControllerFocusBox(
                        cornerRadius: cornerRadius,
                        performanceOptimized: performanceOptimized,
                        animatesArtwork: false,
                        shape: .pill
                    )
                }
            }
    }
}

struct ControllerFocusDepthAnchorTarget {
    let id: String
    let bounds: Anchor<CGRect>
}

struct ControllerFocusDepthAnchorPreferenceKey: PreferenceKey {
    static let defaultValue: [ControllerFocusDepthAnchorTarget] = []

    static func reduce(
        value: inout [ControllerFocusDepthAnchorTarget],
        nextValue: () -> [ControllerFocusDepthAnchorTarget]
    ) {
        value.append(contentsOf: nextValue())
    }
}

private struct ControllerFocusDepthAnchorModifier: ViewModifier {
    let id: String
    let isFocused: Bool
    @Environment(\.controllerNavigationDepthEffectEnabled)
    private var depthEffectEnabled

    func body(content: Content) -> some View {
        content.anchorPreference(
            key: ControllerFocusDepthAnchorPreferenceKey.self,
            value: .bounds
        ) { anchor in
            isFocused && depthEffectEnabled
                ? [ControllerFocusDepthAnchorTarget(id: id, bounds: anchor)]
                : []
        }
    }
}

private struct ControllerFocusDepthBehindGlassModifier: ViewModifier {
    let isEnabled: Bool
    let force: Bool
    let cornerRadius: CGFloat
    @Environment(\.controllerNavigationDepthEffectEnabled)
    private var depthEffectEnabled
    @Environment(\.controllerFocusDepthHandledByAncestor)
    private var handledByAncestor
    @Environment(\.menuControllerInputRouter)
    private var controllerInput
    @Environment(\.controllerAccessibilityNavigationSession)
    private var navigationSession
    @Environment(\.uiAccentColour)
    private var accentColour

    func body(content: Content) -> some View {
        content.backgroundPreferenceValue(
            ControllerFocusDepthAnchorPreferenceKey.self
        ) { anchors in
            GeometryReader { proxy in
                if isEnabled,
                   depthEffectEnabled,
                   (force || !handledByAncestor),
                   let target = anchors.last {
                    let frame = depthFrame(for: target, in: proxy)
                    // The shared navigation session supplies the foreground
                    // presentation rectangle, including its focus inset and
                    // display-linked scroll position. Using that same input
                    // keeps the behind-glass pill locked to the foreground
                    // focus box instead of following a second raw row anchor.
                    ControllerNavigationAnimatedOrbField(
                        targetID: target.id,
                        targetFrame: frame,
                        primaryColor: accentColour,
                        style: .plain,
                        controllerInput: controllerInput,
                        showsOrbs: false,
                        showsNeonOutline: true,
                        tracksTargetFrameDirectly:
                            navigationSession?.isScrollPresentationActive
                                == true,
                        neonCornerRadius: cornerRadius,
                        animatesNeonArtwork: false,
                        neonShape: .pill
                    )
                    .opacity(
                        navigationSession?
                            .focusPresentationIsHiddenForScrolling == true
                            ? 0
                            : 1
                    )
                    .animation(
                        .easeOut(duration: 0.14),
                        value: navigationSession?
                            .focusPresentationIsHiddenForScrolling
                    )
                }
            }
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
    }

    private func depthFrame(
        for target: ControllerFocusDepthAnchorTarget,
        in proxy: GeometryProxy
    ) -> CGRect {
        guard let navigationSession,
              focusedTargetMatches(target.id, in: navigationSession),
              let foregroundFrame = navigationSession.focusedFrame else {
            return proxy[target.bounds]
        }

        // `focusedFrame` is expressed in the hosting window. SwiftUI's global
        // geometry uses the same origin, so remove this glass owner's origin
        // to obtain the exact foreground rectangle in local coordinates.
        let ownerFrame = proxy.frame(in: .global)
        return foregroundFrame.offsetBy(
            dx: -ownerFrame.minX,
            dy: -ownerFrame.minY
        )
    }

    private func focusedTargetMatches(
        _ targetID: String,
        in navigationSession: ControllerAccessibilityNavigationSession
    ) -> Bool {
        guard let focusedID = navigationSession.focusedElementID else {
            return false
        }
        return focusedID == targetID || focusedID.hasPrefix("\(targetID)#")
    }
}

private struct ControllerFocusBoxPresentationModifier: ViewModifier {
    let isVisible: Bool
    let cornerRadius: CGFloat
    let performanceOptimized: Bool

    func body(content: Content) -> some View {
        content
            .modifier(
                ControllerFocusBoxDepthPresentationModifier(
                    isVisible: isVisible,
                    cornerRadius: cornerRadius,
                    performanceOptimized: performanceOptimized
                )
            )
            .overlay {
                if isVisible {
                    ControllerFocusBox(
                        cornerRadius: cornerRadius,
                        performanceOptimized: performanceOptimized,
                        animatesArtwork: true,
                        shape: .pill
                    )
                }
            }
    }
}

private struct ControllerFocusBoxForegroundPresentationModifier:
    ViewModifier {
    let isVisible: Bool
    let cornerRadius: CGFloat
    let performanceOptimized: Bool

    func body(content: Content) -> some View {
        content.overlay {
            if isVisible {
                ControllerFocusBox(
                    cornerRadius: cornerRadius,
                    performanceOptimized: performanceOptimized,
                    animatesArtwork: true,
                    shape: .pill
                )
            }
        }
    }
}

extension View {
    func controllerFocusDepthAnchor(
        id: String,
        isFocused: Bool
    ) -> some View {
        modifier(
            ControllerFocusDepthAnchorModifier(
                id: id,
                isFocused: isFocused
            )
        )
    }

    func controllerFocusDepthBehindGlass(
        isEnabled: Bool = true,
        force: Bool = false,
        cornerRadius: CGFloat = 12
    ) -> some View {
        modifier(
            ControllerFocusDepthBehindGlassModifier(
                isEnabled: isEnabled,
                force: force,
                cornerRadius: cornerRadius
            )
        )
    }

    func controllerFocusBoxDepthPresentation(
        isVisible: Bool,
        cornerRadius: CGFloat = 12,
        performanceOptimized: Bool = false
    ) -> some View {
        modifier(
            ControllerFocusBoxDepthPresentationModifier(
                isVisible: isVisible,
                cornerRadius: cornerRadius,
                performanceOptimized: performanceOptimized
            )
        )
    }

    func controllerFocusBoxPresentation(
        isVisible: Bool,
        cornerRadius: CGFloat = 12,
        performanceOptimized: Bool = false
    ) -> some View {
        modifier(
            ControllerFocusBoxPresentationModifier(
                isVisible: isVisible,
                cornerRadius: cornerRadius,
                performanceOptimized: performanceOptimized
            )
        )
    }

    func controllerFocusBoxForegroundPresentation(
        isVisible: Bool,
        cornerRadius: CGFloat = 12,
        performanceOptimized: Bool = false
    ) -> some View {
        modifier(
            ControllerFocusBoxForegroundPresentationModifier(
                isVisible: isVisible,
                cornerRadius: cornerRadius,
                performanceOptimized: performanceOptimized
            )
        )
    }
}

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
                // One complete dash program is 20 points. Passing absolute
                // uptime here produced a multi-billion-point dash offset and
                // could stall Core Graphics as soon as this style was chosen.
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

    private enum ParticlePreset: Equatable {
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
        // The performance-optimized card path deliberately halves the
        // particle workload.
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

/// A target-rectangle transition equivalent to OrbitKeys' box-anchor program.
/// Retargeting reads `value(at:)` first, so a fast second focus move begins at
/// the actual presentation rectangle rather than the previous destination.
private struct ControllerNavigationOrbFrameTransition {
    let from: CGRect
    let to: CGRect
    let startTime: TimeInterval
    let duration: TimeInterval
    let travelStyle: ControllerNavigationFocusTravelStyle

    func value(at time: TimeInterval) -> CGRect {
        guard duration > 0 else { return to }
        return travelStyle.frame(
            from: from, to: to, progress: (time - startTime) / duration
        )
    }
}

/// Reusable presentation layer for focus systems which already know the
/// focused element's rectangle (Settings, sub-settings, tab bars, and alerts).
/// Geometry moves directly while a focus-ID change uses the selected travel
/// curve, preventing scroll-layout updates from queuing animations.
struct ControllerNavigationAnimatedOrbField: View {
    let targetID: String
    let targetFrame: CGRect
    let primaryColor: Color
    let style: ControllerNavigationOrbStyle
    let controllerInput: MenuControllerInputRouter?
    var interactionOverride: ControllerNavigationOrbInteraction? = nil
    var accentColor: Color = .white
    var inset: CGFloat = 2
    var orbScale: CGFloat = 1
    var showsOrbs = true
    var showsNeonOutline = false
    var focusTravelStyle: ControllerNavigationFocusTravelStyle? = nil
    var tracksTargetFrameDirectly = false
    var neonCornerRadius: CGFloat = 12
    var animatesNeonArtwork = true
    var neonShape: ControllerFocusBoxShape = .pill

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared
    @State private var gameCoverThemePreview = GameCoverThemePreviewStore.shared
    @State private var lastTargetID: String?
    @State private var lastResolvedTargetFrame: CGRect?
    @State private var frameTransitions: [ControllerNavigationOrbFrameTransition] = []
    @State private var reactionSequence: UInt64 = 0
    @State private var commandPalette: ControllerNavigationOrbPalette?
    @State private var commandPaletteTask: Task<Void, Never>?
    @State private var transitionIsActive = false
    @State private var transitionCompletionTask: Task<Void, Never>?
    @State private var transitionDeadline: TimeInterval?
    @State private var orbitSpeedPrograms: [ControllerNavigationOrbSpeedProgram] = []
    @State private var reactionPrograms: [ControllerNavigationOrbReactionProgram] = []

    private var resolvedFocusTravelStyle: ControllerNavigationFocusTravelStyle {
        focusTravelStyle ?? settings.controllerNavigationFocusAnimation
    }

    private var neonArtworkIsAnimated: Bool {
        animatesNeonArtwork && settings.controllerFocusBoxStyle.isAnimated
    }

    private var neonUpdateInterval: TimeInterval {
        // Position changes use the navigation clock. Stationary decorative
        // artwork uses the cheaper effects clock. Both requests are capped by
        // the active display's real maximum refresh rate.
        let domain: UIFrameRateDomain = transitionIsActive
            || tracksTargetFrameDirectly
            ? .controllerNavigation
            : .controllerEffects
        return frameRates.configuration.interval(for: domain)
    }

    var body: some View {
        ZStack(alignment: .topLeading) {
            if showsOrbs {
                orbTimeline
            }

            if showsNeonOutline {
                neonTimeline
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .onAppear {
            retarget(to: MotionTarget(id: targetID, frame: targetFrame))
            updateOrbActivity(isEnabled: showsOrbs)
        }
        .onChange(of: MotionTarget(id: targetID, frame: targetFrame)) { _, next in
            retarget(to: next)
        }
        .onChange(of: tracksTargetFrameDirectly) { _, tracksDirectly in
            guard tracksDirectly else { return }
            retarget(to: MotionTarget(id: targetID, frame: targetFrame))
        }
        .onChange(of: resolvedFocusTravelStyle) { _, _ in
            // Settle an in-flight effect before the new choice takes over.
            // The session still owns any currently scrolling geometry.
            setFrameDirectly(targetFrame, at: Date.timeIntervalSinceReferenceDate)
        }
        .onChange(of: reduceMotion) { _, reducesMotion in
            if reducesMotion {
                setFrameDirectly(targetFrame, at: Date.timeIntervalSinceReferenceDate)
            }
        }
        .onChange(of: showsOrbs) { _, isEnabled in
            updateOrbActivity(isEnabled: isEnabled)
        }
        .onChange(of: showsOrbs ? resolvedInteraction : nil) { _, interaction in
            guard let interaction else { return }
            react(to: interaction.command)
        }
        .onDisappear {
            releaseOrbResources()
            transitionCompletionTask?.cancel()
            transitionCompletionTask = nil
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private var neonTimeline: some View {
        TimelineView(
            .animation(
                minimumInterval: neonUpdateInterval,
                paused: reduceMotion
                    || ((tracksTargetFrameDirectly || !transitionIsActive)
                        && !neonArtworkIsAnimated)
            )
        ) { timeline in
            let time = reduceMotion
                    || ((tracksTargetFrameDirectly || !transitionIsActive)
                        && !neonArtworkIsAnimated)
                ? Date.timeIntervalSinceReferenceDate
                : timeline.date.timeIntervalSinceReferenceDate
            let frame = displayedFrame(at: time)
            let artworkFramesPerSecond = frameRates.configuration
                .framesPerSecond(for: .controllerEffects)
            let artworkTime = floor(time * artworkFramesPerSecond)
                / artworkFramesPerSecond
            ControllerFocusBoxArtwork(
                style: settings.controllerFocusBoxStyle,
                palette: settings.controllerFocusBoxPalette,
                customColors: gameCoverThemePreview.preview.map {
                    $0.backgroundPalette.gradientColors
                } ?? settings.controllerFocusBoxCustomColor.map { [$0.color] },
                animationSpeed: settings.controllerFocusBoxAnimationSpeed,
                glowIntensity: settings.controllerFocusBoxGlowIntensity,
                cornerRadius: controllerFocusCornerRadius(
                    size: frame.size,
                    baseline: neonCornerRadius,
                    shape: neonShape
                ),
                time: reduceMotion || !animatesNeonArtwork ? 0 : artworkTime
            )
            .frame(width: max(1, frame.width), height: max(1, frame.height))
            .position(x: frame.midX, y: frame.midY)
        }
    }

    private var orbTimeline: some View {
        AdaptiveAnimationTimeline(
            domain: .controllerEffects,
            paused: reduceMotion
        ) { timeline in
            let time = reduceMotion
                ? Date.timeIntervalSinceReferenceDate
                : timeline.date.timeIntervalSinceReferenceDate
            let frame = displayedFrame(at: time)
            let phaseTime = orbitSpeedState(at: time).phaseTime
            let reactionState = reactionState(at: time)
            ZStack(alignment: .topLeading) {
                Canvas { context, _ in
                    drawOrbTrails(in: &context, time: time)
                }

                ControllerNavigationOrbField(
                    primaryColor: primaryColor,
                    accentColor: accentColor,
                    paletteColors: settings.controllerOrbColors,
                    pressedColor: pressedPalette?.primaryColor,
                    style: style,
                    inset: inset,
                    orbScale: orbScale,
                    speedMultiplier: 1,
                    dispersion: 1,
                    reaction: nil,
                    renderTimeOverride: time,
                    phaseTimeOverride: phaseTime,
                    dispersionOverride: dispersion(for: frame),
                    reactionStateOverride: reactionState,
                    showsTrails: false,
                    pathShape: resolvedPathShape
                )
                .frame(width: max(1, frame.width), height: max(1, frame.height))
                .position(x: frame.midX, y: frame.midY)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }

    private var resolvedInteraction: ControllerNavigationOrbInteraction? {
        if let interactionOverride { return interactionOverride }
        guard let event = controllerInput?.latestEvent,
              let timestamp = controllerInput?.latestEventTimestamp else { return nil }
        return ControllerNavigationOrbInteraction(
            sequence: event.sequence,
            command: event.command,
            timestamp: timestamp
        )
    }

    private var resolvedPathShape: ControllerNavigationOrbPathShape {
        targetID.hasPrefix("library.game.")
            ? .roundedRectangle
            : .ellipse
    }

    private var pressedPalette: ControllerNavigationOrbPalette? {
        switch controllerInput?.pressedFaceButton {
        case .cross: return .green
        case .circle: return .red
        case .triangle: return .orange
        case nil: return commandPalette
        }
    }

    private func dispersion(for frame: CGRect) -> CGFloat {
        let diagonal = hypot(max(1, frame.width), max(1, frame.height))
        let sizeProgress = min(max((diagonal - 72) / 420, 0), 1)
        return 0.78 + sizeProgress * 0.30
    }

    private func retarget(to next: MotionTarget) {
        let focusTravelStyle = resolvedFocusTravelStyle
        let now = Date.timeIntervalSinceReferenceDate
        let previousTargetFrame = lastResolvedTargetFrame
        let source = lastTargetID == nil
            ? next.frame
            : displayedFrame(at: now)
        let changesFocus = lastTargetID != nil && lastTargetID != next.id
        let tracksFocusedLayout = lastTargetID == next.id
            && previousTargetFrame != next.frame

        if tracksTargetFrameDirectly {
            // The navigation session already supplies the focused row's
            // presentation rectangle once per display refresh while scrolling.
            // Re-easing those samples makes the outline chase stale frames and
            // snap when the independent focus transition completes.
            setFrameDirectly(next.frame, at: now)
        } else if changesFocus && !reduceMotion
                    && focusTravelStyle != .immediate {
            if showsOrbs && !transitionIsActive {
                setOrbitSpeedMultiplier(
                    0.5,
                    at: now,
                    duration: 0.12
                )
            }
            let duration = focusTravelStyle.duration
            transitionIsActive = true
            transitionDeadline = now + duration
            appendTransition(
                from: source,
                to: next.frame,
                startTime: now,
                duration: duration,
                travelStyle: focusTravelStyle
            )
            scheduleTransitionCompletion(after: duration)
        } else if tracksFocusedLayout && !reduceMotion
                    && focusTravelStyle != .immediate {
            // Value changes can resize a focused Form row without changing its
            // stable ID. Ease from the currently displayed bounds to the new
            // bounds; scrolling bypasses this branch and remains display-link
            // driven through `tracksTargetFrameDirectly` above.
            let duration: TimeInterval
            if transitionIsActive,
               let deadline = transitionDeadline,
               deadline > now {
                duration = max(0.03, deadline - now)
            } else {
                duration = min(0.12, focusTravelStyle.duration)
            }
            appendTransition(
                from: source,
                to: next.frame,
                startTime: now,
                duration: duration,
                travelStyle: focusTravelStyle
            )
            transitionIsActive = true
            transitionDeadline = now + duration
            scheduleTransitionCompletion(after: duration)
        } else {
            setFrameDirectly(next.frame, at: now)
        }
        lastTargetID = next.id
        lastResolvedTargetFrame = next.frame
    }

    private func displayedFrame(at time: TimeInterval) -> CGRect {
        if let transition = transition(at: time) {
            return transition.value(at: time)
        }
        return frameTransitions.first?.from ?? targetFrame
    }

    private func transition(
        at time: TimeInterval
    ) -> ControllerNavigationOrbFrameTransition? {
        frameTransitions.reversed().first { time >= $0.startTime }
    }

    private func appendTransition(
        from source: CGRect,
        to destination: CGRect,
        startTime: TimeInterval,
        duration: TimeInterval,
        travelStyle: ControllerNavigationFocusTravelStyle = .orbit
    ) {
        frameTransitions.append(
            ControllerNavigationOrbFrameTransition(
                from: source,
                to: destination,
                startTime: startTime,
                duration: duration,
                travelStyle: travelStyle
            )
        )
        pruneFrameTransitions(at: startTime)
    }

    private func setFrameDirectly(
        _ frame: CGRect,
        at time: TimeInterval
    ) {
        if transitionIsActive {
            endFocusTravel(at: time)
        }
        appendSettledFrame(frame, at: time)
    }

    private func appendSettledFrame(
        _ frame: CGRect,
        at time: TimeInterval
    ) {
        frameTransitions.append(
            ControllerNavigationOrbFrameTransition(
                from: frame,
                to: frame,
                startTime: time,
                duration: 0,
                travelStyle: .immediate
            )
        )
        pruneFrameTransitions(at: time)
    }

    private func pruneFrameTransitions(at time: TimeInterval) {
        let cutoff = time - controllerNavigationOrbTransitionHistoryDuration
        if let firstRecent = frameTransitions.firstIndex(where: {
            $0.startTime >= cutoff
        }) {
            let removableCount = max(0, firstRecent - 1)
            if removableCount > 0 {
                frameTransitions.removeFirst(removableCount)
            }
        } else if frameTransitions.count > 1 {
            frameTransitions.removeFirst(frameTransitions.count - 1)
        }
        if frameTransitions.count > 24 {
            frameTransitions.removeFirst(frameTransitions.count - 24)
        }
    }

    private func scheduleTransitionCompletion(after duration: TimeInterval) {
        transitionCompletionTask?.cancel()
        transitionCompletionTask = Task { @MainActor in
            try? await Task.sleep(
                for: .milliseconds(Int((duration * 1_000).rounded()))
            )
            guard !Task.isCancelled else { return }
            completeTransition(at: Date.timeIntervalSinceReferenceDate)
        }
    }

    private func completeTransition(at time: TimeInterval) {
        transitionCompletionTask?.cancel()
        transitionCompletionTask = nil
        endFocusTravel(at: time)
        // Completion tasks outlive the View value that scheduled them. Settle
        // on the latest observed geometry, not that value's captured frame.
        appendSettledFrame(lastResolvedTargetFrame ?? targetFrame, at: time)
    }

    private func endFocusTravel(at time: TimeInterval) {
        transitionCompletionTask?.cancel()
        transitionCompletionTask = nil
        transitionDeadline = nil
        transitionIsActive = false
        if showsOrbs {
            setOrbitSpeedMultiplier(1, at: time, duration: 0.18)
        }
    }

    private func drawOrbTrails(
        in context: inout GraphicsContext,
        time: TimeInterval
    ) {
        guard !reduceMotion else { return }
        let reactionState = reactionState(at: time)
        let sampleCount = 11 + Int((reactionState.trailBoost * 5).rounded())
        for particle in ControllerNavigationOrbParticle.standard {
            let trail = orbTrailPoints(
                for: particle,
                time: time,
                sampleCount: sampleCount
            )
            let points = trail.points
            guard points.count > 1 else { continue }
            let color = orbParticleColor(particle.colorIndex)
            for index in 0..<(points.count - 1) {
                let fade = 1 - Double(index) / Double(points.count - 1)
                let segment = ControllerNavigationOrbTrail.segment(
                    points: points,
                    index: index,
                    maximumTangentLength: trail.referenceSide * 0.18
                )
                context.stroke(
                    segment,
                    with: .color(color.opacity(fade * 0.16)),
                    style: StrokeStyle(
                        lineWidth: (5 + reactionState.trailBoost * 2.5)
                            * orbScale,
                        lineCap: .round
                    )
                )
                context.stroke(
                    segment,
                    with: .color(color.opacity(fade * 0.92)),
                    style: StrokeStyle(
                        lineWidth: (0.6 + fade * 1.15) * orbScale,
                        lineCap: .round
                    )
                )
            }
        }
    }

    /// OrbitKeys samples both the moving anchor and the continuing orbit at
    /// every historical instant. The resulting trail follows the real global
    /// particle path instead of joining two local ellipses or drawing a static
    /// source-to-destination connector.
    private func orbTrailPoints(
        for particle: ControllerNavigationOrbParticle,
        time: TimeInterval,
        sampleCount: Int
    ) -> OrbTrailSamples {
        let isContextMenuTrail = targetID.hasPrefix("context.")
        let headFrame = displayedFrame(at: time)
        let referenceSide = min(
            260,
            max(48, min(headFrame.width, headFrame.height))
        )
        let maximumLength = referenceSide
            * 0.58
            * CGFloat(
                isContextMenuTrail
                    ? 1
                    : controllerNavigationOrbTrailLengthMultiplier
            )
        let maximumSegmentLength = referenceSide * 0.20
        let sampleInterval = isContextMenuTrail
            ? 0.028
            : controllerNavigationOrbTrailSampleInterval
        var points: [CGPoint] = []
        var accumulatedLength: CGFloat = 0
        var rearwardAxis: CGVector?
        for index in 0..<sampleCount {
            let sampleTime = time
                - Double(index) * sampleInterval
            let frame = displayedFrame(at: sampleTime)
            let offset = orbParticleOffset(
                particle,
                frame: frame,
                phaseTime: orbitSpeedState(at: sampleTime).phaseTime,
                reactionState: reactionState(at: sampleTime)
            )
            let point = CGPoint(
                x: frame.midX + offset.width,
                y: frame.midY + offset.height
            )
            guard let previousPoint = points.last else {
                points.append(point)
                continue
            }

            if isContextMenuTrail, let headPoint = points.first {
                let displacement = CGVector(
                    dx: point.x - headPoint.x,
                    dy: point.y - headPoint.y
                )
                let displacementLength = hypot(
                    displacement.dx,
                    displacement.dy
                )
                if displacementLength > 0.001 {
                    if let rearwardAxis {
                        let rearwardProjection =
                            displacement.dx * rearwardAxis.dx
                                + displacement.dy * rearwardAxis.dy
                        // The context-menu reaction can spin more than one
                        // complete turn during the generic history window.
                        // Stop when history wraps into the orb's forward half
                        // plane so the luminous tail always remains behind it.
                        guard rearwardProjection > 0 else { break }
                    } else {
                        rearwardAxis = CGVector(
                            dx: displacement.dx / displacementLength,
                            dy: displacement.dy / displacementLength
                        )
                    }
                }
            }

            let segmentLength = hypot(
                point.x - previousPoint.x,
                point.y - previousPoint.y
            )
            let remainingLength = maximumLength - accumulatedLength
            guard remainingLength > 0 else { break }
            let allowedLength = min(
                segmentLength,
                maximumSegmentLength,
                remainingLength
            )
            if segmentLength > allowedLength, segmentLength > 0 {
                let progress = allowedLength / segmentLength
                points.append(
                    CGPoint(
                        x: previousPoint.x
                            + (point.x - previousPoint.x) * progress,
                        y: previousPoint.y
                            + (point.y - previousPoint.y) * progress
                    )
                )
                break
            }

            points.append(point)
            accumulatedLength += segmentLength
        }
        return OrbTrailSamples(
            points: points,
            referenceSide: referenceSide
        )
    }

    private func orbParticleOffset(
        _ particle: ControllerNavigationOrbParticle,
        frame: CGRect,
        phaseTime: TimeInterval,
        reactionState: ControllerNavigationOrbReactionState
    ) -> CGSize {
        let diameter = particle.diameter * orbScale * reactionState.scale
        let resolvedDispersion = dispersion(for: frame)
            * reactionState.dispersion
        let radiusX = max(2, frame.width / 2 - diameter / 2 - inset)
            * particle.radiusX * resolvedDispersion
        let radiusY = max(2, frame.height / 2 - diameter / 2 - inset)
            * particle.radiusY * resolvedDispersion
        let angle = phaseTime * particle.speed * particle.direction
            + particle.phase
            + reactionState.phaseTurns * .pi * 2 * particle.direction
        let wobble = particle.wobble * orbScale
        switch resolvedPathShape {
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

    private func orbParticleColor(_ index: Int) -> Color {
        if let pressedPalette {
            return pressedPalette.primaryColor
        }
        switch index % 4 {
        case 0: return primaryColor
        case 1: return accentColor
        case 2: return .cyan
        default: return .white
        }
    }

    private struct OrbTrailSamples {
        let points: [CGPoint]
        let referenceSide: CGFloat
    }

    private func initializeOrbitClockIfNeeded() {
        guard showsOrbs else { return }
        guard orbitSpeedPrograms.isEmpty else { return }
        let now = Date.timeIntervalSinceReferenceDate
        orbitSpeedPrograms = [
            ControllerNavigationOrbSpeedProgram(
                startTime: now,
                initialState: ControllerNavigationOrbSpeedState(
                    phaseTime: now,
                    multiplier: 1
                ),
                targetMultiplier: 1,
                duration: 0
            )
        ]
    }

    private func orbitSpeedState(
        at time: TimeInterval
    ) -> ControllerNavigationOrbSpeedState {
        for program in orbitSpeedPrograms.reversed()
        where time >= program.startTime {
            return program.state(at: time)
        }
        return ControllerNavigationOrbSpeedState(
            phaseTime: time,
            multiplier: 1
        )
    }

    private func setOrbitSpeedMultiplier(
        _ multiplier: Double,
        at time: TimeInterval,
        duration: TimeInterval
    ) {
        guard showsOrbs else { return }
        orbitSpeedPrograms.append(
            ControllerNavigationOrbSpeedProgram(
                startTime: time,
                initialState: orbitSpeedState(at: time),
                targetMultiplier: multiplier,
                duration: reduceMotion ? 0 : duration
            )
        )
        if orbitSpeedPrograms.count > 32 {
            orbitSpeedPrograms.removeFirst(orbitSpeedPrograms.count - 32)
        }
    }

    private func reactionState(
        at time: TimeInterval
    ) -> ControllerNavigationOrbReactionState {
        for program in reactionPrograms.reversed()
        where time >= program.startTime {
            return program.state(at: time)
        }
        return .idle
    }

    private func react(to command: MenuControllerCommand) {
        guard showsOrbs else { return }
        let kind: ControllerNavigationOrbReactionKind
        let palette: ControllerNavigationOrbPalette?
        let holdMilliseconds: Int
        switch command {
        case .activate:
            kind = .activate
            palette = .green
            holdMilliseconds = 420
        case .back:
            kind = .back
            palette = .red
            holdMilliseconds = 420
        case .showContextMenu:
            kind = .contextMenu
            palette = .orange
            holdMilliseconds = 520
        case .toggleFavorite:
            kind = .favorite
            palette = .pink
            holdMilliseconds = 460
        case .previousTab, .nextTab:
            kind = .tab
            palette = .purple
            holdMilliseconds = 360
        case .up, .right, .down, .left,
             .upRight, .downRight, .downLeft, .upLeft:
            // Anchor travel owns the half-speed orbit program. A directional
            // repeat must not stack a second reaction burst on top of it.
            return
        }

        emitReaction(kind)
        guard let palette else { return }
        commandPaletteTask?.cancel()
        commandPalette = palette
        commandPaletteTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(holdMilliseconds))
            guard !Task.isCancelled else { return }
            commandPalette = nil
            commandPaletteTask = nil
        }
    }

    private func emitReaction(_ kind: ControllerNavigationOrbReactionKind) {
        guard showsOrbs else { return }
        reactionSequence &+= 1
        let reaction = ControllerNavigationOrbReaction(
            sequence: reactionSequence,
            kind: kind
        )
        let now = Date.timeIntervalSinceReferenceDate
        let current = reactionState(at: now)
        reactionPrograms.append(
            ControllerNavigationOrbReactionProgram(
                reaction: reaction,
                startTime: now,
                initialPhaseTurns: current.phaseTurns,
                initialDispersion: current.dispersion,
                initialScale: current.scale
            )
        )
        if reactionPrograms.count > 32 {
            reactionPrograms.removeFirst(reactionPrograms.count - 32)
        }
    }

    private func updateOrbActivity(isEnabled: Bool) {
        guard isEnabled else {
            releaseOrbResources()
            return
        }
        initializeOrbitClockIfNeeded()
        if let interaction = resolvedInteraction,
           Date.timeIntervalSinceReferenceDate - interaction.timestamp < 0.65 {
            react(to: interaction.command)
        }
    }

    private func releaseOrbResources() {
        commandPaletteTask?.cancel()
        commandPaletteTask = nil
        commandPalette = nil
        orbitSpeedPrograms.removeAll(keepingCapacity: false)
        reactionPrograms.removeAll(keepingCapacity: false)
        reactionSequence = 0
    }

    private struct MotionTarget: Equatable {
        let id: String
        let frame: CGRect
    }
}

/// Keeps one render timeline alive while controller focus moves. OrbitKeys uses
/// the same presentation-level model: a new 0.34-second anchor transition starts
/// from the field's currently displayed position instead of creating a new field.
private struct ControllerNavigationMovingOrbOverlay: View {
    let target: ControllerNavigationResolvedOrbTarget?
    let controllerInput: MenuControllerInputRouter?

    @State private var settings = SettingsStore.shared
    @State private var displayedTarget: ControllerNavigationResolvedOrbTarget?
    @State private var isVisible = false
    @State private var removalTask: Task<Void, Never>?

    var body: some View {
        ZStack(alignment: .topLeading) {
            if let displayedTarget, displayedTarget.id.hasPrefix("tab-bar.") {
                ControllerNavigationAnimatedOrbField(
                    targetID: displayedTarget.id,
                    targetFrame: displayedTarget.frame,
                    primaryColor: displayedTarget.palette.primaryColor,
                    style: displayedTarget.style,
                    controllerInput: controllerInput,
                    inset: displayedTarget.inset,
                    orbScale: displayedTarget.orbScale,
                    showsOrbs: settings.focusOrbsEnabled,
                    showsNeonOutline: true
                )
                .opacity(isVisible ? 1 : 0)
            } else if settings.focusOrbsEnabled, let displayedTarget {
                ControllerNavigationAnimatedOrbField(
                    targetID: displayedTarget.id,
                    targetFrame: displayedTarget.frame,
                    primaryColor: displayedTarget.palette.primaryColor,
                    style: displayedTarget.style,
                    controllerInput: controllerInput,
                    inset: displayedTarget.inset,
                    orbScale: displayedTarget.orbScale
                )
                .opacity(isVisible ? 1 : 0)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onAppear {
            updateDisplayedTarget(target)
        }
        .onChange(of: target) { _, next in
            updateDisplayedTarget(next)
        }
        .onDisappear {
            removalTask?.cancel()
            removalTask = nil
        }
    }

    private func updateDisplayedTarget(
        _ next: ControllerNavigationResolvedOrbTarget?
    ) {
        removalTask?.cancel()
        removalTask = nil

        guard let next else {
            // A destination can publish one pass after its source disappears.
            // Keep the current field briefly so that transition remains seamless.
            removalTask = Task { @MainActor in
                // Lazy grids can remove the old focused card one pass before
                // the newly revealed card publishes its anchor. Cover the
                // complete controller scroll duration so the field never
                // blinks out between two valid library selections.
                try? await Task.sleep(for: .milliseconds(380))
                guard !Task.isCancelled else { return }
                withAnimation(.easeOut(duration: 0.12)) {
                    isVisible = false
                }
                try? await Task.sleep(for: .milliseconds(130))
                guard !Task.isCancelled else { return }
                displayedTarget = nil
                removalTask = nil
            }
            return
        }

        var transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            displayedTarget = next
        }
        withAnimation(.easeOut(duration: 0.12)) {
            isVisible = true
        }
    }
}

private struct ControllerNavigationOrbTargetModifier: ViewModifier {
    let id: String
    let isActive: Bool
    let isEnabled: Bool
    let palette: ControllerNavigationOrbPalette
    let style: ControllerNavigationOrbStyle
    let inset: CGFloat
    let orbScale: CGFloat
    let segmentIndex: Int?
    let segmentCount: Int?
    let priority: Int

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content.anchorPreference(
                key: ControllerNavigationOrbAnchorPreferenceKey.self,
                value: .bounds
            ) { bounds in
                isActive
                    ? ControllerNavigationOrbAnchorTarget(
                        id: id,
                        bounds: bounds,
                        palette: palette,
                        style: style,
                        inset: inset,
                        orbScale: orbScale,
                        segmentIndex: segmentIndex,
                        segmentCount: segmentCount,
                        priority: priority
                    )
                    : nil
            }
        } else {
            content
        }
    }
}

private struct ControllerNavigationOrbOverlayHostModifier: ViewModifier {
    let controllerInput: MenuControllerInputRouter?

    func body(content: Content) -> some View {
        content.overlayPreferenceValue(
            ControllerNavigationOrbAnchorPreferenceKey.self
        ) { target in
            GeometryReader { proxy in
                ControllerNavigationMovingOrbOverlay(
                    target: resolveActiveTarget(target, in: proxy),
                    controllerInput: controllerInput
                )
            }
            .zIndex(100_000)
        }
    }

    private func resolveActiveTarget(
        _ target: ControllerNavigationOrbAnchorTarget?,
        in proxy: GeometryProxy
    ) -> ControllerNavigationResolvedOrbTarget? {
        if controllerInput?.navigationZone == .tabBar,
           let windowFrame = controllerInput?.tabBarOrbFocusFrameInWindow,
           let focusedIndex = controllerInput?.tabBarOrbFocusedIndex {
            let hostFrame = proxy.frame(in: .global)
            return ControllerNavigationResolvedOrbTarget(
                id: "tab-bar.\(focusedIndex)",
                frame: windowFrame.offsetBy(
                    dx: -hostFrame.minX,
                    dy: -hostFrame.minY
                ),
                palette: focusedIndex
                    == controllerInput?.tabBarOrbSelectedIndex
                    ? .green
                    : .blue,
                style: .liquidGlass,
                inset: 2,
                orbScale: 0.78
            )
        }
        return resolve(target, in: proxy)
    }

    private func resolve(
        _ target: ControllerNavigationOrbAnchorTarget?,
        in proxy: GeometryProxy
    ) -> ControllerNavigationResolvedOrbTarget? {
        guard let target else { return nil }
        var frame = proxy[target.bounds]
        if let index = target.segmentIndex,
           let count = target.segmentCount,
           count > 0,
           (0..<count).contains(index) {
            let segmentWidth = frame.width / CGFloat(count)
            frame.origin.x += CGFloat(index) * segmentWidth
            frame.size.width = segmentWidth
        }
        return ControllerNavigationResolvedOrbTarget(
            id: target.id,
            frame: frame,
            palette: target.palette,
            style: target.style,
            inset: target.inset,
            orbScale: target.orbScale
        )
    }
}

extension View {
    func controllerNavigationOrbTarget(
        id: String,
        isActive: Bool,
        isEnabled: Bool = true,
        palette: ControllerNavigationOrbPalette = .blue,
        style: ControllerNavigationOrbStyle = .liquidGlass,
        inset: CGFloat = 2,
        orbScale: CGFloat = 1,
        segmentIndex: Int? = nil,
        segmentCount: Int? = nil,
        priority: Int = 0
    ) -> some View {
        modifier(
            ControllerNavigationOrbTargetModifier(
                id: id,
                isActive: isActive,
                isEnabled: isEnabled,
                palette: palette,
                style: style,
                inset: inset,
                orbScale: orbScale,
                segmentIndex: segmentIndex,
                segmentCount: segmentCount,
                priority: priority
            )
        )
    }

    func controllerNavigationOrbOverlay(
        controllerInput: MenuControllerInputRouter? = nil
    ) -> some View {
        modifier(
            ControllerNavigationOrbOverlayHostModifier(
                controllerInput: controllerInput
            )
        )
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

private struct ControllerNavigationOrbParticle: Identifiable, Sendable {
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

private enum ControllerNavigationOrbTrail {
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
