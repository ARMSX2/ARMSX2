// AudioSettingsView.swift — emulator volume and SPU2 output settings
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UniformTypeIdentifiers

private struct AudioPackMessage: Identifiable {
    let id = UUID()
    let title: String
    let detail: String
}

private enum AudioFilePickerDestination {
    case pack
    case custom(MenuAudioPackManager.Sound)
}

struct AudioSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var audioPack = MenuAudioPackManager.shared
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var isAudioFilePickerPresented = false
    @State private var audioFilePickerDestination: AudioFilePickerDestination?
    @State private var isRemoveConfirmationPresented = false
    @State private var audioPackMessage: AudioPackMessage?

    var body: some View {
        Form {
            Section {
                NumberRow(.emulatorVolume, value: $settings.emulatorVolumePercent,
                          settings: settings)
                    .controllerAccessibilityTargetID("settings.audio.emulator-volume")

                Text(settings.localized("Controls emulator and game audio only. iOS system volume and other apps stay separate."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Volume"))
            }

            Section {
                Toggle(settings.localized("Time Stretch"), isOn: $settings.audioTimeStretch)
                    .controllerAccessibilityToggleTarget(id: "settings.audio.time-stretch", label: settings.localized("Time Stretch"), isOn: $settings.audioTimeStretch)
                Text(settings.localized("Keeps audio in sync by stretching it during speed changes. Turn off if you hear pops or pitch issues."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.audioBufferMs, value: $settings.audioBufferMs, settings: settings)
                    .controllerAccessibilityTargetID("settings.audio.buffer")
                NumberRow(.audioOutputLatencyMs, value: $settings.audioOutputLatencyMs,
                          settings: settings)
                    .controllerAccessibilityTargetID("settings.audio.latency")
                NumberRow(.fastForwardVolume, value: $settings.audioFastForwardVolume,
                          settings: settings)
                    .controllerAccessibilityTargetID("settings.audio.fast-forward-volume")

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
                    .controllerAccessibilityToggleTarget(id: "settings.audio.swap", label: settings.localized("Left/Right Channel Swap"), isOn: $settings.audioSwapChannels)
                Text(settings.localized("Swaps the left and right channels. Fixes reversed stereo on flipped-speaker or reverse-landscape devices."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Channels"))
            }

            audioPackSection
            interfaceAudioSection
            audioResetSection
        }
        .navigationTitle(settings.localized("Audio"))
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        .navigationBarTitleDisplayMode(.inline)
        .fileImporter(
            isPresented: $isAudioFilePickerPresented,
            allowedContentTypes: audioFilePickerContentTypes,
            allowsMultipleSelection: false,
            onCompletion: handleAudioFileSelection
        )
        .confirmationDialog(
            settings.localized("Remove Audio Pack?"),
            isPresented: $isRemoveConfirmationPresented,
            titleVisibility: .visible
        ) {
            Button(settings.localized("Remove Audio Pack"), role: .destructive) {
                removeAudioPack()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("The bundled default audio pack will be restored."))
        }
        .alert(item: $audioPackMessage) { message in
            Alert(
                title: Text(message.title),
                message: Text(message.detail),
                dismissButton: .default(Text(settings.localized("OK")))
            )
        }
        .onChange(of: audioPackMessage?.id) { _, messageID in
            guard messageID != nil else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playTouchHaptics(.contextMenu)
        }
    }

    private var audioPackSection: some View {
        Section {
            Button {
                presentAudioFilePicker(.pack)
            } label: {
                HStack {
                    Label(
                        settings.localized(
                            audioPack.hasInstalledPack
                                ? "Replace Audio Pack"
                                : "Choose Audio Pack"
                        ),
                        systemImage: "archivebox"
                    )
                    Spacer()
                    if audioPack.isWorking {
                        ProgressView()
                    }
                }
            }
            .controllerAccessibilityActionTarget(
                id: "settings.audio.pack",
                label: settings.localized(audioPackActionTitle),
                activationFeedback: .silent
            ) {
                presentAudioFilePicker(.pack)
            }
            .disabled(audioPack.isWorking)

            if let installedPackName = audioPack.installedPackName {
                LabeledContent(settings.localized("Installed")) {
                    Text(installedPackName)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }

                if !audioPack.usesBundledDefault {
                    Button(settings.localized("Remove Audio Pack"), role: .destructive) {
                        presentRemoveAudioPackConfirmation()
                    }
                    .controllerAccessibilityActionTarget(
                        id: "settings.audio.remove-pack",
                        label: settings.localized("Remove Audio Pack"),
                        activationFeedback: .silent
                    ) {
                        presentRemoveAudioPackConfirmation()
                    }
                    .disabled(audioPack.isWorking)
                }
            }
        } header: {
            Text(settings.localized("Audio Pack"))
        } footer: {
            Text(settings.localized(
                "The ZIP must contain background, navigation, select, switch_toggle_on, switch_toggle_off, return, stopping_game, context_menu, achievement_toast, ui_toast, tab_transition, launch_game, and no_jit. startup is optional and plays only when the imported pack provides it. Every file may be MP3 or WAV."
            ))
        }
    }

    private var interfaceAudioSection: some View {
        Section {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(settings.localized("UI Audio"))
                    Spacer()
                    Text(volumeDescription(audioPack.uiAudioVolume))
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                Slider(
                    value: Binding(
                        get: { audioPack.uiAudioVolume },
                        set: { audioPack.setUIAudioVolume($0) }
                    ),
                    in: 0...1,
                    step: 0.01
                )
                .accessibilityLabel(settings.localized("UI Audio"))
                .accessibilityValue(volumeDescription(audioPack.uiAudioVolume))
            }
            .controllerAccessibilityAdjustableTarget(
                id: "settings.audio.ui-volume", label: settings.localized("UI Audio"),
                value: volumeDescription(audioPack.uiAudioVolume),
                onActivate: { audioPack.preview(.navigation) },
                onIncrement: {
                    audioPack.setUIAudioVolume(
                        adjustedPercentage(audioPack.uiAudioVolume, direction: 1)
                    )
                },
                onDecrement: {
                    audioPack.setUIAudioVolume(
                        adjustedPercentage(audioPack.uiAudioVolume, direction: -1)
                    )
                }
            )

            Toggle(
                settings.localized("Custom UI Audio"),
                isOn: Binding(
                    get: { audioPack.customUIAudioEnabled },
                    set: { audioPack.setCustomUIAudioEnabled($0) }
                )
            )
            .controllerAccessibilityToggleTarget(
                id: "settings.audio.custom", label: settings.localized("Custom UI Audio"),
                isOn: Binding(get: { audioPack.customUIAudioEnabled }, set: { audioPack.setCustomUIAudioEnabled($0) })
            )

            if audioPack.customUIAudioEnabled {
                ForEach(MenuAudioPackManager.Sound.allCases, id: \.rawValue) { sound in
                    customSoundRow(sound)
                }
            }
        } header: {
            Text(settings.localized("Interface Audio"))
        } footer: {
            Text(settings.localized(
                "Custom MP3 or WAV files replace individual sounds from the selected audio pack. Removing one restores the audio-pack version."
            ))
        }
    }

    private var audioResetSection: some View {
        Section {
            ConfirmedSettingsResetButton(
                settings.localized("Reset Audio to Defaults"),
                confirmationTitle: settings.localized("Reset Audio Settings?"),
                confirmationMessage: settings.localized(
                    "This restores emulator and interface audio controls. Imported audio packs and custom sound files are kept."
                ),
                completionMessage: settings.localized("Defaults Restored"),
                controllerTargetID: "settings.audio.reset"
            ) {
                settings.resetAudioDefaults()
                audioPack.resetAudioSettingsToDefaults()
            }
            .uiCriticalForegroundStyle()
        } footer: {
            Text(settings.localized(
                "Imported audio packs and custom MP3 or WAV files are not deleted."
            ))
        }
    }

    private func customSoundRow(_ sound: MenuAudioPackManager.Sound) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .center, spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(settings.localized(sound.displayName))
                    Text(sound.rawValue)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }

                Spacer(minLength: 8)

                Button {
                    previewSound(sound)
                } label: {
                    Image(systemName: "play.fill")
                        .frame(width: 24, height: 24)
                }
                .buttonStyle(.borderless)
                .controllerAccessibilityActionTarget(
                    id: Self.soundTarget(sound, "play"), label: settings.localized("Play \(sound.displayName)"),
                    activationFeedback: .silent
                ) { previewSound(sound) }
                .disabled(!audioPack.hasSound(sound))
                .accessibilityLabel(
                    settings.localized("Play \(sound.displayName)")
                )

                Button {
                    presentAudioFilePicker(.custom(sound))
                } label: {
                    Image(
                        systemName: audioPack.customSounds.contains(sound)
                            ? "doc.badge.arrow.up"
                            : "doc.badge.plus"
                    )
                    .frame(width: 24, height: 24)
                }
                .buttonStyle(.borderless)
                .controllerAccessibilityActionTarget(
                    id: Self.soundTarget(sound, "choose"),
                    label: settings.localized("Choose \(sound.displayName) Audio"),
                    activationFeedback: .silent
                ) {
                    presentAudioFilePicker(.custom(sound))
                }
                .disabled(audioPack.isWorking)
                .accessibilityLabel(
                    settings.localized("Choose \(sound.displayName) Audio")
                )

                Button(role: .destructive) {
                    removeCustomSound(sound)
                } label: {
                    Image(systemName: "trash")
                        .frame(width: 24, height: 24)
                }
                .buttonStyle(.borderless)
                .controllerAccessibilityActionTarget(
                    id: Self.soundTarget(sound, "delete"), label: settings.localized("Delete \(sound.displayName) Audio")
                ) { removeCustomSound(sound) }
                .disabled(
                    audioPack.isWorking || !audioPack.customSounds.contains(sound)
                )
                .accessibilityLabel(
                    settings.localized("Delete \(sound.displayName) Audio")
                )
            }

            HStack(spacing: 10) {
                Image(systemName: "speaker.wave.1")
                    .foregroundStyle(.secondary)
                Slider(
                    value: Binding(
                        get: { audioPack.volume(for: sound) },
                        set: { audioPack.setVolume($0, for: sound) }
                    ),
                    in: 0...1,
                    step: 0.01
                )
                .accessibilityLabel(
                    settings.localized("\(sound.displayName) Volume")
                )
                .accessibilityValue(
                    volumeDescription(audioPack.volume(for: sound))
                )
                Text(volumeDescription(audioPack.volume(for: sound)))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
                    .frame(width: 42, alignment: .trailing)
            }
            .controllerAccessibilityAdjustableTarget(
                id: Self.soundTarget(sound, "volume"), label: settings.localized("\(sound.displayName) Volume"),
                value: volumeDescription(audioPack.volume(for: sound)),
                onActivate: { previewSound(sound) },
                onIncrement: {
                    audioPack.setVolume(
                        adjustedPercentage(
                            audioPack.volume(for: sound),
                            direction: 1
                        ),
                        for: sound
                    )
                },
                onDecrement: {
                    audioPack.setVolume(
                        adjustedPercentage(
                            audioPack.volume(for: sound),
                            direction: -1
                        ),
                        for: sound
                    )
                }
            )
        }
        .padding(.vertical, 2)
    }

    private static func soundTarget(_ sound: MenuAudioPackManager.Sound, _ action: String) -> String {
        "settings.audio.\(sound.rawValue).\(action)"
    }

    /// Includes lazy rows before they mount, with distinct IDs for every
    /// sound's play/file/delete/volume actions. Disabled actions are omitted.
    static var controllerTargetOrder: [String] {
        let audioPack = MenuAudioPackManager.shared
        var order = ["settings.audio.emulator-volume", "settings.audio.time-stretch",
                     "settings.audio.buffer", "settings.audio.latency",
                     "settings.audio.fast-forward-volume", "settings.audio.swap"]
        if !audioPack.isWorking {
            order.append("settings.audio.pack")
            if audioPack.installedPackName != nil, !audioPack.usesBundledDefault {
                order.append("settings.audio.remove-pack")
            }
        }
        order += ["settings.audio.ui-volume", "settings.audio.custom"]
        if audioPack.customUIAudioEnabled {
            for sound in MenuAudioPackManager.Sound.allCases {
                if audioPack.hasSound(sound) { order.append(soundTarget(sound, "play")) }
                if !audioPack.isWorking {
                    order.append(soundTarget(sound, "choose"))
                    if audioPack.customSounds.contains(sound) { order.append(soundTarget(sound, "delete")) }
                }
                order.append(soundTarget(sound, "volume"))
            }
        }
        order.append("settings.audio.reset")
        return order
    }

    private var audioPackActionTitle: String {
        audioPack.hasInstalledPack ? "Replace Audio Pack" : "Choose Audio Pack"
    }

    private var audioFilePickerContentTypes: [UTType] {
        switch audioFilePickerDestination {
        case .pack:
            [.zip]
        case .custom:
            [.mp3, .wav]
        case nil:
            [.zip, .mp3, .wav]
        }
    }

    private func presentAudioFilePicker(_ destination: AudioFilePickerDestination) {
        guard !audioPack.isWorking else { return }
        MenuAudioPackManager.shared.playTouchContextMenu()
        controllerInput?.playTouchHaptics(.contextMenu)
        audioFilePickerDestination = destination
        isAudioFilePickerPresented = true
    }

    private func presentRemoveAudioPackConfirmation() {
        MenuAudioPackManager.shared.playEvent(.uiToast)
        controllerInput?.playTouchHaptics(.contextMenu)
        isRemoveConfirmationPresented = true
    }

    private func previewSound(_ sound: MenuAudioPackManager.Sound) {
        controllerInput?.playTouchHaptics(.activate)
        audioPack.preview(sound)
    }

    private func handleAudioFileSelection(_ result: Result<[URL], Error>) {
        let destination = audioFilePickerDestination
        audioFilePickerDestination = nil

        switch destination {
        case .pack:
            handleAudioPackSelection(result)
        case .custom(let sound):
            handleCustomSoundSelection(result, sound: sound)
        case nil:
            return
        }
    }

    private func handleAudioPackSelection(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            Task { @MainActor in
                do {
                    try await audioPack.importPack(from: url)
                    audioPackMessage = AudioPackMessage(
                        title: settings.localized("Audio Pack Installed"),
                        detail: settings.localized("The UI audio pack is ready to use.")
                    )
                } catch {
                    audioPackMessage = AudioPackMessage(
                        title: settings.localized("Audio Pack Could Not Be Installed"),
                        detail: error.localizedDescription
                    )
                }
            }
        case .failure(let error):
            let cocoaError = error as NSError
            guard !(cocoaError.domain == NSCocoaErrorDomain
                    && cocoaError.code == CocoaError(.userCancelled).errorCode) else { return }
            audioPackMessage = AudioPackMessage(
                title: settings.localized("Audio Pack Could Not Be Installed"),
                detail: error.localizedDescription
            )
        }
    }

    private func handleCustomSoundSelection(
        _ result: Result<[URL], Error>,
        sound: MenuAudioPackManager.Sound
    ) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            Task { @MainActor in
                do {
                    try await audioPack.importCustomSound(from: url, for: sound)
                } catch {
                    audioPackMessage = AudioPackMessage(
                        title: settings.localized("UI Audio Could Not Be Imported"),
                        detail: error.localizedDescription
                    )
                }
            }
        case .failure(let error):
            let cocoaError = error as NSError
            guard !(cocoaError.domain == NSCocoaErrorDomain
                    && cocoaError.code == CocoaError(.userCancelled).errorCode) else { return }
            audioPackMessage = AudioPackMessage(
                title: settings.localized("UI Audio Could Not Be Imported"),
                detail: error.localizedDescription
            )
        }
    }

    private func removeCustomSound(_ sound: MenuAudioPackManager.Sound) {
        Task { @MainActor in
            do {
                try await audioPack.removeCustomSound(sound)
            } catch {
                audioPackMessage = AudioPackMessage(
                    title: settings.localized("UI Audio Could Not Be Removed"),
                    detail: error.localizedDescription
                )
            }
        }
    }

    private func volumeDescription(_ volume: Double) -> String {
        "\(Int((volume * 100).rounded()))%"
    }

    /// Every 0...100% audio track moves by one percent on a tap. Sustained directional input
    /// inherits the shared repeat curve, increasing the number of one-percent steps without
    /// sacrificing precise single presses.
    private func adjustedPercentage(
        _ currentValue: Double,
        direction: Int
    ) -> Double {
        let stepCount: Int
        if controllerInput?.isRepeatingDirectionCommand == true {
            stepCount = max(
                1,
                Int((controllerInput?.directionalRepeatAcceleration ?? 1).rounded())
            )
        } else {
            stepCount = 1
        }
        let next = currentValue + Double(direction * stepCount) * 0.01
        return min(1, max(0, (next * 100).rounded() / 100))
    }

    private func removeAudioPack() {
        Task { @MainActor in
            do {
                try await audioPack.removeInstalledPack()
            } catch {
                audioPackMessage = AudioPackMessage(
                    title: settings.localized("Audio Pack Could Not Be Removed"),
                    detail: error.localizedDescription
                )
            }
        }
    }
}

private extension MenuAudioPackManager.Sound {
    var displayName: String {
        switch self {
        case .startup:
            return "Startup"
        case .background:
            return "Background"
        case .pauseMusic:
            return "Pause Music"
        case .navigation:
            return "Navigation"
        case .select:
            return "Select"
        case .switchToggleOn:
            return "Switch Toggle On"
        case .switchToggleOff:
            return "Switch Toggle Off"
        case .return:
            return "Return"
        case .stoppingGame:
            return "Stopping Game"
        case .contextMenu:
            return "Context Menu"
        case .achievementToast:
            return "Achievement Toast"
        case .uiToast:
            return "UI Toast"
        case .tabTransition:
            return "Tab Transition"
        case .launchGame:
            return "Launch Game"
        case .noJIT:
            return "No JIT"
        }
    }
}
