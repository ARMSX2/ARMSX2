// QuickMenuView+Report.swift — The pause menu's Report a Problem row
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension QuickMenuView {
    @ViewBuilder var reportProblemRow: some View {
        OverlayActionRow(
            controllerNavigationID: QuickMenuNavigationGraph.ControllerTargetID.reportProblem,
            label: settings.localized("Report a Problem"),
            systemImage: "exclamationmark.bubble"
        ) {
            onOpen(.reportProblem)
        }
        .accessibilityHint(
            settings.localized(
                "Send the logs and a note about what went wrong in this session"
            )
        )
    }
}
