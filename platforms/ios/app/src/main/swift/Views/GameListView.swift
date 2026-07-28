// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import UIKit

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

struct ISOEntry: Identifiable, Equatable {
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
        sourceName: String? = nil
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
        displayName = URL(fileURLWithPath: name)
            .deletingPathExtension()
            .lastPathComponent

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
        in registry: GameplayLaunchCardRegistry
    ) -> some View {
        overlay {
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

@MainActor
private final class GameLibrarySnapshot {
	struct CachedGameMetadata: Codable, Sendable {
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
	private let persistenceQueue = DispatchQueue(
		label: "com.armsx2.ios.game-library-metadata",
		qos: .utility
	)

	private static var persistenceURL: URL? {
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
			?? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
		return base?.appendingPathComponent("LibraryMetadataCache.json")
	}

	private init() {
		guard let url = Self.persistenceURL,
		      let data = try? Data(contentsOf: url) else { return }
		if let decoded = try? JSONDecoder().decode([String: CachedGameMetadata].self, from: data) {
			metadataCache = decoded
		}
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

/// Animates only the active game's already-decoded cover. Randomized radius,
/// intensity, and direction avoid the mechanical appearance of a fixed pulse
/// without introducing a library-wide timeline or invalidating other cards.
private struct RunningCoverNeonGlowModifier: ViewModifier {
    let isRunning: Bool
    @State private var phase = RunningCoverGlowPhase.initial

    func body(content: Content) -> some View {
        content
            .shadow(
                color: Color(red: 0.18, green: 1.0, blue: 0.28)
                    .opacity(isRunning ? phase.opacity : 0),
                radius: isRunning ? phase.radius : 0,
                x: isRunning ? phase.x : 0,
                y: isRunning ? phase.y : 0
            )
            .shadow(
                color: Color(red: 0.48, green: 1.0, blue: 0.58)
                    .opacity(isRunning ? phase.opacity * 0.76 : 0),
                radius: isRunning ? phase.radius * 0.68 : 0,
                x: isRunning ? -phase.x * 0.7 : 0,
                y: isRunning ? -phase.y * 0.7 : 0
            )
            .animation(.easeOut(duration: 0.55), value: isRunning)
            .task(id: isRunning) {
                guard isRunning else {
                    phase = .initial
                    return
                }

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
        modifier(
            GameLibraryScrollPhaseModifier(activity: activity)
        )
    }
}

private struct RunningStatusText: View {
    let isStopping: Bool
    let runningLabel: String
    let gameTitle: String
    let stoppingLabel: String
    let stoppingFont: Font
    var centered = false

    @ViewBuilder
    var body: some View {
        if isStopping {
            Text(stoppingLabel)
                .font(stoppingFont.bold())
                .foregroundStyle(.red)
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
                    .foregroundStyle(.secondary)
                Text(gameTitle)
                    .font(.subheadline.weight(.semibold))
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

struct GameListView: View {
    let embeddedInMenuNavigation: Bool
    let ownsEmbeddedMenuToolbar: Bool

    init(
        embeddedInMenuNavigation: Bool = false,
        ownsEmbeddedMenuToolbar: Bool = true
    ) {
        self.embeddedInMenuNavigation = embeddedInMenuNavigation
        self.ownsEmbeddedMenuToolbar = ownsEmbeddedMenuToolbar
    }

    @State private var games: [ISOEntry] = []
    @State private var appState = AppState.shared
	@State private var settings = SettingsStore.shared
	@State private var fileImporter = FileImportHandler.shared
	@State private var coverStore = CoverStore.shared
	@State private var externalLibrary = ExternalGameLibrary.shared
	@State private var externalCoverAutoDownloadAttemptedIDs = Set<String>()
	@State private var coverWorkTask: Task<Void, Never>?
	@State private var showGameImporter = false
    @State private var showCoverImporter = false
    @State private var showCoverPhotoPicker = false
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var showCoverTemplateEditor = false
    @State private var showGameReplacementAlert = false
    @State private var coverTemplateDraft = CoverStore.defaultCoverURLTemplate
    @State private var pendingGameName: String = ""
    @State private var pendingGameplayLaunchTransition: GameplayLaunchTransition?
    @State private var pendingGameImportURLs: [URL] = []
    @State private var existingGameImportFileNames: [String] = []
    @State private var pendingCoverGameName: String?
    @State private var pendingCoverPhotoGameName: String?
    @State private var selectedCoverPhotoItem: PhotosPickerItem?
    @State private var gameInfoTarget: ISOEntry?
    @State private var gameSettingsTarget: ISOEntry?
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
    @AppStorage("ARMSX2iOSGameLibraryLayout") private var libraryLayout = "grid"
    @AppStorage("ARMSX2iOSLandscapeCoverFlowEnabled") private var landscapeCoverFlowEnabled = true
#if DEBUG
    @State private var ranBackgroundValidation = false
#endif
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
        embeddedInMenuNavigation
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
        let cardPadding: CGFloat
        let cornerRadius: CGFloat
        let favoritePadding: CGFloat
        let favoriteInset: CGFloat
        let itemSpacing: CGFloat
        let horizontalPadding: CGFloat
        let verticalPadding: CGFloat
        let statusWidth: CGFloat
        let statusHeight: CGFloat
        let statusIconSize: CGFloat

        init(containerSize: CGSize) {
            isCompact = containerSize.height < 360
            coverWidth = isCompact ? 104 : 150
            coverHeight = isCompact ? 156 : 225
            textWidth = isCompact ? 134 : 164
            cardSpacing = isCompact ? 8 : 12
            cardPadding = isCompact ? 8 : 12
            cornerRadius = isCompact ? 18 : 24
            favoritePadding = isCompact ? 6 : 8
            favoriteInset = isCompact ? 5 : 8
            itemSpacing = isCompact ? 14 : 20
            horizontalPadding = isCompact ? 20 : 32
            verticalPadding = isCompact ? 10 : 18
            statusWidth = isCompact ? 138 : 166
            statusHeight = isCompact ? 190 : 276
            statusIconSize = isCompact ? 36 : 48
        }
    }

    var body: some View {
        OptionalMenuNavigationStack(embedded: embeddedInMenuNavigation) {
            ZStack {
                if hasCustomBackground {
                    MenuBackgroundLayer(isActive: menuTabIsActive)
                }

                GeometryReader { geo in
                    Group {
                        if games.isEmpty && displayedRunningGameName == nil {
                            emptyLibrary(containerSize: geo.size)
                        } else if libraryLayout == "grid" && geo.size.width > geo.size.height && landscapeCoverFlowEnabled {
                            coverFlowLibrary(containerSize: geo.size)
                        } else if libraryLayout == "grid" {
                            gridLibrary
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
                }
            }
            .stableMenuContentGlassContainer()
            .clearNavigationContainerBackground()
            .optionalMenuNavigationChrome(
                title: settings.localized("Games"),
                backgroundHidden: hasCustomBackground,
                embedded: embeddedInMenuNavigation
            )
            .toolbar {
                if !embeddedInMenuNavigation || ownsEmbeddedMenuToolbar {
                ToolbarItem(id: "menu.import", placement: .topBarTrailing) {
                    Button {
                        showGameImporter = true
                    } label: {
                        Image(systemName: "plus")
                            .foregroundStyle(.blue)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Import Games"))
                }
                ToolbarItem(id: "menu.layout", placement: .topBarTrailing) {
                    Menu {
                        Button {
                            libraryLayout = libraryLayout == "grid" ? "list" : "grid"
                        } label: {
                            Label(
                                settings.localized(libraryLayout == "grid" ? "Show List" : "Show Grid"),
                                systemImage: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2"
                            )
                        }

                        if libraryLayout == "grid" {
                            Toggle(isOn: $landscapeCoverFlowEnabled) {
                                Label(settings.localized("Landscape Cover Flow"), systemImage: "rectangle.landscape.rotate")
                            }
                        }
                    } label: {
                        Image(systemName: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2")
                            .foregroundStyle(.blue)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Library Layout"))
                }
                ToolbarItem(id: "menu.covers", placement: .topBarTrailing) {
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
                    } label: {
                        Image(systemName: coverStore.isDownloadingCovers ? "icloud.and.arrow.down" : "photo.stack")
                            .foregroundStyle(.blue)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Covers"))
                }
                ToolbarItem(id: "menu.refresh", placement: .topBarTrailing) {
                    Button { loadGames(forceMetadataRefresh: true) } label: {
                        Image(systemName: "arrow.clockwise")
                            .foregroundStyle(.blue)
                    }
                    .buttonStyle(.plain)
                    .disabled(gameLibraryActivity.isLoading)
                    .accessibilityLabel(settings.localized("Refresh"))
                }
                ToolbarItem(id: "menu.bootBIOS", placement: .topBarLeading) {
                    Button {
                        if appState.runningGameName == "BIOS" {
                            appState.returnToGame()
                        } else if appState.runningGameName != nil {
                            pendingGameName = ""
                            showRestartAlert = true
                        } else {
                            appState.bootBIOSOnly()
                        }
                    } label: {
                        Text(settings.localized("Boot BIOS"))
                            .font(.callout.weight(.semibold))
                            .foregroundStyle(.blue)
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                            .padding(.horizontal, 8)
                            .frame(minHeight: 36)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Boot BIOS"))
                }
                }
            }
            .alert(settings.localized("Cover Result"), isPresented: $coverStore.showCoverAlert) {
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
            .alert(settings.localized("Restart VM?"), isPresented: $showRestartAlert) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingGameplayLaunchTransition = nil
                }
                Button(settings.localized("Restart"), role: .destructive) {
                    if pendingGameName.isEmpty {
                        pendingGameplayLaunchTransition = nil
                        appState.shutdownAndBootBIOS()
                    } else {
                        let transition = pendingGameplayLaunchTransition
                        pendingGameplayLaunchTransition = nil
                        appState.shutdownAndBoot(
                            isoName: pendingGameName,
                            launchTransition: transition
                        )
                    }
                }
			} message: {
				let target = pendingGameName.isEmpty ? "Boot BIOS" : (pendingGameName as NSString).lastPathComponent
				Text("\(settings.localized("VM is currently running."))\n\(settings.localized("Shut down and start")) \(settings.localized(target))?")
			}
            .alert(
                settings.localized("Delete Game Data?"),
                isPresented: Binding(
                    get: { pendingDeleteDataGame != nil },
                    set: { if !$0 { pendingDeleteDataGame = nil } }
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
                    get: { pendingDeleteGame != nil },
                    set: { if !$0 { pendingDeleteGame = nil } }
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
                    get: { gameActionMessage != nil },
                    set: { if !$0 { gameActionMessage = nil } }
                )
            ) {
                Button(settings.localized("OK")) {
                    gameActionMessage = nil
                }
            } message: {
                Text(gameActionMessage ?? "")
            }
            .alert(settings.localized("Replace existing files?"), isPresented: $showGameReplacementAlert) {
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
            .alert(settings.localized("Background image could not be loaded."), isPresented: $showBackgroundAssetError) {
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
            .sheet(item: $gameInfoTarget) { game in
                GameInfoPanel(game: game)
                    .presentationDetents([.medium, .large])
            }
            .sheet(item: $gameSettingsTarget) { game in
                PerGameSettingsPanel(game: game)
                    .presentationDetents([.large])
                    .presentationDragIndicator(.visible)
                    .presentationBackground(.clear)
                    .presentationCornerRadius(34)
            }
            .sheet(item: $discLinkTarget) { game in
                DiscLinkPicker(discs: games.filter { !$0.isELF && $0.id != game.id }) { selected in
                    ARMSX2Bridge.setLinkedDiscPath(selected?.fileURL?.path ?? selected?.bootName, forELF: game.bootName)
                    loadGames()
                }
                .presentationDetents([.medium, .large])
            }
            .sheet(item: $cheatsManagerTarget) { game in
                CheatsPatchesManagerView(
                    isoName: game.bootName,
                    gameTitle: game.name,
                    launchContext: .library
                )
                    .presentationDetents([.large])
                    .presentationDragIndicator(.visible)
            }
			}
            .alert(settings.localized("Stop Emulation?"), isPresented: $showStopAlert) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    showStopAlert = false
                }
                Button(settings.localized("Stop"), role: .destructive) {
                    scheduleStopRunningGame()
                }
            } message: {
                Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
            }
        .onAppear {
            GameLibraryRuntimeResources.activateForMenu()
            restoreCachedGamesIfNeeded()
            scheduleLibraryReload(
                after: .milliseconds(80),
                autoDownloadExternalCovers: true
            )
            if settings.sanitizeBackgroundAssets() {
                showBackgroundAssetError = true
            }
#if DEBUG
            if !ranBackgroundValidation {
                ranBackgroundValidation = true
                BackgroundValidation.run()
            }
#endif
        }
        .onReceive(NotificationCenter.default.publisher(for: ExternalGameLibrary.didChangeNotification)) { _ in
            scheduleLibraryReload(
                after: .milliseconds(120),
                autoDownloadExternalCovers: true
            )
        }
        .onReceive(NotificationCenter.default.publisher(for: InitialContentBootstrap.didChangeNotification)) { _ in
            scheduleLibraryReload(
                after: .milliseconds(120),
                autoDownloadExternalCovers: false
            )
        }
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("ARMSX2iOSReturnToMenu"))) { _ in
            restoreCachedGamesIfNeeded()
            scheduleLibraryReload(
                after: .milliseconds(80),
                autoDownloadExternalCovers: false
            )
        }
        .onDisappear {
            guard case .playing = appState.currentScreen else { return }
            releaseLibraryResourcesForGameplay()
        }
    }

    private var listLibrary: some View {
        List {
            if showsPageOwnedLargeTitle {
                EmbeddedMenuLargeTitle(title: settings.localized("Games"))
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
            }
            ForEach(games) { game in
                gameRow(game)
                    .menuBackgroundListRow(hasCustomBackground)
            }
        }
        .contentMargins(.top, 0, for: .scrollContent)
        .scrollContentBackground(hasCustomBackground ? .hidden : .automatic)
        .scrollDisabled(false)
        .scrollBounceBehavior(.always)
        .trackGameLibraryScrollPhase(gameLibraryActivity)
        .transaction { transaction in
            if transaction.isContinuous || gameLibraryActivity.isScrolling {
                transaction.animation = nil
            }
        }
    }

    private var gridLibrary: some View {
        ScrollView {
            LazyVStack(spacing: 16) {
                if showsPageOwnedLargeTitle {
                    EmbeddedMenuLargeTitle(title: settings.localized("Games"))
                }

                if games.isEmpty && displayedRunningGameName == nil {
                    emptyState
                        .frame(minHeight: 300)
                }

                if let gameName = displayedRunningGameName {
                    vmStatusCard(gameName: gameName)
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

                LazyVGrid(columns: [GridItem(.adaptive(minimum: 142), spacing: 14, alignment: .top)], spacing: 18) {
                    ForEach(games) { game in
                        gameGridCard(game)
                    }
                }
                .padding(.horizontal)
                .padding(.bottom, 20)
            }
            .padding(.top, showsPageOwnedLargeTitle ? 0 : 12)
        }
        .contentMargins(.top, 0, for: .scrollContent)
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
    }

    private func emptyLibrary(containerSize: CGSize) -> some View {
        ScrollView {
            VStack(spacing: 0) {
                if showsPageOwnedLargeTitle {
                    EmbeddedMenuLargeTitle(title: settings.localized("Games"))
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
        let metrics = CoverFlowMetrics(containerSize: containerSize)
        let availableHeight = max(0, containerSize.height - 12)

        return ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(alignment: .center, spacing: metrics.itemSpacing) {
                if games.isEmpty && displayedRunningGameName == nil {
                    emptyState
                        .frame(
                            width: max(
                                0,
                                containerSize.width - (metrics.horizontalPadding * 2)
                            ),
                            height: max(
                                0,
                                availableHeight - (metrics.verticalPadding * 2)
                            )
                        )
                }

                if let gameName = displayedRunningGameName {
                    vmStatusCoverCard(gameName: gameName, metrics: metrics)
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
                    coverFlowCard(game, metrics: metrics)
                }
            }
            .frame(
                minWidth: max(0, containerSize.width - (metrics.horizontalPadding * 2)),
                minHeight: max(0, availableHeight - (metrics.verticalPadding * 2)),
                alignment: .center
            )
            .padding(.horizontal, metrics.horizontalPadding)
            .padding(.vertical, metrics.verticalPadding)
        }
        .scrollDisabled(false)
        .scrollBounceBehavior(.always, axes: .horizontal)
        .trackGameLibraryScrollPhase(gameLibraryActivity)
        .transaction { transaction in
            if transaction.isContinuous || gameLibraryActivity.isScrolling {
                transaction.animation = nil
            }
        }
        .frame(height: availableHeight)
        .frame(maxHeight: .infinity, alignment: .top)
        // The persistent bottom bar shortens the layout proposal. Offset by
        // half the navigation/top-bar imbalance to keep the library visually
        // centered in the complete landscape display.
        .offset(y: 30)
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
                            .foregroundStyle(isStopping ? .red : .green)
                            .font(.title)
                            .contentTransition(.symbolEffect(.replace))

                        RunningStatusText(
                            isStopping: isStopping,
                            runningLabel: settings.localized("Now Running"),
                            gameTitle: displayTitle(forRunningName: gameName),
                            stoppingLabel: settings.localized("Stopping..."),
                            stoppingFont: .title2
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
                .animation(
                    .smooth(duration: 0.42, extraBounce: 0.02),
                    value: isStopping
                )
            }
            .disabled(isStopping)
            .tint(.primary)
            .menuBackgroundListRow(hasCustomBackground)
            .compositingGroup()
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
            }
        }
    }

    private func gameRow(_ game: ISOEntry) -> some View {
        let running = isRunning(game)
        return Button {
            open(game)
        } label: {
            HStack(spacing: 12) {
                coverThumbnail(for: game, running: running)

                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Text(game.displayName)
                            .font(.body)
                            .fontWeight(.medium)
                            .foregroundStyle(.primary)
                            .lineLimit(2)
                            .multilineTextAlignment(.leading)
                            .fixedSize(horizontal: false, vertical: true)
                            .layoutPriority(1)
                        if running {
                            Image(systemName: "circle.fill")
                                .font(.system(size: 8))
                                .foregroundStyle(.green)
                                .accessibilityLabel(settings.localized("Running"))
                                .fixedSize()
                        }
                    }
                    HStack(spacing: 8) {
                        Text(game.sizeLabel)
                        Text(game.name.pathExtensionLabel)
                        if game.isExternal {
                            Label(settings.localized("External"), systemImage: "externaldrive")
                        }
                        if let flag = game.regionFlag {
                            Text(flag).accessibilityLabel(settings.localized("Region"))
                        }
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .layoutPriority(1)
                Spacer()

                favoriteButton(
                    for: game,
                    foreground: game.isFavorite ? .yellow : .gray
                )

                Image(systemName: running ? "play.fill" : "chevron.right")
                    .foregroundStyle(running ? .green : .secondary)
                    .font(.caption)
                    .accessibilityHidden(true)
            }
        }
        .buttonStyle(.plain)
        .foregroundStyle(.primary)
        .contextMenu {
            gameContextMenu(for: game)
        } preview: {
            gameContextMenuPreview(for: game)
        }
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry
        )
    }

    private func gameGridCard(_ game: ISOEntry) -> some View {
        let running = isRunning(game)
        return Button {
            open(game)
        } label: {
            ZStack(alignment: .topTrailing) {
                VStack(alignment: .center, spacing: 10) {
                    coverThumbnail(for: game, width: 126, height: 189, running: running)
                        .frame(maxWidth: .infinity)

                    VStack(alignment: .center, spacing: 4) {
                        HStack(alignment: .firstTextBaseline, spacing: 5) {
                            Text(game.displayName)
                                .font(.subheadline.weight(.semibold))
                                .foregroundStyle(.primary)
                                .lineLimit(2)
                                .multilineTextAlignment(.center)
                                .fixedSize(horizontal: false, vertical: true)
                                .frame(maxWidth: .infinity, alignment: .center)
                                .layoutPriority(1)
                            if running {
                                Image(systemName: "circle.fill")
                                    .font(.system(size: 7))
                                    .foregroundStyle(.green)
                                    .accessibilityLabel(settings.localized("Running"))
                                    .fixedSize()
                            }
                        }
                        .frame(minHeight: 38, alignment: .top)
                        HStack(spacing: 6) {
                            Text(game.name.pathExtensionLabel)
                            Text(game.sizeLabel)
                            if game.isExternal {
                                Image(systemName: "externaldrive")
                                    .accessibilityLabel(settings.localized("External"))
                            }
                            if let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(settings.localized("Region"))
                            }
                        }
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .center)
                    }
                    .frame(maxWidth: .infinity, alignment: .center)
                }
                .padding(12)
                .frame(maxWidth: .infinity, minHeight: 268, alignment: .top)

                favoriteButton(
                    for: game,
                    font: .callout.weight(.semibold),
                    foreground: game.isFavorite ? .yellow : .white.opacity(0.86),
                    backgroundOpacity: 0.36,
                    padding: 6
                )
                .padding(12)
                .zIndex(2)
            }
            .glassSurface(clear: true, cornerRadius: 18)
        }
        .buttonStyle(.plain)
        .contextMenu {
            gameContextMenu(for: game)
        } preview: {
            gameContextMenuPreview(for: game)
        }
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry
        )
    }

    private func coverFlowCard(_ game: ISOEntry, metrics: CoverFlowMetrics) -> some View {
        let running = isRunning(game)
        return Button {
            open(game)
        } label: {
            ZStack(alignment: .topTrailing) {
                VStack(spacing: metrics.cardSpacing) {
                    coverThumbnail(
                        for: game,
                        width: metrics.coverWidth,
                        height: metrics.coverHeight,
                        running: running
                    )
                    .shadow(color: running ? .clear : .black.opacity(0.28), radius: 18, y: 10)

                    VStack(spacing: 4) {
                        Text(game.displayName)
                            .font((metrics.isCompact ? Font.subheadline : Font.headline).weight(.semibold))
                            .multilineTextAlignment(.center)
                            .lineLimit(2)
                            .fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .frame(minHeight: metrics.isCompact ? 38 : 46, alignment: .top)
                        HStack(spacing: 4) {
                            Text(game.isExternal ? "\(game.name.pathExtensionLabel)  \(game.sizeLabel)  \(settings.localized("External"))" : "\(game.name.pathExtensionLabel)  \(game.sizeLabel)")
                            if let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(settings.localized("Region"))
                            }
                        }
                        .font(metrics.isCompact ? .caption2 : .caption)
                        .foregroundStyle(.secondary)
                    }
                    .frame(width: metrics.textWidth)
                }
                .padding(metrics.cardPadding)

                favoriteButton(
                    for: game,
                    font: (metrics.isCompact ? Font.subheadline : Font.headline).weight(.semibold),
                    foreground: game.isFavorite ? .yellow : .white.opacity(0.88),
                    backgroundOpacity: 0.48,
                    padding: metrics.favoritePadding
                )
                .padding(metrics.cardPadding + metrics.favoriteInset)
                .zIndex(2)
            }
            .glassSurface(clear: true, cornerRadius: metrics.cornerRadius)
        }
        .buttonStyle(.plain)
        .contextMenu {
            gameContextMenu(for: game)
        } preview: {
            gameContextMenuPreview(for: game)
        }
        .registerGameplayLaunchCard(
            for: game.id,
            in: gameplayLaunchCardRegistry
        )
    }

    private func favoriteButton(
        for game: ISOEntry,
        font: Font = .body,
        foreground: Color,
        backgroundOpacity: Double? = nil,
        padding: CGFloat = 0
    ) -> some View {
        Button {
            toggleFavorite(game)
        } label: {
            Image(systemName: game.isFavorite ? "star.fill" : "star")
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
                .contentTransition(.symbolEffect)
                .symbolEffect(
                    .bounce,
                    value: favoriteAnimationValues[game.id, default: 0]
                )
        }
        .buttonStyle(.plain)
        .accessibilityLabel(
            game.isFavorite
                ? settings.localized("Remove from favorites")
                : settings.localized("Add to favorites")
        )
    }

	private func coverThumbnail(
		for game: ISOEntry,
		width: CGFloat = 58,
		height: CGFloat = 87,
		running: Bool
	) -> some View {
        CoverThumbnailView(
            gameName: game.name,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature,
            width: width,
            height: height
        )
        .modifier(
            RunningCoverNeonGlowModifier(
                isRunning: running && !nowRunningRemovalAnimationActive
            )
        )
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
                    .foregroundStyle(isStopping ? .red : .green)
                    .contentTransition(.symbolEffect(.replace))

                RunningStatusText(
                    isStopping: isStopping,
                    runningLabel: settings.localized("Now Running"),
                    gameTitle: displayTitle(forRunningName: gameName),
                    stoppingLabel: settings.localized("Stopping..."),
                    stoppingFont: .title2
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
                    Button(role: .destructive) {
                        showStopAlert = true
                    } label: {
                        Image(systemName: "stop.circle")
                    }
                    .buttonStyle(.bordered)
                }
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(
            maxWidth: .infinity,
            alignment: isStopping ? .center : .leading
        )
        .padding()
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
                    .foregroundStyle(isStopping ? .red : .green)
                    .contentTransition(.symbolEffect(.replace))

                RunningStatusText(
                    isStopping: isStopping,
                    runningLabel: settings.localized("Now Running"),
                    gameTitle: displayTitle(forRunningName: gameName),
                    stoppingLabel: settings.localized("Stopping..."),
                    stoppingFont: metrics.isCompact ? .headline : .title3,
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
                    Button(role: .destructive) {
                        showStopAlert = true
                    } label: {
                        Image(systemName: "stop.circle")
                    }
                    .buttonStyle(.bordered)
                }
                .controlSize(metrics.isCompact ? .small : .regular)
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(width: metrics.statusWidth, height: metrics.statusHeight)
        .padding(metrics.cardPadding)
        .animation(
            .smooth(duration: 0.42, extraBounce: 0.02),
            value: isStopping
        )
        .glassSurface(
            clear: true,
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
                gameSettingsTarget = game
            }
        } label: {
            Label(settings.localized("Per-Game Settings"), systemImage: "slider.horizontal.3")
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

	private func open(_ game: ISOEntry) {
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

		if appState.runningGameName != nil {
			pendingGameName = game.bootName
			pendingGameplayLaunchTransition = makeGameplayLaunchTransition(for: game)
			showRestartAlert = true
		} else {
            let transition = makeGameplayLaunchTransition(for: game)
			appState.bootGame(
                isoName: game.bootName,
                launchTransition: transition
            )
		}
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
            let metrics = CoverFlowMetrics(containerSize: window.bounds.size)
            style = .coverFlow
            coverSize = CGSize(
                width: metrics.coverWidth,
                height: metrics.coverHeight
            )
            cornerRadius = metrics.cornerRadius
        } else if libraryLayout == "grid" {
            style = .grid
            coverSize = CGSize(width: 126, height: 189)
            cornerRadius = 18
        } else {
            style = .list
            coverSize = CGSize(width: 58, height: 87)
            cornerRadius = 14
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
        let detailParts = [
            game.name.pathExtensionLabel,
            game.sizeLabel,
            game.isExternal ? settings.localized("External") : nil,
            game.regionFlag,
        ].compactMap { $0 }

        return GameplayLaunchTransition(
            sourceFrame: sourceFrame,
            cornerRadius: cornerRadius,
            style: style,
            gameName: game.name,
            title: game.displayName,
            detail: detailParts.joined(separator: "  "),
            coverImage: coverImage,
            coverSize: coverSize,
            isFavorite: game.isFavorite
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
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows)
            .first(where: { $0.isKeyWindow })
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
        VStack(spacing: 16) {
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
                showGameImporter = true
            } label: {
                Label(settings.localized("Import Games"), systemImage: "plus")
            }
            .buttonStyle(.borderedProminent)
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
        guard !gameLibraryActivity.isLoading else { return }
        gameLibraryActivity.isLoading = true
        defer { gameLibraryActivity.isLoading = false }

        let fm = FileManager.default
        let allowFullMetadata = appState.runningGameName == nil
        let existingGames = GameLibrarySnapshot.shared.existingEntries(
            merging: games
        )
        externalLibrary.reload()
        let rawEntries = ARMSX2Bridge.availableISOEntries()
        let coverLookupIndex = coverStore.makeLibraryCoverLookupIndex(
            gamePaths: rawEntries.compactMap { rawEntry in
                guard let path = rawEntry["path"] as? String else { return nil }
                return URL(fileURLWithPath: path)
            }
        )
        let loadedGames = rawEntries.compactMap { rawEntry -> ISOEntry? in
            guard let name = rawEntry["name"] as? String,
                  let path = rawEntry["path"] as? String else {
                return nil
            }
            let external = (rawEntry["external"] as? NSNumber)?.boolValue
                ?? (rawEntry["external"] as? Bool ?? false)
            let source = rawEntry["source"] as? String
            let bootName = external ? path : name
            let attrs = try? fm.attributesOfItem(atPath: path)
            let size = attrs?[.size] as? UInt64 ?? 0
            let modificationDate = attrs?[.modificationDate] as? Date
            let favorite = ARMSX2Bridge.isFavorite(bootName)
            let fileURL = URL(fileURLWithPath: path)
            let entryID = external ? path : fileURL.path
            let metadata: [String: String]

            if forceMetadataRefresh && allowFullMetadata {
                metadata = ARMSX2Bridge.gameMetadata(forISO: bootName)
                GameLibrarySnapshot.shared.storeMetadata(
                    metadata,
                    modificationDate: modificationDate,
                    size: size,
                    for: entryID
                )
            } else if !allowFullMetadata {
                if let existing = existingGames[entryID] {
                    metadata = existing.metadata
                } else if let cached = GameLibrarySnapshot.shared.cachedMetadata(
                    for: entryID,
                    modificationDate: modificationDate,
                    size: size
                ) {
                    metadata = cached
                } else {
                    metadata = [
                        "fileTitle": (name as NSString).deletingPathExtension
                    ]
                }
            } else if let cached = GameLibrarySnapshot.shared.cachedMetadata(
                for: entryID,
                modificationDate: modificationDate,
                size: size
            ) {
                metadata = cached
            } else {
                metadata = ARMSX2Bridge.gameMetadata(forISO: bootName)
                GameLibrarySnapshot.shared.storeMetadata(
                    metadata,
                    modificationDate: modificationDate,
                    size: size,
                    for: entryID
                )
            }

            let coverURL = coverStore.coverURL(
                forGameName: name,
                gamePath: fileURL,
                metadata: metadata,
                using: coverLookupIndex
            )
            let existingCover = retainedCover(from: existingGames[entryID])
            let resolvedCoverURL = coverURL ?? existingCover?.url
            let coverSignature = CoverThumbnailCache.signature(
                for: resolvedCoverURL
            ) ?? existingCover?.signature

            return ISOEntry(
                name: name,
                fileURL: fileURL,
                bootPath: external ? path : nil,
                coverURL: resolvedCoverURL,
                coverSignature: coverSignature,
                metadata: metadata,
                size: size,
                isFavorite: favorite,
                isExternal: external,
                sourceName: source
            )
        }.sorted { first, second in
            if first.isFavorite != second.isFavorite {
                return first.isFavorite
            }
            return first.name.localizedCaseInsensitiveCompare(second.name)
                == .orderedAscending
        }

        if !allowFullMetadata {
            NSLog("[ARMSX2 iOS GameList] skipped full ISO metadata refresh while VM is active")
        }

        if loadedGames != games {
            games = loadedGames
        }
        GameLibrarySnapshot.shared.purgeMetadata(
            keeping: Set(loadedGames.lazy.map(\.id))
        )
        GameLibrarySnapshot.shared.update(loadedGames)
        GameLibrarySnapshot.shared.persistMetadataCache()

        if autoDownloadExternalCovers {
            autoDownloadExternalCoversIfNeeded()
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
        autoDownloadCovers(for: importedGames)
    }

    private func clearPendingGameImport() {
        pendingGameImportURLs = []
        existingGameImportFileNames = []
    }

    private func downloadCover(for game: ISOEntry) {
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
            appState.runningGameName = nil
            ARMSX2Bridge.requestVMStop()

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

	private func toggleFavorite(_ game: ISOEntry) {
		let key = game.bootName
		let current = ARMSX2Bridge.isFavorite(key)
        let updatedValue = !current
		ARMSX2Bridge.setFavorite(key, favorite: updatedValue)

        withAnimation(.spring(response: 0.46, dampingFraction: 0.82)) {
            favoriteAnimationValues[game.id, default: 0] += 1
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

        GameLibrarySnapshot.shared.update(games)
        UISelectionFeedbackGenerator().selectionChanged()
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
