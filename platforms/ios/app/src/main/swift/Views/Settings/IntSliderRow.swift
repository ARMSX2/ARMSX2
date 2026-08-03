// IntSliderRow.swift — Labeled integer slider used by the global settings screens.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Slider with the value in the header, the bounds underneath, and a reset in the middle.
/// Lived in AudioSettingsView until Frame Pacing needed the same rows for the same keys.
struct IntSliderRow: View {
    let title: String
    @Binding var value: Int
    let range: ClosedRange<Int>
    var suffix: String = ""
    let defaultValue: Int
    let settings: SettingsStore

    init(_ title: String, value: Binding<Int>, range: ClosedRange<Int>,
         suffix: String = "", defaultValue: Int, settings: SettingsStore) {
        self.title = title
        self._value = value
        self.range = range
        self.suffix = suffix
        self.defaultValue = defaultValue
        self.settings = settings
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(settings.localized(title))
                Spacer()
                Text(formatted(value))
                    .foregroundStyle(.secondary)
                    .font(.callout.monospacedDigit())
            }
            Slider(value: Binding(
                get: { Double(value) },
                set: { value = Int($0.rounded()) }
            ), in: Double(range.lowerBound)...Double(range.upperBound))
            HStack {
                Text(formatted(range.lowerBound))
                Spacer()
                Button(settings.localized("Reset")) { value = defaultValue }
                    .buttonStyle(.borderless)
                Spacer()
                Text(formatted(range.upperBound))
            }
            .font(.caption.monospacedDigit())
            .foregroundStyle(.secondary)
        }
    }

    private func formatted(_ value: Int) -> String {
        "\(value)\(suffix)"
    }
}
