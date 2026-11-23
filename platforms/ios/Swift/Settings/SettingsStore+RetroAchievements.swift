// SPDX-License-Identifier: GPL-3.0+
import Foundation

extension SettingsStore {
    static let _achievementsEnabledConfig = Setting<Bool>(
        section: "Achievements", key: "Enabled", default: false,
        codec: .bool)
    static let _achievementsChallengeModeConfig = Setting<Bool>(
        section: "Achievements", key: "ChallengeMode", default: false,
        codec: .bool)
}
