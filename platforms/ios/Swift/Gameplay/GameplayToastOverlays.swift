// GameplayToastOverlays.swift — the game screen's status and RetroAchievements toasts
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

struct GameplayToastOverlays: View {
    let feedback: GameplayFeedback
    let runtimePerGameLivePreviewToast: PerGameLivePreviewToastState?
    @ObservedObject private var statusBanner: TransientBannerController<String>
    @ObservedObject private var achievementsBanner: TransientBannerController<RetroAchievementsToast>

    init(feedback: GameplayFeedback, livePreviewToast: PerGameLivePreviewToastState?) {
        self.feedback = feedback
        runtimePerGameLivePreviewToast = livePreviewToast
        statusBanner = feedback.statusBanner
        achievementsBanner = feedback.achievementsBanner
    }

    private var displaySafeAreaInsets: UIEdgeInsets {
        UIApplication.shared.appWindowScene?.windows.first?.safeAreaInsets ?? .zero
    }

    var body: some View {
        Color.clear
            .overlay(alignment: .bottom) {
                statusToastOverlay
            }
            .overlay(alignment: .top) {
                retroAchievementsToastOverlay
            }
            .onChange(of: statusBanner.generation) { oldGeneration, newGeneration in
                guard newGeneration > oldGeneration else { return }
                MenuAudioPackManager.shared.playEvent(.uiToast)
            }
            .onChange(of: achievementsBanner.generation) { oldGeneration, newGeneration in
                guard newGeneration > oldGeneration else { return }
                MenuAudioPackManager.shared.playEvent(.achievementToast)
            }
    }

    @ViewBuilder
    private var statusToastOverlay: some View {
        GameScreenStatusToastOverlay(
            livePreview: runtimePerGameLivePreviewToast,
            fallback: statusBanner,
            bottomPadding: 24,
            horizontalPadding: max(
                max(
                    displaySafeAreaInsets.left,
                    displaySafeAreaInsets.right
                ),
                14
            )
        )
    }

    @ViewBuilder
    private var retroAchievementsToastOverlay: some View {
        if let retroAchievementsToast = achievementsBanner.content {
            HStack(spacing: 12) {
                retroAchievementsBadge(
                    path: retroAchievementsToast.badgePath,
                    imageData: retroAchievementsToast.imageData,
                    fallbackSystemImage: retroAchievementsToast.symbolName ?? "trophy.fill"
                )

                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 4) {
                        if let symbolName = retroAchievementsToast.symbolName {
                            Image(systemName: symbolName)
                        }
                        Text(retroAchievementsToast.categoryTitle)
                    }
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.yellow)
                    Text(retroAchievementsToast.title)
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(.white)
                        .lineLimit(1)
                    if !retroAchievementsToast.message.isEmpty {
                        Text(retroAchievementsToast.message)
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.82))
                            .lineLimit(2)
                    }
                }

                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(maxWidth: 390)
            .background(.black.opacity(0.78), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(.white.opacity(0.12), lineWidth: 1)
            }
            .padding(.top, max(displaySafeAreaInsets.top, 54))
            .padding(.horizontal, max(max(displaySafeAreaInsets.left, displaySafeAreaInsets.right), 14))
            .allowsHitTesting(false)
            .transition(.opacity.combined(with: .move(edge: .top)))
        }
    }

    @ViewBuilder
    private func retroAchievementsBadge(
        path: String?,
        imageData: Data? = nil,
        fallbackSystemImage: String = "trophy.fill"
    ) -> some View {
        if let imageData,
           let image = UIImage(data: imageData) {
            Image(uiImage: image)
                .resizable()
                .scaledToFill()
                .frame(width: 58, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        } else if let path,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 42, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        } else {
            Image(systemName: fallbackSystemImage)
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(.yellow)
                .frame(width: 42, height: 42)
                .background(.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
    }
}
