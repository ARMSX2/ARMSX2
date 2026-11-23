// VirtualPadThumbstickSections.swift — thumbstick actions, crosshair, stick feel and gyroscope
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct VirtualPadThumbstickSections: View {
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = SettingsStore.shared.dynamicThumbsticks

    var body: some View {
        if dynamicSettings.dynamicThumbsticks {
            thumbstickActionButtonsSection(
                title: settings.localized("Action Buttons Left Thumbstick"),
                toggleTitle: settings.localized("Dynamic Actions in Left Thumbstick"),
                isEnabled: $dynamicSettings.leftThumbstickActionsEnabled,
                aim: $dynamicSettings.leftAimButton,
                fire: $dynamicSettings.leftFireButton,
                holdFire: $dynamicSettings.leftHoldFireButton
            )
        }

        if dynamicSettings.swipeCamera || dynamicSettings.dynamicThumbsticks {
            thumbstickActionButtonsSection(
                title: settings.localized("Action Buttons Right Thumbstick"),
                toggleTitle: settings.localized("Dynamic Actions in Right Thumbstick"),
                isEnabled: $dynamicSettings.rightThumbstickActionsEnabled,
                aim: $dynamicSettings.rightAimButton,
                fire: $dynamicSettings.rightFireButton,
                holdFire: $dynamicSettings.rightHoldFireButton
            )
        }

        Section {
            controllerToggle(
                settings.localized("Dynamic Aiming Crosshair"),
                isOn: $dynamicSettings.dynamicCrosshairEnabled
            )
            if dynamicSettings.dynamicCrosshairEnabled {
                controllerToggle(
                    settings.localized("Show Crosshair While Holding Swipe"),
                    isOn: $dynamicSettings.showCrosshairWhileHoldingSwipe
                )
                .disabled(!dynamicSettings.swipeCamera)

                if dynamicSettings.showCrosshairWhileHoldingSwipe {
                    DynamicControlSlider(
                        title: "Crosshair Hide Delay",
                        value: $dynamicSettings.swipeCrosshairHideDelay,
                        range: 0...3,
                        step: 0.1,
                        format: .seconds
                    )
                    .disabled(!dynamicSettings.swipeCamera)
                }

                DynamicControlSlider(
                    title: "Crosshair Size",
                    value: $dynamicSettings.dynamicCrosshairSize,
                    range: 12...120,
                    step: 1,
                    format: .points
                )
                DynamicControlSlider(
                    title: "Crosshair Opacity",
                    value: $dynamicSettings.dynamicCrosshairOpacity,
                    range: 0.10...1,
                    step: 0.05,
                    format: .unitPercent
                )
                controllerPicker(
                    settings.localized("Crosshair Type"),
                    selection: $dynamicSettings.dynamicCrosshairType,
                    options: DynamicCrosshairType.allCases.map { ($0, settings.localized($0.title)) }
                )
                controllerPicker(
                    settings.localized("Crosshair Animation"),
                    selection: $dynamicSettings.dynamicCrosshairAnimation,
                    options: DynamicCrosshairAnimation.allCases.map { ($0, settings.localized($0.title)) }
                )
            }
        } header: {
            Text(settings.localized("Dynamic Crosshair"))
        } footer: {
            Text(settings.localized("The crosshair appears while Aim Mode is active and can remain visible while holding Swipe Camera. The hide delay controls how long it stays after the swipe ends. Every animation follows live swipe, thumbstick, and gyroscope direction and speed, then reacts separately to single shots and automatic fire."))
        }

        if dynamicSettings.dynamicThumbsticks {
            Section {
                DynamicControlSlider(
                    title: "Maximum Radius",
                    value: $dynamicSettings.thumbstickRadius,
                    range: 40...60,
                    step: 1,
                    format: .points
                )
                DynamicControlSlider(
                    title: "Dead Zone",
                    value: $dynamicSettings.deadZone,
                    range: 0...0.25,
                    step: 0.01,
                    format: .unitPercent
                )
                DynamicControlSlider(
                    title: "Thumbstick Opacity",
                    value: $dynamicSettings.thumbstickOpacity,
                    range: 0...1,
                    step: 0.01,
                    format: .unitPercent
                )
                DynamicControlSlider(
                    title: "Base Opacity",
                    value: $dynamicSettings.baseOpacity,
                    range: 0...1,
                    step: 0.01,
                    format: .unitPercent
                )
                DynamicControlSlider(
                    title: "Trail Opacity",
                    value: $dynamicSettings.trailOpacity,
                    range: 0...1,
                    step: 0.01,
                    format: .unitPercent
                )
                controllerToggle(settings.localized("Activation Haptics"), isOn: $dynamicSettings.activationHaptics)
            } header: {
                Text(settings.localized("Dynamic Thumbstick Feel"))
            } footer: {
                Text(settings.localized("The compact base stays at the initial touch point. Dead zone begins at 0% and progressively reaches the selected value as the stick moves outward. Analog output saturates at the selected radius while the nub and seven-dot trail continue following overdrag."))
            }
        }

        if dynamicSettings.gyroscopeCamera {
            Section {
                DynamicControlSlider(
                    title: "Gyro Sensitivity",
                    value: $dynamicSettings.gyroSensitivity,
                    range: 0.5...4.0,
                    step: 0.1,
                    format: .multiplier.decimals(1)
                )
                DynamicControlSlider(
                    title: "Gyro Acceleration",
                    value: $dynamicSettings.gyroAcceleration,
                    range: 0...2,
                    step: 0.05,
                    format: .unitPercent
                )
                DynamicControlSlider(
                    title: "Gyro Smoothing",
                    value: $dynamicSettings.gyroSmoothing,
                    range: 0...0.95,
                    step: 0.05,
                    format: .unitPercent
                )
                DynamicControlSlider(
                    title: "Gyro Dead Zone",
                    value: $dynamicSettings.gyroDeadZone,
                    range: 0...0.25,
                    step: 0.01,
                    format: .radiansPerSecond
                )
                DynamicControlSlider(
                    title: "Maximum Gyro Rate",
                    value: $dynamicSettings.gyroMaximumRate,
                    range: 1...12,
                    step: 0.5,
                    format: .radiansPerSecond.decimals(1)
                )
                controllerToggle(settings.localized("Invert Gyro Horizontal"), isOn: $dynamicSettings.invertGyroHorizontal)
                controllerToggle(settings.localized("Invert Gyro Vertical"), isOn: $dynamicSettings.invertGyroVertical)
            } header: {
                Text(settings.localized("Gyroscope"))
            } footer: {
                Text(settings.localized("Gyroscope input is active only while the virtual controller is on screen. If the sensor is unavailable, the selected touch camera continues working normally."))
            }
        }
    }

    @ViewBuilder
    private func thumbstickActionButtonsSection(
        title: String,
        toggleTitle: String,
        isEnabled: Binding<Bool>,
        aim: Binding<VirtualPadActionButton>,
        fire: Binding<VirtualPadActionButton>,
        holdFire: Binding<VirtualPadActionButton>
    ) -> some View {
        Section {
            controllerToggle(toggleTitle, isOn: isEnabled)
            if isEnabled.wrappedValue {
                let options = VirtualPadActionButton.allCases.map { (id: $0, title: settings.localized($0.title)) }
                controllerPicker(settings.localized("Aim (Hold Thumbstick)"), selection: aim,
                                 options: options, id: toggleTitle + "/aim")
                controllerPicker(settings.localized("Fire (Tap Thumbstick)"), selection: fire,
                                 options: options, id: toggleTitle + "/fire")
                controllerPicker(settings.localized("Hold Fire (Fast Tap Thumbstick)"), selection: holdFire,
                                 options: options, id: toggleTitle + "/holdFire")
            }
        } header: {
            Text(title)
        }
    }
}
