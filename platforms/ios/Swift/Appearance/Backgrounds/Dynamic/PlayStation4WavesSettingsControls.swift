// PlayStation4WavesSettingsControls.swift — PS4 waves settings controls
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - PlayStation4WavesSettingsControls

struct PlayStation4WavesSettingsControls: View {
  @Binding var settings: PlayStation4WavesSettings
  private let defaults = PlayStation4WavesSettings()

  var body: some View {
    BackgroundSettingsResetHeader { settings = defaults }

    BackgroundControlSection("Visibility") {
      controlToggle(
        "Show ambient glows", value: $settings.showsAmbientGlows,
        defaultValue: defaults.showsAmbientGlows)
      controlToggle(
        "Show native particles", value: $settings.showsParticles,
        defaultValue: defaults.showsParticles)
      controlToggle("Show waves", value: $settings.showsWaves, defaultValue: defaults.showsWaves)
      controlToggle(
        "Show wave glow", value: $settings.showsWaveGlow, defaultValue: defaults.showsWaveGlow)
      controlToggle(
        "Show wave cores", value: $settings.showsWaveCores, defaultValue: defaults.showsWaveCores)
    }
    BackgroundControlSection("Native particles") {
      controlSlider(
        "Particle count", value: $settings.particleCount, range: 0...300,
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
    }
    BackgroundControlSection("Wave geometry") {
      controlSlider(
        "Wave count", value: $settings.waveCount, range: 1...16, defaultValue: defaults.waveCount,
        format: .plain)
      controlSlider(
        "Wave height", value: $settings.waveVerticalPosition, range: 0...1,
        defaultValue: defaults.waveVerticalPosition, format: .unitPercent)
      controlSlider(
        "Wave spacing", value: $settings.waveSpacing, range: 0...3,
        defaultValue: defaults.waveSpacing, format: .multiplier)
      controlSlider(
        "Wave amplitude", value: $settings.waveAmplitude, range: 0...3,
        defaultValue: defaults.waveAmplitude, format: .multiplier)
      controlSlider(
        "Wave curvature", value: $settings.waveCurvature, range: 0...3,
        defaultValue: defaults.waveCurvature, format: .multiplier)
      controlSlider(
        "Phase spread", value: $settings.phaseSpread, range: 0...3,
        defaultValue: defaults.phaseSpread, format: .multiplier)
    }
    BackgroundControlSection("Wave material") {
      controlSlider(
        "Glow blur", value: $settings.glowBlur, range: 0...50, defaultValue: defaults.glowBlur,
        format: .points.decimals(1))
      controlSlider(
        "Glow width", value: $settings.glowWidth, range: 0.1...3, defaultValue: defaults.glowWidth,
        format: .multiplier)
      controlSlider(
        "Glow opacity", value: $settings.glowOpacity, range: 0...3,
        defaultValue: defaults.glowOpacity, format: .unitPercent)
      controlSlider(
        "Core width", value: $settings.coreWidth, range: 0.1...4, defaultValue: defaults.coreWidth,
        format: .multiplier)
      controlSlider(
        "Core opacity", value: $settings.coreOpacity, range: 0...3,
        defaultValue: defaults.coreOpacity, format: .unitPercent)
    }
    BackgroundControlSection("Environment") {
      controlSlider(
        "Ambient glow scale", value: $settings.ambientGlowScale, range: 0.1...3,
        defaultValue: defaults.ambientGlowScale, format: .multiplier)
      controlSlider(
        "Ambient glow intensity", value: $settings.ambientGlowIntensity, range: 0...3,
        defaultValue: defaults.ambientGlowIntensity, format: .unitPercent)
      controlSlider(
        "Vignette intensity", value: $settings.vignetteIntensity, range: 0...3,
        defaultValue: defaults.vignetteIntensity, format: .unitPercent)
    }
  }
}
