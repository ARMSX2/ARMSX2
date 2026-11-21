// ControllerNavigationAnimatedOrbField.swift — animated orb focus field
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Preserve the long OrbitKeys trail while sampling it sparsely enough for the
/// particle field to remain decorative rather than consuming the focus budget.
private let controllerNavigationOrbTrailLengthMultiplier = 3
let controllerNavigationOrbTrailSampleInterval = 0.028
    * Double(controllerNavigationOrbTrailLengthMultiplier)
private let controllerNavigationOrbTransitionHistoryDuration: TimeInterval = 1.5

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
        // Moving uses the navigation clock, still artwork the cheaper effects one.
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
            // The session supplies the row's rectangle every refresh while
            // scrolling; re-easing it chases stale frames and snaps at the end
            // of the focus transition.
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
            // A value change can resize the focused row under the same ID, so ease.
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
                        // The context-menu spin can pass a full turn; stop when
                        // history wraps in front of the orb so the tail stays
                        // behind it.
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
