// VirtualPadDynamicControlSections.swift — dynamic control presets, thumbsticks and sensitivity
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct VirtualPadDynamicControlSections: View {
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = DynamicThumbstickSettings.shared

    var body: some View {
        Section {
            ForEach(BuiltInDynamicControlPreset.allCases) { preset in
                Button {
                    preset.apply(settings: dynamicSettings)
                } label: {
                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(settings.localized(preset.rawValue))
                                .controllerFocusedTextColor()
                            Text(settings.localized(preset.summary))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .multilineTextAlignment(.leading)
                                .lineLimit(nil)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .layoutPriority(1)
                        Spacer()
                        if preset.isActive(settings: dynamicSettings) {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundStyle(.green)
                                .fixedSize()
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .controllerAccessibilityActionTarget(
                    id: settings.localized(preset.rawValue),
                    label: settings.localized(preset.rawValue)
                ) {
                    preset.apply(settings: dynamicSettings)
                }
            }
        } header: {
            Text(settings.localized("Dynamic Control Presets"))
        } footer: {
            Text(settings.localized("Each preset applies its listed Dynamic Control configuration. Unrelated Virtual Pad appearance and controller-layout settings are preserved."))
                .lineLimit(nil)
                .fixedSize(horizontal: false, vertical: true)
        }

        Section {
            controllerToggle(
                settings.localized("Legacy Thumbsticks"),
                isOn: Binding(
                    get: { dynamicSettings.legacyThumbsticks },
                    set: { dynamicSettings.setLegacyThumbsticks($0) }
                )
            )
            controllerToggle(
                settings.localized("Dynamic Thumbsticks"),
                isOn: Binding(
                    get: { dynamicSettings.dynamicThumbsticks },
                    set: { dynamicSettings.setDynamicThumbsticks($0) }
                )
            )
            controllerToggle(settings.localized("Swipe Camera"), isOn: $dynamicSettings.swipeCamera)
            controllerToggle(settings.localized("Gyroscope Camera"), isOn: $dynamicSettings.gyroscopeCamera)
        } header: {
            Text(settings.localized("Dynamic Controls"))
        } footer: {
            Text(settings.localized("Legacy and Dynamic Thumbsticks are mutually exclusive. Dynamic sticks appear where each touch begins. Swipe Camera replaces the right touch stick, while Gyroscope Camera augments the active right-side control."))
        }

        Section {
            controllerToggle(
                settings.localized("Left Thumbstick Instant Deadzone"),
                isOn: $dynamicSettings.leftInstantDeadzoneEnabled
            )
            if dynamicSettings.leftInstantDeadzoneEnabled {
                DynamicControlSlider(
                    title: "Left Negative Deadzone",
                    value: $dynamicSettings.leftNegativeDeadzone,
                    range: -0.25...0,
                    step: 0.01,
                    format: .unitPercent
                )
            }

            controllerToggle(
                settings.localized("Right Thumbstick Instant Deadzone"),
                isOn: $dynamicSettings.rightInstantDeadzoneEnabled
            )
            if dynamicSettings.rightInstantDeadzoneEnabled {
                DynamicControlSlider(
                    title: "Right Negative Deadzone",
                    value: $dynamicSettings.rightNegativeDeadzone,
                    range: -0.25...0,
                    step: 0.01,
                    format: .unitPercent
                )
            }

            DynamicControlSlider(
                title: "Left Thumbstick Movement Area",
                value: $dynamicSettings.leftThumbstickAreaScale,
                range: 1...5,
                step: 0.25,
                format: .multiplier
            )
            DynamicControlSlider(
                title: "Right Thumbstick Movement Area",
                value: $dynamicSettings.rightThumbstickAreaScale,
                range: 1...5,
                step: 0.25,
                format: .multiplier
            )

            controllerToggle(
                settings.localized("Convert Swipe to Dynamic Joystick"),
                isOn: $dynamicSettings.convertSwipeToDynamicJoystick
            )
            if dynamicSettings.convertSwipeToDynamicJoystick {
                DynamicControlSlider(
                    title: "Convert Into Dynamic Thumbstick",
                    value: Binding(
                        get: { dynamicSettings.convertIntoDynamicThumbstickThreshold },
                        set: { dynamicSettings.setConvertIntoDynamicThumbstickThreshold($0) }
                    ),
                    range: 0.01...3,
                    step: 0.01,
                    format: .unitPercent
                )
                DynamicControlSlider(
                    title: "Pulling Back Distance",
                    value: Binding(
                        get: { dynamicSettings.pullingBackDistance },
                        set: { dynamicSettings.setPullingBackDistance($0) }
                    ),
                    range: 0.01...1,
                    step: 0.01,
                    format: .unitPercent
                )
            }
        } header: {
            Text(settings.localized("Instant Movement & Aiming"))
        } footer: {
            Text(settings.localized("Instant deadzones add the selected minimum output only after real movement, while preserving the progressive Dynamic Thumbstick deadzone. Movement areas default to 3× without enlarging the visible controls. Swipe conversion enters Dynamic Thumbstick mode at the selected outward distance. Pulling back by the selected distance returns to Swipe Camera and temporarily rebases the next outward conversion point until the screen is released."))
        }

        controlSensitivitySection
    }

    @ViewBuilder
    private var controlSensitivitySection: some View {
        Section {
            DynamicControlSlider(
                title: "Movement Sensitivity",
                value: $dynamicSettings.movementSensitivity,
                range: 0.33...2.0,
                step: 0.01,
                format: .unitPercent
            )
            DynamicControlSlider(
                title: "Look Sensitivity",
                value: $dynamicSettings.lookSensitivity,
                range: 0.43...1.71,
                step: 0.01,
                format: .unitPercent
            )
            DynamicSwipeSensitivityControl(
                title: "Swipe Sensitivity",
                showsEnableToggle: false,
                isEnabled: .constant(true),
                value: $dynamicSettings.swipeSensitivity,
                horizontalSensitivity: $dynamicSettings.swipeHorizontalSensitivity,
                verticalSensitivity: $dynamicSettings.swipeVerticalSensitivity
            )
            .disabled(!dynamicSettings.swipeCamera)
            DynamicSwipeSensitivityControl(
                title: "Sensitivity While on Aim Mode",
                showsEnableToggle: true,
                isEnabled: $dynamicSettings.swipeSensitivityWhileAimingEnabled,
                value: $dynamicSettings.swipeSensitivityWhileAiming,
                horizontalSensitivity: $dynamicSettings.swipeHorizontalSensitivityWhileAiming,
                verticalSensitivity: $dynamicSettings.swipeVerticalSensitivityWhileAiming
            )
            .disabled(!dynamicSettings.swipeCamera)
            DynamicSwipeSensitivityControl(
                title: "Sensitivity While Not Aiming",
                showsEnableToggle: true,
                isEnabled: $dynamicSettings.swipeSensitivityWhileNotAimingEnabled,
                value: $dynamicSettings.swipeSensitivityWhileNotAiming,
                horizontalSensitivity: $dynamicSettings.swipeHorizontalSensitivityWhileNotAiming,
                verticalSensitivity: $dynamicSettings.swipeVerticalSensitivityWhileNotAiming
            )
            .disabled(!dynamicSettings.swipeCamera)
        } header: {
            Text(settings.localized("Dynamic Control Sensitivity"))
        } footer: {
            Text(settings.localized("Each swipe profile has overall, horizontal, and vertical sensitivity. Disabled aim-state profiles fall back to Swipe Sensitivity."))
        }
    }
}

/// A NumberRow that always steps. Kept as a name of its own because this screen has thirty of
/// them and the argument order reads better than five labelled arguments repeated thirty times.
///
/// These are the only rows left that describe themselves at the call site, and they can: each one
/// is on this screen and nowhere else, so there is nothing for it to disagree with.
struct DynamicControlSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    /// Always a real format, never a prebuilt string: a row that hands over a finished readout
    /// cannot label its bounds or be typed into, and it ends up the odd one out in the stack.
    var format: NumberFormat = .plain
    var controllerID: String? = nil

    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        NumberRow(title,
                  value: $value,
                  in: range,
                  format: format,
                  step: step,
                  detents: NumberRow.stops(in: range, step: step),
                  settings: settings)
            .controllerAccessibilityTargetID(controllerID ?? settings.localized(title))
    }
}

private struct DynamicSwipeSensitivityControl: View {
    private static let sensitivityRange: ClosedRange<Double> = 0.08...0.75
    private static let sensitivityStep = 0.01

    let title: String
    let showsEnableToggle: Bool
    @Binding var isEnabled: Bool
    @Binding var value: Double
    @Binding var horizontalSensitivity: Double
    @Binding var verticalSensitivity: Double

    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            // The toggle used to carry the readout too. It moved down onto the row that actually
            // changes it, which is also the only way the slider gets an announced value.
            if showsEnableToggle {
                controllerToggle(settings.localized(title), isOn: $isEnabled)
            }
            // When the toggle above is already carrying the name, the row underneath is just the
            // amount, so it says so rather than printing the same words twice.
            NumberRow(showsEnableToggle ? "Sensitivity" : title,
                      value: $value,
                      in: Self.sensitivityRange,
                      format: .degreesPerPoint,
                      step: Self.sensitivityStep,
                      detents: NumberRow.stops(in: Self.sensitivityRange,
                                               step: Self.sensitivityStep),
                      settings: settings)
                .controllerAccessibilityTargetID(settings.localized(title) + "/value")
                .disabled(!isEnabled)
            DynamicControlSlider(
                title: "Horizontal Swipe Sensitivity",
                value: $horizontalSensitivity,
                range: 0.25...2,
                step: 0.01,
                format: .unitPercent,
                controllerID: settings.localized(title) + "/horizontal"
            )
            .disabled(!isEnabled)
            DynamicControlSlider(
                title: "Vertical Swipe Sensitivity",
                value: $verticalSensitivity,
                range: 0.25...2,
                step: 0.01,
                format: .unitPercent,
                controllerID: settings.localized(title) + "/vertical"
            )
            .disabled(!isEnabled)
        }
    }
}
