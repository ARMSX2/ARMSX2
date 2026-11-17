// GameplayOverlayModel.swift — The game screen's pause card route and child panel state
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// `.hidden` is gameplay, `.paused` the pause card, `.pausedPresenting` a child screen over it.
enum OverlayRoute: Equatable {
    case hidden
    case paused
    case pausedPresenting(QuickMenuDestination)
}

/// The pause card route, its child panels and the VM pause they hold. GameScreenView owns it as
/// @State, so none of it outlives the screen.
@MainActor
@Observable
final class GameplayOverlayModel {
    var overlayRoute: OverlayRoute = .hidden {
        // The sheet binding's setter never runs for a teardown, so the request goes with the route.
        didSet { if overlayRoute != .pausedPresenting(.reportProblem) { reportRequest = nil } }
    }
    // The pause menu child last opened. Its row takes focus when the card returns.
    var pauseMenuChild: QuickMenuDestination?
    var pauseCardFoldMemory: QuickMenuFoldMemory?
    var reportRequest: ReportSheetRequest?
    private var reportCapture: Task<Void, Never>?
    var cheatsSession = CheatsPatchesSession()
    var runtimePerGameSettingsEntry: ISOEntry?
    var runtimePerGameSettings: [String: Any]?
    var runtimePerGameStartsInGameController = false
    var runtimePerGameStartsInShaders = false
    var runtimePerGameLivePreviewPresentation:
        PerGameLivePreviewPresentation = .editing
    var runtimePerGameLivePreviewDismissRequest: UInt64 = 0
    // Optional, so the per-game panel's observable state goes when the panel closes.
    var runtimePerGameLivePreviewToast:
        PerGameLivePreviewToastState?
    var resumeGameplayTask: Task<Void, Never>?
    private var quickMenuResourceReleaseTask: Task<Void, Never>?
    var runtimePadLayoutIdentity: PadLayoutGameIdentity?
    private(set) var runtimeOverlayPauseActive = false

    private let feedback: GameplayFeedback
    private let shortcuts: GameplayShortcutCoordinator
    private let session = GameSession.shared
    private let settings = SettingsStore.shared
    private let appState = AppState.shared

    init(feedback: GameplayFeedback, shortcuts: GameplayShortcutCoordinator) {
        self.feedback = feedback
        self.shortcuts = shortcuts
    }

    func updateRuntimeOverlayPause() {
        // A save-state macro keeps the VM paused until its CPU-thread work is done.
        let perGameEditorIsVisible: Bool
        if case .pausedPresenting(let destination) = overlayRoute {
            perGameEditorIsVisible = destination == .perGame
                || destination == .gameController
                || destination == .shaders
        } else {
            perGameEditorIsVisible = false
        }
        let previewRunsGame = perGameEditorIsVisible
            && runtimePerGameLivePreviewPresentation.runsGame
        let shouldPause = (overlayRoute != .hidden && !previewRunsGame)
            || shortcuts.operationActive
        guard runtimeOverlayPauseActive != shouldPause else { return }

        runtimeOverlayPauseActive = shouldPause
        if ARMSX2Bridge.isVMRunning() {
            ARMSX2Bridge.setVMPaused(shouldPause)
        }
    }

    /// Drops a closed destination's data once its removal animation ends, so none of it stays while the VM runs.
    @MainActor
    func scheduleInactiveQuickMenuResourceRelease(
        for route: OverlayRoute,
        reduceMotion: Bool
    ) {
        quickMenuResourceReleaseTask?.cancel()
        quickMenuResourceReleaseTask = Task { @MainActor in
            if reduceMotion {
                await Task.yield()
            } else {
                try? await Task.sleep(for: .milliseconds(220))
            }
            guard !Task.isCancelled, overlayRoute == route else { return }
            releaseInactiveQuickMenuPresentationResources(for: route)
            quickMenuResourceReleaseTask = nil
        }
    }

    @MainActor
    private func releaseInactiveQuickMenuPresentationResources(
        for route: OverlayRoute
    ) {
        let activeDestination: QuickMenuDestination?
        if case .pausedPresenting(let destination) = route {
            activeDestination = destination
        } else {
            activeDestination = nil
        }

        if activeDestination != .perGame
            && activeDestination != .gameController
            && activeDestination != .shaders {
            releasePerGamePresentationResources()
        }
        if activeDestination != .cheats {
            PatchStore.shared.releasePresentationResources()
        }
    }

    @MainActor
    private func releasePerGamePresentationResources() {
        ARMSX2Bridge.setPerGameLivePreviewAudioMuted(false)
        runtimePerGameSettingsEntry = nil
        runtimePerGameSettings = nil
        runtimePerGameStartsInGameController = false
        runtimePerGameStartsInShaders = false
        runtimePerGameLivePreviewPresentation = .editing
        runtimePerGameLivePreviewDismissRequest = 0
        runtimePerGameLivePreviewToast?.clear()
        runtimePerGameLivePreviewToast = nil
    }

    @MainActor
    func releaseAllQuickMenuPresentationResources() {
        quickMenuResourceReleaseTask?.cancel()
        quickMenuResourceReleaseTask = nil
        releasePerGamePresentationResources()
        PatchStore.shared.releasePresentationResources()
    }

    @MainActor
    func resumeGameplayAfterControllerRelease(controllerInput: MenuControllerInputRouter?) {
        resumeGameplayTask?.cancel()
        guard controllerInput?.pressedFaceButton == .cross else {
            overlayRoute = .hidden
            return
        }

        // Keep input until Cross is up, or the press that chose Resume reaches the VM.
        resumeGameplayTask = Task { @MainActor in
            while controllerInput?.pressedFaceButton == .cross {
                try? await Task.sleep(for: .milliseconds(8))
                guard !Task.isCancelled else { return }
            }
            try? await Task.sleep(for: .milliseconds(16))
            guard !Task.isCancelled else { return }
            resumeGameplayTask = nil
            overlayRoute = .hidden
        }
    }

    /// `.perGame` needs the VM-safe settings load. Every other destination opens over the card.
    func openPauseMenuChild(_ destination: QuickMenuDestination, gameMenuAvailable: Bool) {
        pauseMenuChild = destination
        // No `default`, so a new destination without a presentation fails to compile.
        switch destination {
        case .perGame:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            openPerGameSettingsForCurrentGame(gameMenuAvailable: gameMenuAvailable)
        case .gameController:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            openPerGameSettingsForCurrentGame(
                gameMenuAvailable: gameMenuAvailable,
                initiallySelectingGameController: true
            )
        case .cheats:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            cheatsSession = CheatsPatchesSession()
            overlayRoute = .pausedPresenting(destination)
        case .controllerSkin:
            openRuntimeControllerSkinPicker()
        case .shaders:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            openPerGameSettingsForCurrentGame(
                gameMenuAvailable: gameMenuAvailable,
                initiallySelectingShaders: true
            )
        case .reportProblem:
            guard let id = SessionStore.currentID else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
            reportRequest = nil
            overlayRoute = .pausedPresenting(destination)
            // Captured once per tap; nil opens the sheet without a frame.
            reportCapture?.cancel()
            reportCapture = Task { @MainActor in
                let png = await ARMSX2Bridge.capturePausedFrame()
                guard !Task.isCancelled, overlayRoute == .pausedPresenting(.reportProblem) else { return }
                reportRequest = ReportSheetRequest(sessionID: id, pausedFramePNG: png)
            }
        case .speed, .saveStates, .retroAchievements, .padLayout, .resetROM,
             .changeDisc:
            overlayRoute = .pausedPresenting(destination)
        }
    }

    private func openRuntimeControllerSkinPicker() {
        let serial = PadLayoutGameIdentity.normalizedSerial(
            runtimePadLayoutIdentity?.serial ?? session.gameplayPadSerial
        )
        let proposals = AutomaticCustomSkinManager.shared
            .installedProposals(forSerial: serial)
        guard !proposals.isEmpty else {
            feedback.presentImportantStatusMessage(
                settings.localized(
                    "Controller skins need a running game with a valid serial."
                )
            )
            return
        }

        MenuAudioPackManager.shared.playEvent(.uiToast)
        overlayRoute = .pausedPresenting(.controllerSkin)
    }

    func dismissRuntimeControllerSkinPicker(playsSound: Bool) {
        guard overlayRoute == .pausedPresenting(.controllerSkin) else { return }
        if playsSound {
            MenuAudioPackManager.shared.playEvent(.return)
        }
        overlayRoute = .paused
    }

    /// Presents a child sheet from the route. Dismissal returns to `.paused` unless a teardown already left it.
    func childPresentedBinding(_ child: QuickMenuDestination) -> Binding<Bool> {
        // Half open, runtimeQuickMenuOverlay hosts the child instead and a fold keeps the route.
        Binding(
            get: { [self] in overlayRoute == .pausedPresenting(child) && !appState.hingePartiallyOpen },
            set: { [self] isPresented in
                guard !isPresented, !appState.hingePartiallyOpen else { return }
                if case .pausedPresenting(let active) = overlayRoute, active == child {
                    overlayRoute = .paused
                }
            }
        )
    }

    /// Same hand-off as childPresentedBinding, but the sheet's content is the request.
    var reportRequestBinding: Binding<ReportSheetRequest?> {
        Binding(
            get: { [self] in
                overlayRoute == .pausedPresenting(.reportProblem) && !appState.hingePartiallyOpen ? reportRequest : nil
            },
            set: { [self] request in
                guard request == nil, !appState.hingePartiallyOpen else { return }
                reportRequest = nil
                if overlayRoute == .pausedPresenting(.reportProblem) { overlayRoute = .paused }
            }
        )
    }

    private func openPerGameSettingsForCurrentGame(
        gameMenuAvailable: Bool,
        initiallySelectingGameController: Bool = false,
        initiallySelectingShaders: Bool = false
    ) {
        runtimePerGameStartsInGameController =
            initiallySelectingGameController
        runtimePerGameStartsInShaders = initiallySelectingShaders
        let destination: QuickMenuDestination = initiallySelectingShaders
            ? .shaders
            : (initiallySelectingGameController ? .gameController : .perGame)
        ARMSX2Bridge.setPerGameLivePreviewAudioMuted(false)
        runtimePerGameLivePreviewPresentation = .editing
        if runtimePerGameLivePreviewToast == nil {
            runtimePerGameLivePreviewToast = PerGameLivePreviewToastState()
        } else {
            runtimePerGameLivePreviewToast?.clear()
        }
        // Use the VM-safe bridge path to avoid a disc-image scan while the game is running.
        guard let gameName = session.currentRuntimeGameName(gameMenuAvailable: gameMenuAvailable),
              let info = ARMSX2Bridge.gameSettingsForCurrentGame() else {
            runtimePerGameSettingsEntry = nil
            runtimePerGameSettings = nil
            withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
                overlayRoute = .pausedPresenting(destination)
            }
            feedback.presentImportantStatusMessage(settings.localized("Per-game settings need a running game."))
            return
        }

        let serial = (info["serial"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        runtimePerGameSettingsEntry = ISOEntry(
            name: gameName,
            fileURL: nil,
            bootPath: nil,
            coverURL: nil,
            coverSignature: nil,
            metadata: ["serial": serial, "crc": (info["crc"] as? String) ?? ""].filter { !$0.value.isEmpty },
            size: 0,
            isFavorite: false
        )
        runtimePerGameSettings = info
        withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
            overlayRoute = .pausedPresenting(destination)
        }
    }

    func closePerGameSettingsOverlay() {
        ARMSX2Bridge.setPerGameLivePreviewAudioMuted(false)
        // Save/Cancel from Per-Game Settings return to the pause menu, not gameplay.
        withAnimation(.easeInOut(duration: 0.2)) {
            overlayRoute = .paused
        }
        // Cleanup waits for the fade, or the outgoing editor would show "No Game Active".
        runtimePerGameLivePreviewToast?.clear()
    }

    func resetCurrentROM() {
        // Leave the overlay first, so the pause state cannot re-pause a resetting VM.
        overlayRoute = .hidden
        session.resetCurrentVM()
        feedback.presentStatusMessage(settings.localized("Restarting ROM..."))
    }

    func clearCurrentGameCache(gameMenuAvailable: Bool) {
        guard let gameName = session.currentRuntimeGameName(gameMenuAvailable: gameMenuAvailable) else {
            feedback.presentImportantStatusMessage(settings.localized("Cache clear needs a running game."))
            return
        }

        let message = ARMSX2Bridge.clearCache(forISO: gameName)
        feedback.presentStatusMessage(message)
    }

    func changeDisc(to discName: String) {
        feedback.presentStatusMessage("Changing disc...")
        ARMSX2Bridge.changeDisc(toISO: discName) { [self] success in
            Task { @MainActor in
                if success {
                    feedback.presentStatusMessage("\(discName) inserted. Use the game's disc-swap prompt if needed.")
                } else {
                    feedback.presentImportantStatusMessage("Could not change discs. Open the game's disc-swap prompt first, or restart with the target disc.")
                }
            }
        }
    }

    func restartWithDisc(_ discName: String) {
        // Leave the overlay first, so the pause state cannot fight the teardown.
        overlayRoute = .hidden
        feedback.presentStatusMessage("Restarting with \(discName)...")
        session.shutdownAndBoot(isoName: discName)
    }

    func ejectDisc() {
        feedback.presentStatusMessage("Ejecting disc...")
        ARMSX2Bridge.ejectDisc { [self] success in
            Task { @MainActor in
                if success {
                    feedback.presentStatusMessage("Disc ejected")
                } else {
                    feedback.presentImportantStatusMessage("Could not eject the disc. Try again after the game has finished loading.")
                }
            }
        }
    }
}
