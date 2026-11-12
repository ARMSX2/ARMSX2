// LightSpeedBackground.swift — light speed background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - LightSpeedBackground

struct LightSpeedBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.lightSpeedMotionSpeed
      let deepColor = theme.sharedColor(index: 0, time: time)
      let primaryColor = theme.sharedColor(index: 1, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        theme.paletteBackgroundColor(deepColor, darkness: 1)

        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(deepColor, darkness: 1),
            theme.paletteBackgroundColor(deepColor, darkness: 0.86),
            theme.paletteBackgroundColor(primaryColor, darkness: 0.95),
            theme.paletteBackgroundColor(primaryColor, darkness: 1),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
          let vanishingPoint = vanishingPoint(size: size, time: time)

          drawBackgroundNebulas(
            context: &context,
            size: size,
            time: time
          )
          drawStarParticles(
            context: &context,
            size: size,
            time: time
          )
          drawLightSpeedStreaks(
            context: &context,
            size: size,
            time: time,
            vanishingPoint: vanishingPoint
          )
        }

        LinearGradient(
          colors: [
            theme.paletteDarkOverlay(opacity: 0.12),
            .clear,
            theme.paletteDarkOverlay(opacity: 0.18),
          ],
          startPoint: .top,
          endPoint: .bottom
        )

        RadialGradient(
          colors: [
            .clear,
            theme.paletteDarkOverlay(opacity: 0.54),
          ],
          center: .center,
          startRadius: 260,
          endRadius: 780
        )
      }
      .ignoresSafeArea()
    }
  }

  private var settings: DynamicParticleSettings {
    theme.particleSettings
  }

  private func drawLightSpeedStreaks(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    vanishingPoint: CGPoint
  ) {
    context.blendMode = .plusLighter

    let diagonal = hypot(size.width, size.height)
    let sizeScale = CGFloat(max(0.45, settings.size))
    let brightness = max(0.42, settings.brightness)
    let speed = max(0.25, settings.speed) * 0.25
    let count = min(
      frameRates.scaledDynamicEffectCount(260),
      max(
        frameRates.scaledDynamicEffectCount(90),
        frameRates.scaledDynamicEffectCount(150 * settings.amount)
      )
    )

    for index in 0..<count {
      let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 22.77)
      let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 65.21)
      let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 91.73)
      let angle =
        seedA * .pi * 2
        + sin(time * 0.02 + seedB * 10) * 0.055 * settings.dispersion
      let progress = DynamicBackgroundMath.unit(
        seedB
          + time * (0.26 + seedC * 0.72) * speed
      )
      let fadeIn = DynamicBackgroundMath.smoothstep(value: progress / 0.16)
      let fadeOut = 1 - DynamicBackgroundMath.smoothstep(value: (progress - 0.76) / 0.24)
      let lifecycleFade = max(0, fadeIn * fadeOut)
      let travel = CGFloat(pow(progress, 1.86))
      let direction = CGVector(
        dx: CGFloat(cos(angle)),
        dy: CGFloat(sin(angle))
      )
      let radius = diagonal * (0.016 + travel * 0.96)
      let length =
        min(
          diagonal * 0.24,
          CGFloat(1.4 + pow(progress, 2.7) * 250)
            * (0.48 + CGFloat(seedC) * 0.74)
            * sizeScale
        )
      let start = CGPoint(
        x: vanishingPoint.x + direction.dx * max(2, radius - length * 0.18),
        y: vanishingPoint.y + direction.dy * max(2, radius - length * 0.18)
      )
      let end = CGPoint(
        x: vanishingPoint.x + direction.dx * (radius + length * 0.82),
        y: vanishingPoint.y + direction.dy * (radius + length * 0.82)
      )
      let color =
        index.isMultiple(of: 9)
        ? theme.highlightColor(index: index, time: time)
        : theme.ribbonColor(index: index, time: time)
      let opacity =
        min(
          1,
          (0.08 + seedC * 0.26 + Double(travel) * 0.82)
            * lifecycleFade
            * brightness
        )
      let lineWidth =
        (0.18 + CGFloat(seedC) * 0.42 + travel * 2.25)
        * sizeScale

      drawWarpStreak(
        context: &context,
        start: start,
        end: end,
        color: color,
        opacity: opacity,
        lineWidth: lineWidth
      )
    }
  }

  private func drawBackgroundNebulas(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval
  ) {
    let nebulaCount = frameRates.scaledDynamicGeometryCount(7, minimum: 3)
    for index in 0..<nebulaCount {
      let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 13.19)
      let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 71.73)
      let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 41.11)
      let phase = time * (0.008 + seedA * 0.006)
      let center = CGPoint(
        x: size.width * CGFloat(0.08 + seedB * 0.84)
          + CGFloat(sin(phase + seedA * 8)) * size.width * 0.045,
        y: size.height * CGFloat(0.06 + seedC * 0.88)
          + CGFloat(cos(phase * 0.9 + seedB * 7)) * size.height * 0.04
      )
      let width = size.width * CGFloat(0.26 + seedA * 0.46)
      let height = size.height * CGFloat(0.11 + seedB * 0.24)
      let color =
        index.isMultiple(of: 2)
        ? theme.sharedColor(index: index, time: time)
        : theme.ribbonColor(index: index, time: time)
      let opacity = (0.012 + seedC * 0.016) * max(0.45, settings.brightness)

      context.drawLayer { nebula in
        nebula.blendMode = .plusLighter
        nebula.addFilter(.blur(radius: 54 + CGFloat(index % 3) * 18))
        nebula.fill(
          Path(ellipseIn: rect(center: center, width: width, height: height)),
          with: .color(color.opacity(opacity))
        )
      }
    }
  }

  private func drawStarParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval
  ) {
    context.blendMode = .plusLighter

    let count = min(
      frameRates.scaledDynamicEffectCount(360),
      max(
        frameRates.scaledDynamicEffectCount(130),
        frameRates.scaledDynamicEffectCount(190 * settings.amount)
      )
    )
    let speed = max(0.12, settings.speed) * 0.18
    let sizeScale = CGFloat(max(0.45, settings.size))
    let brightness = max(0.36, settings.brightness)

    for index in 0..<count {
      let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 12.9898)
      let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 78.233)
      let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 37.719)
      let progress = DynamicBackgroundMath.unit(seedB + time * (0.006 + seedC * 0.008) * speed)
      let x =
        size.width * CGFloat(seedA)
        + CGFloat(sin(time * 0.015 + seedB * 9)) * 10 * CGFloat(settings.drift)
      let y =
        size.height * CGFloat(1 - progress)
        + CGFloat(cos(time * 0.012 + seedA * 8)) * 8 * CGFloat(settings.drift)
      let twinkle = pow(max(0, sin(time * (0.48 + seedC * 0.42) + seedA * 11)), 3)
      let side =
        CGFloat(index.isMultiple(of: 11) ? 1.8 : 0.75)
        * sizeScale
        * CGFloat(0.75 + twinkle * 0.6)
      let color =
        index.isMultiple(of: 13)
        ? theme.highlightColor(index: index, time: time)
        : theme.ribbonColor(index: index, time: time)
      let opacity =
        (index.isMultiple(of: 11) ? 0.26 : 0.12)
        + twinkle * 0.24

      context.fill(
        Path(
          CGRect(
            x: x - side / 2,
            y: y - side / 2,
            width: side,
            height: side
          )
        ),
        with: .color(color.opacity(opacity * brightness))
      )
    }
  }

  private func drawWarpStreak(
    context: inout GraphicsContext,
    start: CGPoint,
    end: CGPoint,
    color: Color,
    opacity: Double,
    lineWidth: CGFloat
  ) {
    var path = Path()
    path.move(to: start)
    path.addLine(to: end)

    context.stroke(
      path,
      with: .color(color.opacity(opacity)),
      style: StrokeStyle(
        lineWidth: lineWidth,
        lineCap: .round
      )
    )
  }

  private func vanishingPoint(size: CGSize, time: TimeInterval) -> CGPoint {
    CGPoint(
      x: size.width * (0.5 + CGFloat(sin(time * 0.036)) * 0.012),
      y: size.height * (0.5 + CGFloat(cos(time * 0.031)) * 0.01)
    )
  }
  private func rect(center: CGPoint, width: CGFloat, height: CGFloat) -> CGRect {
    CGRect(
      x: center.x - width / 2,
      y: center.y - height / 2,
      width: width,
      height: height
    )
  }

}
