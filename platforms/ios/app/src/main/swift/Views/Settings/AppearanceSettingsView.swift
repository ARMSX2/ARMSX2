// AppearanceSettingsView.swift — Library background customization
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

struct AppearanceSettingsView: View {
    private enum PresentedEditor: String, Identifiable {
        case colours
        case gallery

        var id: String { rawValue }
    }

    private enum PresentedControllerColour: String, Identifiable {
        case accent
        case focusBox
        case text
        case focusedText
        case textShadow
        case focusedTextShadow
        case secondary = "secondary-text-colour"
        case critical = "critical-text-colour"
        case tabTitle = "tab-titles-colour"
        case tabSubtitle = "tab-subtitles-colour"
        case bottomTab = "bottom-tab-bar-colour"
        case bottomTabUnselected = "bottom-tab-bar-unselected-colour"
        case cardTitle = "card-titles-colour"
        case contextMenu = "context-menu-colour"
        case importActions = "import-actions-colour"
        case toolbar = "toolbar-icons-colour"
        case orbs = "focus-orb-colours"

        var id: String { rawValue }
    }

    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared
    @State private var themeGallery = ThemeGalleryStore.shared
    @State private var dynamicPreferences = SettingsStore.shared.dynamicAppearancePreferences
    @State private var paletteTarget: ThemePaletteTarget = .shared
    @State private var presentedEditor: PresentedEditor?
    @State private var presentedControllerColour: PresentedControllerColour?
    @State private var showsThemeNameKeyboard = false
    @State private var themeSaveError = false
    @State private var themeShareItem: ShareSheetItem?
    @State private var showsThemeImporter = false
    @State private var themeTransferMessage = ""
    @State private var showsThemeTransferResult = false
    @State private var isShowingBackgroundOnly = false
    @State private var showPrimaryPicker = false
    @State private var showLandscapePicker = false
    @Environment(\.controllerAccessibilityNavigationActive)
    private var controllerNavigationActive
    @Environment(\.menuControllerInputRouter)
    private var controllerInput

    static let controllerTargetOrder = [
        "settings.appearance.primary-background",
        "settings.appearance.landscape-background",
        "settings.appearance.portrait-fit-mode",
        "settings.appearance.landscape-fit-mode",
        "settings.appearance.mute-video",
        "settings.appearance.background-dim",
        "settings.appearance.show-in-bios",
        "settings.appearance.show-in-settings",
        "settings.appearance.clear-liquid-glass",
        "settings.appearance.clear-liquid-glass-sub-settings",
        "settings.appearance.clear-liquid-glass-quick-menu",
        "settings.appearance.clear-liquid-glass-per-game-library",
        "settings.appearance.clear-liquid-glass-per-game-emulation",
        "settings.appearance.game-card-zoom",
        "settings.appearance.favorite-glowing-effect",
        "settings.appearance.hide-intro-status-bar",
        "settings.appearance.hide-menu-status-bar",
        "settings.appearance.hide-gameplay-status-bar",
        "settings.appearance.display-refresh-rate",
        "settings.appearance.touch-navigation-fps",
        "settings.appearance.controller-navigation-fps",
        "settings.appearance.controller-effects-fps",
        "settings.appearance.dynamic-background-fps",
        "settings.appearance.dynamic-particles-fps",
        "settings.appearance.dynamic-face-buttons-fps",
        "settings.appearance.ask-controller-navigation",
        "settings.appearance.ask-touch-navigation",
        "settings.appearance.dynamic-background",
        "settings.appearance.original-full-quality-live-wallpapers",
        "settings.appearance.lightweight-quality-hot-temperature",
        "settings.appearance.dynamic-wallpaper-resolution",
        "settings.appearance.dynamic-face-button-resolution",
        "settings.appearance.dynamic-face-button-amount",
        "settings.appearance.dynamic-particle-amount",
        "settings.appearance.background-style",
        "settings.appearance.colours-effects",
        "settings.appearance.ui-theme-preset",
        "settings.appearance.theme-gallery",
        "settings.appearance.save-custom-theme",
        "settings.appearance.share-theme",
        "settings.appearance.import-theme",
        "settings.appearance.accent-colour",
        "settings.appearance.accent-colour.custom",
        "settings.appearance.text-colour",
        "settings.appearance.text-colour.custom",
        "settings.appearance.secondary-text-colour",
        "settings.appearance.secondary-text-colour.custom",
        "settings.appearance.critical-text-colour",
        "settings.appearance.critical-text-colour.custom",
        "settings.appearance.tab-titles-colour",
        "settings.appearance.tab-titles-colour.custom",
        "settings.appearance.tab-subtitles-colour",
        "settings.appearance.tab-subtitles-colour.custom",
        "settings.appearance.bottom-tab-bar-colour",
        "settings.appearance.bottom-tab-bar-colour.custom",
        "settings.appearance.bottom-tab-bar-unselected-colour",
        "settings.appearance.bottom-tab-bar-unselected-colour.custom",
        "settings.appearance.card-titles-colour",
        "settings.appearance.card-titles-colour.custom",
        "settings.appearance.context-menu-colour",
        "settings.appearance.context-menu-colour.custom",
        "settings.appearance.import-actions-colour",
        "settings.appearance.import-actions-colour.custom",
        "settings.appearance.toolbar-icons-colour",
        "settings.appearance.toolbar-icons-colour.custom",
        "settings.appearance.text-shadow-strength",
        "settings.appearance.text-shadow-colour",
        "settings.appearance.text-shadow-colour.custom",
        "settings.appearance.focused-text-colour",
        "settings.appearance.focused-text-colour.custom",
        "settings.appearance.focused-text-shadow-strength",
        "settings.appearance.focused-text-shadow-colour",
        "settings.appearance.focused-text-shadow-colour.custom",
        "settings.appearance.controller-navigation-depth-effect",
        "settings.appearance.focus-box-style",
        "settings.appearance.navigation-focus-animation",
        "settings.appearance.navigation-focus-animation-preview",
        "settings.appearance.focus-box-colours",
        "settings.appearance.focus-box-colours.custom",
        "settings.appearance.focus-box-animation-speed",
        "settings.appearance.focus-box-glow",
        "settings.appearance.focus-orbs",
        "settings.appearance.focus-orb-colours",
        "settings.appearance.focus-orb-colours.custom",
        "settings.appearance.reset",
    ]

    var body: some View {
        Form {
            Section {
                BackgroundAssetRow(
                    title: settings.localized("Primary Background"),
                    asset: settings.backgroundPrimaryAsset,
                    glyph: "rectangle.portrait",
                    caption: settings.localized("Shown in portrait and anywhere no landscape background is set.")
                ) { showPrimaryPicker = true }
                .modifier(BackgroundSourcePicker(isPresented: $showPrimaryPicker, role: .primary, existingAsset: { settings.backgroundPrimaryAsset }) { updatePrimary($0) })
                .appearanceControllerListRow(
                    "settings.appearance.primary-background"
                )

                BackgroundAssetRow(
                    title: settings.localized("Landscape Background"),
                    asset: settings.backgroundLandscapeAsset,
                    glyph: "rectangle",
                    caption: settings.localized("Optional. Used only when the device is held in landscape.")
                ) { showLandscapePicker = true }
                .modifier(BackgroundSourcePicker(isPresented: $showLandscapePicker, role: .landscape, existingAsset: { settings.backgroundLandscapeAsset }) { updateLandscape($0) })
                .appearanceControllerListRow(
                    "settings.appearance.landscape-background"
                )
            } header: {
                Text(settings.localized("Background"))
            } footer: {
                Text(settings.localized("Each orientation keeps its own background. Setting one never overwrites the other."))
            }

            Section {
                Picker(selection: manualBinding(for: \.backgroundFitMode)) {
                    ForEach(BackgroundFitMode.allCases) { mode in
                        Text(label(for: mode)).tag(mode)
                    }
                } label: {
                    Label(settings.localized("Portrait Fit Mode"), systemImage: "rectangle.portrait")
                }
                .buttonStyle(.plain)
                .controllerAccessibilityOptionsPickerTarget(
                    id: nil,
                    label: settings.localized("Portrait Fit Mode"),
                    selection: manualBinding(for: \.backgroundFitMode),
                    options: BackgroundFitMode.allCases.map {
                        (id: $0, title: label(for: $0))
                    }
                )
                .onChange(of: settings.backgroundFitMode) { _, _ in
                    UISelectionFeedbackGenerator().selectionChanged()
                }
                .appearanceControllerListRow(
                    "settings.appearance.portrait-fit-mode"
                )

                portraitFitModeHint
                    .gameCardTintMenuBackgroundListRow(true)

                Picker(
                    selection: manualBinding(
                        for: \.backgroundLandscapeFitMode
                    )
                ) {
                    ForEach(BackgroundFitMode.allCases) { mode in
                        Text(label(for: mode)).tag(mode)
                    }
                } label: {
                    Label(settings.localized("Landscape Fit Mode"), systemImage: "rectangle")
                }
                .buttonStyle(.plain)
                .controllerAccessibilityOptionsPickerTarget(
                    id: nil,
                    label: settings.localized("Landscape Fit Mode"),
                    selection: manualBinding(
                        for: \.backgroundLandscapeFitMode
                    ),
                    options: BackgroundFitMode.allCases.map {
                        (id: $0, title: label(for: $0))
                    }
                )
                .onChange(of: settings.backgroundLandscapeFitMode) { _, _ in
                    UISelectionFeedbackGenerator().selectionChanged()
                }
                .appearanceControllerListRow(
                    "settings.appearance.landscape-fit-mode"
                )

                landscapeFitModeHint
                    .gameCardTintMenuBackgroundListRow(true)
            } header: {
                Text(settings.localized("Fit Mode"))
            } footer: {
                Text(settings.localized("Each orientation uses its own fit mode."))
            }

            Section {
                Toggle(isOn: manualBinding(for: \.backgroundVideoMuted)) {
                    Label(settings.localized("Mute Video"), systemImage: settings.backgroundVideoMuted ? "speaker.slash" : "speaker.wave.2")
                }
                .appearanceControllerListRow(
                    "settings.appearance.mute-video"
                )

                NumberRow(
                    .backgroundDim,
                    value: manualBinding(for: \.backgroundDim),
                    settings: settings
                )
                    .appearanceControllerListRow(
                        "settings.appearance.background-dim"
                    )
            }

            Section {
                Toggle(isOn: manualBinding(for: \.backgroundEnabledInBIOS)) {
                    Label(settings.localized("BIOS"), systemImage: "cpu")
                }
                .appearanceControllerListRow(
                    "settings.appearance.show-in-bios"
                )
                Toggle(
                    isOn: manualBinding(for: \.backgroundEnabledInSettings)
                ) {
                    Label(settings.localized("Settings"), systemImage: "gearshape")
                }
                .appearanceControllerListRow(
                    "settings.appearance.show-in-settings"
                )
            } header: {
                Text(settings.localized("Show Background In"))
            } footer: {
                Text(settings.localized("The background always shows behind Games. BIOS and Settings can be toggled independently. Dim or mute from the settings above."))
            }

            Section {
                Toggle(isOn: manualBinding(for: \.clearLiquidGlassUI)) {
                    Label(settings.localized("Clear Liquid Glass UI"), systemImage: "rectangle.on.rectangle")
                }
                .appearanceControllerListRow(
                    "settings.appearance.clear-liquid-glass"
                )
                Toggle(
                    isOn: manualBinding(
                        for: \.clearLiquidGlassUISubSettings
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
                    isOn: manualBinding(for: \.clearLiquidGlassUIQuickMenu)
                ) {
                    Label(settings.localized("Clear Liquid Glass UI Quick Menu"), systemImage: "pause.rectangle")
                }
                .appearanceControllerListRow(
                    "settings.appearance.clear-liquid-glass-quick-menu"
                )
                Toggle(
                    isOn: manualBinding(
                        for: \.clearLiquidGlassUIPerGameSettingsLibrary
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
                        for: \.clearLiquidGlassUIPerGameSettingsEmulation
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
                        for: \.gameCardZoomAnimationEnabled
                    )
                ) {
                    Label(settings.localized("Game-Card Zoom Animation"), systemImage: "rectangle.inset.filled.and.person.filled")
                }
                .appearanceControllerListRow(
                    "settings.appearance.game-card-zoom"
                )
                Toggle(
                    isOn: manualBinding(
                        for: \.favoriteGlowingEffectEnabled
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
                Toggle(isOn: manualBinding(for: \.hideIntroStatusBar)) {
                    Label(
                        settings.localized("Hide Status Bar During Intro"),
                        systemImage: "rectangle.topthird.inset.filled"
                    )
                }
                .appearanceControllerListRow(
                    "settings.appearance.hide-intro-status-bar"
                )
                Toggle(isOn: manualBinding(for: \.hideMenuStatusBar)) {
                    Label(
                        settings.localized("Hide Status Bar in Menus"),
                        systemImage: "rectangle.topthird.inset.filled"
                    )
                }
                .appearanceControllerListRow(
                    "settings.appearance.hide-menu-status-bar"
                )
                Toggle(isOn: manualBinding(for: \.hideGameplayStatusBar)) {
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

            // Keep background choice adjacent to the presets it participates
            // in, while avoiding a first-section header underneath the large
            // navigation title.
            DynamicBackgroundAppearanceSections(
                preferences: $dynamicPreferences,
                showPaletteEditor: { presentedEditor = .colours }
            )

            Section {
                HStack(spacing: 8) {
                    Picker(selection: themeSelectionBinding) {
                        ForEach(themeSelections) { selection in
                            Text(
                                settings.localized(
                                    themeGallery.title(for: selection)
                                )
                            )
                            .tag(selection)
                        }
                    } label: {
                        Label(
                            settings.localized("Theme Presets"),
                            systemImage: "sparkles.rectangle.stack.fill"
                        )
                    }
                    .pickerStyle(.menu)
                    .buttonStyle(.plain)
                    .frame(maxWidth: .infinity, alignment: .leading)

                    SettingsValueStepButtons(
                        previousAccessibilityLabel: settings.localized(
                            "Previous Theme"
                        ),
                        nextAccessibilityLabel: settings.localized(
                            "Next Theme"
                        ),
                        canSelectPrevious: canSelectPreviousTheme,
                        canSelectNext: canSelectNextTheme,
                        selectPrevious: { moveTheme(by: -1) },
                        selectNext: { moveTheme(by: 1) }
                    )
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: nil,
                    label: settings.localized("Theme Presets"),
                    selection: themeSelectionBinding,
                    options: themeSelections.map {
                        (
                            id: $0,
                            title: settings.localized(
                                themeGallery.title(for: $0)
                            )
                        )
                    }
                )
                .appearanceControllerListRow(
                    "settings.appearance.ui-theme-preset"
                )

                Button {
                    presentedEditor = .gallery
                } label: {
                    Label("Theme Gallery", systemImage: "square.grid.2x2")
                }
                .controllerAccessibilityActionTarget(id: nil, label: "Theme Gallery") {
                    presentedEditor = .gallery
                }
                .appearanceControllerListRow("settings.appearance.theme-gallery")

                Button {
                    showsThemeNameKeyboard = true
                } label: {
                    Label("Save Custom Theme", systemImage: "plus")
                }
                .controllerAccessibilityActionTarget(id: nil, label: "Save Custom Theme") {
                    showsThemeNameKeyboard = true
                }
                .appearanceControllerListRow("settings.appearance.save-custom-theme")

                Button(action: shareCurrentTheme) {
                    Label(
                        settings.localized("Share Theme Preset"),
                        systemImage: "square.and.arrow.up"
                    )
                }
                .controllerAccessibilityActionTarget(
                    id: nil,
                    label: settings.localized("Share Theme Preset"),
                    action: shareCurrentTheme
                )
                .appearanceControllerListRow("settings.appearance.share-theme")

                Button {
                    showsThemeImporter = true
                } label: {
                    Label(
                        settings.localized("Import Theme Preset"),
                        systemImage: "square.and.arrow.down"
                    )
                }
                .controllerAccessibilityActionTarget(
                    id: nil,
                    label: settings.localized("Import Theme Preset")
                ) {
                    showsThemeImporter = true
                }
                .appearanceControllerListRow("settings.appearance.import-theme")
            } header: {
                Text(settings.localized("Theme Options"))
            }

            Section {
                colourPicker(
                    "Accent Colour", id: "accent-colour",
                    selection: accentPaletteBinding, options: paletteOptions,
                    custom: .accent, preview: settings.controllerNavigationAccentColor
                )
            }

            Section {
                colourPicker(
                    "Text Colour", id: "text-colour",
                    selection: textPaletteBinding, options: optionalPaletteOptions,
                    custom: .text, preview: settings.controllerTextAppearance.normalColor ?? .primary
                )

                colourPicker(
                    "Secondary Text Colour", id: "secondary-text-colour",
                    selection: secondaryTextPaletteBinding, options: optionalPaletteOptions,
                    custom: .secondary, preview: settings.controllerTextAppearance.secondaryColor ?? .secondary
                )

                colourPicker(
                    "Critical Text Colour", id: "critical-text-colour",
                    selection: manualBinding(for: \.controllerCriticalTextPalette), options: paletteOptions,
                    custom: .critical, preview: settings.controllerCriticalTextColor
                )

                optionalRolePalettePicker(
                    "Tab Titles Colours",
                    id: "tab-titles-colour",
                    icon: "textformat.size.larger",
                    selection: manualBinding(
                        for: \.controllerTabTitlePalette
                    )
                )

                optionalRolePalettePicker(
                    "Tab Sub Titles Colour",
                    id: "tab-subtitles-colour",
                    icon: "textformat.size.smaller",
                    selection: manualBinding(
                        for: \.controllerTabSubtitlePalette
                    )
                )

                optionalRolePalettePicker(
                    "Bottom Tab Bar Colour",
                    id: "bottom-tab-bar-colour",
                    icon: "rectangle.bottomthird.inset.filled",
                    selection: manualBinding(
                        for: \.controllerBottomTabBarPalette
                    )
                )

                optionalRolePalettePicker(
                    "Bottom Tab Bar Unselected Colour",
                    id: "bottom-tab-bar-unselected-colour",
                    icon: "rectangle.bottomthird.inset.filled",
                    selection: manualBinding(
                        for: \.controllerBottomTabBarUnselectedPalette
                    )
                )

                optionalRolePalettePicker(
                    "Game & BIOS Card Titles Colour",
                    id: "card-titles-colour",
                    icon: "rectangle.stack.fill",
                    selection: manualBinding(
                        for: \.controllerCardTitlePalette
                    )
                )

                optionalRolePalettePicker(
                    "Context Menu Colour",
                    id: "context-menu-colour",
                    icon: "contextualmenu.and.cursorarrow",
                    selection: manualBinding(
                        for: \.controllerContextMenuPalette
                    )
                )

                optionalRolePalettePicker(
                    "Import Actions Colour",
                    id: "import-actions-colour",
                    icon: "square.and.arrow.down.fill",
                    selection: manualBinding(
                        for: \.controllerImportActionPalette
                    )
                )

                optionalRolePalettePicker(
                    "Toolbar Icons Colour",
                    id: "toolbar-icons-colour",
                    icon: "ellipsis.circle.fill",
                    selection: manualBinding(
                        for: \.controllerToolbarPalette
                    )
                )

                NumberRow(
                    settings.localized("Text Shadow Strength"),
                    value: manualBinding(
                        for: \.controllerTextShadowStrength
                    ),
                    in: 0...1,
                    format: .unitPercent,
                    step: 0.05,
                    icon: "textformat",
                    default: 0,
                    settings: settings
                )
                .appearanceControllerListRow(
                    "settings.appearance.text-shadow-strength"
                )

                colourPicker(
                    "Text Shadow Colour", id: "text-shadow-colour",
                    selection: textShadowPaletteBinding, options: paletteOptions,
                    custom: .textShadow, preview: settings.controllerTextAppearance.normalShadowColor
                )
            } header: {
                Label(
                    settings.localized("Text & Icons Colour"),
                    systemImage: "textformat"
                )
            } footer: {
                Text(settings.localized("Set readable colours for labels, supporting text, titles, icons, menus, and their shared shadow."))
            }

            Section {
                colourPicker(
                    "Text Focus Colour", id: "focused-text-colour",
                    selection: focusedTextPaletteBinding, options: paletteOptions,
                    custom: .focusedText, preview: settings.controllerTextAppearance.focusedColor
                )

                NumberRow(
                    settings.localized("Focused Text Shadow Strength"),
                    value: manualBinding(
                        for: \.controllerFocusedTextShadowStrength
                    ),
                    in: 0...1,
                    format: .unitPercent,
                    step: 0.05,
                    icon: "textformat.alt",
                    default: 0.1,
                    settings: settings
                )
                .appearanceControllerListRow(
                    "settings.appearance.focused-text-shadow-strength"
                )

                colourPicker(
                    "Focused Text Shadow Colour", id: "focused-text-shadow-colour",
                    selection: focusedTextShadowPaletteBinding, options: paletteOptions,
                    custom: .focusedTextShadow, preview: settings.controllerTextAppearance.focusedShadowColor
                )

                Toggle(
                    isOn: manualBinding(
                        for: \.controllerNavigationDepthEffectEnabled
                    )
                ) {
                    Label(
                        settings.localized(
                            "Controller Navigation Depth Effect"
                        ),
                        systemImage: "square.3.layers.3d"
                    )
                }
                .appearanceControllerListRow(
                    "settings.appearance.controller-navigation-depth-effect"
                )

                Picker(
                    selection: manualBinding(for: \.controllerFocusBoxStyle)
                ) {
                    ForEach(ControllerFocusBoxStyle.allCases) { style in
                        Text(settings.localized(style.title)).tag(style)
                    }
                } label: {
                    Label(
                        settings.localized("UI Focus Box"),
                        systemImage: "viewfinder"
                    )
                }
                .buttonStyle(.plain)
                .controllerAccessibilityOptionsPickerTarget(
                    id: nil,
                    label: settings.localized("UI Focus Box"),
                    selection: manualBinding(for: \.controllerFocusBoxStyle),
                    options: ControllerFocusBoxStyle.allCases.map {
                        (id: $0, title: settings.localized($0.title))
                    }
                )
                .appearanceControllerListRow(
                    "settings.appearance.focus-box-style"
                )

                Picker(
                    selection: manualBinding(
                        for: \.controllerNavigationFocusAnimation
                    )
                ) {
                    ForEach(ControllerNavigationFocusTravelStyle.allCases) { animation in
                        Text(settings.localized(animation.title)).tag(animation)
                    }
                } label: {
                    Label(
                        settings.localized("Navigation Focus Animation"),
                        systemImage: "arrow.triangle.swap"
                    )
                }
                .buttonStyle(.plain)
                .controllerAccessibilityOptionsPickerTarget(
                    id: nil,
                    label: settings.localized("Navigation Focus Animation"),
                    selection: manualBinding(
                        for: \.controllerNavigationFocusAnimation
                    ),
                    options: ControllerNavigationFocusTravelStyle.allCases.map {
                        (id: $0, title: settings.localized($0.title))
                    }
                )
                .appearanceControllerListRow(
                    "settings.appearance.navigation-focus-animation"
                )

                NavigationFocusAnimationPreview(
                    animation: settings.controllerNavigationFocusAnimation
                )
                .appearanceControllerListRow(
                    "settings.appearance.navigation-focus-animation-preview"
                )

                colourPicker(
                    "UI Focus Box Colours", id: "focus-box-colours",
                    selection: focusBoxPaletteBinding, options: paletteOptions,
                    custom: .focusBox, preview: settings.controllerFocusBoxCustomColor?.color ?? settings.controllerFocusBoxPalette.semanticAccentColor
                )
                NumberRow(
                    settings.localized("Animation Speed"),
                    value: manualBinding(
                        for: \.controllerFocusBoxAnimationSpeed
                    ),
                    in: 0.25...2,
                    format: .multiplier,
                    step: 0.05,
                    icon: "speedometer",
                    default: 1,
                    settings: settings
                )
                .appearanceControllerListRow(
                    "settings.appearance.focus-box-animation-speed"
                )
                .disabled(!settings.controllerFocusBoxStyle.isAnimated)

                NumberRow(
                    settings.localized("Glow Intensity"),
                    value: manualBinding(for: \.controllerFocusBoxGlowIntensity),
                    in: 0...2,
                    format: .unitPercent,
                    step: 0.05,
                    icon: "sun.max",
                    default: 1,
                    settings: settings
                )
                .appearanceControllerListRow(
                    "settings.appearance.focus-box-glow"
                )

                HStack {
                    Spacer(minLength: 24)
                    Text(settings.localized("Focus Preview"))
                        .font(.headline)
                        .padding(.horizontal, 28)
                        .padding(.vertical, 14)
                        .controllerFocusBoxPresentation(
                            isVisible: true,
                            cornerRadius: 12
                        )
                    Spacer(minLength: 24)
                }
                .padding(.vertical, 8)
                .accessibilityHidden(true)
                .gameCardTintMenuBackgroundListRow(true)
            } header: {
                Label(
                    settings.localized("UI Focus Box"),
                    systemImage: "viewfinder"
                )
            } footer: {
                Text(settings.localized("Customize focused text, its shadow, the focus outline, movement animation, speed, glow, and the optional depth layer behind Liquid Glass."))
            }

            Section {
                Toggle(
                    isOn: manualBinding(for: \.focusOrbsEnabled)
                ) {
                    Label(
                        settings.localized("Focus Orbs"),
                        systemImage: "circle.hexagongrid.fill"
                    )
                }
                .appearanceControllerListRow(
                    "settings.appearance.focus-orbs"
                )

                colourPicker(
                    "Orbs Colours", id: "focus-orb-colours",
                    selection: manualBinding(for: \.controllerOrbPalette), options: paletteOptions,
                    custom: .orbs, preview: settings.controllerOrbColors.first ?? .blue
                )
                .disabled(!settings.focusOrbsEnabled)
            } header: {
                Label(
                    settings.localized("Orbs"),
                    systemImage: "circle.hexagongrid.fill"
                )
            } footer: {
                Text(settings.localized("Enable the ambient focus orbs manually and choose their palette independently from the focus box."))
            }

            Section {
                ConfirmedSettingsResetButton(
                    settings.localized("Reset Appearance to Defaults"),
                    confirmationTitle: settings.localized("Reset Appearance?"),
                    confirmationMessage: settings.localized("This restores the stock background, theme, colours, focus effects, and interface presentation. Saved themes are kept."),
                    completionMessage: settings.localized("Defaults Restored")
                ) {
                    resetAppearance()
                }
                .buttonStyle(.automatic)
                .appearanceControllerListRow("settings.appearance.reset")
                .uiCriticalForegroundStyle()
            } footer: {
                Text(settings.localized("Restores the stock background, theme, colours, focus effects, and interface presentation. Saved themes are kept."))
            }
        }
        // Form's automatic menu-button pressed style resolves against the
        // system light colour scheme after a Picker dismisses. Keep the
        // Appearance surface on its semantic theme colours instead of letting
        // the touched row turn black.
        .foregroundStyle(
            settings.controllerTextAppearance.contentColor ?? .white
        )
        .tint(settings.controllerNavigationAccentColor)
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        // Keep these margins on the Form itself so they reliably reach the
        // private UIKit list created by Form.
        .contentMargins(
            .top,
            controllerNavigationActive ? 96 : 0,
            for: .scrollContent
        )
        .contentMargins(
            .bottom,
            controllerNavigationActive ? 192 : 0,
            for: .scrollContent
        )
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 250,
                isEnabled: controllerNavigationActive,
                searchesNearbyScrollViews: true
            )
            .id("settings.appearance-scroll")
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
        .scrollContentBackground(.hidden)
        .background(Color.clear)
        .navigationTitle(settings.localized("Appearance"))
        // Explicit large-title ownership lets NavigationStack calculate the
        // Form's scroll-edge inset instead of placing the first section under
        // the transparent navigation bar.
        .navigationBarTitleDisplayMode(.large)
        .toolbarBackground(.hidden, for: .navigationBar)
        .sheet(item: $presentedEditor, onDismiss: paletteEditorDidDismiss) { editor in
            switch editor {
            case .colours:
                ThemePaletteEditor(
                    target: $paletteTarget, preferences: $dynamicPreferences,
                    isShowingBackgroundOnly: $isShowingBackgroundOnly,
                    dynamicBackground: dynamicPreferences.dynamicBackground,
                    onSaveAppearance: saveDynamicAppearance
                )
                .presentationDetents([.large])
            case .gallery:
                ThemeGalleryView(controllerInput: controllerInput)
                    .presentationDetents([.large])
            }
        }
        .fullScreenCover(isPresented: $showsThemeNameKeyboard) {
            OrbitKeysKeyboardView(
                title: "Save Custom Theme", initialText: "",
                startsInNormalKeyboard: true,
                onCommit: { name in
                    themeSaveError = !ThemeGalleryStore.shared.save(name: name, settings: settings)
                    showsThemeNameKeyboard = false
                },
                onCancel: { showsThemeNameKeyboard = false }
            )
            .presentationBackground(.clear)
        }
        .alert("Theme Not Saved", isPresented: $themeSaveError) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Enter a name for your theme.")
        }
        .sheet(item: $themeShareItem) { item in
            ActivityShareSheet(activityItems: [item.url])
        }
        .sheet(isPresented: $showsThemeImporter) {
            ImportDocumentPicker(
                allowedContentTypes: [.json, .data],
                allowsMultipleSelection: false
            ) { result in
                importTheme(result)
            }
        }
        .alert(
            settings.localized("Theme Preset"),
            isPresented: $showsThemeTransferResult
        ) {
            Button(settings.localized("OK"), role: .cancel) {}
        } message: {
            Text(settings.localized(themeTransferMessage))
        }
        .sheet(item: $presentedControllerColour) { target in
            ControllerCustomColourEditor(
                title: settings.localized(customColourTitle(for: target)),
                initialColor: controllerCustomColour(for: target),
                canRemove: controllerCustomColourExists(for: target),
                onSave: { colour in
                    saveControllerCustomColour(colour, for: target)
                },
                onRemove: {
                    removeControllerCustomColour(for: target)
                }
            )
            .presentationDetents([.medium])
        }
        .onAppear {
            dynamicPreferences = settings.dynamicAppearancePreferences
        }
        .onChange(of: settings.dynamicAppearancePreferences) { _, preferences in
            if presentedEditor == nil { dynamicPreferences = preferences }
        }
        .onDisappear {
            ThemeGalleryStore.shared.preserveCustomTheme(settings)
        }
    }

    private var themeSelections: [AppearanceThemeSelection] {
        themeGallery.orderedSelections
    }

    private func shareCurrentTheme() {
        do {
            themeShareItem = ShareSheetItem(
                url: try themeGallery.exportCurrentTheme(from: settings)
            )
        } catch {
            themeTransferMessage = error.localizedDescription
            showsThemeTransferResult = true
        }
    }

    private func importTheme(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            do {
                let imported = try themeGallery.importTheme(
                    from: url,
                    applyingTo: settings
                )
                dynamicPreferences = settings.dynamicAppearancePreferences
                themeTransferMessage = "Imported theme preset: \(imported.name)"
                showsThemeTransferResult = true
            } catch {
                themeTransferMessage = error.localizedDescription
                showsThemeTransferResult = true
            }
        case .failure(let error):
            if (error as? CocoaError)?.code == .userCancelled { return }
            themeTransferMessage = error.localizedDescription
            showsThemeTransferResult = true
        }
    }

    private var themeSelectionBinding: Binding<AppearanceThemeSelection> {
        Binding(
            get: { themeGallery.currentSelection(for: settings) },
            set: { selection in
                var transaction = Transaction()
                transaction.animation = nil
                withTransaction(transaction) {
                    // A preset changes many semantic colours plus a renderer
                    // configuration. Publish them in one nonanimated render
                    // transaction instead of animating every Form row and
                    // focus probe independently.
                    themeGallery.apply(selection, to: settings)
                    dynamicPreferences = settings.dynamicAppearancePreferences
                }
            }
        )
    }

    private var selectedThemeIndex: Int? {
        themeSelections.firstIndex(of: themeSelectionBinding.wrappedValue)
    }

    private var canSelectPreviousTheme: Bool {
        guard let selectedThemeIndex else { return false }
        return selectedThemeIndex > themeSelections.startIndex
    }

    private var canSelectNextTheme: Bool {
        guard let selectedThemeIndex else { return false }
        return selectedThemeIndex
            < themeSelections.index(
                before: themeSelections.endIndex
            )
    }

    private func moveTheme(by offset: Int) {
        guard let selectedThemeIndex else { return }
        let themes = themeSelections
        let nextIndex = min(
            max(selectedThemeIndex + offset, themes.startIndex),
            themes.index(before: themes.endIndex)
        )
        guard nextIndex != selectedThemeIndex else { return }
        themeSelectionBinding.wrappedValue = themes[nextIndex]
        UISelectionFeedbackGenerator().selectionChanged()
    }

    private var accentPaletteBinding: Binding<ThemePalette> {
        Binding(
            get: { settings.controllerNavigationAccentPalette },
            set: { palette in
                settings.beginCustomThemeEditing()
                settings.controllerNavigationCustomAccentColor = nil
                settings.controllerNavigationAccentPalette = palette
            }
        )
    }

    private var focusBoxPaletteBinding: Binding<ThemePalette> {
        Binding(
            get: { settings.controllerFocusBoxPalette },
            set: { palette in
                settings.beginCustomThemeEditing()
                settings.controllerFocusBoxCustomColor = nil
                settings.controllerFocusBoxPalette = palette
            }
        )
    }

    private var textPaletteBinding: Binding<ThemePalette?> {
        Binding(
            get: { settings.controllerTextPalette },
            set: { palette in
                settings.beginCustomThemeEditing()
                settings.controllerTextCustomColor = nil
                settings.controllerTextPalette = palette
            }
        )
    }

    private var focusedTextPaletteBinding: Binding<ThemePalette> {
        paletteBinding(
            palette: \.controllerFocusedTextPalette,
            custom: \.controllerFocusedTextCustomColor
        )
    }

    private var secondaryTextPaletteBinding: Binding<ThemePalette?> {
        Binding(
            get: { settings.controllerSecondaryTextPalette },
            set: { palette in
                settings.beginCustomThemeEditing()
                settings.controllerRoleCustomColors["secondary-text-colour"] = nil
                settings.controllerSecondaryTextPalette = palette
            }
        )
    }

    private var textShadowPaletteBinding: Binding<ThemePalette> {
        paletteBinding(
            palette: \.controllerTextShadowPalette,
            custom: \.controllerTextShadowCustomColor
        )
    }

    private var focusedTextShadowPaletteBinding: Binding<ThemePalette> {
        paletteBinding(
            palette: \.controllerFocusedTextShadowPalette,
            custom: \.controllerFocusedTextShadowCustomColor
        )
    }

    private var paletteOptions: [(id: ThemePalette, title: String)] {
        AppearancePaletteOptions.palettes(settings.appLanguage)
    }

    private var optionalRolePaletteOptions: [(
        id: ThemePalette?,
        title: String
    )] {
        AppearancePaletteOptions.optionalPalettes(settings.appLanguage, emptyTitle: "Default")
    }

    private var optionalPaletteOptions: [(
        id: ThemePalette?,
        title: String
    )] {
        AppearancePaletteOptions.optionalPalettes(settings.appLanguage, emptyTitle: "Disabled")
    }

    @ViewBuilder
    private func optionalRolePalettePicker(
        _ title: String,
        id: String,
        icon: String,
        selection: Binding<ThemePalette?>
    ) -> some View {
        colourPicker(
            title, id: id, selection: selection,
            options: optionalRolePaletteOptions,
            custom: PresentedControllerColour(rawValue: id),
            preview: rolePreviewColor(for: id)
        )
    }

    private func colourPicker<Value: Hashable>(
        _ title: String, id: String, selection: Binding<Value>,
        options: [(id: Value, title: String)],
        custom: PresentedControllerColour?, preview: Color
    ) -> some View {
        let binding = Binding<Value>(
            get: { selection.wrappedValue },
            set: { value in
                settings.beginCustomThemeEditing()
                settings.controllerRoleCustomColors[id] = nil
                selection.wrappedValue = value
            }
        )
        return AppearanceColourPickerRow(
            title: settings.localized(title), id: "settings.appearance.\(id)",
            selection: binding, options: options, preview: preview,
            addCustom: custom.map { target in
                { presentedControllerColour = target }
            }
        )
        .appearanceControllerListRow("settings.appearance.\(id)")
    }

    private func rolePreviewColor(for id: String) -> Color {
        switch id {
        case "tab-titles-colour":
            return settings.controllerTabTitleColor
        case "tab-subtitles-colour":
            return settings.controllerTabSubtitleColor
        case "bottom-tab-bar-colour":
            return settings.controllerBottomTabBarColor
        case "bottom-tab-bar-unselected-colour":
            return settings.controllerBottomTabBarUnselectedColor
        case "card-titles-colour":
            return settings.controllerCardTitleColor
        case "context-menu-colour":
            return settings.controllerContextMenuColor
        case "import-actions-colour":
            return settings.controllerImportActionColor
        case "toolbar-icons-colour":
            return settings.controllerToolbarColor
        default:
            return settings.controllerTextAppearance.contentColor ?? .primary
        }
    }

    private func paletteBinding(
        palette: ReferenceWritableKeyPath<SettingsStore, ThemePalette>,
        custom: ReferenceWritableKeyPath<
            SettingsStore,
            SavedPaletteColor?
        >
    ) -> Binding<ThemePalette> {
        Binding(
            get: { settings[keyPath: palette] },
            set: { value in
                settings.beginCustomThemeEditing()
                settings[keyPath: custom] = nil
                settings[keyPath: palette] = value
            }
        )
    }

    private func manualBinding<Value>(
        for keyPath: ReferenceWritableKeyPath<SettingsStore, Value>
    ) -> Binding<Value> {
        Binding(
            get: { settings[keyPath: keyPath] },
            set: { value in
                // Keep the preset's readable colours when making a custom
                // variant, instead of switching to unrelated fallbacks.
                settings.beginCustomThemeEditing()
                settings[keyPath: keyPath] = value
            }
        )
    }

    private func controllerCustomColour(
        for target: PresentedControllerColour
    ) -> Color {
        switch target {
        case .accent:
            return settings.controllerNavigationCustomAccentColor?.color
                ?? settings.controllerNavigationAccentPalette.semanticAccentColor
        case .focusBox:
            return settings.controllerFocusBoxCustomColor?.color
                ?? settings.controllerFocusBoxPalette.semanticAccentColor
        case .text:
            return settings.controllerTextAppearance.normalColor
                ?? .primary
        case .focusedText:
            return settings.controllerTextAppearance.focusedColor
        case .textShadow:
            return settings.controllerTextShadowCustomColor?.color
                ?? settings.controllerTextShadowPalette
                    .semanticInterfaceShadowColor
        case .focusedTextShadow:
            return settings.controllerFocusedTextShadowCustomColor?.color
                ?? settings.controllerFocusedTextShadowPalette.semanticInterfaceShadowColor
        default:
            return settings.customRoleColor(target.rawValue) ?? rolePreviewColor(for: target.rawValue)
        }
    }

    private func controllerCustomColourExists(
        for target: PresentedControllerColour
    ) -> Bool {
        switch target {
        case .accent:
            return settings.controllerNavigationCustomAccentColor != nil
        case .focusBox:
            return settings.controllerFocusBoxCustomColor != nil
        case .text:
            return settings.controllerTextCustomColor != nil
        case .focusedText:
            return settings.controllerFocusedTextCustomColor != nil
        case .textShadow:
            return settings.controllerTextShadowCustomColor != nil
        case .focusedTextShadow:
            return settings.controllerFocusedTextShadowCustomColor != nil
        default:
            return settings.controllerRoleCustomColors[target.rawValue] != nil
        }
    }

    private func saveControllerCustomColour(
        _ colour: Color,
        for target: PresentedControllerColour
    ) {
        guard let savedColour = SavedPaletteColor(color: colour) else {
            return
        }
        settings.beginCustomThemeEditing()
        switch target {
        case .accent:
            settings.controllerNavigationCustomAccentColor = savedColour
        case .focusBox:
            settings.controllerFocusBoxCustomColor = savedColour
        case .text:
            settings.controllerTextCustomColor = savedColour
        case .focusedText:
            settings.controllerFocusedTextCustomColor = savedColour
        case .textShadow:
            settings.controllerTextShadowCustomColor = savedColour
        case .focusedTextShadow:
            settings.controllerFocusedTextShadowCustomColor = savedColour
        default:
            settings.controllerRoleCustomColors[target.rawValue] = savedColour
        }
    }

    private func removeControllerCustomColour(
        for target: PresentedControllerColour
    ) {
        settings.beginCustomThemeEditing()
        switch target {
        case .accent:
            settings.controllerNavigationCustomAccentColor = nil
        case .focusBox:
            settings.controllerFocusBoxCustomColor = nil
        case .text:
            settings.controllerTextCustomColor = nil
        case .focusedText:
            settings.controllerFocusedTextCustomColor = nil
        case .textShadow:
            settings.controllerTextShadowCustomColor = nil
        case .focusedTextShadow:
            settings.controllerFocusedTextShadowCustomColor = nil
        default:
            settings.controllerRoleCustomColors[target.rawValue] = nil
        }
    }

    private func customColourTitle(
        for target: PresentedControllerColour
    ) -> String {
        switch target {
        case .accent: return "Custom Accent Colour"
        case .focusBox: return "Custom UI Focus Box Colour"
        case .text: return "Custom Text Colour"
        case .focusedText: return "Custom Text Focus Colour"
        case .textShadow: return "Custom Text Shadow Colour"
        case .focusedTextShadow:
            return "Custom Focused Text Shadow Colour"
        default:
            return "Custom Colour"
        }
    }

    @ViewBuilder
    private var portraitFitModeHint: some View {
        switch settings.backgroundFitMode {
        case .fill:
            HintRow(icon: "arrow.up.left.and.arrow.down.right", text: settings.localized("Fill — covers the whole screen, cropping edges if needed."))
        case .fit:
            HintRow(icon: "rectangle.compress.vertical", text: settings.localized("Fit — shows the whole image with bars on the empty sides."))
        case .stretch:
            HintRow(icon: "arrow.left.and.right", text: settings.localized("Stretch — fills the screen, distorting to match."))
        }
    }

    @ViewBuilder
    private var landscapeFitModeHint: some View {
        switch settings.backgroundLandscapeFitMode {
        case .fill:
            HintRow(icon: "arrow.up.left.and.arrow.down.right", text: settings.localized("Fill — covers the whole screen, cropping edges if needed."))
        case .fit:
            HintRow(icon: "rectangle.compress.vertical", text: settings.localized("Fit — shows the whole image with bars on the empty sides."))
        case .stretch:
            HintRow(icon: "arrow.left.and.right", text: settings.localized("Stretch — fills the screen, distorting to match."))
        }
    }

    private func label(for mode: BackgroundFitMode) -> String {
        switch mode {
        case .fill: return settings.localized("Fill")
        case .fit: return settings.localized("Fit")
        case .stretch: return settings.localized("Stretch")
        }
    }

    private func saveDynamicAppearance() {
        settings.beginCustomThemeEditing()
        settings.dynamicAppearancePreferences = dynamicPreferences
    }

    private func resetAppearance() {
        settings.resetAppearanceDefaults()
        settings.markAppearanceUserModified()
        dynamicPreferences = settings.dynamicAppearancePreferences
    }

    private func paletteEditorDidDismiss() {
        isShowingBackgroundOnly = false
        dynamicPreferences = settings.dynamicAppearancePreferences
    }

    private func updatePrimary(_ asset: BackgroundAsset?) {
        settings.beginCustomThemeEditing()
        if asset == nil { BackgroundStorage.remove(settings.backgroundPrimaryAsset) }
        if asset != nil { settings.dynamicBackgroundsEnabled = false }
        settings.backgroundPrimaryAsset = asset
    }

    private func updateLandscape(_ asset: BackgroundAsset?) {
        settings.beginCustomThemeEditing()
        if asset == nil { BackgroundStorage.remove(settings.backgroundLandscapeAsset) }
        if asset != nil { settings.dynamicBackgroundsEnabled = false }
        settings.backgroundLandscapeAsset = asset
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

/// An on-demand preview, with no repeating timer while Appearance is idle.
/// Uses the same curve renderer as the real focus box.
private struct NavigationFocusAnimationPreview: View {
    let animation: ControllerNavigationFocusTravelStyle
    @State private var isAtDestination = false
    @State private var settings = SettingsStore.shared

    var body: some View {
        Button {
            isAtDestination.toggle()
        } label: {
            VStack(alignment: .leading, spacing: 12) {
                Label(settings.localized("Preview Animation"), systemImage: "play.circle")
                Text(settings.localized(animation.summary))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                GeometryReader { geometry in
                    let width = min(84, geometry.size.width * 0.25)
                    let inset: CGFloat = 12
                    let frame = CGRect(
                        x: isAtDestination ? geometry.size.width - width - inset : inset,
                        y: 12, width: width, height: 36
                    )
                    HStack {
                        Text("A").frame(width: width, height: 36)
                        Spacer()
                        Text("B").frame(width: width, height: 36)
                    }
                    .font(.headline)
                    .padding(inset)
                    ControllerNavigationAnimatedOrbField(
                        targetID: isAtDestination ? "preview.b" : "preview.a",
                        targetFrame: frame,
                        primaryColor: settings.controllerNavigationAccentColor,
                        style: .plain,
                        controllerInput: nil,
                        showsOrbs: false,
                        showsNeonOutline: true,
                        focusTravelStyle: animation,
                        neonCornerRadius: 18,
                        animatesNeonArtwork: false
                    )
                }
                .frame(height: 60)
                .accessibilityHidden(true)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .controllerAccessibilityActionTarget(
            id: nil, label: settings.localized("Preview Animation")
        ) {
            isAtDestination.toggle()
        }
    }
}

private struct ControllerCustomColourEditor: View {
    let title: String
    let canRemove: Bool
    let onSave: (Color) -> Void
    let onRemove: () -> Void

    @State private var colour: Color
    @Environment(\.dismiss) private var dismiss

    init(
        title: String,
        initialColor: Color,
        canRemove: Bool,
        onSave: @escaping (Color) -> Void,
        onRemove: @escaping () -> Void
    ) {
        self.title = title
        self.canRemove = canRemove
        self.onSave = onSave
        self.onRemove = onRemove
        _colour = State(initialValue: initialColor)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    ColorPicker(
                        "Colour",
                        selection: $colour,
                        supportsOpacity: false
                    )

                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .fill(colour)
                        .frame(height: 74)
                        .overlay {
                            RoundedRectangle(
                                cornerRadius: 14,
                                style: .continuous
                            )
                            .stroke(.white.opacity(0.55), lineWidth: 1)
                        }
                        .accessibilityHidden(true)
                } footer: {
                    Text("The custom colour replaces the selected palette until another palette or theme preset is chosen.")
                }

                if canRemove {
                    Section {
                        Button(role: .destructive) {
                            onRemove()
                            dismiss()
                        } label: {
                            Label("Remove Custom Colour", systemImage: "trash")
                        }
                    }
                }
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        onSave(colour)
                        dismiss()
                    }
                    .fontWeight(.semibold)
                }
            }
        }
    }
}

private struct HintRow: View {
    let icon: String
    let text: String

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon).font(.caption).foregroundStyle(.secondary).frame(width: 16)
            Text(text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(.vertical, 2)
    }
}
