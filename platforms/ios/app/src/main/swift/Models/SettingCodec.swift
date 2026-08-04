// SettingCodec.swift — how one setting crosses the INI boundary
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// The INI read and write for one setting, so the two cannot drift apart.
struct SettingCodec<Value> {
    let read: @MainActor (_ section: String, _ key: String, _ fallback: Value) -> Value
    let write: @MainActor (_ section: String, _ key: String, _ value: Value) -> Void
}

@MainActor
extension SettingCodec where Value == Bool {
    static let bool = Self(read: ARMSX2Bridge.getINIBool, write: ARMSX2Bridge.setINIBool)
}

@MainActor
extension SettingCodec where Value == Float {
    static let float = Self(read: ARMSX2Bridge.getINIFloat, write: ARMSX2Bridge.setINIFloat)
}

@MainActor
extension SettingCodec where Value == String {
    static let string = Self(read: ARMSX2Bridge.getINIString, write: ARMSX2Bridge.setINIString)
}

@MainActor
extension SettingCodec where Value == Int {
    // The bridge speaks Int32 and we speak Int, so every one of these has a cast.
    static let int = Self(
        read: { s, k, d in Int(ARMSX2Bridge.getINIInt(s, key: k, defaultValue: Int32(d))) },
        write: { s, k, v in ARMSX2Bridge.setINIInt(s, key: k, value: Int32(v)) })

    /// Clamps on the way in and out.
    static func int(in range: ClosedRange<Int>) -> Self {
        Self(clampedBy: { SettingsStore.clamped($0, to: range) })
    }

    /// Two clamps that are not plain ranges.
    static let roundMode = Self(clampedBy: SettingsStore.clampedRoundMode)
    static let cycleSkip = Self(clampedBy: SettingsStore.clampedCycleSkip)

    private init(clampedBy clamp: @escaping @MainActor (Int) -> Int) {
        self.init(
            read: { s, k, d in clamp(Int(ARMSX2Bridge.getINIInt(s, key: k, defaultValue: Int32(d)))) },
            write: { s, k, v in ARMSX2Bridge.setINIInt(s, key: k, value: Int32(clamp(v))) })
    }
}

@MainActor
extension SettingCodec where Value: RawRepresentable, Value.RawValue == Int {
    // var, not let: the type is still generic here, so it gets no static storage.
    static var rawInt: Self {
        Self(read: { s, k, d in
                Value(rawValue: Int(ARMSX2Bridge.getINIInt(s, key: k, defaultValue: Int32(d.rawValue)))) ?? d },
             write: { s, k, v in ARMSX2Bridge.setINIInt(s, key: k, value: Int32(v.rawValue)) })
    }
}

@MainActor
extension SettingCodec where Value: RawRepresentable, Value.RawValue == String {
    static var rawString: Self {
        Self(read: { s, k, d in Value(rawValue: ARMSX2Bridge.getINIString(s, key: k, defaultValue: d.rawValue)) ?? d },
             write: { s, k, v in ARMSX2Bridge.setINIString(s, key: k, value: v.rawValue) })
    }
}

// MARK: - The ones that need their own spelling

@MainActor
extension SettingCodec where Value == Bool {
    /// The core stores the opposite of what the switch says.
    static let inverted = Self(
        read: { s, k, d in !ARMSX2Bridge.getINIBool(s, key: k, defaultValue: !d) },
        write: { s, k, v in ARMSX2Bridge.setINIBool(s, key: k, value: !v) })

    /// A switch on our side, a 1 or a 0 on the core's.
    static let boolAsInt = Self(
        read: { s, k, d in ARMSX2Bridge.getINIInt(s, key: k, defaultValue: d ? 1 : 0) != 0 },
        write: { s, k, v in ARMSX2Bridge.setINIInt(s, key: k, value: v ? 1 : 0) })

    /// SPU2 spells its two sync modes out instead of using a flag.
    static let timeStretch = Self(
        read: { s, k, d in
            ARMSX2Bridge.getINIString(s, key: k, defaultValue: d ? "TimeStretch" : "Disabled") != "Disabled" },
        write: { s, k, v in ARMSX2Bridge.setINIString(s, key: k, value: v ? "TimeStretch" : "Disabled") })
}

@MainActor
extension SettingCodec where Value == Int {
    /// Aspect ratio is a menu index to us and a name to the INI.
    static let aspectRatio = Self(
        read: { s, k, d in
            SettingsStore.aspectRatioValue(
                from: ARMSX2Bridge.getINIString(s, key: k, defaultValue: SettingsStore.aspectRatioName(for: d))) },
        write: { s, k, v in ARMSX2Bridge.setINIString(s, key: k, value: SettingsStore.aspectRatioName(for: v)) })
}
