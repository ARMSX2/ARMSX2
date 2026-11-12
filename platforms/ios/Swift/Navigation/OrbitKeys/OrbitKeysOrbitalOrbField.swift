// OrbitKeysOrbitalOrbField.swift — OrbitKeys orbital orb field
// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.

import SwiftUI

// MARK: - Orbit Field

struct OrbitKeysOrbitalOrbField: View {
  let color: Color
  let accentColor: Color
  let stickX: Float
  let stickY: Float
  let selectedPosition: OrbitKeysGridPosition
  let characterFace: OrbitKeysFaceButton?
  let characterPosition: OrbitKeysGridPosition?
  let characterTrigger: Int
  let cellSide: CGFloat
  var isPaused = false
  var customSelectedAnchor: CGSize? = nil
  var customCharacterAnchor: CGSize? = nil

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var orbs = OrbitKeysOrbParticle.randomSet()
  @State private var anchorTransitions: [OrbitKeysOrbAnchorTransition] = []
  @State private var characterAnchorTransitions: [OrbitKeysOrbAnchorTransition] = []
  @State private var focusPrograms: [OrbitKeysOrbFocusProgram] = []
  @State private var spinPrograms: [OrbitKeysOrbSpinProgram] = []
  @State private var orbitSpeedPrograms: [OrbitKeysOrbOrbitSpeedProgram] = []
  @State private var orbitRadiusPrograms: [OrbitKeysOrbRadiusProgram] = []
  @State private var trailTransitionStart: TimeInterval?
  @State private var trailInitialOpacity: CGFloat = 1
  @State private var trailSampleResetTime: TimeInterval?
  @State private var lastSettledPosition: OrbitKeysGridPosition?

  var body: some View {
    GeometryReader { proxy in
      TimelineView(
        .animation(
          minimumInterval: 1 / 30,
          paused: reduceMotion || isPaused
        )
      ) { timeline in
        let time = reduceMotion
          ? 0
          : timeline.date.timeIntervalSinceReferenceDate

        ZStack {
          Canvas(rendersAsynchronously: true) { context, size in
            let trailCount = reduceMotion || isPaused ? 1 : 7
            let trailOpacity = trailVisibility(at: time)
            let earliestSampleTime = trailEarliestSampleTime(at: time)

            for orb in orbs {
              guard trailCount > 1, trailOpacity > 0.001 else { continue }
              let laserColor = particleColor(orb.colorIndex)
              let points = trailPoints(
                for: orb,
                time: time,
                size: size,
                sampleCount: trailCount,
                earliestSampleTime: earliestSampleTime
              )

              for trailIndex in 0..<(points.count - 1) {
                let fade = (1 - Double(trailIndex) / Double(points.count - 1)) * trailOpacity
                let segment = bezierTrailSegment(
                  points: points,
                  index: trailIndex
                )

                context.stroke(
                  segment,
                  with: .color(laserColor.opacity(fade * 0.16)),
                  style: StrokeStyle(lineWidth: 5, lineCap: .round)
                )
                context.stroke(
                  segment,
                  with: .color(laserColor.opacity(fade * 0.92)),
                  style: StrokeStyle(
                    lineWidth: CGFloat(0.55 + fade * 1.15),
                    lineCap: .round
                  )
                )
              }
            }
          }

          glassOrbLayer(time: time, size: proxy.size)
        }
      }
    }
    .allowsHitTesting(false)
    .onAppear {
      let now = Date.timeIntervalSinceReferenceDate
      anchorTransitions = [
        OrbitKeysOrbAnchorTransition(
          from: targetStickVector,
          to: targetStickVector,
          startTime: now,
          duration: 0
        )
      ]
      characterAnchorTransitions = [
        OrbitKeysOrbAnchorTransition(
          from: .zero,
          to: .zero,
          startTime: now,
          duration: 0
        )
      ]
      orbitSpeedPrograms = [
        OrbitKeysOrbOrbitSpeedProgram(
          startTime: now,
          initialState: OrbitKeysOrbOrbitSpeedState(
            phaseTime: now,
            multiplier: targetOrbitSpeedMultiplier
          ),
          targetMultiplier: targetOrbitSpeedMultiplier,
          duration: 0
        )
      ]
      orbitRadiusPrograms = [
        OrbitKeysOrbRadiusProgram(
          startTime: now,
          initialProgress: targetOrbitRadiusProgress,
          targetProgress: targetOrbitRadiusProgress,
          duration: 0
        )
      ]
      lastSettledPosition = selectedPosition
    }
    .onChange(of: targetStickVector) { _, newValue in
      let now = Date.timeIntervalSinceReferenceDate
      let currentValue = anchorVector(at: now)
      anchorTransitions.append(
        OrbitKeysOrbAnchorTransition(
          from: currentValue,
          to: newValue,
          startTime: now,
          duration: reduceMotion ? 0 : 0.34
        )
      )
      if anchorTransitions.count > 80 {
        anchorTransitions.removeFirst(anchorTransitions.count - 80)
      }
    }
    .onChange(of: selectedPosition) { _, _ in
      updateOrbitFocusForSelection()
    }
    .onChange(of: customSelectedAnchor) { _, _ in
      updateOrbitFocusForSelection()
    }
    .task(id: selectedPosition) {
      guard let lastSettledPosition else {
        self.lastSettledPosition = selectedPosition
        return
      }
      guard lastSettledPosition != selectedPosition else { return }

      do {
        try await Task.sleep(for: .milliseconds(420))
      } catch {
        return
      }
      guard !Task.isCancelled else { return }

      let now = Date.timeIntervalSinceReferenceDate
      if let trailTransitionStart, now - trailTransitionStart < 1.25 {
        self.lastSettledPosition = selectedPosition
        return
      }
      trailInitialOpacity = trailVisibility(at: now)
      trailTransitionStart = now
      trailSampleResetTime = now
      self.lastSettledPosition = selectedPosition
    }
    .onChange(of: characterTrigger) { _, _ in
      guard !reduceMotion,
        customCharacterAnchor != nil || (characterFace != nil && characterPosition != nil)
      else {
        return
      }
      let now = Date.timeIntervalSinceReferenceDate
      let currentSpin = spinState(at: now)
      let currentFocus = burstFocus(at: now)
      spinPrograms.append(
        .burst(startTime: now, initialState: currentSpin)
      )
      trimSpinPrograms()
      focusPrograms.append(
        .burst(startTime: now, initialFocus: currentFocus)
      )
      trimFocusPrograms()
      let currentCharacterAnchor = characterAnchor(at: now)
      characterAnchorTransitions.append(
        OrbitKeysOrbAnchorTransition(
          from: currentCharacterAnchor,
          to: resolvedCharacterAnchor(),
          startTime: now,
          duration: 0.28
        )
      )
      trimCharacterAnchorTransitions()
    }
  }

  @ViewBuilder
  private func glassOrbLayer(time: TimeInterval, size: CGSize) -> some View {
    if #available(iOS 26.0, *) {
      GlassEffectContainer(spacing: 3) {
        orbLayer(time: time, size: size, usesGlass: true)
      }
    } else {
      orbLayer(time: time, size: size, usesGlass: false)
    }
  }

  private func orbLayer(time: TimeInterval, size: CGSize, usesGlass: Bool) -> some View {
    ZStack {
      ForEach(orbs) { orb in
        let offset = orbOffset(
          orb,
          time: time,
          size: size,
          anchor: anchorVector(at: time)
        )
        OrbitKeysGlassOrb(
          color: particleColor(orb.colorIndex),
          diameter: orb.diameter,
          usesGlass: usesGlass
        )
        .offset(offset)
      }
    }
    .frame(width: size.width, height: size.height)
  }

  private func orbOffset(
    _ orb: OrbitKeysOrbParticle,
    time: TimeInterval,
    size: CGSize,
    anchor: CGSize
  ) -> CGSize {
    let characterFocus = burstFocus(at: time)
    let burstPhase = spinState(at: time).phaseTurns * .pi * 2 * orb.direction
    let animationTime = reduceMotion ? 0 : orbitSpeedState(at: time).phaseTime
    let angle = animationTime * orb.speed * orb.direction + orb.phase + burstPhase
    let radiusFocus = CGFloat(orbitRadiusProgress(at: time))
    let normalAnchor = normalAnchorOffset(from: anchor)
    let characterAnchor = characterAnchor(at: time)
    let anchorX = mix(normalAnchor.width, characterAnchor.width, characterFocus)
    let anchorY = mix(normalAnchor.height, characterAnchor.height, characterFocus)
    let normalRadiusX = mix(
      size.width * orb.radiusX,
      cellSide * orb.localRadiusX,
      radiusFocus
    )
    let normalRadiusY = mix(
      size.height * orb.radiusY,
      cellSide * orb.localRadiusY,
      radiusFocus
    )
    let ring = CGFloat(orb.id)
    let burstRadiusX = cellSide * (0.12 + ring * 0.055 + orb.localRadiusX * 0.04)
    let burstRadiusY = cellSide * (0.105 + ring * 0.048 + orb.localRadiusY * 0.04)
    let radiusX = mix(normalRadiusX, burstRadiusX, characterFocus)
    let radiusY = mix(normalRadiusY, burstRadiusY, characterFocus)
    let wobbleX = sin(angle * orb.wobbleFrequency + orb.phase) * orb.wobble
    let wobbleY = cos(angle * (orb.wobbleFrequency + 0.31)) * orb.wobble

    return CGSize(
      width: anchorX + cos(angle) * radiusX + wobbleX,
      height: anchorY + sin(angle) * radiusY + wobbleY
    )
  }

  private func trailPoints(
    for orb: OrbitKeysOrbParticle,
    time: TimeInterval,
    size: CGSize,
    sampleCount: Int,
    earliestSampleTime: TimeInterval?
  ) -> [CGPoint] {
    let maximumLength = cellSide * 0.58
    let maximumSegmentLength = cellSide * 0.2
    var points: [CGPoint] = []
    var accumulatedLength: CGFloat = 0

    for index in 0..<sampleCount {
      let sampleTime = time - Double(index) * 0.028
      if let earliestSampleTime, sampleTime < earliestSampleTime {
        break
      }
      let offset = orbOffset(
        orb,
        time: sampleTime,
        size: size,
        anchor: anchorVector(at: sampleTime)
      )
      let point = CGPoint(
        x: size.width / 2 + offset.width,
        y: size.height / 2 + offset.height
      )

      guard let previousPoint = points.last else {
        points.append(point)
        continue
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
            x: previousPoint.x + (point.x - previousPoint.x) * progress,
            y: previousPoint.y + (point.y - previousPoint.y) * progress
          )
        )
        break
      }

      points.append(point)
      accumulatedLength += segmentLength
    }

    return points
  }

  private func bezierTrailSegment(points: [CGPoint], index: Int) -> Path {
    let segment = OrbitKeysOrbTrailGeometry.segment(
      points: points,
      index: index,
      maximumTangentLength: cellSide * 0.18
    )

    var path = Path()
    path.move(to: segment.start)
    path.addCurve(
      to: segment.end,
      control1: segment.control1,
      control2: segment.control2
    )
    return path
  }

  private func trailVisibility(at time: TimeInterval) -> Double {
    guard
      let trailTransitionStart,
      let trailSampleResetTime,
      time >= trailTransitionStart
    else {
      return 1
    }
    let fadeOutElapsed = time - trailTransitionStart

    if fadeOutElapsed < 0.14 {
      let progress = OrbitKeysCubicBezierMotion.value(
        fadeOutElapsed / 0.14,
        control1: 0.12,
        control2: 0.82
      )
      return Double(trailInitialOpacity) * (1 - progress)
    }

    let fadeInStart = trailSampleResetTime + 0.16
    if time < fadeInStart {
      return 0
    }
    if time < fadeInStart + 0.28 {
      return OrbitKeysCubicBezierMotion.value(
        (time - fadeInStart) / 0.28,
        control1: 0.08,
        control2: 0.88
      )
    }
    return 1
  }

  private func trailEarliestSampleTime(at time: TimeInterval) -> TimeInterval? {
    guard let trailSampleResetTime, time >= trailSampleResetTime else {
      return nil
    }
    return trailSampleResetTime
  }

  private func anchorVector(at time: TimeInterval) -> CGSize {
    OrbitKeysOrbMotionResolver.anchorVector(
      transitions: anchorTransitions,
      fallback: targetStickVector,
      at: time
    )
  }

  private func spinState(at time: TimeInterval) -> OrbitKeysOrbSpinState {
    OrbitKeysOrbSpinResolver.state(programs: spinPrograms, at: time)
  }

  private func trimSpinPrograms() {
    if spinPrograms.count > 32 {
      spinPrograms.removeFirst(spinPrograms.count - 32)
    }
  }

  private func orbitSpeedState(at time: TimeInterval) -> OrbitKeysOrbOrbitSpeedState {
    OrbitKeysOrbOrbitSpeedResolver.state(
      programs: orbitSpeedPrograms,
      fallbackTime: time,
      at: time
    )
  }

  private func trimOrbitSpeedPrograms() {
    if orbitSpeedPrograms.count > 32 {
      orbitSpeedPrograms.removeFirst(orbitSpeedPrograms.count - 32)
    }
  }

  private func orbitRadiusProgress(at time: TimeInterval) -> Double {
    OrbitKeysOrbRadiusResolver.progress(
      programs: orbitRadiusPrograms,
      fallback: targetOrbitRadiusProgress,
      at: time
    )
  }

  private func trimOrbitRadiusPrograms() {
    if orbitRadiusPrograms.count > 32 {
      orbitRadiusPrograms.removeFirst(orbitRadiusPrograms.count - 32)
    }
  }

  private func characterAnchor(at time: TimeInterval) -> CGSize {
    OrbitKeysOrbMotionResolver.anchorVector(
      transitions: characterAnchorTransitions,
      fallback: .zero,
      at: time
    )
  }

  private func trimCharacterAnchorTransitions() {
    if characterAnchorTransitions.count > 32 {
      characterAnchorTransitions.removeFirst(characterAnchorTransitions.count - 32)
    }
  }

  private func burstFocus(at time: TimeInterval) -> CGFloat {
    OrbitKeysOrbFocusResolver.focus(programs: focusPrograms, at: time)
  }

  private func trimFocusPrograms() {
    if focusPrograms.count > 32 {
      focusPrograms.removeFirst(focusPrograms.count - 32)
    }
  }

  private func characterAnchor(
    for face: OrbitKeysFaceButton,
    position: OrbitKeysGridPosition
  ) -> CGSize {
    let boxAnchor = CGSize(
      width: CGFloat(position.column - 1) * (cellSide + 8),
      height: CGFloat(position.row - 1) * (cellSide + 8)
    )
    let distance = cellSide * 0.32

    switch face {
    case .square:
      return CGSize(width: boxAnchor.width - distance, height: boxAnchor.height)
    case .cross:
      return CGSize(width: boxAnchor.width, height: boxAnchor.height + distance)
    case .circle:
      return CGSize(width: boxAnchor.width + distance, height: boxAnchor.height)
    case .triangle:
      return CGSize(width: boxAnchor.width, height: boxAnchor.height - distance)
    }
  }

  private func normalAnchorOffset(from anchor: CGSize) -> CGSize {
    if customSelectedAnchor != nil {
      return anchor
    }

    return CGSize(
      width: anchor.width * (cellSide + 8),
      height: -anchor.height * (cellSide + 8)
    )
  }

  private func resolvedCharacterAnchor() -> CGSize {
    if let customCharacterAnchor {
      return customCharacterAnchor
    }
    guard let characterFace, let characterPosition else {
      return .zero
    }

    return characterAnchor(for: characterFace, position: characterPosition)
  }

  private func updateOrbitFocusForSelection() {
    let now = Date.timeIntervalSinceReferenceDate
    let currentOrbitSpeed = orbitSpeedState(at: now)
    orbitSpeedPrograms.append(
      OrbitKeysOrbOrbitSpeedProgram(
        startTime: now,
        initialState: currentOrbitSpeed,
        targetMultiplier: targetOrbitSpeedMultiplier,
        duration: reduceMotion
          ? 0
          : (isUsingCenterOrbit ? 0.78 : 0.44)
      )
    )
    trimOrbitSpeedPrograms()

    let currentRadiusProgress = orbitRadiusProgress(at: now)
    orbitRadiusPrograms.append(
      OrbitKeysOrbRadiusProgram(
        startTime: now,
        initialProgress: currentRadiusProgress,
        targetProgress: targetOrbitRadiusProgress,
        duration: reduceMotion
          ? 0
          : (isUsingCenterOrbit ? 0.82 : 0.68)
      )
    )
    trimOrbitRadiusPrograms()

    let currentFocus = burstFocus(at: now)
    let currentSpin = spinState(at: now)
    if currentFocus > 0.001 || abs(currentSpin.velocityTurnsPerSecond) > 0.001 {
      spinPrograms.append(
        .deceleration(
          startTime: now,
          initialState: currentSpin,
          duration: 0.78
        )
      )
      trimSpinPrograms()
      focusPrograms.append(
        .deceleration(
          startTime: now,
          initialFocus: currentFocus,
          duration: 0.78
        )
      )
      trimFocusPrograms()
    }
  }

  private var targetStickVector: CGSize {
    if let customSelectedAnchor {
      return customSelectedAnchor
    }

    guard selectedPosition == .center else {
      return CGSize(
        width: CGFloat(selectedPosition.column - 1),
        height: CGFloat(1 - selectedPosition.row)
      )
    }
    return stickVector
  }

  private var targetOrbitSpeedMultiplier: Double {
    isUsingCenterOrbit ? 1 : 2.2
  }

  private var targetOrbitRadiusProgress: Double {
    isUsingCenterOrbit ? 0 : 1
  }

  private var isUsingCenterOrbit: Bool {
    customSelectedAnchor == nil && selectedPosition == .center
  }

  private var stickVector: CGSize {
    let x = CGFloat(stickX)
    let y = CGFloat(stickY)
    let radialMagnitude = min(1, hypot(x, y))
    let largestAxis = max(abs(x), abs(y))
    guard largestAxis > 0.0001 else { return .zero }

    let edgeStretch = radialMagnitude / largestAxis
    let squareX = min(max(x * edgeStretch, -1), 1)
    let squareY = min(max(y * edgeStretch, -1), 1)
    return CGSize(
      width: snappedAxis(squareX),
      height: snappedAxis(squareY)
    )
  }

  private func snappedAxis(_ value: CGFloat) -> CGFloat {
    if abs(value) < 0.12 {
      return 0
    }
    if abs(value) > 0.88 {
      return value < 0 ? -1 : 1
    }
    return value
  }

  private func mix(_ start: CGFloat, _ end: CGFloat, _ progress: CGFloat) -> CGFloat {
    start + (end - start) * progress
  }

  private func particleColor(_ index: Int) -> Color {
    switch index % 4 {
    case 0: color
    case 1: accentColor
    case 2: .cyan
    default: .white
    }
  }

}

private struct OrbitKeysGlassOrb: View {
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
          .frame(width: diameter * 0.24, height: diameter * 0.24)
          .padding(diameter * 0.18)
      }
      .shadow(color: color.opacity(0.72), radius: diameter * 0.85)
  }

  @ViewBuilder
  var body: some View {
    if #available(iOS 26.0, *), usesGlass {
      core
        .glassEffect(.regular.tint(color.opacity(0.34)), in: .circle)
        .opacity(0.75)
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

private struct OrbitKeysOrbParticle: Identifiable {
  let id: Int
  let radiusX: CGFloat
  let radiusY: CGFloat
  let localRadiusX: CGFloat
  let localRadiusY: CGFloat
  let speed: Double
  let direction: Double
  let phase: Double
  let wobble: CGFloat
  let wobbleFrequency: Double
  let diameter: CGFloat
  let colorIndex: Int

  static func randomSet(count: Int = 4) -> [OrbitKeysOrbParticle] {
    (0..<count).map { index in
      OrbitKeysOrbParticle(
        id: index,
        radiusX: .random(in: 0.24...0.44),
        radiusY: .random(in: 0.22...0.43),
        localRadiusX: .random(in: 0.12...0.34),
        localRadiusY: .random(in: 0.1...0.32),
        speed: .random(in: 0.95...1.75),
        direction: Bool.random() ? 1 : -1,
        phase: .random(in: 0...(.pi * 2)),
        wobble: .random(in: 1.5...5),
        wobbleFrequency: .random(in: 1.2...2.4),
        diameter: .random(in: 7...13),
        colorIndex: Int.random(in: 0...3)
      )
    }
  }
}
