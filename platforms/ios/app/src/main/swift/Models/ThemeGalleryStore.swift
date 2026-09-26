// ThemeGalleryStore.swift — Persistent, versioned appearance snapshots.
// SPDX-License-Identifier: GPL-3.0+
import SwiftUI
import Observation
import Foundation

/// Game Library View Options are stored outside SettingsStore so the large
/// library does not observe the complete Appearance model. Keep them in the
/// same portable theme document through a small value-only snapshot.
struct GameLibraryViewOptionsSnapshot: Codable {
    var doubles: [String: Double]
    var booleans: [String: Bool]

    private static let doubleDefaults: [String: Double] = [
        "ARMSX2iOSExperimentalPortraitGameCardScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardScale": 1,
        "ARMSX2iOSExperimentalPortraitGameCardWidthScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardWidthScale": 1,
        "ARMSX2iOSExperimentalPortraitGameCardHeightScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardHeightScale": 1,
        "ARMSX2iOSPortraitGameLibraryVerticalPosition": 0,
        "ARMSX2iOSLandscapeGameLibraryVerticalPosition": 0,
        "ARMSX2iOSExperimentalPortraitGameCardGapScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardGapScale": 1,
        "ARMSX2iOSExperimentalPortraitGameCardContentPaddingScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardContentPaddingScale": 1,
        "ARMSX2iOSExperimentalPortraitGameCardVerticalPaddingScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardVerticalPaddingScale": 1,
        "ARMSX2iOSExperimentalPortraitGameCardTextSpacingScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameCardTextSpacingScale": 1,
        "ARMSX2iOSExperimentalPortraitGameNameTextScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameNameTextScale": 1,
        "ARMSX2iOSExperimentalPortraitGameInfoTextScale": 1,
        "ARMSX2iOSExperimentalLandscapeGameInfoTextScale": 1,
        "ARMSX2iOSExperimentalPortraitGameCardCornerRadius": 18,
        "ARMSX2iOSExperimentalLandscapeGameCardCornerRadius": 18,
        "ARMSX2iOSExperimentalPortraitGameCoverCornerRadius": 10,
        "ARMSX2iOSExperimentalLandscapeGameCoverCornerRadius": 10,
        "ARMSX2iOSExperimentalPortraitGameCoverShadow": 1,
        "ARMSX2iOSExperimentalLandscapeGameCoverShadow": 1,
        "ARMSX2iOSExperimentalPortraitGameCoverOpacity": 1,
        "ARMSX2iOSExperimentalLandscapeGameCoverOpacity": 1,
        "ARMSX2iOSPortraitBottomNavigationTabBarHeight": 84,
        "ARMSX2iOSLandscapeBottomNavigationTabBarHeight": 84,
        "ARMSX2iOSPortraitBottomNavigationTabBarIconScale": 1,
        "ARMSX2iOSLandscapeBottomNavigationTabBarIconScale": 1,
        "ARMSX2iOSPortraitBottomNavigationTabBarLabelScale": 1,
        "ARMSX2iOSLandscapeBottomNavigationTabBarLabelScale": 1,
        "ARMSX2iOSPortraitBottomNavigationTabBarClearance": 0,
        "ARMSX2iOSLandscapeBottomNavigationTabBarClearance": 0,
    ]

    private static let booleanDefaults: [String: Bool] = [
        "ARMSX2iOSPortraitUseGamesLogo": true,
        "ARMSX2iOSLandscapeUseGamesLogo": true,
        "ARMSX2iOSPortraitHideGamesScreenTitle": true,
        "ARMSX2iOSLandscapeHideGamesScreenTitle": true,
        "ARMSX2iOSPortraitHideGameName": false,
        "ARMSX2iOSLandscapeHideGameName": false,
        "ARMSX2iOSPortraitHideGameInfo": false,
        "ARMSX2iOSLandscapeHideGameInfo": false,
        "ARMSX2iOSPortraitHideFavoriteButton": false,
        "ARMSX2iOSLandscapeHideFavoriteButton": false,
        "ARMSX2iOSPortraitHideRegionFlag": false,
        "ARMSX2iOSLandscapeHideRegionFlag": false,
        "ARMSX2iOSPortraitSingleLineGameNames": false,
        "ARMSX2iOSLandscapeSingleLineGameNames": false,
        "ARMSX2iOSPortraitHideRunningIndicator": false,
        "ARMSX2iOSLandscapeHideRunningIndicator": false,
        "ARMSX2iOSPortraitBottomNavigationTabBarShowsLabels": true,
        "ARMSX2iOSLandscapeBottomNavigationTabBarShowsLabels": true,
    ]

    init(defaults: UserDefaults = .standard) {
        doubles = [:]
        for (key, fallback) in Self.doubleDefaults {
            doubles[key] = (defaults.object(forKey: key) as? NSNumber)?
                .doubleValue ?? fallback
        }
        booleans = [:]
        for (key, fallback) in Self.booleanDefaults {
            let stored = (defaults.object(forKey: key) as? NSNumber)?.intValue
            booleans[key] = stored.map { $0 >= 0 ? $0 != 0 : fallback }
                ?? fallback
        }
    }

    func restore(defaults: UserDefaults = .standard) {
        for (key, value) in doubles where Self.doubleDefaults[key] != nil {
            defaults.set(value, forKey: key)
        }
        for (key, value) in booleans where Self.booleanDefaults[key] != nil {
            defaults.set(value ? 1 : 0, forKey: key)
        }
    }
}

/// Store values only: no views, images, renderers, or keyboard models.
struct AppearanceThemeSnapshot: Codable {
    var version = 1
    var controllerUIThemePreset: ControllerUIThemePreset
    var controllerFocusBoxStyle: ControllerFocusBoxStyle
    var controllerNavigationFocusAnimation: ControllerNavigationFocusTravelStyle
    var controllerFocusBoxPalette: ThemePalette
    var controllerFocusBoxCustomColor: SavedPaletteColor?
    var controllerOrbPalette: ThemePalette
    var controllerNavigationAccentPalette: ThemePalette
    var controllerNavigationCustomAccentColor: SavedPaletteColor?
    var controllerTextPalette: ThemePalette?
    var controllerTextCustomColor: SavedPaletteColor?
    var controllerSecondaryTextPalette: ThemePalette?
    var controllerCriticalTextPalette: ThemePalette
    var controllerTabTitlePalette: ThemePalette?
    var controllerTabSubtitlePalette: ThemePalette?
    var controllerBottomTabBarPalette: ThemePalette?
    var controllerBottomTabBarUnselectedPalette: ThemePalette?
    var controllerCardTitlePalette: ThemePalette?
    var controllerContextMenuPalette: ThemePalette?
    var controllerImportActionPalette: ThemePalette?
    var controllerToolbarPalette: ThemePalette?
    var controllerFocusedTextPalette: ThemePalette
    var controllerFocusedTextCustomColor: SavedPaletteColor?
    var controllerTextShadowStrength: Double
    var controllerTextShadowPalette: ThemePalette
    var controllerTextShadowCustomColor: SavedPaletteColor?
    var controllerFocusedTextShadowStrength: Double
    var controllerFocusedTextShadowPalette: ThemePalette
    var controllerFocusedTextShadowCustomColor: SavedPaletteColor?
    var controllerFocusBoxAnimationSpeed: Double
    var controllerFocusBoxGlowIntensity: Double
    var focusOrbsEnabled: Bool
    var dynamicBackgroundsEnabled: Bool
    var dynamicAppearancePreferences: DynamicAppearancePreferences
    var backgroundPrimaryAsset: BackgroundAsset?
    var backgroundLandscapeAsset: BackgroundAsset?
    var backgroundFitMode: BackgroundFitMode
    var backgroundLandscapeFitMode: BackgroundFitMode
    var backgroundVideoMuted: Bool
    var backgroundDim: Double
    var backgroundEnabledInBIOS: Bool
    var backgroundEnabledInSettings: Bool
    var clearLiquidGlassUI: Bool
    var clearLiquidGlassUISubSettings: Bool
    var clearLiquidGlassUIQuickMenu: Bool
    var clearLiquidGlassUIPerGameSettingsLibrary: Bool
    var clearLiquidGlassUIPerGameSettingsEmulation: Bool
    var controllerCustomThemeBase: ControllerUIThemePreset
    var controllerRoleCustomColors: [String: SavedPaletteColor]
    var gameLibraryViewOptions: GameLibraryViewOptionsSnapshot?

    @MainActor init(settings: SettingsStore) {
        controllerUIThemePreset = settings.controllerUIThemePreset
        controllerFocusBoxStyle = settings.controllerFocusBoxStyle
        controllerNavigationFocusAnimation = settings.controllerNavigationFocusAnimation
        controllerFocusBoxPalette = settings.controllerFocusBoxPalette
        controllerFocusBoxCustomColor = settings.controllerFocusBoxCustomColor
        controllerOrbPalette = settings.controllerOrbPalette
        controllerNavigationAccentPalette = settings.controllerNavigationAccentPalette
        controllerNavigationCustomAccentColor = settings.controllerNavigationCustomAccentColor
        controllerTextPalette = settings.controllerTextPalette
        controllerTextCustomColor = settings.controllerTextCustomColor
        controllerSecondaryTextPalette = settings.controllerSecondaryTextPalette
        controllerCriticalTextPalette = settings.controllerCriticalTextPalette
        controllerTabTitlePalette = settings.controllerTabTitlePalette
        controllerTabSubtitlePalette = settings.controllerTabSubtitlePalette
        controllerBottomTabBarPalette = settings.controllerBottomTabBarPalette
        controllerBottomTabBarUnselectedPalette = settings.controllerBottomTabBarUnselectedPalette
        controllerCardTitlePalette = settings.controllerCardTitlePalette
        controllerContextMenuPalette = settings.controllerContextMenuPalette
        controllerImportActionPalette = settings.controllerImportActionPalette
        controllerToolbarPalette = settings.controllerToolbarPalette
        controllerFocusedTextPalette = settings.controllerFocusedTextPalette
        controllerFocusedTextCustomColor = settings.controllerFocusedTextCustomColor
        controllerTextShadowStrength = settings.controllerTextShadowStrength
        controllerTextShadowPalette = settings.controllerTextShadowPalette
        controllerTextShadowCustomColor = settings.controllerTextShadowCustomColor
        controllerFocusedTextShadowStrength = settings.controllerFocusedTextShadowStrength
        controllerFocusedTextShadowPalette = settings.controllerFocusedTextShadowPalette
        controllerFocusedTextShadowCustomColor = settings.controllerFocusedTextShadowCustomColor
        controllerFocusBoxAnimationSpeed = settings.controllerFocusBoxAnimationSpeed
        controllerFocusBoxGlowIntensity = settings.controllerFocusBoxGlowIntensity
        focusOrbsEnabled = settings.focusOrbsEnabled
        dynamicBackgroundsEnabled = settings.dynamicBackgroundsEnabled
        dynamicAppearancePreferences = settings.dynamicAppearancePreferences
        backgroundPrimaryAsset = settings.backgroundPrimaryAsset
        backgroundLandscapeAsset = settings.backgroundLandscapeAsset
        backgroundFitMode = settings.backgroundFitMode
        backgroundLandscapeFitMode = settings.backgroundLandscapeFitMode
        backgroundVideoMuted = settings.backgroundVideoMuted
        backgroundDim = settings.backgroundDim
        backgroundEnabledInBIOS = settings.backgroundEnabledInBIOS
        backgroundEnabledInSettings = settings.backgroundEnabledInSettings
        clearLiquidGlassUI = settings.clearLiquidGlassUI
        clearLiquidGlassUISubSettings = settings.clearLiquidGlassUISubSettings
        clearLiquidGlassUIQuickMenu = settings.clearLiquidGlassUIQuickMenu
        clearLiquidGlassUIPerGameSettingsLibrary = settings.clearLiquidGlassUIPerGameSettingsLibrary
        clearLiquidGlassUIPerGameSettingsEmulation = settings.clearLiquidGlassUIPerGameSettingsEmulation
        controllerCustomThemeBase = settings.controllerCustomThemeBase
        controllerRoleCustomColors = settings.controllerRoleCustomColors
        gameLibraryViewOptions = GameLibraryViewOptionsSnapshot()
    }

    @MainActor func restore(to settings: SettingsStore) {
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            settings.controllerFocusBoxStyle = controllerFocusBoxStyle
            settings.controllerNavigationFocusAnimation = controllerNavigationFocusAnimation
            settings.controllerFocusBoxPalette = controllerFocusBoxPalette
            settings.controllerFocusBoxCustomColor = controllerFocusBoxCustomColor
            settings.controllerOrbPalette = controllerOrbPalette
            settings.controllerNavigationAccentPalette = controllerNavigationAccentPalette
            settings.controllerNavigationCustomAccentColor = controllerNavigationCustomAccentColor
            settings.controllerTextPalette = controllerTextPalette
            settings.controllerTextCustomColor = controllerTextCustomColor
            settings.controllerSecondaryTextPalette = controllerSecondaryTextPalette
            settings.controllerCriticalTextPalette = controllerCriticalTextPalette
            settings.controllerTabTitlePalette = controllerTabTitlePalette
            settings.controllerTabSubtitlePalette = controllerTabSubtitlePalette
            settings.controllerBottomTabBarPalette = controllerBottomTabBarPalette
            settings.controllerBottomTabBarUnselectedPalette = controllerBottomTabBarUnselectedPalette
            settings.controllerCardTitlePalette = controllerCardTitlePalette
            settings.controllerContextMenuPalette = controllerContextMenuPalette
            settings.controllerImportActionPalette = controllerImportActionPalette
            settings.controllerToolbarPalette = controllerToolbarPalette
            settings.controllerFocusedTextPalette = controllerFocusedTextPalette
            settings.controllerFocusedTextCustomColor = controllerFocusedTextCustomColor
            settings.controllerTextShadowStrength = controllerTextShadowStrength
            settings.controllerTextShadowPalette = controllerTextShadowPalette
            settings.controllerTextShadowCustomColor = controllerTextShadowCustomColor
            settings.controllerFocusedTextShadowStrength = controllerFocusedTextShadowStrength
            settings.controllerFocusedTextShadowPalette = controllerFocusedTextShadowPalette
            settings.controllerFocusedTextShadowCustomColor = controllerFocusedTextShadowCustomColor
            settings.controllerFocusBoxAnimationSpeed = controllerFocusBoxAnimationSpeed
            settings.controllerFocusBoxGlowIntensity = controllerFocusBoxGlowIntensity
            settings.focusOrbsEnabled = focusOrbsEnabled
            settings.dynamicBackgroundsEnabled = dynamicBackgroundsEnabled
            settings.dynamicAppearancePreferences = dynamicAppearancePreferences
            settings.backgroundPrimaryAsset = backgroundPrimaryAsset
            settings.backgroundLandscapeAsset = backgroundLandscapeAsset
            settings.backgroundFitMode = backgroundFitMode
            settings.backgroundLandscapeFitMode = backgroundLandscapeFitMode
            settings.backgroundVideoMuted = backgroundVideoMuted
            settings.backgroundDim = backgroundDim
            settings.backgroundEnabledInBIOS = backgroundEnabledInBIOS
            settings.backgroundEnabledInSettings = backgroundEnabledInSettings
            settings.clearLiquidGlassUI = clearLiquidGlassUI
            settings.clearLiquidGlassUISubSettings = clearLiquidGlassUISubSettings
            settings.clearLiquidGlassUIQuickMenu = clearLiquidGlassUIQuickMenu
            settings.clearLiquidGlassUIPerGameSettingsLibrary = clearLiquidGlassUIPerGameSettingsLibrary
            settings.clearLiquidGlassUIPerGameSettingsEmulation = clearLiquidGlassUIPerGameSettingsEmulation
            settings.controllerCustomThemeBase = controllerCustomThemeBase
            settings.controllerRoleCustomColors = controllerRoleCustomColors
            settings.controllerUIThemePreset = controllerUIThemePreset
            gameLibraryViewOptions?.restore()
        }
    }

    /// Ender Pearl-Pul is a visual colour/background recipe rather than a
    /// complete UI-layout snapshot. Applying only these fields prevents the
    /// imported document from changing card dimensions, tab-bar geometry,
    /// glass preferences, focus animation, or any other user-owned setting.
    @MainActor func restoreColoursAndDynamicBackground(
        to settings: SettingsStore
    ) {
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            settings.controllerFocusBoxPalette = controllerFocusBoxPalette
            settings.controllerFocusBoxCustomColor = controllerFocusBoxCustomColor
            settings.controllerOrbPalette = controllerOrbPalette
            settings.controllerNavigationAccentPalette = controllerNavigationAccentPalette
            settings.controllerNavigationCustomAccentColor = controllerNavigationCustomAccentColor
            settings.controllerTextPalette = controllerTextPalette
            settings.controllerTextCustomColor = controllerTextCustomColor
            settings.controllerSecondaryTextPalette = controllerSecondaryTextPalette
            settings.controllerCriticalTextPalette = controllerCriticalTextPalette
            settings.controllerTabTitlePalette = controllerTabTitlePalette
            settings.controllerTabSubtitlePalette = controllerTabSubtitlePalette
            settings.controllerBottomTabBarPalette = controllerBottomTabBarPalette
            settings.controllerBottomTabBarUnselectedPalette = controllerBottomTabBarUnselectedPalette
            settings.controllerCardTitlePalette = controllerCardTitlePalette
            settings.controllerContextMenuPalette = controllerContextMenuPalette
            settings.controllerImportActionPalette = controllerImportActionPalette
            settings.controllerToolbarPalette = controllerToolbarPalette
            settings.controllerFocusedTextPalette = controllerFocusedTextPalette
            settings.controllerFocusedTextCustomColor = controllerFocusedTextCustomColor
            settings.controllerTextShadowPalette = controllerTextShadowPalette
            settings.controllerTextShadowCustomColor = controllerTextShadowCustomColor
            settings.controllerFocusedTextShadowPalette = controllerFocusedTextShadowPalette
            settings.controllerFocusedTextShadowCustomColor = controllerFocusedTextShadowCustomColor
            settings.controllerRoleCustomColors = controllerRoleCustomColors
            settings.dynamicBackgroundsEnabled = dynamicBackgroundsEnabled
            settings.dynamicAppearancePreferences = dynamicAppearancePreferences
        }
    }
}

struct SavedAppearanceTheme: Codable, Identifiable {
    let id: UUID
    var name: String
    var snapshot: AppearanceThemeSnapshot
}

/// Portable JSON envelope for one complete Appearance preset. Keeping a
/// format identifier outside the snapshot lets future versions reject an
/// unrelated JSON document without partially changing the interface.
private struct AppearanceThemeTransferDocument: Codable {
    static let format = "com.armsx2.appearance-theme"

    var format: String
    var version: Int
    var name: String
    var snapshot: AppearanceThemeSnapshot
}

private enum AppearanceThemeTransferError: LocalizedError {
    case invalidDocument
    case unsupportedVersion(Int)
    case couldNotSave

    var errorDescription: String? {
        switch self {
        case .invalidDocument:
            return "This JSON file is not an ARMSX2 theme preset."
        case .unsupportedVersion(let version):
            return "This theme preset uses unsupported format version \(version)."
        case .couldNotSave:
            return "The imported theme preset could not be saved."
        }
    }
}

/// One ordered identity shared by the Appearance picker and the main-menu
/// L2/R2 shortcut. Saved themes belong directly after Custom instead of living
/// in a disconnected gallery-only sequence.
enum AppearanceThemeSelection: Hashable, Identifiable {
    case preset(ControllerUIThemePreset)
    case saved(UUID)

    var id: String {
        switch self {
        case .preset(let preset):
            return "preset.\(preset.rawValue)"
        case .saved(let id):
            return "saved.\(id.uuidString)"
        }
    }
}

@MainActor @Observable
final class ThemeGalleryStore {
    static let shared = ThemeGalleryStore()
    private(set) var themes: [SavedAppearanceTheme]
    private(set) var customDraft: AppearanceThemeSnapshot?
    private(set) var activeSavedThemeID: UUID?
    // Hide the presenting menu without destroying its navigation
    // stack, so previews reveal the one existing background renderer.
    var isPreviewing = false
    @ObservationIgnored private let defaults: UserDefaults
    @ObservationIgnored private var customDraftPersistenceTask:
        Task<Void, Never>?
    private static let themesKey = "ARMSX2iOSSavedAppearanceThemesV1"
    private static let draftKey = "ARMSX2iOSCustomAppearanceDraftV1"
    private static let activeSavedThemeKey =
        "ARMSX2iOSActiveSavedAppearanceThemeV1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        themes = defaults.data(forKey: Self.themesKey).flatMap {
            try? JSONDecoder().decode([SavedAppearanceTheme].self, from: $0)
        } ?? []
        customDraft = defaults.data(forKey: Self.draftKey).flatMap {
            try? JSONDecoder().decode(AppearanceThemeSnapshot.self, from: $0)
        }
        let storedActiveID = defaults.string(
            forKey: Self.activeSavedThemeKey
        ).flatMap(UUID.init(uuidString:))
        if let storedActiveID,
           themes.contains(where: { $0.id == storedActiveID }) {
            activeSavedThemeID = storedActiveID
        } else {
            activeSavedThemeID = nil
        }
    }

    var orderedSelections: [AppearanceThemeSelection] {
        [.preset(.custom)]
            + themes.map { .saved($0.id) }
            + ControllerUIThemePreset.allCases
                .filter { $0 != .custom }
                .map(AppearanceThemeSelection.preset)
    }

    func currentSelection(
        for settings: SettingsStore
    ) -> AppearanceThemeSelection {
        if let activeSavedThemeID,
           themes.contains(where: { $0.id == activeSavedThemeID }) {
            return .saved(activeSavedThemeID)
        }
        return .preset(settings.controllerUIThemePreset)
    }

    func title(
        for selection: AppearanceThemeSelection
    ) -> String {
        switch selection {
        case .preset(let preset):
            return preset.title
        case .saved(let id):
            return themes.first(where: { $0.id == id })?.name ?? "Custom"
        }
    }

    func apply(
        _ selection: AppearanceThemeSelection,
        to settings: SettingsStore,
        preservingCustomTheme: Bool = true
    ) {
        if preservingCustomTheme {
            settings.markAppearanceUserModified()
        }
        switch selection {
        case .preset(let preset):
            clearActiveSavedTheme()
            settings.applyControllerUIThemePreset(
                preset,
                preservingCustomTheme: preservingCustomTheme
            )
        case .saved(let id):
            guard let theme = themes.first(where: { $0.id == id }) else {
                clearActiveSavedTheme()
                settings.applyControllerUIThemePreset(
                    .custom,
                    preservingCustomTheme: preservingCustomTheme
                )
                return
            }
            if preservingCustomTheme {
                preserveCustomTheme(settings)
            }
            theme.snapshot.restore(to: settings)
            setActiveSavedTheme(id)
        }
    }

    func setActiveSavedTheme(_ id: UUID) {
        guard themes.contains(where: { $0.id == id }) else {
            clearActiveSavedTheme()
            return
        }
        activeSavedThemeID = id
        defaults.set(id.uuidString, forKey: Self.activeSavedThemeKey)
    }

    func clearActiveSavedTheme() {
        guard activeSavedThemeID != nil
                || defaults.object(forKey: Self.activeSavedThemeKey) != nil
        else { return }
        activeSavedThemeID = nil
        defaults.removeObject(forKey: Self.activeSavedThemeKey)
    }

    /// Applies a bundled transfer document as a first-class built-in preset.
    /// Shipping the same portable JSON accepted by Import keeps the gallery
    /// preset byte-for-byte aligned with the user-provided theme.
    @discardableResult
    func applyBundledPreset(
        _ preset: ControllerUIThemePreset,
        to settings: SettingsStore
    ) -> Bool {
        let resourceName: String
        switch preset {
        case .purpleMilk:
            resourceName = "Purple-Milk.armsx2-theme"
        case .enderPearlPul:
            resourceName = "Ender-Pearl-Pul.armsx2-theme"
        default:
            return false
        }

        guard let url = Bundle.main.url(
                  forResource: resourceName,
                  withExtension: "json"
              ),
              let data = try? Data(contentsOf: url),
              let document = try? JSONDecoder().decode(
                  AppearanceThemeTransferDocument.self,
                  from: data
              ),
              document.format == AppearanceThemeTransferDocument.format,
              document.version == 1 else {
            return false
        }
        let preservesOrbVisibility = settings.focusOrbsEnabled
        var bundledSnapshot = document.snapshot
        // Orb visibility is an explicit user preference, not part of a theme.
        bundledSnapshot.focusOrbsEnabled = preservesOrbVisibility
        if preset == .enderPearlPul {
            // This bundled preset must remain self-contained. The original
            // imported document referenced a machine-local video and enabled
            // focus orbs as a side effect of selecting the theme.
            bundledSnapshot.backgroundPrimaryAsset = nil
            bundledSnapshot.backgroundLandscapeAsset = nil
        }
        if preset == .enderPearlPul {
            bundledSnapshot.restoreColoursAndDynamicBackground(to: settings)
            settings.controllerFocusBoxStyle = .glowingCorners
        } else {
            bundledSnapshot.restore(to: settings)
        }
        settings.controllerCustomThemeBase = preset
        settings.controllerUIThemePreset = preset
        return true
    }

    func preserveCustomTheme(_ settings: SettingsStore) {
        guard settings.controllerUIThemePreset == .custom,
              activeSavedThemeID == nil else { return }
        preserveCustomSnapshot(AppearanceThemeSnapshot(settings: settings))
    }

    func preserveCustomSnapshot(_ snapshot: AppearanceThemeSnapshot) {
        guard snapshot.controllerUIThemePreset == .custom else { return }
        customDraft = snapshot
        customDraftPersistenceTask?.cancel()
        customDraftPersistenceTask = Task { @MainActor [weak self] in
            // NavigationStack and the controller focus session must finish
            // dismantling the large Appearance Form before JSON persistence
            // gets main-actor time. This removes the visible multi-second
            // focus stall when returning to Settings.
            try? await Task.sleep(for: .milliseconds(250))
            guard let self, !Task.isCancelled,
                  let data = try? JSONEncoder().encode(snapshot) else { return }
            self.defaults.set(data, forKey: Self.draftKey)
            self.customDraftPersistenceTask = nil
        }
    }

    func restoreCustomTheme(_ settings: SettingsStore) {
        clearActiveSavedTheme()
        if let customDraft { customDraft.restore(to: settings) }
        settings.beginCustomThemeEditing()
    }

    func hasUnsavedCustomTheme(_ settings: SettingsStore) -> Bool {
        guard settings.controllerUIThemePreset == .custom else { return false }
        let current = AppearanceThemeSnapshot(settings: settings)
        guard let currentData = encodedSnapshot(current) else { return true }
        return !themes.contains { theme in
            encodedSnapshot(theme.snapshot) == currentData
        }
    }

    @discardableResult
    func save(name: String, settings: SettingsStore) -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return false }
        settings.beginCustomThemeEditing()
        let theme = SavedAppearanceTheme(
            id: UUID(), name: String(trimmed.prefix(80)),
            snapshot: AppearanceThemeSnapshot(settings: settings)
        )
        let updated = themes + [theme]
        guard let data = try? JSONEncoder().encode(updated) else { return false }
        defaults.set(data, forKey: Self.themesKey)
        themes = updated
        preserveCustomTheme(settings)
        setActiveSavedTheme(theme.id)
        return true
    }

    func remove(_ id: UUID) {
        let updated = themes.filter { $0.id != id }
        guard let data = try? JSONEncoder().encode(updated) else { return }
        defaults.set(data, forKey: Self.themesKey)
        themes = updated
        if activeSavedThemeID == id {
            clearActiveSavedTheme()
        }
    }

    /// Creates a shareable JSON file containing every value represented by
    /// `AppearanceThemeSnapshot`, including dynamic-background configuration.
    func exportCurrentTheme(from settings: SettingsStore) throws -> URL {
        let selection = currentSelection(for: settings)
        let name = title(for: selection)
        let document = AppearanceThemeTransferDocument(
            format: AppearanceThemeTransferDocument.format,
            version: 1,
            name: name,
            snapshot: AppearanceThemeSnapshot(settings: settings)
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        let data = try encoder.encode(document)
        let fileName = sanitizedFileName(name)
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(fileName).armsx2-theme.json")
        try data.write(to: url, options: .atomic)
        return url
    }

    /// Imports, persists, and activates a complete JSON theme as one saved
    /// Theme Gallery entry. The new UUID prevents an imported file from
    /// overwriting an existing user theme with the same name.
    @discardableResult
    func importTheme(
        from url: URL,
        applyingTo settings: SettingsStore
    ) throws -> SavedAppearanceTheme {
        let grantedAccess = url.startAccessingSecurityScopedResource()
        defer {
            if grantedAccess { url.stopAccessingSecurityScopedResource() }
        }

        let data = try Data(contentsOf: url)
        guard let document = try? JSONDecoder().decode(
            AppearanceThemeTransferDocument.self,
            from: data
        ), document.format == AppearanceThemeTransferDocument.format else {
            throw AppearanceThemeTransferError.invalidDocument
        }
        guard document.version == 1 else {
            throw AppearanceThemeTransferError.unsupportedVersion(
                document.version
            )
        }
        let name = document.name.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        let imported = SavedAppearanceTheme(
            id: UUID(),
            name: String((name.isEmpty ? "Imported Theme" : name).prefix(80)),
            snapshot: document.snapshot
        )
        let updated = themes + [imported]
        guard let encoded = try? JSONEncoder().encode(updated) else {
            throw AppearanceThemeTransferError.couldNotSave
        }
        defaults.set(encoded, forKey: Self.themesKey)
        themes = updated
        settings.markAppearanceUserModified()
        imported.snapshot.restore(to: settings)
        setActiveSavedTheme(imported.id)
        return imported
    }

    private func sanitizedFileName(_ value: String) -> String {
        let forbidden = CharacterSet.alphanumerics.union(
            CharacterSet(charactersIn: "-_ ")
        ).inverted
        let safe = value.components(separatedBy: forbidden).joined()
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: " ", with: "-")
        return safe.isEmpty ? "ARMSX2-Theme" : String(safe.prefix(64))
    }

    private func encodedSnapshot(_ snapshot: AppearanceThemeSnapshot) -> Data? {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return try? encoder.encode(snapshot)
    }
}

extension SettingsStore {
    var controllerEffectiveThemePreset: ControllerUIThemePreset {
        controllerUIThemePreset == .custom ? controllerCustomThemeBase : controllerUIThemePreset
    }

    func beginCustomThemeEditing() {
        markAppearanceUserModified()
        // A changed saved theme is a new Custom draft. Retiring the saved
        // identity also makes Save Custom Theme eligible again.
        ThemeGalleryStore.shared.clearActiveSavedTheme()
        if controllerUIThemePreset != .custom {
            // The Custom base already owns the preset's semantic/readability
            // roles. Copying every resolved colour here issued a burst of
            // observable writes and forced iOS 26 to rematerialize the active
            // Picker's glass row, which could leave it black until another
            // row invalidated the Form. Preserve the base identity instead;
            // explicit custom palettes and colours continue to override it.
            controllerCustomThemeBase = controllerUIThemePreset
            controllerUIThemePreset = .custom
        }
    }

    func customRoleColor(_ role: String) -> Color? {
        controllerRoleCustomColors[role]?.color
    }

    var controllerOrbColors: [Color] {
        if let color = customRoleColor("focus-orb-colours") { return [color] }
        return controllerOrbPalette.colors
    }
}
