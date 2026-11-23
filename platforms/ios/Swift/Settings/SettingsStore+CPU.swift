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

    static func clampedEECycleRate(_ value: Int) -> Int {
        clamped(value, to: eeCycleRateRange)
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

extension SettingsStore {
    static let _iopRecompilerConfig = Setting<Bool>(
        section: "EmuCore/CPU/Recompiler", key: "EnableIOP", default: true,
        codec: .bool)
    static let _vu0RecompilerConfig = Setting<Bool>(
        section: "EmuCore/CPU/Recompiler", key: "EnableVU0", default: true,
        codec: .bool)
    static let _vu1RecompilerConfig = Setting<Bool>(
        section: "EmuCore/CPU/Recompiler", key: "EnableVU1", default: true,
        codec: .bool)
    static let _automaticLoadLastSaveStateConfig = Setting<Bool>(
        section: "ARMSX2iOS/Boot", key: "AutomaticLoadLastSaveState", default: false,
        codec: .bool)
    static let _autoSaveEnabledConfig = Setting<Bool>(
        section: "ARMSX2iOS/SaveStates", key: "AutoSave", default: true, codec: .bool)
    static let _autoSaveIntervalConfig = Setting<Int>(
        section: "ARMSX2iOS/SaveStates", key: "AutoSaveIntervalMinutes", default: 10,
        codec: .int(in: 5...30))
    static let _autoSaveOnLeaveConfig = Setting<Bool>(
        section: "ARMSX2iOS/SaveStates", key: "AutoSaveOnLeave", default: true, codec: .bool)
    static let _autoSaveOnLowBatteryConfig = Setting<Bool>(
        section: "ARMSX2iOS/SaveStates", key: "AutoSaveOnLowBattery", default: true, codec: .bool)
    static let _undoSecondsConfig = Setting<Int>(
        section: "ARMSX2iOS/SaveStates", key: "UndoSeconds", default: 5, codec: .int(in: 1...10))
    static let _automaticLoadLastGameConfig = Setting<Bool>(
        section: "ARMSX2iOS/Boot", key: "AutomaticLoadLastGame", default: false,
        codec: .bool)
    static let _bootISOConfig = Setting<String>(
        section: "GameISO", key: "BootISO", default: "",
        codec: .string)
    static let _lastGameConfig = Setting<String>(
        section: "ARMSX2iOS/Boot", key: "LastGame", default: "",
        codec: .string)
    static let _emulationOnlyModeConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyMode", default: false,
        codec: .bool)
    static let _emulationOnlyDisablePatchesConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisablePatches", default: true,
        codec: .bool)
    static let _emulationOnlyDisablePINEConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisablePINE", default: true,
        codec: .bool)
    static let _emulationOnlyDisableRetroAchievementsConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableRetroAchievements", default: true,
        codec: .bool)
    static let _emulationOnlyDisableInputRecordingConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableInputRecording", default: true,
        codec: .bool)
    static let _emulationOnlyDisableOSDConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableOSD", default: true,
        codec: .bool)
    static let _emulationOnlyDisableFramePacingConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableFramePacing", default: true,
        codec: .bool)
    static let _emulationOnlyDisableVirtualControlsConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableVirtualControls", default: true,
        codec: .bool)
    static let _emulationOnlyDisableQuickMenuConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyDisableQuickMenu", default: true,
        codec: .bool)
    static let _emulationOnlyClearNetworkCacheConfig = Setting<Bool>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyClearNetworkCache", default: true,
        codec: .bool)
    static let _emulationOnlyModeDelayConfig = Setting<Int>(
        section: "ARMSX2iOS/UI", key: "EmulationOnlyModeDelaySeconds",
        default: SettingsStore.defaultEmulationOnlyModeDelaySeconds,
        codec: .int(in: SettingsStore.emulationOnlyModeDelayRange))
    // Clamp modes are one 0-3 level here, unpacked to the three (EE) or six (VU0+VU1)
    // boolean keys the recompiler reads. Changes take effect on next boot.
    static let _eeFpuRoundModeConfig = Setting<Int>(
        section: "EmuCore/CPU", key: "FPU.Roundmode", default: 3,
        codec: .roundMode)
    static let _vu0RoundModeConfig = Setting<Int>(
        section: "EmuCore/CPU", key: "VU0.Roundmode", default: 3,
        codec: .roundMode)
    static let _vu1RoundModeConfig = Setting<Int>(
        section: "EmuCore/CPU", key: "VU1.Roundmode", default: 3,
        codec: .roundMode)
    static let _fastCDVDConfig = Setting<Bool>(
        section: "EmuCore/Speedhacks", key: "fastCDVD", default: false,
        codec: .bool)
}
