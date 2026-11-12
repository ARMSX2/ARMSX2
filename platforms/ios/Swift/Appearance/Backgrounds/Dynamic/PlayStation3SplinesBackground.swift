// PlayStation3SplinesBackground.swift — PS3 splines background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation3SplinesBackground

enum PlayStationRibbonSizing {
  case playStation3
  case playStation4
}

struct PlayStation3SplinesBackground: View {
  let theme: DynamicBackgroundTheme
  let ribbonSizing: PlayStationRibbonSizing
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.playStation3SplinesSpeed
      let settings = theme.particleSettings.backgrounds.playStation3Splines
      let dark = theme.sharedColor(index: 0, time: time)
      let primary = theme.sharedColor(index: 1, time: time)
      let ribbonPrimary = theme.ribbonColor(index: 1, time: time)
      let highlight = theme.ribbonColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(
              dark,
              darkness: 1 - min(1, 0.58 * settings.backgroundIntensity)
            ),
            theme.paletteBackgroundColor(
              primary,
              darkness: 1 - min(1, 0.7 * settings.backgroundIntensity)
            ),
            theme.paletteBackgroundColor(
              primary,
              darkness: 1 - min(1, 0.46 * settings.backgroundIntensity)
            ),
            theme.paletteBackgroundColor(
              highlight,
              darkness: 1 - min(1, 0.24 * settings.backgroundIntensity)
            ),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        RadialGradient(
          colors: [
            highlight.opacity(0.16 * settings.backgroundIntensity),
            primary.opacity(0.08 * settings.backgroundIntensity),
            .clear,
          ],
          center: .bottomTrailing,
          startRadius: 8,
          endRadius: 520
        )

        DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
          if settings.showsParticles {
            drawParticles(
              context: &context,
              size: size,
              time: time,
              color: highlight
            )
          }
          if settings.showsSplines {
            drawSplineBundle(
              context: &context,
              size: size,
              time: time,
              primary: ribbonPrimary,
              highlight: highlight
            )
          }
        }

        LinearGradient(
          colors: [
            theme.paletteDarkOverlay(
              opacity: 0.12 * settings.vignetteIntensity
            ),
            .clear,
            theme.paletteDarkOverlay(
              opacity: 0.18 * settings.vignetteIntensity
            ),
          ],
          startPoint: .top,
          endPoint: .bottom
        )
      }
      .ignoresSafeArea()
    }
  }

  // Draws the XMB3 ribbon bundle; XMB4 reuses it with PS4-sized stroke widths.
  private func drawSplineBundle(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    primary: Color,
    highlight: Color
  ) {
    let strandCount = frameRates.scaledDynamicGeometryCount(
      settings.strandCount
    )

    for index in 0..<strandCount {
      let path = splinePath(size: size, time: time, index: index)
      let fade = 1 - Double(index) / Double(strandCount + 4)
      let color = index.isMultiple(of: 4) ? highlight : primary
      let widths = ribbonStrokeWidths(index: index)

      context.drawLayer { glow in
        glow.blendMode = .plusLighter
        glow.addFilter(
          .blur(radius: ribbonGlowBlur(index: index) * CGFloat(settings.glowBlur))
        )
        glow.stroke(
          path,
          with: .color(color.opacity(fade * 0.075 * settings.glowOpacity)),
          style: StrokeStyle(
            lineWidth: widths.glow * CGFloat(settings.glowWidth),
            lineCap: .round,
            lineJoin: .round
          )
        )
      }

      context.drawLayer { strand in
        strand.blendMode = .plusLighter
        strand.stroke(
          path,
          with: .color(
            index.isMultiple(of: 5)
              ? theme.highlightColor(index: index + 8, time: time)
                .opacity(fade * 0.58 * settings.coreOpacity)
              : color.opacity(fade * 0.3 * settings.coreOpacity)
          ),
          style: StrokeStyle(
            lineWidth: widths.core * CGFloat(settings.coreWidth),
            lineCap: .round,
            lineJoin: .round
          )
        )
      }
    }
  }

  // Draws small twinkling particles around the spline bundle.
  private func drawParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    color: Color
  ) {
    context.blendMode = .plusLighter

    let particleCount = frameRates.scaledDynamicEffectCount(settings.particleCount)
    let particleTime = time * settings.particleSpeed

    for index in 0..<particleCount {
      let seed = DynamicBackgroundMath.unit(sin(Double(index + 1) * 12.9898) * 43_758.5453)
      let secondarySeed = DynamicBackgroundMath.unit(
        sin(Double(index + 7) * 78.233) * 9_631.417
      )
      let speed = 0.006 + Double(index % 7) * 0.0011
      let progress = DynamicBackgroundMath.unit(seed + particleTime * speed)
      let x = CGFloat(progress) * size.width
      let waveCenter =
        size.height
        * (CGFloat(settings.verticalPosition) + 0.02
          + 0.095
          * CGFloat(
            sin(progress * .pi * 2.1 - particleTime * 0.11 + seed * 5)
          ))
      let spread =
        (CGFloat(secondarySeed) - 0.5)
        * size.height
        * (0.18 + CGFloat(seed) * 0.2)
        * CGFloat(settings.particleSpread)
      let y =
        waveCenter + spread
        + CGFloat(sin(particleTime * 0.24 + seed * 18)) * 7
        * CGFloat(settings.particleSpread)
      let twinkle = pow(
        max(
          0,
          sin(particleTime * settings.particleTwinkle * (0.9 + seed) + secondarySeed * 14)
        ),
        3
      )
      let diameter =
        (CGFloat(0.7 + Double(index % 5) * 0.34)
          + CGFloat(twinkle) * 1.35) * CGFloat(settings.particleSize)
      let rect = CGRect(
        x: x - diameter / 2,
        y: y - diameter / 2,
        width: diameter,
        height: diameter
      )

      context.fill(
        Path(ellipseIn: rect),
        with: .color(
          index.isMultiple(of: 8)
            ? .white.opacity((0.35 + twinkle * 0.5) * settings.particleOpacity)
            : color.opacity((0.12 + twinkle * 0.28) * settings.particleOpacity)
        )
      )
    }
  }

  // Builds one animated XMB3 spline from sampled sine waves.
  private func splinePath(
    size: CGSize,
    time: TimeInterval,
    index: Int
  ) -> Path {
    let strand = Double(index)
    let direction = index.isMultiple(of: 3) ? -1.0 : 1.0
    let phase =
      time * (0.1 + strand * 0.0025) * direction
      + strand * 0.24 * settings.phaseSpread
    let centeredIndex =
      CGFloat(index)
        - CGFloat(
          max(
            0,
            frameRates.scaledDynamicGeometryCount(settings.strandCount) - 1
          )
        ) / 2
    let baseY =
      size.height
      * (CGFloat(settings.verticalPosition)
        + centeredIndex * 0.0095 * CGFloat(settings.strandSpacing))
    let amplitude =
      size.height
      * (0.09 + CGFloat(index % 4) * 0.008)
      * CGFloat(settings.waveAmplitude)

    let sampleCount = frameRates.scaledDynamicGeometryCount(32, minimum: 12)
    let points = (0...sampleCount).map { step in
      let progress = CGFloat(step) / CGFloat(sampleCount)
      let progressValue = Double(progress)
      let primaryWave = sin(progressValue * .pi * 2.05 + phase)
      let detailWave = sin(
        progressValue * .pi * 4.1 - phase * 0.44 + strand * 0.33
      )
      let y =
        baseY
        + amplitude * CGFloat(primaryWave)
        + size.height * 0.018 * CGFloat(settings.detailAmplitude) * CGFloat(detailWave)

      return CGPoint(
        x: -size.width * 0.08 + progress * size.width * 1.16,
        y: y
      )
    }

    return DynamicBackgroundGeometry.smoothCurve(through: points)
  }

  // Chooses the glow and core widths for XMB3 or the wider XMB4 variant.
  private func ribbonStrokeWidths(index: Int) -> (glow: CGFloat, core: CGFloat) {
    switch ribbonSizing {
    case .playStation3:
      return (
        glow: 11 + CGFloat(index % 5) * 2.8,
        core: 0.55 + CGFloat(index % 4) * 0.34
      )
    case .playStation4:
      let waveIndex = index % 8
      return (
        glow: CGFloat(20 - waveIndex),
        core: CGFloat(0.7 + Double(7 - waveIndex) * 0.4)
      )
    }
  }

  // Matches the blur radius to the selected ribbon size profile.
  private func ribbonGlowBlur(index: Int) -> CGFloat {
    switch ribbonSizing {
    case .playStation3:
      return 10 + CGFloat(index % 4) * 2.5
    case .playStation4:
      return 18
    }
  }

  // Converts any floating value into a stable 0...1 seed.
  private var settings: PlayStation3SplinesSettings {
    theme.particleSettings.backgrounds.playStation3Splines
  }
}
