// PerGameShaderSection.swift — the shader chain rows on the per-game Graphics tab
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct PerGameShaderCatalogRequest: Identifiable {
    let id = "per-game-shader-catalog"
}

/// Three states, not the global section's two: Use Global, Off, and a preset of this game's own.
/// The preset row is a Button and not a link, because the wide panel has no navigation stack.
struct PerGameShaderSection: View {
    let enabled: Bool
    @Binding var chain: Int
    @Binding var presetRef: String
    let settings: SettingsStore
    let onBrowse: () -> Void
    var showsClearPresetAction = true

    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var catalogRequest: PerGameShaderCatalogRequest?

    var body: some View {
        Section {
            Picker(settings.localized("Shaders"), selection: $chain) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .controllerAccessibilityOptionsPickerTarget(
                id: "per-game.graphics.shader-chain",
                label: settings.localized("Shader Chain"),
                selection: $chain,
                options: [
                    (-1, settings.localized("Use Global")),
                    (0, settings.localized("Off")),
                    (1, settings.localized("On")),
                ]
            )
            .disabled(!enabled)

            Button {
                catalogRequest = PerGameShaderCatalogRequest()
            } label: {
                Label(
                    settings.localized("Download Shaders"),
                    systemImage: "arrow.down.circle"
                )
            }
            .controllerAccessibilityActionTarget(
                id: "per-game.graphics.download-shaders",
                label: settings.localized("Download Shaders")
            ) {
                catalogRequest = PerGameShaderCatalogRequest()
            }
            .sheet(item: $catalogRequest) { _ in
                NavigationStack {
                    ShaderCatalogBrowserView(
                        localized: settings.localized,
                        onSelect: selectDownloadedPreset,
                        controllerInput: controllerInput
                    )
                }
                .presentationDetents([.large])
            }

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
                .controllerAccessibilityActionTarget(
                    id: "per-game.graphics.shader-preset",
                    label: settings.localized("Preset"),
                    action: onBrowse
                )
                .disabled(!enabled)

                if showsClearPresetAction, !presetRef.isEmpty {
                    Button(role: .destructive) {
                        presetRef = ""
                    } label: {
                        Text(settings.localized("Clear Preset"))
                    }
                    .controllerAccessibilityActionTarget(
                        id: "per-game.graphics.clear-shader-preset",
                        label: settings.localized("Clear Preset")
                    ) {
                        presetRef = ""
                    }
                    .disabled(!enabled)
                    .foregroundStyle(.red)
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

    private func selectDownloadedPreset(_ token: String) {
        chain = 1
        presetRef = token
    }
}
