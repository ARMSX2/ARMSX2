// VirtualPadCustomSkinSection.swift — imported skins on the Virtual Pad page
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct VirtualPadCustomSkinSection: View {
    let onOpenPane: (SettingsPane) -> Void
    let selectSkin: @MainActor (String) -> Void
    @Binding var showSkinImporter: Bool
    @Binding var skinPendingRename: VPadSkinDescriptor?
    @Binding var skinRenameDraft: String
    @Binding var skinPendingDelete: VPadSkinDescriptor?

    @State private var settings = SettingsStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared

    var body: some View {
        Section(settings.localized("Custom Skin")) {
            controllerToggle(
                settings.localized("Automatic Download Custom Skin"),
                isOn: Bindable(settings.controller).automaticDownloadCustomSkin
            )
            Text(settings.localized("When a newly added game's serial matches the community catalog, its custom skin and recommended layout are downloaded automatically. You choose whether to apply them when launching the game."))
                .font(.caption)
                .foregroundStyle(.secondary)

            Button {
                showSkinImporter = true
            } label: {
                Label(settings.localized("Import Skin"), systemImage: "paintpalette")
            }
            .controllerAccessibilityActionTarget(id: settings.localized("Import Skin"), label: settings.localized("Import Skin")) {
                showSkinImporter = true
            }

            NavigationLink(value: SettingsPane.skinBrowser) {
                Label("Browse Skins", systemImage: "square.grid.2x2")
            }
            .controllerAccessibilityActionTarget(
                id: settings.localized("Browse Skins"),
                label: settings.localized("Browse Skins")
            ) {
                onOpenPane(.skinBrowser)
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
                                selectSkin(skin.id)
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
                        .controllerAccessibilityMenuTarget(id: "vpad.skin.\(skin.id)", label: skin.displayName)
                    }
                }
            }
        }
    }
}
