// MenuControllerInputRouter+GamepadInput.swift — gamepad profiles, directions, buttons and feedback
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import GameController

extension MenuControllerInputRouter {
    func playFeedback(_ feedback: MenuControllerFeedback) {
        guard isMenuActive else { return }
        // Focus publication and row layout should finish before audio/haptic
        // feedback performs any framework work on the main actor.
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, self.isMenuActive else { return }
            self.feedbackPlayer.play(feedback)
        }
    }

    /// Touch controls use their existing event-specific audio, but share the
    /// same phone and connected-controller haptic patterns as controller input.
    func playTouchHaptics(_ feedback: MenuControllerFeedback) {
        guard isMenuActive else { return }
        feedbackPlayer.playHaptics(feedback)
    }

    func refreshConnectedControllers() {
        var liveExtendedProfiles = Set<ObjectIdentifier>()
        var liveMicroProfiles = Set<ObjectIdentifier>()

        for controller in GCController.controllers() {
            // Game Controller callbacks use this queue. Keeping menu state on
            // main also makes the SwiftUI event publication deterministic.
            controller.handlerQueue = .main

            if let gamepad = controller.extendedGamepad {
                let profileID = ObjectIdentifier(gamepad)
                liveExtendedProfiles.insert(profileID)
                _ = registeredExtendedProfiles.insert(profileID)
                installExtendedProfile(gamepad)
            } else if let gamepad = controller.microGamepad {
                let profileID = ObjectIdentifier(gamepad)
                liveMicroProfiles.insert(profileID)
                _ = registeredMicroProfiles.insert(profileID)
                installMicroProfile(gamepad)
            }
        }

        registeredExtendedProfiles.formIntersection(liveExtendedProfiles)
        registeredMicroProfiles.formIntersection(liveMicroProfiles)
        hasConnectedController = !liveExtendedProfiles.isEmpty
            || !liveMicroProfiles.isEmpty
        if !hasConnectedController {
            pendingNavigationModeSwitchRequest = nil
            if activeNavigationInputMode != .touch
                || isControllerNavigationEnabled {
                activateTouchNavigation()
            } else {
                feedbackPlayer.releaseControllerResources()
            }
        }
        let liveProfiles = liveExtendedProfiles.union(liveMicroProfiles)
            .union([Self.keyboardProfileID])
        let disconnectedSamplingProfiles = directionSamplingTasks.keys.filter {
            !liveProfiles.contains($0)
        }
        for profileID in disconnectedSamplingProfiles {
            directionSamplingTasks.removeValue(forKey: profileID)?.cancel()
        }
        heldDirections = heldDirections.filter { liveProfiles.contains($0.key) }
        directionalHoldStartTimes = directionalHoldStartTimes.filter {
            liveProfiles.contains($0.key)
        }
        lastDirectionEdges = lastDirectionEdges.filter {
            liveProfiles.contains($0.key)
        }
        pressedButtons = pressedButtons.filter { liveProfiles.contains($0.key) }
        emulationMacroLatches = emulationMacroLatches.filter {
            liveProfiles.contains($0.key)
        }
        let disconnectedEmulationRepeatProfiles = emulationMacroRepeatTasks.keys.filter {
                !liveProfiles.contains($0)
            }
        for profileID in disconnectedEmulationRepeatProfiles {
            cancelEmulationMacroRepeats(profileID: profileID)
        }
        refreshPressedFaceButton()
        let disconnectedRepeatProfiles = repeatTasks.keys.filter {
            !liveProfiles.contains($0)
        }
        for profileID in disconnectedRepeatProfiles {
            cancelRepeat(profileID: profileID)
        }
        if liveExtendedProfiles.isEmpty {
            updateRightStick(x: 0, y: 0)
        }
        if isMenuActive, isControllerNavigationEnabled {
            feedbackPlayer.attach(to: GCController.controllers())
        } else {
            feedbackPlayer.releaseControllerResources()
        }
    }

    private func installExtendedProfile(_ gamepad: GCExtendedGamepad) {
        gamepad.valueChangedHandler = { [weak self] gamepad, element in
            MainActor.assumeIsolated {
                self?.handleExtendedInput(gamepad, element: element)
            }
        }
    }

    private func installMicroProfile(_ gamepad: GCMicroGamepad) {
        gamepad.valueChangedHandler = { [weak self] gamepad, element in
            MainActor.assumeIsolated {
                self?.handleMicroInput(gamepad, element: element)
            }
        }
    }

    /// Over a menu, the Undo macro undoes a save-state action instead of what its last button does.
    /// Any binding works, so it can sit on one side of the pad.
    private func completesUndoChord(_ gamepad: GCExtendedGamepad) -> Bool {
        let held = SettingsStore.shared.controller.controllerMacroUndoSaveState.isPressed(on: gamepad)
        defer { undoChordHeld = held }
        guard held, !undoChordHeld, saveStateUndoPending, frontmostManualNavigationCapture == nil else { return false }
        saveStateUndoRequest &+= 1
        return true
    }

    private func handleExtendedInput(
        _ gamepad: GCExtendedGamepad,
        element: GCControllerElement
    ) {
        if consumeLaunchInputIfNeeded(element) { return }
        let profileID = ObjectIdentifier(gamepad)

        // Gameplay macros are sampled from every profile callback, so a custom shoulder, trigger,
        // face-button, stick-click or direction binding is recognized on the same physical edge.
        // SDL polls the shared profile, so the gameplay gate also marks the chord consumed.
        if updateEmulationControllerMacros(gamepad, profileID: profileID) {
            return
        }

        guard isMenuActive else { return }
        if completesUndoChord(gamepad) { return }

        if isDirectionalElement(element, of: gamepad.dpad)
            || isDirectionalElement(element, of: gamepad.leftThumbstick) {
            scheduleDirectionUpdate(gamepad, profileID: profileID)
            return
        }

        if isDirectionalElement(element, of: gamepad.rightThumbstick) {
            updateRightStick(
                x: gamepad.rightThumbstick.xAxis.value,
                y: gamepad.rightThumbstick.yAxis.value
            )
            return
        }

        if element === gamepad.buttonA {
            updateButton(.primary, pressed: gamepad.buttonA.isPressed, command: .activate, profileID: profileID)
        } else if element === gamepad.buttonB {
            updateButton(.secondary, pressed: gamepad.buttonB.isPressed, command: .back, profileID: profileID)
        } else if element === gamepad.buttonX {
            updateButton(.square, pressed: gamepad.buttonX.isPressed, command: .showContextMenu, profileID: profileID)
        } else if element === gamepad.buttonY {
            updateButton(.triangle, pressed: gamepad.buttonY.isPressed, command: .toggleFavorite, profileID: profileID)
        } else if element === gamepad.leftShoulder {
            updateButton(.previousTab, pressed: gamepad.leftShoulder.isPressed, command: .previousTab, profileID: profileID)
        } else if element === gamepad.rightShoulder {
            updateButton(.nextTab, pressed: gamepad.rightShoulder.isPressed, command: .nextTab, profileID: profileID)
        } else if element === gamepad.leftTrigger {
            updateThemePresetButton(.previousTheme, pressed: gamepad.leftTrigger.isPressed, step: -1, profileID: profileID)
        } else if element === gamepad.rightTrigger {
            updateThemePresetButton(.nextTheme, pressed: gamepad.rightTrigger.isPressed, step: 1, profileID: profileID)
        }
    }

    private func handleMicroInput(
        _ gamepad: GCMicroGamepad,
        element: GCControllerElement
    ) {
        if consumeLaunchInputIfNeeded(element) { return }
        guard isMenuActive else { return }
        let profileID = ObjectIdentifier(gamepad)

        if isDirectionalElement(element, of: gamepad.dpad) {
            scheduleDirectionUpdate(gamepad, profileID: profileID)
        } else if element === gamepad.buttonA {
            updateButton(.primary, pressed: gamepad.buttonA.isPressed, command: .activate, profileID: profileID)
        } else if element === gamepad.buttonX {
            updateButton(.secondary, pressed: gamepad.buttonX.isPressed, command: .back, profileID: profileID)
        }
    }

    /// Some controllers publish the aggregate direction pad or an axis, while
    /// others publish the individual up/down/left/right button. Treat all of
    /// those callbacks as one directional source. This also makes simulator
    /// virtual-controller input match physical D-pads and thumbsticks.
    private func isDirectionalElement(
        _ element: GCControllerElement,
        of directionPad: GCControllerDirectionPad
    ) -> Bool {
        element === directionPad
            || element === directionPad.xAxis
            || element === directionPad.yAxis
            || element === directionPad.up
            || element === directionPad.down
            || element === directionPad.left
            || element === directionPad.right
    }

    private func consumeLaunchInputIfNeeded(
        _ element: GCControllerElement
    ) -> Bool {
        guard let launchInputInterceptor else { return false }
        let isEngaged: Bool
        if let button = element as? GCControllerButtonInput {
            isEngaged = button.isPressed
        } else if let axis = element as? GCControllerAxisInput {
            isEngaged = abs(axis.value) >= 0.5
        } else if let directionPad = element as? GCControllerDirectionPad {
            isEngaged = abs(directionPad.xAxis.value) >= 0.5
                || abs(directionPad.yAxis.value) >= 0.5
        } else {
            isEngaged = false
        }
        if isEngaged { launchInputInterceptor() }
        return true
    }

    private func extendedDirection(
        for gamepad: GCExtendedGamepad
    ) -> MenuControllerCommand? {
        let dpadX = gamepad.dpad.xAxis.value
        let dpadY = gamepad.dpad.yAxis.value
        if abs(dpadX) >= 0.5 || abs(dpadY) >= 0.5 {
            return direction(x: dpadX, y: dpadY)
        }
        return direction(
            x: gamepad.leftThumbstick.xAxis.value,
            y: gamepad.leftThumbstick.yAxis.value
        )
    }

    /// GameController may publish X, Y, and aggregate callbacks separately for
    /// one physical diagonal. Sample once after that small callback burst so a
    /// diagonal press cannot first emit a cardinal move and then a second
    /// diagonal move.
    private func scheduleDirectionUpdate(
        _ gamepad: GCExtendedGamepad,
        profileID: ObjectIdentifier
    ) {
        directionSamplingTasks[profileID]?.cancel()
        directionSamplingTasks[profileID] = Task { @MainActor [weak self, weak gamepad] in
            await Task.yield()
            guard let self, let gamepad, !Task.isCancelled else { return }
            self.directionSamplingTasks[profileID] = nil
            self.updateDirection(
                self.extendedDirection(for: gamepad),
                profileID: profileID
            )
        }
    }

    private func scheduleDirectionUpdate(
        _ gamepad: GCMicroGamepad,
        profileID: ObjectIdentifier
    ) {
        directionSamplingTasks[profileID]?.cancel()
        directionSamplingTasks[profileID] = Task { @MainActor [weak self, weak gamepad] in
            await Task.yield()
            guard let self, let gamepad, !Task.isCancelled else { return }
            self.directionSamplingTasks[profileID] = nil
            self.updateDirection(
                self.direction(
                    x: gamepad.dpad.xAxis.value,
                    y: gamepad.dpad.yAxis.value
                ),
                profileID: profileID
            )
        }
    }

    func cancelDirectionSampling() {
        for task in directionSamplingTasks.values { task.cancel() }
        directionSamplingTasks.removeAll(keepingCapacity: true)
    }

    /// Splits the plane into eight 45-degree sectors, so D-pad and analog diagonals reach the same
    /// focus geometry instead of collapsing to whichever axis is fractionally larger on the latest
    /// hardware callback.
    private func direction(x: Float, y: Float) -> MenuControllerCommand? {
        let horizontal = abs(x)
        let vertical = abs(y)
        guard max(horizontal, vertical) >= 0.58 else { return nil }

        // tan(22.5 degrees), the boundary between a cardinal sector and its diagonal neighbor.
        let diagonalRatio: Float = 0.414_213_56
        let isDiagonal = min(horizontal, vertical)
            / max(horizontal, vertical) >= diagonalRatio
        if isDiagonal {
            switch (x < 0, y < 0) {
            case (true, true): return .downLeft
            case (false, true): return .downRight
            case (true, false): return .upLeft
            case (false, false): return .upRight
            }
        }
        if horizontal > vertical { return x < 0 ? .left : .right }
        return y < 0 ? .down : .up
    }

    /// A stick can briefly cross the dead-zone while held, and some controllers publish both the
    /// aggregate D-pad and its axis edge, so a quick same-direction re-entry is the same physical
    /// detent and one press cannot skip an otherwise reachable row.
    private func updateDirection(
        _ direction: MenuControllerCommand?,
        profileID: ObjectIdentifier
    ) {
        let previous = heldDirections[profileID]
        guard previous != direction else { return }
        heldDirections[profileID] = direction

        cancelRepeat(profileID: profileID)
        guard let direction else {
            directionalHoldStartTimes.removeValue(forKey: profileID)
            return
        }

        let now = ProcessInfo.processInfo.systemUptime
        directionalHoldStartTimes[profileID] = now
        let isDuplicateEdge = lastDirectionEdges[profileID].map {
            $0.command == direction && now - $0.timestamp < 0.12
        } ?? false
        if !isDuplicateEdge {
            lastDirectionEdges[profileID] = (direction, now)
            emit(direction)
        }
        repeatTasks[profileID] = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(320))
            guard let self, !Task.isCancelled else { return }

            while !Task.isCancelled,
                  self.isMenuActive,
                  self.heldDirections[profileID] == direction {
                let heldDuration = ProcessInfo.processInfo.systemUptime
                    - (self.directionalHoldStartTimes[profileID]
                        ?? ProcessInfo.processInfo.systemUptime)
                let acceleration = self.directionalAcceleration(
                    heldDuration: heldDuration
                )
                self.emit(
                    direction,
                    isRepeat: true,
                    repeatAcceleration: acceleration
                )
                let interval = max(0.04, 0.10 / acceleration)
                try? await Task.sleep(
                    for: .milliseconds(Int((interval * 1_000).rounded()))
                )
            }
        }
    }

    /// Accelerates a sustained focus traversal without changing the precise
    /// first press. Smoothstep avoids a sudden jump when key repeat begins,
    /// then reaches 25 moves/second after a deliberate long hold.
    private func directionalAcceleration(
        heldDuration: TimeInterval
    ) -> Double {
        let progress = min(1, max(0, (heldDuration - 0.32) / 1.35))
        let eased = progress * progress * (3 - (2 * progress))
        return 1 + (1.5 * eased)
    }

    private func updateButton(
        _ button: Button,
        pressed: Bool,
        command: MenuControllerCommand,
        profileID: ObjectIdentifier
    ) {
        var buttons = pressedButtons[profileID, default: []]
        let wasPressed = buttons.contains(button)
        if pressed {
            buttons.insert(button)
        } else {
            buttons.remove(button)
        }
        pressedButtons[profileID] = buttons
        refreshPressedFaceButton()

        if pressed && !wasPressed {
            emit(command)
        }
    }

    private func refreshPressedFaceButton() {
        let allButtons = pressedButtons.values.reduce(into: Set<Button>()) {
            $0.formUnion($1)
        }
        if allButtons.contains(.triangle) {
            pressedFaceButton = .triangle
        } else if allButtons.contains(.secondary) {
            pressedFaceButton = .circle
        } else if allButtons.contains(.primary) {
            pressedFaceButton = .cross
        } else {
            pressedFaceButton = nil
        }
    }

    private static let keyboardProfileID = ObjectIdentifier(ControllerEventDeliveryCoordinator.self)

    /// Keyboard arrows arrive through UIKit instead of a GCController profile. The newest
    /// arrow still held moves, holds and repeats like a D-pad under its own profile key;
    /// controller duplicates are consumed by the event host.
    func handleKeyboardArrows(_ held: Set<MenuControllerCommand>) {
        let pressed = held.subtracting(keyboardArrowOrder)
        keyboardArrowOrder.removeAll { !held.contains($0) }
        keyboardArrowOrder.append(contentsOf: pressed)
        let command = keyboardArrowOrder.last
        guard command == nil || isMenuActive else { return }
        updateDirection(command, profileID: Self.keyboardProfileID)
    }

    private func cancelRepeat(profileID: ObjectIdentifier) {
        repeatTasks.removeValue(forKey: profileID)?.cancel()
    }
}
