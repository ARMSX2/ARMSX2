// DynamicBackgroundSettingsControls.swift — dynamic background settings controls
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - DynamicBackgroundSettingsControls

struct DynamicBackgroundSettingsControls: View {
  @Binding var particleSettings: DynamicParticleSettings
  @Binding var isPlayStation3XMBPresetExplicit: Bool
  let dynamicBackground: DynamicBackgroundStyle

  @AppStorage("ARMSX2iOSDynamicExpandedNestedGroups")
  private var expandedDynamicSettingsGroups = ""
  private let defaults = DynamicBackgroundMotionSettings()

  var body: some View {
    settingsContent
      .onAppear(perform: expandCurrentBackgroundSettings)
      .onChange(of: dynamicBackground) { _, _ in
        expandCurrentBackgroundSettings()
      }
  }

  private var settingsContent: some View {
    VStack(alignment: .leading, spacing: 10) {
      if dynamicBackground == .playStation3XMBByMart {
        backgroundSettingGroup(.playStation3XMBByMart) {
          PlayStation3XMBMartSettingsControls(
            settings: $particleSettings.playStation3XMB,
            isPresetExplicit: $isPlayStation3XMBPresetExplicit
          )
        }
      }
      activeBackgroundSettingGroup(.multicolorAmbient) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.multicolorAmbientSpeed,
          defaultValue: defaults.multicolorAmbientSpeed
        )
      }
      activeBackgroundSettingGroup(.lightSpeed) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.lightSpeedMotionSpeed,
          defaultValue: defaults.lightSpeedMotionSpeed
        )
      }
      activeBackgroundSettingGroup(.spatialRetro) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.spatialRetroSpeed,
          defaultValue: defaults.spatialRetroSpeed
        )
      }
      activeBackgroundSettingGroup(.towersOrbs) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.towersOrbsSpeed,
          defaultValue: defaults.towersOrbsSpeed
        )
      }
      activeBackgroundSettingGroup(.playStation2Menu) {
        particleSlider(
          "FPS",
          value: $particleSettings.backgrounds.playStation2MenuFramesPerSecond,
          range: 15...120,
          step: 1,
          formattedValue:
            "\(Int(particleSettings.backgrounds.playStation2MenuFramesPerSecond)) FPS",
          resetValue: defaults.playStation2MenuFramesPerSecond
        )
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.playStation2MenuSceneSpeed,
          defaultValue: defaults.playStation2MenuSceneSpeed
        )
        particleSlider(
          "Orbit angle",
          value: $particleSettings.backgrounds.playStation2MenuOrbitDegrees,
          range: 0...45,
          step: 1,
          formattedValue: String(
            format: "%.0f°",
            particleSettings.backgrounds.playStation2MenuOrbitDegrees
          ),
          resetValue: defaults.playStation2MenuOrbitDegrees
        )
        particleSlider(
          "Yaw speed",
          value: $particleSettings.backgrounds.playStation2MenuYawSpeed,
          range: 0...10,
          step: 0.1,
          formattedValue: String(
            format: "%.1fx",
            particleSettings.backgrounds.playStation2MenuYawSpeed
          ),
          resetValue: defaults.playStation2MenuYawSpeed
        )
        particleSlider(
          "Roll speed",
          value: $particleSettings.backgrounds.playStation2MenuRollSpeed,
          range: 0...12,
          step: 0.1,
          formattedValue: String(
            format: "%.1fx",
            particleSettings.backgrounds.playStation2MenuRollSpeed
          ),
          resetValue: defaults.playStation2MenuRollSpeed
        )
        HStack {
          Toggle(
            "Colour towers",
            isOn: $particleSettings.backgrounds.playStation2MenuColorsTowers
          )
          DynamicSettingsResetButton(title: "Colour towers") {
            particleSettings.backgrounds.playStation2MenuColorsTowers =
              defaults.playStation2MenuColorsTowers
          }
        }
      }
      activeBackgroundSettingGroup(.faceButtons) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.faceButtonsSpeed,
          defaultValue: defaults.faceButtonsSpeed
        )
        HStack {
          Toggle(
            "Show line waves",
            isOn: $particleSettings.backgrounds.faceButtonsShowsLightBands
          )
          DynamicSettingsResetButton(title: "Show Face Buttons line waves") {
            particleSettings.backgrounds.faceButtonsShowsLightBands =
              defaults.faceButtonsShowsLightBands
          }
        }
      }
      activeBackgroundSettingGroup(.playStationPortableBlur) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.playStationPortableBlurSpeed,
          defaultValue: defaults.playStationPortableBlurSpeed
        )
        PlayStationPortableBlurSettingsControls(
          settings: $particleSettings.backgrounds.playStationPortableBlur
        )
      }
      activeBackgroundSettingGroup(.playStation3Splines) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.playStation3SplinesSpeed,
          defaultValue: defaults.playStation3SplinesSpeed
        )
        PlayStation3SplinesSettingsControls(
          settings: $particleSettings.backgrounds.playStation3Splines
        )
      }
      activeBackgroundSettingGroup(.playStation4Particles) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.playStation4ParticlesSpeed,
          defaultValue: defaults.playStation4ParticlesSpeed
        )
        PlayStation4ParticlesSettingsControls(
          settings: $particleSettings.backgrounds.playStation4Particles
        )
      }
      activeBackgroundSettingGroup(.playStation4Waves) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.playStation4WavesSpeed,
          defaultValue: defaults.playStation4WavesSpeed
        )
        PlayStation4WavesSettingsControls(
          settings: $particleSettings.backgrounds.playStation4Waves
        )
      }
      activeBackgroundSettingGroup(.playStationRibbons) {
        backgroundSpeedSlider(
          value: $particleSettings.backgrounds.playStationRibbonsSpeed,
          defaultValue: defaults.playStationRibbonsSpeed
        )
        PlayStationRibbonsSettingsControls(
          settings: $particleSettings.backgrounds.playStationRibbons
        )
      }
    }
  }

  private func backgroundSettingGroup<Content: View>(
    _ background: DynamicBackgroundStyle,
    @ViewBuilder content: @escaping () -> Content
  ) -> some View {
    DisclosureGroup(
      isExpanded: DynamicSettingsExpansion.binding(
        storage: $expandedDynamicSettingsGroups,
        identifier: background.settingsExpansionIdentifier
      )
    ) {
      VStack(alignment: .leading, spacing: 12) {
        content()
      }
      .padding(.top, 10)
    } label: {
      Label(background.title, systemImage: background.systemImage)
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(.white)
    }
  }

  @ViewBuilder
  private func activeBackgroundSettingGroup<Content: View>(
    _ background: DynamicBackgroundStyle,
    @ViewBuilder content: @escaping () -> Content
  ) -> some View {
    if background == dynamicBackground {
      backgroundSettingGroup(background, content: content)
    }
  }

  private func expandCurrentBackgroundSettings() {
    DynamicSettingsExpansion.binding(
      storage: $expandedDynamicSettingsGroups,
      identifier: dynamicBackground.settingsExpansionIdentifier
    ).wrappedValue = true
  }

  private func backgroundSpeedSlider(
    value: Binding<Double>,
    defaultValue: Double
  ) -> some View {
    particleSlider(
      "Motion speed",
      value: value,
      range: 0.1...4,
      step: 0.1,
      formattedValue: String(format: "%.1fx", value.wrappedValue),
      resetValue: defaultValue
    )
  }

  private func particleSlider(
    _ title: String,
    value: Binding<Double>,
    range: ClosedRange<Double>,
    step: Double? = nil,
    formattedValue: String,
    resetValue: Double
  ) -> some View {
    NumberRow(
      title,
      value: value,
      in: range,
      format: .opaque(formattedValue),
      step: step,
      default: resetValue,
      settings: SettingsStore.shared
    )
  }

}
