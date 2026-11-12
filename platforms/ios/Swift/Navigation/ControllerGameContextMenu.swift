// ControllerGameContextMenu.swift — controller-navigable game actions
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Foundation

enum GameLibraryControllerMenuSection: Equatable {
    case root
    case covers
    case gameData
}

enum GameLibraryControllerMenuAction: String, Identifiable, Equatable {
    case playOrStop
    case gameInfo
    case perGameSettings
    case perGameShaders
    case perGameCustomSkin
    case discPath
    case cheatsAndPatches
    case covers
    case gameData
    case copyLaunchLink
    case downloadCover
    case chooseCoverPhoto
    case chooseCoverFile
    case removeCover
    case clearGameCache
    case deleteGameData
    case deleteGame
    case rename

    var id: String { rawValue }
}

struct GameLibraryControllerMenuItem: Identifiable, Equatable {
    let action: GameLibraryControllerMenuAction
    let title: String
    let systemImage: String
    var showsDisclosure = false
    var isDestructive = false

    var id: GameLibraryControllerMenuAction { action }
}

struct ControllerGameContextMenu: View {
    let game: ISOEntry
    let controllerInput: MenuControllerInputRouter?
    let sectionTitle: String
    let items: [GameLibraryControllerMenuItem]
    let selectedAction: GameLibraryControllerMenuAction?
    let isGameRunning: Bool
    let onPlayOrStop: () -> Void
    let onSelect: (GameLibraryControllerMenuAction) -> Void
    let onDismiss: () -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.uiContextMenuColour) private var contentTextColour
    @Environment(\.uiContextMenuSecondaryColour) private var secondaryTextColour
    @Environment(\.uiContextMenuFocusedColour) private var focusedTextColour
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCriticalTextColour) private var criticalTextColour
    @Environment(\.menuControllerInputRouter) private var environmentControllerInput

    private var feedbackInput: MenuControllerInputRouter? {
        controllerInput ?? environmentControllerInput
    }

    var body: some View {
        GeometryReader { proxy in
            let isCompact = verticalSizeClass == .compact || proxy.size.height < 500
            let bookFold = proxy.activeFold(book: true)
            let tabletopFold = proxy.activeFold(book: false)
            let panelWidth = min(
                isCompact ? 390 : 440,
                max(280, proxy.size.width - 24),
                (bookFold?.minX ?? .infinity) - 32
            )
            // The reader ignores the safe area, so the home indicator inset comes from the window.
            let bottomInset = UIApplication.shared.appWindowScene?.windows.first?.safeAreaInsets.bottom ?? 0
            let bandHeight = tabletopFold.map { max(0, proxy.size.height - bottomInset - 8 - ($0.maxY + 8)) }
            let rowHeight: CGFloat = isCompact ? 42 : 46
            let rowSpacing: CGFloat = isCompact ? 5 : 7
            let estimatedItemsHeight = CGFloat(items.count) * rowHeight
                + CGFloat(max(0, items.count - 1)) * rowSpacing
            let availableItemsHeight = max(
                rowHeight,
                proxy.size.height - (isCompact ? 132 : 230)
            )
            let menuHeight = min(estimatedItemsHeight, availableItemsHeight)

            ZStack {
                // Keep the modal hit shield, but do not tint the library. The
                // regular-glass window is the only visible context surface.
                Color.clear
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture {
                        feedbackInput?.playFeedback(.back)
                        onDismiss()
                    }

                OverlayPanelScaffold(usesRegularGlass: true) {
                    VStack(alignment: .leading, spacing: isCompact ? 7 : 14) {
                        header(isCompact: isCompact)
                        Divider()
                        menuItems(maxHeight: menuHeight, isCompact: isCompact)
                    }
                    .padding(isCompact ? 11 : 18)
                }
                .frame(width: panelWidth)
                .frame(maxHeight: bandHeight)
                .environment(\.clearLiquidGlassUIEnabled, false)
                .padding(.vertical, tabletopFold == nil ? (isCompact ? 6 : 21) : 0)
                .padding(.top, tabletopFold.map { $0.maxY + 8 } ?? 0)
                .transition(
                    reduceMotion
                        ? .opacity
                        : .scale(scale: 0.92).combined(with: .opacity)
                )
                .frame(
                    maxWidth: .infinity,
                    maxHeight: .infinity,
                    alignment: tabletopFold == nil ? .center : .top
                )
                .centredInLeadingPaneOnFold()
            }
        }
        // The retained library is shortened by the persistent tab bar. Expand
        // this modal surface into the window safe area before centering so a
        // compact landscape menu is centered on screen instead of the lower
        // center of the library viewport.
        .ignoresSafeArea(.container, edges: .all)
        .zIndex(10_000)
        .preferredColorScheme(.dark)
        .environment(\.clearLiquidGlassUIEnabled, false)
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Context Menu for \(game.displayName)")
        .accessibilityAddTraits(.isModal)
        .accessibilityAction(.escape) {
            feedbackInput?.playFeedback(.back)
            onDismiss()
        }
        .onAppear {
            // Presentation itself is tactile even when a long press came from
            // a separate UIKit gesture recognizer. Audio stays at the state
            // transition so the menu cannot play twice.
            feedbackInput?.playTouchHaptics(.contextMenu)
        }
    }

    private func header(isCompact: Bool) -> some View {
        let coverWidth: CGFloat = isCompact ? 36 : 54
        let coverHeight: CGFloat = isCompact ? 54 : 81
        let playOrStopSelected = controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
            && selectedAction == .playOrStop

        return HStack(spacing: isCompact ? 10 : 14) {
            CoverThumbnailView(
                gameName: game.name,
                coverURL: game.coverURL,
                coverSignature: game.coverSignature,
                width: coverWidth,
                height: coverHeight
            )

            VStack(alignment: .leading, spacing: isCompact ? 2 : 4) {
                if !sectionTitle.isEmpty {
                    Text(sectionTitle)
                        .font((isCompact ? Font.caption2 : .caption).weight(.semibold))
                        .foregroundStyle(secondaryTextColour)
                        .textCase(.uppercase)
                }
                Text(game.displayName)
                    .font(
                        isCompact
                            ? .headline.weight(.bold)
                            : .title3.weight(.bold)
                    )
                    .foregroundStyle(accentColour)
                    .lineLimit(isCompact ? 1 : 2)
                Text("\(formatLabel)  \(game.sizeLabel)")
                    .font(isCompact ? .caption2 : .caption)
                    .foregroundStyle(secondaryTextColour)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Button {
                feedbackInput?.playFeedback(.activate)
                onPlayOrStop()
            } label: {
                VStack(spacing: isCompact ? 2 : 5) {
                    Image(
                        systemName: isGameRunning
                            ? "stop.fill"
                            : "play.fill"
                    )
                        .font(isCompact ? .callout.weight(.bold) : .title3.weight(.bold))
                    Text(
                        settingsLocalized(isGameRunning ? "Stop" : "Play")
                    )
                        .font((isCompact ? Font.caption2 : .caption).weight(.semibold))
                }
                .foregroundStyle(isGameRunning ? criticalTextColour : accentColour)
                .frame(width: coverWidth, height: coverHeight)
                .background {
                    RoundedRectangle(cornerRadius: isCompact ? 9 : 12, style: .continuous)
                        .fill(Color.black.opacity(playOrStopSelected ? 0.32 : 0.20))
                }
                .controllerFocusBoxPresentation(
                    isVisible: playOrStopSelected,
                    cornerRadius: isCompact ? 9 : 12
                )
                .contentShape(RoundedRectangle(cornerRadius: isCompact ? 9 : 12))
            }
            .buttonStyle(.plain)
            .focusEffectDisabled()
            .controllerNavigationOrbTarget(
                id: "context.play-or-stop",
                isActive: playOrStopSelected,
                palette: isGameRunning ? .red : .blue,
                inset: 2,
                orbScale: isCompact ? 0.74 : 0.86,
                priority: 100
            )
            .accessibilityLabel(
                settingsLocalized(isGameRunning ? "Stop" : "Play")
            )
        }
    }

    private func settingsLocalized(_ text: String) -> String {
        SettingsStore.shared.localized(text)
    }

    private var formatLabel: String {
        let fileExtension = (game.name as NSString).pathExtension.uppercased()
        return fileExtension.isEmpty ? "FILE" : fileExtension
    }

    private func menuItems(maxHeight: CGFloat, isCompact: Bool) -> some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: isCompact ? 3 : 7) {
                    ControllerRightStickScrollTarget(
                        controllerInput: controllerInput,
                        axes: .vertical,
                        manualCaptureOwner:
                            MenuControllerNavigationCaptureOwner.gameLibraryPresentation,
                        priority: 100,
                        pointsPerSecond: 680
                    )
                    .frame(height: 0)
                    ForEach(items) { item in
                        menuRow(item, isCompact: isCompact)
                            .id(item.action)
                    }
                }
            }
            .frame(maxHeight: maxHeight)
            .scrollIndicators(.visible)
            .onChange(of: selectedAction) { _, action in
                guard let action else { return }
                withAnimation(.snappy(duration: 0.22)) {
                    proxy.scrollTo(action, anchor: .center)
                }
            }
        }
    }

    private func menuRow(
        _ item: GameLibraryControllerMenuItem,
        isCompact: Bool
    ) -> some View {
        // The router remains available to touch-only menus, and its default
        // navigation mode is enabled even before a controller connects. Only
        // publish visual focus when a real controller owns input.
        let isSelected = controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
            && item.action == selectedAction

        return Button {
            feedbackInput?.playFeedback(
                item.showsDisclosure ? .destination : .activate
            )
            onSelect(item.action)
        } label: {
            HStack(spacing: isCompact ? 9 : 12) {
                Image(systemName: item.systemImage)
                    .frame(width: isCompact ? 18 : 22)
                    .foregroundStyle(
                        item.isDestructive ? criticalTextColour : accentColour
                    )
                Text(item.title)
                    .font((isCompact ? Font.callout : .body).weight(isSelected ? .semibold : .regular))
                    .frame(maxWidth: .infinity, alignment: .leading)
                if item.showsDisclosure {
                        Image(systemName: "chevron.right")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(accentColour)
                        .accessibilityHidden(true)
                }
            }
            .foregroundStyle(
                item.isDestructive
                    ? criticalTextColour
                    : (isSelected ? focusedTextColour : contentTextColour)
            )
            .padding(.horizontal, isCompact ? 10 : 14)
            .frame(minHeight: isCompact ? 42 : 46)
            .contentShape(Rectangle())
            .background {
                RoundedRectangle(cornerRadius: isCompact ? 11 : 14, style: .continuous)
                    .fill(isSelected ? Color.black.opacity(0.28) : .clear)
            }
            .controllerFocusBoxPresentation(
                isVisible: isSelected,
                cornerRadius: isCompact ? 11 : 14
            )
            .scaleEffect(isSelected && !reduceMotion ? 1.008 : 1)
            .shadow(
                color: .black.opacity(isSelected ? 0.12 : 0),
                radius: isSelected ? 4 : 0,
                y: isSelected ? 2 : 0
            )
            .animation(
                reduceMotion
                    ? .linear(duration: 0.1)
                    : .smooth(duration: 0.2, extraBounce: 0.04),
                value: isSelected
            )
        }
        .buttonStyle(.plain)
        .focusEffectDisabled()
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        // Read the final button bounds after its focus transform. Anchoring
        // inside the label omitted Button/List adjustments and displaced the
        // orbit from the row that actually receives controller activation.
        .controllerNavigationOrbTarget(
            id: "context.\(item.action.rawValue)",
            isActive: isSelected,
            palette: item.isDestructive ? .red : .blue,
            inset: 2,
            orbScale: isCompact ? 0.74 : 0.86,
            priority: 100
        )
    }
}

