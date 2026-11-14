// GameLibraryControllerCoordinator+Alerts.swift — the library's controller alerts
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension GameLibraryControllerCoordinator {
    func controllerAlertTitle(
        for kind: GameLibraryControllerAlertKind
    ) -> String {
        switch kind {
        case .automaticCustomSkin:
            return settings.localized(
                pendingAutomaticCustomSkinLaunch?.purpose == .library
                    ? "Per-Game Custom Skin"
                    : "Set New Per-Game Custom Skin?"
            )
        case .restart:
            return settings.localized("Restart VM?")
        case .stop:
            return settings.localized("Stop Emulation?")
        case .deleteGameData:
            return settings.localized("Delete Game Data?")
        case .deleteGame:
            return settings.localized("Delete Game?")
        case .gameAction:
            return settings.localized(gameActionTitle.isEmpty ? "Game Action" : gameActionTitle)
        case .coverResult:
            return settings.localized("Cover Result")
        case .replaceFiles:
            return settings.localized("Replace existing files?")
        case .backgroundError:
            return settings.localized("Background image could not be loaded.")
        }
    }

    func controllerAlertMessage(
        for kind: GameLibraryControllerAlertKind
    ) -> String {
        switch kind {
        case .automaticCustomSkin:
            return settings.localized(
                pendingAutomaticCustomSkinLaunch?.purpose == .library
                    ? "Choose the skin and optional linked controller layout for this game."
                    : "Apply the recommended skin and controller layout before launching?"
            )
        case .restart:
            let target = pendingGameName.isEmpty
                ? "Boot BIOS"
                : (pendingGameName as NSString).lastPathComponent
            return "\(settings.localized("VM is currently running."))\n\(settings.localized("Shut down and start")) \(settings.localized(target))?"
        case .stop:
            return settings.localized("This will shut down the running game. All unsaved progress will be lost.")
        case .deleteGameData:
            return settings.localized("This clears save states, PNACH files, per-game settings, compatibility overrides, and generated cache for this game. Memory card contents are not deleted.")
        case .deleteGame:
            return settings.localized("Delete the selected game file? You can also remove its generated game data at the same time.")
        case .gameAction:
            return gameActionMessage ?? ""
        case .coverResult:
            return coverStore.lastCoverMessage ?? ""
        case .replaceFiles:
            return FileImportHandler.replacementConfirmationMessage(
                for: existingGameImportFileNames
            )
        case .backgroundError:
            return ""
        }
    }

    func controllerAlertPreviewImageURL(
        for kind: GameLibraryControllerAlertKind
    ) -> URL? {
        guard kind == .automaticCustomSkin,
              let proposal = pendingAutomaticCustomSkinLaunch?.proposal else {
            return nil
        }
        return AutomaticCustomSkinManager.shared.previewURL(for: proposal)
    }

    func controllerAlertPreviewSkinDescriptor(
        for kind: GameLibraryControllerAlertKind
    ) -> VPadSkinDescriptor? {
        guard kind == .automaticCustomSkin,
              let proposal = pendingAutomaticCustomSkinLaunch?.proposal else {
            return nil
        }
        return VPadSkinLibraryStore.shared.descriptor(id: proposal.skinID)
    }

    func controllerAlertPreviewAccessibilityLabel(
        for kind: GameLibraryControllerAlertKind
    ) -> String? {
        guard kind == .automaticCustomSkin,
              let proposal = pendingAutomaticCustomSkinLaunch?.proposal else {
            return nil
        }
        return "\(settings.localized("Preview")): \(proposal.skinName)"
    }

    func controllerAlertDetails(
        for kind: GameLibraryControllerAlertKind
    ) -> [ControllerNavigationAlertDetail] {
        guard kind == .automaticCustomSkin,
              let pending = pendingAutomaticCustomSkinLaunch else {
            return []
        }
        var details: [ControllerNavigationAlertDetail] = [
            .init(
                id: "skin",
                label: settings.localized("Skin"),
                value: pending.proposal.skinName,
                onPrevious: { [self] in
                    selectAutomaticCustomSkin(offset: -1)
                },
                onNext: { [self] in
                    selectAutomaticCustomSkin(offset: 1)
                },
                onActivate: { [self] in
                    selectAutomaticCustomSkin(offset: 1)
                }
            ),
            .init(
                id: "game",
                label: settings.localized("Game"),
                value: pending.game.displayName
            ),
            .init(
                id: "setCustomLayout",
                label: settings.localized("Set custom layout"),
                value: pending.setCustomLayout
                    ? settings.localized("On")
                    : settings.localized("Off"),
                isOn: pending.setCustomLayout,
                onPrevious: { [self] in
                    setAutomaticCustomLayout(false)
                },
                onNext: { [self] in
                    setAutomaticCustomLayout(true)
                },
                onActivate: { [self] in
                    toggleAutomaticCustomLayout()
                }
            ),
        ]
        if pending.purpose == .gameLaunch {
            details.append(.init(
                id: "doNotShowAgain",
                label: settings.localized("Do not show this again"),
                value: pending.doNotShowAgain
                    ? settings.localized("On")
                    : settings.localized("Off"),
                isOn: pending.doNotShowAgain,
                onPrevious: { [self] in
                    setAutomaticCustomSkinPromptSuppressed(false)
                },
                onNext: { [self] in
                    setAutomaticCustomSkinPromptSuppressed(true)
                },
                onActivate: { [self] in
                    toggleAutomaticCustomSkinPromptSuppressed()
                }
            ))
        }
        return details
    }

    func controllerAlertActions(
        for kind: GameLibraryControllerAlertKind
    ) -> [ControllerNavigationAlertAction] {
        switch kind {
        case .automaticCustomSkin:
            if pendingAutomaticCustomSkinLaunch?.purpose == .library {
                return [
                    .init(id: "apply", title: settings.localized("Apply")),
                    .init(id: "cancel", title: settings.localized("Cancel")),
                ]
            }
            return [
                .init(id: "apply", title: settings.localized("OK")),
                .init(id: "cancelLaunch", title: settings.localized("Cancel and Launch")),
            ]
        case .restart:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "restart", title: settings.localized("Restart"), isDestructive: true),
            ]
        case .stop:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "stop", title: settings.localized("Stop"), isDestructive: true),
            ]
        case .deleteGameData:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "deleteData", title: settings.localized("Delete Game Data"), isDestructive: true),
            ]
        case .deleteGame:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "deleteROM", title: settings.localized("Delete ROM"), isDestructive: true),
                .init(id: "deleteAll", title: settings.localized("Delete ROM + Game Data"), isDestructive: true),
            ]
        case .replaceFiles:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "replace", title: settings.localized("Replace"), isDestructive: true),
            ]
        case .gameAction, .coverResult, .backgroundError:
            return [.init(id: "ok", title: settings.localized("OK"))]
        }
    }

    func handleControllerAlertCommand(
        _ command: MenuControllerCommand,
        kind: GameLibraryControllerAlertKind
    ) {
        let interactiveDetailCount = controllerAlertDetails(for: kind)
            .filter(\.isInteractive)
            .count
        let actionCount = controllerAlertActions(for: kind).count
        let selectionCount = interactiveDetailCount + actionCount
        switch command {
        case .up, .upLeft, .upRight:
            let next: Int
            if controllerAlertSelectedIndex >= interactiveDetailCount {
                next = max(0, interactiveDetailCount - 1)
            } else {
                next = max(0, controllerAlertSelectedIndex - 1)
            }
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .down, .downLeft, .downRight:
            let next: Int
            if controllerAlertSelectedIndex < interactiveDetailCount - 1 {
                next = controllerAlertSelectedIndex + 1
            } else if controllerAlertSelectedIndex < interactiveDetailCount,
                      actionCount > 0 {
                next = interactiveDetailCount
            } else {
                next = controllerAlertSelectedIndex
            }
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .left:
            if kind == .automaticCustomSkin,
               controllerAlertSelectedIndex < interactiveDetailCount {
                adjustAutomaticCustomSkinControl(
                    at: controllerAlertSelectedIndex,
                    incrementing: false
                )
                controllerInput?.playFeedback(.move(command))
                return
            }
            let next = max(
                interactiveDetailCount,
                controllerAlertSelectedIndex - 1
            )
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .right:
            if kind == .automaticCustomSkin,
               controllerAlertSelectedIndex < interactiveDetailCount {
                adjustAutomaticCustomSkinControl(
                    at: controllerAlertSelectedIndex,
                    incrementing: true
                )
                controllerInput?.playFeedback(.move(command))
                return
            }
            let next = min(
                selectionCount - 1,
                controllerAlertSelectedIndex + 1
            )
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .activate:
            if kind == .automaticCustomSkin,
               controllerAlertSelectedIndex < interactiveDetailCount {
                activateAutomaticCustomSkinControl(
                    at: controllerAlertSelectedIndex
                )
            } else {
                performControllerAlertAction(
                    controllerAlertSelectedIndex - interactiveDetailCount,
                    for: kind
                )
            }
            controllerInput?.playFeedback(.activate)
        case .back:
            if kind == .automaticCustomSkin {
                // The generic controller feedback is deferred until after the
                // focus transaction. This chooser clears its pending launch
                // state immediately, so play Return before dismissing it.
                cancelAutomaticCustomSkinLaunch(playsReturnSound: true)
            } else {
                dismissControllerAlert(kind)
                controllerInput?.playFeedback(.back)
            }
        case .toggleFavorite, .showContextMenu:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func adjustAutomaticCustomSkinControl(
        at index: Int,
        incrementing: Bool
    ) {
        switch index {
        case 0:
            selectAutomaticCustomSkin(offset: incrementing ? 1 : -1)
        case 1:
            setAutomaticCustomLayout(incrementing)
        case 2:
            setAutomaticCustomSkinPromptSuppressed(incrementing)
        default:
            break
        }
    }

    private func activateAutomaticCustomSkinControl(at index: Int) {
        switch index {
        case 0:
            selectAutomaticCustomSkin(offset: 1)
        case 1:
            toggleAutomaticCustomLayout()
        case 2:
            toggleAutomaticCustomSkinPromptSuppressed()
        default:
            break
        }
    }

    private func selectAutomaticCustomSkin(offset: Int) {
        guard var pending = pendingAutomaticCustomSkinLaunch,
              pending.proposals.count > 1 else { return }
        let count = pending.proposals.count
        pending.selectedProposalIndex = (
            pending.selectedProposalIndex + offset + count
        ) % count
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func setAutomaticCustomLayout(_ enabled: Bool) {
        guard var pending = pendingAutomaticCustomSkinLaunch,
              pending.setCustomLayout != enabled else { return }
        pending.setCustomLayout = enabled
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func toggleAutomaticCustomLayout() {
        guard var pending = pendingAutomaticCustomSkinLaunch else { return }
        pending.setCustomLayout.toggle()
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func setAutomaticCustomSkinPromptSuppressed(_ suppressed: Bool) {
        guard var pending = pendingAutomaticCustomSkinLaunch,
              pending.doNotShowAgain != suppressed else { return }
        pending.doNotShowAgain = suppressed
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func toggleAutomaticCustomSkinPromptSuppressed() {
        guard var pending = pendingAutomaticCustomSkinLaunch else { return }
        pending.doNotShowAgain.toggle()
        pendingAutomaticCustomSkinLaunch = pending
    }

    func performControllerAlertAction(
        _ index: Int,
        for kind: GameLibraryControllerAlertKind
    ) {
        switch kind {
        case .automaticCustomSkin:
            completeAutomaticCustomSkinLaunch(apply: index == 0)
        case .restart:
            guard index == 1 else {
                dismissControllerAlert(kind)
                return
            }
            showRestartAlert = false
            if pendingGameName.isEmpty {
                pendingGameplayLaunchTransition = nil
                pendingGameplayPadSerial = nil
                pendingGameplayPadCRC = nil
                session.shutdownAndBootBIOS()
            } else {
                let transition = pendingGameplayLaunchTransition
                pendingGameplayLaunchTransition = nil
                session.prepareGameplayPadSelection(
                    forSerial: pendingGameplayPadSerial,
                    crc: pendingGameplayPadCRC
                )
                pendingGameplayPadSerial = nil
                pendingGameplayPadCRC = nil
                session.shutdownAndBoot(
                    isoName: pendingGameName,
                    launchTransition: transition
                )
            }
        case .stop:
            if index == 1 {
                scheduleStopRunningGame()
            } else {
                showStopAlert = false
            }
        case .deleteGameData:
            if index == 1, let game = pendingDeleteDataGame {
                deleteGameData(game)
            }
            pendingDeleteDataGame = nil
        case .deleteGame:
            if let game = pendingDeleteGame {
                if index == 1 {
                    deleteGame(game, deleteData: false)
                } else if index == 2 {
                    deleteGame(game, deleteData: true)
                }
            }
            pendingDeleteGame = nil
        case .gameAction:
            gameActionMessage = nil
        case .coverResult:
            coverStore.showCoverAlert = false
        case .replaceFiles:
            if index == 1 {
                importGames(
                    pendingGameImportURLs,
                    allowReplacingExistingFiles: true
                )
            }
            showGameReplacementAlert = false
            clearPendingGameImport()
        case .backgroundError:
            showBackgroundAssetError = false
        }
    }

    func dismissControllerAlert(
        _ kind: GameLibraryControllerAlertKind
    ) {
        switch kind {
        case .automaticCustomSkin:
            cancelAutomaticCustomSkinLaunch()
        case .restart:
            showRestartAlert = false
            pendingGameplayLaunchTransition = nil
            pendingGameplayPadSerial = nil
            pendingGameplayPadCRC = nil
        case .stop:
            showStopAlert = false
        case .deleteGameData:
            pendingDeleteDataGame = nil
        case .deleteGame:
            pendingDeleteGame = nil
        case .gameAction:
            gameActionMessage = nil
        case .coverResult:
            coverStore.showCoverAlert = false
        case .replaceFiles:
            showGameReplacementAlert = false
            clearPendingGameImport()
        case .backgroundError:
            showBackgroundAssetError = false
        }
    }

    /// Tapping the scrim or pressing Back dismisses the recommendation without
    /// launching the game. "Cancel and Launch" remains the only dismissal path
    /// which deliberately continues into gameplay.
    func cancelAutomaticCustomSkinLaunch(
        playsReturnSound: Bool = false
    ) {
        if playsReturnSound {
            MenuAudioPackManager.shared.playEvent(.return)
        }
        pendingAutomaticCustomSkinLaunch = nil
        pendingGameplayLaunchTransition = nil
        pendingGameplayPadSerial = nil
        pendingGameplayPadCRC = nil
    }

    @discardableResult
    func dismissControllerPresentation() -> Bool {
        if gameInfoTarget != nil {
            gameInfoTarget = nil
        } else if gameSettingsTarget != nil {
            gameSettingsTarget = nil
        } else if discLinkTarget != nil {
            discLinkTarget = nil
        } else if cheatsManagerTarget != nil {
            cheatsManagerTarget = nil
        } else if showRestartAlert {
            showRestartAlert = false
            pendingGameplayLaunchTransition = nil
            pendingGameplayPadSerial = nil
            pendingGameplayPadCRC = nil
        } else if showStopAlert {
            showStopAlert = false
        } else if pendingDeleteGame != nil {
            pendingDeleteGame = nil
        } else if pendingDeleteDataGame != nil {
            pendingDeleteDataGame = nil
        } else if gameActionMessage != nil {
            gameActionMessage = nil
        } else if coverStore.showCoverAlert {
            coverStore.showCoverAlert = false
        } else if showCoverTemplateEditor {
            showCoverTemplateEditor = false
        } else if showGameReplacementAlert {
            showGameReplacementAlert = false
            clearPendingGameImport()
        } else if showBackgroundAssetError {
            showBackgroundAssetError = false
        } else if showGameImporter {
            showGameImporter = false
        } else if showCoverImporter {
            showCoverImporter = false
            pendingCoverGameName = nil
        } else if showCoverPhotoPicker {
            showCoverPhotoPicker = false
            pendingCoverPhotoGameName = nil
        } else {
            return false
        }
        return true
    }
}
