// QuickMenuView.swift — In-game pause menu card
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Destinations the pause menu hands back to the host to present. (Also the associated payload of
/// the host's overlay route state machine.)
enum QuickMenuDestination: Equatable {
    case perGame
    case gameController
    case controllerSkin
    case speed
    case shaders
    case saveStates
    case cheats
    case retroAchievements
    case padLayout
    case resetROM
    case changeDisc
}

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
    @Environment(\.menuControllerInputRouter) private var controllerInput

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

    private enum ControllerTargetID {
        static let stop = "quick-menu.stop"
        static let resume = "quick-menu.resume"
        static let osd = "quick-menu.osd"
        static let virtualPad = "quick-menu.virtual-pad"
        static let fullScreen = "quick-menu.full-screen"
        static let hideMenuButton = "quick-menu.hide-menu-button"
        static let speed = "quick-menu.speed"
        static let perGame = "quick-menu.per-game"
        static let gameController = "quick-menu.game-controller"
        static let controllerSkin = "quick-menu.controller-skin"
        static let editPadLayout = "quick-menu.edit-pad-layout"
        static let saveStates = "quick-menu.save-states"
        static let shaders = "quick-menu.shaders"
        static let changeDisc = "quick-menu.change-disc"
        static let retroAchievements = "quick-menu.retro-achievements"
        static let cheats = "quick-menu.cheats"
        static let resetROM = "quick-menu.reset-rom"
        static let clearCache = "quick-menu.clear-cache"
        static let backToMenu = "quick-menu.back-to-menu"
    }

    var body: some View {
        // The host (GameOverlayContainer) frames this view to the bounded card size; read that
        // size here to decide whether the geometry comfortably supports two columns.
        GeometryReader { geo in
            // Keep the navigation graph aligned with the view tree selected by
            // `overlayBody`. In particular, iPad uses the portrait-style body
            // even when its measured card is wider than it is tall.
            let usesLandscapeMap = variant == .phoneLandscape
            let usesTwoColumns = supportsTwoColumns(
                width: geo.size.width,
                height: geo.size.height
            )
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
                    ControllerNavigationAlert(
                        title: settings.localized("Stop Emulation?"),
                        message: settings.localized(
                            "This will shut down the running game. All unsaved progress will be lost."
                        ),
                        actions: [
                            .init(
                                id: "cancel",
                                title: settings.localized("Cancel")
                            ),
                            .init(
                                id: "stop",
                                title: settings.localized("Stop"),
                                isDestructive: true,
                                activationFeedback: .silent
                            ),
                        ],
                        selectedIndex: 0,
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
                .controllerAccessibilityTargetOrder(
                    showStopConfirmation
                        ? []
                        : usesLandscapeMap
                            ? quickMenuLandscapeControllerTargetOrder(
                                twoColumns: usesTwoColumns
                            )
                            : quickMenuPortraitControllerTargetOrder()
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
                        : usesLandscapeMap
                            ? quickMenuLandscapeDirectionalLinks(
                                twoColumns: usesTwoColumns
                            )
                            : quickMenuPortraitDirectionalLinks(),
                    prioritizesDirectionalLinks: !showStopConfirmation,
                    // Quick Menu publishes a complete semantic graph. Giving
                    // UIKit a second focus owner makes the fixed portrait footer
                    // compete geometrically with the scrolling rows on iOS 26.
                    usesNativeFocusEngine: false,
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
                            : ControllerTargetID.resume
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

    private func quickMenuControllerScopeKey(
        usesLandscapeMap: Bool
    ) -> String {
        let layout = usesLandscapeMap ? "landscape" : "portrait"
        let availability = "\(vmMenuAvailable ? 1 : 0)\(gameMenuAvailable ? 1 : 0)"
        let alert = showStopConfirmation ? "stop-confirmation" : "content"
        return "quick-pause.\(layout).\(availability).\(alert)"
    }

    /// Portrait keeps its scrolling rows and fixed footer in different
    /// coordinate spaces. Publish their semantic order instead of re-sorting
    /// them from frames that change while the scroll view is moving.
    private func quickMenuPortraitControllerTargetOrder() -> [String] {
        var order = [
            ControllerTargetID.osd,
            ControllerTargetID.virtualPad,
            ControllerTargetID.fullScreen,
            ControllerTargetID.hideMenuButton,
        ]
        if vmMenuAvailable { order.append(ControllerTargetID.speed) }
        if gameMenuAvailable { order.append(ControllerTargetID.perGame) }
        order.append(ControllerTargetID.controllerSkin)
        if !hidesPadLayoutEditorForController {
            order.append(ControllerTargetID.editPadLayout)
        }
        if gameMenuAvailable {
            order.append(ControllerTargetID.gameController)
        }
        if gameMenuAvailable || vmMenuAvailable {
            order.append(ControllerTargetID.saveStates)
        }
        if vmMenuAvailable { order.append(ControllerTargetID.changeDisc) }
        if gameMenuAvailable {
            order.append(contentsOf: [
                ControllerTargetID.retroAchievements,
                ControllerTargetID.cheats,
            ])
        }
        if shaderChainAvailable { order.append(ControllerTargetID.shaders) }
        if vmMenuAvailable { order.append(ControllerTargetID.resetROM) }
        if gameMenuAvailable { order.append(ControllerTargetID.clearCache) }
        order.append(contentsOf: [
            ControllerTargetID.backToMenu,
            ControllerTargetID.stop,
            ControllerTargetID.resume,
        ])
        return order
    }

    /// Stop is a secondary action beside Resume, not another row between the
    /// bottom card and the primary footer action. Vertical movement enters and
    /// leaves the footer through Resume; horizontal movement selects Stop.
    private func quickMenuPortraitDirectionalLinks()
        -> [ControllerAccessibilityDirectionalLink] {
        [
            .init(
                fromLabel: ControllerTargetID.backToMenu,
                direction: .down,
                toLabel: ControllerTargetID.resume
            ),
            .init(
                fromLabel: ControllerTargetID.resume,
                direction: .up,
                toLabel: ControllerTargetID.backToMenu
            ),
            .init(
                fromLabel: ControllerTargetID.stop,
                direction: .up,
                toLabel: ControllerTargetID.backToMenu
            ),
            .init(
                fromLabel: ControllerTargetID.resume,
                direction: .left,
                toLabel: ControllerTargetID.stop
            ),
            .init(
                fromLabel: ControllerTargetID.stop,
                direction: .right,
                toLabel: ControllerTargetID.resume
            ),
        ]
    }

    /// Landscape is a command deck, not a free-form proximity surface. Build
    /// its graph from the rows that actually exist so repeated input stays in
    /// the current column and Left/Right always lands on the requested pair.
    /// Missing conditional actions are omitted and their neighbors connect
    /// directly, so the graph remains valid with or without an active VM/game.
    private func quickMenuLandscapeDirectionalLinks(twoColumns: Bool)
        -> [ControllerAccessibilityDirectionalLink] {
        let stop = ControllerTargetID.stop
        let resume = ControllerTargetID.resume
        let osd = ControllerTargetID.osd
        let virtualPad = ControllerTargetID.virtualPad
        let fullScreen = ControllerTargetID.fullScreen
        let hideMenu = ControllerTargetID.hideMenuButton
        let speed = ControllerTargetID.speed
        let perGame = ControllerTargetID.perGame
        let gameController = ControllerTargetID.gameController
        let controllerSkin = ControllerTargetID.controllerSkin
        let editPad = ControllerTargetID.editPadLayout
        let saveStates = ControllerTargetID.saveStates
        let shaders = ControllerTargetID.shaders
        let changeDisc = ControllerTargetID.changeDisc
        let achievements = ControllerTargetID.retroAchievements
        let cheats = ControllerTargetID.cheats
        let resetROM = ControllerTargetID.resetROM
        let clearCache = ControllerTargetID.clearCache
        let backToMenu = ControllerTargetID.backToMenu

        var contentOrder = [osd, virtualPad, fullScreen, hideMenu]
        if vmMenuAvailable { contentOrder.append(speed) }
        if gameMenuAvailable { contentOrder.append(perGame) }
        contentOrder.append(controllerSkin)
        if !hidesPadLayoutEditorForController { contentOrder.append(editPad) }
        if gameMenuAvailable { contentOrder.append(gameController) }
        if gameMenuAvailable || vmMenuAvailable {
            contentOrder.append(saveStates)
        }
        if vmMenuAvailable { contentOrder.append(changeDisc) }
        if gameMenuAvailable {
            contentOrder.append(contentsOf: [achievements, cheats])
        }
        if shaderChainAvailable { contentOrder.append(shaders) }
        if vmMenuAvailable { contentOrder.append(resetROM) }
        if gameMenuAvailable { contentOrder.append(clearCache) }
        contentOrder.append(backToMenu)

        // The iPhone SE landscape layout renders one visual column. Reusing the
        // two-column graph there made Down from Stop enter the left sequence,
        // while Down from Resume jumped into the former right sequence.
        if !twoColumns {
            var links: [ControllerAccessibilityDirectionalLink] = [
                .init(fromLabel: stop, direction: .right, toLabel: resume),
                .init(fromLabel: resume, direction: .left, toLabel: stop),
                .init(fromLabel: stop, direction: .down, toLabel: osd),
                .init(fromLabel: resume, direction: .down, toLabel: osd),
                .init(fromLabel: osd, direction: .up, toLabel: resume),
            ]
            for (upper, lower) in zip(contentOrder, contentOrder.dropFirst()) {
                links.append(
                    .init(fromLabel: upper, direction: .down, toLabel: lower)
                )
                links.append(
                    .init(fromLabel: lower, direction: .up, toLabel: upper)
                )
            }
            return links
        }

        var leftColumn = [stop, osd, virtualPad, fullScreen, hideMenu]
        if vmMenuAvailable { leftColumn.append(speed) }
        if gameMenuAvailable { leftColumn.append(perGame) }
        leftColumn.append(controllerSkin)
        if !hidesPadLayoutEditorForController { leftColumn.append(editPad) }

        var rightColumn = [resume]
        if gameMenuAvailable { rightColumn.append(gameController) }
        if gameMenuAvailable || vmMenuAvailable {
            rightColumn.append(saveStates)
        }
        if vmMenuAvailable { rightColumn.append(changeDisc) }
        if gameMenuAvailable {
            rightColumn.append(contentsOf: [achievements, cheats])
        }
        if shaderChainAvailable { rightColumn.append(shaders) }
        if vmMenuAvailable { rightColumn.append(resetROM) }
        if gameMenuAvailable { rightColumn.append(clearCache) }
        rightColumn.append(backToMenu)

        var links: [ControllerAccessibilityDirectionalLink] = []
        // Moving out of the cards always returns to the primary Resume action.
        // Stop remains reachable immediately to its left.
        links.append(
            ControllerAccessibilityDirectionalLink(
                fromLabel: osd,
                direction: .up,
                toLabel: resume
            )
        )
        func appendVerticalLinks(_ labels: [String]) {
            for (upper, lower) in zip(labels, labels.dropFirst()) {
                links.append(
                    ControllerAccessibilityDirectionalLink(
                        fromLabel: upper,
                        direction: .down,
                        toLabel: lower
                    )
                )
                links.append(
                    ControllerAccessibilityDirectionalLink(
                        fromLabel: lower,
                        direction: .up,
                        toLabel: upper
                    )
                )
            }
        }
        appendVerticalLinks(leftColumn)
        appendVerticalLinks(rightColumn)

        let available = Set(leftColumn + rightColumn)
        let horizontalPairs = [
            (stop, resume),
            (osd, gameController),
            (virtualPad, saveStates),
            (fullScreen, changeDisc),
            (hideMenu, achievements),
            (speed, cheats),
            (perGame, resetROM),
            (controllerSkin, clearCache),
            (editPad, backToMenu),
        ]
        for (left, right) in horizontalPairs
        where available.contains(left) && available.contains(right) {
            links.append(
                ControllerAccessibilityDirectionalLink(
                    fromLabel: left,
                    direction: .right,
                    toLabel: right
                )
            )
            links.append(
                ControllerAccessibilityDirectionalLink(
                    fromLabel: right,
                    direction: .left,
                    toLabel: left
                )
            )
        }
        return links
    }

    /// Fallback ordering uses the same mounted rows as the explicit landscape
    /// links. This keeps a briefly recycled row (notably Shaders) from being
    /// skipped while the scroll view is mounting.
    private func quickMenuLandscapeControllerTargetOrder(
        twoColumns: Bool
    ) -> [String] {
        var contentOrder = [
            ControllerTargetID.osd,
            ControllerTargetID.virtualPad,
            ControllerTargetID.fullScreen,
            ControllerTargetID.hideMenuButton,
        ]
        if vmMenuAvailable { contentOrder.append(ControllerTargetID.speed) }
        if gameMenuAvailable { contentOrder.append(ControllerTargetID.perGame) }
        contentOrder.append(ControllerTargetID.controllerSkin)
        if !hidesPadLayoutEditorForController {
            contentOrder.append(ControllerTargetID.editPadLayout)
        }
        if gameMenuAvailable {
            contentOrder.append(ControllerTargetID.gameController)
        }
        if gameMenuAvailable || vmMenuAvailable {
            contentOrder.append(ControllerTargetID.saveStates)
        }
        if vmMenuAvailable { contentOrder.append(ControllerTargetID.changeDisc) }
        if gameMenuAvailable {
            contentOrder.append(contentsOf: [
                ControllerTargetID.retroAchievements,
                ControllerTargetID.cheats,
            ])
        }
        if shaderChainAvailable {
            contentOrder.append(ControllerTargetID.shaders)
        }
        if vmMenuAvailable { contentOrder.append(ControllerTargetID.resetROM) }
        if gameMenuAvailable { contentOrder.append(ControllerTargetID.clearCache) }
        contentOrder.append(ControllerTargetID.backToMenu)
        return [ControllerTargetID.stop, ControllerTargetID.resume]
            + contentOrder
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
        if variant == .phoneLandscape {
            landscapeBody(
                width: width,
                height: height,
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
                scrollContent(twoColumns: twoColumns)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
    }

    @ViewBuilder
    private func landscapeBody(
        width: CGFloat,
        height: CGFloat,
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

    /// Two columns only when the measured card can keep both columns comfortable.
    /// iPhone portrait always uses one column, while compact landscape and iPad
    /// layouts fall back to one column below their respective usable-size threshold.
    private func supportsTwoColumns(width: CGFloat, height: CGFloat) -> Bool {
        switch variant {
        case .phonePortrait:
            return false
        case .ipadTwoColumn:
            return width >= 500 && height >= 320
        case .phoneLandscape:
            // Notched 19.5:9 iPhones retain at least 640 points after safe-area
            // clearance; smaller 16:9 phones continue using the compact column.
            return width >= 640 && height >= 300
        }
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

    @ViewBuilder
    private func scrollContent(twoColumns: Bool) -> some View {
        // Keep the footer as a real layout sibling. A safe-area overlay leaves
        // the underlying scroll viewport focusable beneath the buttons, so a
        // row can receive controller focus while Stop/Resume visually cover it.
        VStack(spacing: 0) {
            ScrollView {
                quickMenuRightStickTarget
                cardsContent(twoColumns: twoColumns)
            }
            QuickMenuFooter(
                settings: settings,
                compact: compact,
                stopControllerNavigationID: ControllerTargetID.stop,
                resumeControllerNavigationID: ControllerTargetID.resume,
                onStop: { showStopConfirmation = true },
                onResume: onResume
            )
        }
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

private struct LandscapeCommandBar: View {
    let settings: SettingsStore
    let gameTitle: String?
    let stopControllerNavigationID: String
    let resumeControllerNavigationID: String
    let onStop: () -> Void
    let onResume: () -> Void
    let iconOnly: Bool
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "pause.circle.fill")
                    .font(.system(size: 18))
                    .foregroundStyle(accentColour)
                Text(settings.localized("Paused"))
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(
                        textAppearance.normalColor ?? OverlayTheme.textPrimary
                    )
                    .layoutPriority(1)
                if let title = gameTitle, !title.isEmpty {
                    Text(title)
                        .font(.caption)
                        .foregroundStyle(
                            textAppearance.secondaryColor
                                ?? OverlayTheme.textSecondary
                        )
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .layoutPriority(-1)
                }
                Spacer(minLength: 8)
                QuickMenuStopButton(
                    controllerNavigationID: stopControllerNavigationID,
                    accessibilityLabel: settings.localized("Stop"),
                    compact: true,
                    action: onStop
                )
                Button(action: onResume) {
                    if iconOnly {
                        Image(systemName: "play.fill")
                            .foregroundStyle(.white)
                    } else {
                        Label {
                            Text(settings.localized("Resume"))
                        } icon: {
                            Image(systemName: "play.fill")
                                .foregroundStyle(.white)
                        }
                    }
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.regular)
                .controllerAccessibilityActionTarget(
                    id: resumeControllerNavigationID,
                    label: resumeControllerNavigationID,
                    focusedColor: .white,
                    action: onResume
                )
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 6)
            OverlayTheme.separator
                .frame(height: 0.5)
        }
    }
}

private struct QuickMenuFooter: View {
    let settings: SettingsStore
    let compact: Bool
    let stopControllerNavigationID: String
    let resumeControllerNavigationID: String
    let onStop: () -> Void
    let onResume: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            OverlayTheme.separator
                .frame(height: 0.5)
            HStack(spacing: compact ? 10 : 12) {
                QuickMenuStopButton(
                    controllerNavigationID: stopControllerNavigationID,
                    accessibilityLabel: settings.localized("Stop"),
                    compact: compact,
                    action: onStop
                )
                Button(action: onResume) {
                    Label {
                        Text(settings.localized("Resume"))
                    } icon: {
                        Image(systemName: "play.fill")
                            .foregroundStyle(.white)
                    }
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(compact ? .regular : .large)
                .controllerAccessibilityActionTarget(
                    id: resumeControllerNavigationID,
                    label: resumeControllerNavigationID,
                    focusedColor: .white,
                    action: onResume
                )
            }
            .padding(.horizontal, compact ? 18 : 20)
            .padding(.top, 8)
            .padding(.bottom, compact ? 10 : 14)
        }
    }
}

private struct QuickMenuStopButton: View {
    let controllerNavigationID: String
    let accessibilityLabel: String
    let compact: Bool
    let action: () -> Void

    private var diameter: CGFloat { compact ? 36 : 46 }

    var body: some View {
        Button(action: action) {
            Image(systemName: "stop.fill")
                .font(.system(size: compact ? 13 : 16, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: diameter, height: diameter)
                .background(Color.red, in: Circle())
        }
        .buttonStyle(.plain)
        .contentShape(Circle())
        .accessibilityLabel(accessibilityLabel)
        .controllerAccessibilityActionTarget(
            id: controllerNavigationID,
            label: controllerNavigationID,
            focusedColor: .white,
            focusedNeonCornerRadius: diameter / 2,
            action: action
        )
    }
}
