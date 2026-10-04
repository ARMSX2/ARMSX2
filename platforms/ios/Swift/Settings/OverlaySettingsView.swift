// OverlaySettingsView.swift — OSD preset selector
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct OverlaySettingsView: View {
    @State private var settings = SettingsStore.shared

    static let controllerTargetOrder = [
        "overlay.preset",
        "overlay.position",
        "overlay.notifications",
        "overlay.show-fps",
        "overlay.show-vps",
        "overlay.show-speed",
        "overlay.show-cpu",
        "overlay.show-gpu",
        "overlay.show-resolution",
        "overlay.show-gs-stats",
        "overlay.show-indicators",
        "overlay.show-settings",
        "overlay.show-inputs",
        "overlay.show-frame-times",
        "overlay.show-version",
        "overlay.show-hardware-info",
        "overlay.show-texture-replacements",
        "overlay.show-device-stats",
    ]

    var body: some View {
        Form {
            Section(settings.localized("Performance Overlay")) {
                Picker(settings.localized("Preset"), selection: $settings.osdPreset) {
                    ForEach(OsdPreset.allCases, id: \.self) { preset in
                        Text(settings.localized(preset.label)).tag(preset)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "overlay.preset",
                    label: settings.localized("Preset"),
                    selection: $settings.osdPreset,
                    options: OsdPreset.allCases.map {
                        (id: $0, title: settings.localized($0.label))
                    }
                )
                .pickerStyle(.segmented)

                Picker(settings.localized("Position"), selection: $settings.osdPerformancePosition) {
                    Text(settings.localized("Hidden")).tag(0)
                    Text(settings.localized("Top Left")).tag(1)
                    Text(settings.localized("Top Right")).tag(3)
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "overlay.position",
                    label: settings.localized("Position"),
                    selection: $settings.osdPerformancePosition,
                    options: [
                        (0, settings.localized("Hidden")),
                        (1, settings.localized("Top Left")),
                        (3, settings.localized("Top Right")),
                    ]
                )
            }

            Section {
                Toggle(settings.localized("On-screen Notifications"), isOn: $settings.osdShowMessages)
                    .controllerAccessibilityToggleTarget(
                        id: "overlay.notifications",
                        label: settings.localized("On-screen Notifications"),
                        isOn: $settings.osdShowMessages
                    )
                Text(settings.localized("Shows transient in-game messages such as shader compilation, save states, and settings-applied notices. Turn off to hide them. Critical errors and alerts are not affected."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Notifications"))
            }

            Section(settings.localized("Displayed Items")) {
                Toggle(settings.localized("Show FPS"), isOn: $settings.osdShowFPS)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-fps", label: settings.localized("Show FPS"), isOn: $settings.osdShowFPS)
                Toggle(settings.localized("Show VPS"), isOn: $settings.osdShowVPS)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-vps", label: settings.localized("Show VPS"), isOn: $settings.osdShowVPS)
                Toggle(settings.localized("Show Speed"), isOn: $settings.osdShowSpeed)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-speed", label: settings.localized("Show Speed"), isOn: $settings.osdShowSpeed)
                Toggle(settings.localized("Show CPU"), isOn: $settings.osdShowCPU)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-cpu", label: settings.localized("Show CPU"), isOn: $settings.osdShowCPU)
                Toggle(settings.localized("Show GPU"), isOn: $settings.osdShowGPU)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-gpu", label: settings.localized("Show GPU"), isOn: $settings.osdShowGPU)
                Toggle(settings.localized("Show Resolution"), isOn: $settings.osdShowResolution)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-resolution", label: settings.localized("Show Resolution"), isOn: $settings.osdShowResolution)
                Toggle(settings.localized("Show GS Stats"), isOn: $settings.osdShowGSStats)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-gs-stats", label: settings.localized("Show GS Stats"), isOn: $settings.osdShowGSStats)
                Toggle(settings.localized("Show Indicators"), isOn: $settings.osdShowIndicators)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-indicators", label: settings.localized("Show Indicators"), isOn: $settings.osdShowIndicators)
                Toggle(settings.localized("Show Settings"), isOn: $settings.osdShowSettings)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-settings", label: settings.localized("Show Settings"), isOn: $settings.osdShowSettings)
                Toggle(settings.localized("Show Inputs"), isOn: $settings.osdShowInputs)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-inputs", label: settings.localized("Show Inputs"), isOn: $settings.osdShowInputs)
                Toggle(settings.localized("Show Frame Times"), isOn: $settings.osdShowFrameTimes)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-frame-times", label: settings.localized("Show Frame Times"), isOn: $settings.osdShowFrameTimes)
                Toggle(settings.localized("Show Version"), isOn: $settings.osdShowVersion)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-version", label: settings.localized("Show Version"), isOn: $settings.osdShowVersion)
                Toggle(settings.localized("Show Hardware Info"), isOn: $settings.osdShowHardwareInfo)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-hardware-info", label: settings.localized("Show Hardware Info"), isOn: $settings.osdShowHardwareInfo)
                Toggle(settings.localized("Show Texture Replacements"), isOn: $settings.osdShowTextureReplacements)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-texture-replacements", label: settings.localized("Show Texture Replacements"), isOn: $settings.osdShowTextureReplacements)
                Toggle(settings.localized("Show Device Stats"), isOn: $settings.osdShowDeviceStats)
                    .controllerAccessibilityToggleTarget(id: "overlay.show-device-stats", label: settings.localized("Show Device Stats"), isOn: $settings.osdShowDeviceStats)
            }

            Section(settings.localized("Notes")) {
                Text(settings.localized("Device Stats adds battery, iOS heat state, Low Power Mode, and emulator RAM usage. iOS does not expose exact CPU/GPU temperatures, so Heat is an OS-reported warning level rather than a thermometer reading."))
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Overlay"))
        .navigationBarTitleDisplayMode(.inline)
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
    }
}
