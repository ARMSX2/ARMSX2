// NativeMenuTabBar.swift — UIKit tab bar as a SwiftUI view
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

#if !targetEnvironment(macCatalyst)
extension UIWindow {
    /// Leaves out a one-sided inset such as iPhone Duo's status column; mirrored insets cancel.
    var symmetricSafeWidth: CGFloat {
        bounds.width - abs(safeAreaInsets.left - safeAreaInsets.right)
    }
}

/// A real UIKit tab bar kept outside every page's GlassEffectContainer.
/// This gives the bar its native system appearance while guaranteeing that it
/// remains a foreground sibling rather than being composited below tab content.
struct NativeMenuTabBar: UIViewRepresentable {
    @Binding var selection: Int
    let controllerFocusedIndex: Int?
    @Binding var controllerFocusFrame: CGRect?
    let controllerNavigationEnabled: Bool
    let accentColor: UIColor
    let secondaryTextColor: UIColor
    let isCompactHeight: Bool
    let layout: MenuBottomTabBarLayout
    let titles: [String]
    let onReselect: (Int) -> Void

    private let systemImages = [
        "gamecontroller",
        "cpu",
        "gearshape",
    ]

    func makeCoordinator() -> Coordinator {
        Coordinator(
            selection: $selection,
            controllerFocusFrame: $controllerFocusFrame,
            onReselect: onReselect
        )
    }

    func makeUIView(context: Context) -> LegacyCompatibleMenuTabBar {
        let tabBar = LegacyCompatibleMenuTabBar()
        tabBar.delegate = context.coordinator
        if controllerNavigationEnabled {
            tabBar.onControllerFocusFrameChanged =
                context.coordinator.publishControllerFocusFrame
        } else {
            tabBar.onControllerFocusFrameChanged = nil
        }
        tabBar.controllerNavigationEnabled = controllerNavigationEnabled
        configureAppearance(of: tabBar)
        tabBar.items = makeItems()
        tabBar.configurationKey = configurationKey
        if let items = tabBar.items, items.indices.contains(selection) {
            tabBar.selectedItem = items[selection]
        }
        tabBar.controllerFocusedIndex = controllerNavigationEnabled
            ? controllerFocusedIndex
            : nil
        if controllerNavigationEnabled {
            applyControllerFocusColors(to: tabBar)
        }
        return tabBar
    }

    func updateUIView(
        _ tabBar: LegacyCompatibleMenuTabBar,
        context: Context
    ) {
        context.coordinator.selection = $selection
        context.coordinator.controllerFocusFrame = $controllerFocusFrame
        context.coordinator.onReselect = onReselect
        if controllerNavigationEnabled {
            tabBar.onControllerFocusFrameChanged =
                context.coordinator.publishControllerFocusFrame
        } else {
            tabBar.onControllerFocusFrameChanged = nil
        }
        tabBar.controllerNavigationEnabled = controllerNavigationEnabled
        if tabBar.configurationKey != configurationKey {
            configureAppearance(of: tabBar)
            tabBar.items = makeItems()
            tabBar.configurationKey = configurationKey
        }

        // Keep the same UITabBar and item identities, but do not morph its
        // selection glass between tabs. The tab bar remains geometrically
        // stable while title/top-toolbar transitions animate independently.
        if controllerNavigationEnabled {
            UIView.performWithoutAnimation {
                if let items = tabBar.items,
                   items.indices.contains(selection) {
                    tabBar.selectedItem = items[selection]
                }
                tabBar.layoutIfNeeded()
            }
        } else {
            if let items = tabBar.items,
               items.indices.contains(selection) {
                tabBar.selectedItem = items[selection]
            }
        }
        tabBar.controllerFocusedIndex = controllerNavigationEnabled
            ? controllerFocusedIndex
            : nil
        if controllerNavigationEnabled {
            applyControllerFocusColors(to: tabBar)
        }
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: LegacyCompatibleMenuTabBar,
        context: Context
    ) -> CGSize? {
        // The window width wins: a nil proposal after a stop narrowed the bar, old bounds kept landscape.
        let width = uiView.resolvedMeasurementWidth(
            windowWidth: uiView.window?.symmetricSafeWidth ?? 0,
            containerWidth: uiView.superview?.bounds.width ?? 0,
            boundsWidth: uiView.bounds.width,
            proposalWidth: proposal.width ?? 0
        )
        return CGSize(width: width, height: layout.height)
    }

    private var configurationKey: String {
        "\(controllerNavigationEnabled)|\(isCompactHeight)|\(accentColor)|\(secondaryTextColor)|\(layout.height)|\(layout.iconScale)|\(layout.labelScale)|\(layout.showsLabels)|\(titles.joined(separator: "\u{1F}"))"
    }

    private func makeItems() -> [UITabBarItem] {
        systemImages.indices.map { index in
            let item = UITabBarItem(
                title: layout.showsLabels && titles.indices.contains(index)
                    ? titles[index]
                    : nil,
                image: itemImage(at: index),
                tag: index
            )
            item.accessibilityLabel = titles.indices.contains(index)
                ? titles[index]
                : nil
            return item
        }
    }

    private func itemImage(at index: Int) -> UIImage? {
        let configuration = UIImage.SymbolConfiguration(
            pointSize: (isCompactHeight ? 25 : 23) * layout.iconScale,
            weight: .regular
        )
        return UIImage(
            systemName: systemImages[index],
            withConfiguration: configuration
        )
    }

    private func applyControllerFocusColors(
        to tabBar: LegacyCompatibleMenuTabBar
    ) {
        guard let items = tabBar.items else { return }
        for index in items.indices {
            let isControllerFocused = controllerFocusedIndex == index
            let baseImage = itemImage(at: index)
            if isControllerFocused {
                let focusedImage = baseImage?.withTintColor(
                    accentColor,
                    renderingMode: .alwaysOriginal
                )
                items[index].image = focusedImage
                items[index].selectedImage = focusedImage
            } else {
                items[index].image = baseImage
                items[index].selectedImage = baseImage
            }

            items[index].setTitleTextAttributes(
                [.foregroundColor: isControllerFocused
                    ? accentColor
                    : secondaryTextColor],
                for: .normal
            )
            items[index].setTitleTextAttributes(
                [.foregroundColor: accentColor],
                for: .selected
            )
        }
    }

    private func configureAppearance(of tabBar: LegacyCompatibleMenuTabBar) {
        tabBar.tintColor = accentColor
        tabBar.unselectedItemTintColor = secondaryTextColor
        tabBar.isTranslucent = true
        tabBar.isOpaque = false
        tabBar.backgroundColor = .clear
        tabBar.backgroundImage = UIImage()
        tabBar.shadowImage = UIImage()

        let appearance = tabBar.standardAppearance.copy()
        appearance.configureWithTransparentBackground()
        appearance.backgroundColor = .clear
        appearance.backgroundEffect = nil
        appearance.shadowColor = .clear

        let allLayouts = [
            appearance.stackedLayoutAppearance,
            appearance.inlineLayoutAppearance,
            appearance.compactInlineLayoutAppearance,
        ]
        for layout in allLayouts {
            layout.normal.iconColor = secondaryTextColor
            layout.normal.titleTextAttributes[.foregroundColor] =
                secondaryTextColor
            layout.selected.iconColor = accentColor
            layout.selected.titleTextAttributes[.foregroundColor] =
                accentColor
            let baseSize: CGFloat = isCompactHeight ? 13 : 10
            layout.normal.titleTextAttributes[.font] = UIFont.systemFont(
                ofSize: baseSize * self.layout.labelScale,
                weight: .regular
            )
            layout.selected.titleTextAttributes[.font] = UIFont.systemFont(
                ofSize: baseSize * self.layout.labelScale,
                weight: .semibold
            )
        }

        if #available(iOS 26.0, *) {
            // Tint UIKit's own Liquid Glass selection pill instead of replacing
            // it. Its blur, refraction, morph, and press effects remain native.
            appearance.selectionIndicatorImage = nil
            appearance.selectionIndicatorTintColor = controllerNavigationEnabled
                ? .clear
                : accentColor.withAlphaComponent(0.18)
        }
        tabBar.standardAppearance = appearance
        tabBar.scrollEdgeAppearance = appearance
    }

    final class Coordinator: NSObject, UITabBarDelegate {
        var selection: Binding<Int>
        var controllerFocusFrame: Binding<CGRect?>
        var onReselect: (Int) -> Void
        private var latestControllerFocusFrame: CGRect?

        init(
            selection: Binding<Int>,
            controllerFocusFrame: Binding<CGRect?>,
            onReselect: @escaping (Int) -> Void
        ) {
            self.selection = selection
            self.controllerFocusFrame = controllerFocusFrame
            self.onReselect = onReselect
        }

        func publishControllerFocusFrame(_ frame: CGRect?) {
            guard latestControllerFocusFrame != frame
                    || controllerFocusFrame.wrappedValue != frame else {
                return
            }
            latestControllerFocusFrame = frame

            // UIKit can report this from layoutSubviews. Publish on the next
            // main pass so SwiftUI never mutates state during representable
            // reconciliation, and discard an older queued frame if focus moved.
            DispatchQueue.main.async { [weak self] in
                guard let self,
                      self.latestControllerFocusFrame == frame,
                      self.controllerFocusFrame.wrappedValue != frame else {
                    return
                }
                self.controllerFocusFrame.wrappedValue = frame
            }
        }

        func tabBar(_ tabBar: UITabBar, didSelect item: UITabBarItem) {
            if selection.wrappedValue == item.tag {
                onReselect(item.tag)
            } else {
                selection.wrappedValue = item.tag
            }
        }
    }
}
#endif
