// OrbitKeysModel+Input.swift — touch key selection and controller input
// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.

import Foundation
@preconcurrency import GameController

extension OrbitKeysModel {
    func selectFullQwertyKey(_ position: OrbitKeysFullQwertyKeyPosition) {
        selectFullQwertyKey(position, playsAudio: true)
    }

    private func selectFullQwertyKey(
        _ position: OrbitKeysFullQwertyKeyPosition,
        playsAudio: Bool
    ) {
        let nextPosition = OrbitKeysFullQwertyLayout.clampedPosition(position, mode: mode)
        guard nextPosition != fullQwertySelection || fullQwertyToolbarSelection != nil else {
            return
        }
        fullQwertyToolbarSelection = nil
        fullQwertySelection = nextPosition
        selectionSequence += 1
        haptics.play(.selection)
        if playsAudio {
            MenuAudioPackManager.shared.playEvent(.navigation)
        }
    }

    func activateFullQwertyKey(at position: OrbitKeysFullQwertyKeyPosition) {
        selectFullQwertyKey(position, playsAudio: false)
        activateFullQwertySelection()
    }

    func activateFullQwertySelection() {
        guard isKeyboardVisible else { return }

        if let toolbarSelection = fullQwertyToolbarSelection {
            switch toolbarSelection {
            case .send:
                performPrimaryEditorAction()
            case .settings:
                openKeyboardSettings()
            case .close:
                cancelAndClose()
            }
            return
        }

        guard
            let key = OrbitKeysFullQwertyLayout.key(at: fullQwertySelection, mode: mode)
        else {
            return
        }

        performFullQwertyKey(key)
    }

    func setFullQwertyTouchBackspaceHeld(_ isHeld: Bool) {
        if isHeld {
            guard fullQwertyTouchBackspaceRepeatTask == nil else { return }
            performFullQwertyTouchBackspace()
            fullQwertyTouchBackspaceRepeatTask = makeRepeatTask(
                initialDelay: 420,
                repeatDelay: 82
            ) { model in
                model.performFullQwertyTouchBackspace()
                return true
            }
        } else {
            stopFullQwertyTouchBackspaceRepeat()
        }
    }

    func observeControllers() {
        let center = NotificationCenter.default
        observers.append(
            center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) {
                [weak self] notification in
                guard let controller = notification.object as? GCController else { return }
                Task { @MainActor in self?.connect(controller) }
            }
        )
        observers.append(
            center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) {
                [weak self] notification in
                guard let controller = notification.object as? GCController else { return }
                Task { @MainActor in self?.disconnect(controller) }
            }
        )
    }

    func connect(_ controller: GCController) {
        guard activeController == nil || activeController === controller else { return }
        guard let gamepad = controller.extendedGamepad else { return }
        activeController = controller
        GCController.stopWirelessControllerDiscovery()
        if isKeyboardVisible {
        }

        gamepad.leftThumbstick.valueChangedHandler = { [weak self] _, x, y in
            Task { @MainActor in self?.handleStick(x: x, y: y) }
        }
        gamepad.rightThumbstick.valueChangedHandler = { [weak self] _, x, y in
            Task { @MainActor in self?.handleCharacterStick(x: x, y: y) }
        }
        gamepad.leftThumbstickButton?.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in self?.setL3Pressed(pressed) }
        }
        gamepad.rightThumbstickButton?.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in self?.setR3Pressed(pressed) }
        }
        // ARMSX2 owns the aggregate profile callback. OrbitKeys uses the
        // per-element handlers above so both input systems can coexist while the
        // keyboard's modal capture prevents the library from receiving commands.
        controller.physicalInputProfile.buttons[
            GCInputLeftThumbstickButton
        ]?.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in self?.setL3Pressed(pressed) }
        }
        controller.physicalInputProfile.buttons[
            GCInputRightThumbstickButton
        ]?.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in self?.setR3Pressed(pressed) }
        }

        bind(gamepad.buttonX, to: .square)
        bind(gamepad.buttonA, to: .cross)
        bind(gamepad.buttonB, to: .circle)
        bind(gamepad.buttonY, to: .triangle)
        gamepad.buttonMenu.pressedChangedHandler = { [weak self] _, _, pressed in
            guard pressed else { return }
            Task { @MainActor in self?.commitAndClose() }
        }
        gamepad.buttonOptions?.pressedChangedHandler = { [weak self] _, _, pressed in
            guard pressed else { return }
            Task { @MainActor in self?.toggleKeyboardStyle() }
        }

        gamepad.leftShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in self?.setBackspaceHeld(pressed) }
        }
        gamepad.rightShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in self?.setSpaceHeld(pressed) }
        }

        gamepad.leftTrigger.valueChangedHandler = { [weak self] _, value, _ in
            Task { @MainActor in self?.setL2Held(value > 0.45) }
        }
        gamepad.rightTrigger.valueChangedHandler = { [weak self] _, value, _ in
            Task { @MainActor in self?.setR2Held(value > 0.45) }
        }

        gamepad.dpad.left.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in
                guard let self else { return }
                if self.handleKeyboardSettingsDpad(.left, isHeld: pressed) { return }
                if self.usesFullQwerty {
                    self.setFullQwertyMoveHeld(.left, isHeld: pressed)
                } else {
                    self.setCursorHeld(-1, isHeld: pressed)
                }
            }
        }
        gamepad.dpad.right.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in
                guard let self else { return }
                if self.handleKeyboardSettingsDpad(.right, isHeld: pressed) { return }
                if self.usesFullQwerty {
                    self.setFullQwertyMoveHeld(.right, isHeld: pressed)
                } else {
                    self.setCursorHeld(1, isHeld: pressed)
                }
            }
        }
        gamepad.dpad.up.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in
                guard let self else { return }
                if self.handleKeyboardSettingsDpad(.up, isHeld: pressed) { return }
                if self.usesFullQwerty {
                    if self.isKeyboardVisible {
                        self.setFullQwertyMoveHeld(.up, isHeld: pressed)
                    } else if pressed {
                        self.openKeyboard()
                    }
                    return
                }
                guard pressed else { return }
                self.isKeyboardVisible ? self.commitAndClose() : self.openKeyboard()
            }
        }
        gamepad.dpad.down.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in
                guard let self else { return }
                if self.handleKeyboardSettingsDpad(.down, isHeld: pressed) { return }
                if self.usesFullQwerty {
                    self.setFullQwertyMoveHeld(.down, isHeld: pressed)
                    return
                }
                guard pressed else { return }
                self.commitAndClose()
            }
        }
    }

    private func handleKeyboardSettingsDpad(
        _ direction: OrbitKeysFullQwertyDirection,
        isHeld: Bool
    ) -> Bool {
        guard isKeyboardSettingsVisible else { return false }
        setKeyboardSettingsDpadHeld(direction, isHeld: isHeld)
        return true
    }

    private func setKeyboardSettingsDpadHeld(
        _ direction: OrbitKeysFullQwertyDirection,
        isHeld: Bool
    ) {
        guard isHeld else {
            if keyboardSettingsHeldDirection == direction {
                stopKeyboardSettingsDpadRepeat()
            }
            return
        }
        guard keyboardSettingsHeldDirection != direction else { return }

        stopKeyboardSettingsDpadRepeat()
        keyboardSettingsHeldDirection = direction
        applyKeyboardSettingsDpad(direction)

        guard shouldRepeatKeyboardSettingsDpad(direction) else { return }
        let repeatDelay = keyboardSettingsRepeatDelay(for: direction)
        keyboardSettingsDpadRepeatTask = makeRepeatTask(
            initialDelay: 260,
            repeatDelay: repeatDelay
        ) { model in
            guard model.keyboardSettingsHeldDirection == direction else { return false }
            model.applyKeyboardSettingsDpad(direction)
            return true
        }
    }

    private func applyKeyboardSettingsDpad(_ direction: OrbitKeysFullQwertyDirection) {
        switch direction {
        case .up:
            moveKeyboardSettingsSelection(by: -1)
        case .down:
            moveKeyboardSettingsSelection(by: 1)
        case .left:
            adjustKeyboardSettingsSelection(by: -1)
        case .right:
            adjustKeyboardSettingsSelection(by: 1)
        }
    }

    private func shouldRepeatKeyboardSettingsDpad(_ direction: OrbitKeysFullQwertyDirection) -> Bool {
        switch direction {
        case .up, .down:
            return true
        case .left, .right:
            return selectedKeyboardSettingsOption == .transparency
        }
    }

    private func keyboardSettingsRepeatDelay(for direction: OrbitKeysFullQwertyDirection) -> Int {
        switch direction {
        case .up, .down:
            return 76
        case .left, .right:
            return 34
        }
    }

    func stopKeyboardSettingsDpadRepeat() {
        keyboardSettingsDpadRepeatTask?.cancel()
        keyboardSettingsDpadRepeatTask = nil
        keyboardSettingsHeldDirection = nil
    }

    private func bind(_ input: GCControllerButtonInput, to face: OrbitKeysFaceButton) {
        input.pressedChangedHandler = { [weak self] _, _, pressed in
            Task { @MainActor in
                self?.setFaceHeld(face, isHeld: pressed)
            }
        }
    }

    private func disconnect(_ controller: GCController) {
        guard controller === activeController else { return }
        activeController = nil
        setBackspaceHeld(false)
        setSpaceHeld(false)
        stopFaceRepeat()
        stopCursorRepeat()
        stopFullQwertyRepeats()
        stopKeyboardSettingsDpadRepeat()
        resetTransientModeHolds()
        resetPublishedStickVector()
        resetPublishedCharacterStickVector()
        rightStickFace = nil
        isRightStickLatched = false
        isL3Pressed = false
        isR3Pressed = false

        if let nextController = GCController.controllers().first(where: { $0 !== controller }) {
            connect(nextController)
        }
    }

    private func handleStick(x: Float, y: Float) {
        guard isKeyboardVisible else { return }
        guard !isKeyboardSettingsVisible else {
            resetPublishedStickVector()
            stopFullQwertyMoveRepeat()
            handleKeyboardSettingsStick(x: x, y: y)
            return
        }
        if usesFullQwerty {
            resetPublishedStickVector()
            handleFullQwertyStick(x: x, y: y)
            return
        }
        publishStickVector(x: x, y: y)
        select(OrbitKeysGridPosition.from(stickX: x, y: y))
    }

    private func publishStickVector(x: Float, y: Float) {
        let resolvedX: Float = abs(x) < 0.05 ? 0 : x
        let resolvedY: Float = abs(y) < 0.05 ? 0 : y
        guard abs(resolvedX - stickX) >= 0.06
            || abs(resolvedY - stickY) >= 0.06
        else {
            return
        }
        stickX = resolvedX
        stickY = resolvedY
    }

    func resetPublishedStickVector() {
        guard stickX != 0 || stickY != 0 else { return }
        stickX = 0
        stickY = 0
    }

    private func handleKeyboardSettingsStick(x: Float, y: Float) {
        let deadZone: Float = 0.46
        guard max(abs(x), abs(y)) >= deadZone else {
            stopKeyboardSettingsDpadRepeat()
            return
        }

        let direction: OrbitKeysFullQwertyDirection =
            abs(x) > abs(y)
            ? (x < 0 ? .left : .right)
            : (y > 0 ? .up : .down)
        setKeyboardSettingsDpadHeld(direction, isHeld: true)
    }

    private func handleCharacterStick(x: Float, y: Float) {
        if isKeyboardSettingsVisible {
            resetPublishedCharacterStickVector()
            rightStickFace = nil
            isRightStickLatched = false
            return
        }

        if usesFullQwerty {
            resetPublishedCharacterStickVector()
            rightStickFace = nil
            isRightStickLatched = false
            return
        }

        publishCharacterStickVector(x: x, y: y)

        if max(abs(x), abs(y)) < 0.32 {
            rightStickFace = nil
            isRightStickLatched = false
            return
        }

        guard isKeyboardVisible,
            let face = OrbitKeysFaceButton.from(characterStickX: x, y: y)
        else {
            return
        }

        rightStickFace = face
        guard !isRightStickLatched else { return }
        isRightStickLatched = true
        activate(face)
    }

    private func publishCharacterStickVector(x: Float, y: Float) {
        let resolvedX: Float = abs(x) < 0.05 ? 0 : x
        let resolvedY: Float = abs(y) < 0.05 ? 0 : y
        guard abs(resolvedX - rightStickX) >= 0.06
            || abs(resolvedY - rightStickY) >= 0.06
        else {
            return
        }
        rightStickX = resolvedX
        rightStickY = resolvedY
    }

    func resetPublishedCharacterStickVector() {
        guard rightStickX != 0 || rightStickY != 0 else { return }
        rightStickX = 0
        rightStickY = 0
    }

    private func setFaceHeld(_ face: OrbitKeysFaceButton, isHeld: Bool) {
        if isKeyboardSettingsVisible {
            handleKeyboardSettingsFace(face, isHeld: isHeld)
            return
        }

        if usesFullQwerty, isKeyboardVisible {
            setFullQwertyFaceHeld(face, isHeld: isHeld)
            return
        }

        guard isHeld else {
            if repeatingFace == face {
                stopFaceRepeat()
            }
            if pressedFace == face {
                pressedFaceReleaseTask?.cancel()
                pressedFaceReleaseTask = nil
                pressedFace = nil
            }
            return
        }

        guard isKeyboardVisible else {
            activate(face)
            return
        }
        guard repeatingFace != face else { return }

        stopFaceRepeat()
        repeatingFace = face
        activate(face)
        faceRepeatTask = makeRepeatTask(initialDelay: 420, repeatDelay: 82) { model in
            guard model.repeatingFace == face else { return false }
            model.activate(face)
            return true
        }
    }

    private func handleKeyboardSettingsFace(_ face: OrbitKeysFaceButton, isHeld: Bool) {
        guard isHeld else {
            if pressedFace == face {
                pressedFace = nil
            }
            return
        }

        pressedFace = face
        switch face {
        case .cross:
            activateKeyboardSettingsSelection()
        case .circle, .triangle:
            closeKeyboardSettings()
        case .square:
            clearText()
        }

        schedulePressedFaceRelease(face, after: 110)
    }

    func stopFaceRepeat() {
        faceRepeatTask?.cancel()
        faceRepeatTask = nil
        repeatingFace = nil
        pressedFaceReleaseTask?.cancel()
        pressedFaceReleaseTask = nil
        pressedFace = nil
    }

    private func setCursorHeld(_ offset: Int, isHeld: Bool) {
        guard isHeld else {
            if cursorRepeatOffset == offset {
                stopCursorRepeat()
            }
            return
        }
        guard isKeyboardVisible, cursorRepeatOffset != offset else {
            return
        }

        stopCursorRepeat()
        cursorRepeatOffset = offset
        moveCursor(by: offset)
        cursorRepeatTask = makeRepeatTask(initialDelay: 320, repeatDelay: 55) { model in
            guard model.cursorRepeatOffset == offset else { return false }
            model.moveCursor(by: offset)
            return true
        }
    }

    func stopCursorRepeat() {
        cursorRepeatTask?.cancel()
        cursorRepeatTask = nil
        cursorRepeatOffset = nil
    }

    // Converts analog stick movement into repeatable row-key navigation for the full keyboard.
    private func handleFullQwertyStick(x: Float, y: Float) {
        let deadZone: Float = 0.42
        guard max(abs(x), abs(y)) >= deadZone else {
            stopFullQwertyMoveRepeat()
            return
        }

        let direction: OrbitKeysFullQwertyDirection =
            abs(x) > abs(y)
            ? (x < 0 ? .left : .right)
            : (y > 0 ? .up : .down)
        setFullQwertyMoveHeld(direction, isHeld: true)
    }

    // Starts or stops fast repeated movement across the QWERTY rows.
    private func setFullQwertyMoveHeld(
        _ direction: OrbitKeysFullQwertyDirection,
        isHeld: Bool
    ) {
        guard isHeld else {
            if fullQwertyHeldDirection == direction {
                stopFullQwertyMoveRepeat()
            }
            return
        }
        guard isKeyboardVisible, fullQwertyHeldDirection != direction else {
            return
        }

        stopFullQwertyMoveRepeat()
        fullQwertyHeldDirection = direction
        moveFullQwertySelection(direction, permitsToolbarEntry: true)
        let initialDelay = fullQwertyMoveDelay(for: direction)
        let repeatDelay = fullQwertyRepeatDelay(for: direction)
        fullQwertyMoveRepeatTask = makeRepeatTask(
            initialDelay: initialDelay,
            repeatDelay: repeatDelay
        ) { model in
            guard model.fullQwertyHeldDirection == direction else { return false }
            model.moveFullQwertySelection(direction, permitsToolbarEntry: false)
            return true
        }
    }

    private func fullQwertyMoveDelay(for direction: OrbitKeysFullQwertyDirection) -> Int {
        switch direction {
        case .up, .down:
            return 440
        case .left, .right:
            return 220
        }
    }

    private func fullQwertyRepeatDelay(for direction: OrbitKeysFullQwertyDirection) -> Int {
        switch direction {
        case .up, .down:
            return 124
        case .left, .right:
            return 62
        }
    }

    private func moveFullQwertySelection(
        _ direction: OrbitKeysFullQwertyDirection,
        permitsToolbarEntry: Bool
    ) {
        if let toolbarSelection = fullQwertyToolbarSelection {
            let nextToolbarSelection: OrbitKeysFullQwertyToolbarControl?
            switch direction {
            case .left:
                nextToolbarSelection = OrbitKeysFullQwertyToolbarControl(
                    rawValue: max(toolbarSelection.rawValue - 1, 0)
                )
            case .right:
                nextToolbarSelection = OrbitKeysFullQwertyToolbarControl(
                    rawValue: min(
                        toolbarSelection.rawValue + 1,
                        OrbitKeysFullQwertyToolbarControl.allCases.count - 1
                    )
                )
            case .up:
                nextToolbarSelection = toolbarSelection
            case .down:
                nextToolbarSelection = nil
            }

            if direction == .down {
                // The selected key remains unchanged while Send/Settings/Close owns
                // focus, so leaving the toolbar can restore that exact key.
                selectFullQwertyKey(fullQwertySelection)
            } else if let nextToolbarSelection,
                                nextToolbarSelection != toolbarSelection {
                fullQwertyToolbarSelection = nextToolbarSelection
                selectionSequence += 1
                haptics.play(.selection)
                MenuAudioPackManager.shared.playEvent(.navigation)
            }
            return
        }

        if direction == .up,
              fullQwertySelection.row == 0,
              permitsToolbarEntry {
            fullQwertyToolbarSelection = .send
            selectionSequence += 1
            haptics.play(.selection)
            MenuAudioPackManager.shared.playEvent(.navigation)
            return
        }

        let nextPosition = OrbitKeysFullQwertyLayout.movedPosition(
            from: fullQwertySelection,
            direction: direction,
            mode: mode,
            loopsHorizontally: loopsFullQwertyRows
        )
        selectFullQwertyKey(nextPosition)
    }

    private func stopFullQwertyMoveRepeat() {
        fullQwertyMoveRepeatTask?.cancel()
        fullQwertyMoveRepeatTask = nil
        fullQwertyHeldDirection = nil
    }

    // Maps the PlayStation face buttons to common full-keyboard actions.
    private func setFullQwertyFaceHeld(_ face: OrbitKeysFaceButton, isHeld: Bool) {
        guard isHeld else {
            if face == .cross {
                stopFullQwertyKeyRepeat()
            }
            if face == .square {
                setBackspaceHeld(false)
            }
            if pressedFace == face {
                pressedFaceReleaseTask?.cancel()
                pressedFaceReleaseTask = nil
                pressedFace = nil
            }
            return
        }

        pressedFace = face
        switch face {
        case .cross:
            guard !isFullQwertySelectHeld else { return }
            isFullQwertySelectHeld = true
            activateFullQwertySelection()
            startFullQwertyKeyRepeatIfNeeded()
        case .square:
            setBackspaceHeld(true)
        case .circle:
            insertSpace()
        case .triangle:
            insertSpace()
            fullQwertyFeedbackSequence += 1
        }
    }

    private func startFullQwertyKeyRepeatIfNeeded() {
        guard fullQwertyToolbarSelection == nil,
            let key = OrbitKeysFullQwertyLayout.key(at: fullQwertySelection, mode: mode),
            key.repeatsWhileHeld,
            fullQwertyKeyRepeatTask == nil
        else {
            return
        }

        fullQwertyKeyRepeatTask = makeRepeatTask(
            initialDelay: 420,
            repeatDelay: 82
        ) { model in
            guard model.isFullQwertySelectHeld,
                let key = OrbitKeysFullQwertyLayout.key(at: model.fullQwertySelection, mode: model.mode),
                key.repeatsWhileHeld
            else {
                return false
            }

            model.performFullQwertyKey(key)
            return true
        }
    }

    private func stopFullQwertyKeyRepeat() {
        fullQwertyKeyRepeatTask?.cancel()
        fullQwertyKeyRepeatTask = nil
        isFullQwertySelectHeld = false
    }

    private func stopFullQwertyTouchBackspaceRepeat() {
        fullQwertyTouchBackspaceRepeatTask?.cancel()
        fullQwertyTouchBackspaceRepeatTask = nil
    }

    func stopFullQwertyRepeats() {
        stopFullQwertyMoveRepeat()
        stopFullQwertyKeyRepeat()
        stopFullQwertyTouchBackspaceRepeat()
    }

    private func setL3Pressed(_ isPressed: Bool) {
        guard isL3Pressed != isPressed else { return }
        isL3Pressed = isPressed
        if isPressed {
            if usesFullQwerty, isKeyboardVisible {
                setFullQwertyEnabled(false)
            } else {
                toggleKeyboardArrangement()
            }
        }
    }

    private func setR3Pressed(_ isPressed: Bool) {
        guard isR3Pressed != isPressed else { return }
        isR3Pressed = isPressed
        if isPressed {
            toggleFaceIcons()
        }
    }

    private func setL2Held(_ isHeld: Bool) {
        guard isL2Held != isHeld else { return }
        isL2Held = isHeld
        modeDidChange()
        MenuAudioPackManager.shared.playEvent(.toggle(isOn: isHeld))
    }

    private func setR2Held(_ isHeld: Bool) {
        guard isR2Held != isHeld else { return }
        isR2Held = isHeld
        modeDidChange()
        MenuAudioPackManager.shared.playEvent(.toggle(isOn: isHeld))
    }
}
