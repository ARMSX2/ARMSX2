// DynamicBackgrounds.swift — SwiftUI dynamic library backgrounds
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - DynamicBackgroundStyle

enum DynamicBackgroundStyle: String, CaseIterable, Identifiable, Codable {
  case multicolorAmbient = "ambient"
  case lightSpeed = "space"
  case spatialRetro = "ps1"
  case towersOrbs = "towers"
  case playStation2Menu = "towersSky"
  case playStation3XMBByMart = "xmbMart"
  case faceButtons
  case playStationPortableBlur = "xmb"
  case playStation3Splines = "xmb3"
  case playStation4Particles = "xmb4"
  case playStation4Waves = "waves"
  case playStationRibbons = "ps5"

  init(from decoder: Decoder) throws {
    let container = try decoder.singleValueContainer()
    let rawValue = try container.decode(String.self)

    if let current = Self(rawValue: rawValue) {
      self = current
      return
    }

    // The short-lived standalone Y2K renderers were replaced by tuned themes
    // backed by the established PS2/PS3 engines. Preserve stored selections
    // across that migration instead of invalidating all appearance settings.
    switch rawValue {
    case "frutigerEco", "dorfic", "frutigerMetro", "vectorbloom":
      self = .playStation2Menu
    case "frutigerAero", "darkAero", "technozen", "mcBling", "skeuomorphicAqua":
      self = .playStation3XMBByMart
    default:
      throw DecodingError.dataCorruptedError(
        in: container,
        debugDescription: "Unknown dynamic background style: \(rawValue)"
      )
    }
  }

  func encode(to encoder: Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(rawValue)
  }

  var id: String { rawValue }

  var title: String {
    switch self {
    case .multicolorAmbient:
      return "Multicolor Ambient"
    case .lightSpeed:
      return "Light Speed"
    case .spatialRetro:
      return "Spatial Retro"
    case .towersOrbs:
      return "Towers Orbs"
    case .playStation2Menu:
      return "Crystal Towers by Henyckma"
    case .playStation3XMBByMart:
      return "Glass Ribbons by Mart"
    case .faceButtons:
      return "Face Buttons"
    case .playStationPortableBlur:
      return "Portable Blur"
    case .playStation3Splines:
      return "Ribbon Splines"
    case .playStation4Particles:
      return "Particle Field"
    case .playStation4Waves:
      return "Layered Waves"
    case .playStationRibbons:
      return "Luminous Ribbons"
    }
  }

  var systemImage: String {
    switch self {
    case .multicolorAmbient:
      return "circle.hexagongrid.fill"
    case .lightSpeed:
      return "forward.end.fill"
    case .spatialRetro:
      return "square.grid.3x3.fill"
    case .towersOrbs:
      return "circle.grid.2x2.fill"
    case .playStation2Menu:
      return "cube.fill"
    case .playStation3XMBByMart:
      return "water.waves"
    case .faceButtons:
      return "gamecontroller.fill"
    case .playStationPortableBlur:
      return "drop.fill"
    case .playStation3Splines:
      return "waveform.path"
    case .playStation4Particles:
      return "sparkles"
    case .playStation4Waves:
      return "water.waves"
    case .playStationRibbons:
      return "scribble.variable"
    }
  }

  var settingsExpansionIdentifier: String {
    // Keep the pre-rename values so saved disclosure state survives app updates.
    let legacyName: String

    switch self {
    case .multicolorAmbient: legacyName = "Orbit Ambient"
    case .lightSpeed: legacyName = "Space"
    case .spatialRetro: legacyName = "PS1 Style"
    case .towersOrbs: legacyName = "PS2 Towers"
    case .playStation2Menu: legacyName = "Towers Sky"
    case .playStation3XMBByMart: legacyName = "PlayStation 3 XMB by Mart"
    case .faceButtons: legacyName = "Face Buttons"
    case .playStationPortableBlur: legacyName = "XMB"
    case .playStation3Splines: legacyName = "XMB3"
    case .playStation4Particles: legacyName = "XMB4"
    case .playStation4Waves: legacyName = "PS4 Waves"
    case .playStationRibbons: legacyName = "PS5 Style"
    }

    return "background.\(legacyName)"
  }

  // Creates the selected animated background with shared base colors and ribbon accents.
  @MainActor
  @ViewBuilder
  func makeBackground(theme: DynamicBackgroundTheme) -> some View {
    ZStack {
      switch self {
      case .multicolorAmbient:
        MulticolorAmbientBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .lightSpeed:
        LightSpeedBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .spatialRetro:
        SpatialRetroBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .towersOrbs:
        TowersOrbsBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .playStation2Menu:
        PlayStation2MenuBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .playStation3XMBByMart:
        PlayStation3XMBByMartBackground(theme: theme)
      case .faceButtons:
        FaceButtonsBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(
            opaque: true,
            usesFaceButtonResolution: true
          )
      case .playStationPortableBlur:
        PlayStationPortableBlurBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .playStation3Splines:
        PlayStation3SplinesBackground(theme: theme, ribbonSizing: .playStation3)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .playStation4Particles:
        PlayStation4ParticlesBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .playStation4Waves:
        PlayStation4WavesBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      case .playStationRibbons:
        PlayStationRibbonsBackground(theme: theme)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)
      }

      if theme.particleSettings.isEnabled
          || theme.particleSettings.isARMSX2LogoEnabled {
        DynamicParticleOverlay(theme: theme)
          // Keep the particle/logo presentation above UIKit- and
          // Metal-backed dynamic renderers, while remaining below menu UI.
          .zIndex(2)
      }

      if self != .faceButtons && theme.particleSettings.faceButtonsEnabled {
        FaceButtonsBackground(
          theme: theme,
          showsBackdrop: false,
          showsLightBands: false
        )
        .dynamicWallpaperSwiftUIRenderSurface(
          usesFaceButtonResolution: true
        )
        .zIndex(1)
      }
    }
  }
}

