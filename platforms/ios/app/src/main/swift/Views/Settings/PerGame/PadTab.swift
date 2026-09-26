// PadTab.swift — Per-game Virtual Pad category tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadTab: View {
    @Binding var padLayoutIdentity: PadLayoutGameIdentity?
    @Binding var showPadLayoutEditor: Bool

    let layoutPresets: PadLayoutPresetStore
    let skinLibrary: VPadSkinLibraryStore
    let savesToRunningGame: Bool
    let iso: String
    let hasGameSettingsIdentity: Bool

    @State private var inversionDrafts: [String: Int] = [:]
    private let inversionKeys = SettingsStore.stickInversionKeys
    private let inversionSection = "ARMSX2iOS/UI"

    var body: some View {
        PerGameTab(title: "Virtual Pad") {
            Section("Virtual Pad") {
                if let padLayoutIdentity {
                    let layoutSelection = Binding<String?>(
                        get: { layoutPresets.presetID(for: padLayoutIdentity) },
                        set: { layoutPresets.setPreset($0, for: padLayoutIdentity) }
                    )
                    Picker("Layout", selection: layoutSelection) {
                        Text("Global Default (\(globalLayoutDisplayName))").tag(nil as String?)
                        ForEach(layoutPresets.presets) { preset in
                            Text(preset.displayName).tag(Optional(preset.id))
                        }
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "per-game.pad.layout",
                        label: "Layout",
                        selection: layoutSelection,
                        options: layoutControllerOptions
                    )

                    let skinSelection = Binding<String?>(
                        get: { validPerGameSkinID(for: padLayoutIdentity) },
                        set: { skinID in
                            if let skinID {
                                layoutPresets.setSkin(skinID, for: padLayoutIdentity, using: skinLibrary)
                            } else {
                                layoutPresets.clearSkin(for: padLayoutIdentity)
                            }
                        }
                    )
                    Picker("Skin", selection: skinSelection) {
                        Text("Global Default (\(globalSkinDisplayName))").tag(nil as String?)
                        ForEach(skinLibrary.allDescriptors) { skin in
                            Text(skin.displayName).tag(Optional(skin.id))
                        }
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "per-game.pad.skin",
                        label: "Skin",
                        selection: skinSelection,
                        options: skinControllerOptions
                    )

                    if let linkedLayoutID = linkedLayoutIDForCurrentSkin,
                       let linkedLayout = layoutPresets.preset(id: linkedLayoutID) {
                        Button {
                            layoutPresets.setPreset(linkedLayoutID, for: padLayoutIdentity)
                        } label: {
                            Label("Apply Linked Skin Layout to This Game", systemImage: "square.and.arrow.down")
                        }
                        .controllerAccessibilityActionTarget(
                            id: "per-game.pad.apply-linked-layout",
                            label: "Apply Linked Skin Layout to This Game"
                        ) {
                            layoutPresets.setPreset(
                                linkedLayoutID,
                                for: padLayoutIdentity
                            )
                        }
                        Text("Applies \(linkedLayout.displayName) for this game only. The selected skin is unchanged.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    Button {
                        showPadLayoutEditor = true
                    } label: {
                        Label("Edit Layout for This Game", systemImage: "square.resize")
                    }
                    .controllerAccessibilityActionTarget(
                        id: "per-game.pad.edit-layout",
                        label: "Edit Layout for This Game"
                    ) {
                        showPadLayoutEditor = true
                    }

                    Button("Reset VPad Layout to Global") {
                        layoutPresets.setPreset(nil, for: padLayoutIdentity)
                    }
                    .controllerAccessibilityActionTarget(
                        id: "per-game.pad.reset-layout",
                        label: "Reset VPad Layout to Global"
                    ) {
                        layoutPresets.setPreset(nil, for: padLayoutIdentity)
                    }

                    Button("Reset VPad Skin to Global") {
                        layoutPresets.clearSkin(for: padLayoutIdentity)
                    }
                    .controllerAccessibilityActionTarget(
                        id: "per-game.pad.reset-skin",
                        label: "Reset VPad Skin to Global"
                    ) {
                        layoutPresets.clearSkin(for: padLayoutIdentity)
                    }

                    ConfirmedSettingsResetButton(
                        SettingsStore.shared.localized(
                            "Reset All VPad Overrides"
                        ),
                        confirmationTitle: SettingsStore.shared.localized(
                            "Reset All Virtual Pad Overrides?"
                        ),
                        confirmationMessage: SettingsStore.shared.localized(
                            "This returns the layout, skin, and stick inversion settings for this game to their global values."
                        ),
                        completionMessage: SettingsStore.shared.localized(
                            "Overrides Restored"
                        ),
                        controllerTargetID: "per-game.pad.reset-all"
                    ) {
                        layoutPresets.clearVPadOverrides(for: padLayoutIdentity)
                        inversionDrafts = [:]
                        for key in inversionKeys {
                            clearInversionOverride(key)
                        }
                    }
                } else {
                    Text("Start this game once before choosing a custom layout or skin.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            // Inversion lands in the per-game INI, which is keyed on the game's CRC —
            // without one the pickers would move and write nowhere.
            if hasGameSettingsIdentity {
                Section {
                    ForEach(inversionKeys, id: \.self) { key in
                        let selection = Binding<Int>(
                            get: { inversionDrafts[key] ?? -1 },
                            set: { applyInversion(key, value: $0) }
                        )
                        Picker(inversionLabel(for: key), selection: selection) {
                            Text("Use Global").tag(-1)
                            Text("Off").tag(0)
                            Text("On").tag(1)
                        }
                        .controllerAccessibilityOptionsPickerTarget(
                            id: "per-game.pad.inversion.\(key)",
                            label: inversionLabel(for: key),
                            selection: selection,
                            options: inversionControllerOptions
                        )
                    }
                } header: {
                    Text("Stick Inversion")
                } footer: {
                    Text("Overrides the global stick inversion for this game only. Everything on this page saves as you change it, so Save and Cancel don't apply here.")
                }
            }
        }
        .onAppear { loadInversionDrafts() }
    }

    private func inversionLabel(for key: String) -> String {
        switch key {
        case "InvertLeftStickX": return "Left Horizontal"
        case "InvertLeftStickY": return "Left Vertical"
        case "InvertRightStickX": return "Right Horizontal"
        case "InvertRightStickY": return "Right Vertical (Camera)"
        default: return key
        }
    }

    private var layoutControllerOptions: [(id: String?, title: String)] {
        [(nil, "Global Default (\(globalLayoutDisplayName))")]
            + layoutPresets.presets.map { (Optional($0.id), $0.displayName) }
    }

    private var skinControllerOptions: [(id: String?, title: String)] {
        [(nil, "Global Default (\(globalSkinDisplayName))")]
            + skinLibrary.allDescriptors.map {
                (Optional($0.id), $0.displayName)
            }
    }

    private var inversionControllerOptions: [(id: Int, title: String)] {
        [(-1, "Use Global"), (0, "Off"), (1, "On")]
    }

    private func loadInversionDrafts() {
        var drafts: [String: Int] = [:]
        for key in inversionKeys {
            if hasInversionOverride(key) {
                drafts[key] = inversionOverride(key) ? 1 : 0
            }
        }
        inversionDrafts = drafts
    }

    // Commits on pick, like the layout and skin pickers. Nothing in this tab is
    // staged, so none of it feeds the panel's Save fingerprint.
    private func applyInversion(_ key: String, value: Int) {
        var drafts = inversionDrafts
        drafts[key] = value
        inversionDrafts = drafts
        if value == -1 {
            clearInversionOverride(key)
        } else {
            setInversionOverride(key, value == 1)
        }
    }

    // The panel opens both in-game and from the library. The "current game" bridge
    // variants resolve their identity from the running VM and silently no-op without
    // one, so the library path has to address the per-game INI by ISO instead.

    private var targetISO: String? { savesToRunningGame ? nil : iso }

    private func hasInversionOverride(_ key: String) -> Bool {
        ARMSX2Bridge.hasPerGameINIValue(inversionSection, key: key, forISO: targetISO)
    }

    private func inversionOverride(_ key: String) -> Bool {
        ARMSX2Bridge.getPerGameINIBool(inversionSection, key: key, defaultValue: false, forISO: targetISO)
    }

    private func setInversionOverride(_ key: String, _ value: Bool) {
        ARMSX2Bridge.setPerGameINIBool(inversionSection, key: key, value: value, forISO: targetISO)
        if savesToRunningGame {
            SettingsStore.shared.reloadStickInversionOverrides()
        }
    }

    private func clearInversionOverride(_ key: String) {
        ARMSX2Bridge.deletePerGameINIValue(inversionSection, key: key, forISO: targetISO)
        if savesToRunningGame {
            SettingsStore.shared.reloadStickInversionOverrides()
        }
    }

    private var globalLayoutDisplayName: String {
        layoutPresets.effectivePreset(for: nil)?.displayName ?? "Current Layout"
    }

    private var globalSkinDisplayName: String {
        skinLibrary.selectedDescriptor.displayName
    }

    private var linkedLayoutIDForCurrentSkin: String? {
        guard let descriptor = currentPerGameSkinDescriptor,
              let linkedLayoutID = descriptor.linkedLayoutPresetID,
              layoutPresets.preset(id: linkedLayoutID) != nil else {
            return nil
        }
        return linkedLayoutID
    }

    private var currentPerGameSkinDescriptor: VPadSkinDescriptor? {
        layoutPresets.effectiveSkinDescriptor(for: padLayoutIdentity, using: skinLibrary)
    }

    private func validPerGameSkinID(for identity: PadLayoutGameIdentity) -> String? {
        guard let skinID = layoutPresets.skinID(for: identity),
              skinLibrary.descriptor(id: skinID) != nil else {
            return nil
        }
        return skinID
    }
}
