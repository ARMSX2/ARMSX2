// FixesTab.swift — Per-game Fixes & Compatibility category tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct FixesTab: View {
    @Binding var enabled: Bool
    @Binding var perGameAAT: Int
    @Binding var perGameTextureInsideRt: Int
    @Binding var perGameFixes: [String: Int]

    let savesToRunningGame: Bool
    let settings: SettingsStore

    var body: some View {
        PerGameTab(title: settings.localized("Fixes & Compatibility")) {
            Section {
                Picker(settings.localized("Accurate Alpha Test"), selection: $perGameAAT) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("On")).tag(1)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.fixes.accurate-alpha-test",
                    label: settings.localized("Accurate Alpha Test"),
                    selection: $perGameAAT,
                    options: triStateOptions
                )
                .disabled(!enabled)
                Text(settings.localized("Improves the accuracy of transparency and alpha-blended edges. Leave Off unless a game shows halos or broken transparency on Metal. " + (savesToRunningGame ? "Applies when you save." : "Applies on next boot.")))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Picker(settings.localized("Texture Inside RT"), selection: $perGameTextureInsideRt) {
                    ForEach(SettingsOptions.withUseGlobal(SettingsOptions.textureInsideRT), id: \.id) { option in
                        Text(settings.localized(option.title)).tag(option.id)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.fixes.texture-inside-rt",
                    label: settings.localized("Texture Inside RT"),
                    selection: $perGameTextureInsideRt,
                    options: SettingsOptions.withUseGlobal(
                        SettingsOptions.textureInsideRT
                    ).map {
                        (id: $0.id, title: settings.localized($0.title))
                    }
                )
                .disabled(!enabled)
                Text(settings.localized("Fixes games that render into areas of the framebuffer they later read back as textures (common half-screen or garbled-graphics fixes). " + (savesToRunningGame ? "Applies when you save." : "Applies on next boot.")))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                ForEach(SettingsStore.gameFixOptions) { option in
                    Picker(settings.localized(option.label), selection: Binding(
                        get: { perGameFixes[option.key] ?? -1 },
                        set: { perGameFixes[option.key] = $0 }
                    )) {
                        Text(settings.localized("Use Global")).tag(-1)
                        Text(settings.localized("Off")).tag(0)
                        Text(settings.localized("On")).tag(1)
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "per-game.fixes.\(option.key)",
                        label: settings.localized(option.label),
                        selection: fixBinding(for: option.key),
                        options: triStateOptions
                    )
                    .disabled(!enabled)
                    if option.key == "SkipMPEGHack" {
                        Text(settings.localized("Skip MPEG is a last-resort FMV hack that can break interactive cutscenes. Best set per-game."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            } header: {
                Text(settings.localized("Compatibility Overrides"))
            } footer: {
                Text(settings.localized("Override global settings for this game only. Game fixes apply while per-game GameDB Core Fixes is on. " + (savesToRunningGame ? "Changes apply when you save." : "Changes apply on next boot.")))
            }
        }
    }

    private var triStateOptions: [(id: Int, title: String)] {
        [
            (-1, settings.localized("Use Global")),
            (0, settings.localized("Off")),
            (1, settings.localized("On")),
        ]
    }

    private func fixBinding(for key: String) -> Binding<Int> {
        Binding(
            get: { perGameFixes[key] ?? -1 },
            set: { perGameFixes[key] = $0 }
        )
    }
}
