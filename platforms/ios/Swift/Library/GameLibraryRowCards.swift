// GameLibraryRowCards.swift — the library's touch and controller list rows
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Touch-first row retained from Master. Controller-only glass and padding
/// never enter this subtree when no gamepad is connected.
struct GameLibraryTouchRow: View {
    let game: ISOEntry
    let running: Bool
    let stopping: Bool
    let style: GameLibraryCardStyle
    let favoriteBounce: Int
    let glowAllowed: Bool
    let registry: GameplayLaunchCardRegistry
    let actions: GameLibraryControllerCoordinator

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCardSubtitleColour) private var secondaryTextColour

    var body: some View {
        return Button {
            actions.open(game)
        } label: {
            HStack(spacing: 12 * style.resolvedGameCardTextSpacingScale) {
                GameLibraryCoverThumbnail(
                    game: game,
                    style: style,
                    width: 58 * style.resolvedGameCardScale
                        * style.resolvedGameCardWidthScale,
                    height: 87 * style.resolvedGameCardScale
                        * style.resolvedGameCardHeightScale,
                    running: running,
                    stopping: stopping,
                    glowAllowed: glowAllowed
                )

                VStack(alignment: .leading, spacing: 4) {
                    if !style.hideGameName {
                        HStack(spacing: 6) {
                            Text(game.displayName)
                                .font(
                                    .system(
                                        size: 17 * style.resolvedGameNameTextScale,
                                        weight: .medium
                                    )
                                )
                                .foregroundStyle(contentTextColour)
                                .lineLimit(style.gameNameLineLimit)
                                .multilineTextAlignment(.leading)
                                .fixedSize(horizontal: false, vertical: true)
                                .layoutPriority(1)
                            if running && !style.hideRunningIndicator {
                                Image(systemName: "circle.fill")
                                    .font(.system(size: 8))
                                    .foregroundStyle(accentColour)
                                    .accessibilityLabel(
                                        style.runningLabel
                                    )
                                    .fixedSize()
                            }
                        }
                    }
                    if !style.hideGameInfo {
                        HStack(spacing: 8) {
                            Text(game.sizeLabel)
                            Text(game.name.pathExtensionLabel)
                            if game.isExternal {
                                Label(
                                    style.externalLabel,
                                    systemImage: "externaldrive"
                                )
                            }
                            if !style.hideRegionFlag, let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(
                                    style.regionLabel
                                )
                            }
                        }
                        .font(
                            .system(size: 12 * style.resolvedGameInfoTextScale)
                        )
                        .foregroundStyle(secondaryTextColour)
                        .lineLimit(1)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .layoutPriority(1)
                Spacer()

                if !style.hideFavoriteButton {
                    GameLibraryFavoriteButton(
                        game: game,
                        style: style,
                        favoriteBounce: favoriteBounce,
                        actions: actions,
                        foreground: game.isFavorite ? .yellow : .gray
                    )
                }

                Image(
                    systemName: running && !style.hideRunningIndicator
                        ? "play.fill"
                        : "chevron.right"
                )
                    .foregroundStyle(
                        running && !style.hideRunningIndicator
                            ? accentColour
                            : .secondary
                    )
                    .font(.caption)
                    .accessibilityHidden(true)
            }
        }
        .buttonStyle(.plain)
        .foregroundStyle(.primary)
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: registry
        )
    }
}

struct GameLibraryControllerRow: View {
    let game: ISOEntry
    let running: Bool
    let stopping: Bool
    let style: GameLibraryCardStyle
    let favoriteBounce: Int
    let glowAllowed: Bool
    let registry: GameplayLaunchCardRegistry
    let actions: GameLibraryControllerCoordinator
    let controllerFocused: Bool
    let visualEffectsEnabled: Bool

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCardSubtitleColour) private var secondaryTextColour

    var body: some View {
        return Button {
            actions.open(game)
        } label: {
            HStack(spacing: 12 * style.resolvedGameCardTextSpacingScale) {
                GameLibraryCoverThumbnail(
                    game: game,
                    style: style,
                    width: 58 * style.resolvedGameCardScale
                        * style.resolvedGameCardWidthScale,
                    height: 87 * style.resolvedGameCardScale
                        * style.resolvedGameCardHeightScale,
                    running: running,
                    stopping: stopping,
                    glowAllowed: glowAllowed,
                    visualEffectsEnabled: visualEffectsEnabled
                )
                .controllerNavigationOrbTarget(
                    id: "library.game.\(game.id)",
                    isActive: controllerFocused
                        && glowAllowed,
                    isEnabled: true,
                    palette: .blue,
                    inset: -3,
                    orbScale: 0.92,
                    priority: 30
                )

                VStack(alignment: .leading, spacing: 4) {
                    if !style.hideGameName {
                        HStack(spacing: 6) {
                            Text(game.displayName)
                                .font(
                                    .system(
                                        size: 17 * style.resolvedGameNameTextScale,
                                        weight: .medium
                                    )
                                )
                                .foregroundStyle(
                                    controllerFocused
                                        ? style.cardFocusedColor
                                        : contentTextColour
                                )
                                .animation(
                                    ControllerFocusVisualAnimation.textFade,
                                    value: controllerFocused
                                )
                                .lineLimit(style.gameNameLineLimit)
                                .multilineTextAlignment(.leading)
                                .fixedSize(horizontal: false, vertical: true)
                                .layoutPriority(1)
                            if running && !style.hideRunningIndicator {
                                Image(systemName: "circle.fill")
                                    .font(.system(size: 8))
                                    .foregroundStyle(accentColour)
                                    .accessibilityLabel(style.runningLabel)
                                .fixedSize()
                            }
                        }
                    }
                    if !style.hideGameInfo {
                        HStack(spacing: 8) {
                            Text(game.sizeLabel)
                            Text(game.name.pathExtensionLabel)
                            if game.isExternal {
                                Label(style.externalLabel, systemImage: "externaldrive")
                            }
                            if !style.hideRegionFlag, let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(style.regionLabel)
                            }
                        }
                        .font(
                            .system(size: 12 * style.resolvedGameInfoTextScale)
                        )
                        .foregroundStyle(secondaryTextColour)
                        .lineLimit(1)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .layoutPriority(1)
                Spacer()

                if !style.hideFavoriteButton {
                    GameLibraryFavoriteButton(
                        game: game,
                        style: style,
                        favoriteBounce: favoriteBounce,
                        actions: actions,
                        foreground: game.isFavorite
                            ? .yellow
                            : (controllerFocused ? accentColour : .gray)
                    )
                }

                Image(
                    systemName: running && !style.hideRunningIndicator
                        ? "play.fill"
                        : "chevron.right"
                )
                    .foregroundStyle(
                        running && !style.hideRunningIndicator
                            ? accentColour
                            : (controllerFocused ? accentColour : Color.secondary)
                    )
                    .font(.caption)
                    .accessibilityHidden(true)
            }
            .padding(
                .horizontal,
                10 * style.resolvedGameCardContentPaddingScale
            )
        }
        .buttonStyle(.plain)
        .foregroundStyle(.primary)
        .accessibilityLabel(game.displayName)
        .observeGameLibraryCardLongPress(
            onTouchBegan: actions.suppressThemeShortcutForGameCardTouch
        ) {
            actions.handleGameContextMenuLongPress(for: game)
        }
        .registerGameplayLaunchCard(
            for: game.id,
            in: registry,
            isEnabled: visualEffectsEnabled
        )
    }
}
