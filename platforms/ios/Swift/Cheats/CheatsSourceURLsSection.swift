// CheatsSourceURLsSection.swift — The Cheats & Patches panel's source URL editor
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct CheatsSourceURLsSection: View {
    let controllerInput: MenuControllerInputRouter?
    let controllerTarget: CheatsPatchesManagerView.ControllerTarget
    @Binding var showAdvanced: Bool
    @Binding var patchSourcesDraft: [String]
    @Binding var cheatSourcesDraft: [String]

    @State private var settings = SettingsStore.shared
    @State private var store = PatchStore.shared

    private typealias ControllerTarget = CheatsPatchesManagerView.ControllerTarget

    var body: some View {
        Section {
            DisclosureGroup(isExpanded: $showAdvanced) {
                Text(settings.localized("Patch sources"))
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .accessibilityAddTraits(.isHeader)
                ForEach(patchSourcesDraft.indices, id: \.self) { index in
                    sourceRow(
                        placeholder: settings.localized("Patch Source URL"),
                        text: $patchSourcesDraft[index],
                        label: settings.localized("Patch source URL")
                    ) {
                        guard patchSourcesDraft.indices.contains(index) else { return }
                        patchSourcesDraft.remove(at: index)
                    }
                }
                Button {
                    patchSourcesDraft.append("")
                } label: {
                    Label(settings.localized("Add source"), systemImage: "plus.circle")
                        .modifier(controllerFocus(.addPatchSource))
                }
                .buttonStyle(.borderless)
                .id(ControllerTarget.addPatchSource)

                Text(settings.localized("Cheat sources"))
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .accessibilityAddTraits(.isHeader)
                    .padding(.top, 8)
                ForEach(cheatSourcesDraft.indices, id: \.self) { index in
                    sourceRow(
                        placeholder: settings.localized("Cheat Source URL"),
                        text: $cheatSourcesDraft[index],
                        label: settings.localized("Cheat source URL")
                    ) {
                        guard cheatSourcesDraft.indices.contains(index) else { return }
                        cheatSourcesDraft.remove(at: index)
                    }
                }
                Button {
                    cheatSourcesDraft.append("")
                } label: {
                    Label(settings.localized("Add source"), systemImage: "plus.circle")
                        .modifier(controllerFocus(.addCheatSource))
                }
                .buttonStyle(.borderless)
                .id(ControllerTarget.addCheatSource)

                Button(settings.localized("Save Source URLs")) {
                    store.patchDatabaseURLTemplates = patchSourcesDraft
                    store.cheatDatabaseURLTemplates = cheatSourcesDraft
                    patchSourcesDraft = store.patchDatabaseURLTemplates
                    cheatSourcesDraft = store.cheatDatabaseURLTemplates
                    store.applyFeedback(settings.localized("Source URLs saved."), kind: .success)
                }
                .buttonStyle(.borderless)
                .padding(.top, 8)
                .modifier(controllerFocus(.saveSources))
                .id(ControllerTarget.saveSources)

                Text(settings.localized("Supported placeholders: \u{24}{serial}, \u{24}{crc}, and \u{24}{title}. Built-in sources provide PCSX2 patches, an UltraWidescreen / NaturalVision pack, and a community cheat collection. Only add sources you trust."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } label: {
                Label(settings.localized("Source URLs"), systemImage: "link")
                    .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
                    .modifier(controllerFocus(.advanced))
            }
            .id(ControllerTarget.advanced)
        } header: {
            Text(settings.localized("Advanced"))
        }
    }

    // One source-URL row. The text binding is captured per-row so SwiftUI tracks it
    // independently of the surrounding indices; the remove closure guards its index.
    @ViewBuilder
    private func sourceRow(
        placeholder: String,
        text: Binding<String>,
        label: String,
        onRemove: @escaping () -> Void
    ) -> some View {
        HStack(spacing: 8) {
            TextField(placeholder, text: text, axis: .vertical)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .font(.caption.monospaced())
                .accessibilityLabel(label)
            Button(action: onRemove) {
                Image(systemName: "minus.circle.fill")
                    .uiCriticalForegroundStyle()
            }
            .buttonStyle(.plain)
            .accessibilityLabel(settings.localized("Remove source"))
        }
    }

    private func controllerFocus(_ target: ControllerTarget) -> CheatsControllerFocusModifier {
        cheatsControllerFocus(target, controllerInput: controllerInput, controllerTarget: controllerTarget)
    }
}
