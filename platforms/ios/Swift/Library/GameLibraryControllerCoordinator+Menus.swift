// GameLibraryControllerCoordinator+Menus.swift — toolbar commands, View Options and the Layout and More menus
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension GameLibraryControllerCoordinator {
    func handleControllerToolbarCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = GameLibraryControllerToolbarAction.allCases
        guard let index = actions.firstIndex(
            of: controllerToolbarFocusState.selectedAction
        ) else {
            controllerToolbarFocusState.selectedAction = .importGames
            return
        }

        switch command {
        case .left:
            let next = max(actions.startIndex, index - 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerToolbarFocusState.selectedAction = actions[next]
            controllerInput?.playFeedback(.move(.left))
        case .right:
            let next = min(actions.index(before: actions.endIndex), index + 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerToolbarFocusState.selectedAction = actions[next]
            controllerInput?.playFeedback(.move(.right))
        case .down, .downLeft, .downRight:
            controllerInput?.setNavigationZone(.library)
            if !library.games.isEmpty || displayedRunningGameName != nil {
                // In landscape the visual entry point is the centered cover,
                // not the first model item (which is normally offscreen).
                _ = selectInitialControllerGame(preferLast: usesLandscapeCoverFlow)
            } else {
                controllerNavigationActive = true
                controllerSelectedGameID = nil
            }
            controllerInput?.playFeedback(.move(.down))
        case .up, .upLeft, .upRight:
            // Holding Up while entering the toolbar must terminate here. If it
            // re-enters the last card, the controller repeat loops through the
            // entire library instead of leaving the top controls selected.
            controllerInput?.playFeedback(.boundary)
        case .activate:
            let action = controllerToolbarFocusState.selectedAction
            performControllerToolbarAction(action)
            controllerInput?.playFeedback(
                action == .layout
                    ? .contextMenu
                    : .activate
            )
        case .back:
            controllerInput?.setNavigationZone(.library)
            controllerInput?.playFeedback(.back)
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            break
        }
    }

    func handleEmptyLibraryControllerCommand(
        _ command: MenuControllerCommand
    ) {
        switch command {
        case .activate:
            openGameImporter()
            controllerInput?.playFeedback(.destination)
        case .up, .upLeft, .upRight, .back:
            controllerInput?.setNavigationZone(.topToolbar)
            controllerInput?.playFeedback(command == .back ? .back : .move(.up))
        case .down, .downLeft, .downRight:
            controllerInput?.setNavigationZone(.tabBar)
            controllerInput?.playFeedback(.move(.down))
        case .left, .right, .toggleFavorite, .showContextMenu:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func performControllerToolbarAction(
        _ action: GameLibraryControllerToolbarAction
    ) {
        switch action {
        case .bootBIOS:
            performBootBIOSAction()
        case .importGames:
            openGameImporter()
        case .layout:
            presentControllerLayoutMenu()
        case .more:
            presentControllerMoreMenu()
        }
    }

    func presentGameLibraryViewOptions() {
        gameLibraryViewOptionsPreview = nil
        gameLibraryViewOptionsBaseline = appearance.snapshot(
            isLandscape: usesLandscapeGameCardScale
        )
        gameLibraryViewOptionsPresented = true
        MenuAudioPackManager.shared.playEvent(.contextMenu)
    }

    func cancelGameLibraryViewOptions() {
        closeGameLibraryViewOptions(restoringBaseline: true)
    }

    func applyGameLibraryViewOptions() {
        closeGameLibraryViewOptions(restoringBaseline: false)
    }

    private func closeGameLibraryViewOptions(restoringBaseline: Bool) {
        guard gameLibraryViewOptionsPresented else { return }
        if restoringBaseline, let gameLibraryViewOptionsBaseline {
            appearance.restore(
                gameLibraryViewOptionsBaseline,
                isLandscape: usesLandscapeGameCardScale
            )
        }
        gameLibraryViewOptionsPreview = nil
        gameLibraryViewOptionsBaseline = nil
        gameLibraryViewOptionsPresented = false
        MenuAudioPackManager.shared.playEvent(.return)
    }

    var controllerLayoutMenuActions: [ControllerNavigationAlertAction] {
        var actions = [
            ControllerNavigationAlertAction(
                id: "display-mode",
                title: settings.localized(
                    appearance.libraryLayout == "grid" ? "Show List" : "Show Grid"
                )
            ),
        ]
        if appearance.libraryLayout == "grid", usesLandscapeGameCardScale {
            actions.append(
                ControllerNavigationAlertAction(
                    id: "landscape-cover-flow",
                    title: settings.localized("Landscape Cover Flow")
                        + (appearance.landscapeCoverFlowEnabled ? "  ✓" : "")
                )
            )
        }
        actions.append(
            ControllerNavigationAlertAction(
                id: "view-options",
                title: settings.localized(
                    "Game Library View Options"
                )
            )
        )
        return actions
    }

    private func presentControllerLayoutMenu() {
        controllerLayoutMenuSelectedIndex = 0
        controllerLayoutMenuPresented = true
    }

    var controllerMoreMenuActions: [ControllerNavigationAlertAction] {
        var actions = [
            ControllerNavigationAlertAction(
                id: "import-local-covers",
                title: settings.localized("Import Local Covers"),
                systemImage: "photo.badge.plus"
            ),
        ]
        if !coverStore.isDownloadingCovers && !library.games.isEmpty {
            actions.append(
                ControllerNavigationAlertAction(
                    id: "download-missing-covers",
                    title: settings.localized("Download Missing Covers"),
                    systemImage: "icloud.and.arrow.down"
                )
            )
        }
        actions.append(
            contentsOf: [
                ControllerNavigationAlertAction(
                    id: "cover-source",
                    title: settings.localized("Cover Source"),
                    systemImage: "link"
                ),
                ControllerNavigationAlertAction(
                    id: "reset-cover-template",
                    title: settings.localized("Reset Cover Template"),
                    systemImage: "arrow.counterclockwise"
                ),
                ControllerNavigationAlertAction(
                    id: "refresh-library",
                    title: settings.localized("Refresh"),
                    systemImage: "arrow.clockwise"
                ),
            ]
        )
        return actions
    }

    private func presentControllerMoreMenu() {
        controllerMoreMenuSelectedIndex = 0
        controllerMoreMenuPresented = true
    }

    func dismissControllerLayoutMenu(playsFeedback: Bool = true) {
        guard controllerLayoutMenuPresented else { return }
        controllerLayoutMenuPresented = false
        controllerLayoutMenuSelectedIndex = 0
        if playsFeedback {
            controllerInput?.playFeedback(.back)
        }
    }

    func dismissControllerMoreMenu(playsFeedback: Bool = true) {
        guard controllerMoreMenuPresented else { return }
        controllerMoreMenuPresented = false
        controllerMoreMenuSelectedIndex = 0
        if playsFeedback {
            controllerInput?.playFeedback(.back)
        }
    }

    func handleControllerMoreMenuCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = controllerMoreMenuActions
        switch command {
        case .up, .upLeft, .upRight:
            let next = max(0, controllerMoreMenuSelectedIndex - 1)
            guard next != controllerMoreMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerMoreMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.up))
        case .down, .downLeft, .downRight:
            let next = min(
                actions.count - 1,
                controllerMoreMenuSelectedIndex + 1
            )
            guard next != controllerMoreMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerMoreMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.down))
        case .activate:
            performControllerMoreMenuAction(controllerMoreMenuSelectedIndex)
        case .back, .showContextMenu:
            dismissControllerMoreMenu()
        case .left, .right, .toggleFavorite:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    func performControllerMoreMenuAction(_ index: Int) {
        let actions = controllerMoreMenuActions
        guard actions.indices.contains(index) else { return }
        dismissControllerMoreMenu(playsFeedback: false)
        switch actions[index].id {
        case "import-local-covers":
            presentMenuPanel { [self] in
                pendingCoverGameName = nil
                showCoverImporter = true
            }
            controllerInput?.playFeedback(.contextMenu)
        case "download-missing-covers":
            library.downloadMissingCovers()
            controllerInput?.playFeedback(.activate)
        case "cover-source":
            presentMenuPanel { [self] in
                coverTemplateDraft = coverStore.coverURLTemplate
                showCoverTemplateEditor = true
            }
            controllerInput?.playFeedback(.contextMenu)
        case "reset-cover-template":
            presentMenuPanel { [self] in
                coverStore.resetCoverURLTemplate()
            }
            controllerInput?.playFeedback(.activate)
        case "refresh-library":
            library.loadGames(forceMetadataRefresh: true)
            controllerInput?.playFeedback(.activate)
        default:
            break
        }
    }

    func handleControllerLayoutMenuCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = controllerLayoutMenuActions
        switch command {
        case .up, .upLeft, .upRight:
            let next = max(0, controllerLayoutMenuSelectedIndex - 1)
            guard next != controllerLayoutMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerLayoutMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.up))
        case .down, .downLeft, .downRight:
            let next = min(
                actions.count - 1,
                controllerLayoutMenuSelectedIndex + 1
            )
            guard next != controllerLayoutMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerLayoutMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.down))
        case .activate:
            performControllerLayoutMenuAction(
                controllerLayoutMenuSelectedIndex
            )
        case .back, .showContextMenu:
            dismissControllerLayoutMenu()
        case .left, .right, .toggleFavorite:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    func performControllerLayoutMenuAction(_ index: Int) {
        let actions = controllerLayoutMenuActions
        guard actions.indices.contains(index) else {
            return
        }
        switch actions[index].id {
        case "display-mode":
            dismissControllerLayoutMenu(playsFeedback: false)
            selectAlternateLibraryLayout()
            controllerInput?.playFeedback(.activate)
        case "landscape-cover-flow":
            appearance.landscapeCoverFlowEnabled.toggle()
            controllerInput?.playFeedback(
                .toggle(isOn: appearance.landscapeCoverFlowEnabled)
            )
        case "view-options":
            dismissControllerLayoutMenu(playsFeedback: false)
            presentGameLibraryViewOptions()
        default:
            break
        }
    }

    func selectAlternateLibraryLayout() {
        appearance.libraryLayout = appearance.libraryLayout == "grid" ? "list" : "grid"
    }
}
