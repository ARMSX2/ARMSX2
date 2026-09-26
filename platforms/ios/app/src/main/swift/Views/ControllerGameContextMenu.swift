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
            let panelWidth = min(isCompact ? 390 : 440, max(280, proxy.size.width - 24))
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
                .environment(\.clearLiquidGlassUIEnabled, false)
                .padding(.vertical, isCompact ? 6 : 21)
                .transition(
                    reduceMotion
                        ? .opacity
                        : .scale(scale: 0.92).combined(with: .opacity)
                )
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
            .frame(height: maxHeight)
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

struct ControllerNavigationAlertAction: Identifiable, Equatable {
    let id: String
    let title: String
    var systemImage: String? = nil
    var isDestructive = false
    var activationFeedback: MenuControllerFeedback = .activate
}

struct ControllerNavigationAlertDetail: Identifiable {
    let id: String
    let label: String
    let value: String
    var isOn: Bool? = nil
    var onPrevious: (() -> Void)? = nil
    var onNext: (() -> Void)? = nil
    var onActivate: (() -> Void)? = nil

    var isInteractive: Bool {
        onPrevious != nil || onNext != nil || onActivate != nil
    }
}

private struct ControllerNavigationAlertDetailRow: View {
    let detail: ControllerNavigationAlertDetail
    let isSelected: Bool
    let contentColour: Color
    let secondaryContentColour: Color
    let valueColour: Color
    let isCompact: Bool
    let isDense: Bool
    let usesLargeTypography: Bool
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.menuControllerInputRouter) private var controllerInput

    var body: some View {
        HStack(alignment: .center, spacing: isCompact ? 8 : 12) {
            Text(detail.label)
                .foregroundStyle(secondaryContentColour)
                .lineLimit(2)
            Spacer(minLength: 8)
            if detail.isOn == nil
                && (detail.onPrevious != nil || detail.onNext != nil) {
                Button {
                    controllerInput?.playFeedback(.activate)
                    detail.onPrevious?()
                } label: {
                    Image(systemName: "chevron.left")
                        .foregroundStyle(accentColour)
                        .frame(
                            width: isDense ? 26 : (isCompact ? 28 : 32),
                            height: isDense ? 26 : (isCompact ? 28 : 32)
                        )
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(detail.onPrevious == nil)
                .accessibilityLabel("Previous \(detail.label)")

                Text(detail.value)
                    .foregroundStyle(valueColour)
                    .multilineTextAlignment(.center)
                    .lineLimit(2)
                    .minimumScaleFactor(0.72)
                    .frame(minWidth: isDense ? 64 : (isCompact ? 72 : 96))

                Button {
                    controllerInput?.playFeedback(.activate)
                    detail.onNext?()
                } label: {
                    Image(systemName: "chevron.right")
                        .foregroundStyle(accentColour)
                        .frame(
                            width: isDense ? 26 : (isCompact ? 28 : 32),
                            height: isDense ? 26 : (isCompact ? 28 : 32)
                        )
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(detail.onNext == nil)
                .accessibilityLabel("Next \(detail.label)")
            } else if let isOn = detail.isOn {
                Button {
                    let nextValue = !isOn
                    controllerInput?.playFeedback(.toggle(isOn: nextValue))
                    if nextValue {
                        detail.onNext?()
                    } else {
                        detail.onPrevious?()
                    }
                } label: {
                    Capsule(style: .continuous)
                        .fill(isOn ? accentColour : Color.secondary.opacity(0.45))
                        .frame(width: 48, height: 28)
                        .overlay(alignment: isOn ? .trailing : .leading) {
                            Circle()
                                .fill(.white)
                                .padding(3)
                        }
                        .animation(.easeOut(duration: 0.16), value: isOn)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(detail.label)
                .accessibilityValue(isOn ? "On" : "Off")
            } else {
                Text(detail.value)
                    .foregroundStyle(valueColour)
                    .multilineTextAlignment(.trailing)
                    .lineLimit(2)
            }
        }
        .font(
            usesLargeTypography
                ? .body
                : (isDense ? .caption : (isCompact ? .caption : .callout))
        )
        .padding(.horizontal, isDense ? 8 : 10)
        .frame(minHeight: isDense ? 30 : (isCompact ? 38 : 44))
        .background {
            RoundedRectangle(cornerRadius: 11, style: .continuous)
                .fill(isSelected ? Color.black.opacity(0.24) : .clear)
        }
        .contentShape(RoundedRectangle(cornerRadius: 11, style: .continuous))
        .onTapGesture {
            guard detail.onPrevious == nil, detail.onNext == nil else { return }
            controllerInput?.playFeedback(.activate)
            detail.onActivate?()
        }
        .controllerNavigationOrbTarget(
            id: "alert.detail.\(detail.id)",
            isActive: isSelected,
            palette: .blue,
            inset: 2,
            orbScale: 0.78,
            priority: 200
        )
        .modifier(
            ControllerNavigationAlertDetailFocusModifier(detail: detail)
        )
    }
}

private struct ControllerNavigationAlertDetailFocusModifier: ViewModifier {
    let detail: ControllerNavigationAlertDetail

    @ViewBuilder
    func body(content: Content) -> some View {
        if detail.isInteractive {
            content.controllerAccessibilityPickerTarget(
                id: "alert.detail.\(detail.id)",
                label: detail.label,
                value: detail.value,
                onActivate: detail.onActivate,
                onIncrement: { detail.onNext?() },
                onDecrement: { detail.onPrevious?() }
            )
        } else {
            content
        }
    }
}

/// A stable controller-first confirmation surface. Unlike a system alert, its
/// selected row remains visible while the D-pad moves between actions.
struct ControllerNavigationAlert: View {
    let title: String
    var titleColour: Color? = nil
    var contentColour: Color? = nil
    var secondaryContentColour: Color? = nil
    var detailValueColour: Color? = nil
    var previewImageURL: URL? = nil
    var previewSkinDescriptor: VPadSkinDescriptor? = nil
    var previewAccessibilityLabel: String? = nil
    var details: [ControllerNavigationAlertDetail] = []
    var emphasizesPreview = false
    var prefersTopPlacement = false
    var usesClearGlass = false
    var usesLargeLandscapePanel = false
    var usesCompactPreviewPanel = false
    var showsActionButtonBackgrounds = false
    var dimsBackground = true
    let message: String
    let actions: [ControllerNavigationAlertAction]
    let selectedIndex: Int
    let onSelect: (Int) -> Void
    let onDismiss: () -> Void

    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.controllerAccessibilityNavigationActive) private var sharedNavigationActive
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCriticalTextColour) private var criticalTextColour

    private var interactiveDetails: [ControllerNavigationAlertDetail] {
        details.filter(\.isInteractive)
    }

    private var showsControllerSelection: Bool {
        controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
    }

    var body: some View {
        GeometryReader { proxy in
            let isCompact = verticalSizeClass == .compact || proxy.size.height < 500
            let isLandscape = proxy.size.width > proxy.size.height
            let usesWidePreviewLayout = emphasizesPreview
                && isLandscape
            let maximumPanelWidth: CGFloat = {
                if usesCompactPreviewPanel {
                    return usesWidePreviewLayout ? 580 : 440
                }
                return usesWidePreviewLayout
                    ? (usesLargeLandscapePanel ? 840 : 740)
                    : (emphasizesPreview ? 520 : (isCompact ? 390 : 430))
            }()
            let panelWidth = min(
                maximumPanelWidth,
                max(280, proxy.size.width - 32)
            )
            let previewWidth = max(
                150,
                min(
                    usesCompactPreviewPanel
                        ? (usesWidePreviewLayout ? 225 : 230)
                        : (usesWidePreviewLayout
                            ? (usesLargeLandscapePanel ? 390 : 360)
                            : 290),
                    (panelWidth - 42) * (usesWidePreviewLayout ? 0.46 : 0.48)
                )
            )
            let alignsTop = prefersTopPlacement
            let topInset = alignsTop
                ? max(8, proxy.safeAreaInsets.top)
                : 10
            let panelPadding: CGFloat = usesCompactPreviewPanel
                ? 12
                : (usesWidePreviewLayout
                    ? (usesLargeLandscapePanel ? 18 : 8)
                    : (isCompact ? 14 : 20))
            let previewHeight: CGFloat = {
                guard emphasizesPreview else { return 0 }
                if usesCompactPreviewPanel {
                    return usesWidePreviewLayout
                        ? 154
                        : min(176, max(132, proxy.size.height * 0.20))
                }
                return usesWidePreviewLayout
                    ? (usesLargeLandscapePanel ? 294 : 196)
                    : min(248, max(176, proxy.size.height * 0.30))
            }()
            let previewPlacementOffsetY: CGFloat = {
                guard emphasizesPreview, prefersTopPlacement else { return 0 }
                if isLandscape {
                    // The game-library content begins below its navigation
                    // chrome. Lift the chooser back toward the visual centre
                    // of the complete screen and clear the floating tab bar.
                    return -min(84, max(56, proxy.size.height * 0.24))
                }
                // Portrait has considerably more vertical content. Move the
                // complete chooser into the unused card area above it rather
                // than allowing its actions to sit behind the tab bar.
                return -min(152, max(96, proxy.size.height * 0.18))
            }()

            ZStack(alignment: alignsTop ? .top : .center) {
                Group {
                    if dimsBackground {
                        OverlayTheme.scrimBase.opacity(
                            isCompact
                                ? OverlayTheme.scrimPhoneLandscape
                                : OverlayTheme.scrimPhonePortrait
                        )
                    } else {
                        // Alerts launched from the game context menu retain a
                        // modal hit shield without painting a second tint.
                        Color.clear
                    }
                }
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture {
                        controllerInput?.playFeedback(.back)
                        onDismiss()
                    }
                    .zIndex(0)

                // Keep ordinary confirmations at their intrinsic master-style
                // height. The previous greedy ScrollView fallback accepted the
                // full overlay proposal and made one-line alerts nearly
                // fullscreen. The preview chooser uses a dense two-column
                // layout sized to fit without internal scrolling.
                OverlayPanelScaffold(usesRegularGlass: !usesClearGlass) {
                    panelContent(
                        isCompact: isCompact,
                        usesWidePreviewLayout: usesWidePreviewLayout,
                        previewWidth: previewWidth,
                        previewHeight: previewHeight
                    )
                    .padding(panelPadding)
                }
                .frame(width: panelWidth)
                // Let confirmations hug their real contents in every
                // orientation. The large skin chooser still keeps its existing
                // two-column width, preview, controls and typography; it simply
                // no longer stretches into unused vertical space.
                .fixedSize(horizontal: false, vertical: true)
                .environment(
                    \.clearLiquidGlassUIEnabled,
                    usesClearGlass
                )
                .overlay {
                    RoundedRectangle(cornerRadius: 26, style: .continuous)
                        .stroke(Color.primary.opacity(0.12), lineWidth: 0.7)
                }
                .shadow(color: .black.opacity(0.24), radius: 22, y: 10)
                .padding(.top, topInset)
                // The game-library overlay is already inset above its tab bar.
                // Reserving that height here a second time compressed the
                // landscape skin chooser and forced it to scroll.
                .padding(.bottom, alignsTop ? 8 : 0)
                .offset(y: previewPlacementOffsetY)
                .zIndex(1)
            }
        }
        .zIndex(20_000)
        .accessibilityElement(children: .contain)
        .accessibilityLabel(title)
        .onAppear {
            // Every app-owned prompt gets a lightweight presentation pulse;
            // its owner remains responsible for the semantic sound (toast,
            // no-JIT, stop, and so on).
            controllerInput?.playTouchHaptics(.contextMenu)
        }
    }

    @ViewBuilder
    private func panelContent(
        isCompact: Bool,
        usesWidePreviewLayout: Bool,
        previewWidth: CGFloat,
        previewHeight: CGFloat
    ) -> some View {
        VStack(
            alignment: .leading,
            spacing: usesWidePreviewLayout ? 5 : (isCompact ? 8 : 14)
        ) {
            Text(title)
                .font(
                    usesLargeLandscapePanel && usesWidePreviewLayout
                        ? .title2.weight(.bold)
                        : (isCompact ? .headline : .title3.weight(.semibold))
                )
                .foregroundStyle(
                    titleColour ?? contentColour ?? Color.white
                )
            if usesWidePreviewLayout {
                HStack(alignment: .top, spacing: 12) {
                    preview(
                        isCompact: isCompact,
                        preferredHeight: previewHeight
                    )
                        .frame(width: previewWidth)
                    supplementalContent(
                        isCompact: isCompact,
                        isDense: usesCompactPreviewPanel
                            || !usesLargeLandscapePanel,
                        showsActions: false
                    )
                }
                if !actions.isEmpty {
                    HStack(spacing: 8) {
                        alertActionButtons(
                            isCompact: isCompact,
                            isDense: usesCompactPreviewPanel
                        )
                    }
                }
            } else {
                preview(
                    isCompact: isCompact,
                    preferredHeight: previewHeight
                )
                supplementalContent(
                    isCompact: isCompact,
                    isDense: usesCompactPreviewPanel,
                    showsActions: true
                )
            }
        }
    }

    @ViewBuilder
    private func preview(
        isCompact: Bool,
        preferredHeight: CGFloat
    ) -> some View {
        // An installed descriptor is the source of truth for what gameplay
        // will render. Prefer it over a catalogue thumbnail so cycling the
        // picker immediately shows bundled and imported skins without waiting
        // for remote artwork or inheriting an AsyncImage phase from the prior
        // selection.
        if let previewSkinDescriptor {
            ControllerSkinArtworkPreview(descriptor: previewSkinDescriptor)
                .id(previewSkinDescriptor.id)
                .frame(maxWidth: .infinity)
                .frame(
                    height: emphasizesPreview
                        ? preferredHeight
                        : (isCompact ? 82 : 132)
                )
                .background(
                    Color.black.opacity(0.18),
                    in: RoundedRectangle(cornerRadius: 14, style: .continuous)
                )
                .clipShape(
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                )
                .accessibilityLabel(previewAccessibilityLabel ?? title)
        } else if let previewImageURL {
            AsyncImage(url: previewImageURL) { phase in
                switch phase {
                case .success(let image):
                    image
                        .resizable()
                        .scaledToFit()
                case .empty:
                    controllerSkinPreviewFallback(isCompact: isCompact)
                case .failure:
                    controllerSkinPreviewFallback(isCompact: isCompact)
                @unknown default:
                    controllerSkinPreviewFallback(isCompact: isCompact)
                }
            }
            // A manual picker can gain its preview URL after the catalog
            // manifest finishes loading, and cycling reuses this view slot.
            // Key the loader to the concrete artwork so a failed/empty phase
            // from the previous skin cannot remain onscreen.
            .id(previewImageURL.absoluteString)
            .frame(maxWidth: .infinity)
            .frame(
                height: emphasizesPreview
                    ? preferredHeight
                    : (isCompact ? 82 : 132)
            )
            .background(
                Color.black.opacity(0.18),
                in: RoundedRectangle(cornerRadius: 14, style: .continuous)
            )
            .clipShape(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
            )
            .accessibilityLabel(previewAccessibilityLabel ?? title)
        } else if emphasizesPreview {
            controllerSkinPreviewFallback(isCompact: isCompact)
                .frame(maxWidth: .infinity)
                .frame(height: preferredHeight)
                .background(
                    Color.black.opacity(0.18),
                    in: RoundedRectangle(
                        cornerRadius: 14,
                        style: .continuous
                    )
                )
                .accessibilityLabel(previewAccessibilityLabel ?? title)
        }
    }

    @ViewBuilder
    private func controllerSkinPreviewFallback(isCompact: Bool) -> some View {
        Image(systemName: "gamecontroller.fill")
            .font(.system(size: isCompact ? 58 : 78))
            .foregroundStyle(accentColour)
    }

    @ViewBuilder
    private func supplementalContent(
        isCompact: Bool,
        isDense: Bool,
        showsActions: Bool
    ) -> some View {
        VStack(
            alignment: .leading,
            spacing: isDense ? 5 : (isCompact ? 8 : 14)
        ) {
            if !details.isEmpty {
                VStack(spacing: isDense ? 1 : (isCompact ? 3 : 5)) {
                    ForEach(details) { detail in
                        let detailIndex = interactiveDetails.firstIndex {
                            $0.id == detail.id
                        }
                        let isSelected = !sharedNavigationActive
                            && showsControllerSelection
                            && detailIndex == selectedIndex
                        ControllerNavigationAlertDetailRow(
                            detail: detail,
                            isSelected: isSelected,
                            contentColour: contentColour ?? .white,
                            secondaryContentColour:
                                secondaryContentColour
                                ?? (contentColour ?? .white).opacity(0.78),
                            valueColour: detailValueColour ?? contentColour ?? .white,
                            isCompact: isCompact,
                            isDense: isDense,
                            usesLargeTypography: usesLargeLandscapePanel
                        )
                    }
                }
                .padding(isDense ? 4 : (isCompact ? 5 : 7))
                .background(
                    Color.black.opacity(0.16),
                    in: RoundedRectangle(cornerRadius: 13, style: .continuous)
                )
            }
            if !message.isEmpty {
                Text(message)
                    .font(
                        usesLargeLandscapePanel
                            ? .body
                            : (isCompact ? .caption : .callout)
                    )
                    .foregroundStyle(
                        secondaryContentColour
                            ?? (contentColour ?? Color.white).opacity(0.84)
                    )
                    .fixedSize(horizontal: false, vertical: true)
            }
            if showsActions {
                Group {
                    if actions.count == 2 {
                        HStack(spacing: 6) {
                            alertActionButtons(
                                isCompact: isCompact,
                                isDense: isDense
                            )
                        }
                    } else {
                        VStack(spacing: isCompact ? 3 : 6) {
                            alertActionButtons(
                                isCompact: isCompact,
                                isDense: isDense
                            )
                        }
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private func alertActionButtons(
        isCompact: Bool,
        isDense: Bool
    ) -> some View {
                ForEach(Array(actions.enumerated()), id: \.element.id) { index, action in
                    let isSelected = !sharedNavigationActive
                        && showsControllerSelection
                        && selectedIndex == interactiveDetails.count + index
                    let buttonTint = actionButtonTint(action)
                    Button {
                        controllerInput?.playFeedback(action.activationFeedback)
                        onSelect(index)
                    } label: {
                        HStack(spacing: isDense ? 8 : 10) {
                            if let systemImage = action.systemImage {
                                Image(systemName: systemImage)
                                    .font(.body.weight(.semibold))
                                    .foregroundStyle(
                                        action.isDestructive
                                            ? criticalTextColour
                                            : accentColour
                                    )
                                    .frame(width: 22)
                            }
                            Text(action.title)
                            .font(
                                (usesLargeLandscapePanel
                                    ? Font.title3
                                    : (isCompact ? Font.callout : .body))
                                    .weight(isSelected ? .semibold : .regular)
                            )
                            .controllerFocusedTextColor(
                                normal: action.isDestructive
                                    ? criticalTextColour
                                    : (contentColour ?? Color.white),
                                allowsFocusedBlue: false
                            )
                            if action.systemImage != nil
                                || !showsActionButtonBackgrounds {
                                Spacer(minLength: 0)
                            }
                        }
                            .frame(
                                maxWidth: .infinity,
                                alignment: showsActionButtonBackgrounds
                                    ? .center
                                    : .leading
                            )
                            .padding(.horizontal, 12)
                            // Retain Apple's 44-point minimum while avoiding
                            // the oversized confirmation panels produced by a
                            // 48/52-point row. The Button itself stays full
                            // width, so Stop remains easy to tap.
                            .frame(minHeight: isDense ? 36 : 44)
                            .contentShape(
                                RoundedRectangle(
                                    cornerRadius: 11,
                                    style: .continuous
                                )
                            )
                            .background {
                                RoundedRectangle(cornerRadius: 11, style: .continuous)
                                    .fill(
                                        showsActionButtonBackgrounds
                                            ? buttonTint.opacity(
                                                isSelected ? 0.30 : 0.14
                                            )
                                            : (isSelected
                                                ? Color.black.opacity(0.24)
                                                : .clear)
                                    )
                            }
                            .overlay {
                                if showsActionButtonBackgrounds {
                                    RoundedRectangle(
                                        cornerRadius: 11,
                                        style: .continuous
                                    )
                                    .stroke(
                                        buttonTint.opacity(
                                            isSelected ? 0.88 : 0.48
                                        ),
                                        lineWidth: isSelected ? 1.25 : 0.8
                                    )
                                }
                            }
                    }
                    .buttonStyle(.plain)
                    .frame(maxWidth: .infinity)
                    .controllerAccessibilityActionTarget(
                        id: "alert.action.\(action.id)",
                        label: action.title,
                        focusedColor: action.isDestructive
                            ? criticalTextColour
                            : (contentColour ?? Color.white),
                        activationFeedback: action.activationFeedback
                    ) {
                        onSelect(index)
                    }
                    .focusEffectDisabled()
                    .accessibilityAddTraits(isSelected ? .isSelected : [])
                    .controllerNavigationOrbTarget(
                        id: "alert.\(action.id)",
                        isActive: isSelected,
                        palette: action.isDestructive ? .red : .blue,
                        inset: 2,
                        orbScale: 0.78,
                        priority: 200
                    )
                }
    }

    private func actionButtonTint(
        _ action: ControllerNavigationAlertAction
    ) -> Color {
        if action.isDestructive {
            return criticalTextColour
        }
        switch action.id {
        case "apply", "ok":
            return detailValueColour ?? accentColour
        default:
            return contentColour ?? .white
        }
    }
}

/// Local preview used while catalog artwork is loading and for bundled or
/// manually imported skins which do not have a remote preview URL. Keeping the
/// preview tied to the selected descriptor also prevents cycling from leaving
/// the previous skin's failed AsyncImage phase onscreen.
private struct ControllerSkinArtworkPreview: View {
    let descriptor: VPadSkinDescriptor

    var body: some View {
        if let fullSkin = ControllerAsset.fullSkinImage(
            descriptor: descriptor,
            isLandscape: true
        ) ?? ControllerAsset.fullSkinImage(
            descriptor: descriptor,
            isLandscape: false
        ) {
            Image(uiImage: fullSkin)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
        } else {
            controllerFace
        }
    }

    private var controllerFace: some View {
        GeometryReader { proxy in
            let width = proxy.size.width
            let height = proxy.size.height
            let unit = min(width / 9.2, height / 4.5)

            ZStack {
                shoulder(.L2, label: "L2", unit: unit)
                    .position(x: width * 0.16, y: height * 0.19)
                shoulder(.L1, label: "L1", unit: unit)
                    .position(x: width * 0.29, y: height * 0.19)
                shoulder(.R1, label: "R1", unit: unit)
                    .position(x: width * 0.71, y: height * 0.19)
                shoulder(.R2, label: "R2", unit: unit)
                    .position(x: width * 0.84, y: height * 0.19)

                dpad(unit: unit)
                    .position(x: width * 0.20, y: height * 0.57)

                padButton(.select, label: "SELECT", size: unit * 0.80)
                    .position(x: width * 0.45, y: height * 0.43)
                padButton(.start, label: "START", size: unit * 0.80)
                    .position(x: width * 0.55, y: height * 0.43)

                analogStick(isLeft: true, unit: unit)
                    .position(x: width * 0.40, y: height * 0.73)
                analogStick(isLeft: false, unit: unit)
                    .position(x: width * 0.60, y: height * 0.73)

                faceButtons(unit: unit)
                    .position(x: width * 0.80, y: height * 0.57)
            }
            .frame(width: width, height: height)
        }
        .padding(8)
    }

    private func shoulder(
        _ button: ARMSX2PadButton,
        label: String,
        unit: CGFloat
    ) -> some View {
        padButton(button, label: label, size: unit * 1.15)
            .frame(height: unit * 0.60)
    }

    private func dpad(unit: CGFloat) -> some View {
        ZStack {
            padButton(.up, label: "▲", size: unit)
                .offset(y: -unit * 0.52)
            padButton(.down, label: "▼", size: unit)
                .offset(y: unit * 0.52)
            padButton(.left, label: "◀", size: unit)
                .offset(x: -unit * 0.52)
            padButton(.right, label: "▶", size: unit)
                .offset(x: unit * 0.52)
        }
        .frame(width: unit * 2.1, height: unit * 2.1)
    }

    private func faceButtons(unit: CGFloat) -> some View {
        ZStack {
            padButton(.triangle, label: "△", size: unit)
                .offset(y: -unit * 0.52)
            padButton(.cross, label: "×", size: unit)
                .offset(y: unit * 0.52)
            padButton(.square, label: "□", size: unit)
                .offset(x: -unit * 0.52)
            padButton(.circle, label: "○", size: unit)
                .offset(x: unit * 0.52)
        }
        .frame(width: unit * 2.1, height: unit * 2.1)
    }

    private func analogStick(isLeft: Bool, unit: CGFloat) -> some View {
        let baseName = ControllerAsset.analogBaseFileName(
            isLeft: isLeft,
            descriptor: descriptor
        )
        let stickName = ControllerAsset.analogStickFileName(
            isLeft: isLeft,
            descriptor: descriptor
        )
        return ZStack {
            controllerImage(
                fileName: baseName,
                fallback: "",
                size: unit * 1.52
            )
            controllerImage(
                fileName: stickName,
                fallback: isLeft ? "L3" : "R3",
                size: unit * 0.86
            )
        }
        .frame(width: unit * 1.55, height: unit * 1.55)
    }

    private func padButton(
        _ button: ARMSX2PadButton,
        label: String,
        size: CGFloat
    ) -> some View {
        controllerImage(
            fileName: ControllerAsset.fileName(for: button),
            fallback: label,
            size: size
        )
    }

    private func controllerImage(
        fileName: String,
        fallback: String,
        size: CGFloat
    ) -> some View {
        ControllerAssetImage(
            fileName: fileName,
            fallback: fallback,
            fallbackColor: .white,
            fallbackFontSize: max(9, size * 0.24),
            skin: descriptor.virtualPadSkin,
            descriptor: descriptor
        )
        .frame(width: size, height: size)
    }
}
