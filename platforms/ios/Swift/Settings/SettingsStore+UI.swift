// SettingsStore+UI.swift — UI/gamepad localization helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

extension SettingsStore {
    func localized(_ key: String) -> String {
        appLanguage.localized(key)
    }

    var localizedLayoutDirection: LayoutDirection {
        appLanguage.layoutDirection
    }

    var numberLocale: Locale {
        appLanguage.numberLocale
    }
}

extension SettingsStore {
    static let _appLanguageConfig = Setting<AppLanguage>(
        section: "ARMSX2iOS/UI", key: "AppLanguage", default: .system,
        suppressible: false,
        codec: .rawString)
}
