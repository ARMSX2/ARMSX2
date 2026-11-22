// GameplayControllerShortcutHelpOverlay.swift — controller shortcut bar shown in game
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Non-interactive, one-shot startup guidance for the controller macros.
/// GameScreenView inserts this beneath its Quick Menu; RootView uses the same
/// view only when Emulation-Only Mode replaces the complete gameplay hierarchy.
struct GameplayControllerShortcutHelpOverlay: View {
    let settings: SettingsStore
    let controllerInput: MenuControllerInputRouter?
    /// Clear glass reads as part of the game; under the pause card it needs the card's frost.
    var overPauseMenu = false
    /// Just the tiles, for a footer row inside the pause card.
    var inCard = false

    static let height: CGFloat = 52
    private static let bottomPadding: CGFloat = 6
    /// What the pause card keeps free above the safe area: the bar and a gap.
    static let pauseCardReserve = bottomPadding + height + 11

    /// The bar sits this far into the home indicator strip, which holds nothing it could cover.
    static func homeIndicatorSink(_ bottomInset: CGFloat) -> CGFloat { min(14, bottomInset) }

    @ViewBuilder
    var body: some View {
        if controllerInput?.hasConnectedController == true,
           ControllerButtonGlyph.connectedGamepad != nil {
            Group {
                if inCard {
                    tiles.frame(height: Self.height)
                        .accessibilityElement(children: .combine)
                        .accessibilityLabel(spokenHints)
                } else {
                    shortcutContent
                }
            }
            .allowsHitTesting(false)
        }
    }

    private var spokenHints: String {
        [
            "Press \(settings.controller.controllerMacroQuickMenu.title) to open the Quick Menu.",
            "Press \(settings.controller.controllerMacroIncreaseSpeed.title) to increase emulation speed.",
            "Press \(settings.controller.controllerMacroSaveGameState.title) to save a game state.",
            "Press \(settings.controller.controllerMacroLoadGameState.title) to load a game state.",
        ].joined(separator: " ")
    }

    private var tiles: some View {
        HStack(spacing: 0) {
            shortcutTile(
                title: settings.localized("Menu")
            ) {
                macroBadge(settings.controller.controllerMacroQuickMenu)
            }

            shortcutTile(
                title: settings.localized("Speed")
            ) {
                macroBadge(settings.controller.controllerMacroIncreaseSpeed)
            }

            shortcutTile(
                title: settings.localized("State")
            ) {
                macroBadge(settings.controller.controllerMacroSaveGameState)
            }
        }
        .padding(4)
    }

    private var shortcutContent: some View {
        GeometryReader { geometry in
            let accent = themedAccentColor
            // In book pose the bar fits inside the leading pane.
            let paneWidth = geometry.activeFold(book: true).map { $0.minX - 32 }
            tiles
                .frame(height: Self.height)
                .frame(
                    maxWidth: min(
                        420,
                        paneWidth ?? .infinity,
                        max(
                            0,
                            geometry.size.width
                                - geometry.safeAreaInsets.leading
                                - geometry.safeAreaInsets.trailing
                                - 20
                        )
                    )
                )
                .glassSurface(
                    tint: accent.opacity(0.05),
                    clear: !overPauseMenu,
                    forceClear: !overPauseMenu,
                    materializeTransition: true,
                    cornerRadius: 28
                )
                .overlay {
                    Capsule(style: .continuous)
                        .stroke(
                            Color.primary.opacity(0.13),
                            lineWidth: 0.65
                        )
                }
                .shadow(color: .black.opacity(0.12), radius: 12, y: 5)
                // On the bar itself, so VoiceOver frames the bar rather than the whole screen.
                .accessibilityElement(children: .combine)
                .accessibilityLabel(spokenHints)
                .padding(.bottom, geometry.safeAreaInsets.bottom - Self.homeIndicatorSink(geometry.safeAreaInsets.bottom) + Self.bottomPadding)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                .centredInLeadingPaneOnFold()
        }
    }

    private var themedAccentColor: Color {
        settings.appearance.controllerBottomTabBarColor
    }

    private var themedSecondaryTextColor: Color {
        settings.appearance.controllerBottomTabBarUnselectedColor
    }

    private func macroBadge(_ binding: ControllerMacroBinding) -> some View {
        HStack(spacing: 5) {
            glyph(binding.first)
            plusSymbol
            glyph(binding.second)
        }
        .lineLimit(1)
        .fixedSize(horizontal: true, vertical: false)
    }

    private func glyph(_ button: ControllerMacroButton) -> some View {
        ControllerButtonGlyph(
            button: button,
            tint: themedAccentColor,
            symbolFont: .system(size: 16, weight: .semibold),
            chipFont: .system(size: 10, weight: .semibold, design: .rounded)
        )
    }

    @ViewBuilder
    private func shortcutTile<Content: View>(
        title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(spacing: 2) {
            HStack(spacing: 5) {
                content()
            }
            .foregroundStyle(themedAccentColor)

            Text(title)
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(themedSecondaryTextColor)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .frame(maxWidth: .infinity)
    }

    private var plusSymbol: some View {
        Image(systemName: "plus")
            .font(.system(size: 7, weight: .black))
            .foregroundStyle(themedSecondaryTextColor.opacity(0.72))
    }
}
