// VirtualPadSettingsView.swift — Virtual pad opacity, haptic, layout editing
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import UniformTypeIdentifiers

private enum DynamicActionRole {
    case aim
    case fire
    case holdFire
}

private struct SkinReplacePrompt: Identifiable {
    let id = UUID()
    let name: String
    let existingSkinID: String
}

private enum SkinReplaceChoice {
    case replace(String)
    case keepBoth
    case cancel
}

/// Holds the answer the import is waiting on. The view owns one, so leaving the
/// screen with the dialog still up unblocks the import on the way out instead of
/// parking it on a continuation nobody is left to resume.
@MainActor
private final class SkinReplaceGate {
    private var continuation: CheckedContinuation<SkinReplaceChoice, Never>?

    func wait() async -> SkinReplaceChoice {
        await withCheckedContinuation { continuation in
            self.continuation = continuation
        }
    }

    /// Answers at most once, whichever of the buttons or the dismissal gets here first.
    func resume(_ choice: SkinReplaceChoice) {
        guard let continuation else { return }
        self.continuation = nil
        continuation.resume(returning: choice)
    }

    deinit {
        continuation?.resume(returning: .cancel)
    }
}

struct VirtualPadSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = DynamicThumbstickSettings.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var showLayoutEditor = false
    @State private var showSkinImporter = false
    @State private var showSkinImportAlert = false
    @State private var lastSkinImportResult: VPadSkinImportResult?
    @State private var skinImportMessage = ""
    @State private var showLayoutImporter = false
    @State private var showLayoutImportAlert = false
    @State private var layoutImportMessage = ""
    @State private var layoutExportItem: ShareSheetItem?
    @State private var layoutPendingRename: PadLayoutPreset?
    @State private var layoutRenameDraft = ""
    @State private var layoutPendingDelete: PadLayoutPreset?
    @State private var skinPendingDelete: VPadSkinDescriptor?
    @State private var skinPendingRename: VPadSkinDescriptor?
    @State private var skinRenameDraft = ""
    @State private var skinReplacePrompt: SkinReplacePrompt?
    @State private var skinReplaceGate = SkinReplaceGate()
    @State private var automaticFireBlockedByHardcore = false

    var body: some View {
        Form {
            Section(settings.localized("Appearance")) {
                Picker(settings.localized("Button Skin"), selection: Binding<String>(
                    get: { skinLibrary.selectedSkinID },
                    set: { selectSkin(id: $0) }
                )) {
                    ForEach(skinLibrary.allDescriptors) { skin in
                        Text(settings.localized(skin.displayName)).tag(skin.id)
                    }
                }

                Text(settings.localized(selectedSkinDetail))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.padOpacity, value: $settings.padOpacity, settings: settings)
            }

            Section(settings.localized("Gameplay")) {
                Toggle(settings.localized("Hide Virtual Pad When Controller Is Connected"), isOn: $settings.autoHideVirtualPadWhenControllerConnected)
                Text(settings.localized("Automatically hides the on-screen controls while an external controller is connected."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Auto Full Screen"), isOn: $settings.autoFullscreen)
                Toggle(settings.localized("Hide Menu Button"), isOn: $settings.hideMenuButton)

                Toggle(settings.localized("D-pad Diagonals"), isOn: $settings.dpadDiagonalsEnabled)
                Text(settings.localized("Allows one-finger diagonal and quarter-circle motions on the virtual D-pad."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Face Button Combo Zones"), isOn: $settings.faceComboZonesEnabled)
                Text(settings.localized("Press between face buttons to trigger both buttons at once."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.analogStickSize, value: $settings.analogStickScale, settings: settings)

                Text(settings.localized("Double-tap empty gameplay space to show the menu button again."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Toggle(settings.localized("Invert Left Horizontal"), isOn: $settings.invertLeftStickX)
                Toggle(settings.localized("Invert Left Vertical"), isOn: $settings.invertLeftStickY)
                Toggle(settings.localized("Invert Right Horizontal"), isOn: $settings.invertRightStickX)
                Toggle(settings.localized("Invert Right Vertical (Camera)"), isOn: $settings.invertRightStickY)
            } header: {
                Text(settings.localized("Stick Inversion"))
            } footer: {
                Text(settings.localized("Flips the on-screen stick axes. Useful for games with fixed inverted camera or flight controls. Per-game overrides are available in the game’s Virtual Pad settings."))
            }

            Section(settings.localized("Custom Skin")) {
                Button {
                    showSkinImporter = true
                } label: {
                    Label(settings.localized("Import Skin"), systemImage: "paintpalette")
                }

                NavigationLink {
                    SkinBrowserView()
                } label: {
                    Label("Browse Skins", systemImage: "square.grid.2x2")
                }

                Text("Import loose PNG/JPG/WebP button images, a full portrait/landscape controller image, or a zipped skin pack. Button files can be named cross, circle, square, triangle, up, down, left, right, L1, R1, L2, R2, start, select, analog_base, or analog_stick.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                if !skinLibrary.importedDescriptors.isEmpty {
                    ForEach(skinLibrary.importedDescriptors) { skin in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(skin.displayName)
                                if skin.linkedLayoutPresetID != nil {
                                    Text("Includes recommended layout")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                            }
                            Spacer()
                            Menu {
                                Button {
                                    selectSkin(id: skin.id)
                                } label: {
                                    Label("Set as Global Default", systemImage: "checkmark.circle")
                                }
                                Button {
                                    skinPendingRename = skin
                                    skinRenameDraft = skin.displayName
                                } label: {
                                    Label("Rename Skin", systemImage: "pencil")
                                }
                                Button(role: .destructive) {
                                    skinPendingDelete = skin
                                } label: {
                                    Label("Delete Skin", systemImage: "trash")
                                }
                            } label: {
                                Image(systemName: "ellipsis.circle")
                            }
                        }
                    }
                }
            }

            Section(settings.localized("Feedback")) {
                Toggle(settings.localized("Haptic Feedback"), isOn: $settings.hapticFeedback)

                NumberRow(.phoneRumbleStrength, value: $settings.phoneRumbleStrength,
                          settings: settings)
                Text(settings.localized("Controls the iPhone Taptic Engine when no controller is connected. 25% preserves the original Core Haptics intensity; 100% applies up to 3x gain, and 0% disables phone rumble. Has no effect on a controller's own motors."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(
                    settings.localized("Increase Duration of Rumble and Interpolation"),
                    isOn: $settings.increaseRumbleDurationAndInterpolation)
            }

            Section(settings.localized("Layout")) {
                Picker("Default VPad Layout", selection: Binding<String?>(
                    get: { layoutPresets.globalPresetID },
                    set: { layoutPresets.globalPresetID = $0 }
                )) {
                    Text("Current Layout").tag(nil as String?)
                    ForEach(layoutPresets.presets) { preset in
                        Text(preset.displayName).tag(Optional(preset.id))
                    }
                }

                Button {
                    showLayoutEditor = true
                } label: {
                    Label(settings.localized("Edit Layout"), systemImage: "square.resize")
                }
                Text(settings.localized("Drag buttons to reposition. Pinch to resize."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text("Simple custom pad skins are shown behind the blue hit boxes in Edit Layout. Advanced skin packages can include their own PS2 control layout metadata. Non-PS2 Delta/Manic skins are not converted automatically.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Button {
                    showLayoutImporter = true
                } label: {
                    Label("Import Layout", systemImage: "square.and.arrow.down")
                }

                if !layoutPresets.presets.isEmpty {
                    ForEach(layoutPresets.presets) { preset in
                        HStack {
                            Text(preset.displayName)
                                .lineLimit(1)
                            Spacer()
                            Menu {
                                Button {
                                    layoutPendingRename = preset
                                    layoutRenameDraft = preset.displayName
                                } label: {
                                    Label(
                                        settings.localized("Rename Layout"),
                                        systemImage: "pencil"
                                    )
                                }
                                Button {
                                    exportLayout(preset)
                                } label: {
                                    Label(
                                        settings.localized("Share Layout"),
                                        systemImage: "square.and.arrow.up"
                                    )
                                }
                                Button(role: .destructive) {
                                    layoutPendingDelete = preset
                                } label: {
                                    Label(
                                        settings.localized("Delete Layout"),
                                        systemImage: "trash"
                                    )
                                }
                            } label: {
                                Image(systemName: "ellipsis.circle")
                            }
                            .accessibilityLabel(
                                settings.localized("Layout Options") + " \(preset.displayName)"
                            )
                        }
                    }
                }
            }

            Section {
                ForEach(BuiltInDynamicControlPreset.allCases) { preset in
                    Button {
                        preset.apply(settings: dynamicSettings)
                    } label: {
                        HStack(spacing: 12) {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(settings.localized(preset.rawValue))
                                    .foregroundStyle(.primary)
                                Text(settings.localized(preset.summary))
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .multilineTextAlignment(.leading)
                                    .lineLimit(nil)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .layoutPriority(1)
                            Spacer()
                            if preset.isActive(settings: dynamicSettings) {
                                Image(systemName: "checkmark.circle.fill")
                                    .foregroundStyle(.green)
                                    .fixedSize()
                            }
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            } header: {
                Text(settings.localized("Dynamic Control Presets"))
            } footer: {
                Text(settings.localized("Each preset applies its listed Dynamic Control configuration. Unrelated Virtual Pad appearance and controller-layout settings are preserved."))
                    .lineLimit(nil)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section {
                Toggle(
                    settings.localized("Legacy Thumbsticks"),
                    isOn: Binding(
                        get: { dynamicSettings.legacyThumbsticks },
                        set: { dynamicSettings.setLegacyThumbsticks($0) }
                    )
                )
                Toggle(
                    settings.localized("Dynamic Thumbsticks"),
                    isOn: Binding(
                        get: { dynamicSettings.dynamicThumbsticks },
                        set: { dynamicSettings.setDynamicThumbsticks($0) }
                    )
                )
                Toggle(settings.localized("Swipe Camera"), isOn: $dynamicSettings.swipeCamera)
                Toggle(settings.localized("Gyroscope Camera"), isOn: $dynamicSettings.gyroscopeCamera)
            } header: {
                Text(settings.localized("Dynamic Controls"))
            } footer: {
                Text(settings.localized("Legacy and Dynamic Thumbsticks are mutually exclusive. Dynamic sticks appear where each touch begins. Swipe Camera replaces the right touch stick, while Gyroscope Camera augments the active right-side control."))
            }

            Section {
                Toggle(
                    settings.localized("Left Thumbstick Instant Deadzone"),
                    isOn: $dynamicSettings.leftInstantDeadzoneEnabled
                )
                if dynamicSettings.leftInstantDeadzoneEnabled {
                    DynamicControlSlider(
                        title: "Left Negative Deadzone",
                        value: $dynamicSettings.leftNegativeDeadzone,
                        range: -0.25...0,
                        step: 0.01,
                        format: .unitPercent
                    )
                }

                Toggle(
                    settings.localized("Right Thumbstick Instant Deadzone"),
                    isOn: $dynamicSettings.rightInstantDeadzoneEnabled
                )
                if dynamicSettings.rightInstantDeadzoneEnabled {
                    DynamicControlSlider(
                        title: "Right Negative Deadzone",
                        value: $dynamicSettings.rightNegativeDeadzone,
                        range: -0.25...0,
                        step: 0.01,
                        format: .unitPercent
                    )
                }

                DynamicControlSlider(
                    title: "Left Thumbstick Movement Area",
                    value: $dynamicSettings.leftThumbstickAreaScale,
                    range: 1...5,
                    step: 0.25,
                    format: .multiplier
                )
                DynamicControlSlider(
                    title: "Right Thumbstick Movement Area",
                    value: $dynamicSettings.rightThumbstickAreaScale,
                    range: 1...5,
                    step: 0.25,
                    format: .multiplier
                )

                Toggle(
                    settings.localized("Convert Swipe to Dynamic Joystick"),
                    isOn: $dynamicSettings.convertSwipeToDynamicJoystick
                )
                if dynamicSettings.convertSwipeToDynamicJoystick {
                    DynamicControlSlider(
                        title: "Convert Into Dynamic Thumbstick",
                        value: Binding(
                            get: { dynamicSettings.convertIntoDynamicThumbstickThreshold },
                            set: { dynamicSettings.setConvertIntoDynamicThumbstickThreshold($0) }
                        ),
                        range: 0.01...3,
                        step: 0.01,
                        format: .unitPercent
                    )
                    DynamicControlSlider(
                        title: "Pulling Back Distance",
                        value: Binding(
                            get: { dynamicSettings.pullingBackDistance },
                            set: { dynamicSettings.setPullingBackDistance($0) }
                        ),
                        range: 0.01...1,
                        step: 0.01,
                        format: .unitPercent
                    )
                }
            } header: {
                Text(settings.localized("Instant Movement & Aiming"))
            } footer: {
                Text(settings.localized("Instant deadzones add the selected minimum output only after real movement, while preserving the progressive Dynamic Thumbstick deadzone. Movement areas default to 3× without enlarging the visible controls. Swipe conversion enters Dynamic Thumbstick mode at the selected outward distance. Pulling back by the selected distance returns to Swipe Camera and temporarily rebases the next outward conversion point until the screen is released."))
            }

            controlSensitivitySection

            if dynamicSettings.dynamicThumbsticks {
                thumbstickActionButtonsSection(
                    title: settings.localized("Action Buttons Left Thumbstick"),
                    toggleTitle: settings.localized("Dynamic Actions in Left Thumbstick"),
                    isEnabled: $dynamicSettings.leftThumbstickActionsEnabled,
                    aim: $dynamicSettings.leftAimButton,
                    fire: $dynamicSettings.leftFireButton,
                    holdFire: $dynamicSettings.leftHoldFireButton
                )
            }

            if dynamicSettings.swipeCamera || dynamicSettings.dynamicThumbsticks {
                thumbstickActionButtonsSection(
                    title: settings.localized("Action Buttons Right Thumbstick"),
                    toggleTitle: settings.localized("Dynamic Actions in Right Thumbstick"),
                    isEnabled: $dynamicSettings.rightThumbstickActionsEnabled,
                    aim: $dynamicSettings.rightAimButton,
                    fire: $dynamicSettings.rightFireButton,
                    holdFire: $dynamicSettings.rightHoldFireButton
                )
            }

            Section {
                Toggle(
                    settings.localized("Dynamic Aiming Crosshair"),
                    isOn: $dynamicSettings.dynamicCrosshairEnabled
                )
                if dynamicSettings.dynamicCrosshairEnabled {
                    Toggle(
                        settings.localized("Show Crosshair While Holding Swipe"),
                        isOn: $dynamicSettings.showCrosshairWhileHoldingSwipe
                    )
                    .disabled(!dynamicSettings.swipeCamera)

                    if dynamicSettings.showCrosshairWhileHoldingSwipe {
                        DynamicControlSlider(
                            title: "Crosshair Hide Delay",
                            value: $dynamicSettings.swipeCrosshairHideDelay,
                            range: 0...3,
                            step: 0.1,
                            format: .seconds
                        )
                        .disabled(!dynamicSettings.swipeCamera)
                    }

                    DynamicControlSlider(
                        title: "Crosshair Size",
                        value: $dynamicSettings.dynamicCrosshairSize,
                        range: 12...120,
                        step: 1,
                        format: .points
                    )
                    DynamicControlSlider(
                        title: "Crosshair Opacity",
                        value: $dynamicSettings.dynamicCrosshairOpacity,
                        range: 0.10...1,
                        step: 0.05,
                        format: .unitPercent
                    )
                    Picker(
                        settings.localized("Crosshair Type"),
                        selection: $dynamicSettings.dynamicCrosshairType
                    ) {
                        ForEach(DynamicCrosshairType.allCases) { type in
                            Text(settings.localized(type.title)).tag(type)
                        }
                    }
                    Picker(
                        settings.localized("Crosshair Animation"),
                        selection: $dynamicSettings.dynamicCrosshairAnimation
                    ) {
                        ForEach(DynamicCrosshairAnimation.allCases) { animation in
                            Text(settings.localized(animation.title)).tag(animation)
                        }
                    }
                }
            } header: {
                Text(settings.localized("Dynamic Crosshair"))
            } footer: {
                Text(settings.localized("The crosshair appears while Aim Mode is active and can remain visible while holding Swipe Camera. The hide delay controls how long it stays after the swipe ends. Every animation follows live swipe, thumbstick, and gyroscope direction and speed, then reacts separately to single shots and automatic fire."))
            }

            if dynamicSettings.dynamicThumbsticks {
                Section {
                    DynamicControlSlider(
                        title: "Maximum Radius",
                        value: $dynamicSettings.thumbstickRadius,
                        range: 40...60,
                        step: 1,
                        format: .points
                    )
                    DynamicControlSlider(
                        title: "Dead Zone",
                        value: $dynamicSettings.deadZone,
                        range: 0...0.25,
                        step: 0.01,
                        format: .unitPercent
                    )
                    DynamicControlSlider(
                        title: "Thumbstick Opacity",
                        value: $dynamicSettings.thumbstickOpacity,
                        range: 0...1,
                        step: 0.01,
                        format: .unitPercent
                    )
                    DynamicControlSlider(
                        title: "Base Opacity",
                        value: $dynamicSettings.baseOpacity,
                        range: 0...1,
                        step: 0.01,
                        format: .unitPercent
                    )
                    DynamicControlSlider(
                        title: "Trail Opacity",
                        value: $dynamicSettings.trailOpacity,
                        range: 0...1,
                        step: 0.01,
                        format: .unitPercent
                    )
                    Toggle(settings.localized("Activation Haptics"), isOn: $dynamicSettings.activationHaptics)
                } header: {
                    Text(settings.localized("Dynamic Thumbstick Feel"))
                } footer: {
                    Text(settings.localized("The compact base stays at the initial touch point. Dead zone begins at 0% and progressively reaches the selected value as the stick moves outward. Analog output saturates at the selected radius while the nub and seven-dot trail continue following overdrag."))
                }
            }

            if dynamicSettings.gyroscopeCamera {
                Section {
                    DynamicControlSlider(
                        title: "Gyro Sensitivity",
                        value: $dynamicSettings.gyroSensitivity,
                        range: 0.5...4.0,
                        step: 0.1,
                        format: .multiplier.decimals(1)
                    )
                    DynamicControlSlider(
                        title: "Gyro Acceleration",
                        value: $dynamicSettings.gyroAcceleration,
                        range: 0...2,
                        step: 0.05,
                        format: .unitPercent
                    )
                    DynamicControlSlider(
                        title: "Gyro Smoothing",
                        value: $dynamicSettings.gyroSmoothing,
                        range: 0...0.95,
                        step: 0.05,
                        format: .unitPercent
                    )
                    DynamicControlSlider(
                        title: "Gyro Dead Zone",
                        value: $dynamicSettings.gyroDeadZone,
                        range: 0...0.25,
                        step: 0.01,
                        format: .radiansPerSecond
                    )
                    DynamicControlSlider(
                        title: "Maximum Gyro Rate",
                        value: $dynamicSettings.gyroMaximumRate,
                        range: 1...12,
                        step: 0.5,
                        format: .radiansPerSecond.decimals(1)
                    )
                    Toggle(settings.localized("Invert Gyro Horizontal"), isOn: $dynamicSettings.invertGyroHorizontal)
                    Toggle(settings.localized("Invert Gyro Vertical"), isOn: $dynamicSettings.invertGyroVertical)
                } header: {
                    Text(settings.localized("Gyroscope"))
                } footer: {
                    Text(settings.localized("Gyroscope input is active only while the virtual controller is on screen. If the sensor is unavailable, the selected touch camera continues working normally."))
                }
            }

            if dynamicSettings.swipeCamera ||
                (dynamicSettings.dynamicThumbsticks &&
                    (dynamicSettings.leftThumbstickActionsEnabled || dynamicSettings.rightThumbstickActionsEnabled)) {
                Section {
                    if dynamicSettings.swipeCamera {
                        Picker(
                            settings.localized("Trigger Button When Un-holding Swipe"),
                            selection: $dynamicSettings.triggerButtonWhenUnholdingSwipe
                        ) {
                            Text(settings.localized("Off")).tag(-1)
                            ForEach(VirtualPadActionButton.allCases) { button in
                                Text(settings.localized(button.title)).tag(button.rawValue)
                            }
                        }
                    }

                    Toggle(
                        dynamicActionTitle("Hold Aim While Touching Camera", role: .aim),
                        isOn: Binding(
                            get: { dynamicSettings.holdAimWhileSwipe },
                            set: { dynamicSettings.setHoldAimWhileSwipe($0) }
                        )
                    )
                    Toggle(
                        dynamicActionTitle("Double Tap to Hold Aim", role: .aim),
                        isOn: Binding(
                            get: { dynamicSettings.doubleTapToHoldAim },
                            set: { dynamicSettings.setDoubleTapToHoldAim($0) }
                        )
                    )
                    Toggle(
                        settings.localized("Enable Single-Tap Action on Non-Aim Mode"),
                        isOn: Binding(
                            get: { dynamicSettings.singleTapActionAllowedInNonAimMode },
                            set: { dynamicSettings.singleTapActionOnNonAimMode = $0 }
                        )
                    )
                    .disabled(
                        !dynamicSettings.doubleTapToHoldAim ||
                            dynamicSettings.actionsOnNonAimMode
                    )
                    Toggle(
                        settings.localized("Enable Actions on Non-Aim Mode"),
                        isOn: Binding(
                            get: { dynamicSettings.actionsOnNonAimMode },
                            set: { dynamicSettings.setActionsOnNonAimMode($0) }
                        )
                    )
                    DynamicControlSlider(
                        title: dynamicActionTitle("Aim Release Delay", role: .aim),
                        value: $dynamicSettings.aimReleaseDelay,
                        range: 0...2,
                        step: 0.05,
                        format: .seconds
                    )
                    .disabled(!dynamicSettings.holdAimWhileSwipe && !dynamicSettings.doubleTapToHoldAim)
                    DynamicControlSlider(
                        title: dynamicActionTitle("Double-Tap Window", role: .aim),
                        value: $dynamicSettings.doubleTapWindow,
                        range: 0.15...0.60,
                        step: 0.01,
                        format: .seconds
                    )
                    .disabled(!dynamicSettings.doubleTapToHoldAim)
                    Toggle(
                        dynamicActionTitle("Tap to Fire Single Shots", role: .fire),
                        isOn: $dynamicSettings.tapToFire
                    )
                    DynamicControlSlider(
                        title: dynamicActionTitle("Single-Shot Tap Duration", role: .fire),
                        value: $dynamicSettings.tapMaximumDuration,
                        range: 0.10...0.60,
                        step: 0.01,
                        format: .seconds
                    )
                    DynamicControlSlider(
                        title: dynamicActionTitle("Single-Shot Travel Tolerance", role: .fire),
                        value: $dynamicSettings.tapTravelTolerance,
                        range: 4...30,
                        step: 1,
                        format: .points
                    )
                    Toggle(
                        dynamicActionTitle("Multiple Taps Enable Automatic Fire", role: .holdFire),
                        isOn: Binding(
                            get: {
                                dynamicSettings.rapidTapFireEnabled &&
                                    !automaticFireBlockedByHardcore
                            },
                            set: { enabled in
                                guard !automaticFireBlockedByHardcore else { return }
                                dynamicSettings.rapidTapFireEnabled = enabled
                            }
                        )
                    )
                    .disabled(automaticFireBlockedByHardcore)
                    DynamicControlSlider(
                        title: dynamicActionTitle("Multiple-Tap Window", role: .holdFire),
                        value: $dynamicSettings.rapidTapWindow,
                        range: 0.10...0.80,
                        step: 0.01,
                        format: .seconds
                    )
                    .disabled(automaticFireControlsDisabled)
                    DynamicControlSlider(
                        title: dynamicActionTitle("Taps to Activate", role: .holdFire),
                        value: Binding(
                            get: { Double(dynamicSettings.rapidTapActivationCount) },
                            set: { dynamicSettings.rapidTapActivationCount = Int($0.rounded()) }
                        ),
                        range: 2...5,
                        step: 1,
                        format: .taps
                    )
                    .disabled(automaticFireControlsDisabled)
                    DynamicControlSlider(
                        title: dynamicActionTitle("Automatic Fire Interval", role: .holdFire),
                        value: $dynamicSettings.automaticFireInterval,
                        range: 0.06...0.50,
                        step: 0.01,
                        format: .seconds
                    )
                    .disabled(automaticFireControlsDisabled)
                    Toggle(
                        dynamicActionTitle("Extend Automatic Fire While Dragging", role: .holdFire),
                        isOn: $dynamicSettings.extendFireWhileDragging
                    )
                        .disabled(automaticFireControlsDisabled)
                    Toggle(
                        dynamicActionTitle("Release Fire When Touch Ends", role: .holdFire),
                        isOn: $dynamicSettings.releaseFireWhenTouchEnds
                    )
                        .disabled(automaticFireControlsDisabled)
                    DynamicControlSlider(
                        title: dynamicActionTitle("Fire Release Delay", role: .holdFire),
                        value: $dynamicSettings.fireReleaseDelay,
                        range: 0...1,
                        step: 0.05,
                        format: .seconds
                    )
                    .disabled(
                        automaticFireControlsDisabled ||
                            dynamicSettings.releaseFireWhenTouchEnds
                    )
                } header: {
                    Text(settings.localized("Dynamic Actions"))
                } footer: {
                    if automaticFireBlockedByHardcore {
                        Text(settings.localized("Automatic Fire is disabled while RetroAchievements Hardcore Mode is on."))
                    }
                }
            }

            Section {
                Button(settings.localized("Restore Dynamic Control Defaults"), role: .destructive) {
                    dynamicSettings.restoreDefaults()
                }
            }
        }
        .navigationTitle(settings.localized("Virtual Pad"))
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: refreshHardcoreAutomaticFireRestriction)
        .onReceive(
            NotificationCenter.default.publisher(
                for: Notification.Name("ARMSX2RetroAchievementsStateChanged")
            )
        ) { _ in
            refreshHardcoreAutomaticFireRestriction()
        }
        .sheet(isPresented: $showLayoutImporter) {
            ImportDocumentPicker(
                allowedContentTypes: [.json, .data],
                allowsMultipleSelection: true
            ) { result in
                switch result {
                case .success(let urls):
                    layoutImportMessage = importLayouts(urls)
                case .failure(let error):
                    layoutImportMessage = "Layout import failed: \(error.localizedDescription)"
                }
                showLayoutImportAlert = true
            }
        }
        .sheet(item: $layoutExportItem) { item in
            ActivityShareSheet(activityItems: [item.url])
        }
        .alert("Layout Import", isPresented: $showLayoutImportAlert) {
            Button(settings.localized("OK"), role: .cancel) {}
        } message: {
            Text(layoutImportMessage)
        }
        .alert(
            settings.localized("Rename Layout"),
            isPresented: Binding<Bool>(
                get: { layoutPendingRename != nil },
                set: { if !$0 { layoutPendingRename = nil } }
            )
        ) {
            TextField(settings.localized("Name"), text: $layoutRenameDraft)
            Button(settings.localized("Rename")) {
                if let preset = layoutPendingRename {
                    do {
                        try layoutPresets.renamePreset(
                            id: preset.id,
                            to: layoutRenameDraft
                        )
                    } catch {
                        layoutImportMessage =
                            "Layout rename failed: \(error.localizedDescription)"
                        showLayoutImportAlert = true
                    }
                }
                layoutPendingRename = nil
            }
            Button(settings.localized("Cancel"), role: .cancel) {
                layoutPendingRename = nil
            }
        } message: {
            Text(settings.localized("Choose a new name for this layout."))
        }
        .confirmationDialog(
            settings.localized("Delete Layout?"),
            isPresented: Binding<Bool>(
                get: { layoutPendingDelete != nil },
                set: { if !$0 { layoutPendingDelete = nil } }
            ),
            presenting: layoutPendingDelete
        ) { preset in
            Button(
                "\(settings.localized("Delete")) \(preset.displayName)",
                role: .destructive
            ) {
                do {
                    try layoutPresets.deletePreset(id: preset.id)
                } catch {
                    layoutImportMessage =
                        "Layout deletion failed: \(error.localizedDescription)"
                    showLayoutImportAlert = true
                }
                layoutPendingDelete = nil
            }
            Button(settings.localized("Cancel"), role: .cancel) {
                layoutPendingDelete = nil
            }
        } message: { _ in
            Text(
                settings.localized(
                    "Games using this layout will fall back to their next available layout."
                )
            )
        }
        .sheet(isPresented: $showSkinImporter) {
            ImportDocumentPicker(
                allowedContentTypes: [
                    .image,
                    UTType(filenameExtension: "zip") ?? .data,
                    UTType(filenameExtension: "skin") ?? .data,
                    UTType(filenameExtension: "manic") ?? .data,
                    UTType(filenameExtension: "armsx2skin") ?? .data,
                    UTType(filenameExtension: "deltaskin") ?? .data,
                    UTType(filenameExtension: "manicskin") ?? .data,
                    .data
                ],
                allowsMultipleSelection: true
            ) { result in
                switch result {
                case .success(let urls):
                    Task {
                        let outcome = await importCustomSkins(urls)
                        lastSkinImportResult = outcome.importResult
                        skinImportMessage = outcome.message
                        showSkinImportAlert = true
                    }
                case .failure(let error):
                    lastSkinImportResult = nil
                    skinImportMessage = "Skin import failed: \(error.localizedDescription)"
                    showSkinImportAlert = true
                }
            }
        }
        .alert(settings.localized("Custom Skin"), isPresented: $showSkinImportAlert) {
            if let result = lastSkinImportResult {
                if result.includesLinkedLayout {
                    Button("Apply Skin Only Globally") {
                        selectSkin(id: result.descriptor.id)
                    }
                    Button("Apply Skin + Layout Globally") {
                        selectSkin(id: result.descriptor.id)
                        layoutPresets.globalPresetID = result.descriptor.linkedLayoutPresetID
                    }
                    Button("Apply Layout Only Globally") {
                        layoutPresets.globalPresetID = result.descriptor.linkedLayoutPresetID
                    }
                    Button("Later", role: .cancel) {}
                } else {
                    Button("Apply Skin Only Globally") {
                        selectSkin(id: result.descriptor.id)
                    }
                    Button("Later", role: .cancel) {}
                }
            } else {
                Button(settings.localized("OK"), role: .cancel) {}
            }
        } message: {
            Text(skinImportMessage)
        }
        .alert("Rename Skin", isPresented: Binding<Bool>(
            get: { skinPendingRename != nil },
            set: { if !$0 { skinPendingRename = nil } }
        )) {
            TextField("Name", text: $skinRenameDraft)
            Button("Save") {
                if let skin = skinPendingRename {
                    try? skinLibrary.renameImportedSkin(id: skin.id, to: skinRenameDraft)
                }
                skinPendingRename = nil
            }
            Button("Cancel", role: .cancel) {
                skinPendingRename = nil
            }
        } message: {
            Text("Choose a display name for this imported skin.")
        }
        .confirmationDialog(
            "Delete Skin?",
            isPresented: Binding<Bool>(
                get: { skinPendingDelete != nil },
                set: { if !$0 { skinPendingDelete = nil } }
            ),
            presenting: skinPendingDelete
        ) { skin in
            Button("Delete \(skin.displayName)", role: .destructive) {
                try? skinLibrary.deleteImportedSkin(id: skin.id, layoutPresets: layoutPresets)
                syncSettingsSkinFromLibrarySelection()
                skinPendingDelete = nil
            }
            Button("Cancel", role: .cancel) {
                skinPendingDelete = nil
            }
        } message: { skin in
            Text("This removes the imported skin. Linked layout presets are kept.")
        }
        .confirmationDialog(
            "\(skinReplacePrompt?.name ?? "This skin") is already installed",
            isPresented: Binding<Bool>(
                get: { skinReplacePrompt != nil },
                set: {
                    if !$0 {
                        skinReplacePrompt = nil
                        // A tapped button gets there first. This only catches a
                        // dialog that went away without one, which would
                        // otherwise leave the import waiting forever.
                        DispatchQueue.main.async { resumeSkinReplace(.cancel) }
                    }
                }
            ),
            presenting: skinReplacePrompt
        ) { prompt in
            Button("Replace") {
                skinReplacePrompt = nil
                resumeSkinReplace(.replace(prompt.existingSkinID))
            }
            Button("Keep Both") {
                skinReplacePrompt = nil
                resumeSkinReplace(.keepBoth)
            }
            Button("Cancel", role: .cancel) {
                skinReplacePrompt = nil
                resumeSkinReplace(.cancel)
            }
        } message: { _ in
            Text("Replace it, or keep both copies?")
        }
        .fullScreenCover(isPresented: $showLayoutEditor) {
            PadLayoutEditView(
                onDismiss: { showLayoutEditor = false },
                context: PadLayoutEditorContext(
                    presetID: layoutPresets.globalPresetID,
                    gameIdentity: nil,
                    initialSnapshot: layoutPresets.effectiveSnapshot(for: nil)
                )
            )
        }
    }

    private var selectedSkinDetail: String {
        let descriptor = skinLibrary.selectedDescriptor
        if descriptor.source == .imported {
            if descriptor.linkedLayoutPresetID != nil {
                return "Uses imported controller art. A recommended layout is saved separately and only applies when selected."
            }
            return "Uses imported controller art without changing the active layout."
        }
        return descriptor.virtualPadSkin.detail
    }

    private func selectSkin(id: String) {
        skinLibrary.selectSkin(id: id)
        syncSettingsSkinFromLibrarySelection()
    }

    private func syncSettingsSkinFromLibrarySelection() {
        settings.virtualPadSkin = skinLibrary.selectedDescriptor.virtualPadSkin
    }

    private func importLayouts(_ urls: [URL]) -> String {
        var messages: [String] = []
        for sourceURL in urls {
            let accessGranted = sourceURL.startAccessingSecurityScopedResource()
            defer {
                if accessGranted {
                    sourceURL.stopAccessingSecurityScopedResource()
                }
            }
            do {
                let data = try Data(contentsOf: sourceURL)
                let preset = try layoutPresets.importLayout(data: data, fallbackName: sourceURL.lastPathComponent)
                messages.append("Imported layout '\(preset.displayName)'.")
            } catch {
                messages.append("Layout import failed for \(sourceURL.lastPathComponent): \(error.localizedDescription)")
            }
        }
        return messages.isEmpty ? "No layout files were selected." : messages.joined(separator: "\n\n")
    }

    private func exportLayout(_ preset: PadLayoutPreset) {
        do {
            let data = try PadLayoutImportExport.exportData(for: preset)
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent(PadLayoutImportExport.exportedFileName(for: preset.displayName))
            try data.write(to: url, options: .atomic)
            layoutExportItem = ShareSheetItem(url: url)
        } catch {
            layoutImportMessage = "Layout export failed: \(error.localizedDescription)"
            showLayoutImportAlert = true
        }
    }

    private func importCustomSkins(_ urls: [URL]) async -> (message: String, importResult: VPadSkinImportResult?) {
        let stagingDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("ARMSX2SkinImport-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: stagingDirectory, withIntermediateDirectories: true)
        defer {
            try? FileManager.default.removeItem(at: stagingDirectory)
        }

        var messages: [String] = []
        var latestResult: VPadSkinImportResult?
        let looseFiles = urls.filter { !isSkinArchive($0) }
        let archiveFiles = urls.filter { isSkinArchive($0) }

        if !looseFiles.isEmpty {
            let looseDirectory = stagingDirectory.appendingPathComponent("LooseSkin", isDirectory: true)
            try? FileManager.default.createDirectory(at: looseDirectory, withIntermediateDirectories: true)
            for sourceURL in looseFiles {
                let accessGranted = sourceURL.startAccessingSecurityScopedResource()
                defer {
                    if accessGranted {
                        sourceURL.stopAccessingSecurityScopedResource()
                    }
                }
                let destination = looseDirectory.appendingPathComponent(sourceURL.lastPathComponent)
                try? FileManager.default.removeItem(at: destination)
                try? FileManager.default.copyItem(at: sourceURL, to: destination)
            }
            do {
                let result = try await skinLibrary.importSkin(
                    from: looseDirectory,
                    originalImportName: looseFiles.first?.lastPathComponent,
                    layoutPresets: layoutPresets
                )
                latestResult = result
                messages.append(result.message)
            } catch {
                messages.append("Skin import failed: \(error.localizedDescription)")
            }
        }

        for sourceURL in archiveFiles {
            let accessGranted = sourceURL.startAccessingSecurityScopedResource()
            defer {
                if accessGranted {
                    sourceURL.stopAccessingSecurityScopedResource()
                }
            }

            let archiveDirectory = stagingDirectory
                .appendingPathComponent(sourceURL.deletingPathExtension().lastPathComponent, isDirectory: true)
            let isV2Package = SkinManifestImporter.shouldTreatAsV2(
                manifestData: ARMSX2Bridge.peekSkinManifestData(at: sourceURL)
            )
            let extracted: [URL]
            if isV2Package {
                extracted = ARMSX2Bridge.extractSkinPackageArchive(at: sourceURL, to: archiveDirectory)
            } else {
                extracted = ARMSX2Bridge.extractControllerSkinArchive(at: sourceURL, to: archiveDirectory)
            }
            if extracted.isEmpty {
                messages.append("No usable skin files were imported from \(sourceURL.lastPathComponent).")
                continue
            }

            var replacingSkinID: String?
            let intendedName = skinLibrary.intendedDisplayName(forExtractedSkinAt: archiveDirectory)
            if let existing = skinLibrary.existingImportedSkin(matchingName: intendedName) {
                switch await askAboutExistingSkin(named: intendedName, existingSkinID: existing.id) {
                case .replace(let id):
                    replacingSkinID = id
                case .keepBoth:
                    break
                case .cancel:
                    messages.append("Kept the existing '\(intendedName)'.")
                    continue
                }
            }

            do {
                let result = try await skinLibrary.importSkin(
                    from: archiveDirectory,
                    originalImportName: sourceURL.lastPathComponent,
                    replacingSkinID: replacingSkinID,
                    layoutPresets: layoutPresets
                )
                latestResult = result
                messages.append(result.message)
            } catch {
                messages.append("Skin import failed: \(error.localizedDescription)")
            }
        }

        let message = messages.isEmpty
            ? "No usable skin images were imported. Use loose button PNGs/JPGs/WebPs, a portrait/landscape controller image, or a zip skin pack containing image files."
            : messages.joined(separator: "\n\n")
        return (message, latestResult)
    }

    private func askAboutExistingSkin(named name: String, existingSkinID: String) async -> SkinReplaceChoice {
        skinReplacePrompt = SkinReplacePrompt(name: name, existingSkinID: existingSkinID)
        return await skinReplaceGate.wait()
    }

    private func resumeSkinReplace(_ choice: SkinReplaceChoice) {
        skinReplaceGate.resume(choice)
    }

    private func isSkinArchive(_ url: URL) -> Bool {
        let ext = url.pathExtension.lowercased()
        return ext == "zip" || ext == "skin" || ext == "manic"
            || ext == "armsx2skin" || ext == "deltaskin" || ext == "manicskin"
    }

    static func canonicalSkinFileName(forImportPath path: String) -> String? {
        VPadSkinLibraryStore.canonicalSkinFileName(forImportPath: path)
    }

    @ViewBuilder
    private var controlSensitivitySection: some View {
        Section {
            DynamicControlSlider(
                title: "Movement Sensitivity",
                value: $dynamicSettings.movementSensitivity,
                range: 0.33...2.0,
                step: 0.01,
                format: .unitPercent
            )
            DynamicControlSlider(
                title: "Look Sensitivity",
                value: $dynamicSettings.lookSensitivity,
                range: 0.43...1.71,
                step: 0.01,
                format: .unitPercent
            )
            DynamicSwipeSensitivityControl(
                title: "Swipe Sensitivity",
                showsEnableToggle: false,
                isEnabled: .constant(true),
                value: $dynamicSettings.swipeSensitivity,
                horizontalSensitivity: $dynamicSettings.swipeHorizontalSensitivity,
                verticalSensitivity: $dynamicSettings.swipeVerticalSensitivity
            )
            .disabled(!dynamicSettings.swipeCamera)
            DynamicSwipeSensitivityControl(
                title: "Sensitivity While on Aim Mode",
                showsEnableToggle: true,
                isEnabled: $dynamicSettings.swipeSensitivityWhileAimingEnabled,
                value: $dynamicSettings.swipeSensitivityWhileAiming,
                horizontalSensitivity: $dynamicSettings.swipeHorizontalSensitivityWhileAiming,
                verticalSensitivity: $dynamicSettings.swipeVerticalSensitivityWhileAiming
            )
            .disabled(!dynamicSettings.swipeCamera)
            DynamicSwipeSensitivityControl(
                title: "Sensitivity While Not Aiming",
                showsEnableToggle: true,
                isEnabled: $dynamicSettings.swipeSensitivityWhileNotAimingEnabled,
                value: $dynamicSettings.swipeSensitivityWhileNotAiming,
                horizontalSensitivity: $dynamicSettings.swipeHorizontalSensitivityWhileNotAiming,
                verticalSensitivity: $dynamicSettings.swipeVerticalSensitivityWhileNotAiming
            )
            .disabled(!dynamicSettings.swipeCamera)
        } header: {
            Text(settings.localized("Dynamic Control Sensitivity"))
        } footer: {
            Text(settings.localized("Each swipe profile has overall, horizontal, and vertical sensitivity. Disabled aim-state profiles fall back to Swipe Sensitivity."))
        }
    }

    @ViewBuilder
    private func thumbstickActionButtonsSection(
        title: String,
        toggleTitle: String,
        isEnabled: Binding<Bool>,
        aim: Binding<VirtualPadActionButton>,
        fire: Binding<VirtualPadActionButton>,
        holdFire: Binding<VirtualPadActionButton>
    ) -> some View {
        Section {
            Toggle(toggleTitle, isOn: isEnabled)
            if isEnabled.wrappedValue {
                Picker(settings.localized("Aim (Hold Thumbstick)"), selection: aim) {
                    ForEach(VirtualPadActionButton.allCases) { button in
                        Text(settings.localized(button.title)).tag(button)
                    }
                }
                Picker(settings.localized("Fire (Tap Thumbstick)"), selection: fire) {
                    ForEach(VirtualPadActionButton.allCases) { button in
                        Text(settings.localized(button.title)).tag(button)
                    }
                }
                Picker(settings.localized("Hold Fire (Fast Tap Thumbstick)"), selection: holdFire) {
                    ForEach(VirtualPadActionButton.allCases) { button in
                        Text(settings.localized(button.title)).tag(button)
                    }
                }
            }
        } header: {
            Text(title)
        }
    }

    private func dynamicActionTitle(_ baseTitle: String, role: DynamicActionRole) -> String {
        var configuredButtons: [VirtualPadActionButton] = []
        if dynamicSettings.rightThumbstickActionsEnabled {
            configuredButtons.append(dynamicActionButton(role: role, side: .right))
        }
        if dynamicSettings.leftThumbstickActionsEnabled {
            configuredButtons.append(dynamicActionButton(role: role, side: .left))
        }
        if configuredButtons.isEmpty {
            configuredButtons.append(dynamicActionButton(role: role, side: .right))
        }

        var seen: Set<VirtualPadActionButton> = []
        let buttonNames = configuredButtons.compactMap { button -> String? in
            guard seen.insert(button).inserted else { return nil }
            return settings.localized(button.title)
        }
        return "\(settings.localized(baseTitle)) (\(buttonNames.joined(separator: " / ")))"
    }

    private func dynamicActionButton(
        role: DynamicActionRole,
        side: VirtualPadThumbstickSide
    ) -> VirtualPadActionButton {
        switch role {
        case .aim: return dynamicSettings.aimButton(for: side)
        case .fire: return dynamicSettings.fireButton(for: side)
        case .holdFire: return dynamicSettings.holdFireButton(for: side)
        }
    }

    /// The last readout that is a phrase rather than a number in a unit, so it stays a closure.
    private var automaticFireControlsDisabled: Bool {
        automaticFireBlockedByHardcore || !dynamicSettings.rapidTapFireEnabled
    }

    private func refreshHardcoreAutomaticFireRestriction() {
        automaticFireBlockedByHardcore = DynamicThumbstickSettings.automaticFireBlockedByHardcore()
    }
}

/// A NumberRow that always steps. Kept as a name of its own because this screen has thirty of
/// them and the argument order reads better than five labelled arguments repeated thirty times.
///
/// These are the only rows left that describe themselves at the call site, and they can: each one
/// is on this screen and nowhere else, so there is nothing for it to disagree with.
private struct DynamicControlSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    /// Always a real format, never a prebuilt string: a row that hands over a finished readout
    /// cannot label its bounds or be typed into, and it ends up the odd one out in the stack.
    var format: NumberFormat = .plain

    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        NumberRow(title,
                  value: $value,
                  in: range,
                  format: format,
                  step: step,
                  detents: NumberRow.stops(in: range, step: step),
                  settings: settings)
    }
}

private struct DynamicSwipeSensitivityControl: View {
    private static let sensitivityRange: ClosedRange<Double> = 0.08...0.75
    private static let sensitivityStep = 0.01

    let title: String
    let showsEnableToggle: Bool
    @Binding var isEnabled: Bool
    @Binding var value: Double
    @Binding var horizontalSensitivity: Double
    @Binding var verticalSensitivity: Double

    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            // The toggle used to carry the readout too. It moved down onto the row that actually
            // changes it, which is also the only way the slider gets an announced value.
            if showsEnableToggle {
                Toggle(settings.localized(title), isOn: $isEnabled)
            }
            // When the toggle above is already carrying the name, the row underneath is just the
            // amount, so it says so rather than printing the same words twice.
            NumberRow(showsEnableToggle ? "Sensitivity" : title,
                      value: $value,
                      in: Self.sensitivityRange,
                      format: .degreesPerPoint,
                      step: Self.sensitivityStep,
                      detents: NumberRow.stops(in: Self.sensitivityRange,
                                               step: Self.sensitivityStep),
                      settings: settings)
                .disabled(!isEnabled)
            DynamicControlSlider(
                title: "Horizontal Swipe Sensitivity",
                value: $horizontalSensitivity,
                range: 0.25...2,
                step: 0.01,
                format: .unitPercent
            )
            .disabled(!isEnabled)
            DynamicControlSlider(
                title: "Vertical Swipe Sensitivity",
                value: $verticalSensitivity,
                range: 0.25...2,
                step: 0.01,
                format: .unitPercent
            )
            .disabled(!isEnabled)
        }
    }
}
