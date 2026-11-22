// PerGameLivePreviewSession.swift — the in-game live preview of per-game edits
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

enum PerGameLivePreviewPresentation: Equatable {
    case editing
    case playingBefore
    case applying
    case playingAfter
    case restoring
    case finishing

    var hidesSettingsUI: Bool { self != .editing && self != .finishing }
    var runsGame: Bool {
        self == .playingBefore || self == .playingAfter
    }
}

/// One stable live-preview toast update. The setting label deliberately stays out of this model:
/// rapid controller repeats should roll the new value inside the existing toast, not replace a
/// longer `Setting -> Value` sentence twice per adjustment.
struct PerGameLivePreviewStatus: Equatable {
    let value: String
    let countsDown: Bool
}

@MainActor
@Observable
final class PerGameLivePreviewSession {
    let draft: PerGameSettingsDraft
    let settings = SettingsStore.shared
    var livePreviewToken: String?
    var livePreviewTask: Task<Void, Never>?
    var livePreviewPendingFingerprint: Int?
    var livePreviewChangeSequence: UInt64 = 0
    var livePreviewInitialization: Task<String?, Never>?
    var livePreviewSessionIsFinishing = false
    var lastPreviewedFingerprint = 0
    var suppressNextLivePreviewChange = false
    var livePreviewPresentation:
        PerGameLivePreviewPresentation = .editing

    private let onLivePreviewPresentationChange:
        ((PerGameLivePreviewPresentation) -> Void)?
    private let onLivePreviewStatusChange:
        ((PerGameLivePreviewStatus) -> Void)?

    init(
        draft: PerGameSettingsDraft,
        onLivePreviewPresentationChange:
            ((PerGameLivePreviewPresentation) -> Void)?,
        onLivePreviewStatusChange:
            ((PerGameLivePreviewStatus) -> Void)?
    ) {
        self.draft = draft
        self.onLivePreviewPresentationChange =
            onLivePreviewPresentationChange
        self.onLivePreviewStatusChange = onLivePreviewStatusChange
        lastPreviewedFingerprint = draft.perGameLivePreviewFingerprint()
    }

    func fingerprintChanged(_ fingerprint: Int) {
        if suppressNextLivePreviewChange {
            suppressNextLivePreviewChange = false
            lastPreviewedFingerprint = fingerprint
            return
        }
        scheduleLivePreview(for: fingerprint)
    }

    func dismissRequested() {
        guard livePreviewPresentation.hidesSettingsUI else { return }
        // The presentation owner captured this button before it could
        // reach the hidden editor. Cancelling joins any in-flight native
        // apply, restores the temporal state, and remounts the panel.
        livePreviewTask?.cancel()
    }

    func temporalPreviewSettingChanged(_ enabled: Bool) {
        if enabled {
            if livePreviewToken == nil {
                beginLivePreviewSessionIfNeeded()
            } else {
                scheduleLivePreview(
                    for: draft.perGameLivePreviewFingerprint()
                )
            }
        } else {
            livePreviewTask?.cancel()
            if livePreviewTask == nil {
                publishLivePreviewPresentation(.editing)
            }
        }
    }

    func panelDisappeared() {
        let wasAlreadyFinishing = livePreviewSessionIsFinishing
        livePreviewSessionIsFinishing = true
        let previewTask = livePreviewTask
        let initialization = livePreviewInitialization
        let existingToken = livePreviewToken
        previewTask?.cancel()
        guard draft.savesToRunningGame,
              !wasAlreadyFinishing else { return }
        // A system-driven dismissal is cancellation, never an implicit save.
        Task { @MainActor in
            await previewTask?.value
            let initializedToken = await initialization?.value
            guard let token = existingToken ?? initializedToken else { return }
            _ = await ARMSX2Bridge.finishPerGameLivePreview(token: token)
        }
    }

    @MainActor
    func beginLivePreviewSessionIfNeeded() {
        guard draft.savesToRunningGame,
              settings.appearance.temporalSaveStateToLivePreviewChanges,
              livePreviewToken == nil,
              livePreviewInitialization == nil,
              !livePreviewSessionIsFinishing else { return }

        let initialization = Task<String?, Never> { @MainActor in
            await withCheckedContinuation { continuation in
                ARMSX2Bridge.beginPerGameLivePreview { token in
                    continuation.resume(returning: token)
                }
            }
        }
        livePreviewInitialization = initialization
        Task { @MainActor in
            let token = await initialization.value
            // Dismissal owns the transaction once closing starts, including
            // a save-state capture that was still in flight.
            guard !livePreviewSessionIsFinishing else { return }
            livePreviewInitialization = nil
            guard settings.appearance.temporalSaveStateToLivePreviewChanges else {
                if let token {
                    _ = await ARMSX2Bridge.finishPerGameLivePreview(token: token)
                }
                return
            }
            livePreviewToken = token
            guard token != nil else {
                livePreviewPendingFingerprint = nil
                draft.statusMessage = settings.localized(
                    "Live Preview is unavailable while save states are unavailable."
                )
                return
            }
            if livePreviewPendingFingerprint != nil {
                startLivePreviewTaskIfNeeded()
            }
        }
    }

    @MainActor
    private func scheduleLivePreview(for fingerprint: Int) {
        guard draft.savesToRunningGame,
              settings.appearance.temporalSaveStateToLivePreviewChanges,
              !livePreviewSessionIsFinishing else { return }

        let previewIsIdle = livePreviewTask == nil
            && livePreviewPresentation == .editing
        guard !previewIsIdle
                || fingerprint != lastPreviewedFingerprint else { return }

        livePreviewPendingFingerprint = fingerprint
        livePreviewChangeSequence &+= 1

        guard livePreviewToken != nil else {
            beginLivePreviewSessionIfNeeded()
            return
        }

        startLivePreviewTaskIfNeeded()
    }

    @MainActor
    private func startLivePreviewTaskIfNeeded() {
        guard livePreviewTask == nil,
              livePreviewPendingFingerprint != nil,
              livePreviewToken != nil,
              !livePreviewSessionIsFinishing else { return }
        livePreviewTask = Task { @MainActor in
            await runLivePreviewLoop()
        }
    }

    @MainActor
    private func runLivePreviewLoop() async {
        defer {
            livePreviewTask = nil
            if livePreviewPendingFingerprint != nil,
               settings.appearance.temporalSaveStateToLivePreviewChanges,
               !livePreviewSessionIsFinishing {
                startLivePreviewTaskIfNeeded()
            }
        }
        guard let token = livePreviewToken else { return }
        let beforeDuration = max(0, settings.appearance.perGameBeforeChangesPreviewDuration)

        do {
            try Task.checkCancellation()
            // Zero skips the before phase entirely: only native application
            // of the edited values precedes the changed preview.
            if beforeDuration > 0 {
                let initialSequence = livePreviewChangeSequence
                publishLivePreviewPresentation(.playingBefore)
                let beforeDeadline = Date.timeIntervalSinceReferenceDate
                    + beforeDuration
                while Date.timeIntervalSinceReferenceDate < beforeDeadline,
                      livePreviewChangeSequence == initialSequence {
                    try await Task.sleep(for: .milliseconds(16))
                    try Task.checkCancellation()
                }
            }

            while let fingerprint = livePreviewPendingFingerprint {
                let sequenceBeingApplied = livePreviewChangeSequence

                publishLivePreviewPresentation(.applying)
                ARMSX2Bridge.refreshPerGameLivePreviewBaseline(token: token, afterSave: false)
                guard draft.writeCurrentSettings(normalizesEditableValues: false),
                      await applyLivePreview(token: token) else {
                    throw CancellationError()
                }
                try Task.checkCancellation()

                // A value can change while the CPU thread is applying the
                // previous one. Consume the newest state immediately instead
                // of briefly showing an obsolete frame.
                if livePreviewChangeSequence != sequenceBeingApplied {
                    continue
                }

                publishLivePreviewPresentation(.playingAfter)
                lastPreviewedFingerprint = fingerprint
                // Remain in the changed scene until Circle/touch cancels the
                // preview, but immediately apply a newly adjusted Left/Right
                // value without remounting the editor between changes.
                while livePreviewChangeSequence == sequenceBeingApplied {
                    try await Task.sleep(for: .milliseconds(16))
                    try Task.checkCancellation()
                }
            }
        } catch {
            if !livePreviewSessionIsFinishing {
                await recoverCancelledLivePreview()
            }
        }
    }

    @MainActor
    private func recoverCancelledLivePreview() async {
        guard !livePreviewSessionIsFinishing else { return }
        guard let token = livePreviewToken else {
            livePreviewPendingFingerprint = nil
            publishLivePreviewPresentation(.editing)
            return
        }
        publishLivePreviewPresentation(.restoring)
        _ = await restoreLivePreviewState(token: token)
        guard !livePreviewSessionIsFinishing else { return }
        livePreviewPendingFingerprint = nil
        publishLivePreviewPresentation(.editing)
    }

    @MainActor
    func publishLivePreviewStatus(
        value: String?,
        countsDown: Bool
    ) {
        guard livePreviewPendingFingerprint != nil
                || livePreviewPresentation != .editing else { return }
        let resolvedValue = value?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard let resolvedValue, !resolvedValue.isEmpty else { return }
        onLivePreviewStatusChange?(
            PerGameLivePreviewStatus(
                value: settings.localized(resolvedValue),
                countsDown: countsDown
            )
        )
    }

    private func applyLivePreview(token: String) async -> Bool {
        await withCheckedContinuation { continuation in
            ARMSX2Bridge.applyPerGameLivePreview(
                token: token
            ) { success in
                continuation.resume(returning: success)
            }
        }
    }

    private func restoreLivePreviewState(token: String) async -> Bool {
        await withCheckedContinuation { continuation in
            ARMSX2Bridge.restorePerGameLivePreviewState(
                token: token
            ) { success in
                continuation.resume(returning: success)
            }
        }
    }

    @MainActor
    func publishLivePreviewPresentation(
        _ presentation: PerGameLivePreviewPresentation
    ) {
        guard !livePreviewSessionIsFinishing || presentation == .finishing,
              livePreviewPresentation != presentation else { return }
        livePreviewPresentation = presentation
        onLivePreviewPresentationChange?(presentation)
    }
}
