// GameLibraryCoverFlowLayout.swift — the landscape cover flow layout, its cover preheater and scroll observer
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryCoverFlowLayout: View {
    let library: GameLibraryModel
    let controller: GameLibraryControllerCoordinator
    let style: GameLibraryCardStyle
    let containerSize: CGSize
    let controllerInput: MenuControllerInputRouter?
    let displayedRunningGameName: String?
    let hasCustomBackground: Bool

    var body: some View {
        // One saved metric set, so connecting a pad never resizes the rail.
        let controllerConnected = controllerInput?.hasConnectedController == true
        let metrics = GameLibraryCoverFlowMetrics(
            containerSize: containerSize,
            controllerOptimized: true,
            cardScale: style.resolvedGameCardScale,
            widthScale: style.resolvedGameCardWidthScale,
            heightScale: style.resolvedGameCardHeightScale,
            gapScale: style.resolvedGameCardGapScale,
            contentPaddingScale: style.resolvedGameCardContentPaddingScale,
            verticalPaddingScale: style.resolvedGameCardVerticalPaddingScale,
            textSpacingScale: style.resolvedGameCardTextSpacingScale,
            cardCornerRadius: style.resolvedGameCardCornerRadius
        )
        let availableHeight = max(0, containerSize.height - 12)
        let cardWidth = style.coverFlowCardWidth(metrics: metrics)
        // Half a viewport of padding on each side lets the end cards center.
        let horizontalPadding = max(
            metrics.horizontalPadding,
            (containerSize.width - cardWidth) / 2
        )
        return ZStack {
            if hasCustomBackground {
                Color.clear
            } else {
                LinearGradient(
                    colors: [
                        Color(.systemGroupedBackground),
                        Color(.secondarySystemGroupedBackground),
                    ],
                    startPoint: .top,
                    endPoint: .bottom
                )
            }

            ScrollViewReader { proxy in
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(alignment: .center, spacing: metrics.itemSpacing) {
                    if library.games.isEmpty && displayedRunningGameName == nil {
                        GameLibraryEmptyState(controller: controller, controllerInput: controllerInput)
                            .frame(
                                width: max(
                                    0,
                                    containerSize.width - (horizontalPadding * 2)
                                ),
                                height: max(
                                    0,
                                    availableHeight - (metrics.verticalPadding * 2)
                                )
                            )
                    }

                    if let gameName = displayedRunningGameName {
                        GameLibraryNowRunningCoverCard(
                            controller: controller,
                            title: controller.displayTitle(forRunningName: gameName),
                            coverGame: controller.runningLibraryEntry(for: gameName),
                            controllerInput: controllerInput,
                            metrics: metrics,
                            controllerConnected: controllerInput?.hasConnectedController == true
                        )
                            .id(gameLibraryNowRunningFocusID)
                            .offset(y: style.resolvedGameLibraryVerticalPosition)
                            .isolatedMenuGlassContainer()
                            .compositingGroup()
                            .scaleEffect(controller.displayedNowRunningCardScale)
                            .opacity(controller.displayedNowRunningCardOpacity)
                            .animation(
                                .smooth(duration: 0.65, extraBounce: 0.04),
                                value: controller.displayedNowRunningCardScale
                            )
                            .animation(.linear(duration: 0.65), value: controller.displayedNowRunningCardOpacity)
                            .transition(.scale(scale: 0.86).combined(with: .opacity))
                    }

                    ForEach(library.games) { game in
                        GameLibraryControllerFocusScope(
                            focusState: controller.controllerFocusState,
                            gameID: game.id,
                            isEnabled: controllerInput != nil
                        ) { controllerFocused in
                            coverFlowCard(
                                game,
                                metrics: metrics,
                                controllerFocused: controllerFocused,
                                visualEffectsEnabled: true
                            )
                        }
                            .id(game.id)
                            .offset(y: style.resolvedGameLibraryVerticalPosition)
                            .registerGameLibraryFocusScrollTarget(
                                for: game.id,
                                in: controller.focusScrollCoordinator,
                                isEnabled: controllerInput != nil
                            )
                    }
                    }
                    .scrollTargetLayout()
                    .frame(
                        minWidth: max(0, containerSize.width - (horizontalPadding * 2)),
                        minHeight: max(0, availableHeight - (metrics.verticalPadding * 2)),
                        alignment: .center
                    )
                    .padding(.horizontal, horizontalPadding)
                    .padding(.vertical, metrics.verticalPadding)
                    .background(alignment: .leading) {
                        if controllerInput == nil {
                            GameLibraryTouchLongPressLayer(library: library, controller: controller)
                        }
                        if controllerInput != nil {
                            ZStack {
                                GameLibraryControllerRightStickTarget(
                                    controllerInput: controllerInput,
                                    availability: controller.controllerScrollAvailability,
                                    axes: .horizontal,
                                    priority: 10,
                                    pointsPerSecond: 820,
                                    onReachedLeadingEdge:
                                        controller.controllerRightStickReachedFirstCoverFlowGame
                                )
                                Color.clear
                                    .id(GameLibraryScrollObserver.topAnchor)
                            }
                            .frame(width: 0)
                        }
                    }
                }
                .scrollIndicators(.hidden)
                .scrollDisabled(false)
                .scrollBounceBehavior(.always, axes: .horizontal)
                .trackGameLibraryScrollPhase(library.gameLibraryActivity)
                .transaction { transaction in
                    if transaction.isContinuous {
                        transaction.animation = nil
                    }
                }
                .overlay {
                    GameLibraryScrollObserver(
                        focusState: controller.controllerFocusState,
                        proxy: proxy,
                        favoriteScrollRequest: controller.favoriteScrollRequest,
                        restoreState: controller.libraryScrollRestoreState,
                        topAlignment: .leading,
                        usesCompactControllerAnimation: true,
                        focusScrollCoordinator: controller.focusScrollCoordinator,
                        focusScrollAxis: .horizontal
                    )
                }
            }
        }
        .frame(height: availableHeight)
        .overlay {
            ZStack {
                if controllerConnected {
                    let geometry = controllerCoverFlowFocusGeometry(
                        metrics: metrics,
                        availableHeight: availableHeight
                    )
                    GameLibraryControllerCoverFlowLaunchAnchor(
                        focusState: controller.controllerFocusState,
                        registry: controller.gameplayLaunchCardRegistry,
                        width: geometry.width,
                        height: geometry.height,
                        centerCorrection: geometry.centerCorrection,
                        verticalPosition: style.resolvedGameLibraryVerticalPosition
                    )
                }
            }
        }
        // Keep the cover-flow rail below the top toolbar. The focused card now
        // publishes its own transformed bounds, so its focus orbs move with the
        // cover-flow content instead of targeting a viewport-level rectangle.
        .frame(maxHeight: .infinity, alignment: .top)
        .offset(y: 30)
    }

    private func controllerCoverFlowFocusGeometry(
        metrics: GameLibraryCoverFlowMetrics,
        availableHeight: CGFloat
    ) -> (width: CGFloat, height: CGFloat, centerCorrection: CGFloat) {
        let width = style.coverFlowCardWidth(metrics: metrics)
        let titleHeight: CGFloat = style.hideGameName
            ? 0
            : (style.singleLineGameNames
                ? (metrics.isCompact ? 20 : 22)
                : (metrics.isCompact ? 38 : 46)
            ) * style.resolvedGameNameTextScale
        let metadataHeight: CGFloat = style.hideGameInfo
            ? 0
            : (metrics.isCompact ? 14 : 17) * style.resolvedGameInfoTextScale
        let coverToTextSpacing = style.hideGameName && style.hideGameInfo
            ? 0
            : metrics.cardSpacing
        let interTextSpacing = style.hideGameName || style.hideGameInfo
            ? 0
            : 4 * style.resolvedGameCardTextSpacingScale
        let height = metrics.cardVerticalPadding * 2
            + metrics.coverHeight
            + coverToTextSpacing
            + titleHeight
            + interTextSpacing
            + metadataHeight
        let stackMinimumHeight = max(
            0,
            availableHeight - (metrics.verticalPadding * 2)
        )
        return (
            width,
            height,
            max(0, (height - stackMinimumHeight) / 2)
        )
    }

    @ViewBuilder
    private func coverFlowCard(
        _ game: ISOEntry,
        metrics: GameLibraryCoverFlowMetrics,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        let running = library.isRunning(game)
        GameLibraryCoverFlowCard(
            game: game,
            running: running,
            stopping: running && controller.nowRunningRemovalAnimationActive,
            style: style,
            favoriteBounce: controller.favoriteAnimationValues[game.id] ?? 0,
            glowAllowed: !controller.controllerNavigationPresentationActive,
            registry: controller.gameplayLaunchCardRegistry,
            actions: controller,
            controllerFocused: controllerFocused,
            visualEffectsEnabled: visualEffectsEnabled,
            metrics: metrics,
            controllerInputActive: controllerInput != nil
        )
    }
}

@MainActor
final class GameLibraryCoverFlowPreheater {
    private struct Request {
        let items: [CoverThumbnailPreheatItem]
        let width: CGFloat
        let height: CGFloat
        let scale: CGFloat
    }

    private var task: Task<Void, Never>?
    private var pendingRequest: Request?

    func schedule(
        items: [CoverThumbnailPreheatItem],
        width: CGFloat,
        height: CGFloat,
        scale: CGFloat
    ) {
        guard !items.isEmpty else {
            return
        }
        pendingRequest = Request(
            items: items,
            width: width,
            height: height,
            scale: scale
        )
        guard task == nil else { return }
        task = Task(priority: .utility) { [weak self] in
            await self?.drainRequests()
        }
    }

    private func drainRequests() async {
        while !Task.isCancelled, let request = pendingRequest {
            pendingRequest = nil
            await CoverThumbnailCache.shared.preheat(
                request.items,
                width: request.width,
                height: request.height,
                scale: request.scale
            )
        }
        task = nil
    }

    func cancel() {
        task?.cancel()
        task = nil
        pendingRequest = nil
    }
}

struct GameLibraryScrollObserver: View {
    static let topAnchor = "game-library-top"

    let focusState: GameLibraryControllerFocusState
    let proxy: ScrollViewProxy
    let favoriteScrollRequest: GameLibraryFavoriteScrollRequest?
    let restoreState: GameLibraryScrollRestoreState
    let topAlignment: UnitPoint
    var usesCompactControllerAnimation = false
    var focusScrollCoordinator: GameLibraryFocusScrollCoordinator? = nil
    var focusScrollAxis: GameLibraryFocusScrollAxis = .vertical

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(
                of: restoreState.topRequestSequence,
                initial: true
            ) { _, sequence in
                guard sequence > 0,
                      restoreState.consumeTopRestore(sequence) else { return }
                DispatchQueue.main.async {
                    var transaction = Transaction()
                    transaction.animation = nil
                    withTransaction(transaction) {
                        proxy.scrollTo(Self.topAnchor, anchor: topAlignment)
                    }
                }
            }
            .onChange(
                of: focusState.selectionScrollRequest,
                initial: true
            ) { _, request in
                guard let request else { return }
                // Lazy content is still mounting the focused card, so scroll on
                // the next main turn; rapid moves coalesce to the newest request.
                DispatchQueue.main.async {
                    guard focusState.selectionScrollRequest == request else {
                        return
                    }
                    if let focusScrollCoordinator {
                        focusScrollCoordinator.focus(
                            gameID: request.gameID,
                            proxy: proxy,
                            reduceMotion: reduceMotion,
                            axis: focusScrollAxis,
                            usesCompactAnimation:
                                usesCompactControllerAnimation
                        )
                    } else if usesCompactControllerAnimation {
                        if reduceMotion {
                            var transaction = Transaction(animation: nil)
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        } else {
                            withAnimation(
                                .smooth(duration: 0.32, extraBounce: 0.015)
                            ) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        }
                    } else {
                        if reduceMotion {
                            var transaction = Transaction(animation: nil)
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        } else {
                            withAnimation(
                                .smooth(duration: 0.20, extraBounce: 0)
                            ) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        }
                    }
                }
            }
            .onChange(of: favoriteScrollRequest) { _, request in
                guard let request else { return }
                DispatchQueue.main.async {
                    // Resolve the destination after the favorite-first sort
                    // has completed a layout pass.
                    DispatchQueue.main.async {
                        if request.destination == .nowRunning {
                            var transaction = Transaction(animation: nil)
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                let nowRunningAnchor: UnitPoint
                                switch focusScrollAxis {
                                case .horizontal:
                                    nowRunningAnchor = .center
                                case .vertical:
                                    nowRunningAnchor = topAlignment
                                }
                                proxy.scrollTo(
                                    gameLibraryNowRunningFocusID,
                                    anchor: nowRunningAnchor
                                )
                            }
                            return
                        }
                        withAnimation(.smooth(duration: 0.42, extraBounce: 0.02)) {
                            switch request.destination {
                            case .firstGame(let gameID):
                                proxy.scrollTo(gameID, anchor: topAlignment)
                            case .game(let gameID):
                                proxy.scrollTo(gameID, anchor: .center)
                            case .nowRunning:
                                break
                            }
                        }
                    }
                }
            }
    }
}
