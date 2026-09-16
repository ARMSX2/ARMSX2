// CheatsPatchesManagerView.swift — Per-game Cheats & Patches manager
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

private enum InstalledFileRemoval {
    case patch
    case cheat
    case all
}

struct CheatsPatchesManagerView: View {
    let isoName: String
    let gameTitle: String
    let launchContext: CheatsPatchesLaunchContext

    @ObservedObject private var settings = SettingsStore.shared
    @State private var store = PatchStore.shared
    @State private var showImportPicker = false
    @State private var importAsCheat = false
    @State private var patchSourcesDraft: [String] = []
    @State private var cheatSourcesDraft: [String] = []
    @State private var pendingRemoval: InstalledFileRemoval?
    @State private var pendingEntryRemoval: PatchEntry?
    @State private var showAdvanced = false
    @State private var downloadTask: Task<Void, Never>?
    @Environment(\.dismiss) private var dismiss

    init(
        isoName: String,
        gameTitle: String,
        launchContext: CheatsPatchesLaunchContext = .library
    ) {
        self.isoName = isoName
        self.gameTitle = gameTitle
        self.launchContext = launchContext
    }

    var body: some View {
        NavigationStack {
            Form {
                gameSection
                if capabilityMessage != nil {
                    capabilitySection
                }
                if store.showMessage, store.lastMessage != nil {
                    feedbackSection
                }
                if PatchStore.hardcoreBlocksPnachContent() {
                    Section {
                        Label {
                            Text(settings.localized("Hardcore Mode is on. Cheats and most patches are blocked, but widescreen and 60fps patches from a trusted database can still be enabled."))
                                .fixedSize(horizontal: false, vertical: true)
                        } icon: {
                            Image(systemName: "lock.fill")
                                .foregroundStyle(.orange)
                        }
                    }
                } else if PatchStore.hardcorePendingRestart() {
                    // This screen used to claim everything was blocked the moment Hardcore was
                    // switched on. It is not: Hardcore arms on a boot, and until then the
                    // entries below carry on working.
                    Section {
                        Label {
                            Text(settings.localized("Hardcore Mode is switched on but has not taken hold yet. Anything enabled here still works until you boot a game, and will stop then."))
                                .fixedSize(horizontal: false, vertical: true)
                        } icon: {
                            Image(systemName: "clock.badge.exclamationmark")
                                .foregroundStyle(.orange)
                        }
                    }
                }
                installedSection
                availableSection
                importSection
                advancedSection
            }
            .navigationTitle(settings.localized("Cheats & Patches"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(settings.localized("Done")) { dismiss() }
                }
            }
            .onAppear {
                reload()
                patchSourcesDraft = store.patchDatabaseURLTemplates
                cheatSourcesDraft = store.cheatDatabaseURLTemplates
            }
            .onDisappear {
                downloadTask?.cancel()
                downloadTask = nil
            }
            .sheet(isPresented: $showImportPicker) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.pnachContentTypes,
                    allowsMultipleSelection: true
                ) { result in
                    showImportPicker = false
                    switch result {
                    case .success(let urls):
                        _ = PatchStore.shared.importURLs(urls, forISO: isoName, asCheat: importAsCheat)
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            store.applyFeedback(
                                FileImportHandler.failedPNACHPickerMessage(errorDescription: error.localizedDescription),
                                kind: .error
                            )
                        }
                    }
                }
            }
            .confirmationDialog(
                removalTitle,
                isPresented: Binding(get: { pendingRemoval != nil }, set: { if !$0 { pendingRemoval = nil } }),
                titleVisibility: .visible
            ) {
                Button(removalActionTitle, role: .destructive) { performPendingRemoval() }
                Button(settings.localized("Cancel"), role: .cancel) { pendingRemoval = nil }
            } message: {
                Text(removalMessage)
            }
            .confirmationDialog(
                settings.localized("Remove this entry?"),
                isPresented: Binding(get: { pendingEntryRemoval != nil }, set: { if !$0 { pendingEntryRemoval = nil } }),
                titleVisibility: .visible
            ) {
                Button(settings.localized("Remove Entry"), role: .destructive) {
                    if let entry = pendingEntryRemoval {
                        store.removeEntry(entry)
                    }
                    pendingEntryRemoval = nil
                }
                Button(settings.localized("Cancel"), role: .cancel) { pendingEntryRemoval = nil }
            } message: {
                Text(settings.localized("This removes only this entry from its file. All other entries are kept."))
            }
        }
    }

    private func reload() {
        store.dismissMessage()
        store.loadInstalled(forISO: isoName, launchContext: launchContext)
    }

    @ViewBuilder
    private var gameSection: some View {
        if !displayGameTitle.isEmpty {
            Section {
                Text(displayGameTitle)
                    .font(.headline)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
            }
        }
    }

    private var displayGameTitle: String {
        let source = gameTitle.isEmpty ? (isoName as NSString).lastPathComponent : gameTitle
        return (source as NSString).deletingPathExtension
    }

    private var capabilityMessage: String? {
        if let guidance = store.identityState.guidance { return settings.localized(guidance) }
        if !store.canManageInstalledFiles {
            return settings.localized("Patch storage is not ready for this game. Try again in a moment.")
        }
        return nil
    }

    private var capabilitySection: some View {
        Section {
            Label {
                Text(capabilityMessage ?? "")
                    .fixedSize(horizontal: false, vertical: true)
            } icon: {
                Image(systemName: store.identityState == .inGameLoading ? "clock" : "info.circle")
                    .foregroundStyle(.secondary)
            }

            if launchContext == .inGame {
                Button(settings.localized("Retry Game Information")) { reload() }
            }
        } header: {
            Text(settings.localized("Game Identification"))
        }
    }

    private var feedbackSection: some View {
        Section {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: feedbackIcon)
                    .foregroundStyle(feedbackColor)
                    .accessibilityHidden(true)
                Text(store.lastMessage ?? "")
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 8)
                Button {
                    store.dismissMessage()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(settings.localized("Dismiss message"))
            }
        }
    }

    private var feedbackIcon: String {
        switch store.lastMessageKind {
        case .information: return "info.circle.fill"
        case .success: return "checkmark.circle.fill"
        case .error: return "exclamationmark.triangle.fill"
        }
    }

    private var feedbackColor: Color {
        switch store.lastMessageKind {
        case .information: return .secondary
        case .success: return .green
        case .error: return .red
        }
    }

    // MARK: - Installed

    @ViewBuilder
    private var installedSection: some View {
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
                        }
                        .disabled(!store.canEnableAll)

                        Button {
                            store.setAllNamedEntries(enabled: false)
                        } label: {
                            Label(settings.localized("Disable All"), systemImage: "circle.slash")
                        }
                        .disabled(!store.canDisableAll)
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
                }
                .accessibilityHint(settings.localized("Removes complete installed files, not individual entries"))
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

    private var removalTitle: String {
        switch pendingRemoval {
        case .patch: return settings.localized("Remove installed patch file?")
        case .cheat: return settings.localized("Remove installed cheat file?")
        case .all: return settings.localized("Remove all installed files?")
        case nil: return settings.localized("Remove installed files?")
        }
    }

    private var removalActionTitle: String {
        switch pendingRemoval {
        case .patch: return settings.localized("Remove Patch File")
        case .cheat: return settings.localized("Remove Cheat File")
        case .all: return settings.localized("Remove All Files")
        case nil: return settings.localized("Remove")
        }
    }

    private var removalMessage: String {
        switch pendingRemoval {
        case .patch:
            return String(format: settings.localized("This removes the complete patch file and %@ in it. This cannot be undone."), entryCountLabel(patchEntryCount))
        case .cheat:
            return String(format: settings.localized("This removes the complete cheat file and %@ in it. This cannot be undone."), entryCountLabel(cheatEntryCount))
        case .all:
            return String(format: settings.localized("This removes both installed files and %@ in them. This cannot be undone."), entryCountLabel(patchEntryCount + cheatEntryCount))
        case nil:
            return settings.localized("This cannot be undone.")
        }
    }

    private func performPendingRemoval() {
        switch pendingRemoval {
        case .patch:
            store.removeInstalledFile(asCheat: false)
        case .cheat:
            store.removeInstalledFile(asCheat: true)
        case .all:
            store.removeAllInstalled()
        case nil:
            break
        }
        pendingRemoval = nil
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

    // MARK: - Available

    private var availableSection: some View {
        Section {
            if store.hasConfiguredPatchDatabase {
                Button {
                    store.dismissMessage()
                    startDatabaseDownload(asCheat: false)
                } label: {
                    Label(
                        hasDatabasePatch ? settings.localized("Reinstall Patches") : settings.localized("Download Patches"),
                        systemImage: hasDatabasePatch ? "arrow.clockwise.icloud" : "icloud.and.arrow.down"
                    )
                }
                .disabled(!store.identityState.canUseDatabase || !store.canManageInstalledFiles || store.isDownloading)
                .accessibilityHint(settings.localized(store.identityState.canUseDatabase ? "Downloads matching patches from every configured source" : "Requires an identified game CRC"))
            } else {
                Text(settings.localized("No patch download source is configured. Add one in Advanced or import a file below."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if store.hasConfiguredCheatDatabase {
                Button {
                    store.dismissMessage()
                    startDatabaseDownload(asCheat: true)
                } label: {
                    Label(
                        hasDatabaseCheat ? settings.localized("Reinstall Cheats") : settings.localized("Download Cheats"),
                        systemImage: hasDatabaseCheat ? "arrow.clockwise.icloud" : "icloud.and.arrow.down"
                    )
                }
                .disabled(!store.identityState.canUseDatabase || !store.canManageInstalledFiles || store.isDownloading)
            }

            if store.isDownloading {
                HStack(spacing: 10) {
                    ProgressView()
                    Text(settings.localized("Downloading…"))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel(settings.localized("Download in progress"))
            }

        } header: {
            Text(settings.localized("Available"))
        } footer: {
            Text(settings.localized("Downloads query every configured source and merge results into one file after making a backup. Patches must match this game’s region and version."))
        }
    }

    private var hasDatabasePatch: Bool {
        store.installed.contains { $0.source == .database && !$0.isCheat }
    }

    private var hasDatabaseCheat: Bool {
        store.installed.contains { $0.source == .database && $0.isCheat }
    }

    private func startDatabaseDownload(asCheat: Bool) {
        downloadTask?.cancel()
        downloadTask = Task {
            await store.downloadFromDatabase(forISO: isoName, asCheat: asCheat)
        }
    }

    // MARK: - Import

    private var importSection: some View {
        Section {
            Picker(settings.localized("Import As"), selection: $importAsCheat) {
                Text(settings.localized("Patch")).tag(false)
                Text(settings.localized("Cheat")).tag(true)
            }
            .pickerStyle(.segmented)

            Button {
                showImportPicker = true
            } label: {
                Label(settings.localized("Import File"), systemImage: "square.and.arrow.down")
            }
            .disabled(!store.canManageInstalledFiles)
        } header: {
            Text(settings.localized("Import"))
        } footer: {
            Text(settings.localized("Importing merges with the current patch or cheat file after making a backup. Named entries can be enabled individually."))
        }
    }

    // MARK: - Advanced

    private var advancedSection: some View {
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
                }
                .buttonStyle(.borderless)

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
                }
                .buttonStyle(.borderless)

                Button(settings.localized("Save Source URLs")) {
                    store.patchDatabaseURLTemplates = patchSourcesDraft
                    store.cheatDatabaseURLTemplates = cheatSourcesDraft
                    patchSourcesDraft = store.patchDatabaseURLTemplates
                    cheatSourcesDraft = store.cheatDatabaseURLTemplates
                    store.applyFeedback(settings.localized("Source URLs saved."), kind: .success)
                }
                .buttonStyle(.borderless)
                .padding(.top, 8)

                Text(settings.localized("Supported placeholders: \u{24}{serial}, \u{24}{crc}, and \u{24}{title}. Built-in sources provide PCSX2 patches, an UltraWidescreen / NaturalVision pack, and a community cheat collection. Only add sources you trust."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } label: {
                Label(settings.localized("Source URLs"), systemImage: "link")
                    .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
            }
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
                    .foregroundStyle(.red)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(settings.localized("Remove source"))
        }
    }
}
