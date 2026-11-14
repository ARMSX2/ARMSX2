// MenuTabBar.swift — menu tab bar for touch, controller and Catalyst
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

#if !targetEnvironment(macCatalyst)
/// The tab bar is introduced with `safeAreaInset`, whose descendants do not
/// reliably publish anchor preferences back to the page overlay. Measure the
/// compact icon/title bounds here; the presentation-wide orb host consumes the
/// window-space rectangle directly, without reconstructing a second anchor.
private struct ControllerTabBarOrbOverlay: View {
    let exactFrame: CGRect?
    let segmentIndex: Int?
    let segmentCount: Int
    let segmentInset: CGFloat
    let permitsSegmentFallback: Bool
    let selection: Int
    let controllerInput: MenuControllerInputRouter
    var body: some View {
        GeometryReader { proxy in
            if controllerInput.hasConnectedController,
               controllerInput.isControllerNavigationEnabled,
               controllerInput.navigationZone == .tabBar,
               let focusedIndex = segmentIndex,
               let frame = resolvedFrame(in: proxy.size) {
                let overlayFrame = proxy.frame(in: .global)
                let windowFrame = frame.offsetBy(
                    dx: overlayFrame.minX,
                    dy: overlayFrame.minY
                )
                ZStack(alignment: .topLeading) {
                    ControllerBottomNavigationFocusGlow(cornerRadius: 18)
                        .frame(width: frame.width, height: frame.height)
                        .position(x: frame.midX, y: frame.midY)
                    Color.clear
                        .onAppear {
                            controllerInput.updateTabBarOrbFocusFrame(
                                windowFrame,
                                focusedIndex: focusedIndex,
                                selectedIndex: selection
                            )
                        }
                        .onChange(of: windowFrame) { _, next in
                            controllerInput.updateTabBarOrbFocusFrame(
                                next,
                                focusedIndex: focusedIndex,
                                selectedIndex: selection
                            )
                        }
                        .onChange(of: selection) { _, next in
                            controllerInput.updateTabBarOrbFocusFrame(
                                windowFrame,
                                focusedIndex: focusedIndex,
                                selectedIndex: next
                            )
                        }
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func resolvedFrame(in size: CGSize) -> CGRect? {
        if let exactFrame,
           !exactFrame.isNull,
           !exactFrame.isInfinite,
           exactFrame.width > 1,
           exactFrame.height > 1 {
            return compactOrbitFrame(exactFrame)
        }
        guard permitsSegmentFallback,
              let segmentIndex,
              segmentCount > 0,
              (0..<segmentCount).contains(segmentIndex) else { return nil }
        let availableWidth = max(0, size.width - (segmentInset * 2))
        let segmentWidth = availableWidth / CGFloat(segmentCount)
        return compactOrbitFrame(CGRect(
            x: segmentInset + CGFloat(segmentIndex) * segmentWidth,
            y: segmentInset,
            width: segmentWidth,
            height: max(1, size.height - (segmentInset * 2))
        ))
    }

    /// The tab item owns a wide hit target, especially on iPad, but OrbitKeys'
    /// particles circle the visible key rather than the complete grid cell.
    /// Preserve the tab center while keeping the first and last ellipses from
    /// extending beyond the Games and Settings button surfaces.
    private func compactOrbitFrame(_ frame: CGRect) -> CGRect {
        let maximumWidth: CGFloat =
            usesLargeLayout() ? 112 : 96
        let width = min(frame.width, maximumWidth)
        let height = min(frame.height, 40)
        return CGRect(
            x: frame.midX - (width / 2),
            y: frame.midY - (height / 2),
            width: width,
            height: height
        )
    }

}

/// Reads controller-only tab focus at the smallest possible observation scope.
/// Moving across the bar therefore updates only this surface instead of the
/// retained Games/BIOS/Settings hierarchy behind it.
struct ControllerFocusedMenuTabBar: View {
    @Binding var selection: Int
    let focusState: MenuControllerTabFocusState
    let controllerInput: MenuControllerInputRouter
    let layout: MenuBottomTabBarLayout
    let titles: [String]
    let onReselect: (Int) -> Void
    @State private var nativeControllerFocusFrame: CGRect?

    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.uiBottomTabBarColour) private var accentColour
    @Environment(\.uiBottomTabBarUnselectedColour) private var secondaryTextColour

    static var compactOffset: CGFloat {
        if #available(iOS 26.0, *) { return 24.5 }
        return -6
    }

    private var controllerFocusedIndex: Int? {
        controllerInput.hasConnectedController
            && controllerInput.isControllerNavigationEnabled
            && controllerInput.navigationZone == .tabBar
            ? focusState.focusedIndex
            : nil
    }

    @ViewBuilder
    var body: some View {
        Group {
            if #available(iOS 26.0, *) {
                NativeMenuTabBar(
                    selection: $selection,
                    controllerFocusedIndex: controllerFocusedIndex,
                    controllerFocusFrame: $nativeControllerFocusFrame,
                    controllerNavigationEnabled:
                        controllerInput.hasConnectedController
                            && controllerInput.isControllerNavigationEnabled,
                    accentColor: UIColor(accentColour),
                    secondaryTextColor: UIColor(secondaryTextColour),
                    isCompactHeight: verticalSizeClass == .compact,
                    layout: layout,
                    titles: titles,
                    onReselect: onReselect
                )
                .background {
                    ControllerTabBarOrbOverlay(
                        exactFrame: nativeControllerFocusFrame,
                        segmentIndex: controllerFocusedIndex,
                        segmentCount: 3,
                        segmentInset: 4,
                        permitsSegmentFallback: false,
                        selection: selection,
                        controllerInput: controllerInput
                    )
                }
                // Align the native iOS 26 tab bar with compact-height content.
                .offset(y: verticalSizeClass == .compact ? Self.compactOffset : 0)
            } else if controllerInput.hasConnectedController,
                      controllerInput.isControllerNavigationEnabled {
                LegacyGlassMenuTabBar(
                    selection: $selection,
                    controllerFocusedIndex: controllerFocusedIndex,
                    layout: layout,
                    titles: titles,
                    onReselect: onReselect
                )
                .background {
                    ControllerTabBarOrbOverlay(
                        exactFrame: nil,
                        segmentIndex: controllerFocusedIndex,
                        segmentCount: 3,
                        segmentInset: 4,
                        permitsSegmentFallback: true,
                        selection: selection,
                        controllerInput: controllerInput
                    )
                }
                // Raise the legacy compact tab bar above the home indicator.
                .offset(y: verticalSizeClass == .compact ? Self.compactOffset : 0)
            } else {
                OriginalLegacyGlassMenuTabBar(
                    selection: $selection,
                    layout: layout,
                    titles: titles,
                    onReselect: onReselect
                )
                // Raise the legacy compact tab bar above the home indicator.
                .offset(y: verticalSizeClass == .compact ? Self.compactOffset : 0)
            }
        }
        .padding(.bottom, layout.bottomClearance)
    }

}

/// iOS 17/18 does not provide the floating Liquid Glass tab bar used by iOS 26.
/// Keep one compact, orientation-independent glass capsule so landscape uses
/// the same centered icon-over-label layout and dimensions as portrait.
private struct LegacyGlassMenuTabBar: View {
    @Binding var selection: Int
    let controllerFocusedIndex: Int?
    let layout: MenuBottomTabBarLayout
    let titles: [String]
    let onReselect: (Int) -> Void
    @Environment(\.uiBottomTabBarColour) private var accentColour
    @Environment(\.uiBottomTabBarUnselectedColour) private var secondaryTextColour
    private let systemImages = [
        "gamecontroller",
        "cpu",
        "gearshape",
    ]

    private var barWidth: CGFloat {
        usesLargeLayout() ? 360 : 276
    }

    var body: some View {
        HStack(spacing: 0) {
            ForEach(systemImages.indices, id: \.self) { index in
                let controllerFocused = controllerFocusedIndex == index
                Button {
                    if selection == index {
                        onReselect(index)
                    } else {
                        selection = index
                        UISelectionFeedbackGenerator().selectionChanged()
                    }
                } label: {
                    VStack(spacing: 1) {
                        Image(systemName: systemImages[index])
                            .font(
                                .system(
                                    size: 20 * layout.iconScale,
                                    weight: .medium
                                )
                            )
                            .symbolRenderingMode(.monochrome)
                            .scaleEffect(selection == index ? 1.06 : 1)

                        if layout.showsLabels {
                            Text(
                                titles.indices.contains(index)
                                    ? titles[index]
                                    : ""
                            )
                            .font(
                                .system(
                                    size: 10 * layout.labelScale,
                                    weight: .semibold
                                )
                            )
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                        }
                    }
                    .foregroundStyle(
                        controllerFocused
                            ? accentColour
                            : secondaryTextColour
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Capsule())
                    .background {
                        ZStack {
                            if selection == index {
                                Capsule()
                                    .fill(.ultraThinMaterial)
                                    .overlay {
                                        Capsule()
                                            .stroke(
                                                Color.primary.opacity(0.14),
                                                lineWidth: 0.65
                                            )
                                    }
                            }
                        }
                    }
                }
                .buttonStyle(LegacyGlassTabButtonStyle())
                .focusEffectDisabled()
                .accessibilityLabel(
                    titles.indices.contains(index) ? titles[index] : ""
                )
                .accessibilityAddTraits(
                    selection == index ? .isSelected : []
                )
            }
        }
        .padding(4)
        .frame(width: barWidth)
        .frame(height: layout.height)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay {
            Capsule()
                .stroke(Color.primary.opacity(0.13), lineWidth: 0.65)
        }
        .shadow(color: .black.opacity(0.12), radius: 12, y: 5)
    }
}

/// The pre-controller tab bar copied from Master. Keeping it as a separate
/// type guarantees that disconnecting replaces the enhanced subtree and cannot
/// leave controller glass, colors, focus effects, or animation state behind.
private struct OriginalLegacyGlassMenuTabBar: View {
    @Binding var selection: Int
    let layout: MenuBottomTabBarLayout
    let titles: [String]
    let onReselect: (Int) -> Void
    @Namespace private var selectionNamespace
    @Environment(\.uiBottomTabBarColour) private var accentColour
    @Environment(\.uiBottomTabBarUnselectedColour) private var secondaryTextColour

    private let systemImages = [
        "gamecontroller",
        "cpu",
        "gearshape",
    ]

    private let selectionSpring = Animation.spring(
        response: 0.5,
        dampingFraction: 0.7,
        blendDuration: 0.18
    )

    var body: some View {
        HStack(spacing: 0) {
            ForEach(systemImages.indices, id: \.self) { index in
                Button {
                    if selection == index {
                        onReselect(index)
                    } else {
                        selection = index
                        UISelectionFeedbackGenerator().selectionChanged()
                    }
                } label: {
                    VStack(spacing: 1) {
                        Image(systemName: systemImages[index])
                            .font(
                                .system(
                                    size: 20 * layout.iconScale,
                                    weight: .medium
                                )
                            )
                            .symbolRenderingMode(.monochrome)
                            .scaleEffect(selection == index ? 1.06 : 1)

                        if layout.showsLabels {
                            Text(
                                titles.indices.contains(index)
                                    ? titles[index]
                                    : ""
                            )
                            .font(
                                .system(
                                    size: 10 * layout.labelScale,
                                    weight: .semibold
                                )
                            )
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                        }
                    }
                    .foregroundStyle(
                        selection == index
                            ? accentColour
                            : secondaryTextColour
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Capsule())
                    .background {
                        if selection == index {
                            Capsule()
                                .fill(.ultraThinMaterial)
                                .overlay {
                                    Capsule()
                                        .fill(accentColour.opacity(0.075))
                                }
                                .overlay {
                                    Capsule()
                                        .stroke(
                                            accentColour.opacity(0.2),
                                            lineWidth: 0.65
                                        )
                                }
                                .matchedGeometryEffect(
                                    id: "legacy.tab.selection",
                                    in: selectionNamespace
                                )
                        }
                    }
                }
                .buttonStyle(LegacyGlassTabButtonStyle())
                .accessibilityLabel(
                    titles.indices.contains(index) ? titles[index] : ""
                )
                .accessibilityAddTraits(
                    selection == index ? .isSelected : []
                )
            }
        }
        .padding(4)
        .frame(width: 276)
        .frame(height: layout.height)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay {
            Capsule()
                .stroke(Color.primary.opacity(0.13), lineWidth: 0.65)
        }
        .shadow(color: .black.opacity(0.12), radius: 12, y: 5)
        .animation(selectionSpring, value: selection)
    }
}

private struct LegacyGlassTabButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.94 : 1)
            .opacity(configuration.isPressed ? 0.78 : 1)
            .animation(
                .spring(
                    response: 0.25,
                    dampingFraction: 0.62,
                    blendDuration: 0.08
                ),
                value: configuration.isPressed
            )
    }
}
#endif

#if targetEnvironment(macCatalyst)
struct CatalystMenuTabBar: View {
    @Binding var selectedTab: Int
    @State private var settings = SettingsStore.shared

    private let tabs = [
        (0, "Games"),
        (1, "BIOS"),
        (2, "Settings"),
    ]

    var body: some View {
        HStack(spacing: 2) {
            ForEach(tabs, id: \.0) { tab in
                Button {
                    selectedTab = tab.0
                } label: {
                    Text(settings.localized(tab.1))
                        .font(.callout)
                        .fontWeight(selectedTab == tab.0 ? .semibold : .regular)
                        .foregroundStyle(.primary)
                        .frame(minWidth: 82)
                        .padding(.vertical, 6)
                        .background {
                            if selectedTab == tab.0 {
                                Capsule()
                                    .fill(Color.primary.opacity(0.12))
                            }
                        }
                }
                .buttonStyle(.plain)

                if tab.0 != tabs.last?.0 {
                    Divider()
                        .frame(height: 20)
                }
            }
        }
        .padding(4)
        .background(.regularMaterial, in: Capsule())
        .shadow(color: .black.opacity(0.08), radius: 18, y: 8)
    }
}
#endif
