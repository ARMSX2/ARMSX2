// AudioSettingsView.swift — emulator volume and SPU2 output settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct AudioSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                NumberRow(.emulatorVolume, value: $settings.emulatorVolumePercent,
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

                NumberRow(.audioBufferMs, value: $settings.audioBufferMs, settings: settings)
                NumberRow(.audioOutputLatencyMs, value: $settings.audioOutputLatencyMs,
                          settings: settings)
                NumberRow(.fastForwardVolume, value: $settings.audioFastForwardVolume,
                          settings: settings)

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
