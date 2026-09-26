// GameCoverThemePreview.swift — transient cover-derived menu appearance
// SPDX-License-Identifier: GPL-3.0+

import CoreGraphics
import Foundation
import ImageIO
import Observation
import SwiftUI

struct GameCoverThemePreview: Equatable {
    let gameID: String
    let backgroundPalette: SavedPaletteColor
    let accentPalette: SavedPaletteColor
    let primaryTextPalette: SavedPaletteColor
    let secondaryTextPalette: SavedPaletteColor

    var accentColor: Color { accentPalette.color }
    var primaryTextColor: Color { primaryTextPalette.color }
    var secondaryTextColor: Color { secondaryTextPalette.color }
}

/// Owns a single transient preview without mutating the user's saved theme.
/// Cached cover colours apply synchronously; uncached analysis starts as soon
/// as the card receives focus.
@MainActor
@Observable
final class GameCoverThemePreviewStore {
    static let shared = GameCoverThemePreviewStore()

    private(set) var preview: GameCoverThemePreview?

    @ObservationIgnored private var analysisTask: Task<Void, Never>?
    @ObservationIgnored private var requestToken = UUID()
    @ObservationIgnored private var cache: [String: CoverThemeAnalysis] = [:]

    private init() {}

    func schedule(
        gameID: String,
        coverURL: URL?,
        coverSignature: String?
    ) {
        analysisTask?.cancel()
        analysisTask = nil
        requestToken = UUID()

        guard let coverURL else {
            clear()
            return
        }

        if preview?.gameID != gameID {
            withAnimation(.easeOut(duration: 0.18)) {
                preview = nil
            }
        }

        let token = requestToken
        let cacheKey = "\(coverURL.path)|\(coverSignature ?? "")"

        // Reuse an analyzed cover synchronously so focus and command surfaces
        // never flash the previous palette.
        if let cached = cache[cacheKey] {
            apply(cached, gameID: gameID)
            return
        }

        analysisTask = Task { @MainActor [weak self] in
            guard !Task.isCancelled,
                  let self,
                  self.requestToken == token else { return }

            let analysis: CoverThemeAnalysis?
            if let cached = self.cache[cacheKey] {
                analysis = cached
            } else {
                analysis = await Task.detached(priority: .utility) {
                    CoverThemeAnalyzer.analyze(url: coverURL)
                }.value
                if let analysis {
                    self.cache[cacheKey] = analysis
                }
            }

            guard !Task.isCancelled,
                  self.requestToken == token,
                  let analysis else { return }
            self.apply(analysis, gameID: gameID)
        }
    }

    private func apply(_ analysis: CoverThemeAnalysis, gameID: String) {
        let next = GameCoverThemePreview(
            gameID: gameID,
            backgroundPalette: SavedPaletteColor(
                coverThemeHexes: analysis.backgroundHexes
            ),
            accentPalette: SavedPaletteColor(
                coverThemeHex: analysis.accentHex
            ),
            primaryTextPalette: SavedPaletteColor(
                coverThemeHex: analysis.primaryTextHex
            ),
            secondaryTextPalette: SavedPaletteColor(
                coverThemeHex: analysis.secondaryTextHex
            )
        )
        withAnimation(.easeInOut(duration: 0.32)) {
            preview = next
        }
        analysisTask = nil
    }

    func clear(gameID: String? = nil) {
        if let gameID, preview?.gameID != gameID {
            return
        }
        analysisTask?.cancel()
        analysisTask = nil
        requestToken = UUID()
        guard preview != nil else { return }
        withAnimation(.easeOut(duration: 0.26)) {
            preview = nil
        }
    }
}

extension SavedPaletteColor {
    fileprivate init(coverThemeHex: String) {
        hex = coverThemeHex
        gradientHexes = nil
        darkEffectHex = nil
        darkEffectGradientHexes = nil
        darkEffectEnabled = false
    }

    fileprivate init(coverThemeHexes: [String]) {
        let colors = coverThemeHexes.isEmpty ? ["#57B8F9"] : coverThemeHexes
        hex = colors[0]
        gradientHexes = colors.count > 1 ? colors : nil
        darkEffectHex = nil
        darkEffectGradientHexes = nil
        darkEffectEnabled = false
    }
}

private struct CoverThemeAnalysis: Sendable {
    let backgroundHexes: [String]
    let accentHex: String
    let primaryTextHex: String
    let secondaryTextHex: String
}

private enum CoverThemeAnalyzer {
    private struct Sample: Sendable {
        var red: Double
        var green: Double
        var blue: Double
        var count: Int

        var luminance: Double {
            (0.2126 * red) + (0.7152 * green) + (0.0722 * blue)
        }

        var saturation: Double {
            let maximum = max(red, green, blue)
            let minimum = min(red, green, blue)
            return maximum == 0 ? 0 : (maximum - minimum) / maximum
        }
    }

    private struct Bucket {
        var red = 0.0
        var green = 0.0
        var blue = 0.0
        var count = 0

        mutating func add(red: Double, green: Double, blue: Double) {
            self.red += red
            self.green += green
            self.blue += blue
            count += 1
        }

        var sample: Sample {
            let divisor = Double(max(1, count))
            return Sample(
                red: red / divisor,
                green: green / divisor,
                blue: blue / divisor,
                count: count
            )
        }
    }

    static func analyze(url: URL) -> CoverThemeAnalysis? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(
                  source,
                  0,
                  [
                      kCGImageSourceCreateThumbnailFromImageAlways: true,
                      kCGImageSourceThumbnailMaxPixelSize: 48,
                      kCGImageSourceCreateThumbnailWithTransform: true,
                  ] as CFDictionary
              ) else { return nil }

        let width = image.width
        let height = image.height
        guard width > 0, height > 0 else { return nil }

        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let rendered = pixels.withUnsafeMutableBytes { storage -> Bool in
            guard let context = CGContext(
                data: storage.baseAddress,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
                    | CGBitmapInfo.byteOrder32Big.rawValue
            ) else { return false }
            context.interpolationQuality = .low
            context.draw(
                image,
                in: CGRect(x: 0, y: 0, width: width, height: height)
            )
            return true
        }
        guard rendered else { return nil }

        var buckets: [Int: Bucket] = [:]
        for offset in stride(from: 0, to: pixels.count, by: 4) {
            guard pixels[offset + 3] > 96 else { continue }
            let red = Double(pixels[offset]) / 255
            let green = Double(pixels[offset + 1]) / 255
            let blue = Double(pixels[offset + 2]) / 255
            let luminance = (0.2126 * red) + (0.7152 * green) + (0.0722 * blue)
            guard luminance > 0.025, luminance < 0.985 else { continue }
            let key = (Int(red * 15) << 8)
                | (Int(green * 15) << 4)
                | Int(blue * 15)
            buckets[key, default: Bucket()].add(
                red: red,
                green: green,
                blue: blue
            )
        }

        let ranked = buckets.values.map(\.sample).sorted { lhs, rhs in
            score(lhs) > score(rhs)
        }
        guard let first = ranked.first else { return nil }

        var selected = [first]
        for candidate in ranked.dropFirst() {
            guard selected.allSatisfy({ distance($0, candidate) > 0.16 }) else {
                continue
            }
            selected.append(candidate)
            if selected.count == 3 { break }
        }
        while selected.count < 3 {
            selected.append(adjustedVariant(of: selected[0], index: selected.count))
        }

        let accent = readableOnDarkSurface(vividVersion(of: selected.max {
            ($0.saturation * 0.72 + $0.luminance * 0.28)
                < ($1.saturation * 0.72 + $1.luminance * 0.28)
        } ?? first))
        let averageLuminance = selected.map(\.luminance).reduce(0, +)
            / Double(selected.count)
        let usesDarkText = averageLuminance > 0.64

        return CoverThemeAnalysis(
            backgroundHexes: selected.map(hex),
            accentHex: hex(accent),
            primaryTextHex: usesDarkText ? "#111722" : "#FFFFFF",
            secondaryTextHex: usesDarkText ? "#354052" : "#DCE9F5"
        )
    }

    private static func score(_ sample: Sample) -> Double {
        Double(sample.count)
            * (0.42 + sample.saturation)
            * (0.58 + min(sample.luminance, 0.78))
    }

    private static func distance(_ lhs: Sample, _ rhs: Sample) -> Double {
        let red = lhs.red - rhs.red
        let green = lhs.green - rhs.green
        let blue = lhs.blue - rhs.blue
        return sqrt((red * red) + (green * green) + (blue * blue))
    }

    private static func adjustedVariant(of sample: Sample, index: Int) -> Sample {
        let factor = index == 1 ? 0.66 : 1.24
        return Sample(
            red: min(1, sample.red * factor),
            green: min(1, sample.green * factor),
            blue: min(1, sample.blue * factor),
            count: sample.count
        )
    }

    private static func vividVersion(of sample: Sample) -> Sample {
        let maximum = max(sample.red, sample.green, sample.blue)
        let minimum = min(sample.red, sample.green, sample.blue)
        let delta = maximum - minimum
        guard delta > 0.001 else {
            return Sample(red: 0.34, green: 0.72, blue: 0.98, count: sample.count)
        }
        let targetValue = max(0.72, maximum)
        let targetSaturation = max(0.58, delta / maximum)
        let liftedMinimum = targetValue * (1 - targetSaturation)
        func component(_ value: Double) -> Double {
            let normalized = (value - minimum) / delta
            return min(1, max(0, liftedMinimum + normalized * (targetValue - liftedMinimum)))
        }
        return Sample(
            red: component(sample.red),
            green: component(sample.green),
            blue: component(sample.blue),
            count: sample.count
        )
    }

    /// Cover colors also become focused text and icons on dark command panels. Preserve the
    /// cover's hue while blending only very dark accents toward white until they are legible.
    private static func readableOnDarkSurface(_ sample: Sample) -> Sample {
        let minimumLuminance = 0.54
        guard sample.luminance < minimumLuminance else { return sample }
        let blend = min(
            0.72,
            max(0, (minimumLuminance - sample.luminance) / (1 - sample.luminance))
        )
        return Sample(
            red: sample.red + (1 - sample.red) * blend,
            green: sample.green + (1 - sample.green) * blend,
            blue: sample.blue + (1 - sample.blue) * blend,
            count: sample.count
        )
    }

    private static func hex(_ sample: Sample) -> String {
        String(
            format: "#%02X%02X%02X",
            Int(min(1, max(0, sample.red)) * 255),
            Int(min(1, max(0, sample.green)) * 255),
            Int(min(1, max(0, sample.blue)) * 255)
        )
    }
}
