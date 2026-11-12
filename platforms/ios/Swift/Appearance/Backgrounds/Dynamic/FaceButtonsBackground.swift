// FaceButtonsBackground.swift — face buttons background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - FaceButtonsBackground

struct FaceButtonsBackground: View {
  let theme: DynamicBackgroundTheme
  var showsBackdrop = true
  var showsLightBands = true
  @Environment(\.uiFrameRateConfiguration) private var frameRates
  @State private var displayedTheme: DynamicBackgroundTheme
  @State private var previousTheme: DynamicBackgroundTheme?
  @State private var paletteTransitionStartTime: TimeInterval?

  private let paletteTransitionDuration: TimeInterval = 0.46

  init(
    theme: DynamicBackgroundTheme,
    showsBackdrop: Bool = true,
    showsLightBands: Bool = true
  ) {
    self.theme = theme
    self.showsBackdrop = showsBackdrop
    self.showsLightBands = showsLightBands
    _displayedTheme = State(initialValue: theme)
  }

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicFaceButtons) { timeline in
      let time = timeline.date.timeIntervalSinceReferenceDate
      let motionTime =
        time
        * settings.faceButtonSpeed
        * settings.backgrounds.faceButtonsSpeed
      let paletteProgress = paletteTransitionProgress(at: time)
      let lightColor = sharedColor(index: 2, time: time, progress: paletteProgress)
      let sharedGradient = displayedTheme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        if showsBackdrop {
          PaletteGradientField(
            colors: [
              paletteBackgroundColor(
                index: 0,
                time: time,
                darkness: 0.96,
                progress: paletteProgress
              ),
              paletteBackgroundColor(
                index: 0,
                time: time,
                darkness: 0.54,
                progress: paletteProgress
              ),
              paletteBackgroundColor(
                index: 1,
                time: time,
                darkness: 0.66,
                progress: paletteProgress
              ),
              paletteBackgroundColor(
                index: 1,
                time: time,
                darkness: 0.94,
                progress: paletteProgress
              ),
            ],
            startPoint: sharedGradient.start,
            endPoint: sharedGradient.end,
            curvature: displayedTheme.sharedPaletteGradientCurvature
          )

          Circle()
            .fill(lightColor.opacity(0.18))
            .frame(width: 780, height: 780)
            .blur(radius: 160)
            .offset(
              x: CGFloat(sin(motionTime * 0.24)) * 90,
              y: CGFloat(cos(motionTime * 0.18)) * 70
            )
        }

        // Keep the full-screen path pass off the main display transaction so
        // native Game Library scrolling is not serialized behind the animated
        // wallpaper.
        DynamicWallpaperCanvas(
          rendersAsynchronously: true,
          // The complete face-button layer is rasterized once by its dedicated
          // render surface. Avoid multiplying it by the wallpaper scale again.
          usesWallpaperRenderScale: false
        ) { context, size in
          if showsLightBands && settings.backgrounds.faceButtonsShowsLightBands {
            drawWeavingLightBands(
              context: &context,
              size: size,
              motionTime: motionTime,
              colorTime: time,
              paletteProgress: paletteProgress
            )
          }
          drawFaceButtonField(
            context: &context,
            size: size,
            motionTime: motionTime,
            colorTime: time,
            paletteProgress: paletteProgress
          )
        }

        if showsBackdrop {
          LinearGradient(
            colors: [
              paletteDarkOverlay(opacity: 0.18, progress: paletteProgress),
              .clear,
              paletteDarkOverlay(opacity: 0.38, progress: paletteProgress),
            ],
            startPoint: .top,
            endPoint: .bottom
          )
        }
      }
      .ignoresSafeArea()
    }
    .onChange(of: theme) { _, newTheme in
      previousTheme = displayedTheme
      displayedTheme = newTheme
      paletteTransitionStartTime = Date.timeIntervalSinceReferenceDate
    }
  }

  private var settings: DynamicParticleSettings {
    displayedTheme.particleSettings
  }

  // Draws soft sine-wave paths that the face symbols appear to weave through.
  private func drawWeavingLightBands(
    context: inout GraphicsContext,
    size: CGSize,
    motionTime: TimeInterval,
    colorTime: TimeInterval,
    paletteProgress: Double
  ) {
    let bandCount = frameRates.scaledDynamicGeometryCount(5, minimum: 2)
    let steps = frameRates.scaledDynamicGeometryCount(52, minimum: 18)
    for bandIndex in 0..<bandCount {
      var path = Path()
      let baseY = size.height * CGFloat(0.2 + Double(bandIndex) * 0.14)
      let amplitude = size.height * CGFloat(0.035 + Double(bandIndex % 2) * 0.018)
      let phase =
        motionTime * (0.18 + Double(bandIndex) * 0.025)
        + Double(bandIndex) * 1.24

      for step in 0...steps {
        let progress = CGFloat(step) / CGFloat(steps)
        let x = size.width * progress
        let y =
          baseY
          + sin(Double(progress) * .pi * 3.4 + phase) * amplitude
          + cos(Double(progress) * .pi * 1.6 - phase * 0.7) * amplitude * 0.45
        let point = CGPoint(x: x, y: y)

        if step == 0 {
          path.move(to: point)
        } else {
          path.addLine(to: point)
        }
      }

      let color = ribbonColor(
        index: bandIndex,
        time: colorTime,
        progress: paletteProgress
      )
      context.drawLayer { glow in
        glow.addFilter(.blur(radius: 16))
        glow.stroke(
          path,
          with: .color(color.opacity(0.12)),
          style: StrokeStyle(lineWidth: 14, lineCap: .round, lineJoin: .round)
        )
      }
      context.stroke(
        path,
        with: .color(color.opacity(0.3)),
        style: StrokeStyle(lineWidth: 0.9, lineCap: .round, lineJoin: .round)
      )
    }
  }

  // Draws face-button symbols with falling, weaving, looping, and drifting motion.
  private func drawFaceButtonField(
    context: inout GraphicsContext,
    size: CGSize,
    motionTime: TimeInterval,
    colorTime: TimeInterval,
    paletteProgress: Double
  ) {
    context.blendMode = .plusLighter

    let symbolCount = min(
      frameRates.scaledDynamicFaceButtonCount(180),
      frameRates.scaledDynamicFaceButtonCount(72 * settings.faceButtonAmount)
    )
    let glyphs = (0..<symbolCount).map { index in
      let color =
        index.isMultiple(of: 6)
        ? highlightColor(index: index, time: colorTime, progress: paletteProgress)
        : ribbonColor(index: index, time: colorTime, progress: paletteProgress)
      let center = symbolCenter(index: index, size: size, time: motionTime)
      let side = symbolSide(index: index, size: size, time: motionTime)
      let rotation = symbolRotation(index: index, time: motionTime)
      let opacity = (index.isMultiple(of: 5) ? 0.78 : 0.46) * settings.faceButtonOpacity

      return makeFaceButtonGlyph(
        kind: symbolKind(for: index),
        center: center,
        side: side,
        rotation: rotation,
        color: color,
        opacity: opacity
      )
    }

    // One shared offscreen glow replaces one filtered layer per symbol. The
    // visible artwork stays equivalent while leaving GPU/compositor headroom
    // for ReplayKit and system screen recording.
    context.drawLayer { glow in
      glow.blendMode = .plusLighter
      glow.addFilter(.blur(radius: 7))
      for glyph in glyphs {
        glow.stroke(
          glyph.path,
          with: .color(glyph.color.opacity(glyph.opacity * 0.72)),
          style: StrokeStyle(
            lineWidth: glyph.lineWidth * 1.4,
            lineCap: .round,
            lineJoin: .round
          )
        )
      }
    }

    for glyph in glyphs {
      if glyph.kind != .cross {
        context.fill(
          glyph.path,
          with: .color(glyph.color.opacity(glyph.opacity * 0.08))
        )
      }
      context.stroke(
        glyph.path,
        with: .color(glyph.color.opacity(glyph.opacity)),
        style: StrokeStyle(
          lineWidth: glyph.lineWidth,
          lineCap: .round,
          lineJoin: .round
        )
      )
      context.stroke(
        glyph.path,
        with: .color(.white.opacity(glyph.opacity * 0.24)),
        style: StrokeStyle(
          lineWidth: max(0.6, glyph.lineWidth * 0.42),
          lineCap: .round,
          lineJoin: .round
        )
      )
    }
  }

  private struct FaceButtonGlyph {
    let kind: FaceButtonSymbol
    let path: Path
    let color: Color
    let opacity: Double
    let lineWidth: CGFloat
  }

  private func paletteTransitionProgress(at time: TimeInterval) -> Double {
    guard let startTime = paletteTransitionStartTime else { return 1 }
    let linear = min(
      1,
      max(0, (time - startTime) / paletteTransitionDuration)
    )
    return linear * linear * (3 - 2 * linear)
  }

  private func sharedColor(
    index: Int,
    time: TimeInterval,
    progress: Double
  ) -> Color {
    let target = displayedTheme.sharedColor(index: index, time: time)
    guard let previousTheme, progress < 1 else { return target }
    return blendPaletteEffectColor(
      previousTheme.sharedColor(index: index, time: time),
      target,
      amount: progress
    )
  }

  private func ribbonColor(
    index: Int,
    time: TimeInterval,
    progress: Double
  ) -> Color {
    let target = displayedTheme.ribbonColor(index: index, time: time)
    guard let previousTheme, progress < 1 else { return target }
    return blendPaletteEffectColor(
      previousTheme.ribbonColor(index: index, time: time),
      target,
      amount: progress
    )
  }

  private func highlightColor(
    index: Int,
    time: TimeInterval,
    progress: Double
  ) -> Color {
    let target = displayedTheme.highlightColor(index: index, time: time)
    guard let previousTheme, progress < 1 else { return target }
    return blendPaletteEffectColor(
      previousTheme.highlightColor(index: index, time: time),
      target,
      amount: progress
    )
  }

  private func paletteBackgroundColor(
    index: Int,
    time: TimeInterval,
    darkness: Double,
    progress: Double
  ) -> Color {
    let target = displayedTheme.paletteBackgroundColor(
      displayedTheme.sharedColor(index: index, time: time),
      darkness: darkness
    )
    guard let previousTheme, progress < 1 else { return target }
    let source = previousTheme.paletteBackgroundColor(
      previousTheme.sharedColor(index: index, time: time),
      darkness: darkness
    )
    return blendPaletteEffectColor(source, target, amount: progress)
  }

  private func paletteDarkOverlay(
    opacity: Double,
    progress: Double
  ) -> Color {
    let target = displayedTheme.paletteDarkOverlay(opacity: opacity)
    guard let previousTheme, progress < 1 else { return target }
    return blendPaletteEffectColor(
      previousTheme.paletteDarkOverlay(opacity: opacity),
      target,
      amount: progress
    )
  }

  // Resolves geometry once so the glow and crisp passes share exactly the
  // same path instead of rebuilding it in separate compositing layers.
  private func makeFaceButtonGlyph(
    kind: FaceButtonSymbol,
    center: CGPoint,
    side: CGFloat,
    rotation: Double,
    color: Color,
    opacity: Double
  ) -> FaceButtonGlyph {
    let lineWidth = max(1.2, side * 0.07)
    let path: Path

    switch kind {
    case .square:
      path = polygonPath(
        sides: 4, center: center, radius: side * 0.56, rotation: rotation + .pi / 4)
    case .cross:
      path = crossPath(center: center, radius: side * 0.52, rotation: rotation)
    case .circle:
      path = Path(
        ellipseIn: CGRect(
          x: center.x - side * 0.45,
          y: center.y - side * 0.45,
          width: side * 0.9,
          height: side * 0.9
        )
      )
    case .triangle:
      path = polygonPath(sides: 3, center: center, radius: side * 0.6, rotation: rotation - .pi / 2)
    }

    return FaceButtonGlyph(
      kind: kind,
      path: path,
      color: color,
      opacity: opacity,
      lineWidth: lineWidth
    )
  }

  private func symbolKind(for index: Int) -> FaceButtonSymbol {
    switch index % 4 {
    case 0:
      return .square
    case 1:
      return .cross
    case 2:
      return .circle
    default:
      return .triangle
    }
  }

  // Chooses one of several deterministic motion styles for each symbol.
  private func symbolCenter(index: Int, size: CGSize, time: TimeInterval) -> CGPoint {
    let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 12.9898)
    let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 78.233)
    let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 37.719)
    let spread = settings.faceButtonDispersion
    let width = Double(size.width)
    let height = Double(size.height)

    switch index % 5 {
    case 0:
      let progress = DynamicBackgroundMath.unit(seedC + time * (0.025 + seedA * 0.03))
      let x =
        width * (0.08 + seedA * 0.84)
        + sin(time * 1.05 + seedB * 12) * width * 0.075 * spread
      let y = -height * 0.12 + progress * height * 1.24
      return CGPoint(x: CGFloat(x), y: CGFloat(y))
    case 1:
      let progress = DynamicBackgroundMath.unit(seedA + time * (0.012 + seedC * 0.016))
      let x =
        -width * 0.08
        + progress * width * 1.16
        + cos(time * 0.34 + seedB * 10) * width * 0.055 * spread
      let y =
        height * (0.18 + seedB * 0.64)
        + sin(progress * .pi * 3.2 + seedC * 8) * height * 0.08 * spread
      return CGPoint(x: CGFloat(x), y: CGFloat(y))
    case 2:
      let progress = DynamicBackgroundMath.unit(seedB + time * (0.018 + seedA * 0.014))
      let x = width * progress
      let y =
        height * (0.18 + seedC * 0.62)
        + sin(time * 0.9 + progress * .pi * 4 + seedA * 8) * height * 0.08 * spread
      return CGPoint(x: CGFloat(x), y: CGFloat(y))
    case 3:
      let radiusX = width * (0.14 + seedA * 0.22) * spread
      let radiusY = height * (0.11 + seedB * 0.18) * spread
      let x = width * 0.5 + sin(time * (0.22 + seedC * 0.16) + seedA * .pi * 2) * radiusX
      let y = height * 0.48 + cos(time * (0.28 + seedA * 0.16) + seedB * .pi * 2) * radiusY
      return CGPoint(x: CGFloat(x), y: CGFloat(y))
    default:
      let progress = DynamicBackgroundMath.unit(seedA - time * (0.02 + seedB * 0.018))
      let x =
        width * (0.12 + seedC * 0.76)
        + cos(time * 0.72 + seedA * 16) * width * 0.1 * spread
      let y = -height * 0.1 + progress * height * 1.22
      return CGPoint(x: CGFloat(x), y: CGFloat(y))
    }
  }

  private func symbolSide(index: Int, size: CGSize, time: TimeInterval) -> CGFloat {
    let seed = DynamicBackgroundMath.seededUnit(index: index, salt: 19.119)
    let base =
      min(size.width, size.height)
      * CGFloat(0.032 + seed * 0.042)
      * 0.5
      * CGFloat(settings.faceButtonSize)
    let pulse =
      1
      + CGFloat(sin(time * (0.65 + seed) + seed * 9))
      * 0.08
      * CGFloat(settings.faceButtonPulse)
    return min(58, max(8, base * pulse))
  }

  private func symbolRotation(index: Int, time: TimeInterval) -> Double {
    let seed = DynamicBackgroundMath.seededUnit(index: index, salt: 91.733)
    let direction = index.isMultiple(of: 2) ? 1.0 : -1.0
    return seed * .pi * 2 + time * (0.28 + seed * 0.46) * direction * settings.faceButtonRotation
  }

  private func polygonPath(
    sides: Int,
    center: CGPoint,
    radius: CGFloat,
    rotation: Double
  ) -> Path {
    var path = Path()

    for vertexIndex in 0..<sides {
      let angle = rotation + Double(vertexIndex) * (.pi * 2 / Double(sides))
      let point = CGPoint(
        x: center.x + CGFloat(cos(angle)) * radius,
        y: center.y + CGFloat(sin(angle)) * radius
      )

      if vertexIndex == 0 {
        path.move(to: point)
      } else {
        path.addLine(to: point)
      }
    }
    path.closeSubpath()
    return path
  }

  private func crossPath(
    center: CGPoint,
    radius: CGFloat,
    rotation: Double
  ) -> Path {
    var path = Path()
    let points = [
      (CGPoint(x: -radius, y: -radius), CGPoint(x: radius, y: radius)),
      (CGPoint(x: radius, y: -radius), CGPoint(x: -radius, y: radius)),
    ]

    for segment in points {
      path.move(to: rotatedPoint(segment.0, around: center, rotation: rotation))
      path.addLine(to: rotatedPoint(segment.1, around: center, rotation: rotation))
    }

    return path
  }

  private func rotatedPoint(
    _ offset: CGPoint,
    around center: CGPoint,
    rotation: Double
  ) -> CGPoint {
    CGPoint(
      x: center.x + offset.x * CGFloat(cos(rotation)) - offset.y * CGFloat(sin(rotation)),
      y: center.y + offset.x * CGFloat(sin(rotation)) + offset.y * CGFloat(cos(rotation))
    )
  }
}

private enum FaceButtonSymbol: Equatable {
  case square
  case cross
  case circle
  case triangle
}
