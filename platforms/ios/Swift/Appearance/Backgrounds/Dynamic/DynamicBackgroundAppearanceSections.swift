// DynamicBackgroundAppearanceSections.swift — dynamic background appearance sections
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

struct DynamicBackgroundAppearanceSections: View {
    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared
    @Binding var preferences: DynamicAppearancePreferences
    let showPaletteEditor: () -> Void

    var body: some View {
        Section {
            // Like the other appearance controls, this marks the choice as the player's,
            // so the automatic no-JIT theme leaves it alone.
            Toggle(isOn: Binding(
                get: { settings.dynamicBackgroundsEnabled },
                set: { enabled in
                    settings.beginCustomThemeEditing()
                    settings.dynamicBackgroundsEnabled = enabled
                }
            )) {
                Label(
                    settings.localized("Use Dynamic Background"),
                    systemImage: settings.dynamicBackgroundsEnabled ? "waveform.path" : "waveform.path.badge.minus"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.dynamic-background"
            )

            Toggle(
                settings.localized(
                    "Use Original Full Quality Live Wallpapers"
                ),
                isOn: $frameRates.usesOriginalFullQualityLiveWallpapers
            )
            .appearanceControllerListRow(
                "settings.appearance.original-full-quality-live-wallpapers"
            )

            Toggle(
                settings.localized(
                    "Use Lightweight Quality On Hot Temperature"
                ),
                isOn: $frameRates.usesLightweightQualityOnHotTemperature
            )
            .appearanceControllerListRow(
                "settings.appearance.lightweight-quality-hot-temperature"
            )
            .disabled(!frameRates.usesOriginalFullQualityLiveWallpapers)
            .opacity(
                frameRates.usesOriginalFullQualityLiveWallpapers ? 1 : 0.5
            )

            Picker(
                settings.localized("Live Wallpaper Resolution"),
                selection: $frameRates.dynamicWallpaperResolution
            ) {
                ForEach(DynamicWallpaperResolution.allCases) { resolution in
                    Text(
                        settings.localized(
                            frameRates.wallpaperResolutionTitle(resolution)
                        )
                    )
                    .tag(resolution)
                }
            }
            .pickerStyle(.menu)
            .controllerAccessibilityOptionsPickerTarget(
                id: nil,
                label: settings.localized("Live Wallpaper Resolution"),
                selection: $frameRates.dynamicWallpaperResolution,
                options: DynamicWallpaperResolution.allCases.map {
                    (
                        id: $0,
                        title: settings.localized(
                            frameRates.wallpaperResolutionTitle($0)
                        )
                    )
                }
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-wallpaper-resolution"
            )
            .disabled(frameRates.usesOriginalFullQualityLiveWallpapers)
            .opacity(
                frameRates.usesOriginalFullQualityLiveWallpapers ? 0.5 : 1
            )

            Picker(
                settings.localized("Face Button Resolution"),
                selection: $frameRates.dynamicFaceButtonResolution
            ) {
                ForEach(DynamicWallpaperResolution.allCases) { resolution in
                    Text(
                        settings.localized(
                            frameRates.wallpaperResolutionTitle(resolution)
                        )
                    )
                    .tag(resolution)
                }
            }
            .pickerStyle(.menu)
            .controllerAccessibilityOptionsPickerTarget(
                id: nil,
                label: settings.localized("Face Button Resolution"),
                selection: $frameRates.dynamicFaceButtonResolution,
                options: DynamicWallpaperResolution.allCases.map {
                    (
                        id: $0,
                        title: settings.localized(
                            frameRates.wallpaperResolutionTitle($0)
                        )
                    )
                }
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-face-button-resolution"
            )
            .disabled(frameRates.usesOriginalFullQualityLiveWallpapers)
            .opacity(
                frameRates.usesOriginalFullQualityLiveWallpapers ? 0.5 : 1
            )

            DynamicBackgroundDensityRow(
                title: settings.localized("Face Button Amount Percentage"),
                value: $frameRates.dynamicFaceButtonAmount,
                range: UIFrameRateSettings.dynamicFaceButtonAmountRange,
                systemImage: "gamecontroller.fill"
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-face-button-amount"
            )
            .disabled(frameRates.usesOriginalFullQualityLiveWallpapers)
            .opacity(
                frameRates.usesOriginalFullQualityLiveWallpapers ? 0.5 : 1
            )

            DynamicBackgroundDensityRow(
                title: settings.localized("Particle Amount"),
                value: $frameRates.dynamicParticleAmount,
                range: UIFrameRateSettings.dynamicParticleAmountRange,
                systemImage: "sparkles"
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-particle-amount"
            )
            .disabled(frameRates.usesOriginalFullQualityLiveWallpapers)
            .opacity(
                frameRates.usesOriginalFullQualityLiveWallpapers ? 0.5 : 1
            )

            HStack(spacing: 8) {
                Picker(
                    settings.localized("Background Style"),
                    selection: styleBinding
                ) {
                    ForEach(DynamicBackgroundStyle.allCases) { style in
                        Label(
                            settings.localized(style.title),
                            systemImage: style.systemImage
                        )
                        .tag(style)
                    }
                }
                .pickerStyle(.menu)
                .buttonStyle(.plain)
                .frame(maxWidth: .infinity, alignment: .leading)

                SettingsValueStepButtons(
                    previousAccessibilityLabel: settings.localized(
                        "Previous Background Style"
                    ),
                    nextAccessibilityLabel: settings.localized(
                        "Next Background Style"
                    ),
                    canSelectPrevious: canSelectPreviousStyle,
                    canSelectNext: canSelectNextStyle,
                    selectPrevious: { moveStyle(by: -1) },
                    selectNext: { moveStyle(by: 1) }
                )
            }
            .controllerAccessibilityOptionsPickerTarget(
                id: nil,
                label: settings.localized("Background Style"),
                selection: styleBinding,
                options: DynamicBackgroundStyle.allCases.map {
                    (id: $0, title: settings.localized($0.title))
                }
            )
            .appearanceControllerListRow(
                "settings.appearance.background-style"
            )
        } header: {
            Text(settings.localized("Dynamic Backgrounds"))
        } footer: {
            if frameRates.usesOriginalFullQualityLiveWallpapers {
                Text(
                    settings.localized(
                        frameRates.usesLightweightQualityOnHotTemperature
                            ? "Original Full Quality uses native resolution, full geometry and effect density. During serious or critical thermal pressure, the main screen temporarily uses the lightweight resolution and effect settings until the device cools."
                            : "Original Full Quality uses native resolution, full geometry and effect density, and the original presentation cadence. Performance limiters are temporarily disabled while this mode is active."
                    )
                )
            } else {
                Text(settings.localized("Live Wallpaper Resolution changes backing resolution and structural detail without changing full-screen framing. Particle Amount and Face Button Amount independently control animated object counts."))
            }
        }

        Section {
            Button(action: showPaletteEditor) {
                HStack {
                    Label(settings.localized("Colours & Effects"), systemImage: "paintpalette.fill")
                    Spacer()
                    Text(paletteSummary)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Image(systemName: "chevron.right")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.tertiary)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .controllerAccessibilityActionTarget(
                id: nil,
                label: settings.localized("Colours & Effects"),
                action: showPaletteEditor
            )
            .appearanceControllerListRow(
                "settings.appearance.colours-effects"
            )
        } footer: {
            Text(settings.localized("Personalize colours, ribbons, particles, motion, and each background's advanced controls."))
        }
    }

    private var styleBinding: Binding<DynamicBackgroundStyle> {
        Binding(
            get: { preferences.dynamicBackground },
            set: { style in
                selectStyle(style)
            }
        )
    }

    private var paletteSummary: String {
        return "\(preferences.sharedPalette.title) · \(preferences.ribbonPalette.title)"
    }

    private var selectedStyleIndex: Int? {
        DynamicBackgroundStyle.allCases.firstIndex(
            of: preferences.dynamicBackground
        )
    }

    private var canSelectPreviousStyle: Bool {
        guard let selectedStyleIndex else { return false }
        return selectedStyleIndex > DynamicBackgroundStyle.allCases.startIndex
    }

    private var canSelectNextStyle: Bool {
        guard let selectedStyleIndex else { return false }
        return selectedStyleIndex
            < DynamicBackgroundStyle.allCases.index(
                before: DynamicBackgroundStyle.allCases.endIndex
            )
    }

    private func moveStyle(by offset: Int) {
        guard let selectedStyleIndex else { return }
        let styles = DynamicBackgroundStyle.allCases
        let nextIndex = min(
            max(selectedStyleIndex + offset, styles.startIndex),
            styles.index(before: styles.endIndex)
        )
        guard nextIndex != selectedStyleIndex else { return }
        selectStyle(styles[nextIndex])
    }

    private func selectStyle(_ style: DynamicBackgroundStyle) {
        guard style != preferences.dynamicBackground else { return }
        settings.beginCustomThemeEditing()

        if style == .playStation3XMBByMart,
           !preferences.hasSelectedPlayStation3XMBByMart {
            if !preferences.isPlayStation3XMBPresetExplicit {
                preferences.particleSettings.playStation3XMB.gradientPreset = .theme
            }
            preferences.hasSelectedPlayStation3XMBByMart = true
        }

        preferences.dynamicBackground = style
        settings.dynamicAppearancePreferences = preferences
        UISelectionFeedbackGenerator().selectionChanged()
    }

}

private struct DynamicBackgroundDensityRow: View {
    let title: String
    @Binding var value: Double
    var range = UIFrameRateSettings.dynamicParticleAmountRange
    var systemImage = "circle.grid.3x3.fill"

    private var displayedValue: String {
        "\(Int((value * 100).rounded()))%"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Label(title, systemImage: systemImage)
                Spacer(minLength: 12)
                Text(displayedValue)
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            Slider(
                value: $value,
                in: range,
                step: 0.05
            )
        }
        .contentShape(Rectangle())
        .controllerAccessibilityAdjustableTarget(
            id: nil,
            label: title,
            value: displayedValue,
            onActivate: increment,
            onIncrement: increment,
            onDecrement: decrement
        )
    }

    private func increment() {
        value = min(
            range.upperBound,
            value + 0.05
        )
    }

    private func decrement() {
        value = max(
            range.lowerBound,
            value - 0.05
        )
    }
}
