// TowersOrbsBackground.swift — towers and orbs background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - TowersOrbsBackground

struct TowersOrbsBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.towersOrbsSpeed
      let primaryColor = theme.sharedColor(index: 0, time: time)
      let secondaryColor = theme.ribbonColor(index: 1, time: time)
      let darkColor = theme.sharedColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .top,
        to: .bottom
      )

      ZStack {
        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(darkColor, darkness: 0.98),
            theme.paletteBackgroundColor(darkColor, darkness: 0.72),
            theme.paletteBackgroundColor(primaryColor, darkness: 0.76),
            theme.paletteBackgroundColor(primaryColor, darkness: 1),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
          drawOrbitingOrbs(
            context: &context,
            size: size,
            time: time,
            drawsFront: false
          )
          drawTowers(context: &context, size: size, time: time)
          drawOrbitingOrbs(
            context: &context,
            size: size,
            time: time,
            drawsFront: true
          )
        }

        RadialGradient(
          colors: [secondaryColor.opacity(0.24), .clear],
          center: .topTrailing,
          startRadius: 10,
          endRadius: 360
        )
      }
      .ignoresSafeArea()
    }
  }

  // Draws layered PS2-style towers with ribbon-colored faces and edges.
  private func drawTowers(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval
  ) {
    let depthCount = frameRates.scaledDynamicGeometryCount(5, minimum: 2)
    let columnCount = frameRates.scaledDynamicGeometryCount(8, minimum: 4)
    for depth in 0..<depthCount {
      for column in 0..<columnCount {
        let index = depth * columnCount + column
        let towerColor = theme.ribbonColor(index: index, time: time)
        let seed = Double(index) * 1.731
        let spacing = size.width / CGFloat(max(1, columnCount - 1))
        let x =
          CGFloat(column) * spacing
          + CGFloat(depth % 2) * spacing * 0.48
          - spacing * 0.22
          + CGFloat(sin(time * 0.035 + seed)) * 4
        let bottom = size.height * (0.62 + CGFloat(depth) * 0.085)
        let towerWidth = max(8, size.width * CGFloat(0.018 + Double(index % 4) * 0.005))
        let towerHeight =
          size.height
          * CGFloat(
            0.13 + (sin(seed * 1.21) * 0.5 + 0.5) * 0.38
          )
        let top = bottom - towerHeight
        let depthOffset = towerWidth * 0.48
        let depthRise = towerWidth * 0.28
        let frontRect = CGRect(
          x: x - towerWidth / 2,
          y: top,
          width: towerWidth,
          height: towerHeight
        )
        context.fill(
          Path(frontRect),
          with: .linearGradient(
            Gradient(colors: [
              towerColor.opacity(0.34),
              towerColor.opacity(0.08),
            ]),
            startPoint: CGPoint(x: x, y: top),
            endPoint: CGPoint(x: x, y: bottom)
          )
        )
        context.stroke(
          Path(frontRect),
          with: .color(towerColor.opacity(0.34)),
          lineWidth: 0.7
        )

        var side = Path()
        side.move(to: CGPoint(x: frontRect.maxX, y: frontRect.minY))
        side.addLine(
          to: CGPoint(
            x: frontRect.maxX + depthOffset,
            y: frontRect.minY - depthRise
          )
        )
        side.addLine(
          to: CGPoint(
            x: frontRect.maxX + depthOffset,
            y: frontRect.maxY - depthRise
          )
        )
        side.addLine(to: CGPoint(x: frontRect.maxX, y: frontRect.maxY))
        side.closeSubpath()
        context.fill(side, with: .color(towerColor.opacity(0.1)))
        context.stroke(
          side,
          with: .color(towerColor.opacity(0.24)),
          lineWidth: 0.6
        )

        var topFace = Path()
        topFace.move(to: CGPoint(x: frontRect.minX, y: frontRect.minY))
        topFace.addLine(to: CGPoint(x: frontRect.maxX, y: frontRect.minY))
        topFace.addLine(
          to: CGPoint(
            x: frontRect.maxX + depthOffset,
            y: frontRect.minY - depthRise
          )
        )
        topFace.addLine(
          to: CGPoint(
            x: frontRect.minX + depthOffset,
            y: frontRect.minY - depthRise
          )
        )
        topFace.closeSubpath()
        context.fill(topFace, with: .color(towerColor.opacity(0.42)))
        context.stroke(
          topFace,
          with: .color(.white.opacity(0.16)),
          lineWidth: 0.5
        )
      }
    }
  }

  // Draws orbital trails in back and front passes so each orb appears to wrap around.
  private func drawOrbitingOrbs(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    drawsFront: Bool
  ) {
    let orbCount = frameRates.scaledDynamicGeometryCount(4, minimum: 1)
    let trailCount = frameRates.scaledDynamicGeometryCount(13, minimum: 5)
    for orbIndex in 0..<orbCount {
      let orbColor = theme.ribbonColor(index: orbIndex, time: time)
      let configuration = orbitConfiguration(index: orbIndex, size: size)
      let currentMoment = time + configuration.startOffset

      for tailIndex in 0..<trailCount {
        let firstMoment = currentMoment - Double(tailIndex) * 0.12
        let secondMoment = currentMoment - Double(tailIndex + 1) * 0.12
        let middleDepth = orbitDepth(
          moment: (firstMoment + secondMoment) / 2,
          configuration: configuration
        )
        guard (middleDepth >= 0) == drawsFront else { continue }

        let first = orbitPoint(
          moment: firstMoment,
          configuration: configuration
        )
        let second = orbitPoint(
          moment: secondMoment,
          configuration: configuration
        )
        let delta = secondMoment - firstMoment
        let firstTangent = orbitTangent(
          moment: firstMoment,
          configuration: configuration
        )
        let secondTangent = orbitTangent(
          moment: secondMoment,
          configuration: configuration
        )
        let control1 = CGPoint(
          x: first.x + firstTangent.width * delta / 3,
          y: first.y + firstTangent.height * delta / 3
        )
        let control2 = CGPoint(
          x: second.x - secondTangent.width * delta / 3,
          y: second.y - secondTangent.height * delta / 3
        )
        var tail = Path()
        tail.move(to: first)
        tail.addCurve(
          to: second,
          control1: control1,
          control2: control2
        )

        let fade = 1 - Double(tailIndex) / 13
        context.stroke(
          tail,
          with: .color(orbColor.opacity(fade * (drawsFront ? 0.72 : 0.2))),
          style: StrokeStyle(
            lineWidth: CGFloat(0.6 + fade * 2.1),
            lineCap: .round
          )
        )
      }

      let currentDepth = orbitDepth(
        moment: currentMoment,
        configuration: configuration
      )
      guard (currentDepth >= 0) == drawsFront else { continue }
      let point = orbitPoint(
        moment: currentMoment,
        configuration: configuration
      )
      let depthScale = CGFloat(0.72 + (currentDepth + 1) * 0.2)
      let diameter = CGFloat(7 + orbIndex) * depthScale
      let rect = CGRect(
        x: point.x - diameter / 2,
        y: point.y - diameter / 2,
        width: diameter,
        height: diameter
      )

      context.drawLayer { glow in
        glow.addFilter(.shadow(color: orbColor.opacity(0.9), radius: 8))
        glow.fill(
          Path(ellipseIn: rect),
          with: .color(
            drawsFront ? .white : orbColor.opacity(0.45)
          )
        )
      }
    }
  }

  // Creates seeded start positions and non-circular movement values for each orb.
  private func orbitConfiguration(
    index: Int,
    size: CGSize
  ) -> TowerOrbitConfiguration {
    let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 17.129)
    let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 31.415)
    let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 57.721)
    let seedD = DynamicBackgroundMath.seededUnit(index: index, salt: 83.113)
    let seedE = DynamicBackgroundMath.seededUnit(index: index, salt: 109.37)
    let seedF = DynamicBackgroundMath.seededUnit(index: index, salt: 139.81)

    return TowerOrbitConfiguration(
      center: CGPoint(
        x: size.width * CGFloat(0.18 + seedA * 0.64),
        y: size.height * CGFloat(0.32 + seedB * 0.36)
      ),
      radiusX: size.width * CGFloat(0.1 + seedC * 0.17),
      radiusY: size.height * CGFloat(0.05 + seedD * 0.09),
      wobbleX: size.width * CGFloat(0.025 + seedE * 0.065),
      wobbleY: size.height * CGFloat(0.02 + seedF * 0.06),
      speedX: 0.16 + seedB * 0.18,
      speedY: 0.2 + seedC * 0.2,
      wobbleSpeed: 0.34 + seedD * 0.32,
      phaseX: seedE * .pi * 2,
      phaseY: seedF * .pi * 2,
      wobblePhase: seedA * .pi * 2,
      depthSpeed: 0.22 + seedF * 0.18,
      depthPhase: seedC * .pi * 2,
      startOffset: seedD * 11
    )
  }

  // Returns the current point on a layered, non-circular orb path.
  private func orbitPoint(
    moment: Double,
    configuration: TowerOrbitConfiguration
  ) -> CGPoint {
    CGPoint(
      x: configuration.center.x
        + CGFloat(cos(moment * configuration.speedX + configuration.phaseX))
        * configuration.radiusX
        + CGFloat(sin(moment * configuration.wobbleSpeed + configuration.wobblePhase))
        * configuration.wobbleX
        + CGFloat(cos(moment * 0.11 + configuration.phaseY))
        * configuration.radiusX
        * 0.18,
      y: configuration.center.y
        + CGFloat(sin(moment * configuration.speedY + configuration.phaseY))
        * configuration.radiusY
        + CGFloat(cos(moment * configuration.wobbleSpeed * 0.82 + configuration.wobblePhase))
        * configuration.wobbleY
        + CGFloat(sin(moment * 0.13 + configuration.phaseX))
        * configuration.radiusY
        * 0.22
    )
  }

  // Samples nearby points to curve the orb trail along the seeded path.
  private func orbitTangent(
    moment: Double,
    configuration: TowerOrbitConfiguration
  ) -> CGSize {
    let delta = 0.01
    let first = orbitPoint(
      moment: moment - delta,
      configuration: configuration
    )
    let second = orbitPoint(
      moment: moment + delta,
      configuration: configuration
    )

    return CGSize(
      width: (second.x - first.x) / CGFloat(delta * 2),
      height: (second.y - first.y) / CGFloat(delta * 2)
    )
  }

  // Calculates whether an orb is currently behind or in front of the towers.
  private func orbitDepth(
    moment: Double,
    configuration: TowerOrbitConfiguration
  ) -> Double {
    sin(moment * configuration.depthSpeed + configuration.depthPhase)
  }
}

private struct TowerOrbitConfiguration {
  let center: CGPoint
  let radiusX: CGFloat
  let radiusY: CGFloat
  let wobbleX: CGFloat
  let wobbleY: CGFloat
  let speedX: Double
  let speedY: Double
  let wobbleSpeed: Double
  let phaseX: Double
  let phaseY: Double
  let wobblePhase: Double
  let depthSpeed: Double
  let depthPhase: Double
  let startOffset: Double
}
