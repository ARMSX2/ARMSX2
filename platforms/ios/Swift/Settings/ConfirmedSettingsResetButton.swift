// ConfirmedSettingsResetButton.swift — confirmed settings reset button
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// Shared full-reset interaction for Settings surfaces. A reset is important
/// enough to confirm, but its success feedback should stay in context instead
/// of replacing the current screen with a second modal.
struct ConfirmedSettingsResetButton: View {
    let title: String
    let confirmationTitle: String
    let confirmationMessage: String
    let completionMessage: String
    let controllerTargetID: String?
    let action: () -> Void

    @State private var showsCompletion = false
    @State private var completionTask: Task<Void, Never>?
    @Environment(\.menuControllerInputRouter) private var controllerInput

    private var settings: SettingsStore { SettingsStore.shared }

    init(
        _ title: String,
        confirmationTitle: String,
        confirmationMessage: String,
        completionMessage: String,
        controllerTargetID: String? = nil,
        action: @escaping () -> Void
    ) {
        self.title = title
        self.confirmationTitle = confirmationTitle
        self.confirmationMessage = confirmationMessage
        self.completionMessage = completionMessage
        self.controllerTargetID = controllerTargetID
        self.action = action
    }

    var body: some View {
        Button(role: .destructive, action: confirm) {
            HStack(spacing: 12) {
                Text(title)
                Spacer(minLength: 8)
                if showsCompletion {
                    Label(completionMessage, systemImage: "checkmark.circle.fill")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.green)
                        .lineLimit(1)
                        .transition(
                            .opacity.combined(with: .scale(scale: 0.92))
                        )
                }
            }
            .contentShape(Rectangle())
        }
        .controllerAccessibilityActionTarget(
            id: controllerTargetID,
            label: title,
            action: confirm
        )
        .onDisappear {
            completionTask?.cancel()
            completionTask = nil
        }
    }

    private func confirm() {
        ControllerPrompt.shared.ask(
            confirmationTitle,
            message: confirmationMessage,
            actions: [
                .cancel,
                .init(title: settings.localized("Reset"), isDestructive: true, run: performReset),
            ]
        )
    }

    @MainActor
    private func performReset() {
        action()
        completionTask?.cancel()
        withAnimation(.easeOut(duration: 0.18)) {
            showsCompletion = true
        }
        announceSettingsResetSuccess(
            completionMessage,
            controllerInput: controllerInput
        )
        completionTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(2.4))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                showsCompletion = false
            }
            completionTask = nil
        }
    }
}

/// Use this from reset flows which already own a confirmation/result alert.
/// The reusable reset row above calls the same path automatically.
@MainActor
func announceSettingsResetSuccess(
    _ message: String,
    controllerInput: MenuControllerInputRouter? = nil
) {
    MenuAudioPackManager.shared.playEvent(.uiToast)
    if let controllerInput {
        controllerInput.playTouchHaptics(.destination)
    } else {
        UINotificationFeedbackGenerator().notificationOccurred(.success)
    }
    UIAccessibility.post(notification: .announcement, argument: message)
}
