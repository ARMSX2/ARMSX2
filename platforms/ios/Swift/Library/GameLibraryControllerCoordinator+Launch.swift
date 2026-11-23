// GameLibraryControllerCoordinator+Launch.swift — launch, restart and stop, and the game actions the menus run
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UIKit

extension GameLibraryControllerCoordinator {
	func presentMenuPanel(_ action: @escaping () -> Void) {
		DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
			action()
		}
	}

	func open(
        _ game: ISOEntry,
        allowsAutomaticSkinPrompt: Bool = true
    ) {
		// A touch long-press shares the card's gesture arena. Once the unified
		// context surface owns this game, ignore the Button release from that same
		// press instead of launching underneath the menu.
		guard controllerContextMenuState.gameID != game.id else { return }
		MenuAudioPackManager.shared.suppressPendingGenericActivation()
		if library.isRunning(game) {
			session.returnToGame()
			return
		}

		guard ARMSX2Bridge.canResolveISO(game.bootName) else {
			gameActionTitle = settings.localized("Game Not Found")
			gameActionMessage = settings.localized("This game file is no longer available. Refresh the library or import it again.")
			library.loadGames()
			return
		}

        let transition = makeGameplayLaunchTransition(for: game)
        let automaticSkinOptions = allowsAutomaticSkinPrompt
            ? AutomaticCustomSkinManager.shared.availableProposals(
                forSerial: game.metadata[.serial]
            )
            : []
        if !automaticSkinOptions.isEmpty {
            MenuAudioPackManager.shared.playEvent(.uiToast)
            pendingAutomaticCustomSkinLaunch = PendingAutomaticCustomSkinLaunch(
                game: game,
                proposals: automaticSkinOptions,
                purpose: .gameLaunch,
                transition: transition
            )
            return
        }

        continueGameLaunch(game, transition: transition)
	}

    private func continueGameLaunch(
        _ game: ISOEntry,
        transition: GameplayLaunchTransition?,
        preferredPadSerial: String? = nil
    ) {
        let padSerial = preferredPadSerial ?? game.metadata[.serial]
        let padCRC = game.metadata[.crc]
        if session.runningGameName != nil {
            pendingGameName = game.bootName
            pendingGameplayLaunchTransition = transition
            pendingGameplayPadSerial = padSerial
            pendingGameplayPadCRC = padCRC
            showRestartAlert = true
        } else {
            session.prepareGameplayPadSelection(
                forSerial: padSerial,
                crc: padCRC
            )
            session.bootGame(
                isoName: game.bootName,
                launchTransition: transition
            )
        }
    }

    func completeAutomaticCustomSkinLaunch(apply: Bool) {
        guard let pending = pendingAutomaticCustomSkinLaunch else { return }
        pendingAutomaticCustomSkinLaunch = nil
        if apply {
            // A newer pick replaces a download that has not finished.
            catalogSkinInstallTask?.cancel()
            catalogSkinInstallTask = nil
        }
        if apply, pending.proposal.isCatalogListing {
            // Only the library picker lists skins that are not installed yet.
            let identity = PadLayoutGameIdentity(
                serial: pending.game.metadata[.serial],
                crc: pending.game.metadata[.crc]
            )
            let skinAtPick = identity.flatMap { PadLayoutPresetStore.shared.skinID(for: $0) }
            catalogSkinInstallTask = Task { @MainActor in
                let installed = await AutomaticCustomSkinManager.shared
                    .installIfNeeded(pending.proposal)
                guard !Task.isCancelled else { return }
                catalogSkinInstallTask = nil
                // A skin picked in game or reset on the Pad tab meanwhile wins.
                guard identity.flatMap({ PadLayoutPresetStore.shared.skinID(for: $0) })
                    == skinAtPick else { return }
                if let installed {
                    applyAutomaticCustomSkin(installed, from: pending)
                } else {
                    gameActionTitle = settings.localized("Per-Game Custom Skin")
                    gameActionMessage = settings.localized(
                        "The selected controller skin is unavailable."
                    )
                }
            }
        } else if apply {
            applyAutomaticCustomSkin(pending.proposal, from: pending)
        } else if pending.purpose == .gameLaunch {
            AutomaticCustomSkinManager.shared.decline(
                pending.proposal,
                doNotShowAgain: pending.doNotShowAgain
            )
        }
        guard pending.purpose == .gameLaunch else { return }
        continueGameLaunch(
            pending.game,
            transition: pending.transition,
            preferredPadSerial: pending.proposal.serial
        )
    }

    private func applyAutomaticCustomSkin(
        _ proposal: AutomaticCustomSkinProposal,
        from pending: PendingAutomaticCustomSkinLaunch
    ) {
        let applied = AutomaticCustomSkinManager.shared.apply(
            proposal,
            toISO: pending.game.bootName,
            metadata: pending.game.metadata,
            setCustomLayout: pending.setCustomLayout,
            doNotShowAgain: pending.purpose == .gameLaunch
                && pending.doNotShowAgain
        )
        if !applied {
            NSLog(
                "[ARMSX2 iOS Skins] automatic per-game assignment failed serial=%@",
                proposal.serial
            )
        } else if let descriptor = VPadSkinLibraryStore.shared.descriptor(
            id: proposal.skinID
        ) {
            // Populate both render and hit-mask caches before switching to
            // gameplay. No post-launch skin replacement or decode hitch is
            // then needed for the accepted package.
            ControllerAsset.prewarm(descriptor: descriptor)
            ARMSX2VirtualPadMaskImageCache.prewarm(
                descriptor: descriptor
            )
        }
    }

    func presentPerGameCustomSkin(for game: ISOEntry) {
        let serial = game.metadata[.serial]
        let proposals = AutomaticCustomSkinManager.shared
            .installedProposals(forSerial: serial)
        guard !proposals.isEmpty else {
            gameActionTitle = settings.localized("Per-Game Custom Skin")
            gameActionMessage = settings.localized(
                "This game does not have a usable serial for a per-game skin assignment."
            )
            return
        }

        let selectedSkinID = PadLayoutPresetStore.shared.automaticSkinID(
            forSerial: proposals[0].serial
        )
        let selectedIndex = proposals.firstIndex(where: {
            $0.skinID == selectedSkinID
        }) ?? 0
        pendingAutomaticCustomSkinLaunch = PendingAutomaticCustomSkinLaunch(
            game: game,
            proposals: proposals,
            selectedProposalIndex: selectedIndex,
            doNotShowAgain: false,
            purpose: .library,
            transition: nil
        )
        controllerAlertSelectedIndex = 0
        MenuAudioPackManager.shared.playEvent(.uiToast)

        // Add the catalog skins for this game to the already-visible picker.
        // The one the player applies downloads then, and nothing else does.
        Task { @MainActor in
            let refreshed = await AutomaticCustomSkinManager.shared
                .catalogProposals(forSerial: serial)
            guard !refreshed.isEmpty,
                  var pending = pendingAutomaticCustomSkinLaunch,
                  pending.purpose == .library,
                  pending.game.id == game.id else { return }
            let selectedSkinID = pending.proposal.skinID
            pending.proposals = refreshed
            pending.selectedProposalIndex = refreshed.firstIndex {
                $0.skinID == selectedSkinID
            } ?? 0
            pendingAutomaticCustomSkinLaunch = pending
        }
    }

    func performBootBIOSAction() {
        if session.runningGameName == "BIOS" {
            session.returnToGame()
        } else if session.runningGameName != nil {
            pendingGameName = ""
            pendingGameplayLaunchTransition = nil
            pendingGameplayPadSerial = nil
            pendingGameplayPadCRC = nil
            showRestartAlert = true
        } else {
            session.bootBIOSOnly()
        }
    }

    func openGameImporter() {
        MenuAudioPackManager.shared.playEvent(.contextMenu)
        showGameImporter = true
    }

    @MainActor
    private func makeGameplayLaunchTransition(
        for game: ISOEntry,
        respectingLaunchAnimationSetting: Bool = true
    ) -> GameplayLaunchTransition? {
        guard !respectingLaunchAnimationSetting
            || settings.appearance.gameCardZoomAnimationEnabled else {
            return nil
        }

        let cardView = gameplayLaunchCardRegistry.view(for: game.id)
        guard let window = cardView?.window ?? activeMenuWindow() else {
            return nil
        }

        let usesCoverFlow = appearance.libraryLayout == "grid"
            && appearance.landscapeCoverFlowEnabled
            && usesLandscapeGameCardScale
        let style: GameplayLaunchCardStyle
        let coverSize: CGSize
        let cornerRadius: CGFloat
        if usesCoverFlow {
            let metrics = CoverFlowMetrics(
                containerSize: window.bounds.size,
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
            style = .coverFlow
            coverSize = CGSize(
                width: metrics.coverWidth,
                height: metrics.coverHeight
            )
            cornerRadius = metrics.cornerRadius
        } else if appearance.libraryLayout == "grid" {
            style = .grid
            coverSize = CGSize(
                width: 126 * resolvedGameCardScale
                    * resolvedGameCardWidthScale,
                height: 189 * resolvedGameCardScale
                    * resolvedGameCardHeightScale
            )
            cornerRadius = resolvedGameCardCornerRadius
        } else {
            style = .list
            coverSize = CGSize(
                width: 58 * resolvedGameCardScale
                    * resolvedGameCardWidthScale,
                height: 87 * resolvedGameCardScale
                    * resolvedGameCardHeightScale
            )
            cornerRadius = resolvedGameCardCornerRadius
        }

        let sourceFrame: CGRect
        if let cardView,
           cardView.window === window {
            let candidate = cardView
                .convert(cardView.bounds, to: window)
                .intersection(window.bounds)
            sourceFrame = isValidGameplayLaunchFrame(candidate)
                ? candidate
                : fallbackGameplayLaunchFrame(
                    style: style,
                    coverSize: coverSize,
                    in: window.bounds
                )
        } else {
            sourceFrame = fallbackGameplayLaunchFrame(
                style: style,
                coverSize: coverSize,
                in: window.bounds
            )
        }

        let coverImage = game.coverURL.flatMap {
            CoverThumbnailCache.shared.imageForGameplayTransition(
                for: $0,
                signature: game.coverSignature,
                width: coverSize.width,
                height: coverSize.height,
                scale: window.screen.scale
            )
        }
        let detailParts: [String]
        if hideGameInfo {
            detailParts = []
        } else {
            detailParts = [
                game.name.pathExtensionLabel,
                game.sizeLabel,
                game.isExternal ? settings.localized("External") : nil,
                hideRegionFlag ? nil : game.regionFlag,
            ].compactMap { $0 }
        }

        return GameplayLaunchTransition(
            sourceFrame: sourceFrame,
            cornerRadius: cornerRadius,
            style: style,
            gameName: game.name,
            title: game.displayName,
            detail: detailParts.joined(separator: "  "),
            coverImage: coverImage,
            coverSize: coverSize,
            isFavorite: game.isFavorite,
            showsGameName: !hideGameName,
            showsGameInfo: !hideGameInfo,
            showsFavoriteIndicator: !hideFavoriteButton,
            usesSingleLineGameName: singleLineGameNames,
            usesClearGlass: true
        )
    }

    @MainActor
    func activeMenuWindow() -> UIWindow? {
        UIApplication.shared.appWindowScene?.windows.first(where: { $0.isKeyWindow })
    }

    private func isValidGameplayLaunchFrame(_ frame: CGRect) -> Bool {
        !frame.isNull
            && !frame.isInfinite
            && frame.origin.x.isFinite
            && frame.origin.y.isFinite
            && frame.width.isFinite
            && frame.height.isFinite
            && frame.width > 2
            && frame.height > 2
    }

    private func fallbackGameplayLaunchFrame(
        style: GameplayLaunchCardStyle,
        coverSize: CGSize,
        in windowBounds: CGRect
    ) -> CGRect {
        let size: CGSize
        switch style {
        case .list:
            size = CGSize(
                width: max(120, windowBounds.width - 32),
                height: 112
            )
        case .grid:
            size = CGSize(
                width: min(180, max(142, windowBounds.width * 0.42)),
                height: 268
            )
        case .coverFlow:
            size = CGSize(
                width: coverSize.width + 24,
                height: min(
                    windowBounds.height * 0.82,
                    coverSize.height + 88
                )
            )
        }

        return CGRect(
            x: windowBounds.midX - size.width / 2,
            y: windowBounds.midY - size.height / 2,
            width: size.width,
            height: size.height
        ).intersection(windowBounds)
    }

    func prepareGameImport(_ urls: [URL]) {
        let existingFileNames = fileImporter.existingFileNames(for: urls, preferredDestination: .game)
        guard !existingFileNames.isEmpty else {
            importGames(urls, allowReplacingExistingFiles: false)
            return
        }

        pendingGameImportURLs = urls
        existingGameImportFileNames = existingFileNames
        showGameReplacementAlert = true
    }

    func importGames(_ urls: [URL], allowReplacingExistingFiles: Bool) {
        let importedGames = library.importGames(
            urls,
            allowReplacingExistingFiles: allowReplacingExistingFiles
        )
        if !importedGames.isEmpty,
           controllerInput != nil,
           !usesLandscapeCoverFlow {
            libraryScrollRestoreState.requestTopRestore()
        }
    }

    func clearPendingGameImport() {
        pendingGameImportURLs = []
        existingGameImportFileNames = []
    }

    func importCoverPhoto(_ photoItem: PhotosPickerItem, forGameNamed gameName: String) {
        Task { @MainActor in
            if await coverStore.importCoverPhoto(photoItem, forGameNamed: gameName) {
                library.loadGames()
            }
        }
    }

	func releaseLibraryResourcesForGameplay() {
		coverFlowPreheater.cancel()
        nowRunningRemovalResetTask?.cancel()
        nowRunningRemovalResetTask = nil
        favoriteAnimationValues.removeAll(keepingCapacity: false)
        nowRunningRemovalAnimationActive = false
        departingRunningGameName = nil
        nowRunningCardOpacity = 1
        nowRunningCardScale = 1
        nowRunningStatusOpacity = 1
        gameplayLaunchCardRegistry.removeAll()
        library.releaseLibraryResourcesForGameplay()
		}

    func scheduleStopRunningGame() {
        showStopAlert = false
        guard !nowRunningRemovalAnimationActive,
              let runningGameName = session.runningGameName else {
            return
        }
        MenuAudioPackManager.shared.playEvent(.stoppingGame)

        // The two actions disappear during departure; retire their focus at
        // the same time and keep it on the stopped game's library card.
        if controllerNowRunningFocusState.selectedAction != nil {
            controllerNowRunningFocusState.selectedAction = nil
            controllerNavigationActive = true
            controllerSelectedGameID = library.games.first(where: {
                $0.bootName == runningGameName
            })?.id ?? library.games.first?.id
        }

        nowRunningRemovalResetTask?.cancel()
        nowRunningCardOpacity = 1
        nowRunningCardScale = 1
        nowRunningStatusOpacity = 1
        departingRunningGameName = runningGameName
        withAnimation(.smooth(duration: 0.42, extraBounce: 0.02)) {
            // Enter a visible stopping state immediately after confirmation:
            // play becomes stop, actions dissolve, and both labels morph into
            // one prominent status before the whole glass card leaves.
            nowRunningRemovalAnimationActive = true
        }

        nowRunningRemovalResetTask = Task { @MainActor in
            // Let the status morph complete, fade its foreground away, then
            // collapse the complete composited glass surface to zero pixels.
            try? await Task.sleep(for: .milliseconds(420))
            guard !Task.isCancelled else { return }

            withAnimation(.easeInOut(duration: 0.3)) {
                nowRunningStatusOpacity = 0
            }
            try? await Task.sleep(for: .milliseconds(320))
            guard !Task.isCancelled else { return }

            withAnimation(.easeInOut(duration: 0.65)) {
                nowRunningCardOpacity = 0
                nowRunningCardScale = 0
            }
            try? await Task.sleep(for: .milliseconds(700))
            guard !Task.isCancelled else { return }

            // Stop the VM once the card's departure completes. The invisible
            // placeholder stays while a drag or deceleration is active, so
            // removing it cannot move the content under the user's finger.
            session.stopGame()

            while library.gameLibraryActivity.isScrolling {
                try? await Task.sleep(for: .milliseconds(50))
                guard !Task.isCancelled else { return }
            }

            withAnimation(.smooth(duration: 0.52, extraBounce: 0.02)) {
                departingRunningGameName = nil
            }
            try? await Task.sleep(for: .milliseconds(540))
            guard !Task.isCancelled else { return }

            // The collapsed values remain private to the departed placeholder.
            // Clearing only the phase flag avoids a second transaction through
            // every persistent game-card glass surface.
            nowRunningRemovalAnimationActive = false
            nowRunningRemovalResetTask = nil
        }
    }

	@discardableResult
	func toggleFavorite(
        _ game: ISOEntry,
        playsSelectionFeedback: Bool = true
    ) -> Bool {
        var updatedValue = false
        var reorderTransaction = Transaction()
        reorderTransaction.animation = nil
        withTransaction(reorderTransaction) {
            updatedValue = library.toggleFavorite(game)
        }
        if settings.appearance.favoriteGlowingEffectEnabled {
            favoriteAnimationValues[game.id, default: 0] += 1
        }
        if controllerInput != nil {
            favoriteScrollSequence &+= 1
            let firstGameID = library.games.first?.id ?? game.id
            favoriteScrollRequest = GameLibraryFavoriteScrollRequest(
                sequence: favoriteScrollSequence,
                destination: updatedValue
                    ? .firstGame(firstGameID)
                    : .game(game.id)
            )
        }

		if playsSelectionFeedback {
			UISelectionFeedbackGenerator().selectionChanged()
		}
		return updatedValue
	}

	func copyLaunchLink(_ game: ISOEntry) {
		let link = ARMSX2DeepLinkHandler.launchURL(forISO: game.name)
		UIPasteboard.general.string = link
		presentMenuPanel { [self] in
			gameActionTitle = "Launch Link Copied"
			gameActionMessage = link
		}
	}

	func clearGameCache(_ game: ISOEntry) {
		(gameActionTitle, gameActionMessage) = library.clearGameCache(game)
	}

	func deleteGameData(_ game: ISOEntry) {
		(gameActionTitle, gameActionMessage) = library.deleteGameData(game)
	}

	func deleteGame(_ game: ISOEntry, deleteData: Bool) {
		(gameActionTitle, gameActionMessage) = library.deleteGame(game, deleteData: deleteData)
	}
}
