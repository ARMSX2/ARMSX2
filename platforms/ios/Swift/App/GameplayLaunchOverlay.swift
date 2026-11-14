// GameplayLaunchOverlay.swift — launch card that zooms into the game
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Rebuilds the selected card as a live SwiftUI Liquid Glass surface while the
/// menu tree is dismantled underneath it. The cover and text scale with the
/// glass geometry, then the complete surface fades into the live Metal view.
struct GameplayLaunchOverlay: View {
    let transition: GameplayLaunchTransition
    let onBeginAudioFade: (TimeInterval) -> Void
    let onRevealGameplay: () -> Void
    let onComplete: () -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var zoomed = false
    @State private var fadingOut = false

    var body: some View {
        GeometryReader { geometry in
            let viewport = geometry.frame(in: .global)
            let sourceCenter = CGPoint(
                x: transition.sourceFrame.midX - viewport.minX,
                y: transition.sourceFrame.midY - viewport.minY
            )
            let destinationCenter = CGPoint(
                x: geometry.size.width / 2,
                y: geometry.size.height / 2
            )
            let destinationScale = zoomScale(in: geometry.size)

            ZStack {
                Color.black
                    .opacity(zoomed && !reduceMotion ? 0.14 : 0)

                FluidGameplayLaunchCard(transition: transition)
                    .frame(
                        width: transition.sourceFrame.width,
                        height: transition.sourceFrame.height
                    )
                    .scaleEffect(zoomed && !reduceMotion ? destinationScale : 1)
                    .position(zoomed && !reduceMotion ? destinationCenter : sourceCenter)
                    .shadow(
                        color: .black.opacity(fadingOut ? 0 : 0.34),
                        radius: zoomed ? 26 : 8,
                        y: zoomed ? 14 : 4
                    )
            }
            .opacity(fadingOut ? 0 : 1)
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onAppear {
            guard !reduceMotion else { return }
            DispatchQueue.main.async {
                withAnimation(.spring(response: 0.42, dampingFraction: 0.86)) {
                    zoomed = true
                }
            }
        }
        .task(id: transition.id) {
            await revealWhenEmulationIsRunning()
        }
    }

    private func zoomScale(in viewport: CGSize) -> CGFloat {
        guard transition.sourceFrame.width > 0, transition.sourceFrame.height > 0 else {
            return 1.2
        }

        let availableWidthScale = (viewport.width * 0.72) / transition.sourceFrame.width
        let availableHeightScale = (viewport.height * 0.76) / transition.sourceFrame.height
        return min(2.1, max(1.16, min(availableWidthScale, availableHeightScale)))
    }

    @MainActor
    private func revealWhenEmulationIsRunning() async {
        // The spring settles in about 0.42 s, so the zoomed card stays readable for about 1 s.
        let minimumDisplayNanoseconds: UInt64 = reduceMotion
            ? 240_000_000
            : 1_420_000_000
        try? await Task.sleep(nanoseconds: minimumDisplayNanoseconds)
        guard !Task.isCancelled else { return }

        for _ in 0..<24 where !ARMSX2Bridge.isVMRunning() {
            try? await Task.sleep(nanoseconds: 50_000_000)
            guard !Task.isCancelled else { return }
        }

        let fadeDuration = reduceMotion ? 0.15 : 0.30
        onBeginAudioFade(fadeDuration)
        withAnimation(.easeOut(duration: fadeDuration)) {
            onRevealGameplay()
            fadingOut = true
        }

        try? await Task.sleep(nanoseconds: UInt64(fadeDuration * 1_000_000_000))
        guard !Task.isCancelled else { return }
        onComplete()
    }
}

struct FluidGameplayLaunchCard: View {
    let transition: GameplayLaunchTransition
    var showsUnselectedFavoriteIndicator = false

    var body: some View {
        Group {
            switch transition.style {
            case .list:
                listContent
            case .grid, .coverFlow:
                coverContent
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(alignment: .topTrailing) {
            if transition.showsFavoriteIndicator,
               transition.isFavorite || showsUnselectedFavoriteIndicator {
                Image(systemName: transition.isFavorite ? "star.fill" : "star")
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(
                        transition.isFavorite ? .yellow : .white.opacity(0.86)
                    )
                    .padding(10)
                    .shadow(color: .black.opacity(0.28), radius: 4, y: 2)
            }
        }
        .glassSurface(
            interactive: true,
            clear: transition.usesClearGlass,
            cornerRadius: transition.cornerRadius
        )
        .clipShape(
            RoundedRectangle(
                cornerRadius: transition.cornerRadius,
                style: .continuous
            )
        )
    }

    private var listContent: some View {
        HStack(spacing: 12) {
            cover

            VStack(alignment: .leading, spacing: 4) {
                if transition.showsGameName {
                    Text(transition.title)
                        .font(.body.weight(.medium))
                        .foregroundStyle(.primary)
                        .lineLimit(transition.usesSingleLineGameName ? 1 : 2)
                }
                if transition.showsGameInfo {
                    Text(transition.detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Image(systemName: "chevron.right")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(12)
    }

    private var coverContent: some View {
        VStack(spacing: transition.style == .coverFlow ? 8 : 10) {
            cover
                .shadow(color: .black.opacity(0.28), radius: 18, y: 10)

            if transition.showsGameName || transition.showsGameInfo {
                VStack(spacing: 4) {
                    if transition.showsGameName {
                        Text(transition.title)
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.primary)
                            .multilineTextAlignment(.center)
                            .lineLimit(
                                transition.usesSingleLineGameName ? 1 : 2
                            )
                    }
                    if transition.showsGameInfo {
                        Text(transition.detail)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(transition.style == .coverFlow ? 8 : 12)
    }

    private var cover: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))

            if let image = transition.coverImage {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                VStack(spacing: 6) {
                    Image(
                        systemName: transition.gameName.lowercased().hasSuffix(".chd")
                            ? "archivebox"
                            : "opticaldisc"
                    )
                    .font(.system(size: 24, weight: .medium))
                    Text(
                        transition.gameName.lowercased().hasSuffix(".chd")
                            ? "CHD"
                            : "PS2"
                    )
                    .font(.caption2.bold())
                }
                .foregroundStyle(.secondary)
            }
        }
        .frame(
            width: transition.coverSize.width,
            height: transition.coverSize.height
        )
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
}
