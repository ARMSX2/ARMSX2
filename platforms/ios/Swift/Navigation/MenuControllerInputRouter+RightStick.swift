// MenuControllerInputRouter+RightStick.swift — right stick targets and scrolling
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import UIKit

extension MenuControllerInputRouter {
    func registerRightStickTarget(
        id: UUID,
        priority: Int,
        ownerSessionID: UUID?,
        manualCaptureOwner: String?,
        handler: @escaping @MainActor (MenuControllerScrollVector) -> Bool
    ) {
        rightStickRegistrationSequence &+= 1
        rightStickTargets[id] = RightStickTarget(
            priority: priority,
            registrationOrder: rightStickRegistrationSequence,
            ownerSessionID: ownerSessionID,
            manualCaptureOwner: manualCaptureOwner,
            handler: handler
        )
        scheduleRightStickRedispatch()
    }

    func unregisterRightStickTarget(id: UUID) {
        if let target = rightStickTargets.removeValue(forKey: id) {
            _ = target.handler(.zero)
        }
        if activeRightStickTargetID == id {
            finishRightStickScrolling(restoresFocus: false)
            activeRightStickTargetID = nil
        }
        scheduleRightStickRedispatch()
    }

    func rightStickTargetBecameUnavailable(id: UUID) {
        guard activeRightStickTargetID == id else { return }
        finishRightStickScrolling()
        activeRightStickTargetID = nil
        scheduleRightStickRedispatch()
    }

    func rightStickTargetHierarchyDidChange() {
        finishRightStickScrolling(restoresFocus: false)
        activeRightStickTargetID = nil
        scheduleRightStickRedispatch()
    }

    func scheduleRightStickRedispatch() {
        guard rightStickRedispatchTask == nil else { return }
        rightStickRedispatchTask = Task { @MainActor [weak self] in
            // UIViewRepresentable registration and focus release can arrive in
            // the same layout transaction. Resolve the scroll owner once after
            // that transaction, rather than recursively walking UIKit here.
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.rightStickRedispatchTask = nil
            self.dispatchRightStickVector()
        }
    }

    func updateRightStick(x: Float, y: Float) {
        let vector = MenuControllerScrollVector(
            x: normalizedRightStickAxis(x),
            y: normalizedRightStickAxis(y)
        )
        guard vector != rightStickVector else { return }
        let beganScrolling = rightStickVector.isZero && !vector.isZero
        let endedScrolling = !rightStickVector.isZero && vector.isZero
        let changedDirection = !rightStickVector.isZero && !vector.isZero
            && (rightStickVector.x * vector.x
                + rightStickVector.y * vector.y) <= 0
        if !vector.isZero {
            guard prepareControllerNavigationForInput() else {
                rightStickVector = .zero
                return
            }
        }
        rightStickVector = vector
        if beganScrolling || changedDirection {
            rightStickHoldStartTime = CACurrentMediaTime()
        }
        if beganScrolling {
            requestFocusRelease()
        }
        dispatchRightStickVector()
        if endedScrolling {
            rightStickHoldStartTime = nil
            // Publish after every registered target has consumed `.zero`, so
            // focus restoration observes the final UIKit content offset.
            rightStickScrollEndSequence &+= 1
        }
    }

    /// Right-stick scrolling is continuous, so accelerate velocity instead of
    /// producing more input events. The first quarter-second remains precise;
    /// a sustained deflection then ramps smoothly to 3.25x speed.
    func rightStickScrollAcceleration(
        at timestamp: CFTimeInterval
    ) -> CGFloat {
        guard let rightStickHoldStartTime else { return 1 }
        let progress = min(
            1,
            max(0, (timestamp - rightStickHoldStartTime - 0.25) / 1.25)
        )
        let eased = progress * progress * (3 - (2 * progress))
        return CGFloat(1 + (2.25 * eased))
    }

    private func normalizedRightStickAxis(_ value: Float) -> Float {
        let deadZone: Float = 0.16
        let magnitude = abs(value)
        guard magnitude > deadZone else { return 0 }
        let normalized = min(1, (magnitude - deadZone) / (1 - deadZone))
        return value < 0 ? -normalized : normalized
    }

    func dispatchRightStickVector() {
        globalScrollDriver.accelerationProvider = { [weak self] timestamp in
            self?.rightStickScrollAcceleration(at: timestamp) ?? 1
        }
        let activeCaptureOwner = frontmostManualNavigationCapture?.key
        let activeSessionEntry = activeCaptureOwner == nil && navigationZone != .tabBar
            ? frontmostNavigationSessionEntry
            : nil
        if isRightStickScrolling,
           rightStickSessionOwner?.id != activeSessionEntry?.key
            || rightStickSessionOwner?.scopeKey != activeSessionEntry?.value.scopeKey {
            finishRightStickScrolling(restoresFocus: false)
        }
        let globalOwner = activeSessionEntry.map { (id: $0.key, scopeKey: $0.value.scopeKey) }
        globalScrollDriver.onScroll = { [weak self] scrollView, direction in
            self?.didScrollRightStick(
                in: scrollView, direction: direction, targetID: nil,
                ownerSessionID: globalOwner?.id, ownerScopeKey: globalOwner?.scopeKey
            )
        }
        globalScrollDriver.onStopped = { [weak self] in
            self?.rightStickDriverStopped(targetID: nil)
        }
        if let session = activeSessionEntry?.value {
            globalScrollDriver.setPreferredFocus(
                window: session.scrollWindow,
                frame: session.scrollFocusFrame,
                requiresPreferredFocus: true
            )
        } else {
            globalScrollDriver.setPreferredFocus(
                window: nil,
                frame: nil,
                requiresPreferredFocus: false
            )
        }
        guard isMenuActive, !rightStickVector.isZero else {
            activeRightStickTargetID = nil
            for target in rightStickTargets.values {
                _ = target.handler(.zero)
            }
            _ = globalScrollDriver.consume(.zero)
            finishRightStickScrolling()
            return
        }

        let activeSessionID = activeSessionEntry?.key
        let eligibleTargets = rightStickTargets.filter { _, target in
            if let activeCaptureOwner {
                return target.manualCaptureOwner == activeCaptureOwner
            }
            if let activeSessionID {
                return target.ownerSessionID == activeSessionID
            }
            return target.ownerSessionID == nil
                && target.manualCaptureOwner == nil
        }
        let sortedTargets = eligibleTargets.sorted { lhs, rhs in
            if let activeSessionID {
                let lhsOwned = lhs.value.ownerSessionID == activeSessionID
                let rhsOwned = rhs.value.ownerSessionID == activeSessionID
                if lhsOwned != rhsOwned { return lhsOwned }
            }
            if lhs.value.priority != rhs.value.priority {
                return lhs.value.priority > rhs.value.priority
            }
            if lhs.value.registrationOrder != rhs.value.registrationOrder {
                return lhs.value.registrationOrder
                    > rhs.value.registrationOrder
            }
            return lhs.key.uuidString < rhs.key.uuidString
        }
        var selectedTargetID: UUID?
        for (id, target) in sortedTargets {
            if target.handler(rightStickVector) {
                selectedTargetID = id
                break
            }
        }
        if activeRightStickTargetID != selectedTargetID {
            finishRightStickScrolling(restoresFocus: selectedTargetID == nil)
        }
        activeRightStickTargetID = selectedTargetID

        // Keep the selected target's display link alive as analog values
        // change. Only inactive or newly-covered surfaces receive a stop.
        for (id, target) in rightStickTargets where id != selectedTargetID {
            _ = target.handler(.zero)
        }
        let driverIsRunning: Bool
        if selectedTargetID != nil || activeCaptureOwner != nil || activeSessionEntry != nil {
            // A managed pane at its boundary must not fall through to a global
            // search and scroll a sibling column or retained page. Its local
            // markers will redispatch once a replacement surface mounts.
            _ = globalScrollDriver.consume(.zero)
            driverIsRunning = selectedTargetID != nil
        } else {
            driverIsRunning = globalScrollDriver.consume(rightStickVector)
        }
        if driverIsRunning {
            if !isRightStickScrolling {
                isRightStickScrolling = true
                rightStickSessionOwner = globalOwner
                lastRightStickScrollDirection = nil
                ControllerEventDeliveryCoordinator.shared.setRightStickScrolling(true)
                activeSessionEntry?.value.scrollStateHandler(true)
            }
        } else {
            finishRightStickScrolling()
        }
    }

    func finishRightStickScrolling(restoresFocus: Bool = true) {
        let owner = rightStickSessionOwner
        let wasScrolling = isRightStickScrolling
        isRightStickScrolling = false
        rightStickSessionOwner = nil
        ControllerEventDeliveryCoordinator.shared.setRightStickScrolling(false)
        // Clear ownership before reconciliation: the final corridor reveal is
        // now directional scrolling, not a competing analog animation.
        if wasScrolling, restoresFocus, isControllerNavigationEnabled,
           navigationZone != .tabBar, frontmostManualNavigationCapture == nil,
           let owner, let target = navigationSessions[owner.id],
           target.scopeKey == owner.scopeKey,
           frontmostNavigationSessionEntry?.key == owner.id {
            target.scrollStateHandler(false)
        }
    }

    func rightStickDriverStopped(targetID: UUID?) {
        guard activeRightStickTargetID == targetID else { return }
        finishRightStickScrolling()
    }

    func navigationScope(for id: UUID?) -> String? {
        id.flatMap { navigationSessions[$0]?.scopeKey }
    }

    func ownsRightStickScroll(
        targetID: UUID?, ownerSessionID: UUID?, ownerScopeKey: String?
    ) -> Bool {
        guard isRightStickScrolling, activeRightStickTargetID == targetID else { return false }
        guard let ownerSessionID else { return rightStickSessionOwner == nil }
        return navigationZone != .tabBar && frontmostManualNavigationCapture == nil
            && frontmostNavigationSessionEntry?.key == ownerSessionID
            && navigationSessions[ownerSessionID]?.scopeKey == ownerScopeKey
            && rightStickSessionOwner?.id == ownerSessionID
            && rightStickSessionOwner?.scopeKey == ownerScopeKey
    }

    func cancelRepeat() {
        for task in repeatTasks.values { task.cancel() }
        repeatTasks.removeAll(keepingCapacity: true)
    }

    func didScrollRightStick(
        in scrollView: UIScrollView,
        direction: MenuControllerCommand,
        targetID: UUID?,
        ownerSessionID: UUID?,
        ownerScopeKey: String?
    ) {
        guard isMenuActive, isControllerNavigationEnabled,
              isRightStickScrolling, activeRightStickTargetID == targetID else { return }
        lastRightStickScrollDirection = direction
        guard navigationZone != .tabBar,
              frontmostManualNavigationCapture == nil,
              let entry = frontmostNavigationSessionEntry, entry.value.isReady,
              entry.key == ownerSessionID,
              entry.value.scopeKey == ownerScopeKey,
              rightStickSessionOwner?.id == entry.key,
              rightStickSessionOwner?.scopeKey == entry.value.scopeKey else { return }
        entry.value.scrollHandler(scrollView, direction)
    }
}
