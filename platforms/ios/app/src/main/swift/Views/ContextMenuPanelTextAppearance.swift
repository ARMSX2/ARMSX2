// SPDX-License-Identifier: GPL-3.0+
import SwiftUI

/// Dark command decks share the context menu's surface palette,
/// not the text colours chosen for the dynamic background beneath the deck.
private struct ContextMenuPanelTextAppearance: ViewModifier {
    @Environment(\.controllerTextAppearance) private var inheritedAppearance
    @Environment(\.uiContextMenuColour) private var primary
    @Environment(\.uiContextMenuSecondaryColour) private var secondary
    @Environment(\.uiContextMenuFocusedColour) private var focused

    func body(content: Content) -> some View {
        var appearance = inheritedAppearance
        appearance.normalColor = primary
        appearance.titleColor = primary
        appearance.contentColor = primary
        appearance.secondaryColor = secondary
        appearance.unselectedColor = secondary
        appearance.focusedColor = focused
        return content
            .environment(\.controllerTextAppearance, appearance)
            .environment(\.uiTitleTextColour, primary)
            .environment(\.uiContentTextColour, primary)
            .environment(\.uiSecondaryTextColour, secondary)
            .foregroundStyle(primary, secondary)
    }
}

extension View {
    func contextMenuPanelTextAppearance() -> some View {
        modifier(ContextMenuPanelTextAppearance())
    }
}
