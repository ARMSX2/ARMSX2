// GamepadSettingsView.swift — Virtual pad + controller mapping
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct PS2Button: Identifiable {
    let id: Int
    let name: String
}

private let ps2Buttons: [PS2Button] = [
    PS2Button(id: 0,  name: "D-Pad Up"),
    PS2Button(id: 1,  name: "D-Pad Down"),
    PS2Button(id: 2,  name: "D-Pad Left"),
    PS2Button(id: 3,  name: "D-Pad Right"),
    PS2Button(id: 4,  name: "Cross"),
    PS2Button(id: 5,  name: "Circle"),
    PS2Button(id: 6,  name: "Square"),
    PS2Button(id: 7,  name: "Triangle"),
    PS2Button(id: 8,  name: "L1"),
    PS2Button(id: 9,  name: "R1"),
    PS2Button(id: 12, name: "Start"),
    PS2Button(id: 13, name: "Select"),
    PS2Button(id: 14, name: "L3"),
    PS2Button(id: 15, name: "R3"),
]

private let multitapModes: [(id: Int, title: String)] = [
    (0, "Auto (3+ Controllers)"),
    (1, "Disabled"),
    (2, "Port 1 Multitap"),
    (3, "Port 2 Multitap"),
    (4, "Port 1 + Port 2 Multitap"),
]

private func multitapModeTitle(_ id: Int) -> String {
    multitapModes.first(where: { $0.id == id })?.title ?? "Auto"
}

// SDL_GamepadButton → display name (matches SDL3 enum order)
private func sdlButtonName(_ idx: Int) -> String {
    switch idx {
    case 0:  return "A / Cross"
    case 1:  return "B / Circle"
    case 2:  return "X / Square"
    case 3:  return "Y / Triangle"
    case 4:  return "Share / Back"
    case 5:  return "Guide / PS"
    case 6:  return "Options / Start"
    case 7:  return "L-Stick Press"
    case 8:  return "R-Stick Press"
    case 9:  return "L-Shoulder"
    case 10: return "R-Shoulder"
    case 11: return "D-Pad Up"
    case 12: return "D-Pad Down"
    case 13: return "D-Pad Left"
    case 14: return "D-Pad Right"
    case 15: return "Misc / Share"
    case 20: return "Touchpad"
    default: return "Button \(idx)"
    }
}

struct GamepadSettingsView: View {
    static func controllerTargetOrder(
        leftInstantDeadzoneEnabled: Bool,
        rightInstantDeadzoneEnabled: Bool
    ) -> [String] {
        var order = [
                "settings.game-controller.controller-macros",
                "settings.game-controller.dead-zone",
                "settings.game-controller.left-instant-deadzone",
            ]
        if leftInstantDeadzoneEnabled {
            order.append("settings.game-controller.left-negative-deadzone")
        }
        order.append("settings.game-controller.right-instant-deadzone")
        if rightInstantDeadzoneEnabled {
            order.append("settings.game-controller.right-negative-deadzone")
        }
        order += [
                "settings.game-controller.game-rumble-strength",
                "settings.game-controller.ui-rumble-strength",
                "settings.game-controller.test-rumble",
                "settings.game-controller.reset",
                "settings.game-controller.local-multiplayer",
            ]
        order += ps2Buttons.map {
            "settings.game-controller.mapping.\($0.id)"
        }
        order += [
            "settings.game-controller.invert-left-horizontal",
            "settings.game-controller.invert-left-vertical",
            "settings.game-controller.invert-right-horizontal",
            "settings.game-controller.invert-right-vertical",
        ]
        return order
    }

    let onOpenPane: (SettingsPane) -> Void
    private let presentsLinkedPanesModally: Bool

    @State private var settings = SettingsStore.shared
    @State private var capturingIndex: Int? = nil
    @State private var mappingVersion = 0
    @State private var pollTimer: Timer? = nil
    @State private var statusMessage: String?
    @State private var presentedPane: SettingsPane?

    init(
        presentsLinkedPanesModally: Bool = false,
        onOpenPane: @escaping (SettingsPane) -> Void = { _ in }
    ) {
        self.presentsLinkedPanesModally = presentsLinkedPanesModally
        self.onOpenPane = onOpenPane
    }

    private var activeControllerTargetOrder: [String] {
        Self.controllerTargetOrder(
            leftInstantDeadzoneEnabled:
                settings.gameControllerLeftInstantDeadzoneEnabled,
            rightInstantDeadzoneEnabled:
                settings.gameControllerRightInstantDeadzoneEnabled
        )
    }

    var body: some View {
        Form {
            Section {
                linkedPaneRow(
                    .controllerMacros,
                    id: "settings.game-controller.controller-macros"
                ) {
                    Label(
                        settings.localized("Controller Macros"),
                        systemImage: "command"
                    )
                }
            } footer: {
                Text(settings.localized(
                    "Customize two-button shortcuts used while emulation is running."
                ))
            }

            Section {
                NumberRow(
                    .gameControllerDeadZone,
                    value: $settings.gameControllerDeadZone,
                    settings: settings
                )
                .controllerAccessibilityTargetID("settings.game-controller.dead-zone")

                gamepadToggle(
                    "Left Thumbstick Instant Deadzone",
                    id: "settings.game-controller.left-instant-deadzone",
                    isOn: $settings.gameControllerLeftInstantDeadzoneEnabled
                )
                if settings.gameControllerLeftInstantDeadzoneEnabled {
                    NumberRow(
                        .gameControllerLeftNegativeDeadzone,
                        value: $settings.gameControllerLeftNegativeDeadzone,
                        settings: settings
                    )
                    .controllerAccessibilityTargetID("settings.game-controller.left-negative-deadzone")
                }

                gamepadToggle(
                    "Right Thumbstick Instant Deadzone",
                    id: "settings.game-controller.right-instant-deadzone",
                    isOn: $settings.gameControllerRightInstantDeadzoneEnabled
                )
                if settings.gameControllerRightInstantDeadzoneEnabled {
                    NumberRow(
                        .gameControllerRightNegativeDeadzone,
                        value: $settings.gameControllerRightNegativeDeadzone,
                        settings: settings
                    )
                    .controllerAccessibilityTargetID("settings.game-controller.right-negative-deadzone")
                }
            } header: {
                Text(settings.localized("Instant Deadzone"))
            } footer: {
                Text(settings.localized("Dead Zone filters stick drift. Instant deadzones add the selected minimum output only after real controller movement."))
            }

            Section {
                NumberRow(
                    .gameRumbleStrength,
                    value: $settings.gameRumbleStrength,
                    settings: settings
                )
                .controllerAccessibilityTargetID(
                    "settings.game-controller.game-rumble-strength"
                )

                NumberRow(
                    .uiRumbleStrength,
                    value: uiRumbleStrengthDisplayBinding,
                    settings: settings
                )
                .controllerAccessibilityTargetID(
                    "settings.game-controller.ui-rumble-strength"
                )

                Button {
                    ARMSX2Bridge.testControllerRumble()
                    statusMessage = settings.localized("Controller rumble test sent.")
                } label: {
                    Label(settings.localized("Test Controller Rumble"), systemImage: "waveform.path")
                }
                .controllerAccessibilityActionTarget(
                    id: "settings.game-controller.test-rumble",
                    label: settings.localized("Test Controller Rumble")
                ) {
                    ARMSX2Bridge.testControllerRumble()
                    statusMessage = settings.localized("Controller rumble test sent.")
                }

                ConfirmedSettingsResetButton(
                    settings.localized("Reset to Default"),
                    confirmationTitle: settings.localized("Reset Controller Mapping?"),
                    confirmationMessage: settings.localized("This restores every controller button mapping to its default assignment."),
                    completionMessage: settings.localized("Defaults Restored"),
                    controllerTargetID: "settings.game-controller.reset"
                ) {
                    ARMSX2Bridge.resetButtonMappings()
                    mappingVersion += 1
                }
                .uiCriticalForegroundStyle()
            } header: {
                Text(settings.localized("Tools"))
            } footer: {
                if let statusMessage {
                    Text(statusMessage)
                }
            }

            Section {
                linkedPaneRow(
                    .localMultiplayer,
                    id: "settings.game-controller.local-multiplayer"
                ) {
                    Label {
                        HStack {
                            Text(settings.localized("Local Multiplayer"))
                            Spacer()
                            Text(settings.localized(multitapModeTitle(settings.controllerMultitapMode)))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    } icon: {
                        Image(systemName: "person.3")
                    }
                }
            } footer: {
                Text(settings.localized("Configure multitap for 3-4 local controllers."))
            }

            Section {
                ForEach(ps2Buttons) { btn in
                    mappingRow(btn)
                }
            } header: {
                Text(settings.localized("Button Mapping"))
            } footer: {
                Text(settings.localized("Tap a row, then press a button on your controller to assign it. L2/R2 are analog triggers (not remappable)."))
            }

            Section {
                gamepadToggle(
                    "Invert Left Horizontal",
                    id: "settings.game-controller.invert-left-horizontal",
                    isOn: $settings.invertLeftStickX
                )
                gamepadToggle(
                    "Invert Left Vertical",
                    id: "settings.game-controller.invert-left-vertical",
                    isOn: $settings.invertLeftStickY
                )
                gamepadToggle(
                    "Invert Right Horizontal",
                    id: "settings.game-controller.invert-right-horizontal",
                    isOn: $settings.invertRightStickX
                )
                gamepadToggle(
                    "Invert Right Vertical (Camera)",
                    id: "settings.game-controller.invert-right-vertical",
                    isOn: $settings.invertRightStickY
                )
            } header: {
                Text(settings.localized("Stick Inversion"))
            } footer: {
                Text(settings.localized("Flips connected-controller stick axes. The same global inversion choices are shared with Virtual Pad."))
            }
        }
        .controllerAccessibilityTargetOrder(activeControllerTargetOrder)
        .navigationTitle(settings.localized("Game Controller"))
        .navigationBarTitleDisplayMode(.inline)
        .onDisappear {
            stopCapture()
        }
        .sheet(item: $presentedPane) { pane in
            GamepadLinkedSettingsSheet(pane: pane)
        }
    }

    /// The persisted value predates the 0...200% presentation and stores the
    /// actual gain divided by two. Keep that representation so existing 50%
    /// installs remain full-strength, while the UI correctly calls it 100%.
    private var uiRumbleStrengthDisplayBinding: Binding<Float> {
        Binding(
            get: { settings.uiRumbleStrength * 2 },
            set: { settings.uiRumbleStrength = min(max($0 / 2, 0), 1) }
        )
    }

    @ViewBuilder
    private func linkedPaneRow<Label: View>(
        _ pane: SettingsPane,
        id: String,
        @ViewBuilder label: () -> Label
    ) -> some View {
        if presentsLinkedPanesModally {
            Button {
                presentedPane = pane
            } label: {
                label()
            }
            .buttonStyle(.plain)
            .controllerAccessibilityActionTarget(
                id: id,
                label: settings.localized(pane.title)
            ) {
                presentedPane = pane
            }
        } else {
            NavigationLink(value: pane) {
                label()
            }
            .controllerAccessibilityActionTarget(
                id: id,
                label: settings.localized(pane.title)
            ) {
                onOpenPane(pane)
            }
        }
    }

    @ViewBuilder
    private func gamepadToggle(
        _ title: String,
        id: String,
        isOn: Binding<Bool>
    ) -> some View {
        let localizedTitle = settings.localized(title)
        Toggle(localizedTitle, isOn: isOn)
            .controllerAccessibilityToggleTarget(
                id: id,
                label: localizedTitle,
                isOn: isOn
            )
    }

    @ViewBuilder
    private func mappingRow(_ btn: PS2Button) -> some View {
        let isCapturing = capturingIndex == btn.id
        let currentSDL = Int(ARMSX2Bridge.getButtonMapping(Int32(btn.id)))

        Button {
            toggleCapture(for: btn.id)
        } label: {
            HStack {
                // Left: assigned controller button (prominent)
                if isCapturing {
                    Text(settings.localized("Press a button..."))
                        .font(.body)
                        .fontWeight(.medium)
                        .foregroundStyle(.orange)
                } else {
                    Text(settings.localized(sdlButtonName(currentSDL)))
                        .font(.body)
                        .controllerFocusedTextColor()
                }
                Spacer()
                // Right: PS2 function name (secondary)
                Text(settings.localized(btn.name))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .id(mappingVersion)
        }
        .controllerAccessibilityActionTarget(
            id: "settings.game-controller.mapping.\(btn.id)",
            label: settings.localized(btn.name)
        ) {
            toggleCapture(for: btn.id)
        }
        .listRowBackground(isCapturing ? Color.orange.opacity(0.15) : nil)
    }

    private func toggleCapture(for ps2Index: Int) {
        if capturingIndex == ps2Index {
            stopCapture()
        } else {
            startCapture(for: ps2Index)
        }
    }

    private func startCapture(for ps2Index: Int) {
        capturingIndex = ps2Index
        ARMSX2Bridge.startButtonCapture()
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
            ARMSX2Bridge.pollGamepadForCapture()
            let captured = ARMSX2Bridge.capturedButton()
            if captured >= 0 {
                ARMSX2Bridge.setButtonMapping(Int32(ps2Index), toSDLButton: captured)
                Task { @MainActor in
                    stopCapture()
                    mappingVersion += 1
                }
            }
        }
    }

    private func stopCapture() {
        pollTimer?.invalidate()
        pollTimer = nil
        capturingIndex = nil
        ARMSX2Bridge.stopButtonCapture()
    }
}

private struct GamepadLinkedSettingsSheet: View {
    private static let doneControllerTargetID =
        "per-game.game-controller.linked.done"

    let pane: SettingsPane

    @Environment(\.dismiss) private var dismiss
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var settings = SettingsStore.shared

    private var controllerTargetOrder: [String] {
        let contentOrder: [String]
        switch pane {
        case .localMultiplayer:
            contentOrder = LocalMultiplayerSettingsView.controllerTargetOrder
        case .controllerMacros:
            contentOrder = ControllerMacrosSettingsView.controllerTargetOrder
        default:
            contentOrder = []
        }
        return contentOrder + [Self.doneControllerTargetID]
    }

    var body: some View {
        NavigationStack {
            Group {
                switch pane {
                case .localMultiplayer:
                    LocalMultiplayerSettingsView()
                case .controllerMacros:
                    ControllerMacrosSettingsView()
                default:
                    EmptyView()
                }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        close()
                    }
                    .controllerAccessibilityActionTarget(
                        id: Self.doneControllerTargetID,
                        label: settings.localized("Done"),
                        activationFeedback: .silent,
                        action: close
                    )
                }
            }
        }
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 320,
                isEnabled: true,
                searchesNearbyScrollViews: true
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
        }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "per-game.game-controller.linked.\(pane.rawValue)",
            priority: 320,
            orbStyle: .plain,
            onBack: {
                close()
                return true
            },
            usesNativeFocusEngine: false,
            usesExplicitTargetGeometryOnly: true,
            preservesFocusDuringRightStickScrolling: false,
            focusScrollBehavior: .maintainWithinViewport,
            focusTopAlignmentMargin: 84,
            focusBottomAlignmentMargin: 84,
            preferredInitialFocusLabel: controllerTargetOrder.first
        )
    }

    private func close() {
        MenuAudioPackManager.shared.playEvent(.return)
        dismiss()
    }
}

struct ControllerMacrosSettingsView: View {
    static var controllerTargetOrder: [String] {
        ControllerMacroAction.allCases.flatMap { action in
            [
                "controller.macros.\(action.rawValue).first",
                "controller.macros.\(action.rawValue).second",
            ]
        } + ["controller.macros.reset"]
    }

    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            ForEach(ControllerMacroAction.allCases) { action in
                Section {
                    Picker(
                        settings.localized("First Button"),
                        selection: buttonBinding(for: action, first: true)
                    ) {
                        ForEach(ControllerMacroButton.allCases) { button in
                            Text(settings.localized(button.title)).tag(button)
                        }
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "controller.macros.\(action.rawValue).first",
                        label: settings.localized("\(action.title) First Button"),
                        selection: buttonBinding(for: action, first: true),
                        options: ControllerMacroButton.allCases.map {
                            (id: $0, title: settings.localized($0.title))
                        }
                    )

                    Picker(
                        settings.localized("Second Button"),
                        selection: buttonBinding(for: action, first: false)
                    ) {
                        ForEach(ControllerMacroButton.allCases) { button in
                            Text(settings.localized(button.title)).tag(button)
                        }
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "controller.macros.\(action.rawValue).second",
                        label: settings.localized("\(action.title) Second Button"),
                        selection: buttonBinding(for: action, first: false),
                        options: ControllerMacroButton.allCases.map {
                            (id: $0, title: settings.localized($0.title))
                        }
                    )

                    HStack {
                        Text(settings.localized("Current Shortcut"))
                        Spacer()
                        Text(settings.controllerMacroBinding(for: action).title)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text(settings.localized(action.title))
                } footer: {
                    Text(settings.localized(action.detail))
                }
            }

            Section {
                ConfirmedSettingsResetButton(
                    settings.localized("Reset Controller Macros"),
                    confirmationTitle: settings.localized("Reset Controller Macros?"),
                    confirmationMessage: settings.localized("This restores all controller shortcuts to their default button combinations."),
                    completionMessage: settings.localized("Defaults Restored"),
                    controllerTargetID: "controller.macros.reset"
                ) {
                    settings.resetControllerMacros()
                }
                .uiCriticalForegroundStyle()
            }
        }
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        .navigationTitle(settings.localized("Controller Macros"))
        .navigationBarTitleDisplayMode(.inline)
    }

    private func buttonBinding(
        for action: ControllerMacroAction,
        first: Bool
    ) -> Binding<ControllerMacroButton> {
        Binding(
            get: {
                let binding = settings.controllerMacroBinding(for: action)
                return first ? binding.first : binding.second
            },
            set: { selected in
                let current = settings.controllerMacroBinding(for: action)
                let updated: ControllerMacroBinding
                if first {
                    updated = ControllerMacroBinding(
                        first: selected,
                        second: selected == current.second
                            ? current.first
                            : current.second
                    )
                } else {
                    updated = ControllerMacroBinding(
                        first: selected == current.first
                            ? current.second
                            : current.first,
                        second: selected
                    )
                }
                settings.setControllerMacroBinding(updated, for: action)
            }
        )
    }
}

struct LocalMultiplayerSettingsView: View {
    static let controllerTargetOrder = [
        "settings.local-multiplayer.multitap-mode",
    ]

    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                Picker(settings.localized("Multitap Mode"), selection: $settings.controllerMultitapMode) {
                    ForEach(multitapModes, id: \.id) { mode in
                        Text(settings.localized(mode.title)).tag(mode.id)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "settings.local-multiplayer.multitap-mode",
                    label: settings.localized("Multitap Mode"),
                    selection: $settings.controllerMultitapMode,
                    options: multitapModes.map {
                        (id: $0.id, title: settings.localized($0.title))
                    }
                )

                HStack {
                    Text(settings.localized("Current Mode"))
                    Spacer()
                    Text(settings.localized(multitapModeTitle(settings.controllerMultitapMode)))
                        .foregroundStyle(.secondary)
                }
            } header: {
                Text(settings.localized("Multitap"))
            } footer: {
                Text(settings.localized("Auto enables Port 1 multitap when 3 or more controllers are detected before boot. Manual modes take effect on the next boot/reset."))
            }

            Section(settings.localized("Controller Mapping")) {
                Text(settings.localized("Disabled maps controllers 1-2 to normal PS2 ports. Port 1 Multitap maps controllers 1-4 to 1A/1B/1C/1D. Port 2 Multitap keeps controller 1 on Port 1 and maps controllers 2-4 to Port 2 multitap slots."))
                    .foregroundStyle(.secondary)
            }
        }
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        .navigationTitle(settings.localized("Local Multiplayer"))
        .navigationBarTitleDisplayMode(.inline)
    }
}
