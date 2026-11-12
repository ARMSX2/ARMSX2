// PlayStationPortableBlurBackground.swift — PSP blur background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStationPortableBlurBackground

struct PlayStationPortableBlurBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.playStationPortableBlurSpeed
      let settings = theme.particleSettings.backgrounds.playStationPortableBlur
      let base = theme.sharedColor(index: 1, time: time)
      let accent = theme.ribbonColor(index: 1, time: time)
      let shadow = theme.sharedColor(index: 0, time: time)
      let glow = theme.sharedColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(
              shadow,
              darkness: 1 - min(1, settings.backgroundIntensity)
            ),
            theme.paletteBackgroundColor(
              base,
              darkness: 1 - min(1, settings.backgroundIntensity)
            ),
            theme.paletteBackgroundColor(
              accent,
              darkness: 1 - min(1, 0.74 * settings.backgroundIntensity)
            ),
            theme.paletteBackgroundColor(
              glow,
              darkness: 1 - min(1, 0.82 * settings.backgroundIntensity)
            ),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        RadialGradient(
          colors: [
            theme.highlightColor(index: 4, time: time)
              .opacity(0.34 * settings.ambientGlowIntensity),
            .clear,
          ],
          center: .center,
          startRadius: 10,
          endRadius: 430 * settings.ambientGlowScale
        )

        if settings.showsRibbons {
          DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
            let ribbonCount = frameRates.scaledDynamicGeometryCount(
              settings.ribbonCount
            )
            for index in 0..<ribbonCount {
              drawRibbon(
                context: &context,
                size: size,
                time: time,
                index: index
              )
            }
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

  // Draws one XMB ribbon with a blurred glow and a thin bright core.
  private func drawRibbon(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    index: Int
  ) {
    let path = ribbonPath(size: size, time: time, index: index)
    let broadWidth = CGFloat(14 + (index % 4) * 7) * CGFloat(settings.broadWidth)
    let brightness = 0.14 + Double(index % 3) * 0.045
    let ribbonColor =
      index.isMultiple(of: 3)
      ? theme.ribbonColor(index: index, time: time)
      : theme.highlightColor(index: index, time: time)
    let coreColor = theme.highlightColor(index: index + 4, time: time)

    if settings.showsGlow {
      context.drawLayer { glow in
        glow.blendMode = .plusLighter
        glow.addFilter(
          .blur(
            radius: (9 + CGFloat(index % 3) * 4) * CGFloat(settings.glowBlur)
          )
        )
        glow.stroke(
          path,
          with: .color(ribbonColor.opacity(brightness * settings.glowOpacity)),
          style: StrokeStyle(
            lineWidth: broadWidth,
            lineCap: .round,
            lineJoin: .round
          )
        )
      }
    }

    if settings.showsCores {
      context.drawLayer { core in
        core.blendMode = .plusLighter
        core.addFilter(
          .blur(
            radius: (2.4 + CGFloat(index % 2)) * CGFloat(settings.coreBlur)
          )
        )
        core.stroke(
          path,
          with: .color(
            coreColor.opacity(
              (0.16 + Double(index % 3) * 0.035) * settings.coreOpacity
            )
          ),
          style: StrokeStyle(
            lineWidth: (2.2 + CGFloat(index % 3) * 0.7)
              * CGFloat(settings.coreWidth),
            lineCap: .round,
            lineJoin: .round
          )
        )
      }
    }
  }

  // Builds a smooth multi-point XMB wave path across the screen.
  private func ribbonPath(
    size: CGSize,
    time: TimeInterval,
    index: Int
  ) -> Path {
    let basePositions: [CGFloat] = [
      0.58, 0.62, 0.67, 0.7, 0.73, 0.76, 0.64,
    ]
    let amplitudes: [CGFloat] = [
      0.12, 0.095, 0.07, 0.045, 0.035, 0.055, 0.085,
    ]
    let profileIndex = index % basePositions.count
    let direction = settings.alternatesDirection && !index.isMultiple(of: 2) ? -1.0 : 1.0
    let phase =
      time * (0.12 + Double(index) * 0.011) * direction
      + Double(index) * 0.63 * settings.phaseSpread

    let sampleCount = frameRates.scaledDynamicGeometryCount(28, minimum: 10)
    let points = (0...sampleCount).map { step in
      let progress = CGFloat(step) / CGFloat(sampleCount)
      let progressValue = Double(progress)
      let longWave = sin(progressValue * 2.15 + phase)
      let secondaryWave = sin(
        progressValue * 4.4 - phase * 0.52 + Double(index) * 0.37
      )
      let y =
        size.height * (basePositions[profileIndex] + CGFloat(settings.verticalOffset))
        + size.height * amplitudes[profileIndex] * CGFloat(settings.waveAmplitude)
        * CGFloat(longWave)
        + size.height * 0.018 * CGFloat(settings.detailAmplitude)
        * CGFloat(secondaryWave)

      return CGPoint(
        x: -size.width * 0.08 + progress * size.width * 1.16,
        y: y
      )
    }

    return DynamicBackgroundGeometry.smoothCurve(through: points)
  }

  private var settings: PlayStationPortableBlurSettings {
    theme.particleSettings.backgrounds.playStationPortableBlur
  }
}
