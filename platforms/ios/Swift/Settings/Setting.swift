// Setting.swift — the section, key and default behind one INI-backed setting
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// One INI-backed setting. The @Observable macro owns the stored property, `didSet`
/// hands the value to `commit`, and every `EmuCore/GS` key nudges the running VM
/// after it is written unless it is boot only.
///
/// `suppressible` catches nothing today. Keep it: it would catch a setting assigned
/// from a helper `init()` calls, where the observers do fire, and it costs one `&&`.
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
