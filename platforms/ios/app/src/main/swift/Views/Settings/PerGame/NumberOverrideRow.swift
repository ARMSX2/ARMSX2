// NumberOverrideRow.swift — Per-game number with a use-global state.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Lets the parent live-preview transaction distinguish an explicit
/// "Override" activation from an actual value adjustment. The inherited
/// value is copied into the editor by the button, but that bookkeeping action
/// must not interrupt gameplay for a preview.
private struct PerGameOverrideWillActivateKey: EnvironmentKey {
    // SwiftUI reads and invokes this value from the main-actor view tree. The
    // unsafe nonisolated spelling is limited to the immutable empty default;
    // it avoids requiring every UI callback to pretend it is cross-actor
    // Sendable.
    nonisolated(unsafe) static let defaultValue: () -> Void = {}
}

extension EnvironmentValues {
    var perGameOverrideWillActivate: () -> Void {
        get { self[PerGameOverrideWillActivateKey.self] }
        set { self[PerGameOverrideWillActivateKey.self] = newValue }
    }
}

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
    @Environment(\.perGameOverrideWillActivate)
    private var perGameOverrideWillActivate

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
                applyOverride()
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
        }
        .contentShape(Rectangle())
        .controllerAccessibilityActionTarget(
            label: overrideControllerLabel,
            activationFeedback: .activate,
            action: applyOverride
        )
    }

    private var overrideControllerLabel: String {
        "\(settings.localized(setting.title)), \(settings.localized("Override"))"
    }

    private func applyOverride() {
        perGameOverrideWillActivate()
        value = SettingsStore.clamped(global, to: setting.intRange)
    }

    private func formatted(_ value: Int) -> String {
        setting.format.text(Double(value), settings: settings)
    }
}

/// Floating-point counterpart used by frame cadence. It renders the same
/// NumberSetting as the global screen while retaining the per-game inherit state.
struct FloatOverrideRow: View {
    let setting: NumberSetting
    @Binding var value: Float
    let global: Float
    var sentinel: Float = -1.0
    let settings: SettingsStore
    @Environment(\.perGameOverrideWillActivate)
    private var perGameOverrideWillActivate

    init(_ setting: NumberSetting, value: Binding<Float>, global: Float,
         sentinel: Float = -1.0, settings: SettingsStore) {
        self.setting = setting
        self._value = value
        self.global = global
        self.sentinel = sentinel
        self.settings = settings
    }

    var body: some View {
        if value == sentinel {
            HStack {
                Text(settings.localized(setting.title))
                Spacer()
                Text(String(format: settings.localized("Global Default (%@)"), formatted(global)))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
                Button(settings.localized("Override")) {
                    applyOverride()
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }
            .contentShape(Rectangle())
            .controllerAccessibilityActionTarget(
                label: overrideControllerLabel,
                activationFeedback: .activate,
                action: applyOverride
            )
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

    private func formatted(_ value: Float) -> String {
        setting.format.text(Double(value), settings: settings)
    }

    private var overrideControllerLabel: String {
        "\(settings.localized(setting.title)), \(settings.localized("Override"))"
    }

    private func applyOverride() {
        perGameOverrideWillActivate()
        value = Float(
            min(
                max(Double(global), setting.range.lowerBound),
                setting.range.upperBound
            )
        )
    }
}
