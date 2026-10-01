// SPDX-License-Identifier: GPL-3.0+
import SwiftUI

// Localize the palette catalogue once per language, not once for
// each colour row on every slider/focus update. Only value data is retained.
@MainActor
enum AppearancePaletteOptions {
    private static var language: AppLanguage?
    private static var required: [(id: ThemePalette, title: String)] = []
    private static var optional: [(id: ThemePalette?, title: String)] = []

    private static func prepare(_ selectedLanguage: AppLanguage) {
        guard language != selectedLanguage else { return }
        language = selectedLanguage
        required = ThemePalette.allCases.map { ($0, selectedLanguage.localized($0.title)) }
        optional = required.map { (Optional($0.id), $0.title) }
    }

    static func palettes(_ language: AppLanguage) -> [(id: ThemePalette, title: String)] {
        prepare(language)
        return required
    }

    static func optionalPalettes(_ language: AppLanguage, emptyTitle: String) -> [(id: ThemePalette?, title: String)] {
        prepare(language)
        return [(nil, language.localized(emptyTitle))] + optional
    }
}

/// One picker target plus an explicit custom-colour action.
/// Step buttons stay touch-only so each row has predictable controller order.
struct AppearanceColourPickerRow<Value: Hashable>: View {
    let title: String
    let id: String
    @Binding var selection: Value
    let options: [(id: Value, title: String)]
    let preview: Color
    let addCustom: (() -> Void)?

    private var selectedIndex: Int {
        options.firstIndex { $0.id == selection } ?? 0
    }

    var body: some View {
        HStack(spacing: 2) {
            Picker(selection: $selection) {
                ForEach(options, id: \.id) { option in
                    Text(option.title).tag(option.id)
                }
            } label: {
                HStack(spacing: 8) {
                    Circle().fill(preview).frame(width: 12, height: 12)
                        .accessibilityHidden(true)
                    Text(title)
                }
            }
            .pickerStyle(.menu)
            .buttonStyle(.plain)
            .frame(maxWidth: .infinity, alignment: .leading)
            .controllerAccessibilityOptionsPickerTarget(
                id: id, label: title,
                selection: $selection, options: options
            )
            SettingsValueStepButtons(
                previousAccessibilityLabel: "Previous " + title,
                nextAccessibilityLabel: "Next " + title,
                canSelectPrevious: selectedIndex > 0,
                canSelectNext: selectedIndex + 1 < options.count,
                selectPrevious: { move(-1) },
                selectNext: { move(1) }
            )
            if let addCustom {
                Button(action: addCustom) {
                    Image(systemName: "plus")
                        .frame(width: 36, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Add Custom " + title)
                .controllerAccessibilityActionTarget(
                    id: id + ".custom", label: "Add Custom " + title,
                    action: addCustom
                )
                .controllerAccessibilityTargetID(id + ".custom")
            }
        }
    }

    private func move(_ delta: Int) {
        let next = selectedIndex + delta
        guard options.indices.contains(next) else { return }
        selection = options[next].id
        UISelectionFeedbackGenerator().selectionChanged()
    }
}
