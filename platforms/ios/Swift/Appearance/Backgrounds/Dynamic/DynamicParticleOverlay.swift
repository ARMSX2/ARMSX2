// DynamicParticleOverlay.swift — dynamic particle overlay
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - DynamicParticleOverlay

struct DynamicParticleOverlay: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.menuBackgroundSessionStart) private var menuBackgroundSessionStart
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  @ViewBuilder
  var body: some View {
    ZStack {
      if settings.isEnabled {
        if settings.style == .xmbMart {
          PlayStation3XMBMartMetalSurface(
            settings: settings.playStation3XMB,
            theme: theme,
            sessionStartTime: menuBackgroundSessionStart.timeIntervalSinceReferenceDate,
            renderMode: .particlesOnly,
            particleControls: martParticleControls
          )
          .ignoresSafeArea()
        } else if settings.style != .armsx2BouncingLogo {
          AdaptiveAnimationTimeline(domain: .dynamicParticles) { timeline in
            let time = timeline.date.timeIntervalSinceReferenceDate
            DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
              context.blendMode = .plusLighter
              drawParticles(
                context: &context,
                size: size,
                time: time
              )
            }
            .ignoresSafeArea()
          }
          .dynamicWallpaperSwiftUIRenderSurface()
        }
      }

      if settings.isARMSX2LogoEnabled {
        ARMSX2BouncingLogoParticleView(
          settings: settings,
          theme: theme,
          sessionStart: menuBackgroundSessionStart
        )
        .dynamicWallpaperSwiftUIRenderSurface()
        .ignoresSafeArea()
      }
    }
  }

  private var settings: DynamicParticleSettings {
    theme.particleSettings
  }

  private var directedSpeed: Double {
    settings.speed * settings.speedDirection
  }

  private var martParticleControls: PlayStation3XMBMartParticleControls {
    PlayStation3XMBMartParticleControls(
      amount: settings.amount * settings.verticalDensity,
      speed: settings.speed,
      direction: settings.speedDirection,
      dispersion: settings.dispersion,
      verticalSpread: settings.verticalSpread,
      outerDispersion: settings.outerDispersion,
      verticalLevel: settings.verticalLevel,
      size: settings.size,
      brightness: settings.brightness,
      opacity: settings.opacity,
      depthVariation: settings.depthVariation
    )
  }

  // Draws the selected particle family with the user's shared controls.
  private func drawParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval
  ) {
    switch settings.style {
    case .xmb3:
      drawXMBParticles(context: &context, size: size, time: time, baseCount: 150)
    case .xmbMart:
      return
    case .armsx2BouncingLogo:
      return
    case .ps1Dust:
      drawPixelDust(context: &context, size: size, time: time, baseCount: 90)
    case .ps4Glow:
      drawGlowParticles(context: &context, size: size, time: time, baseCount: 56)
    case .ps5Drift:
      drawDriftParticles(context: &context, size: size, time: time, baseCount: 96)
    case .mixed:
      drawXMBParticles(context: &context, size: size, time: time, baseCount: 92)
      drawPixelDust(context: &context, size: size, time: time, baseCount: 42)
      drawGlowParticles(context: &context, size: size, time: time, baseCount: 28)
      drawDriftParticles(context: &context, size: size, time: time, baseCount: 46)
    }
  }

  /// A single, inexpensive DVD-style logo which reflects at each screen edge.
  private struct ARMSX2BouncingLogoParticleView: View {
    @State private var logoStore = ARMSX2LogoStore.shared
    @Environment(\.uiFrameRateConfiguration) private var frameRates
    let settings: DynamicParticleSettings
    let theme: DynamicBackgroundTheme
    let sessionStart: Date

    @ViewBuilder
    var body: some View {
      if let logoImage = logoStore.image {
        GeometryReader { proxy in
          AdaptiveAnimationTimeline(domain: .dynamicParticles) { timeline in
            let size = proxy.size
            let elapsedTime = timeline.date.timeIntervalSince(sessionStart)
            let logoSize = fittedLogoSize(in: size)
            let logoFrame = logoFrame(
              at: elapsedTime,
              canvasSize: size,
              logoSize: logoSize
            )

            ZStack(alignment: .topLeading) {
              orbLayer(
                size: size,
                time: elapsedTime,
                drawsFront: false
              )

              Image(uiImage: logoImage)
                .resizable()
                .interpolation(.high)
                .scaledToFit()
                .frame(width: logoSize.width, height: logoSize.height)
                // Position the image from its leading/top bounds. Using a
                // centre point here made the reflection look as if the logo's
                // centre, rather than its visible sides, hit the screen edge.
                .offset(x: logoFrame.minX, y: logoFrame.minY)
                .opacity(min(max(settings.resolvedARMSX2LogoOpacity, 0), 1))
                .zIndex(2)

              orbLayer(
                size: size,
                time: elapsedTime,
                drawsFront: true
              )
            }
            .frame(width: size.width, height: size.height)
          }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .zIndex(2)
      }
    }

    private func orbLayer(
      size: CGSize,
      time: TimeInterval,
      drawsFront: Bool
    ) -> some View {
      DynamicWallpaperCanvas(rendersAsynchronously: true) { context, _ in
        drawOrbs(
          context: &context,
          size: size,
          time: time,
          drawsFront: drawsFront
        )
      }
    }

    private func fittedLogoSize(in canvasSize: CGSize) -> CGSize {
      let scale = min(max(settings.resolvedARMSX2LogoSize, 0.5), 1.8)
      let idealWidth = min(canvasSize.width * 0.36, 320) * scale
      let maximumWidth = max(1, canvasSize.width - 32)
      let width = min(max(idealWidth, 116), maximumWidth)
      return CGSize(width: width, height: width * (151.0 / 1134.0))
    }

    /// Draws the same moving field on either side of the logo. Crossfading its
    /// depth keeps every orb continuous as it moves behind and in front.
    private func drawOrbs(
      context: inout GraphicsContext,
      size: CGSize,
      time: TimeInterval,
      drawsFront: Bool
    ) {
      let count = min(
        30,
        max(
          2,
          frameRates.scaledDynamicEffectCount(10 * settings.amount)
        )
      )
      let particleOpacity = min(
        max(settings.resolvedARMSX2LogoOrbOpacity, 0), 1
      )
      let brightness = min(max(settings.brightness, 0.15), 1.35)

      for index in 0..<count {
        let state = orbState(index: index, size: size, time: time)
        let frontOpacity = smoothDepthProgress(state.depth)
        let depthOpacity = drawsFront ? frontOpacity : 1 - frontOpacity
        guard depthOpacity > 0.001 else { continue }

        let layerStrength = drawsFront ? 1.0 : 0.58
        let opacity = particleOpacity * brightness * depthOpacity * layerStrength
        let color = theme.ribbonColor(index: index, time: time)
        let rect = CGRect(
          x: state.position.x - state.diameter / 2,
          y: state.position.y - state.diameter / 2,
          width: state.diameter,
          height: state.diameter
        )

        context.drawLayer { glow in
          glow.blendMode = .plusLighter
          glow.addFilter(
            .shadow(
              color: color.opacity(opacity * 0.95),
              radius: state.diameter * 0.9
            )
          )
          glow.fill(
            Path(ellipseIn: rect),
            with: .color(color.opacity(opacity * 0.9))
          )
        }

        let core = rect.insetBy(
          dx: state.diameter * 0.31,
          dy: state.diameter * 0.31
        )
        context.fill(
          Path(ellipseIn: core),
          with: .color(.white.opacity(opacity * 0.82))
        )
      }
    }

    /// Uses seeded, mismatched wave periods so each orb roams independently
    /// across the whole screen without frame-to-frame random jitter.
    private func orbState(
      index: Int,
      size: CGSize,
      time: TimeInterval
    ) -> ARMSX2LogoOrbState {
      let seedA = DynamicBackgroundMath.seededUnit(index: index, salt: 19.17)
      let seedB = DynamicBackgroundMath.seededUnit(index: index, salt: 43.73)
      let seedC = DynamicBackgroundMath.seededUnit(index: index, salt: 71.29)
      let seedD = DynamicBackgroundMath.seededUnit(index: index, salt: 103.91)
      let motionTime = time * max(settings.speed, 0.1)
      let dispersion = min(max(settings.dispersion, 0.25), 1.8)
      let horizontalAmplitude = min(0.47, 0.29 * dispersion)
      let verticalAmplitude = min(0.47, 0.28 * settings.verticalSpread)
      let phaseA = seedA * .pi * 2
      let phaseB = seedB * .pi * 2
      let phaseC = seedC * .pi * 2
      let normalizedX = min(
        0.97,
        max(
          0.03,
          0.5
            + sin(motionTime * (0.10 + seedB * 0.09) + phaseA) * horizontalAmplitude
            + sin(motionTime * (0.031 + seedD * 0.028) + phaseC) * 0.17
        )
      )
      let normalizedY = min(
        0.97,
        max(
          0.03,
          0.5
            + cos(motionTime * (0.085 + seedC * 0.10) + phaseB) * verticalAmplitude
            + sin(motionTime * (0.027 + seedA * 0.032) + phaseA) * 0.18
        )
      )
      let depth = sin(motionTime * (0.19 + seedD * 0.13) + phaseC)
      let depthScale = 0.78 + (depth + 1) * 0.16
      let variation = 1 + (seedC - 0.5) * settings.depthVariation * 0.55
      let diameter = CGFloat(7 + seedA * 11)
        * CGFloat(settings.size * depthScale * variation)

      return ARMSX2LogoOrbState(
        position: CGPoint(
          x: CGFloat(normalizedX) * size.width,
          y: CGFloat(normalizedY) * size.height
        ),
        diameter: max(3, diameter),
        depth: depth
      )
    }

    private func smoothDepthProgress(_ depth: Double) -> Double {
      let progress = min(max((depth + 0.18) / 0.36, 0), 1)
      return progress * progress * (3 - 2 * progress)
    }

    private func logoFrame(
      at elapsedTime: TimeInterval,
      canvasSize: CGSize,
      logoSize: CGSize
    ) -> CGRect {
      // Reflect the leading edge inside the exact travel rectangle. The logo's
      // trailing/bottom bounds consequently touch the opposite inset at the
      // same frame, matching a DVD screensaver collision.
      let inset: CGFloat = 3
      let horizontalTravel = max(0, canvasSize.width - logoSize.width - inset * 2)
      let verticalTravel = max(0, canvasSize.height - logoSize.height - inset * 2)
      let speed = max(settings.resolvedARMSX2LogoSpeed, 0.15)
      let horizontalDistance = elapsedTime * 42 * speed
      let verticalDistance = elapsedTime * 31 * speed

      return CGRect(
        origin: CGPoint(
          x: inset + horizontalTravel
            * reflectedProgress(horizontalDistance, across: horizontalTravel, phase: 0.17),
          y: inset + verticalTravel
            * reflectedProgress(verticalDistance, across: verticalTravel, phase: 0.63)
        ),
        size: logoSize
      )
    }

    private func reflectedProgress(
      _ distance: Double,
      across travel: CGFloat,
      phase: Double
    ) -> CGFloat {
      guard travel > 0 else { return 0.5 }
      var cycle = (distance / Double(travel) + phase).truncatingRemainder(dividingBy: 2)
      if cycle < 0 {
        cycle += 2
      }
      return CGFloat(cycle <= 1 ? cycle : 2 - cycle)
    }

    private struct ARMSX2LogoOrbState {
      let position: CGPoint
      let diameter: CGFloat
      let depth: Double
    }

  }

  // Draws XMB3-style sparkles around the selected vertical band.
  private func drawXMBParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    baseCount: Int
  ) {
    for index in 0..<particleCount(baseCount) {
      let seed = DynamicBackgroundMath.unit(sin(Double(index + 1) * 12.9898) * 43_758.5453)
      let secondarySeed = DynamicBackgroundMath.unit(sin(Double(index + 7) * 78.233) * 9_631.417)
      let speed = (0.006 + Double(index % 7) * 0.0011) * directedSpeed
      let progress = DynamicBackgroundMath.unit(seed + time * speed)
      let center = particleCenter(
        size: size,
        progress: progress,
        seed: seed,
        time: time
      )
      let spread =
        (CGFloat(secondarySeed) - 0.5)
        * size.height
        * (0.18 + CGFloat(seed) * 0.2)
        * CGFloat(settings.dispersion)
        * CGFloat(settings.verticalSpread)
      let twinkle = particleTwinkle(seed: seed, secondarySeed: secondarySeed, time: time)
      let diameter =
        (CGFloat(0.7 + Double(index % 5) * 0.34) + CGFloat(twinkle) * 1.35)
        * CGFloat(settings.size)
        * particleDepthScale(seed: seed)

      fillParticle(
        context: &context,
        rect: CGRect(
          x: CGFloat(progress) * size.width - diameter / 2,
          y: center + spread + outerVerticalOffset(seed: secondarySeed, size: size)
            + CGFloat(sin(time * 0.24 + seed * 18)) * 7 - diameter / 2,
          width: diameter,
          height: diameter
        ),
        color: particleColor(index: index, time: time),
        opacity: (index.isMultiple(of: 8) ? 0.52 + twinkle * 0.48 : 0.18 + twinkle * 0.3)
          * particleDepthOpacity(seed: seed)
      )
    }
  }

  // Draws small square particles based on the PS1 pixel dust.
  private func drawPixelDust(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    baseCount: Int
  ) {
    for index in 0..<particleCount(baseCount) {
      let seed = Double(index) * 1.731
      let progress = DynamicBackgroundMath.unit(
        Double(index) * 0.071
          + time * (0.008 + Double(index % 4) * 0.002) * directedSpeed
      )
      let x =
        CGFloat(
          sin(seed * 2.17 + time * 0.02 * settings.drift * directedSpeed)
            * 0.5 + 0.5
        )
        * size.width
      let y =
        size.height * CGFloat(settings.verticalLevel)
        + CGFloat(0.5 - progress) * size.height * 0.7 * CGFloat(settings.dispersion)
        * CGFloat(settings.verticalSpread)
        + outerVerticalOffset(seed: DynamicBackgroundMath.unit(seed * 0.731), size: size)
      let side =
        CGFloat(index.isMultiple(of: 9) ? 2.2 : 1.1)
        * CGFloat(settings.size)
        * particleDepthScale(seed: seed)

      context.fill(
        Path(
          CGRect(
            x: x - side / 2,
            y: y - side / 2,
            width: side,
            height: side
          )
        ),
        with: .color(
          particleColor(index: index, time: time).opacity(
            settings.brightness * settings.opacity * particleDepthOpacity(seed: seed)
          )
        )
      )
    }
  }

  // Draws soft PS4-style floating particles around the selected band.
  private func drawGlowParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    baseCount: Int
  ) {
    for index in 0..<particleCount(baseCount) {
      let seed = Double(index) * 1.618
      let x =
        CGFloat(sin(time * 0.035 * directedSpeed + seed) * 0.5 + 0.5)
        * size.width
      let y =
        size.height * CGFloat(settings.verticalLevel)
        + CGFloat(cos(time * 0.026 * directedSpeed + seed * 1.37))
        * size.height
        * 0.28
        * CGFloat(settings.dispersion)
        * CGFloat(settings.verticalSpread)
        + outerVerticalOffset(seed: DynamicBackgroundMath.unit(seed * 0.619), size: size)
      let diameter =
        CGFloat(1.2 + Double(index % 4) * 0.55)
        * CGFloat(settings.size)
        * particleDepthScale(seed: seed)

      fillParticle(
        context: &context,
        rect: CGRect(
          x: x - diameter / 2,
          y: y - diameter / 2,
          width: diameter,
          height: diameter
        ),
        color: particleColor(index: index, time: time),
        opacity: (index.isMultiple(of: 5) ? 0.52 : 0.34)
          * particleDepthOpacity(seed: seed)
      )
    }
  }

  // Draws PS5-style drifting points across the whole screen width.
  private func drawDriftParticles(
    context: inout GraphicsContext,
    size: CGSize,
    time: TimeInterval,
    baseCount: Int
  ) {
    for index in 0..<particleCount(baseCount) {
      let seed = Double(index) * 1.618
      let progress = DynamicBackgroundMath.unit(
        Double(index) * 0.047
          + time * (0.006 + Double(index % 5) * 0.0015) * directedSpeed
      )
      let x =
        CGFloat(progress) * size.width
        + CGFloat(sin(time * 0.16 * directedSpeed + seed))
        * 24 * CGFloat(settings.drift)
      let y =
        size.height * CGFloat(settings.verticalLevel)
        + CGFloat(cos(seed * 1.37)) * size.height * 0.42 * CGFloat(settings.dispersion)
        * CGFloat(settings.verticalSpread)
        + outerVerticalOffset(seed: DynamicBackgroundMath.unit(seed * 0.487), size: size)
        + CGFloat(sin(time * 0.1 * directedSpeed + seed))
        * 38 * CGFloat(settings.drift)
      let diameter =
        CGFloat(0.8 + Double(index % 4) * 0.52)
        * CGFloat(settings.size)
        * particleDepthScale(seed: seed)

      fillParticle(
        context: &context,
        rect: CGRect(
          x: x - diameter / 2,
          y: y - diameter / 2,
          width: diameter,
          height: diameter
        ),
        color: particleColor(index: index, time: time),
        opacity: (index.isMultiple(of: 7) ? 0.58 : 0.28)
          * particleDepthOpacity(seed: seed)
      )
    }
  }

  // Draws one particle ellipse with the shared brightness multiplier.
  private func fillParticle(
    context: inout GraphicsContext,
    rect: CGRect,
    color: Color,
    opacity: Double
  ) {
    context.fill(
      Path(ellipseIn: rect),
      with: .color(color.opacity(opacity * settings.brightness * settings.opacity))
    )
  }

  private func particleCenter(
    size: CGSize,
    progress: Double,
    seed: Double,
    time: TimeInterval
  ) -> CGFloat {
    let normalizedY =
      CGFloat(settings.verticalLevel)
      + 0.095
      * CGFloat(
        sin(progress * .pi * 2.1 - time * 0.11 * directedSpeed + seed * 5)
      )
      * CGFloat(settings.dispersion)
      * CGFloat(settings.verticalSpread)
    return size.height * normalizedY
  }

  private func outerVerticalOffset(seed: Double, size: CGSize) -> CGFloat {
    guard settings.outerDispersion > 0 else { return 0 }

    let centeredSeed = CGFloat(DynamicBackgroundMath.unit(seed) * 2 - 1)
    let direction: CGFloat = centeredSeed < 0 ? -1 : 1
    let edgeBias = pow(abs(centeredSeed), 0.45)
    return direction * edgeBias * size.height * 0.16 * CGFloat(settings.outerDispersion)
  }

  private func particleDepthScale(seed: Double) -> CGFloat {
    let variation = (DynamicBackgroundMath.unit(seed * 1.371) - 0.5) * settings.depthVariation
    return CGFloat(max(0.25, 1 + variation))
  }

  private func particleDepthOpacity(seed: Double) -> Double {
    let scale = Double(particleDepthScale(seed: seed))
    return min(1.35, max(0.35, 0.72 + scale * 0.28))
  }

  private func particleTwinkle(
    seed: Double,
    secondarySeed: Double,
    time: TimeInterval
  ) -> Double {
    pow(
      max(0, sin(time * (0.9 + seed) * settings.speed + secondarySeed * 14)),
      3
    ) * settings.twinkle
  }

  private func particleColor(index: Int, time: TimeInterval) -> Color {
    index.isMultiple(of: 8)
      ? theme.highlightColor(index: index, time: time)
      : theme.ribbonColor(index: index, time: time)
  }

  private func particleCount(_ baseCount: Int) -> Int {
    min(
      900,
      max(
        0,
        frameRates.scaledDynamicEffectCount(
          Double(baseCount) * settings.amount * settings.verticalDensity
        )
      )
    )
  }

}
