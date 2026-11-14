// GameLibraryLongPressObservers.swift — The library's touch and card long press recognizers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// A single scroll-level long press for touch browsing. Card geometry already
/// exists for the launch transition, so resolving the touched ID here avoids
/// continuously adding and removing recognizers as lazy cards recycle.
@MainActor
struct GameLibraryTouchLongPressObserver: UIViewRepresentable {
    let registry: GameplayLaunchCardRegistry
    let onTouchBegan: () -> Void
    let onRecognized: (String) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(
            registry: registry,
            onTouchBegan: onTouchBegan,
            onRecognized: onRecognized
        )
    }

    func makeUIView(context: Context) -> MarkerView {
        let view = MarkerView()
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        view.coordinator = context.coordinator
        return view
    }

    func updateUIView(_ uiView: MarkerView, context: Context) {
        context.coordinator.registry = registry
        context.coordinator.onTouchBegan = onTouchBegan
        context.coordinator.onRecognized = onRecognized
        context.coordinator.installIfNeeded(from: uiView)
    }

    static func dismantleUIView(
        _ uiView: MarkerView,
        coordinator: Coordinator
    ) {
        coordinator.uninstall()
        uiView.coordinator = nil
    }

    final class MarkerView: UIView {
        weak var coordinator: Coordinator?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            coordinator?.installIfNeeded(from: self)
        }

        override func didMoveToSuperview() {
            super.didMoveToSuperview()
            coordinator?.installIfNeeded(from: self)
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var registry: GameplayLaunchCardRegistry
        var onTouchBegan: () -> Void
        var onRecognized: (String) -> Void
        private weak var scrollView: UIScrollView?
        private var pendingGameID: String?
        private lazy var recognizer: UILongPressGestureRecognizer = {
            let recognizer = UILongPressGestureRecognizer(
                target: self,
                action: #selector(handleLongPress(_:))
            )
            recognizer.minimumPressDuration = 0.42
            recognizer.allowableMovement = 10
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            recognizer.delegate = self
            return recognizer
        }()

        init(
            registry: GameplayLaunchCardRegistry,
            onTouchBegan: @escaping () -> Void,
            onRecognized: @escaping (String) -> Void
        ) {
            self.registry = registry
            self.onTouchBegan = onTouchBegan
            self.onRecognized = onRecognized
        }

        func installIfNeeded(from markerView: MarkerView) {
            var ancestor = markerView.superview
            var enclosingScrollView: UIScrollView?
            while let view = ancestor {
                if let candidate = view as? UIScrollView {
                    enclosingScrollView = candidate
                    break
                }
                ancestor = view.superview
            }
            guard scrollView !== enclosingScrollView else { return }
            uninstall()
            guard let enclosingScrollView else { return }
            enclosingScrollView.addGestureRecognizer(recognizer)
            scrollView = enclosingScrollView
        }

        func uninstall() {
            scrollView?.removeGestureRecognizer(recognizer)
            scrollView = nil
            pendingGameID = nil
        }

        @objc
        private func handleLongPress(_ recognizer: UILongPressGestureRecognizer) {
            guard recognizer.state == .began,
                  let pendingGameID else { return }
            onRecognized(pendingGameID)
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            guard let scrollView else { return false }
            let location = touch.location(in: scrollView)
            pendingGameID = registry.gameID(at: location, in: scrollView)
            guard pendingGameID != nil else { return false }
            onTouchBegan()
            return true
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer:
                UIGestureRecognizer
        ) -> Bool {
            true
        }
    }
}

/// Observes a stationary hold from the card's enclosing native scroll view.
/// Keeping the recognizer outside SwiftUI's card gesture arena lets a drag
/// that begins on the cover remain owned by List/ScrollView, including its
/// native deceleration and edge behavior.
@MainActor
struct GameLibraryCardLongPressObserver: UIViewRepresentable {
    let onTouchBegan: () -> Void
    let onRecognized: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onTouchBegan: onTouchBegan, onRecognized: onRecognized)
    }

    func makeUIView(context: Context) -> MarkerView {
        let view = MarkerView()
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        view.coordinator = context.coordinator
        context.coordinator.markerView = view
        return view
    }

    func updateUIView(_ uiView: MarkerView, context: Context) {
        context.coordinator.onTouchBegan = onTouchBegan
        context.coordinator.onRecognized = onRecognized
        context.coordinator.installIfNeeded(from: uiView)
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: MarkerView,
        context: Context
    ) -> CGSize? {
        guard let width = proposal.width, let height = proposal.height else {
            return nil
        }
        return CGSize(width: width, height: height)
    }

    static func dismantleUIView(
        _ uiView: MarkerView,
        coordinator: Coordinator
    ) {
        coordinator.uninstall()
        uiView.coordinator = nil
    }

    final class MarkerView: UIView {
        weak var coordinator: Coordinator?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            coordinator?.installIfNeeded(from: self)
        }

        override func didMoveToSuperview() {
            super.didMoveToSuperview()
            coordinator?.installIfNeeded(from: self)
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var onTouchBegan: () -> Void
        var onRecognized: () -> Void
        weak var markerView: MarkerView?
        private weak var scrollView: UIScrollView?
        private lazy var recognizer: UILongPressGestureRecognizer = {
            let recognizer = UILongPressGestureRecognizer(
                target: self,
                action: #selector(handleLongPress(_:))
            )
            recognizer.minimumPressDuration = 0.42
            recognizer.allowableMovement = 10
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            recognizer.delegate = self
            return recognizer
        }()

        init(
            onTouchBegan: @escaping () -> Void,
            onRecognized: @escaping () -> Void
        ) {
            self.onTouchBegan = onTouchBegan
            self.onRecognized = onRecognized
        }

        func installIfNeeded(from markerView: MarkerView) {
            var ancestor = markerView.superview
            var enclosingScrollView: UIScrollView?
            while let view = ancestor {
                if let candidate = view as? UIScrollView {
                    enclosingScrollView = candidate
                    break
                }
                ancestor = view.superview
            }
            guard scrollView !== enclosingScrollView else {
                self.markerView = markerView
                return
            }
            uninstall()
            // `uninstall()` clears the old weak marker. Publish the current
            // card only after teardown so the first touch-down can claim this
            // gesture and suppress the backdrop L2/R2 shortcut.
            self.markerView = markerView
            guard let enclosingScrollView else { return }
            enclosingScrollView.addGestureRecognizer(recognizer)
            scrollView = enclosingScrollView
        }

        func uninstall() {
            scrollView?.removeGestureRecognizer(recognizer)
            scrollView = nil
            markerView = nil
        }

        @objc
        private func handleLongPress(_ recognizer: UILongPressGestureRecognizer) {
            guard recognizer.state == .began else { return }
            onRecognized()
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            guard let markerView,
                  markerView.window != nil,
                  !markerView.isHidden,
                  markerView.alpha >= 0.01,
                  let scrollView else { return false }
            let location = touch.location(in: scrollView)
            let markerLocation = markerView.convert(location, from: scrollView)
            let touchesCard = markerView.bounds.contains(markerLocation)
            if touchesCard {
                onTouchBegan()
            }
            return touchesCard
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer:
                UIGestureRecognizer
        ) -> Bool {
            true
        }
    }
}

extension View {
    @MainActor
    func observeGameLibraryCardLongPress(
        onTouchBegan: @escaping () -> Void,
        _ onRecognized: @escaping () -> Void
    ) -> some View {
        background {
            GameLibraryCardLongPressObserver(
                onTouchBegan: onTouchBegan,
                onRecognized: onRecognized
            )
        }
    }
}
