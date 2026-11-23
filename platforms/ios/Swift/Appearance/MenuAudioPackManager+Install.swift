// MenuAudioPackManager+Install.swift — audio pack and custom sound files
// SPDX-License-Identifier: GPL-3.0+

import AVFoundation
import Foundation

extension MenuAudioPackManager {
    private struct Installation: Sendable {
        let displayName: String
    }

    private struct BundledResource: Sendable {
        let name: String
        let pathExtension: String
        let subdirectory: String
    }

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
            name: ARMSX2UISound.pauseMusic.rawValue,
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

    private nonisolated static var storageRootURL: URL {
        let base = URL.applicationSupportDirectory
        return base.appendingPathComponent("ARMSX2", isDirectory: true)
            .appendingPathComponent("UIAudioPack", isDirectory: true)
    }

    nonisolated static var activeDirectoryURL: URL {
        storageRootURL.appendingPathComponent("Active", isDirectory: true)
    }

    nonisolated static var customDirectoryURL: URL {
        storageRootURL.appendingPathComponent("Custom", isDirectory: true)
    }

    static func clampedVolume(_ volume: Double) -> Double {
        min(max(volume, 0), 1)
    }

    static func soundVolumeDefaultsKey(for sound: Sound) -> String {
        soundVolumeDefaultsKeyPrefix + sound.name.rawValue
    }

    nonisolated static func resolveBundledSoundURLs() -> [Sound: URL] {
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

    nonisolated static func containsRequiredSounds(_ urls: [Sound: URL]) -> Bool {
        requiredSounds.allSatisfy { urls[$0] != nil }
    }

    nonisolated static func resolveSoundURLs(in directory: URL) -> [Sound: URL] {
        let manager = FileManager.default
        var result: [Sound: URL] = [:]
        for sound in Sound.allCases {
            for pathExtension in ["mp3", "wav"] {
                let candidate = directory
                    .appendingPathComponent(sound.name.rawValue, isDirectory: false)
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
            .appendingPathComponent(sound.name.rawValue)
            .appendingPathExtension(pathExtension)
        if manager.fileExists(atPath: destinationURL.path) {
            _ = try manager.replaceItemAt(destinationURL, withItemAt: stagingURL)
        } else {
            try manager.moveItem(at: stagingURL, to: destinationURL)
        }

        for alternateExtension in ["mp3", "wav"] where alternateExtension != pathExtension {
            let alternateURL = customDirectoryURL
                .appendingPathComponent(sound.name.rawValue)
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
                .appendingPathComponent(sound.name.rawValue)
                .appendingPathExtension(pathExtension)
            if manager.fileExists(atPath: url.path) {
                try manager.removeItem(at: url)
            }
        }
    }
}
