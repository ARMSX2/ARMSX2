// DynamicParticleSettingsControls.swift — dynamic particle settings controls
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - DynamicParticleSettingsControls

struct DynamicParticleSettingsControls: View {
  @Binding var particleSettings: DynamicParticleSettings
  @Binding var isPlayStation3XMBPresetExplicit: Bool
  let dynamicBackground: DynamicBackgroundStyle
  let resetAllSettingsAndPalettes: () -> Void

  @State private var logoStore = ARMSX2LogoStore.shared

  @AppStorage("ARMSX2iOSDynamicAddParticlesExpanded")
  private var showsAddParticlesSettings = true
  @AppStorage("ARMSX2iOSDynamicBackgroundsExpanded")
  private var showsBackgroundSettings = false
  @AppStorage("ARMSX2iOSDynamicMultiColorExpanded")
  private var showsMultiColorSettings = false
  @AppStorage("ARMSX2iOSDynamicFaceButtonsExpanded")
  private var showsFaceButtonSettings = false
  @AppStorage("ARMSX2iOSDynamicARMSX2LogoExpanded")
  private var showsARMSX2LogoSettings = true
  private let defaults = DynamicParticleSettings()

  var body: some View {
    particleSection
      .onAppear {
        showsBackgroundSettings = true
        migrateLegacyLogoStyleIfNeeded()
      }
      .onChange(of: dynamicBackground) { _, _ in
        showsBackgroundSettings = true
      }
  }

  private var particleSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      dynamicSettingsGroup(
        "Add particles",
        isExpanded: $showsAddParticlesSettings
      ) {
        addParticlesSettings
      }

      dynamicSettingsGroup(
        "Dynamic Backgrounds Settings",
        isExpanded: $showsBackgroundSettings
      ) {
        DynamicBackgroundSettingsControls(
          particleSettings: $particleSettings,
          isPlayStation3XMBPresetExplicit: $isPlayStation3XMBPresetExplicit,
          dynamicBackground: dynamicBackground
        )
      }

      dynamicSettingsGroup(
        "Multi-color animation",
        isExpanded: $showsMultiColorSettings
      ) {
        multiColorAnimationSettings
      }

      dynamicSettingsGroup(
        "Add face buttons",
        isExpanded: $showsFaceButtonSettings
      ) {
        faceButtonSettings
      }

      dynamicSettingsGroup(
        "Add Logo",
        isExpanded: $showsARMSX2LogoSettings
      ) {
        armsx2LogoSettings
      }
      .disabled(!logoStore.hasLogo)
      .opacity(logoStore.hasLogo ? 1 : 0.48)


      particleSlider(
        "See background preview before applying",
        value: $particleSettings.backgroundPreviewBeforeApplyingDuration,
        range: 0.0...3.0,
        step: 0.1,
        formattedValue: String(
          format: "%.1fs",
          particleSettings.backgroundPreviewBeforeApplyingDuration
        )
      )
      .dynamicSettingsCard()

      particleSlider(
        "See background preview after applying",
        value: $particleSettings.backgroundPreviewAfterApplyingDuration,
        range: 0.0...3.0,
        step: 0.1,
        formattedValue: String(
          format: "%.1fs",
          particleSettings.backgroundPreviewAfterApplyingDuration
        )
      )
      .dynamicSettingsCard()

      ConfirmedSettingsResetButton(
        "Reset Dynamic Background Settings",
        confirmationTitle: "Reset Dynamic Background Settings?",
        confirmationMessage: "This restores every dynamic background control and selected palette to its default value.",
        completionMessage: "Defaults Restored",
        controllerTargetID: "dynamic-background.reset-all"
      ) {
        resetAllSettingsAndPalettes()
      }
      .font(.subheadline.weight(.bold))
      .frame(maxWidth: .infinity)
      .frame(height: 44)
      .buttonStyle(.plain)
      .uiCriticalForegroundStyle()
      .glassSurface(
        tint: Color.red.opacity(0.12),
        interactive: true,
        cornerRadius: 14
      )
      .accessibilityHint("Resets all dynamic settings and selected palettes")
    }
  }

  private var addParticlesSettings: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Toggle("Enable particles", isOn: $particleSettings.isEnabled)
        DynamicSettingsResetButton(title: "Add particles") {
          particleSettings.isEnabled = defaults.isEnabled
        }
      }

      HStack {
        Picker("Particle style", selection: $particleSettings.style) {
          ForEach(DynamicParticleStyle.allCases.filter { $0 != .armsx2BouncingLogo }) { style in
            Text(style.title).tag(style)
          }
        }
        .pickerStyle(.menu)
        DynamicSettingsResetButton(title: "Particle style") {
          particleSettings.style = defaults.style
        }
      }

      particleSlider(
        "Amount",
        value: $particleSettings.amount,
        range: 0.2...3.0,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.amount * 100))%"
      )
      particleSlider(
        "Speed",
        value: $particleSettings.speed,
        range: 0.1...3.0,
        step: 0.1,
        formattedValue: String(format: "%.1fx", particleSettings.speed)
      )
      particleSlider(
        "Particle Opacity",
        value: $particleSettings.opacity,
        range: 0.05...1.0,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.opacity * 100))%"
      )
      particleSlider(
        "Direction of speed",
        value: $particleSettings.speedDirection,
        range: -1.0...1.0,
        step: 0.01,
        formattedValue: String(
          format: "%+.0f%%",
          particleSettings.speedDirection * 100
        )
      )
      particleSlider(
        "Dispersion",
        value: $particleSettings.dispersion,
        range: 0.25...1.8,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.dispersion * 100))%"
      )
      particleSlider(
        "Y-axis spread",
        value: $particleSettings.verticalSpread,
        range: 0.25...3.0,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.verticalSpread * 100))%"
      )
      particleSlider(
        "Y-axis density",
        value: $particleSettings.verticalDensity,
        range: 0.25...3.0,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.verticalDensity * 100))%"
      )
      particleSlider(
        "Outer top/bottom dispersion",
        value: $particleSettings.outerDispersion,
        range: 0.0...2.5,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.outerDispersion * 100))%"
      )
      particleSlider(
        "Vertical level",
        value: $particleSettings.verticalLevel,
        range: 0.18...0.82,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.verticalLevel * 100))%"
      )
      particleSlider(
        "Size",
        value: $particleSettings.size,
        range: 0.4...2.4,
        step: 0.1,
        formattedValue: String(format: "%.1fx", particleSettings.size)
      )
      particleSlider(
        "Brightness",
        value: $particleSettings.brightness,
        range: 0.15...1.35,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.brightness * 100))%"
      )
      particleSlider(
        "Depth variation",
        value: $particleSettings.depthVariation,
        range: 0.0...1.5,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.depthVariation * 100))%"
      )
      particleSlider(
        "Twinkle",
        value: $particleSettings.twinkle,
        range: 0.0...1.5,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.twinkle * 100))%"
      )
      particleSlider(
        "Drift",
        value: $particleSettings.drift,
        range: 0.0...1.5,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.drift * 100))%"
      )

    }
  }

  private var multiColorAnimationSettings: some View {
    VStack(alignment: .leading, spacing: 12) {
      particleSlider(
        "Color speed",
        value: $particleSettings.multiColorAnimationSpeed,
        range: 0.1...4.0,
        step: 0.1,
        formattedValue: String(format: "%.1fx", particleSettings.multiColorAnimationSpeed)
      )
      particleSlider(
        "Color smoothness",
        value: $particleSettings.multiColorAnimationSmoothness,
        range: 0.0...1.0,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.multiColorAnimationSmoothness * 100))%"
      )
      particleSlider(
        "Color wave",
        value: $particleSettings.multiColorAnimationSpread,
        range: 0.0...1.2,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.multiColorAnimationSpread * 100))%"
      )
    }
  }

  private var faceButtonSettings: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Toggle("Enable face buttons", isOn: $particleSettings.faceButtonsEnabled)
        DynamicSettingsResetButton(title: "Add Face Buttons") {
          particleSettings.faceButtonsEnabled =
            defaults.faceButtonsEnabled
        }
      }

      particleSlider(
        "Face Buttons amount",
        value: $particleSettings.faceButtonAmount,
        range: 0.15...2.5,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.faceButtonAmount * 100))%"
      )
      particleSlider(
        "Face Buttons speed",
        value: $particleSettings.faceButtonSpeed,
        range: 0.05...1.25,
        step: 0.01,
        formattedValue: String(format: "%.2fx", particleSettings.faceButtonSpeed)
      )
      particleSlider(
        "Face Buttons dispersion",
        value: $particleSettings.faceButtonDispersion,
        range: 0.25...2.2,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.faceButtonDispersion * 100))%"
      )
      particleSlider(
        "Face Buttons size",
        value: $particleSettings.faceButtonSize,
        range: 0.35...2.4,
        step: 0.1,
        formattedValue: String(format: "%.1fx", particleSettings.faceButtonSize)
      )
      particleSlider(
        "Face Buttons opacity",
        value: $particleSettings.faceButtonOpacity,
        range: 0.1...1.4,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.faceButtonOpacity * 100))%"
      )
      particleSlider(
        "Face Buttons rotation",
        value: $particleSettings.faceButtonRotation,
        range: 0.0...2.0,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.faceButtonRotation * 100))%"
      )
      particleSlider(
        "Face Buttons pulse",
        value: $particleSettings.faceButtonPulse,
        range: 0.0...1.6,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.faceButtonPulse * 100))%"
      )
    }
  }

  private var armsx2LogoSettings: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Toggle("Show Logo", isOn: logoEnabledBinding)
        DynamicSettingsResetButton(title: "Add Logo") {
          particleSettings.isARMSX2LogoEnabled = false
        }
      }

      Group {
        particleSlider(
          "Logo speed",
          value: logoSpeedBinding,
          range: 0.15...1.5,
          step: 0.05,
          formattedValue: String(format: "%.2fx", particleSettings.resolvedARMSX2LogoSpeed),
          resetValue: 0.55
        )
        particleSlider(
          "Logo size",
          value: logoSizeBinding,
          range: 0.5...1.8,
          step: 0.05,
          formattedValue: "\(Int(particleSettings.resolvedARMSX2LogoSize * 100))%",
          resetValue: 1.0
        )
        particleSlider(
          "Logo opacity",
          value: logoOpacityBinding,
          range: 0.0...1.0,
          step: 0.01,
          formattedValue: "\(Int(particleSettings.resolvedARMSX2LogoOpacity * 100))%",
          resetValue: 0.70
        )
        particleSlider(
          "Logo Orbs opacity",
          value: logoOrbOpacityBinding,
          range: 0.05...1.0,
          step: 0.01,
          formattedValue: "\(Int(particleSettings.resolvedARMSX2LogoOrbOpacity * 100))%",
          resetValue: 0.72
        )
      }
      .disabled(!particleSettings.isARMSX2LogoEnabled)
      .opacity(particleSettings.isARMSX2LogoEnabled ? 1 : 0.58)
    }
  }

  private var logoEnabledBinding: Binding<Bool> {
    Binding(
      get: { particleSettings.isARMSX2LogoEnabled },
      set: { particleSettings.isARMSX2LogoEnabled = $0 }
    )
  }

  private var logoSpeedBinding: Binding<Double> {
    Binding(
      get: { particleSettings.resolvedARMSX2LogoSpeed },
      set: { particleSettings.resolvedARMSX2LogoSpeed = $0 }
    )
  }

  private var logoSizeBinding: Binding<Double> {
    Binding(
      get: { particleSettings.resolvedARMSX2LogoSize },
      set: { particleSettings.resolvedARMSX2LogoSize = $0 }
    )
  }

  private var logoOpacityBinding: Binding<Double> {
    Binding(
      get: { particleSettings.resolvedARMSX2LogoOpacity },
      set: { particleSettings.resolvedARMSX2LogoOpacity = $0 }
    )
  }

  private var logoOrbOpacityBinding: Binding<Double> {
    Binding(
      get: { particleSettings.resolvedARMSX2LogoOrbOpacity },
      set: { particleSettings.resolvedARMSX2LogoOrbOpacity = $0 }
    )
  }

  private func migrateLegacyLogoStyleIfNeeded() {
    guard particleSettings.style == .armsx2BouncingLogo else { return }
    particleSettings.armsx2LogoEnabled = true
    particleSettings.style = .xmb3
  }

  private func dynamicSettingsGroup<Content: View>(
    _ title: String,
    isExpanded: Binding<Bool>,
    @ViewBuilder content: @escaping () -> Content
  ) -> some View {
    DisclosureGroup(isExpanded: isExpanded) {
      VStack(alignment: .leading, spacing: 12) {
        content()
      }
      .padding(.top, 10)
    } label: {
      Text(title)
        .font(.headline)
        .foregroundStyle(.white)
    }
    .dynamicSettingsCard()
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
      default: resetValue ?? particleResetValue(for: title),
      settings: SettingsStore.shared
    )
  }

  private func particleResetValue(for title: String) -> Double? {
    switch title {
    case "Amount": return defaults.amount
    case "Speed": return defaults.speed
    case "Direction of speed": return defaults.speedDirection
    case "Dispersion": return defaults.dispersion
    case "Y-axis spread": return defaults.verticalSpread
    case "Y-axis density": return defaults.verticalDensity
    case "Outer top/bottom dispersion": return defaults.outerDispersion
    case "Vertical level": return defaults.verticalLevel
    case "Size": return defaults.size
    case "Brightness": return defaults.brightness
    case "Particle opacity": return defaults.opacity
    case "Depth variation": return defaults.depthVariation
    case "Twinkle": return defaults.twinkle
    case "Drift": return defaults.drift
    case "See background preview before applying":
      return defaults.backgroundPreviewBeforeApplyingDuration
    case "See background preview after applying":
      return defaults.backgroundPreviewAfterApplyingDuration
    case "Motion speed": return defaults.backgrounds.multicolorAmbientSpeed
    case "FPS": return defaults.backgrounds.playStation2MenuFramesPerSecond
    case "Orbit angle": return defaults.backgrounds.playStation2MenuOrbitDegrees
    case "Yaw speed": return defaults.backgrounds.playStation2MenuYawSpeed
    case "Roll speed": return defaults.backgrounds.playStation2MenuRollSpeed
    case "Color speed": return defaults.multiColorAnimationSpeed
    case "Color smoothness": return defaults.multiColorAnimationSmoothness
    case "Color wave": return defaults.multiColorAnimationSpread
    case "Face Buttons amount": return defaults.faceButtonAmount
    case "Face Buttons speed": return defaults.faceButtonSpeed
    case "Face Buttons dispersion": return defaults.faceButtonDispersion
    case "Face Buttons size": return defaults.faceButtonSize
    case "Face Buttons opacity": return defaults.faceButtonOpacity
    case "Face Buttons rotation": return defaults.faceButtonRotation
    case "Face Buttons pulse": return defaults.faceButtonPulse
    default: return nil
    }
  }
}
