// GameLibraryCoverContents.swift — what the library's view options, context menu and skin chooser covers show
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryViewOptionsCover: View {
    let controller: GameLibraryControllerCoordinator
    let appearance: GameLibraryAppearance
    let tabBar: MenuTabBarAppearance
    let settings: SettingsStore
    let logoStore: ARMSX2LogoStore
    let controllerInput: MenuControllerInputRouter?

    var body: some View {
        ZStack {
            GameLibraryForegroundPanel(
                maximumWidth: 520,
                maximumHeight: 600,
                isContentVisible:
                    controller.gameLibraryViewOptionsPreview == nil,
                onDismiss: controller.applyGameLibraryViewOptions
            ) {
                GameLibraryViewOptionsPanel(
                    controllerInput: controllerInput,
                    orientationTitle: settings.localized(
                        controller.usesLandscapeGameCardScale
                            ? "Landscape"
                            : "Portrait"
                    ),
                    cardScale: appearance.activeGameCardScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    cardWidthScale: appearance.activeGameCardWidthScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    cardHeightScale: appearance.activeGameCardHeightScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    libraryVerticalPosition:
                        appearance.activeGameLibraryVerticalPositionBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    gapScale: appearance.activeGameCardGapScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    contentPaddingScale:
                        appearance.activeGameCardContentPaddingScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    verticalPaddingScale:
                        appearance.activeGameCardVerticalPaddingScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    textSpacingScale:
                        appearance.activeGameCardTextSpacingScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    gameNameTextScale:
                        appearance.activeGameNameTextScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    gameInfoTextScale:
                        appearance.activeGameInfoTextScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    cardCornerRadius: appearance.activeGameCardCornerRadiusBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    coverCornerRadius: appearance.activeGameCoverCornerRadiusBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    coverShadowStrength:
                        appearance.activeGameCoverShadowStrengthBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    coverOpacity: appearance.activeGameCoverOpacityBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    bottomNavigationTabBarHeight:
                        tabBar.activeBottomNavigationTabBarHeightBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    bottomNavigationTabBarIconScale:
                        tabBar.activeBottomNavigationTabBarIconScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    bottomNavigationTabBarLabelScale:
                        tabBar.activeBottomNavigationTabBarLabelScaleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    bottomNavigationTabBarClearance:
                        tabBar.activeBottomNavigationTabBarClearanceBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    bottomNavigationTabBarShowsLabels:
                        tabBar.activeBottomNavigationTabBarShowsLabelsBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    hideGamesScreenTitle:
                        appearance.activeHideGamesScreenTitleBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    useGamesLogo: appearance.activeUseGamesLogoBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    logoAvailable: logoStore.hasLogo,
                    hideGameName: appearance.activeHideGameNameBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    hideGameInfo: appearance.activeHideGameInfoBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    hideFavoriteButton:
                        appearance.activeHideFavoriteButtonBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    hideRegionFlag: appearance.activeHideRegionFlagBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    singleLineGameNames:
                        appearance.activeSingleLineGameNamesBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    hideRunningIndicator:
                        appearance.activeHideRunningIndicatorBinding(isLandscape: controller.usesLandscapeGameCardScale),
                    onPreviewChanged: {
                        controller.gameLibraryViewOptionsPreview = $0
                    },
                    onReset: {
                        appearance.resetActiveGameLibraryViewOptions(
                            isLandscape: controller.usesLandscapeGameCardScale
                        )
                    },
                    onCancel: controller.cancelGameLibraryViewOptions,
                    onApply: controller.applyGameLibraryViewOptions
                )
            }

            if let preview = controller.gameLibraryViewOptionsPreview {
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
        .onDisappear { controller.gameLibraryViewOptionsPreview = nil }
    }
}

struct GameLibraryContextMenuCover: View {
    let controller: GameLibraryControllerCoordinator
    let library: GameLibraryModel
    let controllerInput: MenuControllerInputRouter?

    var body: some View {
        GameLibraryControllerContextMenuPresenter(
            state: controller.controllerContextMenuState
        ) { gameID, section, selectedAction in
            if let game = library.games.first(where: { $0.id == gameID }) {
                ControllerGameContextMenu(
                    game: game,
                    controllerInput: controllerInput,
                    sectionTitle: controller.controllerContextMenuTitle(
                        for: section
                    ),
                    items: controller.controllerContextMenuItems(
                        for: game,
                        section: section
                    ),
                    selectedAction: selectedAction,
                    isGameRunning: library.isRunning(game),
                    onPlayOrStop: {
                        controller.deferControllerContextMenuSelection(
                            .playOrStop,
                            for: game
                        )
                    },
                    onSelect: { action in
                        controller.performControllerContextMenuAction(
                            action,
                            for: game
                        )
                    },
                    onDismiss: {
                        controller.closeControllerContextMenu()
                    }
                )
                .isolatedMenuGlassContainer()
            }
        }
    }
}

struct GameLibraryAutomaticSkinCover: View {
    let controller: GameLibraryControllerCoordinator
    let settings: SettingsStore
    let kind: GameLibraryControllerAlertKind

    var body: some View {
        let usesDefaultText = settings.controllerUIThemePreset == .defaultTheme
        ControllerNavigationAlert(
            title: controller.controllerAlertTitle(for: kind),
            titleColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
            contentColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
            secondaryContentColour: usesDefaultText ? .white : settings.controllerContextMenuSecondaryColor,
            detailValueColour: usesDefaultText
                ? settings.controllerNavigationAccentColor
                : nil,
            previewImageURL:
                controller.controllerAlertPreviewImageURL(for: kind),
            previewSkinDescriptor:
                controller.controllerAlertPreviewSkinDescriptor(for: kind),
            previewAccessibilityLabel:
                controller.controllerAlertPreviewAccessibilityLabel(for: kind),
            details: controller.controllerAlertDetails(for: kind),
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
            message: controller.controllerAlertMessage(for: kind),
            actions: controller.controllerAlertActions(for: kind),
            selectedIndex: controller.controllerAlertSelectedIndex,
            onSelect: { index in
                controller.performControllerAlertAction(index, for: kind)
            },
            onDismiss: {
                controller.cancelAutomaticCustomSkinLaunch(
                    playsReturnSound: true
                )
            }
        )
        .onAppear {
            // A focused library row can survive under the cover, so the
            // chooser always starts at its first control.
            controller.controllerAlertSelectedIndex = 0
        }
        .contextMenuPanelTextAppearance()
    }
}
