// GameLibraryCoverFlowCard.swift — the landscape cover flow card and its metrics
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// One explicitly framed landscape surface owns both touch and controller
/// presentation. The complete card zooms from its center while its layout
/// footprint remains stable inside the Cover Flow rail.
struct GameLibraryCoverFlowCard: View {
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
    let metrics: GameLibraryCoverFlowMetrics
    let controllerInputActive: Bool

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCardSubtitleColour) private var secondaryTextColour

    var body: some View {
        let cardWidth = style.coverFlowCardWidth(metrics: metrics)
        let contentWidth = max(
            0,
            cardWidth - (metrics.cardHorizontalPadding * 2)
        )
        let titleHeight: CGFloat = style.hideGameName
            ? 0
            : (style.singleLineGameNames
                ? (metrics.isCompact ? 20 : 22)
                : (metrics.isCompact ? 38 : 46)
            ) * style.resolvedGameNameTextScale
        let metadataHeight: CGFloat = style.hideGameInfo
            ? 0
            : (metrics.isCompact ? 14 : 17) * style.resolvedGameInfoTextScale
        let coverToTextSpacing: CGFloat = style.hideGameName && style.hideGameInfo
            ? 0
            : metrics.cardSpacing
        let interTextSpacing: CGFloat = style.hideGameName || style.hideGameInfo
            ? 0
            : 4 * style.resolvedGameCardTextSpacingScale
        let cardHeight = metrics.cardVerticalPadding * 2
            + metrics.coverHeight
            + coverToTextSpacing
            + titleHeight
            + interTextSpacing
            + metadataHeight

        return Button {
            actions.open(game)
        } label: {
            ZStack(alignment: .top) {
                VStack(spacing: interTextSpacing) {
                    VStack(spacing: coverToTextSpacing) {
                        GameLibraryCoverThumbnail(
                            game: game,
                            style: style,
                            width: metrics.coverWidth,
                            height: metrics.coverHeight,
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
                                    font: (metrics.isCompact
                                        ? Font.subheadline
                                        : Font.headline).weight(.semibold),
                                    foreground: game.isFavorite
                                        ? .yellow
                                        : (controllerFocused
                                            ? accentColour
                                            : .white.opacity(0.88)),
                                    backgroundOpacity: 0.48,
                                    padding: metrics.favoritePadding
                                )
                                .padding(metrics.favoriteInset)
                                .zIndex(2)
                            }
                        }
                        .controllerNavigationOrbTarget(
                            id: "library.game.\(game.id)",
                            isActive: controllerFocused
                                && glowAllowed,
                            isEnabled: controllerInputActive,
                            palette: .blue,
                            inset: -4,
                            orbScale: 0.96,
                            priority: 30
                        )

                        if !style.hideGameName {
                            Text(game.displayName)
                                .font(
                                    .system(
                                        size: (metrics.isCompact ? 15 : 17)
                                            * style.resolvedGameNameTextScale,
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
                                .multilineTextAlignment(.center)
                                .lineLimit(style.gameNameLineLimit)
                                .fixedSize(horizontal: false, vertical: true)
                                .frame(
                                    width: metrics.textWidth,
                                    alignment: .center
                                )
                                .frame(
                                    minHeight: titleHeight,
                                    alignment: .top
                                )
                        }
                    }
                    .frame(width: contentWidth, alignment: .center)

                    if !style.hideGameInfo {
                        HStack(spacing: 4) {
                            Text(
                                game.isExternal
                                    ? "\(game.name.pathExtensionLabel)  \(game.sizeLabel)  \(style.externalLabel)"
                                    : "\(game.name.pathExtensionLabel)  \(game.sizeLabel)"
                            )
                            if !style.hideRegionFlag,
                               let flag = game.regionFlag {
                                Text(flag).accessibilityLabel(
                                    style.regionLabel
                                )
                            }
                        }
                        .font(
                            .system(
                                size: (metrics.isCompact ? 11 : 12)
                                    * style.resolvedGameInfoTextScale
                            )
                        )
                        .foregroundStyle(secondaryTextColour)
                        .frame(width: metrics.textWidth)
                    }
                }
                .padding(.vertical, metrics.cardVerticalPadding)
                .frame(width: contentWidth)
                .frame(
                    width: cardWidth,
                    height: cardHeight,
                    alignment: .center
                )
                .scaleEffect(
                    controllerFocused ? style.resolvedFocusedGameCardScale : 1,
                    anchor: .center
                )
                .offset(
                    x: controllerFocused ? 8 : 0,
                    y: controllerFocused ? 12 : 0
                )

            }
            .frame(width: cardWidth, height: cardHeight)
            .glassSurface(
                interactive: controllerInputActive,
                clear: true,
                forceClear: controllerFocused,
                isEnabled: visualEffectsEnabled,
                cornerRadius: metrics.cornerRadius
            )
        }
        .buttonStyle(.plain)
        .modifier(
            GameLibraryControllerFocusModifier(
                isFocused: controllerFocused,
                cornerRadius: metrics.cornerRadius,
                isFavorite: game.isFavorite,
                favoriteGlowEnabled:
                    style.favoriteGlowEnabled,
                isEnabled: controllerInputActive
                    || (game.isFavorite
                        && style.favoriteGlowEnabled),
                backgroundEffectsEnabled: visualEffectsEnabled,
                performanceOptimized: true,
                focusedScale: 1,
                optimizedAnimationDuration: 0.075
            )
        )
        .scaleEffect(
            controllerFocused ? style.resolvedFocusedGameCardScale : 1,
            anchor: .center
        )
        .offset(
            y: controllerFocused ? -style.resolvedFocusedGameCardLift : 0
        )
        .animation(
            .smooth(duration: 0.075, extraBounce: 0),
            value: controllerFocused
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
    func coverFlowCardWidth(metrics: GameLibraryCoverFlowMetrics) -> CGFloat {
        let textIsVisible = !hideGameName || !hideGameInfo
        let contentWidth = textIsVisible
            ? max(metrics.coverWidth, metrics.textWidth)
            : metrics.coverWidth
        return contentWidth + (metrics.cardHorizontalPadding * 2)
    }
}

struct GameLibraryCoverFlowMetrics {
    let isCompact: Bool
    let coverWidth: CGFloat
    let coverHeight: CGFloat
    let textWidth: CGFloat
    let cardSpacing: CGFloat
    let cardHorizontalPadding: CGFloat
    let cardVerticalPadding: CGFloat
    let cornerRadius: CGFloat
    let favoritePadding: CGFloat
    let favoriteInset: CGFloat
    let itemSpacing: CGFloat
    let horizontalPadding: CGFloat
    let verticalPadding: CGFloat
    let statusWidth: CGFloat
    let statusHeight: CGFloat
    let statusIconSize: CGFloat

    init(
        containerSize: CGSize,
        controllerOptimized: Bool = false,
        cardScale: CGFloat = 1,
        widthScale: CGFloat = 1,
        heightScale: CGFloat = 1,
        gapScale: CGFloat = 1,
        contentPaddingScale: CGFloat = 1,
        verticalPaddingScale: CGFloat = 1,
        textSpacingScale: CGFloat = 1,
        cardCornerRadius: CGFloat? = nil
    ) {
        isCompact = containerSize.height < 360
        let safeCardScale = min(max(cardScale, 0.72), 1.32)
        let safeWidthScale = min(max(widthScale, 0.70), 1.40)
        let safeHeightScale = min(max(heightScale, 0.70), 1.40)
        let safeGapScale = min(max(gapScale, 0.65), 2)
        let safeContentPaddingScale = min(max(contentPaddingScale, 0), 2)
        let safeVerticalPaddingScale = min(max(verticalPaddingScale, 0), 2)
        let safeTextSpacingScale = min(max(textSpacingScale, 0), 2)
        let baseCoverHeight: CGFloat = controllerOptimized
            ? (isCompact ? 198 : 285)
            : (isCompact ? 156 : 225)
        coverHeight = baseCoverHeight * safeCardScale * safeHeightScale
        // Width and height are independent outer-card metrics. Dividing
        // out the height multiplier keeps Width from being changed twice.
        coverWidth = coverHeight * (2.0 / 3.0)
            * (safeWidthScale / safeHeightScale)
        let minimumTextWidth: CGFloat = controllerOptimized
            ? (isCompact ? 164 : 210)
            : (isCompact ? 134 : 164)
        textWidth = max(
            minimumTextWidth * safeCardScale * safeWidthScale,
            coverWidth + (28 * safeCardScale * safeWidthScale)
        )
        cardSpacing = (isCompact ? 8 : 12) * safeCardScale
            * safeTextSpacingScale
        cardHorizontalPadding = (isCompact ? 8 : 12) * safeCardScale
            * safeContentPaddingScale
        cardVerticalPadding = (isCompact ? 8 : 12) * safeCardScale
            * safeVerticalPaddingScale
        cornerRadius = cardCornerRadius
            ?? ((isCompact ? 18 : 24) * safeCardScale)
        favoritePadding = isCompact ? 6 : 8
        favoriteInset = isCompact ? 5 : 8
        itemSpacing = (controllerOptimized
            ? (isCompact ? 24 : 30)
            : (isCompact ? 14 : 20)) * safeGapScale
        horizontalPadding = isCompact ? 20 : 32
        verticalPadding = isCompact ? 10 : 18
        statusWidth = isCompact ? 138 : 166
        statusHeight = isCompact ? 190 : 276
        statusIconSize = isCompact ? 36 : 48
    }
}
