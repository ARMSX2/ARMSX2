// VirtualPadLayoutSection.swift — pad layouts on the Virtual Pad page
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct VirtualPadLayoutSection: View {
    let exportLayout: @MainActor (PadLayoutPreset) -> Void
    @Binding var showLayoutEditor: Bool
    @Binding var showLayoutImporter: Bool
    @Binding var layoutPendingRename: PadLayoutPreset?
    @Binding var layoutRenameDraft: String
    @Binding var layoutPendingDelete: PadLayoutPreset?

    @State private var settings = SettingsStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared

    var body: some View {
        Section(settings.localized("Layout")) {
            if let notice = unreadableFileNotice {
                Label {
                    Text(notice)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } icon: {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(OverlayTheme.warm)
                }
            }

            controllerPicker(settings.localized("Default VPad Layout"), selection: Binding<String?>(
                get: { layoutPresets.globalPresetID },
                set: { layoutPresets.globalPresetID = $0 }
            ), options: [(nil as String?, settings.localized("Current Layout"))]
                + layoutPresets.presets.map { (Optional($0.id), $0.displayName) })

            Button {
                showLayoutEditor = true
            } label: {
                Label(settings.localized("Edit Layout"), systemImage: "square.resize")
            }
            .controllerAccessibilityActionTarget(id: settings.localized("Edit Layout"), label: settings.localized("Edit Layout")) {
                showLayoutEditor = true
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
            .controllerAccessibilityActionTarget(id: settings.localized("Import Layout"), label: settings.localized("Import Layout")) {
                showLayoutImporter = true
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
                        .controllerAccessibilityMenuTarget(id: "vpad.layout.\(preset.id)", label: preset.displayName)
                    }
                }
            }
        }
    }

    private var unreadableFileNotice: String? {
        if let name = layoutPresets.setAsideName {
            return String(format: settings.localized("Your layouts file couldn't be read. It was kept as %@, and layouts start fresh."), name)
        }
        if layoutPresets.readOnly {
            return settings.localized("Your layouts file couldn't be read. Saved layouts can't be changed until the app restarts.")
        }
        return nil
    }
}
