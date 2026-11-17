// DiagnosticsDevTrigger.swift — developer-only report build, driven by a launch environment variable
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import ImageIO
import UIKit
import os

enum DiagnosticsDevTrigger {
    private static let started = OSAllocatedUnfairLock(initialState: false)
    private static let captureStarted = OSAllocatedUnfairLock(initialState: false)

    // Selector: last (newest past session), now (the running one), verdicts (every ending) or an exact session folder name.
    @MainActor static func runIfRequested() {
        let env = ProcessInfo.processInfo.environment
        startCaptureSpike(env)
        guard let selector = [env["ARMSX2_BUILD_REPORT"], env["SIMCTL_CHILD_ARMSX2_BUILD_REPORT"]]
            .compactMap({ $0 }).first(where: { !$0.isEmpty }) else { return }
        guard !started.withLock({ let was = $0; $0 = true; return was }) else { return }
        let displayMaxFPS = UIScreen.main.maximumFramesPerSecond
        let extrasFolder = [env["ARMSX2_REPORT_EXTRAS"], env["SIMCTL_CHILD_ARMSX2_REPORT_EXTRAS"]]
            .compactMap({ $0 }).first(where: { !$0.isEmpty }).map { URL(fileURLWithPath: $0, isDirectory: true) }
        Task.detached(priority: .userInitiated) {
            let out = await report(for: selector, displayMaxFPS: displayMaxFPS, extrasFolder: extrasFolder)
            FileHandle.standardError.write(Data(out.utf8))
        }
    }

    // Spike: seconds to wait once the VM runs, then pause and capture twice.
    @MainActor private static func startCaptureSpike(_ env: [String: String]) {
        guard let raw = [env["ARMSX2_CAPTURE_FRAME"], env["SIMCTL_CHILD_ARMSX2_CAPTURE_FRAME"]]
            .compactMap({ $0 }).first(where: { !$0.isEmpty }) else { return }
        guard !captureStarted.withLock({ let was = $0; $0 = true; return was }) else { return }
        let delay = Int(raw) ?? 8
        Task { @MainActor in
            for _ in 0..<240 where !ARMSX2Bridge.isVMRunning() { try? await Task.sleep(for: .milliseconds(500)) }
            try? await Task.sleep(for: .seconds(delay))
            note("phase=pause")
            ARMSX2Bridge.setVMPaused(true)
            try? await Task.sleep(for: .seconds(1))
            for n in 1...2 {
                let start = ContinuousClock.now
                let png = await ARMSX2Bridge.capturePausedFrame()
                let ms = Int((ContinuousClock.now - start) / .milliseconds(1))
                var size = (w: 0, h: 0)
                if let png, let src = CGImageSourceCreateWithData(png as CFData, nil),
                   let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] {
                    size = (props[kCGImagePropertyPixelWidth] as? Int ?? 0, props[kCGImagePropertyPixelHeight] as? Int ?? 0)
                }
                note("n=\(n) ok=\(png == nil ? 0 : 1) w=\(size.w) h=\(size.h) ms=\(ms) bytes=\(png?.count ?? 0)")
                if n == 1, let png { try? png.write(to: .documentsDirectory.appending(path: "paused-frame-spike.png")) }
                if n == 1 { try? await Task.sleep(for: .seconds(3)) }
            }
            note("phase=resume")
            ARMSX2Bridge.setVMPaused(false)
            note("done=1")
        }
    }

    private static func note(_ text: String) {
        FileHandle.standardError.write(Data("@@DIAG_FRAME@@ \(text)\n".utf8))
    }

    private static func report(for selector: String, displayMaxFPS: Int, extrasFolder: URL?) async -> String {
        if selector == "verdicts" { return verdicts() }
        do {
            guard let id = resolve(selector) else { throw DiagnosticsError.invalidSession(selector) }
            let extras = extrasFolder.map(loadExtras) ?? ReportExtras()
            let zip = try await ReportBuilder.build(sessionID: id, displayMaxFPS: displayMaxFPS, extras: extras)
            if let extrasFolder {
                let text = try await ReportBuilder.summaryText(sessionID: id, displayMaxFPS: displayMaxFPS, form: extras.form)
                try Data(text.utf8).write(to: extrasFolder.appendingPathComponent("summary-out.md"))
            }
            return "@@DIAG_REPORT@@ ok=1 zip=\(zip.lastPathComponent)\n"
        } catch {
            let reason = "\(error)".replacingOccurrences(of: "\"", with: "'").replacingOccurrences(of: "\n", with: " ")
            return "@@DIAG_REPORT@@ ok=0 error=\"\(reason)\"\n"
        }
    }

    // form.json keys are kinds, place, what and scene; the other files are read when present.
    private static func loadExtras(from folder: URL) -> ReportExtras {
        var extras = ReportExtras()
        let json = (try? Data(contentsOf: folder.appendingPathComponent("form.json")))
            .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] } ?? [:]
        extras.form = ReportForm(kinds: (json["kinds"] as? [String] ?? []).compactMap(ReportKind.init(rawValue:)),
                                 place: (json["place"] as? String).flatMap(ReportPlace.init(rawValue:)),
                                 whatHappened: json["what"] as? String ?? "", scene: json["scene"] as? String ?? "")
        if let photo = try? Data(contentsOf: folder.appendingPathComponent("photo.jpg")) {
            extras.photoJPEG = ReportPhoto.reencode(photo)
            if extras.photoJPEG == nil { FileHandle.standardError.write(Data("@@DIAG_REPORT@@ photo=dropped\n".utf8)) }
        }
        extras.pausedFramePNG = try? Data(contentsOf: folder.appendingPathComponent("frame.png"))
        let crash = folder.appendingPathComponent("crash.ips")
        if FileManager.default.fileExists(atPath: crash.path) { extras.crashFiles = [crash] }
        return extras
    }

    // One ending per kept session in a single launch, and no zip.
    private static func verdicts() -> String {
        let ids = SessionStore.list()
        var out = ""
        for id in ids {
            let ending = id == SessionStore.currentID ? "running" : EndingParser.verdict(of: id).ending.rawValue
            out += "@@DIAG_VERDICT@@ session=\(id) ending=\(ending)\n"
        }
        return out + "@@DIAG_VERDICT@@ done=1 count=\(ids.count)\n"
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
