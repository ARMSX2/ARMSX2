// PlayStation3XMBMartSettingsControls.swift — XMB by Mart settings controls
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation3XMBMartSettingsControls

struct PlayStation3XMBMartSettingsControls: View {
  @Binding var settings: PlayStation3XMBSettings
  @Binding var isPresetExplicit: Bool
  @AppStorage("ARMSX2iOSDynamicExpandedNestedGroups")
  private var expandedGroups = ""
  private let defaults = PlayStation3XMBSettings()

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Spacer()

        Button {
          settings = defaults
          isPresetExplicit = false
        } label: {
          Label("Reset", systemImage: "arrow.counterclockwise")
            .font(.caption.weight(.semibold))
        }
        .buttonStyle(.borderless)
      }

      xmbMartSettingGroup("Background") {
        HStack {
          Picker(
            "Gradient preset",
            selection: gradientPresetBinding
          ) {
            ForEach(PlayStation3XMBGradientPreset.allCases) { preset in
              Text(preset.title).tag(preset)
            }
          }
          .pickerStyle(.menu)
          DynamicSettingsResetButton(title: "Gradient preset") {
            settings.gradientPreset =
              defaults.gradientPreset
            isPresetExplicit = false
          }
        }

        particleSlider(
          "Color red",
          value: $settings.colorR,
          range: 0...255,
          step: 1,
          formattedValue: String(format: "%.0f", settings.colorR)
        )
        particleSlider(
          "Color green",
          value: $settings.colorG,
          range: 0...255,
          step: 1,
          formattedValue: String(format: "%.0f", settings.colorG)
        )
        particleSlider(
          "Color blue",
          value: $settings.colorB,
          range: 0...255,
          step: 1,
          formattedValue: String(format: "%.0f", settings.colorB)
        )
        particleSlider(
          "Gradient top multiplier",
          value: $settings.gradientTopMul,
          range: 0...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.gradientTopMul)
        )
        particleSlider(
          "Gradient bottom multiplier",
          value: $settings.gradientBotMul,
          range: 0.2...1.2,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.gradientBotMul)
        )
      }

      xmbMartSettingGroup("Particles") {
        particleSlider(
          "Count",
          value: $settings.particleCount,
          range: 10...4000,
          step: 1,
          formattedValue: String(format: "%.0f", settings.particleCount)
        )
        particleSlider(
          "Opacity",
          value: $settings.particleOpacity,
          range: 0...1,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.particleOpacity)
        )
        particleSlider(
          "Base size",
          value: $settings.particleSizeBase,
          range: 1...40,
          step: 0.1,
          formattedValue: String(format: "%.1f", settings.particleSizeBase)
        )
        particleSlider(
          "Size variance",
          value: $settings.particleSizeVariance,
          range: 0...50,
          step: 0.1,
          formattedValue: String(format: "%.1f", settings.particleSizeVariance)
        )
        particleSlider(
          "Particle flow speed",
          value: $settings.particleFlowSpeed,
          range: 0...3,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.particleFlowSpeed)
        )
        particleSlider(
          "Y-axis spread",
          value: $settings.particleVerticalSpread,
          range: 0.25...3.0,
          step: 0.01,
          formattedValue: "\(Int(settings.particleVerticalSpread * 100))%",
          resetValue: defaults.particleVerticalSpread
        )
        particleSlider(
          "Y-axis density",
          value: $settings.particleVerticalDensity,
          range: 0.25...3.0,
          step: 0.01,
          formattedValue: "\(Int(settings.particleVerticalDensity * 100))%",
          resetValue: defaults.particleVerticalDensity
        )
        particleSlider(
          "Outer top/bottom dispersion",
          value: $settings.particleOuterDispersion,
          range: 0.0...2.5,
          step: 0.01,
          formattedValue: "\(Int(settings.particleOuterDispersion * 100))%",
          resetValue: defaults.particleOuterDispersion
        )
        particleSlider(
          "Depth variation",
          value: $settings.particleDepthVariation,
          range: 0.0...1.5,
          step: 0.01,
          formattedValue: "\(Int(settings.particleDepthVariation * 100))%",
          resetValue: defaults.particleDepthVariation
        )
        HStack {
          Toggle(
            "Follow wave Y-axis",
            isOn: $settings.particlesFollowWaveY
          )
          DynamicSettingsResetButton(title: "Follow wave Y-axis") {
            settings.particlesFollowWaveY =
              defaults.particlesFollowWaveY
          }
        }
      }

      xmbMartSettingGroup("Spline motion") {
        particleSlider(
          "Flow speed",
          value: $settings.flowSpeed,
          range: 0...1.2,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.flowSpeed)
        )
        particleSlider(
          "Tension",
          value: $settings.tension,
          range: 0...0.5,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.tension)
        )
        particleSlider(
          "Damping",
          value: $settings.damping,
          range: 0...0.002,
          step: 0.00001,
          formattedValue: String(format: "%.5f", settings.damping)
        )
        particleSlider(
          "Length",
          value: $settings.length,
          range: 0.05...1.2,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.length)
        )
        particleSlider(
          "Spacing",
          value: $settings.spacing,
          range: 10...800,
          step: 1,
          formattedValue: String(format: "%.0f", settings.spacing)
        )
        particleSlider(
          "Time step",
          value: $settings.timeStep,
          range: 0.1...4,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.timeStep)
        )
      }

      xmbMartSettingGroup("Bands and travel") {
        particleSlider(
          "Band amplitude",
          value: $settings.bandAmplitude,
          range: 0...0.6,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.bandAmplitude)
        )
        particleSlider(
          "Secondary frequency",
          value: $settings.bandSecondaryFrequency,
          range: 0.5...16,
          step: 0.1,
          formattedValue: String(format: "%.1f", settings.bandSecondaryFrequency)
        )
        particleSlider(
          "Secondary amplitude",
          value: $settings.bandSecondaryAmplitude,
          range: 0...0.12,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.bandSecondaryAmplitude)
        )
        particleSlider(
          "Travel speed 1",
          value: $settings.travelSpeed1,
          range: 0...1.5,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.travelSpeed1)
        )
        particleSlider(
          "Travel amplitude 1",
          value: $settings.travelAmplitude1,
          range: 0...0.08,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.travelAmplitude1)
        )
        particleSlider(
          "Travel speed 2",
          value: $settings.travelSpeed2,
          range: 0...1.5,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.travelSpeed2)
        )
        particleSlider(
          "Travel amplitude 2",
          value: $settings.travelAmplitude2,
          range: 0...0.08,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.travelAmplitude2)
        )
      }

      xmbMartSettingGroup("Wave shaping") {
        particleSlider(
          "Perturbation",
          value: $settings.perturbation,
          range: 0...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.perturbation)
        )
        particleSlider(
          "Perturbation scale",
          value: $settings.perturbationScale,
          range: 0...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.perturbationScale)
        )
        particleSlider(
          "Cosine amplitude",
          value: $settings.waveCosineAmplitude,
          range: 0...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.waveCosineAmplitude)
        )
        particleSlider(
          "Wave bias",
          value: $settings.waveBias,
          range: -0.3...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.waveBias)
        )
        particleSlider(
          "Height scale",
          value: $settings.waveHeightScale,
          range: 0...1,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.waveHeightScale)
        )
        particleSlider(
          "Soft clip",
          value: $settings.waveSoftClip,
          range: 0.05...0.5,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.waveSoftClip)
        )
      }

      xmbMartSettingGroup("Reverse pipeline") {
        particleSlider(
          "Pipeline blend",
          value: $settings.reversePipelineBlend,
          range: 0...1,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.reversePipelineBlend)
        )
        particleSlider(
          "Descriptor strength",
          value: $settings.reverseDescriptorStrength,
          range: 0...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.reverseDescriptorStrength)
        )
        particleSlider(
          "Descriptor seed",
          value: $settings.reverseSyntheticDescriptorSeed,
          range: 0...100_000,
          step: 1,
          formattedValue: String(format: "%.0f", settings.reverseSyntheticDescriptorSeed)
        )
        particleSlider(
          "Descriptor motion",
          value: $settings.reverseSyntheticDescriptorMotion,
          range: 0...5,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.reverseSyntheticDescriptorMotion)
        )
        particleSlider(
          "Kernel gain",
          value: $settings.reverseKernelGain,
          range: 0...1,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.reverseKernelGain)
        )
        particleSlider(
          "Normalize gain",
          value: $settings.reverseNormalizeGain,
          range: 0...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.reverseNormalizeGain)
        )
        particleSlider(
          "Kernel phase step",
          value: $settings.reverseKernelPhaseStep,
          range: 0...8,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.reverseKernelPhaseStep)
        )
        particleSlider(
          "Index jitter",
          value: $settings.reverseIndexJitter,
          range: 0...0.5,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.reverseIndexJitter)
        )
        particleSlider(
          "Temporal smoothing",
          value: $settings.reverseTemporalSmooth,
          range: 0...0.98,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.reverseTemporalSmooth)
        )
      }

      xmbMartSettingGroup("Material") {
        particleSlider(
          "Fresnel power",
          value: $settings.fresnelPower,
          range: 0.2...8,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.fresnelPower)
        )
        particleSlider(
          "Fresnel scale",
          value: $settings.fresnelScale,
          range: 0...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.fresnelScale)
        )
        particleSlider(
          "Wave opacity",
          value: $settings.opacity,
          range: 0...1,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.opacity)
        )
        particleSlider(
          "Spline brightness",
          value: $settings.brightness,
          range: 0...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.brightness)
        )
        particleSlider(
          "Z detail scale",
          value: $settings.zDetailScale,
          range: 0...0.25,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.zDetailScale)
        )
      }

      xmbMartSettingGroup("Free-form deformation") {
        particleSlider(
          "Scale 1 X",
          value: $settings.ffdScale1X,
          range: 0...8,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdScale1X)
        )
        particleSlider(
          "Scale 1 Y",
          value: $settings.ffdScale1Y,
          range: 0...3,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdScale1Y)
        )
        particleSlider(
          "Scale 1 Z",
          value: $settings.ffdScale1Z,
          range: 0...3,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdScale1Z)
        )
        particleSlider(
          "Scale 2 X",
          value: $settings.ffdScale2X,
          range: 0...8,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdScale2X)
        )
        particleSlider(
          "Scale 2 Y",
          value: $settings.ffdScale2Y,
          range: 0...3,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdScale2Y)
        )
        particleSlider(
          "Scale 2 Z",
          value: $settings.ffdScale2Z,
          range: 0...6,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdScale2Z)
        )
        particleSlider(
          "Offset X",
          value: $settings.ffdOffsetX,
          range: -2...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdOffsetX)
        )
        particleSlider(
          "Offset Y",
          value: $settings.ffdOffsetY,
          range: -2...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdOffsetY)
        )
        particleSlider(
          "Offset Z",
          value: $settings.ffdOffsetZ,
          range: -2...2,
          step: 0.01,
          formattedValue: String(format: "%.2f", settings.ffdOffsetZ)
        )
        particleSlider(
          "Y amplitude",
          value: $settings.ffdYAmplitude,
          range: 0...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.ffdYAmplitude)
        )
        particleSlider(
          "Z amplitude",
          value: $settings.ffdZAmplitude,
          range: 0...0.3,
          step: 0.001,
          formattedValue: String(format: "%.3f", settings.ffdZAmplitude)
        )
      }
    }
  }

  private var gradientPresetBinding: Binding<PlayStation3XMBGradientPreset> {
    Binding(
      get: { settings.gradientPreset },
      set: { preset in
        settings.gradientPreset = preset
        isPresetExplicit = true
      }
    )
  }

  private func particleSlider(
    _ title: String,
    value: Binding<Double>,
    range: ClosedRange<Double>,
    step: Double? = nil,
    formattedValue: String,
    resetValue: Double? = nil
  ) -> some View {
    NumberRow(
      title,
      value: value,
      in: range,
      format: .opaque(formattedValue),
      step: step,
      default: resetValue ?? defaultValue(for: title),
      settings: SettingsStore.shared
    )
  }

  private func xmbMartSettingGroup<Content: View>(
    _ title: String,
    @ViewBuilder content: @escaping () -> Content
  ) -> some View {
    DisclosureGroup(
      isExpanded: DynamicSettingsExpansion.binding(
        storage: $expandedGroups,
        identifier: "mart.\(title)"
      )
    ) {
      VStack(alignment: .leading, spacing: 12) {
        content()
      }
      .padding(.top, 10)
    } label: {
      Text(title)
        .font(.subheadline.weight(.semibold))
    }
  }

  private func defaultValue(for title: String) -> Double? {
    switch title {
    case "Color red": return defaults.colorR
    case "Color green": return defaults.colorG
    case "Color blue": return defaults.colorB
    case "Gradient top multiplier": return defaults.gradientTopMul
    case "Gradient bottom multiplier": return defaults.gradientBotMul
    case "Count": return defaults.particleCount
    case "Opacity": return defaults.particleOpacity
    case "Base size": return defaults.particleSizeBase
    case "Size variance": return defaults.particleSizeVariance
    case "Particle flow speed": return defaults.particleFlowSpeed
    case "Y-axis spread": return defaults.particleVerticalSpread
    case "Y-axis density": return defaults.particleVerticalDensity
    case "Outer top/bottom dispersion": return defaults.particleOuterDispersion
    case "Depth variation": return defaults.particleDepthVariation
    case "Flow speed": return defaults.flowSpeed
    case "Tension": return defaults.tension
    case "Damping": return defaults.damping
    case "Length": return defaults.length
    case "Spacing": return defaults.spacing
    case "Time step": return defaults.timeStep
    case "Band amplitude": return defaults.bandAmplitude
    case "Secondary frequency": return defaults.bandSecondaryFrequency
    case "Secondary amplitude": return defaults.bandSecondaryAmplitude
    case "Travel speed 1": return defaults.travelSpeed1
    case "Travel amplitude 1": return defaults.travelAmplitude1
    case "Travel speed 2": return defaults.travelSpeed2
    case "Travel amplitude 2": return defaults.travelAmplitude2
    case "Perturbation": return defaults.perturbation
    case "Perturbation scale": return defaults.perturbationScale
    case "Cosine amplitude": return defaults.waveCosineAmplitude
    case "Wave bias": return defaults.waveBias
    case "Height scale": return defaults.waveHeightScale
    case "Soft clip": return defaults.waveSoftClip
    case "Pipeline blend": return defaults.reversePipelineBlend
    case "Descriptor strength": return defaults.reverseDescriptorStrength
    case "Descriptor seed": return defaults.reverseSyntheticDescriptorSeed
    case "Descriptor motion": return defaults.reverseSyntheticDescriptorMotion
    case "Kernel gain": return defaults.reverseKernelGain
    case "Normalize gain": return defaults.reverseNormalizeGain
    case "Kernel phase step": return defaults.reverseKernelPhaseStep
    case "Index jitter": return defaults.reverseIndexJitter
    case "Temporal smoothing": return defaults.reverseTemporalSmooth
    case "Fresnel power": return defaults.fresnelPower
    case "Fresnel scale": return defaults.fresnelScale
    case "Wave opacity": return defaults.opacity
    case "Spline brightness": return defaults.brightness
    case "Z detail scale": return defaults.zDetailScale
    case "Scale 1 X": return defaults.ffdScale1X
    case "Scale 1 Y": return defaults.ffdScale1Y
    case "Scale 1 Z": return defaults.ffdScale1Z
    case "Scale 2 X": return defaults.ffdScale2X
    case "Scale 2 Y": return defaults.ffdScale2Y
    case "Scale 2 Z": return defaults.ffdScale2Z
    case "Offset X": return defaults.ffdOffsetX
    case "Offset Y": return defaults.ffdOffsetY
    case "Offset Z": return defaults.ffdOffsetZ
    case "Y amplitude": return defaults.ffdYAmplitude
    case "Z amplitude": return defaults.ffdZAmplitude
    default: return nil
    }
  }
}
