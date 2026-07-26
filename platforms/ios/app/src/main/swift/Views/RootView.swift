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

extension EnvironmentValues {
    var menuTabIsActive: Bool {
        get { self[MenuTabIsActiveEnvironmentKey.self] }
        set { self[MenuTabIsActiveEnvironmentKey.self] = newValue }
    }

    var menuBackgroundSessionStart: Date {
        get { self[MenuBackgroundSessionStartEnvironmentKey.self] }
        set { self[MenuBackgroundSessionStartEnvironmentKey.self] = newValue }
    }
}

struct RootView: View {
    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBootSplash = true
    @StateObject private var backgroundHost = PersistentMenuBackgroundHost()

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

            switch appState.currentScreen {
            case .menu:
                MenuTabView(backgroundHost: backgroundHost)
            case .playing:
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
    }
}

/// Animates one rasterized Liquid Glass library card while the real menu tree is
/// already being dismantled underneath it. The live Metal view mounts immediately,
/// and the card cross-fades away as soon as the VM reaches its running state.
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

                Image(uiImage: transition.snapshot)
                    .resizable()
                    .interpolation(.high)
                    .frame(
                        width: transition.sourceFrame.width,
                        height: transition.sourceFrame.height
                    )
                    .clipShape(
                        RoundedRectangle(
                            cornerRadius: transition.cornerRadius,
                            style: .continuous
                        )
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
        // Preserve enough time for the zoom to read visually, but never keep the
        // overlay over a game intro while waiting for optional startup work.
        try? await Task.sleep(nanoseconds: 240_000_000)
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

struct MenuTabView: View {
    @State private var settings = SettingsStore.shared
    @State private var selectedTab = 0
    @State private var settingsRootResetRequest = 0
    @ObservedObject var backgroundHost: PersistentMenuBackgroundHost
    @Environment(\.layoutDirection) private var layoutDirection

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

    private var tabSelection: Binding<Int> {
        Binding(
            get: { selectedTab },
            set: { selectTab($0) }
        )
    }

    private func selectTab(_ tab: Int) {
        guard (0...2).contains(tab), tab != selectedTab else { return }
        withAnimation(.easeInOut(duration: 0.24)) {
            selectedTab = tab
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
                    // Games and BIOS share one persistent native navigation bar.
                    // Stable toolbar IDs let the system morph its Liquid Glass
                    // shapes while both tab pages remain mounted underneath.
                    NavigationStack {
                        ZStack {
                            GameListView(embeddedInMenuNavigation: true)
                                .environment(\.menuTabIsActive, selectedTab == 0)
                                .retainedMenuTabPage(0, selection: selectedTab)

                            SafeAreaProtectedMenuTabContent(
                                appliesLegacyLandscapeInsets: !biosBackgroundActive
                            ) {
                                BIOSListView(embeddedInMenuNavigation: true)
                            }
                            .environment(\.menuTabIsActive, selectedTab == 1)
                            .retainedMenuTabPage(1, selection: selectedTab)
                        }
                        .navigationTitle(
                            settings.localized(selectedTab == 1 ? "BIOS" : "Games")
                        )
                        .toolbarBackground(
                            (selectedTab == 0
                                ? settings.hasCustomBackground
                                : biosBackgroundActive) ? .hidden : .automatic,
                            for: .navigationBar
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
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .tint(.blue)
                .contentMargins(
                    .bottom,
                    tabContentBottomMargin,
                    for: .scrollContent
                )
                .safeAreaInset(edge: .bottom, spacing: tabBarContentSpacing) {
                    NativeMenuTabBar(
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
                    .zIndex(1_000)
                }
#endif
            }
        }
        .environment(\.menuBackgroundHost, backgroundHost)
        .environment(\.menuBackgroundSessionStart, backgroundHost.sessionStart)
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
    }
}

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

    func makeUIView(context: Context) -> UITabBar {
        let tabBar = UITabBar()
        tabBar.delegate = context.coordinator
        tabBar.items = makeItems()
        if let items = tabBar.items, items.indices.contains(selection) {
            tabBar.selectedItem = items[selection]
        }
        return tabBar
    }

    func updateUIView(_ tabBar: UITabBar, context: Context) {
        context.coordinator.selection = $selection
        context.coordinator.onReselect = onReselect

        if tabBar.items?.count != systemImages.count {
            tabBar.items = makeItems()
        }

        if let items = tabBar.items {
            for index in items.indices where titles.indices.contains(index) {
                items[index].title = titles[index]
                items[index].image = UIImage(systemName: systemImages[index])
            }
            if items.indices.contains(selection) {
                tabBar.selectedItem = items[selection]
            }
        }
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: UITabBar,
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
                image: UIImage(systemName: systemImages[index]),
                tag: index
            )
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
