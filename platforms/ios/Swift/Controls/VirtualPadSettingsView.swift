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

/// Explicit controls share their label identity with the predeclared Form order.
/// Touch keeps the native controls; Cross opens the same option list.
@MainActor
func controllerToggle(_ title: String, isOn: Binding<Bool>) -> some View {
    Toggle(title, isOn: isOn)
        .controllerAccessibilityToggleTarget(id: title, label: title, isOn: isOn)
}

@MainActor
func controllerPicker<Value: Hashable>(
    _ title: String, selection: Binding<Value>, options: [(id: Value, title: String)],
    id: String? = nil
) -> some View {
    Picker(title, selection: selection) {
        ForEach(options, id: \.id) { option in
            Text(option.title).tag(option.id)
        }
    }
    .controllerAccessibilityOptionsPickerTarget(id: id ?? title, label: title, selection: selection, options: options)
}

struct VirtualPadSettingsView: View {
    let onOpenPane: (SettingsPane) -> Void

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
    @State private var skinImport = VPadSkinImportFlow()
    @State private var automaticFireBlockedByHardcore = false

    init(onOpenPane: @escaping (SettingsPane) -> Void = { _ in }) {
        self.onOpenPane = onOpenPane
    }

    var body: some View {
        Form {
            Section(settings.localized("Appearance")) {
                controllerPicker(settings.localized("Button Skin"), selection: Binding<String>(
                    get: { skinLibrary.selectedSkinID },
                    set: { selectSkin(id: $0) }
                ), options: skinLibrary.allDescriptors.map { ($0.id, settings.localized($0.displayName)) })

                Text(settings.localized(selectedSkinDetail))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.padOpacity, value: $settings.padOpacity, settings: settings)
                    .controllerAccessibilityTargetID(settings.localized(NumberSetting.padOpacity.title))
            }

            Section(settings.localized("Gameplay")) {
                controllerToggle(settings.localized("Hide Virtual Pad When Controller Is Connected"), isOn: $settings.autoHideVirtualPadWhenControllerConnected)
                Text(settings.localized("Automatically hides the on-screen controls while an external controller is connected."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                controllerToggle(settings.localized("Auto Full Screen"), isOn: $settings.autoFullscreen)
                controllerToggle(settings.localized("Hide Menu Button"), isOn: $settings.hideMenuButton)
                Text(settings.localized("Hides the in-game menu button. Tap the game area to show it for a few seconds."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                controllerToggle(settings.localized("D-pad Diagonals"), isOn: $settings.dpadDiagonalsEnabled)
                Text(settings.localized("Allows one-finger diagonal and quarter-circle motions on the virtual D-pad."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                controllerToggle(settings.localized("Face Button Combo Zones"), isOn: $settings.faceComboZonesEnabled)
                Text(settings.localized("Press between face buttons to trigger both buttons at once."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.analogStickSize, value: $settings.analogStickScale, settings: settings)
                    .controllerAccessibilityTargetID(settings.localized(NumberSetting.analogStickSize.title))

                Text(settings.localized("Double-tap empty gameplay space to show the menu button again."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                controllerToggle(settings.localized("Invert Left Horizontal"), isOn: $settings.invertLeftStickX)
                controllerToggle(settings.localized("Invert Left Vertical"), isOn: $settings.invertLeftStickY)
                controllerToggle(settings.localized("Invert Right Horizontal"), isOn: $settings.invertRightStickX)
                controllerToggle(settings.localized("Invert Right Vertical (Camera)"), isOn: $settings.invertRightStickY)
            } header: {
                Text(settings.localized("Stick Inversion"))
            } footer: {
                Text(settings.localized("Flips the virtual and connected-controller stick axes. Useful for games with fixed inverted camera or flight controls. Per-game overrides are available in the game’s Virtual Pad settings."))
            }

            VirtualPadCustomSkinSection(
                onOpenPane: onOpenPane,
                selectSkin: { selectSkin(id: $0) },
                showSkinImporter: $showSkinImporter,
                skinPendingRename: $skinPendingRename,
                skinRenameDraft: $skinRenameDraft,
                skinPendingDelete: $skinPendingDelete
            )

            Section(settings.localized("Feedback")) {
                controllerToggle(settings.localized("Haptic Feedback"), isOn: $settings.hapticFeedback)

                NumberRow(.phoneRumbleStrength, value: $settings.phoneRumbleStrength,
                          settings: settings)
                    .controllerAccessibilityTargetID(settings.localized(NumberSetting.phoneRumbleStrength.title))
                Text(settings.localized("Controls the iPhone Taptic Engine when no controller is connected. 25% preserves the original Core Haptics intensity; 100% applies up to 3x gain, and 0% disables phone rumble. Has no effect on a controller's own motors."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                controllerToggle(
                    settings.localized("Increase Duration of Rumble and Interpolation"),
                    isOn: $settings.increaseRumbleDurationAndInterpolation)
            }

            VirtualPadLayoutSection(
                exportLayout: exportLayout,
                showLayoutEditor: $showLayoutEditor,
                showLayoutImporter: $showLayoutImporter,
                layoutPendingRename: $layoutPendingRename,
                layoutRenameDraft: $layoutRenameDraft,
                layoutPendingDelete: $layoutPendingDelete
            )

            VirtualPadDynamicControlSections()

            VirtualPadThumbstickSections()

            if dynamicSettings.swipeCamera ||
                (dynamicSettings.dynamicThumbsticks &&
                    (dynamicSettings.leftThumbstickActionsEnabled || dynamicSettings.rightThumbstickActionsEnabled)) {
                Section {
                    if dynamicSettings.swipeCamera {
                        controllerPicker(
                            settings.localized("Trigger Button When Un-holding Swipe"),
                            selection: $dynamicSettings.triggerButtonWhenUnholdingSwipe,
                            options: [(-1, settings.localized("Off"))]
                                + VirtualPadActionButton.allCases.map { ($0.rawValue, settings.localized($0.title)) }
                        )
                    }

                    controllerToggle(
                        dynamicActionTitle("Hold Aim While Touching Camera", role: .aim),
                        isOn: Binding(
                            get: { dynamicSettings.holdAimWhileSwipe },
                            set: { dynamicSettings.setHoldAimWhileSwipe($0) }
                        )
                    )
                    controllerToggle(
                        dynamicActionTitle("Double Tap to Hold Aim", role: .aim),
                        isOn: Binding(
                            get: { dynamicSettings.doubleTapToHoldAim },
                            set: { dynamicSettings.setDoubleTapToHoldAim($0) }
                        )
                    )
                    controllerToggle(
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
                    controllerToggle(
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
                    controllerToggle(
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
                    controllerToggle(
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
                    controllerToggle(
                        dynamicActionTitle("Extend Automatic Fire While Dragging", role: .holdFire),
                        isOn: $dynamicSettings.extendFireWhileDragging
                    )
                        .disabled(automaticFireControlsDisabled)
                    controllerToggle(
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
                ConfirmedSettingsResetButton(
                    settings.localized("Restore Dynamic Control Defaults"),
                    confirmationTitle: settings.localized("Restore Dynamic Control Defaults?"),
                    confirmationMessage: settings.localized("This restores every Dynamic Control option to its original value."),
                    completionMessage: settings.localized("Defaults Restored"),
                    controllerTargetID: "settings.virtual-pad.restore-dynamic-defaults"
                ) {
                    dynamicSettings.restoreDefaults()
                }
                .uiCriticalForegroundStyle()
            }
        }
        .navigationTitle(settings.localized("Virtual Pad"))
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
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
        .controllerPrompt(
            "Layout Import",
            isPresented: $showLayoutImportAlert,
            message: layoutImportMessage,
            actions: [.ok]
        )
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
        .controllerPrompt(
            settings.localized("Delete Layout?"),
            isPresented: Binding<Bool>(
                get: { layoutPendingDelete != nil },
                set: { if !$0 { layoutPendingDelete = nil } }
            ),
            message: settings.localized(
                "Games using this layout will fall back to their next available layout."
            ),
            actions: [
                .cancel,
                .init(
                    title: "\(settings.localized("Delete")) \(layoutPendingDelete?.displayName ?? "")",
                    isDestructive: true
                ) {
                    guard let preset = layoutPendingDelete else { return }
                    do {
                        try layoutPresets.deletePreset(id: preset.id)
                    } catch {
                        layoutImportMessage =
                            "Layout deletion failed: \(error.localizedDescription)"
                        showLayoutImportAlert = true
                    }
                },
            ]
        )
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
                        let outcome = await skinImport.importCustomSkins(urls)
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
        .controllerPrompt(
            settings.localized("Custom Skin"),
            isPresented: $showSkinImportAlert,
            message: skinImportMessage,
            actions: skinImportActions
        )
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
        .controllerPrompt(
            "Delete Skin?",
            isPresented: Binding<Bool>(
                get: { skinPendingDelete != nil },
                set: { if !$0 { skinPendingDelete = nil } }
            ),
            message: "This removes the imported skin. Linked layout presets are kept.",
            actions: [
                .cancel,
                .init(title: "Delete \(skinPendingDelete?.displayName ?? "")", isDestructive: true) {
                    guard let skin = skinPendingDelete else { return }
                    try? skinLibrary.deleteImportedSkin(id: skin.id, layoutPresets: layoutPresets)
                    syncSettingsSkinFromLibrarySelection()
                },
            ]
        )
        .controllerPrompt(
            "\(skinImport.skinReplacePrompt?.name ?? "This skin") is already installed",
            isPresented: Binding<Bool>(
                get: { skinImport.skinReplacePrompt != nil },
                set: {
                    if !$0 {
                        skinImport.skinReplacePrompt = nil
                        // A tapped button gets there first. This only catches a
                        // dialog that went away without one, which would
                        // otherwise leave the import waiting forever.
                        DispatchQueue.main.async { skinImport.resumeSkinReplace(.cancel) }
                    }
                }
            ),
            message: "Replace it, or keep both copies?",
            actions: [
                .init(title: "Cancel", isCancel: true) { skinImport.resumeSkinReplace(.cancel) },
                .init(title: "Keep Both") { skinImport.resumeSkinReplace(.keepBoth) },
                .init(title: "Replace") {
                    if let prompt = skinImport.skinReplacePrompt {
                        skinImport.resumeSkinReplace(.replace(prompt.existingSkinID))
                    }
                },
            ]
        )
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

    private var skinImportActions: [ControllerPrompt.Action] {
        guard let result = lastSkinImportResult else { return [.ok] }
        let skinID = result.descriptor.id
        let layoutID = result.descriptor.linkedLayoutPresetID
        var actions: [ControllerPrompt.Action] = [
            .init(title: "Later", isCancel: true),
            .init(title: "Apply Skin Only Globally") { selectSkin(id: skinID) },
        ]
        if result.includesLinkedLayout {
            actions.append(.init(title: "Apply Skin + Layout Globally") {
                selectSkin(id: skinID)
                layoutPresets.globalPresetID = layoutID
            })
            actions.append(.init(title: "Apply Layout Only Globally") {
                layoutPresets.globalPresetID = layoutID
            })
        }
        return actions
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

    /// Form recycles rows: the complete enabled order must exist independently
    /// of which controls are onscreen. Repeated controls have section-qualified
    /// IDs, so left/right and aim profiles cannot replace each other.
    private var controllerTargetOrder: [String] {
        let localized = settings.localized
        var order = ["Button Skin", NumberSetting.padOpacity.title,
                     "Hide Virtual Pad When Controller Is Connected", "Auto Full Screen", "Hide Menu Button",
                     "D-pad Diagonals", "Face Button Combo Zones", NumberSetting.analogStickSize.title,
                     "Invert Left Horizontal", "Invert Left Vertical", "Invert Right Horizontal",
                     "Invert Right Vertical (Camera)", "Automatic Download Custom Skin",
                     "Import Skin", "Browse Skins"].map(localized)
        order += skinLibrary.importedDescriptors.map { "vpad.skin.\($0.id)" }
        order += ["Haptic Feedback", NumberSetting.phoneRumbleStrength.title,
                  "Increase Duration of Rumble and Interpolation", "Default VPad Layout",
                  "Edit Layout", "Import Layout"].map(localized)
        order += layoutPresets.presets.map { "vpad.layout.\($0.id)" }
        order += BuiltInDynamicControlPreset.allCases.map { localized($0.rawValue) }
        order += ["Legacy Thumbsticks", "Dynamic Thumbsticks", "Swipe Camera", "Gyroscope Camera",
                  "Left Thumbstick Instant Deadzone"].map(localized)
        if dynamicSettings.leftInstantDeadzoneEnabled { order.append(localized("Left Negative Deadzone")) }
        order.append(localized("Right Thumbstick Instant Deadzone"))
        if dynamicSettings.rightInstantDeadzoneEnabled { order.append(localized("Right Negative Deadzone")) }
        order += ["Left Thumbstick Movement Area", "Right Thumbstick Movement Area",
                  "Convert Swipe to Dynamic Joystick"].map(localized)
        if dynamicSettings.convertSwipeToDynamicJoystick {
            order += ["Convert Into Dynamic Thumbstick", "Pulling Back Distance"].map(localized)
        }
        order += ["Movement Sensitivity", "Look Sensitivity"].map(localized)
        if dynamicSettings.swipeCamera {
            order += swipeProfileOrder("Swipe Sensitivity", enabled: true, toggle: false)
            order += swipeProfileOrder("Sensitivity While on Aim Mode", enabled: dynamicSettings.swipeSensitivityWhileAimingEnabled)
            order += swipeProfileOrder("Sensitivity While Not Aiming", enabled: dynamicSettings.swipeSensitivityWhileNotAimingEnabled)
        }
        if dynamicSettings.dynamicThumbsticks {
            order += thumbstickActionOrder("Dynamic Actions in Left Thumbstick", enabled: dynamicSettings.leftThumbstickActionsEnabled)
        }
        if dynamicSettings.swipeCamera || dynamicSettings.dynamicThumbsticks {
            order += thumbstickActionOrder("Dynamic Actions in Right Thumbstick", enabled: dynamicSettings.rightThumbstickActionsEnabled)
        }
        order.append(localized("Dynamic Aiming Crosshair"))
        if dynamicSettings.dynamicCrosshairEnabled {
            if dynamicSettings.swipeCamera {
                order.append(localized("Show Crosshair While Holding Swipe"))
                if dynamicSettings.showCrosshairWhileHoldingSwipe { order.append(localized("Crosshair Hide Delay")) }
            }
            order += ["Crosshair Size", "Crosshair Opacity", "Crosshair Type", "Crosshair Animation"].map(localized)
        }
        if dynamicSettings.dynamicThumbsticks {
            order += ["Maximum Radius", "Dead Zone", "Thumbstick Opacity", "Base Opacity", "Trail Opacity", "Activation Haptics"].map(localized)
        }
        if dynamicSettings.gyroscopeCamera {
            order += ["Gyro Sensitivity", "Gyro Acceleration", "Gyro Smoothing", "Gyro Dead Zone", "Maximum Gyro Rate",
                      "Invert Gyro Horizontal", "Invert Gyro Vertical"].map(localized)
        }
        if dynamicSettings.swipeCamera || (dynamicSettings.dynamicThumbsticks
            && (dynamicSettings.leftThumbstickActionsEnabled || dynamicSettings.rightThumbstickActionsEnabled)) {
            if dynamicSettings.swipeCamera { order.append(localized("Trigger Button When Un-holding Swipe")) }
            order += [dynamicActionTitle("Hold Aim While Touching Camera", role: .aim),
                      dynamicActionTitle("Double Tap to Hold Aim", role: .aim)]
            if dynamicSettings.doubleTapToHoldAim && !dynamicSettings.actionsOnNonAimMode {
                order.append(localized("Enable Single-Tap Action on Non-Aim Mode"))
            }
            order.append(localized("Enable Actions on Non-Aim Mode"))
            if dynamicSettings.holdAimWhileSwipe || dynamicSettings.doubleTapToHoldAim {
                order.append(dynamicActionTitle("Aim Release Delay", role: .aim))
            }
            if dynamicSettings.doubleTapToHoldAim { order.append(dynamicActionTitle("Double-Tap Window", role: .aim)) }
            order += ["Tap to Fire Single Shots", "Single-Shot Tap Duration", "Single-Shot Travel Tolerance"].map {
                dynamicActionTitle($0, role: .fire)
            }
            if !automaticFireBlockedByHardcore {
                order.append(dynamicActionTitle("Multiple Taps Enable Automatic Fire", role: .holdFire))
            }
            if !automaticFireControlsDisabled {
                order += ["Multiple-Tap Window", "Taps to Activate", "Automatic Fire Interval",
                          "Extend Automatic Fire While Dragging", "Release Fire When Touch Ends"].map {
                    dynamicActionTitle($0, role: .holdFire)
                }
                if !dynamicSettings.releaseFireWhenTouchEnds { order.append(dynamicActionTitle("Fire Release Delay", role: .holdFire)) }
            }
        }
        order.append("settings.virtual-pad.restore-dynamic-defaults")
        return order
    }

    private func swipeProfileOrder(_ title: String, enabled: Bool, toggle: Bool = true) -> [String] {
        let id = settings.localized(title)
        return (toggle ? [id] : []) + (enabled ? [id + "/value", id + "/horizontal", id + "/vertical"] : [])
    }

    private func thumbstickActionOrder(_ title: String, enabled: Bool) -> [String] {
        let id = settings.localized(title)
        return [id] + (enabled ? [id + "/aim", id + "/fire", id + "/holdFire"] : [])
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
