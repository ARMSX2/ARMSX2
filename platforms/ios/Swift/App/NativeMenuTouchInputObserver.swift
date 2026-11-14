// NativeMenuTouchInputObserver.swift — touch observer under the menu
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

#if !targetEnvironment(macCatalyst)
/// Observes touch-down without delaying or cancelling the native control or
/// scroll gesture underneath it. Controller focus remains disengaged until a
/// new physical controller event arrives.
@MainActor
struct NativeMenuTouchInputObserver: UIViewRepresentable {
    let observesTouchActions: Bool
    let blocksUnderlyingTouches: Bool
    let onTouch: () -> Void
    let onTap: () -> Void
    let onBack: () -> Void
    let onToggle: (Bool) -> Void
    let onContextMenu: () -> Void
    let onEmptySpaceLongPress: (Int) -> Void

    func makeUIView(context: Context) -> InstallerView {
        let view = InstallerView()
        view.observesTouchActions = observesTouchActions
        view.blocksUnderlyingTouches = blocksUnderlyingTouches
        view.onTouch = onTouch
        view.onTap = onTap
        view.onBack = onBack
        view.onToggle = onToggle
        view.onContextMenu = onContextMenu
        view.onEmptySpaceLongPress = onEmptySpaceLongPress
        return view
    }

    func updateUIView(_ uiView: InstallerView, context: Context) {
        uiView.observesTouchActions = observesTouchActions
        uiView.blocksUnderlyingTouches = blocksUnderlyingTouches
        uiView.onTouch = onTouch
        uiView.onTap = onTap
        uiView.onBack = onBack
        uiView.onToggle = onToggle
        uiView.onContextMenu = onContextMenu
        uiView.onEmptySpaceLongPress = onEmptySpaceLongPress
    }

    static func dismantleUIView(_ uiView: InstallerView, coordinator: ()) {
        uiView.uninstall()
    }

    final class InstallerView: UIView {
        var observesTouchActions = true {
            didSet {
                guard oldValue != observesTouchActions else { return }
                let host = gestureHost
                uninstall()
                install(on: host)
            }
        }
        var blocksUnderlyingTouches = false {
            didSet {
                touchDownRecognizer.blocksUnderlyingTouches = blocksUnderlyingTouches
            }
        }
        var onTouch: () -> Void = {}
        var onTap: () -> Void = {}
        var onBack: () -> Void = {}
        var onToggle: (Bool) -> Void = { _ in }
        var onContextMenu: () -> Void = {}
        var onEmptySpaceLongPress: (Int) -> Void = { _ in }
        private weak var gestureHost: UIView?
        private var suppressesTapForCurrentLongPress = false
        private lazy var touchDownRecognizer: TouchDownRecognizer = {
            let recognizer = TouchDownRecognizer()
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            recognizer.onTouchDown = { [weak self] in self?.onTouch() }
            recognizer.blocksUnderlyingTouches = blocksUnderlyingTouches
            return recognizer
        }()
        private lazy var tapRecognizer: ObservationTapGestureRecognizer = {
            let recognizer = ObservationTapGestureRecognizer(
                target: self,
                action: #selector(handleTap(_:))
            )
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            return recognizer
        }()
        private lazy var contextMenuRecognizer: ObservationLongPressGestureRecognizer = {
            let recognizer = ObservationLongPressGestureRecognizer(
                target: self,
                action: #selector(handleLongPress(_:))
            )
            recognizer.minimumPressDuration = 0.42
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            return recognizer
        }()

        override func didMoveToWindow() {
            super.didMoveToWindow()
            install(on: window)
        }

        func uninstall() {
            gestureHost?.removeGestureRecognizer(touchDownRecognizer)
            gestureHost?.removeGestureRecognizer(tapRecognizer)
            gestureHost?.removeGestureRecognizer(contextMenuRecognizer)
            gestureHost = nil
            suppressesTapForCurrentLongPress = false
        }

        private func install(on host: UIView?) {
            guard gestureHost !== host else { return }
            uninstall()
            guard let host else { return }
            host.addGestureRecognizer(touchDownRecognizer)
            if observesTouchActions {
                host.addGestureRecognizer(tapRecognizer)
                host.addGestureRecognizer(contextMenuRecognizer)
            }
            gestureHost = host
        }

        @objc private func handleTap(
            _ recognizer: ObservationTapGestureRecognizer
        ) {
            guard recognizer.state == .ended, let host = gestureHost else { return }
            guard !suppressesTapForCurrentLongPress else { return }
            let hitView = recognizer.initialHitView ?? {
                let point = recognizer.location(in: host)
                return host.hitTest(point, with: nil)
            }()
            if let toggle = touchedSwitch(from: hitView, stoppingAt: host) {
                // SwiftUI applies a Toggle binding in the same event turn. Read
                // UISwitch after that write so audio reflects the resulting state.
                DispatchQueue.main.async { [weak self, weak toggle] in
                    guard let self, let toggle else { return }
                    self.onToggle(toggle.isOn)
                }
            } else if isNavigationBackControl(
                from: hitView,
                stoppingAt: host
            ) {
                onBack()
            } else if ownsTapAction(
                from: hitView,
                stoppingAt: host
            ) {
                onTap()
            }
        }

        @objc private func handleLongPress(
            _ recognizer: UILongPressGestureRecognizer
        ) {
            switch recognizer.state {
            case .began:
                // The tap recognizer stays out of the gesture arena, so a long press
                // must not play a late Navigation sound over an open context menu.
                suppressesTapForCurrentLongPress = true
                guard let host = gestureHost else { return }
                let point = recognizer.location(in: host)
                let hitView = host.hitTest(point, with: nil)
                if ownsContextMenu(from: hitView, stoppingAt: host) {
                    onContextMenu()
                } else if isEmptyThemeShortcutSpace(
                    from: hitView,
                    stoppingAt: host
                ) {
                    // Screen halves map to L2/R2 and do not mirror right to left.
                    let step = point.x < host.bounds.midX ? -1 : 1
                    // A card's own context menu claims the router first, which then drops this.
                    DispatchQueue.main.async { [weak self] in
                        guard let self, self.gestureHost === host else { return }
                        self.onEmptySpaceLongPress(step)
                    }
                }
            case .ended, .cancelled, .failed:
                DispatchQueue.main.async { [weak self] in
                    self?.suppressesTapForCurrentLongPress = false
                }
            default:
                break
            }
        }

        private func touchedSwitch(
            from hitView: UIView?,
            stoppingAt host: UIView
        ) -> UISwitch? {
            var candidate = hitView
            while let view = candidate, view !== host {
                if let toggle = view as? UISwitch {
                    return toggle
                }
                if NSStringFromClass(type(of: view)).localizedCaseInsensitiveContains("Cell"),
                   let toggle = firstSwitch(in: view) {
                    return toggle
                }
                candidate = view.superview
            }
            return nil
        }

        private func firstSwitch(in view: UIView) -> UISwitch? {
            for child in view.subviews {
                if let toggle = child as? UISwitch {
                    return toggle
                }
                if let toggle = firstSwitch(in: child) {
                    return toggle
                }
            }
            return nil
        }

        private func ownsTapAction(
            from hitView: UIView?,
            stoppingAt host: UIView
        ) -> Bool {
            var candidate = hitView
            while let view = candidate, view !== host {
                guard !view.isHidden, view.alpha >= 0.01 else {
                    return false
                }
                if let control = view as? UIControl {
                    return control.isEnabled && control.isUserInteractionEnabled
                }
                if view.isAccessibilityElement {
                    let traits = view.accessibilityTraits
                    if traits.contains(.notEnabled) { return false }
                    if traits.contains(.button)
                        || traits.contains(.link)
                        || traits.contains(.adjustable) {
                        return view.isUserInteractionEnabled
                    }
                }
                candidate = view.superview
            }
            return false
        }

        private func isNavigationBackControl(
            from hitView: UIView?,
            stoppingAt host: UIView
        ) -> Bool {
            var candidate = hitView
            var navigationBar: UINavigationBar?
            var touchesButton = false
            while let view = candidate, view !== host {
                if view is UIControl
                    || view.accessibilityTraits.contains(.button) {
                    touchesButton = true
                }
                if let bar = view as? UINavigationBar {
                    navigationBar = bar
                    break
                }
                candidate = view.superview
            }
            guard touchesButton, let navigationBar,
                  let navigationController = navigationController(
                    containing: navigationBar,
                    from: host.window?.rootViewController
                  ),
                  navigationController.viewControllers.count > 1,
                  let hitView else { return false }

            let hitFrame = hitView.convert(hitView.bounds, to: navigationBar)
            switch navigationBar.effectiveUserInterfaceLayoutDirection {
            case .rightToLeft:
                return hitFrame.midX > navigationBar.bounds.midX
            default:
                return hitFrame.midX < navigationBar.bounds.midX
            }
        }

        private func navigationController(
            containing navigationBar: UINavigationBar,
            from controller: UIViewController?
        ) -> UINavigationController? {
            guard let controller else { return nil }
            if let navigationController = controller as? UINavigationController,
               navigationController.navigationBar === navigationBar {
                return navigationController
            }
            if let presented = navigationController(
                containing: navigationBar,
                from: controller.presentedViewController
            ) {
                return presented
            }
            for child in controller.children {
                if let match = navigationController(
                    containing: navigationBar,
                    from: child
                ) {
                    return match
                }
            }
            return nil
        }

        private func ownsContextMenu(
            from hitView: UIView?,
            stoppingAt host: UIView
        ) -> Bool {
            var candidate = hitView
            while let view = candidate, view !== host {
                if view.interactions.contains(where: { $0 is UIContextMenuInteraction }) {
                    return true
                }
                candidate = view.superview
            }
            return false
        }

        /// Treat only noninteractive backdrop as a theme shortcut. SwiftUI
        /// buttons expose accessibility action traits through their hosting
        /// views, while native controls, cells, and context-menu interactions
        /// are rejected directly. This keeps game-card context menus, sliders,
        /// and navigation chrome in their original gesture arenas.
        private func isEmptyThemeShortcutSpace(
            from hitView: UIView?,
            stoppingAt host: UIView
        ) -> Bool {
            guard let hitView,
                  hitView === host || hitView.isDescendant(of: host),
                  !ownsTapAction(from: hitView, stoppingAt: host) else {
                return false
            }

            var candidate: UIView? = hitView
            while let view = candidate, view !== host {
                guard !view.isHidden,
                      view.alpha >= 0.01,
                      view.isUserInteractionEnabled else {
                    return false
                }
                if view is UIControl
                    || view is UITabBar
                    || view is UITableViewCell
                    || view is UICollectionViewCell
                    || view is UITextField
                    || view is UITextView {
                    return false
                }
                if view.interactions.contains(where: {
                    $0 is UIContextMenuInteraction
                }) {
                    return false
                }
                candidate = view.superview
            }
            return true
        }
    }

    final class TouchDownRecognizer: UIGestureRecognizer {
        var onTouchDown: () -> Void = {}
        var blocksUnderlyingTouches = false {
            didSet { cancelsTouchesInView = blocksUnderlyingTouches }
        }

        override func touchesBegan(
            _ touches: Set<UITouch>,
            with event: UIEvent
        ) {
            onTouchDown()
            // In controller mode, consume only the first touch that presents
            // the handoff prompt. Once the prompt exists this returns to pure
            // observation so its buttons remain directly tappable.
            state = blocksUnderlyingTouches ? .ended : .failed
        }
    }

    final class ObservationTapGestureRecognizer: UITapGestureRecognizer {
        private(set) weak var initialHitView: UIView?

        override func touchesBegan(
            _ touches: Set<UITouch>,
            with event: UIEvent
        ) {
            if let touch = touches.first, let view {
                initialHitView = view.hitTest(
                    touch.location(in: view),
                    with: event
                )
            } else {
                initialHitView = nil
            }
            super.touchesBegan(touches, with: event)
        }

        override func reset() {
            initialHitView = nil
            super.reset()
        }

        override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool {
            false
        }

        override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool {
            false
        }
    }

    final class ObservationLongPressGestureRecognizer: UILongPressGestureRecognizer {
        override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool {
            false
        }

        override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool {
            false
        }
    }
}

#endif
