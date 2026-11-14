// GameLibraryAppearance+ViewOptions.swift — The View Options values and the theme document's keys
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryViewOptionsValues {
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

extension GameLibraryAppearance {
    func snapshot(isLandscape: Bool) -> GameLibraryViewOptionsValues {
        let tabBar = MenuTabBarAppearance.shared
        return GameLibraryViewOptionsValues(
            cardScale: activeGameCardScaleBinding(isLandscape: isLandscape).wrappedValue,
            cardWidthScale: activeGameCardWidthScaleBinding(isLandscape: isLandscape).wrappedValue,
            cardHeightScale: activeGameCardHeightScaleBinding(isLandscape: isLandscape).wrappedValue,
            libraryVerticalPosition:
                activeGameLibraryVerticalPositionBinding(isLandscape: isLandscape).wrappedValue,
            gapScale: activeGameCardGapScaleBinding(isLandscape: isLandscape).wrappedValue,
            contentPaddingScale:
                activeGameCardContentPaddingScaleBinding(isLandscape: isLandscape).wrappedValue,
            verticalPaddingScale:
                activeGameCardVerticalPaddingScaleBinding(isLandscape: isLandscape).wrappedValue,
            textSpacingScale:
                activeGameCardTextSpacingScaleBinding(isLandscape: isLandscape).wrappedValue,
            gameNameTextScale: activeGameNameTextScaleBinding(isLandscape: isLandscape).wrappedValue,
            gameInfoTextScale: activeGameInfoTextScaleBinding(isLandscape: isLandscape).wrappedValue,
            cardCornerRadius:
                activeGameCardCornerRadiusBinding(isLandscape: isLandscape).wrappedValue,
            coverCornerRadius:
                activeGameCoverCornerRadiusBinding(isLandscape: isLandscape).wrappedValue,
            coverShadowStrength:
                activeGameCoverShadowStrengthBinding(isLandscape: isLandscape).wrappedValue,
            coverOpacity: activeGameCoverOpacityBinding(isLandscape: isLandscape).wrappedValue,
            bottomNavigationTabBarHeight:
                tabBar.activeBottomNavigationTabBarHeightBinding(isLandscape: isLandscape).wrappedValue,
            bottomNavigationTabBarIconScale:
                tabBar.activeBottomNavigationTabBarIconScaleBinding(isLandscape: isLandscape).wrappedValue,
            bottomNavigationTabBarLabelScale:
                tabBar.activeBottomNavigationTabBarLabelScaleBinding(isLandscape: isLandscape).wrappedValue,
            bottomNavigationTabBarClearance:
                tabBar.activeBottomNavigationTabBarClearanceBinding(isLandscape: isLandscape).wrappedValue,
            bottomNavigationTabBarShowsLabels:
                tabBar.activeBottomNavigationTabBarShowsLabelsBinding(isLandscape: isLandscape).wrappedValue,
            hideGamesScreenTitle:
                activeHideGamesScreenTitleBinding(isLandscape: isLandscape).wrappedValue,
            useGamesLogo: activeUseGamesLogoBinding(isLandscape: isLandscape).wrappedValue,
            hideGameName: activeHideGameNameBinding(isLandscape: isLandscape).wrappedValue,
            hideGameInfo: activeHideGameInfoBinding(isLandscape: isLandscape).wrappedValue,
            hideFavoriteButton:
                activeHideFavoriteButtonBinding(isLandscape: isLandscape).wrappedValue,
            hideRegionFlag: activeHideRegionFlagBinding(isLandscape: isLandscape).wrappedValue,
            singleLineGameNames:
                activeSingleLineGameNamesBinding(isLandscape: isLandscape).wrappedValue,
            hideRunningIndicator:
                activeHideRunningIndicatorBinding(isLandscape: isLandscape).wrappedValue
        )
    }

    func restore(_ values: GameLibraryViewOptionsValues, isLandscape: Bool) {
        let tabBar = MenuTabBarAppearance.shared
        activeGameCardScaleBinding(isLandscape: isLandscape).wrappedValue = values.cardScale
        activeGameCardWidthScaleBinding(isLandscape: isLandscape).wrappedValue = values.cardWidthScale
        activeGameCardHeightScaleBinding(isLandscape: isLandscape).wrappedValue = values.cardHeightScale
        activeGameLibraryVerticalPositionBinding(isLandscape: isLandscape).wrappedValue =
            values.libraryVerticalPosition
        activeGameCardGapScaleBinding(isLandscape: isLandscape).wrappedValue = values.gapScale
        activeGameCardContentPaddingScaleBinding(isLandscape: isLandscape).wrappedValue =
            values.contentPaddingScale
        activeGameCardVerticalPaddingScaleBinding(isLandscape: isLandscape).wrappedValue =
            values.verticalPaddingScale
        activeGameCardTextSpacingScaleBinding(isLandscape: isLandscape).wrappedValue =
            values.textSpacingScale
        activeGameNameTextScaleBinding(isLandscape: isLandscape).wrappedValue = values.gameNameTextScale
        activeGameInfoTextScaleBinding(isLandscape: isLandscape).wrappedValue = values.gameInfoTextScale
        activeGameCardCornerRadiusBinding(isLandscape: isLandscape).wrappedValue =
            values.cardCornerRadius
        activeGameCoverCornerRadiusBinding(isLandscape: isLandscape).wrappedValue =
            values.coverCornerRadius
        activeGameCoverShadowStrengthBinding(isLandscape: isLandscape).wrappedValue =
            values.coverShadowStrength
        activeGameCoverOpacityBinding(isLandscape: isLandscape).wrappedValue = values.coverOpacity
        tabBar.activeBottomNavigationTabBarHeightBinding(isLandscape: isLandscape).wrappedValue =
            values.bottomNavigationTabBarHeight
        tabBar.activeBottomNavigationTabBarIconScaleBinding(isLandscape: isLandscape).wrappedValue =
            values.bottomNavigationTabBarIconScale
        tabBar.activeBottomNavigationTabBarLabelScaleBinding(isLandscape: isLandscape).wrappedValue =
            values.bottomNavigationTabBarLabelScale
        tabBar.activeBottomNavigationTabBarClearanceBinding(isLandscape: isLandscape).wrappedValue =
            values.bottomNavigationTabBarClearance
        tabBar.activeBottomNavigationTabBarShowsLabelsBinding(isLandscape: isLandscape).wrappedValue =
            values.bottomNavigationTabBarShowsLabels
        activeHideGamesScreenTitleBinding(isLandscape: isLandscape).wrappedValue =
            values.hideGamesScreenTitle
        activeUseGamesLogoBinding(isLandscape: isLandscape).wrappedValue = values.useGamesLogo
        activeHideGameNameBinding(isLandscape: isLandscape).wrappedValue = values.hideGameName
        activeHideGameInfoBinding(isLandscape: isLandscape).wrappedValue = values.hideGameInfo
        activeHideFavoriteButtonBinding(isLandscape: isLandscape).wrappedValue =
            values.hideFavoriteButton
        activeHideRegionFlagBinding(isLandscape: isLandscape).wrappedValue = values.hideRegionFlag
        activeSingleLineGameNamesBinding(isLandscape: isLandscape).wrappedValue =
            values.singleLineGameNames
        activeHideRunningIndicatorBinding(isLandscape: isLandscape).wrappedValue =
            values.hideRunningIndicator
    }

    func resetActiveGameLibraryViewOptions(isLandscape: Bool) {
        let tabBar = MenuTabBarAppearance.shared
        activeGameCardScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCardWidthScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCardHeightScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameLibraryVerticalPositionBinding(isLandscape: isLandscape).wrappedValue = 0
        activeGameCardGapScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCardContentPaddingScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCardVerticalPaddingScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCardTextSpacingScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameNameTextScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameInfoTextScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCardCornerRadiusBinding(isLandscape: isLandscape).wrappedValue = 18
        activeGameCoverCornerRadiusBinding(isLandscape: isLandscape).wrappedValue = 10
        activeGameCoverShadowStrengthBinding(isLandscape: isLandscape).wrappedValue = 1
        activeGameCoverOpacityBinding(isLandscape: isLandscape).wrappedValue = 1
        tabBar.activeBottomNavigationTabBarHeightBinding(isLandscape: isLandscape).wrappedValue = 84
        tabBar.activeBottomNavigationTabBarIconScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        tabBar.activeBottomNavigationTabBarLabelScaleBinding(isLandscape: isLandscape).wrappedValue = 1
        tabBar.activeBottomNavigationTabBarClearanceBinding(isLandscape: isLandscape).wrappedValue = 0
        tabBar.activeBottomNavigationTabBarShowsLabelsBinding(isLandscape: isLandscape).wrappedValue = true
        activeHideGamesScreenTitleBinding(isLandscape: isLandscape).wrappedValue = true
        activeUseGamesLogoBinding(isLandscape: isLandscape).wrappedValue = true
        activeHideGameNameBinding(isLandscape: isLandscape).wrappedValue = false
        activeHideGameInfoBinding(isLandscape: isLandscape).wrappedValue = false
        activeHideFavoriteButtonBinding(isLandscape: isLandscape).wrappedValue = false
        activeHideRegionFlagBinding(isLandscape: isLandscape).wrappedValue = false
        activeSingleLineGameNamesBinding(isLandscape: isLandscape).wrappedValue = false
        activeHideRunningIndicatorBinding(isLandscape: isLandscape).wrappedValue = false
    }

    // Theme documents keep the stored tri-state Ints as Bools; the store converts at its boundary.
    private static let themeDoubles: [String: ReferenceWritableKeyPath<GameLibraryAppearance, Double>] = [
        "ARMSX2iOSExperimentalPortraitGameCardScale": \.portraitGameCardScale,
        "ARMSX2iOSExperimentalLandscapeGameCardScale": \.landscapeGameCardScale,
        "ARMSX2iOSExperimentalPortraitGameCardWidthScale": \.portraitGameCardWidthScale,
        "ARMSX2iOSExperimentalLandscapeGameCardWidthScale": \.landscapeGameCardWidthScale,
        "ARMSX2iOSExperimentalPortraitGameCardHeightScale": \.portraitGameCardHeightScale,
        "ARMSX2iOSExperimentalLandscapeGameCardHeightScale": \.landscapeGameCardHeightScale,
        "ARMSX2iOSPortraitGameLibraryVerticalPosition": \.portraitGameLibraryVerticalPosition,
        "ARMSX2iOSLandscapeGameLibraryVerticalPosition": \.landscapeGameLibraryVerticalPosition,
        "ARMSX2iOSExperimentalPortraitGameCardGapScale": \.portraitGameCardGapScale,
        "ARMSX2iOSExperimentalLandscapeGameCardGapScale": \.landscapeGameCardGapScale,
        "ARMSX2iOSExperimentalPortraitGameCardContentPaddingScale": \.portraitGameCardContentPaddingScale,
        "ARMSX2iOSExperimentalLandscapeGameCardContentPaddingScale": \.landscapeGameCardContentPaddingScale,
        "ARMSX2iOSExperimentalPortraitGameCardVerticalPaddingScale": \.portraitGameCardVerticalPaddingScale,
        "ARMSX2iOSExperimentalLandscapeGameCardVerticalPaddingScale": \.landscapeGameCardVerticalPaddingScale,
        "ARMSX2iOSExperimentalPortraitGameCardTextSpacingScale": \.portraitGameCardTextSpacingScale,
        "ARMSX2iOSExperimentalLandscapeGameCardTextSpacingScale": \.landscapeGameCardTextSpacingScale,
        "ARMSX2iOSExperimentalPortraitGameNameTextScale": \.portraitGameNameTextScale,
        "ARMSX2iOSExperimentalLandscapeGameNameTextScale": \.landscapeGameNameTextScale,
        "ARMSX2iOSExperimentalPortraitGameInfoTextScale": \.portraitGameInfoTextScale,
        "ARMSX2iOSExperimentalLandscapeGameInfoTextScale": \.landscapeGameInfoTextScale,
        "ARMSX2iOSExperimentalPortraitGameCardCornerRadius": \.portraitGameCardCornerRadius,
        "ARMSX2iOSExperimentalLandscapeGameCardCornerRadius": \.landscapeGameCardCornerRadius,
        "ARMSX2iOSExperimentalPortraitGameCoverCornerRadius": \.portraitGameCoverCornerRadius,
        "ARMSX2iOSExperimentalLandscapeGameCoverCornerRadius": \.landscapeGameCoverCornerRadius,
        "ARMSX2iOSExperimentalPortraitGameCoverShadow": \.portraitGameCoverShadowStrength,
        "ARMSX2iOSExperimentalLandscapeGameCoverShadow": \.landscapeGameCoverShadowStrength,
        "ARMSX2iOSExperimentalPortraitGameCoverOpacity": \.portraitGameCoverOpacity,
        "ARMSX2iOSExperimentalLandscapeGameCoverOpacity": \.landscapeGameCoverOpacity,
    ]
    private static let themeTriStates: [String: ReferenceWritableKeyPath<GameLibraryAppearance, Int>] = [
        "ARMSX2iOSPortraitUseGamesLogo": \.portraitUseGamesLogo,
        "ARMSX2iOSLandscapeUseGamesLogo": \.landscapeUseGamesLogo,
        "ARMSX2iOSPortraitHideGamesScreenTitle": \.portraitHideGamesScreenTitle,
        "ARMSX2iOSLandscapeHideGamesScreenTitle": \.landscapeHideGamesScreenTitle,
        "ARMSX2iOSPortraitHideGameName": \.portraitHideGameName,
        "ARMSX2iOSLandscapeHideGameName": \.landscapeHideGameName,
        "ARMSX2iOSPortraitHideGameInfo": \.portraitHideGameInfo,
        "ARMSX2iOSLandscapeHideGameInfo": \.landscapeHideGameInfo,
        "ARMSX2iOSPortraitHideFavoriteButton": \.portraitHideFavoriteButton,
        "ARMSX2iOSLandscapeHideFavoriteButton": \.landscapeHideFavoriteButton,
        "ARMSX2iOSPortraitHideRegionFlag": \.portraitHideRegionFlag,
        "ARMSX2iOSLandscapeHideRegionFlag": \.landscapeHideRegionFlag,
        "ARMSX2iOSPortraitSingleLineGameNames": \.portraitSingleLineGameNames,
        "ARMSX2iOSLandscapeSingleLineGameNames": \.landscapeSingleLineGameNames,
        "ARMSX2iOSPortraitHideRunningIndicator": \.portraitHideRunningIndicator,
        "ARMSX2iOSLandscapeHideRunningIndicator": \.landscapeHideRunningIndicator,
    ]
    private static let tabBarThemeDoubles: [String: ReferenceWritableKeyPath<MenuTabBarAppearance, Double>] = [
        "ARMSX2iOSPortraitBottomNavigationTabBarHeight": \.portraitBottomNavigationTabBarHeight,
        "ARMSX2iOSLandscapeBottomNavigationTabBarHeight": \.landscapeBottomNavigationTabBarHeight,
        "ARMSX2iOSPortraitBottomNavigationTabBarIconScale": \.portraitBottomNavigationTabBarIconScale,
        "ARMSX2iOSLandscapeBottomNavigationTabBarIconScale": \.landscapeBottomNavigationTabBarIconScale,
        "ARMSX2iOSPortraitBottomNavigationTabBarLabelScale": \.portraitBottomNavigationTabBarLabelScale,
        "ARMSX2iOSLandscapeBottomNavigationTabBarLabelScale": \.landscapeBottomNavigationTabBarLabelScale,
        "ARMSX2iOSPortraitBottomNavigationTabBarClearance": \.portraitBottomNavigationTabBarClearance,
        "ARMSX2iOSLandscapeBottomNavigationTabBarClearance": \.landscapeBottomNavigationTabBarClearance,
    ]
    private static let tabBarThemeTriStates: [String: ReferenceWritableKeyPath<MenuTabBarAppearance, Int>] = [
        "ARMSX2iOSPortraitBottomNavigationTabBarShowsLabels": \.portraitBottomNavigationTabBarShowsLabels,
        "ARMSX2iOSLandscapeBottomNavigationTabBarShowsLabels": \.landscapeBottomNavigationTabBarShowsLabels,
    ]

    /// Reads the stored object, not the property, so a key the user never set still exports nil.
    nonisolated static func themeNumber(forKey key: String) -> NSNumber? {
        UserDefaults.standard.object(forKey: key) as? NSNumber
    }

    static func setThemeNumber(_ number: NSNumber, forKey key: String) {
        if let keyPath = themeDoubles[key] {
            shared[keyPath: keyPath] = number.doubleValue
        } else if let keyPath = themeTriStates[key] {
            shared[keyPath: keyPath] = number.intValue
        } else if let keyPath = tabBarThemeDoubles[key] {
            MenuTabBarAppearance.shared[keyPath: keyPath] = number.doubleValue
        } else if let keyPath = tabBarThemeTriStates[key] {
            MenuTabBarAppearance.shared[keyPath: keyPath] = number.intValue
        }
    }
}
