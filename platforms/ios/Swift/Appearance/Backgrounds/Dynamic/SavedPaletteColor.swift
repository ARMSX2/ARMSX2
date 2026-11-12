// SavedPaletteColor.swift — saved palette colour
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UIKit

// MARK: - SavedPaletteColor

struct SavedPaletteColor: Codable, Hashable, Identifiable {
  let hex: String
  let gradientHexes: [String]?
  let darkEffectHex: String?
  let darkEffectGradientHexes: [String]?
  let darkEffectEnabled: Bool?

  var id: String {
    ([hex] + (gradientHexes ?? []) + [darkEffectHex ?? ""]
      + (darkEffectGradientHexes ?? [])
      + [darkEffectEnabled.map(String.init) ?? ""]).joined(separator: "-")
  }

  var isGradient: Bool {
    guard let gradientHexes else { return false }
    return gradientHexes.count > 1
  }

  /// Resolves the saved solid colour without generating the extra gradient
  /// stops used by dynamic-background previews.
  var color: Color {
    Self.color(from: hex)
  }

  var gradientColors: [Color] {
    if let gradientHexes, !gradientHexes.isEmpty {
      return gradientHexes.map(Self.color(from:))
    }

    let components = rgbComponents
    return [
      Color(
        red: components.red * 0.34,
        green: components.green * 0.34,
        blue: components.blue * 0.34
      ),
      Color(
        red: components.red,
        green: components.green,
        blue: components.blue
      ),
      Color(
        red: components.red + (1 - components.red) * 0.38,
        green: components.green + (1 - components.green) * 0.38,
        blue: components.blue + (1 - components.blue) * 0.38
      ),
    ]
  }

  var darkEffectColor: Color? {
    darkEffectColor(at: 0)
  }

  var usesDarkEffect: Bool {
    darkEffectEnabled ?? true
  }

  func darkEffectColor(at progress: Double) -> Color? {
    let hexes = darkEffectGradientHexes ?? darkEffectHex.map { [$0] }
    guard let hexes, let firstHex = hexes.first else { return nil }
    guard hexes.count > 1 else { return Self.color(from: firstHex) }

    let position = min(1, max(0, progress)) * Double(hexes.count - 1)
    let lowerIndex = min(hexes.count - 1, Int(floor(position)))
    let upperIndex = min(hexes.count - 1, lowerIndex + 1)
    let amount = position - Double(lowerIndex)
    let lower = Self.rgbComponents(for: hexes[lowerIndex])
    let upper = Self.rgbComponents(for: hexes[upperIndex])
    return Color(
      red: lower.red + (upper.red - lower.red) * amount,
      green: lower.green + (upper.green - lower.green) * amount,
      blue: lower.blue + (upper.blue - lower.blue) * amount
    )
  }

  // Stores a SwiftUI color as an RGB hex value that can be saved in AppStorage.
  init?(color: Color) {
    guard let hex = Self.hexValue(for: color) else { return nil }
    self.hex = hex
    gradientHexes = nil
    darkEffectHex = nil
    darkEffectGradientHexes = nil
    darkEffectEnabled = nil
  }

  init?(
    colors: [Color],
    darkEffectColors: [Color]? = nil,
    darkEffectEnabled: Bool = true
  ) {
    let hexes = colors.compactMap(Self.hexValue(for:))
    guard hexes.count == colors.count, hexes.count > 1 else { return nil }
    let darkEffectHexes = darkEffectColors?.compactMap(Self.hexValue(for:))
    guard darkEffectColors == nil || darkEffectHexes?.count == darkEffectColors?.count else {
      return nil
    }
    hex = hexes[0]
    gradientHexes = hexes
    darkEffectHex = darkEffectHexes?.first
    darkEffectGradientHexes = darkEffectHexes
    self.darkEffectEnabled = darkEffectEnabled
  }

  // Resolves either a saved gradient stop or the generated variants of a solid color.
  func animatedColor(index: Int, time _: TimeInterval) -> Color {
    let colors = gradientColors
    return colors[index % colors.count]
  }

  private var rgbComponents: (red: Double, green: Double, blue: Double) {
    Self.rgbComponents(for: hex)
  }

  private static func color(from hex: String) -> Color {
    let components = rgbComponents(for: hex)
    return Color(
      red: components.red,
      green: components.green,
      blue: components.blue
    )
  }

  private static func rgbComponents(
    for hex: String
  ) -> (red: Double, green: Double, blue: Double) {
    let value = UInt64(hex.dropFirst(), radix: 16) ?? 0
    return (
      Double((value >> 16) & 0xFF) / 255,
      Double((value >> 8) & 0xFF) / 255,
      Double(value & 0xFF) / 255
    )
  }

  private static func hexValue(for color: Color) -> String? {
    let uiColor = UIColor(color)

    // The picker returns Display P3, which getRed reports in extended sRGB, so a saturated
    // colour can land outside 0...1 and print as bad hex. Convert to sRGB before formatting;
    // the clamp covers whatever the gamut still cannot express.
    var red: CGFloat = 0
    var green: CGFloat = 0
    var blue: CGFloat = 0
    var alpha: CGFloat = 0

    if let srgbSpace = CGColorSpace(name: CGColorSpace.sRGB),
      let converted = uiColor.cgColor.converted(
        to: srgbSpace, intent: .defaultIntent, options: nil),
      let components = converted.components, components.count >= 3
    {
      red = components[0]
      green = components[1]
      blue = components[2]
    } else if !uiColor.getRed(&red, green: &green, blue: &blue, alpha: &alpha) {
      return nil
    }

    let channel: (CGFloat) -> Int = { Int((min(1, max(0, $0)) * 255).rounded()) }
    return String(format: "#%02X%02X%02X", channel(red), channel(green), channel(blue))
  }
}
