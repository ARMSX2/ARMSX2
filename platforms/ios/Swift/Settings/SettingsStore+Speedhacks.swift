// SettingsStore+Speedhacks.swift — speedhack/JIT boot-load helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    static func loadedFastBoot() -> Bool {
        let coreFastBoot = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableFastBoot", defaultValue: false)
        return ARMSX2Bridge.getINIBool("GameISO", key: "FastBoot", defaultValue: coreFastBoot)
    }

    static func loadedJITScriptProtocol() -> JITScriptProtocol {
        let protocolValue = JITScriptProtocol.normalized(
            ARMSX2Bridge.getINIString("ARMSX2iOS/JIT", key: "ScriptProtocol", defaultValue: JITScriptProtocol.defaultValue.rawValue)
        )
        let migrated = ARMSX2Bridge.getINIBool("ARMSX2iOS/Migrations", key: "JITScriptProtocolByOSV1", defaultValue: false)
        if !migrated && JITScriptProtocol.defaultValue == .legacy && protocolValue == .universal {
            ARMSX2Bridge.setINIString("ARMSX2iOS/JIT", key: "ScriptProtocol", value: JITScriptProtocol.legacy.rawValue)
            ARMSX2Bridge.setINIBool("ARMSX2iOS/Migrations", key: "JITScriptProtocolByOSV1", value: true)
            NSLog("[ARMSX2 iOS Settings] Migrated JIT script protocol to legacy for this iOS version")
            return .legacy
        }
        if !migrated {
            ARMSX2Bridge.setINIBool("ARMSX2iOS/Migrations", key: "JITScriptProtocolByOSV1", value: true)
        }
        return protocolValue
    }
}

extension SettingsStore {
    static let _eeCycleRateConfig = Setting<Int>(
        section: "EmuCore/Speedhacks", key: "EECycleRate", default: 0,
        codec: .int)
    static let _vu1InstantConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "vu1Instant", default: true,
        codec: .bool)
    static let _waitLoopConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "WaitLoop", default: true,
        codec: .bool)
    static let _intcStatConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "IntcStat", default: true,
        codec: .bool)
    static let _eeCycleSkipConfig = Setting<Int>(
        section: "EmuCore/Speedhacks", key: "EECycleSkip", default: 0,
        codec: .cycleSkip)
    static let _vuFlagHackConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "vuFlagHack", default: true,
        codec: .bool)
    static let _enableCheatsConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableCheats", default: false,
        suppressible: false,
        codec: .bool)
    static let _enablePatchesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnablePatches", default: true,
        suppressible: false,
        codec: .bool)
    static let _enableGameFixesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableGameFixes", default: true,
        suppressible: false,
        codec: .bool)
    static let _enableGameDBHardwareFixesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks", default: true,
        suppressible: false,
        codec: .inverted)
    static let _enableWidescreenPatchesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableWideScreenPatches", default: false,
        suppressible: false,
        codec: .bool)
    static let _enableNoInterlacingPatchesConfig = Setting<Bool>(
        section: "EmuCore", key: "EnableNoInterlacingPatches", default: false,
        suppressible: false,
        codec: .bool)
    static let _hostFilesystemConfig = Setting<Bool>(
        section: "EmuCore", key: "HostFs", default: false,
        suppressible: false,
        codec: .bool)
    // Automatic performance tuning: when a game keeps missing frames, the core tries performance
    // settings one at a time and keeps the ones that help, saved in that game's own settings.
    static let _autoTuneConfig = Setting<Bool>(
        section: "EmuCore", key: "AutoTune", default: false,
        suppressible: false,
        codec: .bool)
    static let _autoOpenStikDebugConfig = Setting<Bool>(
        section: "ARMSX2iOS/JIT", key: "AutoOpenStikDebug", default: false,
        codec: .bool)
    static let _jitScriptProtocolConfig = Setting<JITScriptProtocol>(
        section: "ARMSX2iOS/JIT", key: "ScriptProtocol",
        // Which one is right depends on the iOS version, so ask rather than assume.
        default: JITScriptProtocol.defaultValue,
        codec: .rawString)
}
