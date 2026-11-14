// GameLibraryModel.swift — The game list, its scan and the library's file actions
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Keeps transient library bookkeeping outside SwiftUI observation so scrolling
/// and refresh guards do not invalidate every visible card.
@MainActor
final class GameLibraryActivityState {
    var isScrolling = false
    var isLoading = false
    private var scheduledReloadTask: Task<Void, Never>?
    private var loadTask: Task<Void, Never>?
    private var cachedRunningName: String?
    private var cachedRunningIdentifiers: Set<String> = []

    func scheduleReload(
        after delay: Duration,
        operation: @escaping @MainActor () -> Void
    ) {
        scheduledReloadTask?.cancel()
        scheduledReloadTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled else { return }
            while self?.isScrolling == true {
                try? await Task.sleep(for: .milliseconds(50))
                guard !Task.isCancelled else { return }
            }
            operation()
            self?.scheduledReloadTask = nil
        }
    }

    func cancelScheduledReload() {
        scheduledReloadTask?.cancel()
        scheduledReloadTask = nil
    }

    func startLoad(
        _ operation: @escaping @MainActor () async -> Void
    ) {
        guard loadTask == nil else { return }
        isLoading = true
        loadTask = Task { @MainActor [weak self] in
            await operation()
            guard let self else { return }
            isLoading = false
            loadTask = nil
        }
    }

    func cancelLoad() {
        loadTask?.cancel()
        loadTask = nil
        isLoading = false
    }

    func runningIdentifiers(for gameName: String?) -> Set<String> {
        guard let gameName else {
            cachedRunningName = nil
            cachedRunningIdentifiers.removeAll(keepingCapacity: true)
            return []
        }
        guard cachedRunningName != gameName else {
            return cachedRunningIdentifiers
        }
        cachedRunningName = gameName
        cachedRunningIdentifiers = ISOEntry.normalizedIdentifierKeys(
            for: gameName
        )
        return cachedRunningIdentifiers
    }
}

@MainActor
@Observable
final class GameLibraryModel {
    static let shared = GameLibraryModel()

    var games: [ISOEntry] = []
    @ObservationIgnored var externalCoverAutoDownloadAttemptedIDs = Set<String>()
    @ObservationIgnored var coverWorkTask: Task<Void, Never>?
    let gameLibraryActivity = GameLibraryActivityState()
    private var session: GameSession { .shared }
    private var settings: SettingsStore { .shared }
    private var fileImporter: FileImportHandler { .shared }
    private var coverStore: CoverStore { .shared }
    private var externalLibrary: ExternalGameLibrary { .shared }

    private init() {}

    func handleExternalLibraryDidChange() {
        GameLibrarySnapshot.shared.markLibraryRefreshNeeded()
        scheduleLibraryReload(
            after: .milliseconds(120),
            autoDownloadExternalCovers: true
        )
    }

    func handleInitialContentDidChange() {
        GameLibrarySnapshot.shared.markLibraryRefreshNeeded()
        scheduleLibraryReload(
            after: .milliseconds(120),
            autoDownloadExternalCovers: false
        )
    }

    func renameGame(_ game: ISOEntry, to proposedName: String) {
        let fallbackName = URL(fileURLWithPath: game.name)
            .deletingPathExtension()
            .lastPathComponent
        GameDisplayNameStore.shared.setDisplayName(
            proposedName,
            for: game.bootName,
            fallback: fallbackName
        )
        loadGames()
    }

    func loadGames(
        autoDownloadExternalCovers: Bool = false,
        forceMetadataRefresh: Bool = false
    ) {
        let allowFullMetadata = session.runningGameName == nil
        let existingGames = GameLibrarySnapshot.shared.existingEntries(
            merging: games
        )
        let existingMetadata = existingGames.mapValues(\.metadata)
        externalLibrary.reload()
        gameLibraryActivity.startLoad { [self] in
            await GameLibrarySnapshot.shared.loadMetadataCacheIfNeeded()
            guard !Task.isCancelled else { return }
            let cachedMetadata = GameLibrarySnapshot.shared.metadataSnapshot()
            let scannedRecords = await Task.detached(priority: .userInitiated) {
                GameLibraryBackgroundScanner.scan(
                    forceMetadataRefresh: forceMetadataRefresh,
                    allowFullMetadata: allowFullMetadata,
                    existingMetadata: existingMetadata,
                    cachedMetadata: cachedMetadata
                )
            }.value
            guard !Task.isCancelled else { return }

            let coverLookupIndex = coverStore.makeLibraryCoverLookupIndex(
                gamePaths: scannedRecords.map {
                    URL(fileURLWithPath: $0.path)
                }
            )
            var loadedGames: [ISOEntry] = []
            loadedGames.reserveCapacity(scannedRecords.count)
            for record in scannedRecords {
                guard !Task.isCancelled else { return }
                let fileURL = URL(fileURLWithPath: record.path)
                if record.shouldStoreMetadata {
                    GameLibrarySnapshot.shared.storeMetadata(
                        record.metadata,
                        modificationDate: record.modificationDate,
                        size: record.size,
                        for: record.id
                    )
                }
                let coverURL = coverStore.coverURL(
                    forGameName: record.name,
                    gamePath: fileURL,
                    metadata: record.metadata,
                    using: coverLookupIndex
                )
                let existingCover = retainedCover(
                    from: existingGames[record.id]
                )
                let resolvedCoverURL = coverURL ?? existingCover?.url
                let coverSignature = CoverThumbnailCache.signature(
                    for: resolvedCoverURL
                ) ?? existingCover?.signature
                let fallbackDisplayName = fileURL
                    .deletingPathExtension()
                    .lastPathComponent
                // Keyed like favorites: the absolute path moves on a reinstall.
                let displayName = GameDisplayNameStore.shared.displayName(
                    for: record.external ? record.path : record.name,
                    fallback: fallbackDisplayName
                )
                loadedGames.append(
                    ISOEntry(
                        name: record.name,
                        fileURL: fileURL,
                        bootPath: record.external ? record.path : nil,
                        coverURL: resolvedCoverURL,
                        coverSignature: coverSignature,
                        metadata: record.metadata,
                        size: record.size,
                        isFavorite: record.favorite,
                        isExternal: record.external,
                        sourceName: record.source,
                        displayNameOverride: displayName
                    )
                )
            }
            loadedGames.sort { first, second in
                if first.isFavorite != second.isFavorite {
                    return first.isFavorite
                }
                return first.displayName.localizedCaseInsensitiveCompare(
                    second.displayName
                ) == .orderedAscending
            }

            if loadedGames != games {
                games = loadedGames
            }
            // Also repair/retry serial matches created by older builds. This is
            // asynchronous and the manager coalesces serials, so it never blocks
            // library rendering or rescans the game files.
            AutomaticCustomSkinManager.shared.enqueueMatches(
                for: loadedGames.map(\.coverInfo)
            )
            GameLibrarySnapshot.shared.purgeMetadata(
                keeping: Set(loadedGames.lazy.map(\.id))
            )
            GameLibrarySnapshot.shared.update(loadedGames)
            GameLibrarySnapshot.shared.markLibraryRefreshCompleted()
            GameLibrarySnapshot.shared.persistMetadataCache()
            if autoDownloadExternalCovers {
                autoDownloadExternalCoversIfNeeded()
            }
        }
    }

    /// Library-change notifications can arrive in bursts while files and
    /// covers are imported. Refresh once, after scrolling settles, so a disk
    /// scan cannot interrupt an active gesture or deceleration.
    func scheduleLibraryReload(
        after delay: Duration,
        autoDownloadExternalCovers: Bool
    ) {
        gameLibraryActivity.scheduleReload(after: delay) { [self] in
            loadGames(
                autoDownloadExternalCovers: autoDownloadExternalCovers
            )
        }
    }

	func restoreCachedGamesIfNeeded() {
		guard games.isEmpty else { return }
		let cachedGames = GameLibrarySnapshot.shared.entries
		if !cachedGames.isEmpty {
			games = cachedGames
		}
	}

	private func retainedCover(from entry: ISOEntry?) -> (url: URL, signature: String?)? {
		guard let url = entry?.coverURL else { return nil }
		guard FileManager.default.fileExists(atPath: url.path) else { return nil }
		return (url, entry?.coverSignature)
	}

    func downloadMissingCovers() {
        let targets = games.map(\.coverInfo)
        coverWorkTask?.cancel()
        coverWorkTask = Task { @MainActor in
            _ = await coverStore.downloadMissingCovers(for: targets)
			guard !Task.isCancelled else { return }
            loadGames()
        }
    }

    func downloadCover(for game: ISOEntry) {
        AutomaticCustomSkinManager.shared.enqueueMatches(for: [game.coverInfo])
        coverWorkTask?.cancel()
        coverWorkTask = Task { @MainActor in
            _ = await coverStore.downloadMissingCovers(for: [game.coverInfo])
			guard !Task.isCancelled else { return }
            loadGames()
        }
    }

	private func autoDownloadCovers(for importedGames: [FileImportHandler.ImportedGame]) {
		guard !importedGames.isEmpty else { return }

		let targets = importedGames.map { game in
			let metadata = ARMSX2Bridge.gameMetadata(forISO: game.name)
			let existingCover = coverStore.coverURL(forGameName: game.name, gamePath: game.fileURL, metadata: metadata)
			return CoverGameInfo(name: game.name, fileURL: game.fileURL, metadata: metadata, hasCover: existingCover != nil)
		}
		AutomaticCustomSkinManager.shared.enqueueMatches(for: targets)

		coverWorkTask?.cancel()
		coverWorkTask = Task { @MainActor in
			let summary = await coverStore.downloadMissingCovers(for: targets, showResult: false)
			guard !Task.isCancelled else { return }
			if summary.downloaded > 0 {
				loadGames()
			}
		}
	}

	private func autoDownloadExternalCoversIfNeeded() {
		guard session.runningGameName == nil else { return }

		let targets = games.filter { game in
			game.isExternal &&
			game.coverURL == nil &&
			!externalCoverAutoDownloadAttemptedIDs.contains(game.id)
		}
		guard !targets.isEmpty else { return }

		for game in targets {
			externalCoverAutoDownloadAttemptedIDs.insert(game.id)
		}

		let coverTargets = targets.map(\.coverInfo)
		AutomaticCustomSkinManager.shared.enqueueMatches(for: coverTargets)
		coverWorkTask?.cancel()
		coverWorkTask = Task { @MainActor in
			let summary = await coverStore.downloadMissingCovers(for: coverTargets, showResult: false)
			guard !Task.isCancelled else { return }
			if summary.downloaded > 0 {
				loadGames()
			}
		}
	}

	func isRunning(_ game: ISOEntry) -> Bool {
        let runningIdentifiers = gameLibraryActivity.runningIdentifiers(
            for: session.runningGameName
        )
        return !runningIdentifiers.isDisjoint(
            with: game.normalizedIdentifiers
        )
    }

	@discardableResult
	func toggleFavorite(_ game: ISOEntry) -> Bool {
		let key = game.bootName
		let current = games.first(where: { $0.id == game.id })?.isFavorite
            ?? ARMSX2Bridge.isFavorite(key)
        let updatedValue = !current
		ARMSX2Bridge.setFavorite(key, favorite: updatedValue)
        if let index = games.firstIndex(where: { $0.id == game.id }) {
            games[index].isFavorite = updatedValue
            games.sort { lhs, rhs in
                if lhs.isFavorite != rhs.isFavorite {
                    return lhs.isFavorite
                }
                return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
            }
        }
        GameLibrarySnapshot.shared.update(games)
		return updatedValue
	}

	func clearGameCache(_ game: ISOEntry) -> (title: String, message: String?) {
		("Clear Game Cache", ARMSX2Bridge.clearCache(forISO: game.bootName))
	}

	func deleteGameData(_ game: ISOEntry) -> (title: String, message: String?) {
		("Delete Game Data", ARMSX2Bridge.deleteGameData(forISO: game.bootName))
	}

	func deleteGame(_ game: ISOEntry, deleteData: Bool) -> (title: String, message: String?) {
		if isRunning(game) {
			return ("Delete Game", settings.localized("Stop this game before deleting it."))
		}

		let success = ARMSX2Bridge.deleteISO(game.bootName, deleteGameData: deleteData)
		if success {
			coverStore.removeManagedCovers(forGameNamed: game.name)
			loadGames()
        }
		return ("Delete Game", success ? settings.localized("Game deleted.") : settings.localized("Could not delete this game file."))
	}

    func importGames(_ urls: [URL], allowReplacingExistingFiles: Bool) -> [FileImportHandler.ImportedGame] {
        let importedGames = fileImporter.importURLs(
            urls,
            preferredDestination: .game,
            allowReplacingExistingFiles: allowReplacingExistingFiles
        )
        loadGames()
        autoDownloadCovers(for: importedGames)
        return importedGames
    }

    func setLinkedDisc(_ disc: ISOEntry?, for game: ISOEntry) {
        ARMSX2Bridge.setLinkedDiscPath(disc?.fileURL?.path ?? disc?.bootName, forELF: game.bootName)
        loadGames()
    }

    func removeCover(for game: ISOEntry) {
        coverStore.removeManagedCovers(forGameNamed: game.name)
        loadGames()
    }

    /// Gameplay needs the memory: the list and the scan state go while a game runs.
	func releaseLibraryResourcesForGameplay() {
		coverWorkTask?.cancel()
		coverWorkTask = nil
        gameLibraryActivity.isScrolling = false
        gameLibraryActivity.cancelScheduledReload()
        gameLibraryActivity.cancelLoad()
		games.removeAll(keepingCapacity: false)
		externalCoverAutoDownloadAttemptedIDs.removeAll(keepingCapacity: false)
		GameLibraryRuntimeResources.releaseForGameplay()
	}
}
