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
    /// Narrow ranges get the stepper. Queue size is fifteen values and you usually want a
    /// specific one; nobody is dragging a slider to land on exactly 6.
    enum Style {
        case stepper
        case slider
    }

    let title: String
    @Binding var value: Int
    let global: Int
    let range: ClosedRange<Int>
    var suffix: String = ""
    var style: Style = .slider
    var sentinel: Int = SettingsOptions.useGlobalID
    let settings: SettingsStore

    init(_ title: String, value: Binding<Int>, global: Int, range: ClosedRange<Int>,
         suffix: String = "", style: Style = .slider,
         sentinel: Int = SettingsOptions.useGlobalID, settings: SettingsStore) {
        self.title = title
        self._value = value
        self.global = global
        self.range = range
        self.suffix = suffix
        self.style = style
        self.sentinel = sentinel
        self.settings = settings
    }

    var body: some View {
        if value == sentinel {
            inheritRow
        } else {
            switch style {
            case .stepper: stepperRow
            case .slider: sliderRow
            }
        }
    }

    private var inheritRow: some View {
        HStack {
            Text(settings.localized(title))
            Spacer()
            Text(String(format: settings.localized("Global Default (%@)"), formatted(global)))
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
            // Seeded from the global, clamped because the global keys are not all bounded on
            // load and a hand-edited INI could hand us something outside this control's range.
            Button(settings.localized("Override")) {
                value = SettingsStore.clamped(global, to: range)
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
        }
    }

    private var stepperRow: some View {
        HStack {
            Stepper(value: $value, in: range) { titleAndValue }
            inheritButton
        }
    }

    private var sliderRow: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                titleAndValue
                inheritButton
            }
            Slider(value: Binding(
                get: { Double(value) },
                set: { value = Int($0.rounded()) }
            ), in: Double(range.lowerBound)...Double(range.upperBound))
        }
    }

    private var titleAndValue: some View {
        HStack {
            Text(settings.localized(title))
            Spacer()
            Text(formatted(value))
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
        }
    }

    private var inheritButton: some View {
        Button(settings.localized("Global")) { value = sentinel }
            .buttonStyle(.bordered)
            .controlSize(.small)
    }

    private func formatted(_ value: Int) -> String {
        "\(value)\(suffix)"
    }
}

/// Floating-point counterpart used by frame cadence. The slider retains the
/// global screen's whole-FPS behavior while exact NTSC-derived rates remain
/// available as shortcuts, and the per-game value can still inherit globally.
struct FloatOverrideRow: View {
    let title: String
    @Binding var value: Float
    let global: Float
    let range: ClosedRange<Float>
    var suffix: String = ""
    var step: Float = 1.0
    var sentinel: Float = -1.0
    var quickValues: [Float] = []
    let settings: SettingsStore

    init(_ title: String, value: Binding<Float>, global: Float,
         range: ClosedRange<Float>, suffix: String = "", step: Float = 1.0,
         sentinel: Float = -1.0, quickValues: [Float] = [],
         settings: SettingsStore) {
        self.title = title
        self._value = value
        self.global = global
        self.range = range
        self.suffix = suffix
        self.step = step
        self.sentinel = sentinel
        self.quickValues = quickValues
        self.settings = settings
    }

    var body: some View {
        if value == sentinel {
            HStack {
                Text(settings.localized(title))
                Spacer()
                Text(String(format: settings.localized("Global Default (%@)"), formatted(global)))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
                Button(settings.localized("Override")) {
                    value = min(max(global, range.lowerBound), range.upperBound)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }
        } else {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(settings.localized(title))
                    Spacer()
                    Text(formatted(value))
                        .font(.callout.monospacedDigit())
                        .foregroundStyle(.secondary)
                    Button(settings.localized("Global")) { value = sentinel }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                }

                Slider(value: $value, in: range, step: step)

                if !quickValues.isEmpty {
                    HStack {
                        ForEach(quickValues, id: \.self) { target in
                            Button(formatted(target)) {
                                value = min(max(target, range.lowerBound), range.upperBound)
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                        }
                    }
                }
            }
        }
    }

    private func formatted(_ value: Float) -> String {
        let number: String
        if abs(value - value.rounded()) < 0.001 {
            number = String(format: "%.0f", value)
        } else if abs((value * 100.0) - (value * 100.0).rounded()) < 0.001 {
            number = String(format: "%.2f", value)
        } else {
            number = String(format: "%.3f", value)
        }
        return "\(number)\(suffix)"
    }
}
