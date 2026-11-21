// MenuControllerInputRouter+Lifecycle.swift — start, stop, capture and input mode switching
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import GameController
import UIKit

extension MenuControllerInputRouter {
    func start() {
        ControllerEventDeliveryCoordinator.shared.install(router: self)
        guard connectObserver == nil else {
            refreshConnectedControllers()
            return
        }

        let center = NotificationCenter.default
        connectObserver = center.addObserver(
            forName: .GCControllerDidConnect,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshConnectedControllers()
            }
        }
        disconnectObserver = center.addObserver(
            forName: .GCControllerDidDisconnect,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshConnectedControllers()
            }
        }
        refreshConnectedControllers()
    }

    func stop() {
        let center = NotificationCenter.default
        if let connectObserver {
            center.removeObserver(connectObserver)
        }
        if let disconnectObserver {
            center.removeObserver(disconnectObserver)
        }
        connectObserver = nil
        disconnectObserver = nil
        setMenuActive(false)
        manuallyCapturedNavigation = false
        manualNavigationCaptures.removeAll(keepingCapacity: false)
        navigationSessions.removeAll(keepingCapacity: false)
        navigationSessionEntryTask?.cancel()
        navigationSessionEntryTask = nil
        pendingNavigationSessionEntryRequest = nil
        launchInputInterceptor = nil
        cancelEmulationShortcutInput()
        cancelThemePresetRepeats()
        pendingNavigationSessionCommands.removeAll(keepingCapacity: false)
        updateNavigationCaptureState()
        registeredExtendedProfiles.removeAll(keepingCapacity: false)
        registeredMicroProfiles.removeAll(keepingCapacity: false)
        hasConnectedController = false
        pendingNavigationModeSwitchRequest = nil
        isControllerNavigationEnabled = false
        activeNavigationInputMode = .touch
        updateRightStick(x: 0, y: 0)
    }

    func setMenuActive(_ active: Bool) {
        MenuAudioPackManager.shared.setInterfaceActive(active)
        ControllerEventDeliveryCoordinator.shared.setMenuActive(active)
        guard active != isMenuActive else { return }
        isMenuActive = active
        cancelRepeat()
        cancelThemePresetRepeats()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        lastPublishedDirectionalEdge = nil
        pressedButtons.removeAll(keepingCapacity: true)
        pressedFaceButton = nil
        cancelEmulationShortcutInput()
        if active {
            refreshConnectedControllers()
        } else {
            latestThemePresetRequest = nil
            rightStickRedispatchTask?.cancel()
            rightStickRedispatchTask = nil
            manuallyCapturedNavigation = false
            manualNavigationCaptures.removeAll(keepingCapacity: true)
            navigationSessions.removeAll(keepingCapacity: true)
            navigationSessionEntryTask?.cancel()
            navigationSessionEntryTask = nil
            pendingNavigationSessionEntryRequest = nil
            pendingNavigationSessionCommands.removeAll(keepingCapacity: true)
            updateNavigationCaptureState()
            navigationZone = .library
            libraryEntrySuppressedEventSequence = nil
            updateRightStick(x: 0, y: 0)
            feedbackPlayer.releaseResources()
        }
    }

    /// UIKit can temporarily replace GameController value handlers while
    /// Control Center owns the foreground. `setMenuActive(true)` intentionally
    /// does nothing when the Quick Menu is already open, so foreground return
    /// needs a separate rebind path that preserves the active navigation
    /// session while discarding stale held-button state.
    func applicationDidBecomeActive() {
        guard isMenuActive else { return }
        cancelRepeat()
        cancelThemePresetRepeats()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        lastPublishedDirectionalEdge = nil
        pressedButtons.removeAll(keepingCapacity: true)
        pressedFaceButton = nil
        updateRightStick(x: 0, y: 0)
        refreshConnectedControllers()
        updateNavigationCaptureState()
        schedulePendingNavigationSessionEntryIfReady()
    }

    func setNavigationCaptured(
        _ captured: Bool,
        owner: String,
        priority: Int
    ) {
        if captured, isMenuActive {
            manualNavigationCaptureSequence &+= 1
            manualNavigationCaptures[owner] = ManualNavigationCapture(
                priority: priority,
                registrationOrder: manualNavigationCaptureSequence
            )
            pendingNavigationSessionCommands.removeAll(keepingCapacity: true)
        } else {
            manualNavigationCaptures.removeValue(forKey: owner)
        }
        manuallyCapturedNavigation = !manualNavigationCaptures.isEmpty
        updateNavigationCaptureState()
        activeRightStickTargetID = nil
        dispatchRightStickVector()
        schedulePendingNavigationSessionEntryIfReady()
    }

    var frontmostManualNavigationCapture: (
        key: String,
        value: ManualNavigationCapture
    )? {
        manualNavigationCaptures.max { lhs, rhs in
            if lhs.value.priority != rhs.value.priority {
                return lhs.value.priority < rhs.value.priority
            }
            return lhs.value.registrationOrder < rhs.value.registrationOrder
        }
    }

    /// Requests an explicit ownership handoff when touch interrupts an active
    /// controller session. The first touch-down publishes the prompt before
    /// its eventual touch-up, so both navigation systems never silently own
    /// the same screen.
    func noteTouchInput() {
        guard isMenuActive else { return }
        guard pendingNavigationModeSwitchRequest == nil else { return }
        guard isControllerNavigationEnabled else { return }
        if isControllerNavigationEnabled, hasConnectedController,
           UIFrameRateSettings.shared.asksBeforeTouchNavigation {
            publishNavigationModeSwitchRequest(.touch)
            return
        }
        activateTouchNavigation()
    }

    func acceptNavigationModeSwitch(
        _ mode: MenuNavigationInputMode,
        doNotAskAgain: Bool
    ) {
        guard pendingNavigationModeSwitchRequest?.mode == mode else { return }
        pendingNavigationModeSwitchRequest = nil
        if doNotAskAgain {
            switch mode {
            case .touch:
                UIFrameRateSettings.shared.asksBeforeTouchNavigation = false
            case .controller:
                UIFrameRateSettings.shared.asksBeforeControllerNavigation = false
            }
        }
        switch mode {
        case .touch: activateTouchNavigation()
        case .controller: activateControllerNavigation()
        }
    }

    func cancelNavigationModeSwitch(_ mode: MenuNavigationInputMode) {
        guard pendingNavigationModeSwitchRequest?.mode == mode else { return }
        pendingNavigationModeSwitchRequest = nil
        switch mode {
        case .touch: activateControllerNavigation()
        case .controller: activateTouchNavigation()
        }
    }

    private func publishNavigationModeSwitchRequest(
        _ mode: MenuNavigationInputMode
    ) {
        sequence &+= 1
        pendingNavigationModeSwitchRequest = MenuNavigationModeSwitchRequest(
            sequence: sequence,
            mode: mode
        )
        if mode == .controller {
            // The confirmation itself must be controller navigable. Mount the
            // controller presentation only for the prompt; Cancel retires it
            // again immediately.
            isControllerNavigationEnabled = true
            cancelRepeat()
            cancelThemePresetRepeats()
            cancelDirectionSampling()
            heldDirections.removeAll(keepingCapacity: true)
            directionalHoldStartTimes.removeAll(keepingCapacity: true)
            rightStickVector = .zero
            rightStickHoldStartTime = nil
        }
    }

    func prepareControllerNavigationForInput() -> Bool {
        if isControllerNavigationEnabled { return true }
        guard pendingNavigationModeSwitchRequest == nil else { return false }
        if UIFrameRateSettings.shared.asksBeforeControllerNavigation {
            publishNavigationModeSwitchRequest(.controller)
            return false
        }
        activateControllerNavigation()
        return true
    }

    func activateTouchNavigation() {
        activeNavigationInputMode = .touch
        isControllerNavigationEnabled = false
        cancelRepeat()
        cancelThemePresetRepeats()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        lastPublishedDirectionalEdge = nil
        rightStickVector = .zero
        rightStickHoldStartTime = nil
        finishRightStickScrolling()
        feedbackPlayer.releaseControllerResources()
        requestFocusRelease()
        tabBarOrbFocusFrameInWindow = nil
        tabBarOrbFocusedIndex = nil
        updateNavigationCaptureState()
        dispatchRightStickVector()
        retireControllerNavigationResourcesAfterHandoff()
    }

    private func activateControllerNavigation() {
        activeNavigationInputMode = .controller
        isControllerNavigationEnabled = true
        updateNavigationCaptureState()
        feedbackPlayer.attach(to: GCController.controllers())
    }

    private func retireControllerNavigationResourcesAfterHandoff() {
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self,
                  self.activeNavigationInputMode == .touch,
                  self.pendingNavigationModeSwitchRequest == nil else { return }
            self.navigationSessionEntryTask?.cancel()
            self.navigationSessionEntryTask = nil
            self.pendingNavigationSessionEntryRequest = nil
            self.pendingNavigationSessionCommands.removeAll(keepingCapacity: false)
            self.navigationSessions.removeAll(keepingCapacity: false)
            self.manuallyCapturedNavigation = false
            self.manualNavigationCaptures.removeAll(keepingCapacity: false)
            self.rightStickRedispatchTask?.cancel()
            self.rightStickRedispatchTask = nil
            self.rightStickTargets.removeAll(keepingCapacity: false)
            self.activeRightStickTargetID = nil
            self.updateNavigationCaptureState()
        }
    }
}
