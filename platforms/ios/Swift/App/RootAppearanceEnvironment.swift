// RootAppearanceEnvironment.swift — theme colours RootView hands down
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct RootAppearanceEnvironment: ViewModifier {
    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared
    @State private var gameCoverThemePreview = GameCoverThemePreviewStore.shared

    private var coverPreviewAccentColor: Color {
        gameCoverThemePreview.preview?.accentColor
            ?? settings.appearance.controllerNavigationAccentColor
    }

    private var coverPreviewPrimaryTextColor: Color {
        gameCoverThemePreview.preview?.primaryTextColor
            ?? (settings.appearance.controllerTextAppearance.contentColor ?? .primary)
    }

    private var coverPreviewSecondaryTextColor: Color {
        gameCoverThemePreview.preview?.secondaryTextColor
            ?? settings.appearance.controllerTextAppearance.unselectedColor
    }

    /// The cover analyzer may choose dark text for a bright dynamic background, but every
    /// Context Menu is presented on dark regular glass. Keep that panel readable while still
    /// using the cover-derived accent for its focus and symbols.
    private var coverPreviewContextMenuPrimaryTextColor: Color {
        gameCoverThemePreview.preview == nil
            ? settings.appearance.controllerContextMenuColor
            : .white
    }

    private var coverPreviewContextMenuSecondaryTextColor: Color {
        gameCoverThemePreview.preview == nil
            ? settings.appearance.controllerContextMenuSecondaryColor
            : Color(red: 0.86, green: 0.91, blue: 0.96)
    }

    private var coverPreviewTextAppearance: ControllerTextAppearance {
        guard let preview = gameCoverThemePreview.preview else {
            return settings.appearance.controllerTextAppearance
        }
        var appearance = settings.appearance.controllerTextAppearance
        appearance.normalColor = preview.primaryTextColor
        appearance.secondaryColor = preview.secondaryTextColor
        appearance.titleColor = preview.primaryTextColor
        appearance.contentColor = preview.primaryTextColor
        appearance.unselectedColor = preview.secondaryTextColor
        appearance.focusedColor = preview.accentColor
        return appearance
    }

    func body(content: Content) -> some View {
        content
            .uiSecondaryForegroundStyle(
                gameCoverThemePreview.preview == nil
                    ? settings.appearance.controllerTextAppearance.contentColor
                    : coverPreviewPrimaryTextColor
            )
            .tint(coverPreviewAccentColor)
            .environment(
                \.uiAccentColour,
                coverPreviewAccentColor
            )
            .environment(
                \.controllerTextAppearance,
                coverPreviewTextAppearance
            )
            .environment(
                \.uiSecondaryTextColour,
                coverPreviewSecondaryTextColor
            )
            .environment(
                \.uiTitleTextColour,
                gameCoverThemePreview.preview?.primaryTextColor
                    ?? settings.appearance.controllerTextAppearance.titleColor
            )
            .environment(
                \.uiContentTextColour,
                coverPreviewPrimaryTextColor
            )
            .environment(
                \.uiTabTitleColour,
                gameCoverThemePreview.preview?.primaryTextColor
                    ?? settings.appearance.controllerTabTitleColor
            )
            .environment(
                \.uiTabSubtitleColour,
                gameCoverThemePreview.preview?.secondaryTextColor
                    ?? settings.appearance.controllerTabSubtitleColor
            )
            .environment(
                \.uiBottomTabBarColour,
                coverPreviewAccentColor
            )
            .environment(
                \.uiBottomTabBarUnselectedColour,
                gameCoverThemePreview.preview?.secondaryTextColor
                    ?? settings.appearance.controllerBottomTabBarUnselectedColor
            )
            .environment(
                \.uiCardTitleColour,
                gameCoverThemePreview.preview?.primaryTextColor
                    ?? settings.appearance.controllerCardTitleColor
            )
            .environment(
                \.uiCardSubtitleColour,
                gameCoverThemePreview.preview?.secondaryTextColor
                    ?? settings.appearance.controllerCardSubtitleColor
            )
            .environment(
                \.uiContextMenuColour,
                coverPreviewContextMenuPrimaryTextColor
            )
            .environment(
                \.uiContextMenuSecondaryColour,
                coverPreviewContextMenuSecondaryTextColor
            )
            .environment(
                \.uiContextMenuFocusedColour,
                coverPreviewAccentColor
            )
            .environment(
                \.uiImportActionColour,
                coverPreviewAccentColor
            )
            .environment(
                \.uiToolbarColour,
                coverPreviewAccentColor
            )
            .environment(
                \.uiCriticalTextColour,
                settings.appearance.controllerCriticalTextColor
            )
            .environment(
                \.uiFrameRateConfiguration,
                frameRates.configuration
            )
            .environment(
                \.controllerNavigationDepthEffectEnabled,
                settings.appearance.controllerNavigationDepthEffectEnabled
            )
            // Master uses white label copy with blue/action-coloured symbols.
            // Install that split once so Settings, Per-Game Settings, Quick
            // Menu, context menus, and standard prompts stay consistent.
            .labelStyle(UIAccentIconLabelStyle())
    }
}
