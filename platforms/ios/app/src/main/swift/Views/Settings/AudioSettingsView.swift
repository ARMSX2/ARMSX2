// AudioSettingsView.swift — emulator volume and SPU2 output settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct AudioSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                NumberRow("Emulator Volume", value: $settings.emulatorVolumePercent,
                          in: SettingsStore.emulatorVolumeRange, format: .percent,
                          default: SettingsStore.defaultEmulatorVolumePercent,
                          hint: "Adjusts emulator game audio without changing iOS system volume or other apps.",
                          settings: settings)

                Text(settings.localized("Controls emulator and game audio only. iOS system volume and other apps stay separate."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Volume"))
            }

            Section {
                Toggle(settings.localized("Time Stretch"), isOn: $settings.audioTimeStretch)
                Text(settings.localized("Keeps audio in sync by stretching it during speed changes. Turn off if you hear pops or pitch issues."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow("Buffer Size", value: $settings.audioBufferMs,
                          in: SettingsStore.audioBufferMsRange, format: .milliseconds,
                          default: 50, settings: settings)
                NumberRow("Output Latency", value: $settings.audioOutputLatencyMs,
                          in: SettingsStore.audioOutputLatencyMsRange, format: .milliseconds,
                          default: 20, settings: settings)
                NumberRow("Fast-Forward Volume", value: $settings.audioFastForwardVolume,
                          in: SettingsStore.fastForwardVolumeRange, format: .percent,
                          default: 100, settings: settings)

                Text(settings.localized("Lower buffer or latency reduces lag but can cause crackling. Fast-forward volume is a percentage of normal volume used while fast-forwarding."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Audio Output"))
            } footer: {
                Text(settings.localized("Audio output changes apply without restarting the game."))
            }

            Section {
                Toggle(settings.localized("Left/Right Channel Swap"), isOn: $settings.audioSwapChannels)
                Text(settings.localized("Swaps the left and right channels. Fixes reversed stereo on flipped-speaker or reverse-landscape devices."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Channels"))
            }
        }
        .navigationTitle(settings.localized("Audio"))
        .navigationBarTitleDisplayMode(.inline)
    }
}
