// GameplayOverlayHost.swift — The game screen's pause card, quick menu and child panels
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// The pause card, its child panels and their sheets. It takes the overlay model whole, since its
/// builders read more of it than eight inputs would carry.
struct GameplayOverlayHost: View {
    let overlay: GameplayOverlayModel
    let feedback: GameplayFeedback
    @Binding var userVirtualPadVisible: Bool
    @Binding var fullScreen: Bool
    @Binding var menuButtonHidden: Bool
    let vmMenuAvailable: Bool
    let gameMenuAvailable: Bool
    let virtualPadHiddenByController: Bool
    let activePadLayoutDisplayName: String
    let effectivePadSkinDescriptor: VPadSkinDescriptor
    let runtimePadLayoutEditorContext: PadLayoutEditorContext
    let screenIsLandscape: Bool
    let applyControllerSkin: (VPadSkinDescriptor) -> Void
    let finishRuntimePadLayoutEditing: () -> Void

    @State private var appState = AppState.shared
    @State private var session = GameSession.shared
    @State private var settings = SettingsStore.shared
    @Environment(\.menuControllerInputRouter) private var controllerInput
    private let saveStateUndo = SaveStateUndoModel.shared

    var body: some View {
        Color.clear
            .allowsHitTesting(false)
            .sheet(isPresented: overlay.childPresentedBinding(.speed)) {
                SpeedControlPanel(settings: settings)
                    .presentationDetents([.medium, .large])
            }
            .sheet(isPresented: overlay.childPresentedBinding(.retroAchievements)) {
                RetroAchievementsGamePanel(settings: settings)
                    .presentationDetents([.medium, .large])
            }
            .sheet(isPresented: overlay.childPresentedBinding(.cheats)) {
                CheatsPatchesManagerView(
                    isoName: ARMSX2Bridge.currentGameISOName() ?? "",
                    gameTitle: "",
                    launchContext: .inGame,
                    controllerInput: controllerInput,
                    session: overlay.cheatsSession
                )
                .presentationDetents([.medium, .large])
            }
            .sheet(item: overlay.reportRequestBinding) {
                ReportSheet(request: $0)
                    .presentationDetents([.medium, .large])
            }
            .overlay {
                runtimeQuickMenuOverlay
            }
            .overlay {
                // The same shortcut bar under the pause menu. It goes the moment the menu closes.
                if overlay.overlayRoute == .paused {
                    GeometryReader { geo in
                        let insets = geo.safeAreaInsets
                        let window = CGSize(width: geo.size.width + insets.leading + insets.trailing,
                                            height: geo.size.height + insets.top + insets.bottom)
                        // iPhone Duo's inner pause card shows the tiles in its footer instead.
                        if !(appState.hingePresent && usesLargeLayout(windowSize: window)) {
                            GameplayControllerShortcutHelpOverlay(
                                settings: settings,
                                controllerInput: controllerInput,
                                overPauseMenu: true
                            )
                        }
                    }
                        .transition(.identity)
                }
            }
    }

    @ViewBuilder
    private var pauseMenuOverlay: some View {
        if case .paused = overlay.overlayRoute {
            // Only in `.paused`: a child covers the screen, and the card returns when it closes.
            GameOverlayContainer(
                onTapOutside: { overlay.overlayRoute = .hidden },
                frameMode: .landscapePanel,
                // Room for the controller shortcut bar, which shows while this card is up.
                bottomReserve: controllerInput?.hasConnectedController == true
                    ? GameplayControllerShortcutHelpOverlay.pauseCardReserve : 0,
                spreadsAcrossFold: true
            ) { metrics in
                QuickMenuView(
                    settings: settings,
                    padVisible: $userVirtualPadVisible,
                    fullScreen: $fullScreen,
                    menuButtonHidden: $menuButtonHidden,
                    vmMenuAvailable: vmMenuAvailable,
                    gameMenuAvailable: gameMenuAvailable,
                    virtualPadHiddenByController: virtualPadHiddenByController,
                    // Presets are frontend work, so they stay when a build omits a renderer.
                    shaderChainAvailable: true,
                    gameTitle: session.currentRuntimeGameName(gameMenuAvailable: gameMenuAvailable),
                    variant: metrics.variant,
                    returningFrom: overlay.pauseMenuChild,
                    foldMemory: Bindable(overlay).pauseCardFoldMemory,
                    activePadLayoutName: activePadLayoutDisplayName,
                    activeControllerSkinName:
                        effectivePadSkinDescriptor.displayName,
                    onCycleOSD: { cycleOsdPreset() },
                    onOpen: { destination in
                        overlay.openPauseMenuChild(destination, gameMenuAvailable: gameMenuAvailable)
                    },
                    onClearCache: {
                        overlay.overlayRoute = .hidden
                        overlay.clearCurrentGameCache(gameMenuAvailable: gameMenuAvailable)
                    },
                    onBackToMenu: {
                        session.returnToMenu()
                        // The game stays open behind the menu, and the app can be closed from there.
                        if settings.autoSaveEnabled, settings.autoSaveOnLeave {
                            SaveStateAutoSave.shared.save(leaving: true) { _ in }
                        }
                    },
                    onStop: {
                        MenuAudioPackManager.shared
                            .playStoppingGameOnMainInterface()
                        if settings.hapticFeedback { GameEventHaptics.shared.medium.impactOccurred() }
                        overlay.overlayRoute = .hidden
                        // Leave now, so nobody watches gameplay through the card while NVRAM flushes.
                        session.returnToMenu()
                        session.stopGame()
                    },
                    onResume: {
                        if settings.hapticFeedback { GameEventHaptics.shared.light.impactOccurred() }
                        overlay.resumeGameplayAfterControllerRelease(controllerInput: controllerInput)
                    }
                )
            }
        }
    }

    /// One host for the whole Quick Menu tree. In gameplay it is EmptyView, so none of it is mounted.
    @ViewBuilder
    private var runtimeQuickMenuOverlay: some View {
        switch overlay.overlayRoute {
        case .hidden:
            EmptyView()
        case .paused:
            // Keyed on rotation, so its GeometryReader drops stale landscape metrics.
            pauseMenuOverlay.id(screenIsLandscape)
        case .pausedPresenting(let destination):
            switch destination {
            case .padLayout:
                PadLayoutEditView(
                    onDismiss: { finishRuntimePadLayoutEditing() },
                    context: runtimePadLayoutEditorContext
                )
                .frame(
                    maxWidth: .infinity,
                    maxHeight: .infinity,
                    alignment: .top
                )
            case .perGame, .gameController, .shaders:
                ZStack {
                    // Not keyed: a rebuild would discard unsaved per-game edits.
                    GameOverlayContainer(frameMode: .landscapePanel) { _ in
                        runtimePerGameSettingsContent
                    }
                    .opacity(
                        overlay.runtimePerGameLivePreviewPresentation.hidesSettingsUI
                            ? 0
                            : 1
                    )
                    .allowsHitTesting(
                        overlay.runtimePerGameLivePreviewPresentation == .editing
                    )
                    .accessibilityHidden(
                        overlay.runtimePerGameLivePreviewPresentation.hidesSettingsUI
                    )

                    if overlay.runtimePerGameLivePreviewPresentation.hidesSettingsUI {
                        PerGameLivePreviewExitOverlay(
                            controllerInput: controllerInput,
                            stopsWithCircle:
                                settings.perGameLivePreviewStopsWithCircle
                        ) {
                            overlay.runtimePerGameLivePreviewDismissRequest &+= 1
                        }
                        .transition(.opacity)
                        .zIndex(20_100)
                    }
                }
            case .controllerSkin:
                RuntimeControllerSkinPicker(
                    serial: PadLayoutGameIdentity.normalizedSerial(
                        overlay.runtimePadLayoutIdentity?.serial ?? session.gameplayPadSerial
                    ),
                    identity: overlay.runtimePadLayoutIdentity,
                    currentSkinID: effectivePadSkinDescriptor.id,
                    gameName: session.currentRuntimeGameName(gameMenuAvailable: gameMenuAvailable),
                    controllerInput: controllerInput,
                    feedback: feedback,
                    onApplied: applyControllerSkin,
                    onDismiss: overlay.dismissRuntimeControllerSkinPicker
                )
            case .saveStates:
                GameOverlayContainer(frameMode: .landscapePanel, ipadPortraitHeightCap: .infinity) { metrics in
                    SaveStatesPanel(
                        settings: settings,
                        undo: saveStateUndo,
                        // A portrait leaf beside iPhone Duo's fold is phone-sized, so it takes the phone rows.
                        variant: metrics.isLandscape || !screenIsLandscape ? metrics.variant : .phonePortrait,
                        landscape: screenIsLandscape,
                        gameTitle: session.currentRuntimeGameName(gameMenuAvailable: gameMenuAvailable),
                        statusHandler: { message, isImportant in
                            feedback.presentStatusMessage(
                                message,
                                displayDuration: isImportant
                                    ? GameplayFeedback.importantStatusDisplayDuration
                                    : GameplayFeedback.briefStatusDisplayDuration
                            )
                        },
                        onLoaded: { overlay.resumeGameplayAfterControllerRelease(controllerInput: controllerInput) },
                        onClose: { overlay.overlayRoute = .paused },
                        onOpenControllerMacros: { overlay.openPauseMenuChild(.gameController, gameMenuAvailable: gameMenuAvailable) }
                    )
                }
            case .changeDisc:
                GameOverlayContainer(frameMode: .landscapePanel) { metrics in
                    ChangeDiscPanel(
                        settings: settings,
                        variant: metrics.variant,
                        discs: session.availableDiscSwapNames,
                        driveDisc: ARMSX2Bridge.discInDriveName(),
                        onEject: {
                            overlay.ejectDisc()
                            overlay.overlayRoute = .paused
                        },
                        onInsert: { disc in
                            overlay.changeDisc(to: disc)
                            overlay.overlayRoute = .paused
                        },
                        onRestart: overlay.restartWithDisc,
                        onClose: { overlay.overlayRoute = .paused }
                    )
                }
            case .resetROM:
                runtimeResetROMAlert
            case .speed where appState.hingePartiallyOpen:
                GameOverlayContainer(frameMode: .landscapePanel) { _ in
                    SpeedControlPanel(settings: settings, onClose: { overlay.overlayRoute = .paused })
                        .glassSurface(clear: false, cornerRadius: 26)
                }
                .accessibilityElement(children: .contain)
                .accessibilityAction(.escape) { overlay.overlayRoute = .paused }
            case .retroAchievements where appState.hingePartiallyOpen:
                GameOverlayContainer(frameMode: .landscapePanel) { _ in
                    RetroAchievementsGamePanel(settings: settings, onClose: { overlay.overlayRoute = .paused })
                        .glassSurface(clear: false, cornerRadius: 26)
                }
                .accessibilityElement(children: .contain)
                .accessibilityAction(.escape) { overlay.overlayRoute = .paused }
            case .cheats where appState.hingePartiallyOpen:
                GameOverlayContainer(frameMode: .landscapePanel) { _ in
                    CheatsPatchesManagerView(
                        isoName: ARMSX2Bridge.currentGameISOName() ?? "",
                        gameTitle: "",
                        launchContext: .inGame,
                        controllerInput: controllerInput,
                        onClose: { overlay.overlayRoute = .paused },
                        session: overlay.cheatsSession
                    )
                }
                .accessibilityElement(children: .contain)
                .accessibilityAction(.escape) { overlay.overlayRoute = .paused }
            case .reportProblem where appState.hingePartiallyOpen:
                if let request = overlay.reportRequest {
                    GameOverlayContainer(frameMode: .landscapePanel) { _ in
                        ReportSheet(request: request, onClose: {
                            overlay.reportRequest = nil
                            overlay.overlayRoute = .paused
                        })
                        .glassSurface(clear: false, cornerRadius: 26)
                    }
                    .accessibilityElement(children: .contain)
                    .accessibilityAction(.escape) {
                        overlay.reportRequest = nil
                        overlay.overlayRoute = .paused
                    }
                }
            case .speed, .cheats, .reportProblem,
                 .retroAchievements:
                // Sheet contents. The route stays so dismissal returns to the pause card.
                EmptyView()
            }
        }
    }

    private var runtimeResetROMAlert: some View {
        ControllerNavigationAlert(
            title: settings.localized("Reset ROM?"),
            message: settings.localized(
                "Restart the current game? Unsaved progress will be lost."
            ),
            actions: [
                .init(
                    id: "cancel",
                    title: settings.localized("Cancel")
                ),
                .init(
                    id: "reset",
                    title: settings.localized("Reset ROM"),
                    isDestructive: true
                ),
            ],
            selectedIndex: 0,
            onSelect: { index in
                if index == 0 {
                    overlay.overlayRoute = .paused
                } else {
                    overlay.resetCurrentROM()
                }
            },
            onDismiss: {
                overlay.overlayRoute = .paused
            }
        )
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "runtime.reset-rom-confirmation",
            priority: 320,
            orbStyle: .plain,
            onBack: {
                overlay.overlayRoute = .paused
                return true
            },
            usesExplicitTargetGeometryOnly: true,
            preferredInitialFocusLabel: settings.localized("Cancel")
        )
    }

    @ViewBuilder
    private var runtimePerGameSettingsContent: some View {
        if let runtimePerGameSettingsEntry = overlay.runtimePerGameSettingsEntry {
            PerGameSettingsPanel(
                game: runtimePerGameSettingsEntry,
                preloadedSettings: overlay.runtimePerGameSettings,
                savesToRunningGame: true,
                initiallySelectsGameController:
                    overlay.runtimePerGameStartsInGameController,
                initiallySelectsShaders: overlay.runtimePerGameStartsInShaders,
                controllerInput: controllerInput,
                livePreviewDismissRequest:
                    overlay.runtimePerGameLivePreviewDismissRequest,
                onLivePreviewPresentationChange: { presentation in
                    guard case .pausedPresenting(let destination) = overlay.overlayRoute,
                          destination == .perGame
                            || destination == .gameController
                            || destination == .shaders else { return }
                    overlay.runtimePerGameLivePreviewPresentation = presentation
                    ARMSX2Bridge.setPerGameLivePreviewAudioMuted(
                        presentation != .editing
                    )
                    if presentation == .editing || presentation == .finishing {
                        overlay.runtimePerGameLivePreviewToast?.clear()
                    }
                    overlay.updateRuntimeOverlayPause()
                },
                onLivePreviewStatusChange: { status in
                    overlay.runtimePerGameLivePreviewToast?.present(status)
                }
            ) {
                overlay.closePerGameSettingsOverlay()
            }
        } else {
            NavigationStack {
                ContentUnavailableView(
                    settings.localized("No Game Active"),
                    systemImage: "gamecontroller",
                    description: Text(settings.localized("Start a game before changing per-game settings."))
                )
                .navigationTitle(settings.localized("Per-Game Settings"))
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button(settings.localized("Done")) {
                            overlay.closePerGameSettingsOverlay()
                        }
                    }
                }
            }
        }
    }

    private func cycleOsdPreset() {
        let allPresets: [OsdPreset] = OsdPreset.allCases
        guard let currentIndex = allPresets.firstIndex(of: settings.osdPreset) else {
            return
        }
        let nextIndex = (currentIndex + 1) % allPresets.count
        let nextPreset = allPresets[nextIndex]

        settings.osdPreset = nextPreset
        ARMSX2Bridge.setPerformanceOverlayVisible(nextPreset != .off)

        let label = nextPreset != .off
            ? settings.localized(nextPreset.label)
            : settings.localized("OFF")
        feedback.presentStatusMessage("OSD: \(label)")
    }
}
