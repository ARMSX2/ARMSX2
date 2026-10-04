// PerGameTab.swift — Container for a per-game settings tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PerGameTab<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    var body: some View {
        Form {
            content
        }
        .perGameRightStickScroll()
        .scrollContentBackground(.hidden)
        .background(Color.clear)
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(.hidden, for: .navigationBar)
    }
}

extension View {
    /// The right stick scrolls a per-game tab's Form, which every tab needs on its own.
    func perGameRightStickScroll() -> some View {
        modifier(PerGameRightStickScroll())
    }
}

private struct PerGameRightStickScroll: ViewModifier {
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.controllerAccessibilityTargetsSuppressed)
    private var controllerTargetsSuppressed

    func body(content: Content) -> some View {
        content.background {
            // Keep the analog-scroll owner alive independently of lazy Form
            // cells. Its full-pane bounds let the local UIKit lookup resolve
            // this Form's scroll view without scanning the entire window.
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 220,
                isEnabled: !controllerTargetsSuppressed,
                searchesNearbyScrollViews: true
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
        }
    }
}
