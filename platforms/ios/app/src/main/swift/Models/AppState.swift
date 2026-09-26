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
    let showsGameName: Bool
    let showsGameInfo: Bool
    let showsFavoriteIndicator: Bool
    let usesSingleLineGameName: Bool
    let usesClearGlass: Bool
}

struct PendingJITGameBoot {
    let isoName: String
    let launchTransition: GameplayLaunchTransition?
    let requiresShutdown: Bool
    var automaticallyLoadLastState = true
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
    private(set) var vmShutdownPending = false
    var bootDisclaimerMessage: String?
    var bootDisclaimerTitle = "BIOS"
    var pendingJITGameBoot: PendingJITGameBoot?
    var pendingRestartGame: String?
    var pendingLibraryExport: String?
    private(set) var automaticGameStartupPending: Bool
    @ObservationIgnored private var automaticGameStartupStarted = false
    var gameplayLaunchTransition: GameplayLaunchTransition?
    var gameplayLaunchControlsVisible = true
    var gameplayLaunchBackgroundVisible = false
    var externalDisplayConnected = false
    /// Serial supplied by the library before the VM has published its CRC.
    /// This lets gameplay resolve a serial-scoped automatic pad assignment on
    /// its very first SwiftUI frame instead of briefly drawing Global Default.
    private(set) var gameplayPadSerial: String?
    /// Library-resolved identity available before the VM publishes its own
    /// settings identity. This prevents the first virtual-pad frame from
    /// falling back to the default layout.
    private(set) var gameplayPadIdentity: PadLayoutGameIdentity?
    /// Changes only when a new VM boot begins. The root gameplay overlay uses
    /// this identity to show startup guidance once per emulation session,
    /// rather than every time GameScreenView remounts after visiting the menu.
    private(set) var emulationSessionID: UUID?
    var isEmulationOnlyMode: Bool = false
    var emulationOnlyPresentation = EmulationOnlyPresentation.minimal
    private(set) var emulationOnlyStartupReady: Bool = false
    var hideStatusBar: Bool = false {
        didSet {
            if systemChromeNotificationsEnabled, oldValue != hideStatusBar {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }
    var hideHomeIndicator: Bool = false {
        didSet {
            if systemChromeNotificationsEnabled, oldValue != hideHomeIndicator {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }

    @ObservationIgnored private var systemChromeNotificationsEnabled = false
    @ObservationIgnored private var pendingBootAction: (() -> Void)?
    @ObservationIgnored private var shutdownObserver: NSObjectProtocol?

    @ObservationIgnored private var autoBootObserver: NSObjectProtocol?
    @ObservationIgnored private var emulationOnlyStartupReadyObserver: NSObjectProtocol?

    private init() {
        // Read only persisted boot policy here, never SettingsStore.shared:
        // singleton initialization can itself reach AppState.
        automaticGameStartupPending = ARMSX2Bridge.getINIBool(
            "ARMSX2iOS/Boot", key: "AutomaticLoadLastGame", defaultValue: false
        )
        // RootView is attached as a child of SDL's controller. Seed the
        // launch-time preference before UIKit asks for its first status-bar
        // appearance, rather than waiting for SwiftUI's onAppear callback.
        // Automatic game startup skips the intro and lets GameScreenView own
        // the gameplay preference.
        if !automaticGameStartupPending {
            hideStatusBar = UserDefaults.standard.object(
                forKey: "ARMSX2iOSHideIntroStatusBar"
            ) as? Bool ?? true
        }
        shutdownObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("ARMSX2iOSVMDidShutdown"),
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.vmShutdownPending = false
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
                self?.gameplayPadSerial = nil
                self?.gameplayPadIdentity = nil
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
            self?.emulationSessionID = UUID()
            self?.currentScreen = .playing
        }

        emulationOnlyStartupReadyObserver = NotificationCenter.default.addObserver(
            forName: Self.emulationOnlyStartupReadyNotification,
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.emulationOnlyStartupReady = true
        }
        systemChromeNotificationsEnabled = true
    }

    /// A cold-launch URL or explicit test boot takes precedence over resume.
    func suppressAutomaticGameStartup() {
        automaticGameStartupStarted = true
        automaticGameStartupPending = false
    }

    /// Call immediately before a library boot. The value deliberately survives
    /// JIT and shutdown gates so the accepted skin is ready before GameScreenView
    /// mounts; the VM's canonical serial/CRC takes over once available.
    func prepareGameplayPadSelection(
        forSerial rawSerial: String?,
        crc rawCRC: String? = nil
    ) {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        gameplayPadSerial = serial.isEmpty ? nil : serial
        gameplayPadIdentity = PadLayoutGameIdentity(
            serial: serial,
            crc: rawCRC
        )
    }

    @MainActor
    func startAutomaticGameIfNeeded() {
        guard automaticGameStartupPending, !automaticGameStartupStarted else { return }
        automaticGameStartupStarted = true
        guard currentScreen == .menu, runningGameName == nil else {
            automaticGameStartupPending = false
            return
        }
        // Older versions retained only BootISO. Use it until the first
        // successful boot records LastGame; BIOS boots cannot clear LastGame.
        let legacyGame = ARMSX2Bridge.getINIString(
            "GameISO", key: "BootISO", defaultValue: ""
        )
        let game = ARMSX2Bridge.getINIString(
            "ARMSX2iOS/Boot", key: "LastGame", defaultValue: legacyGame
        )
        guard !game.isEmpty, ARMSX2Bridge.canResolveISO(game) else {
            automaticGameStartupPending = false
            if !game.isEmpty {
                bootDisclaimerTitle = "Automatic Load Last Game"
                bootDisclaimerMessage = "The last game is no longer available. Reconnect its storage or choose another game."
            }
            return
        }
        if !bootGame(isoName: game), pendingJITGameBoot == nil {
            automaticGameStartupPending = false
        }
    }

    @MainActor
    func resumeAutomaticGameAfterJITIfReady() {
        guard automaticGameStartupPending, pendingJITGameBoot != nil,
              ARMSX2Bridge.isJITAvailable() else { return }
        continuePendingJITGameBoot()
    }

    @discardableResult
    func bootGame(
        isoName: String,
        launchTransition: GameplayLaunchTransition? = nil
    ) -> Bool {
        // Booting under a live VM rewrites its settings and breaks its disc reads.
        if runningGameName != nil {
            pendingRestartGame = isoName
            return false
        }
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
        launchTransition: GameplayLaunchTransition?,
        automaticallyLoadLastState: Bool = true
    ) -> Bool {
        // The library may reappear before the CPU finishes closing its disc.
        // Queue the whole preparation (including shader metadata), not merely
        // the final native boot request. No timing delay is needed.
        if vmShutdownPending {
            pendingBootAction = { [weak self] in
                self?.performBootGame(
                    isoName: isoName,
                    launchTransition: launchTransition,
                    automaticallyLoadLastState: automaticallyLoadLastState
                )
            }
            return true
        }
        let startsWithoutMenu = automaticGameStartupPending
        Task { @MainActor in
            if startsWithoutMenu {
                // Direct startup has no menu music to carry through a card zoom.
                MenuAudioPackManager.shared.playEvent(.launchGame)
            } else {
                MenuAudioPackManager.shared.beginGameLaunch(
                    hasVisualTransition: launchTransition != nil
                )
            }
        }
        pendingJITGameBoot = nil
        isEmulationOnlyMode = false
        emulationOnlyPresentation = .minimal
        emulationOnlyStartupReady = false
        gameplayLaunchTransition = launchTransition
        gameplayLaunchControlsVisible = launchTransition == nil
        gameplayLaunchBackgroundVisible = launchTransition != nil
        // Some launch paths only know the ISO name. Resolve the identity before
        // GameScreenView mounts so a saved per-game layout is its first frame.
        let padSettings = ARMSX2Bridge.gameSettings(forISO: isoName)
        prepareGameplayPadSelection(
            forSerial: gameplayPadSerial ?? (padSettings["serial"] as? String),
            crc: (padSettings["crc"] as? String)
        )
        if launchTransition == nil {
            releaseMenuBackgroundResourcesForGameplay()
        }
        Task { @MainActor in
            StikDebugLauncher.autoOpenIfNeeded(reason: "game boot")
        }
        // Before bootISO, which reads the per-game file and its absolute preset path.
        PerGameShaderSelection.repair(forISO: isoName)
        ARMSX2Bridge.bootISO(isoName)
        ARMSX2Bridge.prepareGameRenderViewForCurrentRenderer()
        runningGameName = isoName
        emulationSessionID = UUID()
        currentScreen = .playing
        automaticGameStartupPending = false
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            ARMSX2Bridge.requestVMBoot(loadLastSaveState: automaticallyLoadLastState)
        }
        return true
    }

    @discardableResult
    func bootBIOSOnly() -> Bool {
        guard requireBootableBIOS() else { return false }
        if vmShutdownPending {
            pendingBootAction = { [weak self] in self?.bootBIOSOnly() }
            return true
        }

        isEmulationOnlyMode = false
        gameplayPadSerial = nil
        gameplayPadIdentity = nil
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
        emulationSessionID = UUID()
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
        launchTransition: GameplayLaunchTransition? = nil,
        automaticallyLoadLastState: Bool = true
    ) {
        guard requireBootableBIOS() else { return }
        guard ARMSX2Bridge.isJITAvailable() else {
            pendingJITGameBoot = PendingJITGameBoot(
                isoName: isoName,
                launchTransition: launchTransition,
                requiresShutdown: true,
                automaticallyLoadLastState: automaticallyLoadLastState
            )
            return
        }
        performShutdownAndBoot(
            isoName: isoName,
            launchTransition: launchTransition,
            automaticallyLoadLastState: automaticallyLoadLastState
        )
    }

    func continuePendingJITGameBoot() {
        guard let request = pendingJITGameBoot else { return }
        pendingJITGameBoot = nil
        guard requireBootableBIOS() else {
            automaticGameStartupPending = false
            return
        }

        if request.requiresShutdown {
            performShutdownAndBoot(
                isoName: request.isoName,
                launchTransition: request.launchTransition,
                automaticallyLoadLastState: request.automaticallyLoadLastState
            )
        } else {
            performBootGame(
                isoName: request.isoName,
                launchTransition: request.launchTransition,
                automaticallyLoadLastState: request.automaticallyLoadLastState
            )
        }
    }

    func cancelPendingJITGameBoot() {
        pendingJITGameBoot = nil
        automaticGameStartupPending = false
    }

    private func performShutdownAndBoot(
        isoName: String,
        launchTransition: GameplayLaunchTransition?,
        automaticallyLoadLastState: Bool = true
    ) {
        pendingBootAction = { [weak self] in
            self?.performBootGame(
                isoName: isoName,
                launchTransition: launchTransition,
                automaticallyLoadLastState: automaticallyLoadLastState
            )
        }
        requestShutdownIfNeeded()
    }

    func shutdownAndBootBIOS() {
        guard requireBootableBIOS() else { return }
        pendingBootAction = { [weak self] in
            self?.bootBIOSOnly()
        }
        requestShutdownIfNeeded()
    }

    private func requestShutdownIfNeeded() {
        guard !vmShutdownPending else { return }
        vmShutdownPending = true
        ARMSX2Bridge.requestVMStop()
    }

    /// Retire the menu identity immediately, but keep the native shutdown gate
    /// until VMDidShutdown. Reopening a card queues one boot behind that gate.
    func stopGame() {
        cancelPendingBoot()
        requestShutdownIfNeeded()
        runningGameName = nil
        gameplayPadSerial = nil
        gameplayPadIdentity = nil
    }

    func resetCurrentVM() {
        guard let runningGameName else { return }

        if runningGameName == "BIOS" {
            shutdownAndBootBIOS()
        } else {
            // Reset ROM must remain a clean boot, not resume the old state.
            shutdownAndBoot(isoName: runningGameName, automaticallyLoadLastState: false)
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
            bootDisclaimerTitle = "BIOS"
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
