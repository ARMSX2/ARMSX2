// PlayStationRibbonsSettingsControls.swift — ribbons settings controls
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - PlayStationRibbonsSettingsControls

struct PlayStationRibbonsSettingsControls: View {
  @Binding var settings: PlayStationRibbonsSettings
  private let defaults = PlayStationRibbonsSettings()

  var body: some View {
    BackgroundSettingsResetHeader { settings = defaults }

    BackgroundControlSection("Visibility") {
      controlToggle("Show panels", value: $settings.showsPanels, defaultValue: defaults.showsPanels)
      controlToggle(
        "Show panel glow", value: $settings.showsPanelGlow, defaultValue: defaults.showsPanelGlow)
      controlToggle(
        "Show panel edges", value: $settings.showsPanelEdges, defaultValue: defaults.showsPanelEdges
      )
      controlToggle(
        "Show native particles", value: $settings.showsParticles,
        defaultValue: defaults.showsParticles)
    }
    BackgroundControlSection("Panel geometry") {
      controlSlider(
        "Panel count", value: $settings.panelCount, range: 1...12,
        defaultValue: defaults.panelCount, format: .plain)
      controlSlider(
        "Panel height", value: $settings.panelVerticalPosition, range: -0.2...0.8,
        defaultValue: defaults.panelVerticalPosition, format: .unitPercent)
      controlSlider(
        "Panel spacing", value: $settings.panelSpacing, range: 0...2,
        defaultValue: defaults.panelSpacing, format: .multiplier)
      controlSlider(
        "Panel amplitude", value: $settings.panelAmplitude, range: 0...3,
        defaultValue: defaults.panelAmplitude, format: .multiplier)
      controlSlider(
        "Panel thickness", value: $settings.panelThickness, range: 0.1...4,
        defaultValue: defaults.panelThickness, format: .multiplier)
      controlSlider(
        "Panel curvature", value: $settings.panelCurvature, range: 0...3,
        defaultValue: defaults.panelCurvature, format: .multiplier)
      controlSlider(
        "Phase spread", value: $settings.phaseSpread, range: 0...3,
        defaultValue: defaults.phaseSpread, format: .multiplier)
    }
    BackgroundControlSection("Panel material") {
      controlSlider(
        "Fill opacity", value: $settings.panelFillOpacity, range: 0...3,
        defaultValue: defaults.panelFillOpacity, format: .unitPercent)
      controlSlider(
        "Glow opacity", value: $settings.panelGlowOpacity, range: 0...3,
        defaultValue: defaults.panelGlowOpacity, format: .unitPercent)
      controlSlider(
        "Glow blur", value: $settings.panelGlowBlur, range: 0...3,
        defaultValue: defaults.panelGlowBlur, format: .multiplier)
      controlSlider(
        "Edge opacity", value: $settings.panelEdgeOpacity, range: 0...3,
        defaultValue: defaults.panelEdgeOpacity, format: .unitPercent)
      controlSlider(
        "Edge width", value: $settings.panelEdgeWidth, range: 0.1...4,
        defaultValue: defaults.panelEdgeWidth, format: .multiplier)
    }
    BackgroundControlSection("Native particles") {
      controlSlider(
        "Particle count", value: $settings.particleCount, range: 0...500,
        defaultValue: defaults.particleCount, format: .plain)
      controlSlider(
        "Particle speed", value: $settings.particleSpeed, range: 0...4,
        defaultValue: defaults.particleSpeed, format: .multiplier)
      controlSlider(
        "Particle drift", value: $settings.particleDrift, range: 0...4,
        defaultValue: defaults.particleDrift, format: .multiplier)
      controlSlider(
        "Vertical spread", value: $settings.particleVerticalSpread, range: 0...3,
        defaultValue: defaults.particleVerticalSpread, format: .multiplier)
      controlSlider(
        "Particle size", value: $settings.particleSize, range: 0.1...5,
        defaultValue: defaults.particleSize, format: .multiplier)
      controlSlider(
        "Particle opacity", value: $settings.particleOpacity, range: 0...3,
        defaultValue: defaults.particleOpacity, format: .unitPercent)
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
