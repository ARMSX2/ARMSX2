// AppearanceInterfaceSections.swift — interface glass, refresh rate and navigation mode
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct AppearanceInterfaceSections: View, AppearanceSettingsEditing {
    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared

    var body: some View {
        Section {
            Toggle(isOn: manualBinding(for: \.appearance.clearLiquidGlassUI)) {
                Label(settings.localized("Clear Liquid Glass UI"), systemImage: "rectangle.on.rectangle")
            }
            .appearanceControllerListRow(
                "settings.appearance.clear-liquid-glass"
            )
            Toggle(
                isOn: manualBinding(
                    for: \.appearance.clearLiquidGlassUISubSettings
                )
            ) {
                Label(
                    settings.localized("Clear Liquid Glass UI Sub-Settings"),
                    systemImage: "rectangle.stack"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.clear-liquid-glass-sub-settings"
            )
            Toggle(
                isOn: manualBinding(for: \.appearance.clearLiquidGlassUIQuickMenu)
            ) {
                Label(settings.localized("Clear Liquid Glass UI Quick Menu"), systemImage: "pause.rectangle")
            }
            .appearanceControllerListRow(
                "settings.appearance.clear-liquid-glass-quick-menu"
            )
            Toggle(
                isOn: manualBinding(
                    for: \.appearance.clearLiquidGlassUIPerGameSettingsLibrary
                )
            ) {
                Label(
                    settings.localized("Clear Liquid Glass UI Per-Game Settings Library"),
                    systemImage: "rectangle.stack"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.clear-liquid-glass-per-game-library"
            )
            Toggle(
                isOn: manualBinding(
                    for: \.appearance.clearLiquidGlassUIPerGameSettingsEmulation
                )
            ) {
                Label(
                    settings.localized("Clear Liquid Glass UI Per-Game Settings Emulation"),
                    systemImage: "gamecontroller"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.clear-liquid-glass-per-game-emulation"
            )
            Toggle(
                isOn: manualBinding(
                    for: \.appearance.gameCardZoomAnimationEnabled
                )
            ) {
                Label(settings.localized("Game-Card Zoom Animation"), systemImage: "rectangle.inset.filled.and.person.filled")
            }
            .appearanceControllerListRow(
                "settings.appearance.game-card-zoom"
            )
            Toggle(
                isOn: manualBinding(
                    for: \.appearance.favoriteGlowingEffectEnabled
                )
            ) {
                Label(
                    settings.localized("Favorite Glowing Effect"),
                    systemImage: "star.circle"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.favorite-glowing-effect"
            )
            Toggle(isOn: Bindable(settings.appearance).gameMenuCoverColoursEnabled) {
                Label(
                    settings.localized("Cover Colours in Game Menus"),
                    systemImage: "paintpalette"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.game-menu-cover-colours"
            )
            Toggle(isOn: manualBinding(for: \.appearance.hideIntroStatusBar)) {
                Label(
                    settings.localized("Hide Status Bar During Intro"),
                    systemImage: "rectangle.topthird.inset.filled"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.hide-intro-status-bar"
            )
            Toggle(isOn: manualBinding(for: \.appearance.hideMenuStatusBar)) {
                Label(
                    settings.localized("Hide Status Bar in Menus"),
                    systemImage: "rectangle.topthird.inset.filled"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.hide-menu-status-bar"
            )
            Toggle(isOn: manualBinding(for: \.appearance.hideGameplayStatusBar)) {
                Label(
                    settings.localized("Hide Status Bar During Gameplay"),
                    systemImage: "rectangle.topthird.inset.filled"
                )
            }
            .appearanceControllerListRow(
                "settings.appearance.hide-gameplay-status-bar"
            )
        } header: {
            Text(settings.localized("Interface"))
        } footer: {
            Text(settings.localized("Choose the clear Liquid Glass style independently for the main interface, sub-settings, Quick Menu, and Per-Game Settings in the library or emulation. Turning off the main interface style also turns it off for sub-settings. Disabled options use the more opaque regular Liquid Glass style. Intro, menu, and gameplay status-bar controls hide the clock, network, and battery indicators."))
        }

        Section {
            Text(
                settings.localized(
                    "60 Hz / 60 FPS are achievable by turning on Low Power Mode."
                )
            )
            .font(.caption)
            .foregroundStyle(.secondary)
            .gameCardTintMenuBackgroundListRow(true)

            AppearanceFrameRateSliderRow(
                title: settings.localized("Display Refresh Rate"),
                systemImage: "display",
                value: $frameRates.displayRefreshRateFramesPerSecond,
                range: UIFrameRateSettings.displayRefreshRateRange,
                step: 30,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond,
                unit: "Hz"
            )
            .appearanceControllerListRow(
                "settings.appearance.display-refresh-rate"
            )

            AppearanceFrameRateSliderRow(
                title: settings.localized("Touch Navigation"),
                systemImage: "hand.tap",
                value: $frameRates.touchNavigationFramesPerSecond,
                range: UIFrameRateSettings.navigationRange,
                step: 30,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond
            )
            .appearanceControllerListRow(
                "settings.appearance.touch-navigation-fps"
            )

            AppearanceFrameRateSliderRow(
                title: settings.localized("Controller Navigation"),
                systemImage: "gamecontroller",
                value: $frameRates.controllerNavigationFramesPerSecond,
                range: UIFrameRateSettings.navigationRange,
                step: 30,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond
            )
            .appearanceControllerListRow(
                "settings.appearance.controller-navigation-fps"
            )

            AppearanceFrameRateSliderRow(
                title: settings.localized("Controller Focus Effects"),
                systemImage: "viewfinder",
                value: $frameRates.controllerEffectsFramesPerSecond,
                range: UIFrameRateSettings.effectRange,
                step: 15,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond
            )
            .appearanceControllerListRow(
                "settings.appearance.controller-effects-fps"
            )

            AppearanceFrameRateSliderRow(
                title: settings.localized("Dynamic Background"),
                systemImage: "waveform.path",
                value: $frameRates.dynamicBackgroundFramesPerSecond,
                range: UIFrameRateSettings.dynamicRange,
                step: 15,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-background-fps"
            )

            AppearanceFrameRateSliderRow(
                title: settings.localized("Background Particles"),
                systemImage: "sparkles",
                value: $frameRates.dynamicParticleFramesPerSecond,
                range: UIFrameRateSettings.effectRange,
                step: 15,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-particles-fps"
            )

            AppearanceFrameRateSliderRow(
                title: settings.localized("Background Face Buttons"),
                systemImage: "circle.grid.cross",
                value: $frameRates.dynamicFaceButtonFramesPerSecond,
                range: UIFrameRateSettings.effectRange,
                step: 15,
                deviceMaximum: frameRates.displayMaximumFramesPerSecond
            )
            .appearanceControllerListRow(
                "settings.appearance.dynamic-face-buttons-fps"
            )
        } header: {
            Label(
                settings.localized("Refresh Rate"),
                systemImage: "gauge.with.dots.needle.67percent"
            )
        } footer: {
            Text(
                settings.localized(
                    "Choose independent frame-rate targets for input presentation and ambient effects. Requested rates are capped by the connected display and may be reduced by iOS for Low Power Mode, accessibility, or thermal conditions."
                )
            )
        }
        .disabled(frameRates.usesOriginalFullQualityLiveWallpapers)
        .opacity(
            frameRates.usesOriginalFullQualityLiveWallpapers ? 0.5 : 1
        )

        Section {
            Toggle(
                settings.localized("Ask Before Controller Navigation"),
                isOn: $frameRates.asksBeforeControllerNavigation
            )
            .appearanceControllerListRow(
                "settings.appearance.ask-controller-navigation"
            )

            Toggle(
                settings.localized("Ask Before Touch Navigation"),
                isOn: $frameRates.asksBeforeTouchNavigation
            )
            .appearanceControllerListRow(
                "settings.appearance.ask-touch-navigation"
            )

            Toggle(
                settings.localized("Change Theme with L2 and R2"),
                isOn: $frameRates.changesThemeWithTriggers
            )
            .appearanceControllerListRow(
                "settings.appearance.theme-triggers"
            )
        } header: {
            Label(
                settings.localized("Navigation Mode"),
                systemImage: "arrow.trianglehead.2.clockwise.rotate.90"
            )
        } footer: {
            Text(
                settings.localized(
                    "Only the active navigation system is mounted. The inactive controller focus graph or touch presentation is released to reduce background work and memory use."
                )
            )
        }
    }
}

private struct AppearanceFrameRateSliderRow: View {
    let title: String
    let systemImage: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let deviceMaximum: Int
    var unit = "FPS"

    private var displayedValue: String {
        let requested = Int(value.rounded())
        if requested > deviceMaximum {
            return "\(requested) \(unit) · \(deviceMaximum) Hz max"
        }
        return "\(requested) \(unit)"
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

            Slider(value: $value, in: range, step: step)
        }
        .contentShape(Rectangle())
        .controllerAccessibilityAdjustableTarget(
            id: nil,
            label: title,
            value: displayedValue,
            onActivate: { increment() },
            onIncrement: { increment() },
            onDecrement: { decrement() }
        )
    }

    private func increment() {
        value = min(range.upperBound, value + step)
    }

    private func decrement() {
        value = max(range.lowerBound, value - step)
    }
}
