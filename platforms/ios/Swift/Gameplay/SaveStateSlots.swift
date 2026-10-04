// SaveStateSlots.swift — The save-state rows and what the app keeps beside their files.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

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

    /// Puts an entry back as it was, or removes it.
    func set(_ entry: SaveStateMetadata?, forFileNamed fileName: String) {
        file.states[fileName] = entry
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

    /// Auto-save and Quick Save show a symbol where the other rows show their slot number.
    var symbol: String? {
        switch kind {
        case .auto: "cloud.fill"
        case .quick: "bolt.fill"
        case .manual, .older: nil
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
    /// Auto-save keeps its "..." while empty: its settings live there.
    var hasMore: Bool { occupied || kind == .auto }

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

/// The one action that can still be undone, and its 8 second window.
@MainActor
@Observable
final class SaveStateUndoModel {
    enum Action {
        case load(path: String)
        case delete(slot: Int, fileName: String)
        case overwrite(slot: Int, backupToken: String, fileName: String, previous: SaveStateMetadata?)
    }

    struct Item: Identifiable {
        let id = UUID()
        let action: Action
        let caption: String
        let name: String
        let preview: Data?
        let undoLabel: String
    }

    static let shared = SaveStateUndoModel()

    /// The window of the toast on screen, from Settings, so the bar drains over the same span.
    private(set) var duration: Double = 5

    private(set) var item: Item?
    private(set) var remaining: Double = 0
    /// Set while the toast holds VoiceOver focus.
    var focusHeld = false
    private var timer: Task<Void, Never>?

    var slot: Int? {
        switch item?.action {
        case .delete(let slot, _), .overwrite(let slot, _, _, _): slot
        case .load, nil: nil
        }
    }

    func show(_ next: Item, announcement: String?) {
        finish()
        item = next
        duration = Double(SettingsStore.shared.undoSeconds)
        remaining = duration
        if let announcement { AccessibilityNotification.Announcement(announcement).post() }
        timer = Task { [weak self] in
            while let self, !Task.isCancelled, self.item?.id == next.id {
                try? await Task.sleep(for: .milliseconds(100))
                // The window waits while the toast has VoiceOver focus or the app is away.
                let paused = self.focusHeld || UIApplication.shared.applicationState != .active
                if !paused { self.remaining -= 0.1 }
                if self.remaining <= 0 { self.finish() }
            }
        }
    }

    /// Ends the window: a deleted state is removed for good, a load's earlier moment dropped.
    func finish() {
        timer?.cancel()
        timer = nil
        guard let current = item else { return }
        item = nil
        switch current.action {
        case .load(let path):
            ARMSX2Bridge.discardUndoLoadState(atPath: path)
        case .delete(let slot, let fileName):
            ARMSX2Bridge.finishDeletingSaveState(inSlot: slot)
            SaveStateMetadataStore.shared.set(nil, forFileNamed: fileName)
        case .overwrite:
            break
        }
    }

    /// Any new write to a slot ends that slot's undo first.
    func finishIfTouching(slot: Int) {
        if self.slot == slot { finish() }
    }

    func undo(completion: @escaping @MainActor (Bool) -> Void) {
        timer?.cancel()
        timer = nil
        guard let current = item else { return completion(false) }
        item = nil
        let done: @Sendable (Bool) -> Void = { ok in
            Task { @MainActor in
                NotificationCenter.default.post(name: Notification.Name("ARMSX2iOSRuntimeMenuStateChanged"), object: nil)
                completion(ok)
            }
        }
        switch current.action {
        case .load(let path):
            ARMSX2Bridge.undoLoadState(fromPath: path) { ok in
                Task { @MainActor in
                    if ok {
                        SaveStateAutoSave.shared.restart()
                    } else if self.item == nil {
                        self.show(current, announcement: nil)
                    } else {
                        ARMSX2Bridge.discardUndoLoadState(atPath: path)
                    }
                    done(ok)
                }
            }
        case .delete(let slot, _):
            ARMSX2Bridge.restoreDeletedSaveState(inSlot: slot, completion: done)
        case .overwrite(let slot, let token, let fileName, let previous):
            ARMSX2Bridge.undoSaveOver(inSlot: slot, backupToken: token) { ok in
                Task { @MainActor in
                    if ok { SaveStateMetadataStore.shared.set(previous, forFileNamed: fileName) }
                    done(ok)
                }
            }
        }
    }
}

/// Auto-save's play clock, and the save itself: on a timer during play and when the game closes.
@MainActor
final class SaveStateAutoSave {
    static let shared = SaveStateAutoSave()
    /// A save this soon after a boot or a load would replace the Auto-save with a quick look.
    static let settleSeconds: Double = 120

    private var sinceLoad: Double = 0
    private var sinceWrite: Double = 0
    private var saving = false
    private var wasLowBattery = false
    private var playedSinceAutoSave = false

    /// A boot or a load starts both clocks again.
    func restart() {
        sinceLoad = 0
        sinceWrite = 0
        wasLowBattery = false
    }

    func didWrite() { sinceWrite = 0 }

    /// Counts seconds of play. Past the interval it tries every tick until a save lands. On a
    /// low battery it saves at once and then every minute: iOS gives no warning before power-off.
    func tick(_ seconds: Double) {
        sinceLoad += seconds
        sinceWrite += seconds
        playedSinceAutoSave = true
        let settings = SettingsStore.shared
        let lowBattery = settings.autoSaveOnLowBattery && Self.batteryIsLow
        if lowBattery && !wasLowBattery { sinceWrite = .infinity }
        wasLowBattery = lowBattery
        let interval = lowBattery ? 60 : Double(settings.autoSaveIntervalMinutes * 60)
        guard settings.autoSaveEnabled, sinceWrite >= interval else { return }
        save(leaving: false) { _ in }
    }

    /// iOS reports the level in 5% steps, so 5% is the last reading before the phone dies.
    private static var batteryIsLow: Bool {
        let device = UIDevice.current
        return device.batteryState == .unplugged && device.batteryLevel >= 0 && device.batteryLevel <= 0.05
    }

    /// Reports false without writing while an undo is pending or the game has only just started.
    func save(leaving: Bool, completion: @escaping @MainActor (Bool) -> Void) {
        guard !saving, sinceLoad >= Self.settleSeconds, SaveStateUndoModel.shared.item == nil else {
            if leaving {
                ARMSX2Bridge.logAutoSaveSkipped(saving ? "another auto-save was still running"
                    : sinceLoad < Self.settleSeconds ? "under 2 minutes of play since the game started or a state loaded"
                    : "an Undo was still pending")
            }
            return completion(false)
        }
        // Back to Menu already saved and nothing has been played since.
        if leaving && !playedSinceAutoSave { return completion(true) }
        saving = true
        ARMSX2Bridge.autoSave(leavingGame: leaving) { ok in
            Task { @MainActor in
                let played = ARMSX2Bridge.currentGamePlayedSeconds()
                self.saving = false
                if ok {
                    self.sinceWrite = 0
                    self.playedSinceAutoSave = false
                    if let file = await SaveStateFile.current().first(where: { $0.slot == SaveStateSlot.autoSlot }) {
                        SaveStateMetadataStore.shared.recordSave(of: file, playedSeconds: played, fresh: true)
                    }
                }
                completion(ok)
            }
        }
    }
}
