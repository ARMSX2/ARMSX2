// ControllerAccessibilityNavigationSession+ScrollPresentation.swift — focus animation while a scroll view moves
// SPDX-License-Identifier: GPL-3.0+

import QuartzCore
import UIKit

extension ControllerAccessibilityNavigationSession {
    func startScrollPresentationTracking(for key: String) {
        scrollPresentationCompletionTask?.cancel()
        scrollPresentationCompletionTask = nil
        focusMotion = nil
        scrollPresentationKey = key
        scrollPresentationStartTime = CACurrentMediaTime()
        scrollPresentationLastFrame = nil
        scrollPresentationStableFrameCount = 0
        if !isScrollPresentationActive { isScrollPresentationActive = true }
        // A held traversal retargets the existing clock instead of repeatedly
        // tearing down both overlay animation owners.
        guard scrollPresentationDisplayLink == nil else { return }

        let driver = ControllerAccessibilityDisplayLinkDriver()
        driver.onFrame = { [weak self] timestamp in
            guard let self, let key = self.scrollPresentationKey else { return }
            self.sampleScrollPresentation(for: key, at: timestamp)
        }
        let displayLink = CADisplayLink(
            target: driver,
            selector: #selector(
                ControllerAccessibilityDisplayLinkDriver.displayLinkDidFire(_:)
            )
        )
        UIFrameRateSettings.shared.configuration.apply(
            to: displayLink,
            domain: .controllerNavigation
        )

        scrollPresentationDriver = driver
        scrollPresentationDisplayLink = displayLink
        displayLink.add(to: .main, forMode: .common)
    }

    /// Share one display-link clock between A-to-B focus travel and the actual
    /// content offset. A UIView animation commits its final model offset at
    /// once, which makes SwiftUI's portalled Liquid Glass rows jump ahead.
    @discardableResult
    func animateFocus(
        to key: String,
        scrolling scrollView: UIScrollView? = nil,
        to destinationOffset: CGPoint = .zero
    ) -> Bool {
        guard !UIAccessibility.isReduceMotionEnabled,
              let view = targets[key]?.view.value,
              let targetFrame = presentationFrameForView(view) else { return false }
        let style = SettingsStore.shared.appearance.controllerNavigationFocusAnimation
        let baseDuration = scrollView == nil ? style.duration
            : scrollAnimationDuration * style.duration
                / ControllerNavigationFocusTravelStyle.easeOutBack.duration
        let acceleration = focusRepeatAcceleration ?? 1
        let duration = focusRepeatAcceleration != nil
                ? min(
                    baseDuration / (acceleration * acceleration),
                    0.085 / acceleration
                )
                : baseDuration
        guard duration > 0 else { return false }
        // Strip only the outline padding; updateFocusedPresentation re-adds it after interpolation.
        let source = focusPresentation.frame?.insetBy(dx: 3, dy: 2) ?? targetFrame
        guard scrollView != nil || !approximatelyEqual(source, targetFrame)
        else { return false }
        startScrollPresentationTracking(for: key)
        focusMotion = FocusMotion(
            sourceFrame: source,
            duration: duration,
            style: style,
            scrollView: scrollView,
            sourceOffset: scrollView?.contentOffset ?? .zero,
            destinationOffset: destinationOffset
        )
        return true
    }

    func sampleScrollPresentation(
        for key: String,
        at timestamp: CFTimeInterval
    ) {
        guard scrollPresentationKey == key else { return }
        let elapsed = timestamp - scrollPresentationStartTime
        guard focusedKey == key || pendingFocusKey == key else {
            stopScrollPresentationTracking(publishesFinalFrame: false)
            return
        }

        if pendingFocusKey == key, isMountedAndEnabled(key) {
            // The display link may see the probe before its mount task runs.
            // Both paths must use the same deferred geometry reconciliation.
            scheduleMountedTargetReconciliation(key: key)
            return
        }
        guard focusedKey == key else { return }

        guard let view = targets[key]?.view.value,
              view.window != nil,
              let frame = presentationFrameForView(view) else {
            scrollPresentationLastFrame = nil
            scrollPresentationStableFrameCount = 0
            if elapsed >= 2,
               !ControllerEventDeliveryCoordinator.shared.rightStickScrolling {
                stopScrollPresentationTracking()
            }
            return
        }

        var displayedFrame = frame
        if let motion = focusMotion {
            let progress = min(1, max(0, elapsed / motion.duration))
            var destinationFrame = frame
            if let scrollView = motion.scrollView {
                if scrollView.isTracking || scrollView.isDragging {
                    // A touch or analog gesture must take ownership immediately.
                    focusMotion = nil
                } else {
                    // Sample before advancing the offset, so a glass row and
                    // its scroll view are measured from the same layout frame.
                    destinationFrame = frame.offsetBy(
                        dx: scrollView.contentOffset.x - motion.destinationOffset.x,
                        dy: scrollView.contentOffset.y - motion.destinationOffset.y
                    )
                    let eased = motion.style.scrollProgress(progress)
                    let offset = CGPoint(
                        x: motion.sourceOffset.x
                            + (motion.destinationOffset.x - motion.sourceOffset.x) * eased,
                        y: motion.sourceOffset.y
                            + (motion.destinationOffset.y - motion.sourceOffset.y) * eased
                    )
                    scrollView.setContentOffset(offset, animated: false)
                }
            }
            if focusMotion != nil {
                displayedFrame = motion.style.frame(
                    from: motion.sourceFrame, to: destinationFrame, progress: progress
                )
                if progress >= 1 {
                    focusMotion = nil
                    motion.scrollView.map(expandTitleAtTop)
                }
            }
        }
        updateFocusedPresentation(frameOverride: displayedFrame)
        publishScrollFocus()

        if let previous = scrollPresentationLastFrame,
           approximatelyEqual(previous, displayedFrame) {
            scrollPresentationStableFrameCount += 1
        } else {
            scrollPresentationStableFrameCount = 0
        }
        scrollPresentationLastFrame = displayedFrame

        let scrollView = enclosingScrollView(for: view)
        let scrollIsMoving = scrollView.map(scrollViewHasActivePresentation)
            ?? false
        let analogScrollIsActive =
            ControllerEventDeliveryCoordinator.shared.rightStickScrolling
        let settled = !analogScrollIsActive && focusMotion == nil
            && scrollPresentationStableFrameCount >= 2
            && !scrollIsMoving
        if settled || (!analogScrollIsActive && elapsed >= 2) {
            stopScrollPresentationTracking()
        }
    }

    func stopScrollPresentationTracking(
        publishesFinalFrame: Bool = true
    ) {
        let key = scrollPresentationKey
        scrollPresentationDisplayLink?.invalidate()
        scrollPresentationDisplayLink = nil
        scrollPresentationDriver?.onFrame = nil
        scrollPresentationDriver = nil
        scrollPresentationKey = nil
        scrollPresentationStartTime = 0
        scrollPresentationLastFrame = nil
        scrollPresentationStableFrameCount = 0
        focusMotion = nil

        guard publishesFinalFrame, key == focusedKey else {
            isScrollPresentationActive = false
            return
        }
        let finalFrame = key.flatMap { targets[$0]?.view.value }
            .flatMap(presentationFrameForView)
        updateFocusedPresentation(frameOverride: finalFrame)
        publishScrollFocus()

        // Leave direct tracking on through the observation turn that publishes
        // the final presentation rectangle. Disabling it at once lets the orb
        // renderer animate to that last sample, the visible end-of-scroll snap.
        scrollPresentationCompletionTask?.cancel()
        scrollPresentationCompletionTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled,
                  self.scrollPresentationKey == nil else { return }
            self.scrollPresentationCompletionTask = nil
            self.isScrollPresentationActive = false
        }
    }

    func scrollViewHasActivePresentation(_ scrollView: UIScrollView) -> Bool {
        if scrollView.isTracking || scrollView.isDragging
            || scrollView.isDecelerating {
            return true
        }
        guard let presentationBounds = scrollView.layer.presentation()?.bounds else {
            return false
        }
        let modelBounds = scrollView.layer.bounds
        return abs(presentationBounds.minX - modelBounds.minX) > 0.25
            || abs(presentationBounds.minY - modelBounds.minY) > 0.25
    }

    private func approximatelyEqual(_ lhs: CGRect, _ rhs: CGRect) -> Bool {
        abs(lhs.minX - rhs.minX) <= 0.25
            && abs(lhs.minY - rhs.minY) <= 0.25
            && abs(lhs.width - rhs.width) <= 0.25
            && abs(lhs.height - rhs.height) <= 0.25
    }

    func optionalFramesApproximatelyEqual(
        _ lhs: CGRect?,
        _ rhs: CGRect?,
        tolerance: CGFloat = 0.5
    ) -> Bool {
        switch (lhs, rhs) {
        case (nil, nil):
            return true
        case let (lhs?, rhs?):
            return abs(lhs.minX - rhs.minX) <= tolerance
                && abs(lhs.minY - rhs.minY) <= tolerance
                && abs(lhs.width - rhs.width) <= tolerance
                && abs(lhs.height - rhs.height) <= tolerance
        default:
            return false
        }
    }

    func frameForView(_ view: UIView) -> CGRect? {
        guard let window = view.window, !view.isHidden, view.alpha >= 0.01 else {
            return nil
        }
        let frame = view.convert(view.bounds, to: window)
        guard frame.width >= 2, frame.height >= 2 else { return nil }
        return frame
    }

    func presentationFrameForView(_ view: UIView) -> CGRect? {
        guard view.window != nil, !view.isHidden, view.alpha >= 0.01 else {
            return nil
        }
        guard var frame = frameForView(view) else { return nil }

        // Reconcile every animated scroll ancestor, or the row can show before its scroll does.
        var ancestor = view.superview
        while let current = ancestor {
            if let scrollView = current as? UIScrollView,
               let presentationBounds = scrollView.layer.presentation()?.bounds {
                let modelBounds = scrollView.layer.bounds
                frame.origin.x += modelBounds.minX - presentationBounds.minX
                frame.origin.y += modelBounds.minY - presentationBounds.minY
            }
            ancestor = current.superview
        }
        return isValidFocusFrame(frame) ? frame : nil
    }

    private func isValidFocusFrame(_ frame: CGRect) -> Bool {
        frame.width >= 2 && frame.height >= 2
            && frame.minX.isFinite && frame.minY.isFinite
            && frame.width.isFinite && frame.height.isFinite
    }
}
