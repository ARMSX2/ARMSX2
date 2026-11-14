// CheatsPatchesManagerView.swift — Per-game Cheats & Patches manager
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

enum InstalledFileRemoval {
    case patch
    case cheat
    case all
}

struct CheatsPatchesManagerView: View {

    enum ControllerTarget: Hashable {
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
    let onClose: (() -> Void)?
    let session: CheatsPatchesSession?

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
    @State private var instance = UUID()
    @Environment(\.dismiss) private var dismiss

    init(
        isoName: String,
        gameTitle: String,
        launchContext: CheatsPatchesLaunchContext = .library,
        controllerInput: MenuControllerInputRouter? = nil,
        onClose: (() -> Void)? = nil,
        session: CheatsPatchesSession? = nil
    ) {
        self.isoName = isoName
        self.gameTitle = gameTitle
        self.launchContext = launchContext
        self.controllerInput = controllerInput
        self.onClose = onClose
        self.session = session
    }

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                Form {
                    gameSection
                    sections
                }
                .scrollContentBackground(.hidden)
                .background(Color.clear)
                .cheatsScrollMemory(session, instance: instance)
                .onChange(of: controllerTarget) { _, target in
                    if session?.shownBy == instance { session?.controllerTarget = target }
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
                if let session {
                    // A restored controller target scrolls itself into view below.
                    let restoresOffset = session.shownBy != nil && session.controllerTarget == .done
                    session.restoreShare = restoresOffset ? session.scrollShare : nil
                    session.shownBy = instance
                }
                reload()
                patchSourcesDraft = session?.patchSourcesDraft ?? store.patchDatabaseURLTemplates
                cheatSourcesDraft = session?.cheatSourcesDraft ?? store.cheatDatabaseURLTemplates
                if let session {
                    showAdvanced = session.showAdvanced
                    controllerTarget = session.controllerTarget
                }
                controllerInput?.setNavigationCaptured(
                    true,
                    owner: MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                    priority: 500
                )
            }
            .onDisappear {
                // A pose change can show the next view before this one leaves, and that one owns the capture.
                guard session?.shownBy ?? instance == instance else { return }
                // Unfolding can drop the hosted view before the sheet shows, so GameScreenView releases the store on close.
                if session == nil || !AppState.shared.hingePresent {
                    downloadTask?.cancel()
                    downloadTask = nil
                    // PatchStore's parsed rows, messages and game identity are presentation-only.
                    store.releasePresentationResources()
                }
                controllerInput?.setNavigationCaptured(
                    false,
                    owner: MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                    priority: 500
                )
            }
            .onChange(of: patchSourcesDraft) { _, drafts in session?.patchSourcesDraft = drafts }
            .onChange(of: cheatSourcesDraft) { _, drafts in session?.cheatSourcesDraft = drafts }
            .onChange(of: showAdvanced) { _, shown in session?.showAdvanced = shown }
            .onChange(of: controllerInput?.latestEvent) { _, event in
                guard let event else { return }
                handleControllerCommand(event)
            }
            // A touch drops the capture, so take it back when the pad returns.
            .onChange(of: controllerInput?.isControllerNavigationEnabled) { _, enabled in
                guard enabled == true else { return }
                controllerInput?.setNavigationCaptured(
                    true,
                    owner: MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                    priority: 500
                )
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
                    .background {
                        if let session { CheatsScrollRestore(session: session) }
                    }
                    // A row of its own for this marker left an empty band above the title.
                    .background {
                        ControllerRightStickScrollTarget(
                            controllerInput: controllerInput,
                            axes: .vertical,
                            manualCaptureOwner:
                                MenuControllerNavigationCaptureOwner.cheatsPatchesManager,
                            priority: 90
                        )
                        .frame(height: 0)
                    }
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

    @ViewBuilder
    private var sections: some View {
        if capabilityMessage != nil { capabilitySection }
        if store.showMessage, store.lastMessage != nil { feedbackSection }
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
            // This screen used to claim everything was blocked the moment Hardcore was switched on.
            // It is not: Hardcore arms on a boot, and until then the entries below carry on working.
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
        CheatsInstalledSection(
            controllerInput: controllerInput,
            controllerTarget: controllerTarget,
            pendingRemoval: $pendingRemoval,
            pendingEntryRemoval: $pendingEntryRemoval
        )
        CheatsAvailableSection(
            controllerInput: controllerInput,
            controllerTarget: controllerTarget,
            onDownload: { asCheat in startDatabaseDownload(asCheat: asCheat) }
        )
        importSection
        CheatsSourceURLsSection(
            controllerInput: controllerInput,
            controllerTarget: controllerTarget,
            showAdvanced: $showAdvanced,
            patchSourcesDraft: $patchSourcesDraft,
            cheatSourcesDraft: $cheatSourcesDraft
        )
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

    private func controllerFocus(
        _ target: ControllerTarget
    ) -> CheatsControllerFocusModifier {
        cheatsControllerFocus(target, controllerInput: controllerInput, controllerTarget: controllerTarget)
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
        // The sheet still hears the pad while a fold swaps it for the hosted panel.
        guard controllerInput != nil,
              (session?.shownBy ?? instance) == instance,
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
                close()
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
        close()
    }

    private func close() {
        if let onClose { onClose() } else { dismiss() }
    }
}
