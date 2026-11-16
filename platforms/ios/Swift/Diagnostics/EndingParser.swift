// EndingParser.swift — how a session ended, read from the marker lines of its log
// SPDX-License-Identifier: GPL-3.0+

import Foundation

// The only copies of the numbers; render-min comes from first frames on a phone (2 discs, slowest 3.0 s).
enum EndingThresholds {
    static let freezeTailSamples = 20
    static let renderMinSamples = 10
    // Boots measured 3 s on a fast phone; 90 leaves room for an A10, the TXM fallback wait and cold boots.
    static let stuckBootSeconds = 90.0
    static let jetsamHeadroomMiB = 350
    static let criticalWindowSeconds = 60.0
    static let jetsamLookBackSamples = 10
    static let freezeFpsFloor = 0.5
    static let renderedFpsFloor = 1.0
}

// The raw values are grepped across tester reports, so they never change.
enum Ending: String {
    case noGame = "no_game"
    case fatal = "fatal"
    case bootFailed = "boot_failed"
    case cleanStop = "clean_stop"
    case jetsam = "jetsam"
    case freeze = "freeze"
    case leftBoot = "left_boot"
    case stuckBoot = "stuck_boot"
    case endedPaused = "ended_paused"
    case killedRunning = "killed_running"

    var label: String {
        switch self {
        case .noGame: "No game booted"
        case .fatal: "Fatal error (crash)"
        case .bootFailed: "Boot failed"
        case .cleanStop: "Stopped normally"
        case .jetsam: "Likely out of memory"
        case .freeze: "Froze"
        case .leftBoot: "Closed during boot"
        case .stuckBoot: "Stuck at boot"
        case .endedPaused: "Closed while paused or in background"
        case .killedRunning: "Ended abruptly while running"
        }
    }
}

struct GameIdentity {
    let serial: String
    let crc: String
    let currentCRC: String
    let format: String
    let title: String
}

struct SessionFacts {
    var game: GameIdentity?
    var booted = false
    // The last raw line of each tag the summary reads; the alloc marker prints many lines.
    var lines: [String: String] = [:]
    var txmFallback = false
    var worstThermal = "unknown"
    var fpsBeforePause: Double?
    var lastT = 0.0
    var sampleCount = 0
}

struct SessionVerdict {
    let ending: Ending
    let facts: SessionFacts
}

enum EndingParser {
    private static let thermalOrder = ["nominal", "fair", "serious", "critical"]
    private static let factTags: Set = ["JIT_MODE", "JIT_DETECT", "JIT_JAILBREAK", "BOOT_JIT_GATE", "JIT_ALLOC", "IOS_JIT_POLICY", "BUILD_ID"]

    // What one boot attempt did; a new boot-thread signal starts it over.
    private struct Segment {
        var startT = 0.0
        var rendered = 0
        var tail: [(state: Int, fps: Double)] = []
        var headrooms: [Int] = []
        var last: (state: Int, background: Bool)?
        var hasGame = false
        var stopped = false
        var bootFailed = false
        var criticalTimes: [Double] = []
        var backgrounded = false

        var tailDead: Bool {
            tail.count >= EndingThresholds.freezeTailSamples
                && tail.allSatisfy { $0.state == 2 && $0.fps < EndingThresholds.freezeFpsFloor }
        }
    }

    static func verdict(of id: String, documents: URL = .documentsDirectory) -> SessionVerdict {
        guard let folder = SessionStore.url(for: id, documents: documents) else {
            return SessionVerdict(ending: .noGame, facts: SessionFacts())
        }
        let building = FileManager.default.fileExists(atPath: folder.appendingPathComponent(SessionStore.buildingMarkerName).path)
        return verdict(log: folder.appendingPathComponent("pcsx2_log.txt"), buildingMarkerPresent: building)
    }

    // The live log of a running session goes through here, so a torn last line must not matter.
    static func verdict(log: URL, buildingMarkerPresent: Bool) -> SessionVerdict {
        var facts = SessionFacts()
        var segment = Segment()
        var fatal = false
        var sawSignal = false
        var pausedSeen = false
        var lastRunningFps: Double?
        var worstRank = -1
        try? SessionLogScanner.markerLines(of: log) { line in
            if line.contains("Aborting application.") { fatal = true }
            guard let marker = MarkerLine.parse(line) else { return }
            let fields = marker.fields
            if let t = Double(fields["t"] ?? "") { facts.lastT = max(facts.lastT, t) }
            switch marker.tag {
            case "DIAG_FATAL":
                fatal = true
            case "BOOT_THREAD_SIGNAL":
                sawSignal = true
                segment = Segment(startT: facts.lastT)
            case "BOOT_FAIL", "BOOT_BIOS_FAIL", "ISO_BOOT_MISSING":
                segment.bootFailed = true
            case "BOOT_VM_INIT":
                if fields["success"] == "1" {
                    facts.booted = true
                    segment.bootFailed = false
                } else if fields["success"] == "0" {
                    segment.bootFailed = true
                }
            case "DIAG_GAME":
                let empty = fields["serial"] == "-" && (Int(fields["crc"] ?? "", radix: 16) ?? 0) == 0 && (fields["title"] ?? "").isEmpty
                guard !empty else { return }
                facts.game = GameIdentity(serial: fields["serial"] ?? "", crc: fields["crc"] ?? "",
                    currentCRC: fields["cur_crc"] ?? "", format: fields["fmt"] ?? "", title: fields["title"] ?? "")
                segment.hasGame = true
                segment.stopped = false
            case "DIAG_STOP":
                segment.stopped = segment.hasGame
            case "DIAG_MEM":
                if fields["level"] == "critical" { segment.criticalTimes.append(facts.lastT) }
            case "DIAG_APP":
                segment.backgrounded = fields["phase"] == "background"
            case "DIAG_PERF":
                // Thermal is the last field, so a torn line fails this guard.
                guard let fps = Double(fields["fps"] ?? ""), let state = Int(fields["state"] ?? ""),
                      let rank = thermalOrder.firstIndex(of: fields["thermal"] ?? "") else { return }
                facts.sampleCount += 1
                if rank > worstRank {
                    worstRank = rank
                    facts.worstThermal = thermalOrder[rank]
                }
                if state == 3, !pausedSeen {
                    pausedSeen = true
                    facts.fpsBeforePause = lastRunningFps
                }
                if state == 2 {
                    if !pausedSeen { lastRunningFps = fps }
                    if fps > EndingThresholds.renderedFpsFloor { segment.rendered += 1 }
                }
                segment.tail.append((state, fps))
                segment.headrooms.append(Int(fields["headroom_mib"] ?? "") ?? 0)
                segment.last = (state, fields["bg"] == "1")
                if segment.tail.count > EndingThresholds.freezeTailSamples { segment.tail.removeFirst() }
                if segment.headrooms.count > EndingThresholds.jetsamLookBackSamples { segment.headrooms.removeFirst() }
            default:
                guard factTags.contains(marker.tag) else { return }
                facts.lines[marker.tag] = line
                if marker.tag == "JIT_ALLOC", line.contains("txm_universal_timeout") { facts.txmFallback = true }
            }
        }
        let ending = decide(facts: facts, segment: segment, fatal: fatal, sawSignal: sawSignal, building: buildingMarkerPresent)
        return SessionVerdict(ending: ending, facts: facts)
    }

    private static func decide(facts: SessionFacts, segment: Segment, fatal: Bool, sawSignal: Bool, building: Bool) -> Ending {
        // Fatal is checked before no_game so a crash at the menu still reads as a crash.
        if fatal { return .fatal }
        if !sawSignal && facts.game == nil && !facts.booted { return .noGame }
        if segment.bootFailed { return .bootFailed }
        if segment.stopped { return .cleanStop }
        let lowHeadroom = segment.headrooms.contains { $0 > 0 && $0 < EndingThresholds.jetsamHeadroomMiB }
        let criticalLate = segment.criticalTimes.contains { $0 >= facts.lastT - EndingThresholds.criticalWindowSeconds }
        if lowHeadroom || criticalLate { return building ? .killedRunning : .jetsam }
        let rendered = segment.rendered >= EndingThresholds.renderMinSamples
        if segment.tailDead && rendered { return .freeze }
        if !rendered && facts.lastT - segment.startT < EndingThresholds.stuckBootSeconds { return .leftBoot }
        if !rendered && (segment.tailDead || segment.tail.isEmpty) { return .stuckBoot }
        if let last = segment.last, last.state == 3 || last.background { return .endedPaused }
        if segment.backgrounded { return .endedPaused }
        return .killedRunning
    }
}
