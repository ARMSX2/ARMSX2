// GameLibraryViewOptionsPanel.swift — The library's View Options panel and its slider rows
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryViewOptionsPreview: Equatable {
    let title: String
    let value: String
}

struct GameLibraryViewOptionsPanel: View {
    let controllerInput: MenuControllerInputRouter?
    let orientationTitle: String
    @Binding var cardScale: Double
    @Binding var cardWidthScale: Double
    @Binding var cardHeightScale: Double
    @Binding var libraryVerticalPosition: Double
    @Binding var gapScale: Double
    @Binding var contentPaddingScale: Double
    @Binding var verticalPaddingScale: Double
    @Binding var textSpacingScale: Double
    @Binding var gameNameTextScale: Double
    @Binding var gameInfoTextScale: Double
    @Binding var cardCornerRadius: Double
    @Binding var coverCornerRadius: Double
    @Binding var coverShadowStrength: Double
    @Binding var coverOpacity: Double
    @Binding var bottomNavigationTabBarHeight: Double
    @Binding var bottomNavigationTabBarIconScale: Double
    @Binding var bottomNavigationTabBarLabelScale: Double
    @Binding var bottomNavigationTabBarClearance: Double
    @Binding var bottomNavigationTabBarShowsLabels: Bool
    @Binding var hideGamesScreenTitle: Bool
    @Binding var useGamesLogo: Bool
    let logoAvailable: Bool
    @Binding var hideGameName: Bool
    @Binding var hideGameInfo: Bool
    @Binding var hideFavoriteButton: Bool
    @Binding var hideRegionFlag: Bool
    @Binding var singleLineGameNames: Bool
    @Binding var hideRunningIndicator: Bool
    let onPreviewChanged: (GameLibraryViewOptionsPreview?) -> Void
    let onReset: () -> Void
    let onCancel: () -> Void
    let onApply: () -> Void

    @Environment(\.uiAccentColour) private var accentColour
    @State private var settings = SettingsStore.shared
    @State private var previewEndTask: Task<Void, Never>?
    @State private var showsResetConfirmation = false
    @State private var resetFeedbackVisible = false
    @State private var resetFeedbackTask: Task<Void, Never>?

    private let targetOrder = [
        "library.options.hide-title",
        "library.options.use-logo",
        "library.options.hide-name",
        "library.options.hide-info",
        "library.options.hide-favorite",
        "library.options.hide-region",
        "library.options.single-line",
        "library.options.hide-running",
        "library.options.card-size",
        "library.options.card-width",
        "library.options.card-height",
        "library.options.vertical-position",
        "library.options.spacing",
        "library.options.glass-padding",
        "library.options.glass-padding-height",
        "library.options.content-spacing",
        "library.options.card-corners",
        "library.options.cover-corners",
        "library.options.cover-shadow",
        "library.options.cover-opacity",
        "library.options.game-name-size",
        "library.options.game-info-size",
        "library.options.tab-bar-height",
        "library.options.tab-bar-icon-size",
        "library.options.tab-bar-label-size",
        "library.options.tab-bar-clearance",
        "library.options.tab-bar-labels",
        "library.options.reset",
        "library.options.cancel",
        "library.options.apply",
    ]

    var body: some View {
        ZStack {
            Color.clear
                .glassSurface(
                    clear: false,
                    cornerRadius: 26
                )

            VStack(spacing: 10) {
                HStack(spacing: 12) {
                    Text(settings.localized("Game Library View Options"))
                        .font(.headline)
                        .lineLimit(1)

                    Spacer(minLength: 8)

                    Text(orientationTitle)
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 12)
                        .frame(height: 30)
                        .glassSurface(
                            tint: accentColour.opacity(0.10),
                            clear: false,
                            cornerRadius: 15
                        )
                }
                .padding(.horizontal, 4)

                ScrollView {
                    // Keep this small modal form mounted as one semantic graph.
                    // Recycling the first Visible Content row while seeking back
                    // from Bottom Navigation stranded Up on an offscreen target.
                    VStack(spacing: 10) {
                        glassSection(settings.localized("Visible Content")) {
                        optionToggle(
                            settings.localized("Hide Games Screen Title"),
                            id: "library.options.hide-title",
                            isOn: $hideGamesScreenTitle
                        )
                        optionToggle(
                            settings.localized("Use Logo"),
                            id: "library.options.use-logo",
                            isOn: $useGamesLogo,
                            isEnabled: logoAvailable
                        )
                        optionToggle(
                            settings.localized("Hide Game Name"),
                            id: "library.options.hide-name",
                            isOn: $hideGameName
                        )
                        optionToggle(
                            settings.localized("Hide Game Info"),
                            id: "library.options.hide-info",
                            isOn: $hideGameInfo
                        )
                        optionToggle(
                            settings.localized("Hide Favorite Button"),
                            id: "library.options.hide-favorite",
                            isOn: $hideFavoriteButton
                        )
                        optionToggle(
                            settings.localized("Hide Region Flag"),
                            id: "library.options.hide-region",
                            isOn: $hideRegionFlag
                        )
                        optionToggle(
                            settings.localized("Single-Line Game Names"),
                            id: "library.options.single-line",
                            isOn: $singleLineGameNames
                        )
                        optionToggle(
                            settings.localized("Hide Running Indicator"),
                            id: "library.options.hide-running",
                            isOn: $hideRunningIndicator
                        )
                    }

                        glassSection(settings.localized("Card Layout")) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-size",
                            title: settings.localized("Card Size"),
                            value: $cardScale,
                            range: 0.72...1.32,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-width",
                            title: settings.localized("Card Width"),
                            value: $cardWidthScale,
                            range: 0.70...1.40,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-height",
                            title: settings.localized("Card Height"),
                            value: $cardHeightScale,
                            range: 0.70...1.40,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.vertical-position",
                            title: settings.localized(
                                "Game Library Vertical Position"
                            ),
                            value: $libraryVerticalPosition,
                            range: -160...160,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.spacing",
                            title: settings.localized("Card Spacing"),
                            value: $gapScale,
                            range: 0.65...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.glass-padding",
                            title: settings.localized("Glass Padding Width"),
                            value: $contentPaddingScale,
                            range: 0...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.glass-padding-height",
                            title: settings.localized("Glass Padding Height"),
                            value: $verticalPaddingScale,
                            range: 0...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.content-spacing",
                            title: settings.localized("Content Spacing"),
                            value: $textSpacingScale,
                            range: 0...2,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.card-corners",
                            title: settings.localized("Card Corners"),
                            value: $cardCornerRadius,
                            range: 6...34,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.cover-corners",
                            title: settings.localized("Cover Corners"),
                            value: $coverCornerRadius,
                            range: 0...28,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                    }

                        glassSection(settings.localized("Cover Appearance")) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.cover-shadow",
                            title: settings.localized("Cover Shadow"),
                            value: $coverShadowStrength,
                            range: 0...2,
                            step: 0.02,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.cover-opacity",
                            title: settings.localized("Cover Opacity"),
                            value: $coverOpacity,
                            range: 0.35...1,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                    }

                        glassSection(settings.localized("Text Size")) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.game-name-size",
                            title: settings.localized("Game Name Size"),
                            value: $gameNameTextScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.game-info-size",
                            title: settings.localized("Game Info Size"),
                            value: $gameInfoTextScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                    }

                        glassSection(
                            settings.localized("Bottom Navigation Tab Bar")
                        ) {
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-height",
                            title: settings.localized(
                                "Bottom Navigation Tab Bar Height"
                            ),
                            value: $bottomNavigationTabBarHeight,
                            range: 42...100,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-icon-size",
                            title: settings.localized("Tab Bar Icon Size"),
                            value: $bottomNavigationTabBarIconScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-label-size",
                            title: settings.localized("Tab Bar Label Size"),
                            value: $bottomNavigationTabBarLabelScale,
                            range: 0.75...1.5,
                            step: 0.01,
                            format: { "\(Int(($0 * 100).rounded()))%" },
                            onPreview: updateSliderPreview
                        )
                        ExperimentalLibrarySliderRow(
                            id: "library.options.tab-bar-clearance",
                            title: settings.localized("Tab Bar Bottom Clearance"),
                            value: $bottomNavigationTabBarClearance,
                            range: -32...32,
                            step: 1,
                            format: { "\(Int($0.rounded())) pt" },
                            onPreview: updateSliderPreview
                        )
                        optionToggle(
                            settings.localized("Show Tab Labels"),
                            id: "library.options.tab-bar-labels",
                            isOn: $bottomNavigationTabBarShowsLabels
                        )
                    }
                    }
                    .padding(.vertical, 2)
                }
                .scrollIndicators(.visible)

                actionBar
            }
            .padding(14)
        }
        .contextMenuPanelTextAppearance()
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            isActive: true,
            scopeKey: "game-library.view-options.\(orientationTitle)",
            priority: 420,
            orbStyle: .plain,
            onBack: {
                onApply()
                return true
            },
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            scrollAnimationDuration: 0.16,
            focusTopAlignmentMargin: 72,
            focusBottomAlignmentMargin: 72,
            preferredInitialFocusLabel: targetOrder.first,
            declaredTargetOrder: targetOrder
        )
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 430,
                isEnabled: true,
                searchesNearbyScrollViews: true
            )
            .allowsHitTesting(false)
        }
        .onDisappear {
            previewEndTask?.cancel()
            previewEndTask = nil
            resetFeedbackTask?.cancel()
            resetFeedbackTask = nil
            onPreviewChanged(nil)
        }
        .controllerPrompt(
            settings.localized("Reset Game Library View Options?"),
            isPresented: $showsResetConfirmation,
            message: settings.localized(
                "This restores the current orientation's card layout and visible content options to their defaults."
            ),
            actions: [
                .cancel,
                .init(title: settings.localized("Reset"), isDestructive: true, run: performReset),
            ]
        )
    }

    private func glassSection<Content: View>(
        _ title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(
            clear: false,
            cornerRadius: 18
        )
    }

    private var actionBar: some View {
        HStack(spacing: 10) {
            actionButton(
                settings.localized(
                    resetFeedbackVisible ? "Restored" : "Reset"
                ),
                systemImage: resetFeedbackVisible
                    ? "checkmark.circle.fill"
                    : "arrow.counterclockwise",
                id: "library.options.reset",
                action: { showsResetConfirmation = true }
            )
            actionButton(
                settings.localized("Cancel"),
                systemImage: "xmark",
                id: "library.options.cancel",
                action: onCancel
            )
            actionButton(
                settings.localized("Apply"),
                systemImage: "checkmark",
                id: "library.options.apply",
                isProminent: true,
                action: onApply
            )
        }
    }

    private func actionButton(
        _ title: String,
        systemImage: String,
        id: String,
        isProminent: Bool = false,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
                .frame(maxWidth: .infinity)
                .frame(height: 38)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(isProminent ? accentColour : Color.primary)
        .glassSurface(
            tint: isProminent ? accentColour.opacity(0.16) : nil,
            interactive: true,
            clear: false,
            cornerRadius: 19
        )
        .controllerAccessibilityActionTarget(
            id: id,
            label: title,
            action: action
        )
    }

    private func updateSliderPreview(
        _ title: String,
        _ value: String
    ) {
        previewEndTask?.cancel()
        previewEndTask = nil
        withAnimation(.easeOut(duration: 0.12)) {
            onPreviewChanged(
                GameLibraryViewOptionsPreview(title: title, value: value)
            )
        }

        // Hiding the popup can stop Slider from sending its final editing-ended
        // callback, so every movement rearms one inactivity deadline and the
        // popup returns a second after touch or controller adjustment stops.
        previewEndTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
            withAnimation(.easeOut(duration: 0.18)) {
                onPreviewChanged(nil)
            }
            previewEndTask = nil
        }
    }

    @MainActor
    private func performReset() {
        onReset()
        resetFeedbackTask?.cancel()
        withAnimation(.easeOut(duration: 0.18)) {
            resetFeedbackVisible = true
        }
        announceSettingsResetSuccess(
            settings.localized("Game Library View Options Restored"),
            controllerInput: controllerInput
        )
        resetFeedbackTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(2.4))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                resetFeedbackVisible = false
            }
            resetFeedbackTask = nil
        }
    }

    private func optionToggle(
        _ title: String,
        id: String,
        isOn: Binding<Bool>,
        isEnabled: Bool = true
    ) -> some View {
        let previewingBinding = Binding(
            get: { isOn.wrappedValue },
            set: { newValue in
                guard isOn.wrappedValue != newValue else { return }
                isOn.wrappedValue = newValue
                updateSliderPreview(
                    title,
                    settings.localized(newValue ? "On" : "Off")
                )
            }
        )
        return Toggle(title, isOn: previewingBinding)
            .controllerAccessibilityToggleTarget(
                id: id,
                label: title,
                isOn: previewingBinding
            )
            .disabled(!isEnabled)
    }
}

struct ExperimentalLibrarySliderRow: View {
    let id: String
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let format: (Double) -> String
    let onPreview: (_ title: String, _ value: String) -> Void

    @State private var isTouchEditing = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title)
                Spacer()
                Text(format(value))
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(
                value: previewingValue,
                in: range,
                step: step,
                onEditingChanged: touchEditingChanged
            )
        }
        .controllerAccessibilityAdjustableTarget(
            id: id,
            label: title,
            value: format(value),
            onActivate: {},
            onIncrement: { adjust(by: step) },
            onDecrement: { adjust(by: -step) }
        )
    }

    private func adjust(by delta: Double) {
        let next = min(max(value + delta, range.lowerBound), range.upperBound)
        guard next != value else { return }
        value = next
        onPreview(title, format(next))
    }

    private var previewingValue: Binding<Double> {
        Binding(
            get: { value },
            set: { next in
                value = next
                guard isTouchEditing else { return }
                onPreview(title, format(next))
            }
        )
    }

    private func touchEditingChanged(_ editing: Bool) {
        isTouchEditing = editing
        onPreview(title, format(value))
    }
}
