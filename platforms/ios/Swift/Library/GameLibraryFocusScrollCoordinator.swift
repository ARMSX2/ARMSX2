// GameLibraryFocusScrollCoordinator.swift — Display-link scrolling that keeps the focused card centred
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// Owns controller-driven Game Library centering on one display-link clock.
/// Repeated SwiftUI `scrollTo` animations can interrupt one another and leave
/// semantic focus several cards ahead of the viewport. Directly retargeting the
/// live UIKit offset keeps held navigation centered without a competing binding.
@MainActor
final class GameLibraryFocusScrollCoordinator: NSObject {
    private final class WeakTarget {
        weak var view: UIView?

        init(_ view: UIView) {
            self.view = view
        }
    }

    private var lastFocusRequestTime: TimeInterval?
    private var targets: [String: WeakTarget] = [:]
    private weak var scrollView: UIScrollView?
    private var displayLink: CADisplayLink?
    private var animationStartOffset = CGPoint.zero
    private var animationDestination = CGPoint.zero
    private var animationStartTime: CFTimeInterval = 0
    private var animationDuration: CFTimeInterval = 0

    func register(_ view: UIView, for gameID: String) {
        guard targets[gameID]?.view !== view else { return }
        targets[gameID] = WeakTarget(view)
    }

    func unregister(_ view: UIView, for gameID: String) {
        guard targets[gameID]?.view === view else { return }
        targets.removeValue(forKey: gameID)
    }

    func focus(
        gameID: String,
        proxy: ScrollViewProxy,
        reduceMotion: Bool,
        axis: GameLibraryFocusScrollAxis,
        usesCompactAnimation: Bool
    ) {
        let now = ProcessInfo.processInfo.systemUptime
        let requestInterval = lastFocusRequestTime.map { now - $0 }
        lastFocusRequestTime = now

        let duration: Double
        if usesCompactAnimation {
            // Held navigation accelerates progressively. Finish each center
            // move before the observed repeat cadence instead of letting old
            // cover-flow animations accumulate behind semantic focus.
            if let requestInterval, requestInterval < 0.18 {
                duration = max(0.024, min(0.075, requestInterval * 0.75))
            } else {
                duration = 0.11
            }
        } else {
            if let requestInterval, requestInterval < 0.22 {
                duration = max(0.024, min(0.10, requestInterval * 0.75))
            } else {
                duration = 0.16
            }
        }

        if let target = targetView(for: gameID),
           let targetScrollView = enclosingScrollView(for: target),
           let destination = centeredDestination(
               for: target,
               in: targetScrollView,
               axis: axis
           ) {
            if reduceMotion || duration <= 0 {
                cancelAnimation()
                targetScrollView.setContentOffset(destination, animated: false)
            } else {
                animate(
                    targetScrollView,
                    to: destination,
                    duration: duration
                )
            }
            return
        }

        // An initial/restored ID can be outside the lazy mount window. Mount it
        // once through its stable ID; adjacent held-navigation targets then use
        // the direct 120 Hz path above.
        if reduceMotion {
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                proxy.scrollTo(gameID, anchor: .center)
            }
            return
        }

        withAnimation(.smooth(duration: duration, extraBounce: 0)) {
            proxy.scrollTo(gameID, anchor: .center)
        }
    }

    func cancel() {
        cancelAnimation()
        targets.removeAll(keepingCapacity: false)
    }

    func stopAnimation() {
        cancelAnimation()
    }

    /// Returns the mounted card closest to the visible center after free
    /// right-stick scrolling. Fully visible cards always win over clipped
    /// cards, keeping list, grid, and landscape cover flow deterministic.
    func centeredVisibleGameID(axis: GameLibraryFocusScrollAxis) -> String? {
        let candidates = targets.compactMap {
            gameID, weakTarget -> (
                gameID: String,
                centerDistance: CGFloat,
                hiddenExtent: CGFloat,
                fullyVisible: Bool
            )? in
            guard let target = weakTarget.view,
                  target.window != nil,
                  isEffectivelyVisible(target),
                  let scrollView = enclosingScrollView(for: target),
                  isEffectivelyVisible(scrollView) else { return nil }
            let targetFrame = target.convert(target.bounds, to: scrollView)
            let visibleFrame = visibleViewportFrame(in: scrollView)
            let intersection = targetFrame.intersection(visibleFrame)
            guard !intersection.isNull, !intersection.isInfinite else {
                return nil
            }

            let targetExtent: CGFloat
            let visibleExtent: CGFloat
            let centerDistance: CGFloat
            switch axis {
            case .horizontal:
                targetExtent = targetFrame.width
                visibleExtent = intersection.width
                centerDistance = abs(targetFrame.midX - visibleFrame.midX)
            case .vertical:
                targetExtent = targetFrame.height
                visibleExtent = intersection.height
                centerDistance = abs(targetFrame.midY - visibleFrame.midY)
            }
            guard targetExtent > 1, visibleExtent > 1 else { return nil }
            let fullyVisible = targetExtent <= {
                switch axis {
                case .horizontal: visibleFrame.width
                case .vertical: visibleFrame.height
                }
            }() + 1 && visibleExtent >= targetExtent - 1
            return (
                gameID,
                centerDistance,
                max(0, targetExtent - visibleExtent),
                fullyVisible
            )
        }
        let preferred = candidates.contains(where: \.fullyVisible)
            ? candidates.filter(\.fullyVisible)
            : candidates
        return preferred.min { lhs, rhs in
            let lhsScore = lhs.centerDistance + lhs.hiddenExtent
            let rhsScore = rhs.centerDistance + rhs.hiddenExtent
            if abs(lhsScore - rhsScore) > 0.5 { return lhsScore < rhsScore }
            return lhs.gameID < rhs.gameID
        }?.gameID
    }

    private func targetView(for gameID: String) -> UIView? {
        guard let view = targets[gameID]?.view else {
            targets.removeValue(forKey: gameID)
            return nil
        }
        return view.window == nil ? nil : view
    }

    private func enclosingScrollView(for view: UIView) -> UIScrollView? {
        var ancestor = view.superview
        while let current = ancestor {
            if let scrollView = current as? UIScrollView {
                return scrollView
            }
            ancestor = current.superview
        }
        return nil
    }

    private func isEffectivelyVisible(_ view: UIView) -> Bool {
        guard let window = view.window else { return false }
        var candidate: UIView? = view
        while let current = candidate {
            if current.isHidden
                || current.alpha < 0.01
                // Geometry probes are intentionally accessibility-hidden. Only
                // a hidden ancestor means the containing library presentation
                // is inactive and should be excluded from center restoration.
                || (current !== view && current.accessibilityElementsHidden) {
                return false
            }
            candidate = current.superview
        }
        let frame = view.convert(view.bounds, to: window)
        return frame.width > 1 && frame.height > 1
            && frame.intersects(window.bounds)
    }

    private func visibleViewportFrame(in scrollView: UIScrollView) -> CGRect {
        let inset = scrollView.adjustedContentInset
        return CGRect(
            x: scrollView.contentOffset.x + inset.left,
            y: scrollView.contentOffset.y + inset.top,
            width: max(1, scrollView.bounds.width - inset.left - inset.right),
            height: max(1, scrollView.bounds.height - inset.top - inset.bottom)
        )
    }

    private func centeredDestination(
        for target: UIView,
        in scrollView: UIScrollView,
        axis: GameLibraryFocusScrollAxis
    ) -> CGPoint? {
        guard target.window != nil else { return nil }
        let frame = target.convert(target.bounds, to: scrollView)
        guard !frame.isNull, !frame.isInfinite,
              frame.width > 1, frame.height > 1 else { return nil }
        let inset = scrollView.adjustedContentInset
        let minimumX = -inset.left
        let minimumY = -inset.top
        let maximumX = max(
            minimumX,
            scrollView.contentSize.width - scrollView.bounds.width + inset.right
        )
        let maximumY = max(
            minimumY,
            scrollView.contentSize.height - scrollView.bounds.height + inset.bottom
        )
        var destination = scrollView.contentOffset
        switch axis {
        case .horizontal:
            let visibleCenter = (inset.left + scrollView.bounds.width - inset.right) / 2
            destination.x = min(
                maximumX,
                max(minimumX, frame.midX - visibleCenter)
            )
        case .vertical:
            let visibleCenter = (inset.top + scrollView.bounds.height - inset.bottom) / 2
            destination.y = min(
                maximumY,
                max(minimumY, frame.midY - visibleCenter)
            )
        }
        return destination
    }

    private func animate(
        _ scrollView: UIScrollView,
        to destination: CGPoint,
        duration: Double
    ) {
        self.scrollView = scrollView
        animationStartOffset = scrollView.contentOffset
        animationDestination = destination
        animationStartTime = CACurrentMediaTime()
        animationDuration = max(1.0 / 120.0, duration)

        if let displayLink {
            displayLink.isPaused = false
            return
        }
        let link = CADisplayLink(
            target: self,
            selector: #selector(advanceAnimation(_:))
        )
        UIFrameRateSettings.shared.configuration.apply(
            to: link,
            domain: .controllerNavigation
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    @objc private func advanceAnimation(_ link: CADisplayLink) {
        guard let scrollView, scrollView.window != nil else {
            cancelAnimation()
            return
        }
        let progress = min(
            1,
            max(0, (link.timestamp - animationStartTime) / animationDuration)
        )
        let remaining = 1 - progress
        let eased = 1 - (remaining * remaining * remaining)
        let offset = CGPoint(
            x: animationStartOffset.x
                + (animationDestination.x - animationStartOffset.x) * eased,
            y: animationStartOffset.y
                + (animationDestination.y - animationStartOffset.y) * eased
        )
        scrollView.setContentOffset(offset, animated: false)
        if progress >= 1 {
            scrollView.setContentOffset(animationDestination, animated: false)
            cancelAnimation()
        }
    }

    private func cancelAnimation() {
        displayLink?.invalidate()
        displayLink = nil
        scrollView = nil
        animationDuration = 0
    }
}

@MainActor
struct GameLibraryFocusScrollRegistrationView: UIViewRepresentable {
    let gameID: String
    let coordinator: GameLibraryFocusScrollCoordinator

    func makeCoordinator() -> Coordinator {
        Coordinator(gameID: gameID, focusScrollCoordinator: coordinator)
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        context.coordinator.focusScrollCoordinator.register(view, for: gameID)
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        if context.coordinator.gameID != gameID {
            context.coordinator.focusScrollCoordinator.unregister(
                uiView,
                for: context.coordinator.gameID
            )
            context.coordinator.gameID = gameID
        }
        context.coordinator.focusScrollCoordinator.register(uiView, for: gameID)
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Coordinator) {
        coordinator.focusScrollCoordinator.unregister(
            uiView,
            for: coordinator.gameID
        )
    }

    @MainActor
    final class Coordinator {
        var gameID: String
        let focusScrollCoordinator: GameLibraryFocusScrollCoordinator

        init(
            gameID: String,
            focusScrollCoordinator: GameLibraryFocusScrollCoordinator
        ) {
            self.gameID = gameID
            self.focusScrollCoordinator = focusScrollCoordinator
        }
    }
}

extension View {
    @MainActor
    func registerGameLibraryFocusScrollTarget(
        for gameID: String,
        in coordinator: GameLibraryFocusScrollCoordinator,
        isEnabled: Bool = true
    ) -> some View {
        overlay {
            if isEnabled {
                GeometryReader { geometry in
                    GameLibraryFocusScrollRegistrationView(
                        gameID: gameID,
                        coordinator: coordinator
                    )
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
                    .allowsHitTesting(false)
                }
            }
        }
    }
}
