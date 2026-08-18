// AppState.swift — App screen state management
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

enum GameplayLaunchCardStyle: Equatable {
    case list
    case grid
    case coverFlow
}

/// Lightweight content used to rebuild the selected library card in SwiftUI.
/// Only the already-decoded cover is retained; the menu hierarchy and full
/// window are never rasterized for the transition.
struct GameplayLaunchTransition: Identifiable {
    let id = UUID()
    let sourceFrame: CGRect
    let cornerRadius: CGFloat
    let style: GameplayLaunchCardStyle
    let gameName: String
    let title: String
    let detail: String
    let coverImage: UIImage?
    let coverSize: CGSize
    let isFavorite: Bool
}

struct PendingJITGameBoot {
    let isoName: String
    let launchTransition: GameplayLaunchTransition?
    let requiresShutdown: Bool
}

struct EmulationOnlyPresentation: Equatable {
    var showsVirtualControls = false
    var showsQuickMenu = false
    var padLayoutSnapshot: PadLayoutSnapshot?
    var padSkinDescriptor: VPadSkinDescriptor?

    static let minimal = EmulationOnlyPresentation()
}

@Observable
final class AppState: @unchecked Sendable {
    static let shared = AppState()
    static let systemChromeNeedsUpdateNotification = Notification.Name("ARMSX2iOSSystemChromeNeedsUpdate")
    static let releaseMenuBackgroundResourcesNotification = Notification.Name("ARMSX2iOSReleaseMenuBackgroundResources")
    static let emulationOnlyStartupReadyNotification = Notification.Name("ARMSX2iOSEmulationOnlyStartupReady")
    static let emulationOnlyResourcesReleasedNotification = Notification.Name("ARMSX2iOSEmulationOnlyResourcesReleased")

    enum Screen {
        case menu
        case playing
    }

    var currentScreen: Screen = .menu
    var selectedTab: Int = 0
    var runningGameName: String? = nil
    var bootDisclaimerMessage: String?
    var pendingJITGameBoot: PendingJITGameBoot?
    var gameplayLaunchTransition: GameplayLaunchTransition?
    var gameplayLaunchControlsVisible = true
    var gameplayLaunchBackgroundVisible = false
    var isEmulationOnlyMode: Bool = false
    var emulationOnlyPresentation = EmulationOnlyPresentation.minimal
    private(set) var emulationOnlyStartupReady: Bool = false
    var hideStatusBar: Bool = false {
        didSet {
            if oldValue != hideStatusBar {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }
    var hideHomeIndicator: Bool = false {
        didSet {
            if oldValue != hideHomeIndicator {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }

    @ObservationIgnored private var pendingBootAction: (() -> Void)?
    @ObservationIgnored private var shutdownObserver: NSObjectProtocol?

    @ObservationIgnored private var autoBootObserver: NSObjectProtocol?
    @ObservationIgnored private var emulationOnlyStartupReadyObserver: NSObjectProtocol?

    private init() {
        shutdownObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("ARMSX2iOSVMDidShutdown"),
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.cancelGameplayLaunchTransition()
            self?.isEmulationOnlyMode = false
            self?.emulationOnlyPresentation = .minimal
            self?.emulationOnlyStartupReady = false
            if let action = self?.pendingBootAction {
                // A restart is one continuous menu session. Keep the current
                // running identity until bootGame/bootBIOS replaces it so the
                // Now Running glass card never disappears between VMs.
                self?.pendingBootAction = nil
                action()
            } else {
                // No pending reboot — return to menu (VM crash / normal shutdown)
                self?.runningGameName = nil
                self?.restoreMenuSystemChrome()
                self?.currentScreen = .menu
            }
        }

        // Synchronize SwiftUI state when the native auto-boot path starts a VM.
        autoBootObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("ARMSX2iOSAutoBootDidStart"),
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.isEmulationOnlyMode = false
            self?.emulationOnlyPresentation = .minimal
            self?.emulationOnlyStartupReady = false
            self?.cancelGameplayLaunchTransition()
            self?.releaseMenuBackgroundResourcesForGameplay()
            self?.runningGameName = "AutoBoot"
            self?.currentScreen = .playing
        }

        emulationOnlyStartupReadyObserver = NotificationCenter.default.addObserver(
            forName: Self.emulationOnlyStartupReadyNotification,
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.emulationOnlyStartupReady = true
        }
    }

    @discardableResult
    func bootGame(
        isoName: String,
        launchTransition: GameplayLaunchTransition? = nil
    ) -> Bool {
        guard requireBootableBIOS() else { return false }
        guard ARMSX2Bridge.isJITAvailable() else {
            pendingJITGameBoot = PendingJITGameBoot(
                isoName: isoName,
                launchTransition: launchTransition,
                requiresShutdown: false
            )
            return false
        }

        return performBootGame(
            isoName: isoName,
            launchTransition: launchTransition
        )
    }

    @discardableResult
    private func performBootGame(
        isoName: String,
        launchTransition: GameplayLaunchTransition?
    ) -> Bool {
        pendingJITGameBoot = nil
        isEmulationOnlyMode = false
        emulationOnlyPresentation = .minimal
        emulationOnlyStartupReady = false
        gameplayLaunchTransition = launchTransition
        gameplayLaunchControlsVisible = launchTransition == nil
        gameplayLaunchBackgroundVisible = launchTransition != nil
        if launchTransition == nil {
            releaseMenuBackgroundResourcesForGameplay()
        }
        Task { @MainActor in
            StikDebugLauncher.autoOpenIfNeeded(reason: "game boot")
        }
        // Before, not after: the boot reads the per-game file, stale absolute and all.
        PerGameShaderSelection.repair(forISO: isoName)
        ARMSX2Bridge.bootISO(isoName)
        ARMSX2Bridge.prepareGameRenderViewForCurrentRenderer()
        runningGameName = isoName
        currentScreen = .playing
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            ARMSX2Bridge.requestVMBoot()
        }
        return true
    }

    @discardableResult
    func bootBIOSOnly() -> Bool {
        guard requireBootableBIOS() else { return false }

        isEmulationOnlyMode = false
        emulationOnlyPresentation = .minimal
        emulationOnlyStartupReady = false
        cancelGameplayLaunchTransition()
        releaseMenuBackgroundResourcesForGameplay()
        Task { @MainActor in
            StikDebugLauncher.autoOpenIfNeeded(reason: "BIOS boot")
        }
        ARMSX2Bridge.setINIString("GameISO", key: "BootISO", value: "")
        ARMSX2Bridge.prepareGameRenderViewForCurrentRenderer()
        runningGameName = "BIOS"
        currentScreen = .playing
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            ARMSX2Bridge.requestVMBoot()
        }
        return true
    }

    /// Drops a queued reboot, so stopping mid Reset ROM quits instead of booting the game again.
    func cancelPendingBoot() {
        pendingBootAction = nil
    }

    func returnToMenu() {
        if ARMSX2Bridge.isVMRunning() {
            ARMSX2Bridge.setVMPaused(true)
        }
        cancelGameplayLaunchTransition()
        restoreMenuSystemChrome()
        currentScreen = .menu
        // Notify the UIKit host to restore the menu presentation.
        NotificationCenter.default.post(name: NSNotification.Name("ARMSX2iOSReturnToMenu"), object: nil)
    }

    func returnToGame() {
        if runningGameName != nil {
            cancelGameplayLaunchTransition()
            releaseMenuBackgroundResourcesForGameplay()
            // Make the hosting surface transparent before revealing Metal output.
            NotificationCenter.default.post(name: NSNotification.Name("ARMSX2iOSEnterGameScreen"), object: nil)
            currentScreen = .playing
            ARMSX2Bridge.setVMPaused(false)
        }
    }

    func shutdownAndBoot(
        isoName: String,
        launchTransition: GameplayLaunchTransition? = nil
    ) {
        guard requireBootableBIOS() else { return }
        guard ARMSX2Bridge.isJITAvailable() else {
            pendingJITGameBoot = PendingJITGameBoot(
                isoName: isoName,
                launchTransition: launchTransition,
                requiresShutdown: true
            )
            return
        }
        performShutdownAndBoot(
            isoName: isoName,
            launchTransition: launchTransition
        )
    }

    func continuePendingJITGameBoot() {
        guard let request = pendingJITGameBoot else { return }
        pendingJITGameBoot = nil
        guard requireBootableBIOS() else { return }

        if request.requiresShutdown {
            performShutdownAndBoot(
                isoName: request.isoName,
                launchTransition: request.launchTransition
            )
        } else {
            performBootGame(
                isoName: request.isoName,
                launchTransition: request.launchTransition
            )
        }
    }

    func cancelPendingJITGameBoot() {
        pendingJITGameBoot = nil
    }

    private func performShutdownAndBoot(
        isoName: String,
        launchTransition: GameplayLaunchTransition?
    ) {
        pendingBootAction = { [weak self] in
            self?.performBootGame(
                isoName: isoName,
                launchTransition: launchTransition
            )
        }
        ARMSX2Bridge.requestVMShutdown()
    }

    func shutdownAndBootBIOS() {
        guard requireBootableBIOS() else { return }
        pendingBootAction = { [weak self] in
            self?.bootBIOSOnly()
        }
        ARMSX2Bridge.requestVMShutdown()
    }

    func resetCurrentVM() {
        guard let runningGameName else { return }

        if runningGameName == "BIOS" {
            shutdownAndBootBIOS()
        } else {
            shutdownAndBoot(isoName: runningGameName)
        }
    }

    /// Records the reduced presentation for the active VM. The state deliberately
    /// survives a temporary return to the menu and is reset only by VM shutdown or
    /// the start of a new VM.
    func enterEmulationOnlyMode(presentation: EmulationOnlyPresentation) {
        guard emulationOnlyStartupReady else { return }
        emulationOnlyPresentation = presentation
        isEmulationOnlyMode = true
    }

    func revealGameplayLaunchControls() {
        gameplayLaunchControlsVisible = true
        gameplayLaunchBackgroundVisible = false
    }

    func completeGameplayLaunchTransition(id: UUID) {
        guard gameplayLaunchTransition?.id == id else { return }
        gameplayLaunchTransition = nil
        gameplayLaunchControlsVisible = true
        gameplayLaunchBackgroundVisible = false
        releaseMenuBackgroundResourcesForGameplay()
    }

    func cancelGameplayLaunchTransition() {
        gameplayLaunchTransition = nil
        gameplayLaunchControlsVisible = true
        gameplayLaunchBackgroundVisible = false
    }

    @discardableResult
    private func requireBootableBIOS() -> Bool {
        guard ARMSX2Bridge.hasBIOS() else {
            bootDisclaimerMessage = "BIOS not yet imported."
            return false
        }
        return true
    }

    private func releaseMenuBackgroundResourcesForGameplay() {
        NotificationCenter.default.post(
            name: Self.releaseMenuBackgroundResourcesNotification,
            object: nil
        )
    }

    private func restoreMenuSystemChrome() {
        // Gameplay fullscreen belongs to SDL's root controller. Clear that
        // authoritative state before asking the child SwiftUI host to show
        // the menu status bar and home indicator again.
        ARMSX2Bridge.setFullScreen(false)
        hideStatusBar = false
        hideHomeIndicator = false
    }
}
