// PerGameShaderSection.swift — the shader chain rows on the per-game Graphics tab
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Three states, not the global section's two: Use Global, Off, and a preset of this game's own.
/// The preset row is a Button and not a link, because the wide panel has no navigation stack.
struct PerGameShaderSection: View {
    let enabled: Bool
    @Binding var chain: Int
    @Binding var presetRef: String
    let settings: SettingsStore
    let onBrowse: () -> Void

    var body: some View {
        Section {
            Picker(settings.localized("Shader Chain"), selection: $chain) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            if chain == 1 {
                Button(action: onBrowse) {
                    HStack {
                        Text(settings.localized("Preset"))
                        Spacer()
                        Text(presetName)
                            .foregroundStyle(.secondary)
                    }
                }
                .disabled(!enabled)

                if !presetRef.isEmpty {
                    Button(role: .destructive) {
                        presetRef = ""
                    } label: {
                        Text(settings.localized("Clear Preset"))
                    }
                    .disabled(!enabled)
                }
            }
        } header: {
            Text(settings.localized("Shaders"))
        } footer: {
            Text(settings.localized("A game with no shader of its own uses the global chain. Off means no shader for this game even while the global chain is running. Parameter values belong to the preset and not to the game, so two games on the same preset share them."))
        }
    }

    /// The tail of the token, which is the file name a player picked it by. Four lines the global
    /// section also carries, copied rather than shared so its fenced initializer stays shut.
    private var presetName: String {
        guard let separator = presetRef.firstIndex(of: ShaderPresetLibrary.markerSeparator) else {
            return settings.localized("None")
        }
        let relative = presetRef[presetRef.index(after: separator)...]
        let name = URL(fileURLWithPath: String(relative))
            .deletingPathExtension().lastPathComponent
        return name.isEmpty ? settings.localized("None") : name
    }
}
