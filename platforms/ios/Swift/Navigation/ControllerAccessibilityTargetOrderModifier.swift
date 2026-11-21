// ControllerAccessibilityTargetOrderModifier.swift — target order and target ID modifiers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ControllerAccessibilityTargetOrderModifier: ViewModifier {
    let navigationIDs: [String]
    let links: [ControllerAccessibilityDirectionalLink]
    @Environment(\.controllerAccessibilityNavigationSession) private var session
    @Environment(\.controllerAccessibilityNavigationActive) private var isActive
    @Environment(\.controllerAccessibilityTargetsSuppressed) private var isSuppressed

    func body(content: Content) -> some View {
        content
            .onAppear { publishOrderIfActive() }
            .onChange(of: navigationIDs) { _, value in
                if isActive, !isSuppressed, session?.ownerDeclaresOrder != true {
                    session?.setDeclaredOrder(value)
                    session?.setPageDirectionalLinks(links)
                }
            }
            .onChange(of: isActive) { _, _ in publishOrderIfActive() }
            .onChange(of: isSuppressed) { _, _ in publishOrderIfActive() }
            .onChange(of: session?.scopeRevision) { _, _ in publishOrderIfActive() }
    }

    private func publishOrderIfActive() {
        // A page sliding out still sees the scope change and would replace
        // the order of the screen it uncovers.
        guard isActive, session?.ownerDeclaresOrder != true else { return }
        session?.setDeclaredOrder(isSuppressed ? [] : navigationIDs)
        session?.setPageDirectionalLinks(isSuppressed ? [] : links)
    }
}

private struct ControllerAccessibilityTargetIDModifier: ViewModifier {
    let navigationID: String

    func body(content: Content) -> some View {
        let normalizedID = navigationID.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        content
            .environment(
                \.controllerAccessibilityInheritedTargetID,
                normalizedID
            )
            .id(ControllerAccessibilityNavigationSession.explicitKey(normalizedID))
    }
}

extension View {
    func controllerAccessibilityTargetOrder(
        _ navigationIDs: [String],
        links: [ControllerAccessibilityDirectionalLink] = []
    ) -> some View {
        modifier(
            ControllerAccessibilityTargetOrderModifier(
                navigationIDs: navigationIDs,
                links: links
            )
        )
    }

    func controllerAccessibilityTargetID(_ navigationID: String) -> some View {
        modifier(
            ControllerAccessibilityTargetIDModifier(
                navigationID: navigationID
            )
        )
    }
}
