// SettingsRootView.swift — Settings navigation root
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

enum SettingsStaticInfo {
    static let buildVersion = ARMSX2Bridge.buildVersion()
}

private struct ControllerSettingsDestinationModifier: ViewModifier {
    let pane: SettingsPane
    @Binding var path: [SettingsPane]

    func body(content: Content) -> some View {
        content
            .id("settings.root.\(pane.rawValue)")
            .controllerAccessibilityActionTarget(
                id: "settings.root.\(pane.rawValue)",
                label: pane.title
            ) {
                guard path.last != pane else { return }
                path.append(pane)
            }
    }
}

private extension View {
    func controllerSettingsDestination(
        _ pane: SettingsPane,
        path: Binding<[SettingsPane]>
    ) -> some View {
        modifier(
            ControllerSettingsDestinationModifier(
                pane: pane,
                path: path
            )
        )
    }
}

struct SettingsRootView: View {
    /// Real content before Language gives ScrollViewReader enough range to
    /// place the first focusable row inside the protected top corridor when
    /// control enters from the persistent bottom tab bar.
    private static let topNavigationFocusClearance: CGFloat = 96

    let resetToRootRequest: Int
    let onNavigationPathActivityChanged: (Bool) -> Void
    let controllerInput: MenuControllerInputRouter?
    let onPreviousControllerTab: @MainActor () -> Bool
    let onNextControllerTab: @MainActor () -> Bool
    let onControllerBoundary: @MainActor (MenuControllerCommand) -> Bool
    @State private var settings = SettingsStore.shared
    @State private var controllerDestinationEntryTask: Task<Void, Never>?
    @State private var rootNeedsControllerReentry = false
    @State private var jitAvailable = false
    @State private var noJITFallbackActive = false
    @State private var hasLoadedJITStatus = false
    @State private var stikDebugOpenFailed = false
    @State private var stikDebugOpenInProgress = false
    @Binding private var navigationPath: [SettingsPane]
    // Remembered for this launch only, so Settings opens at the top after a relaunch.
    @State private var rememberedRootPaneRawValue = ""
    @State private var rememberedRootScrollPositionID = ""
    @State private var rootScrollPositionID: String?
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    /// Clearance above RootView's tab bar, the same for the root List and every pushed page.
    @Environment(\.menuTabBarContentMargin) private var tabBarContentMargin
    @Environment(\.verticalSizeClass) private var verticalSizeClass
#if targetEnvironment(macCatalyst)
    @State private var selectedPane: SettingsPane? = .emulator
#endif

    init(
        navigationPath: Binding<[SettingsPane]>,
        resetToRootRequest: Int = 0,
        onNavigationPathActivityChanged: @escaping (Bool) -> Void = { _ in },
        controllerInput: MenuControllerInputRouter? = nil,
        onPreviousControllerTab: @escaping @MainActor () -> Bool = { false },
        onNextControllerTab: @escaping @MainActor () -> Bool = { false },
        onControllerBoundary: @escaping @MainActor (MenuControllerCommand) -> Bool = { _ in false }
    ) {
        self._navigationPath = navigationPath
        self.resetToRootRequest = resetToRootRequest
        self.onNavigationPathActivityChanged = onNavigationPathActivityChanged
        self.controllerInput = controllerInput
        self.onPreviousControllerTab = onPreviousControllerTab
        self.onNextControllerTab = onNextControllerTab
        self.onControllerBoundary = onControllerBoundary
    }

    private var backgroundConfigured: Bool {
        settings.hasCustomBackground && settings.appearance.backgroundEnabledInSettings
    }

    private var backgroundActive: Bool {
        backgroundConfigured
    }

    private var showsPageOwnedLargeTitle: Bool {
        navigationPath.isEmpty
            && verticalSizeClass != .compact
            && UIDevice.current.userInterfaceIdiom == .phone
    }

    private var rememberedRootPane: SettingsPane? {
        SettingsPane(rawValue: rememberedRootPaneRawValue)
    }

    var body: some View {
#if targetEnvironment(macCatalyst)
        NavigationSplitView {
            List(SettingsPane.rootCases, selection: $selectedPane) { pane in
                Label(settings.localized(pane.title), systemImage: pane.icon)
                    .tag(pane)
            }
            .navigationTitle(settings.localized("Settings"))
            .listStyle(.sidebar)
        } detail: {
            presentedSettingsDetail(for: selectedPane)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .navigationSplitViewStyle(.balanced)
        .containerBackground(backgroundActive ? Color.clear : Color(uiColor: .systemGroupedBackground), for: .navigation)
#else
        NavigationStack(path: $navigationPath) {
        ZStack {
            // Always in the tree, isActive or not: inserting it ahead of the List under a pushed
            // page could invalidate the NavigationStack and detach its controller session.
            MenuBackgroundLayer(
                isActive: menuTabIsActive && backgroundConfigured
            )
            .opacity(backgroundConfigured ? 1 : 0)

            // NavigationStack keeps the root alive under a pushed page, so the List is removed,
            // not hidden, or its rows, probes and collection view stay mounted behind it.
            if navigationPath.isEmpty {
            List {
            // A landscape phone has no room for it: it pushed Language to the bottom.
            if controllerInput != nil, verticalSizeClass != .compact {
                Color.clear
                    .frame(height: Self.topNavigationFocusClearance)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)
            }

            if showsPageOwnedLargeTitle {
                EmbeddedMenuLargeTitle(title: settings.localized("Settings"))
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
            }

            Section {
                NavigationLink(value: SettingsPane.language) {
                    Label(settings.localized("Language"), systemImage: "globe")
                }
                .controllerSettingsDestination(.language, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.appearance) {
                    Label(settings.localized("Appearance"), systemImage: "paintpalette")
                }
                .controllerSettingsDestination(.appearance, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.appIcon) {
                    Label(settings.localized("App Icon"), systemImage: "app.badge")
                }
                .controllerSettingsDestination(.appIcon, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            } header: {
                Text(settings.localized("Interface"))
            }

            Section(settings.localized("Emulation")) {
                NavigationLink(value: SettingsPane.emulator) {
                    Label(settings.localized("Emulator"), systemImage: "cpu")
                }
                .controllerSettingsDestination(.emulator, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.graphics) {
                    Label(settings.localized("Graphics"), systemImage: "paintbrush")
                }
                .controllerSettingsDestination(.graphics, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.shaders) {
                    Label(settings.localized("Shaders"), systemImage: "camera.filters")
                }
                .controllerSettingsDestination(.shaders, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.texturePacks) {
                    Label(settings.localized("Texture Packs"), systemImage: "photo.stack")
                }
                .controllerSettingsDestination(.texturePacks, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.framePacing) {
                    Label(settings.localized("Frame Pacing"), systemImage: "speedometer")
                }
                .controllerSettingsDestination(.framePacing, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.audio) {
                    Label(settings.localized("Audio"), systemImage: "speaker.wave.2")
                }
                .controllerSettingsDestination(.audio, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            Section(settings.localized("Input")) {
                NavigationLink(value: SettingsPane.gameController) {
                    Label(settings.localized("Game Controller"), systemImage: "gamecontroller")
                }
                .controllerSettingsDestination(.gameController, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.virtualPad) {
                    Label(settings.localized("Virtual Pad"), systemImage: "hand.draw")
                }
                .controllerSettingsDestination(.virtualPad, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.localMultiplayer) {
                    Label(settings.localized("Local Multiplayer"), systemImage: "person.3")
                }
                .controllerSettingsDestination(.localMultiplayer, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            Section(settings.localized("Storage & Memory")) {
                NavigationLink(value: SettingsPane.memoryCards) {
                    Label(settings.localized("Memory Cards"), systemImage: "memorychip")
                }
                .controllerSettingsDestination(.memoryCards, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.storage) {
                    Label(settings.localized("Storage"), systemImage: "internaldrive")
                }
                .controllerSettingsDestination(.storage, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.diagnostics) {
                    Label(settings.localized("Diagnostics"), systemImage: "stethoscope")
                }
                .controllerSettingsDestination(.diagnostics, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.network) {
                    Label(settings.localized("Network"), systemImage: "network")
                }
                .controllerSettingsDestination(.network, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            Section(settings.localized("Features")) {
                NavigationLink(value: SettingsPane.settingsPresets) {
                    Label(settings.localized("Settings Presets"), systemImage: "slider.horizontal.3")
                }
                .controllerSettingsDestination(.settingsPresets, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.retroAchievements) {
                    Label(settings.localized("RetroAchievements"), systemImage: "trophy")
                }
                .controllerSettingsDestination(.retroAchievements, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.customSkin) {
                    Label(
                        settings.localized("Custom Skin"),
                        systemImage: "paintpalette"
                    )
                }
                .controllerSettingsDestination(.customSkin, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
                NavigationLink(value: SettingsPane.overlay) {
                    Label(settings.localized("Overlay (OSD)"), systemImage: "text.below.photo")
                }
                .controllerSettingsDestination(.overlay, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            SettingsJITStatusSection(
                jitAvailable: jitAvailable,
                noJITFallbackActive: noJITFallbackActive,
                backgroundActive: backgroundActive,
                stikDebugOpenFailed: $stikDebugOpenFailed,
                stikDebugOpenInProgress: $stikDebugOpenInProgress,
                refreshJITStatus: refreshJITStatus
            )

            Section {
                NavigationLink(value: SettingsPane.licenses) {
                    Label(settings.localized("Licenses & Credits"), systemImage: "doc.text")
                }
                .controllerSettingsDestination(.licenses, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            Section(settings.localized("About")) {
                NavigationLink(value: SettingsPane.about) {
                    HStack {
                        Text(settings.localized("Version"))
                        Spacer()
                        Text(SettingsStaticInfo.buildVersion)
                            .foregroundStyle(.secondary)
                            .font(.caption)
                    }
                }
                .controllerSettingsDestination(.about, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)

                NavigationLink(value: SettingsPane.help) {
                    Label(settings.localized("Help"), systemImage: "questionmark.circle")
                }
                .controllerSettingsDestination(.help, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            // A real row, not a margin UIKit could absorb, so Help settles above the tab bar.
            Color.clear
                .frame(height: tabBarContentMargin)
                .listRowInsets(EdgeInsets())
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
                .accessibilityHidden(true)
        }
        // The tail row clears the tab bar. Zero keeps RootView's library margin
        // from adding a second, empty scroll range below Help.
        .contentMargins(.bottom, 0, for: .scrollContent)
        .scrollDisabled(false)
        .scrollBounceBehavior(.always)
        .scrollPosition(id: $rootScrollPositionID, anchor: .top)
        .onChange(of: rootScrollPositionID) { _, _ in
            // Set by finger and controller scrolling alike, so restoring it ignores input mode.
            rememberRootScrollPositionIfAvailable()
        }
        .scrollContentBackground(backgroundActive ? .hidden : .automatic)
        .background {
            settingsRightStickScrollTarget(
                id: "settings.root-scroll",
                isEnabled: menuTabIsActive && navigationPath.isEmpty
            )
        }
        .onAppear {
            if menuTabIsActive {
                refreshJITStatus()
            }
            // The root List is deliberately dismantled while a destination is
            // pushed. Re-enter only after the replacement List has appeared;
            // the path change can precede registration of its row probes.
            if rootNeedsControllerReentry && navigationPath.isEmpty {
                rootNeedsControllerReentry = false
                scheduleControllerDestinationEntry()
            }
        }
#if canImport(UIKit)
        .onReceive(
            NotificationCenter.default.publisher(
                for: UIApplication.didBecomeActiveNotification
            )
        ) { _ in
            if menuTabIsActive {
                refreshJITStatus()
            }
        }
#endif
            }
        }
        .stableMenuContentGlassContainer()
        .clearNavigationContainerBackground()
        .navigationTitle(
            showsPageOwnedLargeTitle || verticalSizeClass == .compact
                ? ""
                : settings.localized("Settings")
        )
        .toolbar {
            // Not tied to the path: an item put back during the Back animation
            // never shows, and the next page then opens too high.
            if verticalSizeClass == .compact {
                ToolbarItem(id: "menu.settingsCollapsedTitle", placement: .principal) {
                    EmbeddedMenuCompactTitle(
                        title: settings.localized("Settings")
                    )
                }
            }
        }
        .toolbarBackground(
            backgroundActive ? .hidden : .automatic,
            for: .navigationBar
        )
        .navigationBarTitleDisplayMode(.inline)
        .safeAreaInset(edge: .top) {
            Color.clear.frame(height: 6)
        }
        .onChange(of: menuTabIsActive) { _, isActive in
            if isActive,
               rootScrollPositionID == nil,
               !rememberedRootScrollPositionID.isEmpty {
                rootScrollPositionID = rememberedRootScrollPositionID
            } else if !isActive {
                rememberRootScrollPositionIfAvailable()
                rememberControllerRootFocusIfAvailable()
            }
        }
        .navigationDestination(for: SettingsPane.self) { pane in
            // Earlier pages in a deep path keep their route for Back but are dismantled until
            // they are on top again (Game Controller under Controller Macros, for example).
            if navigationPath.last == pane {
                presentedSettingsDetail(for: pane)
                // The safe area spacer already clears the tab bar.
                .contentMargins(.bottom, 0, for: .scrollContent)
                .background {
                    if pane != .appearance {
                        settingsRightStickScrollTarget(
                            id: "settings.detail-scroll.\(pane.rawValue)",
                            isEnabled: menuTabIsActive
                                && navigationPath.last == pane
                        )
                    }
                }
            } else {
                Color.clear
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)
            }
        }
        }
        .onChange(of: resetToRootRequest) { _, _ in
            guard !navigationPath.isEmpty else { return }
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                navigationPath.removeAll()
            }
        }
        .onChange(of: navigationPath) { previousPath, currentPath in
            // Read from the route: the List can drop its focused probe before the session sees
            // the scope change, which could leave Language remembered instead of the pane.
            if previousPath.isEmpty, let selectedPane = currentPath.first {
                rememberRootScrollPositionIfAvailable()
                rememberedRootPaneRawValue = selectedPane.rawValue
                rootNeedsControllerReentry = true
            }
            if currentPath.isEmpty,
               menuTabIsActive,
               !hasLoadedJITStatus {
                refreshJITStatus()
            }
            onNavigationPathActivityChanged(!currentPath.isEmpty)
            scheduleControllerDestinationEntry()
        }
        .onAppear {
            onNavigationPathActivityChanged(!navigationPath.isEmpty)
            if !navigationPath.isEmpty {
                scheduleControllerDestinationEntry()
            }
        }
        .onDisappear {
            rememberRootScrollPositionIfAvailable()
            rememberControllerRootFocusIfAvailable()
            controllerDestinationEntryTask?.cancel()
            controllerDestinationEntryTask = nil
        }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            isActive: menuTabIsActive,
            scopeKey: settingsControllerScopeKey,
            priority: 80,
            orbStyle: .plain,
            onBack: handleControllerBack,
            onPreviousTab: onPreviousControllerTab,
            onNextTab: onNextControllerTab,
            onBoundary: onControllerBoundary,
            boundaryRules: [
                ControllerAccessibilityBoundaryRule(
                    direction: .up,
                    fromLabel: "settings.root.language",
                    requiresFreshPress: navigationPath.isEmpty
                ),
                ControllerAccessibilityBoundaryRule(
                    direction: .down,
                    fromLabel: "settings.root.help",
                    requiresFreshPress: navigationPath.isEmpty
                ),
            ],
            // Wrap only after a fresh Up press at Language. Held traversal
            // stops at both endpoints; Help's next Down enters the tab bar.
            directionalLinks: navigationPath.isEmpty ? [
                ControllerAccessibilityDirectionalLink(
                    fromLabel: "settings.root.language",
                    direction: .up,
                    toLabel: "settings.root.help"
                ),
            ] : [],
            wrapsAtListEnds: !navigationPath.isEmpty,
            // Long lazy Lists use the ordered target graph; each root row has one full-row
            // target, so no second focusable control comes from the Button style.
            usesExplicitTargetGeometryOnly: navigationPath.isEmpty
                || navigationPath.last == .audio,
            // Root Settings and every destination use the Emulator-style
            // direct UIKit scroll path. Focus travels naturally between a
            // guarded top edge and the tab-bar-safe bottom edge.
            preservesFocusDuringRightStickScrolling: false,
            focusScrollBehavior: .maintainWithinViewport,
            scrollAnimationDuration: 0.16,
            focusTopAlignmentMargin: 84,
            focusBottomAlignmentMargin: 84,
            preferredInitialFocusLabel: settingsPreferredInitialFocusLabel,
            declaredTargetOrder: settingsControllerTargetOrder
        )
#endif
    }

    private var settingsControllerScopeKey: String {
        if let pane = navigationPath.last {
            return "settings.detail.\(pane.rawValue)"
        }
        return "settings.root"
    }

    private func rememberRootScrollPositionIfAvailable() {
        guard let positionID = rootScrollPositionID,
              positionID.hasPrefix("settings.root.") else { return }
        rememberedRootScrollPositionID = positionID
    }

    private func rememberControllerRootFocusIfAvailable() {
        guard let key = controllerInput?.rememberedNavigationFocusKey(
            forScope: "settings.root"
        ), key.hasPrefix("settings.root.") else { return }
        let rawValue = String(key.dropFirst("settings.root.".count))
        guard SettingsPane(rawValue: rawValue) != nil else { return }
        rememberedRootPaneRawValue = rawValue
    }

    private var settingsPreferredInitialFocusLabel: String? {
        if navigationPath.isEmpty {
            if let exactTarget = controllerInput?.rememberedNavigationFocusKey(
                forScope: "settings.root"
            ) {
                return exactTarget
            }
            return rememberedRootPane.map { "settings.root.\($0.rawValue)" }
        }
        if navigationPath.last == .customSkin {
            return settings.localized("Automatic Download Custom Skin")
        }
        return nil
    }

    /// Settings Lists and destination Forms are implemented by private UIKit
    /// collection views which sit beside SwiftUI backgrounds rather than inside
    /// them. A full-surface local marker provides deterministic ownership and
    /// outranks the navigation-wide fallback without adding timers.
    private func settingsRightStickScrollTarget(
        id: String,
        isEnabled: Bool
    ) -> some View {
        ControllerRightStickScrollTarget(
            controllerInput: controllerInput,
            axes: .vertical,
            priority: 240,
            isEnabled: isEnabled,
            searchesNearbyScrollViews: true
        )
        .id(id)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private var settingsControllerTargetOrder: [String]? {
        guard let pane = navigationPath.last else {
            return rootControllerTargetOrder
        }
        // Appearance is a long lazy Form; publish its graph before the first row mounts.
        if pane == .appearance {
            return AppearanceSettingsView.controllerTargetOrder
        }
        if pane == .audio {
            return AudioSettingsView.controllerTargetOrder
        }
        if pane == .gameController {
            return GamepadSettingsView.controllerTargetOrder(
                leftInstantDeadzoneEnabled:
                    settings.gameControllerLeftInstantDeadzoneEnabled,
                rightInstantDeadzoneEnabled:
                    settings.gameControllerRightInstantDeadzoneEnabled
            )
        }
        if pane == .controllerMacros {
            return ControllerMacrosSettingsView.controllerTargetOrder
        }
        if pane == .customSkin {
            // A seed for the deep link until the Virtual Pad Form publishes its own order.
            return [settings.localized("Automatic Download Custom Skin")]
        }
        if pane == .network {
            return NetworkSettingsView.controllerTargetOrder
        }
        if pane == .help {
            return HelpView.controllerTargetOrder
        }
        if pane == .overlay {
            return OverlaySettingsView.controllerTargetOrder
        }
        return nil
    }

    private func scheduleControllerDestinationEntry() {
        guard let controllerInput,
              controllerInput.isControllerNavigationEnabled else { return }
        controllerDestinationEntryTask?.cancel()
        let destinationScope = settingsControllerScopeKey
        controllerDestinationEntryTask = Task { @MainActor in
            await Task.yield()
            guard !Task.isCancelled else { return }
            _ = controllerInput.requestNavigationSessionEntry(
                preferLast: false,
                matchingScopePrefix: destinationScope
            )
            controllerDestinationEntryTask = nil
        }
    }

    private var rootControllerTargetOrder: [String] {
        var result = [
            "settings.root.language",
            "settings.root.appearance",
            "settings.root.appIcon",
            "settings.root.emulator",
            "settings.root.graphics",
        ]
        result.append("settings.root.shaders")
        result.append("settings.root.texturePacks")
        result.append(contentsOf: [
            "settings.root.framePacing",
            "settings.root.audio",
            "settings.root.gameController",
            "settings.root.virtualPad",
            "settings.root.localMultiplayer",
            "settings.root.memoryCards",
            "settings.root.storage",
            "settings.root.diagnostics",
            "settings.root.network",
            "settings.root.settingsPresets",
            "settings.root.retroAchievements",
            "settings.root.customSkin",
            "settings.root.overlay",
            "settings.root.open-stik-debug",
            "settings.root.licenses",
            "settings.root.help",
        ])
        return result
    }

    @MainActor
    private func handleControllerBack() -> Bool {
        guard !navigationPath.isEmpty else { return false }
        navigationPath.removeLast()
        return true
    }

    private func refreshJITStatus() {
        jitAvailable = ARMSX2Bridge.isJITAvailable()
        noJITFallbackActive = ARMSX2Bridge.isNoJITFallbackActive()
        hasLoadedJITStatus = true
    }

    @ViewBuilder
    private func presentedSettingsDetail(
        for pane: SettingsPane?
    ) -> some View {
        settingsDetail(for: pane)
            // One inset for every page instead of a spacer in each; the nested scroll view
            // reads it, so the last row settles above the tab bar.
            .safeAreaInset(edge: .bottom, spacing: 0) {
                Color.clear
                    .frame(height: tabBarContentMargin)
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)
            }
            .modifier(
                SettingsDetailClearLiquidGlassModifier(
                    decoratesRows: pane != .appearance,
                    // Shaders always uses Clear Liquid Glass. Its Form cells
                    // previously remained opaque because this destination was
                    // explicitly excluded from the shared row installer.
                    isClear: pane == .shaders
                        || settings.appearance.clearLiquidGlassUISubSettings
                )
            )
            .environment(
                \.clearLiquidGlassUIEnabled,
                pane == .shaders || settings.appearance.clearLiquidGlassUISubSettings
            )
    }

    private func openPane(_ pane: SettingsPane) {
        guard navigationPath.last != pane else { return }
        navigationPath.append(pane)
    }

    @ViewBuilder
    private func settingsDetail(for pane: SettingsPane?) -> some View {
        switch pane {
        case .language:
            LanguageSettingsView()
        case .appearance:
            AppearanceSettingsView()
        case .appIcon:
            AppIconSettingsView()
        case .emulator:
            EmulatorSettingsView()
        case .graphics:
            GraphicsSettingsView()
        case .shaders:
            ShaderSettingsView()
        case .texturePacks:
            TexturePacksView { TextureReplacementSettings() }
        case .framePacing:
            FramePacingSettingsView()
        case .audio:
            AudioSettingsView()
        case .network:
            NetworkSettingsView()
        case .memoryCards:
            MemoryCardSettingsView()
        case .storage:
            StorageSettingsView()
        case .diagnostics:
            DiagnosticsView()
        case .settingsPresets:
            SettingsPresetsView()
        case .retroAchievements:
            RetroAchievementsSettingsView()
        case .customSkin:
            VirtualPadSettingsView(onOpenPane: openPane)
        case .overlay:
            OverlaySettingsView()
        case .gameController:
            GamepadSettingsView(onOpenPane: openPane)
        case .controllerMacros:
            ControllerMacrosSettingsView()
        case .localMultiplayer:
            LocalMultiplayerSettingsView()
        case .virtualPad:
            VirtualPadSettingsView(onOpenPane: openPane)
        case .help:
            HelpView()
        case .licenses:
            LicenseView()
        case .about:
            SettingsAboutView()
        case .skinBrowser:
            SkinBrowserView()
        case .none:
            VStack(spacing: 12) {
                Image(systemName: "gearshape")
                    .font(.system(size: 42))
                    .foregroundStyle(.secondary)
                Text(settings.localized("Select a setting"))
                    .font(.title2)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
