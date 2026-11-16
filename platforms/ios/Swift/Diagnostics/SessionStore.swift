// SessionStore.swift — one folder per app launch under Documents/logs/sessions
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import os

enum SessionStore {
    static let buildingMarkerName = "building"
    private static let idPattern = #"^\d{8}T\d{6}Z(-\d+)?$"#
    private static let running = OSAllocatedUnfairLock<String?>(initialState: nil)

    // Stop and clear code runs detached, and fixture folders can carry future names,
    // so the running id is held here instead of being read as the newest folder.
    static var currentID: String? { running.withLock { $0 } }

    static func publish(_ id: String) {
        running.withLock { $0 = id }
    }

    static func root(documents: URL = .documentsDirectory) -> URL {
        documents.appendingPathComponent("logs/sessions", isDirectory: true)
    }

    static func format(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyyMMdd'T'HHmmss'Z'"
        return formatter.string(from: date)
    }

    static func makeID(now: Date, existing: [String]) -> String {
        let base = format(now)
        var id = base
        var suffix = 2
        while existing.contains(id) {
            id = "\(base)-\(suffix)"
            suffix += 1
        }
        return id
    }

    static func isValidID(_ id: String) -> Bool {
        id.range(of: idPattern, options: .regularExpression) != nil
    }

    static func url(for id: String, documents: URL = .documentsDirectory) -> URL? {
        guard isValidID(id) else { return nil }
        return root(documents: documents).appendingPathComponent(id, isDirectory: true)
    }

    static func list(documents: URL = .documentsDirectory) -> [String] {
        let keys: [URLResourceKey] = [.isDirectoryKey]
        let entries = (try? FileManager.default.contentsOfDirectory(
            at: root(documents: documents), includingPropertiesForKeys: keys)) ?? []
        return entries
            .filter { isValidID($0.lastPathComponent) && (try? $0.resourceValues(forKeys: Set(keys)))?.isDirectory == true }
            .map(\.lastPathComponent)
            .sorted()
    }

    // The launch that was running last, which is where its logs go at the next launch.
    static func previousID(documents: URL = .documentsDirectory) -> String? {
        let pointer = root(documents: documents).appendingPathComponent(".current")
        guard let text = try? String(contentsOf: pointer, encoding: .utf8) else { return nil }
        let id = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let folder = url(for: id, documents: documents),
              FileManager.default.fileExists(atPath: folder.path) else { return nil }
        return id
    }

    static func writeCurrentPointer(_ id: String, documents: URL = .documentsDirectory) {
        try? Data(id.utf8).write(to: root(documents: documents).appendingPathComponent(".current"), options: .atomic)
    }

    static func writeSessionJSON(id: String, started: Date = Date(), documents: URL = .documentsDirectory) {
        guard let folder = url(for: id, documents: documents) else { return }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let json = "{\"schema\":1,\"started\":\"\(started.formatted(.iso8601))\"}"
        try? Data(json.utf8).write(to: folder.appendingPathComponent("session.json"), options: .atomic)
    }

    // A move, never a copy: the live log can be tens of megabytes.
    static func rotate(into id: String, documents: URL = .documentsDirectory) {
        guard let folder = url(for: id, documents: documents) else { return }
        let files = FileManager.default
        try? files.createDirectory(at: folder, withIntermediateDirectories: true)
        let logs: [(URL, String)] = [
            (documents.appendingPathComponent("pcsx2_log.txt"), "pcsx2_log.txt"),
            (documents.appendingPathComponent("logs/emulog.txt"), "emulog.txt"),
        ]
        for (source, name) in logs {
            let destination = folder.appendingPathComponent(name)
            guard files.fileExists(atPath: source.path), !files.fileExists(atPath: destination.path) else { continue }
            try? files.moveItem(at: source, to: destination)
        }
    }

    static func prune(keeping: Int = 10, current: String, documents: URL = .documentsDirectory) {
        var remaining = list(documents: documents).count
        for id in list(documents: documents) where remaining > keeping && id != current {
            guard let folder = url(for: id, documents: documents) else { continue }
            try? FileManager.default.removeItem(at: folder)
            remaining -= 1
        }
    }

    // The first launch after an upgrade has a log and no pointer; its folder takes the file's own time.
    static func adoptStaleLog(documents: URL = .documentsDirectory) -> String? {
        let log = documents.appendingPathComponent("pcsx2_log.txt")
        guard let modified = (try? log.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
        else { return nil }
        let id = makeID(now: modified, existing: list(documents: documents))
        writeSessionJSON(id: id, started: modified, documents: documents)
        return id
    }

    static func startedDate(of id: String, documents: URL = .documentsDirectory) -> Date? {
        if let folder = url(for: id, documents: documents),
           let data = try? Data(contentsOf: folder.appendingPathComponent("session.json")),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let text = json["started"] as? String,
           let date = try? Date(text, strategy: .iso8601) { return date }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyyMMdd'T'HHmmss'Z'"
        return formatter.date(from: String(id.prefix(16)))
    }

    // Reads the whole session log once, off the launch path, for what the folder list should show.
    static func finalize(id: String, documents: URL = .documentsDirectory) {
        guard let folder = url(for: id, documents: documents) else { return }
        var game: [String: String]?
        var booted = false
        try? SessionLogScanner.markerLines(of: folder.appendingPathComponent("pcsx2_log.txt")) { line in
            guard let marker = MarkerLine.parse(line) else { return }
            if marker.tag == "DIAG_GAME" {
                let empty = marker.fields["serial"] == "-" && (Int(marker.fields["crc"] ?? "", radix: 16) ?? 0) == 0
                    && (marker.fields["title"] ?? "").isEmpty
                if !empty { game = marker.fields }
            } else if marker.tag == "BOOT_VM_INIT", marker.fields["success"] == "1" {
                booted = true
            }
        }
        let file = folder.appendingPathComponent("session.json")
        var info = (try? Data(contentsOf: file)).flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] } ?? [:]
        info["schema"] = info["schema"] ?? 1
        info["booted"] = booted
        for key in ["serial", "crc", "title"] {
            if let value = game?[key], !value.isEmpty, value != "-" { info[key] = value }
        }
        guard let data = try? JSONSerialization.data(withJSONObject: info, options: [.sortedKeys]) else { return }
        try? data.write(to: file, options: .atomic)
    }

    // Only entries older than this launch go, so a report this very process is building stays.
    static func sweepStaleTemporaries(launchedAt: Date, documents: URL = .documentsDirectory,
                                      temporary: URL = URL(fileURLWithPath: NSTemporaryDirectory())) {
        let files = FileManager.default
        let reports = documents.appendingPathComponent("Reports", isDirectory: true)
        let candidates = [(temporary, "ARMSX2-report-", ""), (reports, "", ".partial")]
        for (folder, prefix, suffix) in candidates {
            let entries = (try? files.contentsOfDirectory(
                at: folder, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
            for entry in entries where entry.lastPathComponent.hasPrefix(prefix) && entry.lastPathComponent.hasSuffix(suffix) {
                let modified = (try? entry.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
                if let modified, modified < launchedAt { try? files.removeItem(at: entry) }
            }
        }
    }

    // Never touches the live pcsx2_log.txt, logs/emulog.txt, the running folder or the pointer.
    static func clearPast(documents: URL = .documentsDirectory) -> [String] {
        let files = FileManager.default
        var failures: [String] = []
        func remove(_ url: URL) {
            do { try files.removeItem(at: url) } catch { failures.append("\(url.lastPathComponent): \(error.localizedDescription)") }
        }
        let current = currentID
        for id in list(documents: documents) where id != current {
            if let folder = url(for: id, documents: documents) { remove(folder) }
        }
        let logs = documents.appendingPathComponent("logs", isDirectory: true)
        for entry in (try? files.contentsOfDirectory(at: logs, includingPropertiesForKeys: nil)) ?? []
        where !["sessions", "emulog.txt"].contains(entry.lastPathComponent) {
            remove(entry)
        }
        let reports = documents.appendingPathComponent("Reports", isDirectory: true)
        for entry in (try? files.contentsOfDirectory(at: reports, includingPropertiesForKeys: nil)) ?? []
        where ["zip", "partial"].contains(entry.pathExtension) {
            remove(entry)
        }
        return failures
    }
}
