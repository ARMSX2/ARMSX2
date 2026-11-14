// NativeMenuTabSwipeRecognizer.swift — horizontal swipe between menu tabs
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

#if !targetEnvironment(macCatalyst)
/// Recognizes horizontal tab swipes without cancelling touches in the active
/// List. Sliders, the horizontal cover flow, and navigation-edge gestures keep
/// ownership of their own drags.
@MainActor
struct NativeMenuTabSwipeRecognizer: UIViewRepresentable {
    let isEnabled: Bool
    let onSwipe: (CGFloat) -> Void

    func makeUIView(context: Context) -> InstallerView {
        let view = InstallerView()
        view.onSwipe = onSwipe
        view.setEnabled(isEnabled)
        return view
    }

    func updateUIView(_ uiView: InstallerView, context: Context) {
        uiView.onSwipe = onSwipe
        uiView.setEnabled(isEnabled)
    }

    static func dismantleUIView(_ uiView: InstallerView, coordinator: ()) {
        uiView.uninstall()
    }

    final class InstallerView: UIView, UIGestureRecognizerDelegate {
        var onSwipe: (CGFloat) -> Void = { _ in }
        private var recognizesTabSwipes = true
        private weak var gestureHost: UIView?
        private lazy var panGesture: UIPanGestureRecognizer = {
            let gesture = UIPanGestureRecognizer(
                target: self,
                action: #selector(handlePan(_:))
            )
            gesture.cancelsTouchesInView = false
            gesture.delaysTouchesBegan = false
            gesture.delaysTouchesEnded = false
            gesture.delegate = self
            return gesture
        }()

        override func didMoveToWindow() {
            super.didMoveToWindow()
            install(on: window == nil ? nil : nearestViewController()?.view ?? window)
        }

        func uninstall() {
            gestureHost?.removeGestureRecognizer(panGesture)
            gestureHost = nil
        }

        func setEnabled(_ enabled: Bool) {
            guard recognizesTabSwipes != enabled else { return }
            recognizesTabSwipes = enabled
            panGesture.isEnabled = enabled
        }

        private func install(on host: UIView?) {
            guard gestureHost !== host else { return }
            uninstall()
            guard let host else { return }
            host.addGestureRecognizer(panGesture)
            gestureHost = host
        }

        @objc
        private func handlePan(_ gesture: UIPanGestureRecognizer) {
            guard gesture.state == .ended, let gestureHost else { return }
            let translation = gesture.translation(in: gestureHost)
            let velocity = gesture.velocity(in: gestureHost)
            let projectedTranslation = translation.x + velocity.x * 0.12
            let usesDirectTranslation = abs(translation.x) >= 64
            let direction = usesDirectTranslation ? translation.x : projectedTranslation
            guard abs(direction) >= (usesDirectTranslation ? 64 : 96) else { return }
            onSwipe(direction)
        }

        override func gestureRecognizerShouldBegin(
            _ gestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            guard recognizesTabSwipes,
                  let pan = gestureRecognizer as? UIPanGestureRecognizer,
                  let gestureHost else {
                return false
            }
            let velocity = pan.velocity(in: gestureHost)
            return abs(velocity.x) > abs(velocity.y) * 1.25
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            guard let gestureHost,
                  let touchView = touch.view,
                  touchView === gestureHost || touchView.isDescendant(of: gestureHost) else {
                return false
            }

            let location = touch.location(in: gestureHost)
            let protectedLeadingEdge = gestureHost.safeAreaInsets.left + 24
            let protectedTrailingEdge = gestureHost.bounds.width
                - gestureHost.safeAreaInsets.right
                - 24
            guard location.x > protectedLeadingEdge,
                  location.x < protectedTrailingEdge else {
                return false
            }

            var touchedView: UIView? = touchView
            while let view = touchedView, view !== gestureHost {
                if view is UIControl || view is UITabBar {
                    return false
                }
                if let scrollView = view as? UIScrollView,
                   scrollView.isScrollEnabled,
                   (
                       scrollView.alwaysBounceHorizontal
                           || scrollView.contentSize.width > scrollView.bounds.width + 24
                   ) {
                    return false
                }
                touchedView = view.superview
            }
            return true
        }

        private func nearestViewController() -> UIViewController? {
            var responder: UIResponder? = self
            while let current = responder {
                if let controller = current as? UIViewController {
                    return controller
                }
                responder = current.next
            }
            return nil
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            true
        }
    }
}
#endif
