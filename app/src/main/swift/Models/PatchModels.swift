// PatchModels.swift — Cheats & Patches Manager data models
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum CheatsPatchesLaunchContext: Equatable {
    case library
    case inGame
}

enum PatchIdentityState: Equatable {
    case known(crc: String)
    case libraryAwaitingFirstLaunch
    case inGameLoading
    case inGameUnavailable

    var canUseDatabase: Bool {
        if case .known = self { return true }
        return false
    }

    var guidance: String? {
        switch self {
        case .known:
            return nil
        case .libraryAwaitingFirstLaunch:
            return "Boot this game once so ARMSX2 can identify it, then return to manage patches."
        case .inGameLoading:
            return "Game information is still loading. Try again in a moment."
        case .inGameUnavailable:
            return "ARMSX2 couldn’t identify this game’s patch version. Database matching is unavailable for now."
        }
    }
}

enum PatchFeedbackKind {
    case information
    case success
    case error
}

enum PatchDisplayGroup: Int, CaseIterable, Comparable, Hashable {
    case patches
    case cheatsAndHacks

    static func < (lhs: Self, rhs: Self) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    var title: String {
        switch self {
        case .patches: return "Patches"
        case .cheatsAndHacks: return "Cheats & Hacks"
        }
    }
}

enum PatchDisplayCategory: Int {
    case patch
    case fix
    case widescreen
    case fps60
    case cheat
    case hack

    var title: String {
        switch self {
        case .patch: return "Patch"
        case .fix: return "Fix"
        case .widescreen: return "Widescreen"
        case .fps60: return "60 FPS"
        case .cheat: return "Cheat"
        case .hack: return "Hack"
        }
    }
}

enum PatchCategory: String, Codable, CaseIterable, Identifiable {
    case cheats, widescreen, fps60, noInterlace, performance, gameFix, experimental, other

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .cheats: return "Cheats"
        case .widescreen: return "Widescreen"
        case .fps60: return "60 FPS"
        case .noInterlace: return "No-Interlace"
        case .performance: return "Performance"
        case .gameFix: return "Game Fixes"
        case .experimental: return "Experimental"
        case .other: return "Other"
        }
    }
}

enum PatchSource: String, Codable {
    case local
    case database
    case installed

    var displayName: String {
        switch self {
        case .local: return "Imported"
        case .database: return "Database"
        case .installed: return "Installed"
        }
    }
}

// A single user-facing patch/cheat entry shown in the manager. `enabled` reflects the
// PCSX2 per-game enable list (source of truth), not a separate store.
struct PatchEntry: Identifiable, Hashable {
    let id: String
    let name: String
    let summary: String
    let category: PatchCategory
    let source: PatchSource
    let isCheat: Bool
    let fileName: String
    let isLegacy: Bool
    var enabled: Bool

    var displayTitle: String {
        if name.isEmpty { return isCheat ? "Untitled Cheat" : "Untitled Patch" }
        return name
    }

    var displayGroup: PatchDisplayGroup {
        isCheat ? .cheatsAndHacks : .patches
    }

    var displayCategory: PatchDisplayCategory {
        if isCheat {
            let text = "\(name) \(summary)".lowercased()
            return text.contains("hack") ? .hack : .cheat
        }

        switch category {
        case .gameFix:
            return .fix
        case .widescreen:
            return .widescreen
        case .fps60:
            return .fps60
        default:
            return .patch
        }
    }

    static func displayOrdered(_ lhs: PatchEntry, _ rhs: PatchEntry) -> Bool {
        if lhs.displayGroup != rhs.displayGroup {
            return lhs.displayGroup < rhs.displayGroup
        }
        if lhs.displayCategory.rawValue != rhs.displayCategory.rawValue {
            return lhs.displayCategory.rawValue < rhs.displayCategory.rawValue
        }
        let titleOrder = lhs.displayTitle.localizedCaseInsensitiveCompare(rhs.displayTitle)
        if titleOrder != .orderedSame { return titleOrder == .orderedAscending }
        return lhs.id < rhs.id
    }
}
