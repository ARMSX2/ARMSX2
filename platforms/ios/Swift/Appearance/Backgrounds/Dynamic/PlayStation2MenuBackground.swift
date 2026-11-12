// PlayStation2MenuBackground.swift — PS2 menu background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation2MenuBackground

struct PlayStation2MenuBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.menuBackgroundSessionStart) private var cameraStartDate
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    let framesPerSecond = max(
      1,
      theme.particleSettings.backgrounds.playStation2MenuFramesPerSecond
    )

    AdaptiveAnimationTimeline(
      domain: .dynamicBackground,
      maximumFramesPerSecond: framesPerSecond
    ) { timeline in
      let motionSettings = theme.particleSettings.backgrounds
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * motionSettings.playStation2MenuSceneSpeed
      let cameraTime =
        max(0, timeline.date.timeIntervalSince(cameraStartDate))
        * motionSettings.playStation2MenuSceneSpeed
      let deepColor = theme.sharedColor(index: 0, time: time)
      let cloudColor = theme.sharedColor(index: 1, time: time)
      let reflectedColor = theme.ribbonColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        theme.paletteBackgroundColor(deepColor, darkness: 1)

        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(deepColor, darkness: 1),
            theme.paletteBackgroundColor(deepColor, darkness: 0.98),
            theme.paletteBackgroundColor(deepColor, darkness: 0.89),
            theme.paletteBackgroundColor(deepColor, darkness: 1),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        RadialGradient(
          colors: [
            cloudColor.opacity(0.2),
            reflectedColor.opacity(0.065),
            deepColor.opacity(0.035),
            .clear,
          ],
          center: UnitPoint(
            x: 0.53 + CGFloat(sin(time * 0.019)) * 0.035,
            y: 0.58 + CGFloat(cos(time * 0.016)) * 0.045
          ),
          startRadius: 18,
          endRadius: 700
        )

        DynamicWallpaperCanvas(
          opaque: false,
          colorMode: .nonLinear,
          rendersAsynchronously: true
        ) { context, size in
          guard size.width > 1, size.height > 1 else { return }

          let camera = PlayStation2MenuCamera(
            size: size,
            time: cameraTime,
            yawSpeed: motionSettings.playStation2MenuYawSpeed,
            rollSpeed: motionSettings.playStation2MenuRollSpeed,
            orbitDegrees: motionSettings.playStation2MenuOrbitDegrees
          )
          let towers = frameRates.reducedDynamicGeometry(
            towerConfigurations(camera: camera),
            minimum: 5
          )
          let crystals = frameRates.reducedDynamicGeometry(
            crystalConfigurations(camera: camera),
            minimum: 3
          )

          drawFog(
            context: &context,
            camera: camera,
            time: time,
            foreground: false
          )

          drawOrbTrails(
            context: &context,
            camera: camera,
            time: time,
            pass: .far
          )
          drawTowers(
            context: &context,
            camera: camera,
            time: time,
            pass: .far,
            configurations: towers
          )
          drawCrystals(
            context: &context,
            camera: camera,
            time: time,
            pass: .far,
            configurations: crystals
          )

          drawTowers(
            context: &context,
            camera: camera,
            time: time,
            pass: .middle,
            configurations: towers
          )
          drawOrbTrails(
            context: &context,
            camera: camera,
            time: time,
            pass: .middle
          )
          drawCrystals(
            context: &context,
            camera: camera,
            time: time,
            pass: .middle,
            configurations: crystals
          )

          drawTowers(
            context: &context,
            camera: camera,
            time: time,
            pass: .near,
            configurations: towers
          )
          drawTowerBaseClouds(
            context: &context,
            camera: camera,
            time: time,
            configurations: towers
          )
          drawCrystals(
            context: &context,
            camera: camera,
            time: time,
            pass: .near,
            configurations: crystals
          )
          drawOrbTrails(
            context: &context,
            camera: camera,
            time: time,
            pass: .near
          )
        }

        LinearGradient(
          colors: [
            theme.paletteDarkOverlay(opacity: 0.5),
            .clear,
            theme.paletteDarkOverlay(opacity: 0.24),
          ],
          startPoint: .top,
          endPoint: .bottom
        )

        RadialGradient(
          colors: [
            .clear,
            theme.paletteDarkOverlay(opacity: 0.76),
          ],
          center: .center,
          startRadius: 230,
          endRadius: 840
        )
      }
      .ignoresSafeArea()
      .accessibilityHidden(true)
    }
  }
}

// MARK: - PlayStation2MenuRenderer

extension PlayStation2MenuBackground {
  func drawFog(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    time: TimeInterval,
    foreground: Bool
  ) {
    let count = frameRates.scaledDynamicGeometryCount(
      foreground ? 4 : 7,
      minimum: foreground ? 2 : 3
    )
    context.drawLayer { fog in
      fog.blendMode = .plusLighter
      fog.addFilter(.blur(radius: foreground ? 72 : 62))

      for index in 0..<count {
        let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: foreground ? 31.73 : 13.19)
        let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: foreground ? 83.41 : 57.13)
        let seedC = DynamicBackgroundMath.seededUnit(
          index: index, salt: foreground ? 119.17 : 97.61)
        let depth =
          foreground
          ? 0.48 + seedC * 0.52
          : -1.08 + seedC * 0.9
        let worldCenter = PlayStation2MenuVector3(
          x: (-0.92 + seedA * 1.84) * camera.horizontalExtent,
          y: (-0.78 + seedB * 1.56) * camera.verticalExtent,
          z: depth
        )
        let projected = camera.project(worldCenter)
        let driftX = CGFloat(sin(time * (0.009 + seedA * 0.008) + seedB * 9)) * 34
        let driftY = CGFloat(cos(time * (0.008 + seedC * 0.007) + seedA * 11)) * 26
        let width = camera.size.width * CGFloat(0.2 + seedB * 0.31) * projected.scale
        let height = camera.size.height * CGFloat(0.08 + seedC * 0.15) * projected.scale
        let rect = CGRect(
          x: projected.point.x + driftX - width / 2,
          y: projected.point.y + driftY - height / 2,
          width: width,
          height: height
        )
        let color =
          index.isMultiple(of: 3)
          ? theme.sharedColor(index: index + 1, time: time)
          : theme.ribbonColor(index: index + 2, time: time)
        let opacity =
          foreground
          ? 0.018 + seedA * 0.012
          : 0.018 + seedA * 0.022

        fog.fill(
          Path(ellipseIn: rect),
          with: .color(color.opacity(opacity))
        )
      }
    }
  }
}

// MARK: - PlayStation2MenuOrbRenderer

extension PlayStation2MenuBackground {
  func drawOrbTrails(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    time: TimeInterval,
    pass: PlayStation2MenuDepthPass
  ) {
    let orbCount = frameRates.scaledDynamicGeometryCount(4, minimum: 1)
    let samples = frameRates.scaledDynamicGeometryCount(42, minimum: 14)

    for index in 0..<orbCount {
      let configuration = orbConfiguration(index: index, camera: camera)
      let moment = time * configuration.speed + configuration.phase
      let headWorld = orbPoint(moment: moment, configuration: configuration)
      let head = camera.project(headWorld)
      let passVisibility = pass.visibility(at: head.depth)
      guard passVisibility > 0.001 else { continue }

      let projections = (0...samples).map { sample in
        let sampleMoment = moment - Double(samples - sample) * 0.027
        return camera.project(
          orbPoint(moment: sampleMoment, configuration: configuration)
        )
      }
      let points = projections.map(\.point)
      guard let tailPoint = points.first, let headPoint = points.last else {
        continue
      }
      let color = theme.ribbonColor(index: index + 2, time: time)
      var completePath = Path()
      completePath.move(to: tailPoint)
      let maximumTangentLength = min(camera.size.width, camera.size.height) * 0.08

      for sample in 0..<(points.count - 1) {
        let segment = OrbTrailGeometry.segment(
          points: points,
          index: sample,
          maximumTangentLength: maximumTangentLength
        )
        completePath.addCurve(
          to: segment.end,
          control1: segment.control1,
          control2: segment.control2
        )
      }

      let normalizedDepth = max(0, min(1, (head.depth + 0.7) / 1.4))
      let depthProgress = normalizedDepth * normalizedDepth * (3 - 2 * normalizedDepth)
      let depthOpacity = 0.18 + depthProgress * 0.68
      let renderedOpacity = depthOpacity * passVisibility
      let baseWidth = 0.7 + CGFloat(depthProgress) * 0.65

      context.drawLayer { glow in
        glow.blendMode = .plusLighter
        glow.addFilter(.blur(radius: CGFloat(4 + depthProgress * 2)))
        glow.stroke(
          completePath,
          with: .linearGradient(
            Gradient(stops: [
              .init(color: .clear, location: 0),
              .init(color: color.opacity(renderedOpacity * 0.04), location: 0.18),
              .init(color: color.opacity(renderedOpacity * 0.2), location: 0.62),
              .init(color: color.opacity(renderedOpacity * 0.46), location: 1),
            ]),
            startPoint: tailPoint,
            endPoint: headPoint
          ),
          style: StrokeStyle(
            lineWidth: baseWidth * 3,
            lineCap: .round,
            lineJoin: .round
          )
        )
      }

      context.drawLayer { trail in
        trail.blendMode = .plusLighter
        trail.stroke(
          completePath,
          with: .linearGradient(
            Gradient(stops: [
              .init(color: .clear, location: 0),
              .init(color: color.opacity(renderedOpacity * 0.06), location: 0.16),
              .init(color: color.opacity(renderedOpacity * 0.38), location: 0.58),
              .init(color: color.opacity(renderedOpacity), location: 1),
            ]),
            startPoint: tailPoint,
            endPoint: headPoint
          ),
          style: StrokeStyle(
            lineWidth: baseWidth,
            lineCap: .round,
            lineJoin: .round
          )
        )
      }

      let diameter = (5.2 + CGFloat(depthProgress) * 2.3) * head.scale
      let orbRect = CGRect(
        x: head.point.x - diameter / 2,
        y: head.point.y - diameter / 2,
        width: diameter,
        height: diameter
      )

      context.drawLayer { orb in
        orb.blendMode = .plusLighter
        orb.addFilter(
          .shadow(color: color.opacity(renderedOpacity), radius: 10)
        )
        orb.fill(
          Path(ellipseIn: orbRect),
          with: .color(
            .white.opacity((0.55 + depthOpacity * 0.45) * passVisibility)
          )
        )
      }
    }
  }

  func towerConfigurations(
    camera: PlayStation2MenuCamera
  ) -> [PlayStation2MenuTowerConfiguration] {
    [
      makeTower(
        x: -0.82, y: -0.58, depth: 0.64,
        width: 0.54, height: 0.54, shaft: 0.8,
        angle: 0.012, phase: 0.2, camera: camera
      ),
      makeTower(
        x: -0.84, y: -0.08, depth: 0.2,
        width: 0.38, height: 0.38, shaft: 0.5,
        angle: -0.018, phase: 1.1, camera: camera
      ),
      makeTower(
        x: -0.82, y: 0.34, depth: 0.46,
        width: 0.46, height: 0.46, shaft: 0.68,
        angle: 0.01, phase: 2.05, camera: camera
      ),
      makeTower(
        x: -0.42, y: -0.82, depth: 0.12,
        width: 0.44, height: 0.44, shaft: 0.62,
        angle: 0.016, phase: 4.0, camera: camera
      ),
      makeTower(
        x: -0.04, y: -0.84, depth: -0.35,
        width: 0.25, height: 0.25, shaft: 0.34,
        angle: -0.012, phase: 4.8, camera: camera
      ),
      makeTower(
        x: 0.72, y: -0.66, depth: 0.58,
        width: 0.52, height: 0.52, shaft: 0.74,
        angle: -0.016, phase: 5.7, camera: camera
      ),
      makeTower(
        x: 0.84, y: 0.02, depth: 0.24,
        width: 0.4, height: 0.4, shaft: 0.56,
        angle: 0.012, phase: 6.5, camera: camera
      ),
      makeTower(
        x: 0.24, y: 0.08, depth: -0.18,
        width: 0.2, height: 0.2, shaft: 0.18,
        angle: -0.01, phase: 7.1, camera: camera,
        isShortTower: true
      ),
      makeTower(
        x: 0.42, y: 0.34, depth: 0.06,
        width: 0.26, height: 0.26, shaft: 0.22,
        angle: 0.012, phase: 7.7, camera: camera,
        isShortTower: true
      ),
      makeTower(
        x: 0.58, y: 0.58, depth: 0.28,
        width: 0.23, height: 0.23, shaft: 0.16,
        angle: -0.014, phase: 8.3, camera: camera,
        isShortTower: true
      ),
      makeTower(
        x: 0.7, y: 0.82, depth: 0.4,
        width: 0.29, height: 0.29, shaft: 0.24,
        angle: 0.01, phase: 8.9, camera: camera,
        isShortTower: true
      ),
      makeTower(
        x: -0.46, y: 0.9, depth: 0.18,
        width: 0.36, height: 0.36, shaft: 0.48,
        angle: -0.012, phase: 8.2, camera: camera
      ),
      makeTower(
        x: -0.08, y: 0.91, depth: -0.12,
        width: 0.32, height: 0.32, shaft: 0.46,
        angle: 0.014, phase: 9.1, camera: camera
      ),
    ]
  }

  func makeTower(
    x: Double,
    y: Double,
    depth: Double,
    width: Double,
    height: Double,
    shaft: Double,
    angle: Double,
    phase: Double,
    camera: PlayStation2MenuCamera,
    isInner: Bool = false,
    isShortTower: Bool = false
  ) -> PlayStation2MenuTowerConfiguration {
    let variation = DynamicBackgroundMath.unit(abs(phase) * 0.37)

    return PlayStation2MenuTowerConfiguration(
      center: PlayStation2MenuVector3(
        x: x * camera.horizontalExtent,
        y: y * camera.verticalExtent,
        z: depth
      ),
      size: PlayStation2MenuVector3(
        x: width,
        y: height,
        z: shaft
      ),
      rotation: PlayStation2MenuRotation(
        x: 0,
        y: 0,
        z: angle
      ),
      pulseSpeed: 0.04 + variation * 0.025,
      pulseAmount: isShortTower
        ? 0.028
        : (isInner ? 0.025 : 0.035 + variation * 0.03),
      phase: phase,
      isInner: isInner
    )
  }

  func crystalConfigurations(
    camera: PlayStation2MenuCamera
  ) -> [PlayStation2MenuCrystalConfiguration] {
    let x = camera.horizontalExtent
    let y = camera.verticalExtent

    return [
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: 0.08 * x, y: -0.3 * y, z: 0.16),
        side: 0.38,
        phase: 0.35,
        rotationSpeed: 0.035,
        tintIndex: 1
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: -0.62 * x, y: -0.18 * y, z: -0.18),
        side: 0.23,
        phase: 1.7,
        rotationSpeed: 0.028,
        tintIndex: 3
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: 0.68 * x, y: -0.58 * y, z: 0.28),
        side: 0.28,
        phase: 2.8,
        rotationSpeed: 0.032,
        tintIndex: 5
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: 0.76 * x, y: 0.03 * y, z: -0.34),
        side: 0.2,
        phase: 4.2,
        rotationSpeed: 0.026,
        tintIndex: 7
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: 0.82 * x, y: 0.67 * y, z: 0.72),
        side: 0.44,
        phase: 5.5,
        rotationSpeed: 0.024,
        tintIndex: 9
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: -0.8 * x, y: 0.66 * y, z: 0.58),
        side: 0.36,
        phase: 3.9,
        rotationSpeed: 0.027,
        tintIndex: 11
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: -0.73 * x, y: -0.68 * y, z: 0.42),
        side: 0.27,
        phase: 0.9,
        rotationSpeed: 0.03,
        tintIndex: 13
      ),
      PlayStation2MenuCrystalConfiguration(
        center: PlayStation2MenuVector3(x: 0.18 * x, y: 0.62 * y, z: -0.62),
        side: 0.18,
        phase: 2.15,
        rotationSpeed: 0.029,
        tintIndex: 15
      ),
    ]
  }

  func orbConfiguration(
    index: Int,
    camera: PlayStation2MenuCamera
  ) -> PlayStation2MenuOrbConfiguration {
    let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 23.13)
    let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 71.79)
    let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 109.11)
    let seedD = DynamicBackgroundMath.seededUnit(index: index, salt: 157.33)

    return PlayStation2MenuOrbConfiguration(
      center: PlayStation2MenuVector3(
        x: (-0.22 + seedA * 0.44) * camera.horizontalExtent,
        y: (-0.2 + seedB * 0.4) * camera.verticalExtent,
        z: 0
      ),
      radiusX: camera.horizontalExtent * (0.46 + seedC * 0.36),
      radiusY: camera.verticalExtent * (0.24 + seedD * 0.25),
      bend: camera.verticalExtent * (0.08 + seedA * 0.13),
      speed: 0.052 + seedB * 0.035,
      phase: seedC * .pi * 2,
      depthPhase: seedD * .pi * 2
    )
  }

  func orbPoint(
    moment: Double,
    configuration: PlayStation2MenuOrbConfiguration
  ) -> PlayStation2MenuVector3 {
    PlayStation2MenuVector3(
      x: configuration.center.x
        + cos(moment) * configuration.radiusX
        + sin(moment * 1.73 + configuration.depthPhase) * configuration.radiusX * 0.14,
      y: configuration.center.y
        + sin(moment * 0.78 + configuration.depthPhase * 0.2) * configuration.radiusY
        + cos(moment * 0.43) * configuration.bend,
      z: sin(moment * 0.83 + configuration.depthPhase) * 0.96
    )
  }

  func cuboidVertices(
    center: PlayStation2MenuVector3,
    halfSize: PlayStation2MenuVector3,
    rotation: PlayStation2MenuRotation,
    camera: PlayStation2MenuCamera
  ) -> [PlayStation2MenuProjection] {
    [
      PlayStation2MenuVector3(x: -halfSize.x, y: -halfSize.y, z: -halfSize.z),
      PlayStation2MenuVector3(x: halfSize.x, y: -halfSize.y, z: -halfSize.z),
      PlayStation2MenuVector3(x: halfSize.x, y: halfSize.y, z: -halfSize.z),
      PlayStation2MenuVector3(x: -halfSize.x, y: halfSize.y, z: -halfSize.z),
      PlayStation2MenuVector3(x: -halfSize.x, y: -halfSize.y, z: halfSize.z),
      PlayStation2MenuVector3(x: halfSize.x, y: -halfSize.y, z: halfSize.z),
      PlayStation2MenuVector3(x: halfSize.x, y: halfSize.y, z: halfSize.z),
      PlayStation2MenuVector3(x: -halfSize.x, y: halfSize.y, z: halfSize.z),
    ].map { local in
      let rotated =
        local
        .rotatedX(rotation.x)
        .rotatedY(rotation.y)
        .rotatedZ(rotation.z)
      return camera.project(center + rotated)
    }
  }

  func averageDepth(
    _ face: [Int],
    vertices: [PlayStation2MenuProjection]
  ) -> Double {
    face.reduce(0) { $0 + vertices[$1].depth } / Double(face.count)
  }

  func polygonPath(_ points: [CGPoint]) -> Path {
    var path = Path()
    guard let first = points.first else { return path }

    path.move(to: first)
    for point in points.dropFirst() {
      path.addLine(to: point)
    }
    path.closeSubpath()
    return path
  }

  func averagedPoint(_ points: [CGPoint]) -> CGPoint {
    guard !points.isEmpty else { return .zero }

    let total = points.reduce(CGPoint.zero) { result, point in
      CGPoint(x: result.x + point.x, y: result.y + point.y)
    }
    return CGPoint(
      x: total.x / CGFloat(points.count),
      y: total.y / CGFloat(points.count)
    )
  }

  func quadraticPoint(
    from start: CGPoint,
    control: CGPoint,
    to end: CGPoint,
    progress: CGFloat
  ) -> CGPoint {
    let inverse = 1 - progress
    return CGPoint(
      x: inverse * inverse * start.x
        + 2 * inverse * progress * control.x
        + progress * progress * end.x,
      y: inverse * inverse * start.y
        + 2 * inverse * progress * control.y
        + progress * progress * end.y
    )
  }
}
