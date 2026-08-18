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
