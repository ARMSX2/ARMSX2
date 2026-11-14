// GameLibraryCardStyle.swift — the values every library card draws with, and the cover glow
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Built once per library body and handed to every card as one value, so a
/// card body runs again only when one of these values or its own game changes.
struct GameLibraryCardStyle: Equatable {
    let resolvedGameCardScale: CGFloat
    let resolvedGameCardWidthScale: CGFloat
    let resolvedGameCardHeightScale: CGFloat
    let resolvedGameCardGapScale: CGFloat
    let resolvedGameCardContentPaddingScale: CGFloat
    let resolvedGameCardVerticalPaddingScale: CGFloat
    let resolvedGameCardTextSpacingScale: CGFloat
    let resolvedGameCardCornerRadius: CGFloat
    let resolvedGameLibraryVerticalPosition: CGFloat
    let resolvedGameNameTextScale: CGFloat
    let resolvedGameInfoTextScale: CGFloat
    let resolvedGameCoverCornerRadius: CGFloat
    let resolvedGameCoverShadowStrength: CGFloat
    let resolvedGameCoverOpacity: Double
    let resolvedFocusedGameCardScale: CGFloat
    let resolvedFocusedGameCardLift: CGFloat
    let hideGameName: Bool
    let hideGameInfo: Bool
    let hideFavoriteButton: Bool
    let hideRegionFlag: Bool
    let hideRunningIndicator: Bool
    let singleLineGameNames: Bool
    let runningLabel: String
    let externalLabel: String
    let regionLabel: String
    let addToFavoritesLabel: String
    let removeFromFavoritesLabel: String
    let cardFocusedColor: Color
    let favoriteGlowEnabled: Bool
    let glowPrimaryColor: Color
    let glowSecondaryColor: Color

    var gameNameLineLimit: Int {
        singleLineGameNames ? 1 : 2
    }
}

private struct RunningCoverGlowPhase {
    let radius: CGFloat
    let x: CGFloat
    let y: CGFloat
    let opacity: Double

    static let initial = RunningCoverGlowPhase(
        radius: 9,
        x: -0.5,
        y: 0.5,
        opacity: 0.35
    )

    static func random() -> RunningCoverGlowPhase {
        RunningCoverGlowPhase(
            radius: .random(in: 7.5...12.5),
            x: .random(in: -2.25...2.25),
            y: .random(in: -2.25...2.25),
            opacity: .random(in: 0.20...0.45)
        )
    }
}

/// Instantiated only for a cover that is actively glowing. Ordinary cards do
/// not own animation state, shadow passes, or background tasks.
struct GameCoverThemeGlowModifier: ViewModifier {
    let primaryColor: Color
    let secondaryColor: Color
    @State private var phase = RunningCoverGlowPhase.initial

    func body(content: Content) -> some View {
        content
            .shadow(
                color: primaryColor.opacity(phase.opacity),
                radius: phase.radius,
                x: phase.x,
                y: phase.y
            )
            .shadow(
                color: secondaryColor.opacity(phase.opacity * 0.76),
                radius: phase.radius * 0.68,
                x: -phase.x * 0.7,
                y: -phase.y * 0.7
            )
            .task {
                while !Task.isCancelled {
                    let duration = Double.random(in: 0.8...1.35)
                    withAnimation(.easeInOut(duration: duration)) {
                        phase = .random()
                    }
                    try? await Task.sleep(
                        for: .milliseconds(Int(duration * 1_000))
                    )
                }
            }
    }
}
