// FramePacingSettingsView.swift — consolidated Frame Pacing surface (presets + individual controls)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct FramePacingSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var presetDetailsTarget: FramePacingPreset?
    @State private var showResetConfirmation = false

    var body: some View {
        Form {
            if settings.framePacingPreset == .custom {
                Text(settings.localized("You've changed individual settings. Pick a preset to return to its values."))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section {
                ForEach(FramePacingPreset.allCases) { preset in
                    presetRow(preset)
                }
            } header: {
                Text(settings.localized("Preset"))
            }

            Section {
                frameLimiterRows

                NumberRow(.vsyncQueueSize, value: $settings.vsyncQueueSize, settings: settings)

                Toggle(settings.localized("Sync to Host Refresh"), isOn: $settings.syncToHostRefresh)
                Text(settings.localized("Sync to Host Refresh needs a restart to take effect."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.audioBufferMs, value: $settings.audioBufferMs, settings: settings)

                NumberRow(.audioOutputLatencyMs, value: $settings.audioOutputLatencyMs,
                          settings: settings)
            } header: {
                Text(settings.localized("Individual Settings"))
            }

            Section {
                Button(role: .destructive) {
                    showResetConfirmation = true
                } label: {
                    Text(settings.localized("Reset Frame Pacing to Defaults"))
                }
            }
        }
        .navigationTitle(settings.localized("Frame Pacing"))
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $presetDetailsTarget) { preset in
            PresetDetailsSheet(preset: preset)
        }
        .confirmationDialog(
            settings.localized("Reset Frame Pacing?"),
            isPresented: $showResetConfirmation,
            titleVisibility: .visible
        ) {
            Button(settings.localized("Reset"), role: .destructive) {
                settings.applyFramePacingPreset(.optimal)
                settings.framePacingPreset = .optimal
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("This restores the Optimal preset values. Your individual pacing changes are replaced."))
        }
    }

    @ViewBuilder
    private var frameLimiterRows: some View {
        Toggle(settings.localized("Enable Limiter"), isOn: $settings.frameLimiterEnabled)

        if settings.frameLimiterEnabled {
            NumberRow(.targetFPS, value: $settings.targetFPS, settings: settings)
        } else {
            Text(settings.localized("Limiter is OFF. Games can run above normal speed and may draw more power."))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func presetRow(_ preset: FramePacingPreset) -> some View {
        Button {
            settings.framePacingPreset = preset
        } label: {
            HStack(alignment: .top, spacing: 12) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(settings.localized(preset.label))
                        .font(.body)
                        .foregroundStyle(.primary)
                    Text(settings.localized(preset.caption))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Spacer(minLength: 8)

                Button {
                    presetDetailsTarget = preset
                } label: {
                    Image(systemName: "info.circle")
                }
                .accessibilityLabel(settings.localized("Preset details"))
                .buttonStyle(.borderless)

                if settings.framePacingPreset == preset {
                    Image(systemName: "checkmark")
                        .foregroundStyle(Color.accentColor)
                        .accessibilityHidden(true)
                }
            }
            .contentShape(Rectangle())
            .frame(minHeight: 44)
        }
        .buttonStyle(.plain)
    }

}

private struct PresetDetailsSheet: View {
    let preset: FramePacingPreset
    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        NavigationStack {
            ScrollView {
                Text(settings.localized(preset.details))
                    .font(.body)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding()
            }
            .navigationTitle(settings.localized(preset.label))
            .navigationBarTitleDisplayMode(.inline)
        }
        .presentationDetents([.medium])
    }
}

private extension FramePacingPreset {
    var caption: String {
        switch self {
        case .optimal:
            return "Balanced for iOS. Good smoothness with low input lag. Recommended for most games."
        case .smooth:
            return "Largest buffer and most tolerant of jitter. Adds a little input lag."
        case .lowLatency:
            return "Tighter audio and the smallest safe queue. Controls feel snappier; may stutter on heavy games."
        case .batterySaver:
            return "Caps presentation FPS to stretch battery. Game timing stays at 100%, while fast action looks less smooth."
        case .custom:
            return "Your individual pacing settings. Changing any control switches you here."
        }
    }

    var details: String {
        switch self {
        case .optimal:
            return "Tuned for iPhone and iPad. Balances a small vsync queue with safe audio latency so most games feel responsive without stutter. This is the default for fresh installs."
        case .smooth:
            return "Uses a larger vsync queue and audio buffer so the emulator can absorb frame-time spikes without crackle or stutter. Input lag goes up slightly. Good for games that stream or hitch."
        case .lowLatency:
            return "Shrinks the vsync queue and audio latency so button presses land as fast as possible on iOS. Demanding games may stutter; switch back to Optimal if they do."
        case .batterySaver:
            return "Lowers presentation FPS while CPU, audio, and game timing stay at 100%. Eligible skipped frames avoid final composition and post-processing, reducing display-side work, power use, and heat."
        case .custom:
            return "You have changed individual pacing settings. They stay exactly as you set them until you pick a preset again."
        }
    }
}
