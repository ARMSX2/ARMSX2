// CheatsInstalledSection.swift — The Cheats & Patches panel's Installed section
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct CheatsInstalledSection: View {
    let controllerInput: MenuControllerInputRouter?
    let controllerTarget: CheatsPatchesManagerView.ControllerTarget
    @Binding var pendingRemoval: InstalledFileRemoval?
    @Binding var pendingEntryRemoval: PatchEntry?

    @State private var settings = SettingsStore.shared
    @State private var store = PatchStore.shared

    private typealias ControllerTarget = CheatsPatchesManagerView.ControllerTarget

    @ViewBuilder
    var body: some View {
        Section {
            if store.installed.isEmpty {
                Text(installedEmptyMessage)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                ForEach(PatchDisplayGroup.allCases, id: \.self) { group in
                    let entries = store.installed.filter { $0.displayGroup == group }
                    if !entries.isEmpty {
                        Text(settings.localized(group.title))
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.secondary)
                            .accessibilityAddTraits(.isHeader)
                        ForEach(entries) { entry in
                            installedRow(entry)
                                .modifier(
                                    controllerFocus(.installed(entry.id))
                                )
                                .id(ControllerTarget.installed(entry.id))
                                .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                    if !entry.isLegacy && !entry.name.isEmpty {
                                        Button(role: .destructive) {
                                            pendingEntryRemoval = entry
                                        } label: {
                                            Label(settings.localized("Remove Entry"), systemImage: "trash")
                                        }
                                    }
                                }
                        }
                    }
                }

                if hasNamedEntries {
                    HStack(spacing: 16) {
                        Button {
                            store.setAllNamedEntries(enabled: true)
                        } label: {
                            Label(settings.localized("Enable All"), systemImage: "checkmark.circle")
                                .modifier(controllerFocus(.enableAll))
                        }
                        .disabled(!store.canEnableAll)
                        .id(ControllerTarget.enableAll)

                        Button {
                            store.setAllNamedEntries(enabled: false)
                        } label: {
                            Label(settings.localized("Disable All"), systemImage: "circle.slash")
                                .modifier(controllerFocus(.disableAll))
                        }
                        .disabled(!store.canDisableAll)
                        .id(ControllerTarget.disableAll)
                    }
                    .buttonStyle(.borderless)
                    .accessibilityElement(children: .contain)
                }

                Menu {
                    if patchEntryCount > 0 {
                        Button(role: .destructive) {
                            pendingRemoval = .patch
                        } label: {
                            Label(String(format: settings.localized("Remove Patch File (%@)"), entryCountLabel(patchEntryCount)), systemImage: "trash")
                        }
                    }
                    if cheatEntryCount > 0 {
                        Button(role: .destructive) {
                            pendingRemoval = .cheat
                        } label: {
                            Label(String(format: settings.localized("Remove Cheat File (%@)"), entryCountLabel(cheatEntryCount)), systemImage: "trash")
                        }
                    }
                    if patchEntryCount > 0 && cheatEntryCount > 0 {
                        Button(role: .destructive) {
                            pendingRemoval = .all
                        } label: {
                            Label(settings.localized("Remove All Installed Files"), systemImage: "trash.fill")
                        }
                    }
                } label: {
                    Label(settings.localized("Remove Installed Files…"), systemImage: "trash")
                        .modifier(controllerFocus(.removeInstalled))
                }
                .accessibilityHint(settings.localized("Removes complete installed files, not individual entries"))
                .id(ControllerTarget.removeInstalled)
            }
        } header: {
            Text(settings.localized("Installed"))
        } footer: {
            Text(settings.localized("Some changes only take effect after restarting the game."))
        }
    }

    private var installedEmptyMessage: String {
        store.canManageInstalledFiles
            ? settings.localized("No cheats or patches are installed for this game yet.")
            : settings.localized("No installed entries are available yet.")
    }

    private var hasNamedEntries: Bool {
        store.installed.contains { !$0.isLegacy && !$0.name.isEmpty }
    }

    private var patchEntryCount: Int {
        store.installed.filter { !$0.isCheat }.count
    }

    private var cheatEntryCount: Int {
        store.installed.filter { $0.isCheat }.count
    }

    private func entryCountLabel(_ count: Int) -> String {
        count == 1 ? settings.localized("1 entry") : String(format: settings.localized("%d entries"), count)
    }

    @ViewBuilder
    private func installedRow(_ entry: PatchEntry) -> some View {
        let isOn = store.installed.first(where: { $0.id == entry.id })?.enabled ?? entry.enabled
        let hcBlocks = PatchStore.hardcoreBlocksPnachContent()
        let suppressed = hcBlocks && isOn && !PatchStore.hardcoreAllowsPatch(entry)
        VStack(alignment: .leading, spacing: 7) {
            if entry.isLegacy {
                Label(entry.displayTitle, systemImage: "doc.text")
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
            } else if suppressed {
                HStack(alignment: .firstTextBaseline) {
                    Text(entry.displayTitle)
                        .font(.body)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 4)
                    Button {
                        store.toggle(entry)
                    } label: {
                        Label(settings.localized("Suppressed by Hardcore"), systemImage: "lock.fill")
                            .font(.caption2.weight(.medium))
                            .foregroundStyle(.orange)
                    }
                    .buttonStyle(.plain)
                    .accessibilityHint(settings.localized("Turns this entry off. It can’t be turned back on while Hardcore is active."))
                }
            } else {
                Toggle(
                    isOn: Binding(
                        get: { isOn },
                        set: { _ in store.toggle(entry) }
                    )
                ) {
                    HStack(spacing: 6) {
                        Text(entry.displayTitle)
                            .font(.body)
                            .fixedSize(horizontal: false, vertical: true)
                        if PatchStore.hardcoreAllowsPatch(entry) {
                            Image(systemName: "checkmark.shield.fill")
                                .font(.caption2)
                                .foregroundStyle(.green)
                                .accessibilityLabel(settings.localized("Hardcore-safe"))
                        }
                    }
                }
                .accessibilityHint(settings.localized("Enables or disables this entry for the current game"))
                .disabled(!isOn && !PatchStore.hardcorePermitsEnable(entry))
            }

            if !entry.summary.isEmpty {
                Text(entry.summary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 8) {
                Text(settings.localized(entry.displayCategory.title))
                    .font(.caption2.weight(.medium))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.secondary.opacity(0.15), in: Capsule())
                Text(entry.sourceDisplayName)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                if entry.isLegacy {
                    Text(settings.localized("Legacy"))
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
                Spacer(minLength: 4)
            }
        }
        .padding(.vertical, 3)
    }

    private func controllerFocus(_ target: ControllerTarget) -> CheatsControllerFocusModifier {
        cheatsControllerFocus(target, controllerInput: controllerInput, controllerTarget: controllerTarget)
    }
}
