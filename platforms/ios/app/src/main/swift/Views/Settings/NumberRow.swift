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

    /// Digits only. This is what the keyboard edits and what goes into the unit template.
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

/// Set once per surface, so the call sites do not each carry fonts and padding.
struct NumberRowStyle {
    var titleFont: Font = .body
    var valueFont: Font = .callout.monospacedDigit()
    var boundsFont: Font = .caption.monospacedDigit()
    var spacing: CGFloat = 8
    var verticalPadding: CGFloat = 4
    var allowsTypedEntry = true

    static let form = NumberRowStyle()
    static let compact = NumberRowStyle(
        titleFont: .caption.weight(.semibold),
        valueFont: .caption.monospacedDigit(),
        boundsFont: .caption2.monospacedDigit(),
        spacing: 6,
        verticalPadding: 0
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
    private let format: NumberFormat
    private let style: Style
    private let accessory: NumberRowAccessory?
    private let hint: String?
    private let settings: SettingsStore

    @Environment(\.numberRowStyle) private var rowStyle
    @Environment(\.numberRowActivity) private var activity
    @State private var isDragging = false
    @State private var isTyping = false
    @State private var draft = ""
    @State private var seededDraft = ""
    @FocusState private var fieldFocused: Bool

    init(_ title: String,
         value: Binding<Double>,
         in range: ClosedRange<Double>,
         format: NumberFormat = .plain,
         step: Double? = nil,
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
        self.format = format
        self.style = style
        self.hint = hint
        self.settings = settings
        // An explicit accessory wins; `default:` is sugar for the common one.
        let slack = (range.upperBound - range.lowerBound) * 1e-6
        self.accessory = accessory ?? defaultValue.map { fallback in
            NumberRowAccessory(
                systemImage: "arrow.counterclockwise",
                label: "Reset %@",
                isVisible: abs(value.wrappedValue - fallback) > slack,
                action: { value.wrappedValue = fallback }
            )
        }
    }

    init(_ title: String,
         value: Binding<Int>,
         in range: ClosedRange<Int>,
         format: NumberFormat = .plain,
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
        case .slider:
            VStack(alignment: .leading, spacing: rowStyle.spacing) {
                header
                track
            }
        case .stepper:
            // One line by design. A stepper has no track to hang bounds off, and the ranges that
            // earn a stepper are short enough to read off the value.
            Stepper(value: clampedValue, in: range, step: step ?? 1) { header }
        case .field:
            header
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            label
            Spacer(minLength: 8)
            readout
            if let accessory, accessory.isVisible {
                accessoryButton(accessory)
            }
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
        if let step {
            Slider(value: clampedValue,
                   in: range,
                   step: step,
                   label: { Text(localizedTitle) },
                   minimumValueLabel: { bound(range.lowerBound) },
                   maximumValueLabel: { bound(range.upperBound) },
                   onEditingChanged: dragChanged)
        } else {
            Slider(value: clampedValue,
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
                .frame(minWidth: 56, maxWidth: 132)
                .toolbar {
                    // Declared inside the editing branch, so the bar can only come from the row
                    // you are actually in rather than from all 275 at once.
                    ToolbarItemGroup(placement: .keyboard) {
                        Spacer()
                        Button(settings.localized("Done")) { fieldFocused = false }
                    }
                }
        } else if rowStyle.allowsTypedEntry && format.isTypeable {
            Text(displayText)
                .font(rowStyle.valueFont)
                .foregroundStyle(.secondary)
                .contentShape(Rectangle())
                .onTapGesture(perform: beginTyping)
                .accessibilityLabel(localizedTitle)
                .accessibilityValue(displayText)
                .accessibilityHint(settings.localized("Double tap to type a value."))
                .accessibilityAddTraits(.isButton)
        } else {
            Text(displayText)
                .font(rowStyle.valueFont)
                .foregroundStyle(.secondary)
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

    private func clamped(_ value: Double) -> Double {
        guard value.isFinite else { return range.lowerBound }
        return min(max(value, range.lowerBound), range.upperBound)
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
