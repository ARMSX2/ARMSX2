// RootView.swift — Root view switching between menu and game
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private struct MenuTabIsActiveEnvironmentKey: EnvironmentKey {
    static let defaultValue = true
}

private struct MenuBackgroundSessionStartEnvironmentKey: EnvironmentKey {
    static let defaultValue = Date()
}

private struct MenuLargeTitleNamespaceEnvironmentKey: EnvironmentKey {
    static let defaultValue: Namespace.ID? = nil
}

private struct MenuLargeTitleMorphActiveEnvironmentKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var menuTabIsActive: Bool {
        get { self[MenuTabIsActiveEnvironmentKey.self] }
        set { self[MenuTabIsActiveEnvironmentKey.self] = newValue }
    }

    var menuBackgroundSessionStart: Date {
        get { self[MenuBackgroundSessionStartEnvironmentKey.self] }
        set { self[MenuBackgroundSessionStartEnvironmentKey.self] = newValue }
    }

    var menuLargeTitleNamespace: Namespace.ID? {
        get { self[MenuLargeTitleNamespaceEnvironmentKey.self] }
        set { self[MenuLargeTitleNamespaceEnvironmentKey.self] = newValue }
    }

    var menuLargeTitleMorphActive: Bool {
        get { self[MenuLargeTitleMorphActiveEnvironmentKey.self] }
        set { self[MenuLargeTitleMorphActiveEnvironmentKey.self] = newValue }
    }
}

struct RootView: View {
    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBootSplash = true
    // The background layer observes renderer state directly. Keeping only the
    // host reference here prevents snapshot/visibility publications from
    // invalidating the complete menu hierarchy.
    @State private var backgroundHost = PersistentMenuBackgroundHost()

    private var menuScreenActive: Bool {
        if case .menu = appState.currentScreen {
            return true
        }
        return false
    }

    var body: some View {
        ZStack {
            Color(uiColor: .systemGroupedBackground)
                .ignoresSafeArea()

            PersistentMenuBackgroundLayer(host: backgroundHost)
                .opacity(
                    appState.currentScreen == .menu ||
                    appState.gameplayLaunchBackgroundVisible ? 1 : 0
                )
                .zIndex(
                    appState.currentScreen == .playing &&
                    appState.gameplayLaunchTransition != nil ? 80 : 0
                )

            // Keep the menu hierarchy alive for the launch transition so the
            // library fades behind the live SwiftUI card instead of vanishing
            // on the same update that starts the VM.
            if menuScreenActive || appState.gameplayLaunchTransition != nil {
                MenuTabView(backgroundHost: backgroundHost)
                    .opacity(menuScreenActive ? 1 : 0)
                    .animation(
                        .easeOut(duration: 0.34),
                        value: menuScreenActive
                    )
                    .allowsHitTesting(menuScreenActive)
                    .accessibilityHidden(!menuScreenActive)
                    .zIndex(
                        appState.gameplayLaunchTransition == nil ? 1 : 85
                    )
            }

            if case .playing = appState.currentScreen {
                if appState.isEmulationOnlyMode &&
                    !appState.emulationOnlyPresentation.showsQuickMenu {
                    EmulationOnlyGameView()
                } else {
                    GameScreenView()
                }
            }

            if let transition = appState.gameplayLaunchTransition {
                GameplayLaunchOverlay(
                    transition: transition,
                    onRevealGameplay: {
                        appState.revealGameplayLaunchControls()
                    },
                    onComplete: {
                        appState.completeGameplayLaunchTransition(id: transition.id)
                    }
                )
                .zIndex(90)
            }

            if showBootSplash {
                BootSplashView {
                    withAnimation(.easeOut(duration: 0.2)) {
                        showBootSplash = false
                    }
                }
                .transition(.opacity)
                .zIndex(100)
            }
        }
        .environment(\.layoutDirection, settings.localizedLayoutDirection)
        .environment(\.clearLiquidGlassUIEnabled, settings.clearLiquidGlassUI)
        .statusBarHidden(showBootSplash)
        .onAppear {
            StikDebugLauncher.autoOpenIfNeeded(reason: "app launch")
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: AppState.releaseMenuBackgroundResourcesNotification
            )
        ) { _ in
            backgroundHost.release()
            // These stores outlive the tab hierarchy. Explicitly discard their
            // decoded images and presentation-only state instead of depending
            // exclusively on the selected tab's onDisappear ordering.
            GameLibraryRuntimeResources.releaseForGameplay()
            PatchStore.shared.releasePresentationResources()
        }
        .onOpenURL { url in
            if !ARMSX2DeepLinkHandler.handle(url) {
                fileImporter.handleURL(url)
            }
        }
        .alert(settings.localized("File Import"), isPresented: $fileImporter.showImportAlert) {
            Button(settings.localized("OK")) {}
        } message: {
            Text(fileImporter.lastImportMessage ?? "")
        }
        .alert(
            settings.localized("BIOS"),
            isPresented: Binding(
                get: { appState.bootDisclaimerMessage != nil },
                set: { if !$0 { appState.bootDisclaimerMessage = nil } }
            )
        ) {
            Button(settings.localized("OK")) {
                appState.bootDisclaimerMessage = nil
            }
        } message: {
            Text(settings.localized(appState.bootDisclaimerMessage ?? "BIOS not yet imported."))
        }
        .alert(
            "⚠️ \(settings.localized("JIT Access Not Detected"))",
            isPresented: Binding(
                get: { appState.pendingJITGameBoot != nil },
                set: { if !$0 { appState.cancelPendingJITGameBoot() } }
            )
        ) {
            Button(settings.localized("Cancel"), role: .cancel) {
                appState.cancelPendingJITGameBoot()
            }
            Button(settings.localized("Continue")) {
                appState.continuePendingJITGameBoot()
            }
        } message: {
            Text(settings.localized(
                "JIT access is not available. Match the StikDebug script to the JIT Script setting in Emulator settings."
            ))
        }
    }
}

/// Rebuilds the selected card as a live SwiftUI Liquid Glass surface while the
/// menu tree is dismantled underneath it. The cover and text scale with the
/// glass geometry, then the complete surface fades into the live Metal view.
private struct GameplayLaunchOverlay: View {
    let transition: GameplayLaunchTransition
    let onRevealGameplay: () -> Void
    let onComplete: () -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var zoomed = false
    @State private var fadingOut = false

    var body: some View {
        GeometryReader { geometry in
            let viewport = geometry.frame(in: .global)
            let sourceCenter = CGPoint(
                x: transition.sourceFrame.midX - viewport.minX,
                y: transition.sourceFrame.midY - viewport.minY
            )
            let destinationCenter = CGPoint(
                x: geometry.size.width / 2,
                y: geometry.size.height / 2
            )
            let destinationScale = zoomScale(in: geometry.size)

            ZStack {
                Color.black
                    .opacity(zoomed && !reduceMotion ? 0.14 : 0)

                FluidGameplayLaunchCard(transition: transition)
                    .frame(
                        width: transition.sourceFrame.width,
                        height: transition.sourceFrame.height
                    )
                    .scaleEffect(zoomed && !reduceMotion ? destinationScale : 1)
                    .position(zoomed && !reduceMotion ? destinationCenter : sourceCenter)
                    .shadow(
                        color: .black.opacity(fadingOut ? 0 : 0.34),
                        radius: zoomed ? 26 : 8,
                        y: zoomed ? 14 : 4
                    )
            }
            .opacity(fadingOut ? 0 : 1)
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onAppear {
            guard !reduceMotion else { return }
            DispatchQueue.main.async {
                withAnimation(.spring(response: 0.42, dampingFraction: 0.86)) {
                    zoomed = true
                }
            }
        }
        .task(id: transition.id) {
            await revealWhenEmulationIsRunning()
        }
    }

    private func zoomScale(in viewport: CGSize) -> CGFloat {
        guard transition.sourceFrame.width > 0, transition.sourceFrame.height > 0 else {
            return 1.2
        }

        let availableWidthScale = (viewport.width * 0.72) / transition.sourceFrame.width
        let availableHeightScale = (viewport.height * 0.76) / transition.sourceFrame.height
        return min(2.1, max(1.16, min(availableWidthScale, availableHeightScale)))
    }

    @MainActor
    private func revealWhenEmulationIsRunning() async {
        // The spring settles in roughly 0.42 seconds. Keeping the overlay for
        // 1.42 seconds leaves the fully zoomed Liquid Glass card readable for
        // approximately one second without delaying the VM boot itself.
        let minimumDisplayNanoseconds: UInt64 = reduceMotion
            ? 240_000_000
            : 1_420_000_000
        try? await Task.sleep(nanoseconds: minimumDisplayNanoseconds)
        guard !Task.isCancelled else { return }

        for _ in 0..<24 where !ARMSX2Bridge.isVMRunning() {
            try? await Task.sleep(nanoseconds: 50_000_000)
            guard !Task.isCancelled else { return }
        }

        let fadeDuration = reduceMotion ? 0.15 : 0.30
        withAnimation(.easeOut(duration: fadeDuration)) {
            onRevealGameplay()
            fadingOut = true
        }

        try? await Task.sleep(nanoseconds: UInt64(fadeDuration * 1_000_000_000))
        guard !Task.isCancelled else { return }
        onComplete()
    }
}

struct FluidGameplayLaunchCard: View {
    let transition: GameplayLaunchTransition
    var showsUnselectedFavoriteIndicator = false

    var body: some View {
        Group {
            switch transition.style {
            case .list:
                listContent
            case .grid, .coverFlow:
                coverContent
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(alignment: .topTrailing) {
            if transition.isFavorite || showsUnselectedFavoriteIndicator {
                Image(systemName: transition.isFavorite ? "star.fill" : "star")
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(
                        transition.isFavorite ? .yellow : .white.opacity(0.86)
                    )
                    .padding(10)
                    .shadow(color: .black.opacity(0.28), radius: 4, y: 2)
            }
        }
        .glassSurface(clear: true, cornerRadius: transition.cornerRadius)
        .clipShape(
            RoundedRectangle(
                cornerRadius: transition.cornerRadius,
                style: .continuous
            )
        )
    }

    private var listContent: some View {
        HStack(spacing: 12) {
            cover

            VStack(alignment: .leading, spacing: 4) {
                Text(transition.title)
                    .font(.body.weight(.medium))
                    .foregroundStyle(.primary)
                    .lineLimit(2)
                Text(transition.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Image(systemName: "chevron.right")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(12)
    }

    private var coverContent: some View {
        VStack(spacing: transition.style == .coverFlow ? 8 : 10) {
            cover
                .shadow(color: .black.opacity(0.28), radius: 18, y: 10)

            VStack(spacing: 4) {
                Text(transition.title)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.primary)
                    .multilineTextAlignment(.center)
                    .lineLimit(2)
                Text(transition.detail)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity)
        }
        .padding(transition.style == .coverFlow ? 8 : 12)
    }

    private var cover: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))

            if let image = transition.coverImage {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                VStack(spacing: 6) {
                    Image(
                        systemName: transition.gameName.lowercased().hasSuffix(".chd")
                            ? "archivebox"
                            : "opticaldisc"
                    )
                    .font(.system(size: 24, weight: .medium))
                    Text(
                        transition.gameName.lowercased().hasSuffix(".chd")
                            ? "CHD"
                            : "PS2"
                    )
                    .font(.caption2.bold())
                }
                .foregroundStyle(.secondary)
            }
        }
        .frame(
            width: transition.coverSize.width,
            height: transition.coverSize.height
        )
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
}

struct MenuTabView: View {
    @State private var settings = SettingsStore.shared
    @State private var selectedTab = 0
    @State private var activeLibraryTab = 0
    @State private var settingsRootResetRequest = 0
    @State private var menuLargeTitleMorphActive = false
    @State private var menuLargeTitleMorphResetTask: Task<Void, Never>?
    let backgroundHost: PersistentMenuBackgroundHost
    @Namespace private var largeTitleNamespace
    @Environment(\.layoutDirection) private var layoutDirection
    @Environment(\.verticalSizeClass) private var verticalSizeClass

    private var biosBackgroundActive: Bool { settings.hasCustomBackground && settings.backgroundEnabledInBIOS }
    private var settingsBackgroundActive: Bool { settings.hasCustomBackground && settings.backgroundEnabledInSettings }

    private var selectedTabShowsBackground: Bool {
        switch selectedTab {
        case 0:
            return settings.hasCustomBackground
        case 1:
            return biosBackgroundActive
        default:
            return settingsBackgroundActive
        }
    }

    private var tabContentBottomMargin: CGFloat {
        96
    }

    private var tabBarContentSpacing: CGFloat {
        20
    }

    private var usesPageOwnedLibraryLargeTitle: Bool {
        verticalSizeClass != .compact
            && UIDevice.current.userInterfaceIdiom == .phone
    }

    private var tabSelection: Binding<Int> {
        Binding(
            get: { selectedTab },
            set: { selectTab($0) }
        )
    }

    private func selectTab(_ tab: Int) {
        guard (0...2).contains(tab), tab != selectedTab else { return }
        menuLargeTitleMorphResetTask?.cancel()
        menuLargeTitleMorphActive = true
        withAnimation(.easeInOut(duration: 0.24)) {
            // Retain the last library toolbar state while the separate Settings
            // navigation surface fades in.
            if tab <= 1 {
                activeLibraryTab = tab
            }
            selectedTab = tab
        }
        menuLargeTitleMorphResetTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(280))
            guard !Task.isCancelled else { return }
            menuLargeTitleMorphActive = false
            menuLargeTitleMorphResetTask = nil
        }
    }

    private func navigateFromHorizontalSwipe(_ translation: CGFloat) {
        let physicalStep = translation < 0 ? 1 : -1
        let logicalStep = layoutDirection == .rightToLeft ? -physicalStep : physicalStep
        selectTab(selectedTab + logicalStep)
    }

    var body: some View {
        ZStack {
            Group {
#if targetEnvironment(macCatalyst)
                VStack(spacing: 0) {
                    CatalystMenuTabBar(selectedTab: $selectedTab)
                        .padding(.top, 8)
                        .padding(.bottom, 8)

                    Group {
                        switch selectedTab {
                        case 0:
                            GameListView()
                        case 1:
                            BIOSListView()
                        default:
                            SettingsRootView(
                                resetToRootRequest: settingsRootResetRequest
                            )
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .tint(.blue)
#else
                ZStack {
                    // Games and BIOS share one persistent navigation owner.
                    // Their matching toolbar IDs therefore remain the same native
                    // Liquid Glass controls and morph between tab configurations.
                    NavigationStack {
                        ZStack {
                            GameListView(
                                embeddedInMenuNavigation: true,
                                ownsEmbeddedMenuToolbar: activeLibraryTab == 0
                            )
                                .environment(\.menuTabIsActive, selectedTab == 0)
                                .retainedMenuTabPage(0, selection: selectedTab)

                            SafeAreaProtectedMenuTabContent(
                                appliesLegacyLandscapeInsets: !biosBackgroundActive
                            ) {
                                BIOSListView(
                                    embeddedInMenuNavigation: true,
                                    ownsEmbeddedMenuToolbar: activeLibraryTab == 1
                                )
                            }
                            .environment(\.menuTabIsActive, selectedTab == 1)
                            .retainedMenuTabPage(1, selection: selectedTab)
                        }
                        .navigationTitle(
                            settings.localized(
                                usesPageOwnedLibraryLargeTitle
                                    ? ""
                                    : (activeLibraryTab == 1 ? "BIOS" : "Games")
                            )
                        )
                        .toolbarBackground(
                            (activeLibraryTab == 0
                                ? settings.hasCustomBackground
                                : biosBackgroundActive) ? .hidden : .automatic,
                            for: .navigationBar
                        )
                        .navigationBarTitleDisplayMode(
                            usesPageOwnedLibraryLargeTitle ? .inline : .large
                        )
                        .toolbar {
                            if verticalSizeClass == .compact {
                                ToolbarItem(id: "menu.collapsedTitle", placement: .principal) {
                                    Text(
                                        settings.localized(
                                            activeLibraryTab == 1 ? "BIOS" : "Games"
                                        )
                                    )
                                        .font(.headline)
                                        .lineLimit(1)
                                }
                            }
                        }
                    }
                    .safeAreaInset(edge: .top) {
                        Color.clear.frame(
                            height: usesPageOwnedLibraryLargeTitle ? 6 : 0
                        )
                    }
                    .retainedMenuPage(isActive: selectedTab != 2)

                    SafeAreaProtectedMenuTabContent(
                        appliesLegacyLandscapeInsets: !settingsBackgroundActive
                    ) {
                        SettingsRootView(
                            resetToRootRequest: settingsRootResetRequest
                        )
                    }
                    .environment(\.menuTabIsActive, selectedTab == 2)
                    .retainedMenuTabPage(2, selection: selectedTab)

                    NativeMenuTabSwipeRecognizer(
                        onSwipe: navigateFromHorizontalSwipe
                    )
                    .frame(width: 0, height: 0)
                }
                .environment(\.menuLargeTitleNamespace, largeTitleNamespace)
                .environment(
                    \.menuLargeTitleMorphActive,
                    menuLargeTitleMorphActive
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .tint(.blue)
                .contentMargins(
                    .bottom,
                    tabContentBottomMargin,
                    for: .scrollContent
                )
                .safeAreaInset(edge: .bottom, spacing: tabBarContentSpacing) {
                    Group {
                        if #available(iOS 26.0, *) {
                            NativeMenuTabBar(
                                selection: tabSelection,
                                isCompactHeight: verticalSizeClass == .compact,
                                titles: [
                                    settings.localized("Games"),
                                    settings.localized("BIOS"),
                                    settings.localized("Settings"),
                                ],
                                onReselect: { tab in
                                    guard tab == 2 else { return }
                                    settingsRootResetRequest += 1
                                }
                            )
                            // Align the native iOS 26 tab bar with compact-height content.
                            .offset(
                                y: verticalSizeClass == .compact ? 24.5 : 0
                            )
                        } else {
                            LegacyGlassMenuTabBar(
                                selection: tabSelection,
                                titles: [
                                    settings.localized("Games"),
                                    settings.localized("BIOS"),
                                    settings.localized("Settings"),
                                ],
                                onReselect: { tab in
                                    guard tab == 2 else { return }
                                    settingsRootResetRequest += 1
                                }
                            )
                            // Raise the legacy compact tab bar above the home indicator.
                            .offset(
                                y: verticalSizeClass == .compact ? -6 : 0
                            )
                        }
                    }
                    .zIndex(1_000)
                }
#endif
            }
        }
        .environment(\.menuBackgroundHost, backgroundHost)
        .onAppear {
            backgroundHost.reactivateForMenu(isAvailable: settings.hasCustomBackground)
            backgroundHost.setPresentationVisible(selectedTabShowsBackground)
        }
        .onChange(of: settings.hasCustomBackground) { _, hasBackground in
            backgroundHost.setMenuBackgroundAvailable(hasBackground)
        }
        .onChange(of: settings.dynamicBackgroundsEnabled) { _, _ in
            backgroundHost.setMenuBackgroundAvailable(settings.hasCustomBackground)
        }
        .onChange(of: selectedTabShowsBackground) { _, showsBackground in
            backgroundHost.setPresentationVisible(showsBackground)
        }
        .onDisappear {
            menuLargeTitleMorphResetTask?.cancel()
            menuLargeTitleMorphResetTask = nil
            menuLargeTitleMorphActive = false
        }
    }
}

#if !targetEnvironment(macCatalyst)
/// iOS 17/18 does not provide the floating Liquid Glass tab bar used by iOS 26.
/// Keep one compact, orientation-independent glass capsule so landscape uses
/// the same centered icon-over-label layout and dimensions as portrait.
private struct LegacyGlassMenuTabBar: View {
    @Binding var selection: Int
    let titles: [String]
    let onReselect: (Int) -> Void
    @Namespace private var selectionNamespace

    private let systemImages = [
        "gamecontroller",
        "cpu",
        "gearshape",
    ]

    private let selectionSpring = Animation.spring(
        response: 0.5,
        dampingFraction: 0.7,
        blendDuration: 0.18
    )

    var body: some View {
        HStack(spacing: 0) {
            ForEach(systemImages.indices, id: \.self) { index in
                Button {
                    if selection == index {
                        onReselect(index)
                    } else {
                        selection = index
                        UISelectionFeedbackGenerator().selectionChanged()
                    }
                } label: {
                    VStack(spacing: 1) {
                        Image(systemName: systemImages[index])
                            .font(.system(size: 20, weight: .medium))
                            .symbolRenderingMode(.monochrome)
                            .scaleEffect(selection == index ? 1.06 : 1)

                        Text(titles.indices.contains(index) ? titles[index] : "")
                            .font(.system(size: 10, weight: .semibold))
                            .lineLimit(1)
                            .minimumScaleFactor(0.8)
                    }
                    .foregroundStyle(
                        selection == index
                            ? Color.blue
                            : Color.secondary
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Capsule())
                    .background {
                        if selection == index {
                            Capsule()
                                .fill(.ultraThinMaterial)
                                .overlay {
                                    Capsule()
                                        .fill(Color.blue.opacity(0.075))
                                }
                                .overlay {
                                    Capsule()
                                        .stroke(
                                            Color.blue.opacity(0.2),
                                            lineWidth: 0.65
                                        )
                                }
                                .matchedGeometryEffect(
                                    id: "legacy.tab.selection",
                                    in: selectionNamespace
                                )
                        }
                    }
                }
                .buttonStyle(LegacyGlassTabButtonStyle())
                .accessibilityLabel(
                    titles.indices.contains(index) ? titles[index] : ""
                )
                .accessibilityAddTraits(
                    selection == index ? .isSelected : []
                )
            }
        }
        .padding(4)
        .frame(width: 276)
        .frame(height: 50)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay {
            Capsule()
                .stroke(Color.primary.opacity(0.13), lineWidth: 0.65)
        }
        .shadow(color: .black.opacity(0.12), radius: 12, y: 5)
        .animation(selectionSpring, value: selection)
    }
}

private struct LegacyGlassTabButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.94 : 1)
            .opacity(configuration.isPressed ? 0.78 : 1)
            .animation(
                .spring(
                    response: 0.25,
                    dampingFraction: 0.62,
                    blendDuration: 0.08
                ),
                value: configuration.isPressed
            )
    }
}
#endif

private extension View {
    /// Changes only presentation state. The page remains attached to the same
    /// window and its glass/navigation hierarchies keep their identities.
    func retainedMenuTabPage(_ tab: Int, selection: Int) -> some View {
        let active = selection == tab
        return opacity(active ? 1 : 0)
            .animation(.easeInOut(duration: 0.24), value: active)
            .allowsHitTesting(active)
            .accessibilityHidden(!active)
            .zIndex(active ? 1 : 0)
    }

    func retainedMenuPage(isActive: Bool) -> some View {
        opacity(isActive ? 1 : 0)
            .animation(.easeInOut(duration: 0.24), value: isActive)
            .allowsHitTesting(isActive)
            .accessibilityHidden(!isActive)
            .zIndex(isActive ? 1 : 0)
    }
}

#if !targetEnvironment(macCatalyst)
/// A real UIKit tab bar kept outside every page's GlassEffectContainer.
/// This gives the bar its native system appearance while guaranteeing that it
/// remains a foreground sibling rather than being composited below tab content.
private struct NativeMenuTabBar: UIViewRepresentable {
    @Binding var selection: Int
    let isCompactHeight: Bool
    let titles: [String]
    let onReselect: (Int) -> Void

    private let systemImages = [
        "gamecontroller",
        "cpu",
        "gearshape",
    ]

    func makeCoordinator() -> Coordinator {
        Coordinator(selection: $selection, onReselect: onReselect)
    }

    func makeUIView(context: Context) -> LegacyCompatibleMenuTabBar {
        let tabBar = LegacyCompatibleMenuTabBar()
        tabBar.delegate = context.coordinator
        configureAppearance(of: tabBar)
        tabBar.items = makeItems()
        if let items = tabBar.items, items.indices.contains(selection) {
            tabBar.selectedItem = items[selection]
        }
        tabBar.setSelectedIndex(selection, animated: false)
        return tabBar
    }

    func updateUIView(
        _ tabBar: LegacyCompatibleMenuTabBar,
        context: Context
    ) {
        context.coordinator.selection = $selection
        context.coordinator.onReselect = onReselect
        configureAppearance(of: tabBar)

        if tabBar.items?.count != systemImages.count {
            tabBar.items = makeItems()
        }

        if let items = tabBar.items {
            for index in items.indices where titles.indices.contains(index) {
                items[index].title = titles[index]
                items[index].image = itemImage(at: index)
            }
            if items.indices.contains(selection) {
                tabBar.selectedItem = items[selection]
            }
        }
        tabBar.setSelectedIndex(selection, animated: true)
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: LegacyCompatibleMenuTabBar,
        context: Context
    ) -> CGSize? {
        let width = proposal.width ?? uiView.bounds.width
        let fitted = uiView.sizeThatFits(
            CGSize(width: width, height: CGFloat.greatestFiniteMagnitude)
        )
        return CGSize(width: width, height: max(49, fitted.height))
    }

    private func makeItems() -> [UITabBarItem] {
        systemImages.indices.map { index in
            UITabBarItem(
                title: titles.indices.contains(index) ? titles[index] : nil,
                image: itemImage(at: index),
                tag: index
            )
        }
    }

    private func itemImage(at index: Int) -> UIImage? {
        let configuration = UIImage.SymbolConfiguration(
            pointSize: isCompactHeight ? 25 : 23,
            weight: .regular
        )
        return UIImage(
            systemName: systemImages[index],
            withConfiguration: configuration
        )
    }

    private func configureAppearance(of tabBar: LegacyCompatibleMenuTabBar) {
        tabBar.tintColor = .systemBlue
        tabBar.isTranslucent = true
        tabBar.isOpaque = false
        tabBar.backgroundColor = .clear
        tabBar.backgroundImage = UIImage()
        tabBar.shadowImage = UIImage()
        tabBar.isCompactHeightLayout = isCompactHeight

        let appearance = tabBar.standardAppearance.copy()
        appearance.configureWithTransparentBackground()
        appearance.backgroundColor = .clear
        appearance.backgroundEffect = nil
        appearance.shadowColor = .clear

        if isCompactHeight {
            let normalFont = UIFont.systemFont(ofSize: 13, weight: .regular)
            let selectedFont = UIFont.systemFont(ofSize: 13, weight: .semibold)
            appearance.inlineLayoutAppearance.normal.titleTextAttributes[
                .font
            ] = normalFont
            appearance.inlineLayoutAppearance.selected.titleTextAttributes[
                .font
            ] = selectedFont
            appearance.compactInlineLayoutAppearance.normal.titleTextAttributes[
                .font
            ] = normalFont
            appearance.compactInlineLayoutAppearance.selected.titleTextAttributes[
                .font
            ] = selectedFont
        }

        if #available(iOS 26.0, *) {
            // Tint UIKit's own Liquid Glass selection pill instead of replacing
            // it. Its blur, refraction, morph, and press effects remain native.
            appearance.selectionIndicatorImage = nil
            appearance.selectionIndicatorTintColor =
                UIColor.systemBlue.withAlphaComponent(0.18)
            tabBar.usesLegacySelectionPill = false
        } else {
            // iOS 17/18 has no native floating tab capsule. Suppress UIKit's
            // rectangular bar/indicator and render a material outer pill with
            // a live, animated monochrome-blue selection pill below.
            appearance.selectionIndicatorImage = UIImage()
            appearance.selectionIndicatorTintColor = .clear
            tabBar.usesLegacySelectionPill = true

            let layouts = [
                appearance.stackedLayoutAppearance,
                appearance.inlineLayoutAppearance,
                appearance.compactInlineLayoutAppearance,
            ]
            for layout in layouts {
                layout.normal.iconColor = .secondaryLabel
                layout.normal.titleTextAttributes[.foregroundColor] =
                    UIColor.secondaryLabel
                layout.selected.iconColor = .systemBlue
                layout.selected.titleTextAttributes[.foregroundColor] =
                    UIColor.systemBlue
            }
        }
        tabBar.standardAppearance = appearance
        tabBar.scrollEdgeAppearance = appearance
    }

    final class LegacyCompatibleMenuTabBar: UITabBar {
        var isCompactHeightLayout = false {
            didSet {
                guard oldValue != isCompactHeightLayout else { return }
                setNeedsLayout()
            }
        }

        var usesLegacySelectionPill = false {
            didSet {
                legacyBarPill.isHidden = !usesLegacySelectionPill
                legacySelectionPill.isHidden = !usesLegacySelectionPill
                setNeedsLayout()
            }
        }

        private let legacyBarPill = LegacyBarPillView()
        private let legacySelectionPill = LegacySelectionPillView()
        private var selectedIndex = 0

        override init(frame: CGRect) {
            super.init(frame: frame)
            legacyBarPill.isHidden = true
            legacyBarPill.isUserInteractionEnabled = false
            legacySelectionPill.isHidden = true
            legacySelectionPill.isUserInteractionEnabled = false
            addSubview(legacyBarPill)
            addSubview(legacySelectionPill)
        }

        required init?(coder: NSCoder) {
            super.init(coder: coder)
            legacyBarPill.isHidden = true
            legacyBarPill.isUserInteractionEnabled = false
            legacySelectionPill.isHidden = true
            legacySelectionPill.isUserInteractionEnabled = false
            addSubview(legacyBarPill)
            addSubview(legacySelectionPill)
        }

        func setSelectedIndex(_ index: Int, animated: Bool) {
            let changed = selectedIndex != index
            selectedIndex = index
            updateLegacySelectionPill(
                animated: animated && changed && window != nil
            )
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            updateLegacyBarLayout()
            updateLegacySelectionPill(animated: false)
        }

        private var legacyBarFrame: CGRect {
            let maximumWidth: CGFloat = isCompactHeightLayout ? 420 : 360
            let proportionalWidth = bounds.width * 0.84
            let width = min(
                max(0, bounds.width - 24),
                proportionalWidth,
                maximumWidth
            )
            return CGRect(
                x: (bounds.width - width) / 2,
                y: 2,
                width: width,
                height: max(0, bounds.height - 4)
            )
        }

        private func legacyItemControls() -> [UIControl] {
            subviews
                .compactMap { $0 as? UIControl }
                .sorted { $0.frame.minX < $1.frame.minX }
        }

        private func updateLegacyBarLayout() {
            guard usesLegacySelectionPill else {
                setSystemBarBackgroundHidden(false)
                return
            }

            setSystemBarBackgroundHidden(true)
            let barFrame = legacyBarFrame
            legacyBarPill.frame = barFrame
            legacyBarPill.updateCornerRadius(barFrame.height / 2)

            let itemControls = legacyItemControls()
            guard !itemControls.isEmpty else { return }
            let contentFrame = barFrame.insetBy(dx: 8, dy: 0)
            let itemWidth = contentFrame.width / CGFloat(itemControls.count)
            for (index, control) in itemControls.enumerated() {
                control.frame = CGRect(
                    x: contentFrame.minX + (CGFloat(index) * itemWidth),
                    y: contentFrame.minY,
                    width: itemWidth,
                    height: contentFrame.height
                )
            }

            insertSubview(legacyBarPill, at: 0)
            if let firstItem = itemControls.first {
                insertSubview(legacySelectionPill, belowSubview: firstItem)
            }
        }

        private func setSystemBarBackgroundHidden(_ hidden: Bool) {
            for view in subviews {
                let className = NSStringFromClass(type(of: view))
                if className.contains("BarBackground") {
                    view.isHidden = hidden
                }
            }
        }

        private func updateLegacySelectionPill(animated: Bool) {
            guard usesLegacySelectionPill else { return }

            let itemControls = legacyItemControls()
            guard itemControls.indices.contains(selectedIndex) else {
                return
            }

            if let firstItem = itemControls.first {
                insertSubview(legacySelectionPill, belowSubview: firstItem)
            }

            let itemFrame = itemControls[selectedIndex].frame
            // Slightly overlap each slot so the selected pill feels broader
            // than the icon/title pair while remaining inside the outer pill.
            let proposedFrame = itemFrame.insetBy(dx: -5, dy: 4)
            let targetFrame = proposedFrame.intersection(
                legacyBarFrame.insetBy(dx: 4, dy: 4)
            )
            legacySelectionPill.updateCornerRadius(
                max(0, targetFrame.height / 2)
            )

            let updates = {
                self.legacySelectionPill.frame = targetFrame
                self.legacySelectionPill.alpha = 1
            }
            if animated {
                UIView.animate(
                    withDuration: 0.36,
                    delay: 0,
                    usingSpringWithDamping: 0.82,
                    initialSpringVelocity: 0.18,
                    options: [.beginFromCurrentState, .allowUserInteraction],
                    animations: updates
                )
            } else {
                updates()
            }
        }
    }

    final class LegacyBarPillView: UIView {
        private let materialView = UIVisualEffectView(
            effect: UIBlurEffect(style: .systemUltraThinMaterial)
        )

        override init(frame: CGRect) {
            super.init(frame: frame)
            isOpaque = false

            layer.shadowColor = UIColor.black.cgColor
            layer.shadowOpacity = 0.2
            layer.shadowRadius = 14
            layer.shadowOffset = CGSize(width: 0, height: 6)

            materialView.isUserInteractionEnabled = false
            materialView.clipsToBounds = true
            addSubview(materialView)
        }

        required init?(coder: NSCoder) {
            nil
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            materialView.frame = bounds
            layer.shadowPath = UIBezierPath(
                roundedRect: bounds,
                cornerRadius: layer.cornerRadius
            ).cgPath
        }

        func updateCornerRadius(_ radius: CGFloat) {
            layer.cornerRadius = radius
            materialView.layer.cornerRadius = radius
            materialView.layer.borderWidth = 0.65
            materialView.layer.borderColor =
                UIColor.separator.withAlphaComponent(0.22).cgColor
            setNeedsLayout()
        }
    }

    final class LegacySelectionPillView: UIView {
        private let materialView = UIVisualEffectView(
            effect: UIBlurEffect(style: .systemUltraThinMaterial)
        )
        private let blueTintView = UIView()

        override init(frame: CGRect) {
            super.init(frame: frame)
            isOpaque = false

            layer.shadowColor = UIColor.systemBlue.cgColor
            layer.shadowOpacity = 0.1
            layer.shadowRadius = 8
            layer.shadowOffset = CGSize(width: 0, height: 2)

            materialView.isUserInteractionEnabled = false
            materialView.clipsToBounds = true
            addSubview(materialView)

            blueTintView.backgroundColor =
                UIColor.systemBlue.withAlphaComponent(0.11)
            materialView.contentView.addSubview(blueTintView)
        }

        required init?(coder: NSCoder) {
            nil
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            materialView.frame = bounds
            blueTintView.frame = materialView.bounds
            layer.shadowPath = UIBezierPath(
                roundedRect: bounds,
                cornerRadius: layer.cornerRadius
            ).cgPath
        }

        func updateCornerRadius(_ radius: CGFloat) {
            layer.cornerRadius = radius
            materialView.layer.cornerRadius = radius
            materialView.layer.borderWidth = 0.75
            materialView.layer.borderColor =
                UIColor.systemBlue.withAlphaComponent(0.2).cgColor
            setNeedsLayout()
        }
    }

    final class Coordinator: NSObject, UITabBarDelegate {
        var selection: Binding<Int>
        var onReselect: (Int) -> Void

        init(selection: Binding<Int>, onReselect: @escaping (Int) -> Void) {
            self.selection = selection
            self.onReselect = onReselect
        }

        func tabBar(_ tabBar: UITabBar, didSelect item: UITabBarItem) {
            if selection.wrappedValue == item.tag {
                onReselect(item.tag)
            } else {
                selection.wrappedValue = item.tag
            }
        }
    }
}
#endif

#if !targetEnvironment(macCatalyst)
/// Recognizes horizontal tab swipes without cancelling touches in the active
/// List. Sliders, the horizontal cover flow, and navigation-edge gestures keep
/// ownership of their own drags.
@MainActor
private struct NativeMenuTabSwipeRecognizer: UIViewRepresentable {
    let onSwipe: (CGFloat) -> Void

    func makeUIView(context: Context) -> InstallerView {
        let view = InstallerView()
        view.onSwipe = onSwipe
        return view
    }

    func updateUIView(_ uiView: InstallerView, context: Context) {
        uiView.onSwipe = onSwipe
    }

    static func dismantleUIView(_ uiView: InstallerView, coordinator: ()) {
        uiView.uninstall()
    }

    final class InstallerView: UIView, UIGestureRecognizerDelegate {
        var onSwipe: (CGFloat) -> Void = { _ in }
        private weak var gestureHost: UIView?
        private lazy var panGesture: UIPanGestureRecognizer = {
            let gesture = UIPanGestureRecognizer(
                target: self,
                action: #selector(handlePan(_:))
            )
            gesture.cancelsTouchesInView = false
            gesture.delaysTouchesBegan = false
            gesture.delaysTouchesEnded = false
            gesture.delegate = self
            return gesture
        }()

        override func didMoveToWindow() {
            super.didMoveToWindow()
            install(on: window == nil ? nil : nearestViewController()?.view ?? window)
        }

        func uninstall() {
            gestureHost?.removeGestureRecognizer(panGesture)
            gestureHost = nil
        }

        private func install(on host: UIView?) {
            guard gestureHost !== host else { return }
            uninstall()
            guard let host else { return }
            host.addGestureRecognizer(panGesture)
            gestureHost = host
        }

        @objc
        private func handlePan(_ gesture: UIPanGestureRecognizer) {
            guard gesture.state == .ended, let gestureHost else { return }
            let translation = gesture.translation(in: gestureHost)
            let velocity = gesture.velocity(in: gestureHost)
            let projectedTranslation = translation.x + velocity.x * 0.12
            let usesDirectTranslation = abs(translation.x) >= 64
            let direction = usesDirectTranslation ? translation.x : projectedTranslation
            guard abs(direction) >= (usesDirectTranslation ? 64 : 96) else { return }
            onSwipe(direction)
        }

        override func gestureRecognizerShouldBegin(
            _ gestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            guard let pan = gestureRecognizer as? UIPanGestureRecognizer,
                  let gestureHost else {
                return false
            }
            let velocity = pan.velocity(in: gestureHost)
            return abs(velocity.x) > abs(velocity.y) * 1.25
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            guard let gestureHost,
                  let touchView = touch.view,
                  touchView === gestureHost || touchView.isDescendant(of: gestureHost) else {
                return false
            }

            let location = touch.location(in: gestureHost)
            let protectedLeadingEdge = gestureHost.safeAreaInsets.left + 24
            let protectedTrailingEdge = gestureHost.bounds.width
                - gestureHost.safeAreaInsets.right
                - 24
            guard location.x > protectedLeadingEdge,
                  location.x < protectedTrailingEdge else {
                return false
            }

            var touchedView: UIView? = touchView
            while let view = touchedView, view !== gestureHost {
                if view is UIControl || view is UITabBar {
                    return false
                }
                if let scrollView = view as? UIScrollView,
                   scrollView.isScrollEnabled,
                   (
                       scrollView.alwaysBounceHorizontal
                           || scrollView.contentSize.width > scrollView.bounds.width + 24
                   ) {
                    return false
                }
                touchedView = view.superview
            }
            return true
        }

        private func nearestViewController() -> UIViewController? {
            var responder: UIResponder? = self
            while let current = responder {
                if let controller = current as? UIViewController {
                    return controller
                }
                responder = current.next
            }
            return nil
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            true
        }
    }
}
#endif

@MainActor
private struct SafeAreaProtectedMenuTabContent<Content: View>: View {
    @Environment(\.layoutDirection) private var layoutDirection
    @State private var safeAreaInsets = KeyWindowSafeArea.horizontalInsets()
    let appliesLegacyLandscapeInsets: Bool
    let content: Content

    init(
        appliesLegacyLandscapeInsets: Bool = true,
        @ViewBuilder content: () -> Content
    ) {
        self.appliesLegacyLandscapeInsets = appliesLegacyLandscapeInsets
        self.content = content()
    }

    var body: some View {
        // Pre-iOS 26, SwiftUI reports no horizontal safe-area inset for a TabView page in
        // landscape, so a bare list slides under the notch and we pad it manually from the
        // key-window insets. On iOS 26+ SwiftUI gets it right, and padding again would
        // double-inset the column. Tabs that draw their own edge-to-edge background skip
        // this wrapper entirely (see MenuTabView).
        if !appliesLegacyLandscapeInsets {
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            GeometryReader { geometry in
            let isLandscapePhone = geometry.size.width > geometry.size.height
                && UIDevice.current.userInterfaceIdiom == .phone
            let systemProvidesInset = geometry.safeAreaInsets.leading > 0
                || geometry.safeAreaInsets.trailing > 0
            let insets: (left: CGFloat, right: CGFloat) = {
                guard appliesLegacyLandscapeInsets,
                      isLandscapePhone,
                      !systemProvidesInset else {
                    return (0, 0)
                }
                return NormalTabContentMargin.effectiveHorizontalInsets(
                    raw: safeAreaInsets,
                    isLandscape: true,
                    idiom: .phone
                )
            }()
            content
                .padding(.leading, layoutDirection == .rightToLeft ? insets.right : insets.left)
                .padding(.trailing, layoutDirection == .rightToLeft ? insets.left : insets.right)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .onChange(of: geometry.size) { _, _ in
                    safeAreaInsets = KeyWindowSafeArea.horizontalInsets()
                }
            }
            .onAppear {
                safeAreaInsets = KeyWindowSafeArea.horizontalInsets()
            }
        }
    }
}

/// Reads the real left/right safe-area insets from the active key window. Used because
/// SwiftUI-reported horizontal safe-area insets are unreliable inside a TabView page.
private enum KeyWindowSafeArea {
    @MainActor
    static func horizontalInsets() -> (left: CGFloat, right: CGFloat) {
        let insets = keyWindowInsets()
        return (insets.left, insets.right)
    }

    @MainActor
    private static func keyWindowInsets() -> UIEdgeInsets {
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            guard scene.activationState == .foregroundActive || scene.activationState == .foregroundInactive else { continue }
            if let window = scene.windows.first(where: { $0.isKeyWindow }) ?? scene.windows.first {
                return window.safeAreaInsets
            }
        }
        return .zero
    }
}

/// Adds a small readable horizontal margin for normal app tabs in iPhone landscape.
///
/// Only used on the legacy path of `SafeAreaProtectedMenuTabContent` (pre-iOS 26, where
/// SwiftUI reports no horizontal safe-area inset in a TabView page). The raw key-window
/// insets only just clear the notch, so this pads each side a bit more. Portrait, iPad,
/// and gameplay surfaces are unaffected.
private enum NormalTabContentMargin {
    /// Minimum horizontal content margin for normal app tabs on an iPhone in landscape.
    static let minimumLandscapeMargin: CGFloat = 20

    static func effectiveHorizontalInsets(
        raw: (left: CGFloat, right: CGFloat),
        isLandscape: Bool,
        idiom: UIUserInterfaceIdiom
    ) -> (left: CGFloat, right: CGFloat) {
        guard isLandscape, idiom == .phone else { return raw }
        return (max(raw.left, minimumLandscapeMargin), max(raw.right, minimumLandscapeMargin))
    }
}

#if targetEnvironment(macCatalyst)
private struct CatalystMenuTabBar: View {
    @Binding var selectedTab: Int
    @State private var settings = SettingsStore.shared

    private let tabs = [
        (0, "Games"),
        (1, "BIOS"),
        (2, "Settings"),
    ]

    var body: some View {
        HStack(spacing: 2) {
            ForEach(tabs, id: \.0) { tab in
                Button {
                    selectedTab = tab.0
                } label: {
                    Text(settings.localized(tab.1))
                        .font(.callout)
                        .fontWeight(selectedTab == tab.0 ? .semibold : .regular)
                        .foregroundStyle(.primary)
                        .frame(minWidth: 82)
                        .padding(.vertical, 6)
                        .background {
                            if selectedTab == tab.0 {
                                Capsule()
                                    .fill(Color.primary.opacity(0.12))
                            }
                        }
                }
                .buttonStyle(.plain)

                if tab.0 != tabs.last?.0 {
                    Divider()
                        .frame(height: 20)
                }
            }
        }
        .padding(4)
        .background(.regularMaterial, in: Capsule())
        .shadow(color: .black.opacity(0.08), radius: 18, y: 8)
    }
}
#endif
