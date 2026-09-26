// ThemeGalleryView.swift — Themes, saved designs, palettes and effects.
// SPDX-License-Identifier: GPL-3.0+
import SwiftUI

struct ThemeGalleryView: View {
    private enum Page: String, CaseIterable, Identifiable {
        case themes = "Themes"
        case saved = "Saved"
        case shared = "Shared Themes"
        case ribbons = "Ribbons"
        case effects = "Dynamic Settings"
        case design = "UI Design"
        var id: String { rawValue }
    }

    let controllerInput: MenuControllerInputRouter?
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var gallery = ThemeGalleryStore.shared
    @State private var page: Page = .themes
    @State private var preferences = SettingsStore.shared.dynamicAppearancePreferences
    @State private var paletteTarget: ThemePaletteTarget = .shared
    @State private var original: AppearanceThemeSnapshot?
    @State private var originalSelection: AppearanceThemeSelection?
    @State private var customPreviewSnapshot: AppearanceThemeSnapshot?
    @State private var undoHistory: [AppearanceThemeSnapshot] = []
    @State private var didApply = false
    @State private var previewTask: Task<Void, Never>?
    @State private var isPreviewing = false
    @State private var livePreview = true
    @State private var previewTitle = ""
    @State private var showsNameKeyboard = false
    @State private var saveError = false
    @State private var pendingDeletion: SavedAppearanceTheme?
    @AppStorage("ARMSX2iOSDynamicSavedPaletteColors") private var savedColorsJSON = "[]"

    var body: some View {
        ZStack {
            NavigationStack {
                ScrollView {
                    VStack(spacing: 18) {
                        header
                        Picker("Theme Gallery Section", selection: $page) {
                            ForEach(Page.allCases) { Text($0.rawValue).tag($0) }
                        }
                        .pickerStyle(.menu)
                        .controllerAccessibilityOptionsPickerTarget(
                            id: "gallery.page", label: "Theme Gallery Section",
                            selection: $page,
                            options: Page.allCases.map { (id: $0, title: $0.rawValue) }
                        )
                        Toggle("Live Preview", isOn: $livePreview)
                            .controllerAccessibilityToggleTarget(
                                id: "gallery.live-preview", label: "Live Preview", isOn: $livePreview
                            )
                        selectedPage
                    }
                    .padding(20)
                    .frame(maxWidth: 920)
                    .frame(maxWidth: .infinity)
                }
                .background {
                    Color.clear.glassSurface(clear: false, cornerRadius: 26)
                    ControllerRightStickScrollTarget(
                        controllerInput: controllerInput, axes: .vertical,
                        priority: 600, isEnabled: !showsNameKeyboard && pendingDeletion == nil,
                        searchesNearbyScrollViews: true
                    )
                    .allowsHitTesting(false)
                }
                .navigationTitle("Theme Gallery")
                .navigationBarTitleDisplayMode(.inline)
            }
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                isActive: !showsNameKeyboard && pendingDeletion == nil,
                scopeKey: "appearance.theme-gallery." + page.id,
                priority: 600,
                onBack: { cancel(); return true },
                preferredInitialFocusLabel: "Theme Gallery Section",
                declaredTargetOrder: targetOrder
            )
            .opacity(isPreviewing ? 0 : 1)
            if isPreviewing {
                VStack {
                    Spacer()
                    Text(previewTitle)
                        .font(.headline)
                        .foregroundStyle(settings.controllerTextAppearance.focusedColor)
                        .padding(.horizontal, 22)
                        .padding(.vertical, 12)
                        .glassSurface(clear: true, forceClear: true, cornerRadius: 30)
                        .overlay { ControllerFocusBox(cornerRadius: 30, performanceOptimized: true) }
                }
                .padding(.bottom, 32)
                .allowsHitTesting(false)
            }
        }
        .preferredColorScheme(.dark)
        .presentationBackground(.clear)
        .presentationDragIndicator(isPreviewing ? .hidden : .visible)
        .onAppear {
            guard original == nil else { return }
            originalSelection = gallery.currentSelection(for: settings)
            gallery.preserveCustomTheme(settings)
            original = AppearanceThemeSnapshot(settings: settings)
            customPreviewSnapshot = gallery.customDraft
            preferences = settings.dynamicAppearancePreferences
        }
        .onChange(of: page) { _, page in
            endPreview()
            if page == .shared { paletteTarget = .shared }
            if page == .ribbons { paletteTarget = .ribbons }
        }
        .onChange(of: preferences) { _, updated in
            // A preset synchronizes the binding too; only a user edit differs.
            guard updated != settings.dynamicAppearancePreferences else { return }
            rememberUndo()
            settings.beginCustomThemeEditing()
            settings.dynamicAppearancePreferences = updated
            preview(page.rawValue)
        }
        .onChange(of: livePreview) { _, enabled in
            if !enabled { endPreview() }
        }
        .onDisappear {
            guard !showsNameKeyboard else { return }
            endPreview()
            if !didApply { restoreOriginalTheme() }
            undoHistory.removeAll()
            original = nil
            originalSelection = nil
            customPreviewSnapshot = nil
        }
        .fullScreenCover(isPresented: $showsNameKeyboard) {
            OrbitKeysKeyboardView(
                title: "Save Custom Theme", initialText: "",
                startsInNormalKeyboard: true,
                onCommit: { name in
                    saveError = !gallery.save(name: name, settings: settings)
                    showsNameKeyboard = false
                    if !saveError { page = .saved }
                },
                onCancel: { showsNameKeyboard = false }
            )
            .presentationBackground(.clear)
        }
        .alert("Theme Not Saved", isPresented: $saveError) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Enter a name for your theme.")
        }
        .alert("Delete Saved Theme?", isPresented: Binding(
            get: { pendingDeletion != nil },
            set: { if !$0 { pendingDeletion = nil } }
        )) {
            Button("Cancel", role: .cancel) { pendingDeletion = nil }
            Button("Delete", role: .destructive) {
                if let pendingDeletion { gallery.remove(pendingDeletion.id) }
                pendingDeletion = nil
            }
        } message: {
            Text(pendingDeletion?.name ?? "")
        }
    }

    private var header: some View {
        HStack {
            Button("Cancel", action: cancel)
                .controllerAccessibilityActionTarget(id: "gallery.cancel", label: "Cancel", action: cancel)
            Spacer()
            Button(action: undo) {
                Image(systemName: "arrow.uturn.backward")
            }
            .accessibilityLabel("Undo Theme Change")
            .controllerAccessibilityActionTarget(id: "gallery.undo", label: "Undo Theme Change", action: undo)
            .controllerAccessibilityTargetID("gallery.undo")
            .disabled(undoHistory.isEmpty)
            Button(action: openNameKeyboard) {
                Image(systemName: "plus")
            }
            .accessibilityLabel("Save Custom Theme")
            .controllerAccessibilityActionTarget(id: "gallery.save", label: "Save Custom Theme", action: openNameKeyboard)
            .controllerAccessibilityTargetID("gallery.save")
            Button("Apply", action: apply)
            .buttonStyle(.borderedProminent)
            .controllerAccessibilityActionTarget(id: "gallery.apply", label: "Apply", action: apply)
            .controllerAccessibilityTargetID("gallery.apply")
        }
        .buttonStyle(.bordered)
    }

    private var targetOrder: [String]? {
        let controls = ["gallery.cancel", "gallery.undo", "gallery.save", "gallery.apply", "gallery.page", "gallery.live-preview"]
        switch page {
        case .themes:
            return controls + ControllerUIThemePreset.allCases.map { "gallery.theme." + $0.id }
        case .saved:
            return controls + gallery.themes.flatMap {
                ["gallery.saved." + $0.id.uuidString, "gallery.delete." + $0.id.uuidString]
            }
        default:
            return nil
        }
    }

    @ViewBuilder private var selectedPage: some View {
        switch page {
        case .themes:
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 145), spacing: 12)], spacing: 12) {
                ForEach(ControllerUIThemePreset.allCases) { preset in
                    Button {
                        selectPreset(preset)
                    } label: {
                        themeTile(
                            preset.title,
                            colors: preset.configuration?.accentPalette.colors ?? [.gray, .white],
                            selected: settings.controllerUIThemePreset == preset
                        )
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: "gallery.theme." + preset.id, label: preset.title
                    ) {
                        selectPreset(preset)
                    }
                    .controllerAccessibilityTargetID("gallery.theme." + preset.id)
                }
            }
        case .saved:
            if gallery.themes.isEmpty {
                ContentUnavailableView(
                    "No Saved Themes", systemImage: "paintpalette",
                    description: Text("Choose Save Custom Theme to keep your current design.")
                )
            } else {
                LazyVStack(spacing: 12) {
                    ForEach(gallery.themes) { theme in
                        HStack {
                            Button {
                                selectSavedTheme(theme)
                            } label: {
                                themeTile(theme.name, colors: theme.snapshot.controllerNavigationAccentPalette.colors)
                            }
                            .buttonStyle(.plain)
                            .controllerAccessibilityActionTarget(
                                id: "gallery.saved." + theme.id.uuidString, label: theme.name
                            ) { selectSavedTheme(theme) }
                            .controllerAccessibilityTargetID("gallery.saved." + theme.id.uuidString)
                            Button(role: .destructive) { pendingDeletion = theme } label: {
                                Image(systemName: "trash").frame(width: 44, height: 44)
                            }
                            .accessibilityLabel("Delete " + theme.name)
                            .controllerAccessibilityActionTarget(
                                id: "gallery.delete." + theme.id.uuidString, label: "Delete " + theme.name
                            ) { pendingDeletion = theme }
                            .controllerAccessibilityTargetID("gallery.delete." + theme.id.uuidString)
                        }
                    }
                }
            }
        case .shared, .ribbons:
            // Reuse the actual palette editor's controls and persistence.
            // Only this selected section is mounted, never a second renderer.
            ThemePaletteControls(
                target: $paletteTarget,
                sharedPalette: $preferences.sharedPalette,
                sharedCustomColor: $preferences.sharedCustomColor,
                sharedMultiColor: $preferences.sharedMultiColor,
                ribbonPalette: $preferences.ribbonPalette,
                ribbonCustomColor: $preferences.ribbonCustomColor,
                ribbonMultiColor: $preferences.ribbonMultiColor,
                particleSettings: $preferences.particleSettings,
                isPlayStation3XMBPresetExplicit: $preferences.isPlayStation3XMBPresetExplicit,
                savedColorsJSON: $savedColorsJSON,
                dynamicBackground: preferences.dynamicBackground,
                onSaveAppearance: {}
            )
        case .effects:
            DynamicParticleSettingsControls(
                particleSettings: $preferences.particleSettings,
                isPlayStation3XMBPresetExplicit: $preferences.isPlayStation3XMBPresetExplicit,
                dynamicBackground: preferences.dynamicBackground,
                resetAllSettingsAndPalettes: {
                    preferences.particleSettings = DynamicParticleSettings()
                }
            )
        case .design:
            designControls
        }
    }

    private var designControls: some View {
        VStack(spacing: 18) {
            Picker("Background Style", selection: $preferences.dynamicBackground) {
                ForEach(DynamicBackgroundStyle.allCases) { Text($0.title).tag($0) }
            }
            .controllerAccessibilityOptionsPickerTarget(
                id: "gallery.background", label: "Background Style",
                selection: $preferences.dynamicBackground,
                options: DynamicBackgroundStyle.allCases.map { (id: $0, title: $0.title) }
            )
            Picker("Focus Box", selection: designBinding(\.controllerFocusBoxStyle, title: "Focus Box")) {
                ForEach(ControllerFocusBoxStyle.allCases) { Text($0.title).tag($0) }
            }
            .controllerAccessibilityOptionsPickerTarget(
                id: "gallery.focus", label: "Focus Box",
                selection: designBinding(\.controllerFocusBoxStyle, title: "Focus Box"),
                options: ControllerFocusBoxStyle.allCases.map { (id: $0, title: $0.title) }
            )
            Picker("Navigation Focus Animation", selection: designBinding(\.controllerNavigationFocusAnimation, title: "Navigation Focus Animation")) {
                ForEach(ControllerNavigationFocusTravelStyle.allCases) { Text($0.title).tag($0) }
            }
            .controllerAccessibilityOptionsPickerTarget(
                id: "gallery.animation", label: "Navigation Focus Animation",
                selection: designBinding(\.controllerNavigationFocusAnimation, title: "Navigation Focus Animation"),
                options: ControllerNavigationFocusTravelStyle.allCases.map { (id: $0, title: $0.title) }
            )
            Toggle("Orbs", isOn: designBinding(\.focusOrbsEnabled, title: "Orbs"))
            Toggle("Clear Liquid Glass UI", isOn: designBinding(\.clearLiquidGlassUI, title: "Clear Liquid Glass UI"))
        }
        .pickerStyle(.menu)
    }

    private func themeTile(_ name: String, colors: [Color], selected: Bool = false) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            RoundedRectangle(cornerRadius: 14)
                .fill(LinearGradient(colors: colors, startPoint: .topLeading, endPoint: .bottomTrailing))
                .frame(height: 62)
                .overlay(alignment: .topTrailing) {
                    if selected { Image(systemName: "checkmark.circle.fill").padding(8) }
                }
            Text(name).font(.subheadline.weight(.semibold)).lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .foregroundStyle(.white)
        .contentShape(Rectangle())
    }

    private func designBinding<Value>(
        _ keyPath: ReferenceWritableKeyPath<SettingsStore, Value>, title: String
    ) -> Binding<Value> {
        Binding(
            get: { settings[keyPath: keyPath] },
            set: { value in
                rememberUndo()
                settings.beginCustomThemeEditing()
                settings[keyPath: keyPath] = value
                preview(title)
            }
        )
    }

    private func rememberUndo() {
        undoHistory.append(AppearanceThemeSnapshot(settings: settings))
        if undoHistory.count > 12 { undoHistory.removeFirst() }
    }

    private func undo() {
        guard let previous = undoHistory.popLast() else { return }
        gallery.clearActiveSavedTheme()
        previous.restore(to: settings)
        preferences = settings.dynamicAppearancePreferences
        preview("Previous Theme")
    }

    private func openNameKeyboard() {
        endPreview()
        showsNameKeyboard = true
    }

    private func apply() {
        didApply = true
        settings.markAppearanceUserModified()
        if let customPreviewSnapshot { gallery.preserveCustomSnapshot(customPreviewSnapshot) }
        gallery.preserveCustomTheme(settings)
        endPreview()
        dismiss()
    }

    private func selectSavedTheme(_ theme: SavedAppearanceTheme) {
        rememberUndo()
        if settings.controllerUIThemePreset == .custom {
            customPreviewSnapshot = AppearanceThemeSnapshot(settings: settings)
        }
        gallery.apply(
            .saved(theme.id),
            to: settings,
            preservingCustomTheme: false
        )
        preferences = settings.dynamicAppearancePreferences
        preview(theme.name)
    }

    private func selectPreset(_ preset: ControllerUIThemePreset) {
        rememberUndo()
        if settings.controllerUIThemePreset == .custom {
            customPreviewSnapshot = AppearanceThemeSnapshot(settings: settings)
        }
        // Preview changes stay local until Apply, including the
        // custom variant, so Cancel never overwrites the persisted draft.
        if preset == .custom, let customPreviewSnapshot {
            gallery.clearActiveSavedTheme()
            customPreviewSnapshot.restore(to: settings)
        } else {
            gallery.apply(
                .preset(preset),
                to: settings,
                preservingCustomTheme: false
            )
        }
        preferences = settings.dynamicAppearancePreferences
        preview(preset.title)
    }

    private func preview(_ title: String) {
        guard livePreview else { return }
        previewTitle = title
        previewTask?.cancel()
        withAnimation(.easeOut(duration: 0.15)) {
            isPreviewing = true
            gallery.isPreviewing = true
        }
        previewTask = Task { @MainActor in
            do { try await Task.sleep(for: .seconds(2)) } catch { return }
            withAnimation(.easeOut(duration: 0.2)) {
                isPreviewing = false
                gallery.isPreviewing = false
            }
            previewTask = nil
        }
    }

    private func endPreview() {
        previewTask?.cancel()
        previewTask = nil
        isPreviewing = false
        gallery.isPreviewing = false
    }

    private func cancel() {
        endPreview()
        restoreOriginalTheme()
        didApply = true
        dismiss()
    }

    private func restoreOriginalTheme() {
        original?.restore(to: settings)
        switch originalSelection {
        case .some(.saved(let id)):
            gallery.setActiveSavedTheme(id)
        case .some(.preset(_)), .none:
            gallery.clearActiveSavedTheme()
        }
    }
}
