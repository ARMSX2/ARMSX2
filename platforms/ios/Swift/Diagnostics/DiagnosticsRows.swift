// DiagnosticsRows.swift — what the Diagnostics session list shows for each launch
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct SessionRow: Identifiable, Equatable {
    let id: String
    let isNow: Bool
    let title: String
    let started: Date?
    // nil is Unknown, or Running for the live session.
    let ending: Ending?
    let logReadable: Bool
}

enum DiagnosticsRows {
    static func order(_ ids: [String], current: String?) -> [String] {
        let rest = ids.filter { $0 != current }.sorted(by: >)
        guard let current, ids.contains(current) else { return rest }
        return [current] + rest
    }

    static func load(id: String, isNow: Bool, documents: URL = .documentsDirectory) -> SessionRow {
        let folder = SessionStore.url(for: id, documents: documents)
        let info = folder.flatMap { try? Data(contentsOf: $0.appendingPathComponent("session.json")) }
            .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
        let log = folder?.appendingPathComponent("pcsx2_log.txt")
        let values = try? log?.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
        let readable = values?.isRegularFile == true && values?.isSymbolicLink != true
        let ending = readable && !isNow ? EndingParser.verdict(of: id, documents: documents).ending : nil
        return SessionRow(id: id, isNow: isNow, title: info?["title"] as? String ?? "",
                          started: SessionStore.startedDate(of: id, documents: documents),
                          ending: ending, logReadable: readable)
    }
}

extension Ending {
    var needsAttention: Bool {
        switch self {
        case .fatal, .jetsam, .freeze, .bootFailed, .stuckBoot: true
        case .noGame, .cleanStop, .leftBoot, .endedPaused, .killedRunning: false
        }
    }
}
