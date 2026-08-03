// SettingsStore+Audio.swift — audio-domain helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    static func clampedEmulatorVolumePercent(_ value: Int) -> Int {
        clamped(value, to: emulatorVolumeRange)
    }
}
