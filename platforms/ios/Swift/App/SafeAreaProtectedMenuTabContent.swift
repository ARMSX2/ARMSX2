// SafeAreaProtectedMenuTabContent.swift — landscape insets for menu tab pages
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

@MainActor
struct SafeAreaProtectedMenuTabContent<Content: View>: View {
    @Environment(\.layoutDirection) private var layoutDirection
    @State private var safeAreaInsets = KeyWindowSafeArea.horizontalInsets()
    let appliesLegacyLandscapeInsets: Bool
    let content: Content

    init(
        appliesLegacyLandscapeInsets: Bool = true,
        @ViewBuilder content: () -> Content
    ) {
        self.appliesLegacyLandscapeInsets = appliesLegacyLandscapeInsets
        self.content = content()
    }

    var body: some View {
        // Before iOS 26 a landscape TabView page gets no side inset, so pad from the key window.
        // One tree for every theme: swapping branches tore down the Settings navigation stack.
        GeometryReader { geometry in
            // A keyboard shrinks the safe region, so landscape is judged from the window.
            let isLandscapePhone = geometry.size.width + geometry.safeAreaInsets.leading + geometry.safeAreaInsets.trailing
                > geometry.size.height + geometry.safeAreaInsets.top + geometry.safeAreaInsets.bottom
                && UIDevice.current.userInterfaceIdiom == .phone
            let systemProvidesInset = geometry.safeAreaInsets.leading > 0
                || geometry.safeAreaInsets.trailing > 0
            let insets: (left: CGFloat, right: CGFloat) = {
                guard appliesLegacyLandscapeInsets,
                      isLandscapePhone,
                      !systemProvidesInset else {
                    return (0, 0)
                }
                return NormalTabContentMargin.effectiveHorizontalInsets(
                    raw: safeAreaInsets,
                    isLandscape: true,
                    idiom: .phone
                )
            }()
            content
                .padding(.leading, layoutDirection == .rightToLeft ? insets.right : insets.left)
                .padding(.trailing, layoutDirection == .rightToLeft ? insets.left : insets.right)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .onChange(of: geometry.size) { _, _ in
                    safeAreaInsets = KeyWindowSafeArea.horizontalInsets()
                }
        }
        .onAppear {
            safeAreaInsets = KeyWindowSafeArea.horizontalInsets()
        }
    }
}

/// The key window's side insets, since SwiftUI's are unreliable inside a TabView page.
private enum KeyWindowSafeArea {
    @MainActor
    static func horizontalInsets() -> (left: CGFloat, right: CGFloat) {
        let insets = keyWindowInsets()
        return (insets.left, insets.right)
    }

    @MainActor
    private static func keyWindowInsets() -> UIEdgeInsets {
        let windows = UIApplication.shared.appWindowScene?.windows ?? []
        return (windows.first(where: { $0.isKeyWindow }) ?? windows.first)?.safeAreaInsets ?? .zero
    }
}

/// A little more side margin than the notch inset for tab pages in iPhone landscape, before iOS 26.
private enum NormalTabContentMargin {
    /// Minimum horizontal content margin for normal app tabs on an iPhone in landscape.
    static let minimumLandscapeMargin: CGFloat = 20

    static func effectiveHorizontalInsets(
        raw: (left: CGFloat, right: CGFloat),
        isLandscape: Bool,
        idiom: UIUserInterfaceIdiom
    ) -> (left: CGFloat, right: CGFloat) {
        guard isLandscape, idiom == .phone else { return raw }
        return (max(raw.left, minimumLandscapeMargin), max(raw.right, minimumLandscapeMargin))
    }
}
