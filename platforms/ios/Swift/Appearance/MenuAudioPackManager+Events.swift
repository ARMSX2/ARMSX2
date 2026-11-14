// MenuAudioPackManager+Events.swift — menu events turned into UI sounds
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension MenuAudioPackManager {
    func play(_ feedback: MenuControllerFeedback) {
        let event: Event?
        switch feedback {
        case .move:
            event = .navigation
        case .activate, .favorite, .submenu, .destination:
            event = .select
        case .toggle(let isOn):
            event = .toggle(isOn: isOn)
        case .contextMenu:
            event = .contextMenu
        case .back:
            event = .return
        case .previousTab, .nextTab, .tabTransition:
            event = .tabTransition
        case .boundary:
            event = nil
        case .silent:
            event = nil
        }

        if let event {
            playEvent(event)
        }
    }

    func playEvent(_ event: Event) {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        guard introHasFinished, applicationIsActive, hasInstalledPack else { return }

        // A semantic action can run inside a generic controller activation.
        // Keep the semantic sound authoritative when the activation callback
        // returns and the focus engine emits its fallback Select feedback.
        switch event {
        case .return, .stoppingGame, .contextMenu, .uiToast,
             .tabTransition, .launchGame, .noJIT:
            suppressGenericSelection()
        case .navigation, .select, .toggle, .achievementToast:
            break
        }

        let requiresActiveInterface: Bool
        let sound: Sound
        switch event {
        case .navigation:
            requiresActiveInterface = true
            sound = .navigation
        case .select:
            let now = ProcessInfo.processInfo.systemUptime
            guard now >= selectionSuppressedUntil else { return }
            requiresActiveInterface = true
            sound = .select
        case .toggle(let isOn):
            requiresActiveInterface = true
            sound = isOn ? .switchToggleOn : .switchToggleOff
        case .return:
            requiresActiveInterface = true
            sound = .return
        case .stoppingGame:
            requiresActiveInterface = false
            sound = .stoppingGame
            selectionSuppressedUntil = ProcessInfo.processInfo.systemUptime + 0.2
        case .contextMenu:
            requiresActiveInterface = true
            sound = .contextMenu
        case .achievementToast:
            requiresActiveInterface = false
            sound = .achievementToast
        case .uiToast:
            requiresActiveInterface = false
            sound = .uiToast
        case .tabTransition:
            requiresActiveInterface = true
            sound = .tabTransition
        case .launchGame:
            requiresActiveInterface = false
            sound = .launchGame
        case .noJIT:
            requiresActiveInterface = false
            sound = .noJIT
        }
        guard !requiresActiveInterface || interfaceIsActive,
              shouldEmit(event) else { return }
        playEffect(sound)
    }

    private func shouldEmit(_ event: Event) -> Bool {
        let now = ProcessInfo.processInfo.systemUptime
        let minimumInterval: TimeInterval
        switch event {
        case .navigation:
            minimumInterval = 0.025
        case .toggle:
            minimumInterval = 0.04
        default:
            minimumInterval = 0.08
        }
        if let previous = lastEventPlaybackTimes[event],
           now - previous < minimumInterval {
            return false
        }
        lastEventPlaybackTimes[event] = now
        return true
    }

    /// Defers generic tap feedback briefly so a semantic action fired by the
    /// same touch (toggle, context menu, toast, return, tab change) can replace
    /// it instead of producing two sounds.
    func playTouchNavigation() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 60_000_000)
            guard let self, !Task.isCancelled else { return }
            self.pendingTouchNavigationTask = nil
            self.playEvent(.navigation)
        }
    }

    func playTouchToggle(isOn: Bool) {
        playEvent(.toggle(isOn: isOn))
    }

    func playTouchContextMenu() {
        playEvent(.contextMenu)
    }

    /// Removes the window-level tap fallback when the target action owns its
    /// complete sound sequence (for example, launch_game).
    func suppressPendingGenericActivation() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        suppressGenericSelection()
    }

    /// A stop selected inside Quick Menu belongs to the destination frontend,
    /// not to the overlay that is being dismantled.
    func playStoppingGameOnMainInterface() {
        pendingTouchNavigationTask?.cancel()
        pendingTouchNavigationTask = nil
        suppressGenericSelection()
        stoppingGamePendingForMainInterface = true
        stoppingGameWaitsForVMShutdown = true
    }

    private func suppressGenericSelection(for duration: TimeInterval = 0.2) {
        selectionSuppressedUntil = max(
            selectionSuppressedUntil,
            ProcessInfo.processInfo.systemUptime + duration
        )
    }
}
