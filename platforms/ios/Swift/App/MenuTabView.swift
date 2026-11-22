// MenuTabView.swift — Games, BIOS and Settings tabs
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import Observation

private struct MenuTabControllerCommandListener: View {
    let controllerInput: MenuControllerInputRouter
    let onEvent: (MenuControllerInputEvent) -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput.latestEvent) { _, event in
                guard let event else { return }
                onEvent(event)
            }
    }
}

@MainActor
@Observable
final class MenuControllerTabFocusState {
    var focusedIndex = 0

    init(focusedIndex: Int = 0) {
        self.focusedIndex = focusedIndex
    }
}

struct MenuBottomTabBarLayout {
    let height: CGFloat
    let iconScale: CGFloat
    let labelScale: CGFloat
    let bottomClearance: CGFloat
    let showsLabels: Bool
}

struct MenuTabView: View {
    @State private var settings = SettingsStore.shared
    @State private var logoStore = ARMSX2LogoStore.shared
    @Binding var selectedTab: Int
    @Binding var settingsNavigationPath: [SettingsPane]
    @State private var titleMorphSelection = 0
    @State private var settingsRootResetRequest = 0
    @State private var settingsNavigationHasDestination = false
    @State private var menuLargeTitleMorphActive = false
    @State private var menuLargeTitleMorphResetTask: Task<Void, Never>?
    @State private var controllerTabEntryTask: Task<Void, Never>?
    @State private var gameRenamePresented = false
    @State private var controllerTabFocusState: MenuControllerTabFocusState
    @State private var menuViewportSize = CGSize.zero
    @State private var tabBar = MenuTabBarAppearance.shared
    @State private var appearance = GameLibraryAppearance.shared
    let backgroundHost: PersistentMenuBackgroundHost
    let controllerInput: MenuControllerInputRouter
    @Namespace private var largeTitleNamespace
    @Environment(\.layoutDirection) private var layoutDirection
    @Environment(\.verticalSizeClass) private var verticalSizeClass

    init(
        backgroundHost: PersistentMenuBackgroundHost,
        controllerInput: MenuControllerInputRouter,
        selectedTab: Binding<Int>,
        settingsNavigationPath: Binding<[SettingsPane]>
    ) {
        self.backgroundHost = backgroundHost
        self.controllerInput = controllerInput
        self._selectedTab = selectedTab
        self._settingsNavigationPath = settingsNavigationPath
        self._titleMorphSelection = State(initialValue: selectedTab.wrappedValue)
        self._controllerTabFocusState = State(
            initialValue: MenuControllerTabFocusState(
                focusedIndex: selectedTab.wrappedValue
            )
        )
    }

    private var biosBackgroundActive: Bool { settings.hasCustomBackground && settings.appearance.backgroundEnabledInBIOS }
    private var settingsBackgroundActive: Bool { settings.hasCustomBackground && settings.appearance.backgroundEnabledInSettings }

    private var hideGamesScreenTitle: Bool {
        let isLandscape = menuViewportSize.width > menuViewportSize.height
        let stored = isLandscape
            ? appearance.landscapeHideGamesScreenTitle
            : appearance.portraitHideGamesScreenTitle
        return stored >= 0 ? stored == 1 : appearance.legacyHideGamesScreenTitle
    }

    private var useGamesLogo: Bool {
        let isLandscape = menuViewportSize.width > menuViewportSize.height
        let stored = isLandscape
            ? appearance.landscapeUseGamesLogo
            : appearance.portraitUseGamesLogo
        return logoStore.hasLogo && (stored < 0 || stored == 1)
    }

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

    private var bottomTabBarLayout: MenuBottomTabBarLayout {
        let isLandscape = menuViewportSize == .zero
            ? verticalSizeClass == .compact
            : menuViewportSize.width > menuViewportSize.height
        let height = isLandscape
            ? tabBar.landscapeBottomNavigationTabBarHeight
            : tabBar.portraitBottomNavigationTabBarHeight
        let iconScale = isLandscape
            ? tabBar.landscapeBottomNavigationTabBarIconScale
            : tabBar.portraitBottomNavigationTabBarIconScale
        let labelScale = isLandscape
            ? tabBar.landscapeBottomNavigationTabBarLabelScale
            : tabBar.portraitBottomNavigationTabBarLabelScale
        let clearance = isLandscape
            ? tabBar.landscapeBottomNavigationTabBarClearance
            : tabBar.portraitBottomNavigationTabBarClearance
        let storedShowsLabels = isLandscape
            ? tabBar.landscapeBottomNavigationTabBarShowsLabels
            : tabBar.portraitBottomNavigationTabBarShowsLabels

        return MenuBottomTabBarLayout(
            height: CGFloat(min(max(height, 42), 100)),
            iconScale: CGFloat(min(max(iconScale, 0.75), 1.5)),
            labelScale: CGFloat(min(max(labelScale, 0.75), 1.5)),
            bottomClearance: CGFloat(min(max(clearance, -32), 32)),
            showsLabels: storedShowsLabels != 0
        )
    }

    private var tabContentBottomMargin: CGFloat {
        max(
            96,
            bottomTabBarLayout.height
                + bottomTabBarLayout.bottomClearance
                + 46
        )
    }

    private var tabBarContentSpacing: CGFloat {
        20
    }

    /// Settings lists don't get the tab bar's safe area inset and run on to the bottom of the
    /// safe area, so they need room for the part the bar covers. Their own bottom padding is the gap.
    private var settingsTabBarContentMargin: CGFloat {
#if targetEnvironment(macCatalyst)
        let offset: CGFloat = 0
#else
        let offset = verticalSizeClass == .compact ? ControllerFocusedMenuTabBar.compactOffset : 0
#endif
        return bottomTabBarLayout.height + bottomTabBarLayout.bottomClearance - offset
    }

    private var usesPageOwnedLibraryLargeTitle: Bool {
        !(selectedTab == 0 && hideGamesScreenTitle)
            && verticalSizeClass != .compact
            && UIDevice.current.userInterfaceIdiom == .phone
    }

    /// Landscape is the expensive horizontal cover-flow presentation in this
    /// menu (including regular-size-class iPads).
    /// Cross-fading complete retained pages forces every large glass card into
    /// one full-screen compositing group. Portrait can afford that transition;
    /// landscape switches page visibility without animating the page raster.
    private var animatesRetainedPageTransitions: Bool {
        let usesLandscapeLayout = menuViewportSize == .zero
            ? verticalSizeClass == .compact
            : menuViewportSize.width > menuViewportSize.height
        return !usesLandscapeLayout
    }

    private var tabSelection: Binding<Int> {
        Binding(
            get: { selectedTab },
            set: { selectTab($0) }
        )
    }

    /// Passing nil dismantles every controller probe, focus overlay, geometry
    /// sampler, and right-stick target while touch owns the interface. The
    /// router retains only its lightweight physical-input callback so it can
    /// offer the explicit controller-mode handoff.
    private var activeControllerInput: MenuControllerInputRouter? {
        controllerInput.hasConnectedController
            && controllerInput.isControllerNavigationEnabled
            && controllerInput.pendingNavigationModeSwitchRequest?.mode
                != .controller
            ? controllerInput
            : nil
    }

    private func selectTab(
        _ tab: Int,
        playsAudio: Bool = true,
        entersControllerContent: Bool = true
    ) {
        guard (0...2).contains(tab), tab != selectedTab else { return }
        if selectedTab == 0 {
            CoverThumbnailCache.shared.releaseInactiveLibraryImages()
        }
        if playsAudio {
            MenuAudioPackManager.shared.playEvent(.tabTransition)
        }
        controllerTabEntryTask?.cancel()
        controllerTabEntryTask = nil
        if controllerInput.hasConnectedController,
           controllerInput.isControllerNavigationEnabled {
            // Publish the new tab-bar focus before the content selection. This
            // keeps UIKit and the custom orb anchor on one index throughout
            // the retained-page transaction instead of showing the old tab.
            controllerTabFocusState.focusedIndex = tab
        }
        menuLargeTitleMorphResetTask?.cancel()
        menuLargeTitleMorphActive = true
        if animatesRetainedPageTransitions {
            withAnimation(.easeInOut(duration: 0.24)) {
                titleMorphSelection = tab
                selectedTab = tab
            }
        } else {
            // Only the title and toolbar morph in landscape; the pages skip the transaction.
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                selectedTab = tab
            }
            withAnimation(.easeInOut(duration: 0.24)) {
                titleMorphSelection = tab
            }
        }
        if controllerInput.hasConnectedController,
           controllerInput.isControllerNavigationEnabled,
           entersControllerContent {
            // Focus the page first, so a Down sent while it mounts moves on from Language.
            scheduleControllerContentEntry(for: tab)
        }
        menuLargeTitleMorphResetTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(280))
            guard !Task.isCancelled else { return }
            menuLargeTitleMorphActive = false
            menuLargeTitleMorphResetTask = nil
        }
    }

    private func scheduleControllerContentEntry(for tab: Int) {
        guard (0...2).contains(tab) else { return }
        controllerTabEntryTask = Task { @MainActor in
            await Task.yield()
            guard !Task.isCancelled, selectedTab == tab else { return }
            if tab == 0 {
                controllerInput.requestLibraryEntry(preferLast: false)
            } else {
                _ = controllerInput.requestNavigationSessionEntry(
                    preferLast: false,
                    matchingScopePrefix: tab == 1 ? "menu.bios" : "settings."
                )
            }
            controllerTabEntryTask = nil
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
                    CatalystMenuTabBar(selectedTab: tabSelection)
                        .padding(.top, 8)
                        .padding(.bottom, 8)
                        .opacity(gameRenamePresented ? 0 : 1)
                        .allowsHitTesting(!gameRenamePresented)
                        .accessibilityHidden(gameRenamePresented)

                    Group {
                        switch selectedTab {
                        case 0:
                            GameListView(
                                controllerInput: activeControllerInput,
                                onRenamePresentationChanged: {
                                    gameRenamePresented = $0
                                }
                            )
                            .dynamicTypeSize(...DynamicTypeSize.accessibility3)
                        case 1:
                            BIOSListView(
                                controllerInput: activeControllerInput,
                                onPreviousControllerTab:
                                    selectPreviousControllerTab,
                                onNextControllerTab:
                                    selectNextControllerTab,
                                onControllerBoundary:
                                    handleSharedControllerBoundary
                            )
                        default:
                            SettingsRootView(
                                navigationPath: $settingsNavigationPath,
                                resetToRootRequest: settingsRootResetRequest,
                                onNavigationPathActivityChanged: {
                                    settingsNavigationHasDestination = $0
                                },
                                controllerInput: activeControllerInput,
                                onPreviousControllerTab: selectPreviousControllerTab,
                                onNextControllerTab: selectNextControllerTab,
                                onControllerBoundary: handleSharedControllerBoundary
                            )
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
#else
                ZStack {
                    // Only the selected page stays in the hierarchy; three retained stacks left
                    // Games chrome on BIOS and overlapping focus targets in Settings.
                    if selectedTab != 2 {
                        NavigationStack {
                            Group {
                                if selectedTab == 0 {
                                    GameListView(
                                        embeddedInMenuNavigation: true,
                                        ownsEmbeddedMenuToolbar: true,
                                        controllerInput: activeControllerInput,
                                        onRenamePresentationChanged: {
                                            gameRenamePresented = $0
                                        }
                                    )
                                        // Past AX3 the favorite star covers the cover art.
                                        .dynamicTypeSize(...DynamicTypeSize.accessibility3)
                                        .environment(\.menuTabIsActive, true)
                                        .environment(
                                            \.menuLargeTitleIsSource,
                                            titleMorphSelection == 0
                                        )
                                } else {
                                    SafeAreaProtectedMenuTabContent(
                                        appliesLegacyLandscapeInsets:
                                            !biosBackgroundActive
                                    ) {
                                        BIOSListView(
                                            embeddedInMenuNavigation: true,
                                            ownsEmbeddedMenuToolbar: true,
                                            controllerInput:
                                                activeControllerInput,
                                            onPreviousControllerTab:
                                                selectPreviousControllerTab,
                                            onNextControllerTab:
                                                selectNextControllerTab,
                                            onControllerBoundary:
                                                handleSharedControllerBoundary
                                        )
                                    }
                                        .environment(\.menuTabIsActive, true)
                                        .environment(
                                            \.menuLargeTitleIsSource,
                                            titleMorphSelection == 1
                                        )
                                }
                            }
                            // Keep the page-owned Games/BIOS title in the same
                            // content coordinate space as Settings.
                            .safeAreaInset(edge: .top) {
                                Color.clear.frame(
                                    height:
                                        usesPageOwnedLibraryLargeTitle ? 6 : 0
                                )
                            }
                            .navigationTitle(
                                settings.localized(
                                    usesPageOwnedLibraryLargeTitle
                                        || (selectedTab == 0 && hideGamesScreenTitle)
                                        || (selectedTab == 0 && useGamesLogo)
                                        ? ""
                                        : (selectedTab == 1 ? "BIOS" : "Games")
                                )
                            )
                            .toolbarBackground(
                                (selectedTab == 0
                                    ? settings.hasCustomBackground
                                    : biosBackgroundActive) ? .hidden : .automatic,
                                for: .navigationBar
                            )
                            .toolbar(
                                gameRenamePresented ? .hidden : .automatic,
                                for: .navigationBar
                            )
                            .navigationBarTitleDisplayMode(
                                usesPageOwnedLibraryLargeTitle
                                    || (selectedTab == 0 && hideGamesScreenTitle)
                                    || (selectedTab == 0 && useGamesLogo)
                                    ? .inline : .large
                            )
                            .toolbar {
                                if verticalSizeClass == .compact,
                                   !(selectedTab == 0 && hideGamesScreenTitle) {
                                    ToolbarItem(
                                        id: "menu.collapsedTitle",
                                        placement: .principal
                                    ) {
                                        EmbeddedMenuCompactTitle(
                                            title: settings.localized(
                                                selectedTab == 1
                                                    ? "BIOS"
                                                    : "Games"
                                            ),
                                            usesARMSX2Logo: false
                                        )
                                    }
                                }
                            }
                        }
                        // Games and BIOS intentionally get distinct navigation
                        // owners. No native bar/list state is carried across tabs.
                        .id("library-tab.\(selectedTab)")
                        .environment(
                            \.menuLargeTitleIsSource,
                            titleMorphSelection != 2
                        )
                    } else {
                        SafeAreaProtectedMenuTabContent(
                            appliesLegacyLandscapeInsets: !settingsBackgroundActive
                        ) {
                            SettingsRootView(
                                navigationPath: $settingsNavigationPath,
                                resetToRootRequest: settingsRootResetRequest,
                                onNavigationPathActivityChanged: {
                                    settingsNavigationHasDestination = $0
                                },
                                controllerInput: activeControllerInput,
                                onPreviousControllerTab: selectPreviousControllerTab,
                                onNextControllerTab: selectNextControllerTab,
                                onControllerBoundary: handleSharedControllerBoundary
                            )
                        }
                        .environment(\.menuTabIsActive, true)
                        .environment(
                            \.menuLargeTitleIsSource,
                            titleMorphSelection == 2
                        )
                    }

                    NativeMenuTabSwipeRecognizer(
                        isEnabled: selectedTab != 2 || !settingsNavigationHasDestination,
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
                .contentMargins(
                    .bottom,
                    tabContentBottomMargin,
                    for: .scrollContent
                )
                .environment(\.menuTabBarContentMargin, settingsTabBarContentMargin)
                .safeAreaInset(edge: .bottom, spacing: tabBarContentSpacing) {
                    ControllerFocusedMenuTabBar(
                        selection: tabSelection,
                        focusState: controllerTabFocusState,
                        controllerInput: controllerInput,
                        layout: bottomTabBarLayout,
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
                    .frame(maxWidth: .infinity)
                    .centredInLeadingPaneOnFold()
                    .opacity(gameRenamePresented ? 0 : 1)
                    .allowsHitTesting(!gameRenamePresented)
                    .accessibilityHidden(gameRenamePresented)
                    // Retain the UIKit tab bar while OrbitKeys is presented so
                    // its title attributes and item identities survive dismissal.
                    .transaction { transaction in
                        transaction.animation = nil
                    }
                    .zIndex(1_000)
                }
#endif
            }
        }
        .environment(\.menuBackgroundHost, backgroundHost)
        .overlay {
            if activeControllerInput != nil {
                MenuTabControllerCommandListener(
                    controllerInput: controllerInput,
                    onEvent: handleControllerInputEvent
                )
            }
        }
        .background {
            GeometryReader { proxy in
                Color.clear
                    .allowsHitTesting(false)
                    .onAppear {
                        menuViewportSize = proxy.size
                    }
                    .onChange(of: proxy.size) { _, size in
                        guard menuViewportSize != size else { return }
                        menuViewportSize = size
                    }
            }
            // Otherwise a keyboard shortens the menu and portrait gets the landscape layout.
            .ignoresSafeArea(.keyboard)
        }
        // One foreground field owns every controller-focus anchor in this
        // presentation, including cards, toolbars, alerts, and the tab bar.
        .controllerNavigationOrbOverlay(
            controllerInput: gameRenamePresented ? nil : activeControllerInput
        )
        .overlay(alignment: .top) {
            // Theme Gallery observation and its modal state are unnecessary
            // while a Settings destination owns the whole content surface.
            // Remove the subtree instead of retaining it disabled.
            if !gameRenamePresented
                && (selectedTab != 2 || !settingsNavigationHasDestination) {
                MenuThemePresetShortcutOverlay(
                    controllerInput: controllerInput,
                    isEnabled: true
                )
                .padding(.horizontal, 24)
                .padding(.top, 12)
            }
        }
        .onAppear {
            // BIOSListView performs its own refresh when mounted. Avoid disk
            // and metadata work while Games or Settings is the selected page.
            if selectedTab == 1 {
                BIOSLibraryState.shared.refreshIfNeeded()
            }
            backgroundHost.reactivateForMenu(isAvailable: settings.hasCustomBackground)
            backgroundHost.setPresentationVisible(selectedTabShowsBackground)
        }
        .onChange(of: settings.hasCustomBackground) { _, hasBackground in
            backgroundHost.setMenuBackgroundAvailable(hasBackground)
        }
        .onChange(of: settings.appearance.dynamicBackgroundsEnabled) { _, _ in
            backgroundHost.setMenuBackgroundAvailable(settings.hasCustomBackground)
        }
        .onChange(of: selectedTabShowsBackground) { _, showsBackground in
            backgroundHost.setPresentationVisible(showsBackground)
        }
        .onDisappear {
            controllerTabEntryTask?.cancel()
            controllerTabEntryTask = nil
            menuLargeTitleMorphResetTask?.cancel()
            menuLargeTitleMorphResetTask = nil
            menuLargeTitleMorphActive = false
        }
    }

    private func handleControllerInputEvent(
        _ event: MenuControllerInputEvent
    ) {
        guard event.captureOwner == nil,
              !event.isNavigationSessionRouted else { return }
        // The persistent bottom bar remains the owner of its commands even
        // while the selected BIOS/Settings page has a registered session.
        if controllerInput.navigationZone == .tabBar {
            handleControllerTabBarCommand(event.command)
            return
        }
        guard !controllerInput.isNavigationCaptured else { return }
        switch event.command {
        case .previousTab:
            selectTab((selectedTab + 2) % 3, playsAudio: false)
            controllerTabFocusState.focusedIndex = selectedTab
            controllerInput.playFeedback(.previousTab)
        case .nextTab:
            selectTab((selectedTab + 1) % 3, playsAudio: false)
            controllerTabFocusState.focusedIndex = selectedTab
            controllerInput.playFeedback(.nextTab)
        default:
            break
        }
    }

    @MainActor
    private func selectPreviousControllerTab() -> Bool {
        let next = (selectedTab + 2) % 3
        selectTab(next, playsAudio: false)
        controllerTabFocusState.focusedIndex = next
        controllerInput.playFeedback(.previousTab)
        return true
    }

    @MainActor
    private func selectNextControllerTab() -> Bool {
        let next = (selectedTab + 1) % 3
        selectTab(next, playsAudio: false)
        controllerTabFocusState.focusedIndex = next
        controllerInput.playFeedback(.nextTab)
        return true
    }

    private func handleControllerTabBarCommand(
        _ command: MenuControllerCommand
    ) {
        switch command {
        case .left, .right:
            let physicalStep = command == .left ? -1 : 1
            let step = layoutDirection == .rightToLeft
                ? -physicalStep
                : physicalStep
            let next = min(
                2,
                max(0, controllerTabFocusState.focusedIndex + step)
            )
            guard next != controllerTabFocusState.focusedIndex else {
                controllerInput.playFeedback(.boundary)
                return
            }
            controllerTabFocusState.focusedIndex = next
            controllerInput.playFeedback(.move(command))
        case .activate:
            let focusedIndex = controllerTabFocusState.focusedIndex
            let transitionsTab = selectedTab != focusedIndex
            // Cross opens the tab and moves into its page, as L1 and R1 do from a page.
            if selectedTab == focusedIndex {
                if focusedIndex == 2 {
                    settingsRootResetRequest += 1
                }
                scheduleControllerContentEntry(for: focusedIndex)
            } else {
                selectTab(focusedIndex, playsAudio: false)
            }
            controllerInput.playFeedback(
                transitionsTab ? .tabTransition : .activate
            )
        case .up, .upLeft, .upRight:
            if selectedTab == 0 {
                controllerInput.requestLibraryEntry(preferLast: true)
                controllerInput.playFeedback(.move(.up))
            } else if controllerInput.requestNavigationSessionEntry(
                preferLast: true,
                matchingScopePrefix: selectedTab == 1 ? "menu.bios" : "settings."
            ) {
                controllerInput.playFeedback(.move(.up))
            } else {
                controllerInput.playFeedback(.boundary)
            }
        case .back:
            controllerTabFocusState.focusedIndex = selectedTab
            if selectedTab == 0 {
                controllerInput.setNavigationZone(.library)
            }
            controllerInput.playFeedback(.back)
        case .previousTab:
            controllerTabFocusState.focusedIndex = (
                controllerTabFocusState.focusedIndex + 2
            ) % 3
            selectTab(
                controllerTabFocusState.focusedIndex,
                playsAudio: false,
                entersControllerContent: false
            )
            controllerInput.playFeedback(.previousTab)
        case .nextTab:
            controllerTabFocusState.focusedIndex = (
                controllerTabFocusState.focusedIndex + 1
            ) % 3
            selectTab(
                controllerTabFocusState.focusedIndex,
                playsAudio: false,
                entersControllerContent: false
            )
            controllerInput.playFeedback(.nextTab)
        case .down, .downLeft, .downRight:
            if selectedTab == 2,
               !settingsNavigationHasDestination,
               controllerInput.requestNavigationSessionEntry(
                   preferLast: false,
                   matchingScopePrefix: "settings.root"
               ) {
                controllerInput.playFeedback(.move(.down))
            } else {
                controllerInput.playFeedback(.boundary)
            }
        case .toggleFavorite, .showContextMenu:
            controllerInput.playFeedback(.boundary)
        }
    }

    @MainActor
    private func handleSharedControllerBoundary(
        _ direction: MenuControllerCommand
    ) -> Bool {
        guard direction == .down else { return false }
        controllerTabFocusState.focusedIndex = selectedTab
        controllerInput.setNavigationZone(.tabBar)
        return true
    }
}
