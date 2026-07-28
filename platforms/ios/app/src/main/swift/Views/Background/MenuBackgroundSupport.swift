// MenuBackgroundSupport.swift — Shared menu-tab background helpers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

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
    @Published private(set) var inactiveSnapshot: UIImage?
    @Published private(set) var inactiveSnapshotOpacity = 0.0
    @Published private var exclusivePreviewDepth = 0
    private var menuBackgroundAvailable = true
    private var releasedForGameplay = false
    private var snapshotReleaseTask: Task<Void, Never>?

    init() {
        let hasBackground = SettingsStore.shared.hasCustomBackground
        menuBackgroundAvailable = hasBackground
        rendererMounted = hasBackground
    }

    /// Creates or destroys the menu renderer only when the user adds/removes
    /// the configured background itself. Tab selection never calls this.
    func setMenuBackgroundAvailable(_ isAvailable: Bool) {
        menuBackgroundAvailable = isAvailable
        rendererMounted = isAvailable
            && !(exclusivePreviewDepth > 0 && SettingsStore.shared.dynamicBackgroundsEnabled)
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
        }
        setMenuBackgroundAvailable(isAvailable)
    }

    /// Hides the persistent renderer without removing it from SwiftUI. Video
    /// playback position and unmuted audio continue on background-disabled tabs.
    func setPresentationVisible(_ isVisible: Bool) {
        presentationVisible = isVisible
    }

    /// The Appearance screen uses the same expensive renderer in its preview.
    /// Dynamic backgrounds still yield to that preview to avoid two live
    /// animation/Metal renderers. Imported image/video backgrounds remain
    /// mounted but hidden so unmuted video audio and playback time are retained.
    func beginExclusivePreview() {
        exclusivePreviewDepth += 1
        if exclusivePreviewDepth == 1 && SettingsStore.shared.dynamicBackgroundsEnabled {
            rendererMounted = false
        }
    }

    func endExclusivePreview() {
        exclusivePreviewDepth = max(0, exclusivePreviewDepth - 1)
        if exclusivePreviewDepth == 0 {
            rendererMounted = menuBackgroundAvailable
        }
    }

    /// Stops the renderer when the complete menu hierarchy is being released.
    /// Normal tab changes and per-tab visibility toggles do not call this.
    func suspend() {
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
        guard inactiveSnapshot != nil else { return }
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
        exclusivePreviewDepth = 0
        releaseInactiveSnapshot()
        suspend()
        PlayStation3XMBByMartShaderLibrary.releaseSessionCache()
    }

    var shouldShowRenderer: Bool {
        rendererMounted && presentationVisible && exclusivePreviewDepth == 0
    }
}

struct PersistentMenuBackgroundLayer: View {
    @ObservedObject var host: PersistentMenuBackgroundHost

    @ViewBuilder
    var body: some View {
        if host.rendererMounted {
            GeometryReader { geometry in
                ZStack {
                    SnapshottingBackgroundRenderer(
                        size: geometry.size,
                        sessionStart: host.sessionStart,
                        snapshotHost: host
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
            .opacity(host.shouldShowRenderer ? 1 : 0)
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

    func makeUIViewController(
        context: Context
    ) -> BackgroundSnapshotHostingController {
        BackgroundSnapshotHostingController(
            size: size,
            sessionStart: sessionStart,
            snapshotHost: snapshotHost
        )
    }

    func updateUIViewController(
        _ controller: BackgroundSnapshotHostingController,
        context: Context
    ) {
        controller.update(
            size: size,
            sessionStart: sessionStart,
            snapshotHost: snapshotHost
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
    private var observingLifecycle = false

    init(
        size: CGSize,
        sessionStart: Date,
        snapshotHost: PersistentMenuBackgroundHost
    ) {
        renderedSize = size
        renderedSessionStart = sessionStart
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
        installBackground(size: renderedSize, sessionStart: renderedSessionStart)

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
        observingLifecycle = true
    }

    func update(
        size: CGSize,
        sessionStart: Date,
        snapshotHost: PersistentMenuBackgroundHost
    ) {
        self.snapshotHost = snapshotHost
        guard size != renderedSize || sessionStart != renderedSessionStart else {
            return
        }
        renderedSize = size
        renderedSessionStart = sessionStart
        backgroundController?.rootView = backgroundRoot(
            size: size,
            sessionStart: sessionStart
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

    private func installBackground(size: CGSize, sessionStart: Date) {
        let controller = UIHostingController(
            rootView: backgroundRoot(size: size, sessionStart: sessionStart)
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
        sessionStart: Date
    ) -> AnyView {
        AnyView(
            BackgroundContainerView(size: size)
                .environment(\.menuBackgroundSessionStart, sessionStart)
        )
    }

    @objc private func applicationWillResignActive() {
        guard let targetView = backgroundController?.view,
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
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    @Environment(\.menuLargeTitleNamespace) private var transitionNamespace
    @Environment(\.menuLargeTitleMorphActive) private var morphActive

    var body: some View {
        titleContent
            .modifier(
                MenuLargeTitleMorphModifier(
                    namespace: transitionNamespace,
                    isEnabled: morphActive,
                    isSource: menuTabIsActive
                )
            )
    }

    private var titleContent: some View {
        HStack {
            Text(title)
                .font(.largeTitle.bold())
                .foregroundStyle(.primary)
                .contentTransition(.interpolate)
                .accessibilityAddTraits(.isHeader)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 20)
        .padding(.top, 8)
        .padding(.bottom, 6)
    }
}

private struct MenuLargeTitleMorphModifier: ViewModifier {
    let namespace: Namespace.ID?
    let isEnabled: Bool
    let isSource: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled, let namespace {
            content.matchedGeometryEffect(
                id: "menu.largeTitle",
                in: namespace,
                properties: .frame,
                anchor: .topLeading,
                isSource: isSource
            )
        } else {
            content
        }
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

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .glassSurface(clear: true, cornerRadius: 16)
                .listRowInsets(EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 12))
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

    func gameCardTintMenuBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(GameCardTintMenuBackgroundListRowModifier(isEnabled: isEnabled))
    }
}
