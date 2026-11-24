// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import UIKit
import Observation

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

    @State private var appState = AppState.shared
	@State private var settings = SettingsStore.shared
	@State private var logoStore = ARMSX2LogoStore.shared
	@State private var fileImporter = FileImportHandler.shared
	@State private var coverStore = CoverStore.shared
	@State private var externalLibrary = ExternalGameLibrary.shared
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    // controllerInput is nil under touch; this shared one only arbitrates the L2/R2 backdrop gesture.
    @Environment(\.menuControllerInputRouter)
    private var sharedMenuControllerInput
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    // Card text uses fixed point sizes, so this carries Larger Text into them.
    @ScaledMetric(relativeTo: .body) private var dynamicTypeTextScale: CGFloat = 1
    @Environment(\.layoutDirection) private var layoutDirection
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.uiAccentColour) private var accentColour
    @State private var stopConfirmationAnchor: StopConfirmationAnchor?
    @State private var tabBar = MenuTabBarAppearance.shared
    @State private var appearance = GameLibraryAppearance.shared
    @State private var library = GameLibraryModel.shared
    @State private var controller = GameLibraryControllerCoordinator()
    // SettingsStore owns background configuration; the shared container renders it.
    private var hasCustomBackground: Bool {
        settings.appearance.dynamicBackgroundsEnabled
            || settings.appearance.backgroundPrimaryAsset != nil
            || settings.appearance.backgroundLandscapeAsset != nil
    }

    private var hideGamesScreenTitle: Bool {
        appearance.activeHideGamesScreenTitleBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue
    }

    private var useGamesLogo: Bool {
        appearance.activeUseGamesLogoBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue && logoStore.hasLogo
    }

    private var resolvedGameLibraryVerticalPosition: CGFloat {
        // The layouts always sat 13 below SwiftUI's zero; the control still reads zero there.
        let calibratedPosition =
            appearance.activeGameLibraryVerticalPositionBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue + 13
        return CGFloat(
            min(
                max(calibratedPosition, -160),
                160
            )
        )
    }

    private var resolvedGameNameTextScale: CGFloat {
        CGFloat(
            min(max(appearance.activeGameNameTextScaleBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue, 0.75), 1.5)
        ) * dynamicTypeTextScale
    }

    private var resolvedGameInfoTextScale: CGFloat {
        CGFloat(
            min(max(appearance.activeGameInfoTextScaleBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue, 0.75), 1.5)
        ) * dynamicTypeTextScale
    }

    private var resolvedGameCoverCornerRadius: CGFloat {
        CGFloat(min(max(appearance.activeGameCoverCornerRadiusBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue, 0), 28))
    }

    private var resolvedFocusedGameCardScale: CGFloat {
        1.10
    }

    private var resolvedFocusedGameCardLift: CGFloat {
        16
    }

    private var resolvedGameCoverShadowStrength: CGFloat {
        CGFloat(min(max(appearance.activeGameCoverShadowStrengthBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue, 0), 2))
    }

    private var resolvedGameCoverOpacity: Double {
        min(max(appearance.activeGameCoverOpacityBinding(isLandscape: controller.usesLandscapeGameCardScale).wrappedValue, 0.35), 1)
    }

    private var cardStyle: GameLibraryCardStyle {
        let paletteColors = settings.appearance.controllerFocusBoxCustomColor.map {
            [$0.color]
        } ?? settings.appearance.controllerFocusBoxPalette.colors
        let primaryGlow = paletteColors.first ?? accentColour
        return GameLibraryCardStyle(
            resolvedGameCardScale: controller.resolvedGameCardScale,
            resolvedGameCardWidthScale: controller.resolvedGameCardWidthScale,
            resolvedGameCardHeightScale: controller.resolvedGameCardHeightScale,
            resolvedGameCardGapScale: controller.resolvedGameCardGapScale,
            resolvedGameCardContentPaddingScale: controller.resolvedGameCardContentPaddingScale,
            resolvedGameCardVerticalPaddingScale: controller.resolvedGameCardVerticalPaddingScale,
            resolvedGameCardTextSpacingScale: controller.resolvedGameCardTextSpacingScale,
            resolvedGameCardCornerRadius: controller.resolvedGameCardCornerRadius,
            resolvedGameLibraryVerticalPosition: resolvedGameLibraryVerticalPosition,
            resolvedGameNameTextScale: resolvedGameNameTextScale,
            resolvedGameInfoTextScale: resolvedGameInfoTextScale,
            resolvedGameCoverCornerRadius: resolvedGameCoverCornerRadius,
            resolvedGameCoverShadowStrength: resolvedGameCoverShadowStrength,
            resolvedGameCoverOpacity: resolvedGameCoverOpacity,
            resolvedFocusedGameCardScale: resolvedFocusedGameCardScale,
            resolvedFocusedGameCardLift: resolvedFocusedGameCardLift,
            hideGameName: controller.hideGameName,
            hideGameInfo: controller.hideGameInfo,
            hideFavoriteButton: controller.hideFavoriteButton,
            hideRegionFlag: controller.hideRegionFlag,
            hideRunningIndicator: controller.hideRunningIndicator,
            singleLineGameNames: controller.singleLineGameNames,
            runningLabel: settings.localized("Running"),
            externalLabel: settings.localized("External"),
            regionLabel: settings.localized("Region"),
            addToFavoritesLabel: settings.localized("Add to favorites"),
            removeFromFavoritesLabel: settings.localized("Remove from favorites"),
            cardFocusedColor: settings.appearance.controllerCardFocusedColor,
            favoriteGlowEnabled: settings.appearance.favoriteGlowingEffectEnabled,
            glowPrimaryColor: primaryGlow,
            glowSecondaryColor: paletteColors.dropFirst().first ?? primaryGlow
        )
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

        if library.games.isEmpty && controller.displayedRunningGameName == nil {
            return "empty-\(orientation)"
        }
        if appearance.libraryLayout != "grid" {
            return "list-\(orientation)"
        }
        if orientation == "landscape" && appearance.landscapeCoverFlowEnabled {
            return "cover-flow-landscape"
        }
        return "grid-\(orientation)"
    }

    var body: some View {
        // A concrete type before the modifier chain keeps the Release type-check in budget.
        let presentation = OptionalMenuNavigationStack(
            embedded: embeddedInMenuNavigation
        ) {
            ZStack {
                if hasCustomBackground {
                    MenuBackgroundLayer(isActive: menuTabIsActive)
                        .zIndex(0)
                }

                if controller.renameTarget == nil {
                    GeometryReader { geo in
                        // Before the card settings below read the orientation; onChange runs a pass late.
                        let _ = controller.seedLibraryContainerSize(geo.size)
                        Group {
                            if library.games.isEmpty && controller.displayedRunningGameName == nil {
                                GameLibraryEmptyLayout(
                                    controller: controller,
                                    containerSize: geo.size,
                                    controllerInput: controllerInput,
                                    showsPageOwnedLargeTitle: showsPageOwnedLargeTitle,
                                    useGamesLogo: useGamesLogo,
                                    hasCustomBackground: hasCustomBackground
                                )
                            } else if appearance.libraryLayout == "grid" && geo.size.width > geo.size.height && appearance.landscapeCoverFlowEnabled {
                                GameLibraryCoverFlowLayout(
                                    library: library,
                                    controller: controller,
                                    style: cardStyle,
                                    containerSize: geo.size,
                                    controllerInput: controllerInput,
                                    displayedRunningGameName: controller.displayedRunningGameName,
                                    hasCustomBackground: hasCustomBackground
                                )
                            } else if appearance.libraryLayout == "grid" {
                                GameLibraryGridLayout(
                                    library: library,
                                    controller: controller,
                                    style: cardStyle,
                                    containerSize: geo.size,
                                    controllerInput: controllerInput,
                                    displayedRunningGameName: controller.displayedRunningGameName,
                                    showsPageOwnedLargeTitle: showsPageOwnedLargeTitle,
                                    useGamesLogo: useGamesLogo,
                                    hasCustomBackground: hasCustomBackground
                                )
                            } else {
                                GameLibraryListLayout(
                                    library: library,
                                    controller: controller,
                                    style: cardStyle,
                                    controllerInput: controllerInput,
                                    displayedRunningGameName: controller.displayedRunningGameName,
                                    showsPageOwnedLargeTitle: showsPageOwnedLargeTitle,
                                    useGamesLogo: useGamesLogo,
                                    hasCustomBackground: hasCustomBackground
                                )
#if targetEnvironment(macCatalyst)
                                .listStyle(.inset)
#endif
                            }
                        }
                        // A stable identity per layout, so rotation never reuses the old scroll offset.
                        .id(libraryPresentationIdentity(for: geo.size))
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .onAppear {
                            pushControllerContext()
                            controller.handleLibraryContainerAppear(geo: geo)
                        }
                        .onChange(of: geo.size) { previousSize, size in
                            pushControllerContext()
                            controller.handleLibraryContainerSizeChange(previousSize, size)
                        }
                    }
                    // Otherwise the Cover Source keyboard shortens the library and portrait switches to cover flow.
                    .ignoresSafeArea(.keyboard)
                    // One persistent glass sampling group for the cards, outside the orb field.
                    .stableMenuContentGlassContainer()
                    .zIndex(10)
                }

            }
            .environment(\.stopConfirmationPresented, controller.showStopAlert)
            .onPreferenceChange(StopConfirmationAnchorKey.self) { stopConfirmationAnchor = $0 }
            .overlay {
                if controller.controllerLayoutMenuPresented {
                    ControllerNavigationAlert(
                        title: settings.localized("Library Layout"),
                        dimsBackground: false,
                        message: "",
                        actions: controller.controllerLayoutMenuActions,
                        selectedIndex: controller.controllerLayoutMenuSelectedIndex,
                        onSelect: controller.performControllerLayoutMenuAction,
                        onDismiss: {
                            controller.dismissControllerLayoutMenu()
                        }
                    )
                    .zIndex(20_000)
                }
                if controller.controllerMoreMenuPresented {
                    ControllerNavigationAlert(
                        title: settings.localized("More"),
                        titleColour: settings.appearance.controllerContextMenuColor,
                        contentColour: settings.appearance.controllerContextMenuColor,
                        secondaryContentColour:
                            settings.appearance.controllerContextMenuSecondaryColor,
                        usesClearGlass: false,
                        showsActionButtonBackgrounds: true,
                        dimsBackground: false,
                        message: "",
                        actions: controller.controllerMoreMenuActions,
                        selectedIndex: controller.controllerMoreMenuSelectedIndex,
                        onSelect: controller.performControllerMoreMenuAction,
                        onDismiss: {
                            controller.dismissControllerMoreMenu()
                        }
                    )
                    .ignoresSafeArea(.container, edges: .all)
                    .zIndex(20_000)
                }
            }
            .overlay {
                if controller.activeControllerAlertKind == .stop {
                    StopGameConfirmation(
                        gameTitle: controller.displayTitle(forRunningName: controller.displayedRunningGameName ?? ""),
                        anchor: stopConfirmationAnchor,
                        selectedIndex: controller.controllerAlertSelectedIndex,
                        onSelect: { controller.performControllerAlertAction($0, for: .stop) },
                        onDismiss: { controller.dismissControllerAlert(.stop) }
                    )
                } else if let kind = controller.activeControllerAlertKind,
                   kind != .automaticCustomSkin {
                    ControllerNavigationAlert(
                        title: controller.controllerAlertTitle(for: kind),
                        titleColour:
                            kind == .restart || kind == .automaticCustomSkin
                                ? .white
                                : nil,
                        contentColour:
                            kind == .automaticCustomSkin ? .white : nil,
                        previewImageURL:
                            controller.controllerAlertPreviewImageURL(for: kind),
                        previewSkinDescriptor:
                            controller.controllerAlertPreviewSkinDescriptor(for: kind),
                        previewAccessibilityLabel:
                            controller.controllerAlertPreviewAccessibilityLabel(for: kind),
                        details: controller.controllerAlertDetails(for: kind),
                        emphasizesPreview: kind == .automaticCustomSkin,
                        prefersTopPlacement:
                            kind == .automaticCustomSkin,
                        dimsBackground: false,
                        message: controller.controllerAlertMessage(for: kind),
                        actions: controller.controllerAlertActions(for: kind),
                        selectedIndex: controller.controllerAlertSelectedIndex,
                        onSelect: { index in
                            controller.performControllerAlertAction(index, for: kind)
                        },
                        onDismiss: {
                            if kind == .automaticCustomSkin {
                                controller.cancelAutomaticCustomSkinLaunch(
                                    playsReturnSound: true
                                )
                            } else {
                                controller.dismissControllerAlert(kind)
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
                        focusState: controller.controllerFocusState,
                        libraryVisible: menuTabIsActive,
                        onCommand: controller.handleControllerCommand,
                        onLibraryEntry: controller.handleControllerLibraryEntry,
                        onFocusRelease: controller.releaseControllerFocus,
                        onRightStickScrollEnded:
                            controller.restoreCenteredControllerFocusAfterRightStickScrolling,
                        onInputAvailabilityChanged:
                            controller.controllerInputAvailabilityDidChange
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
                    GameLibraryToolbar(
                        coordinator: controller,
                        library: library,
                        appearance: appearance,
                        coverStore: coverStore,
                        settings: settings,
                        logoStore: logoStore,
                        useGamesLogo: useGamesLogo,
                        libraryIsEmpty: library.games.isEmpty,
                        accentColour: accentColour,
                        controllerInput: controllerInput,
                        menuTabIsActive: menuTabIsActive
                    )
                }
            }
            // Tints UIKit's compact overflow ellipsis like the drawn toolbar symbols.
            .tint(accentColour)
            .alert(settings.localized("Cover Source"), isPresented: $controller.showCoverTemplateEditor) {
                TextField("https://.../${serial}.jpg", text: $controller.coverTemplateDraft)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Save")) {
                    if coverStore.saveCoverURLTemplate(
                        controller.coverTemplateDraft,
                        libraryIsEmpty: library.games.isEmpty
                    ) {
                        library.downloadMissingCovers()
                    }
                }
            } message: {
                Text("Use ${serial}, ${title}, or ${filetitle}. Default: \(CoverStore.defaultCoverURLTemplate)")
            }
				.sheet(isPresented: $controller.showGameImporter) {
					ImportDocumentPicker(
						allowedContentTypes: [.item, .data, .content],
						allowsMultipleSelection: true
	                ) { result in
                    controller.showGameImporter = false
                    switch result {
                    case .success(let urls):
                        controller.prepareGameImport(urls)
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            fileImporter.presentImportResult(FileImportHandler.failedGamePickerMessage(errorDescription: error.localizedDescription))
                        }
					}
				}
			}
			.sheet(isPresented: $controller.showCoverImporter) {
				ImportDocumentPicker(
					allowedContentTypes: CoverStore.coverContentTypes,
                    allowsMultipleSelection: controller.pendingCoverGameName == nil
                ) { result in
                    controller.showCoverImporter = false
                    switch result {
                    case .success(let urls):
                        coverStore.importCoverURLs(urls, forGameNamed: controller.pendingCoverGameName)
                        controller.pendingCoverGameName = nil
                        library.loadGames()
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            coverStore.lastCoverMessage = "Cover import failed: \(error.localizedDescription)"
                            coverStore.showCoverAlert = true
                        }
                        controller.pendingCoverGameName = nil
                    }
                }
            }
            .photosPicker(
                isPresented: $controller.showCoverPhotoPicker,
                selection: $controller.selectedCoverPhotoItem,
                matching: .images
            )
            .onChange(of: controller.selectedCoverPhotoItem) { _, photoItem in
                guard let photoItem, let gameName = controller.pendingCoverPhotoGameName else { return }
                controller.selectedCoverPhotoItem = nil
                controller.pendingCoverPhotoGameName = nil
                controller.importCoverPhoto(photoItem, forGameNamed: gameName)
            }
            .fullScreenCover(item: $controller.gameInfoTarget) { game in
                GameLibraryForegroundPanel(
                    maximumWidth: 620,
                    maximumHeight: 700,
                    onDismiss: { controller.gameInfoTarget = nil }
                ) {
                    GameInfoPanel(game: game)
                }
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
            .fullScreenCover(isPresented: $controller.gameLibraryViewOptionsPresented) {
                GameLibraryViewOptionsCover(
                    controller: controller,
                    appearance: appearance,
                    tabBar: tabBar,
                    settings: settings,
                    logoStore: logoStore,
                    controllerInput: controllerInput
                )
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
            // Scene level, since a page overlay cannot outrank the navigation chrome or tab bar.
            .fullScreenCover(
                isPresented: Binding(
                    get: { controller.controllerContextMenuState.gameID != nil },
                    set: { isPresented in
                        if !isPresented,
                           controller.controllerContextMenuState.gameID != nil {
                            controller.closeControllerContextMenu(playsFeedback: false)
                        }
                    }
                ),
                onDismiss: controller.completeDeferredControllerContextMenuSelection
            ) {
                GameLibraryContextMenuCover(
                    controller: controller,
                    library: library,
                    controllerInput: controllerInput
                )
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
            // Scene level for the same reason as the context menu.
            .fullScreenCover(
                isPresented: Binding(
                    get: { controller.pendingAutomaticCustomSkinLaunch != nil },
                    set: { isPresented in
                        if !isPresented,
                           controller.pendingAutomaticCustomSkinLaunch != nil {
                            controller.cancelAutomaticCustomSkinLaunch()
                        }
                    }
                )
            ) {
                if let kind = controller.activeControllerAlertKind,
                   kind == .automaticCustomSkin {
                    GameLibraryAutomaticSkinCover(
                        controller: controller,
                        settings: settings,
                        kind: kind
                    )
                    .presentationBackground(.clear)
                    .appStatusBarHidden()
                }
            }
            .fullScreenCover(
                item: $controller.gameSettingsTarget,
                onDismiss: {
                    controller.gameSettingsStartsInShaders = false
                    controller.releasePreparedGameSettings()
                }
            ) { game in
                LibraryPerGameSettingsOverlay(
                    game: game,
                    preloadedSettings: controller.preparedGameSettingsID == game.id
                        ? controller.preparedGameSettings
                        : nil,
                    initiallySelectsShaders: controller.gameSettingsStartsInShaders,
                    controllerInput: controllerInput,
                    onDone: {
                        controller.gameSettingsTarget = nil
                        controller.gameSettingsStartsInShaders = false
                        controller.releasePreparedGameSettings()
                    }
                )
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
            .fullScreenCover(item: $controller.renameTarget) { game in
                OrbitKeysKeyboardView(
                    title: settings.localized("Rename"),
                    initialText: game.displayName,
                    game: game,
                    startsInNormalKeyboard: true,
                    onCommit: { name in
                        library.renameGame(game, to: name)
                        controller.renameTarget = nil
                    },
                    onCancel: {
                        controller.renameTarget = nil
                    }
                )
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
            .fullScreenCover(item: $controller.discLinkTarget) { game in
                GameLibraryForegroundPanel(
                    maximumWidth: 620,
                    maximumHeight: 680,
                    onDismiss: { controller.discLinkTarget = nil }
                ) {
                    DiscLinkPicker(
                        discs: library.games.filter { !$0.isELF && $0.id != game.id },
                        hasLinkedDisc: ARMSX2Bridge.linkedDiscPath(forELF: game.bootName)?.isEmpty == false
                    ) { selected in
                        library.setLinkedDisc(selected, for: game)
                    }
                }
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
            .fullScreenCover(item: $controller.cheatsManagerTarget) { game in
                GameLibraryForegroundPanel(
                    maximumWidth: 980,
                    maximumHeight: 700,
                    onDismiss: { controller.cheatsManagerTarget = nil }
                ) {
                    CheatsPatchesManagerView(
                        isoName: game.bootName,
                        gameTitle: game.name,
                        launchContext: .library,
                        controllerInput: controllerInput
                    )
                }
                .presentationBackground(.clear)
                .appStatusBarHidden()
            }
			}
        let contextObservers = presentation
        .onChange(of: controllerInput.map(ObjectIdentifier.init), initial: true) { pushControllerContext() }
        .onChange(of: menuTabIsActive, initial: true) { pushControllerContext() }
        .onChange(of: reduceMotion, initial: true) { pushControllerContext() }
        .onChange(of: layoutDirection, initial: true) { pushControllerContext() }
        .onChange(of: sharedMenuControllerInput.map(ObjectIdentifier.init), initial: true) { pushControllerContext() }
        let libraryObservers = contextObservers
        .onAppear {
            pushControllerContext()
            controller.handleLibraryAppear()
        }
        .onChange(of: controller.renameTarget != nil, initial: true) { _, isPresented in
            onRenamePresentationChanged(isPresented)
        }
        .onChange(of: controllerInput?.navigationZone) { _, zone in
            pushControllerContext()
            controller.handleNavigationZoneChange(zone)
        }
        .onChange(of: controller.controllerContextMenuState.gameID) { _, gameID in
            pushControllerContext()
            controller.handleContextMenuGameChange(gameID)
        }
        .onChange(of: controller.controllerToolbarFocusState.selectedAction) { _, _ in
            pushControllerContext()
            controller.handleToolbarSelectionChange()
        }
        .onReceive(
            NotificationCenter.default.publisher(for: ExternalGameLibrary.didChangeNotification)
        ) { _ in library.handleExternalLibraryDidChange() }
        .onReceive(
            NotificationCenter.default.publisher(for: InitialContentBootstrap.didChangeNotification)
        ) { _ in library.handleInitialContentDidChange() }
        .onReceive(
            NotificationCenter.default.publisher(for: .ARMSX2iOSReturnToMenu)
        ) { _ in
            pushControllerContext()
            controller.handleReturnToMenu()
        }

        return libraryObservers
        .onChange(of: library.games) { _, updatedGames in
            pushControllerContext()
            controller.handleGamesChange(updatedGames)
        }
        .onChange(of: controller.displayedRunningGameName) { _, runningGame in
            pushControllerContext()
            controller.handleRunningGameChange(runningGame)
        }
        .onChange(of: controllerInput?.hasConnectedController) { _, connected in
            pushControllerContext()
            controller.handleControllerConnectionChange(connected)
        }
        .onChange(of: menuTabIsActive) { _, active in
            pushControllerContext()
            controller.handleMenuTabActiveChange(active)
        }
        .onChange(of: controller.manuallyCapturedControllerPresentationActive) { _, active in
            pushControllerContext()
            controller.handleCapturedPresentationChange(active)
        }
        .onChange(of: controller.controllerNavigationPresentationActive) { _, active in
            pushControllerContext()
            controller.handleNavigationPresentationChange(active)
        }
        .onChange(of: controller.activeControllerAlertKind) { previous, kind in
            pushControllerContext()
            controller.handleAlertKindChange(previous, kind)
        }
        .onDisappear {
            GameCoverThemePreviewStore.shared.clear()
            onRenamePresentationChanged(false)
            pushControllerContext()
            controller.handleLibraryDisappear()
        }
    }

    // The coordinator cannot read the environment, so the view hands it over before each call.
    private func pushControllerContext() {
        controller.controllerInput = controllerInput
        controller.menuTabIsActive = menuTabIsActive
        controller.reduceMotion = reduceMotion
        controller.layoutDirection = layoutDirection
        controller.sharedMenuControllerInput = sharedMenuControllerInput
    }

    /// Last path component with its disc extension (`.iso`, `.bin`, `.cue`, …) removed,
    /// used when no matching library entry is available.
    static func cleanGameFileName(_ value: String) -> String {
        let fileName = (value as NSString).lastPathComponent
        guard !fileName.isEmpty else { return value }
        return (fileName as NSString).deletingPathExtension
    }

}
