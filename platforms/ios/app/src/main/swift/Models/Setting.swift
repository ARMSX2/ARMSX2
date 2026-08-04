// Setting.swift — the section, key and default behind one INI-backed setting
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// One INI-backed setting. The @Observable macro owns the stored property;
/// `didSet` hands the value to `commit`.
///
/// `suppressible` catches nothing today, and is still worth keeping. init()'s
/// own assignments do not run their observers, so a launch reaches `commit`
/// zero times either way. That is measured, with a control in the same run,
/// not read off the language reference. Leave the flag alone rather than tidy
/// it away: it is what would catch a setting assigned from a helper `init()`
/// calls, where the observers do fire, and it costs one `&&`.
///
/// Every `EmuCore/GS` setting nudges the running VM after it is written. That
/// used to be an opt-in closure, which is how the sprite hacks, the user hacks
/// and the OSD flags ended up writing the INI and never reaching the running
/// VM: a new setting inherited whatever the one above it happened to declare.
/// Boot-only keys opt out.
struct Setting<Value> {
    let section: String
    let key: String
    let defaultValue: Value
    let suppressible: Bool
    let appliesGraphics: Bool
    let codec: SettingCodec<Value>

    init(section: String,
         key: String,
         default defaultValue: Value,
         suppressible: Bool = true,
         bootOnly: Bool = false,
         codec: SettingCodec<Value>) {
        self.section = section
        self.key = key
        self.defaultValue = defaultValue
        self.suppressible = suppressible
        self.appliesGraphics = section == "EmuCore/GS" && !bootOnly
        self.codec = codec
    }

    /// What the INI currently holds, or our default if it holds nothing. The
    /// overload is for the odd setting whose fresh-install value depends on
    /// something only `init()` knows; spelled out so it stays greppable.
    @MainActor func load() -> Value { load(default: defaultValue) }
    @MainActor func load(default fallback: Value) -> Value {
        codec.read(section, key, fallback)
    }

    /// For where `init()` has to correct the INI rather than read it.
    @MainActor func write(_ value: Value) { codec.write(section, key, value) }
}
