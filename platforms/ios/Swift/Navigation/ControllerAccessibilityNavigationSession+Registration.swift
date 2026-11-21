// ControllerAccessibilityNavigationSession+Registration.swift — target registration and activation
// SPDX-License-Identifier: GPL-3.0+

import UIKit

extension ControllerAccessibilityNavigationSession {
    @discardableResult
    func registerTarget(
        registrationID: UUID,
        view: ControllerAccessibilityActionProbeView,
        baseKey: String,
        navigationID: String?,
        label: String,
        value: String?,
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

        // Registration runs inside UIViewRepresentable updates while a lazy
        // List/Form may still be laying out, so focus, presentation state and
        // scrolling are reconciled on the next actor turn, in a stable hierarchy.
        if existing == nil || ownerChanged || availabilityChanged {
            scheduleMountedTargetReconciliation(key: resolvedKey)
            updateReadiness()
        } else if focusedKey == resolvedKey,
                  existing?.label != label || existing?.lastFrame != frame {
            scheduleFocusedPresentationUpdate()
        }
        return resolvedKey
    }

    func unregisterTarget(registrationID: UUID, owner: UIView) {
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

    func targetGeometryDidChange(
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

        // Scrolling moves probes only in window space, so skip the row updates it would publish.
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

    func activate(_ key: String) -> Bool {
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

    func scheduleMountedTargetReconciliation(key: String) {
        guard focusedKey == key || pendingFocusKey == key,
              mountedTargetTask == nil else { return }
        mountedTargetTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.mountedTargetTask = nil
            // Resolve the current owner after yielding: a replacement probe
            // may have registered while this single reconciliation was queued.
            guard let key = self.pendingFocusKey ?? self.focusedKey,
                  self.targets[key]?.view.value != nil,
                  self.isMountedAndEnabled(key) else { return }
            if self.pendingFocusKey == key {
                guard !self.needsLogicalScrollOwner(for: key) else { return }
                // The first seek only mounts a lazy row; reconcile once for the edge preview gaps.
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
                self.publishScrollFocus()
            }
        }
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

    static func explicitKey(_ id: String) -> String {
        "controller.focus.id.\(id)"
    }

    /// Settings hands back the router's remembered key as its preferred label. Matching wraps a
    /// label in `explicitKey` again, so that key never matched and Back landed on the first row.
    static func navigationID(_ label: String) -> String {
        let prefix = explicitKey("")
        return label.hasPrefix(prefix) ? String(label.dropFirst(prefix.count)) : label
    }
}
