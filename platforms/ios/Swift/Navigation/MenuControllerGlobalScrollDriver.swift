// MenuControllerGlobalScrollDriver.swift — global right stick scroll driver
// SPDX-License-Identifier: GPL-3.0+

import UIKit

/// Fallback for screens that do not need controller-specific focus plumbing.
/// It discovers the frontmost visible UIKit scroll surface and drives it with
/// the same display-link velocity as explicit SwiftUI scroll targets. Explicit
/// targets still win, which keeps retained content underneath sheets passive.
@MainActor
final class MenuControllerGlobalScrollDriver {
    var onScroll: (@MainActor (UIScrollView, MenuControllerCommand) -> Void)?
    var onStopped: (@MainActor () -> Void)?
    var accelerationProvider: (@MainActor (CFTimeInterval) -> CGFloat)?
    private weak var scrollView: UIScrollView?
    private weak var preferredWindow: UIWindow?
    private var preferredFocusFrame: CGRect?
    private var requiresPreferredFocus = false
    private var vector = MenuControllerScrollVector.zero
    private var displayLink: CADisplayLink?
    private var lastTimestamp: CFTimeInterval?
    private let pointsPerSecond: CGFloat = 760

    func setPreferredFocus(
        window: UIWindow?,
        frame: CGRect?,
        requiresPreferredFocus: Bool
    ) {
        let unchanged = preferredWindow === window
            && preferredFocusFrame == frame
            && self.requiresPreferredFocus == requiresPreferredFocus
        guard !unchanged else { return }
        preferredWindow = window
        preferredFocusFrame = frame
        self.requiresPreferredFocus = requiresPreferredFocus
        guard let scrollView, let window, let frame else {
            self.scrollView = nil
            return
        }
        let focusPoint = CGPoint(x: frame.midX, y: frame.midY)
        let scrollFrame = scrollView.convert(scrollView.bounds, to: window)
        if !scrollFrame.contains(focusPoint) {
            self.scrollView = nil
        }
    }

    func consume(_ vector: MenuControllerScrollVector) -> Bool {
        if vector.isZero {
            stopScrolling()
            return false
        }
        let target: UIScrollView
        if let scrollView,
           scrollView.window != nil,
           canScroll(scrollView, for: vector) {
            target = scrollView
        } else if let resolved = resolveScrollView(for: vector) {
            target = resolved
        } else {
            stopScrolling()
            return false
        }
        scrollView = target
        self.vector = vector
        startDisplayLinkIfNeeded()
        return true
    }

    private func startDisplayLinkIfNeeded() {
        guard displayLink == nil else { return }
        lastTimestamp = nil
        let link = CADisplayLink(target: self, selector: #selector(step(_:)))
        UIFrameRateSettings.shared.configuration.apply(
            to: link,
            domain: .controllerNavigation
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private func stopScrolling() {
        vector = .zero
        scrollView = nil
        displayLink?.invalidate()
        displayLink = nil
        lastTimestamp = nil
    }

    @objc private func step(_ link: CADisplayLink) {
        guard !vector.isZero else {
            stopScrolling()
            onStopped?()
            return
        }
        if scrollView?.window == nil || !(scrollView.map {
            canScroll($0, for: vector)
        } ?? false) {
            scrollView = resolveScrollView(for: vector)
        }
        guard let scrollView else {
            stopScrolling()
            onStopped?()
            return
        }

        let previousTimestamp = lastTimestamp ?? link.timestamp
        let elapsed = min(1.0 / 30.0, link.timestamp - previousTimestamp)
        lastTimestamp = link.timestamp

        let limits = contentOffsetLimits(for: scrollView)
        let delta = scrollDelta(for: scrollView, vector: vector)
        let acceleration = accelerationProvider?(link.timestamp) ?? 1
        var offset = scrollView.contentOffset
        offset.x = min(
            limits.maximumX,
            max(
                limits.minimumX,
                offset.x + delta.x * pointsPerSecond * acceleration * elapsed
            )
        )
        offset.y = min(
            limits.maximumY,
            max(
                limits.minimumY,
                offset.y + delta.y * pointsPerSecond * acceleration * elapsed
            )
        )
        scrollView.setContentOffset(offset, animated: false)
        onScroll?(scrollView, abs(delta.y) >= abs(delta.x)
            ? (delta.y < 0 ? .up : .down)
            : (delta.x < 0 ? .left : .right))
        if !canScroll(scrollView, for: vector) {
            stopScrolling()
            onStopped?()
        }
    }

    private func resolveScrollView(
        for vector: MenuControllerScrollVector
    ) -> UIScrollView? {
        let windows = (UIApplication.shared.appWindowScene?.windows ?? [])
            .filter { !$0.isHidden && $0.alpha >= 0.01 }
            .sorted { lhs, rhs in
                if lhs.isKeyWindow != rhs.isKeyWindow {
                    return lhs.isKeyWindow
                }
                return lhs.windowLevel.rawValue > rhs.windowLevel.rawValue
            }

        if let preferredWindow,
           windows.contains(where: { $0 === preferredWindow }) {
            let visible = visibleScrollViews(
                in: preferredWindow,
                window: preferredWindow
            )
            let scrollable = visible.filter { canScroll($0, for: vector) }
            if let preferredFocusFrame {
                let point = CGPoint(
                    x: preferredFocusFrame.midX,
                    y: preferredFocusFrame.midY
                )
                let containing = scrollable.filter {
                    $0.convert($0.bounds, to: preferredWindow).contains(point)
                }
                if let target = containing.min(by: {
                    let lhs = $0.convert($0.bounds, to: preferredWindow)
                    let rhs = $1.convert($1.bounds, to: preferredWindow)
                    return lhs.width * lhs.height < rhs.width * rhs.height
                }) {
                    return target
                }
            }

            // Analog scrolling releases focus on purpose, so the active session/window is the only
            // ownership boundary. Use its frontmost visible scroll surface rather than failing when
            // the last focused row was remounted or lived in navigation chrome.
            if let target = scrollable.first {
                return target
            }
        }

        // A registered navigation surface must never fall through to a scroll
        // view in another retained tab or obscured presentation.
        if requiresPreferredFocus { return nil }

        for window in windows {
            if let target = visibleScrollViews(in: window, window: window)
                .first(where: { canScroll($0, for: vector) }) {
                return target
            }
        }
        return nil
    }

    private func visibleScrollViews(
        in view: UIView,
        window: UIWindow
    ) -> [UIScrollView] {
        guard !view.isHidden, view.alpha >= 0.01,
              !view.accessibilityElementsHidden,
              view.window != nil,
              view.bounds.width > 0, view.bounds.height > 0 else {
            return []
        }
        let visibleFrame = view.convert(view.bounds, to: window)
        guard visibleFrame.intersects(window.bounds) else { return [] }

        var result: [UIScrollView] = []
        for subview in view.subviews.reversed() {
            result.append(
                contentsOf: visibleScrollViews(in: subview, window: window)
            )
        }
        if let candidate = view as? UIScrollView,
           candidate.isScrollEnabled,
           candidate.isUserInteractionEnabled {
            result.append(candidate)
        }
        return result
    }

    private func canScroll(
        _ scrollView: UIScrollView,
        for vector: MenuControllerScrollVector
    ) -> Bool {
        let limits = contentOffsetLimits(for: scrollView)
        let epsilon: CGFloat = 0.5
        let delta = scrollDelta(for: scrollView, vector: vector)

        if delta.x != 0 {
            return delta.x < 0
                ? scrollView.contentOffset.x > limits.minimumX + epsilon
                : scrollView.contentOffset.x < limits.maximumX - epsilon
        }
        guard delta.y != 0 else { return false }
        return delta.y < 0
            ? scrollView.contentOffset.y > limits.minimumY + epsilon
            : scrollView.contentOffset.y < limits.maximumY - epsilon
    }

    private func scrollDelta(
        for scrollView: UIScrollView,
        vector: MenuControllerScrollVector
    ) -> CGPoint {
        let limits = contentOffsetLimits(for: scrollView)
        let hasHorizontalRange = limits.maximumX - limits.minimumX > 0.5
        let hasVerticalRange = limits.maximumY - limits.minimumY > 0.5

        if hasHorizontalRange,
           (!hasVerticalRange || abs(vector.x) > abs(vector.y)) {
            let amount = abs(vector.x) >= abs(vector.y)
                ? vector.x
                : -vector.y
            return CGPoint(x: CGFloat(amount), y: 0)
        }
        if hasVerticalRange {
            let amount = abs(vector.y) >= abs(vector.x)
                ? -vector.y
                : vector.x
            return CGPoint(x: 0, y: CGFloat(amount))
        }
        return .zero
    }

    private func contentOffsetLimits(
        for scrollView: UIScrollView
    ) -> (
        minimumX: CGFloat,
        maximumX: CGFloat,
        minimumY: CGFloat,
        maximumY: CGFloat
    ) {
        let inset = scrollView.adjustedContentInset
        let minimumX = -inset.left
        let minimumY = -inset.top
        return (
            minimumX,
            max(
                minimumX,
                scrollView.contentSize.width - scrollView.bounds.width
                    + inset.right
            ),
            minimumY,
            max(
                minimumY,
                scrollView.contentSize.height - scrollView.bounds.height
                    + inset.bottom
            )
        )
    }
}
