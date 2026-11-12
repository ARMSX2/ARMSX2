// DynamicSettings.swift — Dynamic background models and advanced controls
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UIKit

// MARK: - DynamicBackgroundSettings

enum DynamicParticleStyle: String, CaseIterable, Identifiable, Codable {
  case xmb3
  case xmbMart
  case armsx2BouncingLogo
  case ps1Dust
  case ps4Glow
  case ps5Drift
  case mixed

  var id: String { rawValue }

  var title: String {
    switch self {
    case .xmb3:
      return "Glass Sparkles"
    case .xmbMart:
      return "Glass Ribbons by Mart"
    case .armsx2BouncingLogo:
      return "ARMSX2 Bouncing Logo"
    case .ps1Dust:
      return "Pixel Dust"
    case .ps4Glow:
      return "Soft Glow"
    case .ps5Drift:
      return "Ribbon Drift"
    case .mixed:
      return "Mixed"
    }
  }
}

struct DynamicBackgroundMotionSettings: Equatable, Codable {
  var multicolorAmbientSpeed = 1.0
  var lightSpeedMotionSpeed = 1.0
  var spatialRetroSpeed = 1.0
  var towersOrbsSpeed = 1.0
  var playStation2MenuSceneSpeed = 1.0
  var playStation2MenuFramesPerSecond = 15.0
  var playStation2MenuYawSpeed = 2.0
  var playStation2MenuRollSpeed = 6.0
  var playStation2MenuOrbitDegrees = 25.0
  var playStation2MenuColorsTowers = false
  var faceButtonsSpeed = 1.0
  var faceButtonsShowsLightBands = true
  var playStationPortableBlurSpeed = 1.0
  var playStation3SplinesSpeed = 1.0
  var playStation4ParticlesSpeed = 1.0
  var playStation4WavesSpeed = 1.0
  var playStationRibbonsSpeed = 1.0
  var playStationPortableBlur = PlayStationPortableBlurSettings()
  var playStation3Splines = PlayStation3SplinesSettings()
  var playStation4Particles = PlayStation4ParticlesSettings()
  var playStation4Waves = PlayStation4WavesSettings()
  var playStationRibbons = PlayStationRibbonsSettings()
}

struct DynamicParticleSettings: Equatable, Codable {
  var isEnabled = false
  var style: DynamicParticleStyle = .xmb3
  var amount = 1.0
  var speed = 1.0
  var speedDirection = 0.5
  var dispersion = 1.0
  var verticalSpread = 1.0
  var verticalDensity = 1.0
  var outerDispersion = 0.0
  var size = 1.0
  var brightness = 0.7
  var opacity = 1.0
  var depthVariation = 0.0
  var twinkle = 0.65
  var drift = 0.55
  var verticalLevel = 0.5
  var backgroundPreviewBeforeApplyingDuration = 0.3
  var backgroundPreviewAfterApplyingDuration = 0.7
  var disablesDarkPaletteEffects = false
  var paletteDarkEffectIntensity = 1.0
  var sharedPaletteGradientTilt = 0.0
  var sharedPaletteGradientOffsetX = 0.0
  var sharedPaletteGradientOffsetY = 0.0
  var sharedPaletteGradientWidth = 1.0
  var sharedPaletteGradientCurvature = 0.0
  var multiColorAnimationSpeed = 1.0
  var multiColorAnimationSmoothness = 0.85
  var multiColorAnimationSpread = 0.35
  var faceButtonsEnabled = true
  var faceButtonAmount = 0.5
  var faceButtonSpeed = 0.33
  var faceButtonDispersion = 1.0
  var faceButtonSize = 1.3
  var faceButtonOpacity = 0.86
  var faceButtonRotation = 1.0
  var faceButtonPulse = 0.75
  // Optional storage keeps older saved themes decodable. The logo is an
  // independent overlay now, rather than a mutually exclusive particle style.
  var armsx2LogoEnabled: Bool?
  var armsx2LogoSpeed: Double?
  var armsx2LogoSize: Double?
  var armsx2LogoOpacity: Double?
  var armsx2LogoOrbsEnabled: Bool?
  var armsx2LogoOrbOpacity: Double?
  var playStation3XMB = PlayStation3XMBSettings()
  var backgrounds = DynamicBackgroundMotionSettings()

  var isARMSX2LogoEnabled: Bool {
    get { armsx2LogoEnabled ?? (style == .armsx2BouncingLogo) }
    set {
      armsx2LogoEnabled = newValue
      if style == .armsx2BouncingLogo { style = .xmb3 }
    }
  }

  var resolvedARMSX2LogoSpeed: Double {
    get { armsx2LogoSpeed ?? 0.55 }
    set { armsx2LogoSpeed = newValue }
  }

  var resolvedARMSX2LogoSize: Double {
    get { armsx2LogoSize ?? 1.0 }
    set { armsx2LogoSize = newValue }
  }

  var resolvedARMSX2LogoOpacity: Double {
    get { armsx2LogoOpacity ?? 0.70 }
    set { armsx2LogoOpacity = newValue }
  }

  var showsARMSX2LogoOrbs: Bool {
    get { armsx2LogoOrbsEnabled ?? true }
    set { armsx2LogoOrbsEnabled = newValue }
  }

  var resolvedARMSX2LogoOrbOpacity: Double {
    get { armsx2LogoOrbOpacity ?? 0.72 }
    set { armsx2LogoOrbOpacity = newValue }
  }
}

struct ThemeMultiColorSelection: Equatable, Codable {
  var isEnabled = false
  var animates = false
  var palettes: [ThemePalette] = [.blue, .cyan, .pink, .gold]

  private var activePalettes: [ThemePalette] {
    palettes.isEmpty ? [.blue] : palettes
  }

  // Enables multi-color mode and gives it a valid fallback palette.
  mutating func setEnabled(_ enabled: Bool, fallback: ThemePalette) {
    isEnabled = enabled
    if enabled {
      palettes = fallback.relatedMultiColorPalettes
    }
  }

  // Adds or removes a palette while keeping at least one multi-color selected.
  mutating func toggle(_ palette: ThemePalette, fallback: ThemePalette) {
    if let index = palettes.firstIndex(of: palette) {
      if palettes.count > 1 {
        palettes.remove(at: index)
      }
    } else {
      palettes.append(palette)
    }

    if palettes.isEmpty {
      palettes = [fallback]
    }
  }

  // Returns the numbered selection badge for a palette in multi-color mode.
  func selectionNumber(for palette: ThemePalette) -> Int? {
    guard let index = palettes.firstIndex(of: palette) else { return nil }
    return index + 1
  }

  // Resolves colors from the selected palette list with optional soft animation.
  func color(
    index: Int,
    time: TimeInterval,
    settings: DynamicParticleSettings
  ) -> Color {
    let selectedPalettes = activePalettes

    guard animates, selectedPalettes.count > 1 else {
      return selectedPalettes[index % selectedPalettes.count].animatedColor(
        index: index,
        time: 0
      )
    }

    let progress =
      time * max(0.02, settings.multiColorAnimationSpeed) / 8
      + Double(index) * settings.multiColorAnimationSpread
    let step = Int(floor(progress))
    let fraction = progress - floor(progress)
    let fromPalette = selectedPalettes[positiveModulo(index + step, selectedPalettes.count)]
    let toPalette = selectedPalettes[positiveModulo(index + step + 1, selectedPalettes.count)]
    let blend = easedBlend(
      fraction,
      smoothness: settings.multiColorAnimationSmoothness
    )

    return blendedColor(
      fromPalette.animatedColor(
        index: index,
        time: time * 0.12
      ),
      toPalette.animatedColor(
        index: index,
        time: time * 0.12
      ),
      amount: blend
    )
  }

  private func easedBlend(_ fraction: Double, smoothness: Double) -> Double {
    let clampedSmoothness = min(1, max(0, smoothness))
    let smooth = fraction * fraction * (3 - 2 * fraction)
    return fraction * (1 - clampedSmoothness) + smooth * clampedSmoothness
  }

  private func blendedColor(_ first: Color, _ second: Color, amount: Double) -> Color {
    let firstColor = UIColor(first)
    let secondColor = UIColor(second)
    var firstRed: CGFloat = 0
    var firstGreen: CGFloat = 0
    var firstBlue: CGFloat = 0
    var firstAlpha: CGFloat = 0
    var secondRed: CGFloat = 0
    var secondGreen: CGFloat = 0
    var secondBlue: CGFloat = 0
    var secondAlpha: CGFloat = 0

    guard
      firstColor.getRed(
        &firstRed,
        green: &firstGreen,
        blue: &firstBlue,
        alpha: &firstAlpha
      ),
      secondColor.getRed(
        &secondRed,
        green: &secondGreen,
        blue: &secondBlue,
        alpha: &secondAlpha
      )
    else {
      return amount < 0.5 ? first : second
    }

    let blend = min(1, max(0, amount))
    return Color(
      red: Double(firstRed + (secondRed - firstRed) * blend),
      green: Double(firstGreen + (secondGreen - firstGreen) * blend),
      blue: Double(firstBlue + (secondBlue - firstBlue) * blend),
      opacity: Double(firstAlpha + (secondAlpha - firstAlpha) * blend)
    )
  }

  private func positiveModulo(_ value: Int, _ divisor: Int) -> Int {
    ((value % divisor) + divisor) % divisor
  }
}

// MARK: - PlayStationBackgroundSettings

struct PlayStation3SplinesSettings: Equatable, Codable {
  var showsSplines = true
  var showsParticles = true
  var strandCount = 15.0
  var verticalPosition = 0.66
  var strandSpacing = 1.0
  var waveAmplitude = 1.0
  var detailAmplitude = 1.0
  var phaseSpread = 1.0
  var glowOpacity = 1.0
  var coreOpacity = 1.0
  var glowWidth = 1.0
  var coreWidth = 1.0
  var glowBlur = 1.0
  var particleCount = 150.0
  var particleSpeed = 1.0
  var particleSpread = 1.0
  var particleSize = 1.0
  var particleOpacity = 1.0
  var particleTwinkle = 1.0
  var backgroundIntensity = 1.0
  var vignetteIntensity = 1.0
}

struct PlayStation4ParticlesSettings: Equatable, Codable {
  var showsParticles = true
  var showsWaves = true
  var showsWaveGlow = true
  var showsWaveCores = true
  var particleCount = 150.0
  var particleSpeed = 1.0
  var particleVerticalPosition = 0.48
  var particleSpread = 1.0
  var particleSize = 1.0
  var particleOpacity = 1.0
  var particleTwinkle = 1.0
  var waveCount = 8.0
  var waveVerticalPosition = 0.48
  var waveSpacing = 1.0
  var waveAmplitude = 1.0
  var waveCurvature = 1.0
  var phaseSpread = 1.0
  var glowBlur = 18.0
  var glowWidth = 1.0
  var glowOpacity = 1.0
  var coreWidth = 1.0
  var coreOpacity = 1.0
  var backgroundIntensity = 1.0
  var vignetteIntensity = 1.0
}

struct PlayStation4WavesSettings: Equatable, Codable {
  var showsAmbientGlows = true
  var showsParticles = true
  var showsWaves = true
  var showsWaveGlow = true
  var showsWaveCores = true
  var particleCount = 38.0
  var particleSpeed = 1.0
  var particleSpread = 1.0
  var particleSize = 1.0
  var particleOpacity = 1.0
  var waveCount = 8.0
  var waveVerticalPosition = 0.48
  var waveSpacing = 1.0
  var waveAmplitude = 1.0
  var waveCurvature = 1.0
  var phaseSpread = 1.0
  var glowBlur = 18.0
  var glowWidth = 1.0
  var glowOpacity = 1.0
  var coreWidth = 1.0
  var coreOpacity = 1.0
  var ambientGlowScale = 1.0
  var ambientGlowIntensity = 1.0
  var vignetteIntensity = 1.0
}

struct PlayStationRibbonsSettings: Equatable, Codable {
  var showsPanels = true
  var showsPanelGlow = true
  var showsPanelEdges = true
  var showsParticles = true
  var panelCount = 6.0
  var panelVerticalPosition = 0.14
  var panelSpacing = 1.0
  var panelAmplitude = 1.0
  var panelThickness = 1.0
  var panelCurvature = 1.0
  var phaseSpread = 1.0
  var panelFillOpacity = 1.0
  var panelGlowOpacity = 1.0
  var panelGlowBlur = 1.0
  var panelEdgeOpacity = 1.0
  var panelEdgeWidth = 1.0
  var particleCount = 86.0
  var particleSpeed = 1.0
  var particleDrift = 1.0
  var particleVerticalSpread = 1.0
  var particleSize = 1.0
  var particleOpacity = 1.0
  var ambientGlowScale = 1.0
  var ambientGlowIntensity = 1.0
  var vignetteIntensity = 1.0
}

struct PlayStationPortableBlurSettings: Equatable, Codable {
  var showsRibbons = true
  var showsGlow = true
  var showsCores = true
  var alternatesDirection = true
  var ribbonCount = 7.0
  var verticalOffset = 0.0
  var waveAmplitude = 1.0
  var detailAmplitude = 1.0
  var phaseSpread = 1.0
  var broadWidth = 1.0
  var glowBlur = 1.0
  var glowOpacity = 1.0
  var coreWidth = 1.0
  var coreBlur = 1.0
  var coreOpacity = 1.0
  var backgroundIntensity = 1.0
  var ambientGlowScale = 1.0
  var ambientGlowIntensity = 1.0
  var vignetteIntensity = 0.0
}

// MARK: - PlayStation3XMBSettings

enum PlayStation3XMBGradientPreset: String, CaseIterable, Identifiable, Codable {
  case theme
  case original
  case januaryDay = "01_day"
  case januaryNight = "01_night"
  case februaryDay = "02_day"
  case februaryNight = "02_night"
  case marchDay = "03_day"
  case marchNight = "03_night"
  case aprilDay = "04_day"
  case aprilNight = "04_night"
  case mayDay = "05_day"
  case mayNight = "05_night"
  case juneDay = "06_day"
  case juneNight = "06_night"
  case julyDay = "07_day"
  case julyNight = "07_night"
  case augustDay = "08_day"
  case augustNight = "08_night"
  case septemberDay = "09_day"
  case septemberNight = "09_night"
  case octoberDay = "10_day"
  case octoberNight = "10_night"
  case novemberDay = "11_day"
  case novemberNight = "11_night"
  case decemberDay = "12_day"
  case decemberNight = "12_night"

  var id: String { rawValue }

  var title: String {
    switch self {
    case .theme:
      return "Shared Themes + Ribbons"
    case .original:
      return "Original (RGB Sliders)"
    case .januaryDay: return "01 (Day)"
    case .januaryNight: return "01 (Night)"
    case .februaryDay: return "02 (Day)"
    case .februaryNight: return "02 (Night)"
    case .marchDay: return "03 (Day)"
    case .marchNight: return "03 (Night)"
    case .aprilDay: return "04 (Day)"
    case .aprilNight: return "04 (Night)"
    case .mayDay: return "05 (Day)"
    case .mayNight: return "05 (Night)"
    case .juneDay: return "06 (Day)"
    case .juneNight: return "06 (Night)"
    case .julyDay: return "07 (Day)"
    case .julyNight: return "07 (Night)"
    case .augustDay: return "08 (Day)"
    case .augustNight: return "08 (Night)"
    case .septemberDay: return "09 (Day)"
    case .septemberNight: return "09 (Night)"
    case .octoberDay: return "10 (Day)"
    case .octoberNight: return "10 (Night)"
    case .novemberDay: return "11 (Day)"
    case .novemberNight: return "11 (Night)"
    case .decemberDay: return "12 (Day)"
    case .decemberNight: return "12 (Night)"
    }
  }
}

struct PlayStation3XMBSettings: Equatable, Codable {
  var gradientPreset: PlayStation3XMBGradientPreset = .theme

  var colorR = 37.0
  var colorG = 89.0
  var colorB = 179.0
  var gradientTopMul = 0.09
  var gradientBotMul = 0.62

  var particleCount = 600.0
  var particleOpacity = 0.75
  var particleSizeBase = 10.0
  var particleSizeVariance = 1.5
  var particleFlowSpeed = 0.18
  var particleVerticalSpread = 1.0
  var particleVerticalDensity = 1.0
  var particleOuterDispersion = 0.0
  var particleDepthVariation = 0.0
  var particlesFollowWaveY = true

  var flowSpeed = 0.18
  var tension = 0.12
  var damping = 0.0001
  var length = 0.306001
  var spacing = 407.658
  var timeStep = 1.0

  var bandAmplitude = 0.2
  var bandSecondaryFrequency = 7.0
  var bandSecondaryAmplitude = 0.025
  var travelSpeed1 = 0.25
  var travelAmplitude1 = 0.014
  var travelSpeed2 = 0.15
  var travelAmplitude2 = 0.008

  var perturbation = 0.0998587
  var perturbationScale = 0.07
  var waveCosineAmplitude = 0.09
  var waveBias = -0.1
  var waveHeightScale = 0.5
  var waveSoftClip = 0.22

  var reversePipelineBlend = 0.45
  var reverseDescriptorStrength = 0.7
  var reverseSyntheticDescriptorSeed = 1337.0
  var reverseSyntheticDescriptorMotion = 0.65
  var reverseKernelGain = 0.04
  var reverseNormalizeGain = 0.08
  var reverseKernelPhaseStep = 0.45
  var reverseIndexJitter = 0.006
  var reverseTemporalSmooth = 0.84

  var fresnelPower = 4.0
  var fresnelScale = 0.5
  var opacity = 0.7
  var brightness = 0.35
  var zDetailScale = 0.08

  var ffdScale1X = 5.67726
  var ffdScale1Y = 1.00077
  var ffdScale1Z = 1.0
  var ffdScale2X = 2.82755
  var ffdScale2Y = 1.27579
  var ffdScale2Z = 2.88782
  var ffdOffsetX = 0.0
  var ffdOffsetY = -0.469999
  var ffdOffsetZ = 0.0
  var ffdYAmplitude = 0.05
  var ffdZAmplitude = 0.06
}

// MARK: - DynamicAppearancePreferences

struct DynamicAppearancePreferences: Codable, Equatable {
  private static let storageKey = "ARMSX2iOSDynamicAppearancePreferences"
  private static let currentVersion = 1

  var version = currentVersion
  var dynamicBackground: DynamicBackgroundStyle
  var sharedPalette: ThemePalette
  var sharedCustomColor: SavedPaletteColor?
  var sharedMultiColor: ThemeMultiColorSelection
  var ribbonPalette: ThemePalette
  var ribbonCustomColor: SavedPaletteColor?
  var ribbonMultiColor: ThemeMultiColorSelection
  var particleSettings: DynamicParticleSettings
  var hasSelectedPlayStation3XMBByMart: Bool
  var isPlayStation3XMBPresetExplicit: Bool

  static let standard = DynamicAppearancePreferences(
    dynamicBackground: .playStation3XMBByMart,
    sharedPalette: .blue,
    sharedCustomColor: nil,
    sharedMultiColor: ThemeMultiColorSelection(),
    ribbonPalette: .cyan,
    ribbonCustomColor: nil,
    ribbonMultiColor: ThemeMultiColorSelection(),
    particleSettings: DynamicParticleSettings(),
    hasSelectedPlayStation3XMBByMart: false,
    isPlayStation3XMBPresetExplicit: false
  )

  static func load(from userDefaults: UserDefaults = .standard) -> Self? {
    guard
      let data = userDefaults.data(forKey: storageKey),
      let preferences = DynamicBackgroundCoding.decode(Self.self, from: data),
      preferences.version == currentVersion
    else {
      return nil
    }

    return preferences
  }

  func save(to userDefaults: UserDefaults = .standard) {
    guard let data = DynamicBackgroundCoding.encode(self) else { return }
    userDefaults.set(data, forKey: Self.storageKey)
  }
}

// MARK: - PlayStationBackgroundSettingsControls

struct BackgroundSettingsResetHeader: View {
  let action: () -> Void

  var body: some View {
    HStack {
      Text("Advanced controls")
        .font(.caption.weight(.semibold))
        .foregroundStyle(.white.opacity(0.62))
      Spacer()
      Button(action: action) {
        Label("Reset all", systemImage: "arrow.counterclockwise")
          .font(.caption.weight(.semibold))
      }
      .buttonStyle(.borderless)
    }
  }
}

struct DynamicSettingsResetButton: View {
  let title: String
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      Image(systemName: "arrow.counterclockwise")
        .font(.caption.weight(.semibold))
        .frame(width: 28, height: 28)
        .contentShape(Rectangle())
    }
    .buttonStyle(.borderless)
    .foregroundStyle(.white.opacity(0.7))
    .accessibilityLabel("Reset \(title)")
  }
}

struct BackgroundControlSection<Content: View>: View {
  let title: String
  let content: Content

  init(_ title: String, @ViewBuilder content: () -> Content) {
    self.title = title
    self.content = content()
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      Label(title, systemImage: sectionIcon)
        .font(.caption.weight(.bold))
        .foregroundStyle(.white.opacity(0.78))
      content
    }
    .padding(.vertical, 4)
  }

  private var sectionIcon: String {
    switch title {
    case "Visibility": return "eye"
    case "Native particles": return "sparkles"
    case "Environment": return "circle.lefthalf.filled"
    default: return title.contains("material") ? "slider.horizontal.3" : "waveform.path"
    }
  }
}

struct BackgroundControlToggle: View {
  let title: String
  @Binding var value: Bool
  let defaultValue: Bool

  var body: some View {
    HStack {
      Toggle(title, isOn: $value)
      DynamicSettingsResetButton(title: title) {
        value = defaultValue
      }
    }
    .font(.caption.weight(.semibold))
  }
}

/// No stops: these are live preview art controls where every value in the range is a legitimate
/// one. They do snap to the precision they print, though, because a row that reads 50% while
/// storing 0.4973 is lying to the reset arrow as well as to the reader.
@MainActor
func controlSlider(
  _ title: String,
  value: Binding<Double>,
  range: ClosedRange<Double>,
  defaultValue: Double,
  format: NumberFormat
) -> some View {
  NumberRow(
    title,
    value: value,
    in: range,
    format: format,
    step: format.displayStep,
    default: defaultValue,
    settings: SettingsStore.shared
  )
}

extension View {
  func dynamicSettingsCard() -> some View {
    padding(14)
      .background {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
          .fill(.gray.opacity(0.16))
      }
      .overlay {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
          .stroke(.white.opacity(0.09), lineWidth: 0.7)
      }
  }
}

enum DynamicSettingsExpansion {
  static func binding(
    storage: Binding<String>,
    identifier: String
  ) -> Binding<Bool> {
    Binding(
      get: { identifierSet(in: storage.wrappedValue).contains(identifier) },
      set: { isExpanded in
        var identifiers = identifierSet(in: storage.wrappedValue)
        if isExpanded {
          identifiers.insert(identifier)
        } else {
          identifiers.remove(identifier)
        }
        storage.wrappedValue = identifiers.sorted().joined(separator: "|")
      }
    )
  }

  private static func identifierSet(in storage: String) -> Set<String> {
    Set(storage.split(separator: "|").map(String.init))
  }
}

@MainActor
func controlToggle(
  _ title: String,
  value: Binding<Bool>,
  defaultValue: Bool
) -> some View {
  BackgroundControlToggle(
    title: title,
    value: value,
    defaultValue: defaultValue
  )
}

