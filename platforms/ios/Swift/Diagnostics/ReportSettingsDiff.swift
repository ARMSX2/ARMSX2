// ReportSettingsDiff.swift — the report's settings file: fixed groups plus every key that differs, each tagged with its layer
// SPDX-License-Identifier: GPL-3.0+

import Foundation

// Defaults, then the global INI, then the game's INI: the order the emulator itself layers them.
struct SettingsLayers {
    enum Source: String {
        case game, global
        case coreDefault = "default"
    }

    typealias INI = [String: [String: String]]

    let defaults: INI
    let global: INI
    let game: INI

    init(defaults: String, global: String, game: String) {
        self.defaults = Self.parse(defaults)
        self.global = Self.parse(global)
        self.game = Self.parse(game)
    }

    // Case-sensitive, a later duplicate wins, and ; or # lines are comments.
    static func parse(_ text: String) -> INI {
        var result: INI = [:]
        var section = ""
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix(";") || line.hasPrefix("#") { continue }
            if line.hasPrefix("["), line.hasSuffix("]") {
                section = String(line.dropFirst().dropLast())
            } else if let equals = line.firstIndex(of: "=") {
                let key = line[..<equals].trimmingCharacters(in: .whitespaces)
                result[section, default: [:]][key] = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            }
        }
        return result
    }

    func effective(_ section: String, _ key: String) -> (value: String, source: Source)? {
        if let value = game[section]?[key] { return (value, .game) }
        if let value = global[section]?[key] { return (value, .global) }
        if let value = defaults[section]?[key] { return (value, .coreDefault) }
        return nil
    }

    func hasDefault(_ section: String, _ key: String) -> Bool {
        defaults[section]?[key] != nil
    }

    // nil when the key is set nowhere, so the caller can tell off from unknown.
    func isOn(_ section: String, _ key: String) -> Bool? {
        effective(section, key).map { ["true", "1"].contains($0.value.lowercased()) }
    }
}

enum ReportSettingsDiff {
    private struct Entry {
        let section: String
        let key: String
        var core = true
    }

    private static let gs = "EmuCore/GS"
    private static let speed = "EmuCore/Speedhacks"
    private static let cpu = "EmuCore/CPU"
    private static let recompiler = "EmuCore/CPU/Recompiler"
    private static let pacing = "ARMSX2iOS/FramePacing"

    // Pinned from Pcsx2Config.cpp (LoadSave bodies); a spelling that drifts shows up as (unknown key).
    private static let groups: [(title: String, entries: [Entry])] = [
        ("CTX", [Entry(section: gs, key: "Renderer")]
            + ["fpuOverflow", "fpuExtraOverflow", "fpuFullMode", "fpuExactMode"].map { Entry(section: recompiler, key: $0) }
            + ["vu0Overflow", "vu0ExtraOverflow", "vu0SignOverflow", "vu0ExactMode",
               "vu1Overflow", "vu1ExtraOverflow", "vu1SignOverflow", "vu1ExactMode"].map { Entry(section: recompiler, key: $0) }
            + ["FPU.Roundmode", "FPUDiv.Roundmode", "VU0.Roundmode", "VU1.Roundmode"].map { Entry(section: cpu, key: $0) }
            + ["EECycleRate", "EECycleSkip"].map { Entry(section: speed, key: $0) }
            + [Entry(section: gs, key: "upscale_multiplier")]),
        ("Speedhacks", ["vuThread", "vu1Instant", "WaitLoop", "IntcStat", "fastCDVD"].map { Entry(section: speed, key: $0) }),
        ("Recompilers", ["EnableEE", "EnableIOP", "EnableVU0", "EnableVU1", "EnableFastmem"].map { Entry(section: recompiler, key: $0) }),
        ("GS accuracy", ["accurate_blending_unit", "texture_preloading", "hw_mipmap", "HWDownloadMode", "UserHacks"]
            .map { Entry(section: gs, key: $0) }),
        ("iOS frame pacing", [Entry(section: pacing, key: "Preset", core: false), Entry(section: pacing, key: "DynamicResolution", core: false),
                              Entry(section: gs, key: "VsyncEnable"), Entry(section: "Framerate", key: "NominalScalar")]),
        ("Global switches", ["EnablePatches", "EnableCheats", "EnableWideScreenPatches", "EnableFastBoot"]
            .map { Entry(section: "EmuCore", key: $0) }),
    ]

    // Anything that could name an account or hold a credential is left out of every line.
    static func isSensitive(_ key: String) -> Bool {
        let lower = key.lowercased()
        return lower == "username" || ["token", "password", "secret", "login", "auth"].contains { lower.contains($0) }
    }

    static func render(defaults: String, global: String, game: String, displayMaxFPS: Int) -> String {
        let layers = SettingsLayers(defaults: defaults, global: global, game: game)
        var out = "Values are as of report build time (the INI is one file, not a per-session snapshot). "
            + "Source: game = per-game INI, global = PCSX2-iOS.ini, default = core default.\n"
            + "GameDB overrides applied at boot are not shown.\n"
        var shown = Set<String>()
        for group in groups {
            out += "\n## \(group.title)\n"
            if group.title == "iOS frame pacing" {
                out += "ProMotion 120 Hz = \(displayMaxFPS >= 120 ? "yes" : "no") (display maximum \(displayMaxFPS) Hz)\n"
            }
            for entry in group.entries {
                shown.insert(entry.section + "\u{0}" + entry.key)
                out += line(entry, layers) + "\n"
            }
        }
        out += "\n## Other keys that differ from the default\n"
        var others: [String] = []
        for (section, keys) in layers.global.merging(layers.game, uniquingKeysWith: { $0.merging($1, uniquingKeysWith: { _, new in new }) }) {
            for key in keys.keys where !shown.contains(section + "\u{0}" + key) && !isSensitive(key) {
                guard let now = layers.effective(section, key) else { continue }
                let base = layers.defaults[section]?[key]
                if let base, same(base, now.value) { continue }
                others.append("\(section)/\(key) = \(now.value) (\(now.source.rawValue)\(base == nil ? ", no core default" : ""))")
            }
        }
        out += others.isEmpty ? "(none)\n" : others.sorted().joined(separator: "\n") + "\n"
        return out
    }

    private static func line(_ entry: Entry, _ layers: SettingsLayers) -> String {
        let effective = layers.effective(entry.section, entry.key)
        if entry.core {
            guard layers.hasDefault(entry.section, entry.key) else { return "\(entry.key) = (unknown key)" }
            return "\(entry.key) = \(effective?.value ?? "") (\(effective?.source.rawValue ?? ""))"
        }
        guard let effective else { return "\(entry.key) = (not set)" }
        return "\(entry.key) = \(effective.value) (\(effective.source.rawValue), no core default)"
    }

    // "1" and "1.000000" are the same number, and "True" the same boolean as "true".
    private static func same(_ a: String, _ b: String) -> Bool {
        if let x = Double(a), let y = Double(b) { return x == y }
        return a.lowercased() == b.lowercased()
    }
}
