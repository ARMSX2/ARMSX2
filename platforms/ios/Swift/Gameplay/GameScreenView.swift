// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import GameController
import Observation

private let runtimeMenuStateChangedNotification = Notification.Name("ARMSX2iOSRuntimeMenuStateChanged")
let retroAchievementsToastNotification = Notification.Name("ARMSX2RetroAchievementsNotification")

private struct GameScreenSizePreferenceKey: PreferenceKey {
    static let defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        value = nextValue()
    }
}

struct GameScreenView: View {
    let showsGameplayControllerShortcutHelp: Bool

    init(showsGameplayControllerShortcutHelp: Bool) {
        self.showsGameplayControllerShortcutHelp = showsGameplayControllerShortcutHelp
        // Built together so the coordinator and the overlay raise their toasts on the banners this view shows.
        let feedback = GameplayFeedback()
        let shortcuts = GameplayShortcutCoordinator(feedback: feedback)
        _feedback = State(initialValue: feedback)
        _shortcuts = State(initialValue: shortcuts)
        _overlay = State(initialValue: GameplayOverlayModel(feedback: feedback, shortcuts: shortcuts))
    }

    @State private var appState = AppState.shared
    @State private var session = GameSession.shared
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = DynamicThumbstickSettings.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var touchActionSession = VirtualPadTouchActionSession()
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var externalControllerConnected = false
    @State private var fullScreen = false
    @State private var menuButtonHidden = false
    @State private var vmMenuAvailable = false
    @State private var gameMenuAvailable = false
    @State private var noJITFallbackActive = false
    @State private var feedback: GameplayFeedback
    @State private var shortcuts: GameplayShortcutCoordinator
    @State private var overlay: GameplayOverlayModel
    private let saveStateUndo = SaveStateUndoModel.shared
    @State private var previousHideHomeIndicator = false
    @State private var previousHideStatusBar = false
    @State private var wasBackgrounded = false
    // Bumped when the pad editor closes, so the pad is rebuilt with fresh UIKit controls.
    @State private var padRebuildToken = 0
    // A tap on the game shows the hidden menu button for a moment.
    @State private var menuButtonRevealed = false
    @State private var menuRevealTask: Task<Void, Never>?
    // Only the pause menu is keyed on this. The per-game panel holds unsaved edits.
    @State private var screenIsLandscape = true

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        GeometryReader { geo in
            // The window. A keyboard shrinks the safe region until iPad portrait reads wide.
            let screen = CGSize(
                width: geo.size.width + geo.safeAreaInsets.leading + geo.safeAreaInsets.trailing,
                height: geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom
            )
            let isLandscape = screen.width > screen.height

            Group {
                if isLandscape {
                    let padInsets = landscapePadInsets(geo, screen: screen)
                    // Landscape: full-screen layout so pad coordinates match the layout editor.
                    ZStack {
                        PhoneGameSurface()
                            .onTapGesture { revealMenuButtonBriefly() }
                            .accessibilityElement(children: .ignore)
                            .accessibilityLabel(Text("Game display"))
                            .accessibilityAddTraits(.isImage)
                            .accessibilityHint(Text("VoiceOver image recognition can read on-screen text."))
                            .overlay { menuRevealTapCatcher }
                        AccessibilityHUDMirror()
                        if effectiveVirtualPadVisible {
                            VirtualControllerView(
                                isLandscape: true,
                                layoutSnapshot: effectivePadLayoutSnapshot,
                                skinDescriptor: effectivePadSkinDescriptor,
                                touchActionSession: touchActionSession,
                                largeLayout: usesLargeLayout(windowSize: screen)
                            )
                            .padding(padInsets)
                            .id(padRebuildToken)
                            .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
                        }
                        dynamicCrosshairOverlay
                        menuButtonOverlay(isLandscape: true)
                            .padding(padInsets)
                            .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
                    }
                    .ignoresSafeArea()
                } else {
                    // Portrait: the game below the Dynamic Island, the pad deck down to the home indicator.
                    VStack(spacing: 0) {
                        // The deck ignores the bottom inset, so it runs to the foot of the window.
                        let deckHeight = screen.height - geo.safeAreaInsets.top
                        // Tabletop only: a fold running down a portrait window leaves the layout alone.
                        let fold = geo.activeFold(book: false)
                        let (gameHeight, fittedSnapshot, fit) = portraitGameSplit(
                            geo,
                            deckHeight: deckHeight,
                            fold: fold,
                            refitting: appState.hingePresent && effectiveVirtualPadVisible
                                ? effectivePadLayoutSnapshot ?? PadLayoutStore.shared.snapshot() : nil,
                            analogStickScale: settings.analogStickScale
                        )
                        PhoneGameSurface()
                            .frame(height: gameHeight)
                            .clipped()
                            .onTapGesture { revealMenuButtonBriefly() }
                            .accessibilityElement(children: .ignore)
                            .accessibilityLabel(Text("Game display"))
                            .accessibilityAddTraits(.isImage)
                            .accessibilityHint(Text("VoiceOver image recognition can read on-screen text."))
                            .overlay {
                                ZStack {
                                    menuRevealTapCatcher
                                    AccessibilityHUDMirror()
                                    dynamicCrosshairOverlay
                                }
                            }
                        if let fold {
                            Color.black.frame(height: fold.height)
                        }

                        if effectiveVirtualPadVisible {
                            ZStack {
                                Color.black
                                VirtualControllerView(
                                    layoutSnapshot: fittedSnapshot ?? effectivePadLayoutSnapshot,
                                    skinDescriptor: effectivePadSkinDescriptor,
                                    touchActionSession: touchActionSession,
                                    largeLayout: usesLargeLayout(windowSize: screen),
                                    fitScale: fittedSnapshot == nil ? nil : fit?.scale ?? 1
                                )
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                                .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
                            }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            .id(padRebuildToken)
                        }
                    }
                    .overlay(alignment: .topTrailing) {
                        if !menuButtonHidden || menuButtonRevealed {
                            menuButtonCluster()
                                .padding(.top, 8)
                                .padding(.trailing, 4)
                                .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
                        }
                    }
                    // `.keyboard` too: gameplay must not move when an overlay raises one.
                    .ignoresSafeArea([.container, .keyboard], edges: .bottom)
                    // Black fills the top safe area the game stays out of. The root is black only after a boot.
                    .background(Color.black.ignoresSafeArea())
                }
            }
            .preference(key: GameScreenSizePreferenceKey.self, value: screen)
            // Off the safe region, not the preference: the status bar moves one, not the other.
            .onChange(of: geo.size) { _, _ in syncFullscreenStateFromWindow() }
        }
        // An overlay owns focus; the pad and menu button under it stay out of reach.
        .accessibilityHidden(overlay.overlayRoute != .hidden)
        .onPreferenceChange(GameScreenSizePreferenceKey.self) { size in
            // The window, so only a real rotation reaches this.
            let landscape = size.width > size.height
            if screenIsLandscape != landscape {
                screenIsLandscape = landscape
            }
        }
        .overlay {
            GameplayToastOverlays(feedback: feedback, livePreviewToast: overlay.runtimePerGameLivePreviewToast)
        }
        .overlay {
            if showsGameplayControllerShortcutHelp, overlay.overlayRoute == .hidden {
                GameplayControllerShortcutHelpOverlay(
                    settings: settings,
                    controllerInput: controllerInput
                )
                    .transition(.opacity.combined(with: .scale(scale: 0.96)))
            }
        }
        .overlay {
            GameplayOverlayHost(
                overlay: overlay,
                feedback: feedback,
                userVirtualPadVisible: $session.userVirtualPadVisible,
                fullScreen: $fullScreen,
                menuButtonHidden: $menuButtonHidden,
                vmMenuAvailable: vmMenuAvailable,
                gameMenuAvailable: gameMenuAvailable,
                virtualPadHiddenByController: virtualPadHiddenByController,
                activePadLayoutDisplayName: activePadLayoutDisplayName,
                effectivePadSkinDescriptor: effectivePadSkinDescriptor,
                runtimePadLayoutEditorContext: runtimePadLayoutEditorContext,
                screenIsLandscape: screenIsLandscape,
                applyControllerSkin: applyControllerSkin,
                finishRuntimePadLayoutEditing: finishRuntimePadLayoutEditing
            )
        }
        .overlay(alignment: .top) { saveStateUndoOverlay }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.2), value: overlay.overlayRoute)
        .onAppear {
            let nativeEmulationOnlyMode = ARMSX2Bridge.isEmulationOnlyModeActive()
            // Back on a stripped VM, do not recreate what Emulation-Only Mode released.
            if !session.isEmulationOnlyMode && nativeEmulationOnlyMode {
                if session.restoreEmulationOnlyPresentation(makeEmulationOnlyPresentation) {
                    finishEmulationOnlyActivation()
                }
            } else if !session.isEmulationOnlyMode {
                FrameTimeDynamicResolutionController.shared.resumeAfterEmulationOnlyMode()
            }
            enterGameplaySystemChromeMode()
            syncFullscreenStateFromWindow()
            applyInitialFullscreenPreference()
            refreshExternalControllerConnectionState()
            refreshRuntimeMenuState()
            feedback.consumePendingRetroAchievementsToast()
            enterEmulationOnlyModeIfReady()
        }
        .onDisappear {
            overlay.resumeGameplayTask?.cancel()
            overlay.resumeGameplayTask = nil
            overlay.releaseAllQuickMenuPresentationResources()
            session.dropEmulationOnlyTransition()
            feedback.statusBanner.cancelDismiss()
            feedback.achievementsBanner.cancelDismiss()
            cancelMenuButtonReveal()
            leaveGameplaySystemChromeMode()
        }
        // VM pause follows `overlayRoute` alone: any route but `.hidden` keeps it paused.
        .onChange(of: overlay.overlayRoute) { _, route in
            if route == .hidden { (overlay.pauseMenuChild, overlay.pauseCardFoldMemory) = (nil, nil) }
            if route == .paused {
                // Child dismissal and background re-entry reach the card without an open action.
                refreshRuntimeMenuState()
            }
            overlay.updateRuntimeOverlayPause()
            updateRuntimeControllerMenuOwnership()
            overlay.scheduleInactiveQuickMenuResourceRelease(for: route, reduceMotion: reduceMotion)
        }
        .onChange(of: shortcuts.operationActive) { _, _ in
            overlay.updateRuntimeOverlayPause()
            updateRuntimeControllerMenuOwnership()
        }
        .onChange(of: controllerInput?.latestQuickPauseRequest) { _, request in
            guard request != nil,
                  overlay.overlayRoute == .hidden,
                  !shortcuts.operationActive else { return }
            if settings.hapticFeedback { GameEventHaptics.shared.medium.impactOccurred() }
            controllerInput?.setMenuActive(true)
            controllerInput?.playFeedback(.tabTransition)
            refreshRuntimeMenuState()
            overlay.overlayRoute = .paused
        }
        .onChange(of: controllerInput?.latestEmulationShortcutRequest) { _, request in
            guard let request, overlay.overlayRoute == .hidden else { return }
            shortcuts.handleEmulationControllerShortcut(request.shortcut)
        }
        // Not scenePhase: this view lives in SDL's UIKit scene, where it stays .background.
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didEnterBackgroundNotification)) { _ in
            wasBackgrounded = true
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in
            syncFullscreenStateFromWindow()
            // Back from the background, open the pause menu so resuming is deliberate.
            if wasBackgrounded && overlay.overlayRoute == .hidden {
                refreshRuntimeMenuState()
                overlay.overlayRoute = .paused
            }
            wasBackgrounded = false
        }
        .onChange(of: fullScreen) { _, isEnabled in
            applyFullscreenState(isEnabled)
        }
        .onChange(of: session.gameplayLaunchTransition?.id) { previous, current in
            // Keep system chrome still until the launch card's transition has faded away.
            guard previous != nil, current == nil else { return }
            applyFullscreenState(fullScreen)
        }
        .onChange(of: settings.appearance.hideGameplayStatusBar) { _, _ in
            updateGameplayStatusBar(forFullscreen: fullScreen)
        }
        .onChange(of: settings.hideMenuButton) { _, isHidden in
            if menuButtonHidden != isHidden {
                menuButtonHidden = isHidden
            }
            cancelMenuButtonReveal()
        }
        .onChange(of: settings.emulationOnlyModeEnabled) { _, isEnabled in
            if isEnabled {
                enterEmulationOnlyModeIfReady()
            } else {
                session.cancelEmulationOnlyTransition()
            }
        }
        .onChange(of: session.emulationOnlyStartupReady) { _, isReady in
            if isReady {
                enterEmulationOnlyModeIfReady()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
            refreshRuntimeMenuState()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: AppState.emulationOnlyResourcesReleasedNotification
            )
        ) { _ in
            finishEmulationOnlyActivation()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            refreshExternalControllerConnectionState()
            enterEmulationOnlyModeIfReady()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            refreshExternalControllerConnectionState()
        }
        .onReceive(NotificationCenter.default.publisher(for: gameplaySurfaceTapNotification)) { _ in
            revealMenuButtonBriefly()
        }
        .onReceive(NotificationCenter.default.publisher(for: retroAchievementsToastNotification)) { _ in
            feedback.consumePendingRetroAchievementsToast()
        }
        .persistentSystemOverlays(.hidden)
    }

    @ViewBuilder
    private func menuButtonOverlay(isLandscape: Bool) -> some View {
        if !menuButtonHidden || menuButtonRevealed {
            VStack {
                HStack {
                    Spacer()
                    menuButtonCluster()
                }
                .padding(.top, isLandscape ? 8 : 4)
                .padding(.trailing, isLandscape ? 8 : 4)
                Spacer()
            }
        }
    }

    private func menuButton() -> some View {
        Button {
            controllerInput?.setMenuActive(true)
            MenuAudioPackManager.shared.playEvent(.tabTransition)
            if settings.hapticFeedback { GameEventHaptics.shared.light.impactOccurred() }
            refreshRuntimeMenuState()
            overlay.overlayRoute = .paused
        } label: {
            menuButtonLabel
        }
        .accessibilityLabel(settings.localized("Pause Menu"))
        .accessibilityHint(settings.localized("Opens the pause menu"))
    }

    private func menuButtonCluster() -> some View {
        HStack(spacing: 6) {
            if noJITFallbackActive {
                Text(settings.localized("No JIT"))
                    .font(.caption.weight(.semibold))
                    .uiCriticalForegroundStyle()
                    .padding(.horizontal, 9)
                    .padding(.vertical, 6)
                    .background(.black.opacity(0.40), in: Capsule())
                    .accessibilityLabel(settings.localized("No JIT mode"))
            }
            menuButton()
        }
    }

    private func enterEmulationOnlyModeIfReady() {
        session.enterEmulationOnlyModeIfReady(presentation: makeEmulationOnlyPresentation)
    }

    @MainActor
    private func makeEmulationOnlyPresentation() -> EmulationOnlyPresentation {
        let hasExternalController = !GCController.controllers().isEmpty
        let keepsVirtualControls =
            !hasExternalController ||
            (!settings.emulationOnlyDisableVirtualControls && effectiveVirtualPadVisible)
        let keepsQuickMenu = !settings.emulationOnlyDisableQuickMenu
        return EmulationOnlyPresentation(
            showsVirtualControls: keepsVirtualControls,
            showsQuickMenu: keepsQuickMenu,
            padLayoutSnapshot: keepsVirtualControls ? effectivePadLayoutSnapshot : nil,
            padSkinDescriptor: keepsVirtualControls ? effectivePadSkinDescriptor : nil
        )
    }

    @MainActor
    private func finishEmulationOnlyActivation() {
        guard let presentation = session.takeEmulationOnlyActivation() else { return }
        overlay.overlayRoute = .hidden
        if presentation.showsQuickMenu {
            menuButtonHidden = false
        }
        feedback.statusBanner.cancelDismiss()
        feedback.achievementsBanner.cancelDismiss()
        cancelMenuButtonReveal()

        overlay.releaseAllQuickMenuPresentationResources()
        overlay.runtimePadLayoutIdentity = nil
        session.finishEmulationOnlyActivation(presentation)
    }

    private var dynamicCrosshairOverlay: some View {
        DynamicAimCrosshairOverlay(
            settings: dynamicSettings,
            leftRuntime: touchActionSession.left.crosshairState,
            rightRuntime: touchActionSession.right.crosshairState
        )
        .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
    }

    // The render view is non-interactive on iOS 27, so the reveal tap needs a SwiftUI surface.
    private var menuRevealTapCatcher: some View {
        Color.clear
            .contentShape(Rectangle())
            .onTapGesture { revealMenuButtonBriefly() }
            .accessibilityHidden(true)
    }

    @ViewBuilder
    private var menuButtonLabel: some View {
        // An SF Symbol: a bundled PNG loaded fine but was nearly invisible over gameplay.
        Image(systemName: "pause.circle.fill")
            .font(.system(size: 22, weight: .semibold))
            .symbolRenderingMode(.hierarchical)
            .foregroundStyle(.white)
            .frame(width: 30, height: 30)
            .padding(7)
            .background(.black.opacity(0.40), in: Circle())
    }

    private func applyControllerSkin(_ skin: VPadSkinDescriptor) {
        if let identity = overlay.runtimePadLayoutIdentity {
            layoutPresets.setSkin(
                skin.id,
                for: identity,
                using: skinLibrary
            )
        }

        let serial = PadLayoutGameIdentity.normalizedSerial(
            overlay.runtimePadLayoutIdentity?.serial ?? session.gameplayPadSerial
        )
        if !serial.isEmpty {
            layoutPresets.setAutomaticSkin(
                skin.id,
                forSerial: serial,
                using: skinLibrary
            )
        } else {
            // BIOS and serial-less ELF sessions have no per-game key, so keep the Global Default.
            skinLibrary.selectSkin(id: skin.id)
            settings.virtualPadSkin = skin.virtualPadSkin
        }

        // UIKit controls cache their artwork and hit masks, so rebuild the pad now.
        padRebuildToken &+= 1
    }

    private func enterGameplaySystemChromeMode() {
        previousHideHomeIndicator = appState.hideHomeIndicator
        previousHideStatusBar = appState.hideStatusBar
        appState.hideHomeIndicator = true
        UIApplication.shared.isIdleTimerDisabled = true
    }

    private func leaveGameplaySystemChromeMode() {
        if case .menu = appState.currentScreen {
            appState.hideHomeIndicator = false
            appState.hideStatusBar = settings.appearance.hideMenuStatusBar
        } else {
            appState.hideHomeIndicator = previousHideHomeIndicator
            appState.hideStatusBar = previousHideStatusBar
        }
        UIApplication.shared.isIdleTimerDisabled = false
    }

    /// Keeps the SDL window, the status bar policy and the toggle in step, so Full Screen applies at once.
    private func applyFullscreenState(_ enabled: Bool) {
        // SDL's call can change system chrome itself, so it waits while the card zooms.
        if !enabled || session.gameplayLaunchTransition == nil {
            ARMSX2Bridge.setFullScreen(enabled)
        }
        updateGameplayStatusBar(forFullscreen: enabled)
    }

    private func applyInitialFullscreenPreference() {
        menuButtonHidden = settings.hideMenuButton
        // Reconciled on every game entry, so a changed setting applies on the next boot.
        let desired = settings.autoFullscreen
        if fullScreen != desired {
            fullScreen = desired
        }
        applyFullscreenState(desired)
    }

    private func syncFullscreenStateFromWindow() {
        let sdlFullscreen = ARMSX2Bridge.isSDLFullscreen()
        if fullScreen != sdlFullscreen {
            fullScreen = sdlFullscreen
        }
        updateGameplayStatusBar(forFullscreen: sdlFullscreen)
    }

    private func updateGameplayStatusBar(forFullscreen isFullscreen: Bool) {
        let shouldHide = settings.appearance.hideGameplayStatusBar
            && isFullscreen
            && session.gameplayLaunchTransition == nil
        if appState.hideStatusBar != shouldHide {
            appState.hideStatusBar = shouldHide
        }
    }

    private func revealMenuButtonBriefly() {
        guard menuButtonHidden else { return }
        menuRevealTask?.cancel()
        withAnimation(.easeOut(duration: 0.18)) { menuButtonRevealed = true }
        menuRevealTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(4))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.18)) { menuButtonRevealed = false }
        }
    }

    private func cancelMenuButtonReveal() {
        menuRevealTask?.cancel()
        menuRevealTask = nil
        menuButtonRevealed = false
    }

    private func updateRuntimeControllerMenuOwnership() {
        controllerInput?.setMenuActive(overlay.overlayRoute != .hidden)
    }

    private func refreshRuntimeMenuState() {
        let vmRunning = ARMSX2Bridge.isVMRunning()
        let runtimeSettings = vmRunning
            ? ARMSX2Bridge.gameSettingsForCurrentGame()
            : nil
        let identity = runtimePadLayoutIdentity(from: runtimeSettings)
        // Some titles report their settings serial before save states are ready.
        let gameReady = identity != nil || ARMSX2Bridge.hasValidSaveStateGame()
        let noJITActive = ARMSX2Bridge.isNoJITFallbackActive()
        if vmMenuAvailable != vmRunning {
            vmMenuAvailable = vmRunning
        }
        if gameMenuAvailable != gameReady {
            gameMenuAvailable = gameReady
        }
        if noJITFallbackActive != noJITActive {
            noJITFallbackActive = noJITActive
        }
        if let identity {
            AutomaticCustomSkinManager.shared.materializeAcceptedAssignment(
                for: identity
            )
        }
        if overlay.runtimePadLayoutIdentity != identity {
            overlay.runtimePadLayoutIdentity = identity
        }
    }

    private func refreshExternalControllerConnectionState() {
        let connected = !GCController.controllers().isEmpty
        if externalControllerConnected != connected {
            externalControllerConnected = connected
        }
    }

    /// The editor's only dismissal path. An onDisappear could land after Resume and leave a touch-eating overlay.
    private func finishRuntimePadLayoutEditing() {
        guard overlay.overlayRoute == .pausedPresenting(.padLayout) else { return }
        touchActionSession.reset()
        EmulatorBridge.shared.resetVirtualPadAnalogInput()
        padRebuildToken &+= 1
        overlay.overlayRoute = .paused
    }

    /// Over gameplay the toast sits at the top; inside Save States, at the bottom of the panel.
    @ViewBuilder
    private var saveStateUndoOverlay: some View {
        Group {
            if saveStateUndo.item != nil, overlay.overlayRoute != .pausedPresenting(.saveStates) {
                SaveStateUndoToast(
                    undo: saveStateUndo,
                    settings: settings,
                    hint: externalControllerConnected
                        ? ControllerHintLine(parts: [.init(settings.controllerMacroUndoSaveState, settings.localized("Undo"))])
                        : nil,
                    onUndo: shortcuts.undoSaveStateAction
                )
                .padding(.horizontal, 16)
                .padding(.top, 14)
                .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .onChange(of: saveStateUndo.item?.id) { _, id in
            controllerInput?.saveStateUndoPending = id != nil
        }
        .onChange(of: controllerInput?.saveStateUndoRequest) { _, _ in
            shortcuts.undoSaveStateAction()
        }
        .task(id: session.emulationSessionID) {
            SaveStateAutoSave.shared.restart()
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                // Only play counts: not the pause card, the live preview, or time in the library.
                if overlay.overlayRoute == .hidden, !overlay.runtimeOverlayPauseActive, appState.currentScreen == .playing,
                   UIApplication.shared.applicationState == .active, ARMSX2Bridge.isVMRunning() {
                    SaveStateAutoSave.shared.tick(5)
                }
            }
        }
    }

    private var effectiveVirtualPadVisible: Bool {
        if session.isEmulationOnlyMode {
            return session.emulationOnlyPresentation.showsVirtualControls
        }
        return session.userVirtualPadVisible &&
            (!settings.autoHideVirtualPadWhenControllerConnected || !externalControllerConnected) &&
            overlay.overlayRoute != .pausedPresenting(.padLayout)
    }

    private var effectivePadLayoutSnapshot: PadLayoutSnapshot? {
        layoutPresets.effectiveSnapshot(
            for: overlay.runtimePadLayoutIdentity ?? session.gameplayPadIdentity,
            fallbackSerial: session.gameplayPadSerial
        )
    }

    private var activePadLayoutDisplayName: String {
        if let preset = layoutPresets.effectivePreset(
            for: overlay.runtimePadLayoutIdentity ?? session.gameplayPadIdentity,
            fallbackSerial: session.gameplayPadSerial
        ) {
            return preset.displayName
        }
        return settings.localized("Current Layout")
    }

    private var effectivePadSkinDescriptor: VPadSkinDescriptor {
        layoutPresets.effectiveSkinDescriptor(
            for: overlay.runtimePadLayoutIdentity ?? session.gameplayPadIdentity,
            fallbackSerial: session.gameplayPadSerial,
            using: skinLibrary
        )
    }

    private var runtimePadLayoutEditorContext: PadLayoutEditorContext {
        let effectiveIdentity = overlay.runtimePadLayoutIdentity
            ?? session.gameplayPadIdentity
        let preset = layoutPresets.effectivePreset(
            for: effectiveIdentity,
            fallbackSerial: session.gameplayPadSerial
        )
        let editablePresetID = effectiveIdentity.flatMap {
            layoutPresets.presetID(for: $0)
        } ?? (effectiveIdentity == nil ? layoutPresets.globalPresetID : nil)
        return PadLayoutEditorContext(
            presetID: editablePresetID,
            gameIdentity: effectiveIdentity,
            initialSnapshot: preset?.snapshot,
            skinDescriptor: effectivePadSkinDescriptor
        )
    }

    private func runtimePadLayoutIdentity(
        from info: [String: Any]?
    ) -> PadLayoutGameIdentity? {
        guard let info else { return nil }
        return PadLayoutGameIdentity(
            serial: info["serial"] as? String,
            crc: info["crc"] as? String
        )
    }

    private var virtualPadHiddenByController: Bool {
        session.userVirtualPadVisible && settings.autoHideVirtualPadWhenControllerConnected && externalControllerConnected
    }
}
