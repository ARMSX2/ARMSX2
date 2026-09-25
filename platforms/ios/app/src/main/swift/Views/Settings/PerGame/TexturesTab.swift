// TexturesTab.swift — one game's texture packs and replacement overrides
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct TexturesTab: View {
    @Binding var enabled: Bool
    @Binding var perGameLoadTextureReplacements: Int
    @Binding var perGameLoadTextureReplacementsAsync: Int
    @Binding var perGamePrecacheTextureReplacements: Int

    let serial: String
    let settings: SettingsStore

    var body: some View {
        TexturePacksView(serial: serial) {
            Section(settings.localized("Texture Replacement")) {
                Picker(settings.localized("Load Replacement Textures"), selection: $perGameLoadTextureReplacements) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("On")).tag(1)
                }
                .disabled(!enabled)
                Picker(settings.localized("Async Loading"), selection: $perGameLoadTextureReplacementsAsync) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("On")).tag(1)
                }
                .disabled(!enabled)
                Picker(settings.localized("Precache Textures"), selection: $perGamePrecacheTextureReplacements) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("On")).tag(1)
                }
                .disabled(!enabled)
                Text(settings.localized("Texture replacement needs a restart to take effect."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .scrollContentBackground(.hidden)
        .toolbarBackground(OverlayTheme.shell, for: .navigationBar)
        .toolbarBackground(.visible, for: .navigationBar)
    }
}
