// SaveStateSlots.swift — The save-state rows and what the app keeps beside their files.
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// What the bridge knows about one state file, read off the main thread: it opens every zip.
struct SaveStateFile: Sendable {
    let slot: Int
    let occupied: Bool
    let fileName: String
    let modifiedDate: Date?
    let preview: Data?

    init(_ info: ARMSX2SaveStateSlotInfo) {
        slot = info.slot
        occupied = info.occupied
        fileName = info.fileName
        modifiedDate = info.occupied ? info.modifiedDate : nil
        preview = info.previewPNGData
    }

    static func current() async -> [SaveStateFile] {
        await Task.detached(priority: .userInitiated) {
            ARMSX2Bridge.saveStateSlots().map(SaveStateFile.init)
        }.value
    }
}

/// Kept per state file, keyed by its file name. An entry only counts while `savedAt` still
/// matches the file, so a state replaced from the Files app never shows another state's details.
struct SaveStateMetadata: Codable, Equatable {
    var name: String?
    var locked: Bool?
    var playedSeconds: Double?
    var savedAt: Date?

    func matches(_ modified: Date?) -> Bool {
        guard let savedAt, let modified else { return false }
        return abs(savedAt.timeIntervalSince(modified)) < 1
    }
}

@MainActor
final class SaveStateMetadataStore {
    static let shared = SaveStateMetadataStore()

    private struct File: Codable {
        var schemaVersion = 1
        var states: [String: SaveStateMetadata] = [:]
    }

    private var file: File
    private let url: URL

    private init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        let directory = base.appendingPathComponent("ARMSX2", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        url = directory.appendingPathComponent("SaveStateSlots.json")
        file = File()
        guard let data = try? Data(contentsOf: url) else { return }
        if let decoded = try? JSONDecoder().decode(File.self, from: data) {
            file = decoded
        } else {
            // Keep the unreadable file for recovery instead of writing over it.
            let corrupt = directory.appendingPathComponent("SaveStateSlots.corrupt.json")
            try? FileManager.default.removeItem(at: corrupt)
            try? FileManager.default.moveItem(at: url, to: corrupt)
        }
    }

    func metadata(for state: SaveStateFile) -> SaveStateMetadata? {
        guard state.occupied, let entry = file.states[state.fileName],
              entry.matches(state.modifiedDate) else { return nil }
        return entry
    }

    /// Records the play time for a state that was just written. Saving over a state keeps its
    /// name and lock; a state written into an empty slot starts without either.
    func recordSave(of state: SaveStateFile, playedSeconds: Double, fresh: Bool) {
        guard state.occupied, let modified = state.modifiedDate else { return }
        var entry = fresh ? SaveStateMetadata() : file.states[state.fileName] ?? SaveStateMetadata()
        entry.playedSeconds = playedSeconds
        entry.savedAt = modified
        file.states[state.fileName] = entry
        write()
    }

    func update(_ state: SaveStateFile, _ change: (inout SaveStateMetadata) -> Void) {
        guard state.occupied, let modified = state.modifiedDate else { return }
        var entry = metadata(for: state) ?? SaveStateMetadata(savedAt: modified)
        change(&entry)
        file.states[state.fileName] = entry
        write()
    }

    private func write() {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        do {
            try encoder.encode(file).write(to: url, options: .atomic)
        } catch {
            NSLog("[ARMSX2 iOS SaveState] metadata write failed: %@", error.localizedDescription)
        }
    }
}

/// One row of the Save States panel.
struct SaveStateSlot: Identifiable {
    enum Kind {
        case manual
        /// A manual save in slot 9 or 10 from before those rows became Auto-save and Quick Save.
        case older
        case auto
        case quick
    }

    static let autoSlot = -2
    static let quickSlot = 0

    let file: SaveStateFile
    let kind: Kind
    let metadata: SaveStateMetadata?

    var id: Int { file.slot }
    var slot: Int { file.slot }
    var occupied: Bool { file.occupied }
    var modifiedDate: Date? { file.modifiedDate }

    /// The number shown at the start of the row.
    var number: Int {
        switch kind {
        case .auto: 9
        case .quick: 10
        case .manual, .older: slot
        }
    }

    @MainActor
    func title(_ settings: SettingsStore) -> String {
        if isNameable, let name = metadata?.name, !name.isEmpty {
            return name
        }
        return defaultTitle(settings)
    }

    @MainActor
    func defaultTitle(_ settings: SettingsStore) -> String {
        switch kind {
        case .manual: String(format: settings.localized("Slot %d"), slot)
        case .older: String(format: settings.localized("Older Slot %d"), slot)
        case .auto: settings.localized("Auto-save")
        case .quick: settings.localized("Quick Save")
        }
    }

    var isNameable: Bool { kind == .manual || kind == .older }
    var isLocked: Bool { isNameable && occupied && metadata?.locked == true }
    var hasSave: Bool { kind != .auto }
    var hasMore: Bool { occupied && kind != .auto }

    static let nameLimit = 32

    /// One line, trimmed, at most `nameLimit` characters.
    static func cleanName(_ text: String) -> String {
        let line = text.replacingOccurrences(of: "\n", with: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return String(line.prefix(nameLimit))
    }

    /// Rows 1–8 always, Older 9 and 10 only while they hold a state, then Auto-save and Quick Save.
    static func rows(
        from files: [SaveStateFile],
        metadata: (SaveStateFile) -> SaveStateMetadata?
    ) -> [SaveStateSlot] {
        func row(_ file: SaveStateFile, _ kind: Kind) -> SaveStateSlot {
            SaveStateSlot(file: file, kind: kind, metadata: metadata(file))
        }
        let bySlot = Dictionary(files.map { ($0.slot, $0) }, uniquingKeysWith: { first, _ in first })
        var rows = (1...8).compactMap { bySlot[$0].map { row($0, .manual) } }
        rows += [9, 10].compactMap { bySlot[$0].flatMap { $0.occupied ? row($0, .older) : nil } }
        if let auto = bySlot[autoSlot] { rows.append(row(auto, .auto)) }
        if let quick = bySlot[quickSlot] { rows.append(row(quick, .quick)) }
        return rows
    }

    static func latest(in rows: [SaveStateSlot]) -> Int? {
        rows.filter(\.occupied).max { ($0.modifiedDate ?? .distantPast) < ($1.modifiedDate ?? .distantPast) }?.slot
    }
}

@MainActor
enum SaveStateFormat {
    /// The app language with the system region, so a language picked in the app keeps the
    /// user's 24-hour clock and date order.
    static func locale(_ settings: SettingsStore) -> Locale {
        guard settings.appLanguage != .system else { return .autoupdatingCurrent }
        var components = Locale.Components(identifier: settings.appLanguage.bcp47Code)
        if components.region == nil {
            components.region = Locale.current.region
        }
        return Locale(components: components)
    }

    /// "Today 18:13", "Yesterday 21:40" or "26 Sep 14:02".
    static func savedAt(_ date: Date, settings: SettingsStore) -> String {
        let locale = locale(settings)
        let time = date.formatted(.dateTime.hour().minute().locale(locale))
        if Calendar.current.isDateInToday(date) {
            return String(format: settings.localized("Today %@"), time)
        }
        if Calendar.current.isDateInYesterday(date) {
            return String(format: settings.localized("Yesterday %@"), time)
        }
        return date.formatted(.dateTime.day().month(.abbreviated).hour().minute().locale(locale))
    }

    /// "4 h 12 min played".
    static func played(_ seconds: Double, settings: SettingsStore) -> String {
        let duration = Duration.seconds(Int64(seconds.rounded()))
        let text = duration.formatted(
            .units(allowed: [.hours, .minutes], width: .abbreviated).locale(locale(settings))
        )
        return String(format: settings.localized("%@ played"), text)
    }
}
