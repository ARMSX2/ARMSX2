// CheatsPatchesSession.swift — Cheats panel state that outlives one presentation
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// Held by the in-game caller, since iPhone Duo's fold swaps the sheet for a hosted panel.
@MainActor
final class CheatsPatchesSession {
    var patchSourcesDraft: [String]?
    var cheatSourcesDraft: [String]?
    var showAdvanced = false
    var scrollShare: CGFloat?
    var restoreShare: CGFloat?
    var controllerTarget = CheatsPatchesManagerView.ControllerTarget.done
    var shownBy: UUID?
}

extension View {
    @ViewBuilder
    func cheatsScrollMemory(_ session: CheatsPatchesSession?, instance: UUID) -> some View {
        if #available(iOS 18.0, *), let session {
            modifier(CheatsScrollMemory(session: session, instance: instance))
        } else {
            self
        }
    }
}

// The sheet and the panel beside the fold differ in width, so the place is kept as a fraction of the scroll range.
@available(iOS 18.0, *)
private struct CheatsScrollMemory: ViewModifier {
    let session: CheatsPatchesSession
    let instance: UUID

    func body(content: Content) -> some View {
        content.onScrollGeometryChange(for: CGFloat.self, of: { geometry in
            let range = geometry.contentSize.height + geometry.contentInsets.top
                + geometry.contentInsets.bottom - geometry.containerSize.height
            return range > 0 ? (geometry.contentOffset.y + geometry.contentInsets.top) / range : 0
        }) { _, share in
            guard session.shownBy == instance else { return }
            session.scrollShare = share
        }
    }
}

// SwiftUI cannot move a Form to an offset, so this marker in the first row moves its collection view.
struct CheatsScrollRestore: UIViewRepresentable {
    let session: CheatsPatchesSession

    func makeUIView(context: Context) -> Marker { Marker(session: session) }
    func updateUIView(_ marker: Marker, context: Context) {}

    final class Marker: UIView {
        private let session: CheatsPatchesSession

        init(session: CheatsPatchesSession) {
            self.session = session
            super.init(frame: .zero)
            isUserInteractionEnabled = false
        }

        required init?(coder: NSCoder) { nil }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            guard window != nil else { return }
            // By the next turn the swapped-in view has taken the share and sized its rows.
            DispatchQueue.main.async { [weak self] in self?.restore() }
        }

        private func restore() {
            guard let share = session.restoreShare else { return }
            var ancestor = superview
            while let view = ancestor, !(view is UIScrollView) { ancestor = view.superview }
            guard let list = ancestor as? UIScrollView else { return }
            session.restoreShare = nil
            // Rows size as they come into view and change the range, so settle in a few passes.
            for _ in 0..<3 {
                let inset = list.adjustedContentInset
                let range = list.contentSize.height + inset.top + inset.bottom - list.bounds.height
                list.setContentOffset(CGPoint(x: list.contentOffset.x, y: share * max(range, 0) - inset.top), animated: false)
                list.layoutIfNeeded()
            }
        }
    }
}
