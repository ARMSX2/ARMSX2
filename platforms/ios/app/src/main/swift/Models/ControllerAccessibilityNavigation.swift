// ControllerAccessibilityNavigation.swift — native-first controller focus
// SPDX-License-Identifier: GPL-3.0+

import Observation
import QuartzCore
import SwiftUI
import UIKit

let controllerImportBIOSToolbarTargetLabel = "controller.toolbar.import-bios"

struct ControllerAccessibilityDirectionalLink: Equatable {
    static let scrollBoundary = "controller.scroll-boundary"
    static let firstContent = "controller.first-content"
    static let navigationBoundary = "controller.navigation-boundary"

    let fromLabel: String
    let direction: MenuControllerCommand
    let toLabel: String
}

struct ControllerAccessibilityBoundaryRule: Equatable {
    let direction: MenuControllerCommand
    let fromLabel: String
    var requiresFreshPress: Bool = false
}

/// Geometry-only ranking shared by live analog flow and its final selection.
/// Fully visible controls win; ties keep semantic order.
struct ControllerNavigationScrollFocusCandidate {
    let key: String
    let frame: CGRect
    let order: Int

    static func preferredKey(
        in candidates: [Self],
        viewport: CGRect,
        direction: MenuControllerCommand
    ) -> String? {
        let horizontal = direction == .left || direction == .right
        let towardStart = direction == .up || direction == .left
        let ranked = candidates.compactMap { candidate -> (
            candidate: Self, fullyVisible: Bool, hidden: CGFloat, distance: CGFloat
        )? in
            let frame = candidate.frame
            let intersection = frame.intersection(viewport)
            guard !intersection.isNull, !intersection.isInfinite,
                  intersection.width > 1, intersection.height > 1 else { return nil }
            let extent = horizontal ? frame.width : frame.height
            let visible = horizontal ? intersection.width : intersection.height
            let start = horizontal ? frame.minX : frame.minY
            let end = horizontal ? frame.maxX : frame.maxY
            let viewportStart = horizontal ? viewport.minX : viewport.minY
            let viewportEnd = horizontal ? viewport.maxX : viewport.maxY
            return (
                candidate, visible >= extent - 1, max(0, extent - visible),
                towardStart ? abs(start - viewportStart) : abs(end - viewportEnd)
            )
        }
        return ranked.min { lhs, rhs in
            if lhs.fullyVisible != rhs.fullyVisible { return lhs.fullyVisible }
            if !lhs.fullyVisible, abs(lhs.hidden - rhs.hidden) > 0.5 {
                return lhs.hidden < rhs.hidden
            }
            if abs(lhs.distance - rhs.distance) > 0.5 {
                return lhs.distance < rhs.distance
            }
            return lhs.candidate.order < rhs.candidate.order
        }?.candidate.key
    }
}

enum ControllerAccessibilityFocusScrollBehavior: Equatable {
    /// Preserve the current viewport while the focused row remains visible.
    case revealIfNeeded
    /// Keep the focused row inside configurable top and bottom boundaries.
    /// Scroll only when it crosses an edge, retaining its natural travel
    /// through the viewport instead of pinning every row to the top.
    case maintainWithinViewport

    fileprivate var anchor: UnitPoint {
        switch self {
        case .revealIfNeeded: return .center
        case .maintainWithinViewport: return .center
        }
    }
}

private struct ControllerAccessibilityNavigationActiveKey: EnvironmentKey {
    static let defaultValue = false
}

private struct ControllerAccessibilityNavigationSessionKey: EnvironmentKey {
    static let defaultValue: ControllerAccessibilityNavigationSession? = nil
}

private struct ControllerAccessibilityNavigationRegistrationIDKey:
    EnvironmentKey {
    static let defaultValue: UUID? = nil
}

private struct ControllerAccessibilityTargetsSuppressedKey: EnvironmentKey {
    static let defaultValue = false
}

private struct ControllerAccessibilityTargetFocusedKey: EnvironmentKey {
    static let defaultValue = false
}

private struct ControllerAccessibilityInheritedTargetIDKey: EnvironmentKey {
    static let defaultValue: String? = nil
}

private struct ControllerAccessibilityWindowFocusVisualKey: EnvironmentKey {
    static let defaultValue = false
}

private struct ControllerAccessibilityAutomaticTargetSuppressedKey:
    EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var controllerAccessibilityNavigationActive: Bool {
        get { self[ControllerAccessibilityNavigationActiveKey.self] }
        set { self[ControllerAccessibilityNavigationActiveKey.self] = newValue }
    }

    var controllerAccessibilityNavigationSession: ControllerAccessibilityNavigationSession? {
        get { self[ControllerAccessibilityNavigationSessionKey.self] }
        set { self[ControllerAccessibilityNavigationSessionKey.self] = newValue }
    }

    var controllerAccessibilityNavigationRegistrationID: UUID? {
        get { self[ControllerAccessibilityNavigationRegistrationIDKey.self] }
        set {
            self[ControllerAccessibilityNavigationRegistrationIDKey.self] = newValue
        }
    }

    var controllerAccessibilityTargetsSuppressed: Bool {
        get { self[ControllerAccessibilityTargetsSuppressedKey.self] }
        set { self[ControllerAccessibilityTargetsSuppressedKey.self] = newValue }
    }

    var controllerAccessibilityTargetFocused: Bool {
        get { self[ControllerAccessibilityTargetFocusedKey.self] }
        set { self[ControllerAccessibilityTargetFocusedKey.self] = newValue }
    }

    var controllerAccessibilityInheritedTargetID: String? {
        get { self[ControllerAccessibilityInheritedTargetIDKey.self] }
        set { self[ControllerAccessibilityInheritedTargetIDKey.self] = newValue }
    }

    var controllerAccessibilityUsesWindowFocusVisual: Bool {
        get { self[ControllerAccessibilityWindowFocusVisualKey.self] }
        set { self[ControllerAccessibilityWindowFocusVisualKey.self] = newValue }
    }

    var controllerAccessibilityAutomaticTargetSuppressed: Bool {
        get { self[ControllerAccessibilityAutomaticTargetSuppressedKey.self] }
        set {
            self[ControllerAccessibilityAutomaticTargetSuppressedKey.self] = newValue
        }
    }
}

@MainActor
@Observable
fileprivate final class ControllerAccessibilityTargetFocusState {
    var isFocused = false
}

/// A List/Form marker remains beside its real scroll surface even when Liquid
/// Glass portals a row's controls out of the collection-view hierarchy.
@MainActor
protocol ControllerAccessibilityScrollSurface: AnyObject {
    func scrollViewForFocusTarget(_ target: UIView) -> UIScrollView?
}

@MainActor
private final class ControllerAccessibilityDisplayLinkDriver: NSObject {
    var onFrame: ((CFTimeInterval) -> Void)?

    @objc func displayLinkDidFire(_ displayLink: CADisplayLink) {
        onFrame?(displayLink.timestamp)
    }
}

@MainActor
@Observable
final class ControllerAccessibilityNavigationSession {
    private final class WeakView {
        weak var value: UIView?
        init(_ value: UIView?) { self.value = value }
    }

    private struct Target {
        let key: String
        let baseKey: String
        let registrationID: UUID
        var navigationID: String?
        var label: String
        var value: String?
        var traits: UIAccessibilityTraits
        var view: WeakView
        var focusState: ControllerAccessibilityTargetFocusState
        var isEnabled: Bool
        var isPersistent: Bool
        var registrationOrder: UInt64
        var lastFrame: CGRect?
        var lastContentY: CGFloat?
        var onActivate: (() -> Void)?
        var onIncrement: (() -> Void)?
        var onDecrement: (() -> Void)?
        var activationFeedback: MenuControllerFeedback
        var focusedNeonCornerRadius: CGFloat?
    }

    private struct FocusPresentation: Equatable {
        var frame: CGRect?
        var overlayFrame: CGRect?
        var accessibilityLabel: String?

        static let empty = FocusPresentation(
            frame: nil,
            overlayFrame: nil,
            accessibilityLabel: nil
        )
    }

    private struct FocusMotion {
        let sourceFrame: CGRect
        let duration: TimeInterval
        let style: ControllerNavigationFocusTravelStyle
        weak var scrollView: UIScrollView?
        let sourceOffset: CGPoint
        let destinationOffset: CGPoint
    }

    @ObservationIgnored private weak var controllerInput: MenuControllerInputRouter?
    @ObservationIgnored private weak var scopeView: UIView?
    @ObservationIgnored private weak var window: UIWindow?
    @ObservationIgnored private var scopeFrame = CGRect.zero
    @ObservationIgnored private var scopeKey = ""
    @ObservationIgnored private var registrationID: UUID?
    @ObservationIgnored private var targets: [String: Target] = [:]
    @ObservationIgnored private var registrationKeys: [UUID: String] = [:]
    @ObservationIgnored private let scrollSurfaces = NSHashTable<AnyObject>.weakObjects()
    @ObservationIgnored private var duplicateBaseKeys = Set<String>()
    @ObservationIgnored private var registrationSequence: UInt64 = 0
    @ObservationIgnored private var declaredOrder: [String] = []
    @ObservationIgnored private var declaredOrderSet = Set<String>()
    @ObservationIgnored private var declaredOrderIndex: [String: Int] = [:]
    @ObservationIgnored private var focusedKey: String?
    @ObservationIgnored private var permitsAutomaticFocus = true
    @ObservationIgnored private var scrollToTarget: ((String, UnitPoint) -> Void)?
    @ObservationIgnored private var onBack: (@MainActor () -> Bool)?
    @ObservationIgnored private var onPreviousTab: (@MainActor () -> Bool)?
    @ObservationIgnored private var onNextTab: (@MainActor () -> Bool)?
    @ObservationIgnored private var onBoundary: (@MainActor (MenuControllerCommand) -> Bool)?
    @ObservationIgnored private var boundaryRules: [ControllerAccessibilityBoundaryRule] = []
    @ObservationIgnored private var directionalLinks: [ControllerAccessibilityDirectionalLink] = []
    @ObservationIgnored private var prioritizesDirectionalLinks = false
    @ObservationIgnored private var confinesHorizontalFocusMovement = false
    @ObservationIgnored private var focusScrollBehavior:
        ControllerAccessibilityFocusScrollBehavior = .revealIfNeeded
    @ObservationIgnored private var scrollAnimationDuration: Double = 0.22
    @ObservationIgnored private var focusViewportEdgeMargin: CGFloat = 12
    @ObservationIgnored private var focusTopAlignmentMargin: CGFloat = 0
    @ObservationIgnored private var focusBottomAlignmentMargin: CGFloat = 0
    @ObservationIgnored private var onActivateFocusedLabel: (@MainActor (String) -> Bool)?
    @ObservationIgnored private var onAdjustFocusedTarget:
        (@MainActor (String, String?, Bool) -> Void)?
    @ObservationIgnored private var preferredInitialFocusLabel: String?
    @ObservationIgnored private var preferredLastEntryLabel: String?
    @ObservationIgnored private var preferredTrailingFocusLabels: [String] = []
    @ObservationIgnored private weak var pendingNativeFocusView: UIView?
    @ObservationIgnored private var nativeFocusRequestTask: Task<Void, Never>?
    @ObservationIgnored private var nativeFocusLossTask: Task<Void, Never>?
    @ObservationIgnored private var mountedTargetTask: Task<Void, Never>?
    @ObservationIgnored private var focusedPresentationTask: Task<Void, Never>?
    @ObservationIgnored private var focusPresentationRevealTask:
        Task<Void, Never>?
    @ObservationIgnored private var scrollPresentationDisplayLink: CADisplayLink?
    @ObservationIgnored private var scrollPresentationDriver:
        ControllerAccessibilityDisplayLinkDriver?
    @ObservationIgnored private var scrollPresentationKey: String?
    @ObservationIgnored private var scrollPresentationStartTime: CFTimeInterval = 0
    @ObservationIgnored private var scrollPresentationLastFrame: CGRect?
    @ObservationIgnored private var scrollPresentationStableFrameCount = 0
    @ObservationIgnored private var focusMotion: FocusMotion?
    @ObservationIgnored private var scrollPresentationCompletionTask:
        Task<Void, Never>?
    @ObservationIgnored private var readinessTask: Task<Void, Never>?
    @ObservationIgnored private var pendingFocusKey: String?
    // Preserve input timing across the asynchronous lazy-row mount.
    @ObservationIgnored private var focusRepeatAcceleration: Double?
    @ObservationIgnored private var pendingDirectionalMove:
        (direction: MenuControllerCommand, acceleration: Double?)?
    @ObservationIgnored private var pendingFocusExpiryTask: Task<Void, Never>?
    @ObservationIgnored private var scopeRestorationTask: Task<Void, Never>?
    @ObservationIgnored private var adjustedValuePublicationTask:
        Task<Void, Never>?
    @ObservationIgnored private var pendingScrollFocusPublication = false
    @ObservationIgnored private var focusIsSuspendedForScrolling = false
    @ObservationIgnored private weak var rightStickScrollView: UIScrollView?
    @ObservationIgnored private var rightStickManualFocusKey: String?
    @ObservationIgnored private var rightStickFocusCandidates: [ControllerNavigationScrollFocusCandidate]?
    @ObservationIgnored private var rightStickViewportSize = CGSize.zero
    @ObservationIgnored private weak var publishedScrollWindow: UIWindow?
    @ObservationIgnored private var publishedScrollFrame: CGRect?
    @ObservationIgnored private var presentedFocusKey: String?
    private(set) var focusPresentationIsHiddenForScrolling = false
    @ObservationIgnored private var interactionSequence: UInt64 = 0

    private var focusPresentation = FocusPresentation.empty
    private(set) var latestOrbInteraction: ControllerNavigationOrbInteraction?
    private(set) var hasNavigableElements = false
    private(set) var scopeRevision: UInt64 = 0
    private(set) var isScrollPresentationActive = false

    var registrationIdentifier: UUID? { registrationID }
    var focusedElementID: String? { focusedKey }
    var focusedFrame: CGRect? { focusPresentation.frame }
    var focusedOverlayFrame: CGRect? { focusPresentation.overlayFrame }
    var focusedAccessibilityLabel: String? {
        focusPresentation.accessibilityLabel
    }
    var focusedNeonCornerRadius: CGFloat? {
        focusedKey.flatMap { targets[$0]?.focusedNeonCornerRadius }
    }
    func configure(
        controllerInput: MenuControllerInputRouter?,
        scopeKey: String,
        registrationID: UUID,
        onBack: (@MainActor () -> Bool)?,
        onPreviousTab: (@MainActor () -> Bool)?,
        onNextTab: (@MainActor () -> Bool)?,
        onBoundary: (@MainActor (MenuControllerCommand) -> Bool)?,
        boundaryRules: [ControllerAccessibilityBoundaryRule],
        directionalLinks: [ControllerAccessibilityDirectionalLink],
        prioritizesDirectionalLinks: Bool,
        confinesHorizontalFocusMovement: Bool,
        focusScrollBehavior: ControllerAccessibilityFocusScrollBehavior,
        scrollAnimationDuration: Double,
        focusViewportEdgeMargin: CGFloat,
        focusTopAlignmentMargin: CGFloat,
        focusBottomAlignmentMargin: CGFloat,
        onActivateFocusedLabel: (@MainActor (String) -> Bool)?,
        onAdjustFocusedTarget: (@MainActor (String, String?, Bool) -> Void)?,
        preferredInitialFocusLabel: String?,
        preferredLastEntryLabel: String?,
        preferredTrailingFocusLabels: [String]
    ) {
        self.controllerInput = controllerInput
        self.registrationID = registrationID
        self.onBack = onBack
        self.onPreviousTab = onPreviousTab
        self.onNextTab = onNextTab
        self.onBoundary = onBoundary
        self.boundaryRules = boundaryRules
        self.directionalLinks = directionalLinks
        self.prioritizesDirectionalLinks = prioritizesDirectionalLinks
        self.confinesHorizontalFocusMovement = confinesHorizontalFocusMovement
        self.focusScrollBehavior = focusScrollBehavior
        self.scrollAnimationDuration = max(0, scrollAnimationDuration)
        self.focusViewportEdgeMargin = max(0, focusViewportEdgeMargin)
        self.focusTopAlignmentMargin = max(0, focusTopAlignmentMargin)
        self.focusBottomAlignmentMargin = max(0, focusBottomAlignmentMargin)
        self.onActivateFocusedLabel = onActivateFocusedLabel
        self.onAdjustFocusedTarget = onAdjustFocusedTarget
        self.preferredInitialFocusLabel = preferredInitialFocusLabel
        self.preferredLastEntryLabel = preferredLastEntryLabel
        self.preferredTrailingFocusLabels = preferredTrailingFocusLabels

        guard scopeKey != self.scopeKey else { return }
        if self.scopeKey.isEmpty {
            self.scopeKey = scopeKey
            scopeRevision &+= 1
            if let rememberedKey = controllerInput?
                .rememberedNavigationFocusKey(forScope: scopeKey) {
                queueScopeFocusRestoration(rememberedKey)
            }
            return
        }
        rememberCurrentFocus()
        clearFocus()
        self.scopeKey = scopeKey
        scopeRevision &+= 1
        targets.removeAll(keepingCapacity: true)
        registrationKeys.removeAll(keepingCapacity: true)
        duplicateBaseKeys.removeAll(keepingCapacity: true)
        declaredOrder.removeAll(keepingCapacity: true)
        declaredOrderSet.removeAll(keepingCapacity: true)
        declaredOrderIndex.removeAll(keepingCapacity: true)
        hasNavigableElements = false
        rightStickScrollView = nil
        rightStickManualFocusKey = nil
        if let rememberedKey = controllerInput?
            .rememberedNavigationFocusKey(forScope: scopeKey) {
            queueScopeFocusRestoration(rememberedKey)
        }
        updateReadiness()
    }

    func setScrollAction(_ action: @escaping (String, UnitPoint) -> Void) {
        scrollToTarget = action
    }

    func clearScrollAction() { scrollToTarget = nil }

    func registerScrollSurface(_ surface: any ControllerAccessibilityScrollSurface) {
        scrollSurfaces.add(surface)
    }

    func unregisterScrollSurface(_ surface: any ControllerAccessibilityScrollSurface) {
        scrollSurfaces.remove(surface)
    }

    func setDeclaredOrder(_ navigationIDs: [String]) {
        let next = navigationIDs.compactMap { raw -> String? in
            let value = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            return value.isEmpty ? nil : Self.explicitKey(value)
        }
        guard next != declaredOrder else { return }
        declaredOrder = next
        declaredOrderSet = Set(next)
        var nextIndex: [String: Int] = [:]
        for (index, key) in next.enumerated() where nextIndex[key] == nil {
            nextIndex[key] = index
        }
        declaredOrderIndex = nextIndex
        rightStickFocusCandidates = nil
        if let pendingFocusKey, !next.contains(pendingFocusKey) {
            self.pendingFocusKey = nil
            pendingFocusExpiryTask?.cancel()
            pendingFocusExpiryTask = nil
        } else if let pendingFocusKey, next.contains(pendingFocusKey) {
            scheduleScopeRestorationSeek(pendingFocusKey)
        }
        updateReadiness()
    }

    func updateScope(view: UIView) {
        let nextWindow = view.window
        let nextFrame = nextWindow.map { view.convert(view.bounds, to: $0) }
            ?? .zero
        guard scopeView !== view || window !== nextWindow
                || scopeFrame != nextFrame else { return }
        scopeView = view
        window = nextWindow
        scopeFrame = nextFrame
        scheduleFocusedPresentationUpdate()
    }

    func deactivate() {
        rememberCurrentFocus()
        cancelNativeFocusRequest()
        cancelNativeFocusLoss()
        mountedTargetTask?.cancel()
        mountedTargetTask = nil
        focusedPresentationTask?.cancel()
        focusedPresentationTask = nil
        focusPresentationRevealTask?.cancel()
        focusPresentationRevealTask = nil
        stopScrollPresentationTracking(publishesFinalFrame: false)
        scrollPresentationCompletionTask?.cancel()
        scrollPresentationCompletionTask = nil
        isScrollPresentationActive = false
        readinessTask?.cancel()
        readinessTask = nil
        pendingFocusExpiryTask?.cancel()
        pendingFocusExpiryTask = nil
        scopeRestorationTask?.cancel()
        scopeRestorationTask = nil
        adjustedValuePublicationTask?.cancel()
        adjustedValuePublicationTask = nil
        pendingFocusKey = nil
        pendingScrollFocusPublication = false
        focusIsSuspendedForScrolling = false
        rightStickScrollView = nil
        focusPresentationIsHiddenForScrolling = false
        latestOrbInteraction = nil
        clearFocus()
        targets.removeAll(keepingCapacity: false)
        registrationKeys.removeAll(keepingCapacity: false)
        duplicateBaseKeys.removeAll(keepingCapacity: false)
        declaredOrder.removeAll(keepingCapacity: false)
        declaredOrderSet.removeAll(keepingCapacity: false)
        declaredOrderIndex.removeAll(keepingCapacity: false)
        scopeView = nil
        window = nil
        scopeFrame = .zero
        // The mounted modifier still owns its ScrollViewReader. Its onDisappear
        // clears this callback; temporary deactivation must not discard it.
        permitsAutomaticFocus = true
        updateReadiness()
    }

    func suspendFocusForScrolling(preservingPresentation: Bool = false) {
        pendingDirectionalMove = nil
        focusRepeatAcceleration = nil
        guard !ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled else {
            publishScrollFocus()
            return
        }
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
        if let focusedKey { setFocus(focusedKey, requestsNativeFocus: false) }
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

    func publishScrollFocus() {
        guard let registrationID else { return }
        let frame = focusedFrame
        if publishedScrollWindow === window,
           optionalFramesApproximatelyEqual(publishedScrollFrame, frame) {
            return
        }
        publishedScrollWindow = window
        publishedScrollFrame = frame
        controllerInput?.setNavigationSessionScrollFocus(
            id: registrationID,
            window: window,
            frame: frame
        )
    }

    func synchronizeNativeFocus() {
        guard ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled else {
            return
        }
        if focusedKey == nil { _ = focusInitialIfPossible() }
        guard let focusedKey, let view = targets[focusedKey]?.view.value else {
            return
        }
        requestNativeFocus(on: view)
    }

    @discardableResult
    fileprivate func registerTarget(
        registrationID: UUID,
        view: ControllerAccessibilityActionProbeView,
        baseKey: String,
        navigationID: String?,
        label: String,
        value: String?,
        traits: UIAccessibilityTraits,
        focusState: ControllerAccessibilityTargetFocusState,
        isEnabled: Bool,
        persistent: Bool,
        activationFeedback: MenuControllerFeedback,
        focusedNeonCornerRadius: CGFloat?,
        onActivate: (() -> Void)?,
        onIncrement: (() -> Void)?,
        onDecrement: (() -> Void)?
    ) -> String {
        let resolvedKey = resolveKey(
            registrationID: registrationID,
            baseKey: baseKey,
            owner: view,
            hasUniqueIdentity: navigationID != nil
        )
        let frame = frameForView(view)
        let contentY = contentPosition(for: view)
        let existing = targets[resolvedKey]
        let order: UInt64
        if let existing {
            order = existing.registrationOrder
        } else {
            registrationSequence &+= 1
            order = registrationSequence
        }

        let ownerChanged = existing?.view.value !== view
        let availabilityChanged = existing?.isEnabled != isEnabled
        let geometryChanged = existing?.lastFrame != frame
            || existing?.lastContentY != contentY

        targets[resolvedKey] = Target(
            key: resolvedKey,
            baseKey: baseKey,
            registrationID: registrationID,
            navigationID: navigationID,
            label: label,
            value: value,
            traits: traits,
            view: WeakView(view),
            focusState: focusState,
            isEnabled: isEnabled,
            isPersistent: persistent && !duplicateBaseKeys.contains(baseKey),
            registrationOrder: order,
            lastFrame: frame,
            lastContentY: contentY,
            onActivate: onActivate,
            onIncrement: onIncrement,
            onDecrement: onDecrement,
            activationFeedback: activationFeedback,
            focusedNeonCornerRadius: focusedNeonCornerRadius
        )
        registrationKeys[registrationID] = resolvedKey
        if existing == nil || ownerChanged || availabilityChanged || geometryChanged {
            rightStickFocusCandidates = nil
        }

        // Registration occurs inside UIViewRepresentable updates while a lazy
        // List/Form may still be laying out. Keep it passive: focus, observed
        // presentation state, and scrolling are reconciled on the next actor
        // turn after the mounted view has joined a stable hierarchy.
        if existing == nil || ownerChanged || availabilityChanged {
            scheduleMountedTargetReconciliation(key: resolvedKey)
            updateReadiness()
        } else if focusedKey == resolvedKey,
                  existing?.label != label || existing?.lastFrame != frame {
            scheduleFocusedPresentationUpdate()
        }
        return resolvedKey
    }

    fileprivate func unregisterTarget(registrationID: UUID, owner: UIView) {
        guard let key = registrationKeys[registrationID],
              var target = targets[key],
              target.view.value === owner else { return }
        registrationKeys.removeValue(forKey: registrationID)
        rightStickFocusCandidates = nil
        target.focusState.isFocused = false

        if target.isPersistent {
            target.view = WeakView(nil)
            target.lastFrame = frameForView(owner) ?? target.lastFrame
            target.lastContentY = contentPosition(for: owner) ?? target.lastContentY
            targets[key] = target
        } else {
            targets.removeValue(forKey: key)
            if focusedKey == key { clearFocus() }
        }
        updateReadiness()
    }

    fileprivate func targetGeometryDidChange(
        registrationID: UUID,
        owner: UIView
    ) {
        guard let key = registrationKeys[registrationID],
              var target = targets[key],
              target.view.value === owner else { return }
        let nextFrame = frameForView(owner)
        let nextContentY = contentPosition(for: owner)
        let contentPositionChanged: Bool
        switch (target.lastContentY, nextContentY) {
        case let (previous?, next?):
            contentPositionChanged = abs(previous - next) > 0.25
        case (nil, nil):
            contentPositionChanged = false
        default:
            contentPositionChanged = true
        }

        // A vertical content-offset change moves every mounted probe in window
        // coordinates even though its content-space position and size are
        // unchanged. Directional ordering uses content-space Y, and the active
        // focus presentation samples its view directly, so publishing all of
        // those redundant row updates only competes with scrolling.
        let scrollIsMoving = isScrollPresentationActive
            || focusIsSuspendedForScrolling
            || enclosingScrollView(for: owner).map(
                scrollViewHasActivePresentation
            ) == true
        if scrollIsMoving,
           !contentPositionChanged,
           let previous = target.lastFrame,
           let next = nextFrame,
           abs(previous.minX - next.minX) <= 0.25,
           abs(previous.width - next.width) <= 0.25,
           abs(previous.height - next.height) <= 0.25 {
            return
        }
        guard target.lastFrame != nextFrame || contentPositionChanged else {
            return
        }
        if contentPositionChanged
            || target.lastFrame?.size != nextFrame?.size
            || target.lastFrame?.minX != nextFrame?.minX {
            rightStickFocusCandidates = nil
        }
        target.lastFrame = nextFrame
        target.lastContentY = nextContentY
        targets[key] = target
        if focusedKey == key {
            // A display link owns focus presentation while scrolling. Mixing
            // model-layer layout callbacks into that stream makes the outline
            // jump to the final content offset before the pixels arrive there.
            if !isScrollPresentationActive {
                scheduleFocusedPresentationUpdate(publishesScrollFocus: true)
            }
        }
    }

    fileprivate func nativeFocusChanged(
        key: String,
        isFocused: Bool,
        heading: UIFocusHeading
    ) {
        guard ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled else { return }
        if isFocused {
            cancelNativeFocusLoss()
            let changed = focusedKey != key
            guard changed else { return }
            setFocus(key, requestsNativeFocus: false, scrolls: false)
            if changed, let direction = heading.controllerCommand {
                controllerInput?.playFeedback(.move(direction))
            }
        } else if focusedKey == key {
            scheduleNativeFocusLoss(for: key)
        }
        // UIKit notifies the previous item that it lost focus before the next
        // item publishes its gain. Clearing the entire observed SwiftUI focus
        // graph during that midpoint remounts Form probes and can recursively
        // restart the same focus transaction. Defer the loss until the complete
        // UIKit transaction proves that no target in this scope gained focus.
    }

    fileprivate func activate(_ key: String) -> Bool {
        guard let target = targets[key], target.isEnabled else { return false }
        if let onActivateFocusedLabel,
           onActivateFocusedLabel(target.label) {
            controllerInput?.playFeedback(target.activationFeedback)
            return true
        }
        if let action = target.onActivate {
            action()
            controllerInput?.playFeedback(target.activationFeedback)
            return true
        }
        if let view = target.view.value,
           activateNativeControl(near: view) {
            controllerInput?.playFeedback(target.activationFeedback)
            return true
        }
        controllerInput?.playFeedback(.boundary)
        return false
    }

    fileprivate func adjust(_ key: String, increment: Bool) -> Bool {
        guard let target = targets[key], target.isEnabled else { return false }
        let action = increment ? target.onIncrement : target.onDecrement
        guard let action else { return false }
        action()
        let originalLabel = target.label
        let originalValue = target.value
        let originalNavigationID = target.navigationID
        adjustedValuePublicationTask?.cancel()
        adjustedValuePublicationTask = Task { @MainActor [weak self] in
            // Wait one display frame for the binding mutation to update the
            // target probe, then report the resulting value rather than its
            // pre-adjustment value.
            try? await Task.sleep(for: .milliseconds(16))
            guard let self, !Task.isCancelled else { return }
            let updatedTarget = self.targets[key]
                ?? self.targets.values.first {
                    originalNavigationID != nil
                        && $0.navigationID == originalNavigationID
                }
            self.onAdjustFocusedTarget?(
                updatedTarget?.label ?? originalLabel,
                updatedTarget?.value ?? originalValue,
                increment
            )
        }
        if target.value == "On" || target.value == "Off" {
            controllerInput?.playFeedback(.toggle(isOn: increment))
        } else {
            controllerInput?.playFeedback(.activate)
        }
        return true
    }

    fileprivate func handleNativeHorizontalPress(
        _ key: String,
        increment: Bool
    ) -> Bool {
        if adjust(key, increment: increment) { return true }
        guard confinesHorizontalFocusMovement else { return false }
        controllerInput?.playFeedback(.boundary)
        return true
    }

    fileprivate func permitsNativeFocusMovement(
        _ heading: UIFocusHeading
    ) -> Bool {
        guard confinesHorizontalFocusMovement else { return true }
        return !heading.contains(.left) && !heading.contains(.right)
    }

    func handle(_ command: MenuControllerCommand) -> Bool {
        permitsAutomaticFocus = true
        if controllerInput?.isRightStickScrolling != true {
            resumeFocusAfterScrollingIfNeeded()
        }
        defer {
            if command.isSpatialDirection,
               controllerInput?.isRightStickScrolling == true {
                rightStickManualFocusKey = focusedKey
            }
        }
        publishOrbInteractionIfNeeded(command)
        switch command {
        case .previousTab:
            return performChromeAction(onPreviousTab, feedback: .previousTab)
        case .nextTab:
            return performChromeAction(onNextTab, feedback: .nextTab)
        case .back:
            if onBack?() == true {
                controllerInput?.playFeedback(.back)
                return true
            }
            return false
        case .activate:
            guard let focusedKey else { return focusInitialIfPossible() }
            return activate(focusedKey)
        case .toggleFavorite, .showContextMenu:
            return false
        case .left, .right:
            if let focusedKey {
                if adjust(focusedKey, increment: command == .right) {
                    return true
                }
                // A confined settings column may still declare a deliberate
                // horizontal relationship, such as Cancel/Close <-> Save.
                // Honor that semantic edge before treating horizontal input as
                // a boundary; arbitrary spatial movement remains confined.
                if linkedDestination(
                    from: focusedKey,
                    direction: command
                ) != nil {
                    return move(command)
                }
                if confinesHorizontalFocusMovement {
                    controllerInput?.playFeedback(.boundary)
                    return true
                }
            }
            return move(command)
        case .upRight, .downRight, .downLeft, .upLeft:
            if confinesHorizontalFocusMovement,
               let vertical = command.verticalComponent {
                return move(vertical)
            }
            return move(command)
        case .up, .down:
            return move(command)
        }
    }

    func focusContent(preferLast: Bool) -> Bool {
        focusRepeatAcceleration = nil
        pendingDirectionalMove = nil
        permitsAutomaticFocus = true
        let keys = orderedKeys()
        let preferred = preferLast ? preferredLastEntryLabel : preferredInitialFocusLabel
        if let preferred, let key = keyMatching(preferred, in: keys) {
            pendingFocusKey = nil
            pendingFocusExpiryTask?.cancel()
            pendingFocusExpiryTask = nil
            focusOrSeek(key)
            return true
        }
        if let preferred {
            let preferredKey = Self.explicitKey(preferred)
            if declaredOrder.contains(preferredKey) {
                pendingFocusKey = nil
                pendingFocusExpiryTask?.cancel()
                pendingFocusExpiryTask = nil
                focusOrSeek(preferredKey)
                return true
            }
        }
        if let restorationKey = pendingFocusKey {
            if isMountedAndEnabled(restorationKey) {
                setFocus(restorationKey)
            } else {
                queueScopeFocusRestoration(restorationKey)
                if declaredOrder.contains(restorationKey) {
                    scheduleScopeRestorationSeek(restorationKey)
                }
            }
            return true
        }
        if !declaredOrder.isEmpty {
            let preferredKey = preferred.map(Self.explicitKey)
            let preferredCandidate = preferredKey.flatMap {
                declaredOrder.contains($0) ? $0 : nil
            }
            let orderedCandidates = preferLast
                ? Array(declaredOrder.reversed())
                : declaredOrder
            let key = [preferredCandidate].compactMap { $0 }.first(where: {
                targets[$0]?.isEnabled != false
            }) ?? orderedCandidates.first(where: {
                targets[$0]?.isEnabled != false
            })
            if let key {
                focusOrSeek(key)
                return true
            }
        }
        guard let key = preferLast ? keys.last : keys.first else { return false }
        focusOrSeek(key)
        return true
    }

    private func move(
        _ direction: MenuControllerCommand,
        repeatAcceleration: Double? = nil
    ) -> Bool {
        let acceleration = repeatAcceleration
            ?? (controllerInput?.isRepeatingDirectionCommand == true
                ? max(1, controllerInput?.directionalRepeatAcceleration ?? 1)
                : nil)
        if pendingFocusKey != nil {
            // Keep only the latest intention while a lazy destination mounts.
            // Do not drop every repeat or replay a backlog after mounting.
            pendingDirectionalMove = (direction, acceleration)
            return true
        }
        focusRepeatAcceleration = acceleration
        // Reaching an endpoint during a held traversal must not wrap or hand
        // off to chrome until a new physical press.
        if acceleration != nil,
           boundaryRules.contains(where: {
               $0.requiresFreshPress && $0.direction == direction
                   && focusedTargetMatches($0.fromLabel)
           }) { return true }
        let isLinearMove = direction == .up || direction == .down
        let analogIsActive = controllerInput?.isRightStickScrolling == true
        let keys = orderedKeys(includingUnmountedDeclared: isLinearMove)
            // The source remains part of the semantic graph even when a
            // concurrent analog step has carried it beyond the corridor.
            // Otherwise linear movement falls through to spatial guessing.
            .filter { !analogIsActive || $0 == focusedKey || isVisibleInScrollViewport($0) }
        guard !keys.isEmpty else {
            controllerInput?.playFeedback(.boundary)
            return true
        }
        guard let focusedKey else {
            return focusContent(preferLast: direction.verticalComponent == .up)
        }

        if let linked = linkedDestination(from: focusedKey, direction: direction) {
            if linked == ControllerAccessibilityDirectionalLink.navigationBoundary {
                return performBoundary(direction)
            }
            if let destination = resolveLinkDestination(
                linked,
                from: focusedKey,
                direction: direction,
                keys: keys
            ) {
                focusOrSeek(destination)
                controllerInput?.playFeedback(.move(direction))
                return true
            }
        }

        if direction == .up || direction == .down,
           let currentIndex = keys.firstIndex(of: focusedKey) {
            let step = direction == .down ? 1 : -1
            var index = currentIndex + step
            while keys.indices.contains(index) {
                let key = keys[index]
                if targets[key]?.isEnabled != false {
                    focusOrSeek(key)
                    controllerInput?.playFeedback(.move(direction))
                    return true
                }
                index += step
            }
            return performBoundary(direction)
        }

        if let destination = spatialDestination(
            from: focusedKey,
            direction: direction,
            keys: keys
        ) {
            setFocus(destination)
            controllerInput?.playFeedback(.move(direction))
            return true
        }
        return performBoundary(direction)
    }

    private func performBoundary(_ direction: MenuControllerCommand) -> Bool {
        // Free scrolling owns the viewport; a simultaneous left-stick edge
        // should not transfer to a footer/tab bar outside that scroll surface.
        guard controllerInput?.isRightStickScrolling != true else { return true }
        let ruleAllowsTransfer = boundaryRules.isEmpty
            || boundaryRules.contains { rule in
                rule.direction == direction && focusedTargetMatches(rule.fromLabel)
            }
        if ruleAllowsTransfer, onBoundary?(direction) == true {
            // The new owner already captured our source frame. Retire both
            // Settings outline layers before the tab bar draws its own box.
            rememberCurrentFocus()
            permitsAutomaticFocus = false
            clearFocus()
            controllerInput?.playFeedback(.move(direction))
        } else {
            controllerInput?.playFeedback(.boundary)
        }
        return true
    }

    private func focusedTargetMatches(_ label: String) -> Bool {
        guard let focusedKey else { return false }
        if let target = targets[focusedKey] {
            return matches(label, target: target)
        }
        return focusedKey == Self.explicitKey(label)
    }

    private func performChromeAction(
        _ action: (@MainActor () -> Bool)?,
        feedback: MenuControllerFeedback
    ) -> Bool {
        if action?() == true { controllerInput?.playFeedback(feedback) }
        else { controllerInput?.playFeedback(.boundary) }
        return true
    }

    private func setFocus(
        _ key: String,
        requestsNativeFocus: Bool = true,
        scrolls: Bool = true
    ) {
        guard isMountedAndEnabled(key) else { return }
        let scrolls = scrolls && controllerInput?.isRightStickScrolling != true
        focusIsSuspendedForScrolling = false
        pendingFocusKey = nil
        pendingDirectionalMove = nil
        pendingFocusExpiryTask?.cancel()
        pendingFocusExpiryTask = nil
        mountedTargetTask?.cancel()
        mountedTargetTask = nil
        cancelNativeFocusLoss()
        if focusedKey == key {
            if targets[key]?.focusState.isFocused == false {
                targets[key]?.focusState.isFocused = true
            }
            let beganScroll = scrolls && reveal(key)
            if !beganScroll,
               !(isScrollPresentationActive && scrollPresentationKey == key) {
                updateFocusedPresentation()
            }
            if requestsNativeFocus,
               let view = targets[key]?.view.value,
               !view.isFocused {
                requestNativeFocus(on: view)
            }
            publishScrollFocus()
            return
        }
        if let previousKey = focusedKey,
           previousKey != key,
           targets[previousKey]?.focusState.isFocused == true {
            targets[previousKey]?.focusState.isFocused = false
        }
        if targets[key]?.focusState.isFocused == false {
            targets[key]?.focusState.isFocused = true
        }
        focusedKey = key
        if !scopeKey.isEmpty {
            controllerInput?.rememberNavigationFocusKey(
                key,
                forScope: scopeKey
            )
        }
        let beganScroll = scrolls && reveal(key)
        if !beganScroll {
            // An offscreen semantic seek starts display-link tracking before
            // its lazy row mounts. Mount reconciliation reaches this branch
            // while ScrollViewReader is still animating. Preserve that tracker
            // instead of publishing the row's first, edge-clipped frame.
            if isScrollPresentationActive && scrollPresentationKey == key {
                sampleScrollPresentation(
                    for: key,
                    at: CACurrentMediaTime()
                )
            } else if !animateFocus(to: key) {
                stopScrollPresentationTracking(publishesFinalFrame: false)
                updateFocusedPresentation()
            }
        }
        if requestsNativeFocus, let view = targets[key]?.view.value {
            requestNativeFocus(on: view)
        }
        publishScrollFocus()
    }

    private func clearFocus() {
        pendingDirectionalMove = nil
        focusRepeatAcceleration = nil
        focusPresentationRevealTask?.cancel()
        focusPresentationRevealTask = nil
        focusIsSuspendedForScrolling = false
        rightStickScrollView = nil
        rightStickManualFocusKey = nil
        rightStickFocusCandidates = nil
        focusPresentationIsHiddenForScrolling = false
        stopScrollPresentationTracking(publishesFinalFrame: false)
        pendingFocusExpiryTask?.cancel()
        pendingFocusExpiryTask = nil
        pendingFocusKey = nil
        mountedTargetTask?.cancel()
        mountedTargetTask = nil
        cancelNativeFocusRequest()
        cancelNativeFocusLoss()
        for key in targets.keys where targets[key]?.focusState.isFocused == true {
            targets[key]?.focusState.isFocused = false
        }
        guard focusedKey != nil || focusPresentation != .empty else { return }
        focusedKey = nil
        setFocusedPresentation(.empty)
        publishScrollFocus()
    }

    private func rememberCurrentFocus() {
        guard !scopeKey.isEmpty,
              let key = focusedKey ?? pendingFocusKey else { return }
        controllerInput?.rememberNavigationFocusKey(key, forScope: scopeKey)
    }

    private func queueScopeFocusRestoration(_ key: String) {
        pendingFocusKey = key
        pendingFocusExpiryTask?.cancel()
        pendingFocusExpiryTask = Task { @MainActor [weak self] in
            // Lazy Forms may need to scroll before the remembered row remounts.
            // Fall back only if the semantic target genuinely never returns.
            try? await Task.sleep(for: .milliseconds(1_500))
            guard let self, !Task.isCancelled,
                  self.pendingFocusKey == key else { return }
            self.pendingFocusKey = nil
            self.pendingFocusExpiryTask = nil
            _ = self.focusInitialIfPossible()
        }
    }

    private func scheduleScopeRestorationSeek(_ key: String) {
        scopeRestorationTask?.cancel()
        scopeRestorationTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled,
                  self.pendingFocusKey == key else { return }
            self.scopeRestorationTask = nil
            self.focusOrSeek(key)
        }
    }

    @discardableResult
    private func reveal(_ key: String) -> Bool {
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

    private func isFullyVisible(_ view: UIView, in scrollView: UIScrollView) -> Bool {
        guard view.window != nil else { return false }
        let targetFrame = view.convert(view.bounds, to: scrollView)
        let maximumMargin = max(
            0,
            (scrollView.bounds.height - targetFrame.height) / 2 - 1
        )
        let edgeMargin = min(focusViewportEdgeMargin, maximumMargin)
        let visibleFrame = CGRect(
            origin: scrollView.contentOffset,
            size: scrollView.bounds.size
        )
        .insetBy(dx: 0, dy: edgeMargin)
        return visibleFrame.contains(targetFrame)
    }

    private func focusCorridorDestination(
        for view: UIView,
        in scrollView: UIScrollView
    ) -> CGPoint? {
        guard view.window != nil else { return nil }
        let targetFrame = view.convert(view.bounds, to: scrollView)
        let corridor = focusViewportFrame(in: scrollView)
        let proposedY: CGFloat
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
            return nil
        }

        let minimumY = -scrollView.adjustedContentInset.top
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
        // A List can extend underneath RootView's persistent tab bar. Its UIKit
        // bounds are not the usable navigation viewport; clip to the session
        // before reserving either gap.
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
    private func resumeFocusAfterScrollingIfNeeded() {
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
            setFocus(key, requestsNativeFocus: false, scrolls: false)
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

    private func isVisibleInScrollViewport(_ key: String) -> Bool {
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

            // Prefer the row closest to the viewport center, then prefer the row
            // with more of its height visible when two centers are comparable.
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

    private func startScrollPresentationTracking(for key: String) {
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
    private func animateFocus(
        to key: String,
        scrolling scrollView: UIScrollView? = nil,
        to destinationOffset: CGPoint = .zero
    ) -> Bool {
        guard !UIAccessibility.isReduceMotionEnabled,
              let view = targets[key]?.view.value,
              let targetFrame = presentationFrameForView(view) else { return false }
        let style = SettingsStore.shared.controllerNavigationFocusAnimation
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
        // Remove only the outline padding; updateFocusedPresentation reapplies
        // it once, after interpolation, for both foreground and background.
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

    private func sampleScrollPresentation(
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
                if progress >= 1 { focusMotion = nil }
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

    private func stopScrollPresentationTracking(
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

        // Leave direct tracking enabled through the observation turn that
        // publishes the final presentation rectangle. Disabling it immediately
        // lets the orb renderer reinterpret that last sample as a new animated
        // layout target and creates the visible end-of-scroll snap.
        scrollPresentationCompletionTask?.cancel()
        scrollPresentationCompletionTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled,
                  self.scrollPresentationKey == nil else { return }
            self.scrollPresentationCompletionTask = nil
            self.isScrollPresentationActive = false
        }
    }

    private func scrollViewHasActivePresentation(_ scrollView: UIScrollView) -> Bool {
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

    private func optionalFramesApproximatelyEqual(
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

    private func requestNativeFocus(on view: UIView) {
        guard ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled else {
            return
        }
        pendingNativeFocusView = view
        guard nativeFocusRequestTask == nil else { return }
        nativeFocusRequestTask = Task { @MainActor [weak self] in
            // A focus request can trigger SwiftUI reconciliation. Deferring and
            // coalescing it prevents a Form probe update from recursively asking
            // UIKit to synchronously update the same focus item.
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.nativeFocusRequestTask = nil
            guard let view = self.pendingNativeFocusView,
                  view.window != nil,
                  !view.isFocused,
                  ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled,
                  let focusSystem = view.window?.windowScene?.focusSystem else {
                self.pendingNativeFocusView = nil
                return
            }
            self.pendingNativeFocusView = nil
            focusSystem.requestFocusUpdate(to: view)
            focusSystem.updateFocusIfNeeded()
        }
    }

    private func cancelNativeFocusRequest() {
        nativeFocusRequestTask?.cancel()
        nativeFocusRequestTask = nil
        pendingNativeFocusView = nil
    }

    private func scheduleNativeFocusLoss(for key: String) {
        cancelNativeFocusLoss()
        nativeFocusLossTask = Task { @MainActor [weak self] in
            // `didUpdateFocus` sends the old and new item callbacks in the same
            // UIKit transaction. Let both callbacks finish before deciding that
            // focus actually left this navigation scope.
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.nativeFocusLossTask = nil
            guard self.focusedKey == key,
                  !self.targets.values.contains(where: {
                      $0.view.value?.isFocused == true
                  }) else { return }
            self.clearFocus()
        }
    }

    private func cancelNativeFocusLoss() {
        nativeFocusLossTask?.cancel()
        nativeFocusLossTask = nil
    }

    private func focusInitialIfPossible() -> Bool {
        guard focusedKey == nil, permitsAutomaticFocus else { return false }
        return focusContent(preferLast: false)
    }

    private func orderedKeys(
        includingUnmountedDeclared: Bool = false
    ) -> [String] {
        let keys: [String]
        if !declaredOrder.isEmpty {
            let declared = declaredOrder.filter { key in
                if includingUnmountedDeclared {
                    return targets[key]?.isEnabled != false
                }
                return isMountedAndEnabled(key)
            }
            let extras = targets.values
                .filter {
                    !declaredOrderSet.contains($0.key)
                        && isMountedAndEnabled($0.key)
                }
                .sorted(by: visualOrder)
                .map(\.key)
            keys = declared + extras
        } else {
            keys = targets.values
                .filter { isMountedAndEnabled($0.key) }
                .sorted(by: visualOrder)
                .map(\.key)
        }

        guard !preferredTrailingFocusLabels.isEmpty else { return keys }
        let trailingKeys = preferredTrailingFocusLabels.compactMap {
            keyMatching($0, in: keys)
        }
        let trailingSet = Set(trailingKeys)
        return keys.filter { !trailingSet.contains($0) } + trailingKeys
    }

    private func isMountedAndEnabled(_ key: String) -> Bool {
        guard let target = targets[key], target.isEnabled,
              let view = target.view.value else { return false }
        return view.window != nil && view.superview != nil
    }

    private func focusOrSeek(_ key: String) {
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

    private func needsLogicalScrollOwner(for key: String) -> Bool {
        if let target = targets[key],
           preferredTrailingFocusLabels.contains(where: { matches($0, target: target) }) {
            return false
        }
        guard declaredOrder.contains(key), let view = targets[key]?.view.value,
              enclosingScrollView(for: view) == nil,
              let frame = frameForView(view) else { return false }
        // A mounted glass portal may be offscreen before its local scroll
        // marker can associate it. Seek its logical row rather than publishing
        // an offscreen focus rectangle with no way to reveal it. Fixed visible
        // controls (including modal footers) need no scroll owner.
        return !frame.intersects(scopeFrame)
    }

    private func scheduleMountedTargetReconciliation(key: String) {
        guard focusedKey == key || pendingFocusKey == key,
              mountedTargetTask == nil else { return }
        mountedTargetTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.mountedTargetTask = nil
            // Resolve the current owner after yielding: a replacement probe
            // may have registered while this single reconciliation was queued.
            guard let key = self.pendingFocusKey ?? self.focusedKey,
                  let owner = self.targets[key]?.view.value,
                  self.isMountedAndEnabled(key) else { return }
            if self.pendingFocusKey == key {
                guard !self.needsLogicalScrollOwner(for: key) else { return }
                // The first logical seek only mounts a lazy Form row. Reconcile
                // once with its real UIKit geometry so edge-maintained scopes
                // can preserve their configured top and bottom preview gaps.
                let nextMove = self.pendingDirectionalMove
                self.pendingDirectionalMove = nil
                self.setFocus(key)
                if let nextMove {
                    _ = self.move(
                        nextMove.direction,
                        repeatAcceleration: nextMove.acceleration
                    )
                }
            } else if self.focusedKey == key {
                if self.targets[key]?.focusState.isFocused == false {
                    self.targets[key]?.focusState.isFocused = true
                }
                self.updateFocusedPresentation()
                self.requestNativeFocus(on: owner)
                self.publishScrollFocus()
            }
        }
    }

    private func scheduleFocusedPresentationUpdate(
        publishesScrollFocus: Bool = false
    ) {
        pendingScrollFocusPublication = pendingScrollFocusPublication
            || publishesScrollFocus
        guard focusedPresentationTask == nil else { return }
        focusedPresentationTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.focusedPresentationTask = nil
            let publishesScrollFocus = self.pendingScrollFocusPublication
            self.pendingScrollFocusPublication = false
            self.updateFocusedPresentation()
            if publishesScrollFocus { self.publishScrollFocus() }
        }
    }

    private func visualOrder(_ lhs: Target, _ rhs: Target) -> Bool {
        if let lhsY = lhs.lastContentY, let rhsY = rhs.lastContentY,
           abs(lhsY - rhsY) > 2 { return lhsY < rhsY }
        if let lhsFrame = lhs.lastFrame, let rhsFrame = rhs.lastFrame {
            if abs(lhsFrame.midY - rhsFrame.midY) > 8 {
                return lhsFrame.midY < rhsFrame.midY
            }
            if abs(lhsFrame.minX - rhsFrame.minX) > 4 {
                return lhsFrame.minX < rhsFrame.minX
            }
        }
        return lhs.registrationOrder < rhs.registrationOrder
    }

    private func spatialDestination(
        from sourceKey: String,
        direction: MenuControllerCommand,
        keys: [String]
    ) -> String? {
        guard let sourceFrame = targets[sourceKey]?.lastFrame else { return nil }
        let vector = direction.vector
        return keys.compactMap { key -> (String, CGFloat)? in
            guard key != sourceKey,
                  let target = targets[key], target.isEnabled,
                  let frame = target.lastFrame else { return nil }
            let dx = frame.midX - sourceFrame.midX
            let dy = frame.midY - sourceFrame.midY
            let forward = dx * vector.x + dy * vector.y
            guard forward > 1 else { return nil }
            let cross = abs(dx * vector.y - dy * vector.x)
            return (key, forward + cross * 2.25)
        }.min(by: { $0.1 < $1.1 })?.0
    }

    private func linkedDestination(
        from key: String,
        direction: MenuControllerCommand
    ) -> String? {
        guard prioritizesDirectionalLinks || !directionalLinks.isEmpty,
              let target = targets[key] else { return nil }
        let firstContentKey = firstContentTarget(in: orderedKeys())
        return directionalLinks.first(where: { link in
            guard link.direction == direction else { return false }
            if link.fromLabel == ControllerAccessibilityDirectionalLink.firstContent {
                return key == firstContentKey
            }
            return matches(link.fromLabel, target: target)
        })?.toLabel
    }

    private func resolveLinkDestination(
        _ label: String,
        from sourceKey: String,
        direction: MenuControllerCommand,
        keys: [String]
    ) -> String? {
        if label == ControllerAccessibilityDirectionalLink.firstContent {
            return firstContentTarget(in: keys, below: sourceKey)
        }
        if label == ControllerAccessibilityDirectionalLink.scrollBoundary {
            return direction == .up ? keys.first : keys.last
        }
        return keyMatching(label, in: keys)
    }

    private func firstContentTarget(
        in keys: [String],
        below sourceKey: String? = nil
    ) -> String? {
        guard let sourceKey,
              let sourceFrame = targets[sourceKey]?.lastFrame else { return keys.first }
        return keys.first(where: {
            guard let frame = targets[$0]?.lastFrame else { return false }
            return frame.midY > sourceFrame.maxY
        }) ?? keys.first
    }

    private func keyMatching(_ value: String, in keys: [String]) -> String? {
        keys.first(where: { key in
            guard let target = targets[key] else {
                return key == Self.explicitKey(value)
            }
            return matches(value, target: target)
        })
    }

    private func matches(_ value: String, target: Target) -> Bool {
        target.navigationID == value || target.label == value
            || target.baseKey == Self.explicitKey(value)
    }

    private func resolveKey(
        registrationID: UUID,
        baseKey: String,
        owner: UIView,
        hasUniqueIdentity: Bool
    ) -> String {
        if let existing = registrationKeys[registrationID] { return existing }
        guard let occupied = targets[baseKey],
              occupied.view.value !== owner,
              occupied.view.value != nil else { return baseKey }

        // An explicit navigation ID represents one logical control. A newly
        // mounted Form cell replaces the retained/recycled owner atomically;
        // it must never become a location-suffixed second copy.
        if hasUniqueIdentity {
            registrationKeys.removeValue(forKey: occupied.registrationID)
            if occupied.focusState.isFocused {
                let displacedFocusState = occupied.focusState
                Task { @MainActor in
                    await Task.yield()
                    displacedFocusState.isFocused = false
                }
            }
            return baseKey
        }

        duplicateBaseKeys.insert(baseKey)
        var mutableOccupied = occupied
        mutableOccupied.isPersistent = false
        targets[baseKey] = mutableOccupied
        return "\(baseKey)#\(registrationID.uuidString)"
    }

    private func updateReadiness() {
        guard readinessTask == nil else { return }
        readinessTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.readinessTask = nil
            let next = self.targets.keys.contains(
                where: { self.isMountedAndEnabled($0) }
            )
            guard next != self.hasNavigableElements else { return }
            self.hasNavigableElements = next
            if let registrationID = self.registrationID {
                self.controllerInput?.setNavigationSessionReady(
                    id: registrationID,
                    isReady: next
                )
            }
        }
    }

    private func updateFocusedPresentation(frameOverride: CGRect? = nil) {
        guard !(focusIsSuspendedForScrolling
                && focusPresentationIsHiddenForScrolling) else {
            // Keep the last frame available as the fade-out source. The
            // presentation overlay is hidden until scroll-end restoration has
            // selected and measured its replacement row.
            return
        }
        if frameOverride == nil, isScrollPresentationActive {
            // The display link is the only geometry owner during a focus-driven
            // scroll. Model-layer layout callbacks can already contain the final
            // offset and must not get ahead of the pixels being presented.
            return
        }
        guard let focusedKey, let target = targets[focusedKey] else {
            setFocusedPresentation(.empty)
            return
        }
        var mutableTarget = target
        if frameOverride == nil, let view = target.view.value {
            mutableTarget.lastFrame = frameForView(view) ?? target.lastFrame
            mutableTarget.lastContentY = contentPosition(for: view) ?? target.lastContentY
            if mutableTarget.lastContentY != target.lastContentY
                || mutableTarget.lastFrame?.size != target.lastFrame?.size {
                rightStickFocusCandidates = nil
            }
            targets[focusedKey] = mutableTarget
        }
        let nextFrame = (frameOverride ?? mutableTarget.lastFrame)?
            .insetBy(dx: -3, dy: -2)
        let nextOverlayFrame = nextFrame.map {
            $0.offsetBy(
                dx: -scopeFrame.minX,
                dy: -scopeFrame.minY
            )
        }
        setFocusedPresentation(
            FocusPresentation(
                frame: nextFrame,
                overlayFrame: nextOverlayFrame,
                accessibilityLabel: mutableTarget.label
            )
        )
        scheduleFocusPresentationRevealIfNeeded()
    }

    private func scheduleFocusPresentationRevealIfNeeded() {
        guard focusPresentationIsHiddenForScrolling,
              !focusIsSuspendedForScrolling,
              controllerInput?.isControllerNavigationEnabled == true,
              let focusedKey, isMountedAndEnabled(focusedKey),
              focusPresentation.frame != nil,
              focusPresentationRevealTask == nil else { return }
        focusPresentationRevealTask = Task { @MainActor [weak self] in
            // Let the overlay consume measured geometry before fading in.
            // Navigation can change rows during this yield: reveal the current
            // valid focus, not the row captured before the command.
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.focusPresentationRevealTask = nil
            guard !self.focusIsSuspendedForScrolling,
                  self.controllerInput?.isControllerNavigationEnabled == true,
                  let key = self.focusedKey, self.isMountedAndEnabled(key),
                  self.focusPresentation.frame != nil else { return }
            self.focusPresentationIsHiddenForScrolling = false
        }
    }

    private func setFocusedPresentation(_ presentation: FocusPresentation) {
        guard presentedFocusKey != focusedKey
                || focusPresentation.accessibilityLabel != presentation.accessibilityLabel
                || !optionalFramesApproximatelyEqual(
                    focusPresentation.frame,
                    presentation.frame
                )
                || !optionalFramesApproximatelyEqual(
                    focusPresentation.overlayFrame,
                    presentation.overlayFrame
                ) else { return }
        presentedFocusKey = focusedKey
        focusPresentation = presentation
    }

    private func publishOrbInteractionIfNeeded(
        _ command: MenuControllerCommand
    ) {
        // Directional focus travel already drives the orb anchor. Publishing a
        // second observed event for those commands only invalidates the overlay.
        guard !command.isSpatialDirection else { return }
        interactionSequence &+= 1
        latestOrbInteraction = ControllerNavigationOrbInteraction(
            sequence: interactionSequence,
            command: command,
            timestamp: Date.timeIntervalSinceReferenceDate
        )
    }

    private func frameForView(_ view: UIView) -> CGRect? {
        guard let window = view.window, !view.isHidden, view.alpha >= 0.01 else {
            return nil
        }
        let frame = view.convert(view.bounds, to: window)
        guard frame.width >= 2, frame.height >= 2 else { return nil }
        return frame
    }

    private func presentationFrameForView(_ view: UIView) -> CGRect? {
        guard view.window != nil, !view.isHidden, view.alpha >= 0.01 else {
            return nil
        }
        guard var frame = frameForView(view) else { return nil }

        // Always begin with one model-space window frame and reconcile every
        // animated scroll ancestor. Returning an available descendant
        // presentation layer early can skip a separately animated UIScrollView
        // bounds offset, publishing the final row position before the scroll is
        // actually visible.
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

    private func contentPosition(for view: UIView) -> CGFloat? {
        guard let scrollView = enclosingScrollView(for: view) else { return nil }
        return view.convert(view.bounds, to: scrollView).midY
    }

    /// SwiftUI Forms and Lists can put several private UIScrollViews between a
    /// row probe and the visible collection view. The first ancestor is often
    /// an internal control surface, so choose the largest visible vertical
    /// ancestor which actually owns scrollable content.
    private func enclosingScrollView(for view: UIView) -> UIScrollView? {
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

        // On iOS 26 a glass row can live in a sibling SDF/interaction layer,
        // with no UIScrollView ancestor at all. Use the same screen-local
        // owner as analog scrolling, rather than waiting for scrollTo to mount
        // an offscreen row. Weak markers keep retained/covered pages passive.
        return scrollSurfaces.allObjects.lazy
            .compactMap { $0 as? any ControllerAccessibilityScrollSurface }
            .compactMap { $0.scrollViewForFocusTarget(view) }
            .first
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

    private func activateNativeControl(near probe: UIView) -> Bool {
        var root = probe.superview
        var depth = 0
        while let candidate = root, depth < 5 {
            if activateFirstAccessibleControl(in: candidate, excluding: probe) {
                return true
            }
            if candidate is UIScrollView { break }
            root = candidate.superview
            depth += 1
        }
        return false
    }

    private func activateFirstAccessibleControl(
        in root: UIView,
        excluding probe: UIView
    ) -> Bool {
        var queue = root.subviews.filter { $0 !== probe }
        var visited = 0
        while !queue.isEmpty, visited < 96 {
            let view = queue.removeFirst()
            visited += 1
            guard !view.isHidden, view.alpha >= 0.01, view.isUserInteractionEnabled else {
                continue
            }
            if view.isAccessibilityElement,
               !view.accessibilityTraits.contains(.notEnabled),
               (view.accessibilityTraits.contains(.button)
                    || view.accessibilityTraits.contains(.adjustable)),
               view.accessibilityActivate() { return true }
            queue.append(contentsOf: view.subviews)
        }
        return false
    }

    fileprivate static func explicitKey(_ id: String) -> String {
        "controller.focus.id.\(id)"
    }

    fileprivate static func semanticKey(_ label: String) -> String {
        "controller.focus.label.\(label)"
    }
}

private extension MenuControllerCommand {
    var vector: CGPoint {
        switch self {
        case .up: CGPoint(x: 0, y: -1)
        case .upRight: CGPoint(x: 0.707, y: -0.707)
        case .right: CGPoint(x: 1, y: 0)
        case .downRight: CGPoint(x: 0.707, y: 0.707)
        case .down: CGPoint(x: 0, y: 1)
        case .downLeft: CGPoint(x: -0.707, y: 0.707)
        case .left: CGPoint(x: -1, y: 0)
        case .upLeft: CGPoint(x: -0.707, y: -0.707)
        default: .zero
        }
    }
}

private extension UIFocusHeading {
    var controllerCommand: MenuControllerCommand? {
        if contains(.up) && contains(.right) { return .upRight }
        if contains(.down) && contains(.right) { return .downRight }
        if contains(.down) && contains(.left) { return .downLeft }
        if contains(.up) && contains(.left) { return .upLeft }
        if contains(.up) { return .up }
        if contains(.down) { return .down }
        if contains(.left) { return .left }
        if contains(.right) { return .right }
        return nil
    }
}

private final class ControllerAccessibilityScopeProbeView: UIView {
    weak var session: ControllerAccessibilityNavigationSession?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        session?.updateScope(view: self)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        session?.updateScope(view: self)
    }
}

private struct ControllerAccessibilityScopeProbe: UIViewRepresentable {
    let session: ControllerAccessibilityNavigationSession

    func makeUIView(context: Context) -> ControllerAccessibilityScopeProbeView {
        let view = ControllerAccessibilityScopeProbeView()
        view.session = session
        return view
    }

    func updateUIView(
        _ uiView: ControllerAccessibilityScopeProbeView,
        context: Context
    ) {
        uiView.session = session
        uiView.setNeedsLayout()
    }
}

@MainActor
fileprivate final class ControllerAccessibilityActionProbeView: UIControl {
    private weak var navigationSession: ControllerAccessibilityNavigationSession?
    private var registrationID: UUID?
    private var baseKey = ""
    private(set) var resolvedKey = ""
    private var navigationID: String?
    private var targetLabel = ""
    private var targetValue: String?
    private var targetTraits: UIAccessibilityTraits = .button
    private var targetFocusState: ControllerAccessibilityTargetFocusState?
    private var targetEnabled = true
    private var targetPersistent = false
    private var targetFeedback: MenuControllerFeedback = .activate
    private var targetFocusedNeonCornerRadius: CGFloat?
    private var onActivate: (() -> Void)?
    private var onIncrement: (() -> Void)?
    private var onDecrement: (() -> Void)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isAccessibilityElement = false
        isUserInteractionEnabled = true
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override var canBecomeFocused: Bool {
        targetEnabled && navigationSession != nil
            && ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled
    }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool { false }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { unregisterWhileDetached() }
        else { synchronizeRegistration() }
    }

    override func didMoveToSuperview() {
        super.didMoveToSuperview()
        if superview == nil { unregisterWhileDetached() }
        else { synchronizeRegistration() }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let registrationID else { return }
        navigationSession?.targetGeometryDidChange(
            registrationID: registrationID,
            owner: self
        )
    }

    override func shouldUpdateFocus(
        in context: UIFocusUpdateContext
    ) -> Bool {
        guard navigationSession?.permitsNativeFocusMovement(
            context.focusHeading
        ) != false else { return false }
        return super.shouldUpdateFocus(in: context)
    }

    override func didUpdateFocus(
        in context: UIFocusUpdateContext,
        with coordinator: UIFocusAnimationCoordinator
    ) {
        super.didUpdateFocus(in: context, with: coordinator)
        guard !resolvedKey.isEmpty else { return }
        navigationSession?.nativeFocusChanged(
            key: resolvedKey,
            isFocused: context.nextFocusedItem === self,
            heading: context.focusHeading
        )
    }

    override func pressesEnded(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        guard isFocused else {
            super.pressesEnded(presses, with: event)
            return
        }
        if presses.contains(where: { $0.type == .select }),
           navigationSession?.activate(resolvedKey) == true { return }
        if presses.contains(where: { $0.type == .leftArrow }),
           navigationSession?.handleNativeHorizontalPress(
               resolvedKey,
               increment: false
           ) == true { return }
        if presses.contains(where: { $0.type == .rightArrow }),
           navigationSession?.handleNativeHorizontalPress(
               resolvedKey,
               increment: true
           ) == true { return }
        super.pressesEnded(presses, with: event)
    }

    @discardableResult
    func configure(
        registrationID: UUID,
        session: ControllerAccessibilityNavigationSession,
        baseKey: String,
        navigationID: String?,
        label: String,
        value: String?,
        traits: UIAccessibilityTraits,
        focusState: ControllerAccessibilityTargetFocusState,
        isEnabled: Bool,
        persistent: Bool,
        activationFeedback: MenuControllerFeedback,
        focusedNeonCornerRadius: CGFloat?,
        onActivate: (() -> Void)?,
        onIncrement: (() -> Void)?,
        onDecrement: (() -> Void)?
    ) -> Bool {
        let needsGeometryRefresh = self.registrationID != registrationID
            || navigationSession !== session
            || self.baseKey != baseKey
            || targetLabel != label
            || targetValue != value
            || targetEnabled != isEnabled
            || targetFocusedNeonCornerRadius != focusedNeonCornerRadius
        if self.registrationID != registrationID
            || navigationSession !== session
            || self.baseKey != baseKey {
            disconnect()
        }
        self.registrationID = registrationID
        navigationSession = session
        self.baseKey = baseKey
        self.navigationID = navigationID
        targetLabel = label
        targetValue = value
        targetTraits = traits
        targetFocusState = focusState
        targetEnabled = isEnabled
        targetPersistent = persistent
        targetFeedback = activationFeedback
        targetFocusedNeonCornerRadius = focusedNeonCornerRadius
        self.onActivate = onActivate
        self.onIncrement = onIncrement
        self.onDecrement = onDecrement
        synchronizeRegistration()
        return needsGeometryRefresh
    }

    func disconnect() {
        unregisterWhileDetached()
        navigationSession = nil
        registrationID = nil
        resolvedKey = ""
    }

    private func unregisterWhileDetached() {
        guard let registrationID else { return }
        navigationSession?.unregisterTarget(
            registrationID: registrationID,
            owner: self
        )
        resolvedKey = ""
    }

    private func synchronizeRegistration() {
        guard window != nil, superview != nil,
              let registrationID, let navigationSession,
              let targetFocusState else { return }
        configureNativeListFocus()
        resolvedKey = navigationSession.registerTarget(
            registrationID: registrationID,
            view: self,
            baseKey: baseKey,
            navigationID: navigationID,
            label: targetLabel,
            value: targetValue,
            traits: targetTraits,
            focusState: targetFocusState,
            isEnabled: targetEnabled,
            persistent: targetPersistent,
            activationFeedback: targetFeedback,
            focusedNeonCornerRadius: targetFocusedNeonCornerRadius,
            onActivate: onActivate,
            onIncrement: onIncrement,
            onDecrement: onDecrement
        )
    }

    /// SwiftUI Form is backed by a UIKit list on current iOS releases. Let that
    /// native scroll container participate in the focus transaction instead of
    /// reconstructing its cells or scrolling by synthetic viewport steps.
    private func configureNativeListFocus() {
        guard ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled else {
            return
        }
        var ancestor = superview
        while let current = ancestor {
            if let collectionView = current as? UICollectionView {
                collectionView.allowsFocus = true
                collectionView.allowsFocusDuringEditing = true
                collectionView.selectionFollowsFocus = false
                collectionView.remembersLastFocusedIndexPath = true
                return
            }
            if let tableView = current as? UITableView {
                tableView.allowsFocus = true
                tableView.allowsFocusDuringEditing = true
                tableView.selectionFollowsFocus = false
                tableView.remembersLastFocusedIndexPath = true
                return
            }
            ancestor = current.superview
        }
    }
}

private struct ControllerAccessibilityActionProbe: UIViewRepresentable {
    let registrationID: UUID
    let session: ControllerAccessibilityNavigationSession
    let scopeRevision: UInt64
    let baseKey: String
    let navigationID: String?
    let label: String
    let value: String?
    let traits: UIAccessibilityTraits
    let focusState: ControllerAccessibilityTargetFocusState
    let isEnabled: Bool
    let persistent: Bool
    let activationFeedback: MenuControllerFeedback
    let focusedNeonCornerRadius: CGFloat?
    let onActivate: (() -> Void)?
    let onIncrement: (() -> Void)?
    let onDecrement: (() -> Void)?

    func makeUIView(context: Context) -> ControllerAccessibilityActionProbeView {
        ControllerAccessibilityActionProbeView()
    }

    func updateUIView(
        _ uiView: ControllerAccessibilityActionProbeView,
        context: Context
    ) {
        let needsGeometryRefresh = uiView.configure(
            registrationID: registrationID,
            session: session,
            baseKey: baseKey,
            navigationID: navigationID,
            label: label,
            value: value,
            traits: traits,
            focusState: focusState,
            isEnabled: isEnabled,
            persistent: persistent,
            activationFeedback: activationFeedback,
            focusedNeonCornerRadius: focusedNeonCornerRadius,
            onActivate: onActivate,
            onIncrement: onIncrement,
            onDecrement: onDecrement
        )
        if needsGeometryRefresh {
            // A changed Picker value or an Override-to-editor swap can resize its
            // Form row without moving this representable in the hierarchy. Avoid
            // forcing another layout pass for unrelated observation updates.
            uiView.setNeedsLayout()
        }
    }

    static func dismantleUIView(
        _ uiView: ControllerAccessibilityActionProbeView,
        coordinator: Void
    ) { uiView.disconnect() }
}

private struct ControllerAccessibilityExplicitTargetModifier: ViewModifier {
    let navigationID: String?
    let label: String
    let value: String?
    let traits: UIAccessibilityTraits
    let focusedColor: Color?
    let focusedNeonCornerRadius: CGFloat?
    let activationFeedback: MenuControllerFeedback
    let onActivate: (() -> Void)?
    let onIncrement: (() -> Void)?
    let onDecrement: (() -> Void)?

    @Environment(\.controllerAccessibilityNavigationActive) private var isActive
    @Environment(\.controllerAccessibilityNavigationSession) private var session
    @Environment(\.controllerAccessibilityTargetsSuppressed) private var isSuppressed
    @Environment(\.controllerAccessibilityInheritedTargetID) private var inheritedID
    @Environment(\.isEnabled) private var isEnabled
    @State private var registrationID = UUID()
    @State private var focusState = ControllerAccessibilityTargetFocusState()

    private var trimmedID: String? {
        guard let source = navigationID ?? inheritedID else { return nil }
        let value = source.trimmingCharacters(in: .whitespacesAndNewlines)
        return value.isEmpty ? nil : value
    }

    private var trimmedLabel: String {
        label.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private var baseKey: String {
        if let trimmedID {
            return ControllerAccessibilityNavigationSession.explicitKey(trimmedID)
        }
        if !trimmedLabel.isEmpty {
            // A declared order is expressed in stable target IDs. Treat an
            // explicit target's nonempty label as that ID when the caller did
            // not provide one, so lazy Form rows can be sought before mount.
            return ControllerAccessibilityNavigationSession.explicitKey(trimmedLabel)
        }
        return "controller.focus.auto.\(registrationID.uuidString)"
    }

    func body(content: Content) -> some View {
        ControllerAccessibilityFocusedTextTint(
            content: content.focusable(false),
            focusedColor: focusedColor
        )
        // An explicit row owns the logical target. Prevent a nested SwiftUI
        // Button/Toggle (Picker included) from synthesizing a second UUID
        // probe over the same geometry through the navigation-wide styles.
        .environment(\.controllerAccessibilityAutomaticTargetSuppressed, true)
        .environment(\.controllerAccessibilityTargetFocused, focusState.isFocused)
        .controllerFocusDepthAnchor(
            id: baseKey,
            isFocused: focusState.isFocused
        )
        .modifier(
            ControllerAccessibilityScrollIdentityModifier(
                identity: navigationID == nil && inheritedID != nil
                    ? nil
                    : baseKey
            )
        )
        .background {
            if isActive, !isSuppressed, let session {
                ControllerAccessibilityActionProbe(
                    registrationID: registrationID,
                    session: session,
                    // Retained NavigationStack rows must update their probes
                    // after configure clears the preceding screen's registry.
                    scopeRevision: session.scopeRevision,
                    baseKey: baseKey,
                    navigationID: trimmedID,
                    label: trimmedLabel,
                    value: value,
                    traits: traits,
                    focusState: focusState,
                    isEnabled: isEnabled,
                    persistent: trimmedID != nil || !trimmedLabel.isEmpty,
                    activationFeedback: activationFeedback,
                    focusedNeonCornerRadius: focusedNeonCornerRadius,
                    onActivate: onActivate,
                    onIncrement: onIncrement,
                    onDecrement: onDecrement
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }
}

/// Presents one controller-owned list for a SwiftUI Picker. The Picker keeps
/// its native touch presentation, while Cross enters a deterministic semantic
/// target graph that works the same way in Settings and Per-Game Settings.
private struct ControllerAccessibilityOptionsPickerModifier<Selection: Hashable>: ViewModifier {
    let navigationID: String?
    let label: String
    @Binding var selection: Selection
    let options: [(id: Selection, title: String)]

    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var isOptionListPresented = false

    private var scopeID: String {
        let source = navigationID ?? label
        return source.isEmpty ? "controller.options-picker" : source
    }

    func body(content: Content) -> some View {
        content
            .controllerAccessibilityPickerTarget(
                id: navigationID,
                label: label,
                value: options.first(where: {
                    $0.id == selection
                })?.title,
                onActivate: {
                    guard !options.isEmpty else { return }
                    isOptionListPresented = true
                },
                onIncrement: {
                    moveSelection(by: 1)
                },
                onDecrement: {
                    moveSelection(by: -1)
                }
            )
            .sheet(isPresented: $isOptionListPresented) {
                ControllerAccessibilityOptionsList(
                    title: label,
                    scopeID: scopeID,
                    selection: $selection,
                    options: options,
                    controllerInput: controllerInput
                )
                .presentationDetents([.large])
                .presentationDragIndicator(.visible)
            }
    }

    private func moveSelection(by offset: Int) {
        guard let index = options.firstIndex(where: {
            $0.id == selection
        }), !options.isEmpty else { return }
        let nextIndex = min(
            max(index + offset, options.startIndex),
            options.index(before: options.endIndex)
        )
        selection = options[nextIndex].id
    }
}

private struct ControllerAccessibilityOptionsList<Selection: Hashable>: View {
    let title: String
    let scopeID: String
    @Binding var selection: Selection
    let options: [(id: Selection, title: String)]
    let controllerInput: MenuControllerInputRouter?

    @Environment(\.dismiss) private var dismiss
    @Environment(\.uiContentTextColour) private var contentTextColour
    @Environment(\.uiSecondaryTextColour) private var secondaryTextColour

    private var targetIDs: [String] {
        options.indices.map { targetID(for: $0) }
    }

    private var selectedTargetID: String? {
        guard let index = options.firstIndex(where: {
            $0.id == selection
        }) else { return targetIDs.first }
        return targetID(for: index)
    }

    var body: some View {
        NavigationStack {
            List {
                ForEach(Array(options.enumerated()), id: \.offset) { index, option in
                    let isSelected = selection == option.id
                    Button {
                        choose(option.id)
                    } label: {
                        HStack(spacing: 12) {
                            Text(option.title)
                                .foregroundStyle(contentTextColour)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            Image(
                                systemName: isSelected
                                    ? "checkmark.circle.fill"
                                    : "circle"
                            )
                            .foregroundStyle(
                                isSelected ? Color.accentColor : secondaryTextColour
                            )
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: targetID(for: index),
                        label: option.title
                    ) {
                        choose(option.id)
                    }
                }
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
        }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "controller.options.\(scopeID)",
            priority: 700,
            onBack: {
                dismiss()
                return true
            },
            usesNativeFocusEngine: false,
            usesExplicitTargetGeometryOnly: true,
            preservesFocusDuringRightStickScrolling: false,
            focusScrollBehavior: .maintainWithinViewport,
            focusTopAlignmentMargin: 24,
            focusBottomAlignmentMargin: 24,
            preferredInitialFocusLabel: selectedTargetID,
            declaredTargetOrder: targetIDs
        )
    }

    private func targetID(for index: Int) -> String {
        "\(scopeID).option.\(index)"
    }

    private func choose(_ value: Selection) {
        selection = value
        dismiss()
    }
}

private struct ControllerAccessibilityScrollIdentityModifier: ViewModifier {
    let identity: String?

    @ViewBuilder
    func body(content: Content) -> some View {
        if let identity { content.id(identity) }
        else { content }
    }
}

/// A single non-spatial transition for controller-focused text and symbols.
/// Keeping this separate from focus-box travel lets colors cross-fade without
/// adding latency to directional navigation or scroll geometry.
enum ControllerFocusVisualAnimation {
    static let textFade = Animation.easeInOut(duration: 0.16)
}

private struct ControllerAccessibilityFocusedTextTint<Content: View>: View {
    let content: Content
    let focusedColor: Color?
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    var body: some View {
        let controllerOwnsAppearance =
            controllerInput?.isControllerNavigationEnabled != false
        let visuallyFocused = isFocused && controllerOwnsAppearance
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        content
        // Input changes focus ownership, not the screen's theme palette.
        .foregroundColor(
            visuallyFocused
                ? (focusedColor ?? textAppearance.focusedColor)
                : textAppearance.normalColor
        )
        .shadow(
            color: (visuallyFocused
                ? textAppearance.focusedShadowColor
                : textAppearance.normalShadowColor
            ).opacity(shadowStrength),
            radius: CGFloat(5 * shadowStrength),
            y: CGFloat(2 * shadowStrength)
        )
        .animation(
            ControllerFocusVisualAnimation.textFade,
            value: visuallyFocused
        )
    }
}

private struct ControllerFocusedForegroundModifier: ViewModifier {
    let normal: Color
    let allowsFocusedBlue: Bool
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    func body(content: Content) -> some View {
        let controllerOwnsAppearance =
            controllerInput?.isControllerNavigationEnabled != false
        let visuallyFocused = isFocused && controllerOwnsAppearance
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        content
            .foregroundStyle(
                visuallyFocused && allowsFocusedBlue
                    ? textAppearance.focusedColor
                    : (allowsFocusedBlue
                        ? (textAppearance.normalColor ?? normal)
                        : normal)
            )
            .shadow(
                color: (visuallyFocused
                    ? textAppearance.focusedShadowColor
                    : textAppearance.normalShadowColor
                ).opacity(shadowStrength),
                radius: CGFloat(5 * shadowStrength),
                y: CGFloat(2 * shadowStrength)
            )
            .animation(
                ControllerFocusVisualAnimation.textFade,
                value: visuallyFocused
            )
    }
}

private struct ControllerAccessibilityFocusedNeonModifier: ViewModifier {
    let cornerRadius: CGFloat
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused

    func body(content: Content) -> some View {
        content.controllerFocusBoxPresentation(
            isVisible: isFocused,
            cornerRadius: cornerRadius
        )
    }
}

struct ControllerToolbarFocusVisualModifier: ViewModifier {
    let isFocused: Bool
    let focusID: String?
    var baseHorizontalPadding: CGFloat = 0
    var minimumWidth: CGFloat = 36
    var foregroundColour: Color? = nil
    @Environment(\.uiToolbarColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    @ViewBuilder
    func body(content: Content) -> some View {
        let visuallyFocused = isFocused
            && controllerInput?.isControllerNavigationEnabled != false
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        let focusedLabel = content
            .focusEffectDisabled()
            // Toolbar buttons are actions, not body copy. Match master by
            // keeping Boot BIOS and every toolbar symbol in the semantic
            // accent in both focused and unfocused states.
            .foregroundStyle(foregroundColour ?? accentColour)
            .shadow(
                color: (visuallyFocused
                    ? textAppearance.focusedShadowColor
                    : textAppearance.normalShadowColor
                ).opacity(shadowStrength),
                radius: CGFloat(5 * shadowStrength),
                y: CGFloat(2 * shadowStrength)
            )
            .animation(
                ControllerFocusVisualAnimation.textFade,
                value: visuallyFocused
            )
        let visual = focusedLabel
            .padding(.horizontal, baseHorizontalPadding)
            .frame(minWidth: minimumWidth, minHeight: 36)
            .background {
                Capsule(style: .continuous)
                    .fill(Color.black.opacity(visuallyFocused ? 0.48 : 0))
            }
            .controllerFocusBoxPresentation(
                isVisible: visuallyFocused,
                cornerRadius: 10_000
            )
            .animation(.linear(duration: 0.08), value: visuallyFocused)

        if let focusID {
            visual.controllerNavigationOrbTarget(
                id: focusID,
                isActive: visuallyFocused,
                palette: .green,
                inset: 2,
                orbScale: 0.72,
                priority: 20
            )
        } else { visual }
    }
}

private struct ControllerAccessibilityToolbarFocusVisualModifier: ViewModifier {
    let baseHorizontalPadding: CGFloat
    let minimumWidth: CGFloat
    let fallbackHorizontalPadding: CGFloat
    let fallbackMinimumHeight: CGFloat
    @Environment(\.controllerAccessibilityNavigationActive) private var navigationActive
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused
    @Environment(\.controllerAccessibilityUsesWindowFocusVisual)
    private var usesWindowFocusVisual
    @Environment(\.uiToolbarColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    @ViewBuilder
    func body(content: Content) -> some View {
        let visuallyFocused = isFocused
            && controllerInput?.isControllerNavigationEnabled != false
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        if navigationActive, usesWindowFocusVisual {
            let focusedLabel = content
                .focusEffectDisabled()
                .foregroundStyle(accentColour)
                .shadow(
                    color: (visuallyFocused
                        ? textAppearance.focusedShadowColor
                        : textAppearance.normalShadowColor
                    ).opacity(shadowStrength),
                    radius: CGFloat(5 * shadowStrength),
                    y: CGFloat(2 * shadowStrength)
                )
                .animation(
                    ControllerFocusVisualAnimation.textFade,
                    value: visuallyFocused
                )
            focusedLabel
                .padding(.horizontal, baseHorizontalPadding)
                .frame(minWidth: minimumWidth, minHeight: 36)
                .background {
                    Capsule(style: .continuous)
                        .fill(Color.black.opacity(visuallyFocused ? 0.48 : 0))
                }
                .animation(.linear(duration: 0.08), value: visuallyFocused)
        } else if navigationActive {
            content.modifier(
                ControllerToolbarFocusVisualModifier(
                    isFocused: visuallyFocused,
                    focusID: nil,
                    baseHorizontalPadding: baseHorizontalPadding,
                    minimumWidth: minimumWidth
                )
            )
        } else {
            content
                .foregroundStyle(accentColour)
                .shadow(
                    color: textAppearance.normalShadowColor.opacity(
                        textAppearance.normalShadowStrength
                    ),
                    radius: CGFloat(
                        5 * textAppearance.normalShadowStrength
                    ),
                    y: CGFloat(2 * textAppearance.normalShadowStrength)
                )
                .padding(.horizontal, fallbackHorizontalPadding)
                .frame(minHeight: fallbackMinimumHeight)
        }
    }
}

private struct ControllerAccessibilityRegisteringButtonStyle: PrimitiveButtonStyle {
    let isActive: Bool
    @Environment(\.controllerAccessibilityAutomaticTargetSuppressed)
    private var isSuppressed

    @ViewBuilder
    func makeBody(configuration: Configuration) -> some View {
        if isActive, !isSuppressed {
            Button(configuration)
                .buttonStyle(.automatic)
                .controllerAccessibilityActionTarget(label: "") {
                    configuration.trigger()
                }
        } else { Button(configuration).buttonStyle(.automatic) }
    }
}

private struct ControllerAccessibilityRegisteringToggleStyle: ToggleStyle {
    let isActive: Bool
    @Environment(\.controllerAccessibilityAutomaticTargetSuppressed)
    private var isSuppressed

    @ViewBuilder
    func makeBody(configuration: Configuration) -> some View {
        if isActive, !isSuppressed {
            Toggle(configuration)
                .toggleStyle(.switch)
                .controllerAccessibilityToggleTarget(
                    label: "",
                    isOn: configuration.$isOn
                )
        } else { Toggle(configuration).toggleStyle(.switch) }
    }
}

private struct ControllerAccessibilityFocusOverlay: View {
    let session: ControllerAccessibilityNavigationSession
    let controllerInput: MenuControllerInputRouter?
    let style: ControllerNavigationOrbStyle
    let focusNeonExclusionLabels: [String]
    @State private var settings = SettingsStore.shared

    var body: some View {
        if controllerInput?.navigationZone != .tabBar,
           let frame = session.focusedOverlayFrame {
            let usesWindowFocusVisual = focusNeonExclusionLabels.contains(
                session.focusedAccessibilityLabel ?? ""
            )
            Group {
                if !usesWindowFocusVisual {
                    ControllerNavigationAnimatedOrbField(
                        targetID: session.focusedElementID ?? "focused-element",
                        targetFrame: frame,
                        primaryColor: settings.controllerNavigationAccentColor,
                        style: style,
                        controllerInput: controllerInput,
                        interactionOverride: session.latestOrbInteraction,
                        inset: 2,
                        orbScale: 0.72,
                        showsOrbs: settings.focusOrbsEnabled,
                        showsNeonOutline: true,
                        tracksTargetFrameDirectly:
                            session.isScrollPresentationActive,
                        neonCornerRadius:
                            session.focusedNeonCornerRadius ?? 12
                    )
                }
            }
            .opacity(
                session.focusPresentationIsHiddenForScrolling ? 0 : 1
            )
            .animation(
                .easeOut(duration: 0.14),
                value: session.focusPresentationIsHiddenForScrolling
            )
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .zIndex(90_000)
        }
    }
}

/// Toolbar targets can live in a `UINavigationBar`, which is composited above
/// the SwiftUI content hierarchy. Install their focus presentation directly in
/// the window so the neon and orbs cannot be hidden behind the navigation bar.
private struct ControllerAccessibilityWindowFocusOverlay: View {
    let session: ControllerAccessibilityNavigationSession
    let controllerInput: MenuControllerInputRouter?
    let style: ControllerNavigationOrbStyle
    let includedLabels: [String]
    @State private var settings = SettingsStore.shared

    var body: some View {
        ZStack(alignment: .topLeading) {
            if includedLabels.contains(
                session.focusedAccessibilityLabel ?? ""
            ), controllerInput?.isNavigationCaptured != true,
               controllerInput?.navigationZone != .tabBar,
               let frame = session.focusedFrame {
                ControllerNavigationAnimatedOrbField(
                    targetID: session.focusedElementID ?? "focused-toolbar-element",
                    targetFrame: frame,
                    primaryColor: settings.controllerNavigationAccentColor,
                    style: style,
                    controllerInput: controllerInput,
                    interactionOverride: session.latestOrbInteraction,
                    inset: 2,
                    orbScale: 0.72,
                    showsOrbs: settings.focusOrbsEnabled,
                    showsNeonOutline: true,
                    tracksTargetFrameDirectly:
                        session.isScrollPresentationActive
                )
                .opacity(
                    session.focusPresentationIsHiddenForScrolling ? 0 : 1
                )
                .animation(
                    .easeOut(duration: 0.14),
                    value: session.focusPresentationIsHiddenForScrolling
                )
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

@MainActor
private final class ControllerAccessibilityWindowFocusOverlayInstallerView:
    UIView
{
    private weak var installedWindow: UIWindow?
    private var hostingController:
        UIHostingController<ControllerAccessibilityWindowFocusOverlay>?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        installIfNeeded()
    }

    func configure(
        session: ControllerAccessibilityNavigationSession,
        controllerInput: MenuControllerInputRouter?,
        style: ControllerNavigationOrbStyle,
        includedLabels: [String]
    ) {
        let overlay = ControllerAccessibilityWindowFocusOverlay(
            session: session,
            controllerInput: controllerInput,
            style: style,
            includedLabels: includedLabels
        )
        if let hostingController {
            hostingController.rootView = overlay
        } else {
            let hostingController = UIHostingController(rootView: overlay)
            hostingController.view.backgroundColor = .clear
            hostingController.view.isUserInteractionEnabled = false
            hostingController.view.accessibilityElementsHidden = true
            self.hostingController = hostingController
        }
        installIfNeeded()
    }

    func uninstall() {
        hostingController?.view.removeFromSuperview()
        hostingController = nil
        installedWindow = nil
    }

    private func installIfNeeded() {
        guard let window, let hostedView = hostingController?.view else {
            if self.window == nil {
                hostedViewIfPresent?.removeFromSuperview()
                installedWindow = nil
            }
            return
        }
        if installedWindow !== window || hostedView.superview !== window {
            hostedView.removeFromSuperview()
            hostedView.translatesAutoresizingMaskIntoConstraints = false
            window.addSubview(hostedView)
            NSLayoutConstraint.activate([
                hostedView.leadingAnchor.constraint(equalTo: window.leadingAnchor),
                hostedView.trailingAnchor.constraint(equalTo: window.trailingAnchor),
                hostedView.topAnchor.constraint(equalTo: window.topAnchor),
                hostedView.bottomAnchor.constraint(equalTo: window.bottomAnchor),
            ])
            installedWindow = window
        } else {
            window.bringSubviewToFront(hostedView)
        }
    }

    private var hostedViewIfPresent: UIView? { hostingController?.view }
}

private struct ControllerAccessibilityWindowFocusOverlayInstaller:
    UIViewRepresentable
{
    let session: ControllerAccessibilityNavigationSession
    let controllerInput: MenuControllerInputRouter?
    let style: ControllerNavigationOrbStyle
    let includedLabels: [String]

    func makeUIView(context: Context) ->
        ControllerAccessibilityWindowFocusOverlayInstallerView
    {
        let view = ControllerAccessibilityWindowFocusOverlayInstallerView()
        view.isHidden = true
        view.isUserInteractionEnabled = false
        return view
    }

    func updateUIView(
        _ uiView: ControllerAccessibilityWindowFocusOverlayInstallerView,
        context: Context
    ) {
        uiView.configure(
            session: session,
            controllerInput: controllerInput,
            style: style,
            includedLabels: includedLabels
        )
    }

    static func dismantleUIView(
        _ uiView: ControllerAccessibilityWindowFocusOverlayInstallerView,
        coordinator: Void
    ) {
        uiView.uninstall()
    }
}

private struct ControllerAccessibilityTargetOrderModifier: ViewModifier {
    let navigationIDs: [String]
    @Environment(\.controllerAccessibilityNavigationSession) private var session
    @Environment(\.controllerAccessibilityNavigationActive) private var isActive
    @Environment(\.controllerAccessibilityTargetsSuppressed) private var isSuppressed

    func body(content: Content) -> some View {
        content
            .onAppear { publishOrderIfActive() }
            .onChange(of: navigationIDs) { _, value in
                if isActive, !isSuppressed { session?.setDeclaredOrder(value) }
            }
            .onChange(of: isActive) { _, _ in publishOrderIfActive() }
            .onChange(of: isSuppressed) { _, _ in publishOrderIfActive() }
            .onChange(of: session?.scopeRevision) { _, _ in publishOrderIfActive() }
    }

    private func publishOrderIfActive() {
        guard isActive else { return }
        session?.setDeclaredOrder(isSuppressed ? [] : navigationIDs)
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

private struct ControllerAccessibilityNavigationModifier: ViewModifier {
    let controllerInput: MenuControllerInputRouter?
    let isActive: Bool
    let scopeKey: String
    let priority: Int
    let orbStyle: ControllerNavigationOrbStyle
    let focusNeonExclusionLabels: [String]
    let onBack: (@MainActor () -> Bool)?
    let onPreviousTab: (@MainActor () -> Bool)?
    let onNextTab: (@MainActor () -> Bool)?
    let onBoundary: (@MainActor (MenuControllerCommand) -> Bool)?
    let boundaryRules: [ControllerAccessibilityBoundaryRule]
    let directionalLinks: [ControllerAccessibilityDirectionalLink]
    let prioritizesDirectionalLinks: Bool
    let confinesHorizontalFocusMovement: Bool
    let usesNativeFocusEngine: Bool
    let usesExplicitTargetGeometryOnly: Bool
    let preservesFocusDuringRightStickScrolling: Bool
    let focusScrollBehavior: ControllerAccessibilityFocusScrollBehavior
    let scrollAnimationDuration: Double
    let focusViewportEdgeMargin: CGFloat
    let focusTopAlignmentMargin: CGFloat
    let focusBottomAlignmentMargin: CGFloat
    let onActivateFocusedLabel: (@MainActor (String) -> Bool)?
    let onAdjustFocusedTarget: (@MainActor (String, String?, Bool) -> Void)?
    let preferredInitialFocusLabel: String?
    let preferredLastEntryLabel: String?
    let preferredTrailingFocusLabels: [String]
    let declaredTargetOrder: [String]?

    @State private var registrationID = UUID()
    @State private var session = ControllerAccessibilityNavigationSession()

    func body(content: Content) -> some View {
        ScrollViewReader { proxy in
            inputLifecycle(
                configurationLifecycle(navigationContent(content)),
                proxy: proxy
            )
        }
    }

    private func configurationLifecycle<Body: View>(
        _ content: Body
    ) -> some View {
        content
            .onChange(of: isActive) { _, _ in updateRegistration() }
            .onChange(of: scopeKey) { _, _ in updateRegistration() }
            .onChange(of: boundaryRules) { _, _ in updateRegistration() }
            .onChange(of: directionalLinks) { _, _ in updateRegistration() }
            .onChange(of: prioritizesDirectionalLinks) { _, _ in
                updateRegistration()
            }
            .onChange(of: confinesHorizontalFocusMovement) { _, _ in
                updateRegistration()
            }
            .onChange(of: focusScrollBehavior) { _, _ in updateRegistration() }
            .onChange(of: focusViewportEdgeMargin) { _, _ in
                updateRegistration()
            }
            .onChange(of: focusTopAlignmentMargin) { _, _ in
                updateRegistration()
            }
            .onChange(of: focusBottomAlignmentMargin) { _, _ in
                updateRegistration()
            }
            .onChange(of: preferredInitialFocusLabel) { _, _ in
                updateRegistration()
            }
            .onChange(of: preferredLastEntryLabel) { _, _ in
                updateRegistration()
            }
            .onChange(of: preferredTrailingFocusLabels) { _, _ in
                updateRegistration()
            }
            .onChange(of: declaredTargetOrder) { _, _ in updateRegistration() }
    }

    private func inputLifecycle<Body: View>(
        _ content: Body,
        proxy: ScrollViewProxy
    ) -> some View {
        content
            .onAppear {
                session.setScrollAction { id, anchor in
                    if scrollAnimationDuration > 0 {
                        let acceleration = max(
                            1,
                            controllerInput?.directionalRepeatAcceleration ?? 1
                        )
                        let duration = controllerInput?
                            .isRepeatingDirectionCommand == true
                                ? min(
                                    scrollAnimationDuration
                                        / (acceleration * acceleration),
                                    0.085 / acceleration
                                )
                                : scrollAnimationDuration
                        withAnimation(
                            .easeOut(duration: duration)
                        ) {
                            proxy.scrollTo(id, anchor: anchor)
                        }
                    } else {
                        proxy.scrollTo(id, anchor: anchor)
                    }
                }
                updateRegistration()
            }
            .onChange(of: controllerInput?.hasConnectedController) { _, _ in
                updateRegistration()
            }
            .onChange(of: controllerInput?.isMenuActive) { _, _ in
                updateRegistration()
            }
            .onChange(of: controllerInput?.focusReleaseSequence) { _, _ in
                // Analog begin/end is delivered synchronously to its owner.
                // A delayed SwiftUI observation must not hide a newer focus.
                if navigationIsActive,
                   controllerInput?.isControllerNavigationEnabled == false {
                    session.suspendFocusForScrolling()
                }
            }
            .onReceive(
                NotificationCenter.default.publisher(
                    for: ControllerEventDeliveryCoordinator
                        .nativeFocusModeDidChange
                )
            ) { _ in
                if navigationIsActive { session.synchronizeNativeFocus() }
            }
            .onDisappear {
                controllerInput?.unregisterNavigationSession(
                    id: registrationID
                )
                session.clearScrollAction()
                session.deactivate()
            }
    }

    private var navigationIsActive: Bool {
        isActive && controllerInput?.isMenuActive == true
            && controllerInput?.hasConnectedController == true
    }

    @ViewBuilder
    private func navigationContent(_ content: Content) -> some View {
        if controllerInput?.isMenuActive == true,
           controllerInput?.hasConnectedController == true {
            content
                .environment(\.controllerAccessibilityNavigationActive, navigationIsActive)
                .environment(
                    \.controllerAccessibilityNavigationSession,
                    navigationIsActive ? session : nil
                )
                .environment(
                    \.controllerAccessibilityNavigationRegistrationID,
                    navigationIsActive ? registrationID : nil
                )
                .environment(
                    \.controllerAccessibilityUsesWindowFocusVisual,
                    !focusNeonExclusionLabels.isEmpty
                )
                .buttonStyle(
                    ControllerAccessibilityRegisteringButtonStyle(
                        isActive: navigationIsActive
                            && !usesExplicitTargetGeometryOnly
                    )
                )
                .toggleStyle(
                    ControllerAccessibilityRegisteringToggleStyle(
                        isActive: navigationIsActive
                            && !usesExplicitTargetGeometryOnly
                    )
                )
                .background {
                    if navigationIsActive {
                        ZStack {
                            ControllerAccessibilityScopeProbe(session: session)
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                            // One exact scroll owner serves every managed
                            // surface. The focused probe supplies its actual
                            // UIScrollView, so Settings, Appearance, Per-Game,
                            // Quick Menu, and overlays share Emulator's path.
                            ControllerRightStickScrollTarget(
                                controllerInput: controllerInput,
                                axes: .vertical,
                                // This is a fallback for screens without a
                                // concrete List/Form marker. Screen-local owners
                                // know their private SwiftUI scroll hierarchy and
                                // must be offered the gesture first.
                                priority: priority - 10_000,
                                isEnabled: navigationIsActive,
                                preferredScrollViewProvider: {
                                    session.preferredRightStickScrollView()
                                },
                                // `session.configure` runs on appearance. Use
                                // the modifier's stable ID immediately so this
                                // target cannot mount ownerless and be removed
                                // by the router's active-session filter.
                                ownerSessionIDOverride: registrationID
                            )
                            .frame(width: 0, height: 0)
                            .allowsHitTesting(false)
                            .accessibilityHidden(true)
                            if !focusNeonExclusionLabels.isEmpty {
                                ControllerAccessibilityWindowFocusOverlayInstaller(
                                    session: session,
                                    controllerInput: controllerInput,
                                    style: orbStyle,
                                    includedLabels: focusNeonExclusionLabels
                                )
                                .frame(width: 0, height: 0)
                            }
                        }
                    }
                }
                .overlay {
                    if navigationIsActive {
                        ControllerAccessibilityFocusOverlay(
                            session: session,
                            controllerInput: controllerInput,
                            style: orbStyle,
                            focusNeonExclusionLabels: focusNeonExclusionLabels
                        )
                    }
                }
        } else { content }
    }

    private func updateRegistration() {
        session.configure(
            controllerInput: controllerInput,
            scopeKey: scopeKey,
            registrationID: registrationID,
            onBack: onBack,
            onPreviousTab: onPreviousTab,
            onNextTab: onNextTab,
            onBoundary: onBoundary,
            boundaryRules: boundaryRules,
            directionalLinks: directionalLinks,
            prioritizesDirectionalLinks: prioritizesDirectionalLinks,
            confinesHorizontalFocusMovement: confinesHorizontalFocusMovement,
            focusScrollBehavior: focusScrollBehavior,
            scrollAnimationDuration: scrollAnimationDuration,
            focusViewportEdgeMargin: focusViewportEdgeMargin,
            focusTopAlignmentMargin: focusTopAlignmentMargin,
            focusBottomAlignmentMargin: focusBottomAlignmentMargin,
            onActivateFocusedLabel: onActivateFocusedLabel,
            onAdjustFocusedTarget: onAdjustFocusedTarget,
            preferredInitialFocusLabel: preferredInitialFocusLabel,
            preferredLastEntryLabel: preferredLastEntryLabel,
            preferredTrailingFocusLabels: preferredTrailingFocusLabels
        )
        // Screens with a known semantic order must publish it before their
        // first lazy row marks the session ready. Relying only on a descendant
        // `onAppear` lets iPad List mounting accept entry with a one-row graph.
        if let declaredTargetOrder {
            session.setDeclaredOrder(declaredTargetOrder)
        }
        guard navigationIsActive, let controllerInput else {
            controllerInput?.unregisterNavigationSession(id: registrationID)
            session.deactivate()
            return
        }
        let preservesPresentation = preservesFocusDuringRightStickScrolling
        controllerInput.registerNavigationSession(
            id: registrationID,
            scopeKey: scopeKey,
            priority: priority,
            usesNativeFocusEngine: usesNativeFocusEngine,
            isReady: session.hasNavigableElements,
            handler: { [weak session] command in
                session?.handle(command) ?? false
            },
            entryHandler: { [weak session] preferLast in
                session?.focusContent(preferLast: preferLast) ?? false
            },
            scrollHandler: { [weak session] scrollView, direction in
                session?.followRightStickScroll(in: scrollView, direction: direction)
            },
            scrollStateHandler: { [weak session] scrolling in
                if scrolling {
                    session?.suspendFocusForScrolling(
                        preservingPresentation: preservesPresentation
                    )
                } else {
                    session?.resumeFocusAfterRightStickScrolling()
                }
            }
        )
        session.publishScrollFocus()
    }
}

extension View {
    func controllerAccessibilityNavigation(
        controllerInput: MenuControllerInputRouter?,
        isActive: Bool = true,
        scopeKey: String,
        priority: Int = 100,
        orbStyle: ControllerNavigationOrbStyle = .plain,
        focusNeonExclusionLabels: [String] = [],
        onBack: (@MainActor () -> Bool)? = nil,
        onPreviousTab: (@MainActor () -> Bool)? = nil,
        onNextTab: (@MainActor () -> Bool)? = nil,
        onBoundary: (@MainActor (MenuControllerCommand) -> Bool)? = nil,
        boundaryRules: [ControllerAccessibilityBoundaryRule] = [],
        directionalLinks: [ControllerAccessibilityDirectionalLink] = [],
        prioritizesDirectionalLinks: Bool = false,
        confinesHorizontalFocusMovement: Bool = false,
        // Native focus is opt-in. Most ARMSX2 surfaces publish their own stable
        // semantic graph, so device focus capability alone must never steal
        // D-pad or left-stick delivery from that graph on iPad.
        usesNativeFocusEngine: Bool = false,
        usesExplicitTargetGeometryOnly: Bool = false,
        preservesFocusDuringRightStickScrolling: Bool = false,
        focusScrollBehavior: ControllerAccessibilityFocusScrollBehavior =
            .revealIfNeeded,
        scrollAnimationDuration: Double = 0.22,
        focusViewportEdgeMargin: CGFloat = 12,
        focusTopAlignmentMargin: CGFloat = 0,
        focusBottomAlignmentMargin: CGFloat = 0,
        onActivateFocusedLabel: (@MainActor (String) -> Bool)? = nil,
        onAdjustFocusedTarget:
            (@MainActor (String, String?, Bool) -> Void)? = nil,
        preferredInitialFocusLabel: String? = nil,
        preferredLastEntryLabel: String? = nil,
        preferredTrailingFocusLabels: [String] = [],
        declaredTargetOrder: [String]? = nil
    ) -> some View {
        modifier(
            ControllerAccessibilityNavigationModifier(
                controllerInput: controllerInput,
                isActive: isActive,
                scopeKey: scopeKey,
                priority: priority,
                orbStyle: orbStyle,
                focusNeonExclusionLabels: focusNeonExclusionLabels,
                onBack: onBack,
                onPreviousTab: onPreviousTab,
                onNextTab: onNextTab,
                onBoundary: onBoundary,
                boundaryRules: boundaryRules,
                directionalLinks: directionalLinks,
                prioritizesDirectionalLinks: prioritizesDirectionalLinks,
                confinesHorizontalFocusMovement: confinesHorizontalFocusMovement,
                usesNativeFocusEngine: usesNativeFocusEngine,
                usesExplicitTargetGeometryOnly: usesExplicitTargetGeometryOnly,
                preservesFocusDuringRightStickScrolling:
                    preservesFocusDuringRightStickScrolling,
                focusScrollBehavior: focusScrollBehavior,
                scrollAnimationDuration: scrollAnimationDuration,
                focusViewportEdgeMargin: focusViewportEdgeMargin,
                focusTopAlignmentMargin: focusTopAlignmentMargin,
                focusBottomAlignmentMargin: focusBottomAlignmentMargin,
                onActivateFocusedLabel: onActivateFocusedLabel,
                onAdjustFocusedTarget: onAdjustFocusedTarget,
                preferredInitialFocusLabel: preferredInitialFocusLabel,
                preferredLastEntryLabel: preferredLastEntryLabel,
                preferredTrailingFocusLabels: preferredTrailingFocusLabels,
                declaredTargetOrder: declaredTargetOrder
            )
        )
    }

    func controllerAccessibilityTargetOrder(_ navigationIDs: [String]) -> some View {
        modifier(
            ControllerAccessibilityTargetOrderModifier(
                navigationIDs: navigationIDs
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

    func controllerAccessibilityActionTarget(
        id: String? = nil,
        label: String,
        focusedColor: Color? = nil,
        focusedNeonCornerRadius: CGFloat? = nil,
        activationFeedback: MenuControllerFeedback = .activate,
        action: @escaping () -> Void
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: nil,
                traits: .button,
                focusedColor: focusedColor,
                focusedNeonCornerRadius: focusedNeonCornerRadius,
                activationFeedback: activationFeedback,
                onActivate: action,
                onIncrement: nil,
                onDecrement: nil
            )
        )
    }

    func controllerAccessibilityMenuTarget(id: String, label: String) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: nil,
                traits: .button,
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .activate,
                onActivate: nil,
                onIncrement: nil,
                onDecrement: nil
            )
        )
    }

    func controllerAccessibilityToggleTarget(
        id: String? = nil,
        label: String,
        isOn: Binding<Bool>
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: isOn.wrappedValue ? "On" : "Off",
                traits: .adjustable,
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .toggle(isOn: !isOn.wrappedValue),
                onActivate: { isOn.wrappedValue.toggle() },
                onIncrement: { isOn.wrappedValue = true },
                onDecrement: { isOn.wrappedValue = false }
            )
        )
    }

    func controllerAccessibilityAdjustableTarget(
        id: String? = nil,
        label: String,
        value: String? = nil,
        onActivate: @escaping () -> Void,
        onIncrement: @escaping () -> Void,
        onDecrement: @escaping () -> Void
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: value,
                traits: .adjustable,
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .activate,
                onActivate: onActivate,
                onIncrement: onIncrement,
                onDecrement: onDecrement
            )
        )
    }

    func controllerAccessibilityPickerTarget(
        id: String? = nil,
        label: String,
        value: String? = nil,
        onActivate: (() -> Void)? = nil,
        onIncrement: @escaping () -> Void,
        onDecrement: @escaping () -> Void
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: value,
                traits: [.button, .adjustable],
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .activate,
                onActivate: onActivate,
                onIncrement: onIncrement,
                onDecrement: onDecrement
            )
        )
    }

    func controllerAccessibilityOptionsPickerTarget<Selection: Hashable>(
        id: String? = nil,
        label: String,
        selection: Binding<Selection>,
        options: [(id: Selection, title: String)]
    ) -> some View {
        modifier(
            ControllerAccessibilityOptionsPickerModifier(
                navigationID: id,
                label: label,
                selection: selection,
                options: options
            )
        )
    }

    func controllerFocusedTextColor(
        normal: Color = .primary,
        allowsFocusedBlue: Bool = true
    ) -> some View {
        modifier(
            ControllerFocusedForegroundModifier(
                normal: normal,
                allowsFocusedBlue: allowsFocusedBlue
            )
        )
    }

    func controllerAccessibilityFocusedNeon(
        cornerRadius: CGFloat = 12
    ) -> some View {
        modifier(
            ControllerAccessibilityFocusedNeonModifier(cornerRadius: cornerRadius)
        )
    }

    func controllerAccessibilityToolbarFocusVisual(
        baseHorizontalPadding: CGFloat = 0,
        minimumWidth: CGFloat = 36,
        fallbackHorizontalPadding: CGFloat = 0,
        fallbackMinimumHeight: CGFloat = 0
    ) -> some View {
        modifier(
            ControllerAccessibilityToolbarFocusVisualModifier(
                baseHorizontalPadding: baseHorizontalPadding,
                minimumWidth: minimumWidth,
                fallbackHorizontalPadding: fallbackHorizontalPadding,
                fallbackMinimumHeight: fallbackMinimumHeight
            )
        )
    }
}
