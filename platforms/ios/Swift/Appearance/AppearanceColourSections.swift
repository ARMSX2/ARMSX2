// AppearanceColourSections.swift — accent, text, focus box and orb colours
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct AppearanceColourSections: View, AppearanceSettingsEditing {
    typealias PresentedControllerColour = AppearanceSettingsView.PresentedControllerColour
    @State private var settings = SettingsStore.shared
    @Binding var presentedControllerColour: PresentedControllerColour?

    var body: some View {
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
                selection: manualBinding(
                    for: \.controllerTabTitlePalette
                )
            )

            optionalRolePalettePicker(
                "Tab Sub Titles Colour",
                id: "tab-subtitles-colour",
                selection: manualBinding(
                    for: \.controllerTabSubtitlePalette
                )
            )

            optionalRolePalettePicker(
                "Bottom Tab Bar Colour",
                id: "bottom-tab-bar-colour",
                selection: manualBinding(
                    for: \.controllerBottomTabBarPalette
                )
            )

            optionalRolePalettePicker(
                "Bottom Tab Bar Unselected Colour",
                id: "bottom-tab-bar-unselected-colour",
                selection: manualBinding(
                    for: \.controllerBottomTabBarUnselectedPalette
                )
            )

            optionalRolePalettePicker(
                "Game & BIOS Card Titles Colour",
                id: "card-titles-colour",
                selection: manualBinding(
                    for: \.controllerCardTitlePalette
                )
            )

            optionalRolePalettePicker(
                "Context Menu Colour",
                id: "context-menu-colour",
                selection: manualBinding(
                    for: \.controllerContextMenuPalette
                )
            )

            optionalRolePalettePicker(
                "Import Actions Colour",
                id: "import-actions-colour",
                selection: manualBinding(
                    for: \.controllerImportActionPalette
                )
            )

            optionalRolePalettePicker(
                "Toolbar Icons Colour",
                id: "toolbar-icons-colour",
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
