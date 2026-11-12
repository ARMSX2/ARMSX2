// NumberFormat.swift — number format and setting types
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// How a number is written down. A value, not a closure, so the unit and the rounding live in one place.
struct NumberFormat {
    /// Template with one %@ for the digits; String(format:) would crash on a stray %s in a translation.
    let unit: String
    let decimals: Int
    /// Shown = stored * scale. 100 is what turns a 0...1 opacity into a percentage.
    let scale: Double
    let showsSign: Bool
    let trimsTrailingZeros: Bool
    let literal: String?

    init(unit: String = "", decimals: Int = 0, scale: Double = 1,
         showsSign: Bool = false, trimsTrailingZeros: Bool = false,
         literal: String? = nil) {
        self.unit = unit
        self.decimals = decimals
        self.scale = scale
        self.showsSign = showsSign
        self.trimsTrailingZeros = trimsTrailingZeros
        self.literal = literal
    }

    static let plain = NumberFormat()
    /// Already stored as a percentage: volume 0...150, shade boost 1...100.
    static let percent = NumberFormat(unit: "%@%")
    static let unitPercent = NumberFormat(unit: "%@%", scale: 100)
    static let milliseconds = NumberFormat(unit: "%@ ms")
    static let seconds = NumberFormat(unit: "%@ s", decimals: 2)
    static let points = NumberFormat(unit: "%@ pt")
    static let framesPerSecond = NumberFormat(unit: "%@ FPS")
    static let taps = NumberFormat(unit: "%@ taps")
    static let multiplier = NumberFormat(unit: "%@x", decimals: 2)
    static let radiansPerSecond = NumberFormat(unit: "%@ rad/s", decimals: 2)
    static let degreesPerPoint = NumberFormat(unit: "%@°/pt", decimals: 2)

    /// A row built around a string it formatted itself; it cannot label its own bounds.
    static func opaque(_ text: String) -> NumberFormat {
        NumberFormat(literal: text)
    }

    func decimals(_ count: Int) -> NumberFormat {
        NumberFormat(unit: unit, decimals: count, scale: scale, showsSign: showsSign,
                     trimsTrailingZeros: trimsTrailingZeros, literal: literal)
    }

    func compactDecimals(_ count: Int) -> NumberFormat {
        NumberFormat(unit: unit, decimals: count, scale: scale, showsSign: showsSign,
                     trimsTrailingZeros: true, literal: literal)
    }

    var signed: NumberFormat {
        NumberFormat(unit: unit, decimals: decimals, scale: scale, showsSign: true,
                     trimsTrailingZeros: trimsTrailingZeros, literal: literal)
    }

    /// An opaque readout is somebody else's string: it labels no bounds and cannot be typed.
    var labelsBounds: Bool { literal == nil }
    var isTypeable: Bool { literal == nil }
    var isPercentage: Bool { literal == nil && unit == "%@%" }

    /// The smallest change this readout can show; a continuous row snaps to it.
    var displayStep: Double { 1 / (scale * pow(10, Double(decimals))) }

    /// Digits only: what the keyboard edits and the unit template takes. Normally exactly
    /// `decimals` decimals so readouts stay aligned; compact formats keep only meaningful ones.
    func digits(_ value: Double, locale: Locale) -> String {
        if trimsTrailingZeros {
            return (value * scale).formatted(
                .number
                    .precision(.fractionLength(0...decimals))
                    .rounded(rule: .toNearestOrAwayFromZero)
                    .sign(strategy: showsSign ? .always(includingZero: false) : .automatic)
                    .grouping(.never)
                    .locale(locale)
            )
        }
        return (value * scale).formatted(
            .number
                .precision(.fractionLength(decimals))
                .rounded(rule: .toNearestOrAwayFromZero)
                .sign(strategy: showsSign ? .always(includingZero: false) : .automatic)
                .grouping(.never)
                .locale(locale)
        )
    }

    @MainActor
    func text(_ value: Double, settings: SettingsStore) -> String {
        if let literal { return literal }
        let number = digits(value, locale: settings.numberLocale)
        guard !unit.isEmpty else { return number }
        return settings.localized(unit).replacingOccurrences(of: "%@", with: number)
    }

    /// Undoes `scale`, so typing 70 into a 0...1 opacity row gets you 0.7. Accepts '.' and ','.
    func parse(_ text: String) -> Double? {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalized = cleaned.replacingOccurrences(of: ",", with: ".")
        guard let val = Double(normalized), val.isFinite else { return nil }
        return val / scale
    }
}

/// One numeric setting written down once: title, range, format, stops and default.
/// The instances live next to the ranges they use, in SettingsStore.swift.
struct NumberSetting {
    let title: String
    let range: ClosedRange<Double>
    let format: NumberFormat
    /// Empty leaves the track continuous.
    let detents: [Double]
    let step: Double?
    let style: NumberRow.Style
    let defaultValue: Double?
    let icon: String?
    let hint: String?

    init(_ title: String,
         in range: ClosedRange<Double>,
         format: NumberFormat = .plain,
         detents: [Double] = [],
         step: Double? = nil,
         style: NumberRow.Style = .slider,
         default defaultValue: Double? = nil,
         icon: String? = nil,
         hint: String? = nil) {
        self.title = title
        self.range = range
        self.format = format
        self.detents = detents
        self.step = step
        self.style = style
        self.defaultValue = defaultValue
        self.icon = icon
        self.hint = hint
    }

    /// Whole numbers, for the settings the core stores as an Int.
    init(_ title: String,
         in range: ClosedRange<Int>,
         format: NumberFormat = .plain,
         detents: [Int] = [],
         style: NumberRow.Style = .slider,
         default defaultValue: Int? = nil,
         icon: String? = nil,
         hint: String? = nil) {
        self.init(title,
                  in: Double(range.lowerBound)...Double(range.upperBound),
                  format: format,
                  detents: detents.map(Double.init),
                  step: 1,
                  style: style,
                  default: defaultValue.map(Double.init),
                  icon: icon,
                  hint: hint)
    }

    var intRange: ClosedRange<Int> {
        Int(range.lowerBound.rounded())...Int(range.upperBound.rounded())
    }
}
