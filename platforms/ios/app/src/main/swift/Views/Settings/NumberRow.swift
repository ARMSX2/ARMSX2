// NumberRow.swift — the one numeric control: label, tappable readout, slider between its bounds.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// How a number is written down. A value rather than a closure, so the unit lives in one
/// translatable template instead of being spelled " ms" at every call site, and so the rounding
/// happens here once instead of being truncated in some rows and rounded in others.
struct NumberFormat {
    /// Template with one %@ for the digits. Substituted by hand rather than through
    /// String(format:), because a stray %s in a hand written translation would crash.
    let unit: String
    let decimals: Int
    /// Shown = stored * scale. 100 is what turns a 0...1 opacity into a percentage.
    let scale: Double
    let showsSign: Bool
    /// Set by `opaque`, for rows still handing us a readout they built themselves.
    let literal: String?

    init(unit: String = "", decimals: Int = 0, scale: Double = 1,
         showsSign: Bool = false, literal: String? = nil) {
        self.unit = unit
        self.decimals = decimals
        self.scale = scale
        self.showsSign = showsSign
        self.literal = literal
    }

    static let plain = NumberFormat()
    /// Already stored as a percentage: volume 0...150, shade boost 1...100.
    static let percent = NumberFormat(unit: "%@%")
    /// Stored 0...1 and shown as a percentage.
    static let unitPercent = NumberFormat(unit: "%@%", scale: 100)
    static let milliseconds = NumberFormat(unit: "%@ ms")
    static let seconds = NumberFormat(unit: "%@ s", decimals: 2)
    static let points = NumberFormat(unit: "%@ pt")
    static let framesPerSecond = NumberFormat(unit: "%@ FPS")
    static let taps = NumberFormat(unit: "%@ taps")
    static let multiplier = NumberFormat(unit: "%@x", decimals: 2)
    static let radiansPerSecond = NumberFormat(unit: "%@ rad/s", decimals: 2)
    static let degreesPerPoint = NumberFormat(unit: "%@°/pt", decimals: 2)

    /// A row built around a string it formatted itself. It still slides, clamps, brackets and
    /// announces itself, it just cannot label its own bounds, so it does not.
    static func opaque(_ text: String) -> NumberFormat {
        NumberFormat(literal: text)
    }

    func decimals(_ count: Int) -> NumberFormat {
        NumberFormat(unit: unit, decimals: count, scale: scale, showsSign: showsSign)
    }

    var signed: NumberFormat {
        NumberFormat(unit: unit, decimals: decimals, scale: scale, showsSign: true)
    }

    /// An opaque readout is a string somebody else built, so it cannot be labelled onto the
    /// bounds and it cannot be read back off the keyboard either.
    var labelsBounds: Bool { literal == nil }
    var isTypeable: Bool { literal == nil }

    /// The smallest change this readout can show. A continuous row snaps to it so the stored
    /// value is the number on screen rather than a fraction hiding underneath it.
    var displayStep: Double { 1 / (scale * pow(10, Double(decimals))) }

    /// Digits only. This is what the keyboard edits and what goes into the unit template.
    ///
    /// Exactly that many decimals, never fewer. Dropping the trailing zeros looks tidier on any one
    /// value and is wrong in motion: the stops are coarser than the step, so a readout that trims
    /// changes width on every stop you drag past, and the decimal points stop lining up between the
    /// rows in a section that are there to be compared. A row whose stops really are coarser than
    /// its format should say so with `.decimals`.
    func digits(_ value: Double, locale: Locale) -> String {
        (value * scale).formatted(
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

    /// Undoes `scale`, so typing 70 into a 0...1 opacity row gets you 0.7.
    ///
    /// German groups on "." and points on ",", so stripping the grouping separator first turned
    /// a typed "0.5" into "05". Only treat "." as grouping when the text also holds a real
    /// decimal separator; otherwise a lone "." or "," is the point the user meant.
    func parse(_ text: String, locale: Locale) -> Double? {
        var cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let decimal = locale.decimalSeparator, cleaned.contains(decimal) {
            if let grouping = locale.groupingSeparator {
                cleaned = cleaned.replacingOccurrences(of: grouping, with: "")
            }
            cleaned = cleaned.replacingOccurrences(of: decimal, with: ".")
        } else {
            cleaned = cleaned.replacingOccurrences(of: ",", with: ".")
        }
        guard let shown = Double(cleaned), shown.isFinite else { return nil }
        return shown / scale
    }
}

/// One numeric setting written down once: what it is called, where it runs, how it reads, where it
/// stops and what it goes back to. Three screens render the FPS target and each used to spell all
/// five for itself, which is how they came to disagree about all five.
///
/// The instances live next to the ranges they use, in SettingsStore.swift.
struct NumberSetting {
    let title: String
    let range: ClosedRange<Double>
    let format: NumberFormat
    /// Empty leaves the track continuous.
    let detents: [Double]
    /// What a continuous track snaps to. Stops do their own.
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

    /// Whole numbers, for the settings the core stores as an Int. They move by one by definition.
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

    /// The whole-number bounds back again, for the per-game rows that stage an Int override.
    var intRange: ClosedRange<Int> {
        Int(range.lowerBound.rounded())...Int(range.upperBound.rounded())
    }
}

/// Set once per surface, so the call sites do not each carry fonts and padding.
struct NumberRowStyle {
    var titleFont: Font = .body
    var valueFont: Font = .callout.monospacedDigit()
    var boundsFont: Font = .caption.monospacedDigit()
    /// Between the header and the track. The header has its own, below.
    var spacing: CGFloat = 8
    var horizontalSpacing: CGFloat = 10
    var verticalPadding: CGFloat = 4
    var allowsTypedEntry = true

    /// The box the number sits in, the same whether it is being read or typed into, so opening the
    /// keyboard swaps the content and never the geometry. The height matches the reset button, so
    /// on a row that has one the tap target costs nothing.
    var readoutMinWidth: CGFloat = 56
    var readoutMaxWidth: CGFloat = 132
    var readoutMinHeight: CGFloat = 28
    /// Taken back immediately after, so the target reaches 44 by spending the gap under the header
    /// rather than making every row taller.
    var readoutHitInset: CGFloat = 8

    static let form = NumberRowStyle()
    /// The background editor puts a few hundred of these in one sheet. It is the one place worth
    /// trading the 44 point target away, and it should say so here rather than in the row.
    static let compact = NumberRowStyle(
        titleFont: .caption.weight(.semibold),
        valueFont: .caption.monospacedDigit(),
        boundsFont: .caption2.monospacedDigit(),
        spacing: 6,
        horizontalSpacing: 8,
        verticalPadding: 0,
        readoutMinWidth: 44,
        readoutMaxWidth: 108,
        readoutMinHeight: 24,
        readoutHitInset: 3
    )
}

/// Live reporting for a surface that draws its own readout while a slider moves. Only the
/// dynamic background editor supplies one.
///
/// Unchecked because it has to be an environment default and only ever runs from a view body,
/// which is already on the main actor.
struct NumberRowActivity: @unchecked Sendable {
    let update: (_ title: String, _ value: String, _ isEditing: Bool) -> Void
}

/// The one trailing affordance the header has room for. Reset uses it; the per-game row uses it
/// to go back to the global value.
struct NumberRowAccessory {
    let systemImage: String
    /// Localizable, one %@ for the row title. VoiceOver only, the button itself is a glyph.
    let label: String
    let isVisible: Bool
    let action: () -> Void
}

private struct NumberRowStyleKey: EnvironmentKey {
    static let defaultValue = NumberRowStyle.form
}

private struct NumberRowActivityKey: EnvironmentKey {
    static let defaultValue = NumberRowActivity { _, _, _ in }
}

extension EnvironmentValues {
    var numberRowStyle: NumberRowStyle {
        get { self[NumberRowStyleKey.self] }
        set { self[NumberRowStyleKey.self] = newValue }
    }

    var numberRowActivity: NumberRowActivity {
        get { self[NumberRowActivityKey.self] }
        set { self[NumberRowActivityKey.self] = newValue }
    }
}

extension View {
    func numberRowStyle(_ style: NumberRowStyle) -> some View {
        environment(\.numberRowStyle, style)
    }
}

/// Every numeric setting in the app.
///
/// Colours are semantic on purpose. The global Form is light or dark and the per-game panel and
/// background editor both force dark, so .primary and .secondary land correctly in all three and
/// OverlayTheme never has to be reached for.
struct NumberRow: View {
    enum Style {
        case slider
        case stepper
        case field
    }

    private let title: String
    private let icon: String?
    private let store: Binding<Double>
    private let range: ClosedRange<Double>
    private let step: Double?
    /// Non empty turns the track into a short list of stops instead of a continuous range.
    private let detents: [Double]
    private let format: NumberFormat
    private let style: Style
    private let accessory: NumberRowAccessory?
    private let hint: String?
    private let settings: SettingsStore

    @Environment(\.numberRowStyle) private var rowStyle
    @Environment(\.numberRowActivity) private var activity
    // A disabled Text does not dim on its own, so the skipdraw and texture offset fields have been
    // sitting there fully tinted and boxed while doing nothing at all.
    @Environment(\.isEnabled) private var isEnabled
    @State private var isDragging = false
    @State private var isTyping = false
    @State private var draft = ""
    @State private var seededDraft = ""
    @FocusState private var fieldFocused: Bool

    /// What a settings screen writes. The row is named by its setting and nothing else, because
    /// anything descriptive here would be a second opinion about a setting that already has one.
    init(_ setting: NumberSetting,
         value: Binding<Double>,
         accessory: NumberRowAccessory? = nil,
         hint: String? = nil,
         settings: SettingsStore) {
        self.init(setting.title,
                  value: value,
                  in: setting.range,
                  format: setting.format,
                  step: setting.step,
                  detents: setting.detents,
                  icon: setting.icon,
                  style: setting.style,
                  default: setting.defaultValue,
                  accessory: accessory,
                  hint: hint ?? setting.hint,
                  settings: settings)
    }

    init(_ setting: NumberSetting,
         value: Binding<Int>,
         accessory: NumberRowAccessory? = nil,
         hint: String? = nil,
         settings: SettingsStore) {
        self.init(setting,
                  value: Binding(get: { Double(value.wrappedValue) },
                                 set: { value.wrappedValue = Int($0.rounded()) }),
                  accessory: accessory,
                  hint: hint,
                  settings: settings)
    }

    init(_ setting: NumberSetting,
         value: Binding<Float>,
         accessory: NumberRowAccessory? = nil,
         hint: String? = nil,
         settings: SettingsStore) {
        self.init(setting,
                  value: Binding(get: { Double(value.wrappedValue) },
                                 set: { value.wrappedValue = Float($0) }),
                  accessory: accessory,
                  hint: hint,
                  settings: settings)
    }

    init(_ title: String,
         value: Binding<Double>,
         in range: ClosedRange<Double>,
         format: NumberFormat = .plain,
         step: Double? = nil,
         detents: [Double] = [],
         icon: String? = nil,
         style: Style = .slider,
         default defaultValue: Double? = nil,
         accessory: NumberRowAccessory? = nil,
         hint: String? = nil,
         settings: SettingsStore) {
        self.title = title
        self.icon = icon
        self.store = value
        self.range = range
        self.step = step
        // Stops are a shortcut to the values worth having, never the whole list, and typing is what
        // reaches the rest. A row that cannot be typed into keeps its continuous track instead, or
        // stops would be the only values it could reach at all.
        self.detents = format.isTypeable ? detents.sorted() : []
        self.format = format
        self.style = style
        self.hint = hint
        self.settings = settings
        // An explicit accessory wins; `default:` is sugar for the common one.
        self.accessory = accessory ?? defaultValue.map { fallback in
            NumberRowAccessory(
                systemImage: "arrow.counterclockwise",
                label: "Reset %@",
                isVisible: Self.reads(value.wrappedValue, differentlyFrom: fallback,
                                      format: format, step: step),
                action: { value.wrappedValue = fallback }
            )
        }
    }

    init(_ title: String,
         value: Binding<Int>,
         in range: ClosedRange<Int>,
         format: NumberFormat = .plain,
         detents: [Int] = [],
         icon: String? = nil,
         style: Style = .slider,
         default defaultValue: Int? = nil,
         accessory: NumberRowAccessory? = nil,
         hint: String? = nil,
         settings: SettingsStore) {
        self.init(title,
                  value: Binding(get: { Double(value.wrappedValue) },
                                 set: { value.wrappedValue = Int($0.rounded()) }),
                  in: Double(range.lowerBound)...Double(range.upperBound),
                  format: format,
                  step: 1,
                  detents: detents.map(Double.init),
                  icon: icon,
                  style: style,
                  default: defaultValue.map(Double.init),
                  accessory: accessory,
                  hint: hint,
                  settings: settings)
    }

    init(_ title: String,
         value: Binding<Float>,
         in range: ClosedRange<Float>,
         format: NumberFormat = .plain,
         step: Double? = nil,
         detents: [Double] = [],
         icon: String? = nil,
         style: Style = .slider,
         default defaultValue: Float? = nil,
         accessory: NumberRowAccessory? = nil,
         hint: String? = nil,
         settings: SettingsStore) {
        self.init(title,
                  value: Binding(get: { Double(value.wrappedValue) },
                                 set: { value.wrappedValue = Float($0) }),
                  in: Double(range.lowerBound)...Double(range.upperBound),
                  format: format,
                  step: step,
                  detents: detents,
                  icon: icon,
                  style: style,
                  default: defaultValue.map(Double.init),
                  accessory: accessory,
                  hint: hint,
                  settings: settings)
    }

    var body: some View {
        content
            .padding(.vertical, rowStyle.verticalPadding)
            .onChange(of: store.wrappedValue) { _, _ in
                guard isDragging else { return }
                activity.update(localizedTitle, displayText, true)
            }
            .onChange(of: draft) { _, _ in draftChanged() }
            .onChange(of: fieldFocused) { _, focused in
                if !focused { commit() }
            }
            .onDisappear(perform: release)
    }

    @ViewBuilder
    private var content: some View {
        switch style {
        case .slider, .stepper:
            VStack(alignment: .leading, spacing: rowStyle.spacing) {
                header
                control
            }
        case .field:
            header
        }
    }

    /// Whatever goes under the header. A stepper used to swallow the readout as its own label and
    /// build its own row, which put the one stepper in the app ninety points left of the column its
    /// neighbours share, and quietly dropped the reset button and the hint with it.
    @ViewBuilder
    private var control: some View {
        if style == .stepper {
            Stepper("", value: clampedValue, in: range, step: step ?? 1)
                .labelsHidden()
                .fixedSize()
                .frame(maxWidth: .infinity, alignment: .trailing)
                .accessibilityLabel(localizedTitle)
                .accessibilityValue(displayText)
                .modifier(OptionalHint(text: hint.map { settings.localized($0) }))
        } else {
            track
        }
    }

    private var header: some View {
        HStack(spacing: rowStyle.horizontalSpacing) {
            label
            Spacer(minLength: rowStyle.horizontalSpacing)
            // Inboard of the number on purpose. The number is then the last thing in every row: the
            // same column as a hand-written value row, and the same column as the track's own upper
            // bound directly underneath, which it used to sit thirty-six points inside of.
            //
            // Still kept in the layout when hidden, because the reset arrow appears and disappears
            // as you drag past the default and the value must not jump when it does. Here that
            // costs nothing, since the reserved space falls inside the gap.
            if let accessory {
                accessoryButton(accessory)
                    .opacity(accessory.isVisible ? 1 : 0)
                    .disabled(!accessory.isVisible)
                    .accessibilityHidden(!accessory.isVisible)
                    .accessibilitySortPriority(0)
            }
            readout
                .accessibilitySortPriority(1)
        }
        .font(rowStyle.titleFont)
    }

    @ViewBuilder
    private var label: some View {
        if let icon {
            Label(localizedTitle, systemImage: icon)
        } else {
            Text(localizedTitle)
        }
    }

    @ViewBuilder
    private var track: some View {
        if range.lowerBound < range.upperBound {
            slider
                .accessibilityLabel(localizedTitle)
                .accessibilityValue(displayText)
                .modifier(OptionalHint(text: hint.map { settings.localized($0) }))
        }
    }

    @ViewBuilder
    private var slider: some View {
        if !detents.isEmpty {
            // Bound to the position in the list rather than the value, so the stops are evenly
            // spaced. That is what puts 60 in the middle of 30/45/60/90/120 instead of at 43%,
            // and it keeps the tick marks few enough to mean something.
            Slider(value: detentIndex,
                   in: 0...Double(detents.count - 1),
                   step: 1,
                   label: { Text(localizedTitle) },
                   minimumValueLabel: { bound(detents.first ?? range.lowerBound) },
                   maximumValueLabel: { bound(detents.last ?? range.upperBound) },
                   onEditingChanged: dragChanged)
        } else {
            // No step, so iOS draws no tick marks. At a hundred plus steps they degenerate into a
            // grey bar under the track that reads as a second, broken progress bar. Snapping
            // happens in the binding instead.
            Slider(value: snappingValue,
                   in: range,
                   label: { Text(localizedTitle) },
                   minimumValueLabel: { bound(range.lowerBound) },
                   maximumValueLabel: { bound(range.upperBound) },
                   onEditingChanged: dragChanged)
        }
    }

    /// Digits only. The unit is in the readout right above, in the same column, and repeating it
    /// costs about a third of the track on a compact row.
    @ViewBuilder
    private func bound(_ value: Double) -> some View {
        if format.labelsBounds {
            Text(format.digits(value, locale: settings.numberLocale))
                .font(rowStyle.boundsFont)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .accessibilityHidden(true)
        }
    }

    @ViewBuilder
    private var readout: some View {
        if isTyping {
            TextField("", text: $draft)
                .focused($fieldFocused)
                .keyboardType(keyboardType)
                .submitLabel(.done)
                .onSubmit { fieldFocused = false }
                .multilineTextAlignment(.trailing)
                .textFieldStyle(.plain)
                .font(rowStyle.valueFont)
                .frame(minWidth: rowStyle.readoutMinWidth,
                       maxWidth: rowStyle.readoutMaxWidth,
                       alignment: .trailing)
                .padding(.horizontal, style == .field ? 10 : 0)
                .padding(.vertical, style == .field ? 5 : 0)
                .background(
                    RoundedRectangle(cornerRadius: 7)
                        .fill(Color.secondary.opacity(style == .field ? 0.12 : 0))
                )
                .frame(minHeight: rowStyle.readoutMinHeight)
                .toolbar {
                    // Declared inside the editing branch, so the bar can only come from the row
                    // you are actually in rather than from all 275 at once.
                    ToolbarItemGroup(placement: .keyboard) {
                        Spacer()
                        Button(settings.localized("Done")) { fieldFocused = false }
                    }
                }
        } else if rowStyle.allowsTypedEntry && format.isTypeable {
            // Grey reads, tinted types, tinted in a box is a row where the number is the whole
            // control. The track already says the row is interactive; only the colour says the
            // number is. A box here would say it too, and would push the digits ten points off the
            // column every other value in the app lines up on.
            Text(displayText)
                .font(rowStyle.valueFont)
                .foregroundStyle(isEnabled ? Color.accentColor : Color.secondary)
                .frame(minWidth: rowStyle.readoutMinWidth, alignment: .trailing)
                .padding(.horizontal, style == .field ? 10 : 0)
                .padding(.vertical, style == .field ? 5 : 0)
                .background(
                    RoundedRectangle(cornerRadius: 7)
                        .fill(Color.secondary.opacity(style == .field ? 0.12 : 0))
                )
                .frame(minHeight: rowStyle.readoutMinHeight)
                // Out and straight back, so the target reaches a finger without the row growing.
                .padding(.vertical, rowStyle.readoutHitInset)
                .contentShape(Rectangle())
                .padding(.vertical, -rowStyle.readoutHitInset)
                .onTapGesture(perform: beginTyping)
                .accessibilityLabel(localizedTitle)
                .accessibilityValue(displayText)
                .accessibilityHint(settings.localized("Double tap to type a value."))
                .accessibilityAddTraits(.isButton)
        } else {
            // Same box, so a row you cannot type into still lines its number up with one you can.
            Text(displayText)
                .font(rowStyle.valueFont)
                .foregroundStyle(.secondary)
                .frame(minWidth: rowStyle.readoutMinWidth,
                       minHeight: rowStyle.readoutMinHeight,
                       alignment: .trailing)
                .accessibilityHidden(true)
        }
    }

    private func accessoryButton(_ accessory: NumberRowAccessory) -> some View {
        Button(action: accessory.action) {
            Image(systemName: accessory.systemImage)
                .font(.caption.weight(.semibold))
                .frame(width: 28, height: 28)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .accessibilityLabel(
            settings.localized(accessory.label).replacingOccurrences(of: "%@", with: localizedTitle)
        )
    }

    /// A hand edited INI hands us values outside the range and Slider does not cope with that, so
    /// read clamps as well as write.
    private var clampedValue: Binding<Double> {
        Binding(get: { clamped(store.wrappedValue) },
                set: { store.wrappedValue = clamped($0) })
    }

    /// Continuous rows still land on clean values, the snapping just happens here rather than
    /// through `step:`, so nothing gets drawn under the track.
    private var snappingValue: Binding<Double> {
        Binding(get: { clamped(store.wrappedValue) },
                set: { raw in
                    guard let step, step > 0 else { store.wrappedValue = clamped(raw); return }
                    let steps = ((raw - range.lowerBound) / step).rounded()
                    store.wrappedValue = clamped(range.lowerBound + steps * step)
                })
    }

    /// Reading snaps to the nearest stop so the knob always sits on one. Writing only happens on a
    /// real drag, so opening a screen never rewrites a value that was not on the list.
    private var detentIndex: Binding<Double> {
        Binding(get: {
                    let current = clamped(store.wrappedValue)
                    let nearest = detents.enumerated().min {
                        abs($0.element - current) < abs($1.element - current)
                    }
                    return Double(nearest?.offset ?? 0)
                },
                set: { index in
                    let i = min(max(Int(index.rounded()), 0), detents.count - 1)
                    store.wrappedValue = detents[i]
                })
    }

    private func clamped(_ value: Double) -> Double {
        guard value.isFinite else { return range.lowerBound }
        return min(max(value, range.lowerBound), range.upperBound)
    }

    /// Fixed, because these strings are compared with each other and never shown to anyone.
    private static let comparisonLocale = Locale(identifier: "en_US_POSIX")

    /// Whether the row would read differently from its default, which is the only reason to offer
    /// a reset. The old test allowed a millionth of the range, far finer than anything on screen,
    /// so a row could print 50% and still offer to put it back to 50% — and a Float widened to
    /// Double never lands on its literal default anyway.
    private static func reads(_ value: Double, differentlyFrom fallback: Double,
                              format: NumberFormat, step: Double?) -> Bool {
        guard format.isTypeable else {
            // Somebody else built this readout, so the step the row moves in is the finest thing
            // we can honestly compare at.
            return abs(value - fallback) > (step ?? 0) / 2
        }
        return format.digits(value, locale: comparisonLocale)
            != format.digits(fallback, locale: comparisonLocale)
    }

    /// Stops for a row that declares only its bounds and a step: the coarsest 1/2/2.5/5 increment
    /// that still leaves at most eight gaps, plus the bounds themselves so both ends stay
    /// reachable. The virtual pad screen's thirty-odd rows use this rather than each naming a stop
    /// list; the background editor deliberately does not, because every value there is legitimate.
    static func stops(in range: ClosedRange<Double>, step: Double) -> [Double] {
        let span = range.upperBound - range.lowerBound
        guard step > 0, span > step, let increment = niceIncrement(span: span, step: step) else {
            return []
        }
        // Half an increment of clearance, so a bound never ends up with a stop crowded against it.
        let margin = increment / 2
        var stops = [range.lowerBound]
        var multiple = (range.lowerBound / increment).rounded(.down) + 1
        while true {
            let value = multiple * increment
            if value > range.upperBound - margin { break }
            if value > range.lowerBound + margin { stops.append(snapped(value, to: step)) }
            multiple += 1
        }
        stops.append(range.upperBound)
        return stops
    }

    private static func niceIncrement(span: Double, step: Double) -> Double? {
        var exponent = (log10(step)).rounded(.down)
        for _ in 0...8 {
            for mantissa in [1.0, 2.0, 2.5, 5.0] {
                let candidate = mantissa * pow(10, exponent)
                guard candidate >= step * (1 - 1e-9) else { continue }
                // Every stop has to be a value the row could already reach by dragging.
                let steps = candidate / step
                guard abs(steps - steps.rounded()) < 1e-6, span / candidate <= 8 + 1e-9 else {
                    continue
                }
                return candidate
            }
            exponent += 1
        }
        return nil
    }

    /// Multiplying a step out lands on 0.6000000000000001 often enough to be worth undoing, since
    /// that is what a drag then stores.
    private static func snapped(_ value: Double, to step: Double) -> Double {
        for places in 0...9 {
            let factor = pow(10.0, Double(places))
            let scaled = step * factor
            guard abs(scaled - scaled.rounded()) < 1e-9 else { continue }
            return (value * factor).rounded() / factor
        }
        return value
    }

    private var localizedTitle: String { settings.localized(title) }

    private var displayText: String { format.text(clamped(store.wrappedValue), settings: settings) }

    /// Baked in, because the two sliders that did this by hand could leave the counter raised for
    /// the rest of the session if the screen went away mid drag.
    private func dragChanged(_ editing: Bool) {
        guard editing != isDragging else { return }
        isDragging = editing
        if editing {
            settings.beginVisualSliderEdit()
        } else {
            settings.endVisualSliderEdit()
        }
        activity.update(localizedTitle, displayText, editing)
    }

    private func release() {
        if isDragging {
            isDragging = false
            settings.endVisualSliderEdit()
            activity.update(localizedTitle, displayText, false)
        }
        if isTyping { commit() }
    }

    /// Two steps on purpose. Focus cannot land on a field that does not exist yet, and setting
    /// both in one update is where the keyboard appears and immediately vanishes.
    /// The field always edits in Latin digits. Arabic reads as Arabic-Indic, which Double cannot
    /// parse back, so a localized field would have been impossible to type into at all.
    private static let editingLocale = Locale(identifier: "en_US_POSIX")

    private func beginTyping() {
        draft = format.digits(clamped(store.wrappedValue), locale: Self.editingLocale)
        seededDraft = draft
        isTyping = true
        DispatchQueue.main.async { fieldFocused = true }
    }

    /// Commit as they type, but only once the number is inside the range. A panel that reads its
    /// staged value on Save cannot see a draft still sitting in the keyboard, and half typed
    /// digits would otherwise clamp to the nearest bound on the way past.
    private func draftChanged() {
        guard isTyping, draft != seededDraft else { return }
        guard let parsed = format.parse(draft, locale: Self.editingLocale),
              range.contains(parsed) else { return }
        store.wrappedValue = parsed
    }

    /// Nothing unparseable is written and nothing out of range is written; the row snaps back to
    /// what it had. The silence is the point, a settings row is not the place to argue about a typo.
    ///
    /// No bracketing here. That exists to stop a reload per drag tick, and this is one write.
    private func commit() {
        isTyping = false
        // Opening the keyboard and closing it again must not change anything. The draft starts
        // as the rounded readout, so writing it back would quietly drop whatever precision the
        // stored value had beyond the decimals on show.
        guard draft != seededDraft else { return }
        guard let parsed = format.parse(draft, locale: Self.editingLocale) else { return }
        let next = clamped(parsed)
        guard next != store.wrappedValue else { return }
        store.wrappedValue = next
    }

    /// decimalPad has no minus key, which is how the old typed field ended up on
    /// numbersAndPunctuation. Texture offsets, deadzones and gradient tilt all go negative.
    private var keyboardType: UIKeyboardType {
        if range.lowerBound < 0 { return .numbersAndPunctuation }
        return format.decimals == 0 ? .numberPad : .decimalPad
    }
}

/// An empty hint still announces a pause, so only apply it when there is one.
private struct OptionalHint: ViewModifier {
    let text: String?

    @ViewBuilder
    func body(content: Content) -> some View {
        if let text {
            content.accessibilityHint(text)
        } else {
            content
        }
    }
}
