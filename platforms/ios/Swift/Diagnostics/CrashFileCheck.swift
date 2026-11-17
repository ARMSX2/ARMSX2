// CrashFileCheck.swift — decides whether a picked file is this app's own crash report, and whether a session has a MetricKit crash
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum CrashFileCheck {
    static let sizeLimit = 4 << 20
    static let processName = "ARMSX2iOS"
    static let payloadName = #"^[0-9TZ_-]+-\d+\.json$"#
    private static let payloadSizeLimit = 8 << 20
    private static let refusedMarkers = ["<?xml", "<plist", "bplist", "PrivateKey", "PRIVATE KEY"]

    static func isOwnCrashFile(_ url: URL) -> Bool {
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey]),
              values.isRegularFile == true, values.isSymbolicLink != true,
              let size = values.fileSize, size > 0, size <= sizeLimit,
              let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8),
              !refusedMarkers.contains(where: text.contains) else { return false }
        let parts = text.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false)
        guard let header = object(parts[0]), header["bug_type"] != nil else { return false }
        // Another app's report passes the header test, so the process is what decides.
        return header["app_name"] as? String == processName
            || parts.dropFirst().first.flatMap(object)?["procName"] as? String == processName
    }

    // A hang-only payload must not hide the manual .ips steps, so the payload has to name a crash.
    static func sessionHasPayload(id: String, documents: URL = .documentsDirectory) -> Bool {
        guard let folder = SessionStore.url(for: id, documents: documents)?.appendingPathComponent("metrickit", isDirectory: true)
        else { return false }
        let needle = Data("crashDiagnostics".utf8)
        return ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).contains { name in
            let file = folder.appendingPathComponent(name)
            guard name.range(of: payloadName, options: .regularExpression) != nil,
                  let values = try? file.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]), values.isRegularFile == true,
                  (values.fileSize ?? .max) <= payloadSizeLimit, let data = try? Data(contentsOf: file) else { return false }
            return data.range(of: needle) != nil
        }
    }

    private static func object(_ text: Substring) -> [String: Any]? {
        try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
    }
}
