// MulticolorAmbientBackground.swift — multicolor ambient background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - MulticolorAmbientBackground

struct MulticolorAmbientBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  var body: some View {
    AdaptiveAnimationTimeline(domain: .dynamicBackground) { timeline in
      let time =
        timeline.date.timeIntervalSinceReferenceDate
        * theme.particleSettings.backgrounds.multicolorAmbientSpeed
      let primaryColor = theme.sharedColor(index: 0, time: time)
      let secondaryColor = theme.ribbonColor(index: 1, time: time)
      let darkColor = theme.sharedColor(index: 2, time: time)
      let sharedGradient = theme.sharedGradientPoints(
        from: .topLeading,
        to: .bottomTrailing
      )

      ZStack {
        PaletteGradientField(
          colors: [
            theme.paletteBackgroundColor(primaryColor, darkness: 0.91),
            theme.paletteBackgroundColor(darkColor, darkness: 0.76),
            theme.paletteBackgroundColor(primaryColor, darkness: 0.84),
          ],
          startPoint: sharedGradient.start,
          endPoint: sharedGradient.end,
          curvature: theme.sharedPaletteGradientCurvature
        )

        Circle()
          .fill(primaryColor.opacity(0.34))
          .frame(width: 330, height: 330)
          .blur(radius: 80)
          .offset(
            x: CGFloat(sin(time * 0.21)) * 150 - 100,
            y: CGFloat(cos(time * 0.17)) * 100 - 160
          )

        Circle()
          .fill(secondaryColor.opacity(0.24))
          .frame(width: 300, height: 300)
          .blur(radius: 90)
          .offset(
            x: CGFloat(cos(time * 0.19)) * 170 + 130,
            y: CGFloat(sin(time * 0.15)) * 150 + 120
          )

        Circle()
          .fill(.white.opacity(0.12))
          .frame(width: 250, height: 250)
          .blur(radius: 70)
          .offset(
            x: CGFloat(cos(time * 0.13)) * 110,
            y: CGFloat(sin(time * 0.23)) * 130 + 180
          )

        DynamicWallpaperCanvas(rendersAsynchronously: true) { context, size in
          let particleColor = theme.ribbonColor(index: 2, time: time)
          let particleCount = frameRates.scaledDynamicEffectCount(26)
          for index in 0..<particleCount {
            let phase = Double(index) * 1.73
            let x = (sin(time * 0.07 + phase) * 0.5 + 0.5) * size.width
            let y = (cos(time * 0.05 + phase * 1.4) * 0.5 + 0.5) * size.height
            let rect = CGRect(x: x, y: y, width: 1.5, height: 1.5)
            context.fill(Path(ellipseIn: rect), with: .color(particleColor.opacity(0.22)))
          }
        }

        Rectangle()
          .fill(theme.paletteDarkOverlay(opacity: 0.08))
      }
      .ignoresSafeArea()
    }
  }
}
