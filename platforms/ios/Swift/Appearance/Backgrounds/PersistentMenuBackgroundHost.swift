// PersistentMenuBackgroundHost.swift — persistent menu background host
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
