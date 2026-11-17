// ReportBuilder.swift — stages an allowlisted copy of one session and zips it into Documents/Reports
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum DiagnosticsError: Error, CustomStringConvertible {
    case invalidSession(String)
    case notRegularFile(String)
    case zipFailed(String)
    case redactionLeak(String)

    var description: String {
        switch self {
        case .invalidSession(let id): "invalid session \(id)"
        case .notRegularFile(let name): "\(name) is not a regular file"
        case .zipFailed(let reason): "zip failed: \(reason)"
        case .redactionLeak(let file): "redaction left private text in \(file)"
        }
    }
}

enum ReportBuilder {
    private static let keptReports = 5
    private static let gameININame = #"^[A-Z0-9-]+_[0-9A-F]{8}\.ini$"#

    static func build(sessionID: String, displayMaxFPS: Int, extras: ReportExtras = ReportExtras(),
                      documents: URL = .documentsDirectory) async throws -> URL {
        let files = FileManager.default
        let folder = try sessionFolder(sessionID, documents: documents)
        let running = sessionID == SessionStore.currentID
        // The INI save state belongs to the main queue and the INI is read straight after.
        await MainActor.run { ARMSX2Bridge.flushINISettings() }

        // The kill a build can cause ends the running process, so its folder gets SessionStore.buildingMarkerName.
        var marker: URL?
        if let current = SessionStore.currentID, let currentFolder = SessionStore.url(for: current, documents: documents),
           files.fileExists(atPath: currentFolder.path) {
            let url = currentFolder.appendingPathComponent(SessionStore.buildingMarkerName)
            files.createFile(atPath: url.path, contents: nil)
            marker = url
        }
        defer { if let marker { try? files.removeItem(at: marker) } }
        return try stageAndZip(sessionID: sessionID, folder: folder, running: running, displayMaxFPS: displayMaxFPS,
                               extras: extras, documents: documents)
    }

    // summaryText is what Copy Summary pastes, built by the function that writes summary.md so the two cannot differ.
    static func summaryText(sessionID: String, displayMaxFPS: Int, form: ReportForm = ReportForm(),
                            documents: URL = .documentsDirectory) async throws -> String {
        let folder = try sessionFolder(sessionID, documents: documents)
        await MainActor.run { ARMSX2Bridge.flushINISettings() }
        let gathered = try gather(sessionID: sessionID, folder: folder, running: sessionID == SessionStore.currentID,
                                  displayMaxFPS: displayMaxFPS, documents: documents)
        let text = summary(gathered, form: form)
        // The clipboard leaves the app, so it gets the same second check the zip gets.
        guard ReportRedaction.tripwireHits(in: text, context: gathered.context).isEmpty else {
            throw DiagnosticsError.redactionLeak("summary")
        }
        return text
    }

    private static func sessionFolder(_ id: String, documents: URL) throws -> URL {
        guard let folder = SessionStore.url(for: id, documents: documents),
              FileManager.default.fileExists(atPath: folder.path) else { throw DiagnosticsError.invalidSession(id) }
        return folder
    }

    private struct Gathered {
        let machineSummary: String
        let settings: String
        let pairs: [(URL, String, Bool)]
        let context: ReportRedaction.Context
    }

    private static func gather(sessionID: String, folder: URL, running: Bool, displayMaxFPS: Int, documents: URL) throws -> Gathered {
        let verdict = verdict(of: sessionID, running: running, documents: documents)
        let global = text(of: documents.appendingPathComponent("PCSX2-iOS.ini"))
            ?? text(of: documents.appendingPathComponent("ARMSX2-iOS.ini")) ?? ""
        let gameName: String? = verdict.facts.game.map { "\($0.serial)_\($0.crc.uppercased()).ini" }.flatMap {
            $0.range(of: gameININame, options: .regularExpression) != nil ? $0 : nil
        }
        let gameURL = gameName.map { documents.appendingPathComponent("gamesettings/\($0)") }
        let defaults = ARMSX2Bridge.defaultSettingsINIText()
        let game = gameURL.flatMap(text(of:)) ?? ""
        let layers = SettingsLayers(defaults: defaults, global: global, game: game)

        var counts = EmuLogCounts()
        let emulog = running ? documents.appendingPathComponent("logs/emulog.txt") : folder.appendingPathComponent("emulog.txt")
        if ReportStaging.isRegularFile(emulog) == true {
            try? SessionLogScanner.lines(of: emulog, containing: EmuLogCounts.needles) { counts.add($0) }
        }
        let older = SessionStore.list(documents: documents).filter { $0 < sessionID }
        let earlier = older.map { earlierRun($0, documents: documents) }
        let pairs = allowlist(session: sessionID, folder: folder, running: running, older: older,
                              gameINI: gameURL.map { ($0, "gamesettings/\($0.lastPathComponent)") }, documents: documents)
        let context = ReportRedaction.Context.live(documents: documents)
            .adding(userNames: ReportStaging.scanLoginNames(in: pairs.filter { $0.2 }.map { $0.0 }))
        // Names that cannot be matched would ship in clear and blind the tripwire, so stop here.
        guard context.namesCovered else { throw DiagnosticsError.redactionLeak("<ra-user>") }

        let machine = ReportSummary.markdown(sessionID: sessionID, started: started(sessionID, documents), verdict: verdict,
                                             running: running, device: .current(), layers: layers, counts: counts, earlier: earlier)
        let settings = ReportSettingsDiff.render(defaults: defaults, global: global, game: game, displayMaxFPS: displayMaxFPS)
        return Gathered(machineSummary: machine, settings: settings, pairs: pairs, context: context)
    }

    private static func summary(_ gathered: Gathered, form: ReportForm) -> String {
        ReportRedaction.redact(form.markdownBlock() + gathered.machineSummary, context: gathered.context)
    }

    private static func stageAndZip(sessionID: String, folder: URL, running: Bool, displayMaxFPS: Int, extras: ReportExtras,
                                    documents: URL) throws -> URL {
        let files = FileManager.default
        let name = "ARMSX2-report-\(SessionStore.format(Date()))"
        let staging = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true).appendingPathComponent(name)
        let inner = staging.appendingPathComponent(name, isDirectory: true)
        defer { try? files.removeItem(at: staging) }
        try? files.removeItem(at: staging)
        try files.createDirectory(at: inner, withIntermediateDirectories: true)

        let gathered = try gather(sessionID: sessionID, folder: folder, running: running, displayMaxFPS: displayMaxFPS,
                                  documents: documents)
        let context = gathered.context
        try write(ReportRedaction.redact(gathered.settings, context: context), as: "settings.txt", in: inner)
        try write(summary(gathered, form: extras.form), as: "summary.md", in: inner)

        for (source, stagedName, isLog) in gathered.pairs {
            if isLog {
                try ReportStaging.stageLog(source: source, as: stagedName, in: inner, context: context)
            } else {
                try ReportStaging.stageText(source: source, as: stagedName, in: inner, context: context)
            }
        }
        if let photo = extras.photoJPEG { try photo.write(to: inner.appendingPathComponent("photo.jpg")) }
        if let frame = extras.pausedFramePNG { try frame.write(to: inner.appendingPathComponent("paused-frame.png")) }
        for (index, crash) in extras.crashFiles.filter(CrashFileCheck.isOwnCrashFile).prefix(3).enumerated() {
            try ReportStaging.stageText(source: crash, as: "crash/attached-\(index + 1).ips", in: inner, context: context,
                                        limit: 4 << 20)
        }
        if let leftover = ReportStaging.tripwireScan(stagingRoot: inner, context: context) {
            throw DiagnosticsError.redactionLeak(leftover)
        }
        return try makeZip(inner, named: name, documents: documents)
    }

    private static func makeZip(_ inner: URL, named name: String, documents: URL) throws -> URL {
        let files = FileManager.default
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
        // Names are timestamps, so a name sort is a time sort.
        let zips = ((try? files.contentsOfDirectory(atPath: reports.path)) ?? [])
            .filter { $0.hasPrefix("ARMSX2-report-") && $0.hasSuffix(".zip") }.sorted()
        for old in zips.dropLast(keptReports) { try? files.removeItem(at: reports.appendingPathComponent(old)) }
        return zip
    }

    // Built in code; the only folders listed are this session's metrickit one, whose names are checked, and the session ids.
    // photo.jpg, paused-frame.png and crash/attached-N.ips are named in stageAndZip, never from a user path.
    private static func allowlist(session: String, folder: URL, running: Bool, older: [String], gameINI: (URL, String)?,
                                  documents: URL) -> [(URL, String, Bool)] {
        var pairs: [(URL, String, Bool)] = [
            (logURL("pcsx2_log.txt", session, folder, running, documents), "pcsx2_log.txt", true),
            (logURL("emulog.txt", session, folder, running, documents), "emulog.txt", true),
            (folder.appendingPathComponent("session.json"), "session.json", false),
            (documents.appendingPathComponent("PCSX2-iOS.ini"), "PCSX2-iOS.ini", false),
        ]
        if let gameINI { pairs.append((gameINI.0, gameINI.1, false)) }
        let payloads = folder.appendingPathComponent("metrickit", isDirectory: true)
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: payloads.path)) ?? []).sorted()
        for name in names where name.range(of: CrashFileCheck.payloadName, options: .regularExpression) != nil {
            pairs.append((payloads.appendingPathComponent(name), "metrickit/\(name)", false))
        }
        for id in older {
            guard let olderFolder = SessionStore.url(for: id, documents: documents) else { continue }
            let live = id == SessionStore.currentID
            for log in ["pcsx2_log.txt", "emulog.txt"] {
                pairs.append((logURL(log, id, olderFolder, live, documents), "older/\(id)-\(log)", true))
            }
        }
        return pairs
    }

    // The running session's logs are still the two live files in Documents.
    private static func logURL(_ log: String, _ id: String, _ folder: URL, _ live: Bool, _ documents: URL) -> URL {
        guard live else { return folder.appendingPathComponent(log) }
        return log == "emulog.txt" ? documents.appendingPathComponent("logs/emulog.txt") : documents.appendingPathComponent(log)
    }

    private static func verdict(of id: String, running: Bool, documents: URL) -> SessionVerdict {
        running ? EndingParser.verdict(log: documents.appendingPathComponent("pcsx2_log.txt"), buildingMarkerPresent: true)
            : EndingParser.verdict(of: id, documents: documents)
    }

    private static func earlierRun(_ id: String, documents: URL) -> EarlierRun {
        let live = id == SessionStore.currentID
        let verdict = verdict(of: id, running: live, documents: documents)
        let title = verdict.facts.game?.title
        return EarlierRun(id: id, started: started(id, documents), endingText: ReportSummary.endingText(verdict.ending, running: live),
                          game: title?.isEmpty == false ? title : nil)
    }

    private static func started(_ id: String, _ documents: URL) -> String {
        SessionStore.startedDate(of: id, documents: documents).map { $0.formatted(.iso8601) } ?? id
    }

    // A settings INI is a few KiB; anything that is not a small regular file reads as absent.
    private static func text(of url: URL) -> String? {
        guard ReportStaging.isRegularFile(url) == true,
              ((try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0) <= 1 << 20,
              let data = try? Data(contentsOf: url) else { return nil }
        return String(decoding: data, as: UTF8.self)
    }

    private static func write(_ text: String, as name: String, in folder: URL) throws {
        try Data(text.utf8).write(to: folder.appendingPathComponent(name))
    }
}
