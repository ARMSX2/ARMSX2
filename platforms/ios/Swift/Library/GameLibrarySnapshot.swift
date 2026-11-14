// GameLibrarySnapshot.swift — The library's metadata cache and its off-main scanner
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor
final class GameLibrarySnapshot {
	struct CachedGameMetadata: Codable, Sendable {
		let metadata: [String: String]
		let modificationDate: Date?
		let size: UInt64
	}

	static let shared = GameLibrarySnapshot()

	private var entriesByID: [String: ISOEntry] = [:]
	private var orderedEntries: [ISOEntry] = []
	// Per-game metadata keyed by entry id, checked against the ISO's modification time
	// (or size), so a reload reuses serial/CRC/region instead of reopening every ISO.
	// Persisted to disk so a cold launch does not rescan the whole library.
	private var metadataCache: [String: CachedGameMetadata] = [:]
	private var metadataCacheDirty = false
	private var metadataCacheLoaded = false
	private var metadataCacheLoadTask:
		Task<[String: CachedGameMetadata], Never>?
	private let persistenceQueue = DispatchQueue(
		label: "com.armsx2.ios.game-library-metadata",
		qos: .utility
	)
	private var libraryRefreshNeeded = true
	private var lastLibraryRefresh: Date?

	private static var persistenceURL: URL? {
		URL.applicationSupportDirectory.appendingPathComponent("LibraryMetadataCache.json")
	}

	private init() {}

	func loadMetadataCacheIfNeeded() async {
		guard !metadataCacheLoaded else { return }
		if metadataCacheLoadTask == nil {
			let url = Self.persistenceURL
			metadataCacheLoadTask = Task.detached(priority: .utility) {
				guard let url,
				      let data = try? Data(contentsOf: url),
				      let decoded = try? JSONDecoder().decode(
						[String: CachedGameMetadata].self,
						from: data
				      ) else { return [:] }
				return decoded
			}
		}
		guard let metadataCacheLoadTask else { return }
		let decoded = await metadataCacheLoadTask.value
		guard !Task.isCancelled else { return }
		// A rename/import can write fresh metadata while the persisted cache is
		// decoding. Keep the in-memory value whenever both contain the same key.
		for (key, value) in decoded where metadataCache[key] == nil {
			metadataCache[key] = value
		}
		metadataCacheLoaded = true
		self.metadataCacheLoadTask = nil
	}

	var entries: [ISOEntry] {
		orderedEntries
	}

	func existingEntries(merging currentEntries: [ISOEntry]) -> [String: ISOEntry] {
		var merged = entriesByID
		for entry in currentEntries {
			merged[entry.id] = entry
		}
		return merged
	}

	func update(_ entries: [ISOEntry]) {
		orderedEntries = entries
		var updatedEntriesByID: [String: ISOEntry] = [:]
		updatedEntriesByID.reserveCapacity(entries.count)
		for entry in entries {
			updatedEntriesByID[entry.id] = entry
		}
		entriesByID = updatedEntriesByID
	}

	func releaseEntriesForGameplay() {
		orderedEntries.removeAll(keepingCapacity: false)
		entriesByID.removeAll(keepingCapacity: false)
		libraryRefreshNeeded = true
	}

	func markLibraryRefreshNeeded() {
		libraryRefreshNeeded = true
	}

	func shouldRefreshLibrary(maximumAge: TimeInterval = 30) -> Bool {
		guard !orderedEntries.isEmpty,
		      !libraryRefreshNeeded,
		      let lastLibraryRefresh else { return true }
		return Date().timeIntervalSince(lastLibraryRefresh) > maximumAge
	}

	func markLibraryRefreshCompleted() {
		libraryRefreshNeeded = false
		lastLibraryRefresh = Date()
	}

	func storeMetadata(_ metadata: [String: String], modificationDate: Date?, size: UInt64, for id: String) {
		metadataCache[id] = CachedGameMetadata(metadata: metadata, modificationDate: modificationDate, size: size)
		metadataCacheDirty = true
	}

    func metadataSnapshot() -> [String: CachedGameMetadata] {
        metadataCache
    }

	/// Drops metadata for games no longer in the library so deleted files do not linger.
	func purgeMetadata(keeping ids: Set<String>) {
		let removedKeys = metadataCache.keys.filter { !ids.contains($0) }
		guard !removedKeys.isEmpty else { return }
		for key in removedKeys {
			metadataCache.removeValue(forKey: key)
		}
		metadataCacheDirty = true
	}

	/// Copies dirty metadata and serializes it off the main thread. The serial
	/// queue preserves write order when several library refreshes finish close together.
	func persistMetadataCache() {
		guard metadataCacheDirty else { return }
		metadataCacheDirty = false
		guard let url = Self.persistenceURL else { return }
		let snapshot = metadataCache
		persistenceQueue.async {
			try? FileManager.default.createDirectory(
				at: url.deletingLastPathComponent(),
				withIntermediateDirectories: true
			)
			guard let data = try? JSONEncoder().encode(snapshot) else { return }
			try? data.write(to: url, options: .atomic)
		}
	}
}

/// Immutable result of the filesystem/bridge portion of one library refresh.
/// Keeping Objective-C dictionaries inside the detached task avoids carrying
/// non-Sendable bridge objects back into SwiftUI's main-actor state.
struct GameLibraryScannedRecord: Sendable {
    let name: String
    let path: String
    let external: Bool
    let source: String?
    let size: UInt64
    let modificationDate: Date?
    let favorite: Bool
    let metadata: [String: String]
    let shouldStoreMetadata: Bool

    var id: String { path }
}

enum GameLibraryBackgroundScanner {
    static func scan(
        forceMetadataRefresh: Bool,
        allowFullMetadata: Bool,
        existingMetadata: [String: [String: String]],
        cachedMetadata: [String: GameLibrarySnapshot.CachedGameMetadata]
    ) -> [GameLibraryScannedRecord] {
        let fileManager = FileManager.default
        let rawEntries = ARMSX2Bridge.availableISOEntries()
        var result: [GameLibraryScannedRecord] = []
        result.reserveCapacity(rawEntries.count)

        for rawEntry in rawEntries {
            if Task.isCancelled { break }
            guard let name = rawEntry["name"] as? String,
                  let path = rawEntry["path"] as? String else {
                continue
            }
            let external = (rawEntry["external"] as? NSNumber)?.boolValue
                ?? (rawEntry["external"] as? Bool ?? false)
            let source = rawEntry["source"] as? String
            let bootName = external ? path : name
            let attributes = try? fileManager.attributesOfItem(atPath: path)
            let size = attributes?[.size] as? UInt64 ?? 0
            let modificationDate = attributes?[.modificationDate] as? Date
            let entryID = path

            let metadata: [String: String]
            let shouldStoreMetadata: Bool
            if forceMetadataRefresh && allowFullMetadata {
                metadata = ARMSX2Bridge.gameMetadata(forISO: bootName)
                shouldStoreMetadata = true
            } else if !allowFullMetadata {
                if let existing = existingMetadata[entryID] {
                    metadata = existing
                } else if let cached = validatedMetadata(
                    cachedMetadata[entryID],
                    modificationDate: modificationDate,
                    size: size,
                    requiresSettingsIdentity: true
                ) {
                    metadata = cached
                } else {
                    metadata = [
                        "fileTitle": (name as NSString).deletingPathExtension
                    ]
                }
                shouldStoreMetadata = false
            } else if let cached = validatedMetadata(
                cachedMetadata[entryID],
                modificationDate: modificationDate,
                size: size,
                requiresSettingsIdentity: true
            ) {
                metadata = cached
                shouldStoreMetadata = false
            } else {
                // ISO metadata parsing can open every image in a large library,
                // so it stays off the main actor.
                metadata = ARMSX2Bridge.gameMetadata(forISO: bootName)
                shouldStoreMetadata = true
            }

            result.append(
                GameLibraryScannedRecord(
                    name: name,
                    path: path,
                    external: external,
                    source: source,
                    size: size,
                    modificationDate: modificationDate,
                    favorite: ARMSX2Bridge.isFavorite(bootName),
                    metadata: metadata,
                    shouldStoreMetadata: shouldStoreMetadata
                )
            )
        }
        return result
    }

    private static func validatedMetadata(
        _ cached: GameLibrarySnapshot.CachedGameMetadata?,
        modificationDate: Date?,
        size: UInt64,
        requiresSettingsIdentity: Bool
    ) -> [String: String]? {
        guard let cached else { return nil }
        // Older cache entries could hold only fileTitle, which made Per-Game Settings
        // reopen the full image for the CRC on every presentation. Refresh them
        // once in the detached library scan instead.
        if requiresSettingsIdentity,
           PadLayoutGameIdentity.normalizedCRC(cached.metadata["crc"]).isEmpty {
            return nil
        }
        if let modificationDate,
           let cachedDate = cached.modificationDate,
           modificationDate == cachedDate {
            return cached.metadata
        }
        if modificationDate == nil, cached.size > 0, cached.size == size {
            return cached.metadata
        }
        return nil
    }
}

/// Releases the shared, menu-only portion of the game library independently of
/// SwiftUI view-disappearance timing. The persisted metadata index remains on
/// disk, so returning to the menu can rebuild entries without reopening every
/// active disc image.
@MainActor
enum GameLibraryRuntimeResources {
	static func activateForMenu() {
		CoverThumbnailCache.shared.activateForMenu()
	}

	static func releaseForGameplay() {
		GameLibrarySnapshot.shared.releaseEntriesForGameplay()
		CoverThumbnailCache.shared.releaseForGameplay()
	}
}
