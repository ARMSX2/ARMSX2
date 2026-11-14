// GameLibraryControllerFocus.swift — The library's controller focus states, requests and focus modifiers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor
@Observable
final class GameLibraryControllerCardFocusState {
    var isFocused: Bool

    init(isFocused: Bool) {
        self.isFocused = isFocused
    }
}

struct GameLibrarySelectionScrollRequest: Equatable {
    let sequence: UInt64
    let gameID: String
}

enum GameLibraryFavoriteScrollDestination: Equatable {
    case firstGame(String)
    case game(String)
    case nowRunning
}

struct GameLibraryFavoriteScrollRequest: Equatable {
    let sequence: UInt64
    let destination: GameLibraryFavoriteScrollDestination
}

let gameLibraryNowRunningFocusID = "game-library-now-running"

let gameLibraryControllerMemoryScope = "menu.games.content"

let gameLibraryControllerCardMemoryScope = "menu.games.last-card"

enum GameLibraryNowRunningControllerAction: Int, CaseIterable {
    case resume
    case stop
}

@MainActor
@Observable
final class GameLibraryNowRunningControllerFocusState {
    var selectedAction: GameLibraryNowRunningControllerAction?
}

struct GameLibraryNowRunningControllerFocusModifier: ViewModifier {
    let state: GameLibraryNowRunningControllerFocusState
    let action: GameLibraryNowRunningControllerAction
    let controllerInput: MenuControllerInputRouter?
    let menuTabIsActive: Bool

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.uiAccentColour) private var accentColour

    private var isFocused: Bool {
        controllerInput != nil
            && controllerInput?.isControllerNavigationEnabled == true
            && menuTabIsActive
            && controllerInput?.navigationZone == .library
            && state.selectedAction == action
    }

    func body(content: Content) -> some View {
        Group {
            if isFocused {
                content
                    .foregroundStyle(action == .stop ? Color.red : Color.white)
                    .background {
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .fill(Color.black.opacity(0.2))
                    }
                    .controllerFocusBoxPresentation(
                        isVisible: true,
                        cornerRadius: 12
                    )
                    .scaleEffect(reduceMotion ? 1 : 1.01)
                    .shadow(color: .black.opacity(0.1), radius: 4, y: 2)
            } else {
                content
                    .foregroundStyle(action == .stop ? Color.red : Color.white)
            }
        }
            .focusEffectDisabled()
            .controllerNavigationOrbTarget(
                id: "library.now-running.\(action.rawValue)",
                isActive: isFocused,
                palette: .blue,
                inset: 2,
                orbScale: 0.85,
                priority: 20
            )
            .tint(action == .stop ? .red : accentColour)
            .zIndex(isFocused ? 20 : 0)
            .animation(
                reduceMotion
                    ? .linear(duration: 0.1)
                    : .smooth(duration: 0.16, extraBounce: 0),
                value: isFocused
            )
    }
}

@MainActor
@Observable
final class GameLibraryControllerFocusState {
    @ObservationIgnored private var storedSelectedGameID: String?
    @ObservationIgnored private var storedIsActive = false
    @ObservationIgnored private var isLibraryZone = true
    @ObservationIgnored private var isLibraryVisible = true
    @ObservationIgnored private var cardStates: [String: GameLibraryControllerCardFocusState] = [:]
    @ObservationIgnored private var gameIndexByID: [String: Int] = [:]
    @ObservationIgnored private var scrollSequence: UInt64 = 0
    @ObservationIgnored private var suppressNextSelectionScroll = false

    var selectionScrollRequest: GameLibrarySelectionScrollRequest?

    var selectedGameID: String? {
        get { storedSelectedGameID }
        set {
            guard storedSelectedGameID != newValue else { return }
            let previous = storedSelectedGameID
            storedSelectedGameID = newValue
            refreshCard(previous)
            refreshCard(newValue)
            publishSelectionScrollIfNeeded()
        }
    }

    var isActive: Bool {
        get { storedIsActive }
        set {
            guard storedIsActive != newValue else { return }
            storedIsActive = newValue
            refreshCard(presentedGameID)
        }
    }

    func setLibraryZoneActive(_ active: Bool) {
        guard isLibraryZone != active else { return }
        isLibraryZone = active
        refreshCard(presentedGameID)
    }

    func setLibraryVisible(_ visible: Bool) {
        guard isLibraryVisible != visible else { return }
        isLibraryVisible = visible
        refreshCard(presentedGameID)
    }

    func cardState(for gameID: String) -> GameLibraryControllerCardFocusState {
        if let state = cardStates[gameID] {
            return state
        }
        let state = GameLibraryControllerCardFocusState(
            isFocused: isFocused(gameID: gameID)
        )
        cardStates[gameID] = state
        return state
    }

    func isFocused(gameID: String) -> Bool {
        guard isLibraryVisible else { return false }
        return storedIsActive
            && isLibraryZone
            && storedSelectedGameID == gameID
    }

    func updateGameIDs(_ gameIDs: [String]) {
        var updatedIndex: [String: Int] = [:]
        updatedIndex.reserveCapacity(gameIDs.count)
        for (index, gameID) in gameIDs.enumerated() {
            updatedIndex[gameID] = index
        }
        gameIndexByID = updatedIndex
        let retainedIDs = Set(gameIDs)
        cardStates = cardStates.filter { retainedIDs.contains($0.key) }
    }

    func releaseCardStates() {
        // The semantic selection is persisted separately. Per-card observable
        // wrappers are only a controller presentation cache and can be rebuilt
        // lazily if a gamepad reconnects.
        cardStates.removeAll(keepingCapacity: false)
    }

    func index(for gameID: String) -> Int? {
        gameIndexByID[gameID]
    }

    func requestSelectionScroll() {
        publishSelectionScrollIfNeeded()
    }

    func selectWithoutScrolling(_ gameID: String) {
        guard storedSelectedGameID != gameID else { return }
        suppressNextSelectionScroll = true
        selectedGameID = gameID
    }

    private func refreshCard(_ gameID: String?) {
        guard let gameID, let state = cardStates[gameID] else { return }
        state.isFocused = isFocused(gameID: gameID)
    }

    private var presentedGameID: String? {
        storedSelectedGameID
    }

    private func publishSelectionScrollIfNeeded() {
        if suppressNextSelectionScroll {
            suppressNextSelectionScroll = false
            return
        }
        guard storedIsActive, isLibraryZone, isLibraryVisible,
              let gameID = storedSelectedGameID else { return }
        scrollSequence &+= 1
        selectionScrollRequest = GameLibrarySelectionScrollRequest(
            sequence: scrollSequence,
            gameID: gameID
        )
    }
}

struct GameLibraryControllerCommandListener: View {
    let controllerInput: MenuControllerInputRouter?
    let focusState: GameLibraryControllerFocusState
    let libraryVisible: Bool
    let onCommand: (MenuControllerInputEvent) -> Void
    let onLibraryEntry: (Bool) -> Void
    let onFocusRelease: () -> Void
    let onRightStickScrollEnded: () -> Void
    let onInputAvailabilityChanged: () -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput?.latestEvent) { _, event in
                guard let event else { return }
                onCommand(event)
            }
            .onChange(of: controllerInput?.latestLibraryEntryRequest) { _, request in
                guard let request else { return }
                onLibraryEntry(request.preferLast)
            }
            .onChange(of: controllerInput?.focusReleaseSequence) { _, _ in
                onFocusRelease()
            }
            .onChange(
                of: controllerInput?.rightStickScrollEndSequence
            ) { _, _ in
                onRightStickScrollEnded()
            }
            .onChange(
                of: controllerInput?.isControllerNavigationEnabled
            ) { _, enabled in
                if enabled == false { onFocusRelease() }
                onInputAvailabilityChanged()
            }
            .onChange(of: controllerInput?.navigationZone, initial: true) { _, zone in
                focusState.setLibraryZoneActive(zone == .library)
                onInputAvailabilityChanged()
            }
            .onChange(of: controllerInput?.isNavigationCaptured, initial: true) { _, _ in
                onInputAvailabilityChanged()
            }
            .onChange(of: libraryVisible, initial: true) { _, visible in
                focusState.setLibraryVisible(visible)
                onInputAvailabilityChanged()
            }
    }
}

@MainActor
@Observable
final class GameLibraryControllerScrollAvailability {
    var isEnabled = false
}

struct GameLibraryControllerRightStickTarget: View {
    let controllerInput: MenuControllerInputRouter?
    let availability: GameLibraryControllerScrollAvailability
    let axes: MenuControllerScrollAxes
    var priority = 10
    var pointsPerSecond: CGFloat = 760
    var onReachedLeadingEdge: (@MainActor () -> Void)?

    var body: some View {
        ControllerRightStickScrollTarget(
            controllerInput: controllerInput,
            axes: axes,
            priority: priority,
            isEnabled: availability.isEnabled,
            pointsPerSecond: pointsPerSecond,
            onReachedLeadingEdge: onReachedLeadingEdge
        )
    }
}

@MainActor
@Observable
final class GameLibraryScrollRestoreState {
    var topRequestSequence: UInt64 = 0
    @ObservationIgnored private var consumedTopRequestSequence: UInt64 = 0

    func requestTopRestore() {
        topRequestSequence &+= 1
    }

    func consumeTopRestore(_ sequence: UInt64) -> Bool {
        guard sequence > consumedTopRequestSequence else { return false }
        consumedTopRequestSequence = sequence
        return true
    }
}

enum GameLibraryFocusScrollAxis {
    case horizontal
    case vertical
}

struct GameLibraryControllerFocusScope<Content: View>: View {
    let focusState: GameLibraryControllerFocusState
    let gameID: String
    let isEnabled: Bool
    let content: (Bool) -> Content

    var body: some View {
        // Touch-only library browsing does not need one observable focus object
        // per materialized card. The conditional expression preserves the card
        // subtree's identity when a controller connects.
        content(isEnabled ? focusState.cardState(for: gameID).isFocused : false)
    }
}

/// Cover flow always brings the focused controller card to this center frame.
/// Register one reusable launch/context-menu geometry view here instead of one
/// GeometryReader plus UIViewRepresentable on every materialized card.
struct GameLibraryControllerCoverFlowLaunchAnchor: View {
    let focusState: GameLibraryControllerFocusState
    let registry: GameplayLaunchCardRegistry
    let width: CGFloat
    let height: CGFloat
    let centerCorrection: CGFloat
    let verticalPosition: CGFloat

    var body: some View {
        if let gameID = focusState.selectionScrollRequest?.gameID {
            Color.clear
                .frame(width: width, height: height)
                .scaleEffect(1.10)
                .offset(y: centerCorrection - 16 + verticalPosition)
                .registerGameplayLaunchCard(for: gameID, in: registry)
                .allowsHitTesting(false)
                .accessibilityHidden(true)
        }
    }
}

struct GameLibraryControllerFocusModifier: ViewModifier {
    let isFocused: Bool
    let cornerRadius: CGFloat
    var isFavorite = false
    var favoriteGlowEnabled = false
    var isEnabled = true
    var backgroundEffectsEnabled = true
    var performanceOptimized = false
    var focusedScale: CGFloat? = nil
    var optimizedAnimationDuration: Double = 0.12

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            let selectedScale = focusedScale
                ?? (performanceOptimized ? 1.08 : 1.12)
            content
                .focusEffectDisabled()
                .scaleEffect(
                    isFocused ? selectedScale : 1
                )
                .controllerFocusBoxPresentation(
                    isVisible: isFocused,
                    cornerRadius: cornerRadius,
                    performanceOptimized: performanceOptimized
                )
                .background {
                    if !isFocused,
                       isFavorite,
                       favoriteGlowEnabled,
                       backgroundEffectsEnabled {
                        // Favorites retain a themed glow without running a full
                        // animated focus-artwork timeline for every visible card.
                        ControllerBottomNavigationFocusGlow(
                            cornerRadius: cornerRadius
                        )
                    }
                }
                .shadow(
                    color: Color.black.opacity(
                        isFocused && !performanceOptimized ? 0.26 : 0
                    ),
                    radius: isFocused && !performanceOptimized ? 18 : 0,
                    y: isFocused && !performanceOptimized ? 8 : 0
                )
                // A scaled cover can overlap its neighbors during a rapid
                // retarget. Keep the single focused card above the rail so a
                // held direction never lets an unfocused card cover it.
                .zIndex(isFocused ? 20 : 0)
                .animation(
                    reduceMotion
                        ? .linear(
                            duration: performanceOptimized
                                ? min(0.06, optimizedAnimationDuration)
                                : 0.12
                        )
                        : .smooth(
                            duration: performanceOptimized
                                ? optimizedAnimationDuration
                                : 0.24,
                            extraBounce: 0
                        ),
                    value: isFocused
                )
        } else {
            content
        }
    }
}

enum GameLibraryControllerToolbarAction: Int, CaseIterable {
    // Preserve the previous persisted raw values for Import and Layout.
    case bootBIOS = 0
    case importGames = 1
    case layout = 2
    case more = 3
}

@MainActor
@Observable
final class GameLibraryControllerToolbarFocusState {
    var selectedAction: GameLibraryControllerToolbarAction = .importGames
}

enum GameLibraryControllerAlertKind: String, Equatable {
    case automaticCustomSkin
    case restart
    case stop
    case deleteGameData
    case deleteGame
    case gameAction
    case coverResult
    case replaceFiles
    case backgroundError
}

struct PendingAutomaticCustomSkinLaunch {
    enum Purpose: Equatable {
        case gameLaunch
        case library
    }

    let game: ISOEntry
    var proposals: [AutomaticCustomSkinProposal]
    var selectedProposalIndex: Int
    var setCustomLayout: Bool
    var doNotShowAgain: Bool
    let purpose: Purpose
    let transition: GameplayLaunchTransition?

    init(
        game: ISOEntry,
        proposals: [AutomaticCustomSkinProposal],
        selectedProposalIndex: Int = 0,
        setCustomLayout: Bool = false,
        doNotShowAgain: Bool = true,
        purpose: Purpose,
        transition: GameplayLaunchTransition?
    ) {
        self.game = game
        self.proposals = proposals
        self.selectedProposalIndex = min(
            max(0, selectedProposalIndex),
            max(0, proposals.count - 1)
        )
        self.setCustomLayout = setCustomLayout
        self.doNotShowAgain = doNotShowAgain
        self.purpose = purpose
        self.transition = transition
    }

    var proposal: AutomaticCustomSkinProposal {
        proposals[selectedProposalIndex]
    }
}

struct DeferredControllerContextMenuSelection {
    let action: GameLibraryControllerMenuAction
    let gameID: String
}

@MainActor
@Observable
final class GameLibraryControllerContextMenuState {
    var gameID: String?
    var section: GameLibraryControllerMenuSection = .root
    var selectedAction: GameLibraryControllerMenuAction?
}

struct GameLibraryControllerContextMenuPresenter<Content: View>: View {
    let state: GameLibraryControllerContextMenuState
    let content: (
        _ gameID: String,
        _ section: GameLibraryControllerMenuSection,
        _ selectedAction: GameLibraryControllerMenuAction?
    ) -> Content

    init(
        state: GameLibraryControllerContextMenuState,
        @ViewBuilder content: @escaping (
            _ gameID: String,
            _ section: GameLibraryControllerMenuSection,
            _ selectedAction: GameLibraryControllerMenuAction?
        ) -> Content
    ) {
        self.state = state
        self.content = content
    }

    @ViewBuilder
    var body: some View {
        if let gameID = state.gameID {
            content(gameID, state.section, state.selectedAction)
        }
    }
}

struct ControllerToolbarFocusModifier: ViewModifier {
    let focusState: GameLibraryControllerToolbarFocusState
    let action: GameLibraryControllerToolbarAction
    let controllerInput: MenuControllerInputRouter?
    let menuTabIsActive: Bool
    /// A panel or menu opened from the toolbar draws its own focus.
    let isCovered: Bool
    var baseHorizontalPadding: CGFloat = 0
    var minimumWidth: CGFloat = 36

    @ViewBuilder
    func body(content: Content) -> some View {
        let isFocused = menuTabIsActive && !isCovered
            && controllerInput?.isControllerNavigationEnabled == true
            && controllerInput?.navigationZone == .topToolbar
            && focusState.selectedAction == action

        content.modifier(
            ControllerToolbarFocusVisualModifier(
                isFocused: isFocused,
                focusID: "toolbar.\(action.rawValue)",
                baseHorizontalPadding: baseHorizontalPadding,
                minimumWidth: minimumWidth
            )
        )
    }
}
