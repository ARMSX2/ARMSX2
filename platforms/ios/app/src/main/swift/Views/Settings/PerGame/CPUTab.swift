// CPUTab.swift — Per-game CPU & Speedhacks category tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct CPUTab: View {
    @Binding var enabled: Bool
    @Binding var eeCoreType: Int
    @Binding var mtvu: Bool
    @Binding var eeCycleRate: Int
    @Binding var globalEECycleRate: Int
    @Binding var eeCycleSkip: Int
    @Binding var globalEECycleSkip: Int
    @Binding var fastBoot: Int
    @Binding var globalFastBoot: Bool
    @Binding var perGameIOP: Int
    @Binding var perGameVU0: Int
    @Binding var perGameVU1: Int
    @Binding var perGameEEFpuRound: Int
    @Binding var perGameVU0Round: Int
    @Binding var perGameVU1Round: Int
    @Binding var perGameEEClamp: Int
    @Binding var perGameVUClamp: Int

    let savesToRunningGame: Bool
    let settings: SettingsStore
    let eeCycleRateUseGlobalSentinel: Int
    let fastBootUseGlobalSentinel: Int
    let fastBootOff: Int
    let fastBootOn: Int
    let globalEEFpuRound: Int
    let globalVU0Round: Int
    let globalVU1Round: Int
    let globalEEClamp: Int
    let globalVUClamp: Int

    var body: some View {
        PerGameTab(title: settings.localized("CPU & Speedhacks")) {
            Section(settings.localized("CPU")) {
                Picker(settings.localized("EE Core"), selection: $eeCoreType) {
                    Text(settings.localized("ARM64 JIT")).tag(2)
                    Text(settings.localized("Interpreter")).tag(1)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.ee-core",
                    label: settings.localized("EE Core"),
                    selection: $eeCoreType,
                    options: [
                        (id: 2, title: settings.localized("ARM64 JIT")),
                        (id: 1, title: settings.localized("Interpreter")),
                    ]
                )
                .disabled(!enabled)

                Text(settings.localized("Interpreter is slower, but can help isolate EE JIT crashes for specific games. Reset/relaunch after changing it."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("MTVU", isOn: $mtvu)
                    .controllerAccessibilityToggleTarget(
                        id: "per-game.cpu.mtvu",
                        label: "MTVU",
                        isOn: $mtvu
                    )
                    .disabled(!enabled)
                Text(settings.localized("MTVU can improve performance and may help some visual issues, but can cause compatibility problems. Reset/relaunch after changing it."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Picker(settings.localized("IOP Recompiler"), selection: $perGameIOP) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("ARM64 JIT")).tag(1)
                    Text(settings.localized("Interpreter")).tag(0)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.iop-recompiler",
                    label: settings.localized("IOP Recompiler"),
                    selection: $perGameIOP,
                    options: processorModeOptions(jitTitle: "ARM64 JIT")
                )
                .disabled(!enabled)
                Picker(settings.localized("VU0 Recompiler"), selection: $perGameVU0) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("JIT")).tag(1)
                    Text(settings.localized("Interpreter")).tag(0)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.vu0-recompiler",
                    label: settings.localized("VU0 Recompiler"),
                    selection: $perGameVU0,
                    options: processorModeOptions(jitTitle: "JIT")
                )
                .disabled(!enabled)
                Picker(settings.localized("VU1 Recompiler"), selection: $perGameVU1) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("JIT")).tag(1)
                    Text(settings.localized("Interpreter")).tag(0)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.vu1-recompiler",
                    label: settings.localized("VU1 Recompiler"),
                    selection: $perGameVU1,
                    options: processorModeOptions(jitTitle: "JIT")
                )
                .disabled(!enabled)

                Text(settings.localized("IOP, VU0, and VU1 handle PS2 sub-processors. JIT is much faster; Interpreter is a fallback for the rare game that breaks under JIT. Reset or relaunch the game after changing these."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Performance / Compatibility") {
                Picker("EE Cycle Rate", selection: $eeCycleRate) {
                    Text("Global Default (\(Self.formatEECycleRate(globalEECycleRate)))").tag(eeCycleRateUseGlobalSentinel)
                    ForEach(-3...3, id: \.self) { value in
                        Text(Self.formatEECycleRate(value)).tag(value)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.ee-cycle-rate",
                    label: settings.localized("EE Cycle Rate"),
                    selection: $eeCycleRate,
                    options: [(
                        id: eeCycleRateUseGlobalSentinel,
                        title: "Global Default (\(Self.formatEECycleRate(globalEECycleRate)))"
                    )] + Array(-3...3).map {
                        (id: $0, title: Self.formatEECycleRate($0))
                    }
                )
                .disabled(!enabled)

                Button("Reset EE Cycle Rate to Global") {
                    eeCycleRate = eeCycleRateUseGlobalSentinel
                }
                .controllerAccessibilityActionTarget(
                    id: "per-game.cpu.reset-ee-cycle-rate",
                    label: settings.localized("Reset EE Cycle Rate to Global")
                ) {
                    eeCycleRate = eeCycleRateUseGlobalSentinel
                }
                .disabled(!enabled || eeCycleRate == eeCycleRateUseGlobalSentinel)

                Text(settings.localized("Can improve performance in heavy games, but may cause timing or compatibility issues. " + (savesToRunningGame ? "Takes effect when you save." : "Takes effect on next boot.")))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker("EE Cycle Skip", selection: $eeCycleSkip) {
                    Text("Global Default (\(globalEECycleSkip))").tag(-1)
                    ForEach(0...3, id: \.self) { value in
                        Text("\(value)").tag(value)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.ee-cycle-skip",
                    label: settings.localized("EE Cycle Skip"),
                    selection: $eeCycleSkip,
                    options: [
                        (id: -1, title: "Global Default (\(globalEECycleSkip))"),
                        (id: 0, title: "0"),
                        (id: 1, title: "1"),
                        (id: 2, title: "2"),
                        (id: 3, title: "3"),
                    ]
                )
                .disabled(!enabled)

                Button("Reset EE Cycle Skip to Global") {
                    eeCycleSkip = -1
                }
                .controllerAccessibilityActionTarget(
                    id: "per-game.cpu.reset-ee-cycle-skip",
                    label: settings.localized("Reset EE Cycle Skip to Global")
                ) {
                    eeCycleSkip = -1
                }
                .disabled(!enabled || eeCycleSkip == -1)

                Text(settings.localized("Skips EE cycles to boost performance; higher values are more aggressive and can cause audio or timing issues. " + (savesToRunningGame ? "Takes effect when you save." : "Takes effect on next boot.")))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker("Fast Boot", selection: $fastBoot) {
                    Text(String(format: settings.localized("Global Default (%@)"), settings.localized(globalFastBoot ? "On" : "Off"))).tag(fastBootUseGlobalSentinel)
                    Text(settings.localized("On")).tag(fastBootOn)
                    Text(settings.localized("Off")).tag(fastBootOff)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "per-game.cpu.fast-boot",
                    label: settings.localized("Fast Boot"),
                    selection: $fastBoot,
                    options: [
                        (
                            id: fastBootUseGlobalSentinel,
                            title: "Global Default (\(globalFastBoot ? "On" : "Off"))"
                        ),
                        (id: fastBootOn, title: settings.localized("On")),
                        (id: fastBootOff, title: settings.localized("Off")),
                    ]
                )
                .disabled(!enabled)

                Button("Reset Fast Boot to Global") {
                    fastBoot = fastBootUseGlobalSentinel
                }
                .controllerAccessibilityActionTarget(
                    id: "per-game.cpu.reset-fast-boot",
                    label: settings.localized("Reset Fast Boot to Global")
                ) {
                    fastBoot = fastBootUseGlobalSentinel
                }
                .disabled(!enabled || fastBoot == fastBootUseGlobalSentinel)

                Text("Some games may need Fast Boot on or off to avoid looping at the disc screen. Reset or relaunch the game after changing it.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                modeOverridePicker("EE FPU Round Mode", id: "ee-fpu-round", selection: $perGameEEFpuRound,
                                   globalValue: globalEEFpuRound, labels: SettingsStore.roundModeLabels)
                modeOverridePicker("VU0 Round Mode", id: "vu0-round", selection: $perGameVU0Round,
                                   globalValue: globalVU0Round, labels: SettingsStore.roundModeLabels)
                modeOverridePicker("VU1 Round Mode", id: "vu1-round", selection: $perGameVU1Round,
                                   globalValue: globalVU1Round, labels: SettingsStore.roundModeLabels)
                modeOverridePicker("EE Clamp Mode", id: "ee-clamp", selection: $perGameEEClamp,
                                   globalValue: globalEEClamp, labels: SettingsStore.eeClampModeLabels)
                modeOverridePicker("VU Clamp Mode", id: "vu-clamp", selection: $perGameVUClamp,
                                   globalValue: globalVUClamp, labels: SettingsStore.vuClampModeLabels)

                Text("Rounding and clamping can improve compatibility for specific games, but may break others. Reset or relaunch the game after changing these.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Advanced CPU")
            }
        }
    }

    /// A per-game round/clamp picker whose "use global" option (-1) reads the inherited
    /// global level from `labels`, matching the "Global Default (…)" style used elsewhere.
    @ViewBuilder
    private func modeOverridePicker(_ title: String, id: String,
                                    selection: Binding<Int>,
                                    globalValue: Int, labels: [String]) -> some View {
        Picker(title, selection: selection) {
            Text("Global Default (\(labels[min(max(globalValue, 0), labels.count - 1)]))").tag(-1)
            ForEach(Array(labels.enumerated()), id: \.offset) { index, label in
                Text(label).tag(index)
            }
        }
        .controllerAccessibilityOptionsPickerTarget(
            id: "per-game.cpu.\(id)",
            label: settings.localized(title),
            selection: selection,
            options: [(
                id: -1,
                title: "Global Default (\(labels[min(max(globalValue, 0), labels.count - 1)]))"
            )] + Array(labels.enumerated()).map {
                (id: $0.offset, title: $0.element)
            }
        )
        .disabled(!enabled)
    }

    private func processorModeOptions(
        jitTitle: String
    ) -> [(id: Int, title: String)] {
        [
            (id: -1, title: settings.localized("Use Global")),
            (id: 1, title: settings.localized(jitTitle)),
            (id: 0, title: settings.localized("Interpreter")),
        ]
    }

    private static func clampedEECycleRate(_ value: Int) -> Int {
        min(max(value, -3), 3)
    }

    private static func formatEECycleRate(_ value: Int) -> String {
        let clamped = clampedEECycleRate(value)
        return clamped > 0 ? "+\(clamped)" : "\(clamped)"
    }
}
