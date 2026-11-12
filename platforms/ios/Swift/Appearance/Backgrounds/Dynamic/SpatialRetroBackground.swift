// SpatialRetroBackground.swift — spatial retro background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - SpatialRetroBackground

struct SpatialRetroBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.spatialRetroSpeed
      let primary = theme.sharedColor(index: 0, time: time)
      let secondary = theme.sharedColor(index: 1, time: time)
      let highlight = theme.ribbonColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(primary, darkness: 1),
            theme.paletteBackgroundColor(primary, darkness: 0.78),
            theme.paletteBackgroundColor(secondary, darkness: 0.96),
            theme.paletteBackgroundColor(primary, darkness: 1),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        Circle()
          .fill(secondary.opacity(0.24))
          .frame(width: 720, height: 720)
          .blur(radius: 150)
          .offset(
            x: CGFloat(sin(time * 0.09)) * 120,
            y: CGFloat(cos(time * 0.07)) * 80
          )

        DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
          drawPerspectiveGrid(
            context: &context,
            size: size,
            time: time,
            color: primary
          )
          drawPixelDust(
            context: &context,
            size: size,
            time: time,
            color: highlight
          )
          drawPolyhedra(
            context: &context,
            size: size,
            time: time
          )
        }

        LinearGradient(
          colors: [
            theme.paletteDarkOverlay(opacity: 0.08),
            .clear,
            theme.paletteDarkOverlay(opacity: 0.34),
          ],
          startPoint: .top,
          endPoint: .bottom
        )
      }
      .ignoresSafeArea()
    }
  }

  // Draws a moving perspective grid from the horizon to the bottom edge.
  private func drawPerspectiveGrid(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    color: Color
  ) {
    let horizon = size.height * 0.57
    let vanishingPoint = CGPoint(
      x: size.width * 0.5 + CGFloat(sin(time * 0.08)) * 22,
      y: horizon
    )

    let columnExtent = frameRates.scaledDynamicGeometryCount(10, minimum: 4)
    for column in -columnExtent...columnExtent {
      var line = Path()
      line.move(to: vanishingPoint)
      line.addLine(
        to: CGPoint(
          x: size.width * 0.5 + CGFloat(column) * size.width * 0.105,
          y: size.height
        )
      )
      context.stroke(
        line,
        with: .color(color.opacity(0.12)),
        lineWidth: 0.65
      )
    }

    let rowCount = frameRates.scaledDynamicGeometryCount(17, minimum: 7)
    for row in 0..<rowCount {
      let phase = (Double(row) / Double(rowCount) + time * 0.055)
        .truncatingRemainder(dividingBy: 1)
      let progress = CGFloat(phase)
      let depth = progress * progress
      let y = horizon + (size.height - horizon) * depth
      var line = Path()
      line.move(to: CGPoint(x: 0, y: y))
      line.addLine(to: CGPoint(x: size.width, y: y))
      context.stroke(
        line,
        with: .color(color.opacity(0.08 + Double(progress) * 0.22)),
        lineWidth: 0.6 + progress
      )
    }
  }

  // Draws small pixels that drift upward over the PS1-style grid.
  private func drawPixelDust(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    color: Color
  ) {
    let dustCount = frameRates.scaledDynamicEffectCount(76)
    for index in 0..<dustCount {
      let seed = Double(index) * 1.731
      let x = (sin(seed * 2.17) * 0.5 + 0.5) * size.width
      let progress = (Double(index) * 0.071 + time * (0.008 + Double(index % 4) * 0.002))
        .truncatingRemainder(dividingBy: 1)
      let y = (1 - progress) * size.height
      let side = CGFloat(index.isMultiple(of: 9) ? 2.2 : 1.1)
      context.fill(
        Path(
          CGRect(
            x: x - side / 2,
            y: y - side / 2,
            width: side,
            height: side
          )
        ),
        with: .color(color.opacity(index.isMultiple(of: 9) ? 0.46 : 0.2))
      )
    }
  }

  // Draws rotating faceted shapes and colors their facets from the ribbon palette.
  private func drawPolyhedra(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval
  ) {
    let shapeCount = frameRates.scaledDynamicGeometryCount(7, minimum: 3)
    for shapeIndex in 0..<shapeCount {
      let seed = Double(shapeIndex) * 1.43
      let center = CGPoint(
        x: size.width
          * CGFloat(0.12 + Double(shapeIndex % 4) * 0.25)
          + CGFloat(sin(time * 0.07 + seed)) * 28,
        y: size.height
          * CGFloat(0.18 + Double(shapeIndex / 4) * 0.36)
          + CGFloat(cos(time * 0.09 + seed)) * 34
      )
      let radius =
        min(size.width, size.height)
        * CGFloat(0.045 + Double(shapeIndex % 3) * 0.018)
      let vertexCount = 4 + shapeIndex % 3
      let rotation = time * (0.12 + Double(shapeIndex) * 0.008) + seed
      let innerPoint = CGPoint(
        x: center.x + CGFloat(cos(rotation * 0.7)) * radius * 0.17,
        y: center.y + CGFloat(sin(rotation * 0.9)) * radius * 0.12
      )

      var vertices: [CGPoint] = []
      for vertexIndex in 0..<vertexCount {
        let angle =
          rotation
          + Double(vertexIndex) * (.pi * 2 / Double(vertexCount))
        vertices.append(
          CGPoint(
            x: center.x + CGFloat(cos(angle)) * radius,
            y: center.y + CGFloat(sin(angle)) * radius * 0.72
          )
        )
      }

      for vertexIndex in vertices.indices {
        let nextIndex = (vertexIndex + 1) % vertices.count
        var facet = Path()
        facet.move(to: innerPoint)
        facet.addLine(to: vertices[vertexIndex])
        facet.addLine(to: vertices[nextIndex])
        facet.closeSubpath()

        let facetColor = theme.ribbonColor(
          index: shapeIndex + vertexIndex,
          time: time
        )
        context.fill(
          facet,
          with: .color(
            facetColor.opacity(
              0.12 + Double((vertexIndex + shapeIndex) % 3) * 0.07
            )
          )
        )
        context.stroke(
          facet,
          with: .color(facetColor.opacity(0.38)),
          lineWidth: 0.75
        )
      }
    }
  }

}
