// AudioTab.swift — Per-game Audio category tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct AudioTab: View {
    @Binding var enabled: Bool
    @Binding var volumeOverride: Bool
    @Binding var volumePercent: Int
    @Binding var globalVolumePercent: Int
    @Binding var perGameFastForwardVolume: Int

    let settings: SettingsStore

    var body: some View {
        PerGameTab(title: settings.localized("Audio")) {
            Section(settings.localized("Audio")) {
                Toggle(settings.localized("Use Custom Volume"), isOn: volumeOverrideBinding)
                    .disabled(!enabled)

                if volumeOverride {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text(settings.localized("Emulator Volume"))
                            Spacer()
                            Text(Self.formatPercent(volumePercent))
                                .foregroundStyle(.secondary)
                                .font(.callout.monospacedDigit())
                        }

                        Slider(value: volumeSliderBinding, in: 0...100, step: 1)
                            .disabled(!enabled)
                            .accessibilityLabel(settings.localized("Per-Game Emulator Volume"))
                            .accessibilityValue(Self.formatPercent(volumePercent))
                            .accessibilityHint(settings.localized("Adjusts emulator audio for this game without changing iOS system volume or other apps."))

                        HStack {
                            Text("0%")
                            Spacer()
                            Button(settings.localized("Reset to Global")) {
                                volumeOverride = false
                                volumePercent = globalVolumePercent
                            }
                            .buttonStyle(.borderless)
                            .disabled(!enabled)
                            Spacer()
                            Text("100%")
                        }
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                    }
                } else {
                    HStack {
                        Text(settings.localized("Using Global"))
                        Spacer()
                        Text(Self.formatPercent(globalVolumePercent))
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }
                }

                Text(settings.localized("Custom volume changes this game's emulator audio only. Turn it off to inherit the global Emulator Volume setting."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberOverrideRow("Fast-Forward Volume", value: $perGameFastForwardVolume,
                                  global: settings.audioFastForwardVolume,
                                  range: SettingsStore.fastForwardVolumeRange, suffix: "%",
                                  settings: settings)
                    .disabled(!enabled)
                Text(settings.localized("Buffer Size and Output Latency are on the Frame Pacing tab."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var volumeOverrideBinding: Binding<Bool> {
        Binding(
            get: { volumeOverride },
            set: { newValue in
                volumeOverride = newValue
                volumePercent = newValue ? Self.clampedVolume(volumePercent) : globalVolumePercent
            }
        )
    }

    private var volumeSliderBinding: Binding<Double> {
        Binding(
            get: { Double(volumePercent) },
            set: { volumePercent = Self.clampedVolume(Int($0.rounded())) }
        )
    }

    private static func clampedVolume(_ value: Int) -> Int {
        SettingsStore.clampedEmulatorVolumePercent(value)
    }

    private static func formatPercent(_ value: Int) -> String {
        "\(clampedVolume(value))%"
    }
}
