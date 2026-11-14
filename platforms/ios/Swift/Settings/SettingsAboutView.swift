// SettingsAboutView.swift — the app version page
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SettingsAboutView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("App")) {
                HStack {
                    Text(settings.localized("Version"))
                    Spacer()
                    Text(SettingsStaticInfo.buildVersion)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
            }
        }
        .navigationTitle(settings.localized("About"))
    }
}
