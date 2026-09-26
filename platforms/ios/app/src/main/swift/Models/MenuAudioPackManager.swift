// MenuAudioPackManager.swift — persistent UI audio-pack import and playback
// SPDX-License-Identifier: GPL-3.0+

import AVFoundation
import Foundation
import Observation

enum MenuAudioPackImportError: LocalizedError, Sendable {
    case notZip
    case incompletePack
    case unplayableFile(String)

    var errorDescription: String? {
        switch self {
        case .notZip:
            return "Choose a ZIP archive containing all required UI audio files."
        case .incompletePack:
            return "The archive did not contain one playable MP3 or WAV for every required sound."
        case .unplayableFile(let name):
            return "\(name) is not a playable MP3 or WAV file."
        }
    }
}

/// Owns the menu soundtrack and short UI effects. Playback stays centralized so
/// retained Settings views and recycled controller targets never own audio players.
@MainActor
@Observable
final class MenuAudioPackManager {
    enum Sound: String, CaseIterable, Sendable {
        case startup
        case background
        case pauseMusic = "pause_music"
        case navigation
        case select
        case switchToggleOn = "switch_toggle_on"
        case switchToggleOff = "switch_toggle_off"
        case `return`
        case stoppingGame = "stopping_game"
        case contextMenu = "context_menu"
        case achievementToast = "achievement_toast"
        case uiToast = "ui_toast"
        case tabTransition = "tab_transition"
        case launchGame = "launch_game"
        case noJIT = "no_jit"
    }

    enum Event: Hashable, Sendable {
        case navigation
        case select
        case toggle(isOn: Bool)
        case `return`
        case stoppingGame
        case contextMenu
        case achievementToast
        case uiToast
        case tabTransition
        case launchGame
        case noJIT
    }

    static let shared = MenuAudioPackManager()

    private struct Installation: Sendable {
        let displayName: String
    }

    private struct BundledResource: Sendable {
        let name: String
        let pathExtension: String
        let subdirectory: String
    }

    private static let installedNameDefaultsKey = "ARMSX2.MenuAudioPack.InstalledName"
    private static let uiAudioVolumeDefaultsKey = "ARMSX2.MenuAudioPack.UIAudioVolume"
    private static let customAudioEnabledDefaultsKey =
        "ARMSX2.MenuAudioPack.CustomAudioEnabled"
    private static let soundVolumeDefaultsKeyPrefix =
        "ARMSX2.MenuAudioPack.SoundVolume."
    private nonisolated static let requiredSounds = Set(
        Sound.allCases.filter { $0 != .startup && $0 != .pauseMusic }
    )
    private nonisolated static let bundledResources: [Sound: BundledResource] = [
        .achievementToast: BundledResource(
            name: "unlock",
            pathExtension: "wav",
            subdirectory: "sounds/achievements"
        ),
        .navigation: BundledResource(
            name: "sfx_nav_a",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .tabTransition: BundledResource(
            name: "sfx_nav_a",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .background: BundledResource(
            name: "library_music",
            pathExtension: "m4a",
            subdirectory: "ui_audio/default"
        ),
        .pauseMusic: BundledResource(
            name: "pause_music",
            pathExtension: "mp3",
            subdirectory: "ui_audio/default"
        ),
        .noJIT: BundledResource(
            name: "sfx_reset",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .stoppingGame: BundledResource(
            name: "sfx_popup_close",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .uiToast: BundledResource(
            name: "sfx_popup_open",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .contextMenu: BundledResource(
            name: "message",
            pathExtension: "wav",
            subdirectory: "sounds/achievements"
        ),
        .return: BundledResource(
            name: "sfx_nav_b",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .switchToggleOff: BundledResource(
            name: "sfx_toggle_off",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .launchGame: BundledResource(
            name: "sfx_popup_open",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .select: BundledResource(
            name: "sfx_nav_a",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        ),
        .switchToggleOn: BundledResource(
            name: "sfx_toggle_on",
            pathExtension: "wav",
            subdirectory: "ui_audio/default"
        )
    ]

    private(set) var installedPackName: String?
    private(set) var usesBundledDefault = false
    private(set) var isWorking = false
    private(set) var uiAudioVolume = 1.0
    private(set) var customUIAudioEnabled = true
    private(set) var customSounds: Set<Sound> = []

    @ObservationIgnored private var soundURLs: [Sound: URL] = [:]
    @ObservationIgnored private var customSoundURLs: [Sound: URL] = [:]
    @ObservationIgnored private var soundVolumes: [Sound: Double] = [:]
    // Retain the encoded source bytes and prepared players for the lifetime of
    // the installed pack. `AVAudioPlayer(data:)` then never has to reopen a
    // file while rapid controller events are driving the UI.
    @ObservationIgnored private var soundData: [Sound: Data] = [:]
    @ObservationIgnored private var introHasFinished = false
    @ObservationIgnored private var interfaceIsActive = false
    @ObservationIgnored private var mainInterfaceIsActive = false
    @ObservationIgnored private var applicationIsActive = true
    @ObservationIgnored private var audioSessionPrepared = false
    @ObservationIgnored private var startupPlayer: AVAudioPlayer?
    @ObservationIgnored private var startupCompletionTask: Task<Void, Never>?
    @ObservationIgnored private var startupRetryTask: Task<Void, Never>?
    @ObservationIgnored private var startupPlaybackPending = false
    @ObservationIgnored private var startupRetryAttempt = 0
    @ObservationIgnored private var backgroundPlayer: AVAudioPlayer?
    @ObservationIgnored private var pauseMusicPlayer: AVAudioPlayer?
    @ObservationIgnored private var previewPlayer: AVAudioPlayer?
    @ObservationIgnored private var previewSound: Sound?
    @ObservationIgnored private var backgroundFadeTask: Task<Void, Never>?
    @ObservationIgnored private var gameLaunchCompletionTask: Task<Void, Never>?
    @ObservationIgnored private var gameLaunchAudioActive = false
    @ObservationIgnored private var automaticGameStartupActive = false
    @ObservationIgnored private var effectPlayers: [Sound: AVAudioPlayer] = [:]
    @ObservationIgnored private var pendingTouchNavigationTask: Task<Void, Never>?
    @ObservationIgnored private var stoppingGamePendingForMainInterface = false
    @ObservationIgnored private var stoppingGameWaitsForVMShutdown = false
    @ObservationIgnored private var emulationAudioRecoveryTask: Task<Void, Never>?
    @ObservationIgnored private var selectionSuppressedUntil: TimeInterval = 0
    @ObservationIgnored private var lastEventPlaybackTimes: [Event: TimeInterval] = [:]

    var hasInstalledPack: Bool {
        installedPackName != nil && Self.containsRequiredSounds(soundURLs)
    }

    private var shouldPlayBackgroundAudio: Bool {
        introHasFinished
            && !automaticGameStartupActive
            && (mainInterfaceIsActive || gameLaunchAudioActive)
            && applicationIsActive
            && hasInstalledPack
    }

    private var shouldPlayPauseMusic: Bool {
        introHasFinished
            && !automaticGameStartupActive
            && interfaceIsActive
            && !mainInterfaceIsActive
            && !gameLaunchAudioActive
            && applicationIsActive
            && hasInstalledPack
    }

    private init() {
        let defaults = UserDefaults.standard
        if defaults.object(forKey: Self.uiAudioVolumeDefaultsKey) != nil {
            uiAudioVolume = Self.clampedVolume(
                defaults.double(forKey: Self.uiAudioVolumeDefaultsKey)
            )
        }
        if defaults.object(forKey: Self.customAudioEnabledDefaultsKey) != nil {
            customUIAudioEnabled = defaults.bool(
                forKey: Self.customAudioEnabledDefaultsKey
            )
        } else {
            // Enable per-sound customization for new installations while
            // preserving an existing explicit user choice.
            customUIAudioEnabled = true
            defaults.set(true, forKey: Self.customAudioEnabledDefaultsKey)
        }
        for sound in Sound.allCases {
            let key = Self.soundVolumeDefaultsKey(for: sound)
            guard defaults.object(forKey: key) != nil else { continue }
            soundVolumes[sound] = Self.clampedVolume(defaults.double(forKey: key))
        }
        reloadInstalledPack()
    }

    func volume(for sound: Sound) -> Double {
        soundVolumes[sound] ?? Self.defaultVolume(for: sound)
    }

    private nonisolated static func defaultVolume(for sound: Sound) -> Double {
        1
    }

    func setUIAudioVolume(_ volume: Double) {
        let normalizedVolume = Self.clampedVolume(volume)
        guard normalizedVolume != uiAudioVolume else { return }
        uiAudioVolume = normalizedVolume
        UserDefaults.standard.set(
            normalizedVolume,
            forKey: Self.uiAudioVolumeDefaultsKey
        )
        applyConfiguredVolumes()
    }

    func setVolume(_ volume: Double, for sound: Sound) {
        let normalizedVolume = Self.clampedVolume(volume)
        guard normalizedVolume != self.volume(for: sound) else { return }
        soundVolumes[sound] = normalizedVolume
        UserDefaults.standard.set(
            normalizedVolume,
            forKey: Self.soundVolumeDefaultsKey(for: sound)
        )
        applyConfiguredVolume(for: sound)
    }

    func setCustomUIAudioEnabled(_ enabled: Bool) {
        guard enabled != customUIAudioEnabled else { return }
        customUIAudioEnabled = enabled
        UserDefaults.standard.set(
            enabled,
            forKey: Self.customAudioEnabledDefaultsKey
        )
        reloadInstalledPack()
        reconcilePlayback()
    }

    /// Reset mix controls while retaining user-owned audio packs and per-sound replacement files.
    /// Resetting settings must not silently delete content which cannot necessarily be recovered.
    func resetAudioSettingsToDefaults() {
        let defaults = UserDefaults.standard
        let customAudioWasEnabled = customUIAudioEnabled
        uiAudioVolume = 1
        customUIAudioEnabled = true
        soundVolumes.removeAll(keepingCapacity: true)
        defaults.set(1.0, forKey: Self.uiAudioVolumeDefaultsKey)
        defaults.set(true, forKey: Self.customAudioEnabledDefaultsKey)
        for sound in Sound.allCases {
            defaults.removeObject(forKey: Self.soundVolumeDefaultsKey(for: sound))
        }

        if customAudioWasEnabled {
            applyConfiguredVolumes()
        } else {
            reloadInstalledPack()
        }
        reconcilePlayback()
    }

    func hasSound(_ sound: Sound) -> Bool {
        soundURLs[sound] != nil
    }

    func preview(_ sound: Sound) {
        guard prepareAudioSessionIfNeeded(), let data = soundData[sound] else { return }
        do {
            previewPlayer?.stop()
            let player = try AVAudioPlayer(data: data)
            player.numberOfLoops = 0
            player.volume = configuredVolume(for: sound)
            guard player.prepareToPlay(), player.play() else { return }
            previewPlayer = player
            previewSound = sound
        } catch {
            NSLog(
                "[ARMSX2 UI Audio] %@ preview failed: %@",
                sound.rawValue,
                error.localizedDescription
            )
        }
    }

    func importCustomSound(from sourceURL: URL, for sound: Sound) async throws {
        guard !isWorking else { return }
        isWorking = true
        defer { isWorking = false }

        try await Task.detached(priority: .userInitiated) {
            try Self.installCustomSound(from: sourceURL, for: sound)
        }.value

        reloadInstalledPack()
        reconcilePlayback()
    }

    func removeCustomSound(_ sound: Sound) async throws {
        guard !isWorking, customSounds.contains(sound) else { return }
        isWorking = true
        defer { isWorking = false }

        try await Task.detached(priority: .userInitiated) {
            try Self.removeCustomSoundFile(for: sound)
        }.value

        reloadInstalledPack()
        reconcilePlayback()
    }

    func introDidFinish(playsStartupSound: Bool = true) {
        guard !introHasFinished else { return }
        introHasFinished = true
        preloadPlayers()
        startupPlaybackPending = playsStartupSound && startupPlayer != nil
        startupRetryAttempt = 0
        // AVPlayer may have owned the shared session until the exact dismissal
        // turn. Never reuse a preparation result from the boot surface.
        audioSessionPrepared = false
        reconcilePlayback(shouldStartIntroAudio: playsStartupSound)
    }

    func setAutomaticGameStartupActive(_ active: Bool) {
        guard automaticGameStartupActive != active else { return }
        automaticGameStartupActive = active
        reconcilePlayback()
    }

    func setInterfaceActive(_ active: Bool) {
        let becameActive = active && !interfaceIsActive
        interfaceIsActive = active
        if becameActive, introHasFinished, applicationIsActive, hasInstalledPack {
            // Quick Menu effects share the active emulator audio session. Do
            // any one-time ownership/configuration work when the surface opens,
            // outside the rapid directional-navigation path.
            _ = prepareAudioSessionIfNeeded()
        }
        reconcilePlayback()
    }

    /// Controls the music-bearing frontend independently from controller UI
    /// surfaces such as the in-game Quick Menu.
    func setMainInterfaceActive(_ active: Bool) {
        guard active != mainInterfaceIsActive else { return }
        mainInterfaceIsActive = active

        // SDL owns the process audio session during emulation. Force one
        // reconfiguration when ownership crosses between gameplay and the
        // frontend, never for each navigation sound.
        audioSessionPrepared = false
        reconcilePlayback()
        playPendingMainInterfaceEventsIfPossible()
    }

    /// Reclaims the shared audio session after SDL has completed VM shutdown.
    /// The menu can become visible before this notification, so its first
    /// background-play attempt may legitimately lose the ownership race.
    func emulationAudioDidStop() {
        stoppingGameWaitsForVMShutdown = false
        emulationAudioRecoveryTask?.cancel()

        // AVAudioPlayer can retain isPlaying=true after SDL deactivates the
        // process audio session. Pause once at the completed handoff so
        // reconcilePlayback() necessarily issues a fresh play().
        backgroundPlayer?.pause()
        audioSessionPrepared = false
        reconcilePlayback()
        playPendingMainInterfaceEventsIfPossible()

        // A normal menu-state publication can land one run-loop turn after the
        // native shutdown notification. Retry only if playback is still absent.
        emulationAudioRecoveryTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(180))
            guard let self, !Task.isCancelled else { return }
            self.emulationAudioRecoveryTask = nil
            guard self.mainInterfaceIsActive,
                  self.backgroundPlayer?.isPlaying != true else { return }
            self.audioSessionPrepared = false
            self.reconcilePlayback()
            self.playPendingMainInterfaceEventsIfPossible()
        }
    }

    func setApplicationActive(_ active: Bool) {
        if applicationIsActive != active {
            // AVAudioSession can be deactivated or replaced while the app is in
            // the background. Force the next sound to restore our category.
            audioSessionPrepared = false
        }
        applicationIsActive = active
        if active, introHasFinished, interfaceIsActive, hasInstalledPack {
            _ = prepareAudioSessionIfNeeded()
        }
        reconcilePlayback()
        playPendingMainInterfaceEventsIfPossible()
    }

    func play(_ feedback: MenuControllerFeedback) {
        let event: Event?
        switch feedback {
        case .move:
            event = .navigation
        case .activate, .favorite, .submenu, .destination:
            event = .select
        case .toggle(let isOn):
            event = .toggle(isOn: isOn)
        case .contextMenu:
            event = .contextMenu
        case .back:
            event = .return
        case .previousTab, .nextTab, .tabTransition:
            event = .tabTransition
        case .boundary:
            event = nil
        case .silent:
            event = nil
        }

        if let event {
            playEvent(event)
        }
    }

    func playEvent(_ event: Event) {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        guard introHasFinished, applicationIsActive, hasInstalledPack else { return }

        // A semantic action can run inside a generic controller activation.
        // Keep the semantic sound authoritative when the activation callback
        // returns and the focus engine emits its fallback Select feedback.
        switch event {
        case .return, .stoppingGame, .contextMenu, .uiToast,
             .tabTransition, .launchGame, .noJIT:
            suppressGenericSelection()
        case .navigation, .select, .toggle, .achievementToast:
            break
        }

        let requiresActiveInterface: Bool
        let sound: Sound
        switch event {
        case .navigation:
            requiresActiveInterface = true
            sound = .navigation
        case .select:
            let now = ProcessInfo.processInfo.systemUptime
            guard now >= selectionSuppressedUntil else { return }
            requiresActiveInterface = true
            sound = .select
        case .toggle(let isOn):
            requiresActiveInterface = true
            sound = isOn ? .switchToggleOn : .switchToggleOff
        case .return:
            requiresActiveInterface = true
            sound = .return
        case .stoppingGame:
            requiresActiveInterface = false
            sound = .stoppingGame
            selectionSuppressedUntil = ProcessInfo.processInfo.systemUptime + 0.2
        case .contextMenu:
            requiresActiveInterface = true
            sound = .contextMenu
        case .achievementToast:
            requiresActiveInterface = false
            sound = .achievementToast
        case .uiToast:
            requiresActiveInterface = false
            sound = .uiToast
        case .tabTransition:
            requiresActiveInterface = true
            sound = .tabTransition
        case .launchGame:
            requiresActiveInterface = false
            sound = .launchGame
        case .noJIT:
            requiresActiveInterface = false
            sound = .noJIT
        }
        guard !requiresActiveInterface || interfaceIsActive,
              shouldEmit(event) else { return }
        playEffect(sound)
    }

    private func shouldEmit(_ event: Event) -> Bool {
        let now = ProcessInfo.processInfo.systemUptime
        let minimumInterval: TimeInterval
        switch event {
        case .navigation:
            minimumInterval = 0.025
        case .toggle:
            minimumInterval = 0.04
        default:
            minimumInterval = 0.08
        }
        if let previous = lastEventPlaybackTimes[event],
           now - previous < minimumInterval {
            return false
        }
        lastEventPlaybackTimes[event] = now
        return true
    }

    /// Defers generic tap feedback briefly so a semantic action fired by the
    /// same touch (toggle, context menu, toast, return, tab change) can replace
    /// it instead of producing two sounds.
    func playTouchNavigation() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 60_000_000)
            guard let self, !Task.isCancelled else { return }
            self.pendingTouchNavigationTask = nil
            self.playEvent(.navigation)
        }
    }

    func playTouchToggle(isOn: Bool) {
        playEvent(.toggle(isOn: isOn))
    }

    func playTouchContextMenu() {
        playEvent(.contextMenu)
    }

    /// Removes the window-level tap fallback when the target action owns its
    /// complete sound sequence (for example, launch_game).
    func suppressPendingGenericActivation() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        suppressGenericSelection()
    }

    /// A stop selected inside Quick Menu belongs to the destination frontend,
    /// not to the overlay that is being dismantled.
    func playStoppingGameOnMainInterface() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        suppressGenericSelection()
        stoppingGamePendingForMainInterface = true
        stoppingGameWaitsForVMShutdown = true
    }

    private func suppressGenericSelection(for duration: TimeInterval = 0.2) {
        selectionSuppressedUntil = max(
            selectionSuppressedUntil,
            ProcessInfo.processInfo.systemUptime + duration
        )
    }

    func beginGameLaunch(hasVisualTransition: Bool) {
        gameLaunchCompletionTask?.cancel()
        gameLaunchCompletionTask = nil
        gameLaunchAudioActive = true
        reconcilePlayback()
        playEvent(.launchGame)

        guard !hasVisualTransition else { return }
        let launchDuration = max(
            0.35,
            effectPlayers[.launchGame]?.duration ?? 1.4
        )
        let fadeDuration = min(0.30, launchDuration * 0.35)
        scheduleGameLaunchCompletion(
            after: max(0, launchDuration - fadeDuration),
            fadeDuration: fadeDuration
        )
    }

    func finishGameLaunchWithBackgroundFade(duration: TimeInterval) {
        gameLaunchCompletionTask?.cancel()
        gameLaunchCompletionTask = nil
        fadeOutBackground(duration: duration)
        scheduleGameLaunchCompletion(after: 0, fadeDuration: duration)
    }

    func cancelGameLaunchAudio() {
        gameLaunchCompletionTask?.cancel()
        gameLaunchCompletionTask = nil
        backgroundFadeTask?.cancel()
        backgroundFadeTask = nil
        backgroundPlayer?.volume = configuredVolume(for: .background)
        gameLaunchAudioActive = false
        reconcilePlayback()
    }

    func fadeOutBackground(duration: TimeInterval) {
        guard let backgroundPlayer, backgroundPlayer.isPlaying else { return }
        backgroundFadeTask?.cancel()
        let fadeDuration = max(0.01, duration)
        backgroundPlayer.setVolume(0, fadeDuration: fadeDuration)
        backgroundFadeTask = Task { @MainActor [weak self, weak backgroundPlayer] in
            try? await Task.sleep(
                nanoseconds: UInt64(fadeDuration * 1_000_000_000)
            )
            guard let self, !Task.isCancelled,
                  self.backgroundPlayer === backgroundPlayer else { return }
            self.backgroundFadeTask = nil
        }
    }

    func importPack(from sourceURL: URL) async throws {
        guard !isWorking else { return }
        isWorking = true
        defer { isWorking = false }

        let installation = try await Task.detached(priority: .userInitiated) {
            try Self.installPack(from: sourceURL)
        }.value

        stopAllPlayback()
        UserDefaults.standard.set(
            installation.displayName,
            forKey: Self.installedNameDefaultsKey
        )
        resetPerSoundVolumesAfterPackImport()
        reloadInstalledPack()
        if shouldPlayBackgroundAudio {
            startBackgroundIfNeeded()
        }
    }

    /// A new pack is its own mix. Do not carry volume overrides from the
    /// previously installed files into it; every imported sound starts at its
    /// authored 100% level.
    private func resetPerSoundVolumesAfterPackImport() {
        let defaults = UserDefaults.standard
        soundVolumes.removeAll(keepingCapacity: true)
        for sound in Sound.allCases {
            defaults.removeObject(
                forKey: Self.soundVolumeDefaultsKey(for: sound)
            )
        }
        applyConfiguredVolumes()
    }

    func removeInstalledPack() async throws {
        guard !isWorking, hasInstalledPack, !usesBundledDefault else { return }
        isWorking = true
        defer { isWorking = false }
        stopAllPlayback()

        do {
            try await Task.detached(priority: .userInitiated) {
                let manager = FileManager.default
                let activeDirectory = Self.activeDirectoryURL
                if manager.fileExists(atPath: activeDirectory.path) {
                    try manager.removeItem(at: activeDirectory)
                }
            }.value
            UserDefaults.standard.removeObject(forKey: Self.installedNameDefaultsKey)
            reloadInstalledPack()
            reconcilePlayback()
        } catch {
            reloadInstalledPack()
            reconcilePlayback()
            throw error
        }
    }

    private func reloadInstalledPack() {
        stopAllPlayback()
        soundData.removeAll(keepingCapacity: false)
        startupPlayer = nil
        backgroundPlayer = nil
        pauseMusicPlayer = nil
        effectPlayers.removeAll(keepingCapacity: false)
        previewPlayer = nil
        previewSound = nil

        customSoundURLs = Self.resolveSoundURLs(in: Self.customDirectoryURL)
        customSounds = Set(customSoundURLs.keys)

        let resolvedURLs = Self.resolveSoundURLs(in: Self.activeDirectoryURL)
        var baseURLs: [Sound: URL]
        if Self.containsRequiredSounds(resolvedURLs) {
            baseURLs = resolvedURLs
            usesBundledDefault = false
            installedPackName = UserDefaults.standard.string(
                forKey: Self.installedNameDefaultsKey
            ) ?? "Audio Pack"

            // pause_music was added after the original audio-pack contract.
            // Existing packs remain valid and inherit the bundled Quick Menu
            // loop until they provide their own MP3 or WAV replacement.
            if baseURLs[.pauseMusic] == nil {
                baseURLs[.pauseMusic] =
                    Self.resolveBundledSoundURLs()[.pauseMusic]
            }
        } else {
            let bundledURLs = Self.resolveBundledSoundURLs()
            guard Self.containsRequiredSounds(bundledURLs) else {
                soundURLs = [:]
                usesBundledDefault = false
                installedPackName = nil
                return
            }
            baseURLs = bundledURLs
            usesBundledDefault = true
            installedPackName = "Default Audio Pack"
        }

        if customUIAudioEnabled {
            for (sound, url) in customSoundURLs {
                baseURLs[sound] = url
            }
        }
        soundURLs = baseURLs
        // Reading/decoding every sound on the main thread while
        // AVPlayer is mounting delays the intro's picture behind its audio.
        // Keep RAM-backed playback, but prepare it after the intro hands off.
        if introHasFinished {
            preloadPlayers()
        }
    }

    private func reconcilePlayback(shouldStartIntroAudio: Bool = false) {
        guard introHasFinished, applicationIsActive, hasInstalledPack else {
            stopStartup()
            backgroundPlayer?.pause()
            backgroundPlayer?.volume = configuredVolume(for: .background)
            pauseMusicPlayer?.pause()
            pauseMusicPlayer?.volume = configuredVolume(for: .pauseMusic)
            backgroundFadeTask?.cancel()
            backgroundFadeTask = nil
            stopEffects()
            return
        }

        if shouldPlayPauseMusic {
            stopStartup()
            backgroundPlayer?.pause()
            backgroundPlayer?.volume = configuredVolume(for: .background)
            backgroundFadeTask?.cancel()
            backgroundFadeTask = nil
            startPauseMusicIfNeeded()
            return
        }

        pauseMusicPlayer?.pause()
        pauseMusicPlayer?.volume = configuredVolume(for: .pauseMusic)
        guard !automaticGameStartupActive,
              mainInterfaceIsActive || gameLaunchAudioActive else {
            stopStartup()
            backgroundPlayer?.pause()
            backgroundPlayer?.volume = configuredVolume(for: .background)
            backgroundFadeTask?.cancel()
            backgroundFadeTask = nil
            return
        }

        if shouldStartIntroAudio, startupPlayer != nil {
            startupPlaybackPending = true
        }
        if startupPlaybackPending {
            if playStartupThenBackground() {
                startupPlaybackPending = false
                startupRetryAttempt = 0
                startupRetryTask?.cancel()
                startupRetryTask = nil
            } else {
                scheduleStartupPlaybackRetry()
            }
            return
        }
        if startupPlayer?.isPlaying != true {
            startBackgroundIfNeeded()
        }
    }

    @discardableResult
    private func playStartupThenBackground() -> Bool {
        guard let player = startupPlayer else {
            startBackgroundIfNeeded()
            return true
        }

        guard prepareAudioSessionIfNeeded() else { return false }
        backgroundPlayer?.pause()
        stopStartup()
        player.currentTime = 0
        guard player.play() else {
            return false
        }

        let seconds = min(max(player.duration, 0.05), 3_600)
        let nanoseconds = UInt64(seconds * 1_000_000_000)
        startupCompletionTask = Task { @MainActor [weak self, weak player] in
            try? await Task.sleep(nanoseconds: nanoseconds)
            guard let self, let player, !Task.isCancelled,
                  self.startupPlayer === player else { return }
            self.startupCompletionTask = nil
            player.stop()
            player.currentTime = 0
            player.prepareToPlay()
            self.startBackgroundIfNeeded()
        }
        return true
    }

    private func scheduleStartupPlaybackRetry() {
        guard startupPlaybackPending, startupRetryTask == nil else { return }
        // The boot AVPlayer can release the process audio session several main
        // actor turns after its view disappears.
        guard startupRetryAttempt < 12 else {
            startupPlaybackPending = false
            startupRetryAttempt = 0
            startBackgroundIfNeeded()
            return
        }

        startupRetryAttempt += 1
        startupRetryTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(150))
            guard let self, !Task.isCancelled else { return }
            self.startupRetryTask = nil
            self.audioSessionPrepared = false
            self.reconcilePlayback()
        }
    }

    private func startBackgroundIfNeeded() {
        guard shouldPlayBackgroundAudio, startupPlayer?.isPlaying != true else { return }
        guard prepareAudioSessionIfNeeded() else { return }
        guard let backgroundPlayer else { return }
        if !backgroundPlayer.isPlaying {
            backgroundFadeTask?.cancel()
            backgroundFadeTask = nil
            backgroundPlayer.volume = configuredVolume(for: .background)
            backgroundPlayer.play()
        }
    }

    private func startPauseMusicIfNeeded() {
        guard shouldPlayPauseMusic else { return }
        guard prepareAudioSessionIfNeeded() else { return }
        guard let pauseMusicPlayer else { return }
        if !pauseMusicPlayer.isPlaying {
            pauseMusicPlayer.volume = configuredVolume(for: .pauseMusic)
            pauseMusicPlayer.play()
        }
    }

    private func playEffect(_ sound: Sound) {
        guard prepareAudioSessionIfNeeded() else { return }
        guard let player = effectPlayers[sound] else { return }
        player.currentTime = 0
        player.play()
    }

    private func configuredVolume(for sound: Sound) -> Float {
        Float(Self.clampedVolume(uiAudioVolume * volume(for: sound)))
    }

    private func applyConfiguredVolumes() {
        applyConfiguredVolume(for: .startup)
        applyConfiguredVolume(for: .background)
        applyConfiguredVolume(for: .pauseMusic)
        for sound in effectPlayers.keys {
            applyConfiguredVolume(for: sound)
        }
    }

    private func applyConfiguredVolume(for sound: Sound) {
        let configured = configuredVolume(for: sound)
        switch sound {
        case .startup:
            startupPlayer?.volume = configured
        case .background:
            backgroundPlayer?.volume = configured
        case .pauseMusic:
            pauseMusicPlayer?.volume = configured
        default:
            effectPlayers[sound]?.volume = configured
        }
        if previewSound == sound, previewPlayer?.isPlaying == true {
            previewPlayer?.volume = configured
        }
    }

    private func stopStartup() {
        startupCompletionTask?.cancel()
        startupCompletionTask = nil
        startupPlayer?.stop()
        startupPlayer?.currentTime = 0
        startupPlayer?.prepareToPlay()
    }

    private func stopEffects() {
        for player in effectPlayers.values {
            player.stop()
            player.currentTime = 0
            player.prepareToPlay()
        }
    }

    private func stopAllPlayback() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        backgroundFadeTask?.cancel()
        backgroundFadeTask = nil
        gameLaunchCompletionTask?.cancel()
        gameLaunchCompletionTask = nil
        emulationAudioRecoveryTask?.cancel()
        emulationAudioRecoveryTask = nil
        startupRetryTask?.cancel()
        startupRetryTask = nil
        startupPlaybackPending = false
        startupRetryAttempt = 0
        gameLaunchAudioActive = false
        stopStartup()
        previewPlayer?.stop()
        previewPlayer = nil
        previewSound = nil
        backgroundPlayer?.stop()
        backgroundPlayer?.volume = configuredVolume(for: .background)
        backgroundPlayer?.currentTime = 0
        backgroundPlayer?.prepareToPlay()
        pauseMusicPlayer?.stop()
        pauseMusicPlayer?.volume = configuredVolume(for: .pauseMusic)
        pauseMusicPlayer?.currentTime = 0
        pauseMusicPlayer?.prepareToPlay()
        stopEffects()
    }

    private func playPendingMainInterfaceEventsIfPossible() {
        guard stoppingGamePendingForMainInterface,
              !stoppingGameWaitsForVMShutdown,
              mainInterfaceIsActive,
              introHasFinished,
              applicationIsActive,
              hasInstalledPack else {
            return
        }
        stoppingGamePendingForMainInterface = false
        playEvent(.stoppingGame)
    }

    private func scheduleGameLaunchCompletion(
        after delay: TimeInterval,
        fadeDuration: TimeInterval
    ) {
        gameLaunchCompletionTask = Task { @MainActor [weak self] in
            if delay > 0 {
                try? await Task.sleep(
                    nanoseconds: UInt64(delay * 1_000_000_000)
                )
                guard !Task.isCancelled else { return }
                self?.fadeOutBackground(duration: fadeDuration)
            }
            try? await Task.sleep(
                nanoseconds: UInt64(max(0.01, fadeDuration) * 1_000_000_000)
            )
            guard let self, !Task.isCancelled else { return }
            self.gameLaunchCompletionTask = nil
            self.gameLaunchAudioActive = false
            self.reconcilePlayback()
        }
    }

    private func preloadPlayers() {
        var loadedData: [Sound: Data] = [:]
        var loadedEffects: [Sound: AVAudioPlayer] = [:]

        for sound in Sound.allCases {
            guard let url = soundURLs[sound] else { continue }
            do {
                // Default Data loading copies the encoded file bytes into RAM;
                // retaining `soundData` guarantees data-backed players never
                // fall back to reopening the bundle or imported-pack file.
                let data = try Data(contentsOf: url)
                let player = try AVAudioPlayer(data: data)
                player.numberOfLoops =
                    sound == .background || sound == .pauseMusic ? -1 : 0
                player.volume = configuredVolume(for: sound)
                guard player.prepareToPlay() else {
                    throw MenuAudioPackImportError.unplayableFile(url.lastPathComponent)
                }
                loadedData[sound] = data

                switch sound {
                case .startup:
                    startupPlayer = player
                case .background:
                    backgroundPlayer = player
                case .pauseMusic:
                    pauseMusicPlayer = player
                default:
                    loadedEffects[sound] = player
                }
            } catch {
                NSLog(
                    "[ARMSX2 UI Audio] %@ could not be preloaded: %@",
                    sound.rawValue,
                    error.localizedDescription
                )
            }
        }

        soundData = loadedData
        effectPlayers = loadedEffects
    }

    @discardableResult
    private func prepareAudioSessionIfNeeded() -> Bool {
#if !targetEnvironment(macCatalyst)
        // This flag is invalidated at every known lifecycle or ownership
        // boundary. Keep the directional-navigation hot path entirely out of
        // AVAudioSession once the active UI surface has claimed the session.
        if audioSessionPrepared {
            return true
        }

        let session = AVAudioSession.sharedInstance()
        let configurationMatches = session.category == .playback
            && session.mode == .default
            && session.categoryOptions == [.mixWithOthers]
        do {
            if !configurationMatches {
                // UI audio is an explicit user-selected feature, so it must
                // remain audible when the Ring/Silent switch is set to silent.
                try session.setCategory(
                    .playback,
                    mode: .default,
                    options: [.mixWithOthers]
                )
            }

            // Activation happens once per lifecycle/ownership transition.
            // Rapid navigation then only rewinds an already prepared player.
            try session.setActive(true)
            audioSessionPrepared = true
            return true
        } catch {
            // Do not permanently cache a failed activation. A later foreground
            // transition or controller event can then recover the session.
            audioSessionPrepared = false
            NSLog("[ARMSX2 UI Audio] audio session setup failed: %@", error.localizedDescription)
            return false
        }
#else
        audioSessionPrepared = true
        return true
#endif
    }

    private nonisolated static var storageRootURL: URL {
        let manager = FileManager.default
        let base = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? manager.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? manager.temporaryDirectory
        return base.appendingPathComponent("ARMSX2", isDirectory: true)
            .appendingPathComponent("UIAudioPack", isDirectory: true)
    }

    private nonisolated static var activeDirectoryURL: URL {
        storageRootURL.appendingPathComponent("Active", isDirectory: true)
    }

    private nonisolated static var customDirectoryURL: URL {
        storageRootURL.appendingPathComponent("Custom", isDirectory: true)
    }

    private static func clampedVolume(_ volume: Double) -> Double {
        min(max(volume, 0), 1)
    }

    private static func soundVolumeDefaultsKey(for sound: Sound) -> String {
        soundVolumeDefaultsKeyPrefix + sound.rawValue
    }

    private nonisolated static func resolveBundledSoundURLs() -> [Sound: URL] {
        var result: [Sound: URL] = [:]
        for (sound, resource) in bundledResources {
            if let url = Bundle.main.url(
                forResource: resource.name,
                withExtension: resource.pathExtension,
                subdirectory: resource.subdirectory
            ) {
                result[sound] = url
                continue
            }

            // Generated Xcode projects can flatten resource subdirectories.
            // Only resolve the explicitly mapped Android/shared filename so a
            // stale semantic iOS asset can never become the default again.
            if let url = Bundle.main.url(
                forResource: resource.name,
                withExtension: resource.pathExtension
            ) {
                result[sound] = url
            }
        }
        return result
    }

    private nonisolated static func containsRequiredSounds(_ urls: [Sound: URL]) -> Bool {
        requiredSounds.allSatisfy { urls[$0] != nil }
    }

    private nonisolated static func resolveSoundURLs(in directory: URL) -> [Sound: URL] {
        let manager = FileManager.default
        var result: [Sound: URL] = [:]
        for sound in Sound.allCases {
            for pathExtension in ["mp3", "wav"] {
                let candidate = directory
                    .appendingPathComponent(sound.rawValue, isDirectory: false)
                    .appendingPathExtension(pathExtension)
                if manager.fileExists(atPath: candidate.path) {
                    result[sound] = candidate
                    break
                }
            }
        }
        return result
    }

    private nonisolated static func installPack(from sourceURL: URL) throws -> Installation {
        guard sourceURL.pathExtension.caseInsensitiveCompare("zip") == .orderedSame else {
            throw MenuAudioPackImportError.notZip
        }

        let manager = FileManager.default
        let accessing = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if accessing {
                sourceURL.stopAccessingSecurityScopedResource()
            }
        }

        try manager.createDirectory(
            at: storageRootURL,
            withIntermediateDirectories: true
        )
        let stagingDirectory = storageRootURL.appendingPathComponent(
            ".staging-\(UUID().uuidString)",
            isDirectory: true
        )
        try manager.createDirectory(at: stagingDirectory, withIntermediateDirectories: false)
        defer {
            if manager.fileExists(atPath: stagingDirectory.path) {
                try? manager.removeItem(at: stagingDirectory)
            }
        }

        var extractionError: NSError?
        let extracted = ARMSX2Bridge.extractAudioPackArchive(
            at: sourceURL,
            to: stagingDirectory,
            error: &extractionError
        )
        guard extracted.count >= requiredSounds.count else {
            if let extractionError { throw extractionError }
            throw MenuAudioPackImportError.incompletePack
        }

        let resolvedURLs = resolveSoundURLs(in: stagingDirectory)
        guard containsRequiredSounds(resolvedURLs) else {
            throw MenuAudioPackImportError.incompletePack
        }
        for sound in Sound.allCases {
            guard let url = resolvedURLs[sound] else { continue }
            do {
                let player = try AVAudioPlayer(contentsOf: url)
                guard player.duration.isFinite, player.duration > 0 else {
                    throw MenuAudioPackImportError.unplayableFile(url.lastPathComponent)
                }
                guard player.prepareToPlay() else {
                    throw MenuAudioPackImportError.unplayableFile(url.lastPathComponent)
                }
            } catch let error as MenuAudioPackImportError {
                throw error
            } catch {
                throw MenuAudioPackImportError.unplayableFile(url.lastPathComponent)
            }
        }

        let activeDirectory = activeDirectoryURL
        let backupDirectory = storageRootURL.appendingPathComponent(
            ".previous-\(UUID().uuidString)",
            isDirectory: true
        )
        let hadPreviousPack = manager.fileExists(atPath: activeDirectory.path)
        if hadPreviousPack {
            try manager.moveItem(at: activeDirectory, to: backupDirectory)
        }

        do {
            try manager.moveItem(at: stagingDirectory, to: activeDirectory)
        } catch {
            if hadPreviousPack,
               manager.fileExists(atPath: backupDirectory.path),
               !manager.fileExists(atPath: activeDirectory.path) {
                try? manager.moveItem(at: backupDirectory, to: activeDirectory)
            }
            throw error
        }
        if hadPreviousPack {
            try? manager.removeItem(at: backupDirectory)
        }

        let archiveName = sourceURL.deletingPathExtension().lastPathComponent
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return Installation(displayName: archiveName.isEmpty ? "Audio Pack" : archiveName)
    }

    private nonisolated static func installCustomSound(
        from sourceURL: URL,
        for sound: Sound
    ) throws {
        let pathExtension = sourceURL.pathExtension.lowercased()
        guard pathExtension == "mp3" || pathExtension == "wav" else {
            throw MenuAudioPackImportError.unplayableFile(sourceURL.lastPathComponent)
        }

        let manager = FileManager.default
        let accessing = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if accessing {
                sourceURL.stopAccessingSecurityScopedResource()
            }
        }

        try manager.createDirectory(
            at: customDirectoryURL,
            withIntermediateDirectories: true
        )
        let stagingURL = customDirectoryURL
            .appendingPathComponent(".staging-\(UUID().uuidString)")
            .appendingPathExtension(pathExtension)
        defer {
            if manager.fileExists(atPath: stagingURL.path) {
                try? manager.removeItem(at: stagingURL)
            }
        }

        try manager.copyItem(at: sourceURL, to: stagingURL)
        do {
            let player = try AVAudioPlayer(contentsOf: stagingURL)
            guard player.duration.isFinite,
                  player.duration > 0,
                  player.prepareToPlay() else {
                throw MenuAudioPackImportError.unplayableFile(
                    sourceURL.lastPathComponent
                )
            }
        } catch let error as MenuAudioPackImportError {
            throw error
        } catch {
            throw MenuAudioPackImportError.unplayableFile(sourceURL.lastPathComponent)
        }

        let destinationURL = customDirectoryURL
            .appendingPathComponent(sound.rawValue)
            .appendingPathExtension(pathExtension)
        if manager.fileExists(atPath: destinationURL.path) {
            _ = try manager.replaceItemAt(destinationURL, withItemAt: stagingURL)
        } else {
            try manager.moveItem(at: stagingURL, to: destinationURL)
        }

        for alternateExtension in ["mp3", "wav"] where alternateExtension != pathExtension {
            let alternateURL = customDirectoryURL
                .appendingPathComponent(sound.rawValue)
                .appendingPathExtension(alternateExtension)
            if manager.fileExists(atPath: alternateURL.path) {
                try manager.removeItem(at: alternateURL)
            }
        }
    }

    private nonisolated static func removeCustomSoundFile(for sound: Sound) throws {
        let manager = FileManager.default
        for pathExtension in ["mp3", "wav"] {
            let url = customDirectoryURL
                .appendingPathComponent(sound.rawValue)
                .appendingPathExtension(pathExtension)
            if manager.fileExists(atPath: url.path) {
                try manager.removeItem(at: url)
            }
        }
    }
}
