// DiagnosticsDevTrigger.swift — developer-only report build, driven by a launch environment variable
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import os

enum DiagnosticsDevTrigger {
    private static let started = OSAllocatedUnfairLock(initialState: false)

    // Selector: last (newest past session), now (the running one) or an exact session folder name.
    static func runIfRequested() {
        let env = ProcessInfo.processInfo.environment
        guard let selector = [env["ARMSX2_BUILD_REPORT"], env["SIMCTL_CHILD_ARMSX2_BUILD_REPORT"]]
            .compactMap({ $0 }).first(where: { !$0.isEmpty }) else { return }
        guard !started.withLock({ let was = $0; $0 = true; return was }) else { return }
        Task.detached(priority: .userInitiated) {
            FileHandle.standardError.write(Data(await report(for: selector).utf8))
        }
    }

    private static func report(for selector: String) async -> String {
        do {
            guard let id = resolve(selector) else { throw DiagnosticsError.invalidSession(selector) }
            let zip = try await ReportBuilder.build(sessionID: id)
            return "@@DIAG_REPORT@@ ok=1 zip=\(zip.lastPathComponent)\n"
        } catch {
            let reason = "\(error)".replacingOccurrences(of: "\"", with: "'").replacingOccurrences(of: "\n", with: " ")
            return "@@DIAG_REPORT@@ ok=0 error=\"\(reason)\"\n"
        }
    }

    // Anything that is not a well-formed id of an existing folder is refused before a file is read.
    static func resolve(_ selector: String, documents: URL = .documentsDirectory) -> String? {
        let sessions = SessionStore.list(documents: documents)
        switch selector {
        case "last": return sessions.last { $0 != SessionStore.currentID }
        case "now": return SessionStore.currentID
        default: return SessionStore.isValidID(selector) && sessions.contains(selector) ? selector : nil
        }
    }
}
