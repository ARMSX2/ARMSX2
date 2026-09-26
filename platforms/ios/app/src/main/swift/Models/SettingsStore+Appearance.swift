// SPDX-License-Identifier: GPL-3.0+
import SwiftUI

private enum AutomaticJITAppearanceState {
    static let userModifiedKey = "ARMSX2iOSAppearanceUserModifiedV1"
    static let automaticThemeActiveKey =
        "ARMSX2iOSAutomaticNoJITAppearanceActiveV1"

    /// Existing installations must be treated conservatively. These keys
    /// predate the ownership marker and prove that an appearance choice may
    /// already belong to the user.
    static let legacyAppearanceKeys = [
        "ARMSX2iOSControllerUIThemePreset",
        "ARMSX2iOSDynamicAppearancePreferences",
        "ARMSX2iOSCustomAppearanceDraftV1",
        "ARMSX2iOSSavedAppearanceThemesV1",
        "ARMSX2iOSBackgroundPrimaryAsset",
        "ARMSX2iOSBackgroundLandscapeAsset",
    ]
}

// Computed appearance behavior is isolated from the monolithic
// stored-property declaration. Stored observable state remains on the main type.
extension SettingsStore {
    static func initializeAutomaticJITAppearanceState() {
        let defaults = UserDefaults.standard
        guard defaults.object(
            forKey: AutomaticJITAppearanceState.userModifiedKey
        ) == nil else { return }

        let wasAlreadyAutomatic = defaults.bool(
            forKey: AutomaticJITAppearanceState.automaticThemeActiveKey
        )
        let hasExistingAppearance =
            AutomaticJITAppearanceState.legacyAppearanceKeys.contains {
                defaults.object(forKey: $0) != nil
            }
        defaults.set(
            hasExistingAppearance && !wasAlreadyAutomatic,
            forKey: AutomaticJITAppearanceState.userModifiedKey
        )
    }

    /// Records explicit theme selection or editing. Once the user owns the
    /// appearance, later JIT state changes must never replace it.
    func markAppearanceUserModified() {
        let defaults = UserDefaults.standard
        defaults.set(
            true,
            forKey: AutomaticJITAppearanceState.userModifiedKey
        )
        defaults.set(
            false,
            forKey: AutomaticJITAppearanceState.automaticThemeActiveKey
        )
    }

    /// Applies the warning palette only to a truly untouched fresh install.
    /// It changes the established PS2 background colour inputs without
    /// replacing a saved or customized theme snapshot.
    func applyAutomaticNoJITAppearanceIfEligible() {
        let defaults = UserDefaults.standard
        guard !defaults.bool(
            forKey: AutomaticJITAppearanceState.userModifiedKey
        ), !defaults.bool(
            forKey: AutomaticJITAppearanceState.automaticThemeActiveKey
        ) else { return }

        var preferences = dynamicAppearancePreferences
        preferences.dynamicBackground = .playStation2Menu
        preferences.sharedPalette = .crimson
        preferences.sharedCustomColor = nil
        preferences.sharedMultiColor = ThemeMultiColorSelection()
        preferences.ribbonPalette = .silver
        preferences.ribbonCustomColor = nil
        preferences.ribbonMultiColor = ThemeMultiColorSelection()
        preferences.hasSelectedPlayStation3XMBByMart = false
        preferences.isPlayStation3XMBPresetExplicit = false
        dynamicBackgroundsEnabled = true
        dynamicAppearancePreferences = preferences
        defaults.set(
            true,
            forKey: AutomaticJITAppearanceState.automaticThemeActiveKey
        )
    }

    /// Reverts the untouched automatic warning appearance once JIT becomes
    /// available. An intervening user edit clears the automatic marker.
    func restoreDefaultAppearanceAfterAutomaticNoJITIfNeeded(
        jitAvailable: Bool
    ) {
        guard jitAvailable else { return }
        let defaults = UserDefaults.standard
        guard defaults.bool(
            forKey: AutomaticJITAppearanceState.automaticThemeActiveKey
        ), !defaults.bool(
            forKey: AutomaticJITAppearanceState.userModifiedKey
        ) else { return }

        defaults.set(
            false,
            forKey: AutomaticJITAppearanceState.automaticThemeActiveKey
        )
        applyControllerUIThemePreset(
            .defaultTheme,
            preservingCustomTheme: false
        )
    }

    var controllerNavigationAccentColor: Color {
        if let custom = controllerNavigationCustomAccentColor { return custom.color }
        if controllerUIThemePreset == .custom {
            return controllerNavigationAccentPalette.semanticAccentColor
        }
        // Existing installs can retain the old `.blue` selection even after
        // the Default/Heny Blue preset definition changes. Resolve these two
        // presets semantically so every action icon uses #57B8F9 immediately,
        // without requiring the user to reselect the preset.
        if controllerEffectiveThemePreset == .defaultTheme
            || controllerEffectiveThemePreset == .henyBlue {
            return ThemePalette.henyBlue.semanticAccentColor
        }
        return controllerNavigationCustomAccentColor?.color
            ?? controllerNavigationAccentPalette.semanticAccentColor
    }

    var controllerTextAppearance: ControllerTextAppearance {
        let presetText = controllerEffectiveThemePreset.textOverrides
        let readability = controllerEffectiveThemePreset.readabilityOverrides
        // Older installs can legitimately contain `Custom` without a chosen
        // Text Colour. Falling straight through to the palette (or system
        // primary in light appearance) makes a tapped Form row dark/black.
        // Disabled custom colours now inherit the Custom base preset's
        // semantic text roles instead.
        let inheritedNormal = readability?.normal ?? presetText?.normal
        let inheritedSecondary = readability?.secondary
            ?? presetText?.secondary
        let baseConfiguration = controllerCustomThemeBase.configuration
        let inheritsBaseTextPalette = controllerUIThemePreset == .custom
            && controllerTextCustomColor == nil
            && controllerTextPalette == baseConfiguration?.textPalette
        let inheritsBaseSecondaryPalette = controllerUIThemePreset == .custom
            && customRoleColor("secondary-text-colour") == nil
            && controllerSecondaryTextPalette
                == baseConfiguration?.secondaryTextPalette
        let inheritsBaseFocusedPalette = controllerUIThemePreset == .custom
            && controllerFocusedTextCustomColor == nil
            && controllerFocusedTextPalette
                == baseConfiguration?.focusedTextPalette
        let normalColor = controllerTextCustomColor?.color
            ?? (controllerUIThemePreset == .custom
                ? (inheritsBaseTextPalette
                    ? inheritedNormal
                    : controllerTextPalette?.semanticInterfaceTextColor)
                    ?? inheritedNormal
                : (readability?.normal ?? presetText?.normal
                    ?? controllerTextPalette?.semanticInterfaceTextColor))
        let secondaryColor = customRoleColor("secondary-text-colour")
            ?? (controllerUIThemePreset == .custom
                ? (inheritsBaseSecondaryPalette
                    ? inheritedSecondary
                    : controllerSecondaryTextPalette?
                        .semanticInterfaceTextColor)
                    ?? inheritedSecondary
                : nil)
            ?? readability?.secondary ?? presetText?.secondary
            ?? controllerSecondaryTextPalette?.semanticInterfaceTextColor
        let titleColor = readability?.title
            ?? presetText?.title
            ?? secondaryColor
            ?? controllerNavigationAccentColor
        return ControllerTextAppearance(
            normalColor: normalColor,
            secondaryColor: secondaryColor,
            titleColor: titleColor,
            // Ordinary copy uses the primary-text role. Secondary is reserved
            // for captions and metadata so theme palettes do not recolor body
            // labels with the secondary accent.
            contentColor: normalColor,
            // The non-selected tab labels use the same semantic title colour
            // as Games/BIOS/Settings instead of falling back to system white.
            unselectedColor: readability?.unselected ?? titleColor,
            focusedColor: controllerFocusedTextCustomColor?.color
                ?? (controllerUIThemePreset == .custom
                    ? (inheritsBaseFocusedPalette
                        ? (readability?.focused ?? presetText?.focused)
                        : controllerFocusedTextPalette
                            .semanticFocusedInterfaceTextColor)
                    : nil)
                ?? readability?.focused
                ?? presetText?.focused
                ?? controllerFocusedTextPalette.semanticFocusedInterfaceTextColor,
            normalShadowColor: controllerTextShadowCustomColor?.color
                ?? controllerTextShadowPalette.semanticInterfaceShadowColor,
            focusedShadowColor: controllerFocusedTextShadowCustomColor?.color
                ?? controllerFocusedTextShadowPalette.semanticInterfaceShadowColor,
            normalShadowStrength: controllerTextShadowStrength,
            focusedShadowStrength: controllerFocusedTextShadowStrength
        )
    }
    var controllerTabTitleColor: Color {
        customRoleColor("tab-titles-colour") ?? controllerTabTitlePalette?.semanticInterfaceTextColor
            ?? controllerEffectiveThemePreset.readabilityOverrides?.tabTitle
            // The master/default tab titles were white. Theme-specific title
            // palettes remain opt-in through Tab Titles Colours.
            ?? .white
    }
    var controllerTabSubtitleColor: Color {
        customRoleColor("tab-subtitles-colour") ?? controllerTabSubtitlePalette?.semanticInterfaceTextColor
            ?? controllerEffectiveThemePreset.readabilityOverrides?.tabSubtitle
            ?? controllerTextAppearance.secondaryColor
            ?? controllerTextAppearance.contentColor
            ?? .secondary
    }
    var controllerBottomTabBarColor: Color {
        customRoleColor("bottom-tab-bar-colour") ?? controllerBottomTabBarPalette?.semanticAccentColor
            ?? controllerNavigationAccentColor
    }
    var controllerBottomTabBarUnselectedColor: Color {
        if let custom = customRoleColor("bottom-tab-bar-unselected-colour") {
            return custom
        }
        if let selected = controllerBottomTabBarUnselectedPalette {
            return selected.semanticInterfaceTextColor
        }
        // Default mirrors the master palette and Heny Blue: active tabs use
        // #57B8F9, while inactive titles/icons remain white in landscape and
        // portrait instead of inheriting the accent.
        if controllerEffectiveThemePreset == .defaultTheme
            || controllerEffectiveThemePreset == .henyBlue
            || (controllerUIThemePreset == .custom
                && controllerCustomThemeBase == .custom) {
            return .white
        }
        return controllerTextAppearance.unselectedColor
    }
    var controllerCardTitleColor: Color {
        customRoleColor("card-titles-colour") ?? controllerCardTitlePalette?.semanticInterfaceTextColor
            ?? controllerEffectiveThemePreset.readabilityOverrides?.cardTitle
            // Preserve the approved presets' established card colours. The
            // audited presets above opt into a primary card role explicitly.
            ?? controllerTextAppearance.contentColor
            ?? .primary
    }
    var controllerCardSubtitleColor: Color {
        controllerEffectiveThemePreset.readabilityOverrides?.cardSubtitle
            ?? controllerTabSubtitleColor
    }
    var controllerCardFocusedColor: Color {
        // A focused library card is an interactive selection, so its title
        // uses the same theme accent as buttons and focus artwork. Several
        // readability profiles intentionally keep normal card copy neutral;
        // reusing that neutral colour for `cardFocused` made the transition
        // imperceptible on most presets.
        controllerNavigationAccentColor
    }
    var controllerContextMenuColor: Color {
        customRoleColor("context-menu-colour") ?? controllerContextMenuPalette?.semanticInterfaceTextColor
            ?? controllerEffectiveThemePreset.readabilityOverrides?.contextPrimary
            ?? controllerTextAppearance.contentColor
            ?? .primary
    }
    var controllerContextMenuSecondaryColor: Color {
        controllerEffectiveThemePreset.readabilityOverrides?.contextSecondary
            ?? controllerTextAppearance.secondaryColor
            ?? .secondary
    }
    var controllerContextMenuFocusedColor: Color {
        controllerEffectiveThemePreset.readabilityOverrides?.contextFocused
            ?? controllerTextAppearance.focusedColor
    }
    var controllerQuickMenuTextAppearance: ControllerTextAppearance {
        guard let readability = controllerEffectiveThemePreset.readabilityOverrides
        else { return controllerTextAppearance }
        var appearance = controllerTextAppearance
        appearance.normalColor = readability.quickMenuPrimary
        appearance.secondaryColor = readability.quickMenuSecondary
        appearance.titleColor = readability.quickMenuPrimary
        appearance.contentColor = readability.quickMenuPrimary
        appearance.unselectedColor = readability.quickMenuSecondary
        appearance.focusedColor = readability.quickMenuFocused
        return appearance
    }
    var controllerImportActionColor: Color {
        customRoleColor("import-actions-colour") ?? controllerImportActionPalette?.semanticInterfaceTextColor
            ?? controllerTextAppearance.contentColor
            ?? .primary
    }
    var controllerToolbarColor: Color {
        if let custom = customRoleColor("toolbar-icons-colour") { return custom }
        if controllerUIThemePreset == .custom {
            return controllerToolbarPalette?.semanticAccentColor
                ?? controllerNavigationAccentColor
        }
        if controllerEffectiveThemePreset == .defaultTheme
            || controllerEffectiveThemePreset == .henyBlue {
            return ThemePalette.henyBlue.semanticAccentColor
        }
        return controllerToolbarPalette?.semanticAccentColor
            ?? controllerNavigationAccentColor
    }
    var controllerCriticalTextColor: Color {
        customRoleColor("critical-text-colour") ?? controllerCriticalTextPalette.semanticAccentColor
    }
    func applyControllerUIThemePreset(
        _ preset: ControllerUIThemePreset,
        preservingCustomTheme: Bool = true
    ) {
        if preset == .custom {
            ThemeGalleryStore.shared.restoreCustomTheme(self)
            return
        }
        if preservingCustomTheme { ThemeGalleryStore.shared.preserveCustomTheme(self) }
        ThemeGalleryStore.shared.clearActiveSavedTheme()
        if (preset == .purpleMilk || preset == .enderPearlPul),
           ThemeGalleryStore.shared.applyBundledPreset(
               preset,
               to: self
           ) {
            backgroundDim = 0
            return
        }
        guard let configuration = preset.configuration else { return }
        controllerRoleCustomColors = [:]
        controllerNavigationCustomAccentColor = nil
        controllerFocusBoxCustomColor = nil
        controllerTextCustomColor = nil
        controllerFocusedTextCustomColor = nil
        controllerTextShadowCustomColor = nil
        controllerFocusedTextShadowCustomColor = nil
        controllerNavigationAccentPalette = configuration.accentPalette
        controllerFocusBoxStyle = configuration.focusBoxStyle
        controllerFocusBoxPalette = configuration.focusBoxPalette
        controllerOrbPalette = configuration.orbPalette
        controllerTextPalette = configuration.textPalette
        controllerSecondaryTextPalette = configuration.secondaryTextPalette
        controllerCriticalTextPalette = configuration.criticalTextPalette
        // Presets own semantic role colours through their text/accent
        // configuration. Clear per-role manual overrides so every preset is
        // internally coherent while still allowing later customization.
        controllerTabTitlePalette = nil
        controllerTabSubtitlePalette = nil
        controllerBottomTabBarPalette = nil
        controllerBottomTabBarUnselectedPalette = nil
        controllerCardTitlePalette = nil
        controllerContextMenuPalette = nil
        controllerImportActionPalette = nil
        controllerToolbarPalette = nil
        controllerFocusedTextPalette = configuration.focusedTextPalette
        controllerTextShadowPalette = configuration.textShadowPalette
        controllerFocusedTextShadowPalette =
            configuration.focusedTextShadowPalette
        controllerTextShadowStrength = configuration.textShadowStrength
        controllerFocusedTextShadowStrength =
            configuration.focusedTextShadowStrength
        controllerFocusBoxAnimationSpeed = configuration.animationSpeed
        controllerFocusBoxGlowIntensity = configuration.glowIntensity
        // Built-in themes start unobscured. User-saved Custom/Gallery themes
        // still restore their own complete snapshot, including Background Dim.
        backgroundDim = 0
        // Theme presets configure orb appearance, not orb visibility. Keeping
        // this user-owned toggle untouched ensures orbs only turn on manually.

        if let dynamicConfiguration = preset.dynamicConfiguration {
            var preferences = dynamicAppearancePreferences
            preferences.dynamicBackground = dynamicConfiguration.background
            preferences.sharedPalette = dynamicConfiguration.sharedPalette
            preferences.sharedCustomColor = dynamicConfiguration.sharedCustomColor
            preferences.sharedMultiColor = ThemeMultiColorSelection(
                isEnabled: !dynamicConfiguration.sharedMultiColorPalettes.isEmpty,
                animates: dynamicConfiguration.animatesMultiColor,
                palettes: dynamicConfiguration.sharedMultiColorPalettes.isEmpty
                    ? [dynamicConfiguration.sharedPalette]
                    : dynamicConfiguration.sharedMultiColorPalettes
            )
            preferences.ribbonPalette = dynamicConfiguration.ribbonPalette
            preferences.ribbonCustomColor = dynamicConfiguration.ribbonCustomColor
            preferences.ribbonMultiColor = ThemeMultiColorSelection(
                isEnabled: !dynamicConfiguration.ribbonMultiColorPalettes.isEmpty,
                animates: dynamicConfiguration.animatesMultiColor,
                palettes: dynamicConfiguration.ribbonMultiColorPalettes.isEmpty
                    ? [dynamicConfiguration.ribbonPalette]
                    : dynamicConfiguration.ribbonMultiColorPalettes
            )
            preferences.particleSettings = dynamicConfiguration.particleSettings
            preferences.hasSelectedPlayStation3XMBByMart =
                dynamicConfiguration.background == .playStation3XMBByMart
            preferences.isPlayStation3XMBPresetExplicit =
                dynamicConfiguration.background == .playStation3XMBByMart
            // Publish the complete theme before making its renderer visible.
            // Default -> themed transitions otherwise mount the previous
            // background first and replace it again during the same input edge.
            dynamicAppearancePreferences = preferences
            dynamicBackgroundsEnabled = true
        }
        controllerUIThemePreset = preset
    }
}
