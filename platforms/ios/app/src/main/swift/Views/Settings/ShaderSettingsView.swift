// ShaderSettingsView.swift — the RetroArch shader chain as its own settings page
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            if ARMSX2Bridge.isShaderChainSupported() {
                ShaderChainSection(
                    enabled: $settings.shaderChainEnabled,
                    presetRef: $settings.shaderChainPresetRef,
                    localized: settings.localized
                )

                Section {
                    NavigationLink {
                        ShaderCatalogBrowserView(localized: settings.localized)
                    } label: {
                        Label(settings.localized("Download Shaders"),
                              systemImage: "arrow.down.circle")
                    }
                } footer: {
                    Text(settings.localized("Presets from the RetroArch collection, one at a time. Each download is checked against the catalogue's own hash before anything is written, and lands beside a pack you installed by hand."))
                }
            } else {
                Section {
                    Text(settings.localized("This build has no shader support."))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle(settings.localized("Shaders"))
        .navigationBarTitleDisplayMode(.inline)
    }
}
