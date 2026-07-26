// Setting.swift — configuration-only property wrapper for INI-backed settings
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// Configuration-only: holds the INI section/key/writer for a setting. The
/// @Observable macro owns the stored property; didSet consults this config.
///
/// `onSet` runs from each property's `didSet`. Swift suppresses property
/// observers during `init()`, so `onSet` is not reached while
/// `SettingsStore.init()` is running; the graphics-pipeline closures still go
/// through `requestGraphicsApplyGuarded()` (which no-ops while INI is loading)
/// as a guard, should that ever change.
///
/// Every `EmuCore/GS` setting gets that apply hook by default. Opting in per
/// setting is how the sprite hacks, the user hacks and the OSD flags ended up
/// writing the INI and never reaching the running VM: a new setting inherits
/// whatever the one above it happened to declare. Boot-only keys opt out.
struct Setting<Value> {
    let section: String
    let key: String
    let defaultValue: Value
    let suppressible: Bool
    let writer: (String, String, Value) -> Void
    let onSet: (@MainActor (Value) -> Void)?

    init(section: String,
         key: String,
         default defaultValue: Value,
         suppressible: Bool = true,
         bootOnly: Bool = false,
         writer: @escaping (String, String, Value) -> Void,
         onSet: (@MainActor (Value) -> Void)? = nil) {
        self.section = section
        self.key = key
        self.defaultValue = defaultValue
        self.suppressible = suppressible
        self.writer = writer
        if let onSet {
            self.onSet = onSet
        } else if section == "EmuCore/GS" && !bootOnly {
            // Resolved when the closure runs, never here -- touching
            // SettingsStore.shared during init re-enters its swift_once.
            self.onSet = { _ in SettingsStore.shared.requestGraphicsApplyGuarded() }
        } else {
            self.onSet = nil
        }
    }
}
