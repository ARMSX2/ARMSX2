// DynamicEffects.swift — Shared dynamic background effects
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

@MainActor
enum ARMSX2BouncingLogoAsset {
  static var image: UIImage? { ARMSX2LogoStore.shared.image }
}

struct OrbTrailBezierSegment {
  let end: CGPoint
  let control1: CGPoint
  let control2: CGPoint
}

enum OrbTrailGeometry {
  static func segment(
    points: [CGPoint],
    index: Int,
    maximumTangentLength: CGFloat
  ) -> OrbTrailBezierSegment {
    let start = points[index]
    let end = points[index + 1]
    let startTangent = tangent(
      points: points,
      index: index,
      maximumLength: maximumTangentLength
    )
    let endTangent = tangent(
      points: points,
      index: index + 1,
      maximumLength: maximumTangentLength
    )

    return OrbTrailBezierSegment(
      end: end,
      control1: CGPoint(
        x: start.x + startTangent.dx / 3,
        y: start.y + startTangent.dy / 3
      ),
      control2: CGPoint(
        x: end.x - endTangent.dx / 3,
        y: end.y - endTangent.dy / 3
      )
    )
  }

  private static func tangent(
    points: [CGPoint],
    index: Int,
    maximumLength: CGFloat
  ) -> CGVector {
    let point = points[index]
    let previous = points[max(0, index - 1)]
    let following = points[min(points.count - 1, index + 1)]
    let raw = CGVector(
      dx: (following.x - previous.x) * 0.5,
      dy: (following.y - previous.y) * 0.5
    )
    let rawLength = hypot(raw.dx, raw.dy)
    guard rawLength > 0 else { return .zero }

    let adjacentLengths = [
      hypot(point.x - previous.x, point.y - previous.y),
      hypot(following.x - point.x, following.y - point.y),
    ].filter { $0 > 0 }
    guard let shortestAdjacent = adjacentLengths.min() else { return .zero }

    let allowedLength = min(maximumLength, shortestAdjacent * 1.25)
    let scale = min(1, allowedLength / rawLength)
    return CGVector(dx: raw.dx * scale, dy: raw.dy * scale)
  }
}

// MARK: - DynamicBackgroundEffects

struct DynamicBackgroundTheme: Equatable {
  let sharedPalette: ThemePalette
  let sharedCustomColor: SavedPaletteColor?
  let sharedMultiColor: ThemeMultiColorSelection
  let ribbonPalette: ThemePalette
  let ribbonCustomColor: SavedPaletteColor?
  let ribbonMultiColor: ThemeMultiColorSelection
  let particleSettings: DynamicParticleSettings

  var usesDynamicSharedPalette: Bool {
    if let sharedCustomColor {
      return sharedCustomColor.isGradient
    }

    if sharedMultiColor.isEnabled {
      return !sharedMultiColor.palettes.isEmpty
        && sharedMultiColor.palettes.allSatisfy {
          ThemePalette.primaryPalettes.contains($0)
        }
    }

    return ThemePalette.primaryPalettes.contains(sharedPalette)
  }

  // Resolves the base palette used for background gradients and broad color fields.
  func sharedColor(index: Int, time: TimeInterval) -> Color {
    if sharedMultiColor.isEnabled {
      return sharedMultiColor.color(
        index: index,
        time: time,
        settings: particleSettings
      )
    }
    return sharedCustomColor?.animatedColor(index: index, time: time)
      ?? sharedPalette.animatedColor(index: index, time: time)
  }

  // Resolves the accent palette used for ribbons and foreground details.
  func ribbonColor(index: Int, time: TimeInterval) -> Color {
    if ribbonMultiColor.isEnabled {
      return ribbonMultiColor.color(
        index: index,
        time: time,
        settings: particleSettings
      )
    }
    return ribbonCustomColor?.animatedColor(index: index, time: time)
      ?? ribbonPalette.animatedColor(index: index, time: time)
  }

  // Keeps the default PS4 blue base while still allowing Shared Themes to override it.
  func playStation4BaseColor(index: Int, time: TimeInterval) -> Color {
    if sharedMultiColor.isEnabled {
      return sharedMultiColor.color(
        index: index,
        time: time,
        settings: particleSettings
      )
    }
    if let sharedCustomColor {
      return sharedCustomColor.animatedColor(index: index, time: time)
    }
    if sharedPalette == .blue {
      switch index % 3 {
      case 0:
        return Color(red: 0.004, green: 0.025, blue: 0.16)
      case 1:
        return Color(red: 0.015, green: 0.2, blue: 0.58)
      default:
        return Color(red: 0.12, green: 0.42, blue: 1)
      }
    }
    return sharedPalette.animatedColor(index: index, time: time)
  }

  var paletteDarkEffectStrength: Double {
    guard
      !particleSettings.disablesDarkPaletteEffects,
      sharedCustomColor?.usesDarkEffect != false
    else {
      return 0
    }
    return min(2, max(0, particleSettings.paletteDarkEffectIntensity))
  }

  func paletteDarkEffectColor(at progress: Double) -> Color {
    sharedCustomColor?.darkEffectColor(at: progress) ?? .black
  }

  var sharedPaletteGradientTilt: Double {
    particleSettings.sharedPaletteGradientTilt
  }

  var sharedPaletteGradientCurvature: Double {
    particleSettings.sharedPaletteGradientCurvature
  }

  func sharedGradientPoints(
    from startPoint: UnitPoint,
    to endPoint: UnitPoint
  ) -> (start: UnitPoint, end: UnitPoint) {
    transformedGradientPoints(
      from: startPoint,
      to: endPoint,
      degrees: sharedPaletteGradientTilt,
      offsetX: particleSettings.sharedPaletteGradientOffsetX,
      offsetY: particleSettings.sharedPaletteGradientOffsetY,
      width: particleSettings.sharedPaletteGradientWidth
    )
  }

  // Produces an opaque palette stop shaded toward the customizable dark effect.
  func paletteBackgroundColor(_ color: Color, darkness: Double) -> Color {
    blendPaletteEffectColor(
      color,
      paletteDarkEffectColor(at: darkness),
      amount: min(1, max(0, darkness * paletteDarkEffectStrength))
    )
  }

  // Applies the same shared dark effect to vignettes without altering palette stops.
  func paletteDarkOverlay(opacity: Double) -> Color {
    paletteDarkEffectColor(at: opacity).opacity(
      min(1, max(0, opacity * paletteDarkEffectStrength))
    )
  }

  // Keeps default highlights white until the Ribbons palette is changed.
  func highlightColor(index: Int, time: TimeInterval) -> Color {
    if ribbonMultiColor.isEnabled || ribbonCustomColor != nil || ribbonPalette != .cyan {
      return ribbonColor(index: index, time: time)
    }
    return .white
  }
}

private func transformedGradientPoints(
  from startPoint: UnitPoint,
  to endPoint: UnitPoint,
  degrees: Double,
  offsetX: Double,
  offsetY: Double,
  width: Double
) -> (start: UnitPoint, end: UnitPoint) {
  let radians = degrees * .pi / 180
  let cosine = CGFloat(cos(radians))
  let sine = CGFloat(sin(radians))
  let scale = CGFloat(max(0.05, width))
  let center = CGPoint(
    x: (startPoint.x + endPoint.x) * 0.5 + CGFloat(offsetX),
    y: (startPoint.y + endPoint.y) * 0.5 + CGFloat(offsetY)
  )
  let halfVector = CGPoint(
    x: (endPoint.x - startPoint.x) * 0.5 * scale,
    y: (endPoint.y - startPoint.y) * 0.5 * scale
  )
  let rotatedHalfVector = CGPoint(
    x: halfVector.x * cosine - halfVector.y * sine,
    y: halfVector.x * sine + halfVector.y * cosine
  )

  return (
    UnitPoint(
      x: center.x - rotatedHalfVector.x,
      y: center.y - rotatedHalfVector.y
    ),
    UnitPoint(
      x: center.x + rotatedHalfVector.x,
      y: center.y + rotatedHalfVector.y
    )
  )
}

func blendPaletteEffectColor(
  _ first: Color,
  _ second: Color,
  amount: Double
) -> Color {
  let progress = min(1, max(0, amount))
  guard progress > 0 else { return first }

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
    return first
  }

  return Color(
    red: Double(firstRed + (secondRed - firstRed) * progress),
    green: Double(firstGreen + (secondGreen - firstGreen) * progress),
    blue: Double(firstBlue + (secondBlue - firstBlue) * progress),
    opacity: Double(firstAlpha + (secondAlpha - firstAlpha) * progress)
  )
}

// MARK: - DynamicBackgroundUtilities

enum DynamicBackgroundCoding {
  static func decode<Value: Decodable>(
    _ type: Value.Type,
    from data: Data
  ) -> Value? {
    try? JSONDecoder().decode(type, from: data)
  }

  static func decode<Value: Decodable>(
    _ type: Value.Type,
    from json: String
  ) -> Value? {
    guard let data = json.data(using: .utf8) else { return nil }
    return decode(type, from: data)
  }

  static func encode<Value: Encodable>(_ value: Value) -> Data? {
    try? JSONEncoder().encode(value)
  }

  static func encodeJSONString<Value: Encodable>(_ value: Value) -> String? {
    guard let data = encode(value) else { return nil }
    return String(data: data, encoding: .utf8)
  }
}

enum DynamicBackgroundMath {
  static func clamp(_ value: Double, to range: ClosedRange<Double>) -> Double {
    min(range.upperBound, max(range.lowerBound, value))
  }

  static func unit(_ value: Double) -> Double {
    value - floor(value)
  }

  static func seededUnit(index: Int, salt: Double) -> Double {
    unit(sin((Double(index) + 1) * salt) * 43_758.5453)
  }

  static func smoothstep(
    from lowerBound: Double = 0,
    to upperBound: Double = 1,
    value: Double
  ) -> Double {
    let progress = clamp(
      (value - lowerBound) / (upperBound - lowerBound),
      to: 0...1
    )
    return progress * progress * (3 - 2 * progress)
  }
}

enum DynamicBackgroundGeometry {
  // Converts sampled points into a smooth Catmull-Rom-style cubic path.
  static func smoothCurve(through points: [CGPoint]) -> Path {
    guard let first = points.first else { return Path() }

    var path = Path()
    path.move(to: first)

    for index in 0..<(points.count - 1) {
      let previous = index > 0 ? points[index - 1] : points[index]
      let current = points[index]
      let next = points[index + 1]
      let following = index + 2 < points.count ? points[index + 2] : next
      let control1 = CGPoint(
        x: current.x + (next.x - previous.x) / 6,
        y: current.y + (next.y - previous.y) / 6
      )
      let control2 = CGPoint(
        x: next.x - (following.x - current.x) / 6,
        y: next.y - (following.y - current.y) / 6
      )

      path.addCurve(
        to: next,
        control1: control1,
        control2: control2
      )
    }

    return path
  }
}
