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
            Picker(settings.localized("Shaders"), selection: $chain) {
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
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Image(systemName: "chevron.right")
                            .font(.footnote.weight(.semibold))
                            .foregroundStyle(.tertiary)
                    }
                }
                .tint(.primary)
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
            Text(settings.localized("Use Global follows the Shaders page. Off turns shaders off for this game only. Slider values belong to the preset, so games on the same preset share them."))
        }
    }

    private var presetName: String {
        ShaderPresetLibrary.displayName(for: presetRef) ?? settings.localized("None")
    }
}
