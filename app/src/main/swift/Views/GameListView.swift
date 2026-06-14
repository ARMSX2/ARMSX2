// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import UIKit

struct ISOEntry: Identifiable {
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

	var bootName: String {
		isExternal ? (bootPath ?? fileURL?.path ?? name) : name
	}

	var coverInfo: CoverGameInfo {
		CoverGameInfo(name: name, fileURL: fileURL, metadata: metadata, hasCover: coverURL != nil)
	}
}

@MainActor
private final class GameLibrarySnapshot {
	static let shared = GameLibrarySnapshot()

	private var entriesByID: [String: ISOEntry] = [:]
	private var orderedEntries: [ISOEntry] = []

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
		entriesByID = Dictionary(entries.map { ($0.id, $0) }, uniquingKeysWith: { current, _ in current })
	}
}

struct GameListView: View {
    @State private var games: [ISOEntry] = []
    @State private var appState = AppState.shared
	@State private var settings = SettingsStore.shared
	@State private var fileImporter = FileImportHandler.shared
	@State private var coverStore = CoverStore.shared
	@State private var externalLibrary = ExternalGameLibrary.shared
	@State private var externalCoverAutoDownloadAttemptedIDs = Set<String>()
	@State private var showGameImporter = false
	@State private var isLoadingGames = false
	@State private var showCoverImporter = false
    @State private var showCoverPhotoPicker = false
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var showCoverTemplateEditor = false
    @State private var showPNACHImporter = false
    @State private var showGameReplacementAlert = false
    @State private var coverTemplateDraft = CoverStore.defaultCoverURLTemplate
    @State private var pendingGameName: String = ""
    @State private var pendingGameImportURLs: [URL] = []
    @State private var existingGameImportFileNames: [String] = []
    @State private var pendingCoverGameName: String?
    @State private var pendingCoverPhotoGameName: String?
    @State private var selectedCoverPhotoItem: PhotosPickerItem?
    @State private var pendingPNACHGameName: String?
    @State private var gameInfoTarget: ISOEntry?
    @State private var gameSettingsTarget: ISOEntry?
    @State private var gameCompatibilityTarget: ISOEntry?
    @State private var pendingDeleteGame: ISOEntry?
    @State private var pendingDeleteDataGame: ISOEntry?
    @State private var gameActionTitle = ""
    @State private var gameActionMessage: String?
    @AppStorage("ARMSX2iOSGameLibraryLayout") private var libraryLayout = "grid"
    @AppStorage("ARMSX2iOSLandscapeCoverFlowEnabled") private var landscapeCoverFlowEnabled = true
    @State private var backgroundImage: UIImage?
    @State private var landscapeBackgroundImage: UIImage?
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.colorSchemeContrast) private var colorSchemeContrast

    private var hasCustomBackground: Bool {
        backgroundImage != nil || landscapeBackgroundImage != nil
    }

    private var effectiveDim: Double {
        let dim = settings.libraryBackgroundDim
        if reduceTransparency || colorSchemeContrast == .increased {
            return max(dim, 0.55)
        }
        return dim
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

    @ViewBuilder
    private var libraryBackgroundLayer: some View {
        GeometryReader { geometry in
            if let image = libraryBackgroundImage(for: geometry.size) {
                ZStack {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                        .frame(width: geometry.size.width, height: geometry.size.height)
                        .clipped()

                    Color.black.opacity(effectiveDim)
                }
                .frame(width: geometry.size.width, height: geometry.size.height)
            }
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }

    var body: some View {
        NavigationStack {
            ZStack {
                if hasCustomBackground {
                    libraryBackgroundLayer
                }

                GeometryReader { geo in
                    Group {
                        if games.isEmpty && appState.runningGameName == nil {
                            emptyState
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
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
			.navigationTitle(settings.localized("Games"))
				.toolbar {
					ToolbarItem(placement: .topBarTrailing) {
						Button {
							showGameImporter = true
						} label: {
							Image(systemName: "plus")
						}
						.accessibilityLabel(settings.localized("Import Games"))
					}
                ToolbarItem(placement: .topBarTrailing) {
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
                    }
                    .accessibilityLabel(settings.localized("Library Layout"))
                }
                ToolbarItem(placement: .topBarTrailing) {
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
                    }
                    .accessibilityLabel(settings.localized("Covers"))
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { loadGames() } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .disabled(isLoadingGames)
                    .accessibilityLabel(settings.localized("Refresh"))
                }
                ToolbarItem(placement: .topBarLeading) {
                    if ARMSX2Bridge.hasBIOS() {
                        Button(settings.localized("BIOS Only")) {
                            if appState.runningGameName == "BIOS" {
                                appState.returnToGame()
                            } else if appState.runningGameName != nil {
                                pendingGameName = ""
                                showRestartAlert = true
                            } else {
                                appState.bootBIOSOnly()
                            }
                        }
                        .font(.caption)
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
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Restart"), role: .destructive) {
                    if pendingGameName.isEmpty {
                        appState.shutdownAndBootBIOS()
                    } else {
                        appState.shutdownAndBoot(isoName: pendingGameName)
                    }
                }
			} message: {
				let target = pendingGameName.isEmpty ? "BIOS Only" : (pendingGameName as NSString).lastPathComponent
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
            .sheet(isPresented: $showPNACHImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.pnachContentTypes,
                    allowsMultipleSelection: true
                ) { result in
                    showPNACHImporter = false
                    switch result {
                    case .success(let urls):
                        if let pendingPNACHGameName {
                            fileImporter.importPNACHURLs(urls, forISO: pendingPNACHGameName, asCheat: true)
                        } else {
                            fileImporter.presentImportResult(FileImportHandler.pnachImportNeedsGameMessage)
                        }
                        pendingPNACHGameName = nil
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            fileImporter.presentImportResult(FileImportHandler.failedPNACHPickerMessage(errorDescription: error.localizedDescription))
                        }
                        pendingPNACHGameName = nil
                    }
                }
            }
            .sheet(item: $gameInfoTarget) { game in
                GameInfoPanel(game: game, coverStore: coverStore)
                    .presentationDetents([.medium, .large])
            }
            .sheet(item: $gameSettingsTarget) { game in
                PerGameSettingsPanel(game: game)
                    .presentationDetents([.large])
                    .presentationDragIndicator(.visible)
                    .presentationBackground(.regularMaterial)
                    .presentationCornerRadius(34)
            }
            .sheet(item: $gameCompatibilityTarget) { game in
                GameCompatibilityPanel(game: game)
                    .presentationDetents([.medium, .large])
            }
        }
	        .onAppear {
			externalLibrary.reload()
			restoreCachedGamesIfNeeded()
			loadGames(autoDownloadExternalCovers: true)
			reloadLibraryBackground()
		}
		.onChange(of: settings.libraryBackgroundPath) { _, _ in
			reloadLibraryBackground()
		}
		.onChange(of: settings.libraryLandscapeBackgroundPath) { _, _ in
			reloadLibraryBackground()
		}
		.onChange(of: settings.libraryBackgroundRevision) { _, _ in
			reloadLibraryBackground()
		}
		.onReceive(NotificationCenter.default.publisher(for: ExternalGameLibrary.didChangeNotification)) { _ in
			loadGames(autoDownloadExternalCovers: true)
		}
		.onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("ARMSX2iOSReturnToMenu"))) { _ in
			restoreCachedGamesIfNeeded()
			loadGames(autoDownloadExternalCovers: false)
		}
    }

    private var listLibrary: some View {
        List {
            if let gameName = appState.runningGameName {
                vmStatusSection(gameName: gameName)
            }
            ForEach(games) { game in
                gameRow(game)
                    .libraryBackgroundListRow(hasCustomBackground)
            }
        }
        .scrollContentBackground(hasCustomBackground ? .hidden : .automatic)
    }

    private var gridLibrary: some View {
        ScrollView {
            LazyVStack(spacing: 16) {
                if let gameName = appState.runningGameName {
                    vmStatusCard(gameName: gameName)
                        .padding(.horizontal)
                }

                LazyVGrid(columns: [GridItem(.adaptive(minimum: 142), spacing: 14, alignment: .top)], spacing: 18) {
                    ForEach(games) { game in
                        gameGridCard(game)
                    }
                }
                .padding(.horizontal)
                .padding(.bottom, 20)
            }
            .padding(.top, 12)
        }
        .background(hasCustomBackground ? Color.clear : Color(.systemGroupedBackground))
        .transaction { transaction in
            transaction.animation = nil
        }
    }

    private func coverFlowLibrary(containerSize: CGSize) -> some View {
        let metrics = CoverFlowMetrics(containerSize: containerSize)

        return ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(alignment: .center, spacing: metrics.itemSpacing) {
                if let gameName = appState.runningGameName {
                    vmStatusCoverCard(gameName: gameName, metrics: metrics)
                }

                ForEach(games) { game in
                    coverFlowCard(game, metrics: metrics)
                }
            }
            .frame(
                minWidth: max(0, containerSize.width - (metrics.horizontalPadding * 2)),
                minHeight: max(0, containerSize.height - (metrics.verticalPadding * 2)),
                alignment: .center
            )
            .padding(.horizontal, metrics.horizontalPadding)
            .padding(.vertical, metrics.verticalPadding)
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
        .transaction { transaction in
            transaction.animation = nil
        }
    }

    private func vmStatusSection(gameName: String) -> some View {
        Section {
            // Resume row — tap anywhere to return to game
            Button {
                appState.returnToGame()
            } label: {
                HStack {
                    Image(systemName: "play.circle.fill")
                        .foregroundStyle(.green)
                        .font(.title)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(settings.localized("Now Running"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(gameName == "BIOS" ? settings.localized("BIOS Only") : gameName)
                            .font(.body)
                            .fontWeight(.semibold)
                    }
                    Spacer()
                    Text(settings.localized("Resume"))
                        .font(.subheadline)
                        .fontWeight(.medium)
                    Image(systemName: "chevron.right")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
                .padding(.vertical, 6)
            }
            .tint(.primary)
            .libraryBackgroundListRow(hasCustomBackground)

            // Stop button — separate row with confirmation alert
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
            .libraryBackgroundListRow(hasCustomBackground)
        }
        .alert(settings.localized("Stop Emulation?"), isPresented: $showStopAlert) {
            Button(settings.localized("Cancel"), role: .cancel) { }
            Button(settings.localized("Stop"), role: .destructive) {
                ARMSX2Bridge.requestVMStop()
                appState.runningGameName = nil
            }
        } message: {
            Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
        }
    }

    private func gameRow(_ game: ISOEntry) -> some View {
        Button {
            open(game)
        } label: {
            HStack(spacing: 12) {
                coverThumbnail(for: game)

                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Text(coverStore.displayName(forGameName: game.name))
                            .font(.body)
                            .fontWeight(.medium)
                            .foregroundStyle(.primary)
						if isRunning(game) {
							Image(systemName: "circle.fill")
								.font(.system(size: 8))
								.foregroundStyle(.green)
								.accessibilityLabel(settings.localized("Running"))
						}
                    }
                    HStack(spacing: 8) {
						Text(formatSize(game.size))
						Text(game.name.pathExtensionLabel)
						if game.isExternal {
							Label(settings.localized("External"), systemImage: "externaldrive")
						}
					}
					.font(.caption)
					.foregroundStyle(.secondary)
				}
				Spacer()
				Button {
					toggleFavorite(game)
				} label: {
					Image(systemName: game.isFavorite ? "star.fill" : "star")
						.foregroundStyle(game.isFavorite ? .yellow : .gray)
				}
				.buttonStyle(.plain)
				.accessibilityLabel(game.isFavorite ? settings.localized("Remove from favorites") : settings.localized("Add to favorites"))

				Image(systemName: isRunning(game) ? "play.fill" : "chevron.right")
					.foregroundStyle(isRunning(game) ? .green : .secondary)
					.font(.caption)
					.accessibilityHidden(true)
			}
		}
        .foregroundStyle(.primary)
        .contextMenu {
            gameContextMenu(for: game)
        }
    }

    private func gameGridCard(_ game: ISOEntry) -> some View {
        Button {
            open(game)
        } label: {
            VStack(alignment: .leading, spacing: 10) {
                ZStack(alignment: .topTrailing) {
                    coverThumbnail(for: game, width: 126, height: 189)
                        .frame(maxWidth: .infinity)

					Button {
						toggleFavorite(game)
					} label: {
						Image(systemName: game.isFavorite ? "star.fill" : "star")
                            .font(.callout.weight(.semibold))
                            .foregroundStyle(game.isFavorite ? .yellow : .white.opacity(0.86))
                            .padding(8)
                            .background(.black.opacity(0.48), in: Circle())
                    }
                    .buttonStyle(.plain)
                    .padding(6)
                    .accessibilityLabel(game.isFavorite ? settings.localized("Remove from favorites") : settings.localized("Add to favorites"))
                }

                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 5) {
                        Text(coverStore.displayName(forGameName: game.name))
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.primary)
                            .lineLimit(2)
                            .multilineTextAlignment(.leading)
						if isRunning(game) {
							Image(systemName: "circle.fill")
                                .font(.system(size: 7))
                                .foregroundStyle(.green)
                                .accessibilityLabel(settings.localized("Running"))
                        }
                    }
                    HStack(spacing: 6) {
						Text(game.name.pathExtensionLabel)
						Text(formatSize(game.size))
						if game.isExternal {
							Image(systemName: "externaldrive")
								.accessibilityLabel(settings.localized("External"))
						}
					}
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(8)
            .frame(maxWidth: .infinity, minHeight: 268, alignment: .top)
            .background(.background, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
				.overlay {
					RoundedRectangle(cornerRadius: 18, style: .continuous)
						.strokeBorder(isRunning(game) ? .green.opacity(0.6) : .white.opacity(0.08), lineWidth: 1)
				}
        }
        .buttonStyle(.plain)
        .contextMenu {
            gameContextMenu(for: game)
        }
    }

    private func coverFlowCard(_ game: ISOEntry, metrics: CoverFlowMetrics) -> some View {
        Button {
            open(game)
        } label: {
            VStack(spacing: metrics.cardSpacing) {
                ZStack(alignment: .topTrailing) {
                    coverThumbnail(for: game, width: metrics.coverWidth, height: metrics.coverHeight)
                        .shadow(color: .black.opacity(0.28), radius: 18, y: 10)

					Button {
						toggleFavorite(game)
					} label: {
                        Image(systemName: game.isFavorite ? "star.fill" : "star")
                            .font((metrics.isCompact ? Font.subheadline : Font.headline).weight(.semibold))
                            .foregroundStyle(game.isFavorite ? .yellow : .white.opacity(0.88))
                            .padding(metrics.favoritePadding)
                            .background(.black.opacity(0.48), in: Circle())
                    }
                    .buttonStyle(.plain)
                    .padding(metrics.favoriteInset)
                    .accessibilityLabel(game.isFavorite ? settings.localized("Remove from favorites") : settings.localized("Add to favorites"))
                }

                VStack(spacing: 4) {
                    Text(coverStore.displayName(forGameName: game.name))
                        .font((metrics.isCompact ? Font.subheadline : Font.headline).weight(.semibold))
                        .multilineTextAlignment(.center)
                        .lineLimit(2)
				Text(game.isExternal ? "\(game.name.pathExtensionLabel)  \(formatSize(game.size))  \(settings.localized("External"))" : "\(game.name.pathExtensionLabel)  \(formatSize(game.size))")
					.font(metrics.isCompact ? .caption2 : .caption)
					.foregroundStyle(.secondary)
                }
                .frame(width: metrics.textWidth)
            }
            .padding(metrics.cardPadding)
            .background(.thinMaterial, in: RoundedRectangle(cornerRadius: metrics.cornerRadius, style: .continuous))
			.overlay {
				RoundedRectangle(cornerRadius: metrics.cornerRadius, style: .continuous)
					.strokeBorder(isRunning(game) ? .green.opacity(0.7) : .white.opacity(0.12), lineWidth: 1)
			}
        }
        .buttonStyle(.plain)
        .contextMenu {
            gameContextMenu(for: game)
        }
    }

    private func coverThumbnail(for game: ISOEntry, width: CGFloat = 58, height: CGFloat = 87) -> some View {
        CoverThumbnailView(
            gameName: game.name,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature,
            width: width,
            height: height
        )
    }

    private func vmStatusCard(gameName: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "play.circle.fill")
                .font(.title2)
                .foregroundStyle(.green)
            VStack(alignment: .leading, spacing: 3) {
                Text(settings.localized("Now Running"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(gameName == "BIOS" ? settings.localized("BIOS Only") : gameName)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
            }
            Spacer()
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
        .padding()
        .background(.background, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .alert(settings.localized("Stop Emulation?"), isPresented: $showStopAlert) {
            Button(settings.localized("Cancel"), role: .cancel) { }
            Button(settings.localized("Stop"), role: .destructive) {
                ARMSX2Bridge.requestVMStop()
                appState.runningGameName = nil
            }
        } message: {
            Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
        }
    }

    private func vmStatusCoverCard(gameName: String, metrics: CoverFlowMetrics) -> some View {
        VStack(spacing: metrics.isCompact ? 9 : 14) {
            Image(systemName: "play.circle.fill")
                .font(.system(size: metrics.statusIconSize))
                .foregroundStyle(.green)
            Text(settings.localized("Now Running"))
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Text(gameName == "BIOS" ? settings.localized("BIOS Only") : gameName)
                .font((metrics.isCompact ? Font.subheadline : Font.headline).weight(.semibold))
                .lineLimit(2)
                .multilineTextAlignment(.center)
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
        }
        .frame(width: metrics.statusWidth, height: metrics.statusHeight)
        .padding(metrics.cardPadding)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: metrics.cornerRadius, style: .continuous))
        .alert(settings.localized("Stop Emulation?"), isPresented: $showStopAlert) {
            Button(settings.localized("Cancel"), role: .cancel) { }
            Button(settings.localized("Stop"), role: .destructive) {
                ARMSX2Bridge.requestVMStop()
                appState.runningGameName = nil
            }
        } message: {
            Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
        }
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

        Button {
            presentMenuPanel("compatibility_lab") {
                gameCompatibilityTarget = game
            }
        } label: {
            Label(settings.localized("Compatibility Lab"), systemImage: "wand.and.stars")
        }

		Button {
			presentMenuPanel("pnach_import") {
				pendingPNACHGameName = game.bootName
				showPNACHImporter = true
			}
		} label: {
            Label(settings.localized("Import PNACH / 60 FPS Patch"), systemImage: "wand.and.stars")
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

		guard ARMSX2Bridge.hasBIOS() else {
			gameActionTitle = settings.localized("BIOS Required")
			gameActionMessage = settings.localized("Import a valid PS2 BIOS before starting games.")
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
			showRestartAlert = true
		} else {
			appState.bootGame(isoName: game.bootName)
		}
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
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

	private func loadGames(autoDownloadExternalCovers: Bool = false) {
		guard !isLoadingGames else { return }
		isLoadingGames = true
		defer { isLoadingGames = false }

		let fm = FileManager.default
		let allowFullMetadata = appState.runningGameName == nil
		let existingGames = GameLibrarySnapshot.shared.existingEntries(merging: games)
		externalLibrary.reload()
		games = ARMSX2Bridge.availableISOEntries().compactMap { rawEntry -> ISOEntry? in
			guard let name = rawEntry["name"] as? String,
			      let path = rawEntry["path"] as? String else {
				return nil
			}
			let external = (rawEntry["external"] as? NSNumber)?.boolValue ?? (rawEntry["external"] as? Bool ?? false)
			let source = rawEntry["source"] as? String
			let bootName = external ? path : name
			let attrs = try? fm.attributesOfItem(atPath: path)
			let size = attrs?[.size] as? UInt64 ?? 0
			let fav = ARMSX2Bridge.isFavorite(bootName)
			let fileURL = URL(fileURLWithPath: path)
			let entryID = external ? path : fileURL.path
			let metadata: [String: String]
			if allowFullMetadata {
				metadata = ARMSX2Bridge.gameMetadata(forISO: bootName)
			} else if let existing = existingGames[entryID] {
				metadata = existing.metadata
			} else {
				metadata = ["fileTitle": (name as NSString).deletingPathExtension]
			}
			let coverURL = coverStore.coverURL(forGameName: name, gamePath: fileURL, metadata: metadata)
			let existingCover = retainedCover(from: existingGames[entryID])
			let resolvedCoverURL = coverURL ?? existingCover?.url
			let coverSignature = CoverThumbnailCache.signature(for: resolvedCoverURL) ?? existingCover?.signature
			return ISOEntry(
				name: name,
				fileURL: fileURL,
				bootPath: external ? path : nil,
				coverURL: resolvedCoverURL,
				coverSignature: coverSignature,
				metadata: metadata,
				size: size,
				isFavorite: fav,
				isExternal: external,
				sourceName: source
			)
		}.sorted { a, b in
			if a.isFavorite != b.isFavorite { return a.isFavorite }
			return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
		}

		if !allowFullMetadata {
			NSLog("[ARMSX2 iOS GameList] skipped full ISO metadata refresh while VM is active")
		}

		GameLibrarySnapshot.shared.update(games)

		if autoDownloadExternalCovers {
			autoDownloadExternalCoversIfNeeded()
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
        Task {
            _ = await coverStore.downloadMissingCovers(for: targets)
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
        Task {
            _ = await coverStore.downloadMissingCovers(for: [game.coverInfo])
            loadGames()
        }
    }

    private func importCoverPhoto(_ photoItem: PhotosPickerItem, forGameNamed gameName: String) {
        Task {
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

		Task {
			let summary = await coverStore.downloadMissingCovers(for: targets, showResult: false)
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
		Task {
			let summary = await coverStore.downloadMissingCovers(for: coverTargets, showResult: false)
			if summary.downloaded > 0 {
				loadGames()
			}
		}
	}

	private func toggleFavorite(_ game: ISOEntry) {
		let key = game.bootName
		let current = ARMSX2Bridge.isFavorite(key)
		ARMSX2Bridge.setFavorite(key, favorite: !current)
		loadGames()
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
		guard let runningGameName = appState.runningGameName else {
			return false
		}

		if runningGameName == game.bootName || runningGameName == game.name {
			return true
		}

		return (runningGameName as NSString).lastPathComponent == game.name
	}

    private func formatSize(_ bytes: UInt64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1.0 {
            return String(format: "%.1f GB", gb)
        }
        let mb = Double(bytes) / 1_048_576
        return String(format: "%.0f MB", mb)
    }

    private func libraryBackgroundImage(for size: CGSize) -> UIImage? {
        if size.width > size.height, let landscapeBackgroundImage {
            return landscapeBackgroundImage
        }
        return backgroundImage ?? landscapeBackgroundImage
    }

    private func reloadLibraryBackground() {
        backgroundImage = loadLibraryBackground(at: settings.libraryBackgroundPath)
        landscapeBackgroundImage = loadLibraryBackground(at: settings.libraryLandscapeBackgroundPath)
    }

    private func loadLibraryBackground(at path: String) -> UIImage? {
        guard !path.isEmpty, FileManager.default.fileExists(atPath: path) else { return nil }
        return UIImage(contentsOfFile: path)
    }
}

private struct LibraryBackgroundListRowModifier: ViewModifier {
    let isEnabled: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .listRowInsets(EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 12))
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
        } else {
            content
        }
    }
}

private extension View {
    func libraryBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(LibraryBackgroundListRowModifier(isEnabled: isEnabled))
    }
}

private struct GameInfoPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    let game: ISOEntry
    let coverStore: CoverStore

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
                            Text(coverStore.displayName(forGameName: game.name))
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
                        Text(formatSize(game.size))
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
        return "\(Self.regionFlag(for: region)) \(region)"
    }

    private func metadataValue(_ key: String) -> String {
        let value = game.metadata[key]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? settings.localized("Unknown") : value
    }

    private func formatSize(_ bytes: UInt64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1.0 {
            return String(format: "%.1f GB", gb)
        }
        let mb = Double(bytes) / 1_048_576
        return String(format: "%.0f MB", mb)
    }

    private static func regionFlag(for region: String) -> String {
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
        return "🌐"
    }
}

private struct GameCompatibilityPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var selectedPreset: String
    @State private var identity: String
    @State private var statusMessage: String?

    let game: ISOEntry

    init(game: ISOEntry) {
        self.game = game
        _selectedPreset = State(initialValue: ARMSX2Bridge.compatibilityPreset(forISO: game.bootName))
        _identity = State(initialValue: ARMSX2Bridge.compatibilityIdentity(forISO: game.bootName))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    LabeledContent(settings.localized("Current Game")) {
                        Text(identity.isEmpty ? settings.localized("Unknown") : identity)
                            .foregroundStyle(.secondary)
                    }
                    LabeledContent(settings.localized("Current Mode")) {
                        Text(settings.localized(presetTitle(selectedPreset)))
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text(settings.localized("Status"))
                } footer: {
                    Text(settings.localized("Presets are saved for this game and apply on the next boot/reset. Use Off / Default when a preset makes rendering or stability worse."))
                }

                Section(settings.localized("Presets")) {
                    ForEach(compatibilityPresets) { preset in
                        Button {
                            apply(preset)
                        } label: {
                            HStack {
                                Label(settings.localized(preset.title), systemImage: preset.systemImage)
                                Spacer()
                                if selectedPreset == preset.id {
                                    Image(systemName: "checkmark")
                                }
                            }
                        }
                        .foregroundStyle(.primary)
                    }
                }

                Section {
                    ForEach(advancedPresets) { preset in
                        Toggle(isOn: Binding(
                            get: { ARMSX2Bridge.compatibilityFlag(preset.id, forISO: game.bootName) },
                            set: { enabled in
                                ARMSX2Bridge.setCompatibilityFlag(preset.id, enabled: enabled, forISO: game.bootName)
                                selectedPreset = ARMSX2Bridge.compatibilityPreset(forISO: game.bootName)
                                identity = ARMSX2Bridge.compatibilityIdentity(forISO: game.bootName)
                                statusMessage = "\(settings.localized("Custom compatibility flags saved for")) \(identity)"
                            }
                        )) {
                            Label(settings.localized(preset.title), systemImage: preset.systemImage)
                        }
                    }
                } header: {
                    Text(settings.localized("Advanced Custom Flags"))
                } footer: {
                    Text(settings.localized("Toggle one or more flags when one preset is not enough. Changing any flag switches this game to Custom Advanced Flags."))
                }

                Section {
                    Button(role: .destructive) {
                        ARMSX2Bridge.forgetCompatibilityPreset(forISO: game.bootName)
                        selectedPreset = ARMSX2Bridge.compatibilityPreset(forISO: game.bootName)
                        identity = ARMSX2Bridge.compatibilityIdentity(forISO: game.bootName)
                        statusMessage = settings.localized("Compatibility preset reset for this game.")
                    } label: {
                        Label(settings.localized("Forget This Game's Override"), systemImage: "trash")
                    }
                }

                if let statusMessage {
                    Section {
                        Text(statusMessage)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle(settings.localized("Compatibility Lab"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
        }
    }

    private func apply(_ preset: CompatibilityPreset) {
        ARMSX2Bridge.setCompatibilityPreset(preset.id, forISO: game.bootName)
        selectedPreset = ARMSX2Bridge.compatibilityPreset(forISO: game.bootName)
        identity = ARMSX2Bridge.compatibilityIdentity(forISO: game.bootName)
        statusMessage = "\(settings.localized(preset.title)) \(settings.localized("saved for this game. Reset or relaunch to apply."))"
    }

    private func presetTitle(_ id: String) -> String {
        compatibilityPresets.first(where: { $0.id == id })?.title ?? "Custom Advanced Flags"
    }

    private var advancedPresets: [CompatibilityPreset] {
        compatibilityPresets.filter { $0.id != "off" }
    }
}

struct PerGameSettingsPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    private struct PickerOption: Identifiable {
        let id: Int
        let title: String
    }

    private static let useGlobalSentinel = -1
    private static let trilinearUseGlobalSentinel = Int(Int32.min)

    private static let deinterlaceOptions = [
        PickerOption(id: 0, title: "None"),
        PickerOption(id: 1, title: "Weave (TFF)"),
        PickerOption(id: 2, title: "Weave (BFF)"),
        PickerOption(id: 3, title: "Bob (TFF)"),
        PickerOption(id: 4, title: "Bob (BFF)"),
        PickerOption(id: 5, title: "Blend (TFF)"),
        PickerOption(id: 6, title: "Blend (BFF)"),
        PickerOption(id: 7, title: "Adaptive (Default)")
    ]
    private static let trilinearFilteringOptions = [
        PickerOption(id: trilinearUseGlobalSentinel, title: "Use Global"),
        PickerOption(id: -1, title: "Automatic / Default"),
        PickerOption(id: 0, title: "Off"),
        PickerOption(id: 1, title: "PS2"),
        PickerOption(id: 2, title: "Forced")
    ]
    private static let halfPixelOffsetOptions = [
        PickerOption(id: useGlobalSentinel, title: "Use Global"),
        PickerOption(id: 0, title: "Off"),
        PickerOption(id: 1, title: "Normal / Vertex"),
        PickerOption(id: 2, title: "Special / Texture"),
        PickerOption(id: 3, title: "Special / Texture Aggressive"),
        PickerOption(id: 4, title: "Align to Native"),
        PickerOption(id: 5, title: "Align to Native + Texture Offset")
    ]
    private static let roundSpriteOptions = [
        PickerOption(id: useGlobalSentinel, title: "Use Global"),
        PickerOption(id: 0, title: "Off"),
        PickerOption(id: 1, title: "Half"),
        PickerOption(id: 2, title: "Full")
    ]

    let game: ISOEntry
    let onDone: (() -> Void)?
    let savesToRunningGame: Bool

    @State private var enabled: Bool
    @State private var upscaleMultiplier: Float
    @State private var aspectRatio: String
    @State private var textureFiltering: Int
    @State private var hardwareMipmapping: Bool
    @State private var blendingAccuracy: Int
    @State private var interlaceMode: Int
    @State private var trilinearFiltering: Int
    @State private var halfPixelOffset: Int
    @State private var roundSprite: Int
    @State private var alignSpriteOverride: Bool
    @State private var alignSprite: Bool
    @State private var mergeSpriteOverride: Bool
    @State private var mergeSprite: Bool
    @State private var wildArmsOffsetOverride: Bool
    @State private var wildArmsOffset: Bool
    @State private var textureOffsetXOverride: Bool
    @State private var textureOffsetX: Int
    @State private var textureOffsetYOverride: Bool
    @State private var textureOffsetY: Int
    @State private var skipDrawStartOverride: Bool
    @State private var skipDrawStart: Int
    @State private var skipDrawEndOverride: Bool
    @State private var skipDrawEnd: Int
    @State private var eeCoreType: Int
    @State private var mtvu: Bool
    @State private var enableCheats: Bool
    @State private var enablePatches: Bool
    @State private var enableGameFixes: Bool
    @State private var enableGameDBHardwareFixes: Bool
    @State private var statusMessage: String?

    init(
        game: ISOEntry,
        preloadedSettings: [String: Any]? = nil,
        savesToRunningGame: Bool = false,
        onDone: (() -> Void)? = nil
    ) {
        self.game = game
        self.onDone = onDone
        self.savesToRunningGame = savesToRunningGame
        // The runtime caller passes settings it already loaded through a VM-safe path so
        // this view never re-scans the disc image during init while a game is running.
        let info = preloadedSettings ?? ARMSX2Bridge.gameSettings(forISO: game.bootName)
        _enabled = State(initialValue: Self.boolValue(info["enabled"], defaultValue: false))
        _upscaleMultiplier = State(initialValue: Self.floatValue(info["upscaleMultiplier"], defaultValue: 1.0))
        _aspectRatio = State(initialValue: Self.normalizedAspect(info["aspectRatio"] as? String))
        _textureFiltering = State(initialValue: Self.intValue(info["textureFiltering"], defaultValue: 2))
        _hardwareMipmapping = State(initialValue: Self.boolValue(info["hardwareMipmapping"], defaultValue: true))
        _blendingAccuracy = State(initialValue: Self.intValue(info["blendingAccuracy"], defaultValue: 1))
        _interlaceMode = State(initialValue: Self.intValue(info["interlaceMode"], defaultValue: 7))
        _trilinearFiltering = State(initialValue: Self.boolValue(info["hasTrilinearFilteringOverride"], defaultValue: false) ? Self.intValue(info["trilinearFiltering"], defaultValue: -1) : Self.trilinearUseGlobalSentinel)
        _halfPixelOffset = State(initialValue: Self.boolValue(info["hasHalfPixelOffsetOverride"], defaultValue: false) ? Self.intValue(info["halfPixelOffset"], defaultValue: 0) : Self.useGlobalSentinel)
        _roundSprite = State(initialValue: Self.boolValue(info["hasRoundSpriteOverride"], defaultValue: false) ? Self.intValue(info["roundSprite"], defaultValue: 0) : Self.useGlobalSentinel)
        _alignSpriteOverride = State(initialValue: Self.boolValue(info["hasAlignSpriteOverride"], defaultValue: false))
        _alignSprite = State(initialValue: Self.boolValue(info["alignSprite"], defaultValue: false))
        _mergeSpriteOverride = State(initialValue: Self.boolValue(info["hasMergeSpriteOverride"], defaultValue: false))
        _mergeSprite = State(initialValue: Self.boolValue(info["mergeSprite"], defaultValue: false))
        _wildArmsOffsetOverride = State(initialValue: Self.boolValue(info["hasWildArmsOffsetOverride"], defaultValue: false))
        _wildArmsOffset = State(initialValue: Self.boolValue(info["wildArmsOffset"], defaultValue: false))
        _textureOffsetXOverride = State(initialValue: Self.boolValue(info["hasTextureOffsetXOverride"], defaultValue: false))
        _textureOffsetX = State(initialValue: Self.clampedTextureOffset(Self.intValue(info["textureOffsetX"], defaultValue: 0)))
        _textureOffsetYOverride = State(initialValue: Self.boolValue(info["hasTextureOffsetYOverride"], defaultValue: false))
        _textureOffsetY = State(initialValue: Self.clampedTextureOffset(Self.intValue(info["textureOffsetY"], defaultValue: 0)))
        let hasSkipDrawStartOverride = Self.boolValue(info["hasSkipDrawStartOverride"], defaultValue: false)
        let hasSkipDrawEndOverride = Self.boolValue(info["hasSkipDrawEndOverride"], defaultValue: false)
        let initialSkipDrawStart = Self.clampedSkipDraw(Self.intValue(info["skipDrawStart"], defaultValue: 0))
        let initialSkipDrawEnd = Self.normalizedSkipDrawEnd(
            start: initialSkipDrawStart,
            end: Self.intValue(info["skipDrawEnd"], defaultValue: 0),
            startOverride: hasSkipDrawStartOverride,
            endOverride: hasSkipDrawEndOverride
        )
        _skipDrawStartOverride = State(initialValue: hasSkipDrawStartOverride)
        _skipDrawStart = State(initialValue: initialSkipDrawStart)
        _skipDrawEndOverride = State(initialValue: hasSkipDrawEndOverride)
        _skipDrawEnd = State(initialValue: initialSkipDrawEnd)
        _eeCoreType = State(initialValue: Self.intValue(info["eeCoreType"], defaultValue: 2))
        _mtvu = State(initialValue: Self.boolValue(info["mtvu"], defaultValue: true))
        _enableCheats = State(initialValue: Self.boolValue(info["enableCheats"], defaultValue: false))
        _enablePatches = State(initialValue: Self.boolValue(info["enablePatches"], defaultValue: true))
        _enableGameFixes = State(initialValue: Self.boolValue(info["enableGameFixes"], defaultValue: true))
        _enableGameDBHardwareFixes = State(initialValue: Self.boolValue(info["enableGameDBHardwareFixes"], defaultValue: true))
    }

    private var manualAdvancedHacksEnabled: Bool {
        enabled && !enableGameDBHardwareFixes
    }

    private var skipDrawStartBinding: Binding<Int> {
        Binding(
            get: { skipDrawStart },
            set: { newValue in
                skipDrawStart = Self.clampedSkipDraw(newValue)
                normalizeSkipDrawRangeIfNeeded()
            }
        )
    }

    private var skipDrawEndBinding: Binding<Int> {
        Binding(
            get: { skipDrawEnd },
            set: { newValue in
                skipDrawEnd = Self.normalizedSkipDrawEnd(
                    start: skipDrawStart,
                    end: newValue,
                    startOverride: skipDrawStartOverride,
                    endOverride: skipDrawEndOverride
                )
            }
        )
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 12) {
                        Image(systemName: enabled ? "slider.horizontal.3" : "power")
                            .font(.title3)
                            .foregroundStyle(enabled ? Color.accentColor : Color.secondary)
                            .frame(width: 32, height: 32)
                            .background(Color.accentColor.opacity(enabled ? 0.14 : 0), in: Circle())

                        VStack(alignment: .leading, spacing: 4) {
                            Text(displayName)
                                .font(.headline)
                                .lineLimit(2)
                            Text(settings.localized("Saved changes apply to this game only. Done leaves without saving."))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section {
                    Toggle(settings.localized("Use Per-Game Overrides"), isOn: $enabled)
                    Text(settings.localized("Overrides are saved for this game only and apply on the next boot/reset of this title."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section(settings.localized("CPU")) {
                    Picker(settings.localized("EE Core"), selection: $eeCoreType) {
                        Text(settings.localized("ARM64 JIT")).tag(2)
                        Text(settings.localized("Interpreter")).tag(1)
                    }
                    .disabled(!enabled)

                    Text(settings.localized("Interpreter is slower, but can help isolate EE JIT crashes for specific games. Reset/relaunch after changing it."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Toggle("MTVU", isOn: $mtvu)
                        .disabled(!enabled)
                    Text(settings.localized("MTVU can improve performance and may help some visual issues, but can cause compatibility problems. Reset/relaunch after changing it."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section(settings.localized("Graphics")) {
                    Picker(settings.localized("Internal Resolution"), selection: $upscaleMultiplier) {
                        Text("0.25x (Fastest)").tag(Float(0.25))
                        Text("0.5x").tag(Float(0.5))
                        Text("0.75x").tag(Float(0.75))
                        Text("1x Native").tag(Float(1.0))
                        Text("2x").tag(Float(2.0))
                        Text("3x").tag(Float(3.0))
                        Text("4x").tag(Float(4.0))
                    }
                    .disabled(!enabled)

                    Picker(settings.localized("Aspect Ratio"), selection: $aspectRatio) {
                        Text("Auto 4:3 / 3:2").tag("Auto 4:3/3:2")
                        Text("4:3").tag("4:3")
                        Text("16:9").tag("16:9")
                        Text("10:7").tag("10:7")
                        Text("Stretch").tag("Stretch")
                    }
                    .disabled(!enabled)

                    Picker(settings.localized("Texture Filtering"), selection: $textureFiltering) {
                        Text("Nearest").tag(0)
                        Text("Bilinear Forced").tag(1)
                        Text("Bilinear PS2 Default").tag(2)
                        Text("Bilinear excl. Sprite").tag(3)
                    }
                    .disabled(!enabled)

                    Toggle(settings.localized("Hardware Mipmapping"), isOn: $hardwareMipmapping)
                        .disabled(!enabled)
                    Text(settings.localized("Turn this off only for games with mipmap-related texture stripes, shimmer, or bad LOD. Reset/relaunch the game after changing it."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Picker(settings.localized("Blending Accuracy"), selection: $blendingAccuracy) {
                        Text("Minimum").tag(0)
                        Text("Basic").tag(1)
                        Text("Medium").tag(2)
                        Text("High").tag(3)
                        Text("Full").tag(4)
                        Text("Ultra").tag(5)
                    }
                    .disabled(!enabled)

                    Picker(settings.localized("Deinterlace"), selection: $interlaceMode) {
                        ForEach(Self.deinterlaceOptions) { option in
                            Text(settings.localized(option.title)).tag(option.id)
                        }
                    }
                    .disabled(!enabled)
                }

                Section(settings.localized("Advanced Upscaling Hacks")) {
                    Text(settings.localized("Manual advanced hacks only apply when Use Per-Game Overrides is on and GameDB Graphics Fixes is off. Save, then reset or relaunch the game."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    if enabled && enableGameDBHardwareFixes {
                        Text(settings.localized("GameDB Graphics Fixes is on, so manual advanced hacks are saved but ignored until it is turned off for this game."))
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }

                    Picker(settings.localized("Trilinear Filtering"), selection: $trilinearFiltering) {
                        ForEach(Self.trilinearFilteringOptions) { option in
                            Text(settings.localized(option.title)).tag(option.id)
                        }
                    }
                    .disabled(!enabled)

                    if trilinearFiltering != Self.trilinearUseGlobalSentinel && trilinearFiltering != -1 {
                        Text(settings.localized("Non-automatic trilinear filtering may break textures in some games."))
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }

                    Picker(settings.localized("Half-pixel Offset"), selection: $halfPixelOffset) {
                        ForEach(Self.halfPixelOffsetOptions) { option in
                            Text(settings.localized(option.title)).tag(option.id)
                        }
                    }
                    .disabled(!manualAdvancedHacksEnabled)

                    Picker(settings.localized("Round Sprite"), selection: $roundSprite) {
                        ForEach(Self.roundSpriteOptions) { option in
                            Text(settings.localized(option.title)).tag(option.id)
                        }
                    }
                    .disabled(!manualAdvancedHacksEnabled)

                    Toggle(settings.localized("Override Align Sprite"), isOn: $alignSpriteOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if alignSpriteOverride {
                        Toggle(settings.localized("Align Sprite"), isOn: $alignSprite)
                            .disabled(!manualAdvancedHacksEnabled)
                    }

                    Toggle(settings.localized("Override Merge Sprite"), isOn: $mergeSpriteOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if mergeSpriteOverride {
                        Toggle(settings.localized("Merge Sprite"), isOn: $mergeSprite)
                            .disabled(!manualAdvancedHacksEnabled)
                    }

                    Toggle(settings.localized("Override Wild Arms Offset"), isOn: $wildArmsOffsetOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if wildArmsOffsetOverride {
                        Toggle(settings.localized("Wild Arms Offset"), isOn: $wildArmsOffset)
                            .disabled(!manualAdvancedHacksEnabled)
                    }

                    Toggle(settings.localized("Override Texture Offset X"), isOn: $textureOffsetXOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if textureOffsetXOverride {
                        ClampedIntField(title: settings.localized("Texture Offset X"), value: $textureOffsetX, range: SettingsStore.textureOffsetRange, isEnabled: manualAdvancedHacksEnabled)
                    }

                    Toggle(settings.localized("Override Texture Offset Y"), isOn: $textureOffsetYOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if textureOffsetYOverride {
                        ClampedIntField(title: settings.localized("Texture Offset Y"), value: $textureOffsetY, range: SettingsStore.textureOffsetRange, isEnabled: manualAdvancedHacksEnabled)
                    }

                    Toggle(settings.localized("Override Skipdraw Start"), isOn: $skipDrawStartOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if skipDrawStartOverride {
                        ClampedIntField(title: settings.localized("Skipdraw Start"), value: skipDrawStartBinding, range: SettingsStore.skipDrawRange, isEnabled: manualAdvancedHacksEnabled)
                    }

                    Toggle(settings.localized("Override Skipdraw End"), isOn: $skipDrawEndOverride)
                        .disabled(!manualAdvancedHacksEnabled)
                    if skipDrawEndOverride {
                        ClampedIntField(title: settings.localized("Skipdraw End"), value: skipDrawEndBinding, range: SettingsStore.skipDrawRange, isEnabled: manualAdvancedHacksEnabled)
                    }
                    if skipDrawStartOverride || skipDrawEndOverride {
                        Text(settings.localized("For Skipdraw 1, use Start 1 and End 1. Changes apply after reset/relaunch."))
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }

                Section(settings.localized("Patches & Cheats")) {
                    Toggle(settings.localized("Enable PNACH Cheats"), isOn: $enableCheats)
                        .disabled(!enabled)
                    Toggle(settings.localized("GameDB PNACH Patches"), isOn: $enablePatches)
                        .disabled(!enabled)
                    Toggle(settings.localized("GameDB Core Fixes"), isOn: $enableGameFixes)
                        .disabled(!enabled)
                    Toggle(settings.localized("GameDB Graphics Fixes"), isOn: $enableGameDBHardwareFixes)
                        .disabled(!enabled)
                    Text(settings.localized("If a game looks worse after GameDB, turn off GameDB Graphics Fixes for this game and reset/relaunch it. Core fixes cover timing, clamps, and other compatibility behavior."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                if let statusMessage {
                    Section {
                        Text(statusMessage)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .background(Color(.systemGroupedBackground))
            .navigationTitle(settings.localized("Per-Game Settings"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(settings.localized("Done")) {
                        if let onDone {
                            onDone()
                        } else {
                            dismiss()
                        }
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Save")) {
                        save()
                    }
                }
            }
        }
        .background(Color(.systemGroupedBackground))
    }

    private var displayName: String {
        let name = ((game.name as NSString).deletingPathExtension as String).trimmingCharacters(in: .whitespacesAndNewlines)
        let serial = game.metadata["serial"]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""

        if name.isEmpty {
            return serial.isEmpty ? settings.localized("Current Game") : serial
        }
        if serial.isEmpty {
            return name
        }
        return "\(name) - \(serial)"
    }

    private func save() {
        let normalizedSkipDraw = normalizedSkipDrawValues()
        if skipDrawStart != normalizedSkipDraw.start {
            skipDrawStart = normalizedSkipDraw.start
        }
        if skipDrawEnd != normalizedSkipDraw.end {
            skipDrawEnd = normalizedSkipDraw.end
        }

        if savesToRunningGame {
            ARMSX2Bridge.setGameSettingsForCurrentGame(
                enabled: enabled,
                upscaleMultiplier: upscaleMultiplier,
                aspectRatio: aspectRatio,
                textureFiltering: Int32(textureFiltering),
                hardwareMipmapping: hardwareMipmapping,
                blendingAccuracy: Int32(blendingAccuracy),
                interlaceMode: Int32(interlaceMode),
                trilinearFiltering: Int32(trilinearFiltering),
                halfPixelOffset: Int32(halfPixelOffset),
                roundSprite: Int32(roundSprite),
                alignSpriteOverride: alignSpriteOverride,
                alignSprite: alignSprite,
                mergeSpriteOverride: mergeSpriteOverride,
                mergeSprite: mergeSprite,
                wildArmsOffsetOverride: wildArmsOffsetOverride,
                wildArmsOffset: wildArmsOffset,
                textureOffsetXOverride: textureOffsetXOverride,
                textureOffsetX: Int32(textureOffsetX),
                textureOffsetYOverride: textureOffsetYOverride,
                textureOffsetY: Int32(textureOffsetY),
                skipDrawStartOverride: skipDrawStartOverride,
                skipDrawStart: Int32(normalizedSkipDraw.start),
                skipDrawEndOverride: skipDrawEndOverride,
                skipDrawEnd: Int32(normalizedSkipDraw.end),
                eeCoreType: Int32(eeCoreType),
                mtvu: mtvu,
                enableCheats: enableCheats,
                enablePatches: enablePatches,
                enableGameFixes: enableGameFixes,
                enableGameDBHardwareFixes: enableGameDBHardwareFixes
            )
        } else {
            ARMSX2Bridge.setGameSettings(
                forISO: game.bootName,
                enabled: enabled,
                upscaleMultiplier: upscaleMultiplier,
                aspectRatio: aspectRatio,
                textureFiltering: Int32(textureFiltering),
                hardwareMipmapping: hardwareMipmapping,
                blendingAccuracy: Int32(blendingAccuracy),
                interlaceMode: Int32(interlaceMode),
                trilinearFiltering: Int32(trilinearFiltering),
                halfPixelOffset: Int32(halfPixelOffset),
                roundSprite: Int32(roundSprite),
                alignSpriteOverride: alignSpriteOverride,
                alignSprite: alignSprite,
                mergeSpriteOverride: mergeSpriteOverride,
                mergeSprite: mergeSprite,
                wildArmsOffsetOverride: wildArmsOffsetOverride,
                wildArmsOffset: wildArmsOffset,
                textureOffsetXOverride: textureOffsetXOverride,
                textureOffsetX: Int32(textureOffsetX),
                textureOffsetYOverride: textureOffsetYOverride,
                textureOffsetY: Int32(textureOffsetY),
                skipDrawStartOverride: skipDrawStartOverride,
                skipDrawStart: Int32(normalizedSkipDraw.start),
                skipDrawEndOverride: skipDrawEndOverride,
                skipDrawEnd: Int32(normalizedSkipDraw.end),
                eeCoreType: Int32(eeCoreType),
                mtvu: mtvu,
                enableCheats: enableCheats,
                enablePatches: enablePatches,
                enableGameFixes: enableGameFixes,
                enableGameDBHardwareFixes: enableGameDBHardwareFixes
            )
        }
        statusMessage = enabled ? "\(settings.localized("Saved for")) \(game.metadata["serial"] ?? game.name). \(settings.localized("Reset or relaunch the game to apply."))" : settings.localized("Per-game overrides cleared.")
    }

    private func normalizeSkipDrawRangeIfNeeded() {
        let normalized = normalizedSkipDrawValues()
        if skipDrawStart != normalized.start {
            skipDrawStart = normalized.start
        }
        if skipDrawEnd != normalized.end {
            skipDrawEnd = normalized.end
        }
    }

    private func normalizedSkipDrawValues() -> (start: Int, end: Int) {
        let start = Self.clampedSkipDraw(skipDrawStart)
        let end = Self.normalizedSkipDrawEnd(
            start: start,
            end: skipDrawEnd,
            startOverride: skipDrawStartOverride,
            endOverride: skipDrawEndOverride
        )
        return (start, end)
    }

    private static func normalizedAspect(_ value: String?) -> String {
        switch value {
        case "Stretch", "4:3", "16:9", "10:7":
            return value ?? "Auto 4:3/3:2"
        default:
            return "Auto 4:3/3:2"
        }
    }

    private static func boolValue(_ value: Any?, defaultValue: Bool) -> Bool {
        if let bool = value as? Bool {
            return bool
        }
        if let number = value as? NSNumber {
            return number.boolValue
        }
        return defaultValue
    }

    private static func intValue(_ value: Any?, defaultValue: Int) -> Int {
        if let number = value as? NSNumber {
            return number.intValue
        }
        return defaultValue
    }

    private static func floatValue(_ value: Any?, defaultValue: Float) -> Float {
        if let number = value as? NSNumber {
            return number.floatValue
        }
        return defaultValue
    }

    private static func clampedTextureOffset(_ offset: Int) -> Int {
        min(max(offset, SettingsStore.textureOffsetRange.lowerBound), SettingsStore.textureOffsetRange.upperBound)
    }

    private static func clampedSkipDraw(_ value: Int) -> Int {
        min(max(value, SettingsStore.skipDrawRange.lowerBound), SettingsStore.skipDrawRange.upperBound)
    }

    private static func normalizedSkipDrawEnd(start: Int, end: Int, startOverride: Bool, endOverride: Bool) -> Int {
        let clampedEnd = clampedSkipDraw(end)
        guard startOverride && endOverride else {
            return clampedEnd
        }
        return SettingsStore.normalizedSkipDrawEnd(start: start, end: clampedEnd)
    }
}

private extension String {
    var pathExtensionLabel: String {
        let ext = (self as NSString).pathExtension.uppercased()
        return ext.isEmpty ? "FILE" : ext
    }
}
