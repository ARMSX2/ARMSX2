// SettingsStore+CPU.swift — CPU rounding/clamp labels and pure helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    static let roundModeLabels = ["Nearest", "Negative", "Positive", "Chop (Zero)"]
    static let eeClampModeLabels = ["None", "Normal", "Extra", "Full"]
    static let vuClampModeLabels = ["None", "Normal", "Extra", "Extra + Sign"]

    static func clamped(_ value: Int, to range: ClosedRange<Int>) -> Int {
        min(max(value, range.lowerBound), range.upperBound)
    }

    static func eeClampModeFromBools(_ overflow: Bool, _ extra: Bool, _ full: Bool) -> Int {
        if full { return 3 }
        if extra { return 2 }
        if overflow { return 1 }
        return 0
    }

    static func vuClampModeFromBools(_ overflow: Bool, _ extra: Bool, _ sign: Bool) -> Int {
        if sign { return 3 }
        if extra { return 2 }
        if overflow { return 1 }
        return 0
    }
}
