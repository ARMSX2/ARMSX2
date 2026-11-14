// GameLibraryControllerCoordinator+Focus.swift — controller focus moves through the library
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

extension GameLibraryControllerCoordinator {
    func scheduleLandscapeControllerAutofocus() {
        guard landscapeControllerAutofocusPending else { return }

        landscapeControllerAutofocusTask?.cancel()
        landscapeControllerAutofocusTask = Task { @MainActor in
            // Rotation replaces the portrait grid with the horizontal cover
            // flow. Let that stable-ID scroll view mount before publishing its
            // centered selection request.
            await Task.yield()
            await Task.yield()
            guard !Task.isCancelled,
                  landscapeControllerAutofocusPending,
                  usesLandscapeGameCardScale,
                  controllerInput?.hasConnectedController == true,
                  controllerLibraryNavigationAllowed,
                  !library.games.isEmpty else { return }

            landscapeControllerAutofocusPending = false
            controllerInput?.setNavigationZone(.library)
            _ = restoreRememberedControllerContentFocus()
                || selectInitialControllerGame(preferLast: true)
            landscapeControllerAutofocusTask = nil
        }
    }

    func handleControllerLibraryEntry(preferLast: Bool) {
        guard controllerLibraryNavigationAllowed else { return }
        controllerFocusReleased = false
        if library.games.isEmpty && displayedRunningGameName == nil {
            // Treat the empty-state import action as the library's one card.
            // This keeps it focusable after entering from either toolbar.
            controllerNavigationActive = true
            controllerSelectedGameID = nil
            return
        }
        if focusRunningControllerGameIfAvailable() { return }
        if restoreRememberedControllerContentFocus() { return }
        if library.games.contains(where: library.isRunning)
            || (displayedRunningGameName != nil && !preferLast) {
            _ = selectInitialControllerGame(preferLast: false)
            return
        }
        _ = selectInitialControllerGame(preferLast: preferLast)
    }

    @discardableResult
    func restoreRememberedControllerContentFocus() -> Bool {
        guard controllerLibraryNavigationAllowed else { return false }
        let rememberedCard = controllerInput?.rememberedNavigationFocusKey(
            forScope: gameLibraryControllerCardMemoryScope
        )
        let rememberedContent = controllerInput?.rememberedNavigationFocusKey(
            forScope: gameLibraryControllerMemoryScope
        )
        // Back from a Now Running action restores it; else the retained card, then the legacy key.
        let candidates = rememberedContent?.hasPrefix("now-running:") == true
            && session.runningGameName != nil && !nowRunningRemovalAnimationActive
                ? [rememberedContent, rememberedCard] : [rememberedCard, rememberedContent]
        guard let key = candidates.compactMap({ $0 }).first(where: { candidate in
            !candidate.hasPrefix("game:") || library.games.contains(where: { game in
                game.id == String(candidate.dropFirst("game:".count))
            })
        }) else { return false }
        controllerFocusReleased = false
        if key.hasPrefix("toolbar:"),
           let rawValue = Int(key.dropFirst("toolbar:".count)),
           let action = GameLibraryControllerToolbarAction(rawValue: rawValue) {
            controllerToolbarFocusState.selectedAction = action
            controllerNavigationActive = false
            controllerNowRunningFocusState.selectedAction = nil
            controllerInput?.setNavigationZone(.topToolbar)
            updateControllerScrollAvailability()
            return true
        }
        if key.hasPrefix("game:") {
            let gameID = String(key.dropFirst("game:".count))
            guard library.games.contains(where: { $0.id == gameID }) else {
                return false
            }
            controllerNavigationActive = true
            controllerNowRunningFocusState.selectedAction = nil
            controllerSelectedGameID = gameID
            controllerFocusState.requestSelectionScroll()
            return true
        }
        if key.hasPrefix("now-running:"),
           displayedRunningGameName != nil,
           let rawValue = Int(key.dropFirst("now-running:".count)),
           let action = GameLibraryNowRunningControllerAction(
               rawValue: rawValue
           ) {
            focusNowRunning(action)
            return true
        }
        return false
    }

    func rememberControllerToolbarFocus() {
        controllerInput?.rememberNavigationFocusKey(
            "toolbar:\(controllerToolbarFocusState.selectedAction.rawValue)",
            forScope: gameLibraryControllerMemoryScope
        )
    }

    /// Gameplay intentionally releases the library's card models. Restore the
    /// running card only after its model is back and the Games tab can accept
    /// controller focus; otherwise the temporary empty array clears selection
    /// and the later reload has no portrait re-entry trigger.
    @discardableResult
    func focusRunningControllerGameIfAvailable() -> Bool {
        guard runningGameControllerFocusPending,
              controllerInput?.hasConnectedController == true,
              controllerLibraryNavigationAllowed,
              let runningGame = library.games.first(where: library.isRunning) else {
            return false
        }

        runningGameControllerFocusPending = false
        landscapeControllerAutofocusPending = false
        landscapeControllerAutofocusTask?.cancel()
        landscapeControllerAutofocusTask = nil
        controllerFocusReleased = false
        controllerInput?.setNavigationZone(.library)
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = runningGame.id
        if usesLandscapeCoverFlow,
           let selectedIndex = controllerFocusState.index(for: runningGame.id)
                ?? library.games.firstIndex(where: { $0.id == runningGame.id }) {
            preheatLandscapeCovers(around: selectedIndex, direction: 0)
        }
        controllerFocusState.requestSelectionScroll()
        updateControllerScrollAvailability()
        return true
    }

    var controllerSelectedGame: ISOEntry? {
        guard let controllerSelectedGameID else { return nil }
        if let index = controllerFocusState.index(for: controllerSelectedGameID),
           library.games.indices.contains(index),
           library.games[index].id == controllerSelectedGameID {
            return library.games[index]
        }
        return library.games.first(where: { $0.id == controllerSelectedGameID })
    }

    @discardableResult
    func selectInitialControllerGame(preferLast: Bool) -> Bool {
        let runningGame = library.games.first(where: library.isRunning)
        if runningGame == nil,
           !preferLast,
           displayedRunningGameName != nil {
            focusNowRunning(.resume)
            return true
        }
        let game: ISOEntry?
        if let runningGame {
            game = runningGame
        } else if preferLast {
            // Keep a live selection when re-entering from another focus region.
            // With none, even on a fresh launch, begin at the first visible game
            // instead of inventing a last-card selection from the entry direction.
            game = controllerSelectedGame ?? library.games.first
        } else {
            game = library.games.first
        }
        guard let game else {
            if displayedRunningGameName != nil {
                focusNowRunning(.resume)
                return true
            }
            return false
        }
        let alreadySelected = controllerSelectedGameID == game.id
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = game.id
        if usesLandscapeCoverFlow,
           let selectedIndex = controllerFocusState.index(for: game.id)
                ?? library.games.firstIndex(where: { $0.id == game.id }) {
            preheatLandscapeCovers(around: selectedIndex, direction: 0)
        }
        if alreadySelected {
            controllerFocusState.requestSelectionScroll()
        }
        return true
    }

    var usesLandscapeCoverFlow: Bool {
        appearance.libraryLayout == "grid"
            && appearance.landscapeCoverFlowEnabled
            && usesLandscapeGameCardScale
    }

    private func preheatLandscapeCovers(
        around selectedIndex: Int,
        direction: Int
    ) {
        guard usesLandscapeCoverFlow,
              library.games.indices.contains(selectedIndex) else { return }

        let step = direction == 0 ? 1 : (direction < 0 ? -1 : 1)
        let candidateIndices: [Int]
        if direction == 0 {
            candidateIndices = [selectedIndex, selectedIndex - 1, selectedIndex + 1]
        } else {
            candidateIndices = [
                selectedIndex,
                selectedIndex + step,
                selectedIndex + (step * 2),
            ]
        }
        let items = candidateIndices
            .filter { library.games.indices.contains($0) }
            .compactMap { index -> CoverThumbnailPreheatItem? in
                let game = library.games[index]
                guard let coverURL = game.coverURL else { return nil }
                return CoverThumbnailPreheatItem(
                    url: coverURL,
                    signature: game.coverSignature
                )
            }
        let metrics = CoverFlowMetrics(
            containerSize: libraryContainerSize,
            controllerOptimized: true,
            cardScale: resolvedGameCardScale,
            widthScale: resolvedGameCardWidthScale,
            heightScale: resolvedGameCardHeightScale,
            gapScale: resolvedGameCardGapScale,
            contentPaddingScale: resolvedGameCardContentPaddingScale,
            verticalPaddingScale: resolvedGameCardVerticalPaddingScale,
            textSpacingScale: resolvedGameCardTextSpacingScale,
            cardCornerRadius: resolvedGameCardCornerRadius
        )
        let scale = activeMenuWindow()?.screen.scale ?? UIScreen.main.scale
        coverFlowPreheater.schedule(
            items: items,
            width: metrics.coverWidth,
            height: metrics.coverHeight,
            scale: scale
        )
    }

    @discardableResult
    func moveControllerSelection(
        _ requestedDirection: MenuControllerCommand
    ) -> Bool {
        let direction = controllerDirectionForLayout(requestedDirection)
        if let action = controllerNowRunningFocusState.selectedAction,
           canFocusNowRunning {
            return moveFromNowRunning(action, direction: direction)
        }
        guard !library.games.isEmpty else {
            controllerSelectedGameID = nil
            controllerNavigationActive = false
            return false
        }

        guard let currentID = controllerSelectedGameID else {
            return selectInitialControllerGame(
                preferLast: direction == .up || direction == .left
            )
        }
        let cachedIndex = controllerFocusState.index(for: currentID)
        guard let currentIndex = cachedIndex.flatMap({ index in
            library.games.indices.contains(index) && library.games[index].id == currentID
                ? index
                : nil
        }) ?? library.games.firstIndex(where: { $0.id == currentID }) else {
            return selectInitialControllerGame(
                preferLast: direction == .up || direction == .left
            )
        }

        let usesCoverFlow = usesLandscapeCoverFlow
        let nextIndex: Int

        if appearance.libraryLayout != "grid" {
            switch direction.verticalComponent {
            case .up:
                if currentIndex == 0, canFocusNowRunning {
                    focusNowRunning(.stop)
                    return true
                }
                nextIndex = max(0, currentIndex - 1)
            case .down:
                nextIndex = min(library.games.count - 1, currentIndex + 1)
            default:
                return false
            }
        } else if usesCoverFlow {
            switch direction.horizontalComponent {
            case .left:
                if currentIndex == 0, canFocusNowRunning {
                    focusNowRunning(.stop)
                    return true
                }
                nextIndex = max(0, currentIndex - 1)
            case .right:
                nextIndex = min(library.games.count - 1, currentIndex + 1)
            default:
                return false
            }
        } else {
            let columnCount = GameLibraryGridLayout.gameGridColumnCount(
                for: libraryContainerSize.width,
                cardScale: resolvedGameCardScale,
                widthScale: resolvedGameCardWidthScale,
                gapScale: resolvedGameCardGapScale
            )
            let rowStart = (currentIndex / columnCount) * columnCount
            let rowEnd = min(library.games.count - 1, rowStart + columnCount - 1)
            switch direction {
            case .left:
                nextIndex = max(rowStart, currentIndex - 1)
            case .right:
                nextIndex = min(rowEnd, currentIndex + 1)
            case .up:
                let targetIndex = currentIndex - columnCount
                guard targetIndex >= 0 else {
                    if canFocusNowRunning {
                        focusNowRunning(.resume)
                        return true
                    }
                    return false
                }
                nextIndex = targetIndex
            case .down:
                let targetIndex = currentIndex + columnCount
                guard library.games.indices.contains(targetIndex) else { return false }
                nextIndex = targetIndex
            case .upLeft, .upRight, .downLeft, .downRight:
                let currentRow = currentIndex / columnCount
                let currentColumn = currentIndex % columnCount
                let rowDelta = direction.verticalComponent == .up ? -1 : 1
                let columnDelta = direction.horizontalComponent == .left ? -1 : 1
                let targetRow = currentRow + rowDelta
                guard targetRow >= 0 else {
                    if canFocusNowRunning {
                        focusNowRunning(
                            direction.horizontalComponent == .left
                                ? .resume
                                : .stop
                        )
                        return true
                    }
                    return false
                }
                let targetRowStart = targetRow * columnCount
                guard targetRowStart < library.games.count else { return false }
                let targetRowEnd = min(
                    library.games.count - 1,
                    targetRowStart + columnCount - 1
                )
                let targetColumn = currentColumn + columnDelta
                guard targetColumn >= 0,
                      targetColumn <= targetRowEnd - targetRowStart else {
                    return false
                }
                nextIndex = targetRowStart + targetColumn
            default:
                return false
            }
        }

        guard nextIndex != currentIndex else { return false }
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        if usesCoverFlow {
            preheatLandscapeCovers(
                around: nextIndex,
                direction: nextIndex - currentIndex
            )
        }
        let staysInGridRow: Bool
        if !usesCoverFlow, appearance.libraryLayout == "grid" {
            let columnCount = GameLibraryGridLayout.gameGridColumnCount(
                for: libraryContainerSize.width,
                cardScale: resolvedGameCardScale,
                widthScale: resolvedGameCardWidthScale,
                gapScale: resolvedGameCardGapScale
            )
            staysInGridRow = currentIndex / columnCount
                == nextIndex / columnCount
        } else {
            staysInGridRow = false
        }
        if reduceMotion && staysInGridRow {
            let gameID = library.games[nextIndex].id
            controllerFocusState.selectWithoutScrolling(gameID)
            controllerInput?.rememberNavigationFocusKey(
                "game:\(gameID)",
                forScope: gameLibraryControllerMemoryScope
            )
            controllerInput?.rememberNavigationFocusKey(
                "game:\(gameID)",
                forScope: gameLibraryControllerCardMemoryScope
            )
            updateGameCoverThemePreview(for: gameID)
        } else {
            controllerSelectedGameID = library.games[nextIndex].id
        }
        return true
    }

    private func moveFromNowRunning(
        _ action: GameLibraryNowRunningControllerAction,
        direction: MenuControllerCommand
    ) -> Bool {
        // List mode renders Resume and Stop as separate vertical rows.
        if appearance.libraryLayout != "grid" {
            if action == .resume, direction.verticalComponent == .down {
                focusNowRunning(.stop)
                return true
            }
            if action == .stop, direction.verticalComponent == .up {
                focusNowRunning(.resume)
                return true
            }
        }
        if usesLandscapeCoverFlow {
            switch (action, direction.horizontalComponent) {
            case (.resume, .right?):
                controllerNowRunningFocusState.selectedAction = .stop
                return true
            case (.stop, .left?):
                controllerNowRunningFocusState.selectedAction = .resume
                return true
            case (.stop, .right?):
                return focusFirstGame()
            default:
                return false
            }
        }

        switch direction {
        case .left where action == .stop:
            controllerNowRunningFocusState.selectedAction = .resume
            return true
        case .right where action == .resume:
            controllerNowRunningFocusState.selectedAction = .stop
            return true
        case .down, .downLeft, .downRight:
            return focusFirstGame()
        default:
            return false
        }
    }

    @discardableResult
    private func focusFirstGame() -> Bool {
        guard let first = library.games.first else { return false }
        controllerNowRunningFocusState.selectedAction = nil
        controllerNavigationActive = true
        controllerSelectedGameID = first.id
        return true
    }

    private func focusNowRunning(
        _ action: GameLibraryNowRunningControllerAction
    ) {
        guard session.runningGameName != nil, !nowRunningRemovalAnimationActive else {
            _ = focusFirstGame()
            return
        }
        controllerNavigationActive = true
        controllerSelectedGameID = nil
        controllerNowRunningFocusState.selectedAction = action
        controllerInput?.rememberNavigationFocusKey(
            "now-running:\(action.rawValue)",
            forScope: gameLibraryControllerMemoryScope
        )
        if let runningGame = library.games.first(where: library.isRunning) {
            updateGameCoverThemePreview(
                for: runningGame.id,
                includesNonFavoriteGame: true,
                whilePresentationIsActive: true
            )
        }
        favoriteScrollSequence &+= 1
        favoriteScrollRequest = GameLibraryFavoriteScrollRequest(
            sequence: favoriteScrollSequence,
            destination: .nowRunning
        )
    }

    func controllerDirectionForLayout(
        _ direction: MenuControllerCommand
    ) -> MenuControllerCommand {
        guard layoutDirection == .rightToLeft else { return direction }
        switch direction {
        case .left:
            return .right
        case .right:
            return .left
        case .upLeft:
            return .upRight
        case .upRight:
            return .upLeft
        case .downLeft:
            return .downRight
        case .downRight:
            return .downLeft
        default:
            return direction
        }
    }

    func controllerToolbarFocus(
        _ action: GameLibraryControllerToolbarAction,
        controllerInput: MenuControllerInputRouter?,
        menuTabIsActive: Bool,
        baseHorizontalPadding: CGFloat = 0,
        minimumWidth: CGFloat = 36
    ) -> ControllerToolbarFocusModifier {
        ControllerToolbarFocusModifier(
            focusState: controllerToolbarFocusState,
            action: action,
            controllerInput: controllerInput,
            menuTabIsActive: menuTabIsActive,
            isCovered: nonContextControllerNavigationPresentationActive || gameLibraryViewOptionsPresented,
            baseHorizontalPadding: baseHorizontalPadding,
            minimumWidth: minimumWidth
        )
    }

    func nowRunningControllerFocus(
        _ action: GameLibraryNowRunningControllerAction,
        controllerInput: MenuControllerInputRouter?,
        menuTabIsActive: Bool
    ) -> GameLibraryNowRunningControllerFocusModifier {
        GameLibraryNowRunningControllerFocusModifier(
            state: controllerNowRunningFocusState,
            action: action,
            controllerInput: controllerInput,
            menuTabIsActive: menuTabIsActive
        )
    }

    func controllerRightStickReachedTop() {
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library,
              let firstGame = library.games.first else { return }
        controllerFocusReleased = false
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = firstGame.id
        updateControllerScrollAvailability()
        controllerInput?.playFeedback(.boundary)
    }

    /// A horizontal Cover Flow has no toolbar beyond its leading edge. Keep
    /// analog scrolling anchored to the first game instead of interpreting
    /// the left boundary as the vertical list's "move to toolbar" boundary.
    func controllerRightStickReachedFirstCoverFlowGame() {
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library,
              let firstGame = library.games.first else { return }
        controllerFocusReleased = false
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = firstGame.id
        updateControllerScrollAvailability()
    }
}
