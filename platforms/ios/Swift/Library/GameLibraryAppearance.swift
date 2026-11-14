// GameLibraryAppearance.swift — The library's card and layout prefs, one property per UserDefaults key
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Observation

@MainActor
@Observable
final class GameLibraryAppearance {
    static let shared = GameLibraryAppearance()
    @ObservationIgnored private let defaults = UserDefaults.standard

    var libraryLayout: String {
        didSet { defaults.set(libraryLayout, forKey: "ARMSX2iOSGameLibraryLayout") }
    }
    var landscapeCoverFlowEnabled: Bool {
        didSet { defaults.set(landscapeCoverFlowEnabled, forKey: "ARMSX2iOSLandscapeCoverFlowEnabled") }
    }
    // Keep the legacy value as the initial fallback, then persist independent
    // portrait and landscape values as soon as either layout is adjusted.
    // A single shared key made changes in one orientation leak into the other.
    var portraitUseGamesLogo: Int {
        didSet { defaults.set(portraitUseGamesLogo, forKey: "ARMSX2iOSPortraitUseGamesLogo") }
    }
    var landscapeUseGamesLogo: Int {
        didSet { defaults.set(landscapeUseGamesLogo, forKey: "ARMSX2iOSLandscapeUseGamesLogo") }
    }
    var legacyHideGamesScreenTitle: Bool {
        didSet { defaults.set(legacyHideGamesScreenTitle, forKey: "ARMSX2iOSHideGamesScreenTitle") }
    }
    var portraitHideGamesScreenTitle: Int {
        didSet { defaults.set(portraitHideGamesScreenTitle, forKey: "ARMSX2iOSPortraitHideGamesScreenTitle") }
    }
    var landscapeHideGamesScreenTitle: Int {
        didSet { defaults.set(landscapeHideGamesScreenTitle, forKey: "ARMSX2iOSLandscapeHideGamesScreenTitle") }
    }
    var legacyHideGameName: Bool {
        didSet { defaults.set(legacyHideGameName, forKey: "ARMSX2iOSHideGameName") }
    }
    var portraitHideGameName: Int {
        didSet { defaults.set(portraitHideGameName, forKey: "ARMSX2iOSPortraitHideGameName") }
    }
    var landscapeHideGameName: Int {
        didSet { defaults.set(landscapeHideGameName, forKey: "ARMSX2iOSLandscapeHideGameName") }
    }
    var legacyHideGameInfo: Bool {
        didSet { defaults.set(legacyHideGameInfo, forKey: "ARMSX2iOSHideGameInfo") }
    }
    var portraitHideGameInfo: Int {
        didSet { defaults.set(portraitHideGameInfo, forKey: "ARMSX2iOSPortraitHideGameInfo") }
    }
    var landscapeHideGameInfo: Int {
        didSet { defaults.set(landscapeHideGameInfo, forKey: "ARMSX2iOSLandscapeHideGameInfo") }
    }
    var legacyHideFavoriteButton: Bool {
        didSet { defaults.set(legacyHideFavoriteButton, forKey: "ARMSX2iOSHideFavoriteButton") }
    }
    var portraitHideFavoriteButton: Int {
        didSet { defaults.set(portraitHideFavoriteButton, forKey: "ARMSX2iOSPortraitHideFavoriteButton") }
    }
    var landscapeHideFavoriteButton: Int {
        didSet { defaults.set(landscapeHideFavoriteButton, forKey: "ARMSX2iOSLandscapeHideFavoriteButton") }
    }
    var legacyHideRegionFlag: Bool {
        didSet { defaults.set(legacyHideRegionFlag, forKey: "ARMSX2iOSHideRegionFlag") }
    }
    var portraitHideRegionFlag: Int {
        didSet { defaults.set(portraitHideRegionFlag, forKey: "ARMSX2iOSPortraitHideRegionFlag") }
    }
    var landscapeHideRegionFlag: Int {
        didSet { defaults.set(landscapeHideRegionFlag, forKey: "ARMSX2iOSLandscapeHideRegionFlag") }
    }
    var legacySingleLineGameNames: Bool {
        didSet { defaults.set(legacySingleLineGameNames, forKey: "ARMSX2iOSSingleLineGameNames") }
    }
    var portraitSingleLineGameNames: Int {
        didSet { defaults.set(portraitSingleLineGameNames, forKey: "ARMSX2iOSPortraitSingleLineGameNames") }
    }
    var landscapeSingleLineGameNames: Int {
        didSet { defaults.set(landscapeSingleLineGameNames, forKey: "ARMSX2iOSLandscapeSingleLineGameNames") }
    }
    var legacyHideRunningIndicator: Bool {
        didSet { defaults.set(legacyHideRunningIndicator, forKey: "ARMSX2iOSHideRunningIndicator") }
    }
    var portraitHideRunningIndicator: Int {
        didSet { defaults.set(portraitHideRunningIndicator, forKey: "ARMSX2iOSPortraitHideRunningIndicator") }
    }
    var landscapeHideRunningIndicator: Int {
        didSet { defaults.set(landscapeHideRunningIndicator, forKey: "ARMSX2iOSLandscapeHideRunningIndicator") }
    }
    // Experimental presentation metrics are deliberately orientation-specific.
    // Only the complete card surface changes size; cover/title children retain
    // their shared center, preserving the proven focus-zoom geometry.
    var portraitGameCardScale: Double {
        didSet { defaults.set(portraitGameCardScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardScale") }
    }
    var landscapeGameCardScale: Double {
        didSet { defaults.set(landscapeGameCardScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardScale") }
    }
    var portraitGameCardWidthScale: Double {
        didSet { defaults.set(portraitGameCardWidthScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardWidthScale") }
    }
    var landscapeGameCardWidthScale: Double {
        didSet { defaults.set(landscapeGameCardWidthScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardWidthScale") }
    }
    var portraitGameCardHeightScale: Double {
        didSet { defaults.set(portraitGameCardHeightScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardHeightScale") }
    }
    var landscapeGameCardHeightScale: Double {
        didSet { defaults.set(landscapeGameCardHeightScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardHeightScale") }
    }
    var portraitGameLibraryVerticalPosition: Double {
        didSet { defaults.set(portraitGameLibraryVerticalPosition, forKey: "ARMSX2iOSPortraitGameLibraryVerticalPosition") }
    }
    var landscapeGameLibraryVerticalPosition: Double {
        didSet { defaults.set(landscapeGameLibraryVerticalPosition, forKey: "ARMSX2iOSLandscapeGameLibraryVerticalPosition") }
    }
    var portraitGameCardGapScale: Double {
        didSet { defaults.set(portraitGameCardGapScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardGapScale") }
    }
    var landscapeGameCardGapScale: Double {
        didSet { defaults.set(landscapeGameCardGapScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardGapScale") }
    }
    var portraitGameCardContentPaddingScale: Double {
        didSet { defaults.set(portraitGameCardContentPaddingScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardContentPaddingScale") }
    }
    var landscapeGameCardContentPaddingScale: Double {
        didSet { defaults.set(landscapeGameCardContentPaddingScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardContentPaddingScale") }
    }
    var portraitGameCardVerticalPaddingScale: Double {
        didSet { defaults.set(portraitGameCardVerticalPaddingScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardVerticalPaddingScale") }
    }
    var landscapeGameCardVerticalPaddingScale: Double {
        didSet { defaults.set(landscapeGameCardVerticalPaddingScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardVerticalPaddingScale") }
    }
    var portraitGameCardTextSpacingScale: Double {
        didSet { defaults.set(portraitGameCardTextSpacingScale, forKey: "ARMSX2iOSExperimentalPortraitGameCardTextSpacingScale") }
    }
    var landscapeGameCardTextSpacingScale: Double {
        didSet { defaults.set(landscapeGameCardTextSpacingScale, forKey: "ARMSX2iOSExperimentalLandscapeGameCardTextSpacingScale") }
    }
    var portraitGameNameTextScale: Double {
        didSet { defaults.set(portraitGameNameTextScale, forKey: "ARMSX2iOSExperimentalPortraitGameNameTextScale") }
    }
    var landscapeGameNameTextScale: Double {
        didSet { defaults.set(landscapeGameNameTextScale, forKey: "ARMSX2iOSExperimentalLandscapeGameNameTextScale") }
    }
    var portraitGameInfoTextScale: Double {
        didSet { defaults.set(portraitGameInfoTextScale, forKey: "ARMSX2iOSExperimentalPortraitGameInfoTextScale") }
    }
    var landscapeGameInfoTextScale: Double {
        didSet { defaults.set(landscapeGameInfoTextScale, forKey: "ARMSX2iOSExperimentalLandscapeGameInfoTextScale") }
    }
    var portraitGameCardCornerRadius: Double {
        didSet { defaults.set(portraitGameCardCornerRadius, forKey: "ARMSX2iOSExperimentalPortraitGameCardCornerRadius") }
    }
    var landscapeGameCardCornerRadius: Double {
        didSet { defaults.set(landscapeGameCardCornerRadius, forKey: "ARMSX2iOSExperimentalLandscapeGameCardCornerRadius") }
    }
    var portraitGameCoverCornerRadius: Double {
        didSet { defaults.set(portraitGameCoverCornerRadius, forKey: "ARMSX2iOSExperimentalPortraitGameCoverCornerRadius") }
    }
    var landscapeGameCoverCornerRadius: Double {
        didSet { defaults.set(landscapeGameCoverCornerRadius, forKey: "ARMSX2iOSExperimentalLandscapeGameCoverCornerRadius") }
    }
    var portraitGameCoverShadowStrength: Double {
        didSet { defaults.set(portraitGameCoverShadowStrength, forKey: "ARMSX2iOSExperimentalPortraitGameCoverShadow") }
    }
    var landscapeGameCoverShadowStrength: Double {
        didSet { defaults.set(landscapeGameCoverShadowStrength, forKey: "ARMSX2iOSExperimentalLandscapeGameCoverShadow") }
    }
    var portraitGameCoverOpacity: Double {
        didSet { defaults.set(portraitGameCoverOpacity, forKey: "ARMSX2iOSExperimentalPortraitGameCoverOpacity") }
    }
    var landscapeGameCoverOpacity: Double {
        didSet { defaults.set(landscapeGameCoverOpacity, forKey: "ARMSX2iOSExperimentalLandscapeGameCoverOpacity") }
    }

    /// A missing key keeps the @AppStorage default these keys had, where a number read would give 0.
    private init() {
        libraryLayout = defaults.object(forKey: "ARMSX2iOSGameLibraryLayout") as? String ?? "grid"
        landscapeCoverFlowEnabled = defaults.object(forKey: "ARMSX2iOSLandscapeCoverFlowEnabled") as? Bool ?? true
        portraitUseGamesLogo = defaults.object(forKey: "ARMSX2iOSPortraitUseGamesLogo") as? Int ?? -1
        landscapeUseGamesLogo = defaults.object(forKey: "ARMSX2iOSLandscapeUseGamesLogo") as? Int ?? -1
        legacyHideGamesScreenTitle = defaults.object(forKey: "ARMSX2iOSHideGamesScreenTitle") as? Bool ?? true
        portraitHideGamesScreenTitle = defaults.object(forKey: "ARMSX2iOSPortraitHideGamesScreenTitle") as? Int ?? -1
        landscapeHideGamesScreenTitle = defaults.object(forKey: "ARMSX2iOSLandscapeHideGamesScreenTitle") as? Int ?? -1
        legacyHideGameName = defaults.object(forKey: "ARMSX2iOSHideGameName") as? Bool ?? false
        portraitHideGameName = defaults.object(forKey: "ARMSX2iOSPortraitHideGameName") as? Int ?? -1
        landscapeHideGameName = defaults.object(forKey: "ARMSX2iOSLandscapeHideGameName") as? Int ?? -1
        legacyHideGameInfo = defaults.object(forKey: "ARMSX2iOSHideGameInfo") as? Bool ?? false
        portraitHideGameInfo = defaults.object(forKey: "ARMSX2iOSPortraitHideGameInfo") as? Int ?? -1
        landscapeHideGameInfo = defaults.object(forKey: "ARMSX2iOSLandscapeHideGameInfo") as? Int ?? -1
        legacyHideFavoriteButton = defaults.object(forKey: "ARMSX2iOSHideFavoriteButton") as? Bool ?? false
        portraitHideFavoriteButton = defaults.object(forKey: "ARMSX2iOSPortraitHideFavoriteButton") as? Int ?? -1
        landscapeHideFavoriteButton = defaults.object(forKey: "ARMSX2iOSLandscapeHideFavoriteButton") as? Int ?? -1
        legacyHideRegionFlag = defaults.object(forKey: "ARMSX2iOSHideRegionFlag") as? Bool ?? false
        portraitHideRegionFlag = defaults.object(forKey: "ARMSX2iOSPortraitHideRegionFlag") as? Int ?? -1
        landscapeHideRegionFlag = defaults.object(forKey: "ARMSX2iOSLandscapeHideRegionFlag") as? Int ?? -1
        legacySingleLineGameNames = defaults.object(forKey: "ARMSX2iOSSingleLineGameNames") as? Bool ?? false
        portraitSingleLineGameNames = defaults.object(forKey: "ARMSX2iOSPortraitSingleLineGameNames") as? Int ?? -1
        landscapeSingleLineGameNames = defaults.object(forKey: "ARMSX2iOSLandscapeSingleLineGameNames") as? Int ?? -1
        legacyHideRunningIndicator = defaults.object(forKey: "ARMSX2iOSHideRunningIndicator") as? Bool ?? false
        portraitHideRunningIndicator = defaults.object(forKey: "ARMSX2iOSPortraitHideRunningIndicator") as? Int ?? -1
        landscapeHideRunningIndicator = defaults.object(forKey: "ARMSX2iOSLandscapeHideRunningIndicator") as? Int ?? -1
        portraitGameCardScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardScale") as? Double ?? 1.0
        landscapeGameCardScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardScale") as? Double ?? 1.0
        portraitGameCardWidthScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardWidthScale") as? Double ?? 1.0
        landscapeGameCardWidthScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardWidthScale") as? Double ?? 1.0
        portraitGameCardHeightScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardHeightScale") as? Double ?? 1.0
        landscapeGameCardHeightScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardHeightScale") as? Double ?? 1.0
        portraitGameLibraryVerticalPosition = defaults.object(forKey: "ARMSX2iOSPortraitGameLibraryVerticalPosition") as? Double ?? 0.0
        landscapeGameLibraryVerticalPosition = defaults.object(forKey: "ARMSX2iOSLandscapeGameLibraryVerticalPosition") as? Double ?? 0.0
        portraitGameCardGapScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardGapScale") as? Double ?? 1.0
        landscapeGameCardGapScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardGapScale") as? Double ?? 1.0
        portraitGameCardContentPaddingScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardContentPaddingScale") as? Double ?? 1.0
        landscapeGameCardContentPaddingScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardContentPaddingScale") as? Double ?? 1.0
        portraitGameCardVerticalPaddingScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardVerticalPaddingScale") as? Double ?? 1.0
        landscapeGameCardVerticalPaddingScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardVerticalPaddingScale") as? Double ?? 1.0
        portraitGameCardTextSpacingScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardTextSpacingScale") as? Double ?? 1.0
        landscapeGameCardTextSpacingScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardTextSpacingScale") as? Double ?? 1.0
        portraitGameNameTextScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameNameTextScale") as? Double ?? 1.0
        landscapeGameNameTextScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameNameTextScale") as? Double ?? 1.0
        portraitGameInfoTextScale = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameInfoTextScale") as? Double ?? 1.0
        landscapeGameInfoTextScale = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameInfoTextScale") as? Double ?? 1.0
        portraitGameCardCornerRadius = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCardCornerRadius") as? Double ?? 18.0
        landscapeGameCardCornerRadius = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCardCornerRadius") as? Double ?? 18.0
        portraitGameCoverCornerRadius = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCoverCornerRadius") as? Double ?? 10.0
        landscapeGameCoverCornerRadius = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCoverCornerRadius") as? Double ?? 10.0
        portraitGameCoverShadowStrength = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCoverShadow") as? Double ?? 1.0
        landscapeGameCoverShadowStrength = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCoverShadow") as? Double ?? 1.0
        portraitGameCoverOpacity = defaults.object(forKey: "ARMSX2iOSExperimentalPortraitGameCoverOpacity") as? Double ?? 1.0
        landscapeGameCoverOpacity = defaults.object(forKey: "ARMSX2iOSExperimentalLandscapeGameCoverOpacity") as? Double ?? 1.0
    }

    static func orientationBoolBinding(
        portrait: Binding<Int>,
        landscape: Binding<Int>,
        fallback: Bool,
        isLandscape: Bool
    ) -> Binding<Bool> {
        Binding(
            get: {
                let stored = isLandscape
                    ? landscape.wrappedValue
                    : portrait.wrappedValue
                return stored >= 0 ? stored == 1 : fallback
            },
            set: { value in
                if isLandscape {
                    landscape.wrappedValue = value ? 1 : 0
                } else {
                    portrait.wrappedValue = value ? 1 : 0
                }
            }
        )
    }

    static func orientationDoubleBinding(
        portrait: Binding<Double>,
        landscape: Binding<Double>,
        isLandscape: Bool
    ) -> Binding<Double> {
        Binding(
            get: {
                isLandscape
                    ? landscape.wrappedValue
                    : portrait.wrappedValue
            },
            set: { value in
                if isLandscape {
                    landscape.wrappedValue = value
                } else {
                    portrait.wrappedValue = value
                }
            }
        )
    }

    func activeHideGamesScreenTitleBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitHideGamesScreenTitle,
            landscape: Bindable(self).landscapeHideGamesScreenTitle,
            fallback: legacyHideGamesScreenTitle,
            isLandscape: isLandscape
        )
    }

    func activeUseGamesLogoBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitUseGamesLogo,
            landscape: Bindable(self).landscapeUseGamesLogo,
            fallback: true,
            isLandscape: isLandscape
        )
    }

    func activeHideGameNameBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitHideGameName,
            landscape: Bindable(self).landscapeHideGameName,
            fallback: legacyHideGameName,
            isLandscape: isLandscape
        )
    }

    func activeHideGameInfoBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitHideGameInfo,
            landscape: Bindable(self).landscapeHideGameInfo,
            fallback: legacyHideGameInfo,
            isLandscape: isLandscape
        )
    }

    func activeHideFavoriteButtonBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitHideFavoriteButton,
            landscape: Bindable(self).landscapeHideFavoriteButton,
            fallback: legacyHideFavoriteButton,
            isLandscape: isLandscape
        )
    }

    func activeHideRegionFlagBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitHideRegionFlag,
            landscape: Bindable(self).landscapeHideRegionFlag,
            fallback: legacyHideRegionFlag,
            isLandscape: isLandscape
        )
    }

    func activeSingleLineGameNamesBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitSingleLineGameNames,
            landscape: Bindable(self).landscapeSingleLineGameNames,
            fallback: legacySingleLineGameNames,
            isLandscape: isLandscape
        )
    }

    func activeHideRunningIndicatorBinding(isLandscape: Bool) -> Binding<Bool> {
        Self.orientationBoolBinding(
            portrait: Bindable(self).portraitHideRunningIndicator,
            landscape: Bindable(self).landscapeHideRunningIndicator,
            fallback: legacyHideRunningIndicator,
            isLandscape: isLandscape
        )
    }

    func activeGameCardScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardScale,
            landscape: Bindable(self).landscapeGameCardScale,
            isLandscape: isLandscape
        )
    }

    func activeGameCardWidthScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardWidthScale,
            landscape: Bindable(self).landscapeGameCardWidthScale,
            isLandscape: isLandscape
        )
    }

    func activeGameCardHeightScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardHeightScale,
            landscape: Bindable(self).landscapeGameCardHeightScale,
            isLandscape: isLandscape
        )
    }

    func activeGameLibraryVerticalPositionBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameLibraryVerticalPosition,
            landscape: Bindable(self).landscapeGameLibraryVerticalPosition,
            isLandscape: isLandscape
        )
    }

    func activeGameCardGapScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardGapScale,
            landscape: Bindable(self).landscapeGameCardGapScale,
            isLandscape: isLandscape
        )
    }

    func activeGameCardContentPaddingScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardContentPaddingScale,
            landscape: Bindable(self).landscapeGameCardContentPaddingScale,
            isLandscape: isLandscape
        )
    }

    func activeGameCardVerticalPaddingScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardVerticalPaddingScale,
            landscape: Bindable(self).landscapeGameCardVerticalPaddingScale,
            isLandscape: isLandscape
        )
    }

    func activeGameCardTextSpacingScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardTextSpacingScale,
            landscape: Bindable(self).landscapeGameCardTextSpacingScale,
            isLandscape: isLandscape
        )
    }

    func activeGameNameTextScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameNameTextScale,
            landscape: Bindable(self).landscapeGameNameTextScale,
            isLandscape: isLandscape
        )
    }

    func activeGameInfoTextScaleBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameInfoTextScale,
            landscape: Bindable(self).landscapeGameInfoTextScale,
            isLandscape: isLandscape
        )
    }

    func activeGameCardCornerRadiusBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCardCornerRadius,
            landscape: Bindable(self).landscapeGameCardCornerRadius,
            isLandscape: isLandscape
        )
    }

    func activeGameCoverCornerRadiusBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCoverCornerRadius,
            landscape: Bindable(self).landscapeGameCoverCornerRadius,
            isLandscape: isLandscape
        )
    }

    func activeGameCoverShadowStrengthBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCoverShadowStrength,
            landscape: Bindable(self).landscapeGameCoverShadowStrength,
            isLandscape: isLandscape
        )
    }

    func activeGameCoverOpacityBinding(isLandscape: Bool) -> Binding<Double> {
        Self.orientationDoubleBinding(
            portrait: Bindable(self).portraitGameCoverOpacity,
            landscape: Bindable(self).landscapeGameCoverOpacity,
            isLandscape: isLandscape
        )
    }
}
