// PlayStation4WavesBackground.swift — PS4 waves background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation4WavesBackground

struct PlayStation4WavesBackground: View {
  let theme: DynamicBackgroundTheme
  var showsWaves = true
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.playStation4WavesSpeed
      let settings = theme.particleSettings.backgrounds.playStation4Waves
      let backgroundDark = theme.playStation4BaseColor(index: 0, time: time)
      let backgroundMiddle = theme.playStation4BaseColor(index: 1, time: time)
      let backgroundBright = theme.playStation4BaseColor(index: 2, time: time)
      let highlight = theme.highlightColor(index: 9, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        PaletteGradientField(
          colors: [
            backgroundDark,
            backgroundMiddle,
            theme.paletteBackgroundColor(backgroundBright, darkness: 0.54),
            theme.paletteBackgroundColor(backgroundDark, darkness: 0.22),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        if settings.showsAmbientGlows {
          Ellipse()
            .fill(
              theme.ribbonColor(index: 0, time: time)
                .opacity(0.3 * settings.ambientGlowIntensity)
            )
            .frame(
              width: 520 * settings.ambientGlowScale,
              height: 240 * settings.ambientGlowScale
            )
            .blur(radius: 90 * settings.ambientGlowScale)
            .offset(
              x: CGFloat(sin(time * 0.09)) * 180,
              y: CGFloat(cos(time * 0.07)) * 130 - 80
            )

          Circle()
            .fill(highlight.opacity(0.13 * settings.ambientGlowIntensity))
            .frame(
              width: 230 * settings.ambientGlowScale,
              height: 230 * settings.ambientGlowScale
            )
            .blur(radius: 78 * settings.ambientGlowScale)
            .offset(
              x: CGFloat(cos(time * 0.11)) * 220 + 120,
              y: CGFloat(sin(time * 0.08)) * 170
            )
        }

        if settings.showsParticles {
          DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
            drawParticles(
              context: &context,
              size: size,
              time: time,
              highlight: highlight
            )
          }
        }

        if showsWaves && settings.showsWaves {
          DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
            drawWaves(
              context: &context,
              size: size,
              time: time,
              highlight: highlight
            )
          }
        }

        LinearGradient(
          colors: [
            theme.paletteDarkOverlay(
              opacity: 0.18 * settings.vignetteIntensity
            ),
            .clear,
            theme.paletteDarkOverlay(
              opacity: 0.3 * settings.vignetteIntensity
            ),
          ],
          startPoint: .top,
          endPoint: .bottom
        )
      }
      .ignoresSafeArea()
    }
  }

  // Draws slow background particles colored by the ribbon palette.
  private func drawParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    highlight: Color
  ) {
    let particleCount = frameRates.scaledDynamicEffectCount(settings.particleCount)
    let particleTime = time * settings.particleSpeed

    for index in 0..<particleCount {
      let seed = Double(index) * 1.618
      let x =
        (sin(particleTime * 0.035 + seed) * 0.5 + 0.5)
        * size.width
      let normalizedY =
        cos(particleTime * 0.026 + seed * 1.37) * 0.5
        * settings.particleSpread + 0.5
      let y = normalizedY * size.height
      let diameter =
        CGFloat(1.2 + Double(index % 4) * 0.55)
        * CGFloat(settings.particleSize)
      let particle = CGRect(
        x: x - diameter / 2,
        y: y - diameter / 2,
        width: diameter,
        height: diameter
      )
      context.fill(
        Path(ellipseIn: particle),
        with: .color(
          index.isMultiple(of: 5)
            ? highlight.opacity(0.5 * settings.particleOpacity)
            : theme.ribbonColor(index: index, time: time)
              .opacity(0.32 * settings.particleOpacity)
        )
      )
    }
  }

  // Draws the PS4 wave ribbons with a blurred glow and a sharp front stroke.
  private func drawWaves(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    highlight: Color
  ) {
    let waveCount = frameRates.scaledDynamicGeometryCount(settings.waveCount)

    for index in 0..<waveCount {
      let path = wavePath(size: size, time: time, index: index)
      let fade = 1 - Double(index) / Double(waveCount + 2)

      if settings.showsWaveGlow {
        context.drawLayer { glow in
          glow.addFilter(.blur(radius: CGFloat(settings.glowBlur)))
          glow.stroke(
            path,
            with: .color(
              index.isMultiple(of: 2)
                ? theme.ribbonColor(index: index, time: time)
                  .opacity(fade * 0.38 * settings.glowOpacity)
                : theme.ribbonColor(index: index + 1, time: time)
                  .opacity(fade * 0.32 * settings.glowOpacity)
            ),
            style: StrokeStyle(
              lineWidth: CGFloat(max(2, 20 - index)) * CGFloat(settings.glowWidth),
              lineCap: .round
            )
          )
        }
      }

      if settings.showsWaveCores {
        context.stroke(
          path,
          with: .color(
            index.isMultiple(of: 3)
              ? highlight.opacity(fade * 0.7 * settings.coreOpacity)
              : theme.ribbonColor(index: index + 2, time: time)
                .opacity(fade * 0.78 * settings.coreOpacity)
          ),
          style: StrokeStyle(
            lineWidth: CGFloat(0.7 + Double(waveCount - 1 - index) * 0.4)
              * CGFloat(settings.coreWidth),
            lineCap: .round
          )
        )
      }
    }
  }

  // Builds one horizontal PS4 wave path from two cubic curves.
  private func wavePath(size: CGSize, time: TimeInterval, index: Int) -> Path {
    let layer = Double(index)
    let waveCount = frameRates.scaledDynamicGeometryCount(settings.waveCount)
    let phase =
      time * (0.18 + layer * 0.012)
      + layer * 0.74 * settings.phaseSpread
    let centerIndex = CGFloat(max(0, waveCount - 2)) / 2
    let centerY =
      size.height
      * (CGFloat(settings.waveVerticalPosition)
        + (CGFloat(index) - centerIndex) * 0.033 * CGFloat(settings.waveSpacing))
    let amplitude =
      size.height * (0.055 + CGFloat(index % 3) * 0.014)
      * CGFloat(settings.waveAmplitude)
    let start = CGPoint(
      x: -size.width * 0.12,
      y: centerY + sin(phase) * amplitude
    )
    let midpoint = CGPoint(
      x: size.width * 0.5,
      y: centerY - cos(phase * 0.83) * amplitude
    )
    let end = CGPoint(
      x: size.width * 1.12,
      y: centerY + sin(phase * 0.71 + 1.4) * amplitude
    )
    let midpointTangentY = cos(phase * 0.68 + 0.35) * amplitude * 0.62

    var path = Path()
    path.move(to: start)
    path.addCurve(
      to: midpoint,
      control1: CGPoint(
        x: size.width * 0.08,
        y: centerY - cos(phase + 0.6) * amplitude * 1.5
          * CGFloat(settings.waveCurvature)
      ),
      control2: CGPoint(
        x: midpoint.x - size.width * 0.17,
        y: midpoint.y - midpointTangentY * CGFloat(settings.waveCurvature)
      )
    )
    path.addCurve(
      to: end,
      control1: CGPoint(
        x: midpoint.x + size.width * 0.17,
        y: midpoint.y + midpointTangentY * CGFloat(settings.waveCurvature)
      ),
      control2: CGPoint(
        x: size.width * 0.91,
        y: centerY + cos(phase + 0.2) * amplitude * 1.4
          * CGFloat(settings.waveCurvature)
      )
    )
    return path
  }

  private var settings: PlayStation4WavesSettings {
    theme.particleSettings.backgrounds.playStation4Waves
  }
}
