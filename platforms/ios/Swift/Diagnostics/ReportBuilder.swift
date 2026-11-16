// ReportBuilder.swift — stages an allowlisted copy of one session and zips it into Documents/Reports
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum DiagnosticsError: Error, CustomStringConvertible {
    case invalidSession(String)
    case notRegularFile(String)
    case zipFailed(String)

    var description: String {
        switch self {
        case .invalidSession(let id): "invalid session \(id)"
        case .notRegularFile(let name): "\(name) is not a regular file"
        case .zipFailed(let reason): "zip failed: \(reason)"
        }
    }
}

enum ReportBuilder {
    private static let chunk = 4 << 20

    static func build(sessionID: String, documents: URL = .documentsDirectory) async throws -> URL {
        let files = FileManager.default
        guard let folder = SessionStore.url(for: sessionID, documents: documents),
              files.fileExists(atPath: folder.path) else { throw DiagnosticsError.invalidSession(sessionID) }

        let name = "ARMSX2-report-\(SessionStore.format(Date()))"
        let staging = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true).appendingPathComponent(name)
        let inner = staging.appendingPathComponent(name, isDirectory: true)
        defer { try? files.removeItem(at: staging) }
        try? files.removeItem(at: staging)
        try files.createDirectory(at: inner, withIntermediateDirectories: true)

        for (source, stagedName) in allowlist(session: sessionID, folder: folder, documents: documents) {
            guard let regular = isRegularFile(source) else { continue }
            guard regular else { throw DiagnosticsError.notRegularFile(source.lastPathComponent) }
            try copy(source, to: inner.appendingPathComponent(stagedName))
        }
        try Data(summary(session: sessionID, folder: folder).utf8).write(to: inner.appendingPathComponent("summary.md"))

        let reports = documents.appendingPathComponent("Reports", isDirectory: true)
        try files.createDirectory(at: reports, withIntermediateDirectories: true)
        let partial = reports.appendingPathComponent("\(name).zip.partial")
        let zip = reports.appendingPathComponent("\(name).zip")
        try? files.removeItem(at: partial)

        var coordinationError: NSError?
        var copyError: Error?
        NSFileCoordinator().coordinate(readingItemAt: inner, options: .forUploading, error: &coordinationError) { temporaryZip in
            do { try files.copyItem(at: temporaryZip, to: partial) } catch { copyError = error }
        }
        if let failure = coordinationError ?? copyError.map({ $0 as NSError }) {
            throw DiagnosticsError.zipFailed(failure.localizedDescription)
        }
        try? files.removeItem(at: zip)
        try files.moveItem(at: partial, to: zip)
        return zip
    }

    // Built in code and never enumerated, so nothing else in Documents can reach the zip.
    private static func allowlist(session: String, folder: URL, documents: URL) -> [(URL, String)] {
        let live = session == SessionStore.currentID
        return [
            (live ? documents.appendingPathComponent("pcsx2_log.txt") : folder.appendingPathComponent("pcsx2_log.txt"), "pcsx2_log.txt"),
            (live ? documents.appendingPathComponent("logs/emulog.txt") : folder.appendingPathComponent("emulog.txt"), "emulog.txt"),
            (folder.appendingPathComponent("session.json"), "session.json"),
        ]
    }

    // nil means absent; a symlink or directory answers false.
    private static func isRegularFile(_ url: URL) -> Bool? {
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]) else { return nil }
        return values.isRegularFile == true && values.isSymbolicLink != true
    }

    // Reads only the size sampled at the start, since the live log keeps growing.
    private static func copy(_ source: URL, to target: URL) throws {
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        guard FileManager.default.createFile(atPath: target.path, contents: nil) else {
            throw DiagnosticsError.zipFailed("cannot create \(target.lastPathComponent)")
        }
        let output = try FileHandle(forWritingTo: target)
        defer { try? output.close() }
        var remaining = try input.seekToEnd()
        try input.seek(toOffset: 0)
        while remaining > 0 {
            let finished = try autoreleasepool { () throws -> Bool in
                guard let piece = try input.read(upToCount: Int(min(UInt64(chunk), remaining))), !piece.isEmpty else { return true }
                try output.write(contentsOf: piece)
                remaining -= UInt64(piece.count)
                return false
            }
            if finished { break }
        }
    }

    private static func summary(session: String, folder: URL) -> String {
        let data = try? Data(contentsOf: folder.appendingPathComponent("session.json"))
        let object = data.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
        let started = object?["started"] as? String ?? "unknown"
        return "# ARMSX2 problem report\n\n- Session: \(session)\n- Started: \(started)\n"
    }
}
