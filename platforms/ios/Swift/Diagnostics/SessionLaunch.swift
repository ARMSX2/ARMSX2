// SessionLaunch.swift — launch hook that files the previous run's logs before stderr is truncated
// SPDX-License-Identifier: GPL-3.0+

import Foundation

@objc public final class SessionLaunch: NSObject {
    @objc public static func begin() {
        let launchedAt = Date()
        let documents = URL.documentsDirectory
        let sessions = SessionStore.root(documents: documents)
        try? FileManager.default.createDirectory(at: sessions, withIntermediateDirectories: true)
        let previous = SessionStore.previousID(documents: documents) ?? SessionStore.adoptStaleLog(documents: documents)
        if let previous {
            SessionStore.rotate(into: previous, documents: documents)
        }
        let id = SessionStore.makeID(now: launchedAt, existing: SessionStore.list(documents: documents))
        SessionStore.writeSessionJSON(id: id, documents: documents)
        SessionStore.writeCurrentPointer(id, documents: documents)
        SessionStore.publish(id)
        SessionStore.prune(current: id, documents: documents)
        Task.detached(priority: .utility) {
            if let previous { SessionStore.finalize(id: previous, documents: documents) }
            SessionStore.sweepStaleTemporaries(launchedAt: launchedAt, documents: documents)
        }
        CrashDiagnostics.shared.start()
    }
}
