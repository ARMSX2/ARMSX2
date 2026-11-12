// PlayStationRibbonsBackground.swift — ribbons background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStationRibbonsBackground

struct PlayStationRibbonsBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.playStationRibbonsSpeed
      let settings = theme.particleSettings.backgrounds.playStationRibbons
      let dark = theme.sharedColor(index: 0, time: time)
      let primary = theme.sharedColor(index: 1, time: time)
      let highlight = theme.ribbonColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(dark, darkness: 0.96),
            theme.paletteBackgroundColor(dark, darkness: 0.54),
            theme.paletteBackgroundColor(primary, darkness: 0.76),
            theme.paletteBackgroundColor(primary, darkness: 1),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        Circle()
          .fill(primary.opacity(0.22 * settings.ambientGlowIntensity))
          .frame(
            width: 430 * settings.ambientGlowScale,
            height: 430 * settings.ambientGlowScale
          )
          .blur(radius: 110 * settings.ambientGlowScale)
          .offset(
            x: CGFloat(cos(time * 0.08)) * 170 + 90,
            y: CGFloat(sin(time * 0.06)) * 120 - 120
          )

        DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
          if settings.showsPanels {
            drawLuminousPanels(
              context: &context,
              size: size,
              time: time
            )
          }
          if settings.showsParticles {
            drawFloatingLight(
              context: &context,
              size: size,
              time: time,
              color: highlight
            )
          }
        }

        LinearGradient(
          colors: [
            .white.opacity(0.035 * settings.vignetteIntensity),
            .clear,
            theme.paletteDarkOverlay(
              opacity: 0.28 * settings.vignetteIntensity
            ),
          ],
          startPoint: .top,
          endPoint: .bottom
        )
      }
      .ignoresSafeArea()
    }
  }
}

extension PlayStationRibbonsBackground {
  // Draws broad luminous panels whose fill color comes from the ribbon palette.
  private func drawLuminousPanels(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval
  ) {
    let panelCount = frameRates.scaledDynamicGeometryCount(settings.panelCount)

    for index in 0..<panelCount {
      let panel = panelPath(size: size, time: time, index: index)
      let edge = panelEdgePath(size: size, time: time, index: index)
      let color = theme.ribbonColor(index: index + 1, time: time)
      let fade = 1 - Double(index) / Double(panelCount + 2)
      if settings.showsPanelGlow {
        context.drawLayer { glow in
          glow.addFilter(
            .blur(
              radius: (22 + CGFloat(index) * 3) * CGFloat(settings.panelGlowBlur)
            )
          )
          glow.fill(
            panel,
            with: .color(
              color.opacity(0.13 * fade * settings.panelGlowOpacity)
            )
          )
        }
      }

      context.fill(
        panel,
        with: .linearGradient(
          Gradient(colors: [
            .white.opacity(0.025 * settings.panelFillOpacity),
            color.opacity(0.2 * fade * settings.panelFillOpacity),
            .white.opacity(0.085 * fade * settings.panelFillOpacity),
          ]),
          startPoint: CGPoint(x: 0, y: size.height * 0.3),
          endPoint: CGPoint(x: size.width, y: size.height * 0.7)
        )
      )
      if settings.showsPanelEdges {
        context.stroke(
          edge,
          with: .color(
            .white.opacity(0.28 * fade * settings.panelEdgeOpacity)
          ),
          style: StrokeStyle(
            lineWidth: 0.8 * CGFloat(settings.panelEdgeWidth),
            lineCap: .round
          )
        )
      }
    }
  }

  // Draws slow floating points over the PS5 panels.
  private func drawFloatingLight(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    color: Color
  ) {
    let particleCount = frameRates.scaledDynamicEffectCount(settings.particleCount)
    let particleTime = time * settings.particleSpeed

    for index in 0..<particleCount {
      let seed = Double(index) * 1.618
      let progress =
        (Double(index) * 0.047
        + particleTime * (0.006 + Double(index % 5) * 0.0015))
        .truncatingRemainder(dividingBy: 1)
      let x =
        progress * size.width
        + CGFloat(sin(particleTime * 0.16 + seed)) * 24
        * CGFloat(settings.particleDrift)
      let normalizedY =
        cos(seed * 1.37) * 0.5
        * settings.particleVerticalSpread + 0.5
      let y =
        CGFloat(normalizedY) * size.height
        + CGFloat(sin(particleTime * 0.1 + seed)) * 38
        * CGFloat(settings.particleDrift)
      let diameter =
        CGFloat(0.8 + Double(index % 4) * 0.52)
        * CGFloat(settings.particleSize)
      let rect = CGRect(
        x: x - diameter / 2,
        y: y - diameter / 2,
        width: diameter,
        height: diameter
      )
      context.fill(
        Path(ellipseIn: rect),
        with: .color(
          index.isMultiple(of: 7)
            ? .white.opacity(0.5 * settings.particleOpacity)
            : color.opacity(0.25 * settings.particleOpacity)
        )
      )
    }
  }

  // Builds a thick curved panel from paired top and bottom edges.
  private func panelPath(
    size: CGSize,
    time: TimeInterval,
    index: Int
  ) -> Path {
    let phase =
      time * (0.12 + Double(index) * 0.006)
      + Double(index) * 0.72 * settings.phaseSpread
    let baseY =
      size.height
      * (CGFloat(settings.panelVerticalPosition)
        + CGFloat(index) * 0.135 * CGFloat(settings.panelSpacing))
    let amplitude =
      size.height * (0.055 + CGFloat(index % 2) * 0.018)
      * CGFloat(settings.panelAmplitude)
    let thickness =
      size.height * (0.04 + CGFloat(index % 3) * 0.012)
      * CGFloat(settings.panelThickness)
    let curvature = CGFloat(settings.panelCurvature)
    let startTop = CGPoint(
      x: -size.width * 0.18,
      y: baseY + CGFloat(sin(phase)) * amplitude
    )
    let endTop = CGPoint(
      x: size.width * 1.18,
      y: baseY + CGFloat(cos(phase * 0.79)) * amplitude
    )
    let startBottom = CGPoint(
      x: startTop.x,
      y: startTop.y + thickness
    )
    let endBottom = CGPoint(
      x: endTop.x,
      y: endTop.y + thickness * 0.7
    )

    var path = Path()
    path.move(to: startTop)
    path.addCurve(
      to: endTop,
      control1: CGPoint(
        x: size.width * 0.23,
        y: baseY - CGFloat(cos(phase + 0.4)) * amplitude * 1.8 * curvature
      ),
      control2: CGPoint(
        x: size.width * 0.76,
        y: baseY + CGFloat(sin(phase * 0.83 + 1.1)) * amplitude * 1.7 * curvature
      )
    )
    path.addLine(to: endBottom)
    path.addCurve(
      to: startBottom,
      control1: CGPoint(
        x: size.width * 0.76,
        y: baseY + thickness
          + CGFloat(sin(phase * 0.83 + 1.1)) * amplitude * 1.4
          * curvature
      ),
      control2: CGPoint(
        x: size.width * 0.23,
        y: baseY + thickness
          - CGFloat(cos(phase + 0.4)) * amplitude * 1.5
          * curvature
      )
    )
    path.closeSubpath()
    return path
  }

  // Builds the bright top edge that outlines a curved panel.
  private func panelEdgePath(
    size: CGSize,
    time: TimeInterval,
    index: Int
  ) -> Path {
    let phase =
      time * (0.12 + Double(index) * 0.006)
      + Double(index) * 0.72 * settings.phaseSpread
    let baseY =
      size.height
      * (CGFloat(settings.panelVerticalPosition)
        + CGFloat(index) * 0.135 * CGFloat(settings.panelSpacing))
    let amplitude =
      size.height * (0.055 + CGFloat(index % 2) * 0.018)
      * CGFloat(settings.panelAmplitude)
    let curvature = CGFloat(settings.panelCurvature)
    let start = CGPoint(
      x: -size.width * 0.18,
      y: baseY + CGFloat(sin(phase)) * amplitude
    )
    let end = CGPoint(
      x: size.width * 1.18,
      y: baseY + CGFloat(cos(phase * 0.79)) * amplitude
    )

    var path = Path()
    path.move(to: start)
    path.addCurve(
      to: end,
      control1: CGPoint(
        x: size.width * 0.23,
        y: baseY - CGFloat(cos(phase + 0.4)) * amplitude * 1.8 * curvature
      ),
      control2: CGPoint(
        x: size.width * 0.76,
        y: baseY + CGFloat(sin(phase * 0.83 + 1.1)) * amplitude * 1.7 * curvature
      )
    )
    return path
  }

  private var settings: PlayStationRibbonsSettings {
    theme.particleSettings.backgrounds.playStationRibbons
  }
}
