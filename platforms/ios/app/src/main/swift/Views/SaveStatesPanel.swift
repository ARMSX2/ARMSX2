// SaveStatesPanel.swift — Pause → Save States, built from the pause card's own pieces.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Ten fixed rows with Save and Load on each. The pad moves by row and keeps its column.
struct SaveStatesPanel: View {
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize

    let settings: SettingsStore
    let undo: SaveStateUndoModel
    let variant: PauseLayoutVariant
    let landscape: Bool
    let gameTitle: String?
    let statusHandler: (String, Bool) -> Void
    let onLoaded: () -> Void
    let onClose: () -> Void
    let onOpenControllerMacros: () -> Void

    @State private var files: [SaveStateFile] = []
    @State private var metadataRevision = 0
    @State private var busySlot: Int?
    @State private var hardcore = false
    @State private var menuSlot: Int?
    @State private var renameSlot: Int?
    @State private var renameText = ""
    @State private var focusOverride: String?
    @State private var keyboardRequest: SaveStateKeyboardRequest?
    @State private var autoSaveOpen = false

    static let scopeKey = "runtime.save-states"
    private static let menuScope = "runtime.save-states.menu"
    private static let renameScope = "runtime.save-states.rename"
    private static let autoSaveScope = "runtime.save-states.autosave"
    static func saveID(_ slot: Int) -> String { "save-state.slot.\(slot).save" }
    static func loadID(_ slot: Int) -> String { "save-state.slot.\(slot).load" }
    static func moreID(_ slot: Int) -> String { "save-state.slot.\(slot).more" }
    private static let renameName = "save-state.rename.name"
    private static let renameClear = "save-state.rename.clear"
    private static let renameCancel = "save-state.rename.cancel"
    private static let renameConfirm = "save-state.rename.confirm"
    private static let autoSaveEnabled = "save-state.autosave.enabled"
    private static let autoSaveInterval = "save-state.autosave.interval"
    private static let autoSaveOnLeave = "save-state.autosave.leave"
    private static let autoSaveOnLowBattery = "save-state.autosave.battery"
    private static let autoSaveDone = "save-state.autosave.done"

    private var overlayOpen: Bool { menuSlot != nil || renameSlot != nil || autoSaveOpen }

    private var currentScope: String {
        if autoSaveOpen { return Self.autoSaveScope }
        return renameSlot != nil ? Self.renameScope : menuSlot != nil ? Self.menuScope : Self.scopeKey
    }

    private var currentGraph: SaveStateGraph {
        if autoSaveOpen {
            let ids = settings.autoSaveEnabled
                ? [Self.autoSaveEnabled, Self.autoSaveInterval, Self.autoSaveOnLeave, Self.autoSaveOnLowBattery, Self.autoSaveDone]
                : [Self.autoSaveEnabled, Self.autoSaveDone]
            return SaveStateGraph(rows: ids.map { [.init(id: $0, column: 0)] }, wraps: false)
        }
        if renameSlot != nil {
            let ids = [Self.renameName, Self.renameClear, Self.renameCancel, Self.renameConfirm]
            return SaveStateGraph(rows: [ids.enumerated().map { .init(id: $1, column: $0) }], wraps: false)
        }
        if let row = row(menuSlot) {
            let items = menuItems(for: row).filter(\.isEnabled)
            return SaveStateGraph(rows: items.map { [.init(id: $0.id, column: 0)] }, wraps: false)
        }
        return SaveStateGraph(rows: targetRows)
    }

    private func row(_ slot: Int?) -> SaveStateSlot? {
        slot.flatMap { slot in rows.first { $0.slot == slot } }
    }

    private var rows: [SaveStateSlot] {
        _ = metadataRevision
        return SaveStateSlot.rows(from: files) { SaveStateMetadataStore.shared.metadata(for: $0) }
    }

    private var layout: SaveStateRowLayout {
        if dynamicTypeSize.isAccessibilitySize { return .twoLine }
        switch variant {
        case .phoneLandscape: return .line
        case .phonePortrait: return .twoLine
        case .ipadTwoColumn: return landscape ? .wideLine : .stacked
        }
    }

    var body: some View {
        let graph = currentGraph
        Group {
            if layout == .line || layout == .wideLine {
                OverlayPanelScaffold {
                    VStack(spacing: 0) {
                        LandscapeCommandBar(
                            settings: settings,
                            gameTitle: gameTitle,
                            stopControllerNavigationID: "",
                            resumeControllerNavigationID: nil,
                            onStop: {},
                            onResume: onClose,
                            iconOnly: false,
                            systemImage: "square.stack.3d.up",
                            title: settings.localized("Save States"),
                            showsStopButton: false,
                            resumeTitle: settings.localized("Back"),
                            resumeImage: "chevron.left",
                            hint: squareHint
                        )
                        content
                    }
                }
                .environment(\.overlayCompact, true)
            } else {
                OverlayPanelScaffold {
                    VStack(spacing: 0) {
                        OverlayHeader(
                            systemImage: "square.stack.3d.up",
                            title: settings.localized("Save States"),
                            subtitle: gameTitle,
                            compact: variant != .phonePortrait
                        )
                        content
                        QuickMenuFooter(
                            settings: settings,
                            compact: variant != .phonePortrait,
                            stopControllerNavigationID: "",
                            resumeControllerNavigationID: nil,
                            onStop: {},
                            onResume: onClose,
                            showsStopButton: false,
                            resumeTitle: settings.localized("Back"),
                            resumeImage: "chevron.left",
                            hint: squareHint
                        )
                    }
                }
            }
        }
        // The list sits under the row menu and the rename card: out of the pad's reach and VoiceOver's.
        .environment(\.controllerAccessibilityTargetsSuppressed, overlayOpen)
        .accessibilityHidden(overlayOpen)
        .overlayPreferenceValue(SaveStateMoreAnchorKey.self) { anchors in
            menuOverlay(anchors)
        }
        .overlay { renameOverlay }
        .overlay { autoSaveOverlay }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: currentScope,
            priority: 320,
            orbStyle: .liquidGlass,
            onBack: {
                if autoSaveOpen {
                    closeAutoSave()
                } else if renameSlot != nil {
                    closeRename()
                } else if menuSlot != nil {
                    closeMenu()
                } else {
                    onClose()
                }
                return true
            },
            onContextMenu: { id in
                if menuSlot != nil {
                    closeMenu()
                    return true
                }
                guard renameSlot == nil, !autoSaveOpen, let row = row(Self.slot(inTargetID: id)), row.hasMore else {
                    return false
                }
                openMenu(row)
                return true
            },
            boundaryRules: graph.freshPressRules,
            directionalLinks: graph.links,
            prioritizesDirectionalLinks: true,
            confinesHorizontalFocusMovement: true,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            // Room for the card header, and in two-line rows for the title above the buttons.
            focusTopAlignmentMargin: layout == .twoLine ? 96 : 40,
            focusBottomAlignmentMargin: 10,
            preferredInitialFocusLabel: focusOverride ?? (overlayOpen ? nil : targetRows.first?.first?.id),
            declaredTargetOrder: graph.order
        )
        // The keyboard has its own text bar; the Rename card under it only got in the way.
        .opacity(keyboardRequest == nil ? 1 : 0)
        .fullScreenCover(item: $keyboardRequest) { _ in
            OrbitKeysKeyboardView(
                title: settings.localized("Rename"),
                initialText: renameText,
                startsInNormalKeyboard: true,
                onCommit: { text in
                    renameText = SaveStateSlot.cleanName(text)
                    keyboardRequest = nil
                    place(Self.renameConfirm, inScope: Self.renameScope, entering: true)
                },
                onCancel: { keyboardRequest = nil }
            )
            .presentationBackground(.clear)
            .appStatusBarHidden()
        }
        .onChange(of: landscape) { _, _ in
            // The menu is anchored to a row that a new layout may have scrolled away.
            if menuSlot != nil { closeMenu() }
        }
        .task(id: files.isEmpty) {
            // Registering is passive, as in the pause menu, so ask to enter once the rows exist.
            guard !files.isEmpty else { return }
            await Task.yield()
            _ = controllerInput?.requestNavigationSessionEntry(
                preferLast: false,
                matchingScopePrefix: Self.scopeKey
            )
        }
        .task { await refresh() }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2iOSRuntimeMenuStateChanged"))) { _ in
            Task { await refresh() }
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
            Task { await refresh() }
        }
        .environment(\.controllerTextAppearance, settings.controllerQuickMenuTextAppearance)
        .preferredColorScheme(.dark)
        .quickMenuLiquidGlassConfiguration()
    }

    @ViewBuilder
    private var content: some View {
        if hardcore {
            Label(
                settings.localized("Hardcore mode: loading save states is off. Saving still works."),
                systemImage: "trophy"
            )
            .font(.footnote.weight(.semibold))
            .foregroundStyle(OverlayTheme.warm)
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(OverlayTheme.warm.opacity(0.14), in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(OverlayTheme.warm.opacity(0.35)))
            .padding(.horizontal, 14)
            .padding(.top, 8)
        }
        if files.isEmpty {
            VStack(spacing: 6) {
                Text(settings.localized("Save states are not ready yet."))
                    .font(.headline)
                Text(settings.localized("Wait until the game has fully identified, then try again."))
                    .font(.caption)
                    .foregroundStyle(OverlayTheme.textSecondary)
            }
            .multilineTextAlignment(.center)
            .foregroundStyle(OverlayTheme.textPrimary)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(24)
        } else {
            list
        }
    }

    private var list: some View {
        let current = rows
        let latest = SaveStateSlot.latest(in: current)
        return ScrollView {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 320,
                isEnabled: !overlayOpen,
                pointsPerSecond: 680
            )
            .frame(height: 0)
            VStack(spacing: 12) {
                card(
                    title: settings.localized("Your saves"),
                    detail: settings.localized("Slots 1–8"),
                    rows: current.filter { $0.kind == .manual || $0.kind == .older },
                    latest: latest
                )
                card(
                    title: settings.localized("Auto and quick"),
                    detail: nil,
                    rows: current.filter { $0.kind == .auto || $0.kind == .quick },
                    latest: latest
                )
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
        }
        // An inset, not an overlay: the focus corridor leaves it out, so the focused row stays clear.
        .safeAreaInset(edge: .bottom) {
            if undo.item != nil {
                SaveStateUndoToast(undo: undo, settings: settings, hint: undoHint) { undoLast() }
                    .padding(.horizontal, 14)
                    .padding(.bottom, 10)
            }
        }
        .mask {
            VStack(spacing: 0) {
                LinearGradient(colors: [.clear, .black], startPoint: .top, endPoint: .bottom)
                    .frame(height: 20)
                Color.black
                LinearGradient(colors: [.black, .clear], startPoint: .top, endPoint: .bottom)
                    .frame(height: 18)
            }
        }
    }

    private func card(title: String, detail: String?, rows: [SaveStateSlot], latest: Int?) -> some View {
        OverlaySectionCard {
            HStack {
                Text(title)
                    .textCase(.uppercase)
                Spacer()
                if let detail { Text(detail) }
            }
            .font(.caption2)
            .foregroundStyle(OverlayTheme.textSecondary)
            .padding(.horizontal, 4)
            ForEach(rows) { row in
                SaveStateRowView(
                    row: row,
                    layout: layout,
                    settings: settings,
                    isLatest: row.slot == latest,
                    isBusy: busySlot == row.slot,
                    hardcore: hardcore,
                    primaryID: controllerInput?.hasConnectedController == true ? nil : suggestedSaveID,
                    quickSaveLine: quickSaveLine,
                    onSave: { save(row) },
                    onLoad: { load(row) },
                    onMore: { openMenu(row) }
                )
            }
        }
    }

    private var squareHint: ControllerHintLine? {
        controllerInput?.hasConnectedController == true
            ? ControllerHintLine(parts: [.init(.square, settings.localized("Rename, lock, delete"))]) : nil
    }

    private var undoHint: ControllerHintLine? {
        controllerInput?.hasConnectedController == true
            ? ControllerHintLine(parts: [.init(settings.controllerMacroUndoSaveState, settings.localized("Undo"))])
            : nil
    }

    /// The live macro bindings, shown only to controller players.
    private var quickSaveLine: ControllerHintLine? {
        guard controllerInput?.hasConnectedController == true else { return nil }
        let save = ControllerHintLine.Part(settings.controllerMacroSaveGameState, settings.localized("Save"))
        // Hardcore's banner already says loading is off.
        if hardcore { return ControllerHintLine(parts: [save]) }
        return ControllerHintLine(parts: [save, .init(settings.controllerMacroLoadGameState, settings.localized("Load"))])
    }

    private var targetRows: [[SaveStateGraph.Target]] {
        rows.map { row in
            var targets: [SaveStateGraph.Target] = []
            if row.hasSave, !row.isLocked { targets.append(.init(id: Self.saveID(row.slot), column: 0)) }
            if row.occupied, !hardcore { targets.append(.init(id: Self.loadID(row.slot), column: 1)) }
            if row.hasMore { targets.append(.init(id: Self.moreID(row.slot), column: 2)) }
            return targets
        }
        .filter { !$0.isEmpty }
    }

    /// Touch fills in Save on the first empty manual slot, else on Quick Save.
    private var suggestedSaveID: String? {
        let current = rows
        if let empty = current.first(where: { $0.kind == .manual && !$0.occupied }) {
            return Self.saveID(empty.slot)
        }
        return current.first(where: { $0.kind == .quick }).map { Self.saveID($0.slot) }
    }

    private func refresh() async {
        let current = await SaveStateFile.current()
        files = current
        hardcore = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
    }

    /// Saving over a state needs no confirmation: the older one stays one Undo away.
    private func save(_ row: SaveStateSlot) {
        guard busySlot == nil, !row.isLocked else { return }
        let slot = row.slot
        let title = row.title(settings)
        let previous = row.metadata
        let fileName = row.file.fileName
        undo.finishIfTouching(slot: slot)
        busySlot = slot
        ARMSX2Bridge.saveState(toSlot: slot) { success, backupToken in
            Task { @MainActor in
                let played = ARMSX2Bridge.currentGamePlayedSeconds()
                await refresh()
                guard success, let saved = files.first(where: { $0.slot == slot }) else {
                    busySlot = nil
                    statusHandler(
                        "\(String(format: settings.localized("Could not save to %@."), title)) \(settings.localized("Try again after gameplay has fully loaded."))",
                        true
                    )
                    return
                }
                SaveStateMetadataStore.shared.recordSave(of: saved, playedSeconds: played, fresh: backupToken == nil)
                SaveStateAutoSave.shared.didWrite()
                metadataRevision &+= 1
                busySlot = nil
                if let backupToken, let row = self.row(slot) {
                    showUndo(
                        .overwrite(slot: slot, backupToken: backupToken, fileName: fileName, previous: previous),
                        verb: settings.localized("Replaced"),
                        row: row,
                        undoLabel: "Undo save over %@"
                    )
                    // The toast takes room at the bottom; reveal the focused row above it.
                    place(Self.saveID(slot), inScope: Self.scopeKey, entering: true)
                } else {
                    statusHandler(String(format: settings.localized("Saved to %@"), title), false)
                }
            }
        }
    }

    private func showUndo(_ action: SaveStateUndoModel.Action, verb: String, row: SaveStateSlot, undoLabel: String) {
        let name = row.title(settings)
        // A renamed slot keeps its number beside the name; the default names already say which row it is.
        let caption = name == row.defaultTitle(settings)
            ? verb : String(format: settings.localized("%1$@ · Slot %2$d"), verb, row.slot)
        undo.show(
            .init(
                action: action,
                caption: caption,
                name: name,
                preview: row.file.preview,
                undoLabel: String(format: settings.localized(undoLabel), name)
            ),
            announcement: String(format: settings.localized("%1$@ %2$@. Undo is available."), caption, name)
        )
    }

    private func undoLast() {
        undo.undo { ok in
            if !ok { statusHandler(settings.localized("Could not undo."), true) }
        }
    }

    private func load(_ row: SaveStateSlot) {
        guard busySlot == nil, !hardcore else { return }
        let title = row.title(settings)
        busySlot = row.slot
        ARMSX2Bridge.loadState(
            fromSlot: row.slot,
            expectedModified: row.modifiedDate,
            keepingUndo: true
        ) { success, undoPath in
            Task { @MainActor in
                busySlot = nil
                if success {
                    SaveStateAutoSave.shared.restart()
                    if let undoPath {
                        showUndo(.load(path: undoPath), verb: settings.localized("Loaded"), row: row, undoLabel: "Undo load of %@")
                    }
                    onLoaded()
                } else {
                    await refresh()
                    statusHandler(
                        "\(String(format: settings.localized("Could not load %@."), title)) \(settings.localized("Make sure it has a saved state first."))",
                        true
                    )
                }
            }
        }
    }

    // MARK: Row menu and rename

    static func slot(inTargetID id: String) -> Int? {
        let parts = id.split(separator: ".")
        guard parts.count == 4, parts[0] == "save-state", parts[1] == "slot" else { return nil }
        return Int(parts[2])
    }

    /// Focus lands on `id` when `scope` next becomes the active one; `entering` asks for it now.
    private func place(_ id: String, inScope scope: String, entering: Bool = false) {
        controllerInput?.rememberNavigationFocusKey(
            ControllerAccessibilityNavigationSession.explicitKey(id),
            forScope: scope
        )
        focusOverride = id
        guard entering else { return }
        Task { @MainActor in
            await Task.yield()
            _ = controllerInput?.requestNavigationSessionEntry(preferLast: false, matchingScopePrefix: scope)
        }
    }

    private func menuItems(for row: SaveStateSlot) -> [SaveStateMenuItem] {
        switch row.kind {
        case .manual, .older:
            [
                .init(id: "save-state.menu.rename", title: settings.localized("Rename"), systemImage: "pencil") {
                    openRename(row)
                },
                .init(
                    id: "save-state.menu.lock",
                    title: settings.localized(row.isLocked ? "Unlock" : "Lock"),
                    systemImage: row.isLocked ? "lock.open" : "lock"
                ) {
                    SaveStateMetadataStore.shared.update(row.file) { $0.locked = row.isLocked ? nil : true }
                    metadataRevision &+= 1
                    closeMenu()
                },
                deleteItem(for: row),
            ]
        case .quick:
            [
                .init(
                    id: "save-state.menu.macros",
                    title: settings.localized("Controller Macros"),
                    systemImage: "gamecontroller"
                ) {
                    menuSlot = nil
                    onOpenControllerMacros()
                },
                deleteItem(for: row),
            ]
        case .auto:
            [
                .init(
                    id: "save-state.menu.autosave",
                    title: settings.localized("Auto-save Settings"),
                    systemImage: "gearshape"
                ) {
                    place(Self.autoSaveEnabled, inScope: Self.autoSaveScope)
                    menuSlot = nil
                    autoSaveOpen = true
                },
            ] + (row.occupied ? [deleteItem(for: row)] : [])
        }
    }

    /// A locked slot has to be unlocked before it can be deleted.
    private func deleteItem(for row: SaveStateSlot) -> SaveStateMenuItem {
        .init(
            id: "save-state.menu.delete",
            title: settings.localized(row.isLocked ? "Unlock to delete" : "Delete"),
            systemImage: "trash",
            isDestructive: true,
            isEnabled: !row.isLocked
        ) {
            delete(row)
        }
    }

    private func delete(_ row: SaveStateSlot) {
        let slot = row.slot
        let fileName = row.file.fileName
        // Focus stays on the row's Save; the Auto-save row has none, so Quick Save's takes it.
        let focusSlot = row.hasSave ? slot : rows.first { $0.kind == .quick }?.slot ?? slot
        menuSlot = nil
        place(Self.saveID(focusSlot), inScope: Self.scopeKey)
        undo.finishIfTouching(slot: slot)
        ARMSX2Bridge.deleteSaveState(inSlot: slot) { success in
            Task { @MainActor in
                await refresh()
                if success {
                    showUndo(.delete(slot: slot, fileName: fileName), verb: settings.localized("Deleted"), row: row, undoLabel: "Undo delete of %@")
                    // The toast takes room at the bottom; reveal the focused row above it.
                    place(Self.saveID(focusSlot), inScope: Self.scopeKey, entering: true)
                } else {
                    statusHandler(settings.localized("Could not delete this save."), true)
                }
            }
        }
    }

    private func openMenu(_ row: SaveStateSlot) {
        guard let first = menuItems(for: row).first(where: \.isEnabled) else { return }
        // The menu scope remembers its last item; every menu opens on its first one.
        place(first.id, inScope: Self.menuScope)
        menuSlot = row.slot
    }

    private func closeMenu() {
        guard let slot = menuSlot else { return }
        place(Self.moreID(slot), inScope: Self.scopeKey)
        menuSlot = nil
    }

    private func openRename(_ row: SaveStateSlot) {
        renameText = row.metadata?.name ?? ""
        place(Self.renameName, inScope: Self.renameScope)
        menuSlot = nil
        renameSlot = row.slot
    }

    private func closeRename() {
        guard let slot = renameSlot else { return }
        place(Self.moreID(slot), inScope: Self.scopeKey)
        renameSlot = nil
    }

    private func closeAutoSave() {
        place(Self.moreID(SaveStateSlot.autoSlot), inScope: Self.scopeKey)
        autoSaveOpen = false
    }

    private func commitRename() {
        guard let row = row(renameSlot) else { return }
        let name = SaveStateSlot.cleanName(renameText)
        SaveStateMetadataStore.shared.update(row.file) { $0.name = name.isEmpty ? nil : name }
        metadataRevision &+= 1
        closeRename()
    }

    @ViewBuilder
    private func menuOverlay(_ anchors: [Int: Anchor<CGRect>]) -> some View {
        if let row = row(menuSlot), let anchor = anchors[row.slot] {
            GeometryReader { proxy in
                let button = proxy[anchor]
                let items = menuItems(for: row)
                let width = min(240, proxy.size.width - 24)
                let height = 40 + CGFloat(items.count) * 44
                // Below the button when it fits in the card, above it otherwise.
                let below = button.maxY + 10 + height <= proxy.size.height - 8
                ZStack(alignment: .topLeading) {
                    Color.black.opacity(0.3)
                        .contentShape(Rectangle())
                        .onTapGesture { closeMenu() }
                    SaveStateMenuView(
                        title: row.title(settings),
                        subtitle: row.modifiedDate.map { SaveStateFormat.savedAt($0, settings: settings) },
                        items: items
                    )
                    .frame(width: width)
                    .offset(
                        x: min(max(12, button.maxX - width), proxy.size.width - width - 12),
                        y: below ? button.maxY + 10 : max(8, button.minY - 10 - height)
                    )
                }
            }
        }
    }

    @ViewBuilder
    private var renameOverlay: some View {
        if let row = row(renameSlot) {
            ZStack(alignment: .top) {
                Color.black.opacity(0.45)
                    .contentShape(Rectangle())
                    .onTapGesture { closeRename() }
                SaveStateRenameCard(
                    settings: settings,
                    title: String(format: settings.localized("Rename %@"), row.title(settings)),
                    placeholder: row.defaultTitle(settings),
                    wide: layout == .line || layout == .wideLine,
                    text: $renameText,
                    ids: (Self.renameName, Self.renameClear, Self.renameCancel, Self.renameConfirm),
                    onEdit: { keyboardRequest = SaveStateKeyboardRequest(slot: row.slot) },
                    onCancel: closeRename,
                    onConfirm: commitRename
                )
                .padding(14)
            }
        }
    }

    @ViewBuilder
    private var autoSaveOverlay: some View {
        if autoSaveOpen {
            ZStack(alignment: .top) {
                Color.black.opacity(0.45)
                    .contentShape(Rectangle())
                    .onTapGesture { closeAutoSave() }
                SaveStateAutoSaveCard(
                    settings: settings,
                    ids: (Self.autoSaveEnabled, Self.autoSaveInterval, Self.autoSaveOnLeave, Self.autoSaveOnLowBattery, Self.autoSaveDone),
                    onDone: closeAutoSave
                )
                .frame(maxWidth: 460)
                .padding(14)
            }
        }
    }
}

enum SaveStateRowLayout {
    /// iPhone landscape: one line per slot.
    case line
    /// iPad landscape: one line, wider buttons.
    case wideLine
    /// iPhone portrait and the largest text sizes: the buttons get their own line.
    case twoLine
    /// iPad portrait: date and play time on lines of their own.
    case stacked

    var thumbnail: CGSize {
        switch self {
        case .line: CGSize(width: 52, height: 39)
        case .wideLine: CGSize(width: 56, height: 42)
        case .twoLine: CGSize(width: 64, height: 48)
        case .stacked: CGSize(width: 72, height: 54)
        }
    }

    var buttonWidth: CGFloat? {
        switch self {
        case .line: 80
        case .wideLine: 88
        case .twoLine: nil
        case .stacked: 76
        }
    }

    var rowHeight: CGFloat {
        switch self {
        case .line: 50
        case .wideLine: 48
        case .twoLine: 0
        case .stacked: 72
        }
    }
}

/// Up and Down keep the column (Save, Load), or take the nearest button the next row has.
/// The first and last rows wrap on a new press; a held direction stops there.
struct SaveStateGraph {
    struct Target {
        let id: String
        let column: Int
    }

    let order: [String]
    let links: [ControllerAccessibilityDirectionalLink]
    let freshPressRules: [ControllerAccessibilityBoundaryRule]

    init(rows: [[Target]], wraps: Bool = true) {
        let boundary = ControllerAccessibilityDirectionalLink.navigationBoundary
        var links: [ControllerAccessibilityDirectionalLink] = []
        var rules: [ControllerAccessibilityBoundaryRule] = []
        func nearest(in row: [Target], to column: Int) -> String {
            row.min { (abs($0.column - column), $0.column) < (abs($1.column - column), $1.column) }?.id ?? boundary
        }
        for (index, row) in rows.enumerated() {
            let above = index > 0 ? rows[index - 1] : rows[rows.count - 1]
            let below = index + 1 < rows.count ? rows[index + 1] : rows[0]
            for (position, target) in row.enumerated() {
                let left = position > 0 ? row[position - 1].id : boundary
                let right = position + 1 < row.count ? row[position + 1].id : boundary
                links.append(.init(fromLabel: target.id, direction: .left, toLabel: left))
                links.append(.init(fromLabel: target.id, direction: .right, toLabel: right))
                let first = index == 0
                let last = index == rows.count - 1
                let upWraps = first && (!wraps || rows.count < 2)
                let downWraps = last && (!wraps || rows.count < 2)
                links.append(.init(
                    fromLabel: target.id, direction: .up,
                    toLabel: upWraps ? boundary : nearest(in: above, to: target.column)
                ))
                links.append(.init(
                    fromLabel: target.id, direction: .down,
                    toLabel: downWraps ? boundary : nearest(in: below, to: target.column)
                ))
                if wraps, first {
                    rules.append(.init(direction: .up, fromLabel: target.id, requiresFreshPress: true))
                }
                if wraps, last {
                    rules.append(.init(direction: .down, fromLabel: target.id, requiresFreshPress: true))
                }
            }
        }
        order = rows.flatMap { $0.map(\.id) }
        self.links = links
        freshPressRules = rules
    }
}

private struct SaveStateRowView: View {
    let row: SaveStateSlot
    let layout: SaveStateRowLayout
    let settings: SettingsStore
    let isLatest: Bool
    let isBusy: Bool
    let hardcore: Bool
    let primaryID: String?
    let quickSaveLine: ControllerHintLine?
    let onSave: () -> Void
    let onLoad: () -> Void
    let onMore: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    private var title: String { row.title(settings) }

    var body: some View {
        if layout == .twoLine {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    number
                    thumbnail
                    info
                }
                HStack(spacing: 12) {
                    buttons
                }
                .padding(.leading, 26)
            }
            .padding(.top, 8)
            .padding(.bottom, 10)
        } else {
            HStack(spacing: 12) {
                number
                thumbnail
                info
                buttons
            }
            .frame(minHeight: layout.rowHeight)
        }
    }

    private var number: some View {
        Group {
            if let symbol = row.symbol {
                Image(systemName: symbol)
            } else {
                Text("\(row.slot)")
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
            }
        }
        .font(.footnote.weight(.semibold))
        .foregroundStyle(OverlayTheme.textSecondary)
        .frame(width: 18)
        .accessibilityHidden(true)
    }

    private var thumbnail: some View {
        let size = layout.thumbnail
        return ZStack(alignment: .bottomLeading) {
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(row.occupied ? Color.black : Color.white.opacity(0.03))
            if let data = row.file.preview, let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .frame(width: size.width, height: size.height)
            }
        }
        .frame(width: size.width, height: size.height)
        .clipShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(
                    Color.white.opacity(row.occupied ? 0.1 : 0.22),
                    style: StrokeStyle(lineWidth: row.occupied ? 1 : 1.5, dash: row.occupied ? [] : [4, 3])
                )
        }
        .accessibilityHidden(true)
    }

    private var info: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(title)
                    .font(layout == .stacked ? .body.weight(.semibold) : .subheadline.weight(.semibold))
                    .foregroundStyle(row.occupied ? OverlayTheme.textPrimary : OverlayTheme.textSecondary)
                    .lineLimit(1)
                if row.isLocked {
                    Image(systemName: "lock.fill")
                        .font(.caption2)
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .accessibilityLabel(settings.localized("Locked"))
                }
            }
            if layout == .stacked || layout == .twoLine {
                savedLine
                playedLine
            } else {
                HStack(spacing: 10) {
                    savedLine
                    playedLine
                }
            }
            if let chip {
                Text(chip)
                    .font(.caption2)
                    .foregroundStyle(OverlayTheme.textSecondary)
            }
            if row.kind == .quick, let quickSaveLine {
                quickSaveLine
                    .font(.caption2)
                    .foregroundStyle(OverlayTheme.textSecondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder
    private var savedLine: some View {
        if row.occupied, let date = row.modifiedDate {
            let savedAt = SaveStateFormat.savedAt(date, settings: settings)
            if isLatest {
                // Inside the meta style, so the accent wins over its grey.
                Label(settings.localized("Latest") + " · " + savedAt, systemImage: "star.fill")
                    .foregroundStyle(accentColour)
                    .saveStateMeta(layout)
            } else {
                Label(savedAt, systemImage: "clock")
                    .saveStateMeta(layout)
            }
        } else {
            Text(settings.localized("Empty"))
                .saveStateMeta(layout)
        }
    }

    @ViewBuilder
    private var playedLine: some View {
        if row.occupied, let seconds = row.metadata?.playedSeconds {
            Label(SaveStateFormat.played(seconds, settings: settings), systemImage: "hourglass")
                .saveStateMeta(layout)
        }
    }

    private var chip: String? {
        switch row.kind {
        case .auto:
            !settings.autoSaveEnabled ? settings.localized("Off") : String(
                format: settings.localized(settings.autoSaveOnLeave ? "Every %d min and when you exit" : "Every %d min"),
                settings.autoSaveIntervalMinutes
            )
        case .quick, .manual, .older: nil
        }
    }

    @ViewBuilder
    private var buttons: some View {
        let saveID = SaveStatesPanel.saveID(row.slot)
        let loadID = SaveStatesPanel.loadID(row.slot)
        if row.hasSave {
            SaveStateButton(
                id: saveID,
                title: settings.localized("Save"),
                systemImage: row.isLocked ? "lock.fill" : "square.and.arrow.down",
                accessibilityLabel: String(
                    format: settings.localized(
                        row.isLocked ? "Save to %@, locked. Unlock it from More."
                            : row.occupied ? "Save over %@" : "Save to %@"
                    ),
                    title
                ),
                tonal: true,
                accentIcon: !row.occupied,
                isPrimary: primaryID == saveID,
                isBusy: isBusy,
                width: layout.buttonWidth,
                action: onSave
            )
            .disabled(row.isLocked)
        } else {
            placeholder
        }
        if row.occupied {
            SaveStateButton(
                id: loadID,
                title: settings.localized("Load"),
                systemImage: "arrow.counterclockwise",
                accessibilityLabel: String(
                    format: settings.localized(hardcore ? "Load %@. Loading is off in Hardcore mode." : "Load %@"),
                    title
                ),
                tonal: false,
                accentIcon: true,
                isPrimary: false,
                isBusy: false,
                width: layout.buttonWidth,
                action: onLoad
            )
            .disabled(hardcore)
        } else {
            placeholder
        }
        if row.hasMore {
            Button(action: onMore) {
                Image(systemName: "ellipsis")
                    .font(.body.weight(.semibold))
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .frame(width: 44, height: 44)
                    .background(Color.white.opacity(0.10), in: Circle())
                    .overlay(Circle().stroke(Color.white.opacity(0.14), lineWidth: 1))
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(String(format: settings.localized("More for %@"), title))
            .anchorPreference(key: SaveStateMoreAnchorKey.self, value: .bounds) { [row.slot: $0] }
            .controllerAccessibilityActionTarget(
                id: SaveStatesPanel.moreID(row.slot),
                label: SaveStatesPanel.moreID(row.slot),
                focusedNeonCornerRadius: 22,
                action: onMore
            )
        } else {
            Color.clear.frame(width: 44, height: 44)
        }
    }

    @ViewBuilder
    private var placeholder: some View {
        if let width = layout.buttonWidth {
            Color.clear.frame(width: width, height: 44)
        } else {
            Color.clear.frame(maxWidth: .infinity, maxHeight: 44)
        }
    }
}

private extension View {
    func saveStateMeta(_ layout: SaveStateRowLayout) -> some View {
        font(layout == .stacked ? .footnote : .caption)
            .foregroundStyle(OverlayTheme.textSecondary)
            .lineLimit(layout == .line || layout == .wideLine ? 1 : 2)
    }
}

/// Tonal until the pad focuses it; only the focused button, or on touch the one the pad would
/// start on, is filled with the accent.
private struct SaveStateButton: View {
    let id: String
    let title: String
    let systemImage: String
    let accessibilityLabel: String
    let tonal: Bool
    let accentIcon: Bool
    let isPrimary: Bool
    let isBusy: Bool
    let width: CGFloat?
    let action: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        Button(action: action) {
            SaveStateButtonLabel(
                title: title,
                systemImage: systemImage,
                tonal: tonal,
                accentIcon: accentIcon,
                isPrimary: isPrimary,
                isBusy: isBusy,
                width: width
            )
        }
        .buttonStyle(.plain)
        .accessibilityLabel(accessibilityLabel)
        .controllerAccessibilityActionTarget(
            id: id,
            label: id,
            focusedColor: onAccent(accentColour),
            focusedNeonCornerRadius: 22,
            action: action
        )
    }
}

private struct SaveStateButtonLabel: View {
    let title: String
    let systemImage: String
    let tonal: Bool
    let accentIcon: Bool
    let isPrimary: Bool
    let isBusy: Bool
    let width: CGFloat?

    @Environment(\.controllerAccessibilityTargetFocused) private var focused
    @Environment(\.isEnabled) private var isEnabled
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize

    var body: some View {
        let filled = isEnabled && (focused || isPrimary)
        let foreground = filled
            ? onAccent(accentColour)
            : (isEnabled ? OverlayTheme.textPrimary : Color(red: 0.43, green: 0.46, blue: 0.52))
        HStack(spacing: 6) {
            if isBusy {
                ProgressView()
                    .controlSize(.small)
                    .tint(foreground)
            } else if !dynamicTypeSize.isAccessibilitySize {
                Image(systemName: systemImage)
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(filled || !isEnabled || !accentIcon ? foreground : accentColour)
            }
            Text(title)
                .lineLimit(1)
                .minimumScaleFactor(0.75)
        }
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(foreground)
        .padding(.horizontal, 12)
        .frame(minWidth: width ?? 44, maxWidth: width == nil ? .infinity : nil, minHeight: 44)
        .background {
            Capsule()
                .fill(
                    filled ? accentColour
                        : !isEnabled ? Color.white.opacity(0.05)
                        : tonal ? Color.white.opacity(0.10) : accentColour.opacity(0.2)
                )
        }
        .overlay {
            Capsule().stroke(Color.white.opacity(tonal && isEnabled && !filled ? 0.14 : 0), lineWidth: 1)
        }
        .contentShape(Capsule())
    }
}

/// Black or white, whichever reads better on the accent.
private func onAccent(_ accent: Color) -> Color {
    var red: CGFloat = 0, green: CGFloat = 0, blue: CGFloat = 0, alpha: CGFloat = 0
    UIColor(accent).getRed(&red, green: &green, blue: &blue, alpha: &alpha)
    func linear(_ value: CGFloat) -> CGFloat {
        value <= 0.03928 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
    }
    let luminance = 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue)
    return (luminance + 0.05) / 0.05 >= 1.05 / (luminance + 0.05) ? .black : .white
}

struct SaveStateMoreAnchorKey: PreferenceKey {
    static let defaultValue: [Int: Anchor<CGRect>] = [:]
    static func reduce(value: inout [Int: Anchor<CGRect>], nextValue: () -> [Int: Anchor<CGRect>]) {
        value.merge(nextValue()) { _, next in next }
    }
}

struct SaveStateKeyboardRequest: Identifiable {
    let slot: Int
    var id: Int { slot }
}

struct SaveStateMenuItem {
    let id: String
    let title: String
    let systemImage: String
    var isDestructive = false
    var isEnabled = true
    let action: () -> Void
}

private struct SaveStateMenuView: View {
    let title: String
    let subtitle: String?
    let items: [SaveStateMenuItem]

    @Environment(\.uiCriticalTextColour) private var criticalTextColour

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text([title, subtitle].compactMap { $0 }.joined(separator: " · "))
                .font(.caption)
                .foregroundStyle(OverlayTheme.textSecondary)
                .lineLimit(1)
                .padding(.horizontal, 12)
                .padding(.top, 8)
                .padding(.bottom, 6)
            ForEach(items, id: \.id) { item in
                Button(action: item.action) {
                    HStack {
                        Text(item.title)
                        Spacer()
                        Image(systemName: item.systemImage)
                    }
                    .font(.callout)
                    .foregroundStyle(item.isDestructive ? criticalTextColour : OverlayTheme.textPrimary)
                    .padding(.horizontal, 16)
                    .frame(minHeight: 44)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .controllerAccessibilityActionTarget(
                    id: item.id,
                    label: item.id,
                    focusedColor: item.isDestructive ? criticalTextColour : nil,
                    focusedNeonCornerRadius: 12,
                    action: item.action
                )
                .disabled(!item.isEnabled)
                .opacity(item.isEnabled ? 1 : 0.45)
            }
        }
        .padding(4)
        .background(OverlayTheme.card, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).stroke(Color.white.opacity(0.12)))
        .shadow(color: .black.opacity(0.55), radius: 24, y: 18)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
    }
}

/// Up to 32 characters. With a controller the name is typed on OrbitKeys, which has no limit
/// of its own, so the name is cut when it comes back.
private struct SaveStateRenameCard: View {
    let settings: SettingsStore
    let title: String
    let placeholder: String
    let wide: Bool
    @Binding var text: String
    let ids: (name: String, clear: String, cancel: String, confirm: String)
    let onEdit: () -> Void
    let onCancel: () -> Void
    let onConfirm: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(OverlayTheme.textPrimary)
                .lineLimit(1)
            if wide {
                HStack(alignment: .top, spacing: 10) {
                    field
                    buttons
                }
            } else {
                field
                HStack(spacing: 10) { buttons }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(OverlayTheme.card, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).stroke(Color.white.opacity(0.14)))
        .shadow(color: .black.opacity(0.5), radius: 25, y: 20)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
    }

    private var field: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(settings.localized("Name"))
                .font(.caption)
                .foregroundStyle(OverlayTheme.textSecondary)
            HStack(spacing: 0) {
                TextField(placeholder, text: $text)
                    .font(.callout)
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .submitLabel(.done)
                    .onSubmit(onConfirm)
                    .onChange(of: text) { _, value in
                        if value.count > SaveStateSlot.nameLimit {
                            text = String(value.prefix(SaveStateSlot.nameLimit))
                        }
                    }
                    .padding(.leading, 12)
                    .frame(minHeight: 44)
                    .controllerAccessibilityActionTarget(
                        id: ids.name,
                        label: ids.name,
                        focusedNeonCornerRadius: 12,
                        action: onEdit
                    )
                Button { text = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(settings.localized("Clear name"))
                .controllerAccessibilityActionTarget(
                    id: ids.clear,
                    label: ids.clear,
                    focusedNeonCornerRadius: 22
                ) { text = "" }
            }
            .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(accentColour, lineWidth: 2))
            HStack {
                Text(String(format: settings.localized("Empty shows “%@”. Saving over keeps the name."), placeholder))
                Spacer(minLength: 8)
                Text(String(format: settings.localized("%1$d/%2$d"), text.count, SaveStateSlot.nameLimit))
                    .monospacedDigit()
            }
            .font(.caption)
            .foregroundStyle(OverlayTheme.textSecondary)
        }
    }

    @ViewBuilder
    private var buttons: some View {
        Button(settings.localized("Cancel"), action: onCancel)
            .buttonStyle(.bordered)
            .controlSize(.large)
            .controllerAccessibilityActionTarget(id: ids.cancel, label: ids.cancel, action: onCancel)
        Button(settings.localized("Rename"), action: onConfirm)
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .controllerAccessibilityActionTarget(id: ids.confirm, label: ids.confirm, action: onConfirm)
    }
}

/// The same settings as in Settings > Emulator.
private struct SaveStateAutoSaveCard: View {
    @Bindable var settings: SettingsStore
    let ids: (enabled: String, interval: String, leave: String, battery: String, done: String)
    let onDone: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(settings.localized("Auto-save Settings"))
                .font(.subheadline.weight(.semibold))
                .padding(.horizontal, 8)
            Toggle(settings.localized("Auto-save"), isOn: $settings.autoSaveEnabled)
                .padding(8)
                .controllerAccessibilityToggleTarget(id: ids.enabled, label: ids.enabled, isOn: $settings.autoSaveEnabled)
            Group {
                VStack(alignment: .leading, spacing: 6) {
                    Text(settings.localized("Save every"))
                        .font(.caption)
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .padding(.horizontal, 8)
                    Picker(settings.localized("Save every"), selection: $settings.autoSaveIntervalMinutes) {
                        ForEach(SettingsStore.autoSaveIntervals, id: \.self) { minutes in
                            Text(String(format: settings.localized("%d min"), minutes)).tag(minutes)
                        }
                    }
                    .pickerStyle(.segmented)
                    .controllerAccessibilityAdjustableTarget(
                        id: ids.interval,
                        label: ids.interval,
                        value: String(format: settings.localized("%d min"), settings.autoSaveIntervalMinutes),
                        onActivate: { stepInterval(1, wraps: true) },
                        onIncrement: { stepInterval(1) },
                        onDecrement: { stepInterval(-1) }
                    )
                }
                Toggle(settings.localized("Save when leaving the game"), isOn: $settings.autoSaveOnLeave)
                    .padding(8)
                    .controllerAccessibilityToggleTarget(id: ids.leave, label: ids.leave, isOn: $settings.autoSaveOnLeave)
                Toggle(settings.localized("Save when battery is low"), isOn: $settings.autoSaveOnLowBattery)
                    .padding(8)
                    .controllerAccessibilityToggleTarget(id: ids.battery, label: ids.battery, isOn: $settings.autoSaveOnLowBattery)
            }
            .disabled(!settings.autoSaveEnabled)
            Button(settings.localized("Done"), action: onDone)
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .controllerAccessibilityActionTarget(id: ids.done, label: ids.done, action: onDone)
                .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .foregroundStyle(OverlayTheme.textPrimary)
        .tint(accentColour)
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(OverlayTheme.card, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).stroke(Color.white.opacity(0.14)))
        .shadow(color: .black.opacity(0.5), radius: 25, y: 20)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
    }

    private func stepInterval(_ offset: Int, wraps: Bool = false) {
        let all = SettingsStore.autoSaveIntervals
        let next = (all.firstIndex(of: settings.autoSaveIntervalMinutes) ?? 1) + offset
        settings.autoSaveIntervalMinutes = wraps
            ? all[(next + all.count) % all.count] : all[min(max(next, 0), all.count - 1)]
    }
}

/// After a load, delete or save-over: what happened, and an Undo that lasts as long as Settings says.
struct SaveStateUndoToast: View {
    let undo: SaveStateUndoModel
    let settings: SettingsStore
    let hint: ControllerHintLine?
    let onUndo: () -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @AccessibilityFocusState private var focused: Bool

    var body: some View {
        if let item = undo.item {
            HStack(spacing: 12) {
                ZStack {
                    Color.black
                    if let data = item.preview, let image = UIImage(data: data) {
                        Image(uiImage: image).resizable().scaledToFill()
                    }
                }
                .frame(width: 56, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.caption)
                        .font(.caption)
                        .foregroundStyle(OverlayTheme.textSecondary)
                    Text(item.name)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(OverlayTheme.textPrimary)
                        .lineLimit(2)
                    if let hint {
                        hint
                            .font(.caption)
                            .foregroundStyle(OverlayTheme.textSecondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Button(action: onUndo) {
                    Label(settings.localized("Undo"), systemImage: "arrow.uturn.backward")
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(.white)
                        .frame(width: 104, height: 44)
                        .background(Color.white.opacity(0.14), in: Capsule())
                        .overlay(Capsule().stroke(Color.white.opacity(0.2)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(item.undoLabel)
            }
            .padding(EdgeInsets(top: 8, leading: 10, bottom: 11, trailing: 10))
            .overlay(alignment: .bottom) {
                // Reduce Motion leaves the draining bar out; the window is just as long.
                if !reduceMotion {
                    GeometryReader { proxy in
                        Capsule().fill(Color.white.opacity(0.14))
                            .overlay(alignment: .leading) {
                                Capsule().fill(Color.white.opacity(0.75))
                                    .frame(width: proxy.size.width * max(0, undo.remaining) / undo.duration)
                            }
                    }
                    .frame(height: 3)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 3)
                    .accessibilityHidden(true)
                }
            }
            .background(Color(red: 0.133, green: 0.149, blue: 0.180), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).stroke(Color.white.opacity(0.14)))
            .shadow(color: .black.opacity(0.5), radius: 18, y: 14)
            .frame(maxWidth: 460)
            .accessibilityElement(children: .contain)
            .accessibilityFocused($focused)
            .accessibilityAction(.escape) { undo.finish() }
            .onChange(of: focused) { _, value in undo.focusHeld = value }
        }
    }
}
