// LanguageSettingsView.swift — the app language picker
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct LanguageSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("Interface Language")) {
                Picker(settings.localized("App Language"), selection: $settings.appLanguage) {
                    ForEach(AppLanguage.allCases) { language in
                        Text(settings.localized(language.label)).tag(language)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "settings.language.app-language",
                    label: settings.localized("App Language"),
                    selection: $settings.appLanguage,
                    options: AppLanguage.allCases.map {
                        (id: $0, title: settings.localized($0.label))
                    }
                )
                Text(settings.localized("ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Language"))
        .navigationBarTitleDisplayMode(.large)
    }
}
