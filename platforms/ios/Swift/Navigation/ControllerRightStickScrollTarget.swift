// ControllerRightStickScrollTarget.swift — right stick scroll target
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UIKit

/// A marker which either lives inside SwiftUI scroll content or covers one
/// scroll surface. It finds that surface's UIKit scroll view and applies
/// right-stick velocity without frame-by-frame Observation publications.
@MainActor
struct ControllerRightStickScrollTarget: UIViewRepresentable {
    let controllerInput: MenuControllerInputRouter?
    let axes: MenuControllerScrollAxes
    var manualCaptureOwner: String? = nil
    var priority = 0
    var isEnabled = true
    var pointsPerSecond: CGFloat = 760
    var onReachedLeadingEdge: (@MainActor () -> Void)? = nil
    var searchesNearbyScrollViews = false
    var preferredScrollViewProvider: (@MainActor () -> UIScrollView?)? = nil
    var ownerSessionIDOverride: UUID? = nil
    @Environment(\.controllerAccessibilityNavigationSession)
    private var navigationSession
    @Environment(\.controllerAccessibilityNavigationRegistrationID)
    private var navigationRegistrationID

    func makeUIView(context: Context) -> UIView {
        let view = RightStickScrollTargetView()
        view.configure(
            controllerInput: controllerInput,
            navigationSession: navigationSession,
            ownerSessionID: ownerSessionIDOverride
                ?? navigationRegistrationID
                ?? navigationSession?.registrationIdentifier,
            manualCaptureOwner: manualCaptureOwner,
            axes: axes,
            priority: priority,
            isEnabled: isEnabled,
            pointsPerSecond: pointsPerSecond,
            onReachedLeadingEdge: onReachedLeadingEdge,
            searchesNearbyScrollViews: searchesNearbyScrollViews,
            preferredScrollViewProvider: preferredScrollViewProvider
        )
        return view
    }

    func updateUIView(
        _ uiView: UIView,
        context: Context
    ) {
        guard let uiView = uiView as? RightStickScrollTargetView else { return }
        uiView.configure(
            controllerInput: controllerInput,
            navigationSession: navigationSession,
            ownerSessionID: ownerSessionIDOverride
                ?? navigationRegistrationID
                ?? navigationSession?.registrationIdentifier,
            manualCaptureOwner: manualCaptureOwner,
            axes: axes,
            priority: priority,
            isEnabled: isEnabled,
            pointsPerSecond: pointsPerSecond,
            onReachedLeadingEdge: onReachedLeadingEdge,
            searchesNearbyScrollViews: searchesNearbyScrollViews,
            preferredScrollViewProvider: preferredScrollViewProvider
        )
    }

    static func dismantleUIView(
        _ uiView: UIView,
        coordinator: Void
    ) {
        (uiView as? RightStickScrollTargetView)?.disconnect()
    }
}

@MainActor
private final class RightStickScrollTargetView: UIView, ControllerAccessibilityScrollSurface {
    private let registrationID = UUID()
    private weak var controllerInput: MenuControllerInputRouter?
    private weak var navigationSession: ControllerAccessibilityNavigationSession?
    private var ownerSessionID: UUID?
    private var manualCaptureOwner: String?
    private weak var scrollView: UIScrollView?
    private weak var focusOwnerScrollView: UIScrollView?
    private let associatedFocusTargets = NSMapTable<UIView, UIView>.weakToWeakObjects()
    private var scrollingScopeKey: String?
    private var axes: MenuControllerScrollAxes = .vertical
    private var isEnabled = true
    private var pointsPerSecond: CGFloat = 760
    private var vector = MenuControllerScrollVector.zero
    private var displayLink: CADisplayLink?
    private var lastTimestamp: CFTimeInterval?
    private var registeredPriority: Int?
    private var onReachedLeadingEdge: (@MainActor () -> Void)?
    private var notifiedLeadingEdge = false
    private var searchesNearbyScrollViews = false
    private var preferredScrollViewProvider:
        (@MainActor () -> UIScrollView?)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToSuperview() {
        super.didMoveToSuperview()
        scrollView = nil
        controllerInput?.rightStickTargetHierarchyDidChange()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        scrollView = nil
        controllerInput?.rightStickTargetHierarchyDidChange()
    }

    func configure(
        controllerInput: MenuControllerInputRouter?,
        navigationSession: ControllerAccessibilityNavigationSession?,
        ownerSessionID: UUID?,
        manualCaptureOwner: String?,
        axes: MenuControllerScrollAxes,
        priority: Int,
        isEnabled: Bool,
        pointsPerSecond: CGFloat,
        onReachedLeadingEdge: (@MainActor () -> Void)?,
        searchesNearbyScrollViews: Bool,
        preferredScrollViewProvider:
            (@MainActor () -> UIScrollView?)?
    ) {
        if self.navigationSession !== navigationSession {
            self.navigationSession?.unregisterScrollSurface(self)
            self.navigationSession = navigationSession
        }
        // Passive registration only: never focus or scroll during a SwiftUI
        // representable update. Resolution is lazy and shares the analog cache.
        navigationSession?.registerScrollSurface(self)
        let routerChanged = self.controllerInput !== controllerInput
        let axesChanged = self.axes != axes
        let scrollLookupChanged = self.searchesNearbyScrollViews
            != searchesNearbyScrollViews
        let registrationChanged = routerChanged
            || registeredPriority != priority
            || self.ownerSessionID != ownerSessionID
            || self.manualCaptureOwner != manualCaptureOwner
            || self.isEnabled != isEnabled
            || axesChanged
            || scrollLookupChanged
        if routerChanged {
            self.controllerInput?.unregisterRightStickTarget(
                id: registrationID
            )
            self.controllerInput = controllerInput
        }
        if axesChanged || scrollLookupChanged {
            scrollView = nil
            stopScrolling()
        }
        if self.ownerSessionID != ownerSessionID || self.isEnabled != isEnabled {
            associatedFocusTargets.removeAllObjects()
            focusOwnerScrollView = nil
            scrollView = nil
        }
        self.axes = axes
        self.ownerSessionID = ownerSessionID
        self.manualCaptureOwner = manualCaptureOwner
        self.isEnabled = isEnabled
        self.pointsPerSecond = pointsPerSecond
        self.onReachedLeadingEdge = onReachedLeadingEdge
        self.searchesNearbyScrollViews = searchesNearbyScrollViews
        self.preferredScrollViewProvider = preferredScrollViewProvider

        if registrationChanged {
            registeredPriority = priority
            controllerInput?.registerRightStickTarget(
                id: registrationID,
                priority: priority,
                ownerSessionID: ownerSessionID,
                manualCaptureOwner: manualCaptureOwner
            ) { [weak self] vector in
                self?.consume(vector) ?? false
            }
        }
        if !isEnabled {
            stopScrolling()
        }
    }

    func disconnect() {
        navigationSession?.unregisterScrollSurface(self)
        navigationSession = nil
        controllerInput?.unregisterRightStickTarget(id: registrationID)
        controllerInput = nil
        registeredPriority = nil
        stopScrolling()
    }

    /// A fixed footer or a target in another Per-Game column must not acquire this Form's scroll
    /// owner just because both share the same UIWindow.
    func scrollViewForFocusTarget(_ target: UIView) -> UIScrollView? {
        guard isEnabled, axes.contains(.vertical), searchesNearbyScrollViews,
              preferredScrollViewProvider == nil,
              let window, target.window === window, isEffectivelyVisible(self),
              let candidate = enclosingScrollView(for: nil) else { return nil }
        if focusOwnerScrollView !== candidate {
            associatedFocusTargets.removeAllObjects()
            focusOwnerScrollView = candidate
        }
        let markerFrame = convert(bounds, to: window)
        let viewport = markerFrame.intersection(
            candidate.convert(candidate.bounds, to: window)
        )
        let frame = target.convert(target.bounds, to: window)
        guard !viewport.isNull, let parent = target.superview,
              frame.maxX > viewport.minX, frame.minX < viewport.maxX else { return nil }
        if frame.intersects(viewport) {
            associatedFocusTargets.setObject(parent, forKey: target)
        } else if associatedFocusTargets.object(forKey: target) !== parent {
            return nil
        }
        // The association outlives vertical visibility: a glass portal can stay mounted after its
        // row leaves the viewport, and scrolling back still needs this owner. Weak parent identity
        // and column bounds stop a reparented or sibling control from reusing it.
        return candidate
    }

    private func consume(_ vector: MenuControllerScrollVector) -> Bool {
        if vector.isZero {
            stopScrolling()
            return false
        }
        let resolvedVector = mappedVector(vector)
        guard isEnabled,
              !resolvedVector.isZero,
              let scrollView = enclosingScrollView(for: resolvedVector),
              isEffectivelyVisible(scrollView),
              canScroll(scrollView, for: resolvedVector) else {
            stopScrolling()
            return false
        }

        self.vector = resolvedVector
        scrollingScopeKey = controllerInput?.navigationScope(for: ownerSessionID)
        notifiedLeadingEdge = false
        startDisplayLinkIfNeeded()
        return true
    }

    private func isEffectivelyVisible(_ candidate: UIView) -> Bool {
        guard let window = candidate.window else { return false }
        var view: UIView? = candidate
        while let current = view {
            if current.isHidden
                || current.alpha < 0.01
                || current.accessibilityElementsHidden {
                return false
            }
            view = current.superview
        }
        let visibleFrame = candidate.convert(candidate.bounds, to: window)
        return visibleFrame.width > 0
            && visibleFrame.height > 0
            && visibleFrame.intersects(window.bounds)
    }

    private func enclosingScrollView(
        for vector: MenuControllerScrollVector?
    ) -> UIScrollView? {
        // A managed navigation session knows the exact scroll ancestor of its
        // focused probe. Prefer it over hierarchy-wide searches, which can
        // select a sibling Form/List after SwiftUI lazily recycles rows.
        if let preferred = preferredScrollViewProvider?(),
           preferred.window != nil,
           isEffectivelyVisible(preferred),
           canScroll(preferred, for: nil) {
            scrollView = preferred
            return preferred
        }
        if let scrollView,
           scrollView.window != nil,
           isEffectivelyVisible(scrollView),
           canScroll(scrollView, for: nil),
           (isDescendant(of: scrollView)
                || isNearbyScrollView(scrollView)) {
            return scrollView
        }
        scrollView = nil
        var ancestorCandidates: [UIScrollView] = []
        var view = superview
        while let current = view {
            if let candidate = current as? UIScrollView,
               isEffectivelyVisible(candidate),
               canScroll(candidate, for: vector) {
                ancestorCandidates.append(candidate)
            }
            view = current.superview
        }
        if let resolved = preferredCandidate(from: ancestorCandidates) {
            scrollView = resolved
            return resolved
        }
        guard searchesNearbyScrollViews else { return nil }
        let resolved = nearbyScrollView(for: vector)
        scrollView = resolved
        return resolved
    }

    /// SwiftUI installs a Form/List background beside its private scroll view,
    /// not inside it. Walk only the marker's nearest visible hosting ancestors
    /// and select the vertical surface with the greatest viewport overlap.
    private func nearbyScrollView(
        for vector: MenuControllerScrollVector?
    ) -> UIScrollView? {
        guard searchesNearbyScrollViews, let window else { return nil }
        let markerFrame = convert(bounds, to: window)
        guard markerFrame.width > 1, markerFrame.height > 1 else { return nil }
        let markerCenter = CGPoint(x: markerFrame.midX, y: markerFrame.midY)
        var visited = Set<ObjectIdentifier>()
        var ancestor = superview

        for _ in 0..<8 {
            guard let root = ancestor, root !== window else { break }
            var candidates: [UIScrollView] = []
            func visit(_ view: UIView) {
                guard visited.insert(ObjectIdentifier(view)).inserted,
                      !view.isHidden,
                      view.alpha >= 0.01,
                      !view.accessibilityElementsHidden,
                      view.window === window else { return }
                let frame = view.convert(view.bounds, to: window)
                guard frame.intersects(markerFrame) else { return }
                for subview in view.subviews.reversed() { visit(subview) }
                if let candidate = view as? UIScrollView,
                   candidate.isScrollEnabled,
                   candidate.isUserInteractionEnabled,
                   canScroll(candidate, for: vector) {
                    candidates.append(candidate)
                }
            }
            visit(root)

            if let target = candidates.max(by: { lhs, rhs in
                let lhsFrame = lhs.convert(lhs.bounds, to: window)
                let rhsFrame = rhs.convert(rhs.bounds, to: window)
                let lhsContains = lhsFrame.contains(markerCenter)
                let rhsContains = rhsFrame.contains(markerCenter)
                if lhsContains != rhsContains { return !lhsContains }
                let lhsIntersection = lhsFrame.intersection(markerFrame)
                let rhsIntersection = rhsFrame.intersection(markerFrame)
                let lhsArea = lhsIntersection.isNull
                    ? 0 : lhsIntersection.width * lhsIntersection.height
                let rhsArea = rhsIntersection.isNull
                    ? 0 : rhsIntersection.width * rhsIntersection.height
                return lhsArea < rhsArea
            }) {
                return target
            }
            ancestor = root.superview
        }
        return nil
    }

    private func preferredCandidate(
        from candidates: [UIScrollView]
    ) -> UIScrollView? {
        guard let window else { return candidates.last }
        return candidates.max { lhs, rhs in
            let lhsFrame = lhs.convert(lhs.bounds, to: window)
                .intersection(window.bounds)
            let rhsFrame = rhs.convert(rhs.bounds, to: window)
                .intersection(window.bounds)
            let lhsArea = lhsFrame.isNull ? 0 : lhsFrame.width * lhsFrame.height
            let rhsArea = rhsFrame.isNull ? 0 : rhsFrame.width * rhsFrame.height
            if abs(lhsArea - rhsArea) > 0.5 { return lhsArea < rhsArea }
            return scrollableRange(of: lhs) < scrollableRange(of: rhs)
        }
    }

    private func isNearbyScrollView(_ candidate: UIScrollView) -> Bool {
        guard searchesNearbyScrollViews,
              let window,
              candidate.window === window else { return false }
        let markerFrame = convert(bounds, to: window)
        let candidateFrame = candidate.convert(candidate.bounds, to: window)
        return markerFrame.intersects(candidateFrame)
    }

    private func startDisplayLinkIfNeeded() {
        if let displayLink {
            displayLink.isPaused = false
            return
        }
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
        displayLink?.invalidate()
        displayLink = nil
        lastTimestamp = nil
    }

    private func pauseScrollingAtBoundary() {
        displayLink?.isPaused = true
        lastTimestamp = nil
        controllerInput?.rightStickDriverStopped(targetID: registrationID)
    }

    @objc private func step(_ link: CADisplayLink) {
        guard isEnabled,
              controllerInput?.ownsRightStickScroll(
                  targetID: registrationID, ownerSessionID: ownerSessionID,
                  ownerScopeKey: scrollingScopeKey
              ) == true,
              let scrollView = enclosingScrollView(for: nil),
              isEffectivelyVisible(scrollView) else {
            stopScrolling()
            controllerInput?.rightStickTargetBecameUnavailable(
                id: registrationID
            )
            return
        }

        let previousTimestamp = lastTimestamp ?? link.timestamp
        let elapsed = min(1.0 / 30.0, link.timestamp - previousTimestamp)
        lastTimestamp = link.timestamp

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
        let acceleration = controllerInput?.rightStickScrollAcceleration(
            at: link.timestamp
        ) ?? 1
        var offset = scrollView.contentOffset
        if axes.contains(.horizontal) {
            offset.x = min(
                maximumX,
                max(
                    minimumX,
                    offset.x + CGFloat(vector.x) * pointsPerSecond
                        * acceleration * elapsed
                )
            )
        }
        if axes.contains(.vertical) {
            offset.y = min(
                maximumY,
                max(
                    minimumY,
                    offset.y - CGFloat(vector.y) * pointsPerSecond
                        * acceleration * elapsed
                )
            )
        }
        scrollView.setContentOffset(offset, animated: false)

        controllerInput?.didScrollRightStick(
            in: scrollView,
            direction: axes.contains(.vertical) && vector.y != 0
                ? (vector.y > 0 ? .up : .down)
                : (vector.x < 0 ? .left : .right),
            targetID: registrationID,
            ownerSessionID: ownerSessionID,
            ownerScopeKey: scrollingScopeKey
        )

        let movingTowardLeadingEdge = (
            axes.contains(.horizontal)
                && CGFloat(vector.x) < 0
                && offset.x <= minimumX + 0.75
        ) || (
            axes.contains(.vertical)
                && CGFloat(vector.y) > 0
                && offset.y <= minimumY + 0.75
        )
        let movingTowardTrailingEdge = (
            axes.contains(.horizontal)
                && CGFloat(vector.x) > 0
                && offset.x >= maximumX - 0.75
        ) || (
            axes.contains(.vertical)
                && CGFloat(vector.y) < 0
                && offset.y >= maximumY - 0.75
        )
        if movingTowardLeadingEdge, !notifiedLeadingEdge {
            notifiedLeadingEdge = true
            pauseScrollingAtBoundary()
            onReachedLeadingEdge?()
        } else if movingTowardTrailingEdge {
            pauseScrollingAtBoundary()
        } else if !movingTowardLeadingEdge {
            notifiedLeadingEdge = false
        }
    }

    private func mappedVector(
        _ input: MenuControllerScrollVector
    ) -> MenuControllerScrollVector {
        if axes == .horizontal {
            let horizontal = abs(input.x) >= abs(input.y)
                ? input.x
                : -input.y
            return MenuControllerScrollVector(x: horizontal, y: 0)
        }
        if axes == .vertical {
            let vertical = abs(input.y) >= abs(input.x)
                ? input.y
                : -input.x
            return MenuControllerScrollVector(x: 0, y: vertical)
        }
        return input
    }

    private func hasScrollableRange(_ scrollView: UIScrollView) -> Bool {
        let inset = scrollView.adjustedContentInset
        let horizontalRange = scrollView.contentSize.width
            - scrollView.bounds.width + inset.left + inset.right
        let verticalRange = scrollView.contentSize.height
            - scrollView.bounds.height + inset.top + inset.bottom
        return (axes.contains(.horizontal) && horizontalRange > 0.5)
            || (axes.contains(.vertical) && verticalRange > 0.5)
    }

    private func scrollableRange(of scrollView: UIScrollView) -> CGFloat {
        let inset = scrollView.adjustedContentInset
        let horizontalRange = max(
            0,
            scrollView.contentSize.width - scrollView.bounds.width
                + inset.left + inset.right
        )
        let verticalRange = max(
            0,
            scrollView.contentSize.height - scrollView.bounds.height
                + inset.top + inset.bottom
        )
        if axes == .horizontal { return horizontalRange }
        if axes == .vertical { return verticalRange }
        return max(horizontalRange, verticalRange)
    }

    private func canScroll(
        _ scrollView: UIScrollView,
        for vector: MenuControllerScrollVector?
    ) -> Bool {
        guard hasScrollableRange(scrollView) else { return false }
        // Focus geometry still needs the owner at either content boundary,
        // even when that owner cannot consume another analog step.
        guard let vector else { return true }
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
        let epsilon: CGFloat = 0.5

        if axes.contains(.horizontal), vector.x != 0 {
            return vector.x < 0
                ? scrollView.contentOffset.x > minimumX + epsilon
                : scrollView.contentOffset.x < maximumX - epsilon
        }
        guard axes.contains(.vertical), vector.y != 0 else { return false }
        // Vertical targets subtract the stick vector from contentOffset in
        // `step`: positive moves toward the leading edge, negative toward the
        // trailing edge.
        return vector.y > 0
            ? scrollView.contentOffset.y > minimumY + epsilon
            : scrollView.contentOffset.y < maximumY - epsilon
    }
}
