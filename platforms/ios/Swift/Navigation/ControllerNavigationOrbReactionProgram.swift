// ControllerNavigationOrbReactionProgram.swift — orb speed, dispersion and reaction programs
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ControllerNavigationOrbSpeedState {
    let phaseTime: TimeInterval
    let multiplier: Double
}

struct ControllerNavigationOrbSpeedProgram {
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

struct ControllerNavigationOrbDispersionProgram {
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

struct ControllerNavigationOrbReactionProgram {
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
