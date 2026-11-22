// AppearanceBackgroundSections.swift — background picker, fit mode and where the background shows
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct AppearanceBackgroundSections: View, AppearanceSettingsEditing {
    @State private var settings = SettingsStore.shared
    @State private var showPrimaryPicker = false
    @State private var showLandscapePicker = false

    var body: some View {
        Section {
            BackgroundAssetRow(
                title: settings.localized("Primary Background"),
                asset: settings.appearance.backgroundPrimaryAsset,
                glyph: "rectangle.portrait",
                caption: settings.localized("Shown in portrait and anywhere no landscape background is set.")
            ) { showPrimaryPicker = true }
            .modifier(BackgroundSourcePicker(isPresented: $showPrimaryPicker, role: .primary, existingAsset: { settings.appearance.backgroundPrimaryAsset }) { updatePrimary($0) })
            .appearanceControllerListRow(
                "settings.appearance.primary-background"
            )

            BackgroundAssetRow(
                title: settings.localized("Landscape Background"),
                asset: settings.appearance.backgroundLandscapeAsset,
                glyph: "rectangle",
                caption: settings.localized("Optional. Used only when the device is held in landscape.")
            ) { showLandscapePicker = true }
            .modifier(BackgroundSourcePicker(isPresented: $showLandscapePicker, role: .landscape, existingAsset: { settings.appearance.backgroundLandscapeAsset }) { updateLandscape($0) })
            .appearanceControllerListRow(
                "settings.appearance.landscape-background"
            )
        } header: {
            Text(settings.localized("Background"))
        } footer: {
            Text(settings.localized("Each orientation keeps its own background. Setting one never overwrites the other."))
        }

        Section {
            Picker(selection: manualBinding(for: \.appearance.backgroundFitMode)) {
                ForEach(BackgroundFitMode.allCases) { mode in
                    Text(label(for: mode)).tag(mode)
                }
            } label: {
                Label(settings.localized("Portrait Fit Mode"), systemImage: "rectangle.portrait")
            }
            .buttonStyle(.plain)
            .controllerAccessibilityOptionsPickerTarget(
                id: nil,
                label: settings.localized("Portrait Fit Mode"),
                selection: manualBinding(for: \.appearance.backgroundFitMode),
                options: BackgroundFitMode.allCases.map {
                    (id: $0, title: label(for: $0))
                }
            )
            .onChange(of: settings.appearance.backgroundFitMode) { _, _ in
                UISelectionFeedbackGenerator().selectionChanged()
            }
            .appearanceControllerListRow(
                "settings.appearance.portrait-fit-mode"
            )

            portraitFitModeHint
                .gameCardTintMenuBackgroundListRow(true)

            Picker(
                selection: manualBinding(
                    for: \.appearance.backgroundLandscapeFitMode
                )
            ) {
                ForEach(BackgroundFitMode.allCases) { mode in
                    Text(label(for: mode)).tag(mode)
                }
            } label: {
                Label(settings.localized("Landscape Fit Mode"), systemImage: "rectangle")
            }
            .buttonStyle(.plain)
            .controllerAccessibilityOptionsPickerTarget(
                id: nil,
                label: settings.localized("Landscape Fit Mode"),
                selection: manualBinding(
                    for: \.appearance.backgroundLandscapeFitMode
                ),
                options: BackgroundFitMode.allCases.map {
                    (id: $0, title: label(for: $0))
                }
            )
            .onChange(of: settings.appearance.backgroundLandscapeFitMode) { _, _ in
                UISelectionFeedbackGenerator().selectionChanged()
            }
            .appearanceControllerListRow(
                "settings.appearance.landscape-fit-mode"
            )

            landscapeFitModeHint
                .gameCardTintMenuBackgroundListRow(true)
        } header: {
            Text(settings.localized("Fit Mode"))
        } footer: {
            Text(settings.localized("Each orientation uses its own fit mode."))
        }

        Section {
            Toggle(isOn: manualBinding(for: \.appearance.backgroundVideoMuted)) {
                Label(settings.localized("Mute Video"), systemImage: settings.appearance.backgroundVideoMuted ? "speaker.slash" : "speaker.wave.2")
            }
            .appearanceControllerListRow(
                "settings.appearance.mute-video"
            )

            NumberRow(
                .backgroundDim,
                value: manualBinding(for: \.appearance.backgroundDim),
                settings: settings
            )
                .appearanceControllerListRow(
                    "settings.appearance.background-dim"
                )
        }

        Section {
            Toggle(isOn: manualBinding(for: \.appearance.backgroundEnabledInBIOS)) {
                Label(settings.localized("BIOS"), systemImage: "cpu")
            }
            .appearanceControllerListRow(
                "settings.appearance.show-in-bios"
            )
            Toggle(
                isOn: manualBinding(for: \.appearance.backgroundEnabledInSettings)
            ) {
                Label(settings.localized("Settings"), systemImage: "gearshape")
            }
            .appearanceControllerListRow(
                "settings.appearance.show-in-settings"
            )
        } header: {
            Text(settings.localized("Show Background In"))
        } footer: {
            Text(settings.localized("The background always shows behind Games. BIOS and Settings can be toggled independently. Dim or mute from the settings above."))
        }
    }

    @ViewBuilder
    private var portraitFitModeHint: some View {
        switch settings.appearance.backgroundFitMode {
        case .fill:
            HintRow(icon: "arrow.up.left.and.arrow.down.right", text: settings.localized("Fill — covers the whole screen, cropping edges if needed."))
        case .fit:
            HintRow(icon: "rectangle.compress.vertical", text: settings.localized("Fit — shows the whole image with bars on the empty sides."))
        case .stretch:
            HintRow(icon: "arrow.left.and.right", text: settings.localized("Stretch — fills the screen, distorting to match."))
        }
    }

    @ViewBuilder
    private var landscapeFitModeHint: some View {
        switch settings.appearance.backgroundLandscapeFitMode {
        case .fill:
            HintRow(icon: "arrow.up.left.and.arrow.down.right", text: settings.localized("Fill — covers the whole screen, cropping edges if needed."))
        case .fit:
            HintRow(icon: "rectangle.compress.vertical", text: settings.localized("Fit — shows the whole image with bars on the empty sides."))
        case .stretch:
            HintRow(icon: "arrow.left.and.right", text: settings.localized("Stretch — fills the screen, distorting to match."))
        }
    }

    private func label(for mode: BackgroundFitMode) -> String {
        switch mode {
        case .fill: return settings.localized("Fill")
        case .fit: return settings.localized("Fit")
        case .stretch: return settings.localized("Stretch")
        }
    }

    private func updatePrimary(_ asset: BackgroundAsset?) {
        settings.appearance.beginCustomThemeEditing()
        if asset == nil { BackgroundStorage.remove(settings.appearance.backgroundPrimaryAsset) }
        if asset != nil { settings.appearance.dynamicBackgroundsEnabled = false }
        settings.appearance.backgroundPrimaryAsset = asset
    }

    private func updateLandscape(_ asset: BackgroundAsset?) {
        settings.appearance.beginCustomThemeEditing()
        if asset == nil { BackgroundStorage.remove(settings.appearance.backgroundLandscapeAsset) }
        if asset != nil { settings.appearance.dynamicBackgroundsEnabled = false }
        settings.appearance.backgroundLandscapeAsset = asset
    }
}

private struct HintRow: View {
    let icon: String
    let text: String

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon).font(.caption).foregroundStyle(.secondary).frame(width: 16)
            Text(text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(.vertical, 2)
    }
}
