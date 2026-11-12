// OrbitKeysOrbMotion.swift — OrbitKeys orb motion
// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.

import SwiftUI

struct OrbitKeysOrbTrailBezierSegment {
  let start: CGPoint
  let end: CGPoint
  let control1: CGPoint
  let control2: CGPoint
}

enum OrbitKeysOrbTrailGeometry {
  static func segment(
    points: [CGPoint],
    index: Int,
    maximumTangentLength: CGFloat
  ) -> OrbitKeysOrbTrailBezierSegment {
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

    return OrbitKeysOrbTrailBezierSegment(
      start: start,
      end: end,
      control1: CGPoint(
        x: start.x + startTangent.dx / 3,
        y: start.y + startTangent.dy / 3
      ),
      control2: CGPoint(
        x: end.x - endTangent.dx / 3,
        y: end.y - endTangent.dy / 3
      )
    )
  }

  private static func tangent(
    points: [CGPoint],
    index: Int,
    maximumLength: CGFloat
  ) -> CGVector {
    let point = points[index]
    let previous = points[max(0, index - 1)]
    let following = points[min(points.count - 1, index + 1)]
    let raw = CGVector(
      dx: (following.x - previous.x) * 0.5,
      dy: (following.y - previous.y) * 0.5
    )
    let rawLength = hypot(raw.dx, raw.dy)
    guard rawLength > 0 else { return .zero }

    let adjacentLengths = [
      hypot(point.x - previous.x, point.y - previous.y),
      hypot(following.x - point.x, following.y - point.y),
    ].filter { $0 > 0 }
    guard let shortestAdjacent = adjacentLengths.min() else {
      return .zero
    }

    let allowedLength = min(maximumLength, shortestAdjacent * 1.25)
    let scale = min(1, allowedLength / rawLength)
    return CGVector(dx: raw.dx * scale, dy: raw.dy * scale)
  }
}

struct OrbitKeysOrbAnchorTransition {
  let from: CGSize
  let to: CGSize
  let startTime: TimeInterval
  let duration: TimeInterval

  func value(at time: TimeInterval) -> CGSize {
    guard duration > 0 else { return to }
    let linearProgress = min(max((time - startTime) / duration, 0), 1)
    let progress = OrbitKeysCubicBezierMotion.value(
      linearProgress,
      control1: 0.08,
      control2: 0.92
    )
    return CGSize(
      width: from.width + (to.width - from.width) * progress,
      height: from.height + (to.height - from.height) * progress
    )
  }
}

struct OrbitKeysOrbSpinState: Equatable {
  let phaseTurns: Double
  let velocityTurnsPerSecond: Double

  static let zero = OrbitKeysOrbSpinState(
    phaseTurns: 0,
    velocityTurnsPerSecond: 0
  )
}

struct OrbitKeysOrbFocusProgram {
  enum Kind {
    case burst
    case deceleration(duration: TimeInterval)
  }

  let startTime: TimeInterval
  let initialFocus: CGFloat
  let kind: Kind

  static func burst(
    startTime: TimeInterval,
    initialFocus: CGFloat
  ) -> OrbitKeysOrbFocusProgram {
    OrbitKeysOrbFocusProgram(
      startTime: startTime,
      initialFocus: initialFocus,
      kind: .burst
    )
  }

  static func deceleration(
    startTime: TimeInterval,
    initialFocus: CGFloat,
    duration: TimeInterval
  ) -> OrbitKeysOrbFocusProgram {
    OrbitKeysOrbFocusProgram(
      startTime: startTime,
      initialFocus: initialFocus,
      kind: .deceleration(duration: duration)
    )
  }

  func focus(at time: TimeInterval) -> CGFloat {
    let elapsed = max(0, time - startTime)

    switch kind {
    case .burst:
      if elapsed < 0.24 {
        let progress = OrbitKeysCubicBezierMotion.value(
          elapsed / 0.24,
          control1: 0.08,
          control2: 0.9
        )
        return initialFocus
          + (1 - initialFocus) * CGFloat(progress)
      }
      if elapsed < 0.74 {
        return 1
      }
      if elapsed < 1.79 {
        return CGFloat(
          1
            - OrbitKeysCubicBezierMotion.value(
              (elapsed - 0.74) / 1.05,
              control1: 0.08,
              control2: 0.86
            )
        )
      }
      return 0

    case .deceleration(let duration):
      guard duration > 0 else { return 0 }
      let progress = min(elapsed / duration, 1)
      return initialFocus
        * CGFloat(
          1
            - OrbitKeysCubicBezierMotion.value(
              progress,
              control1: 0.08,
              control2: 0.9
            )
        )
    }
  }
}

enum OrbitKeysOrbFocusResolver {
  static func focus(
    programs: [OrbitKeysOrbFocusProgram],
    at time: TimeInterval
  ) -> CGFloat {
    for program in programs.reversed() where time >= program.startTime {
      return program.focus(at: time)
    }
    return 0
  }
}

struct OrbitKeysOrbSpinProgram {
  enum Kind {
    case burst
    case deceleration(duration: TimeInterval)
  }

  let startTime: TimeInterval
  let initialState: OrbitKeysOrbSpinState
  let kind: Kind

  static func burst(
    startTime: TimeInterval,
    initialState: OrbitKeysOrbSpinState
  ) -> OrbitKeysOrbSpinProgram {
    OrbitKeysOrbSpinProgram(
      startTime: startTime,
      initialState: initialState,
      kind: .burst
    )
  }

  static func deceleration(
    startTime: TimeInterval,
    initialState: OrbitKeysOrbSpinState,
    duration: TimeInterval
  ) -> OrbitKeysOrbSpinProgram {
    OrbitKeysOrbSpinProgram(
      startTime: startTime,
      initialState: initialState,
      kind: .deceleration(duration: duration)
    )
  }

  func state(at time: TimeInterval) -> OrbitKeysOrbSpinState {
    let elapsed = max(0, time - startTime)

    switch kind {
    case .burst:
      return burstState(elapsed: elapsed)
    case .deceleration(let duration):
      return velocitySegment(
        start: initialState,
        endVelocity: 0,
        duration: duration,
        elapsed: elapsed
      )
    }
  }

  private func burstState(elapsed: TimeInterval) -> OrbitKeysOrbSpinState {
    let accelerationDuration = 0.32
    let holdDuration = 0.42
    let decelerationDuration = 1.05
    let peakVelocity = max(0.82, initialState.velocityTurnsPerSecond)

    let accelerated = velocitySegment(
      start: initialState,
      endVelocity: peakVelocity,
      duration: accelerationDuration,
      elapsed: min(elapsed, accelerationDuration)
    )
    guard elapsed > accelerationDuration else { return accelerated }

    let heldElapsed = min(elapsed - accelerationDuration, holdDuration)
    let held = OrbitKeysOrbSpinState(
      phaseTurns: accelerated.phaseTurns + peakVelocity * heldElapsed,
      velocityTurnsPerSecond: peakVelocity
    )
    guard elapsed > accelerationDuration + holdDuration else { return held }

    return velocitySegment(
      start: held,
      endVelocity: 0,
      duration: decelerationDuration,
      elapsed: min(
        elapsed - accelerationDuration - holdDuration,
        decelerationDuration
      )
    )
  }

  private func velocitySegment(
    start: OrbitKeysOrbSpinState,
    endVelocity: Double,
    duration: TimeInterval,
    elapsed: TimeInterval
  ) -> OrbitKeysOrbSpinState {
    guard duration > 0 else {
      return OrbitKeysOrbSpinState(
        phaseTurns: start.phaseTurns,
        velocityTurnsPerSecond: endVelocity
      )
    }

    let progress = min(max(elapsed / duration, 0), 1)
    let curve = OrbitKeysCubicBezierMotion.value(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let integratedCurve = OrbitKeysCubicBezierMotion.integral(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let velocityDelta = endVelocity - start.velocityTurnsPerSecond

    return OrbitKeysOrbSpinState(
      phaseTurns: start.phaseTurns
        + duration
        * (start.velocityTurnsPerSecond * progress
          + velocityDelta * integratedCurve),
      velocityTurnsPerSecond: start.velocityTurnsPerSecond
        + velocityDelta * curve
    )
  }
}

enum OrbitKeysOrbSpinResolver {
  static func state(
    programs: [OrbitKeysOrbSpinProgram],
    at time: TimeInterval
  ) -> OrbitKeysOrbSpinState {
    for program in programs.reversed() where time >= program.startTime {
      return program.state(at: time)
    }
    return .zero
  }
}

enum OrbitKeysOrbMotionResolver {
  static func anchorVector(
    transitions: [OrbitKeysOrbAnchorTransition],
    fallback: CGSize,
    at time: TimeInterval
  ) -> CGSize {
    guard let firstTransition = transitions.first else {
      return fallback
    }

    for transition in transitions.reversed() where time >= transition.startTime {
      return transition.value(at: time)
    }
    return firstTransition.from
  }
}

struct OrbitKeysOrbOrbitSpeedState: Equatable {
  let phaseTime: TimeInterval
  let multiplier: Double
}

struct OrbitKeysOrbOrbitSpeedProgram {
  let startTime: TimeInterval
  let initialState: OrbitKeysOrbOrbitSpeedState
  let targetMultiplier: Double
  let duration: TimeInterval

  func state(at time: TimeInterval) -> OrbitKeysOrbOrbitSpeedState {
    let elapsed = max(0, time - startTime)
    guard duration > 0 else {
      return OrbitKeysOrbOrbitSpeedState(
        phaseTime: initialState.phaseTime + targetMultiplier * elapsed,
        multiplier: targetMultiplier
      )
    }

    let transitionElapsed = min(elapsed, duration)
    let progress = transitionElapsed / duration
    let curve = OrbitKeysCubicBezierMotion.value(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let integratedCurve = OrbitKeysCubicBezierMotion.integral(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let multiplierDelta = targetMultiplier - initialState.multiplier
    var phaseTime =
      initialState.phaseTime
      + duration
      * (initialState.multiplier * progress
        + multiplierDelta * integratedCurve)

    if elapsed > duration {
      phaseTime += targetMultiplier * (elapsed - duration)
    }

    return OrbitKeysOrbOrbitSpeedState(
      phaseTime: phaseTime,
      multiplier: initialState.multiplier + multiplierDelta * curve
    )
  }
}

enum OrbitKeysOrbOrbitSpeedResolver {
  static func state(
    programs: [OrbitKeysOrbOrbitSpeedProgram],
    fallbackTime: TimeInterval,
    at time: TimeInterval
  ) -> OrbitKeysOrbOrbitSpeedState {
    for program in programs.reversed() where time >= program.startTime {
      return program.state(at: time)
    }
    return OrbitKeysOrbOrbitSpeedState(
      phaseTime: fallbackTime,
      multiplier: 1
    )
  }
}

struct OrbitKeysOrbRadiusProgram {
  let startTime: TimeInterval
  let initialProgress: Double
  let targetProgress: Double
  let duration: TimeInterval

  func progress(at time: TimeInterval) -> Double {
    guard duration > 0 else { return targetProgress }
    let linearProgress = min(max((time - startTime) / duration, 0), 1)
    let curve = OrbitKeysCubicBezierMotion.value(
      linearProgress,
      control1: 0.06,
      control2: 0.88
    )
    return initialProgress + (targetProgress - initialProgress) * curve
  }
}

enum OrbitKeysOrbRadiusResolver {
  static func progress(
    programs: [OrbitKeysOrbRadiusProgram],
    fallback: Double,
    at time: TimeInterval
  ) -> Double {
    for program in programs.reversed() where time >= program.startTime {
      return program.progress(at: time)
    }
    return programs.first?.initialProgress ?? fallback
  }
}

enum OrbitKeysCubicBezierMotion {
  static func value(
    _ progress: Double,
    control1: Double,
    control2: Double
  ) -> Double {
    let t = min(max(progress, 0), 1)
    let inverse = 1 - t
    return 3 * inverse * inverse * t * control1
      + 3 * inverse * t * t * control2
      + t * t * t
  }

  static func integral(
    _ progress: Double,
    control1: Double,
    control2: Double
  ) -> Double {
    let t = min(max(progress, 0), 1)
    return 1.5 * control1 * t * t
      + (-2 * control1 + control2) * t * t * t
      + (0.75 * control1 - 0.75 * control2 + 0.25)
      * t * t * t * t
  }
}
