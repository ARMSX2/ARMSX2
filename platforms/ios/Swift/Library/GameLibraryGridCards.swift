// GameLibraryGridCards.swift — the library's grid cards, favorite star and cover thumbnail
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Master's original touch-first grid card.
struct GameLibraryTouchGridCard: View {
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
        let horizontalPadding = 12 * style.resolvedGameCardContentPaddingScale
        let verticalPadding = 12 * style.resolvedGameCardVerticalPaddingScale
        return Button {
            actions.open(game)
        } label: {
            ZStack(alignment: .topTrailing) {
                VStack(
                    alignment: .center,
                    spacing: 10 * style.resolvedGameCardTextSpacingScale
                ) {
                    GameLibraryCoverThumbnail(
                        game: game,
                        style: style,
                        width: 126 * style.resolvedGameCardScale
                            * style.resolvedGameCardWidthScale,
                        height: 189 * style.resolvedGameCardScale
                            * style.resolvedGameCardHeightScale,
                        running: running,
                        stopping: stopping,
                        glowAllowed: glowAllowed
                    )
                    .overlay(alignment: .topTrailing) {
                        if !style.hideFavoriteButton {
                            GameLibraryFavoriteButton(
                                game: game,
                                style: style,
                                favoriteBounce: favoriteBounce,
                                actions: actions,
                                font: .callout.weight(.semibold),
                                foreground: game.isFavorite
                                    ? .yellow
                                    : .white.opacity(0.86),
                                backgroundOpacity: 0.36,
                                padding: 6
                            )
                            .zIndex(2)
                        }
                    }
                    .frame(maxWidth: .infinity)

                    if !style.hideGameName || !style.hideGameInfo {
                        VStack(
                            alignment: .center,
                            spacing: 4 * style.resolvedGameCardTextSpacingScale
                        ) {
                            if !style.hideGameName {
                                HStack(alignment: .firstTextBaseline, spacing: 5) {
                                    Text(game.displayName)
                                        .font(
                                            .system(
                                                size: 15 * style.resolvedGameNameTextScale,
                                                weight: .semibold
                                            )
                                        )
                                        .foregroundStyle(contentTextColour)
                                        .lineLimit(style.gameNameLineLimit)
                                        .multilineTextAlignment(.center)
                                        .fixedSize(
                                            horizontal: false,
                                            vertical: true
                                        )
                                        .frame(
                                            maxWidth: .infinity,
                                            alignment: .center
                                        )
                                        .layoutPriority(1)
                                    if running && !style.hideRunningIndicator {
                                        Image(systemName: "circle.fill")
                                            .font(.system(size: 7))
                                            .foregroundStyle(accentColour)
                                            .accessibilityLabel(
                                                style.runningLabel
                                            )
                                            .fixedSize()
                                    }
                                }
                                .frame(
                                    minHeight: (style.singleLineGameNames ? 21 : 38)
                                        * style.resolvedGameNameTextScale,
                                    alignment: .top
                                )
                            }
                            if !style.hideGameInfo {
                                HStack(spacing: 6) {
                                    Text(game.name.pathExtensionLabel)
                                    Text(game.sizeLabel)
                                    if game.isExternal {
                                        Image(systemName: "externaldrive")
                                            .accessibilityLabel(
                                                style.externalLabel
                                            )
                                    }
                                    if !style.hideRegionFlag,
                                       let flag = game.regionFlag {
                                        Text(flag).accessibilityLabel(
                                            style.regionLabel
                                        )
                                    }
                                }
                                .font(
                                    .system(
                                        size: 11 * style.resolvedGameInfoTextScale
                                    )
                                )
                                .foregroundStyle(secondaryTextColour)
                                .frame(maxWidth: .infinity, alignment: .center)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .center)
                    }
                }
                .padding(.horizontal, horizontalPadding)
                .padding(.vertical, verticalPadding)
                .frame(
                    maxWidth: .infinity,
                    minHeight: style.gameGridCardHeight(controllerOptimized: false),
                    alignment: .top
                )
            }
            .glassSurface(
                clear: true,
                cornerRadius: style.resolvedGameCardCornerRadius
            )
        }
        .buttonStyle(.plain)
        .modifier(
            GameLibraryControllerFocusModifier(
                isFocused: false,
                cornerRadius: style.resolvedGameCardCornerRadius,
                isFavorite: game.isFavorite,
                favoriteGlowEnabled:
                    style.favoriteGlowEnabled,
                isEnabled: game.isFavorite
                    && style.favoriteGlowEnabled,
                performanceOptimized: true,
                focusedScale: 1
            )
        )
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: registry
        )
    }
}

struct GameLibraryControllerGridCard: View {
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
        let horizontalPadding = 12 * style.resolvedGameCardContentPaddingScale
        let verticalPadding = 12 * style.resolvedGameCardVerticalPaddingScale
        let cardHeight = style.gameGridCardHeight(controllerOptimized: true)
        return Button {
            actions.open(game)
        } label: {
            ZStack(alignment: .top) {
                GeometryReader { proxy in
                    VStack(
                        alignment: .center,
                        spacing: 4 * style.resolvedGameCardTextSpacingScale
                    ) {
                        VStack(
                            alignment: .center,
                            spacing: 10 * style.resolvedGameCardTextSpacingScale
                        ) {
                            GameLibraryCoverThumbnail(
                                game: game,
                                style: style,
                                width: 126 * style.resolvedGameCardScale
                                    * style.resolvedGameCardWidthScale,
                                height: 189 * style.resolvedGameCardScale
                                    * style.resolvedGameCardHeightScale,
                                running: running,
                                stopping: stopping,
                                glowAllowed: glowAllowed,
                                visualEffectsEnabled: visualEffectsEnabled
                            )
                            .overlay(alignment: .topTrailing) {
                                if !style.hideFavoriteButton {
                                    GameLibraryFavoriteButton(
                                        game: game,
                                        style: style,
                                        favoriteBounce: favoriteBounce,
                                        actions: actions,
                                        font: .callout.weight(.semibold),
                                        foreground: game.isFavorite
                                            ? .yellow
                                            : (controllerFocused
                                                ? accentColour
                                                : .white.opacity(0.86)),
                                        backgroundOpacity: 0.36,
                                        padding: 6
                                    )
                                    .zIndex(2)
                                }
                            }
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

                            if !style.hideGameName {
                            Text(game.displayName)
                                    .font(
                                        .system(
                                            size: 15 * style.resolvedGameNameTextScale,
                                            weight: .semibold
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
                                    .multilineTextAlignment(.center)
                                    .fixedSize(horizontal: false, vertical: true)
                                    .frame(
                                        maxWidth: .infinity,
                                        minHeight: (style.singleLineGameNames ? 21 : 38)
                                            * style.resolvedGameNameTextScale,
                                        alignment: .top
                                    )
                                    .overlay(alignment: .trailing) {
                                        if running && !style.hideRunningIndicator {
                                            Image(systemName: "circle.fill")
                                                .font(.system(size: 7))
                                                .foregroundStyle(accentColour)
                                                .accessibilityLabel(style.runningLabel)
                                        }
                                    }
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .center)

                        if !style.hideGameInfo {
                            HStack(spacing: 6) {
                                Text(game.name.pathExtensionLabel)
                                Text(game.sizeLabel)
                                if game.isExternal {
                                    Image(systemName: "externaldrive")
                                        .accessibilityLabel(style.externalLabel)
                                }
                                if !style.hideRegionFlag, let flag = game.regionFlag {
                                    Text(flag).accessibilityLabel(style.regionLabel)
                                }
                            }
                            .font(
                                .system(size: 11 * style.resolvedGameInfoTextScale)
                            )
                            .foregroundStyle(secondaryTextColour)
                            .frame(maxWidth: .infinity, alignment: .center)
                        }
                    }
                    .frame(
                        width: max(
                            0,
                            proxy.size.width - horizontalPadding * 2
                        )
                    )
                    .padding(.horizontal, horizontalPadding)
                    .padding(.vertical, verticalPadding)
                    .scaleEffect(
                        controllerFocused ? 1.10 : 1,
                        anchor: .center
                    )
                    .position(
                        x: proxy.size.width / 2
                            + (controllerFocused ? 12 : 0),
                        y: proxy.size.height / 2
                            + (controllerFocused ? 12 : 0)
                    )
                    .animation(
                        .smooth(duration: 0.16, extraBounce: 0.02),
                        value: controllerFocused
                    )
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: cardHeight)
            .glassSurface(
                interactive: true,
                clear: true,
                forceClear: controllerFocused,
                isEnabled: visualEffectsEnabled,
                cornerRadius: style.resolvedGameCardCornerRadius
            )
        }
        .buttonStyle(.plain)
        .frame(maxWidth: .infinity, alignment: .center)
        .modifier(
            GameLibraryControllerFocusModifier(
                isFocused: controllerFocused,
                cornerRadius: 18,
                isFavorite: game.isFavorite,
                favoriteGlowEnabled:
                    style.favoriteGlowEnabled,
                isEnabled: true,
                backgroundEffectsEnabled: visualEffectsEnabled
            )
        )
        .observeGameLibraryCardLongPress(
            onTouchBegan: actions.suppressThemeShortcutForGameCardTouch
        ) {
            actions.handleGameContextMenuLongPress(for: game)
        }
        .accessibilityLabel(game.displayName)
        .registerGameplayLaunchCard(
            for: game.id,
            in: registry,
            isEnabled: visualEffectsEnabled
        )
    }
}

extension GameLibraryCardStyle {
    func gameGridCardHeight(
        controllerOptimized: Bool
    ) -> CGFloat {
        _ = controllerOptimized
        let coverHeight = 189 * resolvedGameCardScale
            * resolvedGameCardHeightScale
        let nameHeight = hideGameName
            ? 0
            : (singleLineGameNames ? 21 : 38) * resolvedGameNameTextScale
        let infoHeight = hideGameInfo
            ? 0
            : 13 * resolvedGameInfoTextScale
        let coverToNameSpacing = hideGameName
            ? 0
            : 10 * resolvedGameCardTextSpacingScale
        let contentSpacing = hideGameName || hideGameInfo
            ? 0
            : 4 * resolvedGameCardTextSpacingScale
        let verticalPadding = 24 * resolvedGameCardVerticalPaddingScale
        // Size the glass from the content it actually contains. The old fixed
        // 280-point frame left an unadjustable top/bottom area, especially
        // when titles or metadata were hidden.
        return max(
            96,
            coverHeight
                + nameHeight
                + infoHeight
                + coverToNameSpacing
                + contentSpacing
                + verticalPadding
        )
    }
}

struct GameLibraryFavoriteButton: View {
    let game: ISOEntry
    let style: GameLibraryCardStyle
    let favoriteBounce: Int
    let actions: GameLibraryControllerCoordinator
    var font: Font = .body
    let foreground: Color
    var backgroundOpacity: Double? = nil
    var padding: CGFloat = 0

    var body: some View {
        let icon = Image(systemName: game.isFavorite ? "star.fill" : "star")
            .font(font)
            .foregroundStyle(foreground)
            .padding(padding)
            .background {
                if let backgroundOpacity {
                    Circle().fill(.black.opacity(backgroundOpacity))
                }
            }
            .frame(minWidth: 44, minHeight: 44)
            .contentShape(Rectangle())

        return Button {
            actions.toggleFavorite(game)
        } label: {
            if style.favoriteGlowEnabled {
                icon
                    .contentTransition(.symbolEffect)
                    .symbolEffect(
                        .bounce,
                        value: favoriteBounce
                    )
            } else {
                icon
            }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(
            game.isFavorite
                ? style.removeFromFavoritesLabel
                : style.addToFavoritesLabel
        )
    }
}

struct GameLibraryCoverThumbnail: View {
    let game: ISOEntry
    let style: GameLibraryCardStyle
    let width: CGFloat
    let height: CGFloat
    let running: Bool
    let stopping: Bool
    let glowAllowed: Bool
    var visualEffectsEnabled = true

    @Environment(\.menuTabIsActive) private var menuTabIsActive

    @ViewBuilder
    var body: some View {
        let runningGlow = running
            && !style.hideRunningIndicator
            && !stopping
        let favoriteGlow = game.isFavorite
            && style.favoriteGlowEnabled
        let themedGlow = favoriteGlow || runningGlow
        let usesAnimatedGlow = visualEffectsEnabled
            && themedGlow
            && menuTabIsActive
            && glowAllowed
        let thumbnail = CoverThumbnailView(
            gameName: game.name,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature,
            width: width,
            height: height,
            cornerRadius: style.resolvedGameCoverCornerRadius
        )
        .opacity(style.resolvedGameCoverOpacity)

        if usesAnimatedGlow {
            thumbnail.modifier(
                GameCoverThemeGlowModifier(
                    primaryColor: style.glowPrimaryColor,
                    secondaryColor: style.glowSecondaryColor
                )
            )
        } else {
            thumbnail.shadow(
                color: themedGlow
                    ? .clear
                    : .black.opacity(
                        0.28 * style.resolvedGameCoverShadowStrength
                    ),
                radius: 18 * style.resolvedGameCoverShadowStrength,
                y: 10 * style.resolvedGameCoverShadowStrength
            )
        }
    }
}
