// SettingsRootView.swift — Settings navigation root
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

private enum SettingsStaticInfo {
    static let buildVersion = ARMSX2Bridge.buildVersion()
}

enum SettingsPane: String, CaseIterable, Identifiable {
    case language
    case appearance
    case appIcon
    case emulator
    case graphics
    case shaders
    case framePacing
    case audio
    case network
    case memoryCards
    case storage
    case settingsPresets
    case retroAchievements
    case customSkin
    case overlay
    case gameController
    case controllerMacros
    case localMultiplayer
    case virtualPad
    case help
    case licenses
    case about
    case skinBrowser

    var id: String { rawValue }

    static var rootCases: [SettingsPane] {
        allCases.filter {
            $0 != .skinBrowser && $0 != .controllerMacros
        }
    }

    var title: String {
        switch self {
        case .language:
            return "Language"
        case .appearance:
            return "Appearance"
        case .appIcon:
            return "App Icon"
        case .emulator:
            return "Emulator"
        case .graphics:
            return "Graphics"
        case .shaders:
            return "Shaders"
        case .framePacing:
            return "Frame Pacing"
        case .audio:
            return "Audio"
        case .network:
            return "Network"
        case .memoryCards:
            return "Memory Cards"
        case .storage:
            return "Storage"
        case .settingsPresets:
            return "Settings Presets"
        case .retroAchievements:
            return "RetroAchievements"
        case .customSkin:
            return "Custom Skin"
        case .overlay:
            return "Overlay (OSD)"
        case .gameController:
            return "Game Controller"
        case .controllerMacros:
            return "Controller Macros"
        case .localMultiplayer:
            return "Local Multiplayer"
        case .virtualPad:
            return "Virtual Pad"
        case .help:
            return "Help"
        case .licenses:
            return "Licenses & Credits"
        case .about:
            return "About"
        case .skinBrowser:
            return "Browse Skins"
        }
    }

    var icon: String {
        switch self {
        case .language:
            return "globe"
        case .appearance:
            return "paintpalette"
        case .appIcon:
            return "app.badge"
        case .emulator:
            return "cpu"
        case .graphics:
            return "paintbrush"
        case .shaders:
            return "camera.filters"
        case .framePacing:
            return "speedometer"
        case .audio:
            return "speaker.wave.2"
        case .network:
            return "network"
        case .memoryCards:
            return "memorychip"
        case .storage:
            return "internaldrive"
        case .settingsPresets:
            return "slider.horizontal.3"
        case .retroAchievements:
            return "trophy"
        case .customSkin:
            return "paintpalette"
        case .overlay:
            return "text.below.photo"
        case .gameController:
            return "gamecontroller"
        case .controllerMacros:
            return "command"
        case .localMultiplayer:
            return "person.3"
        case .virtualPad:
            return "hand.draw"
        case .help:
            return "questionmark.circle"
        case .licenses:
            return "doc.text"
        case .about:
            return "info.circle"
        case .skinBrowser:
            return "square.grid.2x2"
        }
    }
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

/// Every Settings destination stays transparent above RootView's one persistent
/// menu renderer. Non-Appearance Forms receive the same independent rounded
/// row glass that Appearance declares directly; no full-screen glass slab is
/// inserted between the list and the background.
private struct SettingsClearGlassListInstaller: UIViewRepresentable {
    let isClear: Bool

    func makeUIView(context: Context) -> UIView {
        let view = SettingsClearGlassListInstallerView()
        view.configure(isClear: isClear)
        view.scheduleInstallation()
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        guard let view = uiView as? SettingsClearGlassListInstallerView else {
            return
        }
        view.configure(isClear: isClear)
        view.scheduleInstallation()
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Void) {
        (uiView as? SettingsClearGlassListInstallerView)?.disconnect()
    }
}

/// Marks a SwiftUI row that owns its Liquid Glass surface. The native Form
/// decorator must not install a second UIVisualEffectView behind such a row.
/// Shader accordions use this because their height changes in place while the
/// glass surface itself must remain mounted.
@MainActor
final class SettingsOwnedClearGlassMarkerView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isUserInteractionEnabled = false
        isAccessibilityElement = false
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

struct SettingsOwnedClearGlassMarker: UIViewRepresentable {
    func makeUIView(context: Context) -> SettingsOwnedClearGlassMarkerView {
        SettingsOwnedClearGlassMarkerView()
    }

    func updateUIView(
        _ uiView: SettingsOwnedClearGlassMarkerView,
        context: Context
    ) {}
}

/// A true background-only rounded surface for Settings rows. Appearance uses a
/// fixed 16-point continuous radius, so compact rows can read as controls while
/// taller explanatory and compound rows remain cards instead of all becoming
/// capsules. Keeping the effect behind the cell leaves text and symbols plain.
@MainActor
private final class SettingsClearGlassRowBackgroundView: UIView {
    private let effectView: UIVisualEffectView
    private var isClear: Bool

    init(isClear: Bool) {
        self.isClear = isClear
        if #available(iOS 26.0, *) {
            effectView = UIVisualEffectView(
                effect: UIGlassEffect(style: isClear ? .clear : .regular)
            )
        } else {
            effectView = UIVisualEffectView(
                effect: UIBlurEffect(
                    style: isClear
                        ? .systemUltraThinMaterial
                        : .systemMaterial
                )
            )
        }
        super.init(frame: .zero)
        backgroundColor = .clear
        isUserInteractionEnabled = false
        effectView.isUserInteractionEnabled = false
        effectView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(effectView)
        NSLayoutConstraint.activate([
            effectView.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            effectView.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            effectView.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            effectView.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func configure(isClear: Bool) {
        guard self.isClear != isClear else { return }
        self.isClear = isClear
        if #available(iOS 26.0, *) {
            effectView.effect = UIGlassEffect(
                style: isClear ? .clear : .regular
            )
        } else {
            effectView.effect = UIBlurEffect(
                style: isClear
                    ? .systemUltraThinMaterial
                    : .systemMaterial
            )
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        effectView.layer.cornerRadius = min(
            16,
            max(0, effectView.bounds.height / 2)
        )
        effectView.layer.cornerCurve = .continuous
        effectView.layer.masksToBounds = true
    }

    /// Form cells animate their height independently from SwiftUI content.
    /// Explicitly following the cell's intermediate bounds keeps the clear
    /// glass sampler attached throughout a shader-card expansion instead of
    /// disappearing until the final layout pass.
    func synchronize() {
        setNeedsLayout()
    }
}

@MainActor
private final class SettingsClearGlassListInstallerView: UIView {
    private weak var listScrollView: UIScrollView?
    private var contentOffsetObservation: NSKeyValueObservation?
    private var contentSizeObservation: NSKeyValueObservation?
    private var pendingInstallation: Task<Void, Never>?
    private var pendingDecoration: Task<Void, Never>?
    private var isClear = true

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isUserInteractionEnabled = false
        isAccessibilityElement = false
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        scheduleInstallation()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        scheduleInstallation()
    }

    func scheduleInstallation() {
        // Coalesce layout invalidations without continually cancelling the
        // already queued installation while a controller scroll is running.
        guard pendingInstallation == nil else { return }
        pendingInstallation = Task { @MainActor [weak self] in
            await Task.yield()
            guard !Task.isCancelled, let self else { return }
            installIfPossible()
            pendingInstallation = nil
        }
    }

    func configure(isClear: Bool) {
        guard self.isClear != isClear else { return }
        self.isClear = isClear
        scheduleVisibleCellDecoration()
    }

    func disconnect() {
        pendingInstallation?.cancel()
        pendingInstallation = nil
        pendingDecoration?.cancel()
        pendingDecoration = nil
        contentOffsetObservation = nil
        contentSizeObservation = nil
        listScrollView = nil
    }

    private func installIfPossible() {
        guard window != nil else {
            disconnect()
            return
        }
        guard let candidate = resolveListScrollView() else { return }
        if listScrollView !== candidate {
            observe(candidate)
        }
        decorateVisibleCells()
    }

    private func observe(_ scrollView: UIScrollView) {
        contentOffsetObservation = nil
        contentSizeObservation = nil
        listScrollView = scrollView

        contentOffsetObservation = scrollView.observe(
            \.contentOffset,
            options: [.new]
        ) { [weak self] _, _ in
            MainActor.assumeIsolated {
                self?.scheduleVisibleCellDecoration()
            }
        }
        contentSizeObservation = scrollView.observe(
            \.contentSize,
            options: [.new]
        ) { [weak self] _, _ in
            MainActor.assumeIsolated {
                self?.scheduleVisibleCellDecoration()
            }
        }
    }

    /// A List can publish several offset and size changes during one rendered
    /// frame. Decorate its newly recycled cells once instead of enqueueing a
    /// complete visible-cell traversal for every KVO callback.
    private func scheduleVisibleCellDecoration() {
        guard pendingDecoration == nil else { return }
        pendingDecoration = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(8))
            guard !Task.isCancelled, let self else { return }
            UIView.performWithoutAnimation {
                self.decorateVisibleCells()
            }
            pendingDecoration = nil
        }
    }

    private func resolveListScrollView() -> UIScrollView? {
        if let listScrollView,
           listScrollView.window === window,
           isListView(listScrollView) {
            return listScrollView
        }
        guard let window else { return nil }
        let markerFrame = convert(bounds, to: window)
        guard markerFrame.width > 1, markerFrame.height > 1 else { return nil }

        var ancestor = superview
        for _ in 0..<8 {
            guard let root = ancestor, root !== window else { break }
            var candidates: [UIScrollView] = []
            collectListViews(
                below: root,
                in: window,
                intersecting: markerFrame,
                into: &candidates
            )
            if let candidate = candidates.max(by: {
                overlapArea(of: $0, with: markerFrame, in: window)
                    < overlapArea(of: $1, with: markerFrame, in: window)
            }) {
                return candidate
            }
            ancestor = root.superview
        }
        return nil
    }

    private func collectListViews(
        below view: UIView,
        in window: UIWindow,
        intersecting markerFrame: CGRect,
        into result: inout [UIScrollView]
    ) {
        guard !view.isHidden,
              view.alpha >= 0.01,
              view.window === window else { return }
        let frame = view.convert(view.bounds, to: window)
        guard frame.intersects(markerFrame) else { return }
        if let scrollView = view as? UIScrollView,
           isListView(scrollView) {
            result.append(scrollView)
        }
        for child in view.subviews.reversed() {
            collectListViews(
                below: child,
                in: window,
                intersecting: markerFrame,
                into: &result
            )
        }
    }

    private func isListView(_ view: UIScrollView) -> Bool {
        view is UICollectionView || view is UITableView
    }

    private func overlapArea(
        of view: UIView,
        with markerFrame: CGRect,
        in window: UIWindow
    ) -> CGFloat {
        let intersection = view.convert(view.bounds, to: window)
            .intersection(markerFrame)
        guard !intersection.isNull else { return 0 }
        return intersection.width * intersection.height
    }

    private func decorateVisibleCells() {
        guard let listScrollView else { return }
        if listScrollView.backgroundColor != .clear {
            listScrollView.backgroundColor = .clear
        }

        if let collectionView = listScrollView as? UICollectionView {
            for cell in collectionView.visibleCells {
                decorate(cell)
            }
        } else if let tableView = listScrollView as? UITableView {
            if tableView.separatorStyle != .none { tableView.separatorStyle = .none }
            for cell in tableView.visibleCells {
                decorate(cell)
            }
        }
    }

    private func decorate(_ cell: UICollectionViewCell) {
        if cell.backgroundColor != .clear { cell.backgroundColor = .clear }
        if cell.contentView.backgroundColor != .clear {
            cell.contentView.backgroundColor = .clear
        }
        if cell.backgroundConfiguration != nil {
            cell.backgroundConfiguration = nil
        }
        if containsOwnedGlassMarker(in: cell.contentView) {
            if cell.backgroundView is SettingsClearGlassRowBackgroundView {
                cell.backgroundView = nil
            }
            return
        }
        if let background = cell.backgroundView
            as? SettingsClearGlassRowBackgroundView {
            background.configure(isClear: isClear)
            background.synchronize()
        } else {
            let background = SettingsClearGlassRowBackgroundView(
                isClear: isClear
            )
            cell.backgroundView = background
            background.synchronize()
        }
    }

    private func decorate(_ cell: UITableViewCell) {
        if cell.backgroundColor != .clear { cell.backgroundColor = .clear }
        if cell.contentView.backgroundColor != .clear {
            cell.contentView.backgroundColor = .clear
        }
        if cell.backgroundConfiguration != nil {
            cell.backgroundConfiguration = nil
        }
        if containsOwnedGlassMarker(in: cell.contentView) {
            if cell.backgroundView is SettingsClearGlassRowBackgroundView {
                cell.backgroundView = nil
            }
            return
        }
        if let background = cell.backgroundView
            as? SettingsClearGlassRowBackgroundView {
            background.configure(isClear: isClear)
            background.synchronize()
        } else {
            let background = SettingsClearGlassRowBackgroundView(
                isClear: isClear
            )
            cell.backgroundView = background
            background.synchronize()
        }
        if cell.selectedBackgroundView == nil {
            cell.selectedBackgroundView = UIView()
        }
        if cell.selectedBackgroundView?.backgroundColor != .clear {
            cell.selectedBackgroundView?.backgroundColor = .clear
        }
    }

    private func containsOwnedGlassMarker(in view: UIView) -> Bool {
        if view is SettingsOwnedClearGlassMarkerView { return true }
        return view.subviews.contains { containsOwnedGlassMarker(in: $0) }
    }
}

private struct SettingsDetailClearLiquidGlassModifier: ViewModifier {
    let decoratesRows: Bool
    let isClear: Bool

    func body(content: Content) -> some View {
        content
            // Native Form glass is installed in UIKit cell backgrounds. Read
            // each focused target's anchor at the Form boundary so the depth
            // copy is composited below those materials, not above them.
            .environment(
                \.controllerFocusDepthHandledByAncestor,
                decoratesRows
            )
            .scrollContentBackground(.hidden)
            .controllerFocusDepthBehindGlass(
                isEnabled: decoratesRows,
                force: true,
                cornerRadius: 16
            )
            .background(Color.clear)
            .overlay {
                if decoratesRows {
                    SettingsClearGlassListInstaller(isClear: isClear)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                }
            }
            .stableMenuContentGlassContainer()
            .clearNavigationContainerBackground()
            .toolbarBackground(.hidden, for: .navigationBar)
    }
}

struct SettingsRootView: View {
    /// Real content before Language gives ScrollViewReader enough range to
    /// place the first focusable row inside the protected top corridor when
    /// control enters from the persistent bottom tab bar.
    private static let topNavigationFocusClearance: CGFloat = 96

    /// Visual clearance above RootView's persistent bottom tab bar. Keep this
    /// identical for the root List and every pushed settings destination.
    private static let bottomNavigationBarClearance: CGFloat = 92

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
    @AppStorage("ARMSX2iOSSettingsRememberedRootPane")
    private var rememberedRootPaneRawValue = ""
    @AppStorage("ARMSX2iOSSettingsRememberedRootScrollPosition")
    private var rememberedRootScrollPositionID = ""
    @State private var rootScrollPositionID: String?
    @Environment(\.menuTabIsActive) private var menuTabIsActive
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
        let storedPosition = UserDefaults.standard.string(
            forKey: "ARMSX2iOSSettingsRememberedRootScrollPosition"
        )
        self._rootScrollPositionID = State(
            initialValue: storedPosition.flatMap { $0.isEmpty ? nil : $0 }
        )
    }

    private var backgroundConfigured: Bool {
        settings.hasCustomBackground && settings.backgroundEnabledInSettings
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
            // Keep the root child order stable when the first theme preset
            // enables dynamic backgrounds. Inserting this child ahead of the
            // List while a destination was pushed could invalidate the live
            // NavigationStack subtree and detach its controller session.
            MenuBackgroundLayer(
                isActive: menuTabIsActive && backgroundConfigured
            )
            .opacity(backgroundConfigured ? 1 : 0)

            // NavigationStack keeps its root view alive while a destination is
            // pushed. Do not merely make the root List transparent: that leaves
            // every row, controller probe, UIKit collection view, and JIT-status
            // subscription mounted behind the active sub-setting. Removing the
            // List subtree leaves only the persistent menu background and the
            // destination currently presented by navigationDestination.
            if navigationPath.isEmpty {
            List {
            if controllerInput != nil {
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

            Section {
                jitStatusRow
                    .gameCardTintMenuBackgroundListRow(backgroundActive)

                Button {
                    openStikDebug()
                } label: {
                    Label(settings.localized("Open StikDebug/StosDebug"), systemImage: "bolt.horizontal.circle")
                }
                .disabled(stikDebugOpenInProgress)
                .controllerAccessibilityActionTarget(
                    id: "settings.root.open-stik-debug",
                    label: settings.localized("Open StikDebug/StosDebug"),
                    action: openStikDebug
                )
                .gameCardTintMenuBackgroundListRow(backgroundActive)

                Text(settings.localized("JIT Access means iOS currently allows executable memory. Confirm the real runtime state in-game: the OSD should show EE:JIT, IOP:JIT, and VU:JIT. Match the StikDebug/StosDebug script to the JIT Script setting in Emulator settings."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .gameCardTintMenuBackgroundListRow(backgroundActive)
            } header: {
                Text(settings.localized("JIT Status"))
            }

            Section {
                NavigationLink(value: SettingsPane.licenses) {
                    Label(settings.localized("Licenses & Credits"), systemImage: "doc.text")
                }
                .controllerSettingsDestination(.licenses, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            Section(settings.localized("About")) {
                HStack {
                    Text(settings.localized("Version"))
                    Spacer()
                    Text(SettingsStaticInfo.buildVersion)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
                .gameCardTintMenuBackgroundListRow(backgroundActive)

                NavigationLink(value: SettingsPane.help) {
                    Label(settings.localized("Help"), systemImage: "questionmark.circle")
                }
                .controllerSettingsDestination(.help, path: $navigationPath)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
            }

            // A real tail row extends the List's content size. Unlike a
            // content margin, UIKit cannot consume this clearance as an
            // overlay inset, so Help can always settle above the persistent
            // bottom tab bar.
            Color.clear
                .frame(height: Self.bottomNavigationBarClearance)
                .listRowInsets(EdgeInsets())
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
                .accessibilityHidden(true)
        }
        // The root's upper clearance is a real first List row. A content
        // margin cannot extend the scroll range before Language, which caused
        // a tab-bar Down handoff to clamp the focus outline at the screen edge.
        // RootView's persistent tab bar lives outside this NavigationStack, so
        // its menu-wide scroll margin does not reliably reach the nested List.
        // Reserve the clearance here while controller navigation is active;
        // Local Multiplayer and every later row can then be mounted/revealed
        // before the Help-only boundary permits entry into the tab bar.
        .contentMargins(
            .bottom,
            controllerInput == nil ? 0 : 192,
            for: .scrollContent
        )
        .scrollDisabled(false)
        .scrollBounceBehavior(.always)
        .scrollPosition(id: $rootScrollPositionID, anchor: .top)
        .onChange(of: rootScrollPositionID) { _, _ in
            // SwiftUI updates this binding for direct-finger scrolling as
            // well as programmatic/controller scrolling. Persist that common
            // viewport anchor instead of tying restoration to input mode.
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
            if navigationPath.isEmpty && verticalSizeClass == .compact {
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
            // NavigationStack also retains earlier destinations in a deep
            // path (for example Game Controller -> Controller Macros). Keep
            // their route values for Back navigation, but dismantle their
            // Form/List trees, tasks, observers, controller targets, and
            // scroll-discovery probes until they become the top destination
            // again.
            if navigationPath.last == pane {
                presentedSettingsDetail(for: pane)
                // Destination Forms sit underneath the persistent menu tab
                // bar just like the root List. Give their final controls enough
                // real scroll range to be revealed above that foreground bar.
                .contentMargins(
                    .top,
                    controllerInput == nil ? 0 : 96,
                    for: .scrollContent
                )
                .contentMargins(
                    .bottom,
                    controllerInput == nil ? 0 : 192,
                    for: .scrollContent
                )
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
            // Remember the root row from the route itself. The root List can
            // recycle/unregister its focused probe before the shared session
            // observes the scope change, so probe-owned memory may otherwise
            // retain the initial Language row instead of the selected pane.
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
            // Long lazy Lists use the ordered target graph without asking
            // UIKit to rebuild focus environments for every mounted row.
            usesNativeFocusEngine: false,
            // Every root row has one explicit full-row target. Do not also
            // synthesize a second focusable UIControl through the global Button
            // style while the large Settings List is mounting.
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
        // Appearance is a long lazy Form. Publish its semantic graph from the
        // session owner before the first row mounts instead of accepting a
        // transient one-row graph during the destination transition.
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
            // Publish the deep-link destination before the offscreen Virtual
            // Pad section mounts; its own complete order replaces this seed
            // as soon as the Form is active.
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
        result.append(contentsOf: [
            "settings.root.framePacing",
            "settings.root.audio",
            "settings.root.gameController",
            "settings.root.virtualPad",
            "settings.root.localMultiplayer",
            "settings.root.memoryCards",
            "settings.root.storage",
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

    private var jitStatusRow: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(jitStatusColor)
                .frame(width: 12, height: 12)
                .shadow(color: jitStatusColor.opacity(0.45), radius: 5)

            VStack(alignment: .leading, spacing: 3) {
                Text(settings.localized(jitStatusTitle))
                    .font(.body)
                Text(settings.localized(jitStatusSubtitle))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Text(settings.localized(jitStatusBadge))
                .font(.caption.weight(.semibold))
                .foregroundStyle(jitStatusColor)
        }
        .accessibilityElement(children: .combine)
    }

    private var jitStatusColor: Color {
        if stikDebugOpenFailed {
            return .orange
        }
        if jitAvailable {
            return .blue
        }
        if noJITFallbackActive {
            return .orange
        }
        return .red
    }

    private var jitStatusTitle: String {
        if stikDebugOpenFailed {
            return "StikDebug/StosDebug Did Not Open"
        }
        if jitAvailable {
            return "JIT Access Detected"
        }
        if noJITFallbackActive {
            return "JIT Off"
        }
        return "JIT Not Detected"
    }

    private var jitStatusSubtitle: String {
        if stikDebugOpenFailed {
            return "Open StikDebug/StosDebug manually, then run the selected script and relaunch ARMSX2."
        }
        if jitAvailable {
            return "Access is available. Confirm EE:JIT / IOP:JIT / VU:JIT in the in-game OSD. Current script: \(settings.jitScriptProtocol.label)."
        }
        if noJITFallbackActive {
            return "No-JIT fallback is active. Use StikDebug/StosDebug/\(settings.jitScriptProtocol.label) for dynarec."
        }
        return "Launch with StikDebug/StosDebug using the \(settings.jitScriptProtocol.label) script to enable JIT."
    }

    private var jitStatusBadge: String {
        if stikDebugOpenFailed {
            return "Manual Open"
        }
        return jitAvailable ? "Check OSD" : "Needs StikDebug/StosDebug"
    }

    private func refreshJITStatus() {
        jitAvailable = ARMSX2Bridge.isJITAvailable()
        noJITFallbackActive = ARMSX2Bridge.isNoJITFallbackActive()
        hasLoadedJITStatus = true
    }

    private func openStikDebug() {
        stikDebugOpenInProgress = true
        stikDebugOpenFailed = false
        StikDebugLauncher.open(reason: "settings-root") { success in
            stikDebugOpenInProgress = false
            stikDebugOpenFailed = !success
            refreshJITStatus()
        }
    }

    @ViewBuilder
    private func presentedSettingsDetail(
        for pane: SettingsPane?
    ) -> some View {
        settingsDetail(for: pane)
            // This is applied outside each Form/List so every Settings
            // destination receives the same unobscured viewport without
            // duplicating a spacer in every sub-setting implementation.
            // `safeAreaInset` is observed by the nested scroll view, allowing
            // its last row to settle above the persistent tab bar.
            .safeAreaInset(edge: .bottom, spacing: 0) {
                Color.clear
                    .frame(height: Self.bottomNavigationBarClearance)
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
                        || settings.clearLiquidGlassUISubSettings
                )
            )
            .environment(
                \.clearLiquidGlassUIEnabled,
                pane == .shaders || settings.clearLiquidGlassUISubSettings
            )
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
        case .settingsPresets:
            SettingsPresetsView()
        case .retroAchievements:
            RetroAchievementsSettingsView()
        case .customSkin:
            VirtualPadSettingsView()
        case .overlay:
            OverlaySettingsView()
        case .gameController:
            GamepadSettingsView { pane in
                guard navigationPath.last != pane else { return }
                navigationPath.append(pane)
            }
        case .controllerMacros:
            ControllerMacrosSettingsView()
        case .localMultiplayer:
            LocalMultiplayerSettingsView()
        case .virtualPad:
            VirtualPadSettingsView()
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

private struct LanguageSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("Interface Language")) {
                Picker(settings.localized("App Language"), selection: $settings.appLanguage) {
                    ForEach(AppLanguage.allCases) { language in
                        Text(settings.localized(language.label)).tag(language)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "settings.language.app-language",
                    label: settings.localized("App Language"),
                    selection: $settings.appLanguage,
                    options: AppLanguage.allCases.map {
                        (id: $0, title: settings.localized($0.label))
                    }
                )
                Text(settings.localized("ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Language"))
        .navigationBarTitleDisplayMode(.large)
    }
}

private struct SettingsAboutView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("App")) {
                HStack {
                    Text(settings.localized("Version"))
                    Spacer()
                    Text(SettingsStaticInfo.buildVersion)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
            }
        }
        .navigationTitle(settings.localized("About"))
    }
}

private struct NetworkSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var hosts: [DNSHost] = []
    @State private var lastPersistedHosts: [DNSHost] = []
    @State private var networkAdapters: [String] = []
    @State private var hasLoadedHosts = false
    @State private var hostSaveTask: Task<Void, Never>?

    static let controllerTargetOrder = [
        "settings.network.hdd-enabled",
        "settings.network.default-hdd",
        "settings.network.ethernet-enabled",
        "settings.network.adapter",
        "settings.network.log-dhcp",
        "settings.network.log-dns",
        "settings.network.dns1",
        "settings.network.dns2",
        "settings.network.add-host",
    ]

    var body: some View {
        Form {
            Section(settings.localized("PS2 HDD")) {
                Toggle(settings.localized("Enable DEV9 Virtual HDD"), isOn: $settings.dev9HddEnabled)
                    .controllerAccessibilityTargetID("settings.network.hdd-enabled")

                HStack {
                    Text(settings.localized("Image"))
                    Spacer()
                    Text(settings.dev9HddFile.isEmpty ? "DEV9hdd.raw" : settings.dev9HddFile)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }

                Button(settings.localized("Use Default HDD Image")) {
                    settings.dev9HddFile = "DEV9hdd.raw"
                }
                .controllerAccessibilityTargetID("settings.network.default-hdd")

                Text(settings.localized("If a compatible HDD image is present in app storage, games that expect an internal hard drive can use it. Requires a VM restart. The image is kept out of iCloud backups."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Online / Ethernet")) {
                Toggle(settings.localized("Enable DEV9 Ethernet"), isOn: $settings.dev9EthernetEnabled)
                    .controllerAccessibilityTargetID("settings.network.ethernet-enabled")

                Group {
                    HStack {
                        Text(settings.localized("Mode"))
                        Spacer()
                        Text(settings.localized("Sockets"))
                            .foregroundStyle(.secondary)
                    }

                    Picker(settings.localized("Adapter"), selection: $settings.dev9EthDevice) {
                        ForEach(networkAdapters, id: \.self) { adapter in
                            Text(adapter).tag(adapter)
                        }
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "settings.network.adapter",
                        label: settings.localized("Adapter"),
                        selection: $settings.dev9EthDevice,
                        options: networkAdapters.map {
                            (id: $0, title: $0)
                        }
                    )

                    Toggle(settings.localized("Log DHCP"), isOn: $settings.dev9EthLogDHCP)
                        .controllerAccessibilityTargetID("settings.network.log-dhcp")
                    Toggle(settings.localized("Log DNS"), isOn: $settings.dev9EthLogDNS)
                        .controllerAccessibilityTargetID("settings.network.log-dns")
                }
                // Ethernet options may be prepared before the adapter is
                // enabled. Disabling this Group made the focus graph end at
                // Enable DEV9 Ethernet.

                Text(settings.localized("Sockets is the iOS-safe DEV9 Ethernet mode exposed here. PCAP bridged/switched modes are compiled out of this iOS build, so they are intentionally not selectable until a real iOS backend exists."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("DNS")) {
                dnsRow("DNS1", id: "settings.network.dns1", mode: $settings.dev9DNS1Mode, address: $settings.dev9DNS1)
                dnsRow("DNS2", id: "settings.network.dns2", mode: $settings.dev9DNS2Mode, address: $settings.dev9DNS2)
            }

            Section(settings.localized("Internal DNS")) {
                ForEach($hosts) { $host in
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            TextField(settings.localized("Hostname"), text: $host.url)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                            Toggle("", isOn: $host.enabled).labelsHidden()
                        }
                        TextField("0.0.0.0", text: $host.address)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .keyboardType(.numbersAndPunctuation)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .onDelete { hosts.remove(atOffsets: $0) }

                Button {
                    hosts.append(DNSHost(url: "", desc: "", address: "0.0.0.0", enabled: true))
                } label: {
                    Label(settings.localized("Add Host"), systemImage: "plus")
                }
                .controllerAccessibilityTargetID("settings.network.add-host")

                Text(settings.localized("Maps a hostname to an IP. Set a DNS mode to Internal to use these entries."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Tester Notes")) {
                Text(settings.localized("Games still need their in-game PS2 network setup. After changing DEV9 settings, use Reset ROM or restart the VM before testing."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Network"))
        .onAppear {
            loadNetworkAdaptersIfNeeded()
            loadHosts()
        }
        .onChange(of: hosts) { _, newHosts in
            guard hasLoadedHosts, newHosts != lastPersistedHosts else {
                return
            }
            scheduleHostSave()
        }
        .onDisappear {
            flushPendingHostSave()
        }
#if canImport(UIKit)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            flushPendingHostSave()
        }
#endif
    }

    @ViewBuilder
    private func ipRow(_ label: String, _ value: Binding<String>) -> some View {
        HStack {
            Text(label)
            Spacer()
            TextField("0.0.0.0", text: value)
                .multilineTextAlignment(.trailing)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.numbersAndPunctuation)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func dnsRow(_ label: String, id: String, mode: Binding<String>, address: Binding<String>) -> some View {
        Picker(label, selection: mode) {
            Text(settings.localized("Auto")).tag("Auto")
            Text(settings.localized("Manual")).tag("Manual")
            Text(settings.localized("Internal")).tag("Internal")
        }
        .controllerAccessibilityOptionsPickerTarget(
            id: id,
            label: label,
            selection: mode,
            options: [
                ("Auto", settings.localized("Auto")),
                ("Manual", settings.localized("Manual")),
                ("Internal", settings.localized("Internal")),
            ]
        )
        ipRow(label + " " + settings.localized("Address"), address)
            .disabled(mode.wrappedValue != "Manual")
    }

    private func loadHosts() {
        let count = Int(ARMSX2Bridge.getINIInt("DEV9/Eth/Hosts", key: "Count", defaultValue: 0))
        var loaded: [DNSHost] = []
        var i = 0
        while i < count {
            let sec = "DEV9/Eth/Hosts/Host\(i)"
            loaded.append(DNSHost(
                url: ARMSX2Bridge.getINIString(sec, key: "Url", defaultValue: ""),
                desc: ARMSX2Bridge.getINIString(sec, key: "Desc", defaultValue: ""),
                address: ARMSX2Bridge.getINIString(sec, key: "Address", defaultValue: "0.0.0.0"),
                enabled: ARMSX2Bridge.getINIBool(sec, key: "Enabled", defaultValue: true)
            ))
            i += 1
        }
        hosts = loaded
        lastPersistedHosts = loaded
        hasLoadedHosts = true
    }

    private func loadNetworkAdaptersIfNeeded() {
        guard networkAdapters.isEmpty else { return }
        var loaded = ARMSX2Bridge.dev9NetworkAdapters()
        if !settings.dev9EthDevice.isEmpty,
           !loaded.contains(settings.dev9EthDevice) {
            loaded.insert(settings.dev9EthDevice, at: 0)
        }
        networkAdapters = loaded
    }

    /// Text fields can publish on every keystroke. Coalescing those writes
    /// avoids repeatedly rewriting every DEV9 host section while typing.
    private func scheduleHostSave() {
        hostSaveTask?.cancel()
        hostSaveTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            saveHosts()
            hostSaveTask = nil
        }
    }

    private func flushPendingHostSave() {
        hostSaveTask?.cancel()
        hostSaveTask = nil
        guard hasLoadedHosts, hosts != lastPersistedHosts else { return }
        saveHosts()
    }

    private func saveHosts() {
        let oldCount = Int(ARMSX2Bridge.getINIInt("DEV9/Eth/Hosts", key: "Count", defaultValue: 0))
        var i = 0
        while i < max(oldCount, hosts.count) {
            ARMSX2Bridge.clearINISection("DEV9/Eth/Hosts/Host\(i)")
            i += 1
        }
        ARMSX2Bridge.setINIInt("DEV9/Eth/Hosts", key: "Count", value: Int32(hosts.count))
        for (idx, host) in hosts.enumerated() {
            let sec = "DEV9/Eth/Hosts/Host\(idx)"
            ARMSX2Bridge.setINIString(sec, key: "Url", value: host.url)
            ARMSX2Bridge.setINIString(sec, key: "Desc", value: host.desc.isEmpty ? host.url : host.desc)
            ARMSX2Bridge.setINIString(sec, key: "Address", value: host.address)
            ARMSX2Bridge.setINIBool(sec, key: "Enabled", value: host.enabled)
        }
        lastPersistedHosts = hosts
    }
}

private struct DNSHost: Identifiable, Equatable {
    let id = UUID()
    var url: String
    var desc: String
    var address: String
    var enabled: Bool
}
