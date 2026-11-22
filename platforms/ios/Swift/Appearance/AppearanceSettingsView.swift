// AppearanceSettingsView.swift — Library background customization
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

struct AppearanceSettingsView: View, AppearanceSettingsEditing {
    private enum PresentedEditor: String, Identifiable {
        case colours
        case gallery

        var id: String { rawValue }
    }

    enum PresentedControllerColour: String, Identifiable {
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
    @State private var dynamicPreferences = SettingsStore.shared.appearance.dynamicAppearancePreferences
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
        "settings.appearance.game-menu-cover-colours",
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
        "settings.appearance.theme-triggers",
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
            AppearanceBackgroundSections()

            AppearanceInterfaceSections()

            // Keep background choice next to the presets it belongs with,
            // without a first-section header under the large title.
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

            AppearanceColourSections(
                presentedControllerColour: $presentedControllerColour
            )

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
        // Form's menu-button pressed style resolves against the light scheme
        // after a Picker dismisses; keep the semantic theme colours.
        .foregroundStyle(
            settings.appearance.controllerTextAppearance.contentColor ?? .white
        )
        .tint(settings.appearance.controllerNavigationAccentColor)
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        // On the Form itself, which reliably reaches its private UIKit list. The
        // pushed-page spacer already clears the tab bar.
        .contentMargins(.bottom, 0, for: .scrollContent)
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
        // Explicit large-title ownership lets NavigationStack inset the Form's
        // scroll edge instead of putting the first section under the bar.
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
                    themeSaveError = !ThemeGalleryStore.shared.save(name: name, settings: settings.appearance)
                    showsThemeNameKeyboard = false
                },
                onCancel: { showsThemeNameKeyboard = false }
            )
            .presentationBackground(.clear)
            .appStatusBarHidden()
        }
        .controllerPrompt(
            "Theme Not Saved",
            isPresented: $themeSaveError,
            message: "Enter a name for your theme.",
            actions: [.ok]
        )
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
        .controllerPrompt(
            settings.localized("Theme Preset"),
            isPresented: $showsThemeTransferResult,
            message: settings.localized(themeTransferMessage),
            actions: [.ok]
        )
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
            dynamicPreferences = settings.appearance.dynamicAppearancePreferences
        }
        .onChange(of: settings.appearance.dynamicAppearancePreferences) { _, preferences in
            if presentedEditor == nil { dynamicPreferences = preferences }
        }
        .onDisappear {
            ThemeGalleryStore.shared.preserveCustomTheme(settings.appearance)
        }
    }

    private var themeSelections: [AppearanceThemeSelection] {
        themeGallery.orderedSelections
    }

    private func shareCurrentTheme() {
        do {
            themeShareItem = ShareSheetItem(
                url: try themeGallery.exportCurrentTheme(from: settings.appearance)
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
                    applyingTo: settings.appearance
                )
                dynamicPreferences = settings.appearance.dynamicAppearancePreferences
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
            get: { themeGallery.currentSelection(for: settings.appearance) },
            set: { selection in
                var transaction = Transaction()
                transaction.animation = nil
                withTransaction(transaction) {
                    // Publish a preset's colours and renderer settings in one nonanimated transaction.
                    themeGallery.apply(selection, to: settings.appearance)
                    dynamicPreferences = settings.appearance.dynamicAppearancePreferences
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

    private func controllerCustomColour(
        for target: PresentedControllerColour
    ) -> Color {
        switch target {
        case .accent:
            return settings.appearance.controllerNavigationCustomAccentColor?.color
                ?? settings.appearance.controllerNavigationAccentPalette.semanticAccentColor
        case .focusBox:
            return settings.appearance.controllerFocusBoxCustomColor?.color
                ?? settings.appearance.controllerFocusBoxPalette.semanticAccentColor
        case .text:
            return settings.appearance.controllerTextAppearance.normalColor
                ?? .primary
        case .focusedText:
            return settings.appearance.controllerTextAppearance.focusedColor
        case .textShadow:
            return settings.appearance.controllerTextShadowCustomColor?.color
                ?? settings.appearance.controllerTextShadowPalette
                    .semanticInterfaceShadowColor
        case .focusedTextShadow:
            return settings.appearance.controllerFocusedTextShadowCustomColor?.color
                ?? settings.appearance.controllerFocusedTextShadowPalette.semanticInterfaceShadowColor
        default:
            return settings.appearance.customRoleColor(target.rawValue) ?? rolePreviewColor(for: target.rawValue)
        }
    }

    private func controllerCustomColourExists(
        for target: PresentedControllerColour
    ) -> Bool {
        switch target {
        case .accent:
            return settings.appearance.controllerNavigationCustomAccentColor != nil
        case .focusBox:
            return settings.appearance.controllerFocusBoxCustomColor != nil
        case .text:
            return settings.appearance.controllerTextCustomColor != nil
        case .focusedText:
            return settings.appearance.controllerFocusedTextCustomColor != nil
        case .textShadow:
            return settings.appearance.controllerTextShadowCustomColor != nil
        case .focusedTextShadow:
            return settings.appearance.controllerFocusedTextShadowCustomColor != nil
        default:
            return settings.appearance.controllerRoleCustomColors[target.rawValue] != nil
        }
    }

    private func saveControllerCustomColour(
        _ colour: Color,
        for target: PresentedControllerColour
    ) {
        guard let savedColour = SavedPaletteColor(color: colour) else {
            return
        }
        settings.appearance.beginCustomThemeEditing()
        switch target {
        case .accent:
            settings.appearance.controllerNavigationCustomAccentColor = savedColour
        case .focusBox:
            settings.appearance.controllerFocusBoxCustomColor = savedColour
        case .text:
            settings.appearance.controllerTextCustomColor = savedColour
        case .focusedText:
            settings.appearance.controllerFocusedTextCustomColor = savedColour
        case .textShadow:
            settings.appearance.controllerTextShadowCustomColor = savedColour
        case .focusedTextShadow:
            settings.appearance.controllerFocusedTextShadowCustomColor = savedColour
        default:
            settings.appearance.controllerRoleCustomColors[target.rawValue] = savedColour
        }
    }

    private func removeControllerCustomColour(
        for target: PresentedControllerColour
    ) {
        settings.appearance.beginCustomThemeEditing()
        switch target {
        case .accent:
            settings.appearance.controllerNavigationCustomAccentColor = nil
        case .focusBox:
            settings.appearance.controllerFocusBoxCustomColor = nil
        case .text:
            settings.appearance.controllerTextCustomColor = nil
        case .focusedText:
            settings.appearance.controllerFocusedTextCustomColor = nil
        case .textShadow:
            settings.appearance.controllerTextShadowCustomColor = nil
        case .focusedTextShadow:
            settings.appearance.controllerFocusedTextShadowCustomColor = nil
        default:
            settings.appearance.controllerRoleCustomColors[target.rawValue] = nil
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

    private func saveDynamicAppearance() {
        settings.appearance.beginCustomThemeEditing()
        settings.appearance.dynamicAppearancePreferences = dynamicPreferences
    }

    private func resetAppearance() {
        settings.resetAppearanceDefaults()
        settings.appearance.markAppearanceUserModified()
        dynamicPreferences = settings.appearance.dynamicAppearancePreferences
    }

    private func paletteEditorDidDismiss() {
        isShowingBackgroundOnly = false
        dynamicPreferences = settings.appearance.dynamicAppearancePreferences
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
