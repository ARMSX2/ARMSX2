// PlayStationPortableBlurSettingsControls.swift — PSP blur settings controls
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - PlayStationPortableBlurSettingsControls

struct PlayStationPortableBlurSettingsControls: View {
  @Binding var settings: PlayStationPortableBlurSettings
  private let defaults = PlayStationPortableBlurSettings()

  var body: some View {
    BackgroundSettingsResetHeader { settings = defaults }

    BackgroundControlSection("Visibility") {
      controlToggle(
        "Show ribbons", value: $settings.showsRibbons, defaultValue: defaults.showsRibbons)
      controlToggle(
        "Show ribbon glow", value: $settings.showsGlow, defaultValue: defaults.showsGlow)
      controlToggle(
        "Show ribbon cores", value: $settings.showsCores, defaultValue: defaults.showsCores)
      controlToggle(
        "Alternate direction", value: $settings.alternatesDirection,
        defaultValue: defaults.alternatesDirection)
    }
    BackgroundControlSection("Ribbon geometry") {
      controlSlider(
        "Ribbon count", value: $settings.ribbonCount, range: 1...14,
        defaultValue: defaults.ribbonCount, format: .plain)
      controlSlider(
        "Vertical offset", value: $settings.verticalOffset, range: -0.6...0.6,
        defaultValue: defaults.verticalOffset, format: .unitPercent.signed)
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
    BackgroundControlSection("Ribbon material") {
      controlSlider(
        "Broad width", value: $settings.broadWidth, range: 0.1...4,
        defaultValue: defaults.broadWidth, format: .multiplier)
      controlSlider(
        "Glow blur", value: $settings.glowBlur, range: 0...3, defaultValue: defaults.glowBlur,
        format: .multiplier)
      controlSlider(
        "Glow opacity", value: $settings.glowOpacity, range: 0...3,
        defaultValue: defaults.glowOpacity, format: .unitPercent)
      controlSlider(
        "Core width", value: $settings.coreWidth, range: 0.1...4, defaultValue: defaults.coreWidth,
        format: .multiplier)
      controlSlider(
        "Core blur", value: $settings.coreBlur, range: 0...3, defaultValue: defaults.coreBlur,
        format: .multiplier)
      controlSlider(
        "Core opacity", value: $settings.coreOpacity, range: 0...3,
        defaultValue: defaults.coreOpacity, format: .unitPercent)
    }
    BackgroundControlSection("Environment") {
      controlSlider(
        "Background intensity", value: $settings.backgroundIntensity, range: 0...2,
        defaultValue: defaults.backgroundIntensity, format: .unitPercent)
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
