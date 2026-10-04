// ControllerNavigationFocusAnimation.swift — Shared focus travel curves
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import CoreGraphics

/// Stateless curves shared by the display-link scroll driver and focus artwork.
/// Each interrupted move starts from its displayed rectangle, without retaining
/// velocity or starting a second animation clock.
enum ControllerNavigationFocusTravelStyle: String, CaseIterable, Identifiable, Codable {
    case easeOutBack
    case easeOut
    case easeIn
    case easeInOut
    case linear
    case sine
    case smoothStep
    case smootherStep
    case exponential
    case circular
    case easeInOutBack
    case softSpring
    case spring
    case elastic
    case bounce
    case arc
    case orbit
    case immediate

    var id: String { rawValue }

    var title: String {
        switch self {
        case .easeOutBack: "Ease Out Back"
        case .easeOut: "Ease Out"
        case .easeIn: "Ease In"
        case .easeInOut: "Ease In Out"
        case .linear: "Linear"
        case .sine: "Sine Glide"
        case .smoothStep: "Smoothstep"
        case .smootherStep: "Silky Smooth"
        case .exponential: "Exponential Snap"
        case .circular: "Circular Glide"
        case .easeInOutBack: "Anticipation"
        case .softSpring: "Soft Spring"
        case .spring: "Bouncy Spring"
        case .elastic: "Elastic"
        case .bounce: "Bounce"
        case .arc: "Arc Glide"
        case .orbit: "Orbital Glide"
        case .immediate: "None (Instant)"
        }
    }

    var summary: String {
        switch self {
        case .easeOutBack: "Quick travel with a gentle overshoot and settle."
        case .easeOut: "Starts quickly and slows smoothly into place."
        case .easeIn: "Starts gently and accelerates toward the next item."
        case .easeInOut: "Gradual acceleration and a soft landing."
        case .linear: "Moves at a steady speed from start to finish."
        case .sine: "A soft, flowing transition with rounded timing."
        case .smoothStep: "Balanced travel with gently eased endpoints."
        case .smootherStep: "Extra-smooth acceleration and settling."
        case .exponential: "Fast initial movement with a precise, soft finish."
        case .circular: "A quick glide that slows gently into the next item."
        case .easeInOutBack: "Pulls back slightly before moving and settling."
        case .softSpring: "A subtle spring with a small, cushioned rebound."
        case .spring: "A lively spring with a more pronounced rebound."
        case .elastic: "An elastic stretch with a short, fading wobble."
        case .bounce: "Lands with a few small, diminishing bounces."
        case .arc: "Follows a shallow curved path between items."
        case .orbit: "Follows a gentle S-shaped path between items."
        case .immediate: "Changes focus immediately without travel animation."
        }
    }

    var duration: TimeInterval {
        switch self {
        case .easeOut, .easeOutBack: 0.16
        case .linear: 0.18
        case .smoothStep: 0.20
        case .easeIn, .easeInOut, .sine, .exponential, .circular: 0.22
        case .smootherStep: 0.24
        case .softSpring, .arc: 0.26
        case .easeInOutBack: 0.28
        case .spring, .bounce: 0.32
        case .orbit: 0.34
        case .elastic: 0.42
        case .immediate: 0
        }
    }

    func progress(_ value: Double) -> Double {
        guard self != .immediate else { return 1 }
        let t = min(max(value, 0), 1)
        // Exact endpoints prevent a final-frame snap for oscillating curves.
        if t == 0 || t == 1 { return t }
        switch self {
        case .linear: return t
        case .easeIn: return t * t * t
        case .easeOut: return 1 - pow(1 - t, 3)
        case .easeInOut, .arc, .orbit:
            return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2
        case .sine: return (1 - cos(.pi * t)) / 2
        case .smoothStep: return t * t * (3 - 2 * t)
        case .smootherStep: return t * t * t * (t * (6 * t - 15) + 10)
        case .exponential: return (1 - pow(2, -10 * t)) / (1 - pow(2, -10))
        case .circular: return sqrt(1 - pow(t - 1, 2))
        case .easeOutBack:
            let remaining = t - 1
            return 1 + 2.70158 * pow(remaining, 3) + 1.70158 * pow(remaining, 2)
        case .easeInOutBack:
            let overshoot = 1.70158 * 1.525
            if t < 0.5 {
                return pow(2 * t, 2) * ((overshoot + 1) * 2 * t - overshoot) / 2
            }
            return (pow(2 * t - 2, 2)
                * ((overshoot + 1) * (2 * t - 2) + overshoot) + 2) / 2
        case .softSpring: return springProgress(t, damping: 8, frequency: 8)
        case .spring: return springProgress(t, damping: 6, frequency: 12)
        case .elastic: return springProgress(t, damping: 7, frequency: 16)
        case .bounce:
            let n = 7.5625
            let d = 2.75
            if t < 1 / d { return n * t * t }
            if t < 2 / d { return n * pow(t - 1.5 / d, 2) + 0.75 }
            if t < 2.5 / d { return n * pow(t - 2.25 / d, 2) + 0.9375 }
            return n * pow(t - 2.625 / d, 2) + 0.984375
        case .immediate: return 1
        }
    }

    /// Keep content monotonic and inside its scroll limits; only the outline
    /// may overshoot, bounce, or take a curved path.
    func scrollProgress(_ value: Double) -> Double {
        switch self {
        case .easeOutBack, .easeInOutBack, .softSpring, .spring, .elastic, .bounce:
            ControllerNavigationFocusTravelStyle.easeOut.progress(value)
        default: progress(value)
        }
    }

    func frame(from source: CGRect, to destination: CGRect, progress value: Double) -> CGRect {
        if self == .immediate || value >= 1 { return destination }
        if value <= 0 || source == destination { return source }
        let p = CGFloat(progress(value))
        let width = max(1, source.width + (destination.width - source.width) * p)
        let height = max(1, source.height + (destination.height - source.height) * p)
        let dx = destination.midX - source.midX
        let dy = destination.midY - source.midY
        var center = CGPoint(x: source.midX + dx * p, y: source.midY + dy * p)
        let distance = hypot(dx, dy)
        // Avoid sideways movement when scrolling leaves the source and
        // destination at the same screen position.
        if distance > 0.5, value > 0, value < 1 {
            let deviation: CGFloat
            switch self {
            case .arc: deviation = min(28, distance * 0.12) * sin(.pi * p)
            case .orbit:
                deviation = min(36, distance * 0.16) * sin(2 * .pi * p) * sin(.pi * p)
            default: deviation = 0
            }
            center.x -= dy / distance * deviation
            center.y += dx / distance * deviation
        }
        return CGRect(
            x: center.x - width / 2, y: center.y - height / 2,
            width: width, height: height
        )
    }

    private func springProgress(_ t: Double, damping: Double, frequency: Double) -> Double {
        func displacement(_ time: Double) -> Double {
            1 - exp(-damping * time)
                * (cos(frequency * time) + damping / frequency * sin(frequency * time))
        }
        return displacement(t) / displacement(1)
    }
}
