// PadLayoutEditView.swift — Drag-to-edit virtual pad layout
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct PadLayoutEditorUndoSnapshot {
    let portrait: [String: PadGroupPosition]
    let landscape: [String: PadGroupPosition]
    let perButtonPortrait: [String: PadGroupPosition]
    let perButtonLandscape: [String: PadGroupPosition]
    let controlVisibility: [String: Bool]
    let skin: VirtualPadSkin
    let skinID: String
}

struct PadEditorControlGeometry {
    let id: String
    let center: CGPoint
    let baseSize: CGSize
    let hitScaleX: CGFloat
    let hitScaleY: CGFloat

    var hitSize: CGSize {
        if isStick(id) {
            return CGSize(
                width: PadLayoutMetrics.visibleLength(baseLength: baseSize.width, visibleScale: hitScaleX),
                height: PadLayoutMetrics.visibleLength(baseLength: baseSize.height, visibleScale: hitScaleY)
            )
        }
        return CGSize(
            width: PadLayoutMetrics.touchLength(baseLength: baseSize.width, hitScale: hitScaleX),
            height: PadLayoutMetrics.touchLength(baseLength: baseSize.height, hitScale: hitScaleY)
        )
    }

    var hitFrame: CGRect {
        CGRect(x: center.x - hitSize.width / 2, y: center.y - hitSize.height / 2, width: hitSize.width, height: hitSize.height)
    }
}

private enum PadLayoutNameAction: Identifiable {
    case saveNew
    case duplicate
    case rename

    var id: String {
        switch self {
        case .saveNew: return "saveNew"
        case .duplicate: return "duplicate"
        case .rename: return "rename"
        }
    }
}

struct PadLayoutEditView: View {
    let onDismiss: () -> Void
    let context: PadLayoutEditorContext
    @State private var settings = SettingsStore.shared
    @State private var layout = PadLayoutStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var editLandscape = false
    @State private var undoStack: [PadLayoutEditorUndoSnapshot] = []
    @State private var originalSnapshot: PadLayoutEditorUndoSnapshot? = nil
    @State private var loadedPresetSnapshot: PadLayoutSnapshot? = nil
    @State private var activePresetID: String? = nil
    @State private var didInitializeOrientation = false
    @State private var selectedControlID: String? = nil
    @State private var pendingNameAction: PadLayoutNameAction? = nil
    @State private var nameDraft = ""
    @State private var showDeleteLayoutConfirmation = false
    @State private var layoutExportItem: ShareSheetItem?
    // Transient editor-only aspect-lock state. Not persisted: a control is
    // treated as unlinked when explicitly toggled here OR when its stored axes
    // already differ. Cleared on dismiss with the rest of the editor view state.
    @State private var unlinkedControls: Set<String> = []
    // The Duo portrait split for this size, held so a drop at a deck edge does not refit the pad.
    @State private var heldSplit: (key: [CGFloat?], gameHeight: CGFloat, fit: PadPortraitFit?)?
    // Bumped when the whole layout changes (load, undo, reset, visibility) so the held split refits.
    @State private var refitGeneration = 0

    init(onDismiss: @escaping () -> Void, context: PadLayoutEditorContext = .current) {
        self.onDismiss = onDismiss
        self.context = context
    }

    private var effectiveSkinDescriptor: VPadSkinDescriptor {
        context.skinDescriptor ?? skinLibrary.selectedDescriptor
    }

    var body: some View {
        GeometryReader { geo in
            let isActuallyLandscape = geo.size.width > geo.size.height
            let isMismatch = isActuallyLandscape != editLandscape
            // On the Duo the portrait deck is split like gameplay's, so the refitted pad shows as it plays.
            let deckHeight = geo.size.height + geo.safeAreaInsets.bottom
            let fold = geo.activeFold(book: false)
            let snapshot = layout.snapshot()
            let splitKey = [geo.size.width, geo.size.height, fold?.minY, fold?.height, CGFloat(refitGeneration)]
            let liveSplit = !isActuallyLandscape && AppState.shared.hingePresent
                ? portraitGameSplit(geo, deckHeight: deckHeight, fold: fold, refitting: snapshot, analogStickScale: settings.analogStickScale)
                : nil
            let split = heldSplit?.key == splitKey ? heldSplit : liveSplit.map { (key: splitKey, gameHeight: $0.gameHeight, fit: $0.fit) }
            let portraitGameHeight = split.map { $0.gameHeight + (fold?.height ?? 0) } ?? min(geo.size.width * 3 / 4, geo.size.height * 0.55)
            let editAreaW = geo.size.width
            let editAreaH = isActuallyLandscape ? geo.size.height : (split == nil ? geo.size.height : deckHeight) - portraitGameHeight
            let panelArea = split?.fit?.referenceArea(deckWidth: editAreaW) ?? CGSize(width: editAreaW, height: editAreaH)
            let fitted = split.map { split in
                split.fit.map {
                    snapshot.fittedToPortraitDeck(
                        $0,
                        size: CGSize(width: editAreaW, height: editAreaH),
                        analogStickScale: CGFloat(SettingsStore.clampedAnalogStickScale(settings.analogStickScale))
                    )
                } ?? snapshot
            }
            ZStack(alignment: .top) {
                Color.clear
                    .contentShape(Rectangle())
                    .onTapGesture {
                        selectedControlID = nil
                    }

                if isActuallyLandscape {
                    landscapeEditorCanvas(isEditable: !isMismatch)
                } else {
                    portraitEditorCanvas(
                        width: geo.size.width,
                        gameHeight: portraitGameHeight,
                        padHeight: editAreaH,
                        fitted: fitted,
                        fit: split?.fit,
                        isEditable: !isMismatch
                    )
                }

                VStack(spacing: 0) {
                    editorToolbar()
                        .padding(.top, max(geo.safeAreaInsets.top, 8))
                        .padding(.horizontal, max(max(geo.safeAreaInsets.leading, geo.safeAreaInsets.trailing), 8))

                    if isMismatch {
                        orientationMismatchView()
                    } else if let selectedControlID {
                        PadLayoutSelectedControlPanel(id: selectedControlID, areaW: panelArea.width, areaH: panelArea.height, isLandscape: editLandscape, unlinkedControls: $unlinkedControls, pushSnapshot: pushSnapshot)
                            .padding(.top, 8)
                            .padding(.horizontal, max(max(geo.safeAreaInsets.leading, geo.safeAreaInsets.trailing), 8))
                        Spacer(minLength: 0)
                    } else {
                        Spacer(minLength: 0)
                    }
                }
                .frame(width: geo.size.width, height: geo.size.height, alignment: .top)
                .position(x: geo.size.width / 2, y: geo.size.height / 2)
                .zIndex(2)
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .top)
            .onChange(of: splitKey, initial: true) {
                heldSplit = liveSplit.map { (key: splitKey, gameHeight: $0.gameHeight, fit: $0.fit) }
            }
            .onAppear {
                if originalSnapshot == nil {
                    originalSnapshot = captureSnapshot()
                    activePresetID = context.presetID
                    if let initialSnapshot = context.initialSnapshot {
                        layout.apply(snapshot: initialSnapshot)
                        loadedPresetSnapshot = initialSnapshot
                        refitGeneration += 1
                    } else {
                        loadedPresetSnapshot = layout.snapshot()
                    }
                }
                if !didInitializeOrientation {
                    editLandscape = isActuallyLandscape
                    didInitializeOrientation = true
                }
            }
        }
        // Otherwise the name keyboard shortens the editor and portrait gets the landscape layout.
        .ignoresSafeArea(.keyboard)
        .navigationBarHidden(true)
        .statusBarHidden()
        .statusColumnDisabled()
        .persistentSystemOverlays(.hidden)
        .onDisappear {
            rollbackUnsavedChanges()
        }
        .alert(nameActionTitle, isPresented: Binding(
            get: { pendingNameAction != nil },
            set: { if !$0 { pendingNameAction = nil } }
        )) {
            TextField("Layout Name", text: $nameDraft)
            Button(nameActionConfirmTitle) {
                commitPendingNameAction()
            }
            Button("Cancel", role: .cancel) {
                pendingNameAction = nil
            }
        }
        .confirmationDialog("Delete Layout?", isPresented: $showDeleteLayoutConfirmation, titleVisibility: .visible) {
            Button("Delete Layout", role: .destructive) {
                deleteCurrentPresetAndDismiss()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This deletes the layout preset. Games using it will fall back to their next available layout.")
        }
        .sheet(item: $layoutExportItem) { item in
            ActivityShareSheet(activityItems: [item.url])
        }
    }

    // MARK: - Landscape editor canvas (always shown when device is landscape)
    private func landscapeEditorCanvas(isEditable: Bool) -> some View {
        GeometryReader { geo in
            let width = geo.size.width
            let height = geo.size.height
            ZStack {
                Color.clear
                    .allowsHitTesting(false)

                if isEditable {
                    customSkinPreview(isLandscape: true, width: width, height: height)
                    editorControls(areaW: width, areaH: height, isLandscape: true)
                }
            }
            .contentShape(Rectangle())
        }
        .ignoresSafeArea()
    }

    // MARK: - Portrait editor canvas (always shown when device is portrait)
    private func portraitEditorCanvas(width: CGFloat, gameHeight: CGFloat, padHeight: CGFloat, fitted: PadLayoutSnapshot?, fit: PadPortraitFit?, isEditable: Bool) -> some View {
        VStack(spacing: 0) {
            // Top: reveal paused gameplay underneath
            Color.clear
                .allowsHitTesting(false)
                .frame(height: gameHeight)

            // Bottom: controller deck editing area (dark enough for controls)
            ZStack {
                Color.black.opacity(0.85)
                if isEditable {
                    customSkinPreview(isLandscape: false, width: width, height: padHeight)
                    editorControls(areaW: width, areaH: padHeight, isLandscape: false, fitted: fitted, fit: fit)
                }
            }
            .frame(height: padHeight)
            .clipped()
        }
    }

    private func editorToolbar() -> some View {
        HStack(spacing: 6) {
            Button {
                rollbackUnsavedChanges()
                onDismiss()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(.white)
                    .frame(width: 28, height: 28)
            }

            Button {
                guard let snapshot = undoStack.popLast() else { return }
                restore(snapshot: snapshot)
            } label: {
                Image(systemName: "arrow.uturn.backward.circle")
                    .font(.title3)
                    .foregroundStyle(undoStack.isEmpty ? .white.opacity(0.3) : .white)
                    .frame(width: 28, height: 28)
            }
            .disabled(undoStack.isEmpty)

            Picker("", selection: $editLandscape) {
                Text("Port.").tag(false)
                Text("Land.").tag(true)
            }
            .pickerStyle(.segmented)
            .frame(width: 112)
            .background(.black.opacity(0.3), in: RoundedRectangle(cornerRadius: 8))

            skinToolbarItem()
            layoutPresetMenuButton()
            optionsMenuButton()

            Button {
                savePrimaryAction()
            } label: {
                Image(systemName: "checkmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(.green)
                    .frame(width: 28, height: 28)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private var currentLayoutTitle: String {
        if let preset = layoutPresets.preset(id: activePresetID) {
            return preset.displayName
        }
        return "Current Layout"
    }

    private var editorIsModified: Bool {
        guard let loadedPresetSnapshot else { return false }
        return layout.snapshot() != loadedPresetSnapshot
    }

    @ViewBuilder
    private func skinToolbarItem() -> some View {
        skinPickerMenuButton()
    }

    private func layoutPresetMenuButton() -> some View {
        Menu {
            Section {
                Text(editorIsModified ? "\(currentLayoutTitle) - Modified" : currentLayoutTitle)
            }
            Button("Save as New Layout") {
                beginNameAction(.saveNew)
            }
            Button("Export Layout") {
                exportCurrentLayout()
            }
            if activePresetID != nil {
                Button("Update Current Layout") {
                    updateCurrentPresetAndDismiss()
                }
                Button("Duplicate Layout") {
                    beginNameAction(.duplicate)
                }
                Button("Rename Layout") {
                    beginNameAction(.rename)
                }
                Button("Delete Layout", role: .destructive) {
                    showDeleteLayoutConfirmation = true
                }
                Button("Set as Global Default") {
                    layoutPresets.globalPresetID = activePresetID
                }
            }
            if context.gameIdentity != nil {
                Button("Use for Current Game") {
                    assignCurrentPresetToGame()
                }
            }
        } label: {
            VStack(spacing: 0) {
                Image(systemName: editorIsModified ? "rectangle.and.pencil.and.ellipsis" : "rectangle.stack")
                    .font(.body)
                    .foregroundStyle(.white)
                Text(layoutPresets.preset(id: activePresetID)?.displayName ?? "Layout")
                    .font(.system(size: 7, weight: .semibold))
                    .foregroundStyle(.white.opacity(0.9))
                    .lineLimit(1)
                    .frame(width: 58)
            }
            .frame(width: 60, height: 28)
        }
    }

    private var nameActionTitle: String {
        switch pendingNameAction {
        case .saveNew: return "Save as New Layout"
        case .duplicate: return "Duplicate Layout"
        case .rename: return "Rename Layout"
        case nil: return "Layout"
        }
    }

    private var nameActionConfirmTitle: String {
        switch pendingNameAction {
        case .rename: return "Rename"
        case .duplicate: return "Duplicate"
        case .saveNew: return "Save"
        case nil: return "OK"
        }
    }

    private func beginNameAction(_ action: PadLayoutNameAction) {
        pendingNameAction = action
        switch action {
        case .saveNew:
            nameDraft = "Layout \(layoutPresets.presets.count + 1)"
        case .duplicate:
            nameDraft = "\(currentLayoutTitle) Copy"
        case .rename:
            nameDraft = currentLayoutTitle
        }
    }

    private func commitPendingNameAction() {
        guard let action = pendingNameAction else { return }
        let snapshot = layout.snapshot()
        switch action {
        case .saveNew:
            let preset = layoutPresets.createPreset(named: nameDraft, snapshot: snapshot)
            activePresetID = preset.id
            loadedPresetSnapshot = snapshot
            if let identity = context.gameIdentity {
                layoutPresets.setPreset(preset.id, for: identity)
            }
            finishPresetEditRestoringOriginal()
        case .duplicate:
            if let activePresetID,
               let duplicate = try? layoutPresets.duplicatePreset(id: activePresetID, named: nameDraft) {
                self.activePresetID = duplicate.id
                loadedPresetSnapshot = duplicate.snapshot
                if let identity = context.gameIdentity {
                    layoutPresets.setPreset(duplicate.id, for: identity)
                }
            }
        case .rename:
            if let activePresetID {
                try? layoutPresets.renamePreset(id: activePresetID, to: nameDraft)
            }
        }
        pendingNameAction = nil
    }

    private func savePrimaryAction() {
        // Only create or update a layout preset when the layout geometry actually
        // changed. Skin-only preview/reset must not force "Save as New Layout".
        guard editorIsModified else {
            // The editor temporarily applies a preset to the shared working
            // layout. Always restore that working copy on exit; gameplay reads
            // the saved preset directly. Leaking this temporary copy used to
            // make a preset appear to work only after opening the editor once.
            finishPresetEditRestoringOriginal()
            return
        }

        if activePresetID != nil {
            updateCurrentPresetAndDismiss()
        } else if context.gameIdentity != nil || context.initialSnapshot != nil {
            beginNameAction(.saveNew)
        } else {
            layout.save()
            undoStack.removeAll()
            originalSnapshot = nil
            onDismiss()
        }
    }

    private func updateCurrentPresetAndDismiss() {
        guard let activePresetID else {
            beginNameAction(.saveNew)
            return
        }
        let snapshot = layout.snapshot()
        try? layoutPresets.updatePreset(id: activePresetID, snapshot: snapshot)
        loadedPresetSnapshot = snapshot
        finishPresetEditRestoringOriginal()
    }

    private func deleteCurrentPresetAndDismiss() {
        guard let activePresetID else { return }
        try? layoutPresets.deletePreset(id: activePresetID)
        finishPresetEditRestoringOriginal()
    }

    private func assignCurrentPresetToGame() {
        guard let identity = context.gameIdentity else { return }
        if let activePresetID {
            layoutPresets.setPreset(activePresetID, for: identity)
        } else {
            beginNameAction(.saveNew)
        }
    }

    private func exportCurrentLayout() {
        do {
            let data = try PadLayoutImportExport.exportData(
                displayName: currentLayoutTitle,
                snapshot: layout.snapshot()
            )
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent(PadLayoutImportExport.exportedFileName(for: currentLayoutTitle))
            try data.write(to: url, options: .atomic)
            layoutExportItem = ShareSheetItem(url: url)
        } catch {
            NSLog("[ARMSX2 iOS Layout] export failed: %@", error.localizedDescription)
        }
    }

    private func finishPresetEditRestoringOriginal() {
        if let originalSnapshot {
            // Roll back the temporary layout working copy while keeping any skin
            // change made inside the editor (skin reset/selection is skin-only).
            let currentSkin = settings.virtualPadSkin
            let currentSkinID = skinLibrary.selectedSkinID
            restore(snapshot: originalSnapshot)
            settings.virtualPadSkin = currentSkin
            skinLibrary.selectSkin(id: currentSkinID)
        }
        undoStack.removeAll()
        originalSnapshot = nil
        onDismiss()
    }

    private func orientationMismatchView() -> some View {
        let message = editLandscape
            ? "Rotate device to landscape to edit landscape layout"
            : "Rotate device to portrait to edit portrait layout"
        let explanation = editLandscape
            ? "Accurate editing requires rotating the device or switching to portrait layout."
            : "Accurate editing requires rotating the device or switching to landscape layout."
        return VStack(spacing: 12) {
            Image(systemName: "rotate.right")
                .font(.system(size: 48, weight: .light))
                .foregroundStyle(.white.opacity(0.6))
            Text(message)
                .font(.headline.weight(.semibold))
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .lineLimit(3)
            Text(explanation)
                .font(.subheadline)
                .foregroundStyle(.white.opacity(0.7))
                .multilineTextAlignment(.center)
                .lineLimit(3)
        }
        .padding(.horizontal, 32)
        .padding(.vertical, 24)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 16))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .allowsHitTesting(false)
    }

    private func skinPickerMenuButton() -> some View {
        Menu {
            ForEach(skinLibrary.allDescriptors) { skin in
                Button {
                    guard skinLibrary.selectedSkinID != skin.id else { return }
                    pushSnapshot()
                    skinLibrary.selectSkin(id: skin.id)
                    settings.virtualPadSkin = skin.virtualPadSkin
                } label: {
                    Label(settings.localized(skin.displayName), systemImage: skinLibrary.selectedSkinID == skin.id ? "checkmark" : "circle")
                }
            }
        } label: {
            Image(systemName: "paintpalette.fill")
                .font(.title3)
                .foregroundStyle(.white)
                .frame(width: 28, height: 28)
        }
    }

    private func optionsMenuButton() -> some View {
        Menu {
            Section("Visibility") {
                ForEach(PadLayoutStore.groupIDs.filter { $0 != "action" }, id: \.self) { (id: String) in
                    visibilityMenuItem(id: id)
                }
                visibilityMenuItem(id: "cross", label: "X")
                visibilityMenuItem(id: "circle", label: "O")
                visibilityMenuItem(id: "triangle", label: "Triangle")
                visibilityMenuItem(id: "square", label: "Square")
            }

            Section("Reset") {
                Button("Reset Action", role: .destructive) {
                    pushSnapshot()
                    layout.resetPerButtonActionButtons(isLandscape: editLandscape)
                    refitGeneration += 1
                }
                Button("Reset D-Pad", role: .destructive) {
                    pushSnapshot()
                    layout.resetPerButtonDPad(isLandscape: editLandscape)
                    refitGeneration += 1
                }
                Button("Reset All", role: .destructive) {
                    pushSnapshot()
                    layout.resetAll()
                    refitGeneration += 1
                }
                Button("Reset Visibility", role: .destructive) {
                    pushSnapshot()
                    layout.resetControlVisibility()
                    refitGeneration += 1
                }
            }
        } label: {
            Image(systemName: "ellipsis.circle")
                .font(.title3)
                .foregroundStyle(.white)
                .frame(width: 28, height: 28)
        }
    }

    private func visibilityMenuItem(id: String, label: String? = nil) -> some View {
        let visible = layout.isControlVisible(id)
        return Button {
            pushSnapshot()
            layout.setControlVisible(id, visible: !visible)
            refitGeneration += 1
        } label: {
            let name = label ?? id.uppercased()
            Label(name, systemImage: visible ? "eye" : "eye.slash")
        }
    }

    @ViewBuilder
    private func customSkinPreview(isLandscape: Bool, width: CGFloat, height: CGFloat) -> some View {
        if let fullSkin = ControllerAsset.gameplayFullSkinImage(descriptor: effectiveSkinDescriptor, isLandscape: isLandscape) {
            Image(uiImage: fullSkin)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFill()
                .frame(width: width, height: height)
                .clipped()
                .opacity(0.42)
                .allowsHitTesting(false)
        }
    }

    @ViewBuilder
    private func editorControls(areaW: CGFloat, areaH: CGFloat, isLandscape: Bool, fitted: PadLayoutSnapshot? = nil, fit: PadPortraitFit? = nil) -> some View {
        let stickScale = CGFloat(SettingsStore.clampedAnalogStickScale(settings.analogStickScale))
        let overlaps = overlappingControlIDs(
            in: (fitted ?? layout.snapshot()).controlGeometries(size: CGSize(width: areaW, height: areaH), landscape: isLandscape, analogStickScale: stickScale)
        )
        ZStack {
            ForEach(PadLayoutStore.perButtonIDs, id: \.self) { (id: String) in
                DraggableButton(
                    id: id,
                    areaW: areaW,
                    areaH: areaH,
                    isLandscape: isLandscape,
                    fitted: fitted,
                    fit: fit,
                    selectedControlID: $selectedControlID,
                    isSelected: selectedControlID == id,
                    isOverlapping: overlaps.contains(id),
                    onBeginEdit: pushSnapshot
                )
            }
            ForEach(PadLayoutStore.groupIDs.filter { $0 != "action" && $0 != "dpad" }, id: \.self) { (id: String) in
                DraggableGroup(
                    id: id,
                    areaW: areaW,
                    areaH: areaH,
                    isLandscape: isLandscape,
                    analogStickScale: stickScale,
                    fitted: fitted,
                    fit: fit,
                    selectedControlID: $selectedControlID,
                    isSelected: selectedControlID == id,
                    isOverlapping: overlaps.contains(id),
                    onBeginEdit: pushSnapshot
                )
            }
        }
        .environment(\.padSkin, effectiveSkinDescriptor.virtualPadSkin)
        .environment(\.padSkinDescriptor, effectiveSkinDescriptor)
        .environment(\.padOpacity, 1.0)
        .environment(\.padUsesFullSkin, false)
    }

    private func overlappingControlIDs(in geometries: [PadEditorControlGeometry]) -> Set<String> {
        var result = Set<String>()
        for leftIndex in geometries.indices {
            for rightIndex in geometries.index(after: leftIndex)..<geometries.endIndex {
                let left = geometries[leftIndex]
                let right = geometries[rightIndex]
                guard left.hitFrame.intersects(right.hitFrame) else { continue }
                let overlap = left.hitFrame.intersection(right.hitFrame)
                if overlap.width > 0 && overlap.height > 0 {
                    result.insert(left.id)
                    result.insert(right.id)
                }
            }
        }
        return result
    }

    // MARK: - Snapshot / Undo

    private func captureSnapshot() -> PadLayoutEditorUndoSnapshot {
        PadLayoutEditorUndoSnapshot(
            portrait: layout.portrait,
            landscape: layout.landscape,
            perButtonPortrait: layout.perButtonPortrait,
            perButtonLandscape: layout.perButtonLandscape,
            controlVisibility: layout.controlVisibility,
            skin: settings.virtualPadSkin,
            skinID: skinLibrary.selectedSkinID
        )
    }

    private func restore(snapshot: PadLayoutEditorUndoSnapshot) {
        layout.portrait = snapshot.portrait
        layout.landscape = snapshot.landscape
        layout.perButtonPortrait = snapshot.perButtonPortrait
        layout.perButtonLandscape = snapshot.perButtonLandscape
        layout.controlVisibility = snapshot.controlVisibility
        skinLibrary.selectSkin(id: snapshot.skinID)
        settings.virtualPadSkin = snapshot.skin
        refitGeneration += 1
    }

    private func pushSnapshot() {
        undoStack.append(captureSnapshot())
    }

    private func rollbackUnsavedChanges() {
        guard let snapshot = originalSnapshot else { return }
        restore(snapshot: snapshot)
        undoStack.removeAll()
        originalSnapshot = nil
    }
}

func isStick(_ id: String) -> Bool {
    id == "lstick" || id == "rstick"
}

