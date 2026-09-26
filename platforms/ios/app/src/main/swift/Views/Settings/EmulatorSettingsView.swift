// EmulatorSettingsView.swift — EE/IOP/VU/boot/speedhack settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct EmulatorSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var stikDebugOpenFailed = false
    @State private var stikDebugOpenInProgress = false
    // Cached rather than asked per redraw: the lookup takes the achievements lock, and
    // this sits in a Form that rebuilds on every other row.
    @State private var hardcoreBlocksCheats = false

    /// Hardcore only clears EnableCheats in the running config, so the INI this row reads
    /// still says on and the row lies. The write is dropped further down as well, without
    /// a word, so turning it on looks like it worked until something reloads.
    private var cheatsBinding: Binding<Bool> {
        Binding(
            get: { hardcoreBlocksCheats ? false : settings.enableCheats },
            set: { newValue in
                guard !hardcoreBlocksCheats else { return }
                settings.enableCheats = newValue
            }
        )
    }

    var body: some View {
        Form {
            Section {
                Toggle(isOn: Binding(
                    get: { settings.eeCoreType != 1 },
                    set: { settings.eeCoreType = $0 ? 2 : 1 }
                )) {
                    HStack {
                        Text(settings.localized("EE Core"))
                        Spacer()
                        Text(settings.localized(settings.eeCoreType != 1 ? "ARM64 JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                .controllerAccessibilityTargetID("settings.emulator.ee-core")
                Toggle(isOn: $settings.iopRecompiler) {
                    HStack {
                        Text("IOP")
                        Spacer()
                        Text(settings.localized(settings.iopRecompiler ? "JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                .controllerAccessibilityTargetID("settings.emulator.iop-core")
                Toggle(isOn: $settings.vu0Recompiler) {
                    HStack {
                        Text("VU0")
                        Spacer()
                        Text(settings.localized(settings.vu0Recompiler ? "JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                .controllerAccessibilityTargetID("settings.emulator.vu0-core")
                Toggle(isOn: $settings.vu1Recompiler) {
                    HStack {
                        Text("VU1")
                        Spacer()
                        Text(settings.localized(settings.vu1Recompiler ? "JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                .controllerAccessibilityTargetID("settings.emulator.vu1-core")
                Text(settings.localized("Changes take effect on next VM boot."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("CPU Recompiler"))
            }

            Section {
                modePicker("EE FPU Round Mode", id: "ee-fpu-round", selection: $settings.eeFpuRoundMode, labels: SettingsStore.roundModeLabels)
                modePicker("VU0 Round Mode", id: "vu0-round", selection: $settings.vu0RoundMode, labels: SettingsStore.roundModeLabels)
                modePicker("VU1 Round Mode", id: "vu1-round", selection: $settings.vu1RoundMode, labels: SettingsStore.roundModeLabels)
                modePicker("EE Clamp Mode", id: "ee-clamp", selection: $settings.eeClampMode, labels: SettingsStore.eeClampModeLabels)
                modePicker("VU Clamp Mode", id: "vu-clamp", selection: $settings.vuClampMode, labels: SettingsStore.vuClampModeLabels)
            } header: {
                Text(settings.localized("Advanced CPU"))
            } footer: {
                Text(settings.localized("Rounding and clamping can improve compatibility for specific games, but may break others. Changes take effect on the next game boot."))
            }

            Section(settings.localized("StikDebug")) {
                Toggle(settings.localized("Auto-open StikDebug/StosDebug"), isOn: $settings.autoOpenStikDebug)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.auto-open-stik-debug"
                    )

                Picker(settings.localized("JIT Script"), selection: $settings.jitScriptProtocol) {
                    ForEach(JITScriptProtocol.allCases) { scriptProtocol in
                        Text(settings.localized(scriptProtocol.label)).tag(scriptProtocol)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "settings.emulator.jit-script",
                    label: settings.localized("JIT Script"),
                    selection: $settings.jitScriptProtocol,
                    options: JITScriptProtocol.allCases.map {
                        (id: $0, title: settings.localized($0.label))
                    }
                )
                .pickerStyle(.segmented)

                Text(settings.localized(settings.jitScriptProtocol.subtitle))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Button {
                    stikDebugOpenInProgress = true
                    stikDebugOpenFailed = false
                    StikDebugLauncher.open(reason: "emulator-settings") { success in
                        stikDebugOpenInProgress = false
                        stikDebugOpenFailed = !success
                    }
                } label: {
                    Label(settings.localized("Open StikDebug/StosDebug"), systemImage: "bolt.horizontal.circle")
                }
                .disabled(stikDebugOpenInProgress)
                .controllerAccessibilityTargetID(
                    "settings.emulator.open-stik-debug"
                )

                Text(settings.localized("Select the same script here that you run in StikDebug/StosDebug. This only changes the debugger breakpoint protocol used to prepare JIT memory. Fully close and relaunch ARMSX2 after switching scripts."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                if stikDebugOpenFailed {
                    Text(settings.localized("Open StikDebug/StosDebug manually, then run the selected script and relaunch ARMSX2."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }

            Section(settings.localized("Boot")) {
                Toggle(settings.localized("Fast Boot"), isOn: $settings.fastBoot)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.fast-boot"
                    )
                Text(settings.localized("Skips BIOS intro. Some games require this OFF."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Toggle(settings.localized("Automatic Load Last Saved State"), isOn: $settings.automaticLoadLastSaveState)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.automatic-load-last-save-state"
                    )
                Toggle(settings.localized("Automatic Load Last Game"), isOn: $settings.automaticLoadLastGame)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.automatic-load-last-game"
                    )
                Text(settings.localized("Load the game's newest saved state before gameplay starts, when available. State loading is skipped in RetroAchievements Hardcore Mode. Automatic Load Last Game skips the app intro and menu, then starts the last successfully launched game after the JIT check."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Host Filesystem")) {
                Toggle(settings.localized("Enable Host Filesystem"), isOn: $settings.hostFilesystem)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.host-filesystem"
                    )
                Text(settings.localized("Allows PS2 homebrew and ELF tools to access files through the host: device. This is separate from USB/SSD game storage, is off by default, and takes effect on next VM boot."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Memory")) {
                Toggle(settings.localized("Fastmem"), isOn: $settings.fastmem)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.fastmem"
                    )
                Text(settings.localized("Direct memory mapping for EE. Disable if 3D graphics are broken. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Performance")) {
                Toggle(settings.localized("Frame Limiter"), isOn: $settings.frameLimiterEnabled)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.frame-limiter"
                    )

                if settings.frameLimiterEnabled {
                    NumberRow(.targetFPS, value: $settings.targetFPS, settings: settings)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.target-fps"
                        )
                } else {
                    HStack {
                        Text(settings.localized("Speed Target"))
                        Spacer()
                        Text(settings.localized("Unlocked"))
                            .foregroundStyle(.orange)
                            .font(.callout.monospacedDigit())
                    }
                }

                HStack {
                    Text(settings.localized("NTSC Base Rate"))
                    Spacer()
                    Text(Self.formatFPS(settings.ntscFramerate))
                        .foregroundStyle(.secondary)
                        .font(.callout.monospacedDigit())
                }

                HStack {
                    Text(settings.localized("PAL Base Rate"))
                    Spacer()
                    Text(Self.formatFPS(settings.palFramerate))
                        .foregroundStyle(.secondary)
                        .font(.callout.monospacedDigit())
                }

                Text(settings.localized("The FPS Target changes display presentation without slowing CPU, audio, or game timing. Fast Forward remains a separate emulation-speed control."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Advanced Emulation")) {
                Toggle(settings.localized("Emulation-Only Mode"), isOn: $settings.emulationOnlyModeEnabled)
                    .controllerAccessibilityTargetID(
                        "settings.emulator.emulation-only-mode"
                    )
                Text(settings.localized("Automatically unloads the selected menus, controls, and optional services for the current emulation session."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Group {
                    NumberRow(.emulationOnlyModeTimer,
                              value: $settings.emulationOnlyModeDelaySeconds, settings: settings)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-delay"
                        )

                    Toggle(
                        settings.localized("Disable Cheats, Widescreen and Dynamic Patches"),
                        isOn: $settings.emulationOnlyDisablePatches
                    )
                    .controllerAccessibilityTargetID(
                        "settings.emulator.emulation-only-disable-patches"
                    )
                    Toggle(settings.localized("Disable PINE Server"), isOn: $settings.emulationOnlyDisablePINE)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-pine"
                        )
                    Toggle(settings.localized("Disable RetroAchievements"), isOn: $settings.emulationOnlyDisableRetroAchievements)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-retro-achievements"
                        )
                    Toggle(settings.localized("Disable PCSX2 Input Recording"), isOn: $settings.emulationOnlyDisableInputRecording)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-input-recording"
                        )
                    Toggle(settings.localized("Disable OSD and Performance Overlays"), isOn: $settings.emulationOnlyDisableOSD)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-osd"
                        )
                    Toggle(settings.localized("Disable Frame Pacing"), isOn: $settings.emulationOnlyDisableFramePacing)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-frame-pacing"
                        )
                    Toggle(settings.localized("Disable Virtual Control Layout"), isOn: $settings.emulationOnlyDisableVirtualControls)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-virtual-controls"
                        )
                    Toggle(settings.localized("Disable Quick Menu"), isOn: $settings.emulationOnlyDisableQuickMenu)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-disable-quick-menu"
                        )
                    Toggle(settings.localized("Clear Network Cache"), isOn: $settings.emulationOnlyClearNetworkCache)
                        .controllerAccessibilityTargetID(
                            "settings.emulator.emulation-only-clear-network-cache"
                        )
                }
                .disabled(!settings.emulationOnlyModeEnabled)

                Text(settings.localized("The timer starts after boot patches and replacement-texture startup complete. Discord Presence is always disabled. All visible cleanup switches default ON, preserving the existing maximum-performance behavior. Disable Frame Pacing stops the optional adaptive frame-time monitor; the core limiter and audio/video timing remain active. Turn a switch off to retain that resource. Without an external controller, the current Virtual Control Layout is retained automatically. Turn Disable Quick Menu off to keep the complete Quick Menu available."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .disabled(!settings.emulationOnlyModeEnabled)
            }

            Section {
                Button(settings.localized("Use VU1 Interpreter Preset")) {
                    settings.applyVU1CompatibilityPreset()
                }
                .controllerAccessibilityTargetID(
                    "settings.emulator.vu1-interpreter-preset"
                )
                Button(settings.localized("Use Full Interpreter Preset")) {
                    settings.applyFullInterpreterPreset()
                }
                .controllerAccessibilityTargetID(
                    "settings.emulator.full-interpreter-preset"
                )
                Text(settings.localized("Use the VU1 preset first for boot crashes or VU1-related texture/rendering glitches. Full Interpreter is much slower, but helps isolate dynarec/JIT issues."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Compatibility"))
            } footer: {
                Text(settings.localized("Changes take effect on next VM boot."))
            }

            Section {
                Toggle(settings.localized("GameDB Automatic Fixes"), isOn: Binding(
                    get: { settings.enableGameFixes && settings.enableGameDBHardwareFixes },
                    set: { enabled in
                        settings.enableGameFixes = enabled
                        settings.enableGameDBHardwareFixes = enabled
                    }
                ))
                .controllerAccessibilityTargetID("settings.emulator.gamedb-automatic-fixes")
                Toggle(settings.localized("GameDB Core Fixes"), isOn: $settings.enableGameFixes)
                    .controllerAccessibilityTargetID("settings.emulator.gamedb-core-fixes")
                Toggle(settings.localized("GameDB Graphics Fixes"), isOn: $settings.enableGameDBHardwareFixes)
                    .controllerAccessibilityTargetID("settings.emulator.gamedb-graphics-fixes")
                Toggle(settings.localized("GameDB PNACH Patches"), isOn: $settings.enablePatches)
                    .controllerAccessibilityTargetID("settings.emulator.gamedb-pnach-patches")
                Toggle(settings.localized("Enable PNACH Cheats"), isOn: cheatsBinding)
                    .disabled(hardcoreBlocksCheats)
                    .controllerAccessibilityTargetID("settings.emulator.enable-pnach-cheats")
                if hardcoreBlocksCheats {
                    Text(settings.localized("Hardcore Mode is turning cheats off. Switch it off in RetroAchievements to use them again."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
                Toggle(settings.localized("Widescreen Patches"), isOn: $settings.enableWidescreenPatches)
                    .controllerAccessibilityTargetID("settings.emulator.widescreen-patches")
                Toggle(settings.localized("No-Interlacing Patches"), isOn: $settings.enableNoInterlacingPatches)
                    .controllerAccessibilityTargetID("settings.emulator.no-interlacing-patches")

                Text(settings.localized("GameDB Core Fixes covers timing, clamps, and gamefixes. GameDB Graphics Fixes covers renderer-specific hardware fixes; turn it off globally or per-game if a title looks worse on Metal. Use Cheats & Patches from the in-game quick menu or a game's long-press menu to import and manage patch files."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Patches & Cheats"))
            } footer: {
                Text(settings.localized("Changes take effect on next VM boot."))
            }

            Section {
                ForEach(SettingsStore.gameFixOptions) { option in
                    Toggle(settings.localized(option.label), isOn: Binding(
                        get: { settings.gameFixEnabled(option.key) },
                        set: { settings.setGameFix(option.key, $0) }
                    ))
                    .controllerAccessibilityTargetID(
                        "settings.emulator.game-fix.\(option.key)"
                    )
                    if option.key == "SkipMPEGHack" {
                        Text(settings.localized("Skip MPEG is a last-resort FMV hack that can break interactive cutscenes. Best set per-game."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .disabled(!settings.enableGameFixes)
            } header: {
                Text(settings.localized("Game Fixes"))
            } footer: {
                Text(settings.localized("Manual game fixes override normal compatibility behavior. Use them only for games that need them. They apply while GameDB Core Fixes is on. Changes take effect on the next game boot."))
            }

            Section {
                Picker(settings.localized("EE Cycle Rate"), selection: $settings.eeCycleRate) {
                    ForEach(-3...3, id: \.self) { value in
                        Text(value > 0 ? "+\(value)" : "\(value)").tag(value)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "settings.emulator.ee-cycle-rate",
                    label: settings.localized("EE Cycle Rate"),
                    selection: $settings.eeCycleRate,
                    options: Array(-3...3).map { value in
                        (id: value, title: value > 0 ? "+\(value)" : "\(value)")
                    }
                )
                Text(settings.localized("0 = Default. Negative = underclock (stable). Positive = overclock (fast but risky)."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Fast CDVD"), isOn: $settings.fastCDVD)
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.emulator.fast-cdvd",
                        label: settings.localized("Fast CDVD"),
                        isOn: $settings.fastCDVD
                    )
                Toggle(settings.localized("VU1 Instant"), isOn: $settings.vu1Instant)
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.emulator.vu1-instant",
                        label: settings.localized("VU1 Instant"),
                        isOn: $settings.vu1Instant
                    )
                Toggle("MTVU", isOn: $settings.mtvu)
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.emulator.mtvu",
                        label: "MTVU",
                        isOn: $settings.mtvu
                    )
                Toggle(settings.localized("Wait Loop Detection"), isOn: $settings.waitLoop)
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.emulator.wait-loop-detection",
                        label: settings.localized("Wait Loop Detection"),
                        isOn: $settings.waitLoop
                    )
                Toggle(settings.localized("INTC Stat Hack"), isOn: $settings.intcStat)
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.emulator.intc-stat-hack",
                        label: settings.localized("INTC Stat Hack"),
                        isOn: $settings.intcStat
                    )
                Picker(settings.localized("EE Cycle Skip"), selection: $settings.eeCycleSkip) {
                    ForEach(0...3, id: \.self) { value in
                        Text("\(value)").tag(value)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "settings.emulator.ee-cycle-skip",
                    label: settings.localized("EE Cycle Skip"),
                    selection: $settings.eeCycleSkip,
                    options: Array(0...3).map { value in
                        (id: value, title: "\(value)")
                    }
                )
                Toggle(settings.localized("VU Flag Hack"), isOn: $settings.vuFlagHack)
                    // This row already has a stable explicit controller probe.
                    // Override the screen-wide registering style locally so a
                    // recycled Form cell cannot expose a second toggle target.
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.emulator.vu-flag-hack",
                        label: settings.localized("VU Flag Hack"),
                        isOn: $settings.vuFlagHack
                    )

                Text(settings.localized("VU1 Instant and MTVU are independent now. MTVU can help some games, but keep it off unless a game specifically benefits on iOS."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Text(settings.localized("Fast CDVD speeds up disc reads and can fix games that stall loading; it rarely causes issues. EE Cycle Skip and VU Flag Hack trade accuracy for speed and can break timing-sensitive games. Wait Loop Detection and INTC Stat Hack are safe, small idle-time savings. Enable speedhacks one at a time per game and relaunch to check."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Speedhacks"))
            } footer: {
                Text(settings.localized("Changes take effect on next VM boot."))
            }

            Section {
                ConfirmedSettingsResetButton(
                    settings.localized("Reset Emulator to Defaults"),
                    confirmationTitle: settings.localized("Reset Emulator Settings?"),
                    confirmationMessage: settings.localized("This restores the global CPU, boot, speed, and compatibility settings to their original values."),
                    completionMessage: settings.localized("Defaults Restored"),
                    controllerTargetID: "settings.emulator.reset-defaults"
                ) {
                    settings.resetEmulatorDefaults()
                }
                .buttonStyle(.automatic)
                .uiCriticalForegroundStyle()
            }
        }
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .navigationTitle(settings.localized("Emulator"))
        .navigationBarTitleDisplayMode(.inline)
        .dynamicTypeSize(...DynamicTypeSize.accessibility3)
        .onAppear {
            hardcoreBlocksCheats = PatchStore.hardcoreBlocksPnachContent()
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
            hardcoreBlocksCheats = PatchStore.hardcoreBlocksPnachContent()
        }
    }

    private static func formatFPS(_ value: Float) -> String {
        String(format: "%.2f FPS", value)
    }


    /// Compact labeled picker over a fixed ordered option list (round/clamp modes).
    @ViewBuilder
    private func modePicker(
        _ title: String,
        id: String,
        selection: Binding<Int>,
        labels: [String]
    ) -> some View {
        Picker(settings.localized(title), selection: selection) {
            ForEach(Array(labels.enumerated()), id: \.offset) { index, label in
                Text(settings.localized(label)).tag(index)
            }
        }
        .controllerAccessibilityOptionsPickerTarget(
            id: "settings.emulator.\(id)",
            label: settings.localized(title),
            selection: selection,
            options: Array(labels.enumerated()).map { index, label in
                (id: index, title: settings.localized(label))
            }
        )
    }

    /// Source order is the navigation graph. It survives SwiftUI Form cell
    /// recycling and lets the session jump directly to an unmounted row.
    private var controllerTargetOrder: [String] {
        var order = [
            "settings.emulator.ee-core",
            "settings.emulator.iop-core",
            "settings.emulator.vu0-core",
            "settings.emulator.vu1-core",
            "settings.emulator.ee-fpu-round",
            "settings.emulator.vu0-round",
            "settings.emulator.vu1-round",
            "settings.emulator.ee-clamp",
            "settings.emulator.vu-clamp",
            "settings.emulator.auto-open-stik-debug",
            "settings.emulator.jit-script",
            "settings.emulator.open-stik-debug",
            "settings.emulator.fast-boot",
            "settings.emulator.automatic-load-last-save-state",
            "settings.emulator.automatic-load-last-game",
            "settings.emulator.host-filesystem",
            "settings.emulator.fastmem",
            "settings.emulator.frame-limiter",
        ]

        if settings.frameLimiterEnabled {
            order.append("settings.emulator.target-fps")
        }

        order.append("settings.emulator.emulation-only-mode")
        if settings.emulationOnlyModeEnabled {
            order += [
                "settings.emulator.emulation-only-delay",
                "settings.emulator.emulation-only-disable-patches",
                "settings.emulator.emulation-only-disable-pine",
                "settings.emulator.emulation-only-disable-retro-achievements",
                "settings.emulator.emulation-only-disable-input-recording",
                "settings.emulator.emulation-only-disable-osd",
                "settings.emulator.emulation-only-disable-frame-pacing",
                "settings.emulator.emulation-only-disable-virtual-controls",
                "settings.emulator.emulation-only-disable-quick-menu",
                "settings.emulator.emulation-only-clear-network-cache",
            ]
        }

        order += [
            "settings.emulator.vu1-interpreter-preset",
            "settings.emulator.full-interpreter-preset",
            "settings.emulator.gamedb-automatic-fixes",
            "settings.emulator.gamedb-core-fixes",
            "settings.emulator.gamedb-graphics-fixes",
            "settings.emulator.gamedb-pnach-patches",
        ]
        if !hardcoreBlocksCheats {
            order.append("settings.emulator.enable-pnach-cheats")
        }
        order += [
            "settings.emulator.widescreen-patches",
            "settings.emulator.no-interlacing-patches",
        ]
        if settings.enableGameFixes {
            order += SettingsStore.gameFixOptions.map {
                "settings.emulator.game-fix.\($0.key)"
            }
        }
        order += [
            "settings.emulator.ee-cycle-rate",
            "settings.emulator.fast-cdvd",
            "settings.emulator.vu1-instant",
            "settings.emulator.mtvu",
            "settings.emulator.wait-loop-detection",
            "settings.emulator.intc-stat-hack",
            "settings.emulator.ee-cycle-skip",
            "settings.emulator.vu-flag-hack",
            "settings.emulator.reset-defaults",
        ]
        return order
    }

}
