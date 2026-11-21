// ControllerAccessibilityNavigationSession+Scrolling.swift — scroll reveal and right stick scrolling
// SPDX-License-Identifier: GPL-3.0+

import UIKit

extension ControllerAccessibilityNavigationSession {
    func suspendFocusForScrolling(preservingPresentation: Bool = false) {
        pendingDirectionalMove = nil
        focusRepeatAcceleration = nil
        permitsAutomaticFocus = false
        focusPresentationRevealTask?.cancel()
        focusPresentationRevealTask = nil
        pendingFocusExpiryTask?.cancel()
        pendingFocusExpiryTask = nil
        pendingFocusKey = nil
        mountedTargetTask?.cancel()
        mountedTargetTask = nil
        stopScrollPresentationTracking(publishesFinalFrame: false)

        // Capture the exact surface before a lazy List/Form recycles the row
        // which owned focus. Analog movement and directional restoration use
        // this same surface for the complete gesture.
        rightStickScrollView = preferredRightStickScrollView()
        rightStickManualFocusKey = nil
        rightStickFocusCandidates = nil

        // Fixed controls such as the Quick Menu Stop/Resume buttons do not move
        // with the scroll view. Keep their semantic and visual focus stable.
        if preservingPresentation,
           let focusedKey,
           let view = targets[focusedKey]?.view.value,
           enclosingScrollView(for: view) == nil {
            publishScrollFocus()
            return
        }

        // Retain the semantic location so the next controller command can
        // continue from the closest visible row instead of re-entering through
        // the screen's initial target.
        focusIsSuspendedForScrolling = true
        focusPresentationIsHiddenForScrolling = !preservingPresentation
        if preservingPresentation {
            if let focusedKey {
                startScrollPresentationTracking(for: focusedKey)
            }
        } else {
            if let focusedKey,
               targets[focusedKey]?.focusState.isFocused == true {
                targets[focusedKey]?.focusState.isFocused = false
            }
            // Retain the last rectangle as the fade-out source. The visual
            // overlay observes `focusPresentationIsHiddenForScrolling`; its
            // geometry remains private and cannot jump while content moves.
        }
        publishScrollFocus()
    }

    func resumeFocusAfterRightStickScrolling() {
        permitsAutomaticFocus = true
        if let scrollView = rightStickScrollView,
           let direction = controllerInput?.lastRightStickScrollDirection {
            followRightStickScroll(in: scrollView, direction: direction)
        }
        if focusIsSuspendedForScrolling {
            resumeFocusAfterScrollingIfNeeded()
        }
        rightStickManualFocusKey = nil
        rightStickScrollView = nil
        if let focusedKey { setFocus(focusedKey) }
    }

    /// Supplies the shared analog driver with the focused probe's actual
    /// scroll ancestor. This removes per-screen hierarchy guessing while
    /// retaining local fallbacks for fixed, non-scrolling controls.
    func preferredRightStickScrollView() -> UIScrollView? {
        if let rightStickScrollView,
           let window, rightStickScrollView.window === window,
           scrollViewIsEffectivelyVisible(rightStickScrollView, in: window) {
            return rightStickScrollView
        }
        guard let focusedKey,
              let view = targets[focusedKey]?.view.value,
              view.window != nil,
              let scrollView = enclosingScrollView(for: view) else {
            return nil
        }
        rightStickScrollView = scrollView
        return scrollView
    }

    @discardableResult
    func reveal(_ key: String) -> Bool {
        guard let target = targets[key],
              let view = target.view.value,
              let scrollView = enclosingScrollView(for: view) else { return false }
        switch focusScrollBehavior {
        case .revealIfNeeded:
            guard !isFullyVisible(view, in: scrollView) else { return false }
        case .maintainWithinViewport:
            guard let destination = focusCorridorDestination(
                for: view,
                in: scrollView
            ) else { return false }
            if !animateFocus(to: key, scrolling: scrollView, to: destination) {
                startScrollPresentationTracking(for: key)
                scrollView.setContentOffset(destination, animated: false)
                expandTitleAtTop(scrollView)
            }
            return true
        }
        if let scrollToTarget {
            // Match Cheats & Patches: move directly to the stable row ID
            // instead of advancing the UIKit viewport in synthetic steps.
            startScrollPresentationTracking(for: key)
            scrollToTarget(target.baseKey, focusScrollBehavior.anchor)
            return true
        }
        let rect = view.convert(view.bounds, to: scrollView)
        startScrollPresentationTracking(for: key)
        scrollView.scrollRectToVisible(
            rect.insetBy(dx: 0, dy: -18),
            animated: true
        )
        return true
    }

    /// A collapsed large title only grows back while the content is pulled past the top,
    /// so at the top, pull once and settle on the grown inset.
    func expandTitleAtTop(_ scrollView: UIScrollView) {
        let x = scrollView.contentOffset.x
        let top = -scrollView.adjustedContentInset.top
        guard scrollView.contentOffset.y <= top + 0.5 else { return }
        scrollView.setContentOffset(CGPoint(x: x, y: top - scrollView.bounds.height), animated: false)
        scrollView.window?.layoutIfNeeded()
        scrollView.setContentOffset(CGPoint(x: x, y: -scrollView.adjustedContentInset.top), animated: false)
    }

    private func isFullyVisible(_ view: UIView, in scrollView: UIScrollView) -> Bool {
        guard view.window != nil else { return false }
        let targetFrame = view.convert(view.bounds, to: scrollView)
        // The bare bounds counted a row hidden under a bar or pinned header as visible.
        let viewport = scrollViewportFrame(in: scrollView)
        let maximumMargin = max(
            0,
            (viewport.height - targetFrame.height) / 2 - 1
        )
        let edgeMargin = min(focusViewportEdgeMargin, maximumMargin)
        return viewport.insetBy(dx: 0, dy: edgeMargin).contains(targetFrame)
    }

    private func focusCorridorDestination(
        for view: UIView,
        in scrollView: UIScrollView
    ) -> CGPoint? {
        guard view.window != nil else { return nil }
        let targetFrame = view.convert(view.bounds, to: scrollView)
        let corridor = focusViewportFrame(in: scrollView)
        var proposedY: CGFloat
        if targetFrame.height >= corridor.height {
            proposedY = scrollView.contentOffset.y
                + targetFrame.minY - corridor.minY
        } else if targetFrame.minY < corridor.minY {
            proposedY = scrollView.contentOffset.y
                - (corridor.minY - targetFrame.minY)
        } else if targetFrame.maxY > corridor.maxY {
            proposedY = scrollView.contentOffset.y
                + (targetFrame.maxY - corridor.maxY)
        } else {
            proposedY = scrollView.contentOffset.y
        }

        let minimumY = -scrollView.adjustedContentInset.top
        // Moving up to within a margin of the top shows the whole top, if the row stays on screen.
        let viewport = scrollViewportFrame(in: scrollView)
        if proposedY <= scrollView.contentOffset.y, proposedY < minimumY + focusTopAlignmentMargin,
           targetFrame.maxY <= viewport.maxY + minimumY - scrollView.contentOffset.y {
            proposedY = minimumY
        }
        let maximumY = max(
            minimumY,
            scrollView.contentSize.height - scrollView.bounds.height
                + scrollView.adjustedContentInset.bottom
        )
        let destination = CGPoint(
            x: scrollView.contentOffset.x,
            y: min(maximumY, max(minimumY, proposedY))
        )
        return abs(destination.y - scrollView.contentOffset.y) > 0.5
            ? destination : nil
    }

    private func effectiveTopAlignmentMargin(
        in scrollView: UIScrollView
    ) -> CGFloat {
        min(
            focusTopAlignmentMargin,
            max(0, scrollViewportFrame(in: scrollView).height * 0.3)
        )
    }

    private func effectiveBottomAlignmentMargin(
        in scrollView: UIScrollView
    ) -> CGFloat {
        // The persistent bottom navigation surface needs twice the breathing
        // room of the title edge. Keep enough of the viewport available for a
        // complete focused row even on compact landscape screens.
        min(
            focusBottomAlignmentMargin * 2,
            max(0, scrollViewportFrame(in: scrollView).height * 0.45)
        )
    }

    private func scrollViewportFrame(in scrollView: UIScrollView) -> CGRect {
        let viewport = scrollView.bounds.inset(by: scrollView.adjustedContentInset)
        guard let window = scrollView.window, window === self.window,
              !scopeFrame.isEmpty else { return viewport }
        // A List can run under RootView's tab bar, so clip to the session before reserving gaps.
        let visibleScope = scopeFrame.intersection(window.bounds)
        let clipped = viewport.intersection(
            scrollView.convert(visibleScope, from: window)
        )
        return clipped.isNull || clipped.isEmpty ? viewport : clipped
    }

    private func focusViewportFrame(in scrollView: UIScrollView) -> CGRect {
        let viewport = scrollViewportFrame(in: scrollView)
        let minimumY = viewport.minY + effectiveTopAlignmentMargin(in: scrollView)
        let maximumY = viewport.maxY - effectiveBottomAlignmentMargin(in: scrollView)
        return CGRect(
            x: viewport.minX,
            y: minimumY,
            width: max(1, viewport.width),
            height: max(1, maximumY - minimumY)
        )
    }

    /// Touch-to-controller recovery retains the row when it is still visible.
    /// Right-stick flow instead uses the direction of the actual scroll driver.
    func resumeFocusAfterScrollingIfNeeded() {
        guard focusIsSuspendedForScrolling else {
            rightStickScrollView = nil
            return
        }
        focusIsSuspendedForScrolling = false
        stopScrollPresentationTracking(publishesFinalFrame: false)

        let retainedKey = focusedKey
        let resumedKey: String?
        if let retainedKey, isVisibleInScrollViewport(retainedKey) {
            resumedKey = retainedKey
        } else {
            resumedKey = nearestVisibleScrollableKey() ?? retainedKey
        }

        if focusedKey != resumedKey {
            if let focusedKey,
               targets[focusedKey]?.focusState.isFocused == true {
                targets[focusedKey]?.focusState.isFocused = false
            }
            focusedKey = resumedKey
        }

        guard let resumedKey, isMountedAndEnabled(resumedKey) else {
            rightStickScrollView = nil
            setFocusedPresentation(.empty)
            focusPresentationIsHiddenForScrolling = false
            publishScrollFocus()
            return
        }
        targets[resumedKey]?.focusState.isFocused = true
        rightStickScrollView = nil
        updateFocusedPresentation()
        publishScrollFocus()
    }

    /// Called by the existing analog display link after it applies the offset.
    /// No additional timer or scrollTo: the stick alone owns scrolling, while
    /// the focus animation follows newly visible rows.
    func followRightStickScroll(
        in scrollView: UIScrollView,
        direction: MenuControllerCommand
    ) {
        guard controllerInput?.isControllerNavigationEnabled == true,
              scrollView.window != nil else { return }
        if rightStickScrollView !== scrollView || rightStickViewportSize != scrollView.bounds.size {
            rightStickFocusCandidates = nil
            rightStickViewportSize = scrollView.bounds.size
        }
        rightStickScrollView = scrollView
        let viewport = focusViewportFrame(in: scrollView)
        let key: String?
        if let manualKey = rightStickManualFocusKey,
           let view = targets[manualKey]?.view.value,
           enclosingScrollView(for: view) === scrollView,
           viewport.insetBy(dx: -1, dy: -1).contains(
               view.convert(view.bounds, to: scrollView)
           ) {
            // Use the same protected viewport as directional navigation, not
            // the larger UIKit bounds underneath the title and tab bar.
            key = manualKey
        } else {
            rightStickManualFocusKey = nil
            key = directionalVisibleScrollableKey(in: scrollView, direction: direction)
        }
        guard let key else { return }
        if focusedKey != key || focusIsSuspendedForScrolling
            || targets[key]?.focusState.isFocused != true {
            setFocus(key, scrolls: false)
        } else if !isScrollPresentationActive {
            updateFocusedPresentation(
                frameOverride: targets[key]?.view.value.flatMap(presentationFrameForView)
            )
        }
        publishScrollFocus()
    }

    private func directionalVisibleScrollableKey(
        in scrollView: UIScrollView,
        direction: MenuControllerCommand
    ) -> String? {
        // Content-space rectangles do not change with contentOffset. Rebuild
        // only for mounting/resizing, not 120 times a second while scrolling.
        if rightStickFocusCandidates == nil {
            rightStickFocusCandidates = targets.values.compactMap { target in
                guard target.isEnabled, let view = target.view.value,
                      enclosingScrollView(for: view) === scrollView,
                      view.window != nil else { return nil }
                return ControllerNavigationScrollFocusCandidate(
                    key: target.key,
                    frame: view.convert(view.bounds, to: scrollView),
                    order: declaredOrderIndex[target.key]
                        ?? Int(target.registrationOrder)
                )
            }
        }
        let candidates = rightStickFocusCandidates ?? []
        return ControllerNavigationScrollFocusCandidate.preferredKey(
            in: candidates,
            viewport: focusViewportFrame(in: scrollView),
            direction: direction
        ) ?? ControllerNavigationScrollFocusCandidate.preferredKey(
            in: candidates,
            viewport: scrollViewportFrame(in: scrollView),
            direction: direction
        )
    }

    func isVisibleInScrollViewport(_ key: String) -> Bool {
        guard let view = targets[key]?.view.value,
              let scrollView = enclosingScrollView(for: view),
              view.window != nil else { return false }
        return visibleIntersectionHeight(of: view, in: scrollView) > 0
    }

    private func nearestVisibleScrollableKey() -> String? {
        orderedKeys().compactMap { key -> (key: String, score: CGFloat)? in
            guard let view = targets[key]?.view.value,
                  let scrollView = enclosingScrollView(for: view),
                  view.window != nil else { return nil }
            let targetFrame = view.convert(view.bounds, to: scrollView)
            let visibleFrame = focusViewportFrame(in: scrollView)
            let visibleHeight = visibleIntersectionHeight(of: view, in: scrollView)
            guard visibleHeight > 0 else { return nil }

            // Prefer the row nearest the viewport center, then the one with more of it visible.
            let centerDistance = abs(targetFrame.midY - visibleFrame.midY)
            let hiddenHeight = max(0, targetFrame.height - visibleHeight)
            return (key, centerDistance + hiddenHeight)
        }
        .min { lhs, rhs in lhs.score < rhs.score }?
        .key
    }

    private func visibleIntersectionHeight(
        of view: UIView,
        in scrollView: UIScrollView
    ) -> CGFloat {
        let targetFrame = view.convert(view.bounds, to: scrollView)
        let visibleFrame = focusViewportFrame(in: scrollView)
        let intersection = targetFrame.intersection(visibleFrame)
        guard !intersection.isNull, !intersection.isInfinite else { return 0 }
        return max(0, intersection.height)
    }

    func focusOrSeek(_ key: String) {
        if isMountedAndEnabled(key), !needsLogicalScrollOwner(for: key) {
            setFocus(key)
            return
        }
        guard declaredOrder.contains(key) else { return }
        pendingFocusKey = key
        pendingFocusExpiryTask?.cancel()
        pendingFocusExpiryTask = Task { @MainActor [weak self] in
            // A long lazy Form may need more than one layout turn to mount a
            // remembered offscreen row after ScrollViewReader seeks to it.
            try? await Task.sleep(for: .milliseconds(1_500))
            guard let self, !Task.isCancelled,
                  self.pendingFocusKey == key else { return }
            self.pendingFocusKey = nil
            self.pendingFocusExpiryTask = nil
            self.pendingDirectionalMove = nil
            self.stopScrollPresentationTracking(publishesFinalFrame: false)
            // A removed/disabled destination must not leave the source at an
            // obsolete offset after the logical seek expires.
            if let visibleKey = self.nearestVisibleScrollableKey() {
                self.setFocus(visibleKey)
            }
        }
        startScrollPresentationTracking(for: key)
        scrollToTarget?(
            targets[key]?.baseKey ?? key,
            focusScrollBehavior.anchor
        )
    }

    func contentPosition(for view: UIView) -> CGFloat? {
        guard let scrollView = enclosingScrollView(for: view) else { return nil }
        return view.convert(view.bounds, to: scrollView).midY
    }

    /// SwiftUI Forms and Lists can put several private UIScrollViews between a
    /// row probe and the visible collection view. The first ancestor is often
    /// an internal control surface, so choose the largest visible vertical
    /// ancestor which actually owns scrollable content.
    func enclosingScrollView(for view: UIView) -> UIScrollView? {
        // Trailing actions are fixed chrome, not Form content. Glass portals
        // can temporarily overlap the Form while it lays out; never associate
        // Close/Save or Stop/Resume with that scrolling surface.
        if let probe = view as? ControllerAccessibilityActionProbeView,
           let target = targets[probe.resolvedKey],
           preferredTrailingFocusLabels.contains(where: { matches($0, target: target) }) {
            return nil
        }
        guard let window = view.window else { return nil }
        let targetFrame = view.convert(view.bounds, to: window)
        let targetPoint = CGPoint(x: targetFrame.midX, y: targetFrame.midY)
        var candidates: [UIScrollView] = []
        var ancestor = view.superview
        while let current = ancestor {
            if let scrollView = current as? UIScrollView,
               scrollView.isScrollEnabled,
               scrollView.isUserInteractionEnabled,
               scrollViewIsEffectivelyVisible(scrollView, in: window),
               verticalScrollableRange(of: scrollView) > 0.5 {
                candidates.append(scrollView)
            }
            ancestor = current.superview
        }
        if let ancestor = candidates.max(by: { lhs, rhs in
            let lhsFrame = lhs.convert(lhs.bounds, to: window)
            let rhsFrame = rhs.convert(rhs.bounds, to: window)
            let lhsContains = lhsFrame.contains(targetPoint)
            let rhsContains = rhsFrame.contains(targetPoint)
            if lhsContains != rhsContains { return !lhsContains }

            let lhsIntersection = lhsFrame.intersection(window.bounds)
            let rhsIntersection = rhsFrame.intersection(window.bounds)
            let lhsArea = lhsIntersection.isNull
                ? 0 : lhsIntersection.width * lhsIntersection.height
            let rhsArea = rhsIntersection.isNull
                ? 0 : rhsIntersection.width * rhsIntersection.height
            if abs(lhsArea - rhsArea) > 0.5 { return lhsArea < rhsArea }
            return verticalScrollableRange(of: lhs)
                < verticalScrollableRange(of: rhs)
        }) {
            return ancestor
        }

        // An iOS 26 glass row may have no UIScrollView ancestor, so use analog scrolling's owner.
        let key = ((view as? ControllerAccessibilityActionProbeView)?.resolvedKey)
            .flatMap { $0.isEmpty ? nil : $0 }
        if let owner = scrollSurfaces.allObjects.lazy
            .compactMap({ $0 as? any ControllerAccessibilityScrollSurface })
            .compactMap({ $0.scrollViewForFocusTarget(view) })
            .first {
            if let key { scrollOwners[key] = WeakView(owner) }
            return owner
        }
        // The marker forgets a row whose cell the List reused, and a row under
        // the title is outside it, so Settings never scrolled back to it.
        guard let key, let owner = scrollOwners[key]?.value as? UIScrollView,
              owner.window === window else { return nil }
        return owner
    }

    private func verticalScrollableRange(of scrollView: UIScrollView) -> CGFloat {
        let inset = scrollView.adjustedContentInset
        return scrollView.contentSize.height - scrollView.bounds.height
            + inset.top + inset.bottom
    }

    private func scrollViewIsEffectivelyVisible(
        _ scrollView: UIScrollView,
        in window: UIWindow
    ) -> Bool {
        var current: UIView? = scrollView
        while let view = current {
            if view.isHidden || view.alpha < 0.01
                || view.accessibilityElementsHidden {
                return false
            }
            if view === window { break }
            current = view.superview
        }
        let frame = scrollView.convert(scrollView.bounds, to: window)
        return frame.width > 1 && frame.height > 1
            && frame.intersects(window.bounds)
    }
}
