// QuickMenuView.swift — In-game pause menu card
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Native in-game pause menu: a clear Liquid Glass "command deck" presented by the host as a
/// bounded card over a controlled dim of the paused gameplay. The host pauses the VM while this
/// card is shown. Toggles are bound directly; everything else is routed back to the host through
/// closures so existing panels, child-screen routing, and confirmation flows remain unchanged.
///
/// Layout adapts to the card's geometry (read via a GeometryReader, since the host frames this
/// view to the bounded card size): two columns when the card is comfortably wide, one column
/// otherwise. Resume is pinned inside the panel footer so it never detaches or hides rows.
struct QuickMenuView: View {
    @State private var showStopConfirmation = false
    @State private var stopConfirmationAnchor: StopConfirmationAnchor?
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.overlayFoldSplit) private var foldSplit
    @Environment(\.layoutDirection) private var layoutDirection

    let settings: SettingsStore
    @Binding var padVisible: Bool
    @Binding var fullScreen: Bool
    @Binding var menuButtonHidden: Bool

    let vmMenuAvailable: Bool
    let gameMenuAvailable: Bool
    let virtualPadHiddenByController: Bool
    let shaderChainAvailable: Bool
    let gameTitle: String?
    let variant: PauseLayoutVariant
    /// The child just closed, whose row gets focus back instead of Resume.
    let returningFrom: QuickMenuDestination?
    /// The last focus scope and size class, held by GameScreenView so a fold keeps the row.
    @Binding var foldMemory: QuickMenuFoldMemory?
    let activePadLayoutName: String
    let activeControllerSkinName: String

    let onCycleOSD: () -> Void
    let onOpen: (QuickMenuDestination) -> Void
    let onClearCache: () -> Void
    let onBackToMenu: () -> Void
    let onStop: () -> Void
    let onResume: () -> Void

    /// Compact sizing for the header/footer on iPad (any orientation) and iPhone landscape; the
    /// larger, liked sizing is reserved for iPhone portrait.
    private var compact: Bool { variant != .phonePortrait }

    /// Layout editing is a direct-manipulation touch surface. Do not advertise
    /// a controller destination that cannot own controller navigation.
    private var hidesPadLayoutEditorForController: Bool {
        controllerInput?.isControllerNavigationEnabled == true
    }

    private typealias ControllerTargetID = QuickMenuNavigationGraph.ControllerTargetID

    private func navigationGraph(twoColumns: Bool) -> QuickMenuNavigationGraph {
        QuickMenuNavigationGraph(
            vmMenuAvailable: vmMenuAvailable,
            gameMenuAvailable: gameMenuAvailable,
            shaderChainAvailable: shaderChainAvailable,
            hidesPadLayoutEditorForController: hidesPadLayoutEditorForController,
            layoutDirection: layoutDirection,
            twoColumns: twoColumns
        )
    }

    var body: some View {
        // The host (GameOverlayContainer) frames this view to the bounded card size; read that
        // size here to decide whether the geometry comfortably supports two columns.
        GeometryReader { geo in
            // Keep the navigation graph aligned with the view tree selected by
            // `overlayBody`. In particular, iPad uses the portrait-style body
            // even when its measured card is wider than it is tall.
            let usesLandscapeMap = variant == .phoneLandscape
            let large = variant == .ipadTwoColumn
            let spread = foldSplit?.vertical == true
            // After a fold, the row a controller had selected instead of Resume.
            let foldFocus = foldMemory.flatMap { memory in
                memory.folded || memory.large != large || memory.pose != foldPose
                    ? controllerInput?.rememberedNavigationFocusKey(forScope: memory.scope) : nil
            }
            let usesTwoColumns = variant.supportsTwoColumns(
                width: geo.size.width,
                height: geo.size.height
            )
            let graph = navigationGraph(twoColumns: usesTwoColumns)
            ZStack {
                overlayBody(
                    width: geo.size.width,
                    height: geo.size.height,
                    twoColumns: usesTwoColumns
                )
                    .accessibilityHidden(showStopConfirmation)
                    .environment(
                        \.controllerAccessibilityTargetsSuppressed,
                        showStopConfirmation
                    )

                if showStopConfirmation {
                    StopGameConfirmation(
                        gameTitle: GameListView.cleanGameFileName(gameTitle ?? ""),
                        anchor: stopConfirmationAnchor,
                        selectedIndex: 0,
                        stopFeedback: .silent,
                        onSelect: { index in
                            if index == 0 {
                                showStopConfirmation = false
                            } else {
                                onStop()
                            }
                        },
                        onDismiss: {
                            showStopConfirmation = false
                        }
                    )
                }
            }
                .environment(\.stopConfirmationPresented, showStopConfirmation)
                .onPreferenceChange(StopConfirmationAnchorKey.self) { stopConfirmationAnchor = $0 }
                .controllerAccessibilityTargetOrder(
                    showStopConfirmation
                        ? []
                        : usesLandscapeMap
                            ? graph.quickMenuLandscapeControllerTargetOrder()
                            : graph.quickMenuPortraitControllerTargetOrder()
                )
                .controllerAccessibilityNavigation(
                    controllerInput: controllerInput,
                    scopeKey: quickMenuControllerScopeKey(
                        usesLandscapeMap: usesLandscapeMap
                    ),
                    priority: 180,
                    orbStyle: .liquidGlass,
                    onBack: {
                        if showStopConfirmation {
                            showStopConfirmation = false
                        } else {
                            onResume()
                        }
                        return true
                    },
                    directionalLinks: showStopConfirmation
                        ? []
                        : spread
                            ? graph.quickMenuSpreadDirectionalLinks()
                        : usesLandscapeMap
                            ? graph.quickMenuLandscapeDirectionalLinks()
                            : graph.quickMenuPortraitDirectionalLinks(),
                    prioritizesDirectionalLinks: !showStopConfirmation,
                    // Quick Menu publishes a complete semantic graph. Giving
                    // UIKit a second focus owner makes the fixed portrait footer
                    // compete geometrically with the scrolling rows on iOS 26.
                    usesExplicitTargetGeometryOnly: true,
                    preservesFocusDuringRightStickScrolling: false,
                    focusScrollBehavior: .maintainWithinViewport,
                    // Held directions repeat every 100 ms. Finish each Quick
                    // Menu reveal before the following repeat can replace it.
                    scrollAnimationDuration: 0.08,
                    focusTopAlignmentMargin: 24,
                    focusBottomAlignmentMargin: 96,
                    onActivateFocusedLabel: { label in
                        guard usesLandscapeMap,
                              !showStopConfirmation else { return false }
                        return activateQuickMenuControllerLabel(label)
                    },
                    preferredInitialFocusLabel:
                        showStopConfirmation
                            ? settings.localized("Cancel")
                            : returningFrom.map(controllerTargetID(for:)) ?? foldFocus
                                ?? ControllerTargetID.resume
                )
                .task(
                    id: quickMenuControllerScopeKey(
                        usesLandscapeMap: usesLandscapeMap
                    )
                ) {
                    // Registration is deliberately passive. Ask the router to
                    // enter this exact surface; it retains the request until
                    // Resume has mounted and can be focused immediately.
                    await Task.yield()
                    guard !Task.isCancelled else { return }
                    _ = controllerInput?.requestNavigationSessionEntry(
                        preferLast: false,
                        matchingScopePrefix: "quick-pause."
                    )
                }
                .onChange(
                    of: QuickMenuFoldMemory(
                        scope: quickMenuControllerScopeKey(usesLandscapeMap: usesLandscapeMap),
                        large: large,
                        pose: foldPose
                    ),
                    initial: true
                ) { _, now in rememberFoldScope(now) }
        }
        .onChange(of: showStopConfirmation) { _, isPresented in
            guard isPresented else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .environment(
            \.controllerTextAppearance,
            settings.controllerQuickMenuTextAppearance
        )
        // Overlay readability profiles assume the dark Liquid Glass variant;
        // keeping the scheme local avoids changing the game or library.
        .preferredColorScheme(.dark)
        .quickMenuLiquidGlassConfiguration()
    }

    private func controllerTargetID(for destination: QuickMenuDestination) -> String {
        switch destination {
        case .perGame: ControllerTargetID.perGame
        case .gameController: ControllerTargetID.gameController
        case .controllerSkin: ControllerTargetID.controllerSkin
        case .speed: ControllerTargetID.speed
        case .shaders: ControllerTargetID.shaders
        case .saveStates: ControllerTargetID.saveStates
        case .cheats: ControllerTargetID.cheats
        case .retroAchievements: ControllerTargetID.retroAchievements
        case .padLayout: ControllerTargetID.editPadLayout
        case .resetROM: ControllerTargetID.resetROM
        case .changeDisc: ControllerTargetID.changeDisc
        case .reportProblem: ControllerTargetID.reportProblem
        }
    }

    private func rememberFoldScope(_ now: QuickMenuFoldMemory) {
        guard !showStopConfirmation else { return }
        guard var memory = foldMemory else {
            foldMemory = now
            return
        }
        memory.folded = memory.folded || memory.large != now.large || memory.pose != now.pose
        if memory.folded, memory.scope != now.scope,
           let key = controllerInput?.rememberedNavigationFocusKey(forScope: memory.scope) {
            controllerInput?.rememberNavigationFocusKey(key, forScope: now.scope)
        }
        memory.scope = now.scope
        memory.large = now.large
        memory.pose = now.pose
        foldMemory = memory
    }

    private func quickMenuControllerScopeKey(
        usesLandscapeMap: Bool
    ) -> String {
        let layout = (usesLandscapeMap ? "landscape" : "portrait") + foldPose
        let availability = "\(vmMenuAvailable ? 1 : 0)\(gameMenuAvailable ? 1 : 0)"
        let alert = showStopConfirmation ? "stop-confirmation" : "content"
        return "quick-pause.\(layout).\(availability).\(alert)"
    }

    private var foldPose: String {
        foldSplit.map { $0.vertical ? ".book" : ".tabletop" } ?? ""
    }

    /// The landscape graph owns activation by the same semantic label used to
    /// draw focus. This prevents a transient replacement probe from invoking a
    /// neighboring row (most visibly the fixed Stop button).
    private func activateQuickMenuControllerLabel(_ label: String) -> Bool {
        func matches(_ key: String) -> Bool {
            label.compare(
                settings.localized(key),
                options: [.caseInsensitive, .diacriticInsensitive]
            ) == .orderedSame
        }

        if label == ControllerTargetID.stop || matches("Stop") {
            showStopConfirmation = true
        } else if label == ControllerTargetID.resume || matches("Resume") {
            onResume()
        } else if matches("OSD") {
            onCycleOSD()
        } else if matches("Virtual Pad") {
            padVisible.toggle()
        } else if matches("Full Screen") {
            fullScreen.toggle()
        } else if matches("Hide Menu Button") {
            let hidden = !settings.hideMenuButton
            menuButtonHidden = hidden
            settings.hideMenuButton = hidden
        } else if matches("Speed / Fast Forward") {
            onOpen(.speed)
        } else if matches("Per-Game Settings") {
            onOpen(.perGame)
        } else if matches("Game Controller") {
            onOpen(.gameController)
        } else if matches("Controller Skin") {
            onOpen(.controllerSkin)
        } else if matches("Edit Virtual Pad Layout") {
            onOpen(.padLayout)
        } else if matches("Save / Load States") {
            onOpen(.saveStates)
        } else if matches("Change Disc") {
            onOpen(.changeDisc)
        } else if matches("Shaders") {
            onOpen(.shaders)
        } else if matches("RetroAchievements") {
            onOpen(.retroAchievements)
        } else if matches("Cheats & Patches") {
            onOpen(.cheats)
        } else if matches("Report a Problem") {
            onOpen(.reportProblem)
        } else if matches("Reset ROM") {
            onOpen(.resetROM)
        } else if matches("Clear Current Game Cache") {
            onClearCache()
        } else if matches("Back to Menu") {
            onBackToMenu()
        } else {
            // Change Disc remains a native Menu and is activated by its
            // underlying UIKit accessibility control.
            return false
        }
        return true
    }

    @ViewBuilder
    private func overlayBody(
        width: CGFloat,
        height: CGFloat,
        twoColumns: Bool
    ) -> some View {
        if let foldSplit, foldSplit.vertical {
            spreadBody(foldSplit, hintTiles: footerHintTiles(width: width, height: height) != nil)
        } else if let foldSplit {
            pairBody(foldSplit, hintTiles: footerHintTiles(width: width, height: height) != nil)
        } else if variant == .phoneLandscape {
            landscapeBody(
                width: width,
                twoColumns: twoColumns
            )
        } else {
            portraitBody(
                width: width,
                height: height,
                twoColumns: twoColumns
            )
        }
    }

    @ViewBuilder
    private func portraitBody(
        width: CGFloat,
        height: CGFloat,
        twoColumns: Bool
    ) -> some View {
        OverlayPanelScaffold {
            VStack(spacing: 0) {
                OverlayHeader(
                    systemImage: "pause.circle.fill",
                    title: settings.localized("Paused"),
                    subtitle: gameTitle,
                    compact: compact
                )
                scrollContent(twoColumns: twoColumns, hintTiles: footerHintTiles(width: width, height: height))
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
    }

    @ViewBuilder
    private func landscapeBody(
        width: CGFloat,
        twoColumns: Bool
    ) -> some View {
        OverlayPanelScaffold {
            VStack(spacing: 0) {
                LandscapeCommandBar(
                    settings: settings,
                    gameTitle: gameTitle,
                    stopControllerNavigationID: ControllerTargetID.stop,
                    resumeControllerNavigationID: ControllerTargetID.resume,
                    onStop: { showStopConfirmation = true },
                    onResume: onResume,
                    iconOnly: width < 380
                )
                ScrollView {
                    quickMenuRightStickTarget
                    cardsContent(twoColumns: twoColumns)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .environment(\.overlayCompact, true)
    }

    /// The card opened at iPhone Duo's fold in book pose: one column per leaf.
    private func spreadBody(_ split: OverlayFoldSplit, hintTiles: Bool) -> some View {
        HStack(alignment: .top, spacing: split.second.minX - split.first.maxX) {
            foldLeaf(split.first.width, split.first.height, foldEdge: .trailing) {
                OverlayHeader(
                    systemImage: "pause.circle.fill",
                    title: settings.localized("Paused"),
                    subtitle: gameTitle,
                    compact: compact
                )
                ScrollView {
                    spreadRightStickTarget(leading: true)
                    VStack(spacing: 14) {
                        OverlaySectionCard(title: settings.localized("Quick Actions")) { quickActionsRows }
                        OverlaySectionCard(title: settings.localized("This Game")) { thisGameRows }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                }
                if hintTiles {
                    pauseFooter(hintTiles: .alone)
                }
            }
            foldLeaf(split.second.width, split.second.height, foldEdge: .leading) {
                ScrollView {
                    spreadRightStickTarget(leading: false)
                    VStack(spacing: 14) {
                        OverlaySectionCard(title: settings.localized("Game Tools")) { gameToolsRows }
                        OverlaySectionCard(title: settings.localized("Reset & Exit")) { resetAndExitRows }
                    }
                    .padding(.horizontal, 16)
                    .padding(.top, 16)
                    .padding(.bottom, 10)
                }
                pauseFooter(hintTiles: hintTiles ? .beside : nil)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    /// Tabletop: what is paused above the fold, every control below it.
    private func pairBody(_ split: OverlayFoldSplit, hintTiles: Bool) -> some View {
        VStack(spacing: split.second.minY - split.first.maxY) {
            foldLeaf(split.first.width, nil, foldEdge: .bottom) {
                OverlayHeader(
                    systemImage: "pause.circle.fill",
                    title: settings.localized("Paused"),
                    subtitle: gameTitle,
                    compact: compact
                )
                .frame(minHeight: 68)
                if hintTiles {
                    GameplayControllerShortcutHelpOverlay(settings: settings, controllerInput: controllerInput, inCard: true)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 4)
                        .overlay(alignment: .top) {
                            OverlayTheme.separator
                                .frame(height: 0.5)
                        }
                }
            }
            .fixedSize(horizontal: false, vertical: true)
            .frame(height: split.first.height, alignment: .bottom)
            foldLeaf(split.second.width, split.second.height, foldEdge: .top) {
                ScrollView {
                    quickMenuRightStickTarget
                    cardsContent(
                        twoColumns: variant.supportsTwoColumns(width: split.second.width, height: split.second.height)
                    )
                }
                pauseFooter(hintTiles: nil)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }

    /// One leaf of the opened card. Corners facing the fold are tighter, and the shadow
    /// stops at the fold's edge so nothing but the scrim lands on it.
    private func foldLeaf<Leaf: View>(
        _ width: CGFloat,
        _ height: CGFloat?,
        foldEdge: Edge,
        @ViewBuilder content: () -> Leaf
    ) -> some View {
        func radius(_ a: Edge, _ b: Edge) -> CGFloat { foldEdge == a || foldEdge == b ? 8 : 26 }
        let radii = RectangleCornerRadii(
            topLeading: radius(.top, .leading),
            bottomLeading: radius(.bottom, .leading),
            bottomTrailing: radius(.bottom, .trailing),
            topTrailing: radius(.top, .trailing)
        )
        return OverlayPanelScaffold(cornerRadii: radii) {
            VStack(spacing: 0) { content() }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .frame(width: width, height: height)
        .accessibilityElement(children: .contain)
        .clipShape(UnevenRoundedRectangle(cornerRadii: radii, style: .continuous))
        .shadow(color: .black.opacity(0.28), radius: 22, x: 0, y: 12)
        .modifier(LeafShadowMask(foldEdge: foldEdge))
    }

    @ViewBuilder
    private func cardsContent(twoColumns: Bool) -> some View {
        if twoColumns {
            HStack(alignment: .top, spacing: 12) {
                VStack(spacing: variant == .phoneLandscape ? 11 : 14) {
                    OverlaySectionCard(title: settings.localized("Quick Actions")) { quickActionsRows }
                    OverlaySectionCard(title: settings.localized("This Game")) { thisGameRows }
                }
                VStack(spacing: variant == .phoneLandscape ? 11 : 14) {
                    OverlaySectionCard(title: settings.localized("Game Tools")) { gameToolsRows }
                    OverlaySectionCard(title: settings.localized("Reset & Exit")) { resetAndExitRows }
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, variant == .phoneLandscape ? 8 : 10)
            .padding(.bottom, variant == .phoneLandscape ? 8 : 10)
        } else {
            VStack(spacing: 14) {
                OverlaySectionCard(title: settings.localized("Quick Actions")) { quickActionsRows }
                OverlaySectionCard(title: settings.localized("This Game")) { thisGameRows }
                OverlaySectionCard(title: settings.localized("Game Tools")) { gameToolsRows }
                OverlaySectionCard(title: settings.localized("Reset & Exit")) { resetAndExitRows }
            }
            .padding(.horizontal, 16)
            .padding(.top, variant == .phoneLandscape ? 8 : 10)
            .padding(.bottom, variant == .phoneLandscape ? 8 : 10)
        }
    }

    /// iPhone Duo's inner screen has no room for the floating shortcut bar under the card.
    private func footerHintTiles(width: CGFloat, height: CGFloat) -> QuickMenuFooter.HintTiles? {
        guard AppState.shared.hingePresent, variant == .ipadTwoColumn,
              controllerInput?.hasConnectedController == true,
              ControllerButtonGlyph.connectedGamepad != nil else { return nil }
        return width > height ? .halves : .row
    }

    @ViewBuilder
    private func scrollContent(twoColumns: Bool, hintTiles: QuickMenuFooter.HintTiles?) -> some View {
        // Keep the footer as a real layout sibling. A safe-area overlay leaves
        // the underlying scroll viewport focusable beneath the buttons, so a
        // row can receive controller focus while Stop/Resume visually cover it.
        VStack(spacing: 0) {
            ScrollView {
                quickMenuRightStickTarget
                cardsContent(twoColumns: twoColumns)
            }
            pauseFooter(hintTiles: hintTiles)
        }
    }

    private func pauseFooter(hintTiles: QuickMenuFooter.HintTiles?) -> some View {
        QuickMenuFooter(
            settings: settings,
            compact: compact,
            stopControllerNavigationID: ControllerTargetID.stop,
            resumeControllerNavigationID: ControllerTargetID.resume,
            onStop: { showStopConfirmation = true },
            onResume: onResume,
            hintTiles: hintTiles
        )
    }

    private func spreadRightStickTarget(leading: Bool) -> some View {
        QuickMenuLeafStickTarget(
            controllerInput: controllerInput,
            leadingRows: navigationGraph(twoColumns: false).spreadLeadingRows,
            leading: leading,
            isEnabled: !showStopConfirmation
        )
    }

    private var quickMenuRightStickTarget: some View {
        ControllerRightStickScrollTarget(
            controllerInput: controllerInput,
            axes: .vertical,
            priority: 180,
            isEnabled: !showStopConfirmation,
            pointsPerSecond: 680
        )
        .frame(height: 0)
    }

    // MARK: - Section row content

    @ViewBuilder private var quickActionsRows: some View {
        OverlayActionRow(
            controllerNavigationID: ControllerTargetID.osd,
            label: settings.localized("OSD"),
            systemImage: "speedometer",
            trailingValue: settings.localized(settings.osdPreset.label),
            action: onCycleOSD
        )
        .accessibilityHint(settings.localized("Cycles the on-screen display"))

        OverlayToggleRow(
            controllerNavigationID: ControllerTargetID.virtualPad,
            label: settings.localized("Virtual Pad"),
            systemImage: "gamecontroller",
            isOn: $padVisible
        )
            .accessibilityHint(settings.localized("Show or hide the on-screen controls"))

        if virtualPadHiddenByController {
            Text(settings.localized("Hidden while controller is connected"))
                .font(.caption)
                .foregroundStyle(
                    settings.controllerQuickMenuTextAppearance.secondaryColor
                        ?? OverlayTheme.textSecondary
                )
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.leading, OverlayTheme.rowLabelInset)
                .padding(.bottom, 4)
        }

        OverlayToggleRow(
            controllerNavigationID: ControllerTargetID.fullScreen,
            label: settings.localized("Full Screen"),
            systemImage: "arrow.up.left.and.arrow.down.right",
            isOn: $fullScreen
        )
            .accessibilityHint(settings.localized("Hide system bars and fill the screen"))

        OverlayToggleRow(
            controllerNavigationID: ControllerTargetID.hideMenuButton,
            label: settings.localized("Hide Menu Button"),
            systemImage: "eye.slash",
            isOn: Binding(
                get: { settings.hideMenuButton },
                set: { newValue in
                    menuButtonHidden = newValue
                    settings.hideMenuButton = newValue
                }
            )
        )
        .accessibilityHint(settings.localized("Tap the game area to show it briefly"))

        if vmMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.speed,
                label: settings.localized("Speed / Fast Forward"),
                systemImage: "forward.fill"
            ) {
                onOpen(.speed)
            }
        }
    }

    @ViewBuilder private var thisGameRows: some View {
        if gameMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.perGame,
                label: settings.localized("Per-Game Settings"),
                systemImage: "slider.horizontal.3"
            ) {
                onOpen(.perGame)
            }
            .accessibilityHint(settings.localized("Graphics, audio, CPU, pad, and fixes for this title"))
        }
        OverlayActionRow(
            controllerNavigationID: ControllerTargetID.controllerSkin,
            label: settings.localized("Controller Skin"),
            systemImage: "paintpalette",
            trailingValue: settings.localized(activeControllerSkinName)
        ) {
            onOpen(.controllerSkin)
        }
        if !hidesPadLayoutEditorForController {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.editPadLayout,
                label: settings.localized("Edit Virtual Pad Layout"),
                systemImage: "square.resize",
                trailingValue: activePadLayoutName
            ) {
                onOpen(.padLayout)
            }
        }
    }

    @ViewBuilder private var gameToolsRows: some View {
        if gameMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.gameController,
                label: settings.localized("Game Controller"),
                systemImage: "gamecontroller"
            ) {
                onOpen(.gameController)
            }
            .accessibilityHint(
                settings.localized(
                    "Open this game's controller settings with per-game overrides enabled"
                )
            )
        }
        if gameMenuAvailable || vmMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.saveStates,
                label: settings.localized("Save / Load States"),
                systemImage: "square.stack.3d.up.fill"
            ) {
                onOpen(.saveStates)
            }
        }
        if vmMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.changeDisc,
                label: settings.localized("Change Disc"),
                systemImage: "opticaldisc"
            ) {
                onOpen(.changeDisc)
            }
        }
        if gameMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.retroAchievements,
                label: settings.localized("RetroAchievements"),
                systemImage: "trophy.fill"
            ) {
                onOpen(.retroAchievements)
            }
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.cheats,
                label: settings.localized("Cheats & Patches"),
                systemImage: "rectangle.stack.badge.plus"
            ) {
                onOpen(.cheats)
            }
        }
        if shaderChainAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.shaders,
                label: settings.localized("Shaders"),
                systemImage: "camera.filters"
            ) {
                onOpen(.shaders)
            }
        }
        reportProblemRow
    }

    @ViewBuilder private var resetAndExitRows: some View {
        if vmMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.resetROM,
                label: settings.localized("Reset ROM"),
                systemImage: "arrow.counterclockwise.circle",
                isDestructive: true
            ) {
                onOpen(.resetROM)
            }
        }
        if gameMenuAvailable {
            OverlayActionRow(
                controllerNavigationID: ControllerTargetID.clearCache,
                label: settings.localized("Clear Current Game Cache"),
                systemImage: "trash.slash",
                action: onClearCache
            )
        }
        OverlayActionRow(
            controllerNavigationID: ControllerTargetID.backToMenu,
            label: settings.localized("Back to Menu"),
            systemImage: "list.bullet",
            action: onBackToMenu
        )
            .accessibilityHint(settings.localized("Leaves the game paused and returns to the library"))
    }

}

/// The right stick scrolls the leaf of the book-pose spread that holds controller focus.
private struct QuickMenuLeafStickTarget: View {
    let controllerInput: MenuControllerInputRouter?
    let leadingRows: [String]
    let leading: Bool
    let isEnabled: Bool
    @Environment(\.controllerAccessibilityNavigationSession) private var session

    var body: some View {
        // The focused frame is observed and the key is not, so read the frame to stay current.
        let key = session?.focusedFrame == nil ? nil : session?.focusedElementID
        let focusIsLeading = key.map { key in leadingRows.contains { key.contains($0) } } ?? false
        ControllerRightStickScrollTarget(
            controllerInput: controllerInput,
            axes: .vertical,
            priority: 180,
            isEnabled: isEnabled && focusIsLeading == leading,
            pointsPerSecond: 680
        )
        .frame(height: 0)
    }
}
