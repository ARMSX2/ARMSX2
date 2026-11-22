// SettingsStoreAppearance.swift — appearance, background and library preferences kept in UserDefaults
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

extension SettingsStore {
    @MainActor
    @Observable
    final class Appearance {
        // ── Library Background ──
        var dynamicBackgroundsEnabled: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    dynamicBackgroundsEnabled,
                    forKey: "ARMSX2iOSDynamicBackgroundsEnabled"
                )
            }
        }
        var dynamicAppearancePreferences: DynamicAppearancePreferences = .standard {
            didSet { dynamicAppearancePreferences.save() }
        }
        var clearLiquidGlassUI: Bool = true {
            didSet {
                UserDefaults.standard.set(clearLiquidGlassUI, forKey: "ARMSX2iOSClearLiquidGlassUI")
                if !clearLiquidGlassUI && clearLiquidGlassUISubSettings {
                    clearLiquidGlassUISubSettings = false
                }
            }
        }
        var clearLiquidGlassUISubSettings: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    clearLiquidGlassUISubSettings,
                    forKey: "ARMSX2iOSClearLiquidGlassUISubSettings"
                )
            }
        }
        var clearLiquidGlassUIQuickMenu: Bool = false {
            didSet {
                UserDefaults.standard.set(
                    clearLiquidGlassUIQuickMenu,
                    forKey: "ARMSX2iOSClearLiquidGlassUIQuickMenu"
                )
            }
        }
        var clearLiquidGlassUIPerGameSettingsLibrary: Bool = false {
            didSet {
                UserDefaults.standard.set(
                    clearLiquidGlassUIPerGameSettingsLibrary,
                    forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsLibrary"
                )
            }
        }
        var clearLiquidGlassUIPerGameSettingsEmulation: Bool = false {
            didSet {
                UserDefaults.standard.set(
                    clearLiquidGlassUIPerGameSettingsEmulation,
                    forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsEmulation"
                )
            }
        }
        static let perGameLivePreviewDurationRange: ClosedRange<Double> = 2...10
        static let perGameBeforeChangesPreviewDurationRange: ClosedRange<Double> = 0...10
        var temporalSaveStateToLivePreviewChanges: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    temporalSaveStateToLivePreviewChanges,
                    forKey: "ARMSX2iOSTemporalSaveStateToLivePreviewChanges"
                )
            }
        }
        var perGameLivePreviewDuration: Double = 2 {
            didSet {
                UserDefaults.standard.set(
                    perGameLivePreviewDuration,
                    forKey: "ARMSX2iOSPerGameLivePreviewDuration"
                )
            }
        }
        var perGameBeforeChangesPreviewDuration: Double = 0 {
            didSet {
                UserDefaults.standard.set(
                    perGameBeforeChangesPreviewDuration,
                    forKey: "ARMSX2iOSPerGameBeforeChangesPreviewDuration"
                )
            }
        }
        var perGameLivePreviewStopsWithCircle: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    perGameLivePreviewStopsWithCircle,
                    forKey: "ARMSX2iOSPerGameLivePreviewStopsWithCircle"
                )
            }
        }
        var gameCardZoomAnimationEnabled: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    gameCardZoomAnimationEnabled,
                    forKey: "ARMSX2iOSGameCardZoomAnimationEnabled"
                )
            }
        }
        var favoriteGlowingEffectEnabled: Bool = false {
            didSet {
                UserDefaults.standard.set(
                    favoriteGlowingEffectEnabled,
                    forKey: "ARMSX2iOSFavoriteGlowingEffectEnabled"
                )
            }
        }
        var gameMenuCoverColoursEnabled: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    gameMenuCoverColoursEnabled,
                    forKey: "ARMSX2iOSGameMenuCoverColoursEnabled"
                )
            }
        }
        var hideGameplayStatusBar: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    hideGameplayStatusBar,
                    forKey: "ARMSX2iOSHideGameplayStatusBar"
                )
            }
        }
        var hideIntroStatusBar: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    hideIntroStatusBar,
                    forKey: "ARMSX2iOSHideIntroStatusBar"
                )
            }
        }
        var hideMenuStatusBar: Bool = false {
            didSet {
                UserDefaults.standard.set(
                    hideMenuStatusBar,
                    forKey: "ARMSX2iOSHideMenuStatusBar"
                )
            }
        }
        var focusOrbsEnabled: Bool = false {
            didSet {
                UserDefaults.standard.set(
                    focusOrbsEnabled,
                    forKey: "ARMSX2iOSFocusOrbsEnabled"
                )
            }
        }
        // Retain the preset's readable semantic defaults while editing Custom.
        var controllerCustomThemeBase: ControllerUIThemePreset = ControllerUIThemePreset(
            rawValue: UserDefaults.standard.string(forKey: "ARMSX2iOSCustomThemeBase") ?? ""
        ) ?? .defaultTheme {
            didSet {
                UserDefaults.standard.set(controllerCustomThemeBase.rawValue, forKey: "ARMSX2iOSCustomThemeBase")
            }
        }
        var controllerRoleCustomColors: [String: SavedPaletteColor] = {
            guard let data = UserDefaults.standard.data(forKey: "ARMSX2iOSRoleCustomColors") else { return [:] }
            return (try? JSONDecoder().decode([String: SavedPaletteColor].self, from: data)) ?? [:]
        }() {
            didSet {
                if let data = try? JSONEncoder().encode(controllerRoleCustomColors) {
                    UserDefaults.standard.set(data, forKey: "ARMSX2iOSRoleCustomColors")
                }
            }
        }
        var controllerUIThemePreset: ControllerUIThemePreset = .defaultTheme {
            didSet {
                UserDefaults.standard.set(
                    controllerUIThemePreset.rawValue,
                    forKey: "ARMSX2iOSControllerUIThemePreset"
                )
            }
        }
        var controllerNavigationDepthEffectEnabled: Bool = true {
            didSet {
                UserDefaults.standard.set(
                    controllerNavigationDepthEffectEnabled,
                    forKey: "ARMSX2iOSControllerNavigationDepthEffectEnabled"
                )
            }
        }
        var controllerFocusBoxStyle: ControllerFocusBoxStyle = .neonBlue {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusBoxStyle.rawValue,
                    forKey: "ARMSX2iOSControllerFocusBoxStyle"
                )
            }
        }
        var controllerNavigationFocusAnimation: ControllerNavigationFocusTravelStyle = .easeInOut {
            didSet {
                UserDefaults.standard.set(
                    controllerNavigationFocusAnimation.rawValue,
                    forKey: "ARMSX2iOSControllerNavigationFocusAnimation"
                )
            }
        }
        var controllerFocusBoxPalette: ThemePalette = .multicolor {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusBoxPalette.rawValue,
                    forKey: "ARMSX2iOSControllerFocusBoxPalette"
                )
            }
        }
        var controllerFocusBoxCustomColor: SavedPaletteColor? {
            didSet {
                Self.persistSavedPaletteColor(
                    controllerFocusBoxCustomColor,
                    forKey: "ARMSX2iOSControllerFocusBoxCustomColor"
                )
            }
        }
        var controllerOrbPalette: ThemePalette = .blue {
            didSet {
                UserDefaults.standard.set(
                    controllerOrbPalette.rawValue,
                    forKey: "ARMSX2iOSControllerOrbPalette"
                )
            }
        }
        var controllerNavigationAccentPalette: ThemePalette = .henyBlue {
            didSet {
                UserDefaults.standard.set(
                    controllerNavigationAccentPalette.rawValue,
                    forKey: "ARMSX2iOSControllerNavigationAccentPalette"
                )
            }
        }
        var controllerNavigationCustomAccentColor: SavedPaletteColor? {
            didSet {
                Self.persistSavedPaletteColor(
                    controllerNavigationCustomAccentColor,
                    forKey: "ARMSX2iOSControllerNavigationCustomAccentColor"
                )
            }
        }
        var controllerTextPalette: ThemePalette? = nil {
            didSet {
                if let controllerTextPalette {
                    UserDefaults.standard.set(
                        controllerTextPalette.rawValue,
                        forKey: "ARMSX2iOSControllerTextPalette"
                    )
                } else {
                    UserDefaults.standard.removeObject(
                        forKey: "ARMSX2iOSControllerTextPalette"
                    )
                }
            }
        }
        var controllerTextCustomColor: SavedPaletteColor? {
            didSet {
                Self.persistSavedPaletteColor(
                    controllerTextCustomColor,
                    forKey: "ARMSX2iOSControllerTextCustomColor"
                )
            }
        }
        var controllerSecondaryTextPalette: ThemePalette? = nil {
            didSet {
                if let controllerSecondaryTextPalette {
                    UserDefaults.standard.set(
                        controllerSecondaryTextPalette.rawValue,
                        forKey: "ARMSX2iOSControllerSecondaryTextPalette"
                    )
                } else {
                    UserDefaults.standard.removeObject(
                        forKey: "ARMSX2iOSControllerSecondaryTextPalette"
                    )
                }
            }
        }
        var controllerCriticalTextPalette: ThemePalette = .crimson {
            didSet {
                UserDefaults.standard.set(
                    controllerCriticalTextPalette.rawValue,
                    forKey: "ARMSX2iOSControllerCriticalTextPalette"
                )
            }
        }
        var controllerTabTitlePalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerTabTitlePalette,
                    forKey: "ARMSX2iOSControllerTabTitlePalette"
                )
            }
        }
        var controllerTabSubtitlePalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerTabSubtitlePalette,
                    forKey: "ARMSX2iOSControllerTabSubtitlePalette"
                )
            }
        }
        var controllerBottomTabBarPalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerBottomTabBarPalette,
                    forKey: "ARMSX2iOSControllerBottomTabBarPalette"
                )
            }
        }
        var controllerBottomTabBarUnselectedPalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerBottomTabBarUnselectedPalette,
                    forKey: "ARMSX2iOSControllerBottomTabBarUnselectedPalette"
                )
            }
        }
        var controllerCardTitlePalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerCardTitlePalette,
                    forKey: "ARMSX2iOSControllerCardTitlePalette"
                )
            }
        }
        var controllerContextMenuPalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerContextMenuPalette,
                    forKey: "ARMSX2iOSControllerContextMenuPalette"
                )
            }
        }
        var controllerImportActionPalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerImportActionPalette,
                    forKey: "ARMSX2iOSControllerImportActionPalette"
                )
            }
        }
        var controllerToolbarPalette: ThemePalette? = nil {
            didSet {
                Self.persistOptionalThemePalette(
                    controllerToolbarPalette,
                    forKey: "ARMSX2iOSControllerToolbarPalette"
                )
            }
        }
        var controllerFocusedTextPalette: ThemePalette = .blue {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusedTextPalette.rawValue,
                    forKey: "ARMSX2iOSControllerFocusedTextPalette"
                )
            }
        }
        var controllerFocusedTextCustomColor: SavedPaletteColor? {
            didSet {
                Self.persistSavedPaletteColor(
                    controllerFocusedTextCustomColor,
                    forKey: "ARMSX2iOSControllerFocusedTextCustomColor"
                )
            }
        }
        var controllerTextShadowStrength: Double = 0 {
            didSet {
                UserDefaults.standard.set(
                    controllerTextShadowStrength,
                    forKey: "ARMSX2iOSControllerTextShadowStrength"
                )
            }
        }
        var controllerTextShadowPalette: ThemePalette = .obsidian {
            didSet {
                UserDefaults.standard.set(
                    controllerTextShadowPalette.rawValue,
                    forKey: "ARMSX2iOSControllerTextShadowPalette"
                )
            }
        }
        var controllerTextShadowCustomColor: SavedPaletteColor? {
            didSet {
                Self.persistSavedPaletteColor(
                    controllerTextShadowCustomColor,
                    forKey: "ARMSX2iOSControllerTextShadowCustomColor"
                )
            }
        }
        var controllerFocusedTextShadowStrength: Double = 0.1 {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusedTextShadowStrength,
                    forKey: "ARMSX2iOSControllerFocusedTextShadowStrength"
                )
            }
        }
        var controllerFocusedTextShadowPalette: ThemePalette = .obsidian {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusedTextShadowPalette.rawValue,
                    forKey: "ARMSX2iOSControllerFocusedTextShadowPalette"
                )
            }
        }
        var controllerFocusedTextShadowCustomColor: SavedPaletteColor? {
            didSet {
                Self.persistSavedPaletteColor(
                    controllerFocusedTextShadowCustomColor,
                    forKey: "ARMSX2iOSControllerFocusedTextShadowCustomColor"
                )
            }
        }
        var controllerFocusBoxAnimationSpeed: Double = 1 {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusBoxAnimationSpeed,
                    forKey: "ARMSX2iOSControllerFocusBoxAnimationSpeed"
                )
            }
        }
        var controllerFocusBoxGlowIntensity: Double = 1 {
            didSet {
                UserDefaults.standard.set(
                    controllerFocusBoxGlowIntensity,
                    forKey: "ARMSX2iOSControllerFocusBoxGlowIntensity"
                )
            }
        }
        var backgroundPrimaryAsset: BackgroundAsset? {
            didSet {
                if let asset = backgroundPrimaryAsset {
                    UserDefaults.standard.set(try? JSONEncoder().encode(asset), forKey: "ARMSX2iOSBackgroundPrimaryAsset")
                } else {
                    UserDefaults.standard.removeObject(forKey: "ARMSX2iOSBackgroundPrimaryAsset")
                }
            }
        }
        var backgroundLandscapeAsset: BackgroundAsset? {
            didSet {
                if let asset = backgroundLandscapeAsset {
                    UserDefaults.standard.set(try? JSONEncoder().encode(asset), forKey: "ARMSX2iOSBackgroundLandscapeAsset")
                } else {
                    UserDefaults.standard.removeObject(forKey: "ARMSX2iOSBackgroundLandscapeAsset")
                }
            }
        }
        var backgroundFitMode: BackgroundFitMode {
            didSet { UserDefaults.standard.set(backgroundFitMode.rawValue, forKey: "ARMSX2iOSBackgroundFitMode") }
        }
        var backgroundLandscapeFitMode: BackgroundFitMode = .fill {
            didSet { UserDefaults.standard.set(backgroundLandscapeFitMode.rawValue, forKey: "ARMSX2iOSBackgroundLandscapeFitMode") }
        }
        var backgroundVideoMuted: Bool {
            didSet { UserDefaults.standard.set(backgroundVideoMuted, forKey: "ARMSX2iOSBackgroundVideoMuted") }
        }
        var backgroundDim: Double {
            didSet {
                let clamped = Self.clampedBackgroundDim(backgroundDim)
                guard backgroundDim == clamped else { backgroundDim = clamped; return }
                UserDefaults.standard.set(backgroundDim, forKey: "ARMSX2iOSBackgroundDim")
            }
        }
        var backgroundEnabledInBIOS: Bool = true {
            didSet {
                UserDefaults.standard.set(backgroundEnabledInBIOS, forKey: "ARMSX2iOSBackgroundEnabledInBIOS")
            }
        }
        var backgroundEnabledInHelp: Bool = true {
            didSet {
                UserDefaults.standard.set(backgroundEnabledInHelp, forKey: "ARMSX2iOSBackgroundEnabledInHelp")
            }
        }
        var backgroundEnabledInSettings: Bool = true {
            didSet {
                UserDefaults.standard.set(backgroundEnabledInSettings, forKey: "ARMSX2iOSBackgroundEnabledInSettings")
            }
        }

        init() {
            BackgroundStorage.migrateLegacyBackgroundsIfNeeded()
            dynamicBackgroundsEnabled = UserDefaults.standard.object(
                forKey: "ARMSX2iOSDynamicBackgroundsEnabled"
            ) as? Bool ?? true
            var loadedDynamicAppearance =
                DynamicAppearancePreferences.load() ?? .standard
            let storedThemePreset = ControllerUIThemePreset(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerUIThemePreset"
                ) ?? ""
            )
            if storedThemePreset == .defaultTheme,
               loadedDynamicAppearance.particleSettings.armsx2LogoEnabled == nil {
                // The logo option was added after existing Default-theme payloads
                // had already been persisted. Upgrade only the missing value;
                // an explicit user-off selection remains respected.
                loadedDynamicAppearance.particleSettings.armsx2LogoEnabled = true
                loadedDynamicAppearance.save()
            }
            dynamicAppearancePreferences = loadedDynamicAppearance
            let storedClearGlass = UserDefaults.standard.object(
                forKey: "ARMSX2iOSClearLiquidGlassUI"
            ) as? Bool ?? true
            clearLiquidGlassUI = storedClearGlass
            // New in this release, so an upgrade starts from the main toggle.
            clearLiquidGlassUISubSettings = UserDefaults.standard.object(
                forKey: "ARMSX2iOSClearLiquidGlassUISubSettings"
            ) as? Bool ?? storedClearGlass
            clearLiquidGlassUIQuickMenu = UserDefaults.standard.object(
                forKey: "ARMSX2iOSClearLiquidGlassUIQuickMenu"
            ) as? Bool ?? false
            let perGameRegularGlassMigrationKey =
                "ARMSX2iOSPerGameSettingsRegularGlassMigrationV1"
            if !UserDefaults.standard.bool(forKey: perGameRegularGlassMigrationKey) {
                // Restore the earlier regular (non-clear) Liquid Glass presentation
                // once for existing installs as well as new ones. Users can still
                // opt into Clear independently afterward.
                clearLiquidGlassUIPerGameSettingsLibrary = false
                clearLiquidGlassUIPerGameSettingsEmulation = false
                UserDefaults.standard.set(true, forKey: perGameRegularGlassMigrationKey)
            } else {
                clearLiquidGlassUIPerGameSettingsLibrary = UserDefaults.standard.object(
                    forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsLibrary"
                ) as? Bool ?? false
                clearLiquidGlassUIPerGameSettingsEmulation = UserDefaults.standard.object(
                    forKey: "ARMSX2iOSClearLiquidGlassUIPerGameSettingsEmulation"
                ) as? Bool ?? false
            }
            temporalSaveStateToLivePreviewChanges = UserDefaults.standard.object(
                forKey: "ARMSX2iOSTemporalSaveStateToLivePreviewChanges"
            ) as? Bool ?? true
            perGameLivePreviewDuration = min(
                max(
                    UserDefaults.standard.object(
                        forKey: "ARMSX2iOSPerGameLivePreviewDuration"
                    ) as? Double ?? 2,
                    Self.perGameLivePreviewDurationRange.lowerBound
                ),
                Self.perGameLivePreviewDurationRange.upperBound
            )
            perGameBeforeChangesPreviewDuration = min(
                max(
                    UserDefaults.standard.double(
                        forKey: "ARMSX2iOSPerGameBeforeChangesPreviewDuration"
                    ),
                    Self.perGameBeforeChangesPreviewDurationRange.lowerBound
                ),
                Self.perGameBeforeChangesPreviewDurationRange.upperBound
            )
            perGameLivePreviewStopsWithCircle = UserDefaults.standard.object(
                forKey: "ARMSX2iOSPerGameLivePreviewStopsWithCircle"
            ) as? Bool ?? true
            gameCardZoomAnimationEnabled = UserDefaults.standard.object(
                forKey: "ARMSX2iOSGameCardZoomAnimationEnabled"
            ) as? Bool ?? true
            favoriteGlowingEffectEnabled = UserDefaults.standard.object(
                forKey: "ARMSX2iOSFavoriteGlowingEffectEnabled"
            ) as? Bool ?? false
            gameMenuCoverColoursEnabled = UserDefaults.standard.object(
                forKey: "ARMSX2iOSGameMenuCoverColoursEnabled"
            ) as? Bool ?? true
            hideGameplayStatusBar = UserDefaults.standard.object(
                forKey: "ARMSX2iOSHideGameplayStatusBar"
            ) as? Bool ?? true
            hideIntroStatusBar = UserDefaults.standard.object(
                forKey: "ARMSX2iOSHideIntroStatusBar"
            ) as? Bool ?? true
            hideMenuStatusBar = UserDefaults.standard.object(
                forKey: "ARMSX2iOSHideMenuStatusBar"
            ) as? Bool ?? false
            focusOrbsEnabled = UserDefaults.standard.object(
                forKey: "ARMSX2iOSFocusOrbsEnabled"
            ) as? Bool ?? false
            let storedControllerUIThemePreset = ControllerUIThemePreset(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerUIThemePreset"
                ) ?? ""
            ) ?? .defaultTheme
            controllerUIThemePreset = storedControllerUIThemePreset
            controllerNavigationDepthEffectEnabled = UserDefaults.standard.object(
                forKey: "ARMSX2iOSControllerNavigationDepthEffectEnabled"
            ) as? Bool ?? true
            controllerFocusBoxStyle = ControllerFocusBoxStyle(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerFocusBoxStyle"
                ) ?? ""
            ) ?? .neonBlue
            controllerNavigationFocusAnimation = ControllerNavigationFocusTravelStyle(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerNavigationFocusAnimation"
                ) ?? ""
            ) ?? .easeInOut
            controllerFocusBoxPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerFocusBoxPalette"
                ) ?? ""
            ) ?? .multicolor
            controllerFocusBoxCustomColor = Self.loadSavedPaletteColor(
                forKey: "ARMSX2iOSControllerFocusBoxCustomColor"
            )
            controllerOrbPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerOrbPalette"
                ) ?? ""
            ) ?? .blue
            controllerNavigationAccentPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerNavigationAccentPalette"
                ) ?? ""
            ) ?? .henyBlue
            controllerNavigationCustomAccentColor = Self.loadSavedPaletteColor(
                forKey: "ARMSX2iOSControllerNavigationCustomAccentColor"
            )
            controllerTextPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerTextPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerTextCustomColor = Self.loadSavedPaletteColor(
                forKey: "ARMSX2iOSControllerTextCustomColor"
            )
            controllerSecondaryTextPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerSecondaryTextPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerCriticalTextPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerCriticalTextPalette"
                ) ?? ""
            ) ?? .crimson
            controllerTabTitlePalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerTabTitlePalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerTabSubtitlePalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerTabSubtitlePalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerBottomTabBarPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerBottomTabBarPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerBottomTabBarUnselectedPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerBottomTabBarUnselectedPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerCardTitlePalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerCardTitlePalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerContextMenuPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerContextMenuPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerImportActionPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerImportActionPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerToolbarPalette = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerToolbarPalette"
            ).flatMap(ThemePalette.init(rawValue:))
            controllerFocusedTextPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerFocusedTextPalette"
                ) ?? ""
            ) ?? .blue
            controllerFocusedTextCustomColor = Self.loadSavedPaletteColor(
                forKey: "ARMSX2iOSControllerFocusedTextCustomColor"
            )
            controllerTextShadowStrength = min(
                max(
                    UserDefaults.standard.object(
                        forKey: "ARMSX2iOSControllerTextShadowStrength"
                    ) as? Double ?? 0,
                    0
                ),
                1
            )
            controllerTextShadowPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerTextShadowPalette"
                ) ?? ""
            ) ?? .obsidian
            controllerTextShadowCustomColor = Self.loadSavedPaletteColor(
                forKey: "ARMSX2iOSControllerTextShadowCustomColor"
            )
            controllerFocusedTextShadowStrength = min(
                max(
                    UserDefaults.standard.object(
                        forKey: "ARMSX2iOSControllerFocusedTextShadowStrength"
                    ) as? Double ?? 0.1,
                    0
                ),
                1
            )
            controllerFocusedTextShadowPalette = ThemePalette(
                rawValue: UserDefaults.standard.string(
                    forKey: "ARMSX2iOSControllerFocusedTextShadowPalette"
                ) ?? ""
            ) ?? .obsidian
            controllerFocusedTextShadowCustomColor = Self.loadSavedPaletteColor(
                forKey: "ARMSX2iOSControllerFocusedTextShadowCustomColor"
            )
            controllerFocusBoxAnimationSpeed = min(
                max(
                    UserDefaults.standard.object(
                        forKey: "ARMSX2iOSControllerFocusBoxAnimationSpeed"
                    ) as? Double ?? 1,
                    0.25
                ),
                2
            )
            controllerFocusBoxGlowIntensity = min(
                max(
                    UserDefaults.standard.object(
                        forKey: "ARMSX2iOSControllerFocusBoxGlowIntensity"
                    ) as? Double ?? 1,
                    0
                ),
                2
            )
            backgroundPrimaryAsset = Self.loadBackgroundAsset(forKey: "ARMSX2iOSBackgroundPrimaryAsset")
            backgroundLandscapeAsset = Self.loadBackgroundAsset(forKey: "ARMSX2iOSBackgroundLandscapeAsset")
            backgroundFitMode = BackgroundFitMode(rawValue: UserDefaults.standard.string(forKey: "ARMSX2iOSBackgroundFitMode") ?? "") ?? .fill
            backgroundLandscapeFitMode = BackgroundFitMode(rawValue: UserDefaults.standard.string(forKey: "ARMSX2iOSBackgroundLandscapeFitMode") ?? "") ?? .fill
            backgroundVideoMuted = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundVideoMuted") as? Bool ?? true
            backgroundEnabledInBIOS = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInBIOS") as? Bool ?? true
            backgroundEnabledInHelp = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInHelp") as? Bool ?? true
            backgroundEnabledInSettings = UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundEnabledInSettings") as? Bool ?? true
            backgroundDim = Self.clampedBackgroundDim(UserDefaults.standard.object(forKey: "ARMSX2iOSBackgroundDim") as? Double ?? 0.0)
        }
    }
}
