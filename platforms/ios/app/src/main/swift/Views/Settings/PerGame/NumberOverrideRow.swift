// NumberOverrideRow.swift — Per-game number with a use-global state.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// A per-game number that can fall back to the global one. The inherit row names the value it
/// inherits and Override starts you there, neither of which the Shade Boost row this replaces
/// did: that one just said "Use Global" and seeded a hardcoded 50.
///
/// Replaces the per-game pickers that offered a handful of values while the global screen took
/// any of them, which is why an off-list value from a preset used to render as a blank row.
struct NumberOverrideRow: View {
    let setting: NumberSetting
    @Binding var value: Int
    let global: Int
    var sentinel: Int = SettingsOptions.useGlobalID
    let settings: SettingsStore

    init(_ setting: NumberSetting, value: Binding<Int>, global: Int,
         sentinel: Int = SettingsOptions.useGlobalID, settings: SettingsStore) {
        self.setting = setting
        self._value = value
        self.global = global
        self.sentinel = sentinel
        self.settings = settings
    }

    var body: some View {
        if value == sentinel {
            inheritRow
        } else {
            NumberRow(setting, value: $value,
                      accessory: NumberRowAccessory(
                          systemImage: "arrow.uturn.backward",
                          label: "Use the global value for %@",
                          isVisible: true,
                          action: { value = sentinel }),
                      settings: settings)
        }
    }

    private var inheritRow: some View {
        HStack {
            Text(settings.localized(setting.title))
            Spacer()
            Text(String(format: settings.localized("Global Default (%@)"), formatted(global)))
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
            // Seeded from the global, clamped because the global keys are not all bounded on
            // load and a hand-edited INI could hand us something outside this control's range.
            Button(settings.localized("Override")) {
                value = SettingsStore.clamped(global, to: setting.intRange)
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
        }
    }

    private func formatted(_ value: Int) -> String {
        setting.format.text(Double(value), settings: settings)
    }
}
