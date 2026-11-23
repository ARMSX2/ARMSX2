// DynamicThumbstickSettings.swift — Persistent virtual-pad gesture configuration
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum VirtualPadActionButton: Int, CaseIterable, Hashable, Identifiable {
    case leftShoulder
    case rightShoulder
    case leftTrigger
    case rightTrigger
    case faceBottom
    case faceLeft
    case faceRight
    case faceTop
    case up
    case down
    case left
    case right
    case start
    case select
    case leftStick
    case rightStick

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .leftShoulder: return "L1"
        case .rightShoulder: return "R1"
        case .leftTrigger: return "L2"
        case .rightTrigger: return "R2"
        case .faceBottom: return "Cross"
        case .faceLeft: return "Square"
        case .faceRight: return "Circle"
        case .faceTop: return "Triangle"
        case .up: return "Up"
        case .down: return "Down"
        case .left: return "Left"
        case .right: return "Right"
        case .start: return "Start"
        case .select: return "Select"
        case .leftStick: return "L3"
        case .rightStick: return "R3"
        }
    }

    var padButton: ARMSX2PadButton {
        switch self {
        case .leftShoulder: return .L1
        case .rightShoulder: return .R1
        case .leftTrigger: return .L2
        case .rightTrigger: return .R2
        case .faceBottom: return .cross
        case .faceLeft: return .square
        case .faceRight: return .circle
        case .faceTop: return .triangle
        case .up: return .up
        case .down: return .down
        case .left: return .left
        case .right: return .right
        case .start: return .start
        case .select: return .select
        case .leftStick: return .L3
        case .rightStick: return .R3
        }
    }
}

enum VirtualPadThumbstickSide {
    case left
    case right
}

enum DynamicCrosshairType: Int, CaseIterable, Identifiable {
    case classic = 0
    case dot = 1
    case circle = 2
    case circleDot = 3
    case cross = 4
    case chevron = 5
    case brackets = 6
    case diamond = 7
    case shotgun = 8
    case sniper = 9
    case tactical = 10
    case burst = 11
    case fourBoxes = 12
    case triad = 13
    case reactiveDot = 14

    var id: Int { rawValue }

    static let allCases: [DynamicCrosshairType] = [
        .fourBoxes,
        .triad,
        .reactiveDot,
        .classic,
        .dot,
        .circle,
        .circleDot,
        .cross,
        .chevron,
        .brackets,
        .diamond,
        .shotgun,
        .sniper,
        .tactical,
        .burst
    ]

    var title: String {
        switch self {
        case .classic: return "Classic"
        case .dot: return "Dot"
        case .circle: return "Circle"
        case .circleDot: return "Circle + Dot"
        case .cross: return "Full Cross"
        case .chevron: return "Chevron"
        case .brackets: return "Corner Brackets"
        case .diamond: return "Diamond"
        case .shotgun: return "Shotgun"
        case .sniper: return "Sniper Scope"
        case .tactical: return "Tactical"
        case .burst: return "Eight-Point Burst"
        case .fourBoxes: return "Four-Box Reactive"
        case .triad: return "Three-Line Triangle"
        case .reactiveDot: return "Reactive Dot"
        }
    }
}

enum DynamicCrosshairAnimation: Int, CaseIterable, Identifiable {
    case reactive = 0
    case pulse
    case expand
    case rotate
    case recoil
    case orbit
    case focus
    case wave
    case directional
    case elastic
    case parallax
    case velocity
    case stabilizer
    case snap
    case drift
    case tilt
    case bloom
    case directionLock

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .reactive: return "Reactive"
        case .pulse: return "Pulse"
        case .expand: return "Expand"
        case .rotate: return "Rotate"
        case .recoil: return "Recoil"
        case .orbit: return "Orbit"
        case .focus: return "Focus"
        case .wave: return "Wave"
        case .directional: return "Directional"
        case .elastic: return "Elastic"
        case .parallax: return "Parallax"
        case .velocity: return "Velocity"
        case .stabilizer: return "Stabilizer"
        case .snap: return "Snap"
        case .drift: return "Drift"
        case .tilt: return "Tilt"
        case .bloom: return "Bloom"
        case .directionLock: return "Direction Lock"
        }
    }
}
