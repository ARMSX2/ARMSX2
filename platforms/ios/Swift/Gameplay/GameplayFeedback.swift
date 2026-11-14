// GameplayFeedback.swift — the game screen's status and achievement banners
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor
@Observable
final class GameplayFeedback {
    // A status banner replaces the one showing. Achievement banners queue, so a burst of
    // unlocks plays one at a time.
    let statusBanner = TransientBannerController<String>(defaultDisplayDuration: GameplayFeedback.briefStatusDisplayDuration)
    let achievementsBanner = TransientBannerController<RetroAchievementsToast>(defaultDisplayDuration: GameplayFeedback.retroAchievementsToastDisplayDuration, queuesConcurrentPresentations: true)

    static let briefStatusDisplayDuration: TimeInterval = 2.2
    static let importantStatusDisplayDuration: TimeInterval = 6.0
    private static let retroAchievementsToastDisplayDuration: TimeInterval = 5.0

    func presentStatusMessage(
        _ message: String,
        displayDuration: TimeInterval = GameplayFeedback.briefStatusDisplayDuration
    ) {
        statusBanner.present(message, displayDuration: displayDuration)
    }

    func presentRetroAchievementsToast(
        title rawTitle: String,
        message rawMessage: String,
        badgePath rawBadgePath: String,
        duration: TimeInterval?,
        categoryTitle: String = "RetroAchievements",
        symbolName: String? = nil,
        imageData: Data? = nil
    ) {
        let title = rawTitle.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty else { return }

        let message = rawMessage.trimmingCharacters(in: .whitespacesAndNewlines)
        let badgePathValue = rawBadgePath.trimmingCharacters(in: .whitespacesAndNewlines)
        let toast = RetroAchievementsToast(
            categoryTitle: categoryTitle,
            title: title,
            message: message,
            badgePath: badgePathValue.isEmpty ? nil : badgePathValue,
            symbolName: symbolName,
            imageData: imageData
        )

        achievementsBanner.present(toast, displayDuration: duration)
    }

    func consumePendingRetroAchievementsToast() {
        guard let pending = ARMSX2Bridge.consumePendingRetroAchievementsNotification() else { return }
        presentRetroAchievementsToast(
            title: pending.title,
            message: pending.message,
            badgePath: pending.badgePath,
            duration: pending.duration > 0 ? pending.duration : nil
        )
    }

    func presentImportantStatusMessage(_ message: String) {
        presentStatusMessage(message, displayDuration: Self.importantStatusDisplayDuration)
    }
}
