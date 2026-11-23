// RootView.swift — Root view switching between menu and game
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import GameController

private struct RootControllerAlertCommandListener: View {
    let controllerInput: MenuControllerInputRouter
    let onCommand: (MenuControllerCommand) -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput.latestEvent) { _, event in
                guard let event,
                      event.captureOwner
                        == MenuControllerNavigationCaptureOwner.rootAlert else {
                    return
                }
                onCommand(event.command)
            }
    }
}

/// Shows RootView's prompts in a window above the main one. The window lives only
/// while a prompt is up and never becomes key, so controller events still reach
/// the main window's event host.
private struct RootAlertWindow: UIViewRepresentable {
    let content: AnyView?

    func makeUIView(context: Context) -> RootAlertWindowAnchor {
        RootAlertWindowAnchor()
    }

    func updateUIView(_ anchor: RootAlertWindowAnchor, context: Context) {
        anchor.show(content.map { AnyView($0.environment(\.self, context.environment)) })
    }

    static func dismantleUIView(_ anchor: RootAlertWindowAnchor, coordinator: ()) {
        anchor.show(nil)
    }
}

private final class RootAlertWindowAnchor: UIView {
    private var alertWindow: UIWindow?
    private var content: AnyView?

    func show(_ content: AnyView?) {
        self.content = content
        update()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        update()
    }

    private func update() {
        guard let content else {
            alertWindow?.isHidden = true
            alertWindow = nil
            return
        }
        if let host = alertWindow?.rootViewController as? ARMSX2HostingController<AnyView> {
            host.rootView = content
            return
        }
        guard let scene = window?.windowScene else { return }
        let host = ARMSX2HostingController(rootView: content)
        host.view.backgroundColor = .clear
        host.view.accessibilityViewIsModal = true
        let alertWindow = UIWindow(windowScene: scene)
        alertWindow.windowLevel = .alert
        alertWindow.rootViewController = host
        alertWindow.isHidden = false
        self.alertWindow = alertWindow
    }
}

private enum RootControllerAlertKind: Equatable {
    case fileImport
    case bios
    case restartVM
    case jitInitial
    case jitFinal
    case navigationMode(MenuNavigationInputMode)
    case libraryExport
    case prompt(UUID)

    /// Shown over gameplay too, not only on the menu screens.
    var showsOverGameplay: Bool {
        switch self {
        case .navigationMode, .prompt: true
        default: false
        }
    }
}

struct RootView: View {
    @State private var appState = AppState.shared
    @State private var session = GameSession.shared
    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBootSplash = !GameSession.shared.automaticGameStartupPending
    @State private var showNoJITFinalConfirmation = false
    @State private var rootControllerAlertSelection = 0
    @State private var menuControllerInput = MenuControllerInputRouter()
    @State private var bootSplashInputReleaseTask: Task<Void, Never>?
    @State private var presentedGameplayHelpSessionID: UUID?
    // Theme/background renderer changes are allowed to rebuild MenuTabView,
    // but they must never reset the selected tab or discard a pushed Settings
    // destination. Keep both routing values above that reactive subtree.
    @State private var selectedMenuTab = 0
    @State private var settingsMenuNavigationPath: [SettingsPane] = []
    @StateObject private var gameplayHelpBanner =
        TransientBannerController<UUID>(defaultDisplayDuration: 7.0)
    // The background layer observes renderer state directly. Keeping only the
    // host reference here prevents snapshot/visibility publications from
    // invalidating the complete menu hierarchy.
    @State private var backgroundHost = PersistentMenuBackgroundHost()

    private var menuScreenActive: Bool {
        if case .menu = appState.currentScreen {
            return true
        }
        return false
    }

    private var mainAudioInterfaceActive: Bool {
        menuPresentationActive || session.gameplayLaunchTransition != nil
    }

    private var menuPresentationActive: Bool {
        // Keep library layout, covers and dynamic background
        // rendering out of the intro's first-frame presentation transaction.
        menuScreenActive && !showBootSplash && !session.automaticGameStartupPending
    }

    var body: some View {
        lifecycleContent
            .modifier(RootAppearanceEnvironment())
    }

    private var baseContent: some View {
        ZStack {
            // Menu backdrop only: over gameplay it shows near white in the top safe area
            // portrait leaves for the Dynamic Island. Same condition as the layer below so
            // the two cannot drift apart.
            Color(uiColor: .systemGroupedBackground)
                .ignoresSafeArea()
                .opacity(
                    menuPresentationActive ||
                    session.gameplayLaunchBackgroundVisible ? 1 : 0
                )

            // Do not construct the library or its renderer for direct startup.
            // The same host still survives ordinary tab changes.
            if menuPresentationActive || session.gameplayLaunchTransition != nil {
                PersistentMenuBackgroundLayer(host: backgroundHost)
                    .opacity(
                        menuPresentationActive ||
                        session.gameplayLaunchBackgroundVisible ? 1 : 0
                    )
                    .zIndex(
                        appState.currentScreen == .playing &&
                        session.gameplayLaunchTransition != nil ? 80 : 0
                    )
            }

            // Keep the menu hierarchy alive for the launch transition so the
            // library fades behind the live SwiftUI card instead of vanishing
            // on the same update that starts the VM.
            if menuPresentationActive || session.gameplayLaunchTransition != nil {
                MenuTabView(
                    backgroundHost: backgroundHost,
                    controllerInput: menuControllerInput,
                    selectedTab: $selectedMenuTab,
                    settingsNavigationPath: $settingsMenuNavigationPath
                )
                    .opacity(menuScreenActive && !ThemeGalleryStore.shared.isPreviewing ? 1 : 0)
                    .animation(
                        .easeOut(duration: 0.34),
                        value: menuScreenActive
                    )
                    .allowsHitTesting(menuScreenActive)
                    .accessibilityHidden(!menuScreenActive)
                    .zIndex(
                        session.gameplayLaunchTransition == nil ? 1 : 85
                    )
            }

            if case .playing = appState.currentScreen {
                if session.isEmulationOnlyMode &&
                    !session.emulationOnlyPresentation.showsQuickMenu {
                    EmulationOnlyGameView()
                } else {
                    GameScreenView(
                        showsGameplayControllerShortcutHelp:
                            gameplayHelpBanner.content != nil
                                && menuControllerInput.hasConnectedController
                    )
                }
            }

            if let transition = session.gameplayLaunchTransition {
                GameplayLaunchOverlay(
                    transition: transition,
                    onBeginAudioFade: { duration in
                        MenuAudioPackManager.shared
                            .finishGameLaunchWithBackgroundFade(
                                duration: duration
                            )
                    },
                    onRevealGameplay: {
                        session.revealGameplayLaunchControls()
                    },
                    onComplete: {
                        session.completeGameplayLaunchTransition(id: transition.id)
                    }
                )
                .zIndex(90)
            }

            if gameplayHelpBanner.content != nil,
               appState.currentScreen == .playing,
               session.gameplayLaunchTransition == nil,
               !showBootSplash,
               session.isEmulationOnlyMode,
               menuControllerInput.hasConnectedController,
               !session.emulationOnlyPresentation.showsQuickMenu {
                GameplayControllerShortcutHelpOverlay(
                    settings: settings,
                    controllerInput: menuControllerInput
                )
                    .transition(.opacity.combined(with: .scale(scale: 0.96)))
                    .zIndex(75)
            }

            if showBootSplash {
                BootSplashView {
                    finishBootSplash()
                }
                .transition(.opacity)
                .zIndex(100)
            }

            if session.automaticGameStartupPending {
                ZStack {
                    Color.black.ignoresSafeArea()
                    ProgressView(settings.localized("Starting Last Game…"))
                        .tint(settings.appearance.controllerNavigationAccentColor)
                }
                .zIndex(100)
            }
        }
        // A sheet from the pause menu covers every layer of this view, so the
        // prompts get a window of their own.
        .background {
            RootAlertWindow(content: rootControllerAlert)
        }
        .environment(\.locale, settings.appLanguage == .system ? .autoupdatingCurrent : Locale(identifier: settings.appLanguage.bcp47Code))
        .environment(\.layoutDirection, settings.localizedLayoutDirection)
        .environment(\.clearLiquidGlassUIEnabled, settings.appearance.clearLiquidGlassUI)
        .environment(\.menuControllerInputRouter, menuControllerInput)
#if !targetEnvironment(macCatalyst)
        .background {
            NativeMenuTouchInputObserver(
                observesTouchActions:
                    !menuControllerInput.isControllerNavigationEnabled,
                blocksUnderlyingTouches:
                    menuControllerInput.isMenuActive
                        && menuControllerInput.isControllerNavigationEnabled
                        && menuControllerInput.pendingNavigationModeSwitchRequest == nil
                        && frameRates.asksBeforeTouchNavigation,
                onTouch: {
                    menuControllerInput.noteTouchInput()
                },
                onTap: {
                    MenuAudioPackManager.shared.playTouchNavigation()
                    menuControllerInput.playTouchHaptics(.activate)
                },
                onBack: {
                    MenuAudioPackManager.shared.playEvent(.return)
                    menuControllerInput.playTouchHaptics(.back)
                },
                onToggle: { isOn in
                    MenuAudioPackManager.shared.playTouchToggle(isOn: isOn)
                    menuControllerInput.playTouchHaptics(.toggle(isOn: isOn))
                },
                onContextMenu: {
                    MenuAudioPackManager.shared.playTouchContextMenu()
                    menuControllerInput.playTouchHaptics(.contextMenu)
                },
                onEmptySpaceLongPress: { step in
                    menuControllerInput.requestThemePresetChangeFromTouch(
                        step: step
                    )
                }
            )
            .frame(width: 0, height: 0)
        }
#endif
        .statusBarHidden(
            showBootSplash
                ? settings.appearance.hideIntroStatusBar
                : (menuScreenActive && settings.appearance.hideMenuStatusBar)
        )
    }

    private var lifecycleContent: some View {
        baseContent
        .modifier(RootLifecycleModifier(
            showBootSplash: showBootSplash,
            menuScreenActive: menuScreenActive,
            mainAudioInterfaceActive: mainAudioInterfaceActive,
            menuControllerInput: menuControllerInput,
            backgroundHost: backgroundHost,
            finishBootSplash: finishBootSplash,
            scheduleGameplayControllerHelpIfReady: scheduleGameplayControllerHelpIfReady,
            handleMenuScreenActiveChange: handleMenuScreenActiveChange
        ))
        .onChange(of: activeControllerAlertKind, initial: true) { previous, kind in
            rootControllerAlertSelection = 0
            updateRootAlertNavigationCapture(isPresented: kind != nil)
            guard let kind, kind != previous else { return }
            // The sound belongs to the modal transition, not a hidden row under it.
            // Import handlers often play the toast too; the audio coalescer drops repeats.
            switch kind {
            case .fileImport, .bios, .restartVM, .libraryExport:
                MenuAudioPackManager.shared.playEvent(.uiToast)
            case .jitInitial, .jitFinal:
                break
            case .navigationMode, .prompt:
                MenuAudioPackManager.shared.playEvent(.uiToast)
            }
        }
        .onChange(
            of: session.pendingJITGameBoot != nil,
            initial: true
        ) { _, isPresented in
            guard isPresented else { return }
            presentNoJITWarningEffects()
        }
        .onDisappear {
            bootSplashInputReleaseTask?.cancel()
            bootSplashInputReleaseTask = nil
            MenuAudioPackManager.shared.setMainInterfaceActive(false)
            MenuAudioPackManager.shared.setApplicationActive(false)
            menuControllerInput.stop()
            gameplayHelpBanner.clear()
        }
    }

    @MainActor
    private func finishBootSplash() {
        guard showBootSplash else { return }
        NotificationCenter.default.post(
            name: BootSplashView.stopPlaybackForAudioHandoff,
            object: nil
        )
        withAnimation(.easeOut(duration: 0.2)) {
            showBootSplash = false
        }
        Task { @MainActor in
            // Keep the status bar out of the splash fade, then apply the menu
            // preference once the intro has actually left the hierarchy.
            try? await Task.sleep(for: .milliseconds(220))
            guard !Task.isCancelled,
                  !showBootSplash,
                  appState.currentScreen == .menu else { return }
            appState.hideStatusBar = settings.appearance.hideMenuStatusBar
        }
        scheduleGameplayControllerHelpIfReady()

        bootSplashInputReleaseTask?.cancel()
        bootSplashInputReleaseTask = Task { @MainActor in
            // Keep consuming the press which dismissed the movie until UIKit
            // has completed the splash transition. Otherwise the same Cross
            // edge can reach the already-mounted game library underneath.
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            menuControllerInput.setLaunchInputInterceptor(nil)
            bootSplashInputReleaseTask = nil
        }

        // The opacity transition retains the boot AVPlayer for its full
        // duration. Wait until it is gone before claiming the audio session.
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 240_000_000)
            guard !Task.isCancelled else { return }
            MenuAudioPackManager.shared.introDidFinish()
        }
    }

    private var rootControllerAlert: AnyView? {
        guard let kind = activeControllerAlertKind,
              menuScreenActive || kind.showsOverGameplay else { return nil }
        return AnyView(
            ControllerNavigationAlert(
                title: controllerAlertTitle(for: kind),
                message: controllerAlertMessage(for: kind),
                actions: controllerAlertActions(for: kind),
                selectedIndex: rootControllerAlertSelection,
                onSelect: { index in
                    selectControllerAlertAction(index, kind: kind)
                },
                onDismiss: {
                    dismissControllerAlert(kind)
                }
            )
            .overlay {
                RootControllerAlertCommandListener(
                    controllerInput: menuControllerInput,
                    onCommand: { command in
                        handleControllerAlertCommand(command, kind: kind)
                    }
                )
            }
        )
    }

    private var activeControllerAlertKind: RootControllerAlertKind? {
        if let request = menuControllerInput.pendingNavigationModeSwitchRequest {
            return .navigationMode(request.mode)
        }
        if let request = ControllerPrompt.shared.request {
            return .prompt(request.id)
        }
        if session.pendingJITGameBoot != nil {
            return showNoJITFinalConfirmation ? .jitFinal : .jitInitial
        }
        if session.pendingRestartGame != nil { return .restartVM }
        if fileImporter.showImportAlert { return .fileImport }
        if session.bootDisclaimerMessage != nil { return .bios }
        if appState.pendingLibraryExport != nil { return .libraryExport }
        return nil
    }

    private func handleMenuScreenActiveChange(_ active: Bool) {
        menuControllerInput.setMenuActive(active)
        guard active else {
            scheduleGameplayControllerHelpIfReady()
            return
        }
        if !showBootSplash {
            appState.hideStatusBar = settings.appearance.hideMenuStatusBar
        }
        gameplayHelpBanner.clear()
        updateRootAlertNavigationCapture(
            isPresented: activeControllerAlertKind != nil
        )
    }

    private func scheduleGameplayControllerHelpIfReady() {
        Task { @MainActor in
            // Coalesce AppState's session, screen, and launch-transition
            // publications before deciding whether gameplay is actually visible.
            await Task.yield()
            guard !showBootSplash,
                  appState.currentScreen == .playing,
                  session.gameplayLaunchTransition == nil,
                  menuControllerInput.hasConnectedController,
                  GCController.controllers().contains(where: {
                      $0.extendedGamepad != nil
                  }),
                  let sessionID = session.emulationSessionID,
                  sessionID != presentedGameplayHelpSessionID else {
                return
            }
            presentedGameplayHelpSessionID = sessionID
            gameplayHelpBanner.present(sessionID, displayDuration: 7.0)
        }
    }

    private func updateRootAlertNavigationCapture(isPresented: Bool) {
        if isPresented {
            // Import completion follows a touch-owned document picker. Restore
            // controller presentation before the modal claims its input so OK
            // is visibly selected as soon as the alert appears.
            menuControllerInput.claimPresentedNavigationFocus()
        }
        menuControllerInput.setNavigationCaptured(
            isPresented,
            owner: MenuControllerNavigationCaptureOwner.rootAlert,
            priority: 1_000
        )
    }

    private func controllerAlertTitle(
        for kind: RootControllerAlertKind
    ) -> String {
        switch kind {
        case .fileImport: settings.localized("File Import")
        case .bios: settings.localized(session.bootDisclaimerTitle)
        case .restartVM: settings.localized("Restart VM?")
        case .jitInitial, .jitFinal:
            "⚠️ \(settings.localized("JIT Access Not Detected"))"
        case .navigationMode(.controller):
            settings.localized("Use Controller Navigation?")
        case .navigationMode(.touch):
            settings.localized("Use Touch Navigation?")
        case .libraryExport:
            settings.localized("Send your game library?")
        case .prompt:
            ControllerPrompt.shared.request?.title ?? ""
        }
    }

    private func controllerAlertMessage(
        for kind: RootControllerAlertKind
    ) -> String {
        switch kind {
        case .fileImport:
            settings.localized(fileImporter.lastImportMessage ?? "")
        case .bios:
            settings.localized(
                session.bootDisclaimerMessage ?? "BIOS not yet imported."
            )
        case .restartVM:
            "\(settings.localized("VM is currently running."))\n\(settings.localized("Shut down and start")) \(((session.pendingRestartGame ?? "") as NSString).lastPathComponent)?"
        case .jitInitial:
            settings.localized(
                "JIT access is not available. Match the StikDebug script to the JIT Script setting in Emulator settings."
            )
        case .jitFinal:
            settings.localized(
                "Are you sure you want to continue?\nJIT access is required for 60 FPS.\n\nEXPECT TOO LOW PERFORMANCE.\nPROCEED AT YOUR OWN RISK!"
            )
        case .navigationMode(.controller):
            settings.localized(
                "Controller navigation will replace touch navigation and load controller focus only for the active screen."
            )
        case .navigationMode(.touch):
            settings.localized(
                "Touch navigation will release controller focus and unload its inactive focus and scroll resources."
            )
        case .libraryExport:
            String(
                format: settings.localized(
                    "A link is asking for the name, serial and CRC of every game you have.\nSend that list to %@?"
                ),
                URL(string: appState.pendingLibraryExport ?? "")?.host
                    ?? appState.pendingLibraryExport ?? ""
            )
        case .prompt:
            ControllerPrompt.shared.request?.message ?? ""
        }
    }

    private func controllerAlertActions(
        for kind: RootControllerAlertKind
    ) -> [ControllerNavigationAlertAction] {
        switch kind {
        case .prompt:
            (ControllerPrompt.shared.request?.actions ?? []).enumerated().map {
                .init(
                    id: "prompt.\($0.offset)",
                    title: $0.element.title,
                    isDestructive: $0.element.isDestructive
                )
            }
        case .fileImport, .bios:
            [.init(id: "ok", title: settings.localized("OK"))]
        case .restartVM:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(
                    id: "restart",
                    title: settings.localized("Restart"),
                    isDestructive: true
                ),
            ]
        case .jitInitial, .jitFinal:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(
                    id: "continue",
                    title: settings.localized("Continue"),
                    isDestructive: true
                ),
            ]
        case .navigationMode:
            [
                .init(id: "ok", title: settings.localized("OK")),
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(
                    id: "do-not-ask-again",
                    title: settings.localized("Do Not Ask Again")
                ),
            ]
        case .libraryExport:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(
                    id: "send",
                    title: settings.localized("Send"),
                    isDestructive: true
                ),
            ]
        }
    }

    private func handleControllerAlertCommand(
        _ command: MenuControllerCommand,
        kind: RootControllerAlertKind
    ) {
        guard activeControllerAlertKind == kind else { return }
        switch command {
        case .activate:
            selectControllerAlertAction(
                rootControllerAlertSelection,
                kind: kind
            )
        case .back:
            dismissControllerAlert(kind)
        case .up, .upLeft, .left, .downLeft:
            moveControllerAlertSelection(by: -1, kind: kind, command: command)
        case .upRight, .right, .downRight, .down:
            moveControllerAlertSelection(by: 1, kind: kind, command: command)
        case .toggleFavorite, .showContextMenu,
             .previousTab, .nextTab:
            menuControllerInput.playFeedback(.boundary)
        }
    }

    private func moveControllerAlertSelection(
        by offset: Int,
        kind: RootControllerAlertKind,
        command: MenuControllerCommand
    ) {
        let actions = controllerAlertActions(for: kind)
        guard !actions.isEmpty else { return }
        let next = min(
            max(rootControllerAlertSelection + offset, actions.startIndex),
            actions.index(before: actions.endIndex)
        )
        guard next != rootControllerAlertSelection else {
            menuControllerInput.playFeedback(.boundary)
            return
        }
        rootControllerAlertSelection = next
        menuControllerInput.playFeedback(.move(command))
    }

    private func selectControllerAlertAction(
        _ index: Int,
        kind: RootControllerAlertKind
    ) {
        guard controllerAlertActions(for: kind).indices.contains(index),
              activeControllerAlertKind == kind else { return }
        switch kind {
        case .fileImport, .bios:
            closeControllerAlert(kind, feedback: .activate)
        case .restartVM:
            if index == 0 {
                session.pendingRestartGame = nil
                menuControllerInput.playFeedback(.back)
            } else {
                if let game = session.pendingRestartGame {
                    session.shutdownAndBoot(isoName: game)
                }
                session.pendingRestartGame = nil
                menuControllerInput.playFeedback(.activate)
            }
        case .jitInitial:
            if index == 0 {
                session.cancelPendingJITGameBoot()
                menuControllerInput.playFeedback(.back)
            } else {
                rootControllerAlertSelection = 0
                showNoJITFinalConfirmation = true
                menuControllerInput.playFeedback(.activate)
            }
        case .jitFinal:
            showNoJITFinalConfirmation = false
            if index == 0 {
                session.cancelPendingJITGameBoot()
                menuControllerInput.playFeedback(.back)
            } else {
                session.continuePendingJITGameBoot()
                menuControllerInput.playFeedback(.activate)
            }
        case .navigationMode(let mode):
            if index == 1 {
                menuControllerInput.cancelNavigationModeSwitch(mode)
                menuControllerInput.playFeedback(.back)
            } else {
                menuControllerInput.acceptNavigationModeSwitch(
                    mode,
                    doNotAskAgain: index == 2
                )
                menuControllerInput.playFeedback(.activate)
                if mode == .controller {
                    restoreControllerFocusAfterModeHandoff()
                }
            }
        case .libraryExport:
            if index == 1, let callback = appState.pendingLibraryExport {
                ARMSX2DeepLinkHandler.performLibraryExport(callback: callback)
            }
            appState.pendingLibraryExport = nil
            menuControllerInput.playFeedback(index == 1 ? .activate : .back)
        case .prompt:
            let cancels = ControllerPrompt.shared.request?.actions[index].isCancel == true
            ControllerPrompt.shared.answer(index)
            menuControllerInput.playFeedback(cancels ? .back : .activate)
        }
    }

    private func restoreControllerFocusAfterModeHandoff() {
        Task { @MainActor in
            // Controller probes are intentionally absent while touch owns the
            // screen. Give SwiftUI one mounting turn, then use the router's
            // mount-aware entry request rather than polling for rows.
            await Task.yield()
            guard menuControllerInput.isControllerNavigationEnabled else {
                return
            }
            if selectedMenuTab == 0 {
                menuControllerInput.requestLibraryEntry(preferLast: true)
            } else {
                _ = menuControllerInput.requestNavigationSessionEntry(
                    preferLast: true,
                    matchingScopePrefix: selectedMenuTab == 1
                        ? "menu.bios"
                        : "settings."
                )
            }
        }
    }

    private func dismissControllerAlert(_ kind: RootControllerAlertKind) {
        guard activeControllerAlertKind == kind else { return }
        switch kind {
        case .fileImport, .bios, .restartVM, .libraryExport:
            closeControllerAlert(kind, feedback: .back)
        case .jitInitial, .jitFinal:
            showNoJITFinalConfirmation = false
            session.cancelPendingJITGameBoot()
            menuControllerInput.playFeedback(.back)
        case .navigationMode(let mode):
            menuControllerInput.cancelNavigationModeSwitch(mode)
            menuControllerInput.playFeedback(.back)
        case .prompt:
            ControllerPrompt.shared.answer(nil)
            menuControllerInput.playFeedback(.back)
        }
    }

    private func closeControllerAlert(
        _ kind: RootControllerAlertKind,
        feedback: MenuControllerFeedback
    ) {
        guard activeControllerAlertKind == kind else { return }
        switch kind {
        case .fileImport:
            fileImporter.showImportAlert = false
        case .bios:
            session.bootDisclaimerMessage = nil
        case .restartVM:
            session.pendingRestartGame = nil
        case .jitInitial, .jitFinal:
            showNoJITFinalConfirmation = false
            session.cancelPendingJITGameBoot()
        case .navigationMode(let mode):
            menuControllerInput.cancelNavigationModeSwitch(mode)
        case .libraryExport:
            appState.pendingLibraryExport = nil
        case .prompt:
            ControllerPrompt.shared.answer(nil)
        }
        menuControllerInput.playFeedback(feedback)
    }

    private func presentNoJITWarningEffects() {
        MenuAudioPackManager.shared.playEvent(.noJIT)
        settings.appearance.applyAutomaticNoJITAppearanceIfEligible()
    }
}
