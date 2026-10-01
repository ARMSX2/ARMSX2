// MenuBackgroundSupport.swift — Shared menu-tab background helpers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private extension Notification.Name {
    static let captureMenuBackgroundSnapshot = Notification.Name(
        "ARMSX2.captureMenuBackgroundSnapshot"
    )
}

private struct MenuBackgroundHostEnvironmentKey: EnvironmentKey {
    static let defaultValue: PersistentMenuBackgroundHost? = nil
}

extension EnvironmentValues {
    var menuBackgroundHost: PersistentMenuBackgroundHost? {
        get { self[MenuBackgroundHostEnvironmentKey.self] }
        set { self[MenuBackgroundHostEnvironmentKey.self] = newValue }
    }
}

/// Owns the lifecycle state for the single background renderer installed above
/// the complete menu TabView hierarchy.
@MainActor
final class PersistentMenuBackgroundHost: ObservableObject {
    @Published private(set) var sessionStart = Date()

    @Published private(set) var rendererMounted: Bool
    @Published private(set) var presentationVisible = true
    @Published private(set) var menuBackgroundAvailable: Bool
    @Published private(set) var inactiveSnapshot: UIImage?
    @Published private(set) var inactiveSnapshotOpacity = 0.0
    private var releasedForGameplay = false
    private var snapshotReleaseTask: Task<Void, Never>?
    private var requestedPresentationVisible = true
    private var frozenPresentationCount = 0

    init() {
        let hasBackground = SettingsStore.shared.hasCustomBackground
        menuBackgroundAvailable = hasBackground
        // Keep the UIKit hosting boundary stable for the entire menu session.
        // Enabling a theme must not insert a new child view controller while a
        // Settings List is processing the controller press which chose it.
        rendererMounted = true
    }

    /// Shows or hides configured background content without changing the
    /// renderer's identity. The renderer is released only with the menu itself.
    func setMenuBackgroundAvailable(_ isAvailable: Bool) {
        guard menuBackgroundAvailable != isAvailable else { return }
        menuBackgroundAvailable = isAvailable
        if !isAvailable {
            releaseInactiveSnapshot()
            PlayStation3XMBByMartShaderLibrary.releaseSessionCache()
        }
    }

    /// Starts a fresh renderer session only after gameplay released the previous
    /// one. Normal tab changes retain the same session and renderer identity.
    func reactivateForMenu(isAvailable: Bool) {
        if releasedForGameplay {
            releasedForGameplay = false
            sessionStart = Date()
            rendererMounted = true
        }
        setMenuBackgroundAvailable(isAvailable)
    }

    /// Keeps the persistent hosting boundary stable while suspending its live
    /// animated/video/Metal content on background-disabled tabs.
    func setPresentationVisible(_ isVisible: Bool) {
        requestedPresentationVisible = isVisible
        reconcilePresentationVisibility()
    }

    /// Freezes the shared animated background on its current rendered frame.
    /// Shader management uses the still frame as its page background, avoiding
    /// both a second glass sheet and live full-screen rendering behind a long
    /// expanded editor.
    func beginFrozenPresentation() {
        frozenPresentationCount += 1
        guard frozenPresentationCount == 1 else { return }
        NotificationCenter.default.post(
            name: .captureMenuBackgroundSnapshot,
            object: nil
        )
        reconcilePresentationVisibility()
    }

    func endFrozenPresentation() {
        frozenPresentationCount = max(0, frozenPresentationCount - 1)
        guard frozenPresentationCount == 0 else { return }
        reconcilePresentationVisibility()
        if requestedPresentationVisible {
            releaseInactiveSnapshotWhenRendererIsReady()
        }
    }

    private func reconcilePresentationVisibility() {
        let visible = requestedPresentationVisible
            && frozenPresentationCount == 0
        guard presentationVisible != visible else { return }
        presentationVisible = visible
    }

    /// Stops the renderer when the complete menu hierarchy is being released.
    /// Normal tab changes and per-tab visibility toggles do not call this.
    func suspend() {
        guard rendererMounted else { return }
        rendererMounted = false
    }

    /// Keeps one rasterized frame visible while iOS deactivates the scene and
    /// tears down live video, display-link, and Metal rendering. The image is
    /// intentionally held only across that inactive interval.
    func retainInactiveSnapshot(_ image: UIImage) {
        guard menuBackgroundAvailable, rendererMounted else { return }
        snapshotReleaseTask?.cancel()
        snapshotReleaseTask = nil
        inactiveSnapshot = image
        // Capture immediately on suspension; crossfade only after live
        // rendering resumes.
        inactiveSnapshotOpacity = 1
    }

    /// Scene activation restarts the live renderer first. Once it has produced
    /// frames again, crossfade the still image away and release its pixel storage.
    func releaseInactiveSnapshotWhenRendererIsReady() {
        snapshotReleaseTask?.cancel()
        guard frozenPresentationCount == 0,
              requestedPresentationVisible,
              inactiveSnapshot != nil else { return }
        // Keep the held frame fully visible while the live renderer begins its
        // warm-up interval.
        inactiveSnapshotOpacity = 1
        snapshotReleaseTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(180))
            guard !Task.isCancelled else { return }
            withAnimation(.easeInOut(duration: 0.36)) {
                self?.inactiveSnapshotOpacity = 0
            }
            try? await Task.sleep(for: .milliseconds(360))
            guard !Task.isCancelled else { return }
            self?.inactiveSnapshot = nil
            self?.snapshotReleaseTask = nil
        }
    }

    private func releaseInactiveSnapshot() {
        snapshotReleaseTask?.cancel()
        snapshotReleaseTask = nil
        inactiveSnapshotOpacity = 0
        inactiveSnapshot = nil
    }

    /// The surrounding MenuTabView is also removed at gameplay start, ensuring
    /// SwiftUI dismantles video, animated-image, display-link, and Metal views.
    func release() {
        releasedForGameplay = true
        menuBackgroundAvailable = false
        releaseInactiveSnapshot()
        suspend()
        PlayStation3XMBByMartShaderLibrary.releaseSessionCache()
    }

    var shouldMountRenderer: Bool { rendererMounted }

    var shouldShowRenderer: Bool {
        shouldMountRenderer && menuBackgroundAvailable && presentationVisible
    }

    /// Snapshot only while the live wallpaper is actually visible. A frozen
    /// editor already owns a valid still frame; capturing its paused renderer
    /// during Control Center presentation would replace that frame with black.
    var canCaptureLiveBackground: Bool { shouldShowRenderer }
}

struct PersistentMenuBackgroundLayer: View {
    @ObservedObject var host: PersistentMenuBackgroundHost

    @ViewBuilder
    var body: some View {
        if host.shouldMountRenderer {
            GeometryReader { geometry in
                ZStack {
                    SnapshottingBackgroundRenderer(
                        size: geometry.size,
                        sessionStart: host.sessionStart,
                        snapshotHost: host,
                        isPresentationActive: host.shouldShowRenderer
                    )

                    if let snapshot = host.inactiveSnapshot {
                        Image(uiImage: snapshot)
                            .resizable()
                            .scaledToFill()
                            .frame(
                                width: geometry.size.width,
                                height: geometry.size.height
                            )
                            .clipped()
                            .opacity(host.inactiveSnapshotOpacity)
                    }
                }
            }
            .opacity(
                host.shouldShowRenderer || host.inactiveSnapshot != nil ? 1 : 0
            )
            .ignoresSafeArea()
            .accessibilityHidden(true)
            .allowsHitTesting(false)
        }
    }
}

/// Gives the persistent menu background its own UIKit subtree. Capturing this
/// view therefore records only the background—not Games/BIOS/Settings chrome—
/// before iOS suspends Metal, video, and display-link rendering.
private struct SnapshottingBackgroundRenderer: UIViewControllerRepresentable {
    let size: CGSize
    let sessionStart: Date
    let snapshotHost: PersistentMenuBackgroundHost
    let isPresentationActive: Bool

    func makeUIViewController(
        context: Context
    ) -> BackgroundSnapshotHostingController {
        BackgroundSnapshotHostingController(
            size: size,
            sessionStart: sessionStart,
            snapshotHost: snapshotHost,
            isPresentationActive: isPresentationActive
        )
    }

    func updateUIViewController(
        _ controller: BackgroundSnapshotHostingController,
        context: Context
    ) {
        controller.update(
            size: size,
            sessionStart: sessionStart,
            snapshotHost: snapshotHost,
            isPresentationActive: isPresentationActive
        )
    }

    static func dismantleUIViewController(
        _ controller: BackgroundSnapshotHostingController,
        coordinator: ()
    ) {
        controller.teardown()
    }
}

@MainActor
private final class BackgroundSnapshotHostingController: UIViewController {
    private var backgroundController: UIHostingController<AnyView>?
    private weak var snapshotHost: PersistentMenuBackgroundHost?
    private var renderedSize: CGSize
    private var renderedSessionStart: Date
    private var presentationActive: Bool
    private var observingLifecycle = false

    init(
        size: CGSize,
        sessionStart: Date,
        snapshotHost: PersistentMenuBackgroundHost,
        isPresentationActive: Bool
    ) {
        renderedSize = size
        renderedSessionStart = sessionStart
        presentationActive = isPresentationActive
        self.snapshotHost = snapshotHost
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .clear
        installBackground(
            size: renderedSize,
            sessionStart: renderedSessionStart,
            isPresentationActive: presentationActive
        )

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationWillResignActive),
            name: UIApplication.willResignActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationDidBecomeActive),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(captureRequested),
            name: .captureMenuBackgroundSnapshot,
            object: nil
        )
        observingLifecycle = true
    }

    func update(
        size: CGSize,
        sessionStart: Date,
        snapshotHost: PersistentMenuBackgroundHost,
        isPresentationActive: Bool
    ) {
        self.snapshotHost = snapshotHost
        guard size != renderedSize
                || sessionStart != renderedSessionStart
                || isPresentationActive != presentationActive else {
            return
        }
        renderedSize = size
        renderedSessionStart = sessionStart
        presentationActive = isPresentationActive
        backgroundController?.rootView = backgroundRoot(
            size: size,
            sessionStart: sessionStart,
            isPresentationActive: isPresentationActive
        )
    }

    func teardown() {
        if observingLifecycle {
            NotificationCenter.default.removeObserver(self)
            observingLifecycle = false
        }
        if let backgroundController {
            backgroundController.willMove(toParent: nil)
            backgroundController.view.removeFromSuperview()
            backgroundController.removeFromParent()
        }
        backgroundController = nil
        snapshotHost = nil
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    private func installBackground(
        size: CGSize,
        sessionStart: Date,
        isPresentationActive: Bool
    ) {
        let controller = UIHostingController(
            rootView: backgroundRoot(
                size: size,
                sessionStart: sessionStart,
                isPresentationActive: isPresentationActive
            )
        )
        controller.view.backgroundColor = .clear
        addChild(controller)
        view.addSubview(controller.view)
        controller.view.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            controller.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            controller.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            controller.view.topAnchor.constraint(equalTo: view.topAnchor),
            controller.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        controller.didMove(toParent: self)
        backgroundController = controller
    }

    private func backgroundRoot(
        size: CGSize,
        sessionStart: Date,
        isPresentationActive: Bool
    ) -> AnyView {
        AnyView(
            BackgroundContainerView(
                size: size,
                isPresentationActive: isPresentationActive
            )
                .environment(\.menuBackgroundSessionStart, sessionStart)
        )
    }

    @objc private func applicationWillResignActive() {
        captureSnapshot()
    }

    @objc private func captureRequested() {
        captureSnapshot()
    }

    private func captureSnapshot() {
        guard snapshotHost?.canCaptureLiveBackground == true,
              let targetView = backgroundController?.view,
              targetView.window != nil,
              targetView.bounds.width > 0,
              targetView.bounds.height > 0 else {
            return
        }

        targetView.layoutIfNeeded()
        let format = UIGraphicsImageRendererFormat.preferred()
        format.scale = targetView.window?.screen.scale ?? 1
        format.opaque = false
        let renderer = UIGraphicsImageRenderer(
            bounds: targetView.bounds,
            format: format
        )
        let snapshot = renderer.image { context in
            let captured = targetView.drawHierarchy(
                in: targetView.bounds,
                afterScreenUpdates: false
            )
            if !captured {
                targetView.layer.render(in: context.cgContext)
            }
        }
        snapshotHost?.retainInactiveSnapshot(snapshot)
    }

    @objc private func applicationDidBecomeActive() {
        snapshotHost?.releaseInactiveSnapshotWhenRendererIsReady()
    }
}

struct MenuBackgroundLayer: View {
    var isActive = true
    @Environment(\.menuBackgroundHost) private var persistentHost

    @ViewBuilder
    var body: some View {
        if persistentHost != nil {
            Color.clear
                .ignoresSafeArea()
                .accessibilityHidden(true)
                .allowsHitTesting(false)
        } else if isActive {
            GeometryReader { geometry in
                BackgroundContainerView(size: geometry.size)
            }
            .ignoresSafeArea()
            .accessibilityHidden(true)
            .allowsHitTesting(false)
        }
    }
}

/// Lets Games and BIOS publish their existing navigation/toolbar preferences
/// into MenuTabView's one persistent NavigationStack. Standalone previews and
/// Catalyst call sites retain their original self-contained navigation stack.
struct OptionalMenuNavigationStack<Content: View>: View {
    let embedded: Bool
    let content: Content

    init(embedded: Bool, @ViewBuilder content: () -> Content) {
        self.embedded = embedded
        self.content = content()
    }

    @ViewBuilder
    var body: some View {
        if embedded {
            content
        } else {
            NavigationStack {
                content
            }
        }
    }
}

/// A page-owned replacement for UINavigationBar's large title on the retained
/// Games/BIOS pages. One native large title cannot reliably track two live
/// scroll views in the same NavigationStack; keeping the title in each page's
/// scroll content makes its visibility and collapse state deterministic.
struct EmbeddedMenuLargeTitle: View {
    let title: String
    var usesARMSX2Logo = false
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    @Environment(\.menuLargeTitleNamespace) private var transitionNamespace
    @Environment(\.menuLargeTitleMorphActive) private var morphActive
    @Environment(\.menuLargeTitleIsSource) private var explicitMorphSource
    @Environment(\.uiTabTitleColour) private var titleTextColour

    var body: some View {
        titleContent
            .modifier(
                MenuLargeTitleMorphModifier(
                    id: "menu.largeTitle",
                    namespace: transitionNamespace,
                    isEnabled: morphActive,
                    isSource: explicitMorphSource ?? menuTabIsActive
                )
            )
    }

    private var titleContent: some View {
        HStack {
            titleLabel(compact: false)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 20)
        .padding(.top, 8)
        .padding(.bottom, 6)
        // List, LazyVStack, and the empty-state ScrollView otherwise negotiate
        // subtly different first-row heights. A shared title footprint keeps
        // Games and BIOS at the same screen point as Settings before and after
        // their content changes from empty to populated.
        .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
    }

    @ViewBuilder
    private func titleLabel(compact: Bool) -> some View {
        if usesARMSX2Logo, let image = ARMSX2BouncingLogoAsset.image {
            Image(uiImage: image)
                .resizable()
                .interpolation(.high)
                .scaledToFit()
                .frame(width: compact ? 132 : 224, height: compact ? 18 : 30)
                .accessibilityLabel(title)
                .accessibilityAddTraits(.isHeader)
        } else {
            Text(title)
                .font(.system(size: 38, weight: .bold))
                .foregroundStyle(titleTextColour)
                .contentTransition(.interpolate)
                .accessibilityAddTraits(.isHeader)
        }
    }
}

/// Compact-height counterpart of `EmbeddedMenuLargeTitle`. Games/BIOS share a
/// NavigationStack, while Settings owns another retained stack; applying the
/// same matched title identity makes all three labels participate in the tab
/// morph instead of fading Settings as unrelated toolbar text.
struct EmbeddedMenuCompactTitle: View {
    let title: String
    var usesARMSX2Logo = false
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    @Environment(\.menuLargeTitleNamespace) private var transitionNamespace
    @Environment(\.menuLargeTitleMorphActive) private var morphActive
    @Environment(\.menuLargeTitleIsSource) private var explicitMorphSource
    @Environment(\.uiTabTitleColour) private var titleTextColour

    var body: some View {
        compactTitle
            .modifier(
                MenuLargeTitleMorphModifier(
                    id: "menu.compactTitle",
                    namespace: transitionNamespace,
                    isEnabled: morphActive,
                    isSource: explicitMorphSource ?? menuTabIsActive
                )
            )
    }

    @ViewBuilder
    private var compactTitle: some View {
        if usesARMSX2Logo, let image = ARMSX2BouncingLogoAsset.image {
            Image(uiImage: image)
                .resizable()
                .interpolation(.high)
                .scaledToFit()
                .frame(width: 132, height: 18)
                .accessibilityLabel(title)
                .accessibilityAddTraits(.isHeader)
        } else {
            Text(title)
                .font(.headline)
                .foregroundStyle(titleTextColour)
                .lineLimit(1)
                .contentTransition(.interpolate)
        }
    }
}

/// One stable empty-state import label shared by Games and BIOS. The explicit
/// focus value is used by the Games library engine; BIOS inherits focus from
/// the shared accessibility-navigation target wrapped around the button.
struct MenuImportActionLabel: View {
    let title: String
    var explicitlyFocused: Bool? = nil

    @Environment(\.controllerAccessibilityTargetFocused) private var targetFocused
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiImportActionColour) private var contentTextColour

    private var isFocused: Bool {
        explicitlyFocused ?? targetFocused
    }

    var body: some View {
        let focusedLabel = Label(title, systemImage: "plus")
            .font(.body.weight(isFocused ? .semibold : .regular))
            .foregroundStyle(isFocused ? accentColour : contentTextColour)
            .animation(
                ControllerFocusVisualAnimation.textFade,
                value: isFocused
            )
        focusedLabel
            .padding(.horizontal, 16)
            .frame(minHeight: 44)
            .background {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(accentColour.opacity(isFocused ? 0.22 : 0.12))
                    .overlay {
                        RoundedRectangle(
                            cornerRadius: 12,
                            style: .continuous
                        )
                        .stroke(
                            accentColour.opacity(isFocused ? 0.78 : 0.34),
                            lineWidth: isFocused ? 1.2 : 0.7
                        )
                    }
            }
    }
}

private struct MenuLargeTitleMorphModifier: ViewModifier {
    let id: String
    let namespace: Namespace.ID?
    let isEnabled: Bool
    let isSource: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if let namespace {
            // Keep the matched identity installed between transitions. Adding
            // and removing matchedGeometryEffect only after a tab change gives
            // SwiftUI no stable source/destination pair, so the titles fade or
            // snap instead of morphing.
            content
                .matchedGeometryEffect(
                    id: id,
                    in: namespace,
                    properties: .frame,
                    anchor: .topLeading,
                    isSource: isSource
                )
                .transaction { transaction in
                    if !isEnabled {
                        transaction.animation = nil
                    }
                }
        } else {
            content
        }
    }
}

/// Stable matched positions for the common Games/BIOS toolbar controls. The
/// navigation bar keeps its native controls and sizes; only their rendered
/// positions interpolate, avoiding the landscape resize/layout regression.
private struct MenuToolbarMorphElementModifier: ViewModifier {
    let id: String
    @Environment(\.menuLargeTitleNamespace) private var transitionNamespace
    @Environment(\.menuLargeTitleMorphActive) private var morphActive
    @Environment(\.menuLargeTitleIsSource) private var explicitMorphSource
    @Environment(\.menuTabIsActive) private var menuTabIsActive

    @ViewBuilder
    func body(content: Content) -> some View {
        if let transitionNamespace {
            content
                .matchedGeometryEffect(
                    id: "menu.toolbar.\(id)",
                    in: transitionNamespace,
                    properties: .position,
                    anchor: .center,
                    isSource: explicitMorphSource ?? menuTabIsActive
                )
                .transaction { transaction in
                    if !morphActive { transaction.animation = nil }
                }
        } else {
            content
        }
    }
}

extension View {
    func menuToolbarMorphElement(_ id: String) -> some View {
        modifier(MenuToolbarMorphElementModifier(id: id))
    }
}

private struct OptionalMenuNavigationChromeModifier: ViewModifier {
    let title: String
    let backgroundHidden: Bool
    let embedded: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if embedded {
            content
        } else {
            content
                .navigationTitle(title)
                .toolbarBackground(
                    backgroundHidden ? .hidden : .automatic,
                    for: .navigationBar
                )
        }
    }
}

private struct ClearNavigationContainerBackgroundModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.containerBackground(Color.clear, for: .navigation)
        } else {
            content
        }
    }
}

/// Keeps a tab's custom glass scene attached below its NavigationStack chrome.
/// MenuTabView retains every tab page, so this container is not dismantled when
/// another tab is selected.
private struct StableMenuContentGlassContainerModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: 0) {
                content
            }
        } else {
            content
        }
    }
}

/// Prevents a transient glass surface from joining the persistent menu-card
/// effect group. Removing the transient surface then cannot cause every
/// remaining card to rematerialize its Liquid Glass properties.
private struct IsolatedMenuGlassContainerModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: 0) {
                content
            }
        } else {
            content
        }
    }
}

struct MenuBackgroundListRowModifier: ViewModifier {
    let isEnabled: Bool
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(reduceTransparency ? AnyShapeStyle(.background) : AnyShapeStyle(.regularMaterial), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .listRowInsets(EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 12))
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
        } else {
            content
        }
    }
}

/// Gives non-library menu rows the clear Liquid Glass tint used by Games cards
/// without changing the Games list-mode treatment.
struct GameCardTintMenuBackgroundListRowModifier: ViewModifier {
    let isEnabled: Bool
    let glassEnabled: Bool
    let forceClear: Bool
    let cornerRadius: CGFloat
    let rowSpacingScale: CGFloat
    let contentPaddingScale: CGFloat
    let verticalPaddingScale: CGFloat

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content
                // Clear-glass rows should occupy one consistent column width
                // even when their custom control has a small intrinsic size.
                // This normalizes Appearance's mixed row types.
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 12 * contentPaddingScale)
                .padding(.vertical, 8 * verticalPaddingScale)
                // Apply glass to the complete row so Liquid Glass always
                // composites behind its labels and controls. The earlier
                // detached background surface could be promoted above the
                // content by the iOS 26 glass compositor.
                .glassSurface(
                    clear: true,
                    forceClear: forceClear,
                    isEnabled: glassEnabled,
                    cornerRadius: cornerRadius
                )
                .listRowInsets(
                    EdgeInsets(
                        top: 6 * rowSpacingScale,
                        leading: 12,
                        bottom: 6 * rowSpacingScale,
                        trailing: 12
                    )
                )
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
        } else {
            content
        }
    }
}

extension View {
    func optionalMenuNavigationChrome(
        title: String,
        backgroundHidden: Bool,
        embedded: Bool
    ) -> some View {
        modifier(
            OptionalMenuNavigationChromeModifier(
                title: title,
                backgroundHidden: backgroundHidden,
                embedded: embedded
            )
        )
    }

    func stableMenuContentGlassContainer() -> some View {
        modifier(StableMenuContentGlassContainerModifier())
    }

    func isolatedMenuGlassContainer() -> some View {
        modifier(IsolatedMenuGlassContainerModifier())
    }

    func clearNavigationContainerBackground() -> some View {
        modifier(ClearNavigationContainerBackgroundModifier())
    }

    func menuBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(MenuBackgroundListRowModifier(isEnabled: isEnabled))
    }

    func gameCardTintMenuBackgroundListRow(
        _ isEnabled: Bool,
        glassEnabled: Bool = true,
        forceClear: Bool = false,
        cornerRadius: CGFloat = 16,
        rowSpacingScale: CGFloat = 1,
        contentPaddingScale: CGFloat = 1,
        verticalPaddingScale: CGFloat = 1
    ) -> some View {
        modifier(
            GameCardTintMenuBackgroundListRowModifier(
                isEnabled: isEnabled,
                glassEnabled: glassEnabled,
                forceClear: forceClear,
                cornerRadius: cornerRadius,
                rowSpacingScale: min(max(rowSpacingScale, 0.5), 2),
                contentPaddingScale: min(max(contentPaddingScale, 0), 2),
                verticalPaddingScale: min(max(verticalPaddingScale, 0), 2)
            )
        )
    }

    /// Keeps the logical controller ID on the outer `Form` row. Appearance
    /// rows add their own glass wrapper, so an ID attached before that wrapper
    /// cannot reliably materialize an offscreen lazy row for `scrollTo`.
    func appearanceControllerListRow(_ navigationID: String) -> some View {
        gameCardTintMenuBackgroundListRow(true)
            .controllerAccessibilityTargetID(navigationID)
    }
}

/// Compact touch controls for stepping through a value without opening its
/// picker. The containing picker remains the single controller-navigation
/// target, so these buttons do not fragment directional focus.
struct SettingsValueStepButtons: View {
    let previousAccessibilityLabel: String
    let nextAccessibilityLabel: String
    let canSelectPrevious: Bool
    let canSelectNext: Bool
    let selectPrevious: () -> Void
    let selectNext: () -> Void

    var body: some View {
        HStack(spacing: 4) {
            stepButton(
                systemImage: "chevron.left",
                accessibilityLabel: previousAccessibilityLabel,
                isEnabled: canSelectPrevious,
                action: selectPrevious
            )
            stepButton(
                systemImage: "chevron.right",
                accessibilityLabel: nextAccessibilityLabel,
                isEnabled: canSelectNext,
                action: selectNext
            )
        }
        .fixedSize(horizontal: true, vertical: false)
    }

    private func stepButton(
        systemImage: String,
        accessibilityLabel: String,
        isEnabled: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.caption.weight(.bold))
                .frame(width: 28, height: 28)
                .contentShape(Circle())
        }
        .buttonStyle(.bordered)
        .buttonBorderShape(.circle)
        .disabled(!isEnabled)
        .accessibilityLabel(accessibilityLabel)
    }
}
