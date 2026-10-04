// AppLanguage.swift - app language model and localization data for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

enum AppLanguage: String, CaseIterable, Identifiable {
    case system
    case english
    case simplifiedChinese
    case arabic
    case spanish
    case french
    case german
    case italian
    case portuguese
    case japanese
    case korean

    var id: String { rawValue }

    var label: String {
        switch self {
        case .system: return "System Default"
        case .english: return "English"
        case .simplifiedChinese: return "简体中文"
        case .arabic: return "العربية"
        case .spanish: return "Español"
        case .french: return "Français"
        case .german: return "Deutsch"
        case .italian: return "Italiano"
        case .portuguese: return "Português"
        case .japanese: return "日本語"
        case .korean: return "한국어"
        }
    }

    static func resolvedSystemLanguage() -> AppLanguage {
        let code = Locale.current.language.languageCode?.identifier.lowercased() ?? "en"
        switch code {
        case "zh":
            return .simplifiedChinese
        case "ar":
            return .arabic
        case "es":
            return .spanish
        case "fr":
            return .french
        case "de":
            return .german
        case "it":
            return .italian
        case "pt":
            return .portuguese
        case "ja":
            return .japanese
        case "ko":
            return .korean
        default:
            return .english
        }
    }

    var resolved: AppLanguage {
        self == .system ? Self.resolvedSystemLanguage() : self
    }

    var layoutDirection: LayoutDirection {
        resolved == .arabic ? .rightToLeft : .leftToRight
    }

    /// Numbers follow the chosen UI language rather than the device region: someone who picked
    /// German expects 1,5x whatever their region says. System means whatever iOS is already doing.
    var numberLocale: Locale {
        guard self != .system else { return .autoupdatingCurrent }
        switch resolved {
        case .simplifiedChinese: return Locale(identifier: "zh_Hans")
        case .arabic: return Locale(identifier: "ar")
        case .spanish: return Locale(identifier: "es")
        case .french: return Locale(identifier: "fr")
        case .german: return Locale(identifier: "de")
        case .italian: return Locale(identifier: "it")
        case .portuguese: return Locale(identifier: "pt")
        case .japanese: return Locale(identifier: "ja")
        case .korean: return Locale(identifier: "ko")
        case .system, .english: return Locale(identifier: "en")
        }
    }

    var bcp47Code: String {
        switch resolved {
        case .simplifiedChinese: return "zh-Hans"
        case .arabic: return "ar"
        case .spanish: return "es"
        case .french: return "fr"
        case .german: return "de"
        case .italian: return "it"
        case .portuguese: return "pt"
        case .japanese: return "ja"
        case .korean: return "ko"
        case .system, .english: return "en"
        }
    }

    private static let bundleCache: [AppLanguage: Bundle] = {
        var cache: [AppLanguage: Bundle] = [:]
        for language in allCases where language != .system {
            if let path = Bundle.main.path(forResource: language.bcp47Code, ofType: "lproj"),
               let bundle = Bundle(path: path) {
                cache[language] = bundle
            }
        }
        return cache
    }()

    var bundle: Bundle {
        guard self != .system else { return .main }
        return Self.bundleCache[self] ?? .main
    }

    func localized(_ key: String) -> String {
        bundle.localizedString(forKey: key, value: key, table: nil)
    }

}
