// RootLifecycleModifier.swift — app, audio and controller lifecycle under RootView
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

struct RootLifecycleModifier: ViewModifier {
    let showBootSplash: Bool
    let menuScreenActive: Bool
    let mainAudioInterfaceActive: Bool
    let menuControllerInput: MenuControllerInputRouter
    let backgroundHost: PersistentMenuBackgroundHost
    let finishBootSplash: @MainActor () -> Void
    let scheduleGameplayControllerHelpIfReady: @MainActor () -> Void
    let handleMenuScreenActiveChange: @MainActor (Bool) -> Void

    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var session = GameSession.shared

    func body(content: Content) -> some View {
        content
        .onAppear {
            settings.appearance.restoreDefaultAppearanceAfterAutomaticNoJITIfNeeded(
                jitAvailable: ARMSX2Bridge.isJITAvailable()
            )
            // The app is hosted by a custom UIKit controller whose status-bar
            // answer comes from AppState, so mirror the active presentation's
            // preference as soon as the root hierarchy appears.
            appState.hideStatusBar = showBootSplash
                ? settings.appearance.hideIntroStatusBar
                : (menuScreenActive && settings.appearance.hideMenuStatusBar)
            MenuAudioPackManager.shared.setAutomaticGameStartupActive(
                session.automaticGameStartupPending
            )
            // SDL's UIKit scene hosts RootView, so scenePhase can read inactive while it is
            // interactive; the process state and UIKit notifications gate audio instead.
            MenuAudioPackManager.shared.setApplicationActive(
                UIApplication.shared.applicationState == .active
            )
            MenuAudioPackManager.shared.setMainInterfaceActive(
                mainAudioInterfaceActive
            )
            menuControllerInput.start()
            if showBootSplash {
                menuControllerInput.setLaunchInputInterceptor {
                    finishBootSplash()
                }
            } else {
                menuControllerInput.setLaunchInputInterceptor(nil)
                MenuAudioPackManager.shared.introDidFinish(playsStartupSound: false)
            }
            menuControllerInput.setMenuActive(menuScreenActive)
            StikDebugLauncher.autoOpenIfNeeded(reason: "app launch")
            ShaderCatalogInstaller.sweepStagedDownloads()
            let textureRoot = TexturePackLibrary.root
            // A pack the app was killed while unpacking can hold 100,000 files.
            Task.detached(priority: .utility) { TexturePackLibrary.sweepStaging(in: textureRoot) }
            scheduleGameplayControllerHelpIfReady()
        }
        .task {
            session.startAutomaticGameIfNeeded()
        }
        .onChange(of: session.automaticGameStartupPending) { _, pending in
            // Update the destination before releasing the startup music gate;
            // controller-owned JIT prompts are not an emulation Quick Menu.
            menuControllerInput.setMenuActive(menuScreenActive)
            MenuAudioPackManager.shared.setMainInterfaceActive(mainAudioInterfaceActive)
            MenuAudioPackManager.shared.setAutomaticGameStartupActive(pending)
        }
        .onChange(of: settings.appearance.hideIntroStatusBar) { _, hideStatusBar in
            guard showBootSplash else { return }
            appState.hideStatusBar = hideStatusBar
        }
        .onChange(of: settings.appearance.hideMenuStatusBar) { _, hideStatusBar in
            guard !showBootSplash, menuScreenActive else { return }
            appState.hideStatusBar = hideStatusBar
        }
        .onChange(of: menuScreenActive) { _, active in
            handleMenuScreenActiveChange(active)
        }
        .onChange(of: mainAudioInterfaceActive) { _, active in
            MenuAudioPackManager.shared.setMainInterfaceActive(active)
        }
        .onChange(of: session.gameplayLaunchTransition?.id) { previous, current in
            if previous != nil, current == nil {
                MenuAudioPackManager.shared.setMainInterfaceActive(
                    mainAudioInterfaceActive
                )
                MenuAudioPackManager.shared.cancelGameLaunchAudio()
                scheduleGameplayControllerHelpIfReady()
            }
        }
        .onChange(of: session.emulationSessionID) { _, _ in
            scheduleGameplayControllerHelpIfReady()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: UIApplication.didBecomeActiveNotification
            )
        ) { _ in
            MenuAudioPackManager.shared.setApplicationActive(true)
            menuControllerInput.applicationDidBecomeActive()
            settings.appearance.restoreDefaultAppearanceAfterAutomaticNoJITIfNeeded(
                jitAvailable: ARMSX2Bridge.isJITAvailable()
            )
            session.resumeAutomaticGameAfterJITIfReady()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: .ARMSX2iOSIdleVMPrewarmResolved
            )
        ) { _ in
            settings.appearance.restoreDefaultAppearanceAfterAutomaticNoJITIfNeeded(
                jitAvailable: ARMSX2Bridge.isJITAvailable()
            )
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: .ARMSX2iOSVMDidShutdown
            )
        ) { _ in
            MenuAudioPackManager.shared.emulationAudioDidStop()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: UIApplication.willResignActiveNotification
            )
        ) { _ in
            MenuAudioPackManager.shared.setApplicationActive(false)
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: AppState.releaseMenuBackgroundResourcesNotification
            )
        ) { _ in
            backgroundHost.release()
            // These stores outlive the tabs, so do not rely on a tab's onDisappear to free them.
            GameLibraryRuntimeResources.releaseForGameplay()
            PatchStore.shared.releasePresentationResources()
        }
        .modifier(LaunchReportOffer(
            menuScreenActive: menuScreenActive,
            showBootSplash: showBootSplash,
            menuControllerInput: menuControllerInput
        ))
    }
}
