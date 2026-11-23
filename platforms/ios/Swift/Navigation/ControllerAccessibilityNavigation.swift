// ControllerAccessibilityNavigation.swift — native-first controller focus
// SPDX-License-Identifier: GPL-3.0+

import Observation
import QuartzCore
import SwiftUI
import UIKit

struct ControllerAccessibilityDirectionalLink: Equatable {
    static let scrollBoundary = "controller.scroll-boundary"
    static let firstContent = "controller.first-content"
    static let navigationBoundary = "controller.navigation-boundary"
    private static let nearestPrefix = "controller.nearest:"

    /// The target among `labels` that the spatial lookup ranks first in the link's direction.
    static func nearest(of labels: [String]) -> String {
        nearestPrefix + labels.joined(separator: "\n")
    }

    static func nearestLabels(_ label: String) -> [String]? {
        guard label.hasPrefix(nearestPrefix) else { return nil }
        return label.dropFirst(nearestPrefix.count).split(separator: "\n").map(String.init)
    }

    let fromLabel: String
    let direction: MenuControllerCommand
    let toLabel: String
}

struct ControllerAccessibilityBoundaryRule: Equatable {
    let direction: MenuControllerCommand
    let fromLabel: String
    var requiresFreshPress: Bool = false
}

enum ControllerAccessibilityFocusScrollBehavior: Equatable {
    /// Preserve the current viewport while the focused row remains visible.
    case revealIfNeeded
    /// Keep the focused row inside configurable top and bottom boundaries.
    /// Scroll only when it crosses an edge, retaining its natural travel
    /// through the viewport instead of pinning every row to the top.
    case maintainWithinViewport

    var anchor: UnitPoint {
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
final class ControllerAccessibilityTargetFocusState {
    var isFocused = false
}

/// A List/Form marker remains beside its real scroll surface even when Liquid
/// Glass portals a row's controls out of the collection-view hierarchy.
@MainActor
protocol ControllerAccessibilityScrollSurface: AnyObject {
    func scrollViewForFocusTarget(_ target: UIView) -> UIScrollView?
}

@MainActor
final class ControllerAccessibilityDisplayLinkDriver: NSObject {
    var onFrame: ((CFTimeInterval) -> Void)?

    @objc func displayLinkDidFire(_ displayLink: CADisplayLink) {
        onFrame?(displayLink.timestamp)
    }
}

@MainActor
@Observable
final class ControllerAccessibilityNavigationSession {
    final class WeakView {
        weak var value: UIView?
        init(_ value: UIView?) { self.value = value }
    }

    struct Target {
        let key: String
        let baseKey: String
        let registrationID: UUID
        var navigationID: String?
        var label: String
        var value: String?
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

    struct FocusPresentation: Equatable {
        var frame: CGRect?
        var overlayFrame: CGRect?
        var accessibilityLabel: String?

        static let empty = FocusPresentation(
            frame: nil,
            overlayFrame: nil,
            accessibilityLabel: nil
        )
    }

    struct FocusMotion {
        let sourceFrame: CGRect
        let duration: TimeInterval
        let style: ControllerNavigationFocusTravelStyle
        weak var scrollView: UIScrollView?
        let sourceOffset: CGPoint
        let destinationOffset: CGPoint
    }

    @ObservationIgnored weak var controllerInput: MenuControllerInputRouter?
    @ObservationIgnored private weak var scopeView: UIView?
    @ObservationIgnored weak var window: UIWindow?
    @ObservationIgnored private var measuredScopeFrame = CGRect.zero
    /// Measured when read: an ancestor can move the scope without laying it out again.
    var scopeFrame: CGRect {
        guard let scopeView, let window = scopeView.window else { return measuredScopeFrame }
        return scopeView.convert(scopeView.bounds, to: window)
    }
    @ObservationIgnored private var scopeKey = ""
    @ObservationIgnored private var registrationID: UUID?
    @ObservationIgnored var targets: [String: Target] = [:]
    @ObservationIgnored var scrollOwners: [String: WeakView] = [:]
    @ObservationIgnored var registrationKeys: [UUID: String] = [:]
    @ObservationIgnored let scrollSurfaces = NSHashTable<AnyObject>.weakObjects()
    @ObservationIgnored var duplicateBaseKeys = Set<String>()
    @ObservationIgnored var registrationSequence: UInt64 = 0
    @ObservationIgnored var declaredOrder: [String] = []
    @ObservationIgnored var declaredOrderSet = Set<String>()
    @ObservationIgnored var declaredOrderIndex: [String: Int] = [:]
    @ObservationIgnored var focusedKey: String?
    @ObservationIgnored var permitsAutomaticFocus = true
    @ObservationIgnored var scrollToTarget: ((String, UnitPoint) -> Void)?
    @ObservationIgnored var onBack: (@MainActor () -> Bool)?
    @ObservationIgnored var onContextMenu: (@MainActor (String) -> Bool)?
    @ObservationIgnored var onPreviousTab: (@MainActor () -> Bool)?
    @ObservationIgnored var onNextTab: (@MainActor () -> Bool)?
    @ObservationIgnored var onBoundary: (@MainActor (MenuControllerCommand) -> Bool)?
    @ObservationIgnored var boundaryRules: [ControllerAccessibilityBoundaryRule] = []
    @ObservationIgnored var directionalLinks: [ControllerAccessibilityDirectionalLink] = []
    @ObservationIgnored var pageDirectionalLinks: [ControllerAccessibilityDirectionalLink] = []
    @ObservationIgnored var prioritizesDirectionalLinks = false
    @ObservationIgnored var confinesHorizontalFocusMovement = false
    @ObservationIgnored var wrapsAtListEnds = false
    @ObservationIgnored var focusScrollBehavior:
        ControllerAccessibilityFocusScrollBehavior = .revealIfNeeded
    @ObservationIgnored var scrollAnimationDuration: Double = 0.22
    @ObservationIgnored var focusViewportEdgeMargin: CGFloat = 12
    @ObservationIgnored var focusTopAlignmentMargin: CGFloat = 0
    @ObservationIgnored var focusBottomAlignmentMargin: CGFloat = 0
    @ObservationIgnored var onActivateFocusedLabel: (@MainActor (String) -> Bool)?
    @ObservationIgnored var onAdjustFocusedTarget:
        (@MainActor (String, String?, Bool) -> Void)?
    @ObservationIgnored var preferredInitialFocusLabel: String?
    @ObservationIgnored var preferredLastEntryLabel: String?
    @ObservationIgnored var preferredTrailingFocusLabels: [String] = []
    @ObservationIgnored var mountedTargetTask: Task<Void, Never>?
    @ObservationIgnored private var focusedPresentationTask: Task<Void, Never>?
    @ObservationIgnored var focusPresentationRevealTask:
        Task<Void, Never>?
    @ObservationIgnored var scrollPresentationDisplayLink: CADisplayLink?
    @ObservationIgnored var scrollPresentationDriver:
        ControllerAccessibilityDisplayLinkDriver?
    @ObservationIgnored var scrollPresentationKey: String?
    @ObservationIgnored var scrollPresentationStartTime: CFTimeInterval = 0
    @ObservationIgnored var scrollPresentationLastFrame: CGRect?
    @ObservationIgnored var scrollPresentationStableFrameCount = 0
    @ObservationIgnored var focusMotion: FocusMotion?
    @ObservationIgnored var scrollPresentationCompletionTask:
        Task<Void, Never>?
    @ObservationIgnored private var readinessTask: Task<Void, Never>?
    @ObservationIgnored var pendingFocusKey: String?
    @ObservationIgnored var ownerDeclaresOrder = false
    @ObservationIgnored var isRevealingRows = false
    // Preserve input timing across the asynchronous lazy-row mount.
    @ObservationIgnored var focusRepeatAcceleration: Double?
    @ObservationIgnored var pendingDirectionalMove:
        (direction: MenuControllerCommand, acceleration: Double?)?
    @ObservationIgnored var pendingFocusExpiryTask: Task<Void, Never>?
    @ObservationIgnored private var scopeRestorationTask: Task<Void, Never>?
    @ObservationIgnored var adjustedValuePublicationTask:
        Task<Void, Never>?
    @ObservationIgnored private var pendingScrollFocusPublication = false
    @ObservationIgnored var focusIsSuspendedForScrolling = false
    @ObservationIgnored weak var rightStickScrollView: UIScrollView?
    @ObservationIgnored var rightStickManualFocusKey: String?
    @ObservationIgnored var rightStickFocusCandidates: [ControllerNavigationScrollFocusCandidate]?
    @ObservationIgnored var rightStickViewportSize = CGSize.zero
    @ObservationIgnored private weak var publishedScrollWindow: UIWindow?
    @ObservationIgnored private var publishedScrollFrame: CGRect?
    @ObservationIgnored private var presentedFocusKey: String?
    var focusPresentationIsHiddenForScrolling = false
    @ObservationIgnored private var interactionSequence: UInt64 = 0

    var focusPresentation = FocusPresentation.empty
    private(set) var latestOrbInteraction: ControllerNavigationOrbInteraction?
    private(set) var hasNavigableElements = false
    private(set) var scopeRevision: UInt64 = 0
    var isScrollPresentationActive = false

    var registrationIdentifier: UUID? { registrationID }

    /// A sheet or panel opened over this screen takes the input, so this screen's ring goes.
    func ownsInput(in controllerInput: MenuControllerInputRouter?) -> Bool {
        guard let inputSession = controllerInput?.navigationInputSessionID else { return true }
        return inputSession == registrationID
    }

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
        onContextMenu: (@MainActor (String) -> Bool)?,
        onPreviousTab: (@MainActor () -> Bool)?,
        onNextTab: (@MainActor () -> Bool)?,
        onBoundary: (@MainActor (MenuControllerCommand) -> Bool)?,
        boundaryRules: [ControllerAccessibilityBoundaryRule],
        directionalLinks: [ControllerAccessibilityDirectionalLink],
        prioritizesDirectionalLinks: Bool,
        confinesHorizontalFocusMovement: Bool,
        wrapsAtListEnds: Bool,
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
        self.onContextMenu = onContextMenu
        self.onPreviousTab = onPreviousTab
        self.onNextTab = onNextTab
        self.onBoundary = onBoundary
        self.boundaryRules = boundaryRules
        self.directionalLinks = directionalLinks
        self.prioritizesDirectionalLinks = prioritizesDirectionalLinks
        self.confinesHorizontalFocusMovement = confinesHorizontalFocusMovement
        self.wrapsAtListEnds = wrapsAtListEnds
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
        scrollOwners.removeAll()
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

    func setPageDirectionalLinks(_ links: [ControllerAccessibilityDirectionalLink]) {
        pageDirectionalLinks = links
    }

    func setDeclaredOrder(_ navigationIDs: [String]) {
        let next = navigationIDs.compactMap { raw -> String? in
            let value = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            return value.isEmpty ? nil : Self.explicitKey(value)
        }
        guard next != declaredOrder else { return }
        let previousOrder = declaredOrderSet
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
        if let focusedKey, previousOrder.contains(focusedKey), !declaredOrderSet.contains(focusedKey) {
            refocusReplacement(of: focusedKey, added: declaredOrderSet.subtracting(previousOrder))
        }
        updateReadiness()
    }

    /// The focused row left the order for good, as Save does once a slot holds a state. Its
    /// persistent entry kept the ring on the empty spot. Focus the closest row that replaced it,
    /// or else the closest row, once the replacements have registered.
    private func refocusReplacement(of droppedKey: String, added: Set<String>) {
        guard let frame = targets[droppedKey]?.lastFrame else { return }
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, self.focusedKey == droppedKey else { return }
            // Storage takes its buttons out of the order while it cleans; they are still there.
            if self.targets[droppedKey]?.view.value?.window != nil { return }
            let mounted = self.declaredOrder.filter(self.isMountedAndEnabled)
            let replacements = mounted.filter(added.contains)
            let nearest = (replacements.isEmpty ? mounted : replacements).min { lhs, rhs in
                self.distance(from: frame, to: lhs) < self.distance(from: frame, to: rhs)
            }
            if let nearest { self.setFocus(nearest) }
        }
    }

    private func distance(from frame: CGRect, to key: String) -> CGFloat {
        guard let other = targets[key]?.lastFrame else { return .greatestFiniteMagnitude }
        return hypot(other.midX - frame.midX, other.midY - frame.midY)
    }

    func updateScope(view: UIView) {
        let nextWindow = view.window
        let nextFrame = nextWindow.map { view.convert(view.bounds, to: $0) }
            ?? .zero
        guard scopeView !== view || window !== nextWindow
                || measuredScopeFrame != nextFrame else { return }
        scopeView = view
        window = nextWindow
        measuredScopeFrame = nextFrame
        scheduleFocusedPresentationUpdate()
    }

    func deactivate() {
        rememberCurrentFocus()
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
        measuredScopeFrame = .zero
        // The mounted modifier still owns its ScrollViewReader. Its onDisappear
        // clears this callback; temporary deactivation must not discard it.
        permitsAutomaticFocus = true
        updateReadiness()
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

    func setFocus(
        _ key: String,
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
        if focusedKey == key {
            if targets[key]?.focusState.isFocused == false {
                targets[key]?.focusState.isFocused = true
            }
            let beganScroll = scrolls && reveal(key)
            if !beganScroll,
               !(isScrollPresentationActive && scrollPresentationKey == key) {
                updateFocusedPresentation()
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
            // An offscreen seek tracks the display link before its lazy row mounts,
            // so keep that tracker rather than publish the row's edge-clipped frame.
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
        publishScrollFocus()
    }

    func clearFocus() {
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
        for key in targets.keys where targets[key]?.focusState.isFocused == true {
            targets[key]?.focusState.isFocused = false
        }
        guard focusedKey != nil || focusPresentation != .empty else { return }
        focusedKey = nil
        setFocusedPresentation(.empty)
        publishScrollFocus()
    }

    func rememberCurrentFocus() {
        guard !scopeKey.isEmpty,
              let key = focusedKey ?? pendingFocusKey else { return }
        controllerInput?.rememberNavigationFocusKey(key, forScope: scopeKey)
    }

    func queueScopeFocusRestoration(_ key: String) {
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

    func scheduleScopeRestorationSeek(_ key: String) {
        scopeRestorationTask?.cancel()
        scopeRestorationTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled,
                  self.pendingFocusKey == key else { return }
            self.scopeRestorationTask = nil
            self.focusOrSeek(key)
        }
    }

    func scheduleFocusedPresentationUpdate(
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

    func updateReadiness() {
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

    func updateFocusedPresentation(frameOverride: CGRect? = nil) {
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

    func setFocusedPresentation(_ presentation: FocusPresentation) {
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

    func publishOrbInteractionIfNeeded(
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

}

/// A single non-spatial transition for controller-focused text and symbols.
/// Keeping this separate from focus-box travel lets colors cross-fade without
/// adding latency to directional navigation or scroll geometry.
enum ControllerFocusVisualAnimation {
    static let textFade = Animation.easeInOut(duration: 0.16)
}
