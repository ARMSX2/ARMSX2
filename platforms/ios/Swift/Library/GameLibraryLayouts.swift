// GameLibraryLayouts.swift — the library's list, grid and empty layouts
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryListLayout: View {
    let library: GameLibraryModel
    let controller: GameLibraryControllerCoordinator
    let style: GameLibraryCardStyle
    let controllerInput: MenuControllerInputRouter?
    let displayedRunningGameName: String?
    let showsPageOwnedLargeTitle: Bool
    let useGamesLogo: Bool
    let hasCustomBackground: Bool

    var body: some View {
        ScrollViewReader { proxy in
            List {
                if controllerInput == nil {
                    GameLibraryTouchLongPressLayer(library: library, controller: controller)
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                if controllerInput != nil {
                    ZStack {
                        GameLibraryControllerRightStickTarget(
                            controllerInput: controllerInput,
                            availability: controller.controllerScrollAvailability,
                            axes: .vertical,
                            priority: 10,
                            onReachedLeadingEdge: controller.controllerRightStickReachedTop
                        )
                        Color.clear
                            .id(GameLibraryScrollObserver.topAnchor)
                    }
                    .frame(height: 0)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                }
                if showsPageOwnedLargeTitle {
                    EmbeddedMenuLargeTitle(
                        title: controller.settings.localized("Games"),
                        usesARMSX2Logo: useGamesLogo
                    )
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                if library.games.isEmpty && displayedRunningGameName == nil {
                    GameLibraryEmptyState(controller: controller, controllerInput: controllerInput)
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                if let gameName = displayedRunningGameName {
                    GameLibraryNowRunningSection(
                        controller: controller,
                        title: controller.displayTitle(forRunningName: gameName),
                        coverGame: controller.runningLibraryEntry(for: gameName),
                        controllerInput: controllerInput,
                        opacity: controller.displayedNowRunningCardOpacity,
                        scale: controller.displayedNowRunningCardScale,
                        hasCustomBackground: hasCustomBackground
                    )
                    .id(gameLibraryNowRunningFocusID)
                    .offset(y: style.resolvedGameLibraryVerticalPosition)
                }
                ForEach(library.games) { game in
                    GameLibraryControllerFocusScope(
                        focusState: controller.controllerFocusState,
                        gameID: game.id,
                        isEnabled: controllerInput != nil
                    ) { controllerFocused in
                        gameRow(
                            game,
                            controllerFocused: controllerFocused,
                            visualEffectsEnabled: true
                        )
                        .gameCardTintMenuBackgroundListRow(
                            true,
                            contentPaddingScale:
                                style.resolvedGameCardContentPaddingScale,
                            verticalPaddingScale:
                                style.resolvedGameCardVerticalPaddingScale
                        )
                        .modifier(
                            GameLibraryControllerFocusModifier(
                                isFocused: controllerFocused,
                                cornerRadius: 16,
                                isFavorite: game.isFavorite,
                                favoriteGlowEnabled:
                                    style.favoriteGlowEnabled,
                                isEnabled: controllerInput != nil
                                    || (game.isFavorite
                                        && style
                                            .favoriteGlowEnabled)
                            )
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
            .contentMargins(.top, 0, for: .scrollContent)
            .scrollIndicators(.hidden)
            .modifier(GameLibraryListTopEdgeEffectModifier())
            .scrollContentBackground(.hidden)
            .scrollDisabled(false)
            .scrollBounceBehavior(.always)
            .trackGameLibraryScrollPhase(library.gameLibraryActivity)
            .transaction { transaction in
                if transaction.isContinuous || library.gameLibraryActivity.isScrolling {
                    transaction.animation = nil
                }
            }
            .background {
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
            }
            .overlay {
                GameLibraryScrollObserver(
                    focusState: controller.controllerFocusState,
                    proxy: proxy,
                    favoriteScrollRequest: controller.favoriteScrollRequest,
                    restoreState: controller.libraryScrollRestoreState,
                    topAlignment: .top,
                    focusScrollCoordinator: controller.focusScrollCoordinator,
                    focusScrollAxis: .vertical
                )
            }
        }
    }

    @ViewBuilder
    private func gameRow(
        _ game: ISOEntry,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        let running = library.isRunning(game)
        if controllerInput == nil {
            GameLibraryTouchRow(
                game: game,
                running: running,
                stopping: running && controller.nowRunningRemovalAnimationActive,
                style: style,
                favoriteBounce: controller.favoriteAnimationValues[game.id] ?? 0,
                glowAllowed: !controller.controllerNavigationPresentationActive,
                registry: controller.gameplayLaunchCardRegistry,
                actions: controller
            )
        } else {
            GameLibraryControllerRow(
                game: game,
                running: running,
                stopping: running && controller.nowRunningRemovalAnimationActive,
                style: style,
                favoriteBounce: controller.favoriteAnimationValues[game.id] ?? 0,
                glowAllowed: !controller.controllerNavigationPresentationActive,
                registry: controller.gameplayLaunchCardRegistry,
                actions: controller,
                controllerFocused: controllerFocused,
                visualEffectsEnabled: visualEffectsEnabled
            )
        }
    }
}

struct GameLibraryGridLayout: View {
    let library: GameLibraryModel
    let controller: GameLibraryControllerCoordinator
    let style: GameLibraryCardStyle
    let containerSize: CGSize
    let controllerInput: MenuControllerInputRouter?
    let displayedRunningGameName: String?
    let showsPageOwnedLargeTitle: Bool
    let useGamesLogo: Bool
    let hasCustomBackground: Bool

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 16) {
                    if showsPageOwnedLargeTitle {
                        EmbeddedMenuLargeTitle(
                            title: controller.settings.localized("Games"),
                            usesARMSX2Logo: useGamesLogo
                        )
                    }

                    if library.games.isEmpty && displayedRunningGameName == nil {
                        GameLibraryEmptyState(controller: controller, controllerInput: controllerInput)
                            .frame(minHeight: 300)
                    }

                    if let gameName = displayedRunningGameName {
                        GameLibraryNowRunningCard(
                            controller: controller,
                            title: controller.displayTitle(forRunningName: gameName),
                            coverGame: controller.runningLibraryEntry(for: gameName),
                            controllerInput: controllerInput
                        )
                            .id(gameLibraryNowRunningFocusID)
                            .offset(y: style.resolvedGameLibraryVerticalPosition)
                            .isolatedMenuGlassContainer()
                            .padding(.horizontal)
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

                    LazyVGrid(
                        columns: gameGridColumns(for: containerSize.width),
                        spacing: 18 * style.resolvedGameCardGapScale
                    ) {
                        ForEach(library.games) { game in
                            GameLibraryControllerFocusScope(
                                focusState: controller.controllerFocusState,
                                gameID: game.id,
                                isEnabled: controllerInput != nil
                            ) { controllerFocused in
                                gameGridCard(
                                    game,
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
                    .padding(.horizontal)
                    .padding(.bottom, 20)
                }
                .background(alignment: .top) {
                    if controllerInput == nil {
                        GameLibraryTouchLongPressLayer(library: library, controller: controller)
                    }
                    if controllerInput != nil {
                        ZStack {
                            GameLibraryControllerRightStickTarget(
                                controllerInput: controllerInput,
                                availability: controller.controllerScrollAvailability,
                                axes: .vertical,
                                priority: 10,
                                onReachedLeadingEdge:
                                    controller.controllerRightStickReachedTop
                            )
                            Color.clear
                                .id(GameLibraryScrollObserver.topAnchor)
                        }
                        .frame(height: 0)
                    }
                }
                .padding(.top, showsPageOwnedLargeTitle ? 0 : 12)
            }
            .contentMargins(.top, 0, for: .scrollContent)
            .scrollIndicators(.hidden)
            .scrollDisabled(false)
            .scrollBounceBehavior(.always)
            .trackGameLibraryScrollPhase(library.gameLibraryActivity)
            .transaction { transaction in
                if transaction.isContinuous || library.gameLibraryActivity.isScrolling {
                    transaction.animation = nil
                }
            }
            .background(
                Group {
                    if hasCustomBackground {
                        Color.clear
                    } else {
                        LinearGradient(
                            colors: [Color(.systemGroupedBackground), Color(.secondarySystemGroupedBackground)],
                            startPoint: .top,
                            endPoint: .bottom
                        )
                    }
                }
            )
            .overlay {
                GameLibraryScrollObserver(
                    focusState: controller.controllerFocusState,
                    proxy: proxy,
                    favoriteScrollRequest: controller.favoriteScrollRequest,
                    restoreState: controller.libraryScrollRestoreState,
                    topAlignment: .top,
                    focusScrollCoordinator: controller.focusScrollCoordinator,
                    focusScrollAxis: .vertical
                )
            }
        }
    }

    private func gameGridColumns(for containerWidth: CGFloat) -> [GridItem] {
        let minimumCardWidth = 142 * style.resolvedGameCardScale
            * style.resolvedGameCardWidthScale
        return Array(
            repeating: GridItem(
                .flexible(minimum: minimumCardWidth),
                spacing: 14 * style.resolvedGameCardGapScale,
                alignment: .top
            ),
            count: Self.gameGridColumnCount(
                for: containerWidth,
                cardScale: style.resolvedGameCardScale,
                widthScale: style.resolvedGameCardWidthScale,
                gapScale: style.resolvedGameCardGapScale
            )
        )
    }

    static func gameGridColumnCount(
        for containerWidth: CGFloat,
        cardScale: CGFloat,
        widthScale: CGFloat,
        gapScale: CGFloat
    ) -> Int {
        let availableWidth = max(0, containerWidth - 32)
        let minimumCardWidth = 142 * cardScale
            * widthScale
        return max(
            1,
            Int(
                (availableWidth + 14 * gapScale)
                    / (minimumCardWidth + 14 * gapScale)
            )
        )
    }

    @ViewBuilder
    private func gameGridCard(
        _ game: ISOEntry,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        let running = library.isRunning(game)
        if controllerInput == nil {
            GameLibraryTouchGridCard(
                game: game,
                running: running,
                stopping: running && controller.nowRunningRemovalAnimationActive,
                style: style,
                favoriteBounce: controller.favoriteAnimationValues[game.id] ?? 0,
                glowAllowed: !controller.controllerNavigationPresentationActive,
                registry: controller.gameplayLaunchCardRegistry,
                actions: controller
            )
        } else {
            GameLibraryControllerGridCard(
                game: game,
                running: running,
                stopping: running && controller.nowRunningRemovalAnimationActive,
                style: style,
                favoriteBounce: controller.favoriteAnimationValues[game.id] ?? 0,
                glowAllowed: !controller.controllerNavigationPresentationActive,
                registry: controller.gameplayLaunchCardRegistry,
                actions: controller,
                controllerFocused: controllerFocused,
                visualEffectsEnabled: visualEffectsEnabled
            )
        }
    }
}

struct GameLibraryEmptyLayout: View {
    let controller: GameLibraryControllerCoordinator
    let containerSize: CGSize
    let controllerInput: MenuControllerInputRouter?
    let showsPageOwnedLargeTitle: Bool
    let useGamesLogo: Bool
    let hasCustomBackground: Bool

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                if controllerInput != nil {
                    GameLibraryControllerRightStickTarget(
                        controllerInput: controllerInput,
                        availability: controller.controllerScrollAvailability,
                        axes: .vertical,
                        priority: 10,
                        onReachedLeadingEdge: controller.controllerRightStickReachedTop
                    )
                    .frame(height: 0)
                }
                if showsPageOwnedLargeTitle {
                    EmbeddedMenuLargeTitle(
                        title: controller.settings.localized("Games"),
                        usesARMSX2Logo: useGamesLogo
                    )
                }

                GameLibraryEmptyState(controller: controller, controllerInput: controllerInput)
                    .frame(
                        maxWidth: .infinity,
                        minHeight: max(
                            300,
                            containerSize.height
                                - (showsPageOwnedLargeTitle ? 64 : 0)
                        ),
                        alignment: .center
                    )
            }
        }
        .contentMargins(.top, 0, for: .scrollContent)
        .scrollIndicators(.hidden)
        .scrollDisabled(false)
        .scrollBounceBehavior(.always)
        .background(
            Group {
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
            }
        )
    }
}

struct GameLibraryEmptyState: View {
    let controller: GameLibraryControllerCoordinator
    let controllerInput: MenuControllerInputRouter?

    @Environment(\.menuTabIsActive) private var menuTabIsActive

    var body: some View {
        let controllerFocused = menuTabIsActive
            && controllerInput?.isControllerNavigationEnabled == true
            && !controller.controllerFocusReleased
            && controllerInput?.navigationZone == .library

        return VStack(spacing: 16) {
            Image(systemName: "opticaldisc")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            Text(controller.settings.localized("No Games Found"))
                .font(.title2)
                .fontWeight(.semibold)
            Text(controller.settings.localized("Import PS2 disc images to add them here."))
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                controller.openGameImporter()
            } label: {
                MenuImportActionLabel(
                    title: controller.settings.localized("Import Games"),
                    explicitlyFocused: controllerFocused
                )
            }
            .buttonStyle(.plain)
            .focusEffectDisabled()
            .accessibilityAddTraits(controllerFocused ? .isSelected : [])
            .modifier(
                GameLibraryControllerFocusModifier(
                    isFocused: controllerFocused,
                    cornerRadius: 12,
                    isEnabled: controllerInput != nil,
                    performanceOptimized: true,
                    focusedScale: 1
                )
            )
            .controllerNavigationOrbTarget(
                id: "library.import-library.games",
                isActive: controllerFocused,
                palette: .blue,
                inset: 2,
                orbScale: 0.9,
                priority: 10
            )
			Label(controller.settings.localized("External Games are in Settings > Storage"), systemImage: "externaldrive")
				.font(.caption)
				.foregroundStyle(.secondary)
			Text("\(controller.settings.localized("Supported Formats")): ISO, CHD, BIN, CSO, ZSO, GZ, ELF")
				.font(.caption)
				.foregroundStyle(.secondary)
				.multilineTextAlignment(.center)
        }
        .padding()
        .frame(maxWidth: .infinity, minHeight: 240)
        .centredInLeadingPaneOnFold()
    }
}

struct GameLibraryTouchLongPressLayer: View {
    let library: GameLibraryModel
    let controller: GameLibraryControllerCoordinator

    var body: some View {
        GameLibraryTouchLongPressObserver(
            registry: controller.gameplayLaunchCardRegistry,
            onTouchBegan: controller.suppressThemeShortcutForGameCardTouch
        ) { gameID in
            guard let game = library.games.first(where: { $0.id == gameID }) else {
                return
            }
            controller.handleGameContextMenuLongPress(for: game)
        }
        .frame(width: 0, height: 0)
    }
}

private struct GameLibraryScrollPhaseModifier: ViewModifier {
    let activity: GameLibraryActivityState

    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.onScrollPhaseChange { _, newPhase in
                activity.isScrolling = newPhase.isScrolling
            }
        } else {
            content
        }
    }
}

extension View {
    func trackGameLibraryScrollPhase(
        _ activity: GameLibraryActivityState
    ) -> some View {
        modifier(GameLibraryScrollPhaseModifier(activity: activity))
    }
}

private struct GameLibraryListTopEdgeEffectModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            content.scrollEdgeEffectHidden(true, for: .top)
        } else {
            content
        }
    }
}
