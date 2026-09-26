// CheatsPatchesManagerView.swift — Per-game Cheats & Patches manager
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

private struct CheatsControllerFocusModifier: ViewModifier {
    let targetID: String
    let isFocused: Bool
    let cornerRadius: CGFloat

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.uiAccentColour) private var accentColour

    func body(content: Content) -> some View {
        content
            .focusEffectDisabled()
            .padding(.horizontal, 7)
            .foregroundStyle(isFocused ? accentColour : Color.primary)
            .background {
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(isFocused ? Color.black.opacity(0.2) : .clear)
            }
            .controllerFocusBoxPresentation(
                isVisible: isFocused,
                cornerRadius: cornerRadius,
                performanceOptimized: true
            )
            .controllerNavigationOrbTarget(
                id: "cheats.\(targetID)",
                isActive: isFocused,
                palette: .blue,
                style: .plain,
                inset: 2,
                orbScale: 0.7,
                priority: 10
            )
            .scaleEffect(isFocused && !reduceMotion ? 1.01 : 1)
            .shadow(
                color: .black.opacity(isFocused ? 0.1 : 0),
                radius: isFocused ? 4 : 0,
                y: isFocused ? 2 : 0
            )
            .animation(
                reduceMotion ? .linear(duration: 0.1) : .smooth(duration: 0.2),
                value: isFocused
            )
    }
}

private enum InstalledFileRemoval {
    case patch
    case cheat
    case all
}

struct CheatsPatchesManagerView: View {
    private enum ControllerTarget: Hashable {
        case done
        case retryIdentity
        case dismissFeedback
        case installed(String)
        case enableAll
        case disableAll
        case removeInstalled
        case downloadPatches
        case downloadCheats
        case importType
        case importFile
        case advanced
        case addPatchSource
        case addCheatSource
        case saveSources
    }

    let isoName: String
    let gameTitle: String
    let launchContext: CheatsPatchesLaunchContext
    let controllerInput: MenuControllerInputRouter?

    @State private var settings = SettingsStore.shared
    @State private var store = PatchStore.shared
    @State private var showImportPicker = false
    @State private var importAsCheat = false
    @State private var patchSourcesDraft: [String] = []
    @State private var cheatSourcesDraft: [String] = []
    @State private var pendingRemoval: InstalledFileRemoval?
    @State private var pendingEntryRemoval: PatchEntry?
    @State private var showAdvanced = false
    @State private var downloadTask: Task<Void, Never>?
    @State private var controllerTarget: ControllerTarget = .done
    @State private var controllerConfirmationIndex = 0
    @Environment(\.dismiss) private var dismiss

    init(
        isoName: String,
        gameTitle: String,
        launchContext: CheatsPatchesLaunchContext = .library,
        controllerInput: MenuControllerInputRouter? = nil
    ) {
        self.isoName = isoName
        self.gameTitle = gameTitle
        self.launchContext = launchContext
        self.controllerInput = controllerInput
    }

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                Form {
                    ControllerRightStickScrollTarget(
                        controllerInput: controllerInput,
                        axes: .vertical,
                        manualCaptureOwner:
                            MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                        priority: 90
                    )
                    .frame(height: 0)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
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
                .scrollContentBackground(.hidden)
                .background(Color.clear)
                .onChange(of: controllerTarget) { _, target in
                    withAnimation(.snappy(duration: 0.22)) {
                        proxy.scrollTo(target, anchor: .center)
                    }
                }
            }
            .navigationTitle(settings.localized("Cheats & Patches"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button { dismissWithReturnSound() } label: {
                        Text(settings.localized("Done"))
                            .modifier(
                                CheatsControllerFocusModifier(
                                    targetID: "done",
                                    isFocused:
                                        controllerInput?.isControllerNavigationEnabled == true
                                            && controllerTarget == .done,
                                    cornerRadius: 14
                                )
                            )
                    }
                }
            }
            .onAppear {
                reload()
                patchSourcesDraft = store.patchDatabaseURLTemplates
                cheatSourcesDraft = store.cheatDatabaseURLTemplates
                controllerInput?.setNavigationCaptured(
                    true,
                    owner: MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                    priority: 500
                )
            }
            .onDisappear {
                downloadTask?.cancel()
                downloadTask = nil
                // PatchStore is shared for persistence, but its parsed rows,
                // messages, and current-game identity are presentation-only.
                // Discard them when this manager leaves the hierarchy so the
                // Quick Menu does not keep them resident during gameplay.
                store.releasePresentationResources()
                controllerInput?.setNavigationCaptured(
                    false,
                    owner: MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                    priority: 500
                )
            }
            .onChange(of: controllerInput?.latestEvent) { _, event in
                guard let event else { return }
                handleControllerCommand(event)
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
                isPresented: Binding(
                    get: {
                        pendingRemoval != nil
                            && controllerInput?.hasConnectedController != true
                    },
                    set: { presented in
                        if !presented,
                           controllerInput?.hasConnectedController != true {
                            pendingRemoval = nil
                        }
                    }
                ),
                titleVisibility: .visible
            ) {
                Button(removalActionTitle, role: .destructive) { performPendingRemoval() }
                Button(settings.localized("Cancel"), role: .cancel) { pendingRemoval = nil }
            } message: {
                Text(removalMessage)
            }
            .confirmationDialog(
                settings.localized("Remove this entry?"),
                isPresented: Binding(
                    get: {
                        pendingEntryRemoval != nil
                            && controllerInput?.hasConnectedController != true
                    },
                    set: { presented in
                        if !presented,
                           controllerInput?.hasConnectedController != true {
                            pendingEntryRemoval = nil
                        }
                    }
                ),
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
            .accessibilityHidden(controllerConfirmationActive)
        }
        .background(Color.clear)
        .glassSurface(clear: false, cornerRadius: 26)
        .controllerNavigationOrbOverlay(controllerInput: controllerInput)
        .overlay {
            if controllerConfirmationActive {
                ControllerNavigationAlert(
                    title: controllerConfirmationTitle,
                    dimsBackground: launchContext != .library,
                    message: controllerConfirmationMessage,
                    actions: controllerConfirmationActions,
                    selectedIndex: controllerConfirmationIndex,
                    onSelect: performControllerConfirmationAction,
                    onDismiss: dismissControllerConfirmation
                )
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
                Button { reload() } label: {
                    Text(settings.localized("Retry Game Information"))
                        .modifier(controllerFocus(.retryIdentity))
                }
                .id(ControllerTarget.retryIdentity)
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
                        .modifier(controllerFocus(.dismissFeedback))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(settings.localized("Dismiss message"))
                .id(ControllerTarget.dismissFeedback)
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
                    .modifier(controllerFocus(.downloadPatches))
                }
                .disabled(!store.identityState.canUseDatabase || !store.canManageInstalledFiles || store.isDownloading)
                .accessibilityHint(settings.localized(store.identityState.canUseDatabase ? "Downloads matching patches from every configured source" : "Requires an identified game CRC"))
                .id(ControllerTarget.downloadPatches)
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
                    .modifier(controllerFocus(.downloadCheats))
                }
                .disabled(!store.identityState.canUseDatabase || !store.canManageInstalledFiles || store.isDownloading)
                .id(ControllerTarget.downloadCheats)
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
            .modifier(controllerFocus(.importType))
            .id(ControllerTarget.importType)

            Button {
                showImportPicker = true
            } label: {
                Label(settings.localized("Import File"), systemImage: "square.and.arrow.down")
                    .modifier(controllerFocus(.importFile))
            }
            .disabled(!store.canManageInstalledFiles)
            .id(ControllerTarget.importFile)
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

    private func controllerFocus(
        _ target: ControllerTarget
    ) -> CheatsControllerFocusModifier {
        CheatsControllerFocusModifier(
            targetID: String(describing: target),
            isFocused:
                controllerInput?.isControllerNavigationEnabled == true
                    && controllerTarget == target,
            cornerRadius: 12
        )
    }

    private var controllerConfirmationActive: Bool {
        controllerInput?.hasConnectedController == true
            && (pendingRemoval != nil || pendingEntryRemoval != nil)
    }

    private var controllerConfirmationTitle: String {
        pendingEntryRemoval != nil ? "Remove this entry?" : removalTitle
    }

    private var controllerConfirmationMessage: String {
        pendingEntryRemoval != nil
            ? "This removes only this entry from its file. All other entries are kept."
            : removalMessage
    }

    private var controllerConfirmationActions:
        [ControllerNavigationAlertAction] {
        [
            .init(id: "cancel", title: "Cancel"),
            .init(
                id: "remove",
                title: pendingEntryRemoval != nil
                    ? "Remove Entry"
                    : removalActionTitle,
                isDestructive: true
            ),
        ]
    }

    private func dismissControllerConfirmation() {
        pendingRemoval = nil
        pendingEntryRemoval = nil
        controllerConfirmationIndex = 0
    }

    private func performControllerConfirmationAction(_ index: Int) {
        guard index == 1 else {
            dismissControllerConfirmation()
            return
        }
        if let entry = pendingEntryRemoval {
            store.removeEntry(entry)
            pendingEntryRemoval = nil
        } else {
            performPendingRemoval()
        }
        controllerConfirmationIndex = 0
    }

    private func handleControllerConfirmationCommand(
        _ command: MenuControllerCommand
    ) {
        switch command {
        case .up, .upLeft, .left, .downLeft:
            controllerConfirmationIndex = 0
            controllerInput?.playFeedback(.move(command))
        case .upRight, .right, .downRight, .down:
            controllerConfirmationIndex = 1
            controllerInput?.playFeedback(.move(command))
        case .activate:
            performControllerConfirmationAction(controllerConfirmationIndex)
            controllerInput?.playFeedback(.activate)
        case .back:
            dismissControllerConfirmation()
            controllerInput?.playFeedback(.back)
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            controllerInput?.playFeedback(.boundary)
        }
    }

    private var controllerTargets: [ControllerTarget] {
        var targets: [ControllerTarget] = [.done]
        if capabilityMessage != nil, launchContext == .inGame {
            targets.append(.retryIdentity)
        }
        if store.showMessage, store.lastMessage != nil {
            targets.append(.dismissFeedback)
        }
        targets.append(
            contentsOf: store.installed.map { .installed($0.id) }
        )
        if hasNamedEntries {
            targets.append(contentsOf: [.enableAll, .disableAll])
        }
        if !store.installed.isEmpty {
            targets.append(.removeInstalled)
        }
        if store.hasConfiguredPatchDatabase {
            targets.append(.downloadPatches)
        }
        if store.hasConfiguredCheatDatabase {
            targets.append(.downloadCheats)
        }
        targets.append(contentsOf: [.importType, .importFile, .advanced])
        if showAdvanced {
            targets.append(
                contentsOf: [.addPatchSource, .addCheatSource, .saveSources]
            )
        }
        return targets
    }

    private func handleControllerCommand(_ event: MenuControllerInputEvent) {
        guard controllerInput != nil,
              event.captureOwner
                == MenuControllerNavigationCaptureOwner.cheatsPatchesManager else {
            return
        }
        let command = event.command
        if showImportPicker {
            if command == .back {
                showImportPicker = false
                controllerInput?.playFeedback(.back)
            }
            return
        }
        if controllerConfirmationActive {
            handleControllerConfirmationCommand(command)
            return
        }
        let targets = controllerTargets
        guard !targets.isEmpty else { return }
        let index = targets.firstIndex(of: controllerTarget) ?? 0
        if !targets.contains(controllerTarget) {
            controllerTarget = targets[index]
        }

        switch command {
        case .up, .upLeft, .upRight:
            let next = max(targets.startIndex, index - 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerTarget = targets[next]
            controllerInput?.playFeedback(.move(.up))
        case .down, .downLeft, .downRight:
            let next = min(targets.index(before: targets.endIndex), index + 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerTarget = targets[next]
            controllerInput?.playFeedback(.move(.down))
        case .left, .right:
            guard controllerTarget == .importType else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            importAsCheat = command == .right
            controllerInput?.playFeedback(.move(command))
        case .activate:
            activateControllerTarget()
        case .back:
            if showAdvanced {
                showAdvanced = false
                controllerTarget = .advanced
            } else {
                dismiss()
            }
            controllerInput?.playFeedback(.back)
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            break
        }
    }

    private func activateControllerTarget() {
        switch controllerTarget {
        case .done:
            dismissWithReturnSound()
        case .retryIdentity:
            reload()
        case .dismissFeedback:
            store.dismissMessage()
        case .installed(let id):
            guard let entry = store.installed.first(where: { $0.id == id }),
                  !entry.isLegacy else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            store.toggle(entry)
        case .enableAll:
            guard store.canEnableAll else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            store.setAllNamedEntries(enabled: true)
        case .disableAll:
            guard store.canDisableAll else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            store.setAllNamedEntries(enabled: false)
        case .removeInstalled:
            if patchEntryCount > 0, cheatEntryCount > 0 {
                pendingRemoval = .all
            } else if patchEntryCount > 0 {
                pendingRemoval = .patch
            } else if cheatEntryCount > 0 {
                pendingRemoval = .cheat
            } else {
                controllerInput?.playFeedback(.boundary)
                return
            }
        case .downloadPatches:
            guard store.identityState.canUseDatabase,
                  store.canManageInstalledFiles,
                  !store.isDownloading else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            store.dismissMessage()
            startDatabaseDownload(asCheat: false)
        case .downloadCheats:
            guard store.identityState.canUseDatabase,
                  store.canManageInstalledFiles,
                  !store.isDownloading else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            store.dismissMessage()
            startDatabaseDownload(asCheat: true)
        case .importType:
            importAsCheat.toggle()
        case .importFile:
            guard store.canManageInstalledFiles else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            showImportPicker = true
        case .advanced:
            showAdvanced.toggle()
        case .addPatchSource:
            patchSourcesDraft.append("")
        case .addCheatSource:
            cheatSourcesDraft.append("")
        case .saveSources:
            store.patchDatabaseURLTemplates = patchSourcesDraft
            store.cheatDatabaseURLTemplates = cheatSourcesDraft
            patchSourcesDraft = store.patchDatabaseURLTemplates
            cheatSourcesDraft = store.cheatDatabaseURLTemplates
            store.applyFeedback("Source URLs saved.", kind: .success)
        }
        controllerInput?.playFeedback(.activate)
    }

    private func dismissWithReturnSound() {
        MenuAudioPackManager.shared.playEvent(.return)
        dismiss()
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
}
