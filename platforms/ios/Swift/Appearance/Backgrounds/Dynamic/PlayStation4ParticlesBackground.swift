// PlayStation4ParticlesBackground.swift — PS4 particles background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation4ParticlesBackground

struct PlayStation4ParticlesBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.playStation4ParticlesSpeed
      let settings = theme.particleSettings.backgrounds.playStation4Particles
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
          if settings.showsWaves {
            drawPlayStationWaves(
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

  // Draws the original XMB3 particles centered on the PS4 wave band.
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
      let secondarySeed = DynamicBackgroundMath.unit(sin(Double(index + 7) * 78.233) * 9_631.417)
      let speed = 0.006 + Double(index % 7) * 0.0011
      let progress = DynamicBackgroundMath.unit(seed + particleTime * speed)
      let x = CGFloat(progress) * size.width
      let waveCenter =
        size.height
        * (CGFloat(settings.particleVerticalPosition)
          + 0.08
          * CGFloat(
            sin(progress * .pi * 2.1 - particleTime * 0.11 + seed * 5)
          ))
      let spread =
        (CGFloat(secondarySeed) - 0.5)
        * size.height
        * (0.12 + CGFloat(seed) * 0.16)
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

  // Draws PS4 wave paths on top of the XMB3 background.
  private func drawPlayStationWaves(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    primary: Color,
    highlight: Color
  ) {
    let waveCount = frameRates.scaledDynamicGeometryCount(settings.waveCount)

    for index in 0..<waveCount {
      let path = playStationWavePath(size: size, time: time, index: index)
      let fade = 1 - Double(index) / Double(waveCount + 2)

      if settings.showsWaveGlow {
        context.drawLayer { glow in
          glow.blendMode = .plusLighter
          glow.addFilter(.blur(radius: CGFloat(settings.glowBlur)))
          glow.stroke(
            path,
            with: .color(
              index.isMultiple(of: 2)
                ? primary.opacity(fade * 0.38 * settings.glowOpacity)
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

  // Builds the same two-curve wave shape used by PS4 Waves.
  private func playStationWavePath(size: CGSize, time: TimeInterval, index: Int) -> Path {
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
  private var settings: PlayStation4ParticlesSettings {
    theme.particleSettings.backgrounds.playStation4Particles
  }
}
