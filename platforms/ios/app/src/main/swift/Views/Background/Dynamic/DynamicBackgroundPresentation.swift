// DynamicBackgroundPresentation.swift — ARMSX2 dynamic background integration
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private struct ClearLiquidGlassUIEnvironmentKey: EnvironmentKey {
    static let defaultValue = true
}

extension EnvironmentValues {
    var clearLiquidGlassUIEnabled: Bool {
        get { self[ClearLiquidGlassUIEnvironmentKey.self] }
        set { self[ClearLiquidGlassUIEnvironmentKey.self] = newValue }
    }
}

/// Keeps every app-owned overlay on the exact same Regular/Clear glass choice
/// as the Quick Menu. Geometry and interaction remain local to each surface;
/// only the material style comes from the shared persisted preference.
private struct QuickMenuLiquidGlassConfigurationModifier: ViewModifier {
    @State private var settings = SettingsStore.shared

    func body(content: Content) -> some View {
        content.environment(
            \.clearLiquidGlassUIEnabled,
            settings.clearLiquidGlassUIQuickMenu
        )
    }
}

extension View {
    func quickMenuLiquidGlassConfiguration() -> some View {
        modifier(QuickMenuLiquidGlassConfigurationModifier())
    }

    /// Temporarily selects the lightweight live-wallpaper renderer while a
    /// dense foreground editor is mounted, without changing the persisted
    /// Original Full Quality preference.
    func lightweightLiveWallpaperWhilePresented() -> some View {
        modifier(LightweightLiveWallpaperPresentationModifier())
    }
}

private struct LightweightLiveWallpaperPresentationModifier: ViewModifier {
    @State private var acquiredOverride = false

    func body(content: Content) -> some View {
        content
            .onAppear {
                guard !acquiredOverride else { return }
                acquiredOverride = true
                UIFrameRateSettings.shared
                    .beginLightweightLiveWallpaperPresentation()
            }
            .onDisappear {
                guard acquiredOverride else { return }
                acquiredOverride = false
                UIFrameRateSettings.shared
                    .endLightweightLiveWallpaperPresentation()
            }
    }
}

struct DynamicBackgroundRendererView: View {
    let preferences: DynamicAppearancePreferences
    var allowsMainMenuThermalFallback = false
    @State private var isRenderingEnabled = true

    var body: some View {
        Group {
            if isRenderingEnabled {
                let theme = DynamicBackgroundTheme(preferences: preferences)
                DynamicBackgroundStyleCrossfadeView(
                    style: preferences.dynamicBackground,
                    theme: theme,
                    allowsMainMenuThermalFallback:
                        allowsMainMenuThermalFallback
                )
            }
        }
        .onAppear {
            isRenderingEnabled = true
        }
        .onReceive(NotificationCenter.default.publisher(for: UIScene.willDeactivateNotification)) { _ in
            isRenderingEnabled = false
        }
        .onReceive(NotificationCenter.default.publisher(for: UIScene.didActivateNotification)) { _ in
            isRenderingEnabled = true
        }
        .onReceive(NotificationCenter.default.publisher(for: AppState.releaseMenuBackgroundResourcesNotification)) { _ in
            isRenderingEnabled = false
        }
    }
}

private struct DynamicBackgroundStylePresentation: Equatable {
    let style: DynamicBackgroundStyle
    let theme: DynamicBackgroundTheme

    var paletteSignature: DynamicBackgroundPaletteSignature {
        DynamicBackgroundPaletteSignature(theme: theme)
    }
}

/// The palette-facing subset of a dynamic theme. Geometry-only setting edits
/// continue updating their existing renderer, while every source of colour
/// changes (theme shortcuts, cover previews, favourites, and the Context Menu)
/// follows the same crossfade path.
private struct DynamicBackgroundPaletteSignature: Equatable {
    let sharedPalette: ThemePalette
    let sharedCustomColor: SavedPaletteColor?
    let sharedMultiColor: ThemeMultiColorSelection
    let ribbonPalette: ThemePalette
    let ribbonCustomColor: SavedPaletteColor?
    let ribbonMultiColor: ThemeMultiColorSelection
    let disablesDarkPaletteEffects: Bool
    let paletteDarkEffectIntensity: Double
    let sharedPaletteGradientTilt: Double
    let sharedPaletteGradientOffsetX: Double
    let sharedPaletteGradientOffsetY: Double
    let sharedPaletteGradientWidth: Double
    let sharedPaletteGradientCurvature: Double
    let multiColorAnimationSpeed: Double
    let multiColorAnimationSmoothness: Double
    let multiColorAnimationSpread: Double
    let glassGradientPreset: PlayStation3XMBGradientPreset
    let glassColorR: Double
    let glassColorG: Double
    let glassColorB: Double
    let glassGradientTopMultiplier: Double
    let glassGradientBottomMultiplier: Double

    init(theme: DynamicBackgroundTheme) {
        let settings = theme.particleSettings
        let glass = settings.playStation3XMB
        sharedPalette = theme.sharedPalette
        sharedCustomColor = theme.sharedCustomColor
        sharedMultiColor = theme.sharedMultiColor
        ribbonPalette = theme.ribbonPalette
        ribbonCustomColor = theme.ribbonCustomColor
        ribbonMultiColor = theme.ribbonMultiColor
        disablesDarkPaletteEffects = settings.disablesDarkPaletteEffects
        paletteDarkEffectIntensity = settings.paletteDarkEffectIntensity
        sharedPaletteGradientTilt = settings.sharedPaletteGradientTilt
        sharedPaletteGradientOffsetX = settings.sharedPaletteGradientOffsetX
        sharedPaletteGradientOffsetY = settings.sharedPaletteGradientOffsetY
        sharedPaletteGradientWidth = settings.sharedPaletteGradientWidth
        sharedPaletteGradientCurvature = settings.sharedPaletteGradientCurvature
        multiColorAnimationSpeed = settings.multiColorAnimationSpeed
        multiColorAnimationSmoothness = settings.multiColorAnimationSmoothness
        multiColorAnimationSpread = settings.multiColorAnimationSpread
        glassGradientPreset = glass.gradientPreset
        glassColorR = glass.colorR
        glassColorG = glass.colorG
        glassColorB = glass.colorB
        glassGradientTopMultiplier = glass.gradientTopMul
        glassGradientBottomMultiplier = glass.gradientBotMul
    }
}

private struct DynamicBackgroundStyleLayer: Identifiable {
    let id: UUID
    var presentation: DynamicBackgroundStylePresentation
    var opacity: Double
}

/// Crossfades every live-wallpaper engine without blending their geometry.
/// Keeping each renderer independent prevents ribbons, towers, particles,
/// face buttons, and other unrelated scenes from sliding between coordinate
/// systems while a Theme Preset changes.
private struct DynamicBackgroundStyleCrossfadeView: View {
    let style: DynamicBackgroundStyle
    let theme: DynamicBackgroundTheme
    let allowsMainMenuThermalFallback: Bool

    @State private var layers: [DynamicBackgroundStyleLayer]
    @State private var activeLayerID: UUID
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private let transitionDuration: TimeInterval = 0.46

    init(
        style: DynamicBackgroundStyle,
        theme: DynamicBackgroundTheme,
        allowsMainMenuThermalFallback: Bool
    ) {
        self.style = style
        self.theme = theme
        self.allowsMainMenuThermalFallback = allowsMainMenuThermalFallback

        let id = UUID()
        _layers = State(
            initialValue: [
                DynamicBackgroundStyleLayer(
                    id: id,
                    presentation: DynamicBackgroundStylePresentation(
                        style: style,
                        theme: theme
                    ),
                    opacity: 1
                )
            ]
        )
        _activeLayerID = State(initialValue: id)
    }

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                ForEach(layers) { layer in
                    ResolutionAwareDynamicBackgroundContentView(
                        style: layer.presentation.style,
                        theme: layer.presentation.theme,
                        allowsMainMenuThermalFallback:
                            allowsMainMenuThermalFallback
                    )
                    // Removing the outgoing renderer must never change the
                    // incoming camera's proposed viewport. Crystal therefore
                    // keeps one fixed projection throughout the entire fade.
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
                    .clipped()
                    .opacity(layer.opacity)
                    .allowsHitTesting(false)
                }
            }
            .frame(
                width: geometry.size.width,
                height: geometry.size.height
            )
            .clipped()
        }
        .ignoresSafeArea()
        .onChange(of: requestedPresentation) { _, next in
            updatePresentation(to: next)
        }
    }

    private var requestedPresentation: DynamicBackgroundStylePresentation {
        DynamicBackgroundStylePresentation(style: style, theme: theme)
    }

    private func updatePresentation(
        to next: DynamicBackgroundStylePresentation
    ) {
        guard let activeIndex = layers.firstIndex(
            where: { $0.id == activeLayerID }
        ) else {
            replaceAllLayers(with: next)
            return
        }

        let current = layers[activeIndex].presentation
        guard current != next else { return }

        guard !reduceMotion else {
            replaceAllLayers(with: next)
            return
        }

        let styleChanges = current.style != next.style
        let paletteChanges =
            current.paletteSignature != next.paletteSignature

        guard styleChanges || paletteChanges else {
            layers[activeIndex].presentation = next
            return
        }

        let retiringIDs = Set(layers.map(\.id))
        let nextID = UUID()
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            layers.append(
                DynamicBackgroundStyleLayer(
                    id: nextID,
                    presentation: next,
                    opacity: 0
                )
            )
            activeLayerID = nextID
            if layers.count > 4 {
                layers.removeFirst(layers.count - 4)
            }
        }

        Task { @MainActor in
            await Task.yield()
            withAnimation(.easeInOut(duration: transitionDuration)) {
                for index in layers.indices {
                    layers[index].opacity = layers[index].id == nextID ? 1 : 0
                }
            }

            try? await Task.sleep(for: .seconds(transitionDuration + 0.08))
            layers.removeAll {
                retiringIDs.contains($0.id) && $0.id != activeLayerID
            }
        }
    }

    private func replaceAllLayers(
        with presentation: DynamicBackgroundStylePresentation
    ) {
        let id = UUID()
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            layers = [
                DynamicBackgroundStyleLayer(
                    id: id,
                    presentation: presentation,
                    opacity: 1
                )
            ]
            activeLayerID = id
        }
    }

}

/// Applies the same frame-rate and render-resolution configuration to every
/// place that presents a dynamic background, including the full-screen editor
/// preview. Keeping this wrapper shared prevents previews from silently using
/// the environment defaults instead of the user's selected resolution.
struct ResolutionAwareDynamicBackgroundContentView: View {
    let style: DynamicBackgroundStyle
    let theme: DynamicBackgroundTheme
    var allowsMainMenuThermalFallback = false

    @State private var frameRates = UIFrameRateSettings.shared
    @State private var thermalState = ProcessInfo.processInfo.thermalState

    var body: some View {
        let configuration = frameRates.configuration(
            usingLightweightLiveWallpaperQuality:
                shouldUseLightweightThermalQuality
        )
        DynamicBackgroundContentView(style: style, theme: theme)
            .environment(\.uiFrameRateConfiguration, configuration)
            // Canvas and Metal surfaces can retain their previous backing
            // allocation when only an environment value changes. A resolution
            // identity recreates them without changing full-screen geometry.
            .id(
                "\(style.id)-\(configuration.dynamicWallpaperResolution.rawValue)"
                    + "-face:\(configuration.dynamicFaceButtonResolution.rawValue)"
                    + "-original:\(configuration.usesOriginalFullQualityLiveWallpapers)"
            )
            .onReceive(
                NotificationCenter.default.publisher(
                    for: ProcessInfo.thermalStateDidChangeNotification
                )
            ) { _ in
                thermalState = ProcessInfo.processInfo.thermalState
            }
    }

    private var shouldUseLightweightThermalQuality: Bool {
        guard allowsMainMenuThermalFallback,
              frameRates.usesOriginalFullQualityLiveWallpapers,
              frameRates.usesLightweightQualityOnHotTemperature
        else {
            return false
        }

        switch thermalState {
        case .serious, .critical:
            return true
        case .nominal, .fair:
            return false
        @unknown default:
            return false
        }
    }
}

/// Preserves the stock full-screen coordinate system, compresses that rendered
/// result into a smaller drawing-group target, then enlarges the one surface.
/// Unlike `displayScale`, this establishes concrete reduced bounds for the
/// offscreen texture. Metal content uses its own explicit drawable size.
struct DynamicWallpaperSwiftUIRenderSurfaceModifier: ViewModifier {
    var opaque = false
    var usesFaceButtonResolution = false
    @Environment(\.uiFrameRateConfiguration) private var frameRates

    @ViewBuilder
    func body(content: Content) -> some View {
        let scale = CGFloat(
            usesFaceButtonResolution
                ? frameRates.dynamicFaceButtonRenderScale
                : frameRates.dynamicWallpaperRenderScale
        )
        if scale < 0.999 {
            GeometryReader { geometry in
                let fullSize = CGSize(
                    width: max(1, geometry.size.width),
                    height: max(1, geometry.size.height)
                )
                let rasterSize = CGSize(
                    width: max(1, (fullSize.width * scale).rounded(.up)),
                    height: max(1, (fullSize.height * scale).rounded(.up))
                )

                ZStack(alignment: .topLeading) {
                    content
                        .frame(width: fullSize.width, height: fullSize.height)
                        .ignoresSafeArea()
                        .scaleEffect(scale, anchor: .topLeading)
                        .frame(
                            width: rasterSize.width,
                            height: rasterSize.height,
                            alignment: .topLeading
                        )
                        .clipped()
                        .drawingGroup(opaque: opaque, colorMode: .nonLinear)
                        .scaleEffect(1 / scale, anchor: .topLeading)
                }
                .frame(
                    width: fullSize.width,
                    height: fullSize.height,
                    alignment: .topLeading
                )
                .clipped()
            }
            // Match the direct full-quality path: reduced backing resolution
            // must not reintroduce the safe-area proposal or shrink the scene.
            .ignoresSafeArea()
        } else {
            content
        }
    }
}

extension View {
    func dynamicWallpaperSwiftUIRenderSurface(
        opaque: Bool = false,
        usesFaceButtonResolution: Bool = false
    ) -> some View {
        modifier(
            DynamicWallpaperSwiftUIRenderSurfaceModifier(
                opaque: opaque,
                usesFaceButtonResolution: usesFaceButtonResolution
            )
        )
    }
}

/// A Canvas whose logical coordinate system remains screen-sized while its
/// actual backing surface follows Live Wallpaper Resolution. This avoids the
/// independent full-resolution Canvas allocation that can otherwise survive
/// inside an outer drawing group.
struct DynamicWallpaperCanvas: View {
    var opaque = false
    var colorMode: ColorRenderingMode = .nonLinear
    var rendersAsynchronously = true
    var usesWallpaperRenderScale = true
    let renderer: (inout GraphicsContext, CGSize) -> Void

    @Environment(\.uiFrameRateConfiguration) private var frameRates

    init(
        opaque: Bool = false,
        colorMode: ColorRenderingMode = .nonLinear,
        rendersAsynchronously: Bool = true,
        usesWallpaperRenderScale: Bool = true,
        renderer: @escaping (inout GraphicsContext, CGSize) -> Void
    ) {
        self.opaque = opaque
        self.colorMode = colorMode
        self.rendersAsynchronously = rendersAsynchronously
        self.usesWallpaperRenderScale = usesWallpaperRenderScale
        self.renderer = renderer
    }

    var body: some View {
        GeometryReader { geometry in
            let scale = CGFloat(
                usesWallpaperRenderScale
                    ? frameRates.dynamicWallpaperRenderScale
                    : 1
            )
            let fullSize = CGSize(
                width: max(1, geometry.size.width),
                height: max(1, geometry.size.height)
            )
            let rasterSize = CGSize(
                width: max(1, (fullSize.width * scale).rounded(.up)),
                height: max(1, (fullSize.height * scale).rounded(.up))
            )

            Canvas(
                opaque: opaque,
                colorMode: colorMode,
                rendersAsynchronously: rendersAsynchronously
            ) { context, _ in
                context.scaleBy(x: scale, y: scale)
                renderer(&context, fullSize)
            }
            .frame(width: rasterSize.width, height: rasterSize.height)
            .scaleEffect(1 / scale, anchor: .topLeading)
            .frame(
                width: fullSize.width,
                height: fullSize.height,
                alignment: .topLeading
            )
            .clipped()
        }
        .ignoresSafeArea()
    }
}

struct DynamicBackgroundContentView: View {
    let style: DynamicBackgroundStyle
    let theme: DynamicBackgroundTheme

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    @ViewBuilder
    var body: some View {
        if reduceMotion {
            StaticDynamicBackgroundView(theme: theme)
        } else {
            style.makeBackground(theme: theme)
                .id(style.id)
        }
    }
}

private struct StaticDynamicBackgroundView: View {
    let theme: DynamicBackgroundTheme

    var body: some View {
        let deep = theme.sharedColor(index: 0, time: 0)
        let middle = theme.sharedColor(index: 1, time: 0)
        let accent = theme.ribbonColor(index: 2, time: 0)
        let gradient = theme.sharedGradientPoints(from: .topLeading, to: .bottomTrailing)

        ZStack {
            PaletteGradientField(
                colors: [
                    theme.paletteBackgroundColor(deep, darkness: 0.96),
                    theme.paletteBackgroundColor(middle, darkness: 0.72),
                    theme.paletteBackgroundColor(accent, darkness: 0.9),
                    theme.paletteBackgroundColor(deep, darkness: 1),
                ],
                startPoint: gradient.start,
                endPoint: gradient.end,
                curvature: theme.sharedPaletteGradientCurvature
            )

            Circle()
                .fill(accent.opacity(0.18))
                .frame(width: 520, height: 520)
                .blur(radius: 130)
                .offset(x: 140, y: -90)
        }
        .ignoresSafeArea()
    }
}

private extension DynamicBackgroundTheme {
    init(preferences: DynamicAppearancePreferences) {
        self.init(
            sharedPalette: preferences.sharedPalette,
            sharedCustomColor: preferences.sharedCustomColor,
            sharedMultiColor: preferences.sharedMultiColor,
            ribbonPalette: preferences.ribbonPalette,
            ribbonCustomColor: preferences.ribbonCustomColor,
            ribbonMultiColor: preferences.ribbonMultiColor,
            particleSettings: preferences.particleSettings
        )
    }
}

struct DynamicBackgroundAppearanceSections: View {
    @State private var settings = SettingsStore.shared
    @State private var frameRates = UIFrameRateSettings.shared
    @Binding var preferences: DynamicAppearancePreferences
    let showPaletteEditor: () -> Void

    var body: some View {
        Section {
            Toggle(isOn: $settings.dynamicBackgroundsEnabled) {
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

extension View {
    func glassSurface(
        tint: Color? = nil,
        interactive: Bool = false,
        clear: Bool = false,
        forceClear: Bool = false,
        materializeTransition: Bool = false,
        isEnabled: Bool = true,
        cornerRadius: CGFloat
    ) -> some View {
        modifier(
            DynamicBackgroundGlassSurfaceModifier(
                tint: tint,
                interactive: interactive,
                clear: clear,
                forceClear: forceClear,
                materializeTransition: materializeTransition,
                isEnabled: isEnabled,
                cornerRadius: cornerRadius
            )
        )
    }
}

private struct DynamicBackgroundGlassSurfaceModifier: ViewModifier {
    @Environment(\.clearLiquidGlassUIEnabled) private var clearLiquidGlassUIEnabled
    let tint: Color?
    let interactive: Bool
    let clear: Bool
    let forceClear: Bool
    let materializeTransition: Bool
    let isEnabled: Bool
    let cornerRadius: CGFloat

    @ViewBuilder
    func body(content: Content) -> some View {
        let usesClearGlass = clear && (forceClear || clearLiquidGlassUIEnabled)
        if !isEnabled {
            // Glass is a compositor effect, not layout. Far-offscreen lazy
            // content can keep its exact measured card geometry without
            // retaining a live backdrop sampler.
            content
        } else if #available(iOS 26.0, *) {
            let glass = usesClearGlass
                ? Glass.clear
                : Glass.regular
            content
                .glassEffect(
                    glass.tint(tint).interactive(interactive),
                    in: .rect(cornerRadius: cornerRadius)
                )
                .glassEffectTransition(
                    materializeTransition ? .materialize : .identity
                )
                .controllerFocusDepthBehindGlass(
                    cornerRadius: cornerRadius
                )
        } else {
            let fallbackMaterial = usesClearGlass
                ? AnyShapeStyle(.ultraThinMaterial)
                : AnyShapeStyle(.regularMaterial)
            if materializeTransition {
                content
                    .background(
                        fallbackMaterial,
                        in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    )
                    .transition(.opacity.combined(with: .scale(scale: 0.94)))
                    .controllerFocusDepthBehindGlass(
                        cornerRadius: cornerRadius
                    )
            } else {
                content
                    .background(
                        fallbackMaterial,
                        in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    )
                    .controllerFocusDepthBehindGlass(
                        cornerRadius: cornerRadius
                    )
            }
        }
    }
}
