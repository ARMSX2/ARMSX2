// SettingsStore+RumbleMigration.swift — phone rumble strength rescale migration
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    /// Slider value that reproduces the old full strength, and the factor for rescaling onto the
    /// new curve. GamepadHaptics.mm keeps its own copy.
    private static let phoneRumbleBaselineSetting: Float = 0.25

    /// PhoneRumbleStrength kept its key but changed meaning: 0.25 is now what 1.0 used to be, so
    /// without this everyone who ever moved the slider gets louder on update.
    ///
    /// Runs from SettingsStore.init(), so it writes the INI directly — touching SettingsStore.shared
    /// there re-enters the in-flight swift_once and deadlocks.
    static func migratePhoneRumbleStrengthRescaleV1() {
        let migrated = ARMSX2Bridge.getINIBool("ARMSX2iOS/Migrations", key: "PhoneRumbleStrengthRescaleV1", defaultValue: false)
        if migrated { return }

        // Negative sentinel to tell "never saved" from "saved zero". A fresh install has no key
        // and its 0.25 default is already right; rescaling that would quarter it.
        let stored = ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "PhoneRumbleStrength", defaultValue: -1.0)
        if stored >= 0.0 {
            let rescaled = min(max(stored * phoneRumbleBaselineSetting, 0.0), 1.0)
            ARMSX2Bridge.setINIFloat("ARMSX2iOS/UI", key: "PhoneRumbleStrength", value: rescaled)
            NSLog("[ARMSX2 iOS Settings] Phone rumble migration: %.2f -> %.2f", stored, rescaled)
        }

        ARMSX2Bridge.setINIBool("ARMSX2iOS/Migrations", key: "PhoneRumbleStrengthRescaleV1", value: true)
    }
}
