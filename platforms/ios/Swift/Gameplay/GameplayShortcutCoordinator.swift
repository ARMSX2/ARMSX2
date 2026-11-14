// GameplayShortcutCoordinator.swift — the controller's quick save, load, undo and speed shortcuts
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor
@Observable
final class GameplayShortcutCoordinator {
    private var saveStateShortcutOperationActive = false
    private let saveStateUndo = SaveStateUndoModel.shared
    private var runtimeShortcutSpeedPercent: Int?
    private let settings = SettingsStore.shared
    private let feedback: GameplayFeedback

    /// True while a quick save or load runs. The view keeps the VM paused and owns the menu input.
    var operationActive: Bool { saveStateShortcutOperationActive }

    init(feedback: GameplayFeedback) {
        self.feedback = feedback
    }

    func handleEmulationControllerShortcut(
        _ shortcut: EmulationControllerShortcut
    ) {
        guard !saveStateShortcutOperationActive,
              ARMSX2Bridge.isVMRunning() else { return }

        switch shortcut {
        case .saveLastState:
            requestSaveToLastState()
        case .loadLastState:
            requestLoadFromLastState()
        case .increaseSpeed:
            if let percent = adjustActiveEmulationSpeed(byPercent: 25) {
                feedback.presentStatusMessage(
                    "\(settings.localized("Emulation Speed")): \(percent)%"
                )
            }
        case .decreaseSpeed:
            if let percent = adjustActiveEmulationSpeed(byPercent: -25) {
                feedback.presentStatusMessage(
                    "\(settings.localized("Emulation Speed")): \(percent)%"
                )
            }
        case .enableFastForward:
            settings.fastForwardScalar = 10.0
            settings.setRuntimeFastForwardEnabled(true)
            feedback.presentStatusMessage(settings.localized("Fast Forward: ON (1000%)"))
        case .disableFastForward:
            runtimeShortcutSpeedPercent = 100
            settings.setRuntimeFastForwardEnabled(false)
            feedback.presentStatusMessage(settings.localized("Fast Forward: OFF (100%)"))
        case .undoSaveState:
            undoSaveStateAction()
        }
    }

    @discardableResult
    private func adjustActiveEmulationSpeed(byPercent delta: Int) -> Int? {
        if ARMSX2Bridge.limiterMode() == 1 {
            let currentPercent = Int((settings.fastForwardScalar * 100).rounded())
            let nextPercent = min(
                max(currentPercent + delta, 125),
                1_000
            )
            guard nextPercent != currentPercent else { return nil }
            settings.fastForwardScalar = Float(nextPercent) / 100.0
            settings.setRuntimeFastForwardEnabled(true)
            return nextPercent
        }

        let storedPercent = Int((ARMSX2Bridge.getINIFloat(
            "Framerate",
            key: "NominalScalar",
            defaultValue: 1.0
        ) * 100).rounded())
        let currentPercent = runtimeShortcutSpeedPercent ?? storedPercent
        let minimumPercent = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
            ? 100
            : 25
        let nextPercent = min(
            max(currentPercent + delta, minimumPercent),
            1_000
        )
        guard nextPercent != currentPercent else { return nil }
        runtimeShortcutSpeedPercent = nextPercent
        ARMSX2Bridge.setRuntimeEmulationSpeedPercent(Int32(nextPercent))
        return nextPercent
    }

    /// Saves to Quick Save at once. Saving over the last one leaves it one Undo away.
    private func requestSaveToLastState() {
        guard let quick = ARMSX2Bridge.saveStateSlots().first(where: { $0.slot == SaveStateSlot.quickSlot }) else {
            feedback.presentRetroAchievementsToast(
                title: settings.localized("Save states are not ready yet."),
                message: settings.localized(
                    "Wait until the game has fully identified, then try again."
                ),
                badgePath: "",
                duration: nil
            )
            return
        }
        let previous = SaveStateMetadataStore.shared.metadata(for: SaveStateFile(quick))
        let fileName = quick.fileName
        let slot = quick.slot
        saveStateUndo.finishIfTouching(slot: slot)
        beginSaveStateShortcutOperation()
        ARMSX2Bridge.saveState(toSlot: slot) { [self] success, backupToken in
            Task { @MainActor in
                finishSaveStateShortcutOperation()
                guard success,
                      let saved = ARMSX2Bridge.saveStateSlots().first(where: { $0.slot == slot })
                        .map(SaveStateFile.init) else {
                    feedback.presentImportantStatusMessage(
                        "\(settings.localized("Quick Save")): \(settings.localized("Try again after gameplay has fully loaded."))"
                    )
                    return
                }
                SaveStateMetadataStore.shared.recordSave(
                    of: saved,
                    playedSeconds: ARMSX2Bridge.currentGamePlayedSeconds(),
                    fresh: backupToken == nil
                )
                SaveStateAutoSave.shared.didWrite()
                if let backupToken {
                    showQuickSaveUndo(
                        .overwrite(slot: slot, backupToken: backupToken, fileName: fileName, previous: previous),
                        verb: settings.localized("Replaced"),
                        preview: saved.preview,
                        undoLabel: "Undo save over %@"
                    )
                } else {
                    feedback.presentRetroAchievementsToast(
                        title: settings.localized("Quick Saved"),
                        message: settings.localized("Saved State Created"),
                        badgePath: "",
                        duration: nil,
                        categoryTitle: settings.localized("Save Game State"),
                        symbolName: "square.stack.3d.up.fill",
                        imageData: saved.preview
                    )
                }
            }
        }
    }

    /// Loads Quick Save at once, keeping the moment before it one Undo away.
    private func requestLoadFromLastState() {
        guard !ARMSX2Bridge.isRetroAchievementsHardcoreActive() else {
            feedback.presentRetroAchievementsToast(
                title: settings.localized("Save State Unavailable"),
                message: settings.localized(
                    "Hardcore mode blocks loading save states."
                ),
                badgePath: "",
                duration: nil
            )
            return
        }

        guard let quick = ARMSX2Bridge.saveStateSlots().first(where: { $0.slot == SaveStateSlot.quickSlot }),
              quick.occupied else {
            feedback.presentRetroAchievementsToast(
                title: settings.localized("No Quick Save yet"),
                message: settings.localized("Create one by pressing")
                    + " \(settings.controllerMacroSaveGameState.title).",
                badgePath: "",
                duration: nil
            )
            return
        }
        let preview = quick.previewPNGData
        beginSaveStateShortcutOperation()
        ARMSX2Bridge.loadState(
            fromSlot: quick.slot,
            expectedModified: quick.modifiedDate,
            keepingUndo: true
        ) { [self] success, undoPath in
            Task { @MainActor in
                finishSaveStateShortcutOperation()
                if success, let undoPath {
                    SaveStateAutoSave.shared.restart()
                    showQuickSaveUndo(
                        .load(path: undoPath),
                        verb: settings.localized("Loaded"),
                        preview: preview,
                        undoLabel: "Undo load of %@"
                    )
                } else if !success {
                    feedback.presentImportantStatusMessage(
                        "\(settings.localized("Quick Save")): \(settings.localized("Make sure it has a saved state first."))"
                    )
                }
            }
        }
    }

    private func showQuickSaveUndo(
        _ action: SaveStateUndoModel.Action,
        verb: String,
        preview: Data?,
        undoLabel: String
    ) {
        let name = settings.localized("Quick Save")
        let caption = verb
        saveStateUndo.show(
            .init(
                action: action,
                caption: caption,
                name: name,
                preview: preview,
                undoLabel: String(format: settings.localized(undoLabel), name)
            ),
            announcement: String(format: settings.localized("%1$@ %2$@. Undo is available."), caption, name)
        )
    }

    func undoSaveStateAction() {
        guard saveStateUndo.item != nil else { return }
        saveStateUndo.undo { [self] ok in
            if !ok { feedback.presentImportantStatusMessage(settings.localized("Could not undo.")) }
        }
    }

    private func beginSaveStateShortcutOperation() {
        saveStateShortcutOperationActive = true
    }

    private func finishSaveStateShortcutOperation() {
        saveStateShortcutOperationActive = false
    }
}
