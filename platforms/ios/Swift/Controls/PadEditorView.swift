// PadEditorView.swift — the full-screen pad layout editor
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// A layout name alert, captured when it opens.
struct PadEditorNameRequest: Identifiable {
    enum Purpose { case saveAsNew, duplicate, rename }

    let id = UUID()
    let purpose: Purpose
    let title: String
    let field: String
    let confirm: String
    let cancel: String
}

/// What shows behind the pad: the paused game under the in-game overlay, or the game's cover.
enum PadEditorBackdrop: Equatable {
    case pausedGame
    case cover(URL?)
}

struct PadEditorView: View {
    let backdrop: PadEditorBackdrop
    let onDismiss: () -> Void
    // What Update saves to; picking another layout in the Layouts card moves it.
    @State private var context: PadLayoutEditorContext
    @State private var switched = false
    @State private var pickedSlot: PadEditorSlot?
    @State private var editor: PadEditor
    @State private var settings = SettingsStore.shared
    @State private var store = PadLayoutPresetStore.shared
    @State private var drop: Drop?
    @State private var scope: PadEditorSaveScope
    @State private var saveAsNew = false
    @State private var nameRequest: PadEditorNameRequest?
    @State private var nameDraft = ""
    @State private var barFrame: CGRect = .zero
    @State private var padFrame: CGRect = .zero
    @State private var panelSize: CGSize = .zero
    @State private var touchAreas = false
    private var preferences = PadEditorPreferences()
    @AppStorage("ARMSX2iOSPadEditorHintShown") private var hintShown = false
    @State private var hintHeight: CGFloat = 0
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var controllerSession = UUID()
    @State private var panelFocused = false
    // With a controller connected a name is typed in OrbitKeys; the alert's field takes no pad input.
    @State private var keyboardRequest: PadEditorNameRequest?
    @State private var typedName: PadEditorNameRequest.Purpose?
    // The Duo portrait split, held so a drop at a deck edge does not refit the pad under the finger.
    @State private var heldSplit: (key: [CGFloat?], gameHeight: CGFloat, fit: PadPortraitFit?)?

    private enum Drop { case save, more, reset, layouts }

    init(context: PadLayoutEditorContext, backdrop: PadEditorBackdrop = .cover(nil), onDismiss: @escaping () -> Void) {
        _context = State(initialValue: context)
        self.backdrop = backdrop
        self.onDismiss = onDismiss
        let window = UIApplication.shared.appWindowScene?.coordinateSpace.bounds.size ?? .zero
        _editor = State(initialValue: PadEditor(
            snapshot: context.initialSnapshot ?? PadLayoutStore.shared.snapshot(),
            slot: .forWindow(window),
            deviceClass: .current
        ))
        _scope = State(initialValue: context.gameIdentity == nil ? .allGames : .thisGame)
    }

    private nonisolated static let rootSpace = "ARMSX2iOSPadEditorRoot"

    /// No preset in effect: the editor works on the INI working copy.
    private var editsCurrentLayout: Bool { context.presetID == nil && context.initialSnapshot == nil }

    private var analogStickScale: CGFloat {
        CGFloat(SettingsStore.clampedAnalogStickScale(settings.controller.analogStickScale))
    }

    /// Built-in presets and a game on the default layout save as new instead, and so does a game on the
    /// Current Layout or the default preset, which every game without its own layout shares.
    private var canUpdate: Bool {
        if context.gameIdentity != nil && (editsCurrentLayout || context.presetID == store.globalPresetID) { return false }
        return editsCurrentLayout || store.preset(id: context.presetID).map { $0.source != .builtIn } ?? false
    }

    /// An imported V2 skin plays its own layout, so the editor draws the built-in look under a banner.
    private var overridingSkin: VPadSkinDescriptor? {
        let skin = context.skinDescriptor ?? VPadSkinLibraryStore.shared.selectedDescriptor
        return skin.source == .imported && skin.manifestVersion == 2 ? skin : nil
    }

    /// The layout on screen. A game on the default layout opens that preset with presetID nil, so Update stays off.
    private var openID: String? { context.presetID ?? context.effectivePresetID }

    private var layoutName: String {
        store.preset(id: openID)?.displayName ?? settings.localized("Current Layout")
    }

    var body: some View {
        GeometryReader { geo in
            let screen = CGSize(
                width: geo.size.width + geo.safeAreaInsets.leading + geo.safeAreaInsets.trailing,
                height: geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom
            )
            let windowSlot = PadEditorSlot.forWindow(screen)
            let editing = pickedSlot ?? windowSlot
            ZStack(alignment: .top) {
                OverlayTheme.scrimBase.opacity(backdrop == .pausedGame ? 0 : 1).ignoresSafeArea()
                if editing != windowSlot {
                    preview(editing, screen: screen)
                } else if windowSlot.isLandscape {
                    PadEditorGameArea(backdrop: backdrop, outlined: false).ignoresSafeArea()
                    measured(padArea(landscape: true, fit: nil, largeLayout: usesLargeLayout(windowSize: screen)))
                        .padding(landscapePadInsets(geo, screen: screen))
                        .ignoresSafeArea()
                } else {
                    portraitCanvas(geo, largeLayout: usesLargeLayout(windowSize: screen))
                }
                if drop != nil {
                    Color.clear.contentShape(Rectangle()).onTapGesture { drop = nil }
                }
                piecePanel(geo)
                if !hintShown && padFrame != .zero && !editor.trying { firstRunHint }
                barStack(size: geo.size, windowSlot: windowSlot)
                if controllerInput?.hasConnectedController == true, drop == nil, !panelFocused {
                    PadEditorControllerHint(parts: hintParts)
                        .frame(maxHeight: .infinity, alignment: .bottom)
                        .padding(.bottom, 8)
                        .allowsHitTesting(false)
                }
            }
            .coordinateSpace(.named(Self.rootSpace))
            // No mask that follows the hint: one that changes mid-touch cancels the drag that first touch starts.
            .simultaneousGesture(DragGesture(minimumDistance: 0).onChanged { _ in dismissHint() })
            .onChange(of: editor.selection) { _, selection in
                if selection != nil { dismissHint() }
                panelFocused = false
            }
            .onChange(of: editing, initial: true) { _, slot in
                editor.switchSlot(to: slot)
            }
        }
        .onAppear {
            controllerInput?.registerNavigationSession(
                id: controllerSession, scopeKey: "pad.editor", priority: 800, isReady: true,
                handler: { handle($0) }, entryHandler: { _ in true }
            )
            _ = controllerInput?.requestNavigationSessionEntry(preferLast: false, matchingScopePrefix: "pad.editor")
        }
        .onDisappear {
            controllerInput?.unregisterNavigationSession(id: controllerSession)
            endTrying()
        }
        .onChange(of: drop) { _, drop in
            if drop != nil { _ = controllerInput?.requestNavigationSessionEntry(preferLast: false, matchingScopePrefix: "pad.editor.drop") }
        }
        .onChange(of: panelFocused) { _, focused in
            if focused { _ = controllerInput?.requestNavigationSessionEntry(preferLast: false, matchingScopePrefix: "pad.editor.panel") }
        }
        // A sheet's swipe-down would skip the question Cancel asks.
        .interactiveDismissDisabled(editor.isDirty)
        .onChange(of: preferences.snap, initial: true) { editor.snapEnabled = preferences.snap }
        .onChange(of: preferences.mirror, initial: true) { editor.mirrorEnabled = preferences.mirror }
        .preferredColorScheme(.dark)
        // Otherwise a keyboard shortens the editor and portrait gets the landscape layout.
        .ignoresSafeArea(.keyboard)
        .navigationBarHidden(true)
        .statusBarHidden()
        .statusColumnDisabled()
        .persistentSystemOverlays(.hidden)
        // SwiftUI has no item-only alert that holds a text field, so the request drives isPresented.
        .alert(
            nameRequest?.title ?? "",
            isPresented: Binding(get: { nameRequest != nil }, set: { if !$0 { nameRequest = nil } }),
            presenting: nameRequest
        ) { request in
            TextField(request.field, text: $nameDraft)
            Button(request.confirm) { named(request.purpose) }
                .disabled(nameDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            Button(request.cancel, role: .cancel) {}
        }
        .fullScreenCover(item: $keyboardRequest, onDismiss: {
            // After the keyboard has gone, since saving can close the editor's own cover.
            if let purpose = typedName { named(purpose) }
            typedName = nil
        }) { request in
            OrbitKeysKeyboardView(title: request.title, initialText: nameDraft, startsInNormalKeyboard: true) { name in
                if !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                    nameDraft = name
                    typedName = request.purpose
                }
                keyboardRequest = nil
            } onCancel: {
                keyboardRequest = nil
            }
            .presentationBackground(.clear)
            .appStatusBarHidden()
        }
    }

    // MARK: Controller

    /// The editor's own commands; an open card and the focused panel sit 10 above and take theirs first.
    private func handle(_ command: MenuControllerCommand) -> Bool {
        // The name alert has the screen and no pad input reaches it.
        guard nameRequest == nil else { return true }
        if editor.trying {
            if command == .back { endTrying() }
            return true
        }
        switch command {
        case .up, .upRight, .right, .downRight, .down, .downLeft, .left, .upLeft:
            let dx: CGFloat = command.horizontalComponent == .right ? 1 : command.horizontalComponent == .left ? -1 : 0
            let dy: CGFloat = command.verticalComponent == .down ? 1 : command.verticalComponent == .up ? -1 : 0
            if let id = editor.selection {
                editor.nudge(id, dx: dx, dy: dy)
            } else {
                let middle = CGPoint(x: editor.area.size.width / 2, y: editor.area.size.height / 2)
                editor.selection = editor.neighbour(from: middle, direction: CGVector(dx: dx, dy: dy)) ?? editor.readingOrder().first
            }
        case .previousTab, .nextTab:
            editor.cycle(forward: command == .nextTab)
        case .activate:
            if editor.selection == nil { editor.selection = editor.readingOrder().first } else { panelFocused = true }
        case .back:
            if editor.selection != nil {
                editor.selection = nil
            } else if drop != nil {
                drop = nil
            } else {
                cancel()
            }
        case .showContextMenu:
            drop = .more
        case .toggleFavorite:
            done()
        }
        return true
    }

    private var hintParts: [ControllerHintLine.Part] {
        let l = { settings.localized($0) }
        if editor.trying { return [.init(.circle, l("Stop"))] }
        guard editor.selection != nil else {
            return [.init(.dpadUp, l("Select")), .init(.triangle, l("Done")), .init(.square, l("More")), .init(.circle, l("Cancel"))]
        }
        return [.init(.dpadUp, l("Move")), .init(.l1, l("Previous")), .init(.r1, l("Next")),
                .init(.cross, l("Options")), .init(.circle, l("Deselect"))]
    }

    private func askName(_ request: PadEditorNameRequest) {
        if controllerInput?.hasConnectedController == true { keyboardRequest = request } else { nameRequest = request }
    }

    // MARK: Canvas

    private func portraitCanvas(_ geo: GeometryProxy, largeLayout: Bool) -> some View {
        let deckHeight = geo.size.height + geo.safeAreaInsets.bottom
        let fold = geo.activeFold(book: false)
        let key = [geo.size.width, geo.size.height, fold?.minY, fold?.height, CGFloat(editor.refitGeneration)]
        let live = portraitGameSplit(
            geo,
            deckHeight: deckHeight,
            fold: fold,
            refitting: AppState.shared.hingePresent ? editor.renderSnapshot : nil,
            analogStickScale: settings.controller.analogStickScale
        )
        let split = heldSplit.flatMap { $0.key == key ? ($0.gameHeight, $0.fit) : nil } ?? (live.gameHeight, live.fit)
        return VStack(spacing: 0) {
            PadEditorGameArea(backdrop: backdrop, labelTop: barFrame.maxY).frame(height: split.0)
            if let fold { scrim.frame(height: fold.height) }
            ZStack {
                Color.black
                measured(padArea(landscape: false, fit: split.1, largeLayout: largeLayout))
            }
        }
        .ignoresSafeArea([.container, .keyboard], edges: .bottom)
        .onChange(of: key, initial: true) {
            heldSplit = (key, live.gameHeight, live.fit)
        }
    }

    private var scrim: some View {
        OverlayTheme.scrimBase.opacity(reduceTransparency ? 0.8 : 0.6)
    }

    private func preview(_ slot: PadEditorSlot, screen: CGSize) -> some View {
        let pad = PadEditorPreview<EmptyView>.padSize(slot, screen: screen)
        let scale = PadEditorPreview<EmptyView>.scale(pad, screen: screen)
        // Duo portrait play fits the layout to its deck, so this preview fits it too, held as the canvas holds it.
        let key: [CGFloat?] = [pad.width, pad.height, nil, nil, CGFloat(editor.refitGeneration)]
        let live = AppState.shared.hingePresent && slot == .portrait
            ? PadPortraitFit(envelope: editor.renderSnapshot.portraitEnvelope(analogStickScale: analogStickScale), deck: pad, barBase: 0)
            : nil
        return measured(PadEditorPreview(slot: slot, pad: pad, scale: scale) {
            padArea(landscape: slot.isLandscape, fit: heldSplit?.key == key ? heldSplit?.fit : live,
                    largeLayout: usesLargeLayout(windowSize: screen), scale: scale)
        })
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(scrim)
        .ignoresSafeArea()
        .onChange(of: key, initial: true) {
            heldSplit = (key, 0, live)
        }
    }

    /// Outside any scale effect, so the frame is the one on screen.
    private func measured(_ pad: some View) -> some View {
        pad.onGeometryChange(for: CGRect.self) { $0.frame(in: .named(Self.rootSpace)) } action: { padFrame = $0 }
    }

    private func padArea(landscape: Bool, fit: PadPortraitFit?, largeLayout: Bool, scale: CGFloat = 1) -> some View {
        GeometryReader { pad in
            ZStack {
                // In try mode the pad is drawn as play draws it and takes touches, which the bridge mutes.
                VirtualControllerView(
                    isLandscape: landscape,
                    layoutSnapshot: editor.trying ? editor.shownSnapshot() : editor.drawnSnapshot(),
                    skinDescriptor: overridingSkin == nil ? context.skinDescriptor : VPadSkinLibraryStore.defaultDescriptor,
                    largeLayout: largeLayout,
                    fitScale: fit?.scale
                )
                .allowsHitTesting(editor.trying)
                .accessibilityHidden(!editor.trying)
                PadEditorMarks(editor: editor, showGrid: preferences.grid && !editor.trying, showTouchAreas: touchAreas || editor.trying)
                if !editor.trying { PadEditorPieces(editor: editor) }
            }
            // A preview is not on the device's screen, so no fold crosses it.
            .onChange(
                of: PadArea(
                    size: pad.size, fit: fit, analogStickScale: analogStickScale,
                    fold: scale < 1 ? nil : pad.activeFold(book: true), scale: scale
                ),
                initial: true
            ) { _, area in
                editor.area = area
            }
        }
    }

    // MARK: Piece panel

    private func piecePanel(_ geo: GeometryProxy) -> some View {
        let shown = editor.dragging || drop != nil ? nil : editor.selection
        let width = min(340, geo.size.width - 32)
        return ZStack {
            if let id = shown {
                let room = dockRoom(id, width: width, geo)
                PadPiecePanel(
                    editor: editor, id: id, controllerInput: controllerInput, focused: panelFocused, maxHeight: room
                ) { panelFocused = false }
                    .frame(width: width)
                    .onGeometryChange(for: CGSize.self) { $0.size } action: { panelSize = $0 }
                    .position(dock(id, geo, size: CGSize(width: width, height: min(panelSize == .zero ? 300 : panelSize.height, room))))
                    .transition(.opacity)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: shown)
    }

    /// A frame in pad points as it sits on screen, in the root space.
    private func onScreen(_ f: CGRect) -> CGRect {
        let s = editor.area.scale
        return CGRect(x: padFrame.minX + f.minX * s, y: padFrame.minY + f.minY * s, width: f.width * s, height: f.height * s)
    }

    /// The panel's height under the bar. With no room to either side of the selected piece it takes the taller
    /// gap above or below it and scrolls, since a panel that fits neither is put on the pad's centre, over the piece.
    /// A card can't cross a tabletop fold, so the room ends at the fold on the piece's side of it.
    private func dockRoom(_ id: String, width: CGFloat, _ geo: GeometryProxy) -> CGFloat {
        var top = barFrame.maxY, bottom = geo.size.height
        guard let s = editor.geometries().first(where: { $0.id == id }).map({ onScreen($0.visibleFrame) }) else {
            return max(160, bottom - top - 16)
        }
        if let fold = geo.activeFold(book: false) {
            if s.midY > fold.midY { top = max(top, fold.maxY) } else { bottom = fold.minY }
        }
        // 32 holds the keep-out round the piece, the side margin and one step of the docking grid.
        guard max(s.minX, geo.size.width - s.maxX) - 32 < width else { return max(160, bottom - top - 16) }
        return max(160, min(bottom - top - 16, max(s.minY - top, bottom - s.maxY) - 32))
    }

    private func dock(_ id: String, _ geo: GeometryProxy, size: CGSize) -> CGPoint {
        let frames = editor.geometries().map { ($0.id, onScreen($0.visibleFrame)) }
        let centre = CGPoint(x: padFrame.midX, y: padFrame.midY)
        guard let selected = frames.first(where: { $0.0 == id })?.1 else { return centre }
        return PadPiecePanel.dock(
            size,
            top: barFrame.maxY + 8,
            bounds: geo.size,
            selected: selected,
            others: frames.filter { $0.0 != id }.map(\.1),
            fold: geo.activeFold(),
            fallback: centre
        )
    }

    // MARK: Bar and cards

    private func barStack(size: CGSize, windowSlot: PadEditorSlot) -> some View {
        let side = barSide(width: size.width)
        let room = max(160, size.height - barFrame.maxY - 20)
        return VStack(spacing: 8) {
            PadEditorBar(
                editor: editor,
                title: layoutName,
                subtitle: editor.slot.title,
                onCancel: cancel,
                onTitle: { drop = drop == .layouts ? nil : .layouts },
                onReset: { drop = drop == .reset ? nil : .reset },
                onMore: { drop = drop == .more ? nil : .more },
                onDone: done,
                onStopTrying: endTrying
            )
            .frame(maxWidth: barRoom(width: size.width))
            .onGeometryChange(for: CGRect.self) { $0.frame(in: .named(Self.rootSpace)) } action: { barFrame = $0 }
            // Reduce Motion fades the bar to its new corner instead of sliding it there.
            .id(reduceMotion ? side : 0)
            .transition(.opacity)
            if let open = drop {
                card(open, room: room, windowSlot: windowSlot).padEditorDropScope(controllerInput) { drop = nil }
            } else if let skin = overridingSkin, !editor.dragging, !editor.trying {
                PadEditorSkinBanner(name: skin.displayName) { askDefaultSkin(skin) }
                    .frame(width: barFrame.width > 0 ? barFrame.width : nil)
            }
        }
        .frame(maxWidth: .infinity, alignment: side < 0 ? .leading : side > 0 ? .trailing : .center)
        .padding(.horizontal, 16)
        .padding(.top, 8)
        .animation(.easeInOut(duration: 0.2), value: side)
    }

    @ViewBuilder private func card(_ open: Drop, room: CGFloat, windowSlot: PadEditorSlot) -> some View {
        switch open {
        case .save: saveCard(maxHeight: room)
        case .more:
            PadEditorMoreCard(
                editor: editor,
                maxHeight: room,
                snap: preferences.$snap,
                grid: preferences.$grid,
                mirror: preferences.$mirror,
                touchAreas: $touchAreas,
                onLayouts: { drop = .layouts },
                onReset: { drop = .reset },
                onTry: startTrying
            )
        case .reset: PadEditorResetCard(editor: editor, maxHeight: room) { drop = nil }
        case .layouts:
            PadEditorLayoutsCard(
                presets: store.presets,
                openID: openID,
                canRename: store.preset(id: openID).map { $0.source != .builtIn } ?? false,
                readOnly: store.readOnly,
                deviceSlot: windowSlot,
                maxHeight: room,
                slot: Binding(get: { editor.slot }, set: { pickedSlot = $0 == windowSlot ? nil : $0 }),
                onPick: pick,
                onDuplicate: duplicateTapped,
                onRename: renameTapped
            )
        }
    }

    /// -1 or 1 while a dragged piece comes near the bar, which then steps to the far top corner.
    private func barSide(width: CGFloat) -> Int {
        // Against the centred bar, so stepping aside cannot undo itself.
        let centred = CGRect(x: (width - barFrame.width) / 2, y: barFrame.minY, width: barFrame.width, height: barFrame.height)
        guard editor.dragging, let frame = editor.dragFrame.map(onScreen),
              frame.insetBy(dx: -8, dy: -8).intersects(centred) else { return 0 }
        return frame.midX > width / 2 ? -1 : 1
    }

    /// The gap between pieces along the top edge, such as the landscape shoulders, so the bar sits clear of them.
    private func barRoom(width: CGFloat) -> CGFloat {
        guard editor.slot.isLandscape else { return .infinity }
        let band = editor.geometries()
            .map { onScreen($0.visibleFrame) }
            .filter { $0.minY < 72 }
        let left = band.filter { $0.midX < width / 2 }.map(\.maxX).max() ?? 0
        let right = band.filter { $0.midX >= width / 2 }.map(\.minX).min() ?? width
        let room = right - left - 16
        return room >= 300 ? room : .infinity
    }

    // MARK: First-run hint and V2 skin

    private var firstRunHint: some View {
        let top = editor.geometries().map { onScreen($0.visibleFrame).minY }.min() ?? padFrame.midY
        return Text(settings.localized("Drag to move. Pinch or drag the corner to resize."))
            .font(.subheadline)
            .foregroundStyle(OverlayTheme.textPrimary)
            .multilineTextAlignment(.center)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(OverlayFrostBackground().clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous)))
            .frame(maxWidth: 360)
            .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { hintHeight = $0 }
            // Kept below the bar when the top pieces leave no room above them.
            .position(x: padFrame.midX, y: max(top - 32 - hintHeight / 2, barFrame.maxY + 8 + hintHeight / 2))
            .allowsHitTesting(false)
            .transition(.opacity)
    }

    private func dismissHint() {
        guard !hintShown else { return }
        withAnimation(.easeOut(duration: 0.3)) { hintShown = true }
    }

    private func askDefaultSkin(_ skin: VPadSkinDescriptor) {
        ControllerPrompt.shared.ask(
            settings.localized("Use the default skin?"),
            message: String(
                format: settings.localized("“%@” stops being used here. You can pick it again in Settings > Virtual Pad."),
                skin.displayName
            ),
            actions: [
                .init(title: settings.localized("Use Default Skin"), run: { useDefaultSkin(replacing: skin) }),
                .cancel,
            ]
        )
    }

    /// A skin setting, outside the draft, so Discard keeps it.
    private func useDefaultSkin(replacing skin: VPadSkinDescriptor) {
        let skins = VPadSkinLibraryStore.shared, builtIn = VPadSkinLibraryStore.defaultDescriptor
        if let identity = context.gameIdentity,
           (store.skinID(for: identity) ?? store.automaticSkinID(forSerial: identity.serial)) == skin.id {
            store.setSkin(builtIn.id, for: identity, using: skins)
        } else {
            skins.selectSkin(id: builtIn.id)
            settings.controller.virtualPadSkin = skins.selectedDescriptor.virtualPadSkin
        }
        context.skinDescriptor = builtIn
    }

    private func startTrying() {
        editor.endDrag()
        editor.selection = nil
        drop = nil
        editor.trying = true
        EmulatorBridge.shared.muted = true
    }

    /// Every way out of try mode and out of the editor comes here, so the game never stays deaf.
    private func endTrying() {
        editor.trying = false
        EmulatorBridge.shared.muted = false
        EmulatorBridge.shared.resetVirtualPadAnalogInput()
    }

    private func close() {
        endTrying()
        onDismiss()
    }

    private func cancel() {
        endTrying()
        guard editor.isDirty else {
            close()
            return
        }
        editor.endDrag()
        ControllerPrompt.shared.ask(
            settings.localized("Save your changes?"),
            message: String(format: settings.localized("Discard puts “%@” back the way it was."), layoutName),
            actions: [
                .init(title: settings.localized("Save Layout…"), run: { done() }),
                .init(title: settings.localized("Discard Changes"), isDestructive: true, run: { close() }),
                .init(title: settings.localized("Keep Editing"), isCancel: true),
            ]
        )
    }

    private func done() {
        endTrying()
        guard editor.isDirty || switched else {
            close()
            return
        }
        editor.endDrag()
        editor.selection = nil
        saveAsNew = !canUpdate
        drop = .save
    }

    private func saveCard(maxHeight: CGFloat) -> some View {
        PadEditorSaveCard(
            updateLabel: !canUpdate ? nil : editsCurrentLayout
                ? settings.localized("Update Current Layout")
                : String(format: settings.localized("Update “%@”"), layoutName),
            hasGame: context.gameIdentity != nil,
            globalName: store.preset(id: store.globalPresetID)?.displayName ?? settings.localized("Current Layout"),
            readOnly: store.readOnly && (saveAsNew || !editsCurrentLayout),
            maxHeight: maxHeight,
            saveAsNew: $saveAsNew,
            scope: $scope,
            onBack: { drop = nil },
            onSave: saveTapped
        )
    }

    private func saveTapped() {
        guard saveAsNew else {
            save(.update)
            return
        }
        nameDraft = String(format: settings.localized("Layout %d"), store.presets.count + 1)
        askName(PadEditorNameRequest(
            purpose: .saveAsNew,
            title: settings.localized("Name this layout"),
            field: settings.localized("Layout Name"),
            confirm: settings.localized("Save Layout"),
            cancel: settings.localized("Cancel")
        ))
    }

    private func save(_ target: PadEditorSaveTarget) {
        do {
            try editor.save(target: target, scope: scope, context: context, store: store, layout: PadLayoutStore.shared)
            close()
        } catch {
            saveFailed(error)
        }
    }

    private func saveFailed(_ error: Error) {
        NSLog("[ARMSX2 iOS VPad] Layout save failed: %@", error.localizedDescription)
        ControllerPrompt.shared.ask(
            settings.localized("Couldn't save the layout"),
            message: settings.localized("Check that the device has free space, then try again."),
            actions: [.init(title: settings.localized("Keep Editing"), isCancel: true)]
        )
    }

    // MARK: Layouts card

    /// Loads the layout as this device class plays it, as a new draft, and makes it what Update saves to.
    private func pick(_ preset: PadLayoutPreset?) {
        guard preset?.id != openID else {
            drop = nil
            return
        }
        let open: @MainActor () -> Void = {
            let snapshot = preset?.snapshot(for: editor.deviceClass)
            editor.load(snapshot ?? PadLayoutStore.shared.snapshot())
            context.presetID = preset?.id
            context.effectivePresetID = preset?.id
            context.initialSnapshot = snapshot
            switched = true
            drop = nil
        }
        guard editor.isDirty else {
            open()
            return
        }
        ControllerPrompt.shared.ask(
            String(format: settings.localized("Switch to “%@”?"), preset?.displayName ?? settings.localized("Current Layout")),
            message: String(format: settings.localized("Your changes to “%@” aren't saved."), layoutName),
            actions: [
                .init(title: settings.localized("Discard and Switch"), isDestructive: true, run: open),
                .init(title: settings.localized("Keep Editing"), isCancel: true),
            ]
        )
    }

    private func duplicateTapped() {
        nameDraft = String(format: settings.localized("%@ Copy"), layoutName)
        askName(PadEditorNameRequest(
            purpose: .duplicate,
            title: settings.localized("Name this layout"),
            field: settings.localized("Layout Name"),
            confirm: settings.localized("Duplicate Layout"),
            cancel: settings.localized("Cancel")
        ))
    }

    private func renameTapped() {
        nameDraft = layoutName
        askName(PadEditorNameRequest(
            purpose: .rename,
            title: settings.localized("Rename layout"),
            field: settings.localized("Layout Name"),
            confirm: settings.localized("Rename Layout"),
            cancel: settings.localized("Cancel")
        ))
    }

    private func named(_ purpose: PadEditorNameRequest.Purpose) {
        do {
            switch purpose {
            case .saveAsNew:
                save(.saveAsNew(name: nameDraft))
            case .duplicate:
                try editor.duplicate(openID, named: nameDraft, store: store, layout: PadLayoutStore.shared)
            case .rename:
                if let id = openID { try editor.rename(id, to: nameDraft, store: store) }
            }
        } catch {
            saveFailed(error)
        }
    }
}

private struct PadEditorSkinBanner: View {
    let name: String
    let onUseDefault: () -> Void
    private let settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accent

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "paintpalette").font(.body).foregroundStyle(OverlayTheme.warm)
                .accessibilityHidden(true)
            Text(String(format: settings.localized("“%@” sets its own layout in game. You're editing the layout underneath it."), name))
                .font(.subheadline)
                .foregroundStyle(OverlayTheme.textPrimary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            Button(action: onUseDefault) {
                Text(settings.localized("Use Default Skin")).font(.body).foregroundStyle(accent)
                    .frame(minHeight: 44).contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .fixedSize()
        }
        .padding(12)
        .dynamicTypeSize(...DynamicTypeSize.accessibility3)
        .background(OverlayFrostBackground().clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous)))
    }
}
