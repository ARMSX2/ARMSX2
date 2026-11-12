// PlayStation2MenuBackground+TowerRenderer.swift — PS2 menu tower renderer
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation2MenuTowerRenderer

extension PlayStation2MenuBackground {
  func drawTowers(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    time: TimeInterval,
    pass: PlayStation2MenuDepthPass,
    configurations: [PlayStation2MenuTowerConfiguration]
  ) {
    let visibleConfigurations =
      configurations
      .enumerated()
      .filter { pass.contains($0.element.center.z) }
      .sorted { $0.element.center.z < $1.element.center.z }

    for (index, configuration) in visibleConfigurations {
      let pulse =
        1
        + sin(time * configuration.pulseSpeed + configuration.phase)
        * configuration.pulseAmount
      drawTower(
        context: &context,
        camera: camera,
        configuration: configuration,
        growth: pulse,
        time: time,
        index: index,
        pass: pass
      )
    }
  }

  func drawTower(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    configuration: PlayStation2MenuTowerConfiguration,
    growth: Double,
    time: TimeInterval,
    index: Int,
    pass: PlayStation2MenuDepthPass
  ) {
    let baseCenter = towerBaseCenter(
      configuration: configuration,
      growth: growth
    )
    let top = towerFaceCorners(
      center: configuration.center,
      size: configuration.size,
      scale: 1,
      rotation: configuration.rotation,
      camera: camera
    )
    let base = towerFaceCorners(
      center: baseCenter,
      size: configuration.size,
      scale: configuration.isInner ? 0.7 : 0.56,
      rotation: configuration.rotation,
      camera: camera
    )
    let sideFaces = [
      [base[0], base[1], top[1], top[0]],
      [base[1], base[2], top[2], top[1]],
      [base[2], base[3], top[3], top[2]],
      [base[3], base[0], top[0], top[3]],
    ].enumerated().sorted { lhs, rhs in
      averagedDepth(lhs.element) < averagedDepth(rhs.element)
    }
    let tint =
      index.isMultiple(of: 7)
      ? theme.ribbonColor(index: index, time: time)
      : theme.sharedColor(index: index, time: time)
    let colorsTowers =
      theme.particleSettings.backgrounds.playStation2MenuColorsTowers
    let materialOpacity: Double

    switch pass {
    case .far:
      materialOpacity = 0.22
    case .middle:
      materialOpacity = 0.42
    case .near:
      materialOpacity = 0.62
    }

    let faceLight: [Double] = [0.28, 0.68, 0.42, 0.78]

    for (faceIndex, face) in sideFaces {
      let path = polygonPath(face.map { $0.point })
      let light = faceLight[faceIndex]
      let baseEdgeCenter = averagedPoint([face[0].point, face[1].point])
      let topEdgeCenter = averagedPoint([face[2].point, face[3].point])
      let depthBase: (red: Double, green: Double, blue: Double)

      switch pass {
      case .far:
        depthBase = (0.14, 0.18, 0.26)
      case .middle:
        depthBase = (0.24, 0.3, 0.4)
      case .near:
        depthBase = (0.38, 0.46, 0.58)
      }

      let neutral = Color(
        red: depthBase.red + light * 0.12,
        green: depthBase.green + light * 0.11,
        blue: depthBase.blue + light * 0.1
      )
      let towerSurface = colorsTowers ? tint : neutral

      context.fill(
        path,
        with: .linearGradient(
          Gradient(stops: [
            .init(color: .clear, location: 0),
            .init(
              color: tint.opacity(
                materialOpacity * (colorsTowers ? 0.18 : 0.025)
              ),
              location: 0.16
            ),
            .init(
              color: towerSurface.opacity(
                materialOpacity * (colorsTowers ? 0.38 : 0.2)
              ),
              location: 0.34
            ),
            .init(
              color: towerSurface.opacity(materialOpacity * (0.7 + light * 0.12)),
              location: 0.88
            ),
            .init(
              color: .white.opacity(materialOpacity * light * 0.24),
              location: 1
            ),
          ]),
          startPoint: baseEdgeCenter,
          endPoint: topEdgeCenter
        )
      )

      context.stroke(
        path,
        with: .linearGradient(
          Gradient(stops: [
            .init(color: .clear, location: 0),
            .init(
              color: .white.opacity(materialOpacity * 0.035),
              location: 0.34
            ),
            .init(
              color: .white.opacity(materialOpacity * (0.08 + light * 0.05)),
              location: 1
            ),
          ]),
          startPoint: baseEdgeCenter,
          endPoint: topEdgeCenter
        ),
        lineWidth: pass == .near ? 0.7 : 0.45
      )
    }

    let topPath = polygonPath(top.map { $0.point })
    let topBounds = topPath.boundingRect
    let topColor: Color

    switch pass {
    case .far:
      topColor = Color(red: 0.3, green: 0.36, blue: 0.47)
    case .middle:
      topColor = Color(red: 0.46, green: 0.54, blue: 0.65)
    case .near:
      topColor = Color(red: 0.62, green: 0.7, blue: 0.8)
    }

    context.fill(
      topPath,
      with: .linearGradient(
        Gradient(colors: [
          .white.opacity(materialOpacity * 0.24),
          (colorsTowers ? tint : topColor).opacity(materialOpacity * 0.76),
          tint.opacity(materialOpacity * (colorsTowers ? 0.48 : 0.14)),
          .black.opacity(materialOpacity * 0.22),
        ]),
        startPoint: topBounds.origin,
        endPoint: CGPoint(x: topBounds.maxX, y: topBounds.maxY)
      )
    )
    context.stroke(
      topPath,
      with: .color(.white.opacity(materialOpacity * 0.13)),
      lineWidth: pass == .near ? 0.75 : 0.5
    )
  }

  func towerBaseCenter(
    configuration: PlayStation2MenuTowerConfiguration,
    growth: Double
  ) -> PlayStation2MenuVector3 {
    let radialDistance = max(
      0.001,
      hypot(configuration.center.x, configuration.center.y)
    )
    let inward = PlayStation2MenuVector3(
      x: -configuration.center.x / radialDistance,
      y: -configuration.center.y / radialDistance,
      z: 0
    )
    let maximumShaftLength = radialDistance * (configuration.isInner ? 0.22 : 0.68)
    let shaftLength = min(configuration.size.z * growth, maximumShaftLength)

    return PlayStation2MenuVector3(
      x: configuration.center.x + inward.x * shaftLength,
      y: configuration.center.y + inward.y * shaftLength,
      z: configuration.center.z - 0.1 - shaftLength * 0.18
    )
  }

  func towerFaceCorners(
    center: PlayStation2MenuVector3,
    size: PlayStation2MenuVector3,
    scale: Double,
    rotation: PlayStation2MenuRotation,
    camera: PlayStation2MenuCamera
  ) -> [PlayStation2MenuProjection] {
    let halfWidth = size.x * scale * 0.5
    let halfHeight = size.y * scale * 0.5

    return [
      PlayStation2MenuVector3(x: -halfWidth, y: -halfHeight, z: 0),
      PlayStation2MenuVector3(x: halfWidth, y: -halfHeight, z: 0),
      PlayStation2MenuVector3(x: halfWidth, y: halfHeight, z: 0),
      PlayStation2MenuVector3(x: -halfWidth, y: halfHeight, z: 0),
    ].map { local in
      camera.project(center + local.rotatedZ(rotation.z))
    }
  }

  func averagedDepth(_ projections: [PlayStation2MenuProjection]) -> Double {
    projections.reduce(0) { $0 + $1.depth } / Double(projections.count)
  }

  func drawTowerBaseClouds(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    time: TimeInterval,
    configurations: [PlayStation2MenuTowerConfiguration]
  ) {
    let bankCenter = camera.project(
      PlayStation2MenuVector3(
        x: -0.06 * camera.horizontalExtent,
        y: 0.08 * camera.verticalExtent,
        z: -0.08
      )
    ).point
    let bankRect = CGRect(
      x: bankCenter.x - camera.size.width * 0.31,
      y: bankCenter.y - camera.size.height * 0.15,
      width: camera.size.width * 0.62,
      height: camera.size.height * 0.3
    )

    context.drawLayer { bank in
      bank.addFilter(.blur(radius: 68))
      bank.fill(
        Path(ellipseIn: bankRect),
        with: .color(Color(red: 0.08, green: 0.12, blue: 0.2).opacity(0.3))
      )
    }
    context.drawLayer { bankGlow in
      bankGlow.blendMode = .plusLighter
      bankGlow.addFilter(.blur(radius: 84))
      bankGlow.fill(
        Path(ellipseIn: bankRect.insetBy(dx: bankRect.width * 0.08, dy: 0)),
        with: .color(theme.sharedColor(index: 1, time: time).opacity(0.075))
      )
    }

    let outerConfigurations =
      configurations
      .enumerated()
      .filter { !$0.element.isInner }
    let minimumDimension = min(camera.size.width, camera.size.height)

    let cloudElements = outerConfigurations.map { entry in
      let (index, configuration) = entry
      let pulse =
        1
        + sin(time * configuration.pulseSpeed + configuration.phase)
        * configuration.pulseAmount
      let projected = camera.project(
        towerBaseCenter(configuration: configuration, growth: pulse)
      )
      let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 37.11)
      let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 79.63)
      let drift = CGPoint(
        x: CGFloat(sin(time * 0.012 + Double(index) * 1.7)) * 12,
        y: CGFloat(cos(time * 0.01 + Double(index) * 1.3)) * 9
      )
      let width = minimumDimension * CGFloat(0.21 + seedA * 0.09)
      let height = minimumDimension * CGFloat(0.09 + seedB * 0.055)
      let rect = CGRect(
        x: projected.point.x + drift.x - width / 2,
        y: projected.point.y + drift.y - height / 2,
        width: width,
        height: height
      )
      let color =
        index.isMultiple(of: 3)
        ? theme.sharedColor(index: index + 1, time: time)
        : theme.ribbonColor(index: index + 2, time: time)

      return PlayStation2MenuCloudElement(
        rect: rect,
        glowRect: rect.insetBy(dx: width * 0.1, dy: height * 0.08),
        color: color,
        cloudOpacity: 0.28 + seedA * 0.12,
        glowOpacity: 0.08 + seedB * 0.045
      )
    }

    context.drawLayer { cloud in
      cloud.addFilter(.blur(radius: 34))
      for element in cloudElements {
        cloud.fill(
          Path(ellipseIn: element.rect),
          with: .color(
            Color(red: 0.16, green: 0.21, blue: 0.3)
              .opacity(element.cloudOpacity)
          )
        )
      }
    }
    context.drawLayer { glow in
      glow.blendMode = .plusLighter
      glow.addFilter(.blur(radius: 47))
      for element in cloudElements {
        glow.fill(
          Path(ellipseIn: element.glowRect),
          with: .color(element.color.opacity(element.glowOpacity))
        )
      }
    }
  }
}
