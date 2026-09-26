// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import UIKit
import Observation

/// Region-string to flag emoji mapping, shared by library cards and the game
/// info detail view. Returns an empty string for unknown regions so callers can
/// omit the flag without rendering a broken placeholder.
enum RegionFlag {
    static func emoji(for region: String?) -> String {
        guard let region, !region.isEmpty else { return "" }
        let value = region.lowercased()
        if value.contains("japan") || value.contains("ntsc-j") {
            return "🇯🇵"
        }
        if value.contains("usa") || value.contains("america") || value.contains("ntsc-u") {
            return "🇺🇸"
        }
        if value.contains("europe") || value.contains("pal") {
            return "🇪🇺"
        }
        if value.contains("korea") || value.contains("ntsc-k") {
            return "🇰🇷"
        }
        if value.contains("china") || value.contains("ntsc-c") {
            return "🇨🇳"
        }
        if value.contains("hong kong") || value.contains("ntsc-hk") {
            return "🇭🇰"
        }
        if value.contains("australia") {
            return "🇦🇺"
        }
        return ""
    }
}

struct ISOEntry: Identifiable, Equatable, Sendable {
	var id: String { bootPath ?? fileURL?.path ?? name }
	let name: String
	let fileURL: URL?
	let bootPath: String?
	let coverURL: URL?
	let coverSignature: String?
	let metadata: [String: String]
	let size: UInt64
	var isFavorite: Bool
    var isExternal: Bool = false
    var sourceName: String? = nil
    let displayName: String
    let sizeLabel: String
    let regionFlag: String?
    fileprivate let normalizedIdentifiers: Set<String>

    init(
        name: String,
        fileURL: URL?,
        bootPath: String?,
        coverURL: URL?,
        coverSignature: String?,
        metadata: [String: String],
        size: UInt64,
        isFavorite: Bool,
        isExternal: Bool = false,
        sourceName: String? = nil,
        displayNameOverride: String? = nil
    ) {
        self.name = name
        self.fileURL = fileURL
        self.bootPath = bootPath
        self.coverURL = coverURL
        self.coverSignature = coverSignature
        self.metadata = metadata
        self.size = size
        self.isFavorite = isFavorite
        self.isExternal = isExternal
        self.sourceName = sourceName
        let fallbackDisplayName = URL(fileURLWithPath: name)
            .deletingPathExtension()
            .lastPathComponent
        let override = displayNameOverride?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if let override, !override.isEmpty {
            displayName = override
        } else {
            displayName = fallbackDisplayName
        }

        let gigabytes = Double(size) / 1_073_741_824
        if gigabytes >= 1 {
            sizeLabel = String(format: "%.1f GB", gigabytes)
        } else {
            sizeLabel = String(
                format: "%.0f MB",
                Double(size) / 1_048_576
            )
        }

        let region = metadata["region"]?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let flag = RegionFlag.emoji(for: region)
        regionFlag = flag.isEmpty ? nil : flag

        let identifierCandidates: [String?] = [
            bootPath ?? fileURL?.path ?? name,
            name,
            fileURL?.path,
        ]
        let identifiers: [String] = identifierCandidates.compactMap { $0 }
        normalizedIdentifiers = identifiers.reduce(into: Set<String>()) {
            result, identifier in
            result.formUnion(Self.normalizedIdentifierKeys(for: identifier))
        }
    }

	var bootName: String {
		isExternal ? (bootPath ?? fileURL?.path ?? name) : name
	}

	var isELF: Bool {
		(bootPath ?? fileURL?.path ?? name).lowercased().hasSuffix(".elf")
	}

	var coverInfo: CoverGameInfo {
		CoverGameInfo(name: name, fileURL: fileURL, metadata: metadata, hasCover: coverURL != nil)
	}

    fileprivate static func normalizedIdentifierKeys(
        for value: String
    ) -> Set<String> {
        let normalized = (value.removingPercentEncoding ?? value)
            .replacingOccurrences(of: "\\", with: "/")
            .lowercased()
        guard !normalized.isEmpty else { return [] }
        let fileName = (normalized as NSString).lastPathComponent
        return fileName.isEmpty ? [normalized] : [normalized, fileName]
    }
}

@MainActor
private final class GameplayLaunchCardRegistry {
    private final class WeakCardView {
        weak var view: UIView?

        init(_ view: UIView) {
            self.view = view
        }
    }

    private var cards: [String: WeakCardView] = [:]

    func register(_ view: UIView, for gameID: String) {
        guard cards[gameID]?.view !== view else { return }
        cards[gameID] = WeakCardView(view)
    }

    func unregister(_ view: UIView, for gameID: String) {
        guard cards[gameID]?.view === view else { return }
        cards.removeValue(forKey: gameID)
    }

    func view(for gameID: String) -> UIView? {
        guard let view = cards[gameID]?.view else {
            cards.removeValue(forKey: gameID)
            return nil
        }
        return view
    }

    /// Resolves a touch against the small set of currently mounted lazy cards.
    /// This lets touch browsing use one long-press recognizer per scroll view
    /// instead of attaching one recognizer per card.
    func gameID(at location: CGPoint, in coordinateView: UIView) -> String? {
        cards = cards.filter { $0.value.view != nil }
        for (gameID, weakCard) in cards {
            guard let cardView = weakCard.view,
                  cardView.window != nil,
                  !cardView.isHidden,
                  cardView.alpha >= 0.01 else { continue }
            let localPoint = cardView.convert(location, from: coordinateView)
            if cardView.bounds.contains(localPoint) {
                return gameID
            }
        }
        return nil
    }

    func removeAll() {
        cards.removeAll(keepingCapacity: false)
    }
}

@MainActor
private struct GameplayLaunchCardRegistrationView: UIViewRepresentable {
    let gameID: String
    let registry: GameplayLaunchCardRegistry

    func makeCoordinator() -> Coordinator {
        Coordinator(gameID: gameID, registry: registry)
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        context.coordinator.registry.register(view, for: gameID)
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        if context.coordinator.gameID != gameID {
            context.coordinator.registry.unregister(
                uiView,
                for: context.coordinator.gameID
            )
            context.coordinator.gameID = gameID
        }
        context.coordinator.registry.register(uiView, for: gameID)
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: UIView,
        context: Context
    ) -> CGSize? {
        guard let width = proposal.width, let height = proposal.height else {
            return nil
        }
        return CGSize(width: width, height: height)
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Coordinator) {
        coordinator.registry.unregister(uiView, for: coordinator.gameID)
    }

    @MainActor
    final class Coordinator {
        var gameID: String
        let registry: GameplayLaunchCardRegistry

        init(gameID: String, registry: GameplayLaunchCardRegistry) {
            self.gameID = gameID
            self.registry = registry
        }
    }
}

private extension View {
    @MainActor
    func registerGameplayLaunchCard(
        for gameID: String,
        in registry: GameplayLaunchCardRegistry,
        isEnabled: Bool = true
    ) -> some View {
        overlay {
            if isEnabled {
                GeometryReader { geometry in
                    GameplayLaunchCardRegistrationView(
                        gameID: gameID,
                        registry: registry
                    )
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
                    .allowsHitTesting(false)
                }
            }
        }
    }
}

/// A single scroll-level long press for touch browsing. Card geometry already
/// exists for the launch transition, so resolving the touched ID here avoids
/// continuously adding and removing recognizers as lazy cards recycle.
@MainActor
private struct GameLibraryTouchLongPressObserver: UIViewRepresentable {
    let registry: GameplayLaunchCardRegistry
    let onTouchBegan: () -> Void
    let onRecognized: (String) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(
            registry: registry,
            onTouchBegan: onTouchBegan,
            onRecognized: onRecognized
        )
    }

    func makeUIView(context: Context) -> MarkerView {
        let view = MarkerView()
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        view.coordinator = context.coordinator
        context.coordinator.markerView = view
        return view
    }

    func updateUIView(_ uiView: MarkerView, context: Context) {
        context.coordinator.registry = registry
        context.coordinator.onTouchBegan = onTouchBegan
        context.coordinator.onRecognized = onRecognized
        context.coordinator.installIfNeeded(from: uiView)
    }

    static func dismantleUIView(
        _ uiView: MarkerView,
        coordinator: Coordinator
    ) {
        coordinator.uninstall()
        uiView.coordinator = nil
    }

    final class MarkerView: UIView {
        weak var coordinator: Coordinator?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            coordinator?.installIfNeeded(from: self)
        }

        override func didMoveToSuperview() {
            super.didMoveToSuperview()
            coordinator?.installIfNeeded(from: self)
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var registry: GameplayLaunchCardRegistry
        var onTouchBegan: () -> Void
        var onRecognized: (String) -> Void
        weak var markerView: MarkerView?
        private weak var scrollView: UIScrollView?
        private var pendingGameID: String?
        private lazy var recognizer: UILongPressGestureRecognizer = {
            let recognizer = UILongPressGestureRecognizer(
                target: self,
                action: #selector(handleLongPress(_:))
            )
            recognizer.minimumPressDuration = 0.42
            recognizer.allowableMovement = 10
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            recognizer.delegate = self
            return recognizer
        }()

        init(
            registry: GameplayLaunchCardRegistry,
            onTouchBegan: @escaping () -> Void,
            onRecognized: @escaping (String) -> Void
        ) {
            self.registry = registry
            self.onTouchBegan = onTouchBegan
            self.onRecognized = onRecognized
        }

        func installIfNeeded(from markerView: MarkerView) {
            var ancestor = markerView.superview
            var enclosingScrollView: UIScrollView?
            while let view = ancestor {
                if let candidate = view as? UIScrollView {
                    enclosingScrollView = candidate
                    break
                }
                ancestor = view.superview
            }
            guard scrollView !== enclosingScrollView else {
                self.markerView = markerView
                return
            }
            uninstall()
            self.markerView = markerView
            guard let enclosingScrollView else { return }
            enclosingScrollView.addGestureRecognizer(recognizer)
            scrollView = enclosingScrollView
        }

        func uninstall() {
            scrollView?.removeGestureRecognizer(recognizer)
            scrollView = nil
            markerView = nil
            pendingGameID = nil
        }

        @objc
        private func handleLongPress(_ recognizer: UILongPressGestureRecognizer) {
            guard recognizer.state == .began,
                  let pendingGameID else { return }
            onRecognized(pendingGameID)
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            guard let scrollView else { return false }
            let location = touch.location(in: scrollView)
            pendingGameID = registry.gameID(at: location, in: scrollView)
            guard pendingGameID != nil else { return false }
            onTouchBegan()
            return true
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer:
                UIGestureRecognizer
        ) -> Bool {
            true
        }
    }
}

/// Observes a stationary hold from the card's enclosing native scroll view.
/// Keeping the recognizer outside SwiftUI's card gesture arena lets a drag
/// that begins on the cover remain owned by List/ScrollView, including its
/// native deceleration and edge behavior.
@MainActor
private struct GameLibraryCardLongPressObserver: UIViewRepresentable {
    let onTouchBegan: () -> Void
    let onRecognized: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onTouchBegan: onTouchBegan, onRecognized: onRecognized)
    }

    func makeUIView(context: Context) -> MarkerView {
        let view = MarkerView()
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        view.coordinator = context.coordinator
        context.coordinator.markerView = view
        return view
    }

    func updateUIView(_ uiView: MarkerView, context: Context) {
        context.coordinator.onTouchBegan = onTouchBegan
        context.coordinator.onRecognized = onRecognized
        context.coordinator.installIfNeeded(from: uiView)
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: MarkerView,
        context: Context
    ) -> CGSize? {
        guard let width = proposal.width, let height = proposal.height else {
            return nil
        }
        return CGSize(width: width, height: height)
    }

    static func dismantleUIView(
        _ uiView: MarkerView,
        coordinator: Coordinator
    ) {
        coordinator.uninstall()
        uiView.coordinator = nil
    }

    final class MarkerView: UIView {
        weak var coordinator: Coordinator?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            coordinator?.installIfNeeded(from: self)
        }

        override func didMoveToSuperview() {
            super.didMoveToSuperview()
            coordinator?.installIfNeeded(from: self)
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var onTouchBegan: () -> Void
        var onRecognized: () -> Void
        weak var markerView: MarkerView?
        private weak var scrollView: UIScrollView?
        private lazy var recognizer: UILongPressGestureRecognizer = {
            let recognizer = UILongPressGestureRecognizer(
                target: self,
                action: #selector(handleLongPress(_:))
            )
            recognizer.minimumPressDuration = 0.42
            recognizer.allowableMovement = 10
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            recognizer.delegate = self
            return recognizer
        }()

        init(
            onTouchBegan: @escaping () -> Void,
            onRecognized: @escaping () -> Void
        ) {
            self.onTouchBegan = onTouchBegan
            self.onRecognized = onRecognized
        }

        func installIfNeeded(from markerView: MarkerView) {
            var ancestor = markerView.superview
            var enclosingScrollView: UIScrollView?
            while let view = ancestor {
                if let candidate = view as? UIScrollView {
                    enclosingScrollView = candidate
                    break
                }
                ancestor = view.superview
            }
            guard scrollView !== enclosingScrollView else {
                self.markerView = markerView
                return
            }
            uninstall()
            // `uninstall()` clears the old weak marker. Publish the current
            // card only after teardown so the first touch-down can claim this
            // gesture and suppress the backdrop L2/R2 shortcut.
            self.markerView = markerView
            guard let enclosingScrollView else { return }
            enclosingScrollView.addGestureRecognizer(recognizer)
            scrollView = enclosingScrollView
        }

        func uninstall() {
            scrollView?.removeGestureRecognizer(recognizer)
            scrollView = nil
            markerView = nil
        }

        @objc
        private func handleLongPress(_ recognizer: UILongPressGestureRecognizer) {
            guard recognizer.state == .began else { return }
            onRecognized()
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            guard let markerView,
                  markerView.window != nil,
                  !markerView.isHidden,
                  markerView.alpha >= 0.01,
                  let scrollView else { return false }
            let location = touch.location(in: scrollView)
            let markerLocation = markerView.convert(location, from: scrollView)
            let touchesCard = markerView.bounds.contains(markerLocation)
            if touchesCard {
                onTouchBegan()
            }
            return touchesCard
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer:
                UIGestureRecognizer
        ) -> Bool {
            true
        }
    }
}

private extension View {
    @MainActor
    func observeGameLibraryCardLongPress(
        onTouchBegan: @escaping () -> Void,
        _ onRecognized: @escaping () -> Void
    ) -> some View {
        background {
            GameLibraryCardLongPressObserver(
                onTouchBegan: onTouchBegan,
                onRecognized: onRecognized
            )
        }
    }
}

@MainActor
private final class GameLibrarySnapshot {
	fileprivate struct CachedGameMetadata: Codable, Sendable {
		let metadata: [String: String]
		let modificationDate: Date?
		let size: UInt64
	}

	static let shared = GameLibrarySnapshot()

	private var entriesByID: [String: ISOEntry] = [:]
	private var orderedEntries: [ISOEntry] = []
	// Per-game metadata keyed by entry id and validated against the ISO file's
	// modification time (falling back to file size), so reloading the library reuses
	// known serial/CRC/region instead of opening every ISO again. Persisted to disk
	// so a cold app launch does not rescan the whole library.
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
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
			?? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
		return base?.appendingPathComponent("LibraryMetadataCache.json")
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

	/// Returns cached metadata when the file is unchanged: a matching modification
	/// time, or a matching size when the modification time is unavailable (some
	/// external folders report no stable date).
	func cachedMetadata(for id: String, modificationDate: Date?, size: UInt64) -> [String: String]? {
		guard let cached = metadataCache[id] else { return nil }
		if let fileDate = modificationDate, let cachedDate = cached.modificationDate, fileDate == cachedDate {
			return cached.metadata
		}
		if modificationDate == nil, cached.size > 0, cached.size == size {
			return cached.metadata
		}
		return nil
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
private struct GameLibraryScannedRecord: Sendable {
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

private enum GameLibraryBackgroundScanner {
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
        // Older cache entries could contain only fileTitle. Treating those as
        // complete forced Per-Game Settings to rediscover the CRC by opening the
        // full image on every presentation. Refresh them once in the existing
        // detached library scan instead.
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

private struct RunningCoverGlowPhase {
    let radius: CGFloat
    let x: CGFloat
    let y: CGFloat
    let opacity: Double

    static let initial = RunningCoverGlowPhase(
        radius: 9,
        x: -0.5,
        y: 0.5,
        opacity: 0.35
    )

    static func random() -> RunningCoverGlowPhase {
        RunningCoverGlowPhase(
            radius: .random(in: 7.5...12.5),
            x: .random(in: -2.25...2.25),
            y: .random(in: -2.25...2.25),
            opacity: .random(in: 0.20...0.45)
        )
    }
}

/// Instantiated only for a cover that is actively glowing. Ordinary cards do
/// not own animation state, shadow passes, or background tasks.
private struct GameCoverThemeGlowModifier: ViewModifier {
    let primaryColor: Color
    let secondaryColor: Color
    @State private var phase = RunningCoverGlowPhase.initial

    func body(content: Content) -> some View {
        content
            .shadow(
                color: primaryColor.opacity(phase.opacity),
                radius: phase.radius,
                x: phase.x,
                y: phase.y
            )
            .shadow(
                color: secondaryColor.opacity(phase.opacity * 0.76),
                radius: phase.radius * 0.68,
                x: -phase.x * 0.7,
                y: -phase.y * 0.7
            )
            .task {
                while !Task.isCancelled {
                    let duration = Double.random(in: 0.8...1.35)
                    withAnimation(.easeInOut(duration: duration)) {
                        phase = .random()
                    }
                    try? await Task.sleep(
                        for: .milliseconds(Int(duration * 1_000))
                    )
                }
            }
    }
}

/// Keeps transient library bookkeeping outside SwiftUI observation so scrolling
/// and refresh guards do not invalidate every visible card.
@MainActor
private final class GameLibraryActivityState {
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

private struct GameLibraryScrollPhaseModifier: ViewModifier {
    let activity: GameLibraryActivityState

    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.onScrollPhaseChange { _, newPhase in
                activity.isScrolling = newPhase.isScrolling
            }
        } else {
            content
        }
    }
}

private extension View {
    func trackGameLibraryScrollPhase(
        _ activity: GameLibraryActivityState
    ) -> some View {
        modifier(GameLibraryScrollPhaseModifier(activity: activity))
    }
}

private struct RunningStatusText: View {
    let isStopping: Bool
    let runningLabel: String
    let gameTitle: String
    let stoppingLabel: String
    let stoppingFont: Font
    let statusColor: Color
    let titleColor: Color
    var centered = false

    @ViewBuilder
    var body: some View {
        if isStopping {
            Text(stoppingLabel)
                .font(stoppingFont.bold())
                .uiCriticalForegroundStyle()
                .lineLimit(1)
                .minimumScaleFactor(0.72)
                .multilineTextAlignment(.center)
                .transition(
                    .opacity
                        .combined(with: .scale(scale: 0.82))
                        .combined(with: .offset(x: 8))
                )
        } else {
            VStack(
                alignment: centered ? .center : .leading,
                spacing: 3
            ) {
                Text(runningLabel)
                    .font(.caption)
                    .foregroundStyle(statusColor)
                Text(gameTitle)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(titleColor)
                    .lineLimit(centered ? 2 : 1)
                    .truncationMode(.tail)
                    .multilineTextAlignment(centered ? .center : .leading)
            }
            .transition(
                .opacity
                    .combined(with: .scale(scale: 0.88))
                    .combined(with: .offset(x: -8))
            )
        }
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

@MainActor
@Observable
private final class GameLibraryControllerCardFocusState {
    var isFocused: Bool

    init(isFocused: Bool) {
        self.isFocused = isFocused
    }
}

private struct GameLibrarySelectionScrollRequest: Equatable {
    let sequence: UInt64
    let gameID: String
}

private enum GameLibraryFavoriteScrollDestination: Equatable {
    case firstGame(String)
    case game(String)
    case nowRunning
}

private struct GameLibraryFavoriteScrollRequest: Equatable {
    let sequence: UInt64
    let destination: GameLibraryFavoriteScrollDestination
}

private let gameLibraryNowRunningFocusID = "game-library-now-running"
private let gameLibraryControllerMemoryScope = "menu.games.content"
private let gameLibraryControllerCardMemoryScope = "menu.games.last-card"

private enum GameLibraryNowRunningControllerAction: Int, CaseIterable {
    case resume
    case stop
}

@MainActor
@Observable
private final class GameLibraryNowRunningControllerFocusState {
    var selectedAction: GameLibraryNowRunningControllerAction?
}

private struct GameLibraryNowRunningControllerFocusModifier: ViewModifier {
    let state: GameLibraryNowRunningControllerFocusState
    let action: GameLibraryNowRunningControllerAction
    let controllerInput: MenuControllerInputRouter?
    let menuTabIsActive: Bool

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.uiAccentColour) private var accentColour

    private var isFocused: Bool {
        controllerInput != nil
            && controllerInput?.isControllerNavigationEnabled == true
            && menuTabIsActive
            && controllerInput?.navigationZone == .library
            && state.selectedAction == action
    }

    func body(content: Content) -> some View {
        Group {
            if isFocused {
                content
                    .foregroundStyle(action == .stop ? Color.red : Color.white)
                    .background {
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .fill(Color.black.opacity(0.2))
                    }
                    .controllerFocusBoxPresentation(
                        isVisible: true,
                        cornerRadius: 12
                    )
                    .scaleEffect(reduceMotion ? 1 : 1.01)
                    .shadow(color: .black.opacity(0.1), radius: 4, y: 2)
            } else {
                content
                    .foregroundStyle(action == .stop ? Color.red : Color.white)
            }
        }
            .focusEffectDisabled()
            .controllerNavigationOrbTarget(
                id: "library.now-running.\(action.rawValue)",
                isActive: isFocused,
                palette: .blue,
                inset: 2,
                orbScale: 0.85,
                priority: 20
            )
            .tint(action == .stop ? .red : accentColour)
            .zIndex(isFocused ? 20 : 0)
            .animation(
                reduceMotion
                    ? .linear(duration: 0.1)
                    : .smooth(duration: 0.16, extraBounce: 0),
                value: isFocused
            )
    }
}

@MainActor
@Observable
private final class GameLibraryControllerFocusState {
    @ObservationIgnored private var storedSelectedGameID: String?
    @ObservationIgnored private var storedIsActive = false
    @ObservationIgnored private var isLibraryZone = true
    @ObservationIgnored private var isLibraryVisible = true
    @ObservationIgnored private var cardStates: [String: GameLibraryControllerCardFocusState] = [:]
    @ObservationIgnored private var gameIndexByID: [String: Int] = [:]
    @ObservationIgnored private var scrollSequence: UInt64 = 0
    @ObservationIgnored private var suppressNextSelectionScroll = false

    var selectionScrollRequest: GameLibrarySelectionScrollRequest?

    var selectedGameID: String? {
        get { storedSelectedGameID }
        set {
            guard storedSelectedGameID != newValue else { return }
            let previous = storedSelectedGameID
            storedSelectedGameID = newValue
            refreshCard(previous)
            refreshCard(newValue)
            publishSelectionScrollIfNeeded()
        }
    }

    var isActive: Bool {
        get { storedIsActive }
        set {
            guard storedIsActive != newValue else { return }
            storedIsActive = newValue
            refreshCard(presentedGameID)
        }
    }

    func setLibraryZoneActive(_ active: Bool) {
        guard isLibraryZone != active else { return }
        isLibraryZone = active
        refreshCard(presentedGameID)
    }

    func setLibraryVisible(_ visible: Bool) {
        guard isLibraryVisible != visible else { return }
        isLibraryVisible = visible
        refreshCard(presentedGameID)
    }

    func cardState(for gameID: String) -> GameLibraryControllerCardFocusState {
        if let state = cardStates[gameID] {
            return state
        }
        let state = GameLibraryControllerCardFocusState(
            isFocused: isFocused(gameID: gameID)
        )
        cardStates[gameID] = state
        return state
    }

    func isFocused(gameID: String) -> Bool {
        guard isLibraryVisible else { return false }
        return storedIsActive
            && isLibraryZone
            && storedSelectedGameID == gameID
    }

    func updateGameIDs(_ gameIDs: [String]) {
        var updatedIndex: [String: Int] = [:]
        updatedIndex.reserveCapacity(gameIDs.count)
        for (index, gameID) in gameIDs.enumerated() {
            updatedIndex[gameID] = index
        }
        gameIndexByID = updatedIndex
        let retainedIDs = Set(gameIDs)
        cardStates = cardStates.filter { retainedIDs.contains($0.key) }
    }

    func releaseCardStates() {
        // The semantic selection is persisted separately. Per-card observable
        // wrappers are only a controller presentation cache and can be rebuilt
        // lazily if a gamepad reconnects.
        cardStates.removeAll(keepingCapacity: false)
    }

    func index(for gameID: String) -> Int? {
        gameIndexByID[gameID]
    }

    func requestSelectionScroll() {
        publishSelectionScrollIfNeeded()
    }

    func selectWithoutScrolling(_ gameID: String) {
        guard storedSelectedGameID != gameID else { return }
        suppressNextSelectionScroll = true
        selectedGameID = gameID
    }

    private func refreshCard(_ gameID: String?) {
        guard let gameID, let state = cardStates[gameID] else { return }
        state.isFocused = isFocused(gameID: gameID)
    }

    private var presentedGameID: String? {
        storedSelectedGameID
    }

    private func publishSelectionScrollIfNeeded() {
        if suppressNextSelectionScroll {
            suppressNextSelectionScroll = false
            return
        }
        guard storedIsActive, isLibraryZone, isLibraryVisible,
              let gameID = storedSelectedGameID else { return }
        scrollSequence &+= 1
        selectionScrollRequest = GameLibrarySelectionScrollRequest(
            sequence: scrollSequence,
            gameID: gameID
        )
    }
}

private struct GameLibraryControllerCommandListener: View {
    let controllerInput: MenuControllerInputRouter?
    let focusState: GameLibraryControllerFocusState
    let libraryVisible: Bool
    let onCommand: (MenuControllerInputEvent) -> Void
    let onLibraryEntry: (Bool) -> Void
    let onFocusRelease: () -> Void
    let onRightStickScrollEnded: () -> Void
    let onInputAvailabilityChanged: () -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput?.latestEvent) { _, event in
                guard let event else { return }
                onCommand(event)
            }
            .onChange(of: controllerInput?.latestLibraryEntryRequest) { _, request in
                guard let request else { return }
                onLibraryEntry(request.preferLast)
            }
            .onChange(of: controllerInput?.focusReleaseSequence) { _, _ in
                onFocusRelease()
            }
            .onChange(
                of: controllerInput?.rightStickScrollEndSequence
            ) { _, _ in
                onRightStickScrollEnded()
            }
            .onChange(
                of: controllerInput?.isControllerNavigationEnabled
            ) { _, enabled in
                if enabled == false { onFocusRelease() }
                onInputAvailabilityChanged()
            }
            .onChange(of: controllerInput?.navigationZone, initial: true) { _, zone in
                focusState.setLibraryZoneActive(zone == .library)
                onInputAvailabilityChanged()
            }
            .onChange(of: controllerInput?.isNavigationCaptured, initial: true) { _, _ in
                onInputAvailabilityChanged()
            }
            .onChange(of: libraryVisible, initial: true) { _, visible in
                focusState.setLibraryVisible(visible)
                onInputAvailabilityChanged()
            }
    }
}

@MainActor
@Observable
private final class GameLibraryControllerScrollAvailability {
    var isEnabled = false
}

private struct GameLibraryControllerRightStickTarget: View {
    let controllerInput: MenuControllerInputRouter?
    let availability: GameLibraryControllerScrollAvailability
    let axes: MenuControllerScrollAxes
    var priority = 10
    var pointsPerSecond: CGFloat = 760
    var onReachedLeadingEdge: (@MainActor () -> Void)?

    var body: some View {
        ControllerRightStickScrollTarget(
            controllerInput: controllerInput,
            axes: axes,
            priority: priority,
            isEnabled: availability.isEnabled,
            pointsPerSecond: pointsPerSecond,
            onReachedLeadingEdge: onReachedLeadingEdge
        )
    }
}

@MainActor
@Observable
private final class GameLibraryScrollRestoreState {
    var topRequestSequence: UInt64 = 0
    @ObservationIgnored private var consumedTopRequestSequence: UInt64 = 0

    func requestTopRestore() {
        topRequestSequence &+= 1
    }

    func consumeTopRestore(_ sequence: UInt64) -> Bool {
        guard sequence > consumedTopRequestSequence else { return false }
        consumedTopRequestSequence = sequence
        return true
    }
}

private enum GameLibraryFocusScrollAxis {
    case horizontal
    case vertical
}

/// Owns controller-driven Game Library centering on one display-link clock.
/// Repeated SwiftUI `scrollTo` animations can interrupt one another and leave
/// semantic focus several cards ahead of the viewport. Directly retargeting the
/// live UIKit offset keeps held navigation centered without a competing binding.
@MainActor
private final class GameLibraryFocusScrollCoordinator: NSObject {
    private final class WeakTarget {
        weak var view: UIView?

        init(_ view: UIView) {
            self.view = view
        }
    }

    private var lastFocusRequestTime: TimeInterval?
    private var targets: [String: WeakTarget] = [:]
    private weak var scrollView: UIScrollView?
    private var displayLink: CADisplayLink?
    private var animationStartOffset = CGPoint.zero
    private var animationDestination = CGPoint.zero
    private var animationStartTime: CFTimeInterval = 0
    private var animationDuration: CFTimeInterval = 0

    func register(_ view: UIView, for gameID: String) {
        guard targets[gameID]?.view !== view else { return }
        targets[gameID] = WeakTarget(view)
    }

    func unregister(_ view: UIView, for gameID: String) {
        guard targets[gameID]?.view === view else { return }
        targets.removeValue(forKey: gameID)
    }

    func focus(
        gameID: String,
        proxy: ScrollViewProxy,
        reduceMotion: Bool,
        axis: GameLibraryFocusScrollAxis,
        usesCompactAnimation: Bool
    ) {
        let now = ProcessInfo.processInfo.systemUptime
        let requestInterval = lastFocusRequestTime.map { now - $0 }
        lastFocusRequestTime = now

        let duration: Double
        if usesCompactAnimation {
            // Held navigation accelerates progressively. Finish each center
            // move before the observed repeat cadence instead of letting old
            // cover-flow animations accumulate behind semantic focus.
            if let requestInterval, requestInterval < 0.18 {
                duration = max(0.024, min(0.075, requestInterval * 0.75))
            } else {
                duration = 0.11
            }
        } else {
            if let requestInterval, requestInterval < 0.22 {
                duration = max(0.024, min(0.10, requestInterval * 0.75))
            } else {
                duration = 0.16
            }
        }

        if let target = targetView(for: gameID),
           let targetScrollView = enclosingScrollView(for: target),
           let destination = centeredDestination(
               for: target,
               in: targetScrollView,
               axis: axis
           ) {
            if reduceMotion || duration <= 0 {
                cancelAnimation()
                targetScrollView.setContentOffset(destination, animated: false)
            } else {
                animate(
                    targetScrollView,
                    to: destination,
                    duration: duration
                )
            }
            return
        }

        // An initial/restored ID can be outside the lazy mount window. Mount it
        // once through its stable ID; adjacent held-navigation targets then use
        // the direct 120 Hz path above.
        if reduceMotion {
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                proxy.scrollTo(gameID, anchor: .center)
            }
            return
        }

        withAnimation(.smooth(duration: duration, extraBounce: 0)) {
            proxy.scrollTo(gameID, anchor: .center)
        }
    }

    func cancel() {
        cancelAnimation()
        targets.removeAll(keepingCapacity: false)
    }

    func stopAnimation() {
        cancelAnimation()
    }

    /// Returns the mounted card closest to the visible center after free
    /// right-stick scrolling. Fully visible cards always win over clipped
    /// cards, keeping list, grid, and landscape cover flow deterministic.
    func centeredVisibleGameID(axis: GameLibraryFocusScrollAxis) -> String? {
        let candidates = targets.compactMap {
            gameID, weakTarget -> (
                gameID: String,
                centerDistance: CGFloat,
                hiddenExtent: CGFloat,
                fullyVisible: Bool
            )? in
            guard let target = weakTarget.view,
                  target.window != nil,
                  isEffectivelyVisible(target),
                  let scrollView = enclosingScrollView(for: target),
                  isEffectivelyVisible(scrollView) else { return nil }
            let targetFrame = target.convert(target.bounds, to: scrollView)
            let visibleFrame = visibleViewportFrame(in: scrollView)
            let intersection = targetFrame.intersection(visibleFrame)
            guard !intersection.isNull, !intersection.isInfinite else {
                return nil
            }

            let targetExtent: CGFloat
            let visibleExtent: CGFloat
            let centerDistance: CGFloat
            switch axis {
            case .horizontal:
                targetExtent = targetFrame.width
                visibleExtent = intersection.width
                centerDistance = abs(targetFrame.midX - visibleFrame.midX)
            case .vertical:
                targetExtent = targetFrame.height
                visibleExtent = intersection.height
                centerDistance = abs(targetFrame.midY - visibleFrame.midY)
            }
            guard targetExtent > 1, visibleExtent > 1 else { return nil }
            let fullyVisible = targetExtent <= {
                switch axis {
                case .horizontal: visibleFrame.width
                case .vertical: visibleFrame.height
                }
            }() + 1 && visibleExtent >= targetExtent - 1
            return (
                gameID,
                centerDistance,
                max(0, targetExtent - visibleExtent),
                fullyVisible
            )
        }
        let preferred = candidates.contains(where: \.fullyVisible)
            ? candidates.filter(\.fullyVisible)
            : candidates
        return preferred.min { lhs, rhs in
            let lhsScore = lhs.centerDistance + lhs.hiddenExtent
            let rhsScore = rhs.centerDistance + rhs.hiddenExtent
            if abs(lhsScore - rhsScore) > 0.5 { return lhsScore < rhsScore }
            return lhs.gameID < rhs.gameID
        }?.gameID
    }

    private func targetView(for gameID: String) -> UIView? {
        guard let view = targets[gameID]?.view else {
            targets.removeValue(forKey: gameID)
            return nil
        }
        return view.window == nil ? nil : view
    }

    private func enclosingScrollView(for view: UIView) -> UIScrollView? {
        var ancestor = view.superview
        while let current = ancestor {
            if let scrollView = current as? UIScrollView {
                return scrollView
            }
            ancestor = current.superview
        }
        return nil
    }

    private func isEffectivelyVisible(_ view: UIView) -> Bool {
        guard let window = view.window else { return false }
        var candidate: UIView? = view
        while let current = candidate {
            if current.isHidden
                || current.alpha < 0.01
                // Geometry probes are intentionally accessibility-hidden. Only
                // a hidden ancestor means the containing library presentation
                // is inactive and should be excluded from center restoration.
                || (current !== view && current.accessibilityElementsHidden) {
                return false
            }
            candidate = current.superview
        }
        let frame = view.convert(view.bounds, to: window)
        return frame.width > 1 && frame.height > 1
            && frame.intersects(window.bounds)
    }

    private func visibleViewportFrame(in scrollView: UIScrollView) -> CGRect {
        let inset = scrollView.adjustedContentInset
        return CGRect(
            x: scrollView.contentOffset.x + inset.left,
            y: scrollView.contentOffset.y + inset.top,
            width: max(1, scrollView.bounds.width - inset.left - inset.right),
            height: max(1, scrollView.bounds.height - inset.top - inset.bottom)
        )
    }

    private func centeredDestination(
        for target: UIView,
        in scrollView: UIScrollView,
        axis: GameLibraryFocusScrollAxis
    ) -> CGPoint? {
        guard target.window != nil else { return nil }
        let frame = target.convert(target.bounds, to: scrollView)
        guard !frame.isNull, !frame.isInfinite,
              frame.width > 1, frame.height > 1 else { return nil }
        let inset = scrollView.adjustedContentInset
        let minimumX = -inset.left
        let minimumY = -inset.top
        let maximumX = max(
            minimumX,
            scrollView.contentSize.width - scrollView.bounds.width + inset.right
        )
        let maximumY = max(
            minimumY,
            scrollView.contentSize.height - scrollView.bounds.height + inset.bottom
        )
        var destination = scrollView.contentOffset
        switch axis {
        case .horizontal:
            let visibleCenter = (inset.left + scrollView.bounds.width - inset.right) / 2
            destination.x = min(
                maximumX,
                max(minimumX, frame.midX - visibleCenter)
            )
        case .vertical:
            let visibleCenter = (inset.top + scrollView.bounds.height - inset.bottom) / 2
            destination.y = min(
                maximumY,
                max(minimumY, frame.midY - visibleCenter)
            )
        }
        return destination
    }

    private func animate(
        _ scrollView: UIScrollView,
        to destination: CGPoint,
        duration: Double
    ) {
        self.scrollView = scrollView
        animationStartOffset = scrollView.contentOffset
        animationDestination = destination
        animationStartTime = CACurrentMediaTime()
        animationDuration = max(1.0 / 120.0, duration)

        if let displayLink {
            displayLink.isPaused = false
            return
        }
        let link = CADisplayLink(
            target: self,
            selector: #selector(advanceAnimation(_:))
        )
        UIFrameRateSettings.shared.configuration.apply(
            to: link,
            domain: .controllerNavigation
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    @objc private func advanceAnimation(_ link: CADisplayLink) {
        guard let scrollView, scrollView.window != nil else {
            cancelAnimation()
            return
        }
        let progress = min(
            1,
            max(0, (link.timestamp - animationStartTime) / animationDuration)
        )
        let remaining = 1 - progress
        let eased = 1 - (remaining * remaining * remaining)
        let offset = CGPoint(
            x: animationStartOffset.x
                + (animationDestination.x - animationStartOffset.x) * eased,
            y: animationStartOffset.y
                + (animationDestination.y - animationStartOffset.y) * eased
        )
        scrollView.setContentOffset(offset, animated: false)
        if progress >= 1 {
            scrollView.setContentOffset(animationDestination, animated: false)
            cancelAnimation()
        }
    }

    private func cancelAnimation() {
        displayLink?.invalidate()
        displayLink = nil
        scrollView = nil
        animationDuration = 0
    }
}

@MainActor
private struct GameLibraryFocusScrollRegistrationView: UIViewRepresentable {
    let gameID: String
    let coordinator: GameLibraryFocusScrollCoordinator

    func makeCoordinator() -> Coordinator {
        Coordinator(gameID: gameID, focusScrollCoordinator: coordinator)
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        context.coordinator.focusScrollCoordinator.register(view, for: gameID)
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        if context.coordinator.gameID != gameID {
            context.coordinator.focusScrollCoordinator.unregister(
                uiView,
                for: context.coordinator.gameID
            )
            context.coordinator.gameID = gameID
        }
        context.coordinator.focusScrollCoordinator.register(uiView, for: gameID)
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Coordinator) {
        coordinator.focusScrollCoordinator.unregister(
            uiView,
            for: coordinator.gameID
        )
    }

    @MainActor
    final class Coordinator {
        var gameID: String
        let focusScrollCoordinator: GameLibraryFocusScrollCoordinator

        init(
            gameID: String,
            focusScrollCoordinator: GameLibraryFocusScrollCoordinator
        ) {
            self.gameID = gameID
            self.focusScrollCoordinator = focusScrollCoordinator
        }
    }
}

private extension View {
    @MainActor
    func registerGameLibraryFocusScrollTarget(
        for gameID: String,
        in coordinator: GameLibraryFocusScrollCoordinator,
        isEnabled: Bool = true
    ) -> some View {
        overlay {
            if isEnabled {
                GeometryReader { geometry in
                    GameLibraryFocusScrollRegistrationView(
                        gameID: gameID,
                        coordinator: coordinator
                    )
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
                    .allowsHitTesting(false)
                }
            }
        }
    }
}

@MainActor
private final class GameLibraryCoverFlowPreheater {
    private struct Request {
        let items: [CoverThumbnailPreheatItem]
        let width: CGFloat
        let height: CGFloat
        let scale: CGFloat
    }

    private var task: Task<Void, Never>?
    private var pendingRequest: Request?

    func schedule(
        items: [CoverThumbnailPreheatItem],
        width: CGFloat,
        height: CGFloat,
        scale: CGFloat
    ) {
        guard !items.isEmpty else {
            return
        }
        pendingRequest = Request(
            items: items,
            width: width,
            height: height,
            scale: scale
        )
        guard task == nil else { return }
        task = Task(priority: .utility) { [weak self] in
            await self?.drainRequests()
        }
    }

    private func drainRequests() async {
        while !Task.isCancelled, let request = pendingRequest {
            pendingRequest = nil
            await CoverThumbnailCache.shared.preheat(
                request.items,
                width: request.width,
                height: request.height,
                scale: request.scale
            )
        }
        task = nil
    }

    func cancel() {
        task?.cancel()
        task = nil
        pendingRequest = nil
    }
}

private struct GameLibraryControllerFocusScope<Content: View>: View {
    let focusState: GameLibraryControllerFocusState
    let gameID: String
    let isEnabled: Bool
    let content: (Bool) -> Content

    var body: some View {
        // Touch-only library browsing does not need one observable focus object
        // per materialized card. The conditional expression preserves the card
        // subtree's identity when a controller connects.
        content(isEnabled ? focusState.cardState(for: gameID).isFocused : false)
    }
}

/// Cover flow always brings the focused controller card to this center frame.
/// Register one reusable launch/context-menu geometry view here instead of one
/// GeometryReader plus UIViewRepresentable on every materialized card.
private struct GameLibraryControllerCoverFlowLaunchAnchor: View {
    let focusState: GameLibraryControllerFocusState
    let registry: GameplayLaunchCardRegistry
    let width: CGFloat
    let height: CGFloat
    let centerCorrection: CGFloat
    let verticalPosition: CGFloat

    var body: some View {
        if let gameID = focusState.selectionScrollRequest?.gameID {
            Color.clear
                .frame(width: width, height: height)
                .scaleEffect(1.10)
                .offset(y: centerCorrection - 16 + verticalPosition)
                .registerGameplayLaunchCard(for: gameID, in: registry)
                .allowsHitTesting(false)
                .accessibilityHidden(true)
        }
    }
}

private struct GameLibraryScrollObserver: View {
    static let topAnchor = "game-library-top"

    let focusState: GameLibraryControllerFocusState
    let proxy: ScrollViewProxy
    let favoriteScrollRequest: GameLibraryFavoriteScrollRequest?
    let restoreState: GameLibraryScrollRestoreState
    let topAlignment: UnitPoint
    var usesCompactControllerAnimation = false
    var focusScrollCoordinator: GameLibraryFocusScrollCoordinator? = nil
    var focusScrollAxis: GameLibraryFocusScrollAxis = .vertical

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(
                of: restoreState.topRequestSequence,
                initial: true
            ) { _, sequence in
                guard sequence > 0,
                      restoreState.consumeTopRestore(sequence) else { return }
                DispatchQueue.main.async {
                    var transaction = Transaction()
                    transaction.animation = nil
                    withTransaction(transaction) {
                        proxy.scrollTo(Self.topAnchor, anchor: topAlignment)
                    }
                }
            }
            .onChange(
                of: focusState.selectionScrollRequest,
                initial: true
            ) { _, request in
                guard let request else { return }
                // Both portrait and landscape publish selection while lazy
                // content is reconciling the new focused card. Move on the
                // following main turn so an offscreen stable ID is mounted;
                // rapid navigation coalesces to the newest request.
                DispatchQueue.main.async {
                    guard focusState.selectionScrollRequest == request else {
                        return
                    }
                    if let focusScrollCoordinator {
                        focusScrollCoordinator.focus(
                            gameID: request.gameID,
                            proxy: proxy,
                            reduceMotion: reduceMotion,
                            axis: focusScrollAxis,
                            usesCompactAnimation:
                                usesCompactControllerAnimation
                        )
                    } else if usesCompactControllerAnimation {
                        if reduceMotion {
                            var transaction = Transaction(animation: nil)
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        } else {
                            withAnimation(
                                .smooth(duration: 0.32, extraBounce: 0.015)
                            ) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        }
                    } else {
                        if reduceMotion {
                            var transaction = Transaction(animation: nil)
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        } else {
                            withAnimation(
                                .smooth(duration: 0.20, extraBounce: 0)
                            ) {
                                proxy.scrollTo(request.gameID, anchor: .center)
                            }
                        }
                    }
                }
            }
            .onChange(of: favoriteScrollRequest) { _, request in
                guard let request else { return }
                DispatchQueue.main.async {
                    // Resolve the destination after the favorite-first sort
                    // has completed a layout pass.
                    DispatchQueue.main.async {
                        if request.destination == .nowRunning {
                            var transaction = Transaction(animation: nil)
                            transaction.disablesAnimations = true
                            withTransaction(transaction) {
                                let nowRunningAnchor: UnitPoint
                                switch focusScrollAxis {
                                case .horizontal:
                                    nowRunningAnchor = .center
                                case .vertical:
                                    nowRunningAnchor = topAlignment
                                }
                                proxy.scrollTo(
                                    gameLibraryNowRunningFocusID,
                                    anchor: nowRunningAnchor
                                )
                            }
                            return
                        }
                        withAnimation(.smooth(duration: 0.42, extraBounce: 0.02)) {
                            switch request.destination {
                            case .firstGame(let gameID):
                                proxy.scrollTo(gameID, anchor: topAlignment)
                            case .game(let gameID):
                                proxy.scrollTo(gameID, anchor: .center)
                            case .nowRunning:
                                break
                            }
                        }
                    }
                }
            }
    }
}

private struct GameLibraryControllerFocusModifier: ViewModifier {
    let isFocused: Bool
    let cornerRadius: CGFloat
    var isFavorite = false
    var favoriteGlowEnabled = false
    var isEnabled = true
    var backgroundEffectsEnabled = true
    var performanceOptimized = false
    var focusedScale: CGFloat? = nil
    var optimizedAnimationDuration: Double = 0.12

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            let selectedScale = focusedScale
                ?? (performanceOptimized ? 1.08 : 1.12)
            content
                .focusEffectDisabled()
                .scaleEffect(
                    isFocused ? selectedScale : 1
                )
                .controllerFocusBoxPresentation(
                    isVisible: isFocused,
                    cornerRadius: cornerRadius,
                    performanceOptimized: performanceOptimized
                )
                .background {
                    if !isFocused,
                       isFavorite,
                       favoriteGlowEnabled,
                       backgroundEffectsEnabled {
                        // Favorites retain a themed glow without running a full
                        // animated focus-artwork timeline for every visible card.
                        ControllerBottomNavigationFocusGlow(
                            cornerRadius: cornerRadius
                        )
                    }
                }
                .shadow(
                    color: Color.black.opacity(
                        isFocused && !performanceOptimized ? 0.26 : 0
                    ),
                    radius: isFocused && !performanceOptimized ? 18 : 0,
                    y: isFocused && !performanceOptimized ? 8 : 0
                )
                // A scaled cover can overlap its neighbors during a rapid
                // retarget. Keep the single focused card above the rail so a
                // held direction never lets an unfocused card cover it.
                .zIndex(isFocused ? 20 : 0)
                .animation(
                    reduceMotion
                        ? .linear(
                            duration: performanceOptimized
                                ? min(0.06, optimizedAnimationDuration)
                                : 0.12
                        )
                        : .smooth(
                            duration: performanceOptimized
                                ? optimizedAnimationDuration
                                : 0.24,
                            extraBounce: 0
                        ),
                    value: isFocused
                )
        } else {
            content
        }
    }
}

private enum GameLibraryControllerToolbarAction: Int, CaseIterable {
    // Preserve the previous persisted raw values for Import and Layout.
    case bootBIOS = 0
    case importGames = 1
    case layout = 2
    case more = 3
}

@MainActor
@Observable
private final class GameLibraryControllerToolbarFocusState {
    var selectedAction: GameLibraryControllerToolbarAction = .importGames
}

private enum GameLibraryControllerAlertKind: String, Equatable {
    case automaticCustomSkin
    case restart
    case stop
    case deleteGameData
    case deleteGame
    case gameAction
    case coverResult
    case replaceFiles
    case backgroundError
}

private struct PendingAutomaticCustomSkinLaunch {
    enum Purpose: Equatable {
        case gameLaunch
        case library
    }

    let game: ISOEntry
    var proposals: [AutomaticCustomSkinProposal]
    var selectedProposalIndex: Int
    var setCustomLayout: Bool
    var doNotShowAgain: Bool
    let purpose: Purpose
    let transition: GameplayLaunchTransition?

    init(
        game: ISOEntry,
        proposals: [AutomaticCustomSkinProposal],
        selectedProposalIndex: Int = 0,
        setCustomLayout: Bool = false,
        doNotShowAgain: Bool = true,
        purpose: Purpose,
        transition: GameplayLaunchTransition?
    ) {
        self.game = game
        self.proposals = proposals
        self.selectedProposalIndex = min(
            max(0, selectedProposalIndex),
            max(0, proposals.count - 1)
        )
        self.setCustomLayout = setCustomLayout
        self.doNotShowAgain = doNotShowAgain
        self.purpose = purpose
        self.transition = transition
    }

    var proposal: AutomaticCustomSkinProposal {
        proposals[selectedProposalIndex]
    }
}

private struct DeferredControllerContextMenuSelection {
    let action: GameLibraryControllerMenuAction
    let gameID: String
}

@MainActor
@Observable
private final class GameLibraryControllerContextMenuState {
    var gameID: String?
    var section: GameLibraryControllerMenuSection = .root
    var selectedAction: GameLibraryControllerMenuAction?
}

private struct GameLibraryControllerContextMenuPresenter<Content: View>: View {
    let state: GameLibraryControllerContextMenuState
    let content: (
        _ gameID: String,
        _ section: GameLibraryControllerMenuSection,
        _ selectedAction: GameLibraryControllerMenuAction?
    ) -> Content

    init(
        state: GameLibraryControllerContextMenuState,
        @ViewBuilder content: @escaping (
            _ gameID: String,
            _ section: GameLibraryControllerMenuSection,
            _ selectedAction: GameLibraryControllerMenuAction?
        ) -> Content
    ) {
        self.state = state
        self.content = content
    }

    @ViewBuilder
    var body: some View {
        if let gameID = state.gameID {
            content(gameID, state.section, state.selectedAction)
        }
    }
}

private struct ControllerToolbarFocusModifier: ViewModifier {
    let focusState: GameLibraryControllerToolbarFocusState
    let action: GameLibraryControllerToolbarAction
    let controllerInput: MenuControllerInputRouter?
    let menuTabIsActive: Bool
    var baseHorizontalPadding: CGFloat = 0
    var minimumWidth: CGFloat = 36

    @ViewBuilder
    func body(content: Content) -> some View {
        let isFocused = menuTabIsActive
            && controllerInput?.isControllerNavigationEnabled == true
            && controllerInput?.navigationZone == .topToolbar
            && focusState.selectedAction == action

        content.modifier(
            ControllerToolbarFocusVisualModifier(
                isFocused: isFocused,
                focusID: "toolbar.\(action.rawValue)",
                baseHorizontalPadding: baseHorizontalPadding,
                minimumWidth: minimumWidth
            )
        )
    }
}

private struct GameLibraryListTopEdgeEffectModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            content.scrollEdgeEffectHidden(true, for: .top)
        } else {
            content
        }
    }
}

struct GameListView: View {
    let embeddedInMenuNavigation: Bool
    let ownsEmbeddedMenuToolbar: Bool
    let controllerInput: MenuControllerInputRouter?
    let onRenamePresentationChanged: (Bool) -> Void

    init(
        embeddedInMenuNavigation: Bool = false,
        ownsEmbeddedMenuToolbar: Bool = true,
        controllerInput: MenuControllerInputRouter? = nil,
        onRenamePresentationChanged: @escaping (Bool) -> Void = { _ in }
    ) {
        self.embeddedInMenuNavigation = embeddedInMenuNavigation
        self.ownsEmbeddedMenuToolbar = ownsEmbeddedMenuToolbar
        self.controllerInput = controllerInput
        self.onRenamePresentationChanged = onRenamePresentationChanged
    }

    @State private var games: [ISOEntry] = []
    @State private var appState = AppState.shared
	@State private var settings = SettingsStore.shared
	@State private var logoStore = ARMSX2LogoStore.shared
	@State private var fileImporter = FileImportHandler.shared
	@State private var coverStore = CoverStore.shared
	@State private var externalLibrary = ExternalGameLibrary.shared
	@State private var externalCoverAutoDownloadAttemptedIDs = Set<String>()
	@State private var coverWorkTask: Task<Void, Never>?
	@State private var showGameImporter = false
    @State private var showCoverImporter = false
    @State private var showCoverPhotoPicker = false
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    // `controllerInput` is nil while touch owns the UI so controller focus
    // resources can be dismantled. Keep the lightweight shared router solely
    // for gesture arbitration with RootView's L2/R2 backdrop shortcut.
    @Environment(\.menuControllerInputRouter)
    private var sharedMenuControllerInput
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.layoutDirection) private var layoutDirection
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCardSubtitleColour) private var secondaryTextColour
    @Environment(\.uiTitleTextColour) private var titleTextColour
    @Environment(\.uiContentTextColour) private var themedContentTextColour
    @Environment(\.uiSecondaryTextColour) private var themedSecondaryTextColour
    @Environment(\.uiCriticalTextColour) private var criticalTextColour
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var showCoverTemplateEditor = false
    @State private var showGameReplacementAlert = false
    @State private var pendingAutomaticCustomSkinLaunch:
        PendingAutomaticCustomSkinLaunch?
    @State private var coverTemplateDraft = CoverStore.defaultCoverURLTemplate
    @State private var pendingGameName: String = ""
    @State private var pendingGameplayLaunchTransition: GameplayLaunchTransition?
    @State private var pendingGameplayPadSerial: String?
    @State private var pendingGameplayPadCRC: String?
    @State private var pendingGameImportURLs: [URL] = []
    @State private var existingGameImportFileNames: [String] = []
    @State private var pendingCoverGameName: String?
    @State private var pendingCoverPhotoGameName: String?
    @State private var selectedCoverPhotoItem: PhotosPickerItem?
	@State private var gameInfoTarget: ISOEntry?
    @State private var gameSettingsTarget: ISOEntry?
	@State private var gameSettingsStartsInShaders = false
	@State private var preparedGameSettingsID: String?
	@State private var preparedGameSettings: [String: Any]?
	@State private var renameTarget: ISOEntry?
    @State private var discLinkTarget: ISOEntry?
    @State private var cheatsManagerTarget: ISOEntry?
    @State private var pendingDeleteGame: ISOEntry?
    @State private var pendingDeleteDataGame: ISOEntry?
    @State private var gameActionTitle = ""
    @State private var gameActionMessage: String?
    @State private var showBackgroundAssetError = false
    @State private var gameplayLaunchCardRegistry = GameplayLaunchCardRegistry()
    @State private var favoriteAnimationValues: [String: Int] = [:]
    @State private var nowRunningRemovalAnimationActive = false
    @State private var nowRunningRemovalResetTask: Task<Void, Never>?
    @State private var departingRunningGameName: String?
    @State private var nowRunningCardOpacity = 1.0
    @State private var nowRunningCardScale = 1.0
    @State private var nowRunningStatusOpacity = 1.0
    @State private var gameLibraryActivity = GameLibraryActivityState()
    @State private var controllerFocusState = GameLibraryControllerFocusState()
    @State private var controllerScrollAvailability =
        GameLibraryControllerScrollAvailability()
    @State private var libraryScrollRestoreState =
        GameLibraryScrollRestoreState()
    @State private var favoriteScrollRequest: GameLibraryFavoriteScrollRequest?
    @State private var favoriteScrollSequence: UInt64 = 0
    @State private var focusScrollCoordinator =
        GameLibraryFocusScrollCoordinator()
    @State private var coverFlowPreheater = GameLibraryCoverFlowPreheater()
    @State private var libraryContainerSize: CGSize = .zero
    @State private var landscapeControllerAutofocusPending = false
    @State private var landscapeControllerAutofocusTask: Task<Void, Never>?
    @State private var runningGameControllerFocusPending = false
    @State private var controllerContextMenuState =
        GameLibraryControllerContextMenuState()
    @State private var deferredControllerContextMenuSelection:
        DeferredControllerContextMenuSelection?
    @State private var controllerToolbarFocusState = GameLibraryControllerToolbarFocusState()
    @State private var controllerNowRunningFocusState =
        GameLibraryNowRunningControllerFocusState()
    @State private var controllerAlertSelectedIndex = 0
    @State private var controllerLayoutMenuPresented = false
    @State private var controllerLayoutMenuSelectedIndex = 0
    @State private var gameLibraryViewOptionsPresented = false
    @State private var gameLibraryViewOptionsPreview:
        GameLibraryViewOptionsPreview?
    @State private var gameLibraryViewOptionsBaseline:
        GameLibraryViewOptionsValues?
    @State private var controllerMoreMenuPresented = false
    @State private var controllerMoreMenuSelectedIndex = 0
    @State private var controllerFocusReleased = false
    @AppStorage("ARMSX2iOSGameLibraryLayout") private var libraryLayout = "grid"
    @AppStorage("ARMSX2iOSLandscapeCoverFlowEnabled") private var landscapeCoverFlowEnabled = true
    // Keep the legacy value as the initial fallback, then persist independent
    // portrait and landscape values as soon as either layout is adjusted.
    // A single shared key made changes in one orientation leak into the other.
    @AppStorage("ARMSX2iOSPortraitUseGamesLogo")
    private var portraitUseGamesLogo = -1
    @AppStorage("ARMSX2iOSLandscapeUseGamesLogo")
    private var landscapeUseGamesLogo = -1
    @AppStorage("ARMSX2iOSHideGamesScreenTitle")
    private var legacyHideGamesScreenTitle = true
    @AppStorage("ARMSX2iOSPortraitHideGamesScreenTitle")
    private var portraitHideGamesScreenTitle = -1
    @AppStorage("ARMSX2iOSLandscapeHideGamesScreenTitle")
    private var landscapeHideGamesScreenTitle = -1
    @AppStorage("ARMSX2iOSHideGameName")
    private var legacyHideGameName = false
    @AppStorage("ARMSX2iOSPortraitHideGameName")
    private var portraitHideGameName = -1
    @AppStorage("ARMSX2iOSLandscapeHideGameName")
    private var landscapeHideGameName = -1
    @AppStorage("ARMSX2iOSHideGameInfo")
    private var legacyHideGameInfo = false
    @AppStorage("ARMSX2iOSPortraitHideGameInfo")
    private var portraitHideGameInfo = -1
    @AppStorage("ARMSX2iOSLandscapeHideGameInfo")
    private var landscapeHideGameInfo = -1
    @AppStorage("ARMSX2iOSHideFavoriteButton")
    private var legacyHideFavoriteButton = false
    @AppStorage("ARMSX2iOSPortraitHideFavoriteButton")
    private var portraitHideFavoriteButton = -1
    @AppStorage("ARMSX2iOSLandscapeHideFavoriteButton")
    private var landscapeHideFavoriteButton = -1
    @AppStorage("ARMSX2iOSHideRegionFlag")
    private var legacyHideRegionFlag = false
    @AppStorage("ARMSX2iOSPortraitHideRegionFlag")
    private var portraitHideRegionFlag = -1
    @AppStorage("ARMSX2iOSLandscapeHideRegionFlag")
    private var landscapeHideRegionFlag = -1
    @AppStorage("ARMSX2iOSSingleLineGameNames")
    private var legacySingleLineGameNames = false
    @AppStorage("ARMSX2iOSPortraitSingleLineGameNames")
    private var portraitSingleLineGameNames = -1
    @AppStorage("ARMSX2iOSLandscapeSingleLineGameNames")
    private var landscapeSingleLineGameNames = -1
    @AppStorage("ARMSX2iOSHideRunningIndicator")
    private var legacyHideRunningIndicator = false
    @AppStorage("ARMSX2iOSPortraitHideRunningIndicator")
    private var portraitHideRunningIndicator = -1
    @AppStorage("ARMSX2iOSLandscapeHideRunningIndicator")
    private var landscapeHideRunningIndicator = -1
    // Experimental presentation metrics are deliberately orientation-specific.
    // Only the complete card surface changes size; cover/title children retain
    // their shared center, preserving the proven focus-zoom geometry.
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardScale")
    private var portraitGameCardScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardScale")
    private var landscapeGameCardScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardWidthScale")
    private var portraitGameCardWidthScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardWidthScale")
    private var landscapeGameCardWidthScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardHeightScale")
    private var portraitGameCardHeightScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardHeightScale")
    private var landscapeGameCardHeightScale = 1.0
    @AppStorage("ARMSX2iOSPortraitGameLibraryVerticalPosition")
    private var portraitGameLibraryVerticalPosition = 0.0
    @AppStorage("ARMSX2iOSLandscapeGameLibraryVerticalPosition")
    private var landscapeGameLibraryVerticalPosition = 0.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardGapScale")
    private var portraitGameCardGapScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardGapScale")
    private var landscapeGameCardGapScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardContentPaddingScale")
    private var portraitGameCardContentPaddingScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardContentPaddingScale")
    private var landscapeGameCardContentPaddingScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardVerticalPaddingScale")
    private var portraitGameCardVerticalPaddingScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardVerticalPaddingScale")
    private var landscapeGameCardVerticalPaddingScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardTextSpacingScale")
    private var portraitGameCardTextSpacingScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardTextSpacingScale")
    private var landscapeGameCardTextSpacingScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameNameTextScale")
    private var portraitGameNameTextScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameNameTextScale")
    private var landscapeGameNameTextScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameInfoTextScale")
    private var portraitGameInfoTextScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameInfoTextScale")
    private var landscapeGameInfoTextScale = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCardCornerRadius")
    private var portraitGameCardCornerRadius = 18.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCardCornerRadius")
    private var landscapeGameCardCornerRadius = 18.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCoverCornerRadius")
    private var portraitGameCoverCornerRadius = 10.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCoverCornerRadius")
    private var landscapeGameCoverCornerRadius = 10.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCoverShadow")
    private var portraitGameCoverShadowStrength = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCoverShadow")
    private var landscapeGameCoverShadowStrength = 1.0
    @AppStorage("ARMSX2iOSExperimentalPortraitGameCoverOpacity")
    private var portraitGameCoverOpacity = 1.0
    @AppStorage("ARMSX2iOSExperimentalLandscapeGameCoverOpacity")
    private var landscapeGameCoverOpacity = 1.0
    @AppStorage("ARMSX2iOSPortraitBottomNavigationTabBarHeight")
    private var portraitBottomNavigationTabBarHeight = 84.0
    @AppStorage("ARMSX2iOSLandscapeBottomNavigationTabBarHeight")
    private var landscapeBottomNavigationTabBarHeight = 84.0
    @AppStorage("ARMSX2iOSPortraitBottomNavigationTabBarIconScale")
    private var portraitBottomNavigationTabBarIconScale = 1.0
    @AppStorage("ARMSX2iOSLandscapeBottomNavigationTabBarIconScale")
    private var landscapeBottomNavigationTabBarIconScale = 1.0
    @AppStorage("ARMSX2iOSPortraitBottomNavigationTabBarLabelScale")
    private var portraitBottomNavigationTabBarLabelScale = 1.0
    @AppStorage("ARMSX2iOSLandscapeBottomNavigationTabBarLabelScale")
    private var landscapeBottomNavigationTabBarLabelScale = 1.0
    @AppStorage("ARMSX2iOSPortraitBottomNavigationTabBarClearance")
    private var portraitBottomNavigationTabBarClearance = 0.0
    @AppStorage("ARMSX2iOSLandscapeBottomNavigationTabBarClearance")
    private var landscapeBottomNavigationTabBarClearance = 0.0
    @AppStorage("ARMSX2iOSPortraitBottomNavigationTabBarShowsLabels")
    private var portraitBottomNavigationTabBarShowsLabels = 1
    @AppStorage("ARMSX2iOSLandscapeBottomNavigationTabBarShowsLabels")
    private var landscapeBottomNavigationTabBarShowsLabels = 1
    // SettingsStore owns background configuration; the shared container renders it.
    private var hasCustomBackground: Bool {
        settings.dynamicBackgroundsEnabled
            || settings.backgroundPrimaryAsset != nil
            || settings.backgroundLandscapeAsset != nil
    }

    private var displayedRunningGameName: String? {
        if let departingRunningGameName {
            return departingRunningGameName
        }
        guard case .menu = appState.currentScreen else {
            return nil
        }
        return appState.runningGameName
    }

    private var canFocusNowRunning: Bool {
        appState.runningGameName != nil && !nowRunningRemovalAnimationActive
    }

    private var usesLandscapeGameCardScale: Bool {
        // The geometry state is intentionally updated after SwiftUI finishes a
        // layout transaction. During rotation that left every orientation-
        // specific card setting (scale, padding and corner radii) reading the
        // previous orientation until this entire view was reconstructed.
        // UIWindowScene is the authoritative orientation source and has
        // already committed the new interface orientation when the rotated
        // library body is evaluated.
        if let orientation = activeMenuWindow()?.windowScene?.interfaceOrientation,
           orientation != .unknown {
            return orientation.isLandscape
        }
        return libraryContainerSize.width > libraryContainerSize.height
    }

    private func orientationBoolBinding(
        portrait: Binding<Int>,
        landscape: Binding<Int>,
        fallback: Bool
    ) -> Binding<Bool> {
        Binding(
            get: {
                let stored = usesLandscapeGameCardScale
                    ? landscape.wrappedValue
                    : portrait.wrappedValue
                return stored >= 0 ? stored == 1 : fallback
            },
            set: { value in
                if usesLandscapeGameCardScale {
                    landscape.wrappedValue = value ? 1 : 0
                } else {
                    portrait.wrappedValue = value ? 1 : 0
                }
            }
        )
    }

    private func orientationDoubleBinding(
        portrait: Binding<Double>,
        landscape: Binding<Double>
    ) -> Binding<Double> {
        Binding(
            get: {
                usesLandscapeGameCardScale
                    ? landscape.wrappedValue
                    : portrait.wrappedValue
            },
            set: { value in
                if usesLandscapeGameCardScale {
                    landscape.wrappedValue = value
                } else {
                    portrait.wrappedValue = value
                }
            }
        )
    }

    private var activeHideGamesScreenTitleBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitHideGamesScreenTitle,
            landscape: $landscapeHideGamesScreenTitle,
            fallback: legacyHideGamesScreenTitle
        )
    }

    private var activeUseGamesLogoBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitUseGamesLogo,
            landscape: $landscapeUseGamesLogo,
            fallback: true
        )
    }

    private var activeHideGameNameBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitHideGameName,
            landscape: $landscapeHideGameName,
            fallback: legacyHideGameName
        )
    }

    private var activeHideGameInfoBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitHideGameInfo,
            landscape: $landscapeHideGameInfo,
            fallback: legacyHideGameInfo
        )
    }

    private var activeHideFavoriteButtonBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitHideFavoriteButton,
            landscape: $landscapeHideFavoriteButton,
            fallback: legacyHideFavoriteButton
        )
    }

    private var activeHideRegionFlagBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitHideRegionFlag,
            landscape: $landscapeHideRegionFlag,
            fallback: legacyHideRegionFlag
        )
    }

    private var activeSingleLineGameNamesBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitSingleLineGameNames,
            landscape: $landscapeSingleLineGameNames,
            fallback: legacySingleLineGameNames
        )
    }

    private var activeHideRunningIndicatorBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitHideRunningIndicator,
            landscape: $landscapeHideRunningIndicator,
            fallback: legacyHideRunningIndicator
        )
    }

    private var activeGameCardScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardScale,
            landscape: $landscapeGameCardScale
        )
    }

    private var activeGameCardWidthScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardWidthScale,
            landscape: $landscapeGameCardWidthScale
        )
    }

    private var activeGameCardHeightScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardHeightScale,
            landscape: $landscapeGameCardHeightScale
        )
    }

    private var activeGameLibraryVerticalPositionBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameLibraryVerticalPosition,
            landscape: $landscapeGameLibraryVerticalPosition
        )
    }

    private var activeGameCardGapScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardGapScale,
            landscape: $landscapeGameCardGapScale
        )
    }

    private var activeGameCardContentPaddingScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardContentPaddingScale,
            landscape: $landscapeGameCardContentPaddingScale
        )
    }

    private var activeGameCardVerticalPaddingScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardVerticalPaddingScale,
            landscape: $landscapeGameCardVerticalPaddingScale
        )
    }

    private var activeGameCardTextSpacingScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardTextSpacingScale,
            landscape: $landscapeGameCardTextSpacingScale
        )
    }

    private var activeGameNameTextScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameNameTextScale,
            landscape: $landscapeGameNameTextScale
        )
    }

    private var activeGameInfoTextScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameInfoTextScale,
            landscape: $landscapeGameInfoTextScale
        )
    }

    private var activeGameCardCornerRadiusBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCardCornerRadius,
            landscape: $landscapeGameCardCornerRadius
        )
    }

    private var activeGameCoverCornerRadiusBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCoverCornerRadius,
            landscape: $landscapeGameCoverCornerRadius
        )
    }

    private var activeGameCoverShadowStrengthBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCoverShadowStrength,
            landscape: $landscapeGameCoverShadowStrength
        )
    }

    private var activeGameCoverOpacityBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitGameCoverOpacity,
            landscape: $landscapeGameCoverOpacity
        )
    }

    private var activeBottomNavigationTabBarHeightBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitBottomNavigationTabBarHeight,
            landscape: $landscapeBottomNavigationTabBarHeight
        )
    }

    private var activeBottomNavigationTabBarIconScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitBottomNavigationTabBarIconScale,
            landscape: $landscapeBottomNavigationTabBarIconScale
        )
    }

    private var activeBottomNavigationTabBarLabelScaleBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitBottomNavigationTabBarLabelScale,
            landscape: $landscapeBottomNavigationTabBarLabelScale
        )
    }

    private var activeBottomNavigationTabBarClearanceBinding: Binding<Double> {
        orientationDoubleBinding(
            portrait: $portraitBottomNavigationTabBarClearance,
            landscape: $landscapeBottomNavigationTabBarClearance
        )
    }

    private var activeBottomNavigationTabBarShowsLabelsBinding: Binding<Bool> {
        orientationBoolBinding(
            portrait: $portraitBottomNavigationTabBarShowsLabels,
            landscape: $landscapeBottomNavigationTabBarShowsLabels,
            fallback: true
        )
    }

    private var hideGamesScreenTitle: Bool {
        activeHideGamesScreenTitleBinding.wrappedValue
    }

    private var useGamesLogo: Bool {
        activeUseGamesLogoBinding.wrappedValue && logoStore.hasLogo
    }

    private var hideGameName: Bool {
        activeHideGameNameBinding.wrappedValue
    }

    private var hideGameInfo: Bool {
        activeHideGameInfoBinding.wrappedValue
    }

    private var hideFavoriteButton: Bool {
        activeHideFavoriteButtonBinding.wrappedValue
    }

    private var hideRegionFlag: Bool {
        activeHideRegionFlagBinding.wrappedValue
    }

    private var singleLineGameNames: Bool {
        activeSingleLineGameNamesBinding.wrappedValue
    }

    private var hideRunningIndicator: Bool {
        activeHideRunningIndicatorBinding.wrappedValue
    }

    private var resolvedGameCardScale: CGFloat {
        CGFloat(min(max(activeGameCardScaleBinding.wrappedValue, 0.72), 1.32))
    }

    private var resolvedGameCardWidthScale: CGFloat {
        return CGFloat(
            min(max(activeGameCardWidthScaleBinding.wrappedValue, 0.70), 1.40)
        )
    }

    private var resolvedGameCardHeightScale: CGFloat {
        CGFloat(
            min(max(activeGameCardHeightScaleBinding.wrappedValue, 0.70), 1.40)
        )
    }

    private var resolvedGameLibraryVerticalPosition: CGFloat {
        // The visual baseline historically sat 13 points below SwiftUI's
        // nominal zero. Keep the persisted/user-facing control calibrated to
        // zero while retaining that proven placement in every layout.
        let calibratedPosition =
            activeGameLibraryVerticalPositionBinding.wrappedValue + 13
        return CGFloat(
            min(
                max(calibratedPosition, -160),
                160
            )
        )
    }

    private var resolvedGameCardGapScale: CGFloat {
        CGFloat(min(max(activeGameCardGapScaleBinding.wrappedValue, 0.65), 2))
    }

    private var resolvedGameCardContentPaddingScale: CGFloat {
        CGFloat(
            min(
                max(activeGameCardContentPaddingScaleBinding.wrappedValue, 0),
                2
            )
        )
    }

    private var resolvedGameCardVerticalPaddingScale: CGFloat {
        CGFloat(
            min(
                max(activeGameCardVerticalPaddingScaleBinding.wrappedValue, 0),
                2
            )
        )
    }

    private var resolvedGameCardTextSpacingScale: CGFloat {
        CGFloat(
            min(
                max(activeGameCardTextSpacingScaleBinding.wrappedValue, 0),
                2
            )
        )
    }

    private var resolvedGameNameTextScale: CGFloat {
        CGFloat(
            min(max(activeGameNameTextScaleBinding.wrappedValue, 0.75), 1.5)
        )
    }

    private var resolvedGameInfoTextScale: CGFloat {
        CGFloat(
            min(max(activeGameInfoTextScaleBinding.wrappedValue, 0.75), 1.5)
        )
    }

    private var resolvedGameCardCornerRadius: CGFloat {
        CGFloat(min(max(activeGameCardCornerRadiusBinding.wrappedValue, 6), 34))
    }

    private var resolvedGameCoverCornerRadius: CGFloat {
        CGFloat(min(max(activeGameCoverCornerRadiusBinding.wrappedValue, 0), 28))
    }

    private var resolvedFocusedGameCardScale: CGFloat {
        1.10
    }

    private var resolvedFocusedGameCardLift: CGFloat {
        16
    }

    private var resolvedGameCoverShadowStrength: CGFloat {
        CGFloat(min(max(activeGameCoverShadowStrengthBinding.wrappedValue, 0), 2))
    }

    private var resolvedGameCoverOpacity: Double {
        min(max(activeGameCoverOpacityBinding.wrappedValue, 0.35), 1)
    }

    private var gameNameLineLimit: Int {
        singleLineGameNames ? 1 : 2
    }

    private var controllerSelectedGameID: String? {
        get { controllerFocusState.selectedGameID }
        nonmutating set {
            let previousValue = controllerFocusState.selectedGameID
            controllerFocusState.selectedGameID = newValue
            if let newValue {
                controllerInput?.rememberNavigationFocusKey(
                    "game:\(newValue)",
                    forScope: gameLibraryControllerMemoryScope
                )
                // Toolbar/tab-bar focus must not replace the last game card.
                // The router survives rebuilding this tab's view.
                controllerInput?.rememberNavigationFocusKey(
                    "game:\(newValue)",
                    forScope: gameLibraryControllerCardMemoryScope
                )
            }
            if previousValue != newValue {
                updateGameCoverThemePreview(for: newValue)
            }
        }
    }

    /// Favorite cards opt into the cover-derived appearance while focused.
    /// The context menu can temporarily opt any linked game into the same
    /// preview without changing its favorite state or the saved theme.
    private func updateGameCoverThemePreview(
        for gameID: String?,
        includesNonFavoriteGame: Bool = false,
        whilePresentationIsActive: Bool = false
    ) {
        guard menuTabIsActive,
              whilePresentationIsActive
                || (controllerInput?.navigationZone == .library
                    && !controllerNavigationPresentationActive),
              let gameID else {
            GameCoverThemePreviewStore.shared.clear()
            return
        }
        // With the optional favorite treatment disabled, normal focus changes
        // must remain O(1): do not scan the library or schedule palette work.
        // Explicit Context Menu/Now Running previews still opt in.
        guard includesNonFavoriteGame
                || settings.favoriteGlowingEffectEnabled else {
            GameCoverThemePreviewStore.shared.clear()
            return
        }
        guard let game = games.first(where: { $0.id == gameID }),
              includesNonFavoriteGame || game.isFavorite else {
            GameCoverThemePreviewStore.shared.clear()
            return
        }
        GameCoverThemePreviewStore.shared.schedule(
            gameID: game.id,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature
        )
    }

    private var controllerNavigationActive: Bool {
        get { controllerFocusState.isActive }
        nonmutating set { controllerFocusState.isActive = newValue }
    }

    /// Departure values belong only to the retained stopping placeholder.
    /// A normal running card always starts from its canonical appearance, so
    /// cleanup never needs to reset animation state across the library tree.
    private var displayedNowRunningCardOpacity: Double {
        departingRunningGameName == nil ? 1 : nowRunningCardOpacity
    }

    private var displayedNowRunningCardScale: Double {
        departingRunningGameName == nil ? 1 : nowRunningCardScale
    }

    private var displayedNowRunningStatusOpacity: Double {
        departingRunningGameName == nil ? 1 : nowRunningStatusOpacity
    }

    private var showsPageOwnedLargeTitle: Bool {
        !hideGamesScreenTitle
            && !useGamesLogo
            && embeddedInMenuNavigation
            && verticalSizeClass != .compact
            && UIDevice.current.userInterfaceIdiom == .phone
    }

    private func libraryPresentationIdentity(
        for containerSize: CGSize
    ) -> String {
        let orientation = containerSize.width > containerSize.height
            ? "landscape"
            : "portrait"

        if games.isEmpty && displayedRunningGameName == nil {
            return "empty-\(orientation)"
        }
        if libraryLayout != "grid" {
            return "list-\(orientation)"
        }
        if orientation == "landscape" && landscapeCoverFlowEnabled {
            return "cover-flow-landscape"
        }
        return "grid-\(orientation)"
    }

    private struct CoverFlowMetrics {
        let isCompact: Bool
        let coverWidth: CGFloat
        let coverHeight: CGFloat
        let textWidth: CGFloat
        let cardSpacing: CGFloat
        let cardHorizontalPadding: CGFloat
        let cardVerticalPadding: CGFloat
        let cornerRadius: CGFloat
        let favoritePadding: CGFloat
        let favoriteInset: CGFloat
        let itemSpacing: CGFloat
        let horizontalPadding: CGFloat
        let verticalPadding: CGFloat
        let statusWidth: CGFloat
        let statusHeight: CGFloat
        let statusIconSize: CGFloat

        init(
            containerSize: CGSize,
            controllerOptimized: Bool = false,
            cardScale: CGFloat = 1,
            widthScale: CGFloat = 1,
            heightScale: CGFloat = 1,
            gapScale: CGFloat = 1,
            contentPaddingScale: CGFloat = 1,
            verticalPaddingScale: CGFloat = 1,
            textSpacingScale: CGFloat = 1,
            cardCornerRadius: CGFloat? = nil
        ) {
            isCompact = containerSize.height < 360
            let safeCardScale = min(max(cardScale, 0.72), 1.32)
            let safeWidthScale = min(max(widthScale, 0.70), 1.40)
            let safeHeightScale = min(max(heightScale, 0.70), 1.40)
            let safeGapScale = min(max(gapScale, 0.65), 2)
            let safeContentPaddingScale = min(max(contentPaddingScale, 0), 2)
            let safeVerticalPaddingScale = min(max(verticalPaddingScale, 0), 2)
            let safeTextSpacingScale = min(max(textSpacingScale, 0), 2)
            let baseCoverHeight: CGFloat = controllerOptimized
                ? (isCompact ? 198 : 285)
                : (isCompact ? 156 : 225)
            coverHeight = baseCoverHeight * safeCardScale * safeHeightScale
            // Width and height are independent outer-card metrics. Dividing
            // out the height multiplier keeps Width from being changed twice.
            coverWidth = coverHeight * (2.0 / 3.0)
                * (safeWidthScale / safeHeightScale)
            let minimumTextWidth: CGFloat = controllerOptimized
                ? (isCompact ? 164 : 210)
                : (isCompact ? 134 : 164)
            textWidth = max(
                minimumTextWidth * safeCardScale * safeWidthScale,
                coverWidth + (28 * safeCardScale * safeWidthScale)
            )
            cardSpacing = (isCompact ? 8 : 12) * safeCardScale
                * safeTextSpacingScale
            cardHorizontalPadding = (isCompact ? 8 : 12) * safeCardScale
                * safeContentPaddingScale
            cardVerticalPadding = (isCompact ? 8 : 12) * safeCardScale
                * safeVerticalPaddingScale
            cornerRadius = cardCornerRadius
                ?? ((isCompact ? 18 : 24) * safeCardScale)
            favoritePadding = isCompact ? 6 : 8
            favoriteInset = isCompact ? 5 : 8
            itemSpacing = (controllerOptimized
                ? (isCompact ? 24 : 30)
                : (isCompact ? 14 : 20)) * safeGapScale
            horizontalPadding = isCompact ? 20 : 32
            verticalPadding = isCompact ? 10 : 18
            statusWidth = isCompact ? 138 : 166
            statusHeight = isCompact ? 190 : 276
            statusIconSize = isCompact ? 36 : 48
        }
    }

    var body: some View {
        // Give the compiler a concrete boundary before the lifecycle modifier
        // chain. This screen owns many independent presentations; inferring
        // all of them plus every `onChange` as one nested generic type can
        // exceed Swift's Release type-check budget.
        let presentation = OptionalMenuNavigationStack(
            embedded: embeddedInMenuNavigation
        ) {
            ZStack {
                if hasCustomBackground {
                    MenuBackgroundLayer(isActive: menuTabIsActive)
                        .zIndex(0)
                }

                if renameTarget == nil {
                    GeometryReader { geo in
                        Group {
                            if games.isEmpty && displayedRunningGameName == nil {
                                emptyLibrary(containerSize: geo.size)
                            } else if libraryLayout == "grid" && geo.size.width > geo.size.height && landscapeCoverFlowEnabled {
                                coverFlowLibrary(containerSize: geo.size)
                            } else if libraryLayout == "grid" {
                                gridLibrary(containerSize: geo.size)
                            } else {
                                listLibrary
#if targetEnvironment(macCatalyst)
                                .listStyle(.inset)
#endif
                            }
                        }
                        // Portrait grids/lists and landscape cover flow use
                        // different scroll axes. Give each presentation a stable
                        // identity so rotation cannot reuse the previous
                        // UIScrollView's content offset or alignment geometry.
                        .id(libraryPresentationIdentity(for: geo.size))
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .onAppear {
                            libraryContainerSize = geo.size
                            if geo.size.width > geo.size.height {
                                landscapeControllerAutofocusPending = true
                                scheduleLandscapeControllerAutofocus()
                            }
                        }
                        .onChange(of: geo.size) { previousSize, size in
                            let enteredLandscape = size.width > size.height
                                && previousSize.width <= previousSize.height
                            libraryContainerSize = size
                            if enteredLandscape {
                                landscapeControllerAutofocusPending = true
                                scheduleLandscapeControllerAutofocus()
                            } else if size.width <= size.height {
                                landscapeControllerAutofocusPending = false
                                landscapeControllerAutofocusTask?.cancel()
                                landscapeControllerAutofocusTask = nil
                                coverFlowPreheater.cancel()
                            }
                        }
                    }
                    // Keep card glass in one persistent sampling group without
                    // absorbing either half of the depth-split orb field.
                    .stableMenuContentGlassContainer()
                    .zIndex(10)
                }

            }
            .overlay {
                if controllerLayoutMenuPresented {
                    ControllerNavigationAlert(
                        title: settings.localized("Library Layout"),
                        dimsBackground: false,
                        message: "",
                        actions: controllerLayoutMenuActions,
                        selectedIndex: controllerLayoutMenuSelectedIndex,
                        onSelect: performControllerLayoutMenuAction,
                        onDismiss: {
                            dismissControllerLayoutMenu()
                        }
                    )
                    .zIndex(20_000)
                }
                if controllerMoreMenuPresented {
                    ControllerNavigationAlert(
                        title: settings.localized("More"),
                        titleColour: settings.controllerContextMenuColor,
                        contentColour: settings.controllerContextMenuColor,
                        secondaryContentColour:
                            settings.controllerContextMenuSecondaryColor,
                        usesClearGlass: false,
                        showsActionButtonBackgrounds: true,
                        dimsBackground: false,
                        message: "",
                        actions: controllerMoreMenuActions,
                        selectedIndex: controllerMoreMenuSelectedIndex,
                        onSelect: performControllerMoreMenuAction,
                        onDismiss: {
                            dismissControllerMoreMenu()
                        }
                    )
                    .ignoresSafeArea(.container, edges: .all)
                    .zIndex(20_000)
                }
            }
            .overlay {
                if let kind = activeControllerAlertKind,
                   kind != .automaticCustomSkin {
                    ControllerNavigationAlert(
                        title: controllerAlertTitle(for: kind),
                        titleColour:
                            kind == .restart || kind == .automaticCustomSkin
                                ? .white
                                : nil,
                        contentColour:
                            kind == .automaticCustomSkin ? .white : nil,
                        previewImageURL:
                            controllerAlertPreviewImageURL(for: kind),
                        previewSkinDescriptor:
                            controllerAlertPreviewSkinDescriptor(for: kind),
                        previewAccessibilityLabel:
                            controllerAlertPreviewAccessibilityLabel(for: kind),
                        details: controllerAlertDetails(for: kind),
                        emphasizesPreview: kind == .automaticCustomSkin,
                        prefersTopPlacement:
                            kind == .automaticCustomSkin,
                        dimsBackground: false,
                        message: controllerAlertMessage(for: kind),
                        actions: controllerAlertActions(for: kind),
                        selectedIndex: controllerAlertSelectedIndex,
                        onSelect: { index in
                            performControllerAlertAction(index, for: kind)
                        },
                        onDismiss: {
                            if kind == .automaticCustomSkin {
                                cancelAutomaticCustomSkinLaunch(
                                    playsReturnSound: true
                                )
                            } else {
                                dismissControllerAlert(kind)
                            }
                        }
                    )
                    .zIndex(20_000)
                }
            }
            .overlay {
                if controllerInput != nil {
                    GameLibraryControllerCommandListener(
                        controllerInput: controllerInput,
                        focusState: controllerFocusState,
                        libraryVisible: menuTabIsActive,
                        onCommand: handleControllerCommand,
                        onLibraryEntry: handleControllerLibraryEntry,
                        onFocusRelease: releaseControllerFocus,
                        onRightStickScrollEnded:
                            restoreCenteredControllerFocusAfterRightStickScrolling,
                        onInputAvailabilityChanged:
                            controllerInputAvailabilityDidChange
                    )
                }
            }
            .clearNavigationContainerBackground()
            .optionalMenuNavigationChrome(
                title: hideGamesScreenTitle ? "" : settings.localized("Games"),
                backgroundHidden: hasCustomBackground,
                embedded: embeddedInMenuNavigation
            )
            .toolbar {
                if !embeddedInMenuNavigation || ownsEmbeddedMenuToolbar {
                ToolbarItem(id: "menu.gamesBrand", placement: .topBarLeading) {
                    if useGamesLogo, let image = logoStore.image {
                        Button(action: performBootBIOSAction) {
                            Image(uiImage: image)
                                .resizable()
                                .interpolation(.high)
                                .scaledToFit()
                                .frame(width: 154, height: 22)
                                .padding(.horizontal, 18)
                                .frame(minHeight: 42)
                                .contentShape(Capsule())
                                .modifier(controllerToolbarFocus(
                                    .bootBIOS,
                                    minimumWidth: 190
                                ))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(settings.localized("Boot BIOS"))
                    } else {
                        Button(action: performBootBIOSAction) {
                            Text(settings.localized("Boot BIOS"))
                                .font(.callout.weight(.semibold))
                                .foregroundStyle(accentColour)
                                .lineLimit(1)
                                .fixedSize(horizontal: true, vertical: false)
                                .modifier(controllerToolbarFocus(
                                    .bootBIOS,
                                    baseHorizontalPadding: 14,
                                    minimumWidth: 112
                                ))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(settings.localized("Boot BIOS"))
                    }
                }
                ToolbarItem(id: "menu.import", placement: .topBarTrailing) {
                    Button {
                        openGameImporter()
                    } label: {
                        Image(systemName: "plus")
                            .modifier(controllerToolbarFocus(.importGames))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Import Games"))
                }
                ToolbarItem(id: "menu.layout", placement: .topBarTrailing) {
                    Menu {
                        Button {
                            selectAlternateLibraryLayout()
                        } label: {
                            Label(
                                settings.localized(libraryLayout == "grid" ? "Show List" : "Show Grid"),
                                systemImage: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2"
                            )
                        }

                        if libraryLayout == "grid",
                           usesLandscapeGameCardScale {
                            Toggle(isOn: $landscapeCoverFlowEnabled) {
                                Label(settings.localized("Landscape Cover Flow"), systemImage: "rectangle.landscape.rotate")
                            }
                        }

                        Divider()

                        Button {
                            presentGameLibraryViewOptions()
                        } label: {
                            Label(
                                settings.localized(
                                    "Game Library View Options"
                                ),
                                systemImage: "slider.horizontal.3"
                            )
                        }
                    } label: {
                        Image(systemName: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2")
                            .modifier(controllerToolbarFocus(.layout))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Library Layout"))
                }
                ToolbarItem(id: "menu.more", placement: .topBarTrailing) {
                    Menu {
                        Button {
                            presentMenuPanel("cover_import_all") {
                                pendingCoverGameName = nil
                                showCoverImporter = true
                            }
                        } label: {
                            Label(settings.localized("Import Local Covers"), systemImage: "photo.badge.plus")
                        }

                        Button {
                            downloadMissingCovers()
                        } label: {
                            Label(settings.localized("Download Missing Covers"), systemImage: "icloud.and.arrow.down")
                        }
                        .disabled(coverStore.isDownloadingCovers || games.isEmpty)

                        Button {
                            presentMenuPanel("cover_source") {
                                coverTemplateDraft = coverStore.coverURLTemplate
                                showCoverTemplateEditor = true
                            }
                        } label: {
                            Label(settings.localized("Cover Source"), systemImage: "link")
                        }

                        Button {
                            presentMenuPanel("cover_template_reset") {
                                coverStore.coverURLTemplate = CoverStore.defaultCoverURLTemplate
                                coverStore.lastCoverMessage = "Cover URL template reset to the ARMSX2 Android default."
                                coverStore.showCoverAlert = true
                            }
                        } label: {
                            Label(settings.localized("Reset Cover Template"), systemImage: "arrow.counterclockwise")
                        }
                        Divider()
                        Button {
                            loadGames(forceMetadataRefresh: true)
                        } label: {
                            Label(settings.localized("Refresh"), systemImage: "arrow.clockwise")
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .modifier(controllerToolbarFocus(.more))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("More"))
                }
                }
            }
            // UIKit owns the compact overflow button. Supplying the same
            // semantic tint as our custom focus visuals keeps its ellipsis in
            // sync with every explicitly rendered toolbar symbol.
            .tint(accentColour)
            .alert(settings.localized("Cover Result"), isPresented: nativeAlertBinding($coverStore.showCoverAlert)) {
                Button(settings.localized("OK")) {}
            } message: {
                Text(coverStore.lastCoverMessage ?? "")
            }
            .alert(settings.localized("Cover Source"), isPresented: $showCoverTemplateEditor) {
                TextField("https://.../${serial}.jpg", text: $coverTemplateDraft)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Save")) {
                    coverStore.coverURLTemplate = coverTemplateDraft
                    if games.isEmpty {
                        coverStore.lastCoverMessage = "Cover URL template saved."
                        coverStore.showCoverAlert = true
                    } else {
                        downloadMissingCovers()
                    }
                }
            } message: {
                Text("Use ${serial}, ${title}, or ${filetitle}. Default: \(CoverStore.defaultCoverURLTemplate)")
            }
            .alert(settings.localized("Restart VM?"), isPresented: nativeAlertBinding($showRestartAlert)) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingGameplayLaunchTransition = nil
                    pendingGameplayPadSerial = nil
                    pendingGameplayPadCRC = nil
                }
                Button(settings.localized("Restart"), role: .destructive) {
                    if pendingGameName.isEmpty {
                        pendingGameplayLaunchTransition = nil
                        pendingGameplayPadSerial = nil
                        pendingGameplayPadCRC = nil
                        appState.shutdownAndBootBIOS()
                    } else {
                        let transition = pendingGameplayLaunchTransition
                        pendingGameplayLaunchTransition = nil
                        appState.prepareGameplayPadSelection(
                            forSerial: pendingGameplayPadSerial,
                            crc: pendingGameplayPadCRC
                        )
                        pendingGameplayPadSerial = nil
                        pendingGameplayPadCRC = nil
                        appState.shutdownAndBoot(
                            isoName: pendingGameName,
                            launchTransition: transition
                        )
                    }
                }
			} message: {
				let target = pendingGameName.isEmpty ? settings.localized("Boot BIOS") : (pendingGameName as NSString).lastPathComponent
				Text(String(format: settings.localized("VM is currently running.\nShut down and start %@?"), target))
			}
            .alert(
                settings.localized("Delete Game Data?"),
                isPresented: Binding(
                    get: { false },
                    set: { if !$0 && controllerInput == nil { pendingDeleteDataGame = nil } }
                )
            ) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingDeleteDataGame = nil
                }
                Button(settings.localized("Delete Game Data"), role: .destructive) {
                    if let game = pendingDeleteDataGame {
                        deleteGameData(game)
                    }
                    pendingDeleteDataGame = nil
                }
            } message: {
                Text(settings.localized("This clears save states, PNACH files, per-game settings, compatibility overrides, and generated cache for this game. Memory card contents are not deleted."))
            }
            .alert(
                settings.localized("Delete Game?"),
                isPresented: Binding(
                    get: { false },
                    set: { if !$0 && controllerInput == nil { pendingDeleteGame = nil } }
                )
            ) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingDeleteGame = nil
                }
                Button(settings.localized("Delete ROM"), role: .destructive) {
                    if let game = pendingDeleteGame {
                        deleteGame(game, deleteData: false)
                    }
                    pendingDeleteGame = nil
                }
                Button(settings.localized("Delete ROM + Game Data"), role: .destructive) {
                    if let game = pendingDeleteGame {
                        deleteGame(game, deleteData: true)
                    }
                    pendingDeleteGame = nil
                }
            } message: {
                Text(settings.localized("Delete the selected game file? You can also remove its generated game data at the same time."))
            }
            .alert(
                settings.localized(gameActionTitle.isEmpty ? "Game Action" : gameActionTitle),
                isPresented: Binding(
                    get: { false },
                    set: { if !$0 && controllerInput == nil { gameActionMessage = nil } }
                )
            ) {
                Button(settings.localized("OK")) {
                    gameActionMessage = nil
                }
            } message: {
                Text(gameActionMessage ?? "")
            }
            .alert(settings.localized("Replace existing files?"), isPresented: nativeAlertBinding($showGameReplacementAlert)) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    clearPendingGameImport()
                }
                Button(settings.localized("Replace"), role: .destructive) {
                    importGames(pendingGameImportURLs, allowReplacingExistingFiles: true)
                    clearPendingGameImport()
                }
            } message: {
                Text(FileImportHandler.replacementConfirmationMessage(for: existingGameImportFileNames))
            }
            .alert(settings.localized("Background image could not be loaded."), isPresented: nativeAlertBinding($showBackgroundAssetError)) {
                Button(settings.localized("OK")) {
                    showBackgroundAssetError = false
                }
            }
				.sheet(isPresented: $showGameImporter) {
					ImportDocumentPicker(
						allowedContentTypes: FileImportHandler.gameContentTypes,
						allowsMultipleSelection: true,
						legacyDocumentTypes: ["public.item", "public.data", "public.content"]
	                ) { result in
                    showGameImporter = false
                    switch result {
                    case .success(let urls):
                        prepareGameImport(urls)
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            fileImporter.presentImportResult(FileImportHandler.failedGamePickerMessage(errorDescription: error.localizedDescription))
                        }
					}
				}
			}
			.sheet(isPresented: $showCoverImporter) {
				ImportDocumentPicker(
					allowedContentTypes: CoverStore.coverContentTypes,
                    allowsMultipleSelection: pendingCoverGameName == nil
                ) { result in
                    showCoverImporter = false
                    switch result {
                    case .success(let urls):
                        coverStore.importCoverURLs(urls, forGameNamed: pendingCoverGameName)
                        pendingCoverGameName = nil
                        loadGames()
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            coverStore.lastCoverMessage = "Cover import failed: \(error.localizedDescription)"
                            coverStore.showCoverAlert = true
                        }
                        pendingCoverGameName = nil
                    }
                }
            }
            .photosPicker(
                isPresented: $showCoverPhotoPicker,
                selection: $selectedCoverPhotoItem,
                matching: .images
            )
            .onChange(of: selectedCoverPhotoItem) { _, photoItem in
                guard let photoItem, let gameName = pendingCoverPhotoGameName else { return }
                selectedCoverPhotoItem = nil
                pendingCoverPhotoGameName = nil
                importCoverPhoto(photoItem, forGameNamed: gameName)
            }
            .fullScreenCover(item: $gameInfoTarget) { game in
                GameLibraryForegroundPanel(
                    maximumWidth: 620,
                    maximumHeight: 700,
                    onDismiss: { gameInfoTarget = nil }
                ) {
                    GameInfoPanel(game: game)
                }
                .presentationBackground(.clear)
            }
            .fullScreenCover(isPresented: $gameLibraryViewOptionsPresented) {
                ZStack {
                    GameLibraryForegroundPanel(
                        maximumWidth: 520,
                        maximumHeight: 600,
                        isContentVisible:
                            gameLibraryViewOptionsPreview == nil,
                        onDismiss: applyGameLibraryViewOptions
                    ) {
                        GameLibraryViewOptionsPanel(
                            controllerInput: controllerInput,
                            orientationTitle: settings.localized(
                                usesLandscapeGameCardScale
                                    ? "Landscape"
                                    : "Portrait"
                            ),
                            cardScale: activeGameCardScaleBinding,
                            cardWidthScale: activeGameCardWidthScaleBinding,
                            cardHeightScale: activeGameCardHeightScaleBinding,
                            libraryVerticalPosition:
                                activeGameLibraryVerticalPositionBinding,
                            gapScale: activeGameCardGapScaleBinding,
                            contentPaddingScale:
                                activeGameCardContentPaddingScaleBinding,
                            verticalPaddingScale:
                                activeGameCardVerticalPaddingScaleBinding,
                            textSpacingScale:
                                activeGameCardTextSpacingScaleBinding,
                            gameNameTextScale:
                                activeGameNameTextScaleBinding,
                            gameInfoTextScale:
                                activeGameInfoTextScaleBinding,
                            cardCornerRadius: activeGameCardCornerRadiusBinding,
                            coverCornerRadius: activeGameCoverCornerRadiusBinding,
                            coverShadowStrength:
                                activeGameCoverShadowStrengthBinding,
                            coverOpacity: activeGameCoverOpacityBinding,
                            bottomNavigationTabBarHeight:
                                activeBottomNavigationTabBarHeightBinding,
                            bottomNavigationTabBarIconScale:
                                activeBottomNavigationTabBarIconScaleBinding,
                            bottomNavigationTabBarLabelScale:
                                activeBottomNavigationTabBarLabelScaleBinding,
                            bottomNavigationTabBarClearance:
                                activeBottomNavigationTabBarClearanceBinding,
                            bottomNavigationTabBarShowsLabels:
                                activeBottomNavigationTabBarShowsLabelsBinding,
                            hideGamesScreenTitle:
                                activeHideGamesScreenTitleBinding,
                            useGamesLogo: activeUseGamesLogoBinding,
                            logoAvailable: logoStore.hasLogo,
                            hideGameName: activeHideGameNameBinding,
                            hideGameInfo: activeHideGameInfoBinding,
                            hideFavoriteButton:
                                activeHideFavoriteButtonBinding,
                            hideRegionFlag: activeHideRegionFlagBinding,
                            singleLineGameNames:
                                activeSingleLineGameNamesBinding,
                            hideRunningIndicator:
                                activeHideRunningIndicatorBinding,
                            onPreviewChanged: {
                                gameLibraryViewOptionsPreview = $0
                            },
                            onReset: resetActiveGameLibraryViewOptions,
                            onCancel: cancelGameLibraryViewOptions,
                            onApply: applyGameLibraryViewOptions
                        )
                    }

                    if let preview = gameLibraryViewOptionsPreview {
                        ThemePaletteEditorSliderReadout(
                            title: preview.title,
                            value: preview.value
                        )
                        .transition(
                            .opacity.combined(
                                with: .scale(
                                    scale: 0.96,
                                    anchor: .topTrailing
                                )
                            )
                        )
                    }
                }
                .onDisappear { gameLibraryViewOptionsPreview = nil }
                .presentationBackground(.clear)
            }
            // A page overlay cannot outrank NavigationStack chrome or the
            // root safe-area tab bar. Present the controller context menu at
            // the scene presentation level so its scrim and controls really
            // are the foreground interaction surface.
            .fullScreenCover(
                isPresented: Binding(
                    get: { controllerContextMenuState.gameID != nil },
                    set: { isPresented in
                        if !isPresented,
                           controllerContextMenuState.gameID != nil {
                            closeControllerContextMenu(playsFeedback: false)
                        }
                    }
                ),
                onDismiss: completeDeferredControllerContextMenuSelection
            ) {
                GameLibraryControllerContextMenuPresenter(
                    state: controllerContextMenuState
                ) { gameID, section, selectedAction in
                    if let game = games.first(where: { $0.id == gameID }) {
                        ControllerGameContextMenu(
                            game: game,
                            controllerInput: controllerInput,
                            sectionTitle: controllerContextMenuTitle(
                                for: section
                            ),
                            items: controllerContextMenuItems(
                                for: game,
                                section: section
                            ),
                            selectedAction: selectedAction,
                            isGameRunning: isRunning(game),
                            onPlayOrStop: {
                                deferControllerContextMenuSelection(
                                    .playOrStop,
                                    for: game
                                )
                            },
                            onSelect: { action in
                                performControllerContextMenuAction(
                                    action,
                                    for: game
                                )
                            },
                            onDismiss: {
                                closeControllerContextMenu()
                            }
                        )
                        .isolatedMenuGlassContainer()
                    }
                }
                .presentationBackground(.clear)
            }
            // Match Context Menu's scene-level presentation. A page overlay
            // cannot render above NavigationStack chrome or the persistent tab
            // bar, regardless of its local z-index.
            .fullScreenCover(
                isPresented: Binding(
                    get: { pendingAutomaticCustomSkinLaunch != nil },
                    set: { isPresented in
                        if !isPresented,
                           pendingAutomaticCustomSkinLaunch != nil {
                            cancelAutomaticCustomSkinLaunch()
                        }
                    }
                )
            ) {
                if let kind = activeControllerAlertKind,
                   kind == .automaticCustomSkin {
                    let usesDefaultText = settings.controllerUIThemePreset == .defaultTheme
                    ControllerNavigationAlert(
                        title: controllerAlertTitle(for: kind),
                        titleColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
                        contentColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
                        secondaryContentColour: usesDefaultText ? .white : settings.controllerContextMenuSecondaryColor,
                        detailValueColour: usesDefaultText
                            ? settings.controllerNavigationAccentColor
                            : nil,
                        previewImageURL:
                            controllerAlertPreviewImageURL(for: kind),
                        previewSkinDescriptor:
                            controllerAlertPreviewSkinDescriptor(for: kind),
                        previewAccessibilityLabel:
                            controllerAlertPreviewAccessibilityLabel(for: kind),
                        details: controllerAlertDetails(for: kind),
                        emphasizesPreview: true,
                        // Match the library Per-Game Settings command deck:
                        // regular Liquid Glass remains readable over every
                        // game cover and does not create a clear tinted slab.
                        usesClearGlass: false,
                        usesLargeLandscapePanel: true,
                        // The library chooser keeps its existing typography
                        // and controls, but no longer consumes Per-Game
                        // Settings-sized space around them.
                        usesCompactPreviewPanel: true,
                        showsActionButtonBackgrounds: true,
                        dimsBackground: false,
                        message: controllerAlertMessage(for: kind),
                        actions: controllerAlertActions(for: kind),
                        selectedIndex: controllerAlertSelectedIndex,
                        onSelect: { index in
                            performControllerAlertAction(index, for: kind)
                        },
                        onDismiss: {
                            cancelAutomaticCustomSkinLaunch(
                                playsReturnSound: true
                            )
                        }
                    )
                    .onAppear {
                        // A previously focused library row can survive beneath
                        // the full-screen cover. Always enter this chooser at
                        // its first control instead of inheriting a visual
                        // position near the bottom action row.
                        controllerAlertSelectedIndex = 0
                    }
                    .contextMenuPanelTextAppearance()
                    .presentationBackground(.clear)
                }
            }
            .fullScreenCover(
                item: $gameSettingsTarget,
                onDismiss: {
                    gameSettingsStartsInShaders = false
                    releasePreparedGameSettings()
                }
            ) { game in
                LibraryPerGameSettingsOverlay(
                    game: game,
                    preloadedSettings: preparedGameSettingsID == game.id
                        ? preparedGameSettings
                        : nil,
                    initiallySelectsShaders: gameSettingsStartsInShaders,
                    controllerInput: controllerInput,
                    onDone: {
                        gameSettingsTarget = nil
                        gameSettingsStartsInShaders = false
                        releasePreparedGameSettings()
                    }
                )
                .presentationBackground(.clear)
            }
            .fullScreenCover(item: $renameTarget) { game in
                OrbitKeysKeyboardView(
                    title: settings.localized("Rename"),
                    initialText: game.displayName,
                    game: game,
                    startsInNormalKeyboard: true,
                    onCommit: { name in
                        renameGame(game, to: name)
                        renameTarget = nil
                    },
                    onCancel: {
                        renameTarget = nil
                    }
                )
                .presentationBackground(.clear)
            }
            .fullScreenCover(item: $discLinkTarget) { game in
                GameLibraryForegroundPanel(
                    maximumWidth: 620,
                    maximumHeight: 680,
                    onDismiss: { discLinkTarget = nil }
                ) {
                    DiscLinkPicker(discs: games.filter { !$0.isELF && $0.id != game.id }) { selected in
                        ARMSX2Bridge.setLinkedDiscPath(selected?.fileURL?.path ?? selected?.bootName, forELF: game.bootName)
                        loadGames()
                    }
                }
                .presentationBackground(.clear)
            }
            .fullScreenCover(item: $cheatsManagerTarget) { game in
                GameLibraryForegroundPanel(
                    maximumWidth: 980,
                    maximumHeight: 780,
                    onDismiss: { cheatsManagerTarget = nil }
                ) {
                    CheatsPatchesManagerView(
                        isoName: game.bootName,
                        gameTitle: game.name,
                        launchContext: .library,
                        controllerInput: controllerInput
                    )
                }
                .presentationBackground(.clear)
            }
			}
            .alert(settings.localized("Stop Emulation?"), isPresented: nativeAlertBinding($showStopAlert)) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    showStopAlert = false
                }
                Button(settings.localized("Stop"), role: .destructive) {
                    scheduleStopRunningGame()
                }
            } message: {
                Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
            }
        let libraryObservers = presentation
        .onAppear(perform: handleLibraryAppear)
        .onChange(of: renameTarget != nil, initial: true) { _, isPresented in
            onRenamePresentationChanged(isPresented)
        }
        .onChange(of: controllerInput?.navigationZone) { _, zone in
            if zone == .topToolbar {
                rememberControllerToolbarFocus()
            }
            updateGameCoverThemePreview(
                for: zone == .library ? controllerSelectedGameID : nil
            )
        }
        .onChange(of: controllerContextMenuState.gameID) { _, gameID in
            if let gameID {
                updateGameCoverThemePreview(
                    for: gameID,
                    includesNonFavoriteGame: true,
                    whilePresentationIsActive: true
                )
            } else {
                updateGameCoverThemePreview(for: controllerSelectedGameID)
            }
        }
        .onChange(of: controllerToolbarFocusState.selectedAction) { _, _ in
            if controllerInput?.navigationZone == .topToolbar {
                rememberControllerToolbarFocus()
            }
        }
        .onReceive(
            NotificationCenter.default.publisher(for: ExternalGameLibrary.didChangeNotification),
            perform: handleExternalLibraryDidChange
        )
        .onReceive(
            NotificationCenter.default.publisher(for: InitialContentBootstrap.didChangeNotification),
            perform: handleInitialContentDidChange
        )
        .onReceive(
            NotificationCenter.default.publisher(for: NSNotification.Name("ARMSX2iOSReturnToMenu")),
            perform: handleReturnToMenu
        )

        return libraryObservers
        .onChange(of: games) { _, updatedGames in
            controllerFocusState.updateGameIDs(updatedGames.map(\.id))
            updateGameCoverThemePreview(for: controllerSelectedGameID)
            if let controllerSelectedGameID,
               !updatedGames.contains(where: { $0.id == controllerSelectedGameID }) {
                self.controllerSelectedGameID = nil
                controllerNavigationActive = false
                closeControllerContextMenu(playsFeedback: false)
            }
            if controllerNavigationActive,
               controllerSelectedGameID == nil,
               controllerNowRunningFocusState.selectedAction == nil {
                _ = restoreRememberedControllerContentFocus()
                    || selectInitialControllerGame(preferLast: false)
            }
            let restoredRunningGameFocus =
                focusRunningControllerGameIfAvailable()
            if !restoredRunningGameFocus,
               usesLandscapeGameCardScale {
                landscapeControllerAutofocusPending = true
                scheduleLandscapeControllerAutofocus()
            }
        }
        .onChange(of: displayedRunningGameName) { _, runningGame in
            if runningGame == nil {
                runningGameControllerFocusPending = false
                controllerNowRunningFocusState.selectedAction = nil
            } else if controllerInput?.hasConnectedController == true {
                runningGameControllerFocusPending = true
                _ = focusRunningControllerGameIfAvailable()
            }
        }
        .onChange(of: controllerInput?.hasConnectedController) { _, connected in
            if connected == true {
                if appState.runningGameName != nil {
                    runningGameControllerFocusPending = true
                }
                if !focusRunningControllerGameIfAvailable() {
                    landscapeControllerAutofocusPending =
                        usesLandscapeGameCardScale
                    scheduleLandscapeControllerAutofocus()
                }
            } else {
                runningGameControllerFocusPending = false
                controllerFocusState.releaseCardStates()
                focusScrollCoordinator.cancel()
            }
        }
        .onChange(of: menuTabIsActive) { _, active in
            updateGameCoverThemePreview(
                for: active ? controllerSelectedGameID : nil
            )
            if active,
               !restoreRememberedControllerContentFocus(),
               !focusRunningControllerGameIfAvailable(),
               usesLandscapeGameCardScale {
                landscapeControllerAutofocusPending = true
                scheduleLandscapeControllerAutofocus()
            }
        }
        .onChange(of: manuallyCapturedControllerPresentationActive) { _, active in
            let shouldCapture = active
                || controllerContextMenuState.gameID != nil
            controllerInput?.setNavigationCaptured(
                shouldCapture,
                owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
                priority: 300
            )
            updateControllerScrollAvailability()
            if !shouldCapture {
                scheduleLandscapeControllerAutofocus()
            }
        }
        .onChange(of: controllerNavigationPresentationActive) { _, active in
            guard active else { return }
            // Modal destinations own the foreground. Stop speculative cover
            // decoding and any in-flight centering work until the library is
            // interactive again; navigation restarts both on demand.
            coverFlowPreheater.cancel()
            focusScrollCoordinator.stopAnimation()
            gameLibraryActivity.isScrolling = false
        }
        .onChange(of: activeControllerAlertKind) { previous, kind in
            controllerAlertSelectedIndex = 0
            guard let kind, kind != previous else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .onDisappear {
            GameCoverThemePreviewStore.shared.clear()
            onRenamePresentationChanged(false)
            controllerLayoutMenuPresented = false
            gameLibraryViewOptionsPresented = false
            controllerMoreMenuPresented = false
            landscapeControllerAutofocusTask?.cancel()
            landscapeControllerAutofocusTask = nil
            coverFlowPreheater.cancel()
            coverWorkTask?.cancel()
            coverWorkTask = nil
            focusScrollCoordinator.cancel()
            gameLibraryActivity.cancelScheduledReload()
            gameLibraryActivity.cancelLoad()
            gameplayLaunchCardRegistry.removeAll()
            favoriteAnimationValues.removeAll(keepingCapacity: false)
            CoverThumbnailCache.shared.releaseInactiveLibraryImages()
            controllerInput?.setNavigationCaptured(
                false,
                owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
                priority: 300
            )
            guard case .playing = appState.currentScreen else { return }
            releaseLibraryResourcesForGameplay()
        }
    }

    private func handleLibraryAppear() {
        GameLibraryRuntimeResources.activateForMenu()
        updateControllerScrollAvailability()
        restoreCachedGamesIfNeeded()
        if controllerInput?.navigationZone == .library {
            _ = restoreRememberedControllerContentFocus()
        }
        if GameLibrarySnapshot.shared.shouldRefreshLibrary() {
            scheduleLibraryReload(
                after: .milliseconds(80),
                autoDownloadExternalCovers: true
            )
        }
        if settings.sanitizeBackgroundAssets() {
            showBackgroundAssetError = true
        }
    }

    private func handleExternalLibraryDidChange(_ notification: Notification) {
        GameLibrarySnapshot.shared.markLibraryRefreshNeeded()
        scheduleLibraryReload(
            after: .milliseconds(120),
            autoDownloadExternalCovers: true
        )
    }

    private func handleInitialContentDidChange(_ notification: Notification) {
        GameLibrarySnapshot.shared.markLibraryRefreshNeeded()
        scheduleLibraryReload(
            after: .milliseconds(120),
            autoDownloadExternalCovers: false
        )
    }

    private func handleReturnToMenu(_ notification: Notification) {
        runningGameControllerFocusPending =
            controllerInput?.hasConnectedController == true
            && appState.runningGameName != nil
        restoreCachedGamesIfNeeded()
        GameLibrarySnapshot.shared.markLibraryRefreshNeeded()
        _ = focusRunningControllerGameIfAvailable()
        scheduleLibraryReload(
            after: .milliseconds(80),
            autoDownloadExternalCovers: false
        )
    }

    private var touchLibraryLongPressObserver: some View {
        GameLibraryTouchLongPressObserver(
            registry: gameplayLaunchCardRegistry,
            onTouchBegan: suppressThemeShortcutForGameCardTouch
        ) { gameID in
            guard let game = games.first(where: { $0.id == gameID }) else {
                return
            }
            handleGameContextMenuLongPress(for: game)
        }
        .frame(width: 0, height: 0)
    }

    private var listLibrary: some View {
        ScrollViewReader { proxy in
            List {
                if controllerInput == nil {
                    touchLibraryLongPressObserver
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                if controllerInput != nil {
                    ZStack {
                        GameLibraryControllerRightStickTarget(
                            controllerInput: controllerInput,
                            availability: controllerScrollAvailability,
                            axes: .vertical,
                            priority: 10,
                            onReachedLeadingEdge: controllerRightStickReachedTop
                        )
                        Color.clear
                            .id(GameLibraryScrollObserver.topAnchor)
                    }
                    .frame(height: 0)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                }
                if showsPageOwnedLargeTitle {
                    EmbeddedMenuLargeTitle(
                        title: settings.localized("Games"),
                        usesARMSX2Logo: useGamesLogo
                    )
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                if games.isEmpty && displayedRunningGameName == nil {
                    emptyState
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                if let gameName = displayedRunningGameName {
                    vmStatusSection(
                        gameName: gameName,
                        opacity: displayedNowRunningCardOpacity,
                        scale: displayedNowRunningCardScale
                    )
                    .id(gameLibraryNowRunningFocusID)
                    .offset(y: resolvedGameLibraryVerticalPosition)
                }
                ForEach(games) { game in
                    GameLibraryControllerFocusScope(
                        focusState: controllerFocusState,
                        gameID: game.id,
                        isEnabled: controllerInput != nil
                    ) { controllerFocused in
                        gameRow(
                            game,
                            controllerFocused: controllerFocused,
                            visualEffectsEnabled: true
                        )
                        .gameCardTintMenuBackgroundListRow(
                            true,
                            contentPaddingScale:
                                resolvedGameCardContentPaddingScale,
                            verticalPaddingScale:
                                resolvedGameCardVerticalPaddingScale
                        )
                        .modifier(
                            GameLibraryControllerFocusModifier(
                                isFocused: controllerFocused,
                                cornerRadius: 16,
                                isFavorite: game.isFavorite,
                                favoriteGlowEnabled:
                                    settings.favoriteGlowingEffectEnabled,
                                isEnabled: controllerInput != nil
                                    || (game.isFavorite
                                        && settings
                                            .favoriteGlowingEffectEnabled)
                            )
                        )
                    }
                        .id(game.id)
                        .offset(y: resolvedGameLibraryVerticalPosition)
                        .registerGameLibraryFocusScrollTarget(
                            for: game.id,
                            in: focusScrollCoordinator,
                            isEnabled: controllerInput != nil
                        )
                }
            }
            .contentMargins(.top, 0, for: .scrollContent)
            .scrollIndicators(.hidden)
            .modifier(GameLibraryListTopEdgeEffectModifier())
            .scrollContentBackground(.hidden)
            .scrollDisabled(false)
            .scrollBounceBehavior(.always)
            .trackGameLibraryScrollPhase(gameLibraryActivity)
            .transaction { transaction in
                if transaction.isContinuous || gameLibraryActivity.isScrolling {
                    transaction.animation = nil
                }
            }
            .background {
                if hasCustomBackground {
                    Color.clear
                } else {
                    LinearGradient(
                        colors: [
                            Color(.systemGroupedBackground),
                            Color(.secondarySystemGroupedBackground),
                        ],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                }
            }
            .overlay {
                GameLibraryScrollObserver(
                    focusState: controllerFocusState,
                    proxy: proxy,
                    favoriteScrollRequest: favoriteScrollRequest,
                    restoreState: libraryScrollRestoreState,
                    topAlignment: .top,
                    focusScrollCoordinator: focusScrollCoordinator,
                    focusScrollAxis: .vertical
                )
            }
        }
    }

    private func gridLibrary(containerSize: CGSize) -> some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 16) {
                    if showsPageOwnedLargeTitle {
                        EmbeddedMenuLargeTitle(
                            title: settings.localized("Games"),
                            usesARMSX2Logo: useGamesLogo
                        )
                    }

                    if games.isEmpty && displayedRunningGameName == nil {
                        emptyState
                            .frame(minHeight: 300)
                    }

                    if let gameName = displayedRunningGameName {
                        vmStatusCard(gameName: gameName)
                            .id(gameLibraryNowRunningFocusID)
                            .offset(y: resolvedGameLibraryVerticalPosition)
                            .isolatedMenuGlassContainer()
                            .padding(.horizontal)
                            .compositingGroup()
                            .scaleEffect(displayedNowRunningCardScale)
                            .opacity(displayedNowRunningCardOpacity)
                            .animation(
                                .smooth(duration: 0.65, extraBounce: 0.04),
                                value: displayedNowRunningCardScale
                            )
                            .animation(.linear(duration: 0.65), value: displayedNowRunningCardOpacity)
                            .transition(.scale(scale: 0.86).combined(with: .opacity))
                    }

                    LazyVGrid(
                        columns: gameGridColumns(for: containerSize.width),
                        spacing: 18 * resolvedGameCardGapScale
                    ) {
                        ForEach(games) { game in
                            GameLibraryControllerFocusScope(
                                focusState: controllerFocusState,
                                gameID: game.id,
                                isEnabled: controllerInput != nil
                            ) { controllerFocused in
                                gameGridCard(
                                    game,
                                    controllerFocused: controllerFocused,
                                    visualEffectsEnabled: true
                                )
                            }
                                .id(game.id)
                                .offset(y: resolvedGameLibraryVerticalPosition)
                                .registerGameLibraryFocusScrollTarget(
                                    for: game.id,
                                    in: focusScrollCoordinator,
                                    isEnabled: controllerInput != nil
                                )
                        }
                    }
                    .padding(.horizontal)
                    .padding(.bottom, 20)
                }
                .background(alignment: .top) {
                    if controllerInput == nil {
                        touchLibraryLongPressObserver
                    }
                    if controllerInput != nil {
                        ZStack {
                            GameLibraryControllerRightStickTarget(
                                controllerInput: controllerInput,
                                availability: controllerScrollAvailability,
                                axes: .vertical,
                                priority: 10,
                                onReachedLeadingEdge:
                                    controllerRightStickReachedTop
                            )
                            Color.clear
                                .id(GameLibraryScrollObserver.topAnchor)
                        }
                        .frame(height: 0)
                    }
                }
                .padding(.top, showsPageOwnedLargeTitle ? 0 : 12)
            }
            .contentMargins(.top, 0, for: .scrollContent)
            .scrollIndicators(.hidden)
            .scrollDisabled(false)
            .scrollBounceBehavior(.always)
            .trackGameLibraryScrollPhase(gameLibraryActivity)
            .transaction { transaction in
                if transaction.isContinuous || gameLibraryActivity.isScrolling {
                    transaction.animation = nil
                }
            }
            .background(
                Group {
                    if hasCustomBackground {
                        Color.clear
                    } else {
                        LinearGradient(
                            colors: [Color(.systemGroupedBackground), Color(.secondarySystemGroupedBackground)],
                            startPoint: .top,
                            endPoint: .bottom
                        )
                    }
                }
            )
            .overlay {
                GameLibraryScrollObserver(
                    focusState: controllerFocusState,
                    proxy: proxy,
                    favoriteScrollRequest: favoriteScrollRequest,
                    restoreState: libraryScrollRestoreState,
                    topAlignment: .top,
                    focusScrollCoordinator: focusScrollCoordinator,
                    focusScrollAxis: .vertical
                )
            }
        }
    }

    private func emptyLibrary(containerSize: CGSize) -> some View {
        ScrollView {
            VStack(spacing: 0) {
                if controllerInput != nil {
                    GameLibraryControllerRightStickTarget(
                        controllerInput: controllerInput,
                        availability: controllerScrollAvailability,
                        axes: .vertical,
                        priority: 10,
                        onReachedLeadingEdge: controllerRightStickReachedTop
                    )
                    .frame(height: 0)
                }
                if showsPageOwnedLargeTitle {
                    EmbeddedMenuLargeTitle(
                        title: settings.localized("Games"),
                        usesARMSX2Logo: useGamesLogo
                    )
                }

                emptyState
                    .frame(
                        maxWidth: .infinity,
                        minHeight: max(
                            300,
                            containerSize.height
                                - (showsPageOwnedLargeTitle ? 64 : 0)
                        ),
                        alignment: .center
                    )
            }
        }
        .contentMargins(.top, 0, for: .scrollContent)
        .scrollIndicators(.hidden)
        .scrollDisabled(false)
        .scrollBounceBehavior(.always)
        .background(
            Group {
                if hasCustomBackground {
                    Color.clear
                } else {
                    LinearGradient(
                        colors: [
                            Color(.systemGroupedBackground),
                            Color(.secondarySystemGroupedBackground),
                        ],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                }
            }
        )
    }

    private func coverFlowLibrary(containerSize: CGSize) -> some View {
        // Card geometry is a saved presentation preference, not controller
        // state. Keep one metric set so connecting a pad cannot resize the
        // complete landscape rail.
        let controllerConnected = controllerInput?.hasConnectedController == true
        let metrics = CoverFlowMetrics(
            containerSize: containerSize,
            controllerOptimized: true,
            cardScale: resolvedGameCardScale,
            widthScale: resolvedGameCardWidthScale,
            heightScale: resolvedGameCardHeightScale,
            gapScale: resolvedGameCardGapScale,
            contentPaddingScale: resolvedGameCardContentPaddingScale,
            verticalPaddingScale: resolvedGameCardVerticalPaddingScale,
            textSpacingScale: resolvedGameCardTextSpacingScale,
            cardCornerRadius: resolvedGameCardCornerRadius
        )
        let availableHeight = max(0, containerSize.height - 12)
        let cardWidth = coverFlowCardWidth(metrics: metrics)
        // A centered target needs half a viewport of usable content on both
        // edges. The previous fixed 20–32 point padding made it geometrically
        // impossible to center the first and last cards.
        let horizontalPadding = max(
            metrics.horizontalPadding,
            (containerSize.width - cardWidth) / 2
        )
        return ZStack {
            if hasCustomBackground {
                Color.clear
            } else {
                LinearGradient(
                    colors: [
                        Color(.systemGroupedBackground),
                        Color(.secondarySystemGroupedBackground),
                    ],
                    startPoint: .top,
                    endPoint: .bottom
                )
            }

            ScrollViewReader { proxy in
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(alignment: .center, spacing: metrics.itemSpacing) {
                    if games.isEmpty && displayedRunningGameName == nil {
                        emptyState
                            .frame(
                                width: max(
                                    0,
                                    containerSize.width - (horizontalPadding * 2)
                                ),
                                height: max(
                                    0,
                                    availableHeight - (metrics.verticalPadding * 2)
                                )
                            )
                    }

                    if let gameName = displayedRunningGameName {
                        vmStatusCoverCard(gameName: gameName, metrics: metrics)
                            .id(gameLibraryNowRunningFocusID)
                            .offset(y: resolvedGameLibraryVerticalPosition)
                            .isolatedMenuGlassContainer()
                            .compositingGroup()
                            .scaleEffect(displayedNowRunningCardScale)
                            .opacity(displayedNowRunningCardOpacity)
                            .animation(
                                .smooth(duration: 0.65, extraBounce: 0.04),
                                value: displayedNowRunningCardScale
                            )
                            .animation(.linear(duration: 0.65), value: displayedNowRunningCardOpacity)
                            .transition(.scale(scale: 0.86).combined(with: .opacity))
                    }

                    ForEach(games) { game in
                        GameLibraryControllerFocusScope(
                            focusState: controllerFocusState,
                            gameID: game.id,
                            isEnabled: controllerInput != nil
                        ) { controllerFocused in
                            coverFlowCard(
                                game,
                                metrics: metrics,
                                controllerFocused: controllerFocused,
                                visualEffectsEnabled: true
                            )
                        }
                            .id(game.id)
                            .offset(y: resolvedGameLibraryVerticalPosition)
                            .registerGameLibraryFocusScrollTarget(
                                for: game.id,
                                in: focusScrollCoordinator,
                                isEnabled: controllerInput != nil
                            )
                    }
                    }
                    .scrollTargetLayout()
                    .frame(
                        minWidth: max(0, containerSize.width - (horizontalPadding * 2)),
                        minHeight: max(0, availableHeight - (metrics.verticalPadding * 2)),
                        alignment: .center
                    )
                    .padding(.horizontal, horizontalPadding)
                    .padding(.vertical, metrics.verticalPadding)
                    .background(alignment: .leading) {
                        if controllerInput == nil {
                            touchLibraryLongPressObserver
                        }
                        if controllerInput != nil {
                            ZStack {
                                GameLibraryControllerRightStickTarget(
                                    controllerInput: controllerInput,
                                    availability: controllerScrollAvailability,
                                    axes: .horizontal,
                                    priority: 10,
                                    pointsPerSecond: 820,
                                    onReachedLeadingEdge:
                                        controllerRightStickReachedFirstCoverFlowGame
                                )
                                Color.clear
                                    .id(GameLibraryScrollObserver.topAnchor)
                            }
                            .frame(width: 0)
                        }
                    }
                }
                .scrollIndicators(.hidden)
                .scrollDisabled(false)
                .scrollBounceBehavior(.always, axes: .horizontal)
                .trackGameLibraryScrollPhase(gameLibraryActivity)
                .transaction { transaction in
                    if transaction.isContinuous {
                        transaction.animation = nil
                    }
                }
                .overlay {
                    GameLibraryScrollObserver(
                        focusState: controllerFocusState,
                        proxy: proxy,
                        favoriteScrollRequest: favoriteScrollRequest,
                        restoreState: libraryScrollRestoreState,
                        topAlignment: .leading,
                        usesCompactControllerAnimation: true,
                        focusScrollCoordinator: focusScrollCoordinator,
                        focusScrollAxis: .horizontal
                    )
                }
            }
        }
        .frame(height: availableHeight)
        .overlay {
            ZStack {
                if controllerConnected {
                    let geometry = controllerCoverFlowFocusGeometry(
                        metrics: metrics,
                        availableHeight: availableHeight
                    )
                    GameLibraryControllerCoverFlowLaunchAnchor(
                        focusState: controllerFocusState,
                        registry: gameplayLaunchCardRegistry,
                        width: geometry.width,
                        height: geometry.height,
                        centerCorrection: geometry.centerCorrection,
                        verticalPosition: resolvedGameLibraryVerticalPosition
                    )
                }
            }
        }
        // Keep the cover-flow rail below the top toolbar. The focused card now
        // publishes its own transformed bounds, so its focus orbs move with the
        // cover-flow content instead of targeting a viewport-level rectangle.
        .frame(maxHeight: .infinity, alignment: .top)
        .offset(y: 30)
    }

    private func controllerCoverFlowFocusGeometry(
        metrics: CoverFlowMetrics,
        availableHeight: CGFloat
    ) -> (width: CGFloat, height: CGFloat, centerCorrection: CGFloat) {
        let width = coverFlowCardWidth(metrics: metrics)
        let titleHeight: CGFloat = hideGameName
            ? 0
            : (singleLineGameNames
                ? (metrics.isCompact ? 20 : 22)
                : (metrics.isCompact ? 38 : 46)
            ) * resolvedGameNameTextScale
        let metadataHeight: CGFloat = hideGameInfo
            ? 0
            : (metrics.isCompact ? 14 : 17) * resolvedGameInfoTextScale
        let coverToTextSpacing = hideGameName && hideGameInfo
            ? 0
            : metrics.cardSpacing
        let interTextSpacing = hideGameName || hideGameInfo
            ? 0
            : 4 * resolvedGameCardTextSpacingScale
        let height = metrics.cardVerticalPadding * 2
            + metrics.coverHeight
            + coverToTextSpacing
            + titleHeight
            + interTextSpacing
            + metadataHeight
        let stackMinimumHeight = max(
            0,
            availableHeight - (metrics.verticalPadding * 2)
        )
        return (
            width,
            height,
            max(0, (height - stackMinimumHeight) / 2)
        )
    }

    private func coverFlowCardWidth(metrics: CoverFlowMetrics) -> CGFloat {
        let textIsVisible = !hideGameName || !hideGameInfo
        let contentWidth = textIsVisible
            ? max(metrics.coverWidth, metrics.textWidth)
            : metrics.coverWidth
        return contentWidth + (metrics.cardHorizontalPadding * 2)
    }

    private func gameGridColumns(for containerWidth: CGFloat) -> [GridItem] {
        let minimumCardWidth = 142 * resolvedGameCardScale
            * resolvedGameCardWidthScale
        return Array(
            repeating: GridItem(
                .flexible(minimum: minimumCardWidth),
                spacing: 14 * resolvedGameCardGapScale,
                alignment: .top
            ),
            count: gameGridColumnCount(for: containerWidth)
        )
    }

    private func gameGridColumnCount(for containerWidth: CGFloat) -> Int {
        let availableWidth = max(0, containerWidth - 32)
        let minimumCardWidth = 142 * resolvedGameCardScale
            * resolvedGameCardWidthScale
        return max(
            1,
            Int(
                (availableWidth + 14 * resolvedGameCardGapScale)
                    / (minimumCardWidth + 14 * resolvedGameCardGapScale)
            )
        )
    }

    private func nativeAlertBinding(_ source: Binding<Bool>) -> Binding<Bool> {
        Binding(
            // App-owned prompts render through ControllerNavigationAlert for
            // both touch and controller interaction, giving every confirmation
            // the same clear Liquid Glass surface.
            get: { false },
            set: { presented in
                if !presented && controllerInput == nil {
                    source.wrappedValue = false
                }
            }
        )
    }

    private var activeControllerAlertKind: GameLibraryControllerAlertKind? {
        if pendingAutomaticCustomSkinLaunch != nil { return .automaticCustomSkin }
        if showRestartAlert { return .restart }
        if showStopAlert { return .stop }
        if pendingDeleteDataGame != nil { return .deleteGameData }
        if pendingDeleteGame != nil { return .deleteGame }
        if gameActionMessage != nil { return .gameAction }
        if coverStore.showCoverAlert { return .coverResult }
        if showGameReplacementAlert { return .replaceFiles }
        if showBackgroundAssetError { return .backgroundError }
        return nil
    }

    private func controllerAlertTitle(
        for kind: GameLibraryControllerAlertKind
    ) -> String {
        switch kind {
        case .automaticCustomSkin:
            return settings.localized(
                pendingAutomaticCustomSkinLaunch?.purpose == .library
                    ? "Per-Game Custom Skin"
                    : "Set New Per-Game Custom Skin?"
            )
        case .restart:
            return settings.localized("Restart VM?")
        case .stop:
            return settings.localized("Stop Emulation?")
        case .deleteGameData:
            return settings.localized("Delete Game Data?")
        case .deleteGame:
            return settings.localized("Delete Game?")
        case .gameAction:
            return settings.localized(gameActionTitle.isEmpty ? "Game Action" : gameActionTitle)
        case .coverResult:
            return settings.localized("Cover Result")
        case .replaceFiles:
            return settings.localized("Replace existing files?")
        case .backgroundError:
            return settings.localized("Background image could not be loaded.")
        }
    }

    private func controllerAlertMessage(
        for kind: GameLibraryControllerAlertKind
    ) -> String {
        switch kind {
        case .automaticCustomSkin:
            return settings.localized(
                pendingAutomaticCustomSkinLaunch?.purpose == .library
                    ? "Choose the skin and optional linked controller layout for this game."
                    : "Apply the recommended skin and controller layout before launching?"
            )
        case .restart:
            let target = pendingGameName.isEmpty
                ? "Boot BIOS"
                : (pendingGameName as NSString).lastPathComponent
            return "\(settings.localized("VM is currently running."))\n\(settings.localized("Shut down and start")) \(settings.localized(target))?"
        case .stop:
            return settings.localized("This will shut down the running game. All unsaved progress will be lost.")
        case .deleteGameData:
            return settings.localized("This clears save states, PNACH files, per-game settings, compatibility overrides, and generated cache for this game. Memory card contents are not deleted.")
        case .deleteGame:
            return settings.localized("Delete the selected game file? You can also remove its generated game data at the same time.")
        case .gameAction:
            return gameActionMessage ?? ""
        case .coverResult:
            return coverStore.lastCoverMessage ?? ""
        case .replaceFiles:
            return FileImportHandler.replacementConfirmationMessage(
                for: existingGameImportFileNames
            )
        case .backgroundError:
            return ""
        }
    }

    private func controllerAlertPreviewImageURL(
        for kind: GameLibraryControllerAlertKind
    ) -> URL? {
        guard kind == .automaticCustomSkin,
              let proposal = pendingAutomaticCustomSkinLaunch?.proposal else {
            return nil
        }
        return AutomaticCustomSkinManager.shared.previewURL(for: proposal)
    }

    private func controllerAlertPreviewSkinDescriptor(
        for kind: GameLibraryControllerAlertKind
    ) -> VPadSkinDescriptor? {
        guard kind == .automaticCustomSkin,
              let proposal = pendingAutomaticCustomSkinLaunch?.proposal else {
            return nil
        }
        return VPadSkinLibraryStore.shared.descriptor(id: proposal.skinID)
    }

    private func controllerAlertPreviewAccessibilityLabel(
        for kind: GameLibraryControllerAlertKind
    ) -> String? {
        guard kind == .automaticCustomSkin,
              let proposal = pendingAutomaticCustomSkinLaunch?.proposal else {
            return nil
        }
        return "\(settings.localized("Preview")): \(proposal.skinName)"
    }

    private func controllerAlertDetails(
        for kind: GameLibraryControllerAlertKind
    ) -> [ControllerNavigationAlertDetail] {
        guard kind == .automaticCustomSkin,
              let pending = pendingAutomaticCustomSkinLaunch else {
            return []
        }
        var details: [ControllerNavigationAlertDetail] = [
            .init(
                id: "skin",
                label: settings.localized("Skin"),
                value: pending.proposal.skinName,
                onPrevious: {
                    selectAutomaticCustomSkin(offset: -1)
                },
                onNext: {
                    selectAutomaticCustomSkin(offset: 1)
                },
                onActivate: {
                    selectAutomaticCustomSkin(offset: 1)
                }
            ),
            .init(
                id: "game",
                label: settings.localized("Game"),
                value: pending.game.displayName
            ),
            .init(
                id: "setCustomLayout",
                label: settings.localized("Set custom layout"),
                value: pending.setCustomLayout
                    ? settings.localized("On")
                    : settings.localized("Off"),
                isOn: pending.setCustomLayout,
                onPrevious: {
                    setAutomaticCustomLayout(false)
                },
                onNext: {
                    setAutomaticCustomLayout(true)
                },
                onActivate: {
                    toggleAutomaticCustomLayout()
                }
            ),
        ]
        if pending.purpose == .gameLaunch {
            details.append(.init(
                id: "doNotShowAgain",
                label: settings.localized("Do not show this again"),
                value: pending.doNotShowAgain
                    ? settings.localized("On")
                    : settings.localized("Off"),
                isOn: pending.doNotShowAgain,
                onPrevious: {
                    setAutomaticCustomSkinPromptSuppressed(false)
                },
                onNext: {
                    setAutomaticCustomSkinPromptSuppressed(true)
                },
                onActivate: {
                    toggleAutomaticCustomSkinPromptSuppressed()
                }
            ))
        }
        return details
    }

    private func controllerAlertActions(
        for kind: GameLibraryControllerAlertKind
    ) -> [ControllerNavigationAlertAction] {
        switch kind {
        case .automaticCustomSkin:
            if pendingAutomaticCustomSkinLaunch?.purpose == .library {
                return [
                    .init(id: "apply", title: settings.localized("Apply")),
                    .init(id: "cancel", title: settings.localized("Cancel")),
                ]
            }
            return [
                .init(id: "apply", title: settings.localized("OK")),
                .init(id: "cancelLaunch", title: settings.localized("Cancel and Launch")),
            ]
        case .restart:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "restart", title: settings.localized("Restart"), isDestructive: true),
            ]
        case .stop:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "stop", title: settings.localized("Stop"), isDestructive: true),
            ]
        case .deleteGameData:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "deleteData", title: settings.localized("Delete Game Data"), isDestructive: true),
            ]
        case .deleteGame:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "deleteROM", title: settings.localized("Delete ROM"), isDestructive: true),
                .init(id: "deleteAll", title: settings.localized("Delete ROM + Game Data"), isDestructive: true),
            ]
        case .replaceFiles:
            return [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "replace", title: settings.localized("Replace"), isDestructive: true),
            ]
        case .gameAction, .coverResult, .backgroundError:
            return [.init(id: "ok", title: settings.localized("OK"))]
        }
    }

    private func handleControllerAlertCommand(
        _ command: MenuControllerCommand,
        kind: GameLibraryControllerAlertKind
    ) {
        let interactiveDetailCount = controllerAlertDetails(for: kind)
            .filter(\.isInteractive)
            .count
        let actionCount = controllerAlertActions(for: kind).count
        let selectionCount = interactiveDetailCount + actionCount
        switch command {
        case .up, .upLeft, .upRight:
            let next: Int
            if controllerAlertSelectedIndex >= interactiveDetailCount {
                next = max(0, interactiveDetailCount - 1)
            } else {
                next = max(0, controllerAlertSelectedIndex - 1)
            }
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .down, .downLeft, .downRight:
            let next: Int
            if controllerAlertSelectedIndex < interactiveDetailCount - 1 {
                next = controllerAlertSelectedIndex + 1
            } else if controllerAlertSelectedIndex < interactiveDetailCount,
                      actionCount > 0 {
                next = interactiveDetailCount
            } else {
                next = controllerAlertSelectedIndex
            }
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .left:
            if kind == .automaticCustomSkin,
               controllerAlertSelectedIndex < interactiveDetailCount {
                adjustAutomaticCustomSkinControl(
                    at: controllerAlertSelectedIndex,
                    incrementing: false
                )
                controllerInput?.playFeedback(.move(command))
                return
            }
            let next = max(
                interactiveDetailCount,
                controllerAlertSelectedIndex - 1
            )
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .right:
            if kind == .automaticCustomSkin,
               controllerAlertSelectedIndex < interactiveDetailCount {
                adjustAutomaticCustomSkinControl(
                    at: controllerAlertSelectedIndex,
                    incrementing: true
                )
                controllerInput?.playFeedback(.move(command))
                return
            }
            let next = min(
                selectionCount - 1,
                controllerAlertSelectedIndex + 1
            )
            guard next != controllerAlertSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerAlertSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .activate:
            if kind == .automaticCustomSkin,
               controllerAlertSelectedIndex < interactiveDetailCount {
                activateAutomaticCustomSkinControl(
                    at: controllerAlertSelectedIndex
                )
            } else {
                performControllerAlertAction(
                    controllerAlertSelectedIndex - interactiveDetailCount,
                    for: kind
                )
            }
            controllerInput?.playFeedback(.activate)
        case .back:
            if kind == .automaticCustomSkin {
                // The generic controller feedback is deferred until after the
                // focus transaction. This chooser clears its pending launch
                // state immediately, so play Return before dismissing it.
                cancelAutomaticCustomSkinLaunch(playsReturnSound: true)
            } else {
                dismissControllerAlert(kind)
                controllerInput?.playFeedback(.back)
            }
        case .toggleFavorite, .showContextMenu:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func adjustAutomaticCustomSkinControl(
        at index: Int,
        incrementing: Bool
    ) {
        switch index {
        case 0:
            selectAutomaticCustomSkin(offset: incrementing ? 1 : -1)
        case 1:
            setAutomaticCustomLayout(incrementing)
        case 2:
            setAutomaticCustomSkinPromptSuppressed(incrementing)
        default:
            break
        }
    }

    private func activateAutomaticCustomSkinControl(at index: Int) {
        switch index {
        case 0:
            selectAutomaticCustomSkin(offset: 1)
        case 1:
            toggleAutomaticCustomLayout()
        case 2:
            toggleAutomaticCustomSkinPromptSuppressed()
        default:
            break
        }
    }

    private func selectAutomaticCustomSkin(offset: Int) {
        guard var pending = pendingAutomaticCustomSkinLaunch,
              pending.proposals.count > 1 else { return }
        let count = pending.proposals.count
        pending.selectedProposalIndex = (
            pending.selectedProposalIndex + offset + count
        ) % count
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func setAutomaticCustomLayout(_ enabled: Bool) {
        guard var pending = pendingAutomaticCustomSkinLaunch,
              pending.setCustomLayout != enabled else { return }
        pending.setCustomLayout = enabled
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func toggleAutomaticCustomLayout() {
        guard var pending = pendingAutomaticCustomSkinLaunch else { return }
        pending.setCustomLayout.toggle()
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func setAutomaticCustomSkinPromptSuppressed(_ suppressed: Bool) {
        guard var pending = pendingAutomaticCustomSkinLaunch,
              pending.doNotShowAgain != suppressed else { return }
        pending.doNotShowAgain = suppressed
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func toggleAutomaticCustomSkinPromptSuppressed() {
        guard var pending = pendingAutomaticCustomSkinLaunch else { return }
        pending.doNotShowAgain.toggle()
        pendingAutomaticCustomSkinLaunch = pending
    }

    private func performControllerAlertAction(
        _ index: Int,
        for kind: GameLibraryControllerAlertKind
    ) {
        switch kind {
        case .automaticCustomSkin:
            completeAutomaticCustomSkinLaunch(apply: index == 0)
        case .restart:
            guard index == 1 else {
                dismissControllerAlert(kind)
                return
            }
            showRestartAlert = false
            if pendingGameName.isEmpty {
                pendingGameplayLaunchTransition = nil
                pendingGameplayPadSerial = nil
                pendingGameplayPadCRC = nil
                appState.shutdownAndBootBIOS()
            } else {
                let transition = pendingGameplayLaunchTransition
                pendingGameplayLaunchTransition = nil
                appState.prepareGameplayPadSelection(
                    forSerial: pendingGameplayPadSerial,
                    crc: pendingGameplayPadCRC
                )
                pendingGameplayPadSerial = nil
                pendingGameplayPadCRC = nil
                appState.shutdownAndBoot(
                    isoName: pendingGameName,
                    launchTransition: transition
                )
            }
        case .stop:
            if index == 1 {
                scheduleStopRunningGame()
            } else {
                showStopAlert = false
            }
        case .deleteGameData:
            if index == 1, let game = pendingDeleteDataGame {
                deleteGameData(game)
            }
            pendingDeleteDataGame = nil
        case .deleteGame:
            if let game = pendingDeleteGame {
                if index == 1 {
                    deleteGame(game, deleteData: false)
                } else if index == 2 {
                    deleteGame(game, deleteData: true)
                }
            }
            pendingDeleteGame = nil
        case .gameAction:
            gameActionMessage = nil
        case .coverResult:
            coverStore.showCoverAlert = false
        case .replaceFiles:
            if index == 1 {
                importGames(
                    pendingGameImportURLs,
                    allowReplacingExistingFiles: true
                )
            }
            showGameReplacementAlert = false
            clearPendingGameImport()
        case .backgroundError:
            showBackgroundAssetError = false
        }
    }

    private func dismissControllerAlert(
        _ kind: GameLibraryControllerAlertKind
    ) {
        switch kind {
        case .automaticCustomSkin:
            cancelAutomaticCustomSkinLaunch()
        case .restart:
            showRestartAlert = false
            pendingGameplayLaunchTransition = nil
            pendingGameplayPadSerial = nil
            pendingGameplayPadCRC = nil
        case .stop:
            showStopAlert = false
        case .deleteGameData:
            pendingDeleteDataGame = nil
        case .deleteGame:
            pendingDeleteGame = nil
        case .gameAction:
            gameActionMessage = nil
        case .coverResult:
            coverStore.showCoverAlert = false
        case .replaceFiles:
            showGameReplacementAlert = false
            clearPendingGameImport()
        case .backgroundError:
            showBackgroundAssetError = false
        }
    }

    /// Tapping the scrim or pressing Back dismisses the recommendation without
    /// launching the game. "Cancel and Launch" remains the only dismissal path
    /// which deliberately continues into gameplay.
    private func cancelAutomaticCustomSkinLaunch(
        playsReturnSound: Bool = false
    ) {
        if playsReturnSound {
            MenuAudioPackManager.shared.playEvent(.return)
        }
        pendingAutomaticCustomSkinLaunch = nil
        pendingGameplayLaunchTransition = nil
        pendingGameplayPadSerial = nil
        pendingGameplayPadCRC = nil
    }

    private var nonContextControllerNavigationPresentationActive: Bool {
        controllerLayoutMenuPresented
            || controllerMoreMenuPresented
            || showGameImporter
            || showCoverImporter
            || showCoverPhotoPicker
            || showRestartAlert
            || showStopAlert
            || showCoverTemplateEditor
            || showGameReplacementAlert
            || pendingAutomaticCustomSkinLaunch != nil
            || showBackgroundAssetError
            || gameInfoTarget != nil
            || gameSettingsTarget != nil
            || renameTarget != nil
            || discLinkTarget != nil
            || cheatsManagerTarget != nil
            || pendingDeleteGame != nil
            || pendingDeleteDataGame != nil
            || gameActionMessage != nil
            || coverStore.showCoverAlert
            || deferredControllerContextMenuSelection != nil
    }

    /// Per-game settings owns a shared accessibility session and must remain
    /// routable. The other library presentations are driven by the custom
    /// command listener or native UIKit and therefore capture the retained
    /// library explicitly.
    private var manuallyCapturedControllerPresentationActive: Bool {
        nonContextControllerNavigationPresentationActive
            && gameSettingsTarget == nil
    }

    private var controllerNavigationPresentationActive: Bool {
        controllerContextMenuState.gameID != nil
            || gameLibraryViewOptionsPresented
            || nonContextControllerNavigationPresentationActive
    }

    private var controllerLibraryNavigationAllowed: Bool {
        menuTabIsActive
            && appState.currentScreen == .menu
            && (controllerInput?.isControllerNavigationEnabled ?? false)
            && !(controllerInput?.isNavigationCaptured ?? false)
            && !controllerNavigationPresentationActive
    }

    private var controllerLibraryRightStickAllowed: Bool {
        controllerLibraryNavigationAllowed
            && controllerInput?.navigationZone == .library
    }

    private func updateControllerScrollAvailability() {
        controllerScrollAvailability.isEnabled =
            controllerLibraryRightStickAllowed
    }

    private func controllerInputAvailabilityDidChange() {
        updateControllerScrollAvailability()
        // Settings can still own input during the Games view's onAppear.
        // Restore once that owner leaves, but never interrupt touch/analog
        // focus release or take focus away from the bottom tab bar.
        if controllerLibraryRightStickAllowed, !controllerFocusReleased,
           !controllerNavigationActive, controllerSelectedGameID == nil {
            _ = restoreRememberedControllerContentFocus()
        }
    }

    private func releaseControllerFocus() {
        focusScrollCoordinator.stopAnimation()
        controllerFocusReleased = true
        controllerNavigationActive = false
        controllerSelectedGameID = nil
        controllerNowRunningFocusState.selectedAction = nil
        controllerContextMenuState.selectedAction = nil
        updateControllerScrollAvailability()
    }

    private func restoreCenteredControllerFocusAfterRightStickScrolling() {
        guard controllerLibraryRightStickAllowed,
              let gameID = focusScrollCoordinator.centeredVisibleGameID(
                  axis: usesLandscapeCoverFlow ? .horizontal : .vertical
              ),
              games.contains(where: { $0.id == gameID }) else { return }
        controllerFocusReleased = false
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = gameID
        updateControllerScrollAvailability()
    }

    private func handleControllerCommand(_ event: MenuControllerInputEvent) {
        guard !event.isNavigationSessionRouted else { return }
        if let captureOwner = event.captureOwner,
           captureOwner
            != MenuControllerNavigationCaptureOwner.gameLibraryPresentation {
            return
        }
        let command = event.command
        controllerFocusReleased = false
        if let game = controllerContextMenuGame {
            handleControllerContextMenuCommand(command, for: game)
            return
        }

        // OrbitKeys consumes the controller profile directly so it can retain
        // both sticks, triggers, shoulder buttons, L3/R3, and all four face
        // buttons. The library capture remains mounted only to keep every
        // underlying card and toolbar passive.
        if renameTarget != nil {
            return
        }

        if controllerLayoutMenuPresented {
            handleControllerLayoutMenuCommand(command)
            return
        }

        if controllerMoreMenuPresented {
            handleControllerMoreMenuCommand(command)
            return
        }

        if let alertKind = activeControllerAlertKind {
            handleControllerAlertCommand(command, kind: alertKind)
            return
        }

        // These destinations own explicit controller focus and activation.
        // Leave the retained library completely passive underneath their
        // presentation so one button press cannot trigger two actions.
        if gameSettingsTarget != nil
            || cheatsManagerTarget != nil {
            return
        }

        if controllerNavigationPresentationActive {
            switch command {
            case .up, .upRight, .right, .downRight,
                 .down, .downLeft, .left, .upLeft:
                // SwiftUI's focused Form/List controls receive the same
                // physical input. Add the matching controller sensation while
                // keeping the retained library from moving underneath them.
                controllerInput?.playFeedback(.move(command))
            case .activate:
                controllerInput?.playFeedback(.activate)
            case .back:
                if dismissControllerPresentation() {
                    controllerInput?.playFeedback(.back)
                }
            case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
                break
            }
            return
        }

        guard controllerLibraryNavigationAllowed else { return }

        if controllerInput?.navigationZone == .tabBar {
            return
        }

        if controllerInput?.navigationZone == .topToolbar {
            handleControllerToolbarCommand(command)
            return
        }

        if controllerInput?.suppressesLibraryEvent(event) == true {
            return
        }

        if games.isEmpty && displayedRunningGameName == nil {
            handleEmptyLibraryControllerCommand(command)
            return
        }

        switch command {
        case .up, .upRight, .right, .downRight,
             .down, .downLeft, .left, .upLeft:
            let moved = moveControllerSelection(command)
            if !moved, command.verticalComponent == .up {
                controllerNavigationActive = false
                controllerInput?.setNavigationZone(.topToolbar)
                controllerInput?.playFeedback(.move(.up))
                return
            }
            if !moved, command.verticalComponent == .down {
                controllerInput?.prepareTabBarOrbTransition(
                    sourceFrame: controllerFocusedCardFrameInWindow()
                )
                controllerNavigationActive = false
                controllerInput?.setNavigationZone(.tabBar)
                controllerInput?.playFeedback(.move(.down))
                return
            }
            controllerInput?.playFeedback(
                moved ? .move(command) : .boundary
            )
        case .activate:
            if let nowRunningAction = controllerNowRunningFocusState.selectedAction,
               displayedRunningGameName != nil {
                controllerInput?.playFeedback(.activate)
                switch nowRunningAction {
                case .resume:
                    appState.returnToGame()
                case .stop:
                    showStopAlert = true
                }
                return
            }
            guard let game = controllerSelectedGame else {
                if selectInitialControllerGame(preferLast: false) {
                    controllerInput?.playFeedback(.move(.down))
                }
                return
            }
            // Controller activation launches immediately; the automatic skin
            // recommendation remains a touch/library-management surface.
            open(game, allowsAutomaticSkinPrompt: false)
        case .back:
            guard controllerNavigationActive else { return }
            controllerSelectedGameID = nil
            controllerNowRunningFocusState.selectedAction = nil
            controllerNavigationActive = false
            controllerInput?.playFeedback(.back)
        case .toggleFavorite:
            guard controllerNowRunningFocusState.selectedAction == nil else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            guard let game = controllerSelectedGame else { return }
            let isFavorite = toggleFavorite(
                game,
                playsSelectionFeedback: false
            )
            controllerInput?.playFeedback(
                .favorite(isFavorite: isFavorite)
            )
        case .showContextMenu:
            guard controllerNowRunningFocusState.selectedAction == nil else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            guard let game = controllerSelectedGame else { return }
            openControllerContextMenu(for: game)
        case .previousTab, .nextTab:
            break
        }
    }

    private func scheduleLandscapeControllerAutofocus() {
        guard landscapeControllerAutofocusPending else { return }

        landscapeControllerAutofocusTask?.cancel()
        landscapeControllerAutofocusTask = Task { @MainActor in
            // Rotation replaces the portrait grid with the horizontal cover
            // flow. Let that stable-ID scroll view mount before publishing its
            // centered selection request.
            await Task.yield()
            await Task.yield()
            guard !Task.isCancelled,
                  landscapeControllerAutofocusPending,
                  usesLandscapeGameCardScale,
                  controllerInput?.hasConnectedController == true,
                  controllerLibraryNavigationAllowed,
                  !games.isEmpty else { return }

            landscapeControllerAutofocusPending = false
            controllerInput?.setNavigationZone(.library)
            _ = restoreRememberedControllerContentFocus()
                || selectInitialControllerGame(preferLast: true)
            landscapeControllerAutofocusTask = nil
        }
    }

    private func handleControllerLibraryEntry(preferLast: Bool) {
        guard controllerLibraryNavigationAllowed else { return }
        controllerFocusReleased = false
        if games.isEmpty && displayedRunningGameName == nil {
            // Treat the empty-state import action as the library's one card.
            // This keeps it focusable after entering from either toolbar.
            controllerNavigationActive = true
            controllerSelectedGameID = nil
            return
        }
        if focusRunningControllerGameIfAvailable() { return }
        if restoreRememberedControllerContentFocus() { return }
        if games.contains(where: isRunning)
            || (displayedRunningGameName != nil && !preferLast) {
            _ = selectInitialControllerGame(preferLast: false)
            return
        }
        _ = selectInitialControllerGame(preferLast: preferLast)
    }

    @discardableResult
    private func restoreRememberedControllerContentFocus() -> Bool {
        guard controllerLibraryNavigationAllowed else { return false }
        let rememberedCard = controllerInput?.rememberedNavigationFocusKey(
            forScope: gameLibraryControllerCardMemoryScope
        )
        let rememberedContent = controllerInput?.rememberedNavigationFocusKey(
            forScope: gameLibraryControllerMemoryScope
        )
        // Prefer the retained card, but fall back if it was deleted or filtered
        // out. The legacy content key also restores existing session memories.
        // Returning from a Now Running action/confirmation restores that action,
        // not the older card retained separately for tab changes.
        let candidates = rememberedContent?.hasPrefix("now-running:") == true
            && appState.runningGameName != nil && !nowRunningRemovalAnimationActive
                ? [rememberedContent, rememberedCard] : [rememberedCard, rememberedContent]
        guard let key = candidates.compactMap({ $0 }).first(where: { candidate in
            !candidate.hasPrefix("game:") || games.contains(where: { game in
                game.id == String(candidate.dropFirst("game:".count))
            })
        }) else { return false }
        controllerFocusReleased = false
        if key.hasPrefix("toolbar:"),
           let rawValue = Int(key.dropFirst("toolbar:".count)),
           let action = GameLibraryControllerToolbarAction(rawValue: rawValue) {
            controllerToolbarFocusState.selectedAction = action
            controllerNavigationActive = false
            controllerNowRunningFocusState.selectedAction = nil
            controllerInput?.setNavigationZone(.topToolbar)
            updateControllerScrollAvailability()
            return true
        }
        if key.hasPrefix("game:") {
            let gameID = String(key.dropFirst("game:".count))
            guard games.contains(where: { $0.id == gameID }) else {
                return false
            }
            controllerNavigationActive = true
            controllerNowRunningFocusState.selectedAction = nil
            controllerSelectedGameID = gameID
            controllerFocusState.requestSelectionScroll()
            return true
        }
        if key.hasPrefix("now-running:"),
           displayedRunningGameName != nil,
           let rawValue = Int(key.dropFirst("now-running:".count)),
           let action = GameLibraryNowRunningControllerAction(
               rawValue: rawValue
           ) {
            focusNowRunning(action)
            return true
        }
        return false
    }

    private func rememberControllerToolbarFocus() {
        controllerInput?.rememberNavigationFocusKey(
            "toolbar:\(controllerToolbarFocusState.selectedAction.rawValue)",
            forScope: gameLibraryControllerMemoryScope
        )
    }

    /// Gameplay intentionally releases the library's card models. Restore the
    /// running card only after its model is back and the Games tab can accept
    /// controller focus; otherwise the temporary empty array clears selection
    /// and the later reload has no portrait re-entry trigger.
    @discardableResult
    private func focusRunningControllerGameIfAvailable() -> Bool {
        guard runningGameControllerFocusPending,
              controllerInput?.hasConnectedController == true,
              controllerLibraryNavigationAllowed,
              let runningGame = games.first(where: isRunning) else {
            return false
        }

        runningGameControllerFocusPending = false
        landscapeControllerAutofocusPending = false
        landscapeControllerAutofocusTask?.cancel()
        landscapeControllerAutofocusTask = nil
        controllerFocusReleased = false
        controllerInput?.setNavigationZone(.library)
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = runningGame.id
        if usesLandscapeCoverFlow,
           let selectedIndex = controllerFocusState.index(for: runningGame.id)
                ?? games.firstIndex(where: { $0.id == runningGame.id }) {
            preheatLandscapeCovers(around: selectedIndex, direction: 0)
        }
        controllerFocusState.requestSelectionScroll()
        updateControllerScrollAvailability()
        return true
    }

    private var controllerSelectedGame: ISOEntry? {
        guard let controllerSelectedGameID else { return nil }
        if let index = controllerFocusState.index(for: controllerSelectedGameID),
           games.indices.contains(index),
           games[index].id == controllerSelectedGameID {
            return games[index]
        }
        return games.first(where: { $0.id == controllerSelectedGameID })
    }

    private func controllerFocusedCardFrameInWindow() -> CGRect? {
        guard let gameID = controllerSelectedGameID,
              let cardView = gameplayLaunchCardRegistry.view(for: gameID),
              let window = cardView.window else { return nil }
        let frame = cardView.convert(cardView.bounds, to: window)
        guard !frame.isNull, !frame.isInfinite,
              frame.width > 1, frame.height > 1 else { return nil }
        return frame
    }

    @discardableResult
    private func selectInitialControllerGame(preferLast: Bool) -> Bool {
        let runningGame = games.first(where: isRunning)
        if runningGame == nil,
           !preferLast,
           displayedRunningGameName != nil {
            focusNowRunning(.resume)
            return true
        }
        let game: ISOEntry?
        if let runningGame {
            game = runningGame
        } else if preferLast {
            // Preserve a live selection when re-entering from another focus
            // region. With no selection (including a fresh app launch), begin
            // at the first visible game instead of manufacturing a last-card
            // selection from the entry direction.
            game = controllerSelectedGame ?? games.first
        } else {
            game = games.first
        }
        guard let game else {
            if displayedRunningGameName != nil {
                focusNowRunning(.resume)
                return true
            }
            return false
        }
        let alreadySelected = controllerSelectedGameID == game.id
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = game.id
        if usesLandscapeCoverFlow,
           let selectedIndex = controllerFocusState.index(for: game.id)
                ?? games.firstIndex(where: { $0.id == game.id }) {
            preheatLandscapeCovers(around: selectedIndex, direction: 0)
        }
        if alreadySelected {
            controllerFocusState.requestSelectionScroll()
        }
        return true
    }

    private var usesLandscapeCoverFlow: Bool {
        libraryLayout == "grid"
            && landscapeCoverFlowEnabled
            && usesLandscapeGameCardScale
    }

    private func preheatLandscapeCovers(
        around selectedIndex: Int,
        direction: Int
    ) {
        guard usesLandscapeCoverFlow,
              games.indices.contains(selectedIndex) else { return }

        let step = direction == 0 ? 1 : (direction < 0 ? -1 : 1)
        let candidateIndices: [Int]
        if direction == 0 {
            candidateIndices = [selectedIndex, selectedIndex - 1, selectedIndex + 1]
        } else {
            candidateIndices = [
                selectedIndex,
                selectedIndex + step,
                selectedIndex + (step * 2),
            ]
        }
        let items = candidateIndices
            .filter { games.indices.contains($0) }
            .compactMap { index -> CoverThumbnailPreheatItem? in
                let game = games[index]
                guard let coverURL = game.coverURL else { return nil }
                return CoverThumbnailPreheatItem(
                    url: coverURL,
                    signature: game.coverSignature
                )
            }
        let metrics = CoverFlowMetrics(
            containerSize: libraryContainerSize,
            controllerOptimized: true,
            cardScale: resolvedGameCardScale,
            widthScale: resolvedGameCardWidthScale,
            heightScale: resolvedGameCardHeightScale,
            gapScale: resolvedGameCardGapScale,
            contentPaddingScale: resolvedGameCardContentPaddingScale,
            verticalPaddingScale: resolvedGameCardVerticalPaddingScale,
            textSpacingScale: resolvedGameCardTextSpacingScale,
            cardCornerRadius: resolvedGameCardCornerRadius
        )
        let scale = activeMenuWindow()?.screen.scale ?? UIScreen.main.scale
        coverFlowPreheater.schedule(
            items: items,
            width: metrics.coverWidth,
            height: metrics.coverHeight,
            scale: scale
        )
    }

    @discardableResult
    private func moveControllerSelection(
        _ requestedDirection: MenuControllerCommand
    ) -> Bool {
        let direction = controllerDirectionForLayout(requestedDirection)
        if let action = controllerNowRunningFocusState.selectedAction,
           canFocusNowRunning {
            return moveFromNowRunning(action, direction: direction)
        }
        guard !games.isEmpty else {
            controllerSelectedGameID = nil
            controllerNavigationActive = false
            return false
        }

        guard let currentID = controllerSelectedGameID else {
            return selectInitialControllerGame(
                preferLast: direction == .up || direction == .left
            )
        }
        let cachedIndex = controllerFocusState.index(for: currentID)
        guard let currentIndex = cachedIndex.flatMap({ index in
            games.indices.contains(index) && games[index].id == currentID
                ? index
                : nil
        }) ?? games.firstIndex(where: { $0.id == currentID }) else {
            return selectInitialControllerGame(
                preferLast: direction == .up || direction == .left
            )
        }

        let usesCoverFlow = usesLandscapeCoverFlow
        let nextIndex: Int

        if libraryLayout != "grid" {
            switch direction.verticalComponent {
            case .up:
                if currentIndex == 0, canFocusNowRunning {
                    focusNowRunning(.stop)
                    return true
                }
                nextIndex = max(0, currentIndex - 1)
            case .down:
                nextIndex = min(games.count - 1, currentIndex + 1)
            default:
                return false
            }
        } else if usesCoverFlow {
            switch direction.horizontalComponent {
            case .left:
                if currentIndex == 0, canFocusNowRunning {
                    focusNowRunning(.stop)
                    return true
                }
                nextIndex = max(0, currentIndex - 1)
            case .right:
                nextIndex = min(games.count - 1, currentIndex + 1)
            default:
                return false
            }
        } else {
            let columnCount = gameGridColumnCount(
                for: libraryContainerSize.width
            )
            let rowStart = (currentIndex / columnCount) * columnCount
            let rowEnd = min(games.count - 1, rowStart + columnCount - 1)
            switch direction {
            case .left:
                nextIndex = max(rowStart, currentIndex - 1)
            case .right:
                nextIndex = min(rowEnd, currentIndex + 1)
            case .up:
                let targetIndex = currentIndex - columnCount
                guard targetIndex >= 0 else {
                    if canFocusNowRunning {
                        focusNowRunning(.resume)
                        return true
                    }
                    return false
                }
                nextIndex = targetIndex
            case .down:
                let targetIndex = currentIndex + columnCount
                guard games.indices.contains(targetIndex) else { return false }
                nextIndex = targetIndex
            case .upLeft, .upRight, .downLeft, .downRight:
                let currentRow = currentIndex / columnCount
                let currentColumn = currentIndex % columnCount
                let rowDelta = direction.verticalComponent == .up ? -1 : 1
                let columnDelta = direction.horizontalComponent == .left ? -1 : 1
                let targetRow = currentRow + rowDelta
                guard targetRow >= 0 else {
                    if canFocusNowRunning {
                        focusNowRunning(
                            direction.horizontalComponent == .left
                                ? .resume
                                : .stop
                        )
                        return true
                    }
                    return false
                }
                let targetRowStart = targetRow * columnCount
                guard targetRowStart < games.count else { return false }
                let targetRowEnd = min(
                    games.count - 1,
                    targetRowStart + columnCount - 1
                )
                let targetColumn = currentColumn + columnDelta
                guard targetColumn >= 0,
                      targetColumn <= targetRowEnd - targetRowStart else {
                    return false
                }
                nextIndex = targetRowStart + targetColumn
            default:
                return false
            }
        }

        guard nextIndex != currentIndex else { return false }
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        if usesCoverFlow {
            preheatLandscapeCovers(
                around: nextIndex,
                direction: nextIndex - currentIndex
            )
        }
        let staysInGridRow: Bool
        if !usesCoverFlow, libraryLayout == "grid" {
            let columnCount = gameGridColumnCount(
                for: libraryContainerSize.width
            )
            staysInGridRow = currentIndex / columnCount
                == nextIndex / columnCount
        } else {
            staysInGridRow = false
        }
        if reduceMotion && staysInGridRow {
            let gameID = games[nextIndex].id
            controllerFocusState.selectWithoutScrolling(gameID)
            controllerInput?.rememberNavigationFocusKey(
                "game:\(gameID)",
                forScope: gameLibraryControllerMemoryScope
            )
            controllerInput?.rememberNavigationFocusKey(
                "game:\(gameID)",
                forScope: gameLibraryControllerCardMemoryScope
            )
            updateGameCoverThemePreview(for: gameID)
        } else {
            controllerSelectedGameID = games[nextIndex].id
        }
        return true
    }

    private func moveFromNowRunning(
        _ action: GameLibraryNowRunningControllerAction,
        direction: MenuControllerCommand
    ) -> Bool {
        // List mode renders Resume and Stop as separate vertical rows.
        if libraryLayout != "grid" {
            if action == .resume, direction.verticalComponent == .down {
                focusNowRunning(.stop)
                return true
            }
            if action == .stop, direction.verticalComponent == .up {
                focusNowRunning(.resume)
                return true
            }
        }
        if usesLandscapeCoverFlow {
            switch (action, direction.horizontalComponent) {
            case (.resume, .right?):
                controllerNowRunningFocusState.selectedAction = .stop
                return true
            case (.stop, .left?):
                controllerNowRunningFocusState.selectedAction = .resume
                return true
            case (.stop, .right?):
                return focusFirstGame()
            default:
                return false
            }
        }

        switch direction {
        case .left where action == .stop:
            controllerNowRunningFocusState.selectedAction = .resume
            return true
        case .right where action == .resume:
            controllerNowRunningFocusState.selectedAction = .stop
            return true
        case .down, .downLeft, .downRight:
            return focusFirstGame()
        default:
            return false
        }
    }

    @discardableResult
    private func focusFirstGame() -> Bool {
        guard let first = games.first else { return false }
        controllerNowRunningFocusState.selectedAction = nil
        controllerNavigationActive = true
        controllerSelectedGameID = first.id
        return true
    }

    private func focusNowRunning(
        _ action: GameLibraryNowRunningControllerAction
    ) {
        guard appState.runningGameName != nil, !nowRunningRemovalAnimationActive else {
            _ = focusFirstGame()
            return
        }
        controllerNavigationActive = true
        controllerSelectedGameID = nil
        controllerNowRunningFocusState.selectedAction = action
        controllerInput?.rememberNavigationFocusKey(
            "now-running:\(action.rawValue)",
            forScope: gameLibraryControllerMemoryScope
        )
        if let runningGame = games.first(where: isRunning) {
            updateGameCoverThemePreview(
                for: runningGame.id,
                includesNonFavoriteGame: true,
                whilePresentationIsActive: true
            )
        }
        favoriteScrollSequence &+= 1
        favoriteScrollRequest = GameLibraryFavoriteScrollRequest(
            sequence: favoriteScrollSequence,
            destination: .nowRunning
        )
    }

    private func controllerDirectionForLayout(
        _ direction: MenuControllerCommand
    ) -> MenuControllerCommand {
        guard layoutDirection == .rightToLeft else { return direction }
        switch direction {
        case .left:
            return .right
        case .right:
            return .left
        case .upLeft:
            return .upRight
        case .upRight:
            return .upLeft
        case .downLeft:
            return .downRight
        case .downRight:
            return .downLeft
        default:
            return direction
        }
    }

    private func isControllerFocused(_ game: ISOEntry) -> Bool {
        controllerFocusState.isFocused(gameID: game.id)
    }

    private func controllerToolbarFocus(
        _ action: GameLibraryControllerToolbarAction,
        baseHorizontalPadding: CGFloat = 0,
        minimumWidth: CGFloat = 36
    ) -> ControllerToolbarFocusModifier {
        ControllerToolbarFocusModifier(
            focusState: controllerToolbarFocusState,
            action: action,
            controllerInput: controllerInput,
            menuTabIsActive: menuTabIsActive,
            baseHorizontalPadding: baseHorizontalPadding,
            minimumWidth: minimumWidth
        )
    }

    private func nowRunningControllerFocus(
        _ action: GameLibraryNowRunningControllerAction
    ) -> GameLibraryNowRunningControllerFocusModifier {
        GameLibraryNowRunningControllerFocusModifier(
            state: controllerNowRunningFocusState,
            action: action,
            controllerInput: controllerInput,
            menuTabIsActive: menuTabIsActive
        )
    }

    private func controllerRightStickReachedTop() {
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library,
              let firstGame = games.first else { return }
        controllerFocusReleased = false
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = firstGame.id
        updateControllerScrollAvailability()
        controllerInput?.playFeedback(.boundary)
    }

    /// A horizontal Cover Flow has no toolbar beyond its leading edge. Keep
    /// analog scrolling anchored to the first game instead of interpreting
    /// the left boundary as the vertical list's "move to toolbar" boundary.
    private func controllerRightStickReachedFirstCoverFlowGame() {
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library,
              let firstGame = games.first else { return }
        controllerFocusReleased = false
        controllerNavigationActive = true
        controllerNowRunningFocusState.selectedAction = nil
        controllerSelectedGameID = firstGame.id
        updateControllerScrollAvailability()
    }

    private func handleControllerToolbarCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = GameLibraryControllerToolbarAction.allCases
        guard let index = actions.firstIndex(
            of: controllerToolbarFocusState.selectedAction
        ) else {
            controllerToolbarFocusState.selectedAction = .importGames
            return
        }

        switch command {
        case .left:
            let next = max(actions.startIndex, index - 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerToolbarFocusState.selectedAction = actions[next]
            controllerInput?.playFeedback(.move(.left))
        case .right:
            let next = min(actions.index(before: actions.endIndex), index + 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerToolbarFocusState.selectedAction = actions[next]
            controllerInput?.playFeedback(.move(.right))
        case .down, .downLeft, .downRight:
            controllerInput?.setNavigationZone(.library)
            if !games.isEmpty || displayedRunningGameName != nil {
                // In landscape the visual entry point is the centered cover,
                // not the first model item (which is normally offscreen).
                _ = selectInitialControllerGame(preferLast: usesLandscapeCoverFlow)
            } else {
                controllerNavigationActive = true
                controllerSelectedGameID = nil
            }
            controllerInput?.playFeedback(.move(.down))
        case .up, .upLeft, .upRight:
            // Holding Up while entering the toolbar must terminate here. If it
            // re-enters the last card, the controller repeat loops through the
            // entire library instead of leaving the top controls selected.
            controllerInput?.playFeedback(.boundary)
        case .activate:
            let action = controllerToolbarFocusState.selectedAction
            performControllerToolbarAction(action)
            controllerInput?.playFeedback(
                action == .layout
                    ? .contextMenu
                    : .activate
            )
        case .back:
            controllerInput?.setNavigationZone(.library)
            controllerInput?.playFeedback(.back)
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            break
        }
    }

    private func handleEmptyLibraryControllerCommand(
        _ command: MenuControllerCommand
    ) {
        switch command {
        case .activate:
            openGameImporter()
            controllerInput?.playFeedback(.destination)
        case .up, .upLeft, .upRight, .back:
            controllerInput?.setNavigationZone(.topToolbar)
            controllerInput?.playFeedback(command == .back ? .back : .move(.up))
        case .down, .downLeft, .downRight:
            controllerInput?.setNavigationZone(.tabBar)
            controllerInput?.playFeedback(.move(.down))
        case .left, .right, .toggleFavorite, .showContextMenu:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func performControllerToolbarAction(
        _ action: GameLibraryControllerToolbarAction
    ) {
        switch action {
        case .bootBIOS:
            performBootBIOSAction()
        case .importGames:
            openGameImporter()
        case .layout:
            presentControllerLayoutMenu()
        case .more:
            presentControllerMoreMenu()
        }
    }

    private func presentGameLibraryViewOptions() {
        gameLibraryViewOptionsPreview = nil
        gameLibraryViewOptionsBaseline = currentGameLibraryViewOptionsValues
        gameLibraryViewOptionsPresented = true
        MenuAudioPackManager.shared.playEvent(.contextMenu)
    }

    private func cancelGameLibraryViewOptions() {
        closeGameLibraryViewOptions(restoringBaseline: true)
    }

    private func applyGameLibraryViewOptions() {
        closeGameLibraryViewOptions(restoringBaseline: false)
    }

    private func closeGameLibraryViewOptions(restoringBaseline: Bool) {
        guard gameLibraryViewOptionsPresented else { return }
        if restoringBaseline, let gameLibraryViewOptionsBaseline {
            applyGameLibraryViewOptionsValues(gameLibraryViewOptionsBaseline)
        }
        gameLibraryViewOptionsPreview = nil
        gameLibraryViewOptionsBaseline = nil
        gameLibraryViewOptionsPresented = false
        MenuAudioPackManager.shared.playEvent(.return)
    }

    private var currentGameLibraryViewOptionsValues:
        GameLibraryViewOptionsValues {
        GameLibraryViewOptionsValues(
            cardScale: activeGameCardScaleBinding.wrappedValue,
            cardWidthScale: activeGameCardWidthScaleBinding.wrappedValue,
            cardHeightScale: activeGameCardHeightScaleBinding.wrappedValue,
            libraryVerticalPosition:
                activeGameLibraryVerticalPositionBinding.wrappedValue,
            gapScale: activeGameCardGapScaleBinding.wrappedValue,
            contentPaddingScale:
                activeGameCardContentPaddingScaleBinding.wrappedValue,
            verticalPaddingScale:
                activeGameCardVerticalPaddingScaleBinding.wrappedValue,
            textSpacingScale:
                activeGameCardTextSpacingScaleBinding.wrappedValue,
            gameNameTextScale: activeGameNameTextScaleBinding.wrappedValue,
            gameInfoTextScale: activeGameInfoTextScaleBinding.wrappedValue,
            cardCornerRadius:
                activeGameCardCornerRadiusBinding.wrappedValue,
            coverCornerRadius:
                activeGameCoverCornerRadiusBinding.wrappedValue,
            coverShadowStrength:
                activeGameCoverShadowStrengthBinding.wrappedValue,
            coverOpacity: activeGameCoverOpacityBinding.wrappedValue,
            bottomNavigationTabBarHeight:
                activeBottomNavigationTabBarHeightBinding.wrappedValue,
            bottomNavigationTabBarIconScale:
                activeBottomNavigationTabBarIconScaleBinding.wrappedValue,
            bottomNavigationTabBarLabelScale:
                activeBottomNavigationTabBarLabelScaleBinding.wrappedValue,
            bottomNavigationTabBarClearance:
                activeBottomNavigationTabBarClearanceBinding.wrappedValue,
            bottomNavigationTabBarShowsLabels:
                activeBottomNavigationTabBarShowsLabelsBinding.wrappedValue,
            hideGamesScreenTitle:
                activeHideGamesScreenTitleBinding.wrappedValue,
            useGamesLogo: activeUseGamesLogoBinding.wrappedValue,
            hideGameName: activeHideGameNameBinding.wrappedValue,
            hideGameInfo: activeHideGameInfoBinding.wrappedValue,
            hideFavoriteButton:
                activeHideFavoriteButtonBinding.wrappedValue,
            hideRegionFlag: activeHideRegionFlagBinding.wrappedValue,
            singleLineGameNames:
                activeSingleLineGameNamesBinding.wrappedValue,
            hideRunningIndicator:
                activeHideRunningIndicatorBinding.wrappedValue
        )
    }

    private func applyGameLibraryViewOptionsValues(
        _ values: GameLibraryViewOptionsValues
    ) {
        activeGameCardScaleBinding.wrappedValue = values.cardScale
        activeGameCardWidthScaleBinding.wrappedValue = values.cardWidthScale
        activeGameCardHeightScaleBinding.wrappedValue = values.cardHeightScale
        activeGameLibraryVerticalPositionBinding.wrappedValue =
            values.libraryVerticalPosition
        activeGameCardGapScaleBinding.wrappedValue = values.gapScale
        activeGameCardContentPaddingScaleBinding.wrappedValue =
            values.contentPaddingScale
        activeGameCardVerticalPaddingScaleBinding.wrappedValue =
            values.verticalPaddingScale
        activeGameCardTextSpacingScaleBinding.wrappedValue =
            values.textSpacingScale
        activeGameNameTextScaleBinding.wrappedValue = values.gameNameTextScale
        activeGameInfoTextScaleBinding.wrappedValue = values.gameInfoTextScale
        activeGameCardCornerRadiusBinding.wrappedValue =
            values.cardCornerRadius
        activeGameCoverCornerRadiusBinding.wrappedValue =
            values.coverCornerRadius
        activeGameCoverShadowStrengthBinding.wrappedValue =
            values.coverShadowStrength
        activeGameCoverOpacityBinding.wrappedValue = values.coverOpacity
        activeBottomNavigationTabBarHeightBinding.wrappedValue =
            values.bottomNavigationTabBarHeight
        activeBottomNavigationTabBarIconScaleBinding.wrappedValue =
            values.bottomNavigationTabBarIconScale
        activeBottomNavigationTabBarLabelScaleBinding.wrappedValue =
            values.bottomNavigationTabBarLabelScale
        activeBottomNavigationTabBarClearanceBinding.wrappedValue =
            values.bottomNavigationTabBarClearance
        activeBottomNavigationTabBarShowsLabelsBinding.wrappedValue =
            values.bottomNavigationTabBarShowsLabels
        activeHideGamesScreenTitleBinding.wrappedValue =
            values.hideGamesScreenTitle
        activeUseGamesLogoBinding.wrappedValue = values.useGamesLogo
        activeHideGameNameBinding.wrappedValue = values.hideGameName
        activeHideGameInfoBinding.wrappedValue = values.hideGameInfo
        activeHideFavoriteButtonBinding.wrappedValue =
            values.hideFavoriteButton
        activeHideRegionFlagBinding.wrappedValue = values.hideRegionFlag
        activeSingleLineGameNamesBinding.wrappedValue =
            values.singleLineGameNames
        activeHideRunningIndicatorBinding.wrappedValue =
            values.hideRunningIndicator
    }

    private func resetActiveGameLibraryViewOptions() {
        activeGameCardScaleBinding.wrappedValue = 1
        activeGameCardWidthScaleBinding.wrappedValue = 1
        activeGameCardHeightScaleBinding.wrappedValue = 1
        activeGameLibraryVerticalPositionBinding.wrappedValue = 0
        activeGameCardGapScaleBinding.wrappedValue = 1
        activeGameCardContentPaddingScaleBinding.wrappedValue = 1
        activeGameCardVerticalPaddingScaleBinding.wrappedValue = 1
        activeGameCardTextSpacingScaleBinding.wrappedValue = 1
        activeGameNameTextScaleBinding.wrappedValue = 1
        activeGameInfoTextScaleBinding.wrappedValue = 1
        activeGameCardCornerRadiusBinding.wrappedValue = 18
        activeGameCoverCornerRadiusBinding.wrappedValue = 10
        activeGameCoverShadowStrengthBinding.wrappedValue = 1
        activeGameCoverOpacityBinding.wrappedValue = 1
        activeBottomNavigationTabBarHeightBinding.wrappedValue = 84
        activeBottomNavigationTabBarIconScaleBinding.wrappedValue = 1
        activeBottomNavigationTabBarLabelScaleBinding.wrappedValue = 1
        activeBottomNavigationTabBarClearanceBinding.wrappedValue = 0
        activeBottomNavigationTabBarShowsLabelsBinding.wrappedValue = true
        activeHideGamesScreenTitleBinding.wrappedValue = true
        activeUseGamesLogoBinding.wrappedValue = true
        activeHideGameNameBinding.wrappedValue = false
        activeHideGameInfoBinding.wrappedValue = false
        activeHideFavoriteButtonBinding.wrappedValue = false
        activeHideRegionFlagBinding.wrappedValue = false
        activeSingleLineGameNamesBinding.wrappedValue = false
        activeHideRunningIndicatorBinding.wrappedValue = false
    }

    private var controllerLayoutMenuActions: [ControllerNavigationAlertAction] {
        var actions = [
            ControllerNavigationAlertAction(
                id: "display-mode",
                title: settings.localized(
                    libraryLayout == "grid" ? "Show List" : "Show Grid"
                )
            ),
        ]
        if libraryLayout == "grid", usesLandscapeGameCardScale {
            actions.append(
                ControllerNavigationAlertAction(
                    id: "landscape-cover-flow",
                    title: settings.localized("Landscape Cover Flow")
                        + (landscapeCoverFlowEnabled ? "  ✓" : "")
                )
            )
        }
        actions.append(
            ControllerNavigationAlertAction(
                id: "view-options",
                title: settings.localized(
                    "Game Library View Options"
                )
            )
        )
        return actions
    }

    private func presentControllerLayoutMenu() {
        controllerLayoutMenuSelectedIndex = 0
        controllerLayoutMenuPresented = true
    }

    private var controllerMoreMenuActions: [ControllerNavigationAlertAction] {
        var actions = [
            ControllerNavigationAlertAction(
                id: "import-local-covers",
                title: settings.localized("Import Local Covers"),
                systemImage: "photo.badge.plus"
            ),
        ]
        if !coverStore.isDownloadingCovers && !games.isEmpty {
            actions.append(
                ControllerNavigationAlertAction(
                    id: "download-missing-covers",
                    title: settings.localized("Download Missing Covers"),
                    systemImage: "icloud.and.arrow.down"
                )
            )
        }
        actions.append(
            contentsOf: [
                ControllerNavigationAlertAction(
                    id: "cover-source",
                    title: settings.localized("Cover Source"),
                    systemImage: "link"
                ),
                ControllerNavigationAlertAction(
                    id: "reset-cover-template",
                    title: settings.localized("Reset Cover Template"),
                    systemImage: "arrow.counterclockwise"
                ),
                ControllerNavigationAlertAction(
                    id: "refresh-library",
                    title: settings.localized("Refresh"),
                    systemImage: "arrow.clockwise"
                ),
            ]
        )
        return actions
    }

    private func presentControllerMoreMenu() {
        controllerMoreMenuSelectedIndex = 0
        controllerMoreMenuPresented = true
    }

    private func dismissControllerLayoutMenu(playsFeedback: Bool = true) {
        guard controllerLayoutMenuPresented else { return }
        controllerLayoutMenuPresented = false
        controllerLayoutMenuSelectedIndex = 0
        if playsFeedback {
            controllerInput?.playFeedback(.back)
        }
    }

    private func dismissControllerMoreMenu(playsFeedback: Bool = true) {
        guard controllerMoreMenuPresented else { return }
        controllerMoreMenuPresented = false
        controllerMoreMenuSelectedIndex = 0
        if playsFeedback {
            controllerInput?.playFeedback(.back)
        }
    }

    private func handleControllerMoreMenuCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = controllerMoreMenuActions
        switch command {
        case .up, .upLeft, .upRight:
            let next = max(0, controllerMoreMenuSelectedIndex - 1)
            guard next != controllerMoreMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerMoreMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.up))
        case .down, .downLeft, .downRight:
            let next = min(
                actions.count - 1,
                controllerMoreMenuSelectedIndex + 1
            )
            guard next != controllerMoreMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerMoreMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.down))
        case .activate:
            performControllerMoreMenuAction(controllerMoreMenuSelectedIndex)
        case .back, .showContextMenu:
            dismissControllerMoreMenu()
        case .left, .right, .toggleFavorite:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func performControllerMoreMenuAction(_ index: Int) {
        let actions = controllerMoreMenuActions
        guard actions.indices.contains(index) else { return }
        dismissControllerMoreMenu(playsFeedback: false)
        switch actions[index].id {
        case "import-local-covers":
            presentMenuPanel("cover_import_all") {
                pendingCoverGameName = nil
                showCoverImporter = true
            }
            controllerInput?.playFeedback(.contextMenu)
        case "download-missing-covers":
            downloadMissingCovers()
            controllerInput?.playFeedback(.activate)
        case "cover-source":
            presentMenuPanel("cover_source") {
                coverTemplateDraft = coverStore.coverURLTemplate
                showCoverTemplateEditor = true
            }
            controllerInput?.playFeedback(.contextMenu)
        case "reset-cover-template":
            presentMenuPanel("cover_template_reset") {
                coverStore.coverURLTemplate = CoverStore.defaultCoverURLTemplate
                coverStore.lastCoverMessage =
                    "Cover URL template reset to the ARMSX2 Android default."
                coverStore.showCoverAlert = true
            }
            controllerInput?.playFeedback(.activate)
        case "refresh-library":
            loadGames(forceMetadataRefresh: true)
            controllerInput?.playFeedback(.activate)
        default:
            break
        }
    }

    private func handleControllerLayoutMenuCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = controllerLayoutMenuActions
        switch command {
        case .up, .upLeft, .upRight:
            let next = max(0, controllerLayoutMenuSelectedIndex - 1)
            guard next != controllerLayoutMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerLayoutMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.up))
        case .down, .downLeft, .downRight:
            let next = min(
                actions.count - 1,
                controllerLayoutMenuSelectedIndex + 1
            )
            guard next != controllerLayoutMenuSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerLayoutMenuSelectedIndex = next
            controllerInput?.playFeedback(.move(.down))
        case .activate:
            performControllerLayoutMenuAction(
                controllerLayoutMenuSelectedIndex
            )
        case .back, .showContextMenu:
            dismissControllerLayoutMenu()
        case .left, .right, .toggleFavorite:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func performControllerLayoutMenuAction(_ index: Int) {
        let actions = controllerLayoutMenuActions
        guard actions.indices.contains(index) else {
            return
        }
        switch actions[index].id {
        case "display-mode":
            dismissControllerLayoutMenu(playsFeedback: false)
            selectAlternateLibraryLayout()
            controllerInput?.playFeedback(.activate)
        case "landscape-cover-flow":
            landscapeCoverFlowEnabled.toggle()
            controllerInput?.playFeedback(
                .toggle(isOn: landscapeCoverFlowEnabled)
            )
        case "view-options":
            dismissControllerLayoutMenu(playsFeedback: false)
            presentGameLibraryViewOptions()
        default:
            break
        }
    }

    private func selectAlternateLibraryLayout() {
        libraryLayout = libraryLayout == "grid" ? "list" : "grid"
    }

    private var controllerContextMenuGame: ISOEntry? {
        guard let gameID = controllerContextMenuState.gameID else { return nil }
        return games.first(where: { $0.id == gameID })
    }

    private func controllerContextMenuTitle(
        for section: GameLibraryControllerMenuSection
    ) -> String {
        switch section {
        case .root:
            return ""
        case .covers:
            return settings.localized("Covers")
        case .gameData:
            return settings.localized("Game Data")
        }
    }

    private func controllerContextMenuItems(
        for game: ISOEntry,
        section: GameLibraryControllerMenuSection? = nil
    ) -> [GameLibraryControllerMenuItem] {
        switch section ?? controllerContextMenuState.section {
        case .root:
            var items = [
                GameLibraryControllerMenuItem(
                    action: .gameInfo,
                    title: settings.localized("Game Info"),
                    systemImage: "info.circle"
                ),
                GameLibraryControllerMenuItem(
                    action: .perGameSettings,
                    title: settings.localized("Per-Game Settings"),
                    systemImage: "slider.horizontal.3"
                ),
                GameLibraryControllerMenuItem(
                    action: .perGameCustomSkin,
                    title: settings.localized("Per-Game Custom Skin"),
                    systemImage: "gamecontroller.fill"
                ),
            ]
            if game.isELF {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .discPath,
                        title: settings.localized("Disc Path"),
                        systemImage: "opticaldisc"
                    )
                )
            }
            items.append(
                contentsOf: [
                    GameLibraryControllerMenuItem(
                        action: .cheatsAndPatches,
                        title: settings.localized("Cheats & Patches"),
                        systemImage: "rectangle.stack.badge.plus"
                    ),
                    GameLibraryControllerMenuItem(
                        action: .covers,
                        title: settings.localized("Covers"),
                        systemImage: "photo.stack",
                        showsDisclosure: true
                    ),
                    GameLibraryControllerMenuItem(
                        action: .gameData,
                        title: settings.localized("Game Data"),
                        systemImage: "externaldrive",
                        showsDisclosure: true
                    ),
                    GameLibraryControllerMenuItem(
                        action: .copyLaunchLink,
                        title: settings.localized("Copy Launch Link"),
                        systemImage: "doc.on.doc"
                    ),
                ]
            )
            items.append(
                GameLibraryControllerMenuItem(
                    action: .rename,
                    title: settings.localized("Rename"),
                    systemImage: "pencil"
                )
            )
            return items

        case .covers:
            var items: [GameLibraryControllerMenuItem] = []
            if !coverStore.isDownloadingCovers {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .downloadCover,
                        title: settings.localized("Download Cover"),
                        systemImage: "icloud.and.arrow.down"
                    )
                )
            }
            items.append(
                contentsOf: [
                    GameLibraryControllerMenuItem(
                        action: .chooseCoverPhoto,
                        title: settings.localized("Choose from Photos"),
                        systemImage: "photo.on.rectangle"
                    ),
                    GameLibraryControllerMenuItem(
                        action: .chooseCoverFile,
                        title: settings.localized("Choose from Files"),
                        systemImage: "folder"
                    ),
                ]
            )
            if game.coverURL != nil {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .removeCover,
                        title: settings.localized("Remove Cover"),
                        systemImage: "trash",
                        isDestructive: true
                    )
                )
            }
            return items

        case .gameData:
            var items = [
                GameLibraryControllerMenuItem(
                    action: .clearGameCache,
                    title: settings.localized("Clear Game Cache"),
                    systemImage: "trash.slash"
                ),
                GameLibraryControllerMenuItem(
                    action: .deleteGameData,
                    title: settings.localized("Delete Game Data"),
                    systemImage: "externaldrive.badge.xmark",
                    isDestructive: true
                ),
            ]
            if !game.isExternal {
                items.append(
                    GameLibraryControllerMenuItem(
                        action: .deleteGame,
                        title: settings.localized("Delete Game"),
                        systemImage: "trash",
                        isDestructive: true
                    )
                )
            }
            return items
        }
    }

    private func handleGameContextMenuLongPress(for game: ISOEntry) {
        // Claim the stationary hold before RootView's empty-space recognizer
        // can interpret it as an L2/R2 theme shortcut.
        sharedMenuControllerInput?.suppressTouchThemePresetShortcut()
        openControllerContextMenu(for: game)
    }

    private func suppressThemeShortcutForGameCardTouch() {
        sharedMenuControllerInput?.suppressTouchThemePresetShortcut()
    }

    private func openControllerContextMenu(for game: ISOEntry) {
        let openedByController = controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = openedByController
            ? .gameInfo : nil
        // The foreground row must be the only published orb destination.
        // Otherwise the retained focused card and the context overlay compete
        // across separate preference branches and the orbit can stay behind.
        controllerNavigationActive = false
        controllerInput?.setNavigationCaptured(
            true,
            owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
            priority: 300
        )
        updateControllerScrollAvailability()
        withAnimation(.smooth(duration: 0.24, extraBounce: 0.03)) {
            controllerContextMenuState.gameID = game.id
        }
        if let controllerInput {
            controllerInput.playFeedback(.contextMenu)
        } else {
            MenuAudioPackManager.shared.playEvent(.contextMenu)
        }
    }

    @MainActor
    private func prepareGameSettings(for game: ISOEntry) {
        guard preparedGameSettingsID != game.id
                || preparedGameSettings == nil else { return }

        if let crc = game.metadata["crc"], !crc.isEmpty {
            preparedGameSettings = ARMSX2Bridge.gameSettings(
                forSerial: game.metadata["serial"],
                crc: crc
            )
        } else {
            // Some manually added ELF entries do not have a cached CRC. Keep
            // the compatibility fallback for those uncommon entries only.
            preparedGameSettings = ARMSX2Bridge.gameSettings(
                forISO: game.bootName
            )
        }
        preparedGameSettingsID = game.id
    }

    private func releasePreparedGameSettings() {
        preparedGameSettingsID = nil
        preparedGameSettings = nil
    }

    private func beginRenaming(_ game: ISOEntry) {
        controllerNavigationActive = false
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = nil
        renameTarget = game

        // Replace the context surface instead of stacking a keyboard above it.
        // Keeping the capture continuous also prevents the selection press
        // from falling through and opening the focused game card.
        withAnimation(.easeOut(duration: 0.16)) {
            controllerContextMenuState.gameID = nil
        }
        controllerInput?.setNavigationCaptured(
            true,
            owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
            priority: 300
        )
        updateControllerScrollAvailability()
    }

    private func renameGame(_ game: ISOEntry, to proposedName: String) {
        let fallbackName = URL(fileURLWithPath: game.name)
            .deletingPathExtension()
            .lastPathComponent
        GameDisplayNameStore.shared.setDisplayName(
            proposedName,
            for: game.id,
            fallback: fallbackName
        )
        loadGames()
    }

    private func closeControllerContextMenu(
        playsFeedback: Bool = true
    ) {
        guard controllerContextMenuState.gameID != nil else { return }
        withAnimation(.easeOut(duration: 0.18)) {
            controllerContextMenuState.gameID = nil
        }
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = nil
        Task { @MainActor in
            // Let a destination selected from the menu publish first. This
            // avoids briefly reactivating and scrolling the library between
            // the context menu and its sheet.
            await Task.yield()
            guard controllerContextMenuState.gameID == nil else { return }
            let shouldCapture = manuallyCapturedControllerPresentationActive
            controllerInput?.setNavigationCaptured(
                shouldCapture,
                owner: MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
                priority: 300
            )
            updateControllerScrollAvailability()
            if !shouldCapture {
                controllerNavigationActive = controllerSelectedGameID != nil
                if controllerNavigationActive {
                    controllerFocusState.requestSelectionScroll()
                } else {
                    scheduleLandscapeControllerAutofocus()
                }
            }
        }
        if playsFeedback {
            if let controllerInput {
                controllerInput.playFeedback(.back)
            } else {
                MenuAudioPackManager.shared.playEvent(.return)
            }
        }
    }

    private func handleControllerContextMenuCommand(
        _ command: MenuControllerCommand,
        for game: ISOEntry
    ) {
        let direction = controllerDirectionForLayout(command)
        switch direction {
        case .up, .upLeft, .upRight, .down, .downLeft, .downRight:
            moveControllerContextMenuSelection(
                direction.verticalComponent ?? direction,
                for: game
            )
        case .left, .back:
            navigateBackFromControllerContextMenu()
        case .right, .activate:
            guard let action = controllerContextMenuState.selectedAction else {
                selectFirstControllerContextMenuItem(for: game)
                return
            }
            performControllerContextMenuAction(action, for: game)
        case .toggleFavorite:
            let isFavorite = toggleFavorite(
                game,
                playsSelectionFeedback: false
            )
            controllerInput?.playFeedback(
                .favorite(isFavorite: isFavorite)
            )
        case .showContextMenu:
            closeControllerContextMenu()
        case .previousTab, .nextTab:
            break
        }
    }

    private func moveControllerContextMenuSelection(
        _ direction: MenuControllerCommand,
        for game: ISOEntry
    ) {
        let actions = controllerContextMenuNavigationActions(for: game)
        guard !actions.isEmpty else {
            controllerInput?.playFeedback(.boundary)
            return
        }

        let currentIndex = controllerContextMenuState.selectedAction.flatMap { action in
            actions.firstIndex(of: action)
        }
        let proposedIndex: Int
        if let currentIndex {
            proposedIndex = direction == .up
                ? max(0, currentIndex - 1)
                : min(actions.count - 1, currentIndex + 1)
            guard proposedIndex != currentIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
        } else {
            proposedIndex = direction == .up ? actions.count - 1 : 0
        }

        controllerContextMenuState.selectedAction = actions[proposedIndex]
        controllerInput?.playFeedback(.move(direction))
    }

    private func controllerContextMenuNavigationActions(
        for game: ISOEntry
    ) -> [GameLibraryControllerMenuAction] {
        let rowActions = controllerContextMenuItems(for: game).map(\.action)
        guard controllerContextMenuState.section == .root else {
            return rowActions
        }
        return [.playOrStop] + rowActions
    }

    private func navigateBackFromControllerContextMenu() {
        guard controllerContextMenuState.section != .root else {
            closeControllerContextMenu()
            return
        }

        let parentAction: GameLibraryControllerMenuAction =
            controllerContextMenuState.section == .covers ? .covers : .gameData
        controllerContextMenuState.section = .root
        controllerContextMenuState.selectedAction = parentAction
        controllerInput?.playFeedback(.back)
    }

    private func selectFirstControllerContextMenuItem(
        for game: ISOEntry,
        playsFeedback: Bool = true
    ) {
        controllerContextMenuState.selectedAction = controllerContextMenuItems(
            for: game
        ).first?.action
        if playsFeedback {
            controllerInput?.playFeedback(.move(.down))
        }
    }

    private func performControllerContextMenuAction(
        _ action: GameLibraryControllerMenuAction,
        for game: ISOEntry
    ) {
        switch action {
        case .playOrStop:
            deferControllerContextMenuSelection(action, for: game)

        case .covers:
            controllerContextMenuState.section = .covers
            selectInitialControllerContextMenuItemIfNeeded(for: game)
            controllerInput?.playFeedback(.submenu)

        case .gameData:
            controllerContextMenuState.section = .gameData
            selectInitialControllerContextMenuItemIfNeeded(for: game)
            controllerInput?.playFeedback(.submenu)

        case .gameInfo:
            deferControllerContextMenuSelection(action, for: game)

        case .perGameSettings:
            deferControllerContextMenuSelection(action, for: game)

        case .perGameShaders:
            deferControllerContextMenuSelection(action, for: game)

        case .perGameCustomSkin:
            deferControllerContextMenuSelection(action, for: game)

        case .discPath:
            deferControllerContextMenuSelection(action, for: game)

        case .cheatsAndPatches:
            deferControllerContextMenuSelection(action, for: game)

        case .copyLaunchLink:
            deferControllerContextMenuSelection(action, for: game)

        case .downloadCover:
            deferControllerContextMenuSelection(action, for: game)

        case .chooseCoverPhoto:
            deferControllerContextMenuSelection(action, for: game)

        case .chooseCoverFile:
            deferControllerContextMenuSelection(action, for: game)

        case .removeCover:
            deferControllerContextMenuSelection(action, for: game)

        case .clearGameCache:
            deferControllerContextMenuSelection(action, for: game)

        case .deleteGameData:
            deferControllerContextMenuSelection(action, for: game)

        case .deleteGame:
            deferControllerContextMenuSelection(action, for: game)

        case .rename:
            deferControllerContextMenuSelection(action, for: game)
        }
    }

    private func selectInitialControllerContextMenuItemIfNeeded(
        for game: ISOEntry
    ) {
        guard controllerInput?.hasConnectedController == true,
              controllerInput?.isControllerNavigationEnabled == true else {
            controllerContextMenuState.selectedAction = nil
            return
        }
        selectFirstControllerContextMenuItem(for: game, playsFeedback: false)
    }

    /// The scene-level context menu has to finish dismissing before SwiftUI
    /// can present the chosen destination. Resolve it from `onDismiss` instead
    /// of relying on a device-dependent delay.
    private func deferControllerContextMenuSelection(
        _ action: GameLibraryControllerMenuAction,
        for game: ISOEntry
    ) {
        deferredControllerContextMenuSelection = .init(
            action: action,
            gameID: game.id
        )
        closeControllerContextMenu(playsFeedback: false)
    }

    private func completeDeferredControllerContextMenuSelection() {
        guard let selection = deferredControllerContextMenuSelection,
              let game = games.first(where: { $0.id == selection.gameID })
        else {
            deferredControllerContextMenuSelection = nil
            return
        }

        switch selection.action {
        case .playOrStop:
            if isRunning(game) {
                showStopAlert = true
            } else {
                open(
                    game,
                    allowsAutomaticSkinPrompt:
                        controllerInput?.isControllerNavigationEnabled != true
                )
            }
        case .gameInfo:
            gameInfoTarget = game
            controllerInput?.playFeedback(.destination)
        case .perGameSettings:
            prepareGameSettings(for: game)
            gameSettingsStartsInShaders = false
            gameSettingsTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .perGameShaders:
            prepareGameSettings(for: game)
            gameSettingsStartsInShaders = true
            gameSettingsTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .perGameCustomSkin:
            presentPerGameCustomSkin(for: game)
            controllerInput?.playFeedback(.destination)
        case .discPath:
            discLinkTarget = game
            controllerInput?.playFeedback(.destination)
        case .cheatsAndPatches:
            cheatsManagerTarget = game
            MenuAudioPackManager.shared.playEvent(.uiToast)
            controllerInput?.playFeedback(.destination)
        case .copyLaunchLink:
            copyLaunchLink(game)
            controllerInput?.playFeedback(.activate)
        case .downloadCover:
            downloadCover(for: game)
            controllerInput?.playFeedback(.activate)
        case .chooseCoverPhoto:
            pendingCoverPhotoGameName = game.name
            showCoverPhotoPicker = true
            controllerInput?.playFeedback(.destination)
        case .chooseCoverFile:
            pendingCoverGameName = game.name
            showCoverImporter = true
            controllerInput?.playFeedback(.destination)
        case .removeCover:
            coverStore.removeManagedCovers(forGameNamed: game.name)
            loadGames()
            controllerInput?.playFeedback(.activate)
        case .clearGameCache:
            clearGameCache(game)
            controllerInput?.playFeedback(.destination)
        case .deleteGameData:
            pendingDeleteDataGame = game
            controllerInput?.playFeedback(.destination)
        case .deleteGame:
            pendingDeleteGame = game
            controllerInput?.playFeedback(.destination)
        case .rename:
            beginRenaming(game)
            controllerInput?.playFeedback(.destination)
        case .covers, .gameData:
            break
        }

        deferredControllerContextMenuSelection = nil
    }

    @discardableResult
    private func dismissControllerPresentation() -> Bool {
        if gameInfoTarget != nil {
            gameInfoTarget = nil
        } else if gameSettingsTarget != nil {
            gameSettingsTarget = nil
        } else if discLinkTarget != nil {
            discLinkTarget = nil
        } else if cheatsManagerTarget != nil {
            cheatsManagerTarget = nil
        } else if showRestartAlert {
            showRestartAlert = false
            pendingGameplayLaunchTransition = nil
            pendingGameplayPadSerial = nil
            pendingGameplayPadCRC = nil
        } else if showStopAlert {
            showStopAlert = false
        } else if pendingDeleteGame != nil {
            pendingDeleteGame = nil
        } else if pendingDeleteDataGame != nil {
            pendingDeleteDataGame = nil
        } else if gameActionMessage != nil {
            gameActionMessage = nil
        } else if coverStore.showCoverAlert {
            coverStore.showCoverAlert = false
        } else if showCoverTemplateEditor {
            showCoverTemplateEditor = false
        } else if showGameReplacementAlert {
            showGameReplacementAlert = false
            clearPendingGameImport()
        } else if showBackgroundAssetError {
            showBackgroundAssetError = false
        } else if showGameImporter {
            showGameImporter = false
        } else if showCoverImporter {
            showCoverImporter = false
            pendingCoverGameName = nil
        } else if showCoverPhotoPicker {
            showCoverPhotoPicker = false
            pendingCoverPhotoGameName = nil
        } else {
            return false
        }
        return true
    }

    /// Clean display title for the running game, preferring the library entry over the raw path.
    private func displayTitle(forRunningName name: String) -> String {
        if name == "BIOS" {
            return settings.localized("Boot BIOS")
        }
        if let entry = games.first(where: { $0.bootName == name || $0.name == name }) {
            let title = entry.metadata["title"]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return title.isEmpty ? entry.name : title
        }
        return Self.cleanGameFileName(name)
    }

    private func runningLibraryEntry(for name: String) -> ISOEntry? {
        games.first(where: isRunning)
            ?? games.first {
                $0.bootName == name || $0.name == name
            }
    }

    private func nowRunningCoverBackground(
        gameName: String,
        cornerRadius: CGFloat
    ) -> some View {
        GeometryReader { geometry in
            if let game = runningLibraryEntry(for: gameName),
               game.coverURL != nil,
               geometry.size.width > 1,
               geometry.size.height > 1 {
                CoverThumbnailView(
                    gameName: game.name,
                    coverURL: game.coverURL,
                    coverSignature: game.coverSignature,
                    width: geometry.size.width,
                    height: geometry.size.height,
                    cornerRadius: cornerRadius
                )
                .scaleEffect(1.08)
                .blur(radius: 14)
                .overlay(Color.black.opacity(0.34))
                .clipShape(
                    RoundedRectangle(
                        cornerRadius: cornerRadius,
                        style: .continuous
                    )
                )
                .allowsHitTesting(false)
                .accessibilityHidden(true)
            }
        }
    }

    /// Last path component with its disc extension (`.iso`, `.bin`, `.cue`, …) removed,
    /// used when no matching library entry is available.
    private static func cleanGameFileName(_ value: String) -> String {
        let fileName = (value as NSString).lastPathComponent
        guard !fileName.isEmpty else { return value }
        return (fileName as NSString).deletingPathExtension
    }

    private func vmStatusSection(
        gameName: String,
        opacity: Double,
        scale: Double
    ) -> some View {
        let isStopping = nowRunningRemovalAnimationActive

        return Section {
            Button {
                appState.returnToGame()
            } label: {
                HStack {
                    HStack(spacing: 8) {
                        Image(
                            systemName: isStopping
                                ? "stop.circle.fill"
                                : "play.circle.fill"
                        )
                            .foregroundStyle(
                                isStopping ? criticalTextColour : accentColour
                            )
                            .font(.title)
                            .contentTransition(.symbolEffect(.replace))

                        RunningStatusText(
                            isStopping: isStopping,
                            runningLabel: settings.localized("Now Running"),
                            gameTitle: displayTitle(forRunningName: gameName),
                            stoppingLabel: settings.localized("Stopping..."),
                            stoppingFont: .title2,
                            statusColor: accentColour,
                            titleColor: contentTextColour
                        )
                    }
                    .opacity(displayedNowRunningStatusOpacity)
                    .animation(.easeInOut(duration: 0.3), value: displayedNowRunningStatusOpacity)

                    if !isStopping {
                        Spacer()
                        HStack(spacing: 6) {
                            Text(settings.localized("Resume"))
                                .font(.subheadline)
                                .fontWeight(.medium)
                            Image(systemName: "chevron.right")
                                .font(.caption)
                                .foregroundStyle(.tertiary)
                        }
                        .transition(.opacity.combined(with: .scale(scale: 0.9)))
                    }
                }
                .frame(
                    maxWidth: .infinity,
                    alignment: isStopping ? .center : .leading
                )
                .padding(.vertical, 6)
                .background {
                    nowRunningCoverBackground(
                        gameName: gameName,
                        cornerRadius: 14
                    )
                }
                .clipShape(
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                )
                .animation(
                    .smooth(duration: 0.42, extraBounce: 0.02),
                    value: isStopping
                )
            }
            .disabled(isStopping)
            .tint(accentColour)
            .menuBackgroundListRow(hasCustomBackground)
            .compositingGroup()
            .modifier(nowRunningControllerFocus(.resume))
            .scaleEffect(scale)
            .opacity(opacity)
            .animation(.linear(duration: 0.65), value: opacity)
            .animation(
                .smooth(duration: 0.65, extraBounce: 0.04),
                value: scale
            )

            if !isStopping {
                Button(role: .destructive) {
                    showStopAlert = true
                } label: {
                    HStack {
                        Spacer()
                        Label(settings.localized("Stop Emulation"), systemImage: "stop.circle")
                            .font(.subheadline)
                        Spacer()
                    }
                }
                .menuBackgroundListRow(hasCustomBackground)
                .compositingGroup()
                .scaleEffect(scale)
                .opacity(opacity)
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
                .animation(.linear(duration: 0.65), value: opacity)
                .animation(
                    .smooth(duration: 0.65, extraBounce: 0.04),
                    value: scale
                )
                .modifier(nowRunningControllerFocus(.stop))
            }
        }
    }

    @ViewBuilder
    private func gameRow(
        _ game: ISOEntry,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        if controllerInput == nil {
            originalGameRow(game)
        } else {
            controllerGameRow(
                game,
                controllerFocused: controllerFocused,
                visualEffectsEnabled: visualEffectsEnabled
            )
        }
    }

    /// Touch-first row retained from Master. Controller-only glass and padding
    /// never enter this subtree when no gamepad is connected.
    private func originalGameRow(_ game: ISOEntry) -> some View {
        let running = isRunning(game)
        return Button {
            open(game)
        } label: {
            HStack(spacing: 12 * resolvedGameCardTextSpacingScale) {
                coverThumbnail(
                    for: game,
                    width: 58 * resolvedGameCardScale
                        * resolvedGameCardWidthScale,
                    height: 87 * resolvedGameCardScale
                        * resolvedGameCardHeightScale,
                    running: running
                )

                VStack(alignment: .leading, spacing: 4) {
                    if !hideGameName {
                        HStack(spacing: 6) {
                            Text(game.displayName)
                                .font(
                                    .system(
                                        size: 17 * resolvedGameNameTextScale,
                                        weight: .medium
                                    )
                                )
                                .foregroundStyle(contentTextColour)
                                .lineLimit(gameNameLineLimit)
                                .multilineTextAlignment(.leading)
                                .fixedSize(horizontal: false, vertical: true)
                                .layoutPriority(1)
                            if running && !hideRunningIndicator {
                                Image(systemName: "circle.fill")
                                    .font(.system(size: 8))
                                    .foregroundStyle(accentColour)
                                    .accessibilityLabel(
                                        settings.localized("Running")
                                    )
                                    .fixedSize()
                            }
                        }
                    }
                    if !hideGameInfo {
                        HStack(spacing: 8) {
                            Text(game.sizeLabel)
                            Text(game.name.pathExtensionLabel)
                            if game.isExternal {
                                Label(
                                    settings.localized("External"),
                                    systemImage: "externaldrive"
                                )
                            }
                            if !hideRegionFlag, let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(
                                    settings.localized("Region")
                                )
                            }
                        }
                        .font(
                            .system(size: 12 * resolvedGameInfoTextScale)
                        )
                        .foregroundStyle(secondaryTextColour)
                        .lineLimit(1)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .layoutPriority(1)
                Spacer()

                if !hideFavoriteButton {
                    favoriteButton(
                        for: game,
                        foreground: game.isFavorite ? .yellow : .gray
                    )
                }

                Image(
                    systemName: running && !hideRunningIndicator
                        ? "play.fill"
                        : "chevron.right"
                )
                    .foregroundStyle(
                        running && !hideRunningIndicator
                            ? accentColour
                            : .secondary
                    )
                    .font(.caption)
                    .accessibilityHidden(true)
            }
        }
        .buttonStyle(.plain)
        .foregroundStyle(.primary)
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry
        )
    }

    private func controllerGameRow(
        _ game: ISOEntry,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        let running = isRunning(game)
        return Button {
            open(game)
        } label: {
            HStack(spacing: 12 * resolvedGameCardTextSpacingScale) {
                coverThumbnail(
                    for: game,
                    width: 58 * resolvedGameCardScale
                        * resolvedGameCardWidthScale,
                    height: 87 * resolvedGameCardScale
                        * resolvedGameCardHeightScale,
                    running: running,
                    visualEffectsEnabled: visualEffectsEnabled
                )
                .controllerNavigationOrbTarget(
                    id: "library.game.\(game.id)",
                    isActive: controllerFocused
                        && !controllerNavigationPresentationActive,
                    isEnabled: controllerInput != nil,
                    palette: .blue,
                    inset: -3,
                    orbScale: 0.92,
                    priority: 30
                )

                VStack(alignment: .leading, spacing: 4) {
                    if !hideGameName {
                        HStack(spacing: 6) {
                            Text(game.displayName)
                                .font(
                                    .system(
                                        size: 17 * resolvedGameNameTextScale,
                                        weight: .medium
                                    )
                                )
                                .foregroundStyle(
                                    controllerFocused
                                        ? settings.controllerCardFocusedColor
                                        : contentTextColour
                                )
                                .animation(
                                    ControllerFocusVisualAnimation.textFade,
                                    value: controllerFocused
                                )
                                .lineLimit(gameNameLineLimit)
                                .multilineTextAlignment(.leading)
                                .fixedSize(horizontal: false, vertical: true)
                                .layoutPriority(1)
                            if running && !hideRunningIndicator {
                                Image(systemName: "circle.fill")
                                    .font(.system(size: 8))
                                    .foregroundStyle(accentColour)
                                    .accessibilityLabel(settings.localized("Running"))
                                .fixedSize()
                            }
                        }
                    }
                    if !hideGameInfo {
                        HStack(spacing: 8) {
                            Text(game.sizeLabel)
                            Text(game.name.pathExtensionLabel)
                            if game.isExternal {
                                Label(settings.localized("External"), systemImage: "externaldrive")
                            }
                            if !hideRegionFlag, let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(settings.localized("Region"))
                            }
                        }
                        .font(
                            .system(size: 12 * resolvedGameInfoTextScale)
                        )
                        .foregroundStyle(secondaryTextColour)
                        .lineLimit(1)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .layoutPriority(1)
                Spacer()

                if !hideFavoriteButton {
                    favoriteButton(
                        for: game,
                        foreground: game.isFavorite
                            ? .yellow
                            : (controllerFocused ? accentColour : .gray)
                    )
                }

                Image(
                    systemName: running && !hideRunningIndicator
                        ? "play.fill"
                        : "chevron.right"
                )
                    .foregroundStyle(
                        running && !hideRunningIndicator
                            ? accentColour
                            : (controllerFocused ? accentColour : Color.secondary)
                    )
                    .font(.caption)
                    .accessibilityHidden(true)
            }
            .padding(
                .horizontal,
                10 * resolvedGameCardContentPaddingScale
            )
        }
        .buttonStyle(.plain)
        .foregroundStyle(.primary)
        .accessibilityLabel(game.displayName)
        .observeGameLibraryCardLongPress(
            onTouchBegan: suppressThemeShortcutForGameCardTouch
        ) {
            handleGameContextMenuLongPress(for: game)
        }
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry,
            isEnabled: visualEffectsEnabled
        )
    }

    @ViewBuilder
    private func gameGridCard(
        _ game: ISOEntry,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        if controllerInput == nil {
            originalGameGridCard(game)
        } else {
            controllerGameGridCard(
                game,
                controllerFocused: controllerFocused,
                visualEffectsEnabled: visualEffectsEnabled
            )
        }
    }

    /// Master's original touch-first grid card.
    private func originalGameGridCard(_ game: ISOEntry) -> some View {
        let running = isRunning(game)
        let horizontalPadding = 12 * resolvedGameCardContentPaddingScale
        let verticalPadding = 12 * resolvedGameCardVerticalPaddingScale
        return Button {
            open(game)
        } label: {
            ZStack(alignment: .topTrailing) {
                VStack(
                    alignment: .center,
                    spacing: 10 * resolvedGameCardTextSpacingScale
                ) {
                    coverThumbnail(
                        for: game,
                        width: 126 * resolvedGameCardScale
                            * resolvedGameCardWidthScale,
                        height: 189 * resolvedGameCardScale
                            * resolvedGameCardHeightScale,
                        running: running
                    )
                    .overlay(alignment: .topTrailing) {
                        if !hideFavoriteButton {
                            favoriteButton(
                                for: game,
                                font: .callout.weight(.semibold),
                                foreground: game.isFavorite
                                    ? .yellow
                                    : .white.opacity(0.86),
                                backgroundOpacity: 0.36,
                                padding: 6
                            )
                            .zIndex(2)
                        }
                    }
                    .frame(maxWidth: .infinity)

                    if !hideGameName || !hideGameInfo {
                        VStack(
                            alignment: .center,
                            spacing: 4 * resolvedGameCardTextSpacingScale
                        ) {
                            if !hideGameName {
                                HStack(alignment: .firstTextBaseline, spacing: 5) {
                                    Text(game.displayName)
                                        .font(
                                            .system(
                                                size: 15 * resolvedGameNameTextScale,
                                                weight: .semibold
                                            )
                                        )
                                        .foregroundStyle(contentTextColour)
                                        .lineLimit(gameNameLineLimit)
                                        .multilineTextAlignment(.center)
                                        .fixedSize(
                                            horizontal: false,
                                            vertical: true
                                        )
                                        .frame(
                                            maxWidth: .infinity,
                                            alignment: .center
                                        )
                                        .layoutPriority(1)
                                    if running && !hideRunningIndicator {
                                        Image(systemName: "circle.fill")
                                            .font(.system(size: 7))
                                            .foregroundStyle(accentColour)
                                            .accessibilityLabel(
                                                settings.localized("Running")
                                            )
                                            .fixedSize()
                                    }
                                }
                                .frame(
                                    minHeight: (singleLineGameNames ? 21 : 38)
                                        * resolvedGameNameTextScale,
                                    alignment: .top
                                )
                            }
                            if !hideGameInfo {
                                HStack(spacing: 6) {
                                    Text(game.name.pathExtensionLabel)
                                    Text(game.sizeLabel)
                                    if game.isExternal {
                                        Image(systemName: "externaldrive")
                                            .accessibilityLabel(
                                                settings.localized("External")
                                            )
                                    }
                                    if !hideRegionFlag,
                                       let flag = game.regionFlag {
                                        Text(flag).accessibilityLabel(
                                            settings.localized("Region")
                                        )
                                    }
                                }
                                .font(
                                    .system(
                                        size: 11 * resolvedGameInfoTextScale
                                    )
                                )
                                .foregroundStyle(secondaryTextColour)
                                .frame(maxWidth: .infinity, alignment: .center)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .center)
                    }
                }
                .padding(.horizontal, horizontalPadding)
                .padding(.vertical, verticalPadding)
                .frame(
                    maxWidth: .infinity,
                    minHeight: gameGridCardHeight(controllerOptimized: false),
                    alignment: .top
                )
            }
            .glassSurface(
                clear: true,
                cornerRadius: resolvedGameCardCornerRadius
            )
        }
        .buttonStyle(.plain)
        .modifier(
            GameLibraryControllerFocusModifier(
                isFocused: false,
                cornerRadius: resolvedGameCardCornerRadius,
                isFavorite: game.isFavorite,
                favoriteGlowEnabled:
                    settings.favoriteGlowingEffectEnabled,
                isEnabled: game.isFavorite
                    && settings.favoriteGlowingEffectEnabled,
                performanceOptimized: true,
                focusedScale: 1
            )
        )
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry
        )
    }

    private func controllerGameGridCard(
        _ game: ISOEntry,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        let running = isRunning(game)
        let horizontalPadding = 12 * resolvedGameCardContentPaddingScale
        let verticalPadding = 12 * resolvedGameCardVerticalPaddingScale
        let cardHeight = gameGridCardHeight(controllerOptimized: true)
        return Button {
            open(game)
        } label: {
            ZStack(alignment: .top) {
                GeometryReader { proxy in
                    VStack(
                        alignment: .center,
                        spacing: 4 * resolvedGameCardTextSpacingScale
                    ) {
                        VStack(
                            alignment: .center,
                            spacing: 10 * resolvedGameCardTextSpacingScale
                        ) {
                            coverThumbnail(
                                for: game,
                                width: 126 * resolvedGameCardScale
                                    * resolvedGameCardWidthScale,
                                height: 189 * resolvedGameCardScale
                                    * resolvedGameCardHeightScale,
                                running: running,
                                visualEffectsEnabled: visualEffectsEnabled
                            )
                            .overlay(alignment: .topTrailing) {
                                if !hideFavoriteButton {
                                    favoriteButton(
                                        for: game,
                                        font: .callout.weight(.semibold),
                                        foreground: game.isFavorite
                                            ? .yellow
                                            : (controllerFocused
                                                ? accentColour
                                                : .white.opacity(0.86)),
                                        backgroundOpacity: 0.36,
                                        padding: 6
                                    )
                                    .zIndex(2)
                                }
                            }
                            .controllerNavigationOrbTarget(
                                id: "library.game.\(game.id)",
                                isActive: controllerFocused
                                    && !controllerNavigationPresentationActive,
                                isEnabled: controllerInput != nil,
                                palette: .blue,
                                inset: -3,
                                orbScale: 0.92,
                                priority: 30
                            )

                            if !hideGameName {
                            Text(game.displayName)
                                    .font(
                                        .system(
                                            size: 15 * resolvedGameNameTextScale,
                                            weight: .semibold
                                        )
                                    )
                                    .foregroundStyle(
                                        controllerFocused
                                            ? settings.controllerCardFocusedColor
                                            : contentTextColour
                                    )
                                    .animation(
                                        ControllerFocusVisualAnimation.textFade,
                                        value: controllerFocused
                                    )
                                    .lineLimit(gameNameLineLimit)
                                    .multilineTextAlignment(.center)
                                    .fixedSize(horizontal: false, vertical: true)
                                    .frame(
                                        maxWidth: .infinity,
                                        minHeight: (singleLineGameNames ? 21 : 38)
                                            * resolvedGameNameTextScale,
                                        alignment: .top
                                    )
                                    .overlay(alignment: .trailing) {
                                        if running && !hideRunningIndicator {
                                            Image(systemName: "circle.fill")
                                                .font(.system(size: 7))
                                                .foregroundStyle(accentColour)
                                                .accessibilityLabel(settings.localized("Running"))
                                        }
                                    }
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .center)

                        if !hideGameInfo {
                            HStack(spacing: 6) {
                                Text(game.name.pathExtensionLabel)
                                Text(game.sizeLabel)
                                if game.isExternal {
                                    Image(systemName: "externaldrive")
                                        .accessibilityLabel(settings.localized("External"))
                                }
                                if !hideRegionFlag, let flag = game.regionFlag {
                                    Text(flag).accessibilityLabel(settings.localized("Region"))
                                }
                            }
                            .font(
                                .system(size: 11 * resolvedGameInfoTextScale)
                            )
                            .foregroundStyle(secondaryTextColour)
                            .frame(maxWidth: .infinity, alignment: .center)
                        }
                    }
                    .frame(
                        width: max(
                            0,
                            proxy.size.width - horizontalPadding * 2
                        )
                    )
                    .padding(.horizontal, horizontalPadding)
                    .padding(.vertical, verticalPadding)
                    .scaleEffect(
                        controllerFocused ? 1.10 : 1,
                        anchor: .center
                    )
                    .position(
                        x: proxy.size.width / 2
                            + (controllerFocused ? 12 : 0),
                        y: proxy.size.height / 2
                            + (controllerFocused ? 12 : 0)
                    )
                    .animation(
                        .smooth(duration: 0.16, extraBounce: 0.02),
                        value: controllerFocused
                    )
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: cardHeight)
            .glassSurface(
                interactive: true,
                clear: true,
                forceClear: controllerFocused,
                isEnabled: visualEffectsEnabled,
                cornerRadius: resolvedGameCardCornerRadius
            )
        }
        .buttonStyle(.plain)
        .frame(maxWidth: .infinity, alignment: .center)
        .modifier(
            GameLibraryControllerFocusModifier(
                isFocused: controllerFocused,
                cornerRadius: 18,
                isFavorite: game.isFavorite,
                favoriteGlowEnabled:
                    settings.favoriteGlowingEffectEnabled,
                isEnabled: controllerInput != nil
                    || (game.isFavorite
                        && settings.favoriteGlowingEffectEnabled),
                backgroundEffectsEnabled: visualEffectsEnabled
            )
        )
        .observeGameLibraryCardLongPress(
            onTouchBegan: suppressThemeShortcutForGameCardTouch
        ) {
            handleGameContextMenuLongPress(for: game)
        }
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry,
            isEnabled: visualEffectsEnabled
        )
    }

    private func gameGridCardHeight(
        controllerOptimized: Bool
    ) -> CGFloat {
        _ = controllerOptimized
        let coverHeight = 189 * resolvedGameCardScale
            * resolvedGameCardHeightScale
        let nameHeight = hideGameName
            ? 0
            : (singleLineGameNames ? 21 : 38) * resolvedGameNameTextScale
        let infoHeight = hideGameInfo
            ? 0
            : 13 * resolvedGameInfoTextScale
        let coverToNameSpacing = hideGameName
            ? 0
            : 10 * resolvedGameCardTextSpacingScale
        let contentSpacing = hideGameName || hideGameInfo
            ? 0
            : 4 * resolvedGameCardTextSpacingScale
        let verticalPadding = 24 * resolvedGameCardVerticalPaddingScale
        // Size the glass from the content it actually contains. The old fixed
        // 280-point frame left an unadjustable top/bottom area, especially
        // when titles or metadata were hidden.
        return max(
            96,
            coverHeight
                + nameHeight
                + infoHeight
                + coverToNameSpacing
                + contentSpacing
                + verticalPadding
        )
    }

    @ViewBuilder
    private func coverFlowCard(
        _ game: ISOEntry,
        metrics: CoverFlowMetrics,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        coverFlowCardSurface(
            game,
            metrics: metrics,
            controllerFocused: controllerFocused,
            visualEffectsEnabled: visualEffectsEnabled
        )
    }

    /// One explicitly framed landscape surface owns both touch and controller
    /// presentation. The complete card zooms from its center while its layout
    /// footprint remains stable inside the Cover Flow rail.
    private func coverFlowCardSurface(
        _ game: ISOEntry,
        metrics: CoverFlowMetrics,
        controllerFocused: Bool,
        visualEffectsEnabled: Bool
    ) -> some View {
        let running = isRunning(game)
        let cardWidth = coverFlowCardWidth(metrics: metrics)
        let contentWidth = max(
            0,
            cardWidth - (metrics.cardHorizontalPadding * 2)
        )
        let titleHeight: CGFloat = hideGameName
            ? 0
            : (singleLineGameNames
                ? (metrics.isCompact ? 20 : 22)
                : (metrics.isCompact ? 38 : 46)
            ) * resolvedGameNameTextScale
        let metadataHeight: CGFloat = hideGameInfo
            ? 0
            : (metrics.isCompact ? 14 : 17) * resolvedGameInfoTextScale
        let coverToTextSpacing: CGFloat = hideGameName && hideGameInfo
            ? 0
            : metrics.cardSpacing
        let interTextSpacing: CGFloat = hideGameName || hideGameInfo
            ? 0
            : 4 * resolvedGameCardTextSpacingScale
        let cardHeight = metrics.cardVerticalPadding * 2
            + metrics.coverHeight
            + coverToTextSpacing
            + titleHeight
            + interTextSpacing
            + metadataHeight

        return Button {
            open(game)
        } label: {
            ZStack(alignment: .top) {
                VStack(spacing: interTextSpacing) {
                    VStack(spacing: coverToTextSpacing) {
                        coverThumbnail(
                            for: game,
                            width: metrics.coverWidth,
                            height: metrics.coverHeight,
                            running: running,
                            visualEffectsEnabled: visualEffectsEnabled
                        )
                        .overlay(alignment: .topTrailing) {
                            if !hideFavoriteButton {
                                favoriteButton(
                                    for: game,
                                    font: (metrics.isCompact
                                        ? Font.subheadline
                                        : Font.headline).weight(.semibold),
                                    foreground: game.isFavorite
                                        ? .yellow
                                        : (controllerFocused
                                            ? accentColour
                                            : .white.opacity(0.88)),
                                    backgroundOpacity: 0.48,
                                    padding: metrics.favoritePadding
                                )
                                .padding(metrics.favoriteInset)
                                .zIndex(2)
                            }
                        }
                        .controllerNavigationOrbTarget(
                            id: "library.game.\(game.id)",
                            isActive: controllerFocused
                                && !controllerNavigationPresentationActive,
                            isEnabled: controllerInput != nil,
                            palette: .blue,
                            inset: -4,
                            orbScale: 0.96,
                            priority: 30
                        )

                        if !hideGameName {
                            Text(game.displayName)
                                .font(
                                    .system(
                                        size: (metrics.isCompact ? 15 : 17)
                                            * resolvedGameNameTextScale,
                                        weight: .semibold
                                    )
                                )
                                .foregroundStyle(
                                    controllerFocused
                                        ? settings.controllerCardFocusedColor
                                        : contentTextColour
                                )
                                .animation(
                                    ControllerFocusVisualAnimation.textFade,
                                    value: controllerFocused
                                )
                                .multilineTextAlignment(.center)
                                .lineLimit(gameNameLineLimit)
                                .fixedSize(horizontal: false, vertical: true)
                                .frame(
                                    width: metrics.textWidth,
                                    alignment: .center
                                )
                                .frame(
                                    minHeight: titleHeight,
                                    alignment: .top
                                )
                        }
                    }
                    .frame(width: contentWidth, alignment: .center)

                    if !hideGameInfo {
                        HStack(spacing: 4) {
                            Text(
                                game.isExternal
                                    ? "\(game.name.pathExtensionLabel)  \(game.sizeLabel)  \(settings.localized("External"))"
                                    : "\(game.name.pathExtensionLabel)  \(game.sizeLabel)"
                            )
                            if !hideRegionFlag,
                               let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(
                                    settings.localized("Region")
                                )
                            }
                        }
                        .font(
                            .system(
                                size: (metrics.isCompact ? 11 : 12)
                                    * resolvedGameInfoTextScale
                            )
                        )
                        .foregroundStyle(secondaryTextColour)
                        .frame(width: metrics.textWidth)
                    }
                }
                .padding(.vertical, metrics.cardVerticalPadding)
                .frame(width: contentWidth)
                .frame(
                    width: cardWidth,
                    height: cardHeight,
                    alignment: .center
                )
                .scaleEffect(
                    controllerFocused ? resolvedFocusedGameCardScale : 1,
                    anchor: .center
                )
                .offset(
                    x: controllerFocused ? 8 : 0,
                    y: controllerFocused ? 12 : 0
                )

            }
            .frame(width: cardWidth, height: cardHeight)
            .glassSurface(
                interactive: controllerInput != nil,
                clear: true,
                forceClear: controllerFocused,
                isEnabled: visualEffectsEnabled,
                cornerRadius: metrics.cornerRadius
            )
        }
        .buttonStyle(.plain)
        .modifier(
            GameLibraryControllerFocusModifier(
                isFocused: controllerFocused,
                cornerRadius: metrics.cornerRadius,
                isFavorite: game.isFavorite,
                favoriteGlowEnabled:
                    settings.favoriteGlowingEffectEnabled,
                isEnabled: controllerInput != nil
                    || (game.isFavorite
                        && settings.favoriteGlowingEffectEnabled),
                backgroundEffectsEnabled: visualEffectsEnabled,
                performanceOptimized: true,
                focusedScale: 1,
                optimizedAnimationDuration: 0.075
            )
        )
        .scaleEffect(
            controllerFocused ? resolvedFocusedGameCardScale : 1,
            anchor: .center
        )
        .offset(
            y: controllerFocused ? -resolvedFocusedGameCardLift : 0
        )
        .animation(
            .smooth(duration: 0.075, extraBounce: 0),
            value: controllerFocused
        )
        .observeGameLibraryCardLongPress(
            onTouchBegan: suppressThemeShortcutForGameCardTouch
        ) {
            handleGameContextMenuLongPress(for: game)
        }
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry,
            isEnabled: visualEffectsEnabled
        )
    }

    private func favoriteButton(
        for game: ISOEntry,
        font: Font = .body,
        foreground: Color,
        backgroundOpacity: Double? = nil,
        padding: CGFloat = 0
    ) -> some View {
        let icon = Image(systemName: game.isFavorite ? "star.fill" : "star")
            .font(font)
            .foregroundStyle(foreground)
            .padding(padding)
            .background {
                if let backgroundOpacity {
                    Circle().fill(.black.opacity(backgroundOpacity))
                }
            }
            .frame(minWidth: 44, minHeight: 44)
            .contentShape(Rectangle())

        return Button {
            toggleFavorite(game)
        } label: {
            if settings.favoriteGlowingEffectEnabled {
                icon
                    .contentTransition(.symbolEffect)
                    .symbolEffect(
                        .bounce,
                        value: favoriteAnimationValues[game.id, default: 0]
                    )
            } else {
                icon
            }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(
            game.isFavorite
                ? settings.localized("Remove from favorites")
                : settings.localized("Add to favorites")
        )
    }

	@ViewBuilder
    private func coverThumbnail(
		for game: ISOEntry,
		width: CGFloat = 58,
		height: CGFloat = 87,
		running: Bool,
        visualEffectsEnabled: Bool = true
	) -> some View {
        let runningGlow = running
            && !hideRunningIndicator
            && !nowRunningRemovalAnimationActive
        let favoriteGlow = game.isFavorite
            && settings.favoriteGlowingEffectEnabled
        let themedGlow = favoriteGlow || runningGlow
        let usesAnimatedGlow = visualEffectsEnabled
            && themedGlow
            && menuTabIsActive
            && !controllerNavigationPresentationActive
        let thumbnail = CoverThumbnailView(
            gameName: game.name,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature,
            width: width,
            height: height,
            cornerRadius: resolvedGameCoverCornerRadius
        )
        .opacity(resolvedGameCoverOpacity)

        if usesAnimatedGlow {
            let paletteColors = settings.controllerFocusBoxCustomColor.map {
                [$0.color]
            } ?? settings.controllerFocusBoxPalette.colors
            let primaryGlow = paletteColors.first ?? accentColour
            let secondaryGlow = paletteColors.dropFirst().first ?? primaryGlow
            thumbnail.modifier(
                GameCoverThemeGlowModifier(
                    primaryColor: primaryGlow,
                    secondaryColor: secondaryGlow
                )
            )
        } else {
            thumbnail.shadow(
                color: themedGlow
                    ? .clear
                    : .black.opacity(
                        0.28 * resolvedGameCoverShadowStrength
                    ),
                radius: 18 * resolvedGameCoverShadowStrength,
                y: 10 * resolvedGameCoverShadowStrength
            )
        }
    }

    private func vmStatusCard(gameName: String) -> some View {
        let isStopping = nowRunningRemovalAnimationActive

        return HStack(spacing: 12) {
            HStack(spacing: 8) {
                Image(
                    systemName: isStopping
                        ? "stop.circle.fill"
                        : "play.circle.fill"
                )
                    .font(.title2)
                    .foregroundStyle(
                        isStopping ? criticalTextColour : accentColour
                    )
                    .contentTransition(.symbolEffect(.replace))

                RunningStatusText(
                    isStopping: isStopping,
                    runningLabel: settings.localized("Now Running"),
                    gameTitle: displayTitle(forRunningName: gameName),
                    stoppingLabel: settings.localized("Stopping..."),
                    stoppingFont: .title2,
                    statusColor: accentColour,
                    titleColor: contentTextColour
                )
            }
            .opacity(displayedNowRunningStatusOpacity)
            .animation(.easeInOut(duration: 0.3), value: displayedNowRunningStatusOpacity)

            if !isStopping {
                Spacer()
                HStack(spacing: 8) {
                    Button(settings.localized("Resume")) {
                        appState.returnToGame()
                    }
                    .buttonStyle(.borderedProminent)
                    .modifier(nowRunningControllerFocus(.resume))
                    Button(role: .destructive) {
                        showStopAlert = true
                    } label: {
                        Image(systemName: "stop.circle")
                    }
                    .buttonStyle(.bordered)
                    .modifier(nowRunningControllerFocus(.stop))
                }
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(
            maxWidth: .infinity,
            alignment: isStopping ? .center : .leading
        )
        .padding()
        .background {
            nowRunningCoverBackground(
                gameName: gameName,
                cornerRadius: 18
            )
        }
        .clipShape(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
        )
        .animation(
            .smooth(duration: 0.42, extraBounce: 0.02),
            value: isStopping
        )
        .glassSurface(
            clear: true,
            materializeTransition: true,
            cornerRadius: 18
        )
    }

    private func vmStatusCoverCard(gameName: String, metrics: CoverFlowMetrics) -> some View {
        let isStopping = nowRunningRemovalAnimationActive

        return VStack(spacing: metrics.isCompact ? 9 : 14) {
            VStack(spacing: metrics.isCompact ? 7 : 10) {
                Image(
                    systemName: isStopping
                        ? "stop.circle.fill"
                        : "play.circle.fill"
                )
                    .font(.system(size: metrics.statusIconSize))
                    .foregroundStyle(
                        isStopping ? criticalTextColour : accentColour
                    )
                    .contentTransition(.symbolEffect(.replace))

                RunningStatusText(
                    isStopping: isStopping,
                    runningLabel: settings.localized("Now Running"),
                    gameTitle: displayTitle(forRunningName: gameName),
                    stoppingLabel: settings.localized("Stopping..."),
                    stoppingFont: metrics.isCompact ? .headline : .title3,
                    statusColor: accentColour,
                    titleColor: contentTextColour,
                    centered: true
                )
            }
            .opacity(displayedNowRunningStatusOpacity)
            .animation(.easeInOut(duration: 0.3), value: displayedNowRunningStatusOpacity)

            if !isStopping {
                HStack(spacing: 8) {
                    Button(settings.localized("Resume")) {
                        appState.returnToGame()
                    }
                    .buttonStyle(.borderedProminent)
                    .modifier(nowRunningControllerFocus(.resume))
                    Button(role: .destructive) {
                        showStopAlert = true
                    } label: {
                        Image(systemName: "stop.circle")
                    }
                    .buttonStyle(.bordered)
                    .modifier(nowRunningControllerFocus(.stop))
                }
                .controlSize(metrics.isCompact ? .small : .regular)
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(width: metrics.statusWidth, height: metrics.statusHeight)
        .padding(.horizontal, metrics.cardHorizontalPadding)
        .padding(.vertical, metrics.cardVerticalPadding)
        .background {
            nowRunningCoverBackground(
                gameName: gameName,
                cornerRadius: metrics.cornerRadius
            )
        }
        .clipShape(
            RoundedRectangle(
                cornerRadius: metrics.cornerRadius,
                style: .continuous
            )
        )
        .animation(
            .smooth(duration: 0.42, extraBounce: 0.02),
            value: isStopping
        )
        .glassSurface(
            clear: true,
            forceClear: controllerInput?.hasConnectedController == true,
            materializeTransition: true,
            cornerRadius: metrics.cornerRadius
        )
    }

	@ViewBuilder
	private func gameContextMenu(for game: ISOEntry) -> some View {
        Button {
            presentMenuPanel("game_info") {
                gameInfoTarget = game
            }
        } label: {
            Label(settings.localized("Game Info"), systemImage: "info.circle")
        }

		Button {
			presentMenuPanel("per_game_settings") {
				gameSettingsStartsInShaders = false
				prepareGameSettings(for: game)
				gameSettingsTarget = game
			}
		} label: {
			Label(settings.localized("Per-Game Settings"), systemImage: "slider.horizontal.3")
		}

        Button {
            presentMenuPanel("per_game_custom_skin") {
                presentPerGameCustomSkin(for: game)
            }
        } label: {
            Label(
                settings.localized("Per-Game Custom Skin"),
                systemImage: "gamecontroller.fill"
            )
        }

        if game.isELF {
            discPathMenu(for: game)
        }

        Button {
            presentMenuPanel("cheats_patches") {
                cheatsManagerTarget = game
            }
        } label: {
            Label(settings.localized("Cheats & Patches"), systemImage: "rectangle.stack.badge.plus")
        }

        Menu {
            Button {
                downloadCover(for: game)
            } label: {
                Label(settings.localized("Download Cover"), systemImage: "icloud.and.arrow.down")
            }
            .disabled(coverStore.isDownloadingCovers)

            Button {
                presentMenuPanel("cover_photos") {
                    pendingCoverPhotoGameName = game.name
                    showCoverPhotoPicker = true
                }
            } label: {
                Label(settings.localized("Choose from Photos"), systemImage: "photo.on.rectangle")
            }

            Button {
                presentMenuPanel("cover_files") {
                    pendingCoverGameName = game.name
                    showCoverImporter = true
                }
            } label: {
                Label(settings.localized("Choose from Files"), systemImage: "folder")
            }

            if game.coverURL != nil {
                Button(role: .destructive) {
                    coverStore.removeManagedCovers(forGameNamed: game.name)
                    loadGames()
                } label: {
                    Label(settings.localized("Remove Cover"), systemImage: "trash")
                }
            }
        } label: {
            Label(settings.localized("Covers"), systemImage: "photo.stack")
        }

        Button {
            copyLaunchLink(game)
        } label: {
            Label(settings.localized("Copy Launch Link"), systemImage: "doc.on.doc")
        }

        Divider()

        Menu {
            Button {
                clearGameCache(game)
            } label: {
                Label(settings.localized("Clear Game Cache"), systemImage: "trash.slash")
            }

            Button(role: .destructive) {
                presentMenuPanel("delete_game_data") {
                    pendingDeleteDataGame = game
                }
            } label: {
                Label(settings.localized("Delete Game Data"), systemImage: "externaldrive.badge.xmark")
            }

			if !game.isExternal {
				Button(role: .destructive) {
					presentMenuPanel("delete_game") {
						pendingDeleteGame = game
					}
				} label: {
					Label(settings.localized("Delete Game"), systemImage: "trash")
				}
			}
		} label: {
			Label(settings.localized("Game Data"), systemImage: "externaldrive")
		}

        Divider()

        Button {
            presentMenuPanel("rename") {
                beginRenaming(game)
            }
        } label: {
            Label(settings.localized("Rename"), systemImage: "pencil")
        }
	}

	@ViewBuilder
	private func discPathMenu(for game: ISOEntry) -> some View {
		let linkedDisc = ARMSX2Bridge.linkedDiscPath(forELF: game.bootName)
		Menu {
			Button {
				presentMenuPanel("disc_path") {
					discLinkTarget = game
				}
			} label: {
				Label(settings.localized(linkedDisc?.isEmpty == false ? "Change Disc Image" : "Link Disc Image"), systemImage: "link")
			}

			if let linkedDisc, !linkedDisc.isEmpty {
				Button(role: .destructive) {
					ARMSX2Bridge.setLinkedDiscPath(nil, forELF: game.bootName)
					loadGames()
				} label: {
					Label(settings.localized("Remove Disc Link"), systemImage: "xmark.circle")
				}
			}
		} label: {
			Label(settings.localized("Disc Path"), systemImage: "opticaldisc")
		}
	}

	private func presentMenuPanel(_ name: String, _ action: @escaping () -> Void) {
		NSLog("[ARMSX2 iOS GameListMenu] present \(name)")
		DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
			action()
		}
	}

	private func open(
        _ game: ISOEntry,
        allowsAutomaticSkinPrompt: Bool = true
    ) {
		// A touch long-press shares the card's gesture arena. Once the unified
		// context surface owns this game, ignore the Button release from that same
		// press instead of launching underneath the menu.
		guard controllerContextMenuState.gameID != game.id else { return }
		MenuAudioPackManager.shared.suppressPendingGenericActivation()
		if isRunning(game) {
			appState.returnToGame()
			return
		}

		guard ARMSX2Bridge.canResolveISO(game.bootName) else {
			gameActionTitle = settings.localized("Game Not Found")
			gameActionMessage = settings.localized("This game file is no longer available. Refresh the library or import it again.")
			loadGames()
			return
		}

        let transition = makeGameplayLaunchTransition(for: game)
        let automaticSkinOptions = allowsAutomaticSkinPrompt
            ? AutomaticCustomSkinManager.shared.availableProposals(
                forSerial: game.metadata["serial"]
            )
            : []
        if !automaticSkinOptions.isEmpty {
            MenuAudioPackManager.shared.playEvent(.uiToast)
            pendingAutomaticCustomSkinLaunch = PendingAutomaticCustomSkinLaunch(
                game: game,
                proposals: automaticSkinOptions,
                purpose: .gameLaunch,
                transition: transition
            )
            return
        }

        continueGameLaunch(game, transition: transition)
	}

    private func continueGameLaunch(
        _ game: ISOEntry,
        transition: GameplayLaunchTransition?,
        preferredPadSerial: String? = nil
    ) {
        let padSerial = preferredPadSerial ?? game.metadata["serial"]
        let padCRC = game.metadata["crc"]
        if appState.runningGameName != nil {
            pendingGameName = game.bootName
            pendingGameplayLaunchTransition = transition
            pendingGameplayPadSerial = padSerial
            pendingGameplayPadCRC = padCRC
            showRestartAlert = true
        } else {
            appState.prepareGameplayPadSelection(
                forSerial: padSerial,
                crc: padCRC
            )
            appState.bootGame(
                isoName: game.bootName,
                launchTransition: transition
            )
        }
    }

    private func completeAutomaticCustomSkinLaunch(apply: Bool) {
        guard let pending = pendingAutomaticCustomSkinLaunch else { return }
        pendingAutomaticCustomSkinLaunch = nil
        if apply {
            let applied = AutomaticCustomSkinManager.shared.apply(
                pending.proposal,
                toISO: pending.game.bootName,
                metadata: pending.game.metadata,
                setCustomLayout: pending.setCustomLayout,
                doNotShowAgain: pending.purpose == .gameLaunch
                    && pending.doNotShowAgain
            )
            if !applied {
                NSLog(
                    "[ARMSX2 iOS Skins] automatic per-game assignment failed serial=%@",
                    pending.proposal.serial
                )
            } else if let descriptor = VPadSkinLibraryStore.shared.descriptor(
                id: pending.proposal.skinID
            ) {
                // Populate both render and hit-mask caches before switching to
                // gameplay. No post-launch skin replacement or decode hitch is
                // then needed for the accepted package.
                ControllerAsset.prewarm(descriptor: descriptor)
                ARMSX2VirtualPadMaskImageCache.prewarm(
                    descriptor: descriptor
                )
            }
        } else if pending.purpose == .gameLaunch {
            AutomaticCustomSkinManager.shared.decline(
                pending.proposal,
                doNotShowAgain: pending.doNotShowAgain
            )
        }
        guard pending.purpose == .gameLaunch else { return }
        continueGameLaunch(
            pending.game,
            transition: pending.transition,
            preferredPadSerial: pending.proposal.serial
        )
    }

    private func presentPerGameCustomSkin(for game: ISOEntry) {
        let serial = game.metadata["serial"]
        let proposals = AutomaticCustomSkinManager.shared
            .installedProposals(forSerial: serial)
        guard !proposals.isEmpty else {
            gameActionTitle = settings.localized("Per-Game Custom Skin")
            gameActionMessage = settings.localized(
                "This game does not have a usable serial for a per-game skin assignment."
            )
            return
        }

        let selectedSkinID = PadLayoutPresetStore.shared.automaticSkinID(
            forSerial: proposals[0].serial
        )
        let selectedIndex = proposals.firstIndex(where: {
            $0.skinID == selectedSkinID
        }) ?? 0
        pendingAutomaticCustomSkinLaunch = PendingAutomaticCustomSkinLaunch(
            game: game,
            proposals: proposals,
            selectedProposalIndex: selectedIndex,
            doNotShowAgain: false,
            purpose: .library,
            transition: nil
        )
        controllerAlertSelectedIndex = 0
        MenuAudioPackManager.shared.playEvent(.uiToast)

        // The manual picker is an explicit request: run the same exact/`any`
        // catalog installation used by Automatic Download Custom Skin, then
        // merge the downloaded choices into the already-visible picker.
        Task { @MainActor in
            let refreshed = await AutomaticCustomSkinManager.shared
                .downloadAvailableProposals(forSerial: serial)
            guard !refreshed.isEmpty,
                  var pending = pendingAutomaticCustomSkinLaunch,
                  pending.purpose == .library,
                  pending.game.id == game.id else { return }
            let selectedSkinID = pending.proposal.skinID
            pending.proposals = refreshed
            pending.selectedProposalIndex = refreshed.firstIndex {
                $0.skinID == selectedSkinID
            } ?? 0
            pendingAutomaticCustomSkinLaunch = pending
        }
    }

    private func performBootBIOSAction() {
        if appState.runningGameName == "BIOS" {
            appState.returnToGame()
        } else if appState.runningGameName != nil {
            pendingGameName = ""
            pendingGameplayLaunchTransition = nil
            pendingGameplayPadSerial = nil
            pendingGameplayPadCRC = nil
            showRestartAlert = true
        } else {
            appState.bootBIOSOnly()
        }
    }

    private func openGameImporter() {
        MenuAudioPackManager.shared.playEvent(.contextMenu)
        showGameImporter = true
    }

    @MainActor
    private func makeGameplayLaunchTransition(
        for game: ISOEntry,
        respectingLaunchAnimationSetting: Bool = true
    ) -> GameplayLaunchTransition? {
        guard !respectingLaunchAnimationSetting
            || settings.gameCardZoomAnimationEnabled else {
            return nil
        }

        let cardView = gameplayLaunchCardRegistry.view(for: game.id)
        guard let window = cardView?.window ?? activeMenuWindow() else {
            return nil
        }

        let usesCoverFlow = libraryLayout == "grid"
            && landscapeCoverFlowEnabled
            && window.bounds.width > window.bounds.height
        let style: GameplayLaunchCardStyle
        let coverSize: CGSize
        let cornerRadius: CGFloat
        if usesCoverFlow {
            let metrics = CoverFlowMetrics(
                containerSize: window.bounds.size,
                controllerOptimized: true,
                cardScale: resolvedGameCardScale,
                widthScale: resolvedGameCardWidthScale,
                heightScale: resolvedGameCardHeightScale,
                gapScale: resolvedGameCardGapScale,
                contentPaddingScale: resolvedGameCardContentPaddingScale,
                verticalPaddingScale: resolvedGameCardVerticalPaddingScale,
                textSpacingScale: resolvedGameCardTextSpacingScale,
                cardCornerRadius: resolvedGameCardCornerRadius
            )
            style = .coverFlow
            coverSize = CGSize(
                width: metrics.coverWidth,
                height: metrics.coverHeight
            )
            cornerRadius = metrics.cornerRadius
        } else if libraryLayout == "grid" {
            style = .grid
            coverSize = CGSize(
                width: 126 * resolvedGameCardScale
                    * resolvedGameCardWidthScale,
                height: 189 * resolvedGameCardScale
                    * resolvedGameCardHeightScale
            )
            cornerRadius = resolvedGameCardCornerRadius
        } else {
            style = .list
            coverSize = CGSize(
                width: 58 * resolvedGameCardScale
                    * resolvedGameCardWidthScale,
                height: 87 * resolvedGameCardScale
                    * resolvedGameCardHeightScale
            )
            cornerRadius = resolvedGameCardCornerRadius
        }

        let sourceFrame: CGRect
        if let cardView,
           cardView.window === window {
            let candidate = cardView
                .convert(cardView.bounds, to: window)
                .intersection(window.bounds)
            sourceFrame = isValidGameplayLaunchFrame(candidate)
                ? candidate
                : fallbackGameplayLaunchFrame(
                    style: style,
                    coverSize: coverSize,
                    in: window.bounds
                )
        } else {
            sourceFrame = fallbackGameplayLaunchFrame(
                style: style,
                coverSize: coverSize,
                in: window.bounds
            )
        }

        let coverImage = game.coverURL.flatMap {
            CoverThumbnailCache.shared.imageForGameplayTransition(
                for: $0,
                signature: game.coverSignature,
                width: coverSize.width,
                height: coverSize.height,
                scale: window.screen.scale
            )
        }
        let detailParts: [String]
        if hideGameInfo {
            detailParts = []
        } else {
            detailParts = [
                game.name.pathExtensionLabel,
                game.sizeLabel,
                game.isExternal ? settings.localized("External") : nil,
                hideRegionFlag ? nil : game.regionFlag,
            ].compactMap { $0 }
        }

        return GameplayLaunchTransition(
            sourceFrame: sourceFrame,
            cornerRadius: cornerRadius,
            style: style,
            gameName: game.name,
            title: game.displayName,
            detail: detailParts.joined(separator: "  "),
            coverImage: coverImage,
            coverSize: coverSize,
            isFavorite: game.isFavorite,
            showsGameName: !hideGameName,
            showsGameInfo: !hideGameInfo,
            showsFavoriteIndicator: !hideFavoriteButton,
            usesSingleLineGameName: singleLineGameNames,
            usesClearGlass: true
        )
    }

    /// Supplies a larger live SwiftUI target for the system context-menu zoom.
    /// Reusing the launch card keeps its glass, cover, text, and star in one
    /// composited surface instead of scaling a rasterized snapshot.
    @ViewBuilder
    private func gameContextMenuPreview(for game: ISOEntry) -> some View {
        if let transition = makeGameplayLaunchTransition(
            for: game,
            respectingLaunchAnimationSetting: false
        ) {
            let sourceSize = contextMenuPreviewSourceSize(
                for: game,
                transition: transition
            )
            let scale = contextMenuPreviewScale(
                for: transition.style,
                sourceSize: sourceSize
            )

            FluidGameplayLaunchCard(
                transition: transition,
                showsUnselectedFavoriteIndicator: true
            )
                .frame(width: sourceSize.width, height: sourceSize.height)
                .scaleEffect(scale)
                .frame(
                    width: sourceSize.width * scale,
                    height: sourceSize.height * scale
                )
                .shadow(
                    color: .black.opacity(0.32),
                    radius: 22,
                    y: 12
                )
        } else {
            EmptyView()
        }
    }

    private func contextMenuPreviewSourceSize(
        for game: ISOEntry,
        transition: GameplayLaunchTransition
    ) -> CGSize {
        if let view = gameplayLaunchCardRegistry.view(for: game.id) {
            let size = view.bounds.size
            if size.width > 2,
               size.height > 2,
               size.width.isFinite,
               size.height.isFinite {
                return size
            }
        }
        return transition.sourceFrame.size
    }

    private func contextMenuPreviewScale(
        for style: GameplayLaunchCardStyle,
        sourceSize: CGSize
    ) -> CGFloat {
        let desiredScale: CGFloat
        switch style {
        case .list:
            desiredScale = 1.06
        case .grid:
            desiredScale = 1.20
        case .coverFlow:
            desiredScale = 1.14
        }

        let viewport = activeMenuWindow()?.bounds.size
            ?? UIScreen.main.bounds.size
        let widthLimit = (viewport.width * 0.88) / max(sourceSize.width, 1)
        let heightLimit = (viewport.height * 0.76) / max(sourceSize.height, 1)
        return max(1, min(desiredScale, min(widthLimit, heightLimit)))
    }

    @MainActor
    private func activeMenuWindow() -> UIWindow? {
        UIApplication.shared.appWindowScene?.windows.first(where: { $0.isKeyWindow })
    }

    private func isValidGameplayLaunchFrame(_ frame: CGRect) -> Bool {
        !frame.isNull
            && !frame.isInfinite
            && frame.origin.x.isFinite
            && frame.origin.y.isFinite
            && frame.width.isFinite
            && frame.height.isFinite
            && frame.width > 2
            && frame.height > 2
    }

    private func fallbackGameplayLaunchFrame(
        style: GameplayLaunchCardStyle,
        coverSize: CGSize,
        in windowBounds: CGRect
    ) -> CGRect {
        let size: CGSize
        switch style {
        case .list:
            size = CGSize(
                width: max(120, windowBounds.width - 32),
                height: 112
            )
        case .grid:
            size = CGSize(
                width: min(180, max(142, windowBounds.width * 0.42)),
                height: 268
            )
        case .coverFlow:
            size = CGSize(
                width: coverSize.width + 24,
                height: min(
                    windowBounds.height * 0.82,
                    coverSize.height + 88
                )
            )
        }

        return CGRect(
            x: windowBounds.midX - size.width / 2,
            y: windowBounds.midY - size.height / 2,
            width: size.width,
            height: size.height
        ).intersection(windowBounds)
    }

    private var emptyState: some View {
        let controllerFocused = menuTabIsActive
            && controllerInput?.isControllerNavigationEnabled == true
            && !controllerFocusReleased
            && controllerInput?.navigationZone == .library

        return VStack(spacing: 16) {
            Image(systemName: "opticaldisc")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            Text(settings.localized("No Games Found"))
                .font(.title2)
                .fontWeight(.semibold)
            Text(settings.localized("Import PS2 disc images to add them here."))
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                openGameImporter()
            } label: {
                MenuImportActionLabel(
                    title: settings.localized("Import Games"),
                    explicitlyFocused: controllerFocused
                )
            }
            .buttonStyle(.plain)
            .focusEffectDisabled()
            .accessibilityAddTraits(controllerFocused ? .isSelected : [])
            .modifier(
                GameLibraryControllerFocusModifier(
                    isFocused: controllerFocused,
                    cornerRadius: 12,
                    isEnabled: controllerInput != nil,
                    performanceOptimized: true,
                    focusedScale: 1
                )
            )
            .controllerNavigationOrbTarget(
                id: "library.import-games",
                isActive: controllerFocused,
                palette: .blue,
                inset: 2,
                orbScale: 0.9,
                priority: 10
            )
			Label(settings.localized("External Games are in Settings > Storage"), systemImage: "externaldrive")
				.font(.caption)
				.foregroundStyle(.secondary)
			Text("\(settings.localized("Supported Formats")): ISO, CHD, BIN, CSO, ZSO, GZ, ELF")
				.font(.caption)
				.foregroundStyle(.secondary)
				.multilineTextAlignment(.center)
        }
        .padding()
        .frame(maxWidth: .infinity, minHeight: 240)
    }

    private func loadGames(
        autoDownloadExternalCovers: Bool = false,
        forceMetadataRefresh: Bool = false
    ) {
        let allowFullMetadata = appState.runningGameName == nil
        let existingGames = GameLibrarySnapshot.shared.existingEntries(
            merging: games
        )
        let existingMetadata = existingGames.mapValues(\.metadata)
        externalLibrary.reload()
        gameLibraryActivity.startLoad {
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
                let displayName = GameDisplayNameStore.shared.displayName(
                    for: record.id,
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
    private func scheduleLibraryReload(
        after delay: Duration,
        autoDownloadExternalCovers: Bool
    ) {
        gameLibraryActivity.scheduleReload(after: delay) {
            loadGames(
                autoDownloadExternalCovers: autoDownloadExternalCovers
            )
        }
    }

	private func restoreCachedGamesIfNeeded() {
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

    private func downloadMissingCovers() {
        let targets = games.map(\.coverInfo)
        coverWorkTask?.cancel()
        coverWorkTask = Task { @MainActor in
            _ = await coverStore.downloadMissingCovers(for: targets)
			guard !Task.isCancelled else { return }
            loadGames()
        }
    }

    private func prepareGameImport(_ urls: [URL]) {
        let existingFileNames = fileImporter.existingFileNames(for: urls, preferredDestination: .game)
        guard !existingFileNames.isEmpty else {
            importGames(urls, allowReplacingExistingFiles: false)
            return
        }

        pendingGameImportURLs = urls
        existingGameImportFileNames = existingFileNames
        showGameReplacementAlert = true
    }

    private func importGames(_ urls: [URL], allowReplacingExistingFiles: Bool) {
        let importedGames = fileImporter.importURLs(
            urls,
            preferredDestination: .game,
            allowReplacingExistingFiles: allowReplacingExistingFiles
        )
        loadGames()
        if !importedGames.isEmpty,
           controllerInput != nil,
           !usesLandscapeCoverFlow {
            libraryScrollRestoreState.requestTopRestore()
        }
        autoDownloadCovers(for: importedGames)
    }

    private func clearPendingGameImport() {
        pendingGameImportURLs = []
        existingGameImportFileNames = []
    }

    private func downloadCover(for game: ISOEntry) {
        AutomaticCustomSkinManager.shared.enqueueMatches(for: [game.coverInfo])
        coverWorkTask?.cancel()
        coverWorkTask = Task { @MainActor in
            _ = await coverStore.downloadMissingCovers(for: [game.coverInfo])
			guard !Task.isCancelled else { return }
            loadGames()
        }
    }

    private func importCoverPhoto(_ photoItem: PhotosPickerItem, forGameNamed gameName: String) {
        Task { @MainActor in
            do {
                guard let data = try await photoItem.loadTransferable(type: Data.self) else {
                    coverStore.lastCoverMessage = "The selected photo could not be loaded."
                    coverStore.showCoverAlert = true
                    return
                }
                coverStore.importCoverData(data, forGameNamed: gameName)
                loadGames()
            } catch {
                coverStore.lastCoverMessage = "Cover import failed: \(error.localizedDescription)"
                coverStore.showCoverAlert = true
            }
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
		guard appState.runningGameName == nil else { return }

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
		let serials = coverTargets.map { $0.metadata["serial"] ?? "" }.filter { !$0.isEmpty }.joined(separator: ",")
		NSLog("[ARMSX2 iOS Covers] auto-download external missing covers count=%d serials=%@", targets.count, serials)
		coverWorkTask?.cancel()
		coverWorkTask = Task { @MainActor in
			let summary = await coverStore.downloadMissingCovers(for: coverTargets, showResult: false)
			guard !Task.isCancelled else { return }
			if summary.downloaded > 0 {
				loadGames()
			}
		}
	}

	private func releaseLibraryResourcesForGameplay() {
		coverFlowPreheater.cancel()
		coverWorkTask?.cancel()
		coverWorkTask = nil
        nowRunningRemovalResetTask?.cancel()
        nowRunningRemovalResetTask = nil
        favoriteAnimationValues.removeAll(keepingCapacity: false)
        nowRunningRemovalAnimationActive = false
        departingRunningGameName = nil
        nowRunningCardOpacity = 1
        nowRunningCardScale = 1
        nowRunningStatusOpacity = 1
        gameLibraryActivity.isScrolling = false
        gameLibraryActivity.cancelScheduledReload()
        gameLibraryActivity.cancelLoad()
        gameplayLaunchCardRegistry.removeAll()
			games.removeAll(keepingCapacity: false)
		externalCoverAutoDownloadAttemptedIDs.removeAll(keepingCapacity: false)
		GameLibraryRuntimeResources.releaseForGameplay()
		}

    private func scheduleStopRunningGame() {
        showStopAlert = false
        guard !nowRunningRemovalAnimationActive,
              let runningGameName = appState.runningGameName else {
            return
        }
        MenuAudioPackManager.shared.playEvent(.stoppingGame)

        // The two actions disappear during departure; retire their focus at
        // the same time and keep it on the stopped game's library card.
        if controllerNowRunningFocusState.selectedAction != nil {
            controllerNowRunningFocusState.selectedAction = nil
            controllerNavigationActive = true
            controllerSelectedGameID = games.first(where: {
                $0.bootName == runningGameName
            })?.id ?? games.first?.id
        }

        nowRunningRemovalResetTask?.cancel()
        nowRunningCardOpacity = 1
        nowRunningCardScale = 1
        nowRunningStatusOpacity = 1
        departingRunningGameName = runningGameName
        withAnimation(.smooth(duration: 0.42, extraBounce: 0.02)) {
            // Enter a visible stopping state immediately after confirmation:
            // play becomes stop, actions dissolve, and both labels morph into
            // one prominent status before the whole glass card leaves.
            nowRunningRemovalAnimationActive = true
        }

        nowRunningRemovalResetTask = Task { @MainActor in
            // Let the status morph complete, fade its foreground away, then
            // collapse the complete composited glass surface to zero pixels.
            try? await Task.sleep(for: .milliseconds(420))
            guard !Task.isCancelled else { return }

            withAnimation(.easeInOut(duration: 0.3)) {
                nowRunningStatusOpacity = 0
            }
            try? await Task.sleep(for: .milliseconds(320))
            guard !Task.isCancelled else { return }

            withAnimation(.easeInOut(duration: 0.65)) {
                nowRunningCardOpacity = 0
                nowRunningCardScale = 0
            }
            try? await Task.sleep(for: .milliseconds(700))
            guard !Task.isCancelled else { return }

            // Stop the VM as soon as the card's own departure completes. Keep
            // the now-invisible layout placeholder alive while a drag or
            // deceleration is active, so removing it cannot move the content
            // underneath the user's finger.
            appState.stopGame()

            while gameLibraryActivity.isScrolling {
                try? await Task.sleep(for: .milliseconds(50))
                guard !Task.isCancelled else { return }
            }

            withAnimation(.smooth(duration: 0.52, extraBounce: 0.02)) {
                departingRunningGameName = nil
            }
            try? await Task.sleep(for: .milliseconds(540))
            guard !Task.isCancelled else { return }

            // The collapsed values remain private to the departed placeholder.
            // Clearing only the phase flag avoids a second transaction through
            // every persistent game-card glass surface.
            nowRunningRemovalAnimationActive = false
            nowRunningRemovalResetTask = nil
        }
    }

	@discardableResult
	private func toggleFavorite(
        _ game: ISOEntry,
        playsSelectionFeedback: Bool = true
    ) -> Bool {
		let key = game.bootName
		let current = games.first(where: { $0.id == game.id })?.isFavorite
            ?? ARMSX2Bridge.isFavorite(key)
        let updatedValue = !current
		ARMSX2Bridge.setFavorite(key, favorite: updatedValue)
        if settings.favoriteGlowingEffectEnabled {
            favoriteAnimationValues[game.id, default: 0] += 1
        }
        var reorderTransaction = Transaction()
        reorderTransaction.animation = nil
        withTransaction(reorderTransaction) {
            if let index = games.firstIndex(where: { $0.id == game.id }) {
                games[index].isFavorite = updatedValue
                games.sort { lhs, rhs in
                    if lhs.isFavorite != rhs.isFavorite {
                        return lhs.isFavorite
                    }
                    return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
                }
            }
        }
        if controllerInput != nil {
            favoriteScrollSequence &+= 1
            let firstGameID = games.first?.id ?? game.id
            favoriteScrollRequest = GameLibraryFavoriteScrollRequest(
                sequence: favoriteScrollSequence,
                destination: updatedValue
                    ? .firstGame(firstGameID)
                    : .game(game.id)
            )
        }

        GameLibrarySnapshot.shared.update(games)
		if playsSelectionFeedback {
			UISelectionFeedbackGenerator().selectionChanged()
		}
		return updatedValue
	}

	private func copyLaunchLink(_ game: ISOEntry) {
		let link = ARMSX2DeepLinkHandler.launchURL(forISO: game.name)
		UIPasteboard.general.string = link
		presentMenuPanel("copy_launch_link") {
			gameActionTitle = "Launch Link Copied"
			gameActionMessage = link
		}
	}

	private func clearGameCache(_ game: ISOEntry) {
		gameActionTitle = "Clear Game Cache"
		gameActionMessage = ARMSX2Bridge.clearCache(forISO: game.bootName)
	}

	private func deleteGameData(_ game: ISOEntry) {
		gameActionTitle = "Delete Game Data"
		gameActionMessage = ARMSX2Bridge.deleteGameData(forISO: game.bootName)
	}

	private func deleteGame(_ game: ISOEntry, deleteData: Bool) {
		if isRunning(game) {
			gameActionTitle = "Delete Game"
			gameActionMessage = settings.localized("Stop this game before deleting it.")
			return
		}

		let success = ARMSX2Bridge.deleteISO(game.bootName, deleteGameData: deleteData)
		if success {
			coverStore.removeManagedCovers(forGameNamed: game.name)
			loadGames()
        }
		gameActionTitle = "Delete Game"
		gameActionMessage = success ? settings.localized("Game deleted.") : settings.localized("Could not delete this game file.")
	}

	private func isRunning(_ game: ISOEntry) -> Bool {
        let runningIdentifiers = gameLibraryActivity.runningIdentifiers(
            for: appState.runningGameName
        )
        return !runningIdentifiers.isDisjoint(
            with: game.normalizedIdentifiers
        )
    }

}

/// Uses the same bounded command-deck presentation as the in-emulation Quick
/// Menu route. Keeping this in a full-screen presentation removes the sheet's
/// detent scroll view and isolates the retained Game Library from touch and
/// right-stick scrolling while per-game controls are active.
private struct LibraryPerGameSettingsOverlay: View {
    let game: ISOEntry
    let preloadedSettings: [String: Any]?
    let initiallySelectsShaders: Bool
    let controllerInput: MenuControllerInputRouter?
    let onDone: () -> Void

    var body: some View {
        GameOverlayContainer(
            frameMode: .landscapePanel,
            dimsBackground: false
        ) { _ in
            PerGameSettingsPanel(
                game: game,
                preloadedSettings: preloadedSettings,
                initiallySelectsShaders: initiallySelectsShaders,
                controllerInput: controllerInput,
                onDone: onDone
            )
        }
        .background(Color.clear)
        .lightweightLiveWallpaperWhilePresented()
    }
}

/// Library destinations use a transparent full-screen presentation so the
/// system sheet dimmer cannot darken the game library. The invisible outer
/// surface still blocks and dismisses touch input; only the destination window
/// is visible.
private struct GameLibraryForegroundPanel<Content: View>: View {
    let maximumWidth: CGFloat
    let maximumHeight: CGFloat
    let isContentVisible: Bool
    let onDismiss: () -> Void
    private let content: Content

    init(
        maximumWidth: CGFloat,
        maximumHeight: CGFloat,
        isContentVisible: Bool = true,
        onDismiss: @escaping () -> Void,
        @ViewBuilder content: () -> Content
    ) {
        self.maximumWidth = maximumWidth
        self.maximumHeight = maximumHeight
        self.isContentVisible = isContentVisible
        self.onDismiss = onDismiss
        self.content = content()
    }

    var body: some View {
        GeometryReader { proxy in
            let width = min(maximumWidth, max(280, proxy.size.width - 32))
            let height = min(maximumHeight, max(300, proxy.size.height - 32))

            ZStack {
                Color.clear
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture(perform: onDismiss)

                content
                    .frame(width: width, height: height)
                    .clipShape(
                        RoundedRectangle(cornerRadius: 26, style: .continuous)
                    )
                    .overlay {
                        RoundedRectangle(
                            cornerRadius: 26,
                            style: .continuous
                        )
                        .stroke(Color.primary.opacity(0.12), lineWidth: 0.7)
                    }
                    .shadow(color: .black.opacity(0.24), radius: 22, y: 10)
                    // Keep the controls mounted while previewing. An invisible
                    // Slider can finish the same touch drag, and the controller
                    // graph retains its focused row.
                    .opacity(isContentVisible ? 1 : 0)
                    .animation(
                        .easeOut(duration: isContentVisible ? 0.18 : 0.12),
                        value: isContentVisible
                    )
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .ignoresSafeArea(.container, edges: .all)
    }
}

private struct GameLibraryViewOptionsPreview: Equatable {
    let title: String
    let value: String
}

private struct GameLibraryViewOptionsValues {
    let cardScale: Double
    let cardWidthScale: Double
    let cardHeightScale: Double
    let libraryVerticalPosition: Double
    let gapScale: Double
    let contentPaddingScale: Double
    let verticalPaddingScale: Double
    let textSpacingScale: Double
    let gameNameTextScale: Double
    let gameInfoTextScale: Double
    let cardCornerRadius: Double
    let coverCornerRadius: Double
    let coverShadowStrength: Double
    let coverOpacity: Double
    let bottomNavigationTabBarHeight: Double
    let bottomNavigationTabBarIconScale: Double
    let bottomNavigationTabBarLabelScale: Double
    let bottomNavigationTabBarClearance: Double
    let bottomNavigationTabBarShowsLabels: Bool
    let hideGamesScreenTitle: Bool
    let useGamesLogo: Bool
    let hideGameName: Bool
    let hideGameInfo: Bool
    let hideFavoriteButton: Bool
    let hideRegionFlag: Bool
    let singleLineGameNames: Bool
    let hideRunningIndicator: Bool
}

private struct GameLibraryViewOptionsPanel: View {
    let controllerInput: MenuControllerInputRouter?
    let orientationTitle: String
    @Binding var cardScale: Double
    @Binding var cardWidthScale: Double
    @Binding var cardHeightScale: Double
    @Binding var libraryVerticalPosition: Double
    @Binding var gapScale: Double
    @Binding var contentPaddingScale: Double
    @Binding var verticalPaddingScale: Double
    @Binding var textSpacingScale: Double
    @Binding var gameNameTextScale: Double
    @Binding var gameInfoTextScale: Double
    @Binding var cardCornerRadius: Double
    @Binding var coverCornerRadius: Double
    @Binding var coverShadowStrength: Double
    @Binding var coverOpacity: Double
    @Binding var bottomNavigationTabBarHeight: Double
    @Binding var bottomNavigationTabBarIconScale: Double
    @Binding var bottomNavigationTabBarLabelScale: Double
    @Binding var bottomNavigationTabBarClearance: Double
    @Binding var bottomNavigationTabBarShowsLabels: Bool
    @Binding var hideGamesScreenTitle: Bool
    @Binding var useGamesLogo: Bool
    let logoAvailable: Bool
    @Binding var hideGameName: Bool
    @Binding var hideGameInfo: Bool
    @Binding var hideFavoriteButton: Bool
    @Binding var hideRegionFlag: Bool
    @Binding var singleLineGameNames: Bool
    @Binding var hideRunningIndicator: Bool
    let onPreviewChanged: (GameLibraryViewOptionsPreview?) -> Void
    let onReset: () -> Void
    let onCancel: () -> Void
    let onApply: () -> Void

    @Environment(\.uiAccentColour) private var accentColour
    @State private var settings = SettingsStore.shared
    @State private var previewEndTask: Task<Void, Never>?
    @State private var showsResetConfirmation = false
    @State private var resetFeedbackVisible = false
    @State private var resetFeedbackTask: Task<Void, Never>?

    private let targetOrder = [
        "library.options.hide-title",
        "library.options.use-logo",
        "library.options.hide-name",
        "library.options.hide-info",
        "library.options.hide-favorite",
        "library.options.hide-region",
        "library.options.single-line",
        "library.options.hide-running",
        "library.options.card-size",
        "library.options.card-width",
        "library.options.card-height",
        "library.options.vertical-position",
        "library.options.spacing",
        "library.options.glass-padding",
        "library.options.glass-padding-height",
        "library.options.content-spacing",
        "library.options.card-corners",
        "library.options.cover-corners",
        "library.options.cover-shadow",
        "library.options.cover-opacity",
        "library.options.game-name-size",
        "library.options.game-info-size",
        "library.options.tab-bar-height",
        "library.options.tab-bar-icon-size",
        "library.options.tab-bar-label-size",
        "library.options.tab-bar-clearance",
        "library.options.tab-bar-labels",
        "library.options.reset",
        "library.options.cancel",
        "library.options.apply",
    ]

    var body: some View {
        ZStack {
            Color.clear
                .glassSurface(
                    clear: false,
                    cornerRadius: 26
                )

            VStack(spacing: 10) {
                HStack(spacing: 12) {
                    Text(settings.localized("Game Library View Options"))
                        .font(.headline)
                        .lineLimit(1)

                    Spacer(minLength: 8)

                    Text(orientationTitle)
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 12)
                        .frame(height: 30)
                        .glassSurface(
                            tint: accentColour.opacity(0.10),
                            clear: false,
                            cornerRadius: 15
                        )
                }
                .padding(.horizontal, 4)

                ScrollView {
                    // Keep this small, modal form mounted as one semantic
                    // graph. Recycling the first Visible Content row while
                    // seeking back into Bottom Navigation stranded Up on a
                    // pending offscreen target.
                    VStack(spacing: 10) {
                        glassSection(settings.localized("Visible Content")) {
                        optionToggle(
                            settings.localized("Hide Games Screen Title"),
                            id: "library.options.hide-title",
                            isOn: $hideGamesScreenTitle
                        )
                        optionToggle(
                            settings.localized("Use Logo"),
                            id: "library.options.use-logo",
                            isOn: $useGamesLogo,
                            isEnabled: logoAvailable
                        )
                        optionToggle(
                            settings.localized("Hide Game Name"),
                            id: "library.options.hide-name",
                            isOn: $hideGameName
                        )
                        optionToggle(
                            settings.localized("Hide Game Info"),
                            id: "library.options.hide-info",
                            isOn: $hideGameInfo
                        )
                        optionToggle(
                            settings.localized("Hide Favorite Button"),
                            id: "library.options.hide-favorite",
                            isOn: $hideFavoriteButton
                        )
                        optionToggle(
                            settings.localized("Hide Region Flag"),
                            id: "library.options.hide-region",
                            isOn: $hideRegionFlag
                        )
                        optionToggle(
                            settings.localized("Single-Line Game Names"),
                            id: "library.options.single-line",
                            isOn: $singleLineGameNames
                        )
                        optionToggle(
                            settings.localized("Hide Running Indicator"),
                            id: "library.options.hide-running",
                            isOn: $hideRunningIndicator
                        )
                    }

                        glassSection(settings.localized("Card Layout")) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-size",
                            title: settings.localized("Card Size"),
                            value: $cardScale,
                            range: 0.72...1.32,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-width",
                            title: settings.localized("Card Width"),
                            value: $cardWidthScale,
                            range: 0.70...1.40,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-height",
                            title: settings.localized("Card Height"),
                            value: $cardHeightScale,
                            range: 0.70...1.40,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.vertical-position",
                            title: settings.localized(
                                "Game Library Vertical Position"
                            ),
                            value: $libraryVerticalPosition,
                            range: -160...160,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.spacing",
                            title: settings.localized("Card Spacing"),
                            value: $gapScale,
                            range: 0.65...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.glass-padding",
                            title: settings.localized("Glass Padding Width"),
                            value: $contentPaddingScale,
                            range: 0...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.glass-padding-height",
                            title: settings.localized("Glass Padding Height"),
                            value: $verticalPaddingScale,
                            range: 0...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.content-spacing",
                            title: settings.localized("Content Spacing"),
                            value: $textSpacingScale,
                            range: 0...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-corners",
                            title: settings.localized("Card Corners"),
                            value: $cardCornerRadius,
                            range: 6...34,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.cover-corners",
                            title: settings.localized("Cover Corners"),
                            value: $coverCornerRadius,
                            range: 0...28,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                    }

                        glassSection(settings.localized("Cover Appearance")) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.cover-shadow",
                            title: settings.localized("Cover Shadow"),
                            value: $coverShadowStrength,
                            range: 0...2,
                            step: 0.02,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.cover-opacity",
                            title: settings.localized("Cover Opacity"),
                            value: $coverOpacity,
                            range: 0.35...1,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                    }

                        glassSection(settings.localized("Text Size")) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.game-name-size",
                            title: settings.localized("Game Name Size"),
                            value: $gameNameTextScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.game-info-size",
                            title: settings.localized("Game Info Size"),
                            value: $gameInfoTextScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                    }

                        glassSection(
                            settings.localized("Bottom Navigation Tab Bar")
                        ) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-height",
                            title: settings.localized(
                                "Bottom Navigation Tab Bar Height"
                            ),
                            value: $bottomNavigationTabBarHeight,
                            range: 42...100,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-icon-size",
                            title: settings.localized("Tab Bar Icon Size"),
                            value: $bottomNavigationTabBarIconScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-label-size",
                            title: settings.localized("Tab Bar Label Size"),
                            value: $bottomNavigationTabBarLabelScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-clearance",
                            title: settings.localized("Tab Bar Bottom Clearance"),
                            value: $bottomNavigationTabBarClearance,
                            range: -32...32,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        optionToggle(
                            settings.localized("Show Tab Labels"),
                            id: "library.options.tab-bar-labels",
                            isOn: $bottomNavigationTabBarShowsLabels
                        )
                    }
                    }
                    .padding(.vertical, 2)
                }
                .scrollIndicators(.visible)

                actionBar
            }
            .padding(14)
        }
        .contextMenuPanelTextAppearance()
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            isActive: true,
            scopeKey: "game-library.view-options.\(orientationTitle)",
            priority: 420,
            orbStyle: .plain,
            onBack: {
                onApply()
                return true
            },
            usesNativeFocusEngine: false,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            scrollAnimationDuration: 0.16,
            focusTopAlignmentMargin: 72,
            focusBottomAlignmentMargin: 72,
            preferredInitialFocusLabel: targetOrder.first,
            declaredTargetOrder: targetOrder
        )
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 430,
                isEnabled: true,
                searchesNearbyScrollViews: true
            )
            .allowsHitTesting(false)
        }
        .onDisappear {
            previewEndTask?.cancel()
            previewEndTask = nil
            resetFeedbackTask?.cancel()
            resetFeedbackTask = nil
            onPreviewChanged(nil)
        }
        .confirmationDialog(
            settings.localized("Reset Game Library View Options?"),
            isPresented: $showsResetConfirmation,
            titleVisibility: .visible
        ) {
            Button(settings.localized("Reset"), role: .destructive) {
                performReset()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(
                settings.localized(
                    "This restores the current orientation's card layout and visible content options to their defaults."
                )
            )
        }
    }

    private func glassSection<Content: View>(
        _ title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(
            clear: false,
            cornerRadius: 18
        )
    }

    private var actionBar: some View {
        HStack(spacing: 10) {
            actionButton(
                settings.localized(
                    resetFeedbackVisible ? "Restored" : "Reset"
                ),
                systemImage: resetFeedbackVisible
                    ? "checkmark.circle.fill"
                    : "arrow.counterclockwise",
                id: "library.options.reset",
                action: { showsResetConfirmation = true }
            )
            actionButton(
                settings.localized("Cancel"),
                systemImage: "xmark",
                id: "library.options.cancel",
                action: onCancel
            )
            actionButton(
                settings.localized("Apply"),
                systemImage: "checkmark",
                id: "library.options.apply",
                isProminent: true,
                action: onApply
            )
        }
    }

    private func actionButton(
        _ title: String,
        systemImage: String,
        id: String,
        isProminent: Bool = false,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
                .frame(maxWidth: .infinity)
                .frame(height: 38)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(isProminent ? accentColour : Color.primary)
        .glassSurface(
            tint: isProminent ? accentColour.opacity(0.16) : nil,
            interactive: true,
            clear: false,
            cornerRadius: 19
        )
        .controllerAccessibilityActionTarget(
            id: id,
            label: title,
            action: action
        )
    }

    private func updateSliderPreview(
        _ title: String,
        _ value: String,
        _ interactionContinues: Bool
    ) {
        previewEndTask?.cancel()
        previewEndTask = nil
        withAnimation(.easeOut(duration: 0.12)) {
            onPreviewChanged(
                GameLibraryViewOptionsPreview(title: title, value: value)
            )
        }

        // Hiding the popup can prevent SwiftUI's Slider from delivering its
        // final editing-ended callback. Every movement therefore rearms the
        // same inactivity deadline; the popup reliably returns one second
        // after either touch or controller adjustment stops.
        previewEndTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
            withAnimation(.easeOut(duration: 0.18)) {
                onPreviewChanged(nil)
            }
            previewEndTask = nil
        }
    }

    @MainActor
    private func performReset() {
        onReset()
        resetFeedbackTask?.cancel()
        withAnimation(.easeOut(duration: 0.18)) {
            resetFeedbackVisible = true
        }
        announceSettingsResetSuccess(
            settings.localized("Game Library View Options Restored"),
            controllerInput: controllerInput
        )
        resetFeedbackTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(2.4))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                resetFeedbackVisible = false
            }
            resetFeedbackTask = nil
        }
    }

    private func optionToggle(
        _ title: String,
        id: String,
        isOn: Binding<Bool>,
        isEnabled: Bool = true
    ) -> some View {
        let previewingBinding = Binding(
            get: { isOn.wrappedValue },
            set: { newValue in
                guard isOn.wrappedValue != newValue else { return }
                isOn.wrappedValue = newValue
                updateSliderPreview(
                    title,
                    settings.localized(newValue ? "On" : "Off"),
                    false
                )
            }
        )
        return Toggle(title, isOn: previewingBinding)
            .controllerAccessibilityToggleTarget(
                id: id,
                label: title,
                isOn: previewingBinding
            )
            .disabled(!isEnabled)
    }
}

private struct ExperimentalLibrarySliderRow: View {
    let id: String
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let format: (Double) -> String
    let onPreview: (_ title: String, _ value: String,
                    _ interactionContinues: Bool) -> Void

    @State private var isTouchEditing = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title)
                Spacer()
                Text(format(value))
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(
                value: previewingValue,
                in: range,
                step: step,
                onEditingChanged: touchEditingChanged
            )
        }
        .controllerAccessibilityAdjustableTarget(
            id: id,
            label: title,
            value: format(value),
            onActivate: {},
            onIncrement: { adjust(by: step) },
            onDecrement: { adjust(by: -step) }
        )
    }

    private func adjust(by delta: Double) {
        let next = min(max(value + delta, range.lowerBound), range.upperBound)
        guard next != value else { return }
        value = next
        onPreview(title, format(next), false)
    }

    private var previewingValue: Binding<Double> {
        Binding(
            get: { value },
            set: { next in
                value = next
                guard isTouchEditing else { return }
                onPreview(title, format(next), true)
            }
        )
    }

    private func touchEditingChanged(_ editing: Bool) {
        isTouchEditing = editing
        onPreview(title, format(value), editing)
    }
}

private struct GameInfoPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    let game: ISOEntry

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 14) {
                        CoverThumbnailView(
                            gameName: game.name,
                            coverURL: game.coverURL,
                            coverSignature: game.coverSignature,
                            width: 84,
                            height: 126
                        )

                        VStack(alignment: .leading, spacing: 6) {
                            Text(game.displayName)
                                .font(.headline)
                            Text(game.name)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section(settings.localized("Disc")) {
                    LabeledContent(settings.localized("Region")) {
                        Text(regionDisplay)
                    }
                    LabeledContent(settings.localized("Serial")) {
                        Text(metadataValue("serial"))
                            .textSelection(.enabled)
                    }
                    LabeledContent(settings.localized("CRC")) {
                        Text(metadataValue("crc"))
                            .textSelection(.enabled)
                    }
                    LabeledContent(settings.localized("Format")) {
                        Text(game.name.pathExtensionLabel)
                    }
                    LabeledContent(settings.localized("Size")) {
                        Text(game.sizeLabel)
                    }
                }

                Section(settings.localized("File")) {
                    Text(game.fileURL?.path ?? settings.localized("File path unavailable"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
            }
            .navigationTitle(settings.localized("Game Info"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
        }
    }

    private var regionDisplay: String {
        let region = metadataValue("region")
        let flag = RegionFlag.emoji(for: region)
        return flag.isEmpty ? region : "\(flag) \(region)"
    }

    private func metadataValue(_ key: String) -> String {
        let value = game.metadata[key]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? settings.localized("Unknown") : value
    }
}

private struct DiscLinkPicker: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    let discs: [ISOEntry]
    let onSelect: (ISOEntry?) -> Void

    var body: some View {
        NavigationStack {
            List {
                if discs.isEmpty {
                    Text(settings.localized("No disc images found. Import an ISO first, then link it here."))
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(discs) { disc in
                        Button {
                            onSelect(disc)
                            dismiss()
                        } label: {
                            Text(disc.name).lineLimit(1)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            .navigationTitle(settings.localized("Disc Path"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(settings.localized("Cancel")) { dismiss() }
                }
            }
        }
    }
}

private extension String {
    var pathExtensionLabel: String {
        let ext = (self as NSString).pathExtension.uppercased()
        return ext.isEmpty ? "FILE" : ext
    }
}
