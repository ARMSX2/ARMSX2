// GeneralTab.swift — Per-game General category tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Landscape shows these three sections as their own category; portrait puts them at the
/// top of the panel's root form above the category links. Same content either way, so the
/// sections live here once. They used to be written out twice and had already drifted.
struct GeneralTab: View {
    @Binding var enabled: Bool
    @Binding var hasGameSettingsIdentity: Bool
    @Binding var showResetAllConfirmation: Bool
    @Binding var statusMessage: String?

    let displayName: String
    let hasPendingChanges: Bool
    let savesToRunningGame: Bool
    let game: ISOEntry
    let settings: SettingsStore

    var body: some View {
        PerGameTab(title: settings.localized("General")) {
            PerGameIdentitySection(
                enabled: enabled,
                displayName: displayName,
                hasPendingChanges: hasPendingChanges,
                savesToRunningGame: savesToRunningGame,
                game: game,
                settings: settings
            )
            PerGameOverridesSection(
                enabled: $enabled,
                showResetAllConfirmation: $showResetAllConfirmation,
                hasGameSettingsIdentity: hasGameSettingsIdentity,
                savesToRunningGame: savesToRunningGame,
                settings: settings
            )
            PerGameLivePreviewSection(
                savesToRunningGame: savesToRunningGame,
                settings: settings
            )
            PerGameStatusSection(statusMessage: statusMessage, settings: settings)
        }
    }
}

struct PerGameLivePreviewSection: View {
    let savesToRunningGame: Bool
    let settings: SettingsStore

    private var previewEnabled: Binding<Bool> {
        Binding(
            get: { settings.temporalSaveStateToLivePreviewChanges },
            set: { settings.temporalSaveStateToLivePreviewChanges = $0 }
        )
    }

    private var beforeDuration: Binding<Double> {
        Binding(
            get: { settings.perGameBeforeChangesPreviewDuration },
            set: { settings.perGameBeforeChangesPreviewDuration = $0 }
        )
    }

    private var stopsWithCircle: Binding<Bool> {
        Binding(
            get: { settings.perGameLivePreviewStopsWithCircle },
            set: { settings.perGameLivePreviewStopsWithCircle = $0 }
        )
    }

    var body: some View {
        Section {
            Toggle(
                settings.localized("Preview Changes In-Game"),
                isOn: previewEnabled
            )
            .controllerAccessibilityToggleTarget(
                id: "per-game.general.live-preview",
                label: settings.localized("Preview Changes In-Game"),
                isOn: previewEnabled
            )

            if settings.temporalSaveStateToLivePreviewChanges {
                Toggle(
                    settings.localized("Press Circle to Stop Live Preview"),
                    isOn: stopsWithCircle
                )
                .controllerAccessibilityToggleTarget(
                    id: "per-game.general.live-preview-circle-exit",
                    label: settings.localized(
                        "Press Circle to Stop Live Preview"
                    ),
                    isOn: stopsWithCircle
                )

                durationRow(
                    title: "Original View Duration",
                    value: beforeDuration,
                    range: SettingsStore.perGameBeforeChangesPreviewDurationRange,
                    id: "per-game.general.before-changes-preview-duration"
                )
            }

            Text(settings.localized(savesToRunningGame
                ? "When a setting changes, the panel hides and keeps the changed result visible. When Circle-only exit is enabled, Left and Right continue changing the focused value and Circle returns to the panel. Original View Duration can show the unchanged scene first; set it to 0s for an immediate preview. ARMSX2 then restores the same gameplay moment using a private temporary state that never occupies a save slot."
                : "Live previews are available when Per-Game Settings is opened while a game is running."))
                .font(.caption)
                .foregroundStyle(.secondary)
        } header: {
            Text(settings.localized("Live Preview"))
        }
    }

    private func durationRow(
        title: String,
        value: Binding<Double>,
        range: ClosedRange<Double>,
        id: String
    ) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(settings.localized(title))
                Spacer()
                Text("\(Int(value.wrappedValue))s")
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(value: value, in: range, step: 1)
        }
        .controllerAccessibilityAdjustableTarget(
            id: id,
            label: settings.localized(title),
            value: "\(Int(value.wrappedValue)) seconds",
            onActivate: {},
            onIncrement: { value.wrappedValue = min(range.upperBound, value.wrappedValue + 1) },
            onDecrement: { value.wrappedValue = max(range.lowerBound, value.wrappedValue - 1) }
        )
    }
}

struct PerGameIdentitySection: View {
    let enabled: Bool
    let displayName: String
    let hasPendingChanges: Bool
    let savesToRunningGame: Bool
    let game: ISOEntry
    let settings: SettingsStore

    var body: some View {
        Section {
            HStack(spacing: 12) {
                Image(systemName: enabled ? "slider.horizontal.3" : "power")
                    .font(.title3)
                    .foregroundStyle(enabled ? Color.accentColor : Color.secondary)
                    .frame(width: 32, height: 32)
                    .background(Color.accentColor.opacity(enabled ? 0.14 : 0), in: Circle())

                VStack(alignment: .leading, spacing: 4) {
                    Text(displayName)
                        .font(.headline)
                        .lineLimit(2)
                    if let serial = game.metadata["serial"], !serial.isEmpty {
                        Text("\(serial)  ·  CRC \(PadLayoutGameIdentity.normalizedCRC(game.metadata["crc"] ?? ""))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                    Text(pendingChangesCaption)
                        .font(.caption)
                        .foregroundStyle(hasPendingChanges ? Color.accentColor : Color.secondary)
                }
            }
            .padding(.vertical, 4)
        }
    }

    private var pendingChangesCaption: String {
        guard hasPendingChanges else { return settings.localized("No pending changes.") }
        return savesToRunningGame
            ? settings.localized("Unsaved changes — tap Save to apply now.")
            : settings.localized("Unsaved changes — Save to apply on next boot.")
    }
}

struct PerGameOverridesSection: View {
    @Binding var enabled: Bool
    @Binding var showResetAllConfirmation: Bool

    let hasGameSettingsIdentity: Bool
    let savesToRunningGame: Bool
    let settings: SettingsStore

    var body: some View {
        Section {
            Toggle(settings.localized("Use Per-Game Overrides"), isOn: $enabled)
                .accessibilityIdentifier("per-game.use-overrides")
                .controllerAccessibilityToggleTarget(
                    id: "per-game.general.use-overrides",
                    label: settings.localized("Use Per-Game Overrides"),
                    isOn: $enabled
                )
            Text(settings.localized(savesToRunningGame
                ? "Overrides are saved for this game only and apply when you save, while the game runs."
                : "Overrides are saved for this game only and apply on the next boot of this title."))
                .font(.caption)
                .foregroundStyle(.secondary)
            if !hasGameSettingsIdentity {
                Text(settings.localized("Start this game once before saving its settings."))
                    .font(.caption)
                    .foregroundStyle(OverlayTheme.warm)
            }
            Button(role: .destructive) {
                showResetAllConfirmation = true
            } label: {
                Label(settings.localized("Reset All Overrides"), systemImage: "arrow.counterclockwise")
            }
            .accessibilityIdentifier("per-game.reset-all-overrides")
            .controllerAccessibilityActionTarget(
                id: "per-game.general.reset-all",
                label: settings.localized("Reset All Overrides")
            ) {
                showResetAllConfirmation = true
            }
            .disabled(!hasGameSettingsIdentity)
        }
    }
}

struct PerGameStatusSection: View {
    let statusMessage: String?
    let settings: SettingsStore

    var body: some View {
        if let statusMessage {
            Section {
                Text(statusMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
