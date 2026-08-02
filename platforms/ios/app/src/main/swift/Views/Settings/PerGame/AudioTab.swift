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
                    // Full global range, not 0 to 100. The clamp always allowed 150, so the old
                    // slider could not reach values the setting accepts.
                    NumberRow("Emulator Volume", value: volumeBinding,
                              in: SettingsStore.emulatorVolumeRange, format: .percent,
                              accessory: NumberRowAccessory(
                                  systemImage: "arrow.uturn.backward",
                                  label: "Use the global value for %@",
                                  isVisible: true,
                                  action: {
                                      volumeOverride = false
                                      volumePercent = globalVolumePercent
                                  }),
                              hint: "Adjusts emulator audio for this game without changing iOS system volume or other apps.",
                              settings: settings)
                        .disabled(!enabled)
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
                                  range: SettingsStore.fastForwardVolumeRange, format: .percent,
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

    private var volumeBinding: Binding<Int> {
        Binding(
            get: { volumePercent },
            set: { volumePercent = Self.clampedVolume($0) }
        )
    }

    private static func clampedVolume(_ value: Int) -> Int {
        SettingsStore.clampedEmulatorVolumePercent(value)
    }

    private static func formatPercent(_ value: Int) -> String {
        "\(clampedVolume(value))%"
    }
}
