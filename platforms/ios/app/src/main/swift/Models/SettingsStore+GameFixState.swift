// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Mutable game-fix behavior is isolated from stored settings.
extension SettingsStore {
    func gameFixEnabled(_ key: String) -> Bool {
        gameFixes[key] ?? false
    }

    func setGameFix(_ key: String, _ value: Bool) {
        gameFixes[key] = value
        guard !suppressINIWrites else { return }
        ARMSX2Bridge.setINIBool("EmuCore/Gamefixes", key: key, value: value)
    }

    static func loadGameFixes() -> [String: Bool] {
        var values: [String: Bool] = [:]
        for option in gameFixOptions {
            values[option.key] = ARMSX2Bridge.getINIBool("EmuCore/Gamefixes", key: option.key, defaultValue: false)
        }
        return values
    }

    // ── Graphics ──

    /// MetalFX Spatial upscaling requires iOS 16+ and a device GPU that supports it.
    /// Probes at runtime so the UI can hide the option on unsupported hardware.
}
