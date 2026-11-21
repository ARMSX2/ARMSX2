// ControllerAccessibilityNavigationSession+Commands.swift — commands and focus movement
// SPDX-License-Identifier: GPL-3.0+

import UIKit

extension ControllerAccessibilityNavigationSession {
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
        case .showContextMenu:
            // Square opens the focused row's menu on screens that have one.
            guard let onContextMenu else { return false }
            if let focusedKey, let id = targets[focusedKey]?.navigationID, onContextMenu(id) {
                controllerInput?.playFeedback(.activate)
            } else {
                controllerInput?.playFeedback(.boundary)
            }
            return true
        case .toggleFavorite:
            return false
        case .left, .right:
            if let focusedKey {
                if adjust(focusedKey, increment: command == .right) {
                    return true
                }
                // A confined settings column may still declare a horizontal link,
                // such as Cancel/Close <-> Save. Honor it before treating horizontal
                // input as a boundary; arbitrary spatial movement stays confined.
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
        let preferred = (preferLast ? preferredLastEntryLabel : preferredInitialFocusLabel)
            .map(Self.navigationID)
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

    func move(
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
            return revealMoreRows(direction) || wrapToOtherEnd(direction)
                || performBoundary(direction)
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

    /// A page without a declared order only knows its mounted rows. When the
    /// rows past the focus are disabled or plain text, the next control can sit
    /// outside the List's mounted range, so scroll half a screen and try again.
    private func revealMoreRows(_ direction: MenuControllerCommand) -> Bool {
        guard declaredOrder.isEmpty, controllerInput?.isRightStickScrolling != true,
              let focusedKey, let view = targets[focusedKey]?.view.value,
              let scrollView = enclosingScrollView(for: view) else { return false }
        if isRevealingRows {
            scheduleFocusedPresentationUpdate()
            return true
        }
        // Only content counts, not the margins kept clear for the tab bar.
        let offset = scrollView.contentOffset.y
        let end = max(0, scrollView.contentSize.height - scrollView.bounds.height)
        let step = scrollView.bounds.height / 2
        let next = direction == .down ? min(end, offset + step) : max(0, offset - step)
        guard direction == .down ? next > offset + 1 : next < offset - 1 else { return false }
        scrollView.setContentOffset(CGPoint(x: scrollView.contentOffset.x, y: next), animated: false)
        scrollView.layoutIfNeeded()
        isRevealingRows = true
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self else { return }
            _ = self.move(direction)
            self.isRevealingRows = false
        }
        return true
    }

    /// A new press past the last row jumps to the first one, and Up past the first to the last.
    private func wrapToOtherEnd(_ direction: MenuControllerCommand) -> Bool {
        guard wrapsAtListEnds, focusRepeatAcceleration == nil,
              controllerInput?.isRightStickScrolling != true,
              let focusedKey, let view = targets[focusedKey]?.view.value,
              let scrollView = enclosingScrollView(for: view) else { return false }
        let inset = scrollView.adjustedContentInset
        let bottom = scrollView.contentSize.height - scrollView.bounds.height + inset.bottom
        let y = direction == .down ? -inset.top : max(-inset.top, bottom)
        scrollView.setContentOffset(CGPoint(x: scrollView.contentOffset.x, y: y), animated: false)
        expandTitleAtTop(scrollView)
        scrollView.layoutIfNeeded()
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, self.focusedKey == focusedKey else { return }
            let keys = self.orderedKeys(includingUnmountedDeclared: true)
            if let key = direction == .down ? keys.first : keys.last { self.focusOrSeek(key) }
        }
        controllerInput?.playFeedback(.move(direction))
        return true
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

    func focusInitialIfPossible() -> Bool {
        guard focusedKey == nil, permitsAutomaticFocus else { return false }
        return focusContent(preferLast: false)
    }

    func orderedKeys(
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

    func isMountedAndEnabled(_ key: String) -> Bool {
        guard let target = targets[key], target.isEnabled,
              let view = target.view.value else { return false }
        return view.window != nil && view.superview != nil
    }

    func needsLogicalScrollOwner(for key: String) -> Bool {
        if let target = targets[key],
           preferredTrailingFocusLabels.contains(where: { matches($0, target: target) }) {
            return false
        }
        guard declaredOrder.contains(key), let view = targets[key]?.view.value,
              enclosingScrollView(for: view) == nil,
              let frame = frameForView(view) else { return false }
        // A mounted glass portal may be offscreen before its scroll marker
        // associates it: seek its row instead of publishing a focus rectangle
        // nothing can reveal. Fixed visible controls (modal footers too) need no owner.
        return !frame.intersects(scopeFrame)
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
        guard prioritizesDirectionalLinks || !directionalLinks.isEmpty
                || !pageDirectionalLinks.isEmpty,
              let target = targets[key] else { return nil }
        let firstContentKey = firstContentTarget(in: orderedKeys())
        return (directionalLinks + pageDirectionalLinks).first(where: { link in
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
        if let labels = ControllerAccessibilityDirectionalLink.nearestLabels(label) {
            let candidates = keys.filter { key in
                targets[key].map { target in labels.contains { matches($0, target: target) } } ?? false
            }
            return spatialDestination(from: sourceKey, direction: direction, keys: candidates)
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

    func matches(_ value: String, target: Target) -> Bool {
        target.navigationID == value || target.label == value
            || target.baseKey == Self.explicitKey(value)
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
