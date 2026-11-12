// ControllerNavigationAlert.swift — controller navigation alert
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Foundation

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
        .controllerFocusBoxPresentation(isVisible: isSelected, cornerRadius: 11)
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

                // Keep ordinary confirmations at their intrinsic height; a greedy
                // ScrollView fallback made one-line alerts nearly fullscreen.
                // The preview chooser is dense enough to fit without scrolling.
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
                // Let confirmations hug their contents in every orientation; the
                // skin chooser keeps its two-column width but no longer stretches
                // into unused vertical space.
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
        .accessibilityAddTraits(.isModal)
        .accessibilityAction(.escape) {
            controllerInput?.playFeedback(.back)
            onDismiss()
        }
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
        // An installed descriptor is what gameplay renders, so prefer it over a
        // catalogue thumbnail: cycling the picker then shows bundled and imported
        // skins at once, without waiting for remote artwork.
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
            // A manual picker can gain its preview URL after the catalog loads and
            // cycling reuses this view slot, so key the loader to the artwork.
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
                            // Keep Apple's 44-point minimum without the oversized panels a
                            // 48/52-point row produced; the Button stays full width.
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
                                    .stroke(buttonTint.opacity(0.48), lineWidth: 0.8)
                                }
                            }
                            .controllerFocusBoxPresentation(isVisible: isSelected, cornerRadius: 11)
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
