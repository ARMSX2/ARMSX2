// GameLibraryNowRunningCards.swift — the library's Now Running section and cards
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryNowRunningSection: View {
    let controller: GameLibraryControllerCoordinator
    let title: String
    let coverGame: ISOEntry?
    let controllerInput: MenuControllerInputRouter?
    let opacity: Double
    let scale: Double
    let hasCustomBackground: Bool

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCriticalTextColour) private var criticalTextColour
    @Environment(\.menuTabIsActive) private var menuTabIsActive

    var body: some View {
        let isStopping = controller.nowRunningRemovalAnimationActive

        return Section {
            Button {
                controller.session.returnToGame()
            } label: {
                HStack {
                    HStack(spacing: 8) {
                        Image(
                            systemName: isStopping
                                ? "stop.circle.fill"
                                : "play.circle.fill"
                        )
                            .foregroundStyle(
                                isStopping ? criticalTextColour : accentColour
                            )
                            .font(.title)
                            .contentTransition(.symbolEffect(.replace))

                        RunningStatusText(
                            isStopping: isStopping,
                            runningLabel: controller.settings.localized("Now Running"),
                            gameTitle: title,
                            stoppingLabel: controller.settings.localized("Stopping..."),
                            stoppingFont: .title2,
                            statusColor: accentColour,
                            titleColor: contentTextColour
                        )
                    }
                    .opacity(controller.displayedNowRunningStatusOpacity)
                    .animation(.easeInOut(duration: 0.3), value: controller.displayedNowRunningStatusOpacity)

                    if !isStopping {
                        Spacer()
                        HStack(spacing: 6) {
                            Text(controller.settings.localized("Resume"))
                                .font(.subheadline)
                                .fontWeight(.medium)
                            Image(systemName: "chevron.right")
                                .font(.caption)
                                .foregroundStyle(.tertiary)
                        }
                        .transition(.opacity.combined(with: .scale(scale: 0.9)))
                    }
                }
                .frame(
                    maxWidth: .infinity,
                    alignment: isStopping ? .center : .leading
                )
                .padding(.vertical, 6)
                .background {
                    GameLibraryNowRunningCoverBackground(
                        coverGame: coverGame,
                        cornerRadius: 14
                    )
                }
                .clipShape(
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                )
                .animation(
                    .smooth(duration: 0.42, extraBounce: 0.02),
                    value: isStopping
                )
            }
            .disabled(isStopping)
            .tint(accentColour)
            .menuBackgroundListRow(hasCustomBackground)
            .compositingGroup()
            .modifier(controller.nowRunningControllerFocus(.resume, controllerInput: controllerInput, menuTabIsActive: menuTabIsActive))
            .scaleEffect(scale)
            .opacity(opacity)
            .animation(.linear(duration: 0.65), value: opacity)
            .animation(
                .smooth(duration: 0.65, extraBounce: 0.04),
                value: scale
            )

            if !isStopping {
                Button(role: .destructive) {
                    controller.showStopAlert = true
                } label: {
                    HStack {
                        Spacer()
                        Label(controller.settings.localized("Stop Emulation"), systemImage: "stop.circle")
                            .font(.subheadline)
                        Spacer()
                    }
                }
                .menuBackgroundListRow(hasCustomBackground)
                .compositingGroup()
                .scaleEffect(scale)
                .opacity(opacity)
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
                .animation(.linear(duration: 0.65), value: opacity)
                .animation(
                    .smooth(duration: 0.65, extraBounce: 0.04),
                    value: scale
                )
                .modifier(controller.nowRunningControllerFocus(.stop, controllerInput: controllerInput, menuTabIsActive: menuTabIsActive))
            }
        }
    }
}

struct GameLibraryNowRunningCard: View {
    let controller: GameLibraryControllerCoordinator
    let title: String
    let coverGame: ISOEntry?
    let controllerInput: MenuControllerInputRouter?

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCriticalTextColour) private var criticalTextColour
    @Environment(\.menuTabIsActive) private var menuTabIsActive

    var body: some View {
        let isStopping = controller.nowRunningRemovalAnimationActive

        return HStack(spacing: 12) {
            HStack(spacing: 8) {
                Image(
                    systemName: isStopping
                        ? "stop.circle.fill"
                        : "play.circle.fill"
                )
                    .font(.title2)
                    .foregroundStyle(
                        isStopping ? criticalTextColour : accentColour
                    )
                    .contentTransition(.symbolEffect(.replace))

                RunningStatusText(
                    isStopping: isStopping,
                    runningLabel: controller.settings.localized("Now Running"),
                    gameTitle: title,
                    stoppingLabel: controller.settings.localized("Stopping..."),
                    stoppingFont: .title2,
                    statusColor: accentColour,
                    titleColor: contentTextColour
                )
            }
            .opacity(controller.displayedNowRunningStatusOpacity)
            .animation(.easeInOut(duration: 0.3), value: controller.displayedNowRunningStatusOpacity)

            if !isStopping {
                Spacer()
                HStack(spacing: 8) {
                    Button(controller.settings.localized("Resume")) {
                        controller.session.returnToGame()
                    }
                    .buttonStyle(.borderedProminent)
                    .modifier(controller.nowRunningControllerFocus(.resume, controllerInput: controllerInput, menuTabIsActive: menuTabIsActive))
                    Button(role: .destructive) {
                        controller.showStopAlert = true
                    } label: {
                        Image(systemName: "stop.circle")
                    }
                    .buttonStyle(.bordered)
                    .modifier(controller.nowRunningControllerFocus(.stop, controllerInput: controllerInput, menuTabIsActive: menuTabIsActive))
                    .stopConfirmationSource()
                }
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(
            maxWidth: .infinity,
            alignment: isStopping ? .center : .leading
        )
        .padding()
        .background {
            GameLibraryNowRunningCoverBackground(
                coverGame: coverGame,
                cornerRadius: 18
            )
        }
        .clipShape(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
        )
        .animation(
            .smooth(duration: 0.42, extraBounce: 0.02),
            value: isStopping
        )
        .glassSurface(
            clear: true,
            materializeTransition: true,
            cornerRadius: 18
        )
    }
}

struct GameLibraryNowRunningCoverCard: View {
    let controller: GameLibraryControllerCoordinator
    let title: String
    let coverGame: ISOEntry?
    let controllerInput: MenuControllerInputRouter?
    let metrics: GameLibraryCoverFlowMetrics
    let controllerConnected: Bool

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCriticalTextColour) private var criticalTextColour
    @Environment(\.menuTabIsActive) private var menuTabIsActive

    var body: some View {
        let isStopping = controller.nowRunningRemovalAnimationActive

        return VStack(spacing: metrics.isCompact ? 9 : 14) {
            VStack(spacing: metrics.isCompact ? 7 : 10) {
                Image(
                    systemName: isStopping
                        ? "stop.circle.fill"
                        : "play.circle.fill"
                )
                    .font(.system(size: metrics.statusIconSize))
                    .foregroundStyle(
                        isStopping ? criticalTextColour : accentColour
                    )
                    .contentTransition(.symbolEffect(.replace))

                RunningStatusText(
                    isStopping: isStopping,
                    runningLabel: controller.settings.localized("Now Running"),
                    gameTitle: title,
                    stoppingLabel: controller.settings.localized("Stopping..."),
                    stoppingFont: metrics.isCompact ? .headline : .title3,
                    statusColor: accentColour,
                    titleColor: contentTextColour,
                    centered: true
                )
            }
            .opacity(controller.displayedNowRunningStatusOpacity)
            .animation(.easeInOut(duration: 0.3), value: controller.displayedNowRunningStatusOpacity)

            if !isStopping {
                HStack(spacing: 8) {
                    Button(controller.settings.localized("Resume")) {
                        controller.session.returnToGame()
                    }
                    .buttonStyle(.borderedProminent)
                    .modifier(controller.nowRunningControllerFocus(.resume, controllerInput: controllerInput, menuTabIsActive: menuTabIsActive))
                    Button(role: .destructive) {
                        controller.showStopAlert = true
                    } label: {
                        Image(systemName: "stop.circle")
                    }
                    .buttonStyle(.bordered)
                    .modifier(controller.nowRunningControllerFocus(.stop, controllerInput: controllerInput, menuTabIsActive: menuTabIsActive))
                    .stopConfirmationSource()
                }
                .controlSize(metrics.isCompact ? .small : .regular)
                .transition(.opacity.combined(with: .scale(scale: 0.9)))
            }
        }
        .frame(width: metrics.statusWidth, height: metrics.statusHeight)
        .padding(.horizontal, metrics.cardHorizontalPadding)
        .padding(.vertical, metrics.cardVerticalPadding)
        .background {
            GameLibraryNowRunningCoverBackground(
                coverGame: coverGame,
                cornerRadius: metrics.cornerRadius
            )
        }
        .clipShape(
            RoundedRectangle(
                cornerRadius: metrics.cornerRadius,
                style: .continuous
            )
        )
        .animation(
            .smooth(duration: 0.42, extraBounce: 0.02),
            value: isStopping
        )
        .glassSurface(
            clear: true,
            forceClear: controllerConnected,
            materializeTransition: true,
            cornerRadius: metrics.cornerRadius
        )
        .stopConfirmationCard()
    }
}

struct GameLibraryNowRunningCoverBackground: View {
    let coverGame: ISOEntry?
    let cornerRadius: CGFloat

    var body: some View {
        GeometryReader { geometry in
            if let game = coverGame,
               game.coverURL != nil,
               geometry.size.width > 1,
               geometry.size.height > 1 {
                CoverThumbnailView(
                    gameName: game.name,
                    coverURL: game.coverURL,
                    coverSignature: game.coverSignature,
                    width: geometry.size.width,
                    height: geometry.size.height,
                    cornerRadius: cornerRadius
                )
                .scaleEffect(1.08)
                .blur(radius: 14)
                .overlay(Color.black.opacity(0.34))
                .clipShape(
                    RoundedRectangle(
                        cornerRadius: cornerRadius,
                        style: .continuous
                    )
                )
                .allowsHitTesting(false)
                .accessibilityHidden(true)
            }
        }
    }
}

private struct RunningStatusText: View {
    let isStopping: Bool
    let runningLabel: String
    let gameTitle: String
    let stoppingLabel: String
    let stoppingFont: Font
    let statusColor: Color
    let titleColor: Color
    var centered = false

    @ViewBuilder
    var body: some View {
        if isStopping {
            Text(stoppingLabel)
                .font(stoppingFont.bold())
                .uiCriticalForegroundStyle()
                .lineLimit(1)
                .minimumScaleFactor(0.72)
                .multilineTextAlignment(.center)
                .transition(
                    .opacity
                        .combined(with: .scale(scale: 0.82))
                        .combined(with: .offset(x: 8))
                )
        } else {
            VStack(
                alignment: centered ? .center : .leading,
                spacing: 3
            ) {
                Text(runningLabel)
                    .font(.caption)
                    .foregroundStyle(statusColor)
                Text(gameTitle)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(titleColor)
                    .lineLimit(centered ? 2 : 1)
                    .truncationMode(.tail)
                    .multilineTextAlignment(centered ? .center : .leading)
            }
            .transition(
                .opacity
                    .combined(with: .scale(scale: 0.88))
                    .combined(with: .offset(x: -8))
            )
        }
	}
}
