// SettingsTypes.swift — small types the settings screens share
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// [P51] OSD preset levels
enum OsdPreset: Int, CaseIterable {
    case off = 0
    case simple = 1    // FPS + speed + CPU usage + device stats
    case detail = 2    // All except frame times graph
    case full = 3      // Everything
    case custom = 4    // User-defined toggle set (not derived from a preset table)

    var label: String {
        switch self {
        case .off: return "OFF"
        case .simple: return "Simple"
        case .detail: return "Detail"
        case .full: return "Full"
        case .custom: return "Custom"
        }
    }
}

/// Frame Pacing presets. Mirrors the OsdPreset shape so the consolidated
/// Settings panel and the per-game tab can drive a single picker that fans
/// out to the underlying EmuCore/GS + SPU2/Output + Framerate keys.
enum FramePacingPreset: Int, CaseIterable, Identifiable {
    case optimal = 0       // ARMSX2-tuned default for fresh installs
    case smooth = 1        // larger queues / buffers for visual stability
    case lowLatency = 2    // tight queues + low audio latency for input feel
    case batterySaver = 3  // 45 fps cap + larger audio buffer
    case custom = 4        // user-tweaked; not derived from the table

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .optimal: return "Optimal"
        case .smooth: return "Smooth"
        case .lowLatency: return "Low Latency"
        case .batterySaver: return "Battery Saver"
        case .custom: return "Custom"
        }
    }
}

enum JITScriptProtocol: String, CaseIterable, Identifiable {
    case universal
    case legacy

    var id: String { rawValue }

    var label: String {
        switch self {
        case .universal:
            return "Universal"
        case .legacy:
            return "Legacy"
        }
    }

    var subtitle: String {
        switch self {
        case .universal:
            return "Uses brk #0xf00d prepare + detach."
        case .legacy:
            return "Uses the iOS 17/18 scriptless/legacy JIT path."
        }
    }

    static var defaultValue: JITScriptProtocol {
        ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 26 ? .universal : .legacy
    }

    static func normalized(_ rawValue: String) -> JITScriptProtocol {
        switch rawValue.lowercased() {
        case "legacy", "utm-dolphin", "utm_dolphin":
            return .legacy
        default:
            return .universal
        }
    }
}

/// A manual per-fix toggle under EmuCore/Gamefixes. The `key` is the exact PCSX2
/// config key; `label` is the localized user-facing name.
struct GameFixOption: Identifiable, Hashable {
    let key: String
    let label: String
    var id: String { key }
}

/// Which analog stick an axis-inversion setting applies to.
enum StickSide: String, CaseIterable, Identifiable {
    case left, right
    var id: String { rawValue }
}
