// GameLibraryControllerCoordinator+ContextMenu.swift — the game context menu
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension GameLibraryControllerCoordinator {
    var controllerContextMenuGame: ISOEntry? {
        guard let gameID = controllerContextMenuState.gameID else { return nil }
        return library.games.first(where: { $0.id == gameID })
    }

    func controllerContextMenuTitle(
        for section: GameLibraryControllerMenuSection
    ) -> String {
        switch section {
        case .root:
            return ""
        case .covers:
            return settings.localized("Covers")
        case .gameData:
            return settings.localized("Game Data")
        }
    }

    func controllerContextMenuItems(
        for game: ISOEntry,
        section: GameLibraryControllerMenuSection? = nil
    ) -> [GameLibraryControllerMenuItem] {
        switch section ?? controllerContextMenuState.section {
        case .root:
            var items = [
                GameLibraryControllerMenuItem(
                    action: .gameInfo,
                    title: settings.localized("Game Info"),
                    systemImage: "info.circle"
                ),
                GameLibraryControllerMenuItem(
                    action: .perGameSettings,
                    title: settings.localized("Per-Game Settings"),
                    systemImage: "slider.horizontal.3"
                ),
                GameLibraryControllerMenuItem(
                    action: .perGameCustomSkin,
                    title: settings.localized("Per-Game Custom Skin"),
                    systemImage: "gamecontroller.fill"
                ),
                GameLibraryControllerMenuItem(
                    action: .editTouchLayout,
                    title: settings.localized("Edit Virtual Pad Layout"),
                    systemImage: "square.resize"
                ),
            ]
            if game.isELF {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .discPath,
                        title: settings.localized("Disc Path"),
                        systemImage: "opticaldisc"
                    )
                )
            }
            items.append(
                contentsOf: [
                    GameLibraryControllerMenuItem(
                        action: .cheatsAndPatches,
                        title: settings.localized("Cheats & Patches"),
                        systemImage: "rectangle.stack.badge.plus"
                    ),
                    GameLibraryControllerMenuItem(
                        action: .covers,
                        title: settings.localized("Covers"),
                        systemImage: "photo.stack",
                        showsDisclosure: true
                    ),
                    GameLibraryControllerMenuItem(
                        action: .gameData,
                        title: settings.localized("Game Data"),
                        systemImage: "externaldrive",
                        showsDisclosure: true
                    ),
                    GameLibraryControllerMenuItem(
                        action: .copyLaunchLink,
                        title: settings.localized("Copy Launch Link"),
                        systemImage: "doc.on.doc"
                    ),
                ]
            )
            items.append(
                GameLibraryControllerMenuItem(
                    action: .rename,
                    title: settings.localized("Rename"),
                    systemImage: "pencil"
                )
            )
            return items

        case .covers:
            var items: [GameLibraryControllerMenuItem] = []
            if !coverStore.isDownloadingCovers {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .downloadCover,
                        title: settings.localized("Download Cover"),
                        systemImage: "icloud.and.arrow.down"
                    )
                )
            }
            items.append(
                contentsOf: [
                    GameLibraryControllerMenuItem(
                        action: .chooseCoverPhoto,
                        title: settings.localized("Choose from Photos"),
                        systemImage: "photo.on.rectangle"
                    ),
                    GameLibraryControllerMenuItem(
                        action: .chooseCoverFile,
                        title: settings.localized("Choose from Files"),
                        systemImage: "folder"
                    ),
                ]
            )
            if game.coverURL != nil {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .removeCover,
                        title: settings.localized("Remove Cover"),
                        systemImage: "trash",
                        isDestructive: true
                    )
                )
            }
            return items

        case .gameData:
            var items = [
                GameLibraryControllerMenuItem(
                    action: .clearGameCache,
                    title: settings.localized("Clear Game Cache"),
                    systemImage: "trash.slash"
                ),
                GameLibraryControllerMenuItem(
                    action: .deleteGameData,
                    title: settings.localized("Delete Game Data"),
                    systemImage: "externaldrive.badge.xmark",
                    isDestructive: true
                ),
            ]
            if !game.isExternal {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .deleteGame,
                        title: settings.localized("Delete Game"),
                        systemImage: "trash",
                        isDestructive: true
                    )
                )
            }
            return items
        }
    }

    func handleGameContextMenuLongPress(for game: ISOEntry) {
        // Claim the stationary hold before RootView's empty-space recognizer
        // can interpret it as an L2/R2 theme shortcut.
        sharedMenuControllerInput?.suppressTouchThemePresetShortcut()
        openControllerContextMenu(for: game)
    }

    func suppressThemeShortcutForGameCardTouch() {
        sharedMenuControllerInput?.suppressTouchThemePresetShortcut()
    }

    func openControllerContextMenu(for game: ISOEntry) {
        let openedByController = controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = openedByController
            ? .gameInfo : nil
        // The foreground row must be the only published orb destination.
        // Otherwise the retained focused card and the context overlay compete
        // across separate preference branches and the orbit can stay behind.
        controllerNavigationActive = false
        controllerInput?.setNavigationCaptured(
            true,
            owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
            priority: 300
        )
        updateControllerScrollAvailability()
        withAnimation(.smooth(duration: 0.24, extraBounce: 0.03)) {
            controllerContextMenuState.gameID = game.id
        }
        if let controllerInput {
            controllerInput.playFeedback(.contextMenu)
        } else {
            MenuAudioPackManager.shared.playEvent(.contextMenu)
        }
    }

    @MainActor
    private func prepareGameSettings(for game: ISOEntry) {
        guard preparedGameSettingsID != game.id
                || preparedGameSettings == nil else { return }

        if let crc = game.metadata[.crc], !crc.isEmpty {
            preparedGameSettings = ARMSX2Bridge.gameSettings(
                forSerial: game.metadata[.serial],
                crc: crc
            )
        } else {
            // Some manually added ELF entries do not have a cached CRC. Keep
            // the compatibility fallback for those uncommon entries only.
            preparedGameSettings = ARMSX2Bridge.gameSettings(
                forISO: game.bootName
            )
        }
        preparedGameSettingsID = game.id
    }

    func releasePreparedGameSettings() {
        preparedGameSettingsID = nil
        preparedGameSettings = nil
    }

    private func beginRenaming(_ game: ISOEntry) {
        controllerNavigationActive = false
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = nil
        renameTarget = game

        // Replace the context surface instead of stacking a keyboard above it.
        // Keeping the capture continuous also prevents the selection press
        // from falling through and opening the focused game card.
        withAnimation(.easeOut(duration: 0.16)) {
            controllerContextMenuState.gameID = nil
        }
        controllerInput?.setNavigationCaptured(
            true,
            owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
            priority: 300
        )
        updateControllerScrollAvailability()
    }

    func closeControllerContextMenu(
        playsFeedback: Bool = true
    ) {
        guard controllerContextMenuState.gameID != nil else { return }
        withAnimation(.easeOut(duration: 0.18)) {
            controllerContextMenuState.gameID = nil
        }
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = nil
        Task { @MainActor in
            // Let a destination selected from the menu publish first. This
            // avoids briefly reactivating and scrolling the library between
            // the context menu and its sheet.
            await Task.yield()
            guard controllerContextMenuState.gameID == nil else { return }
            let shouldCapture = manuallyCapturedControllerPresentationActive
            controllerInput?.setNavigationCaptured(
                shouldCapture,
                owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
                priority: 300
            )
            updateControllerScrollAvailability()
            if !shouldCapture {
                controllerNavigationActive = controllerSelectedGameID != nil
                if controllerNavigationActive {
                    controllerFocusState.requestSelectionScroll()
                } else {
                    scheduleLandscapeControllerAutofocus()
                }
            }
        }
        if playsFeedback {
            if let controllerInput {
                controllerInput.playFeedback(.back)
            } else {
                MenuAudioPackManager.shared.playEvent(.return)
            }
        }
    }

    func handleControllerContextMenuCommand(
        _ command: MenuControllerCommand,
        for game: ISOEntry
    ) {
        let direction = controllerDirectionForLayout(command)
        switch direction {
        case .up, .upLeft, .upRight, .down, .downLeft, .downRight:
            moveControllerContextMenuSelection(
                direction.verticalComponent ?? direction,
                for: game
            )
        case .left, .back:
            navigateBackFromControllerContextMenu()
        case .right, .activate:
            guard let action = controllerContextMenuState.selectedAction else {
                selectFirstControllerContextMenuItem(for: game)
                return
            }
            performControllerContextMenuAction(action, for: game)
        case .toggleFavorite:
            let isFavorite = toggleFavorite(
                game,
                playsSelectionFeedback: false
            )
            controllerInput?.playFeedback(
                .favorite(isFavorite: isFavorite)
            )
        case .showContextMenu:
            closeControllerContextMenu()
        case .previousTab, .nextTab:
            break
        }
    }

    private func moveControllerContextMenuSelection(
        _ direction: MenuControllerCommand,
        for game: ISOEntry
    ) {
        let actions = controllerContextMenuNavigationActions(for: game)
        guard !actions.isEmpty else {
            controllerInput?.playFeedback(.boundary)
            return
        }

        let currentIndex = controllerContextMenuState.selectedAction.flatMap { action in
            actions.firstIndex(of: action)
        }
        let proposedIndex: Int
        if let currentIndex {
            proposedIndex = direction == .up
                ? max(0, currentIndex - 1)
                : min(actions.count - 1, currentIndex + 1)
            guard proposedIndex != currentIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
        } else {
            proposedIndex = direction == .up ? actions.count - 1 : 0
        }

        controllerContextMenuState.selectedAction = actions[proposedIndex]
        controllerInput?.playFeedback(.move(direction))
    }

    private func controllerContextMenuNavigationActions(
        for game: ISOEntry
    ) -> [GameLibraryControllerMenuAction] {
        let rowActions = controllerContextMenuItems(for: game).map(\.action)
        guard controllerContextMenuState.section == .root else {
            return rowActions
        }
        return [.playOrStop] + rowActions
    }

    private func navigateBackFromControllerContextMenu() {
        guard controllerContextMenuState.section != .root else {
            closeControllerContextMenu()
            return
        }

        let parentAction: GameLibraryControllerMenuAction =
            controllerContextMenuState.section == .covers ? .covers : .gameData
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = parentAction
        controllerInput?.playFeedback(.back)
    }

    private func selectFirstControllerContextMenuItem(
        for game: ISOEntry,
        playsFeedback: Bool = true
    ) {
        controllerContextMenuState.selectedAction = controllerContextMenuItems(
            for: game
        ).first?.action
        if playsFeedback {
            controllerInput?.playFeedback(.move(.down))
        }
    }

    func performControllerContextMenuAction(
        _ action: GameLibraryControllerMenuAction,
        for game: ISOEntry
    ) {
        switch action {
        case .playOrStop:
            deferControllerContextMenuSelection(action, for: game)

        case .covers:
            controllerContextMenuState.section = .covers
            selectInitialControllerContextMenuItemIfNeeded(for: game)
            controllerInput?.playFeedback(.submenu)

        case .gameData:
            controllerContextMenuState.section = .gameData
            selectInitialControllerContextMenuItemIfNeeded(for: game)
            controllerInput?.playFeedback(.submenu)

        case .gameInfo:
            deferControllerContextMenuSelection(action, for: game)

        case .perGameSettings:
            deferControllerContextMenuSelection(action, for: game)

        case .perGameShaders:
            deferControllerContextMenuSelection(action, for: game)

        case .perGameCustomSkin:
            deferControllerContextMenuSelection(action, for: game)

        case .editTouchLayout:
            deferControllerContextMenuSelection(action, for: game)

        case .discPath:
            deferControllerContextMenuSelection(action, for: game)

        case .cheatsAndPatches:
            deferControllerContextMenuSelection(action, for: game)

        case .copyLaunchLink:
            deferControllerContextMenuSelection(action, for: game)

        case .downloadCover:
            deferControllerContextMenuSelection(action, for: game)

        case .chooseCoverPhoto:
            deferControllerContextMenuSelection(action, for: game)

        case .chooseCoverFile:
            deferControllerContextMenuSelection(action, for: game)

        case .removeCover:
            deferControllerContextMenuSelection(action, for: game)

        case .clearGameCache:
            deferControllerContextMenuSelection(action, for: game)

        case .deleteGameData:
            deferControllerContextMenuSelection(action, for: game)

        case .deleteGame:
            deferControllerContextMenuSelection(action, for: game)

        case .rename:
            deferControllerContextMenuSelection(action, for: game)
        }
    }

    private func selectInitialControllerContextMenuItemIfNeeded(
        for game: ISOEntry
    ) {
        guard controllerInput?.hasConnectedController == true,
              controllerInput?.isControllerNavigationEnabled == true else {
            controllerContextMenuState.selectedAction = nil
            return
        }
        selectFirstControllerContextMenuItem(for: game, playsFeedback: false)
    }

    /// The scene-level context menu has to finish dismissing before SwiftUI
    /// can present the chosen destination. Resolve it from `onDismiss` instead
    /// of relying on a device-dependent delay.
    func deferControllerContextMenuSelection(
        _ action: GameLibraryControllerMenuAction,
        for game: ISOEntry
    ) {
        deferredControllerContextMenuSelection = .init(
            action: action,
            gameID: game.id
        )
        closeControllerContextMenu(playsFeedback: false)
    }

    func completeDeferredControllerContextMenuSelection() {
        guard let selection = deferredControllerContextMenuSelection,
              let game = library.games.first(where: { $0.id == selection.gameID })
        else {
            deferredControllerContextMenuSelection = nil
            return
        }

        switch selection.action {
        case .playOrStop:
            if library.isRunning(game) {
                showStopAlert = true
            } else {
                open(
                    game,
                    allowsAutomaticSkinPrompt:
                        controllerInput?.isControllerNavigationEnabled != true
                )
            }
        case .gameInfo:
            gameInfoTarget = game
            controllerInput?.playFeedback(.destination)
        case .perGameSettings:
            prepareGameSettings(for: game)
            gameSettingsStartsInShaders = false
            gameSettingsTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .perGameShaders:
            prepareGameSettings(for: game)
            gameSettingsStartsInShaders = true
            gameSettingsTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .perGameCustomSkin:
            presentPerGameCustomSkin(for: game)
            controllerInput?.playFeedback(.destination)
        case .editTouchLayout:
            padLayoutEditorTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .discPath:
            discLinkTarget = game
            controllerInput?.playFeedback(.destination)
        case .cheatsAndPatches:
            cheatsManagerTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .copyLaunchLink:
            copyLaunchLink(game)
            controllerInput?.playFeedback(.activate)
        case .downloadCover:
            library.downloadCover(for: game)
            controllerInput?.playFeedback(.activate)
        case .chooseCoverPhoto:
            pendingCoverPhotoGameName = game.name
            showCoverPhotoPicker = true
            controllerInput?.playFeedback(.destination)
        case .chooseCoverFile:
            pendingCoverGameName = game.name
            showCoverImporter = true
            controllerInput?.playFeedback(.destination)
        case .removeCover:
            library.removeCover(for: game)
            controllerInput?.playFeedback(.activate)
        case .clearGameCache:
            clearGameCache(game)
            controllerInput?.playFeedback(.destination)
        case .deleteGameData:
            pendingDeleteDataGame = game
            controllerInput?.playFeedback(.destination)
        case .deleteGame:
            pendingDeleteGame = game
            controllerInput?.playFeedback(.destination)
        case .rename:
            beginRenaming(game)
            controllerInput?.playFeedback(.destination)
        case .covers, .gameData:
            break
        }

        deferredControllerContextMenuSelection = nil
    }
}
