// PerGameTab.swift — Container for a per-game settings tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PerGameTab<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.controllerAccessibilityTargetsSuppressed)
    private var controllerTargetsSuppressed

    var body: some View {
        Form {
            content
        }
        .background {
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
        .scrollContentBackground(.hidden)
        .background(Color.clear)
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(.hidden, for: .navigationBar)
    }
}
