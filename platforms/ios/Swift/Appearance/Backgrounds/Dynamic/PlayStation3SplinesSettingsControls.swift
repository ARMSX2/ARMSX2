// PlayStation3SplinesSettingsControls.swift — PS3 splines settings controls
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - PlayStation3SplinesSettingsControls

struct PlayStation3SplinesSettingsControls: View {
  @Binding var settings: PlayStation3SplinesSettings
  private let defaults = PlayStation3SplinesSettings()

  var body: some View {
    BackgroundSettingsResetHeader { settings = defaults }

    BackgroundControlSection("Visibility") {
      controlToggle(
        "Show splines", value: $settings.showsSplines, defaultValue: defaults.showsSplines)
      controlToggle(
        "Show native particles", value: $settings.showsParticles,
        defaultValue: defaults.showsParticles)
    }
    BackgroundControlSection("Spline geometry") {
      controlSlider(
        "Strand count", value: $settings.strandCount, range: 1...30,
        defaultValue: defaults.strandCount, format: .plain)
      controlSlider(
        "Vertical position", value: $settings.verticalPosition, range: 0.2...0.9,
        defaultValue: defaults.verticalPosition, format: .unitPercent)
      controlSlider(
        "Strand spacing", value: $settings.strandSpacing, range: 0...3,
        defaultValue: defaults.strandSpacing, format: .multiplier)
      controlSlider(
        "Wave amplitude", value: $settings.waveAmplitude, range: 0...3,
        defaultValue: defaults.waveAmplitude, format: .multiplier)
      controlSlider(
        "Detail amplitude", value: $settings.detailAmplitude, range: 0...3,
        defaultValue: defaults.detailAmplitude, format: .multiplier)
      controlSlider(
        "Phase spread", value: $settings.phaseSpread, range: 0...3,
        defaultValue: defaults.phaseSpread, format: .multiplier)
    }
    BackgroundControlSection("Spline material") {
      controlSlider(
        "Glow opacity", value: $settings.glowOpacity, range: 0...3,
        defaultValue: defaults.glowOpacity, format: .unitPercent)
      controlSlider(
        "Core opacity", value: $settings.coreOpacity, range: 0...3,
        defaultValue: defaults.coreOpacity, format: .unitPercent)
      controlSlider(
        "Glow width", value: $settings.glowWidth, range: 0.1...3, defaultValue: defaults.glowWidth,
        format: .multiplier)
      controlSlider(
        "Core width", value: $settings.coreWidth, range: 0.1...4, defaultValue: defaults.coreWidth,
        format: .multiplier)
      controlSlider(
        "Glow blur", value: $settings.glowBlur, range: 0...3, defaultValue: defaults.glowBlur,
        format: .multiplier)
    }
    BackgroundControlSection("Native particles") {
      controlSlider(
        "Particle count", value: $settings.particleCount, range: 0...600,
        defaultValue: defaults.particleCount, format: .plain)
      controlSlider(
        "Particle speed", value: $settings.particleSpeed, range: 0...4,
        defaultValue: defaults.particleSpeed, format: .multiplier)
      controlSlider(
        "Particle spread", value: $settings.particleSpread, range: 0...3,
        defaultValue: defaults.particleSpread, format: .multiplier)
      controlSlider(
        "Particle size", value: $settings.particleSize, range: 0.1...5,
        defaultValue: defaults.particleSize, format: .multiplier)
      controlSlider(
        "Particle opacity", value: $settings.particleOpacity, range: 0...3,
        defaultValue: defaults.particleOpacity, format: .unitPercent)
      controlSlider(
        "Twinkle speed", value: $settings.particleTwinkle, range: 0...4,
        defaultValue: defaults.particleTwinkle, format: .multiplier)
    }
    BackgroundControlSection("Environment") {
      controlSlider(
        "Background intensity", value: $settings.backgroundIntensity, range: 0...2,
        defaultValue: defaults.backgroundIntensity, format: .unitPercent)
      controlSlider(
        "Vignette intensity", value: $settings.vignetteIntensity, range: 0...3,
        defaultValue: defaults.vignetteIntensity, format: .unitPercent)
    }
  }
}
