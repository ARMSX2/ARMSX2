// GameLibraryControllerCoordinator.swift — the library's controller focus, commands and presentation state
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UIKit
import Observation

@MainActor
@Observable
final class GameLibraryControllerCoordinator {
    var showGameImporter = false
    var showCoverImporter = false
    var showCoverPhotoPicker = false
    var showRestartAlert = false
    var showStopAlert = false
    var showCoverTemplateEditor = false
    var showGameReplacementAlert = false
    var catalogSkinInstallTask: Task<Void, Never>?
    var pendingAutomaticCustomSkinLaunch: PendingAutomaticCustomSkinLaunch?
    var coverTemplateDraft = CoverStore.defaultCoverURLTemplate
    var pendingGameName: String = ""
    var pendingGameplayLaunchTransition: GameplayLaunchTransition?
    var pendingGameplayPadSerial: String?
    var pendingGameplayPadCRC: String?
    var pendingGameImportURLs: [URL] = []
    var existingGameImportFileNames: [String] = []
    var pendingCoverGameName: String?
    var pendingCoverPhotoGameName: String?
    var selectedCoverPhotoItem: PhotosPickerItem?
    var gameInfoTarget: ISOEntry?
    var gameSettingsTarget: ISOEntry?
    var padLayoutEditorTarget: ISOEntry?
    var gameSettingsStartsInShaders = false
    var preparedGameSettingsID: String?
    var preparedGameSettings: [ARMSX2PerGameKey: Any]?
    var renameTarget: ISOEntry?
    var discLinkTarget: ISOEntry?
    var cheatsManagerTarget: ISOEntry?
    var pendingDeleteGame: ISOEntry?
    var pendingDeleteDataGame: ISOEntry?
    var gameActionTitle = ""
    var gameActionMessage: String?
    var showBackgroundAssetError = false
    let gameplayLaunchCardRegistry = GameplayLaunchCardRegistry()
    var favoriteAnimationValues: [String: Int] = [:]
    var nowRunningRemovalAnimationActive = false
    var nowRunningRemovalResetTask: Task<Void, Never>?
    var departingRunningGameName: String?
    var nowRunningCardOpacity = 1.0
    var nowRunningCardScale = 1.0
    var nowRunningStatusOpacity = 1.0
    let controllerFocusState = GameLibraryControllerFocusState()
    let controllerScrollAvailability = GameLibraryControllerScrollAvailability()
    let libraryScrollRestoreState = GameLibraryScrollRestoreState()
    var favoriteScrollRequest: GameLibraryFavoriteScrollRequest?
    var favoriteScrollSequence: UInt64 = 0
    let focusScrollCoordinator = GameLibraryFocusScrollCoordinator()
    let coverFlowPreheater = GameLibraryCoverFlowPreheater()
    var libraryContainerSize: CGSize = .zero
    @ObservationIgnored private var seededContainerSize: CGSize = .zero
    var landscapeControllerAutofocusPending = false
    var landscapeControllerAutofocusTask: Task<Void, Never>?
    var runningGameControllerFocusPending = false
    let controllerContextMenuState = GameLibraryControllerContextMenuState()
    var deferredControllerContextMenuSelection: DeferredControllerContextMenuSelection?
    let controllerToolbarFocusState = GameLibraryControllerToolbarFocusState()
    let controllerNowRunningFocusState = GameLibraryNowRunningControllerFocusState()
    var controllerAlertSelectedIndex = 0
    var controllerLayoutMenuPresented = false
    var controllerLayoutMenuSelectedIndex = 0
    var gameLibraryViewOptionsPresented = false
    var gameLibraryViewOptionsPreview: GameLibraryViewOptionsPreview?
    var gameLibraryViewOptionsBaseline: GameLibraryViewOptionsValues?
    var controllerMoreMenuPresented = false
    var controllerMoreMenuSelectedIndex = 0
    var controllerFocusReleased = false

    @ObservationIgnored var controllerInput: MenuControllerInputRouter?
    @ObservationIgnored var menuTabIsActive = true
    @ObservationIgnored var reduceMotion = false
    @ObservationIgnored var layoutDirection: LayoutDirection = .leftToRight
    @ObservationIgnored var sharedMenuControllerInput: MenuControllerInputRouter?

    var appState: AppState { .shared }
    var session: GameSession { .shared }
    var settings: SettingsStore { .shared }
    var coverStore: CoverStore { .shared }
    var fileImporter: FileImportHandler { .shared }
    var appearance: GameLibraryAppearance { .shared }
    var library: GameLibraryModel { .shared }
    typealias CoverFlowMetrics = GameLibraryCoverFlowMetrics

    var displayedRunningGameName: String? {
        if let departingRunningGameName {
            return departingRunningGameName
        }
        guard case .menu = appState.currentScreen else {
            return nil
        }
        return session.runningGameName
    }

    var canFocusNowRunning: Bool {
        session.runningGameName != nil && !nowRunningRemovalAnimationActive
    }

    /// Departure values belong only to the retained stopping placeholder.
    /// A normal running card always starts from its canonical appearance, so
    /// cleanup never needs to reset animation state across the library tree.
    var displayedNowRunningCardOpacity: Double {
        departingRunningGameName == nil ? 1 : nowRunningCardOpacity
    }

    var displayedNowRunningCardScale: Double {
        departingRunningGameName == nil ? 1 : nowRunningCardScale
    }

    var displayedNowRunningStatusOpacity: Double {
        departingRunningGameName == nil ? 1 : nowRunningStatusOpacity
    }

    /// Clean display title for the running game, preferring the library entry over the raw path.
    func displayTitle(forRunningName name: String) -> String {
        if name == "BIOS" {
            return settings.localized("Boot BIOS")
        }
        if let entry = library.games.first(where: { $0.bootName == name || $0.name == name }) {
            let title = entry.metadata[.title]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return title.isEmpty ? GameListView.cleanGameFileName(entry.name) : title
        }
        return GameListView.cleanGameFileName(name)
    }

    func runningLibraryEntry(for name: String) -> ISOEntry? {
        library.games.first(where: library.isRunning)
            ?? library.games.first {
                $0.bootName == name || $0.name == name
            }
    }

    var usesLandscapeGameCardScale: Bool {
        // The read of libraryContainerSize keeps views outside the geometry pass subscribed to rotations.
        let observed = libraryContainerSize
        let size = seededContainerSize == .zero ? observed : seededContainerSize
        return size.width > size.height
    }

    func seedLibraryContainerSize(_ size: CGSize) {
        seededContainerSize = size
    }

    var hideGameName: Bool {
        appearance.activeHideGameNameBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue
    }

    var hideGameInfo: Bool {
        appearance.activeHideGameInfoBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue
    }

    var hideFavoriteButton: Bool {
        appearance.activeHideFavoriteButtonBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue
    }

    var hideRegionFlag: Bool {
        appearance.activeHideRegionFlagBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue
    }

    var singleLineGameNames: Bool {
        appearance.activeSingleLineGameNamesBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue
    }

    var hideRunningIndicator: Bool {
        appearance.activeHideRunningIndicatorBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue
    }

    var resolvedGameCardScale: CGFloat {
        CGFloat(min(max(appearance.activeGameCardScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0.72), 1.32))
    }

    var resolvedGameCardWidthScale: CGFloat {
        return CGFloat(
            min(max(appearance.activeGameCardWidthScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0.70), 1.40)
        )
    }

    var resolvedGameCardHeightScale: CGFloat {
        CGFloat(
            min(max(appearance.activeGameCardHeightScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0.70), 1.40)
        )
    }

    var resolvedGameCardGapScale: CGFloat {
        CGFloat(min(max(appearance.activeGameCardGapScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0.65), 2))
    }

    var resolvedGameCardContentPaddingScale: CGFloat {
        CGFloat(
            min(
                max(appearance.activeGameCardContentPaddingScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0),
                2
            )
        )
    }

    var resolvedGameCardVerticalPaddingScale: CGFloat {
        CGFloat(
            min(
                max(appearance.activeGameCardVerticalPaddingScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0),
                2
            )
        )
    }

    var resolvedGameCardTextSpacingScale: CGFloat {
        CGFloat(
            min(
                max(appearance.activeGameCardTextSpacingScaleBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 0),
                2
            )
        )
    }

    var resolvedGameCardCornerRadius: CGFloat {
        CGFloat(min(max(appearance.activeGameCardCornerRadiusBinding(isLandscape: usesLandscapeGameCardScale).wrappedValue, 6), 34))
    }

    var controllerSelectedGameID: String? {
        get { controllerFocusState.selectedGameID }
        set {
            let previousValue = controllerFocusState.selectedGameID
            controllerFocusState.selectedGameID = newValue
            if let newValue {
                controllerInput?.rememberNavigationFocusKey(
                    "game:\(newValue)",
                    forScope: gameLibraryControllerMemoryScope
                )
                // Toolbar/tab-bar focus must not replace the last game card.
                // The router survives rebuilding this tab's view.
                controllerInput?.rememberNavigationFocusKey(
                    "game:\(newValue)",
                    forScope: gameLibraryControllerCardMemoryScope
                )
            }
            if previousValue != newValue {
                updateGameCoverThemePreview(for: newValue)
            }
        }
    }

    /// Favorite cards opt into the cover-derived appearance while focused.
    /// The context menu can temporarily opt any linked game into the same
    /// preview without changing its favorite state or the saved theme.
    func updateGameCoverThemePreview(
        for gameID: String?,
        includesNonFavoriteGame: Bool = false,
        whilePresentationIsActive: Bool = false
    ) {
        guard menuTabIsActive,
              whilePresentationIsActive
                || (controllerInput?.navigationZone == .library
                    && !controllerNavigationPresentationActive),
              let gameID else {
            GameCoverThemePreviewStore.shared.clear()
            return
        }
        // Favorite focus and the Context Menu/Now Running previews each have
        // a switch. With it off, a focus change must not scan the library or
        // schedule palette work.
        guard includesNonFavoriteGame
                ? settings.appearance.gameMenuCoverColoursEnabled
                : settings.appearance.favoriteGlowingEffectEnabled else {
            GameCoverThemePreviewStore.shared.clear()
            return
        }
        guard let game = library.games.first(where: { $0.id == gameID }),
              includesNonFavoriteGame || game.isFavorite else {
            GameCoverThemePreviewStore.shared.clear()
            return
        }
        GameCoverThemePreviewStore.shared.schedule(
            gameID: game.id,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature
        )
    }

    var controllerNavigationActive: Bool {
        get { controllerFocusState.isActive }
        set { controllerFocusState.isActive = newValue }
    }

    func handleLibraryAppear() {
        GameLibraryRuntimeResources.activateForMenu()
        updateControllerScrollAvailability()
        library.restoreCachedGamesIfNeeded()
        if controllerInput?.navigationZone == .library {
            _ = restoreRememberedControllerContentFocus()
        }
        if GameLibrarySnapshot.shared.shouldRefreshLibrary() {
            library.scheduleLibraryReload(
                after: .milliseconds(80),
                autoDownloadExternalCovers: true
            )
        }
        if settings.sanitizeBackgroundAssets() {
            showBackgroundAssetError = true
        }
    }

    func handleReturnToMenu() {
        runningGameControllerFocusPending =
            controllerInput?.hasConnectedController == true
            && session.runningGameName != nil
        library.restoreCachedGamesIfNeeded()
        GameLibrarySnapshot.shared.markLibraryRefreshNeeded()
        _ = focusRunningControllerGameIfAvailable()
        library.scheduleLibraryReload(
            after: .milliseconds(80),
            autoDownloadExternalCovers: false
        )
    }


    var activeControllerAlertKind: GameLibraryControllerAlertKind? {
        if pendingAutomaticCustomSkinLaunch != nil { return .automaticCustomSkin }
        if showRestartAlert { return .restart }
        if showStopAlert { return .stop }
        if pendingDeleteDataGame != nil { return .deleteGameData }
        if pendingDeleteGame != nil { return .deleteGame }
        if gameActionMessage != nil { return .gameAction }
        if coverStore.showCoverAlert { return .coverResult }
        if showGameReplacementAlert { return .replaceFiles }
        if showBackgroundAssetError { return .backgroundError }
        return nil
    }

    var nonContextControllerNavigationPresentationActive: Bool {
        controllerLayoutMenuPresented
            || controllerMoreMenuPresented
            || showGameImporter
            || showCoverImporter
            || showCoverPhotoPicker
            || showRestartAlert
            || showStopAlert
            || showCoverTemplateEditor
            || showGameReplacementAlert
            || pendingAutomaticCustomSkinLaunch != nil
            || showBackgroundAssetError
            || gameInfoTarget != nil
            || gameSettingsTarget != nil
            || padLayoutEditorTarget != nil
            || renameTarget != nil
            || discLinkTarget != nil
            || cheatsManagerTarget != nil
            || pendingDeleteGame != nil
            || pendingDeleteDataGame != nil
            || gameActionMessage != nil
            || coverStore.showCoverAlert
            || deferredControllerContextMenuSelection != nil
    }

    /// Per-game settings and the pad editor own their controller focus and must
    /// remain routable. The other library presentations are driven by the custom
    /// command listener or native UIKit and therefore capture the retained
    /// library explicitly.
    var manuallyCapturedControllerPresentationActive: Bool {
        nonContextControllerNavigationPresentationActive
            && gameSettingsTarget == nil
            && padLayoutEditorTarget == nil
    }

    var controllerNavigationPresentationActive: Bool {
        controllerContextMenuState.gameID != nil
            || gameLibraryViewOptionsPresented
            || nonContextControllerNavigationPresentationActive
    }

    var controllerLibraryNavigationAllowed: Bool {
        menuTabIsActive
            && appState.currentScreen == .menu
            && (controllerInput?.isControllerNavigationEnabled ?? false)
            && !(controllerInput?.isNavigationCaptured ?? false)
            && !controllerNavigationPresentationActive
    }

    private var controllerLibraryRightStickAllowed: Bool {
        controllerLibraryNavigationAllowed
            && controllerInput?.navigationZone == .library
    }

    func updateControllerScrollAvailability() {
        controllerScrollAvailability.isEnabled =
            controllerLibraryRightStickAllowed
    }

    func controllerInputAvailabilityDidChange() {
        updateControllerScrollAvailability()
        // Settings can still own input during the Games view's onAppear.
        // Restore once that owner leaves, but never interrupt touch/analog
        // focus release or take focus away from the bottom tab bar.
        if controllerLibraryRightStickAllowed, !controllerFocusReleased,
           !controllerNavigationActive, controllerSelectedGameID == nil {
            _ = restoreRememberedControllerContentFocus()
        }
    }

    func releaseControllerFocus() {
        focusScrollCoordinator.stopAnimation()
        controllerFocusReleased = true
        controllerNavigationActive = false
        controllerSelectedGameID = nil
        controllerNowRunningFocusState.selectedAction = nil
        controllerContextMenuState.selectedAction = nil
        updateControllerScrollAvailability()
    }

    func restoreCenteredControllerFocusAfterRightStickScrolling() {
        guard controllerLibraryRightStickAllowed,
              let gameID = focusScrollCoordinator.centeredVisibleGameID(
                  axis: usesLandscapeCoverFlow ? .horizontal : .vertical
              ),
              library.games.contains(where: { $0.id == gameID }) else { return }
        controllerFocusReleased = false
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = gameID
        updateControllerScrollAvailability()
    }

    func handleControllerCommand(_ event: MenuControllerInputEvent) {
        guard !event.isNavigationSessionRouted else { return }
        if let captureOwner = event.captureOwner,
           captureOwner
            != MenuControllerNavigationCaptureOwner.gameLibraryPresentation {
            return
        }
        let command = event.command
        controllerFocusReleased = false
        if let game = controllerContextMenuGame {
            handleControllerContextMenuCommand(command, for: game)
            return
        }

        // OrbitKeys reads the controller profile directly to keep both sticks,
        // triggers, shoulders, L3/R3 and all four face buttons. The library
        // capture stays mounted only to keep the cards and toolbar passive.
        if renameTarget != nil {
            return
        }

        if controllerLayoutMenuPresented {
            handleControllerLayoutMenuCommand(command)
            return
        }

        if controllerMoreMenuPresented {
            handleControllerMoreMenuCommand(command)
            return
        }

        if let alertKind = activeControllerAlertKind {
            handleControllerAlertCommand(command, kind: alertKind)
            return
        }

        // These destinations own explicit controller focus and activation.
        // Leave the retained library completely passive underneath their
        // presentation so one button press cannot trigger two actions.
        if gameSettingsTarget != nil
            || padLayoutEditorTarget != nil
            || cheatsManagerTarget != nil {
            return
        }

        if controllerNavigationPresentationActive {
            switch command {
            case .up, .upRight, .right, .downRight,
                 .down, .downLeft, .left, .upLeft:
                // SwiftUI's focused Form/List controls receive the same
                // physical input. Add the matching controller sensation while
                // keeping the retained library from moving underneath them.
                controllerInput?.playFeedback(.move(command))
            case .activate:
                controllerInput?.playFeedback(.activate)
            case .back:
                if dismissControllerPresentation() {
                    controllerInput?.playFeedback(.back)
                }
            case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
                break
            }
            return
        }

        guard controllerLibraryNavigationAllowed else { return }

        if controllerInput?.navigationZone == .tabBar {
            return
        }

        if controllerInput?.navigationZone == .topToolbar {
            handleControllerToolbarCommand(command)
            return
        }

        if controllerInput?.suppressesLibraryEvent(event) == true {
            return
        }

        if library.games.isEmpty && displayedRunningGameName == nil {
            handleEmptyLibraryControllerCommand(command)
            return
        }

        switch command {
        case .up, .upRight, .right, .downRight,
             .down, .downLeft, .left, .upLeft:
            let moved = moveControllerSelection(command)
            if !moved, command.verticalComponent == .up {
                controllerNavigationActive = false
                controllerInput?.setNavigationZone(.topToolbar)
                controllerInput?.playFeedback(.move(.up))
                return
            }
            if !moved, command.verticalComponent == .down {
                controllerNavigationActive = false
                controllerInput?.setNavigationZone(.tabBar)
                controllerInput?.playFeedback(.move(.down))
                return
            }
            controllerInput?.playFeedback(
                moved ? .move(command) : .boundary
            )
        case .activate:
            if let nowRunningAction = controllerNowRunningFocusState.selectedAction,
               displayedRunningGameName != nil {
                controllerInput?.playFeedback(.activate)
                switch nowRunningAction {
                case .resume:
                    session.returnToGame()
                case .stop:
                    showStopAlert = true
                }
                return
            }
            guard let game = controllerSelectedGame else {
                if selectInitialControllerGame(preferLast: false) {
                    controllerInput?.playFeedback(.move(.down))
                }
                return
            }
            // Controller activation launches immediately; the automatic skin
            // recommendation remains a touch/library-management surface.
            open(game, allowsAutomaticSkinPrompt: false)
        case .back:
            guard controllerNavigationActive else { return }
            controllerSelectedGameID = nil
            controllerNowRunningFocusState.selectedAction = nil
            controllerNavigationActive = false
            controllerInput?.playFeedback(.back)
        case .toggleFavorite:
            guard controllerNowRunningFocusState.selectedAction == nil else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            guard let game = controllerSelectedGame else { return }
            let isFavorite = toggleFavorite(
                game,
                playsSelectionFeedback: false
            )
            controllerInput?.playFeedback(
                .favorite(isFavorite: isFavorite)
            )
        case .showContextMenu:
            guard controllerNowRunningFocusState.selectedAction == nil else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            guard let game = controllerSelectedGame else { return }
            openControllerContextMenu(for: game)
        case .previousTab, .nextTab:
            break
        }
    }

    func handleLibraryContainerAppear(geo: GeometryProxy) {
        libraryContainerSize = geo.size
        if geo.size.width > geo.size.height {
            landscapeControllerAutofocusPending = true
            scheduleLandscapeControllerAutofocus()
        }
    }

    func handleLibraryContainerSizeChange(_ previousSize: CGSize, _ size: CGSize) {
        let enteredLandscape = size.width > size.height
            && previousSize.width <= previousSize.height
        libraryContainerSize = size
        if enteredLandscape {
            landscapeControllerAutofocusPending = true
            scheduleLandscapeControllerAutofocus()
        } else if size.width <= size.height {
            landscapeControllerAutofocusPending = false
            landscapeControllerAutofocusTask?.cancel()
            landscapeControllerAutofocusTask = nil
            coverFlowPreheater.cancel()
        }
    }

    func handleNavigationZoneChange(_ zone: MenuControllerNavigationZone?) {
        if zone == .topToolbar {
            rememberControllerToolbarFocus()
        }
        updateGameCoverThemePreview(
            for: zone == .library ? controllerSelectedGameID : nil
        )
    }

    func handleContextMenuGameChange(_ gameID: String?) {
        if let gameID {
            updateGameCoverThemePreview(
                for: gameID,
                includesNonFavoriteGame: true,
                whilePresentationIsActive: true
            )
        } else {
            updateGameCoverThemePreview(for: controllerSelectedGameID)
        }
    }

    func handleToolbarSelectionChange() {
        if controllerInput?.navigationZone == .topToolbar {
            rememberControllerToolbarFocus()
        }
    }

    func handleGamesChange(_ updatedGames: [ISOEntry]) {
        controllerFocusState.updateGameIDs(updatedGames.map(\.id))
        updateGameCoverThemePreview(for: controllerSelectedGameID)
        if let controllerSelectedGameID,
           !updatedGames.contains(where: { $0.id == controllerSelectedGameID }) {
            self.controllerSelectedGameID = nil
            controllerNavigationActive = false
            closeControllerContextMenu(playsFeedback: false)
        }
        if controllerNavigationActive,
           controllerSelectedGameID == nil,
           controllerNowRunningFocusState.selectedAction == nil {
            _ = restoreRememberedControllerContentFocus()
                || selectInitialControllerGame(preferLast: false)
        }
        let restoredRunningGameFocus =
            focusRunningControllerGameIfAvailable()
        if !restoredRunningGameFocus,
           usesLandscapeGameCardScale {
            landscapeControllerAutofocusPending = true
            scheduleLandscapeControllerAutofocus()
        }
    }

    func handleRunningGameChange(_ runningGame: String?) {
        if runningGame == nil {
            runningGameControllerFocusPending = false
            controllerNowRunningFocusState.selectedAction = nil
        } else if controllerInput?.hasConnectedController == true {
            runningGameControllerFocusPending = true
            _ = focusRunningControllerGameIfAvailable()
        }
    }

    func handleControllerConnectionChange(_ connected: Bool?) {
        if connected == true {
            if session.runningGameName != nil {
                runningGameControllerFocusPending = true
            }
            if !focusRunningControllerGameIfAvailable() {
                landscapeControllerAutofocusPending =
                    usesLandscapeGameCardScale
                scheduleLandscapeControllerAutofocus()
            }
        } else {
            runningGameControllerFocusPending = false
            controllerFocusState.releaseCardStates()
            focusScrollCoordinator.cancel()
        }
    }

    func handleMenuTabActiveChange(_ active: Bool) {
        updateGameCoverThemePreview(
            for: active ? controllerSelectedGameID : nil
        )
        if active,
           !restoreRememberedControllerContentFocus(),
           !focusRunningControllerGameIfAvailable(),
           usesLandscapeGameCardScale {
            landscapeControllerAutofocusPending = true
            scheduleLandscapeControllerAutofocus()
        }
    }

    func handleCapturedPresentationChange(_ active: Bool) {
        let shouldCapture = active
            || controllerContextMenuState.gameID != nil
        controllerInput?.setNavigationCaptured(
            shouldCapture,
            owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
            priority: 300
        )
        updateControllerScrollAvailability()
        if !shouldCapture {
            scheduleLandscapeControllerAutofocus()
        }
    }

    func handleNavigationPresentationChange(_ active: Bool) {
        guard active else { return }
        // Modal destinations own the foreground. Stop speculative cover
        // decoding and any in-flight centering work until the library is
        // interactive again; navigation restarts both on demand.
        coverFlowPreheater.cancel()
        focusScrollCoordinator.stopAnimation()
        library.gameLibraryActivity.isScrolling = false
    }

    func handleAlertKindChange(_ previous: GameLibraryControllerAlertKind?, _ kind: GameLibraryControllerAlertKind?) {
        controllerAlertSelectedIndex = 0
        guard let kind, kind != previous else { return }
        MenuAudioPackManager.shared.playEvent(.uiToast)
    }

    func handleLibraryDisappear() {
        controllerLayoutMenuPresented = false
        gameLibraryViewOptionsPresented = false
        controllerMoreMenuPresented = false
        landscapeControllerAutofocusTask?.cancel()
        landscapeControllerAutofocusTask = nil
        coverFlowPreheater.cancel()
        library.coverWorkTask?.cancel()
        library.coverWorkTask = nil
        focusScrollCoordinator.cancel()
        library.gameLibraryActivity.cancelScheduledReload()
        library.gameLibraryActivity.cancelLoad()
        gameplayLaunchCardRegistry.removeAll()
        favoriteAnimationValues.removeAll(keepingCapacity: false)
        CoverThumbnailCache.shared.releaseInactiveLibraryImages()
        controllerInput?.setNavigationCaptured(
            false,
            owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
            priority: 300
        )
        guard case .playing = appState.currentScreen else { return }
        releaseLibraryResourcesForGameplay()
    }
}
