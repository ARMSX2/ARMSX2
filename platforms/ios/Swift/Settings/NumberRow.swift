// NumberRow.swift — the one numeric control: label, tappable readout, slider between its bounds.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

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
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.controllerAccessibilityInheritedTargetID)
    private var inheritedControllerTargetID
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
            .controllerAccessibilityAdjustableTarget(
                label: localizedTitle,
                value: displayText,
                onActivate: beginTyping,
                onIncrement: { controllerAdjust(by: 1) },
                onDecrement: { controllerAdjust(by: -1) }
            )
            .onChange(of: store.wrappedValue) { _, _ in
                guard isDragging else { return }
                activity.update(localizedTitle, displayText, true)
            }
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
            Stepper("", value: clampedValue, in: range, step: effectiveStep ?? 1)
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
            // Inboard of the number, so the number stays the last thing in every row and lines
            // up with the track's upper bound underneath. Kept in the layout when hidden, or the
            // value jumps as the reset arrow comes and goes while you drag past the default.
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
        if usesDetents {
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
                .onSubmit { commitAndDismissKeyboard() }
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
                        Button(
                            settings.localized("Done"),
                            action: commitAndDismissKeyboard
                        )
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

    @ViewBuilder
    private func accessoryButton(_ accessory: NumberRowAccessory) -> some View {
        let label = settings.localized(accessory.label)
            .replacingOccurrences(of: "%@", with: localizedTitle)
        let button = Button(action: accessory.action) {
            Image(systemName: accessory.systemImage)
                .font(.caption.weight(.semibold))
                .frame(width: 28, height: 28)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .accessibilityLabel(label)

        if inheritedControllerTargetID == nil {
            button.controllerAccessibilityActionTarget(
                label: label,
                activationFeedback: .activate,
                action: accessory.action
            )
        } else {
            // The surrounding numeric row already owns the stable controller
            // ID. A second inherited probe here would replace that owner every
            // time Override remounts the row and trap directional focus.
            button
        }
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
                    guard let step = effectiveStep, step > 0 else {
                        store.wrappedValue = clamped(raw)
                        return
                    }
                    let steps = ((raw - range.lowerBound) / step).rounded()
                    store.wrappedValue = clamped(range.lowerBound + steps * step)
                })
    }

    /// Percentage tracks ending at 100 are continuous 1% controls. Detents remain useful visual
    /// suggestions for irregular ranges such as FPS, but they must not turn 0/25/50/75/100 into
    /// the only values reachable from a controller or a drag.
    private var usesOnePercentGranularity: Bool {
        guard format.isPercentage else { return false }
        let shownLowerBound = range.lowerBound * format.scale
        let shownUpperBound = range.upperBound * format.scale
        return shownLowerBound >= -0.000_001
            && shownLowerBound <= 100.000_001
            && abs(shownUpperBound - 100) <= 0.000_001
    }

    private var onePercentStep: Double { 1 / format.scale }

    private var effectiveStep: Double? {
        usesOnePercentGranularity ? onePercentStep : step
    }

    private var usesDetents: Bool {
        !usesOnePercentGranularity && !detents.isEmpty
    }

    /// The initial edge remains a precise one-step edit. Only repeat events inherit the router's
    /// smooth hold acceleration, so a long hold advances several slider increments per callback
    /// while ordinary taps never skip a value.
    private var controllerAdjustmentStepCount: Int {
        guard controllerInput?.isRepeatingDirectionCommand == true else { return 1 }
        return max(
            1,
            Int((controllerInput?.directionalRepeatAcceleration ?? 1).rounded())
        )
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

    private func controllerAdjust(by direction: Int) {
        guard isEnabled, direction != 0 else { return }
        let movement = direction * controllerAdjustmentStepCount

        if usesOnePercentGranularity {
            let next = store.wrappedValue
                + Double(movement) * onePercentStep
            store.wrappedValue = clamped(
                Self.snapped(next, to: onePercentStep)
            )
            return
        }

        if usesDetents {
            let current = clamped(store.wrappedValue)
            let currentIndex = detents.enumerated().min {
                abs($0.element - current) < abs($1.element - current)
            }?.offset ?? 0
            let nextIndex = min(
                max(currentIndex + movement, detents.startIndex),
                detents.index(before: detents.endIndex)
            )
            store.wrappedValue = detents[nextIndex]
            return
        }

        let span = range.upperBound - range.lowerBound
        let increment = step ?? max(format.displayStep, span / 20)
        store.wrappedValue = clamped(
            store.wrappedValue + Double(movement) * increment
        )
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
    /// Arabic reads as Arabic-Indic digits; keyboards type in Latin digits, so edit in Latin.
    private var editingLocale: Locale {
        settings.appLanguage.resolved == .arabic ? Locale(identifier: "en") : settings.numberLocale
    }

    private func beginTyping() {
        draft = format.digits(clamped(store.wrappedValue), locale: editingLocale)
        seededDraft = draft
        isTyping = true
        DispatchQueue.main.async { fieldFocused = true }
    }

    /// Keep the draft local until editing ends. Besides making Done deterministic,
    /// this prevents Per-Game live preview from restarting for every digit.
    /// Only drops focus; onChange(of: fieldFocused) commits. Removing the field and its keyboard bar
    /// in the same update as the focus leaves the bar's height behind as keyboard inset.
    private func commitAndDismissKeyboard() {
        fieldFocused = false
        UIApplication.shared.sendAction(
            #selector(UIResponder.resignFirstResponder),
            to: nil,
            from: nil,
            for: nil
        )
    }

    /// Nothing unparseable is written and nothing out of range is written; the row snaps back to
    /// what it had. The silence is the point, a settings row is not the place to argue about a typo.
    ///
    /// No bracketing here. That exists to stop a reload per drag tick, and this is one write.
    private func commit() {
        guard isTyping else { return }
        isTyping = false
        // Opening the keyboard and closing it again must not change anything. The draft starts
        // as the rounded readout, so writing it back would quietly drop whatever precision the
        // stored value had beyond the decimals on show.
        guard draft != seededDraft else { return }
        guard let parsed = format.parse(draft) else { return }
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
