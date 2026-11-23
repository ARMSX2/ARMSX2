// GameSession.swift — The running game: boot, stop, restart and the launch card
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

private enum EmulationOnlyNativeReleaseFlag {
    // Keep these bit positions synchronized with VMManager.h.
    static let patches: UInt = 1 << 0
    static let discordPresence: UInt = 1 << 1
    static let pine: UInt = 1 << 2
    static let achievements: UInt = 1 << 3
    static let inputRecording: UInt = 1 << 4
    static let osd: UInt = 1 << 5
}

@MainActor
@Observable
final class GameSession {
    static let shared = GameSession()

    var runningGameName: String? = nil
    private(set) var vmShutdownPending = false
    var bootDisclaimerMessage: String?
    var bootDisclaimerTitle = "BIOS"
    var pendingJITGameBoot: PendingJITGameBoot?
    var pendingRestartGame: String?
    private(set) var automaticGameStartupPending: Bool
    @ObservationIgnored private var automaticGameStartupStarted = false
    var gameplayLaunchTransition: GameplayLaunchTransition?
    var gameplayLaunchControlsVisible = true
    var gameplayLaunchBackgroundVisible = false
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
    private(set) var emulationSessionID: UUID? {
        didSet {
            userVirtualPadVisible = true
            runtimeShortcutSpeedPercent = nil
        }
    }
    /// The pad toggle and the speed shortcut outlive GameScreenView, which the menu unmounts.
    var userVirtualPadVisible = true
    var runtimeShortcutSpeedPercent: Int?
    var isEmulationOnlyMode: Bool = false
    var emulationOnlyPresentation = EmulationOnlyPresentation.minimal
    private(set) var emulationOnlyStartupReady: Bool = false
    private var emulationOnlyTransitionTask: Task<Void, Never>?
    private var emulationOnlyActivationInFlight = false
    private var pendingEmulationOnlyPresentation: EmulationOnlyPresentation?

    @ObservationIgnored private var pendingBootAction: (() -> Void)?
    @ObservationIgnored private var leaveSave = 0
    @ObservationIgnored private var shutdownObserver: NSObjectProtocol?

    @ObservationIgnored private var autoBootObserver: NSObjectProtocol?
    @ObservationIgnored private var emulationOnlyStartupReadyObserver: NSObjectProtocol?

    private var currentScreen: AppState.Screen {
        get { AppState.shared.currentScreen }
        set { AppState.shared.currentScreen = newValue }
    }

    private func restoreMenuSystemChrome() { AppState.shared.restoreMenuSystemChrome() }
    private var settings: SettingsStore { SettingsStore.shared }

    private init() {
        // Read only persisted boot policy here, never SettingsStore.shared:
        // singleton initialization can itself reach AppState.
        automaticGameStartupPending = ARMSX2Bridge.getINIBool(
            "ARMSX2iOS/Boot", key: "AutomaticLoadLastGame", defaultValue: false
        )
        shutdownObserver = NotificationCenter.default.addObserver(
            forName: .ARMSX2iOSVMDidShutdown,
            object: nil, queue: .main
        ) { [weak self] _ in MainActor.assumeIsolated {
            self?.reset()
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
        } }

        // Synchronize SwiftUI state when the native auto-boot path starts a VM.
        autoBootObserver = NotificationCenter.default.addObserver(
            forName: .ARMSX2iOSAutoBootDidStart,
            object: nil, queue: .main
        ) { [weak self] _ in MainActor.assumeIsolated {
            self?.isEmulationOnlyMode = false
            self?.emulationOnlyPresentation = .minimal
            self?.emulationOnlyStartupReady = false
            self?.cancelGameplayLaunchTransition()
            self?.releaseMenuBackgroundResourcesForGameplay()
            self?.runningGameName = "AutoBoot"
            self?.emulationSessionID = UUID()
            self?.currentScreen = .playing
        } }

        emulationOnlyStartupReadyObserver = NotificationCenter.default.addObserver(
            forName: .ARMSX2iOSEmulationOnlyStartupReady,
            object: nil, queue: .main
        ) { [weak self] _ in MainActor.assumeIsolated {
            self?.emulationOnlyStartupReady = true
        } }
    }

    /// Clears what a VM shutdown ends, whether or not a restart is queued behind it.
    func reset() {
        // Also when the VM stopped by itself: an Undo must not reach the next game.
        SaveStateUndoModel.shared.finish()
        vmShutdownPending = false
        cancelGameplayLaunchTransition()
        isEmulationOnlyMode = false
        emulationOnlyPresentation = .minimal
        emulationOnlyStartupReady = false
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
        // Older versions kept only BootISO; it stands in until a game boot records LastGame.
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
        // Launches that know only the ISO name get the pad identity before GameScreenView mounts.
        let padSettings = ARMSX2Bridge.gameSettings(forISO: isoName)
        prepareGameplayPadSelection(
            forSerial: gameplayPadSerial ?? (padSettings[.serial] as? String),
            crc: (padSettings[.crc] as? String)
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
        NotificationCenter.default.post(name: .ARMSX2iOSReturnToMenu, object: nil)
    }

    func returnToGame() {
        if runningGameName != nil {
            cancelGameplayLaunchTransition()
            releaseMenuBackgroundResourcesForGameplay()
            // Make the hosting surface transparent before revealing Metal output.
            NotificationCenter.default.post(name: .ARMSX2iOSEnterGameScreen, object: nil)
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
        Task { @MainActor in
            // Undo can't reach a closed game, so a deleted state goes for good now.
            SaveStateUndoModel.shared.finish()
            let settings = SettingsStore.shared
            guard settings.autoSaveEnabled, settings.autoSaveOnLeave else { return ARMSX2Bridge.requestVMStop() }
            leaveSave &+= 1
            let id = leaveSave
            SaveStateAutoSave.shared.save(leaving: true) { _ in self.stopAfterLeaveSave(id) }
            // Stop never waits long on a save stuck behind other work.
            try? await Task.sleep(for: .seconds(2))
            stopAfterLeaveSave(id)
        }
    }

    private func stopAfterLeaveSave(_ id: Int) {
        guard leaveSave == id else { return }
        leaveSave &+= 1
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

    @MainActor
    func enterEmulationOnlyModeIfReady(
        presentation makePresentation: @escaping @MainActor () -> EmulationOnlyPresentation
    ) {
        guard settings.emulationOnlyModeEnabled,
              emulationOnlyStartupReady,
              !isEmulationOnlyMode,
              ARMSX2Bridge.isVMRunning(),
              emulationOnlyTransitionTask == nil,
              !emulationOnlyActivationInFlight
        else {
            return
        }

        let delaySeconds = settings.emulationOnlyModeDelaySeconds
        guard delaySeconds > 0 else {
            activateEmulationOnlyModeIfReady(presentation: makePresentation)
            return
        }

        emulationOnlyTransitionTask = Task { @MainActor in
            do {
                try await Task.sleep(nanoseconds: UInt64(delaySeconds) * 1_000_000_000)
            } catch {
                return
            }

            guard !Task.isCancelled else { return }
            emulationOnlyTransitionTask = nil
            activateEmulationOnlyModeIfReady(presentation: makePresentation)
        }
    }

    @MainActor
    private func activateEmulationOnlyModeIfReady(
        presentation makePresentation: @MainActor () -> EmulationOnlyPresentation
    ) {
        guard settings.emulationOnlyModeEnabled,
              emulationOnlyStartupReady,
              !isEmulationOnlyMode,
              ARMSX2Bridge.isVMRunning(),
              !emulationOnlyActivationInFlight
        else {
            return
        }

        pendingEmulationOnlyPresentation = makePresentation()
        emulationOnlyActivationInFlight = true
        ARMSX2Bridge.releaseNonEmulationResources(emulationOnlyNativeReleaseFlags)
    }

    /// True when a native emulation-only VM needs its presentation back; the view then finishes it.
    @MainActor
    func restoreEmulationOnlyPresentation(
        _ makePresentation: () -> EmulationOnlyPresentation
    ) -> Bool {
        guard ARMSX2Bridge.isVMRunning(),
              ARMSX2Bridge.isEmulationOnlyModeActive(),
              !isEmulationOnlyMode
        else {
            return false
        }

        pendingEmulationOnlyPresentation = makePresentation()
        emulationOnlyActivationInFlight = true
        return true
    }

    /// Takes the activation the native release answered, or nil when it no longer applies.
    @MainActor
    func takeEmulationOnlyActivation() -> EmulationOnlyPresentation? {
        guard emulationOnlyActivationInFlight,
              let presentation = pendingEmulationOnlyPresentation
        else {
            return nil
        }

        emulationOnlyActivationInFlight = false
        pendingEmulationOnlyPresentation = nil
        guard ARMSX2Bridge.isVMRunning(),
              emulationOnlyStartupReady
        else {
            return nil
        }
        return presentation
    }

    @MainActor
    func finishEmulationOnlyActivation(_ presentation: EmulationOnlyPresentation) {
        if !presentation.showsVirtualControls {
            ARMSX2VirtualPadMaskImageCache.releaseForEmulationOnlyMode()
        }
        GameEventHaptics.shared.releaseForEmulationOnlyMode()
        PatchStore.shared.releasePresentationResources()
        if settings.emulationOnlyClearNetworkCache {
            URLCache.shared.removeAllCachedResponses()
        }
        if settings.emulationOnlyDisableFramePacing {
            FrameTimeDynamicResolutionController.shared.suspendForEmulationOnlyMode()
        }

        ARMSX2Bridge.setVMPaused(false)
        enterEmulationOnlyMode(presentation: presentation)
    }

    @MainActor
    func cancelEmulationOnlyTransition() {
        emulationOnlyTransitionTask?.cancel()
        emulationOnlyTransitionTask = nil
    }

    /// The game screen leaving drops a pending activation too, as its own state did.
    func dropEmulationOnlyTransition() {
        cancelEmulationOnlyTransition()
        emulationOnlyActivationInFlight = false
        pendingEmulationOnlyPresentation = nil
    }

    private var emulationOnlyNativeReleaseFlags: UInt {
        var flags: UInt = 0
        if settings.emulationOnlyDisablePatches { flags |= EmulationOnlyNativeReleaseFlag.patches }
        if settings.emulationOnlyDisableDiscordPresence { flags |= EmulationOnlyNativeReleaseFlag.discordPresence }
        if settings.emulationOnlyDisablePINE { flags |= EmulationOnlyNativeReleaseFlag.pine }
        if settings.emulationOnlyDisableRetroAchievements { flags |= EmulationOnlyNativeReleaseFlag.achievements }
        if settings.emulationOnlyDisableInputRecording { flags |= EmulationOnlyNativeReleaseFlag.inputRecording }
        if settings.emulationOnlyDisableOSD { flags |= EmulationOnlyNativeReleaseFlag.osd }
        return flags
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
            name: AppState.releaseMenuBackgroundResourcesNotification,
            object: nil
        )
    }

    func currentRuntimeGameName(gameMenuAvailable: Bool) -> String? {
        if let gameName = normalizedRuntimeGameName(runningGameName) {
            return gameName
        }

        // BIOS alone has no game name. The library match below opens every local disc
        // image, so it waits until a disc makes gameMenuAvailable true.
        if runningGameName == "BIOS" && !gameMenuAvailable {
            return nil
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentGameISOName()) {
            return gameName
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentISOPath()) {
            return gameName
        }

        let bootISO = ARMSX2Bridge.getINIString("GameISO", key: "BootISO", defaultValue: "")
        if let gameName = normalizedRuntimeGameName(bootISO) {
            return gameName
        }

        return gameNameMatchingRuntimeIdentity()
    }

    private func normalizedRuntimeGameName(_ value: String?) -> String? {
        guard var value = value?.trimmingCharacters(in: .whitespacesAndNewlines),
              !value.isEmpty else {
            return nil
        }

        value = value.trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
        let fileName = (value as NSString).lastPathComponent
        guard !fileName.isEmpty,
              fileName != "BIOS",
              fileName != "AutoBoot" else {
            return nil
        }

        return fileName
    }

    private func gameNameMatchingRuntimeIdentity() -> String? {
        let identity = normalizedRuntimeIdentity(ARMSX2Bridge.currentDiscIdentity())
        guard !identity.isEmpty else {
            return nil
        }

        for gameName in ARMSX2Bridge.availableISOs() {
            let metadata = ARMSX2Bridge.gameMetadata(forISO: gameName)
            let serial = normalizedRuntimeIdentity(metadata[.serial])
            if !serial.isEmpty && serial == identity {
                return gameName
            }

            if let crc = metadata[.crc]?.trimmingCharacters(in: .whitespacesAndNewlines).uppercased(),
               !crc.isEmpty,
               (identity == crc || identity == "CRC-\(crc)") {
                return gameName
            }
        }

        return nil
    }

    private func normalizedRuntimeIdentity(_ value: String?) -> String {
        (value ?? "")
            .replacingOccurrences(of: "_", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased()
    }

    var availableDiscSwapNames: [String] {
        ARMSX2Bridge.availableISOs().filter { !$0.lowercased().hasSuffix(".elf") }
    }
}
