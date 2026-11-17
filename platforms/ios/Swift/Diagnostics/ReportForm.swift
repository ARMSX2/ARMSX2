// ReportForm.swift — what the tester picks and types in the report sheet, and the English block it adds to summary.md
// SPDX-License-Identifier: GPL-3.0+

import Foundation

// Raw values are the English text that goes into the report; the sheet localizes only what it shows.
enum ReportKind: String, CaseIterable, Identifiable {
    case crash = "Crash"
    case freeze = "Freeze"
    case wontBoot = "Won't boot"
    case graphics = "Graphics glitch"
    case slow = "Slow / stutter"
    case audio = "Audio"
    case controls = "Controls"
    case save = "Save problem"
    case jit = "JIT won't enable"
    case setup = "BIOS / setup"

    var id: String { rawValue }
}

enum ReportPlace: String, CaseIterable, Identifiable {
    case boot = "Boot / BIOS"
    case menu = "Menu"
    case inGame = "In-game"
    case cutscene = "Cutscene / FMV"
    case loading = "Loading"
    case saving = "Saving"
    case library = "Library"
    case settings = "Settings"

    var id: String { rawValue }
}

struct ReportForm: Equatable {
    static let fieldLimit = 2000

    var kinds: [ReportKind] = []
    var place: ReportPlace?
    var whatHappened = ""
    var scene = ""

    var isEmpty: Bool { markdownBlock().isEmpty }

    func markdownBlock() -> String {
        var lines: [String] = []
        let kindText = ReportKind.allCases.filter(kinds.contains).map(\.rawValue).joined(separator: ", ")
        if !kindText.isEmpty { lines.append("What happened: \(kindText)") }
        if let place { lines.append("Where: \(place.rawValue)") }
        for (name, value) in [("Details", whatHappened), ("Level or scene", scene)] {
            let text = Self.clean(value)
            if !text.isEmpty { lines.append("\(name): \(text)") }
        }
        return lines.isEmpty ? "" : lines.joined(separator: "\n") + "\n\n"
    }

    // One line per field, counted in Characters so a cut never lands inside a grapheme cluster.
    private static func clean(_ text: String) -> String {
        String(text.split(whereSeparator: \.isWhitespace).joined(separator: " ").prefix(fieldLimit))
    }
}

struct ReportExtras: Sendable {
    var form = ReportForm()
    var photoJPEG: Data?
    var pausedFramePNG: Data?
    var crashFiles: [URL] = []
}
