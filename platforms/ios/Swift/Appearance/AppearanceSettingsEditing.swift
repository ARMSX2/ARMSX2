// AppearanceSettingsEditing.swift — the binding every Appearance section writes through
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor
protocol AppearanceSettingsEditing {}

extension AppearanceSettingsEditing {
    func manualBinding<Value>(
        for keyPath: ReferenceWritableKeyPath<SettingsStore, Value>
    ) -> Binding<Value> {
        let settings = SettingsStore.shared
        return Binding(
            get: { settings[keyPath: keyPath] },
            set: { value in
                // Keep the preset's readable colours when making a custom
                // variant, instead of switching to unrelated fallbacks.
                settings.beginCustomThemeEditing()
                settings[keyPath: keyPath] = value
            }
        )
    }

    func rolePreviewColor(for id: String) -> Color {
        let settings = SettingsStore.shared
        switch id {
        case "tab-titles-colour":
            return settings.controllerTabTitleColor
        case "tab-subtitles-colour":
            return settings.controllerTabSubtitleColor
        case "bottom-tab-bar-colour":
            return settings.controllerBottomTabBarColor
        case "bottom-tab-bar-unselected-colour":
            return settings.controllerBottomTabBarUnselectedColor
        case "card-titles-colour":
            return settings.controllerCardTitleColor
        case "context-menu-colour":
            return settings.controllerContextMenuColor
        case "import-actions-colour":
            return settings.controllerImportActionColor
        case "toolbar-icons-colour":
            return settings.controllerToolbarColor
        default:
            return settings.controllerTextAppearance.contentColor ?? .primary
        }
    }
}
