// SpeedControlPanel.swift — the in-game emulation speed and fast forward panel
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SpeedControlPanel: View {
    @Bindable var settings: SettingsStore
    @Environment(\.dismiss) private var dismiss
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var hardcoreActive = false
    var onClose: (() -> Void)? = nil

    // Done sits in the toolbar above the rows, so it comes first.
    private static let controllerTargetOrder = [
        "runtime.speed.done",
        "runtime.speed.fast-forward",
        "runtime.speed.scalar",
    ]

    var body: some View {
        NavigationStack {
            Form {
                Section(settings.localized("Fast Forward")) {
                    Toggle(
                        settings.localized("Enable Fast Forward"),
                        isOn: fastForwardEnabledBinding
                    )
                    .controllerAccessibilityToggleTarget(
                        id: "runtime.speed.fast-forward",
                        label: settings.localized("Enable Fast Forward"),
                        isOn: fastForwardEnabledBinding
                    )

                    NumberRow(.fastForwardSpeed, value: $settings.fastForwardScalar,
                              settings: settings)
                        .controllerAccessibilityTargetID(
                            "runtime.speed.scalar"
                        )
                }

                if hardcoreActive {
                    Section(settings.localized("Hardcore Mode")) {
                        Text(settings.localized("Hardcore mode blocks slowdown and frame advance. Fast forward stays available; Normal Speed is locked at 100% or higher."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Section(settings.localized("How It Works")) {
                    Text(settings.localized("The FPS Target changes display presentation without slowing CPU, audio, or game timing. Fast Forward remains a separate emulation-speed control."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    HStack {
                        Text(settings.localized("Normal Speed"))
                        Spacer()
                        Text(Self.formatPercent(SettingsStore.normalSpeedScalar(
                            frameLimiterEnabled: settings.frameLimiterEnabled)))
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }
                }
            }
            .scrollContentBackground(onClose == nil ? .automatic : .hidden)
            .navigationTitle(settings.localized("Speed / Fast Forward"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        close()
                    }
                    .controllerAccessibilityActionTarget(
                        id: "runtime.speed.done",
                        label: settings.localized("Done"),
                        activationFeedback: .back
                    ) {
                        close()
                    }
                }
            }
            .onAppear {
                refreshRuntimeState()
            }
            .onReceive(NotificationCenter.default.publisher(for: .ARMSX2RetroAchievementsStateChanged)) { _ in
                refreshRuntimeState()
            }
        }
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "runtime.speed-control",
            priority: 345,
            orbStyle: .plain,
            onBack: {
                close()
                return true
            },
            confinesHorizontalFocusMovement: true,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            preferredInitialFocusLabel: "runtime.speed.fast-forward",
            declaredTargetOrder: Self.controllerTargetOrder
        )
    }

    private func close() {
        if let onClose { onClose() } else { dismiss() }
    }

    private var fastForwardEnabledBinding: Binding<Bool> {
        Binding(
            get: { settings.fastForwardRuntimeEnabled },
            set: { enabled in
                settings.setRuntimeFastForwardEnabled(enabled)
            }
        )
    }

    private func refreshRuntimeState() {
        settings.fastForwardRuntimeEnabled = ARMSX2Bridge.limiterMode() == 1
        hardcoreActive = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
    }

    private static func formatPercent(_ scalar: Float) -> String {
        String(format: "%.0f%%", scalar * 100.0)
    }
}
