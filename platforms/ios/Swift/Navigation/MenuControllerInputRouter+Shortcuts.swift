// MenuControllerInputRouter+Shortcuts.swift — theme preset and emulation macro shortcuts
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import GameController
import UIKit

extension MenuControllerInputRouter {
    /// Theme shortcuts belong to the main tabs, never a gameplay overlay,
    /// keyboard, picker, or dialog. Keep them out of directional routing.
    var canChangeMainMenuTheme: Bool {
        isMenuActive
            && AppState.shared.currentScreen == .menu
            && GameSession.shared.gameplayLaunchTransition == nil
            && launchInputInterceptor == nil
            && frontmostManualNavigationCapture == nil
            && (frontmostNavigationSessionEntry?.value.priority ?? 0) < 100
    }

    func updateThemePresetButton(
        _ button: Button,
        pressed: Bool,
        step: Int,
        profileID: ObjectIdentifier
    ) {
        let wasPressed = pressedButtons[profileID, default: []].contains(button)
        if pressed {
            pressedButtons[profileID, default: []].insert(button)
        } else {
            pressedButtons[profileID, default: []].remove(button)
            themePresetRepeatTasks[profileID]?[button]?.cancel()
            themePresetRepeatTasks[profileID]?.removeValue(forKey: button)
            if themePresetRepeatTasks[profileID]?.isEmpty == true {
                themePresetRepeatTasks.removeValue(forKey: profileID)
            }
            return
        }
        guard pressed, !wasPressed, UIFrameRateSettings.shared.changesThemeWithTriggers,
              canChangeMainMenuTheme else { return }
        guard publishThemePresetRequest(step: step) else { return }

        themePresetRepeatTasks[profileID]?[button]?.cancel()
        let task = Task { @MainActor [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(420))
            } catch {
                return
            }
            var repeatInterval = 250
            while !Task.isCancelled {
                guard let self,
                      self.pressedButtons[profileID]?.contains(button) == true,
                      self.canChangeMainMenuTheme else { return }
                guard self.publishThemePresetRequest(step: step) else { return }
                do {
                    try await Task.sleep(
                        for: .milliseconds(repeatInterval)
                    )
                } catch {
                    return
                }
                // Deliberate trigger holds become a quick gallery scrub while
                // the first press remains a precise single-step selection.
                repeatInterval = max(70, Int(Double(repeatInterval) * 0.84))
            }
        }
        themePresetRepeatTasks[profileID, default: [:]][button] = task
    }

    func cancelThemePresetRepeats() {
        for tasks in themePresetRepeatTasks.values {
            for task in tasks.values {
                task.cancel()
            }
        }
        themePresetRepeatTasks.removeAll(keepingCapacity: true)
    }

    @discardableResult
    private func publishThemePresetRequest(step: Int) -> Bool {
        guard prepareControllerNavigationForInput() else { return false }
        sequence &+= 1
        latestThemePresetRequest = .init(sequence: sequence, step: step)
        return true
    }

    /// Mirrors an L2/R2 theme step for a direct-touch gesture without making
    /// controller focus visible. The shared overlay still owns selection,
    /// confirmation, wrapping, audio, and haptics.
    func requestThemePresetChangeFromTouch(step: Int) {
        guard step != 0,
              Date.timeIntervalSinceReferenceDate
                >= touchThemePresetShortcutSuppressedUntil,
              canChangeMainMenuTheme else { return }
        noteTouchInput()
        guard pendingNavigationModeSwitchRequest == nil,
              activeNavigationInputMode == .touch else { return }
        sequence &+= 1
        latestThemePresetRequest = .init(sequence: sequence, step: step)
    }

    /// A SwiftUI game card owns its custom long press without exposing a
    /// `UIContextMenuInteraction` to RootView's window recognizer. Suppress the
    /// deferred empty-space shortcut for this gesture transaction only.
    func suppressTouchThemePresetShortcut() {
        touchThemePresetShortcutSuppressedUntil = max(
            touchThemePresetShortcutSuppressedUntil,
            // Card recognizers suppress from touch-down, before their 420 ms stationary-hold
            // threshold. Keep the claim through the release transaction so RootView's window
            // recognizer cannot read the same hold as an empty-space L2/R2 shortcut.
            Date.timeIntervalSinceReferenceDate + 1.0
        )
    }

    /// Gameplay macros never participate in menu focus routing. This is
    /// important: opening a confirmation surface from a save/load chord must
    /// not also deliver its face button into the newly mounted alert.
    @discardableResult
    func updateEmulationControllerMacros(
        _ gamepad: GCExtendedGamepad,
        profileID: ObjectIdentifier
    ) -> Bool {
        guard !isMenuActive, ARMSX2Bridge.isVMRunning() else {
            emulationMacroLatches.removeValue(forKey: profileID)
            cancelEmulationMacroRepeats(profileID: profileID)
            return false
        }

        let settings = SettingsStore.shared
        var active = Set<ControllerMacroAction>()
        for action in ControllerMacroAction.allCases
        where settings.controller.controllerMacroBinding(for: action).isPressed(on: gamepad) {
            active.insert(action)
        }

        let previous = emulationMacroLatches[profileID, default: []]
        for action in active.subtracting(previous) {
            let binding = settings.controller.controllerMacroBinding(for: action)
            ARMSX2Bridge.consumeControllerMacroInput(mask: binding.gameplayInputMask)
            publishControllerMacro(action)
            if action.repeatsWhileHeld {
                beginEmulationMacroRepeat(
                    action,
                    gamepad: gamepad,
                    profileID: profileID
                )
            }
        }
        for action in previous.subtracting(active) {
            cancelEmulationMacroRepeat(action, profileID: profileID)
        }

        if active.isEmpty {
            emulationMacroLatches.removeValue(forKey: profileID)
        } else {
            emulationMacroLatches[profileID] = active
        }
        return !active.isEmpty
    }

    private func publishControllerMacro(_ action: ControllerMacroAction) {
        if action == .quickMenu {
            sequence &+= 1
            latestQuickPauseRequest = MenuControllerQuickPauseRequest(
                sequence: sequence
            )
        } else if let shortcut = action.emulationShortcut {
            publishEmulationShortcut(shortcut)
        }
    }

    private func beginEmulationMacroRepeat(
        _ action: ControllerMacroAction,
        gamepad: GCExtendedGamepad,
        profileID: ObjectIdentifier
    ) {
        cancelEmulationMacroRepeat(action, profileID: profileID)
        let task = Task { @MainActor [weak self, weak gamepad] in
            try? await Task.sleep(for: .milliseconds(320))
            guard let self, let gamepad, !Task.isCancelled else { return }
            while !Task.isCancelled,
                  !self.isMenuActive,
                  ARMSX2Bridge.isVMRunning(),
                  SettingsStore.shared.controller.controllerMacroBinding(for: action)
                    .isPressed(on: gamepad),
                  self.emulationMacroLatches[profileID]?.contains(action) == true {
                self.publishControllerMacro(action)
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
        emulationMacroRepeatTasks[profileID, default: [:]][action] = task
    }

    private func publishEmulationShortcut(
        _ shortcut: EmulationControllerShortcut
    ) {
        sequence &+= 1
        latestEmulationShortcutRequest = EmulationControllerShortcutRequest(
            sequence: sequence,
            shortcut: shortcut
        )
    }

    private func cancelEmulationMacroRepeat(
        _ action: ControllerMacroAction,
        profileID: ObjectIdentifier
    ) {
        emulationMacroRepeatTasks[profileID]?.removeValue(forKey: action)?.cancel()
        if emulationMacroRepeatTasks[profileID]?.isEmpty == true {
            emulationMacroRepeatTasks.removeValue(forKey: profileID)
        }
    }

    func cancelEmulationMacroRepeats(profileID: ObjectIdentifier) {
        guard let tasks = emulationMacroRepeatTasks
            .removeValue(forKey: profileID)?.values else { return }
        for task in tasks {
            task.cancel()
        }
    }

    func cancelEmulationShortcutInput() {
        for tasks in emulationMacroRepeatTasks.values {
            for task in tasks.values { task.cancel() }
        }
        emulationMacroRepeatTasks.removeAll(keepingCapacity: true)
        emulationMacroLatches.removeAll(keepingCapacity: true)
    }
}
