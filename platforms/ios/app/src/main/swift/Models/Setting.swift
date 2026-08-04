// Setting.swift — where one INI-backed setting lives and how it travels
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// Section, key, default and codec for one setting. The @Observable macro owns
/// the stored property; `didSet` consults this config.
///
/// `suppressible` does much less than it looks like. Swift skips property
/// observers inside a class's own initializer, so none of these didSets run
/// while `init()` loads the INI, and that window is the only time
/// `suppressINIWrites` is ever true. Measured on a launch: zero commits. What
/// the flag still catches is assignments made by helpers `init()` calls, which
/// are ordinary method calls and do fire their observers.
///
/// So the 88 settings marked `suppressible: false` do not write our defaults
/// into the INI at startup, whatever the folklore says. They write nothing at
/// startup. Left alone here because deciding what each one ought to be is its
/// own job, not part of moving the write path.
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

    /// What the INI currently holds, or our default if it holds nothing.
    @MainActor func load() -> Value { codec.read(section, key, defaultValue) }

    /// Same, for the odd setting whose fresh-install value depends on something
    /// only `init()` knows. Spelled out so the disagreement is greppable.
    @MainActor func load(default override: Value) -> Value {
        codec.read(section, key, override)
    }
}
