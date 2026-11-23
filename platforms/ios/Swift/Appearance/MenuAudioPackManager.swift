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
    enum Sound: CaseIterable, Sendable {
        case startup
        case background
        case pauseMusic
        case navigation
        case select
        case switchToggleOn
        case switchToggleOff
        case `return`
        case stoppingGame
        case contextMenu
        case achievementToast
        case uiToast
        case tabTransition
        case launchGame
        case noJIT

        var name: ARMSX2UISound {
            switch self {
            case .startup: .startup
            case .background: .background
            case .pauseMusic: .pauseMusic
            case .navigation: .navigation
            case .select: .select
            case .switchToggleOn: .switchToggleOn
            case .switchToggleOff: .switchToggleOff
            case .return: .return
            case .stoppingGame: .stoppingGame
            case .contextMenu: .contextMenu
            case .achievementToast: .achievementToast
            case .uiToast: .uiToast
            case .tabTransition: .tabTransition
            case .launchGame: .launchGame
            case .noJIT: .noJit
            }
        }
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

    static let installedNameDefaultsKey = "ARMSX2.MenuAudioPack.InstalledName"
    private static let uiAudioVolumeDefaultsKey = "ARMSX2.MenuAudioPack.UIAudioVolume"
    private static let customAudioEnabledDefaultsKey =
        "ARMSX2.MenuAudioPack.CustomAudioEnabled"
    static let soundVolumeDefaultsKeyPrefix =
        "ARMSX2.MenuAudioPack.SoundVolume."
    private(set) var installedPackName: String?
    private(set) var usesBundledDefault = false
    var isWorking = false
    private(set) var uiAudioVolume = 1.0
    private(set) var customUIAudioEnabled = true
    private(set) var customSounds: Set<Sound> = []

    @ObservationIgnored private var soundURLs: [Sound: URL] = [:]
    @ObservationIgnored private var customSoundURLs: [Sound: URL] = [:]
    @ObservationIgnored var soundVolumes: [Sound: Double] = [:]
    // Kept for the pack's lifetime, so AVAudioPlayer(data:) never reopens a file mid-navigation.
    @ObservationIgnored private var soundData: [Sound: Data] = [:]
    @ObservationIgnored var introHasFinished = false
    @ObservationIgnored var interfaceIsActive = false
    @ObservationIgnored private var mainInterfaceIsActive = false
    @ObservationIgnored var applicationIsActive = true
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
    @ObservationIgnored var pendingTouchNavigationTask: Task<Void, Never>?
    @ObservationIgnored var stoppingGamePendingForMainInterface = false
    @ObservationIgnored var stoppingGameWaitsForVMShutdown = false
    @ObservationIgnored private var emulationAudioRecoveryTask: Task<Void, Never>?
    @ObservationIgnored var selectionSuppressedUntil: TimeInterval = 0
    @ObservationIgnored var lastEventPlaybackTimes: [Event: TimeInterval] = [:]

    var hasInstalledPack: Bool {
        installedPackName != nil && Self.containsRequiredSounds(soundURLs)
    }

    var shouldPlayBackgroundAudio: Bool {
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
        soundVolumes[sound] ?? 1
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
                sound.name.rawValue,
                error.localizedDescription
            )
        }
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
            // Quick Menu effects share the emulator's audio session; set it up once when
            // the surface opens, outside the directional-navigation path.
            _ = prepareAudioSessionIfNeeded()
        }
        reconcilePlayback()
    }

    /// Controls the music-bearing frontend independently from controller UI
    /// surfaces such as the in-game Quick Menu.
    func setMainInterfaceActive(_ active: Bool) {
        guard active != mainInterfaceIsActive else { return }
        mainInterfaceIsActive = active

        // SDL owns the audio session in game; reconfigure once per handoff, not per sound.
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

        // AVAudioPlayer can keep isPlaying=true after SDL drops the session; pausing
        // here makes reconcilePlayback() issue a fresh play().
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

    func reloadInstalledPack() {
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

            // Packs from before pause_music fall back to the bundled Quick Menu loop.
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

    func reconcilePlayback(shouldStartIntroAudio: Bool = false) {
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

    func startBackgroundIfNeeded() {
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

    func playEffect(_ sound: Sound) {
        guard prepareAudioSessionIfNeeded() else { return }
        guard let player = effectPlayers[sound] else { return }
        player.currentTime = 0
        player.play()
    }

    private func configuredVolume(for sound: Sound) -> Float {
        Float(Self.clampedVolume(uiAudioVolume * volume(for: sound)))
    }

    func applyConfiguredVolumes() {
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

    func stopAllPlayback() {
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
                // soundData keeps these bytes, so the player never reopens the file.
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
                    sound.name.rawValue,
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
}
