// VirtualPadTouchActionController.swift — virtual pad touch action controller
// SPDX-License-Identifier: GPL-3.0+

import QuartzCore

@MainActor
private final class VirtualPadActionPressCoordinator {
    static let shared = VirtualPadActionPressCoordinator()

    private var pressedSources: [VirtualPadActionButton: Set<String>] = [:]
    private var pulseTasks: [String: Task<Void, Never>] = [:]
    private var pulseButtons: [String: VirtualPadActionButton] = [:]

    func set(_ button: VirtualPadActionButton, source: String, pressed: Bool) {
        var sources = pressedSources[button, default: []]
        let wasPressed = !sources.isEmpty
        if pressed {
            sources.insert(source)
        } else {
            sources.remove(source)
        }

        if sources.isEmpty {
            pressedSources.removeValue(forKey: button)
        } else {
            pressedSources[button] = sources
        }

        let isPressed = !sources.isEmpty
        if wasPressed != isPressed {
            EmulatorBridge.shared.setPadButton(button.padButton, pressed: isPressed)
        }
    }

    func pulse(
        _ button: VirtualPadActionButton,
        source: String,
        duration: TimeInterval = 0.075
    ) {
        pulseTasks[source]?.cancel()
        if let previous = pulseButtons[source], previous != button {
            set(previous, source: source, pressed: false)
        }

        pulseButtons[source] = button
        set(button, source: source, pressed: true)
        pulseTasks[source] = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(max(duration, 0)))
            guard !Task.isCancelled, let self else { return }
            self.set(button, source: source, pressed: false)
            self.pulseTasks.removeValue(forKey: source)
            self.pulseButtons.removeValue(forKey: source)
        }
    }
}

@MainActor
func pulseVirtualPadActionButton(
    _ button: VirtualPadActionButton,
    source: String,
    duration: TimeInterval = 0.075
) {
    VirtualPadActionPressCoordinator.shared.pulse(
        button,
        source: source,
        duration: duration
    )
}

@MainActor
@Observable
final class DynamicCrosshairRuntimeState {
    private(set) var isAiming = false
    private(set) var isSwipeCrosshairVisible = false
    private(set) var isCameraMoving = false
    private(set) var isCameraSettling = false
    private(set) var isShooting = false
    private(set) var isRapidFiring = false
    private(set) var cameraMotion = DynamicThumbstickVector.zero
    private(set) var cameraAcceleration = DynamicThumbstickVector.zero
    private(set) var movementStartedAt = 0.0
    private(set) var movementEndedAt = 0.0
    private(set) var shotStartedAt = 0.0
    private(set) var shotTiltRadians: CGFloat = 0
    private(set) var rapidFireStartedAt = 0.0

    @ObservationIgnored private var sourceMotion: [DynamicCrosshairMotionSource: DynamicThumbstickVector] = [:]
    @ObservationIgnored private var settlingTask: Task<Void, Never>?
    @ObservationIgnored private var shotTask: Task<Void, Never>?
    @ObservationIgnored private var swipeHideTask: Task<Void, Never>?

    func setAiming(_ aiming: Bool) {
        isAiming = aiming
        if !aiming && !isSwipeCrosshairVisible {
            clearTransientActivity()
        }
    }

    func updateCameraMotion(_ motion: DynamicThumbstickVector, source: DynamicCrosshairMotionSource) {
        guard isAiming || isSwipeCrosshairVisible else { return }

        let limitedMotion = motion.limited(to: 1.35)
        if limitedMotion.magnitude > 0.003 {
            sourceMotion[source] = limitedMotion
        } else {
            sourceMotion.removeValue(forKey: source)
        }

        let combined = sourceMotion.values.reduce(DynamicThumbstickVector.zero) { partial, next in
            DynamicThumbstickVector(x: partial.x + next.x, y: partial.y + next.y)
        }
        let target = combined.limited(to: 1.35)
        let now = Date.timeIntervalSinceReferenceDate

        guard target.magnitude > 0.003 else {
            beginCameraSettle(at: now)
            return
        }

        if !isCameraMoving {
            movementStartedAt = now
        }
        if isCameraSettling {
            settlingTask?.cancel()
            settlingTask = nil
        }

        let previous = cameraMotion
        let response: CGFloat = isCameraMoving ? 0.52 : 0.72
        let filtered = DynamicThumbstickVector(
            x: previous.x + (target.x - previous.x) * response,
            y: previous.y + (target.y - previous.y) * response
        )
        cameraAcceleration = DynamicThumbstickVector(
            x: (filtered.x - previous.x) * 2.2,
            y: (filtered.y - previous.y) * 2.2
        ).limited(to: 1)
        cameraMotion = filtered
        isCameraMoving = true
        isCameraSettling = false
    }

    func triggerShot() {
        guard isAiming else { return }
        shotStartedAt = Date.timeIntervalSinceReferenceDate
        shotTiltRadians = .random(in: -0.07...0.07)
        isShooting = true
        shotTask?.cancel()
        shotTask = nil
        guard !isRapidFiring else { return }
        shotTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(160))
            guard !Task.isCancelled else { return }
            self?.isShooting = false
        }
    }

    func setRapidFiring(_ firing: Bool) {
        guard isAiming || !firing else { return }
        if firing && !isRapidFiring {
            rapidFireStartedAt = Date.timeIntervalSinceReferenceDate
        }
        isRapidFiring = firing
        if !firing {
            shotTask?.cancel()
            shotTask = nil
            isShooting = false
        }
    }

    func reset() {
        swipeHideTask?.cancel()
        swipeHideTask = nil
        isAiming = false
        isSwipeCrosshairVisible = false
        clearTransientActivity()
    }

    func swipeInteractionBegan() {
        swipeHideTask?.cancel()
        swipeHideTask = nil
        isSwipeCrosshairVisible = true
    }

    func swipeInteractionEnded(hideDelay: TimeInterval) {
        swipeHideTask?.cancel()
        let delay = max(hideDelay, 0)
        swipeHideTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self else { return }
            self.isSwipeCrosshairVisible = false
            self.swipeHideTask = nil
            if !self.isAiming {
                self.clearTransientActivity()
            }
        }
    }

    private func clearTransientActivity() {
        settlingTask?.cancel()
        shotTask?.cancel()
        settlingTask = nil
        shotTask = nil
        sourceMotion.removeAll(keepingCapacity: true)
        isCameraMoving = false
        isCameraSettling = false
        isShooting = false
        isRapidFiring = false
        cameraMotion = .zero
        cameraAcceleration = .zero
        shotTiltRadians = 0
    }

    private func beginCameraSettle(at now: TimeInterval) {
        guard isCameraMoving else { return }
        isCameraMoving = false
        isCameraSettling = true
        movementEndedAt = now
        settlingTask?.cancel()
        settlingTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(260))
            guard !Task.isCancelled, let self else { return }
            self.isCameraSettling = false
            self.cameraMotion = .zero
            self.cameraAcceleration = .zero
            self.settlingTask = nil
        }
    }
}

@MainActor
final class VirtualPadTouchActionController {
    private let side: VirtualPadThumbstickSide
    let crosshairState = DynamicCrosshairRuntimeState()
    private var lastInteractionTapTime: CFTimeInterval?
    private var lastFireTapTime: CFTimeInterval?
    private var fireTapCount = 0
    private var touchActive = false
    private var aimEngaged = false
    private var enteredAimThisInteraction = false
    private var rapidFireEngaged = false
    private var automaticFireBlockedByHardcore = false
    private var aimReleaseTask: Task<Void, Never>?
    private var fireReleaseTask: Task<Void, Never>?
    private var fireLoopTask: Task<Void, Never>?
    private var tapFireTask: Task<Void, Never>?
    private var pendingSingleFireTask: Task<Void, Never>?
    private var activeAimButton: VirtualPadActionButton?
    private var activeTapFireButton: VirtualPadActionButton?
    private var activeHoldFireButton: VirtualPadActionButton?

    init(side: VirtualPadThumbstickSide) {
        self.side = side
    }

    var isAiming: Bool { aimEngaged }

    func updateCameraMotion(_ motion: DynamicThumbstickVector, source: DynamicCrosshairMotionSource) {
        crosshairState.updateCameraMotion(motion, source: source)
    }

    func interactionBegan() {
        let settings = DynamicThumbstickSettings.shared
        let now = CACurrentMediaTime()
        touchActive = true
        enteredAimThisInteraction = false
        aimReleaseTask?.cancel()
        fireReleaseTask?.cancel()

        let isDoubleTap = settings.doubleTapToHoldAim &&
            !aimEngaged &&
            lastInteractionTapTime.map { now - $0 <= settings.doubleTapWindow } == true
        if !aimEngaged && (settings.holdAimWhileSwipe || isDoubleTap) {
            aimEngaged = true
            enteredAimThisInteraction = true
            cancelPendingSingleFire()
            resetFireTapSequence()
            setAimPressed(true)
        }

        let automaticActionsAllowed = aimEngaged || settings.actionsOnNonAimMode
        if automaticFireIsBlocked || !automaticActionsAllowed {
            resetFireTapSequence()
            if automaticFireIsBlocked {
                stopRapidFire()
            }
        } else {
            let insideRapidWindow = lastFireTapTime.map { now - $0 <= settings.rapidTapWindow } == true
            if !insideRapidWindow { fireTapCount = 0 }
            if !enteredAimThisInteraction &&
                settings.rapidTapFireEnabled &&
                insideRapidWindow &&
                fireTapCount >= max(settings.rapidTapActivationCount - 1, 1) {
                startRapidFire()
            }
        }
    }

    func interactionActivity() {
        guard touchActive else { return }
        if DynamicThumbstickSettings.shared.extendFireWhileDragging && rapidFireEngaged {
            fireReleaseTask?.cancel()
        }
    }

    func interactionTapped() {
        let settings = DynamicThumbstickSettings.shared
        let now = CACurrentMediaTime()
        lastInteractionTapTime = now

        // The touch that first engages aim is a mode-changing gesture, not a
        // shot. It also must not seed the rapid-fire recognizer.
        if enteredAimThisInteraction {
            enteredAimThisInteraction = false
            resetFireTapSequence()
            return
        }

        if (aimEngaged || settings.actionsOnNonAimMode) && !automaticFireIsBlocked {
            if let lastFireTapTime, now - lastFireTapTime <= settings.rapidTapWindow {
                fireTapCount += 1
            } else {
                fireTapCount = 1
            }
            self.lastFireTapTime = now
        } else {
            resetFireTapSequence()
        }

        if rapidFireEngaged {
            return
        }
        guard settings.tapToFire,
              aimEngaged || settings.singleTapActionAllowedInNonAimMode else {
            return
        }
        if settings.doubleTapToHoldAim && !aimEngaged {
            scheduleSingleFire(after: settings.doubleTapWindow)
        } else {
            pulseFire()
        }
    }

    func interactionEnded() {
        touchActive = false
        scheduleAimRelease()
        guard rapidFireEngaged else { return }
        if DynamicThumbstickSettings.shared.releaseFireWhenTouchEnds {
            stopRapidFire()
        } else {
            scheduleFireRelease()
        }
    }

    func reset() {
        aimReleaseTask?.cancel()
        fireReleaseTask?.cancel()
        fireLoopTask?.cancel()
        tapFireTask?.cancel()
        pendingSingleFireTask?.cancel()
        aimReleaseTask = nil
        fireReleaseTask = nil
        fireLoopTask = nil
        tapFireTask = nil
        pendingSingleFireTask = nil
        touchActive = false
        aimEngaged = false
        enteredAimThisInteraction = false
        rapidFireEngaged = false
        lastInteractionTapTime = nil
        resetFireTapSequence()
        setAimPressed(false)
        setTapFirePressed(false)
        setHoldFirePressed(false)
        crosshairState.reset()
    }

    private func scheduleAimRelease() {
        guard aimEngaged else { return }
        aimReleaseTask?.cancel()
        let delay = max(DynamicThumbstickSettings.shared.aimReleaseDelay, 0)
        aimReleaseTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self, !self.touchActive else { return }
            self.aimEngaged = false
            self.setAimPressed(false)
        }
    }

    private func scheduleFireRelease() {
        fireReleaseTask?.cancel()
        let delay = max(DynamicThumbstickSettings.shared.fireReleaseDelay, 0)
        fireReleaseTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self, !self.touchActive else { return }
            self.stopRapidFire()
        }
    }

    private func pulseFire() {
        tapFireTask?.cancel()
        setTapFirePressed(false)
        setTapFirePressed(true)
        crosshairState.triggerShot()
        GameEventHaptics.shared.dynamicActionShot()
        tapFireTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(75))
            guard !Task.isCancelled else { return }
            self?.setTapFirePressed(false)
        }
    }

    private func scheduleSingleFire(after delay: TimeInterval) {
        pendingSingleFireTask?.cancel()
        pendingSingleFireTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(max(delay, 0)))
            guard !Task.isCancelled, let self else { return }
            self.pendingSingleFireTask = nil
            self.pulseFire()
        }
    }

    private func cancelPendingSingleFire() {
        pendingSingleFireTask?.cancel()
        pendingSingleFireTask = nil
        tapFireTask?.cancel()
        tapFireTask = nil
        setTapFirePressed(false)
    }

    private func resetFireTapSequence() {
        lastFireTapTime = nil
        fireTapCount = 0
    }

    private func startRapidFire() {
        let settings = DynamicThumbstickSettings.shared
        guard !rapidFireEngaged,
              !automaticFireIsBlocked,
              aimEngaged || settings.actionsOnNonAimMode else {
            return
        }
        cancelPendingSingleFire()
        rapidFireEngaged = true
        crosshairState.setRapidFiring(true)
        GameEventHaptics.shared.dynamicActionRapidFire()
        fireLoopTask?.cancel()
        fireLoopTask = Task { @MainActor [weak self] in
            while !Task.isCancelled, let self, self.rapidFireEngaged {
                guard !self.automaticFireIsBlocked else {
                    self.rapidFireEngaged = false
                    self.crosshairState.setRapidFiring(false)
                    self.resetFireTapSequence()
                    break
                }
                let interval = max(DynamicThumbstickSettings.shared.automaticFireInterval, 0.06)
                self.setHoldFirePressed(true)
                self.crosshairState.triggerShot()
                try? await Task.sleep(for: .seconds(min(interval * 0.45, 0.05)))
                if Task.isCancelled { break }
                self.setHoldFirePressed(false)
                try? await Task.sleep(for: .seconds(max(interval * 0.55, 0.02)))
            }
            self?.setHoldFirePressed(false)
        }
    }

    private func stopRapidFire() {
        rapidFireEngaged = false
        crosshairState.setRapidFiring(false)
        fireLoopTask?.cancel()
        fireLoopTask = nil
        setHoldFirePressed(false)
    }

    private func setAimPressed(_ pressed: Bool) {
        crosshairState.setAiming(pressed)
        Self.setActionButton(
            pressed: pressed,
            activeButton: &activeAimButton,
            selectedButton: DynamicThumbstickSettings.shared.aimButton(for: side),
            source: "\(sourcePrefix).aim"
        )
        if pressed {
            GameEventHaptics.shared.dynamicActionAim()
        }
    }

    private func setTapFirePressed(_ pressed: Bool) {
        Self.setActionButton(
            pressed: pressed,
            activeButton: &activeTapFireButton,
            selectedButton: DynamicThumbstickSettings.shared.fireButton(for: side),
            source: "\(sourcePrefix).tapFire"
        )
    }

    private func setHoldFirePressed(_ pressed: Bool) {
        if pressed && automaticFireIsBlocked {
            Self.setActionButton(
                pressed: false,
                activeButton: &activeHoldFireButton,
                selectedButton: DynamicThumbstickSettings.shared.holdFireButton(for: side),
                source: "\(sourcePrefix).holdFire"
            )
            return
        }
        Self.setActionButton(
            pressed: pressed,
            activeButton: &activeHoldFireButton,
            selectedButton: DynamicThumbstickSettings.shared.holdFireButton(for: side),
            source: "\(sourcePrefix).holdFire"
        )
    }

    func updateHardcoreAutomaticFireRestriction(_ blocked: Bool) {
        automaticFireBlockedByHardcore = blocked
        guard blocked else { return }
        resetFireTapSequence()
        stopRapidFire()
    }

    private var automaticFireIsBlocked: Bool {
        automaticFireBlockedByHardcore || ARMSX2Bridge.isRetroAchievementsHardcoreActive()
    }

    private var sourcePrefix: String {
        side == .left ? "leftThumbstick" : "rightThumbstick"
    }

    private static func setActionButton(
        pressed: Bool,
        activeButton: inout VirtualPadActionButton?,
        selectedButton: VirtualPadActionButton,
        source: String
    ) {
        if pressed {
            if let previous = activeButton, previous != selectedButton {
                VirtualPadActionPressCoordinator.shared.set(previous, source: source, pressed: false)
            }
            activeButton = selectedButton
            VirtualPadActionPressCoordinator.shared.set(selectedButton, source: source, pressed: true)
        } else if let button = activeButton {
            VirtualPadActionPressCoordinator.shared.set(button, source: source, pressed: false)
            activeButton = nil
        }
    }
}

/// Shared ownership boundary for camera-touch actions and their crosshair
/// runtime. GameScreenView keeps this session so the crosshair can render in
/// the Metal viewport while VirtualControllerView continues driving input.
@MainActor
final class VirtualPadTouchActionSession {
    let left = VirtualPadTouchActionController(side: .left)
    let right = VirtualPadTouchActionController(side: .right)

    init() {
        refreshHardcoreAutomaticFireRestriction()
    }

    func refreshHardcoreAutomaticFireRestriction() {
        let blocked = DynamicThumbstickSettings.automaticFireBlockedByHardcore()
        left.updateHardcoreAutomaticFireRestriction(blocked)
        right.updateHardcoreAutomaticFireRestriction(blocked)
    }

    func reset() {
        left.reset()
        right.reset()
    }
}
