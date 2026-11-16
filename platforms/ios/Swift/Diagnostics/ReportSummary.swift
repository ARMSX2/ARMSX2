// ReportSummary.swift — the text of summary.md, also what Phase 13's Copy Summary puts on the clipboard
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct DeviceFacts {
    let model: String
    let osVersion: String
    let osBuild: String
    let ramMiB: Int
    let appVersion: String
    let appBuild: String

    // The machine id, never the name the user gave the device.
    static func current() -> DeviceFacts {
        var info = utsname()
        uname(&info)
        let machine = withUnsafeBytes(of: &info.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let version = ProcessInfo.processInfo.operatingSystemVersion
        var size = 0
        sysctlbyname("kern.osversion", nil, &size, nil, 0)
        var buffer = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("kern.osversion", &buffer, &size, nil, 0)
        let bundle = Bundle.main.infoDictionary
        return DeviceFacts(
            model: ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] ?? machine,
            osVersion: "\(version.majorVersion).\(version.minorVersion).\(version.patchVersion)",
            osBuild: String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self),
            ramMiB: Int(ProcessInfo.processInfo.physicalMemory >> 20),
            appVersion: bundle?["CFBundleShortVersionString"] as? String ?? "unknown",
            appBuild: bundle?["CFBundleVersion"] as? String ?? "unknown")
    }
}

struct EarlierRun {
    let id: String
    let started: String
    let endingText: String
    let game: String?
}

// nil is a line the log never had, which prints blank; the wording upstream may change.
struct EmuLogCounts {
    var enabledPatchLines: Int?
    var gamePatchesFound: Int?
    var cheatsFound: Int?

    static let needles = ["Enabled patch: ", "game patches in", "cheats in "]

    mutating func add(_ line: String) {
        if line.contains("Enabled patch: ") {
            enabledPatchLines = (enabledPatchLines ?? 0) + 1
        } else if let found = Self.number(in: line, before: " game patches in") {
            gamePatchesFound = found
        } else if let found = Self.number(in: line, before: " cheats in ") {
            cheatsFound = found
        }
    }

    private static func number(in line: String, before tail: String) -> Int? {
        guard let end = line.range(of: tail),
              let start = line.range(of: "Found ", options: .backwards, range: line.startIndex..<end.lowerBound) else { return nil }
        return Int(line[start.upperBound..<end.lowerBound])
    }
}

enum ReportSummary {
    static let unknown = "unknown (not in this session's log)"

    static func endingText(_ ending: Ending, running: Bool) -> String {
        running ? "Still running (running)" : "\(ending.label) (\(ending.rawValue))"
    }

    static func markdown(sessionID: String, started: String, verdict: SessionVerdict, running: Bool, device: DeviceFacts,
                         layers: SettingsLayers, counts: EmuLogCounts, earlier: [EarlierRun]) -> String {
        let facts = verdict.facts
        let game = facts.game
        var out = "# ARMSX2 problem report summary\n\n"
        out += "Report format: 1\nSession: \(sessionID)\nStarted: \(started)\n"
        out += "Ending: \(endingText(verdict.ending, running: running))\n"

        out += "\n## Device\n"
        out += "Device model: \(device.model)\niOS version: \(device.osVersion) (\(device.osBuild))\nRAM: \(device.ramMiB) MiB\n"
        out += "App version: \(device.appVersion) (\(device.appBuild))\n"

        let jit = jitFacts(facts)
        out += "\n## JIT\nJIT method: \(jit.method)\nJIT came up: \(jit.up)\nTXM: \(jit.txm)\nJIT fallback cause: \(jit.cause)\n"

        out += "\n## Game\n"
        out += "Serial: \(game.map { $0.serial.isEmpty ? unknown : $0.serial } ?? unknown)\n"
        out += "CRC: \(game.map { $0.crc.isEmpty ? unknown : $0.crc } ?? unknown)\n"
        out += "Title: \(game.map { $0.title.isEmpty ? unknown : $0.title } ?? unknown)\n"
        out += "Disc format: \(game.map { $0.format.isEmpty ? unknown : $0.format } ?? unknown)\n"

        out += "\n## Patches (settings as of report build time)\n"
        out += "Patches: \(flag(layers, "EnablePatches")) (found: \(blank(counts.gamePatchesFound)), enabled: \(blank(counts.enabledPatchLines)))\n"
        out += "Cheats: \(flag(layers, "EnableCheats")) (found: \(blank(counts.cheatsFound)))\n"
        out += "Widescreen fixes: \(flag(layers, "EnableWideScreenPatches"))\n"

        out += "\n## Session\n"
        out += "Worst thermal state: \(facts.sampleCount == 0 ? unknown : facts.worstThermal)\n"
        out += "Last fps before a pause: \(facts.fpsBeforePause.map { String(format: "%.1f", $0) } ?? "none")\n"

        out += "\n## Earlier runs\n"
        out += earlier.isEmpty ? "none kept\n" : earlier.map {
            "Earlier run \($0.id) (\($0.started)): \($0.endingText) - \($0.game ?? "Menu")\n"
        }.joined()
        return out
    }

    private static func flag(_ layers: SettingsLayers, _ key: String) -> String {
        layers.isOn("EmuCore", key).map { $0 ? "on" : "off" } ?? "unknown"
    }

    private static func blank(_ count: Int?) -> String {
        count.map(String.init) ?? ""
    }

    // Read from the last line of each tag, since the engine prints a tag again after a retry.
    static func jitFacts(_ facts: SessionFacts) -> (method: String, up: String, txm: String, cause: String) {
        let mode = field("mode", in: facts.lines["JIT_MODE"])
        let txm = field("txm", in: facts.lines["JIT_MODE"])
        let detect = facts.lines["JIT_DETECT"] ?? ""
        let gate = facts.lines["BOOT_JIT_GATE"] ?? ""
        let dopamine = field("systemhook", in: facts.lines["JIT_JAILBREAK"]) == "1"

        let interpreter = gate.contains("available=0") || detect.contains("result=UNAVAILABLE")
        let up: String
        if interpreter {
            up = "no"
        } else if mode == "Simulator" || detect.contains("CS_DEBUGGED=1") || detect.contains("result=AVAILABLE") || gate.contains("available=1") {
            up = "yes"
        } else {
            up = unknown
        }

        let cause: String
        if interpreter {
            cause = "interpreter fallback (JIT unavailable)"
        } else if facts.txmFallback {
            cause = "TXM protocol timed out, legacy brk used"
        } else if up != "yes" {
            cause = unknown
        } else if mode == "Simulator" {
            cause = "none (simulator)"
        } else if dopamine {
            cause = "none (Dopamine)"
        } else {
            cause = "none (external debugger)"
        }
        return (mode ?? unknown, up, txm.map { $0 == "1" ? "yes" : "no" } ?? unknown, cause)
    }

    private static func field(_ key: String, in line: String?) -> String? {
        line.flatMap { MarkerLine.parse($0)?.fields[key] }
    }
}
