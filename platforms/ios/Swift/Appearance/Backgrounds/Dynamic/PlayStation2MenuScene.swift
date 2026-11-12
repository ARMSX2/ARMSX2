// PlayStation2MenuScene.swift — PS2 menu scene types
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation2MenuScene

enum PlayStation2MenuDepthPass {
  case far
  case middle
  case near

  func contains(_ depth: Double) -> Bool {
    switch self {
    case .far:
      return depth < -0.25
    case .middle:
      return depth >= -0.25 && depth < 0.45
    case .near:
      return depth >= 0.45
    }
  }

  func visibility(at depth: Double) -> Double {
    let feather = 0.16
    let middleEntry = DynamicBackgroundMath.smoothstep(
      from: -0.25 - feather,
      to: -0.25 + feather,
      value: depth
    )
    let nearEntry = DynamicBackgroundMath.smoothstep(
      from: 0.45 - feather,
      to: 0.45 + feather,
      value: depth
    )

    switch self {
    case .far:
      return 1 - middleEntry
    case .middle:
      return middleEntry * (1 - nearEntry)
    case .near:
      return middleEntry * nearEntry
    }
  }
}

struct PlayStation2MenuCamera {
  let size: CGSize
  let horizontalExtent: Double
  let verticalExtent: Double

  private let yawSine: Double
  private let yawCosine: Double
  private let pitchSine: Double
  private let pitchCosine: Double
  private let rollSine: Double
  private let rollCosine: Double
  private let viewportScale: Double
  private let panX: Double
  private let panY: Double

  init(
    size: CGSize,
    time: TimeInterval,
    yawSpeed: Double,
    rollSpeed: Double,
    orbitDegrees: Double
  ) {
    self.size = size

    let minimumDimension = max(1, min(size.width, size.height))
    horizontalExtent = Double(size.width / minimumDimension)
    verticalExtent = Double(size.height / minimumDimension)
    viewportScale = Double(minimumDimension * 0.5)

    let orbitLimit = orbitDegrees * Double.pi / 180
    let yawMotion =
      sin(time * 0.024 * yawSpeed) * 0.055
      + sin(time * 0.008 * yawSpeed) * 0.015
    let yaw = yawMotion / 0.07 * orbitLimit
    let pitch = sin(time * 0.019 + 1.2) * 0.035
    let roll = sin(time * 0.014 * rollSpeed) * 0.012

    yawSine = sin(yaw)
    yawCosine = cos(yaw)
    pitchSine = sin(pitch)
    pitchCosine = cos(pitch)
    rollSine = sin(roll)
    rollCosine = cos(roll)
    panX =
      sin(time * 0.013) * Double(size.width) * 0.024
      + sin(time * 0.005 + 1.3) * Double(size.width) * 0.008
    panY =
      cos(time * 0.011) * Double(size.height) * 0.018
      + sin(time * 0.004 + 0.7) * Double(size.height) * 0.007
  }

  func project(_ world: PlayStation2MenuVector3) -> PlayStation2MenuProjection {
    let yawX = world.x * yawCosine + world.z * yawSine
    let yawZ = -world.x * yawSine + world.z * yawCosine
    let pitchY = world.y * pitchCosine - yawZ * pitchSine
    let pitchZ = world.y * pitchSine + yawZ * pitchCosine
    let transformedX = yawX * rollCosine - pitchY * rollSine
    let transformedY = yawX * rollSine + pitchY * rollCosine
    let perspective = 1 / max(0.68, 1 - pitchZ * 0.17)

    return PlayStation2MenuProjection(
      point: CGPoint(
        x: size.width * 0.5
          + CGFloat(transformedX * viewportScale * perspective + panX),
        y: size.height * 0.52
          + CGFloat(transformedY * viewportScale * perspective + panY)
          - CGFloat(pitchZ) * size.height * 0.018
      ),
      depth: pitchZ,
      scale: CGFloat(perspective)
    )
  }
}

struct PlayStation2MenuVector3 {
  let x: Double
  let y: Double
  let z: Double

  static func + (
    lhs: PlayStation2MenuVector3,
    rhs: PlayStation2MenuVector3
  ) -> PlayStation2MenuVector3 {
    PlayStation2MenuVector3(
      x: lhs.x + rhs.x,
      y: lhs.y + rhs.y,
      z: lhs.z + rhs.z
    )
  }

  func rotatedX(_ angle: Double) -> PlayStation2MenuVector3 {
    PlayStation2MenuVector3(
      x: x,
      y: y * cos(angle) - z * sin(angle),
      z: y * sin(angle) + z * cos(angle)
    )
  }

  func rotatedY(_ angle: Double) -> PlayStation2MenuVector3 {
    PlayStation2MenuVector3(
      x: x * cos(angle) + z * sin(angle),
      y: y,
      z: -x * sin(angle) + z * cos(angle)
    )
  }

  func rotatedZ(_ angle: Double) -> PlayStation2MenuVector3 {
    PlayStation2MenuVector3(
      x: x * cos(angle) - y * sin(angle),
      y: x * sin(angle) + y * cos(angle),
      z: z
    )
  }
}

struct PlayStation2MenuRotation {
  let x: Double
  let y: Double
  let z: Double
}

struct PlayStation2MenuProjection {
  let point: CGPoint
  let depth: Double
  let scale: CGFloat
}

struct PlayStation2MenuTowerConfiguration {
  let center: PlayStation2MenuVector3
  let size: PlayStation2MenuVector3
  let rotation: PlayStation2MenuRotation
  let pulseSpeed: Double
  let pulseAmount: Double
  let phase: Double
  let isInner: Bool
}

struct PlayStation2MenuCrystalConfiguration {
  let center: PlayStation2MenuVector3
  let side: Double
  let phase: Double
  let rotationSpeed: Double
  let tintIndex: Int
}

struct PlayStation2MenuCloudElement {
  let rect: CGRect
  let glowRect: CGRect
  let color: Color
  let cloudOpacity: Double
  let glowOpacity: Double
}

struct PlayStation2MenuOrbConfiguration {
  let center: PlayStation2MenuVector3
  let radiusX: Double
  let radiusY: Double
  let bend: Double
  let speed: Double
  let phase: Double
  let depthPhase: Double
}

enum PlayStation2MenuGeometry {
  static let cubeFaces: [[Int]] = [
    [0, 1, 2, 3],
    [4, 7, 6, 5],
    [0, 4, 5, 1],
    [1, 5, 6, 2],
    [2, 6, 7, 3],
    [3, 7, 4, 0],
  ]

  static let crystalCrossEdges: [(Int, Int)] = [
    (0, 6),
    (1, 7),
    (2, 4),
    (3, 5),
  ]
}
