// MenuTabBarAppearance.swift — The bottom tab bar's prefs, one property per UserDefaults key
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Observation

@MainActor
@Observable
final class MenuTabBarAppearance {
    static let shared = MenuTabBarAppearance()
    @ObservationIgnored private let defaults = UserDefaults.standard

    var portraitBottomNavigationTabBarHeight: Double {
        didSet { defaults.set(portraitBottomNavigationTabBarHeight, forKey: "ARMSX2iOSPortraitBottomNavigationTabBarHeight") }
    }
    var landscapeBottomNavigationTabBarHeight: Double {
        didSet { defaults.set(landscapeBottomNavigationTabBarHeight, forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarHeight") }
    }
    var portraitBottomNavigationTabBarIconScale: Double {
        didSet { defaults.set(portraitBottomNavigationTabBarIconScale, forKey: "ARMSX2iOSPortraitBottomNavigationTabBarIconScale") }
    }
    var landscapeBottomNavigationTabBarIconScale: Double {
        didSet { defaults.set(landscapeBottomNavigationTabBarIconScale, forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarIconScale") }
    }
    var portraitBottomNavigationTabBarLabelScale: Double {
        didSet { defaults.set(portraitBottomNavigationTabBarLabelScale, forKey: "ARMSX2iOSPortraitBottomNavigationTabBarLabelScale") }
    }
    var landscapeBottomNavigationTabBarLabelScale: Double {
        didSet { defaults.set(landscapeBottomNavigationTabBarLabelScale, forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarLabelScale") }
    }
    var portraitBottomNavigationTabBarClearance: Double {
        didSet { defaults.set(portraitBottomNavigationTabBarClearance, forKey: "ARMSX2iOSPortraitBottomNavigationTabBarClearance") }
    }
    var landscapeBottomNavigationTabBarClearance: Double {
        didSet { defaults.set(landscapeBottomNavigationTabBarClearance, forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarClearance") }
    }
    var portraitBottomNavigationTabBarShowsLabels: Int {
        didSet { defaults.set(portraitBottomNavigationTabBarShowsLabels, forKey: "ARMSX2iOSPortraitBottomNavigationTabBarShowsLabels") }
    }
    var landscapeBottomNavigationTabBarShowsLabels: Int {
        didSet { defaults.set(landscapeBottomNavigationTabBarShowsLabels, forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarShowsLabels") }
    }

    /// A missing key keeps the @AppStorage default these keys had, where a number read would give 0.
    private init() {
        portraitBottomNavigationTabBarHeight = defaults.object(forKey: "ARMSX2iOSPortraitBottomNavigationTabBarHeight") as? Double ?? 84.0
        landscapeBottomNavigationTabBarHeight = defaults.object(forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarHeight") as? Double ?? 84.0
        portraitBottomNavigationTabBarIconScale = defaults.object(forKey: "ARMSX2iOSPortraitBottomNavigationTabBarIconScale") as? Double ?? 1.0
        landscapeBottomNavigationTabBarIconScale = defaults.object(forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarIconScale") as? Double ?? 1.0
        portraitBottomNavigationTabBarLabelScale = defaults.object(forKey: "ARMSX2iOSPortraitBottomNavigationTabBarLabelScale") as? Double ?? 1.0
        landscapeBottomNavigationTabBarLabelScale = defaults.object(forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarLabelScale") as? Double ?? 1.0
        portraitBottomNavigationTabBarClearance = defaults.object(forKey: "ARMSX2iOSPortraitBottomNavigationTabBarClearance") as? Double ?? 0.0
        landscapeBottomNavigationTabBarClearance = defaults.object(forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarClearance") as? Double ?? 0.0
        portraitBottomNavigationTabBarShowsLabels = defaults.object(forKey: "ARMSX2iOSPortraitBottomNavigationTabBarShowsLabels") as? Int ?? 1
        landscapeBottomNavigationTabBarShowsLabels = defaults.object(forKey: "ARMSX2iOSLandscapeBottomNavigationTabBarShowsLabels") as? Int ?? 1
    }

    func activeBottomNavigationTabBarHeightBinding(isLandscape: Bool) -> Binding<Double> {
        GameLibraryAppearance.orientationDoubleBinding(
            portrait: Bindable(self).portraitBottomNavigationTabBarHeight,
            landscape: Bindable(self).landscapeBottomNavigationTabBarHeight,
            isLandscape: isLandscape
        )
    }

    func activeBottomNavigationTabBarIconScaleBinding(isLandscape: Bool) -> Binding<Double> {
        GameLibraryAppearance.orientationDoubleBinding(
            portrait: Bindable(self).portraitBottomNavigationTabBarIconScale,
            landscape: Bindable(self).landscapeBottomNavigationTabBarIconScale,
            isLandscape: isLandscape
        )
    }

    func activeBottomNavigationTabBarLabelScaleBinding(isLandscape: Bool) -> Binding<Double> {
        GameLibraryAppearance.orientationDoubleBinding(
            portrait: Bindable(self).portraitBottomNavigationTabBarLabelScale,
            landscape: Bindable(self).landscapeBottomNavigationTabBarLabelScale,
            isLandscape: isLandscape
        )
    }

    func activeBottomNavigationTabBarClearanceBinding(isLandscape: Bool) -> Binding<Double> {
        GameLibraryAppearance.orientationDoubleBinding(
            portrait: Bindable(self).portraitBottomNavigationTabBarClearance,
            landscape: Bindable(self).landscapeBottomNavigationTabBarClearance,
            isLandscape: isLandscape
        )
    }

    func activeBottomNavigationTabBarShowsLabelsBinding(isLandscape: Bool) -> Binding<Bool> {
        GameLibraryAppearance.orientationBoolBinding(
            portrait: Bindable(self).portraitBottomNavigationTabBarShowsLabels,
            landscape: Bindable(self).landscapeBottomNavigationTabBarShowsLabels,
            fallback: true,
            isLandscape: isLandscape
        )
    }
}
