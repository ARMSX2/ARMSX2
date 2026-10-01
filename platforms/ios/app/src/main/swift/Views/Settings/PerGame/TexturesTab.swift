// TexturesTab.swift — one game's texture packs and replacement overrides
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct TexturesTab: View {
    @Binding var enabled: Bool
    @Binding var perGameLoadTextureReplacements: Int
    @Binding var perGameLoadTextureReplacementsAsync: Int
    @Binding var perGamePrecacheTextureReplacements: Int
    @Binding var controllerTargets: [String]

    let serial: String
    let settings: SettingsStore

    var body: some View {
        TexturePacksView(serial: serial, controllerTargets: $controllerTargets) {
            Section(settings.localized("Texture Replacement")) {
                override("Load Replacement Textures", id: "per-game.textures.load-replacement-textures",
                         selection: $perGameLoadTextureReplacements)
                override("Async Loading", id: "per-game.textures.async-loading",
                         selection: $perGameLoadTextureReplacementsAsync)
                override("Precache Textures", id: "per-game.textures.precache-textures",
                         selection: $perGamePrecacheTextureReplacements)
                Text(settings.localized("Texture replacement needs a restart to take effect."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .perGameRightStickScroll()
        .scrollContentBackground(.hidden)
        .toolbarBackground(.hidden, for: .navigationBar)
    }

    private func override(_ title: String, id: String, selection: Binding<Int>) -> some View {
        let options = [
            (id: -1, title: settings.localized("Use Global")),
            (id: 0, title: settings.localized("Off")),
            (id: 1, title: settings.localized("On")),
        ]
        return Picker(settings.localized(title), selection: selection) {
            ForEach(options, id: \.id) { Text($0.title).tag($0.id) }
        }
        .controllerAccessibilityOptionsPickerTarget(
            id: id,
            label: settings.localized(title),
            selection: selection,
            options: options
        )
        .disabled(!enabled)
    }
}
