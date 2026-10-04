// SwiftUIHost.swift — ObjC-callable helper to create SwiftUI hosting controllers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
#if canImport(GameController)
import GameController
#endif

/// Owns the controller delivery boundary for the complete SDL/SwiftUI window.
@MainActor
@objc(ARMSX2ControllerEventHostViewController)
final class ARMSX2ControllerEventHostViewController: GCEventViewController {
    private let contentController: UIViewController

    @objc init(contentController: UIViewController) {
        self.contentController = contentController
        super.init(nibName: nil, bundle: nil)
        controllerUserInteractionEnabled = false
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemGroupedBackground

        if #available(iOS 26.0, *) {
            let controllerEvents = GCEventInteraction()
            controllerEvents.handledEventTypes = .gamepad
            controllerEvents.receivesEventsInView = false
            view.addInteraction(controllerEvents)
        }

        addChild(contentController)
        contentController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(contentController.view)
        NSLayoutConstraint.activate([
            contentController.view.topAnchor.constraint(equalTo: view.topAnchor),
            contentController.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            contentController.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            contentController.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
        ])
        contentController.didMove(toParent: self)
    }

    override var childForStatusBarHidden: UIViewController? {
        children.last
    }

    override var childForStatusBarStyle: UIViewController? {
        children.last
    }

    override var childForHomeIndicatorAutoHidden: UIViewController? {
        children.last
    }

    override var childForScreenEdgesDeferringSystemGestures: UIViewController? {
        children.last
    }

#if canImport(UIKit, _version: 9127.0.85)
    @available(iOS 27.1, *)
    override var childForPreferredVerticalBarBehavior: UIViewController? {
        children.last
    }
#endif

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        contentController.supportedInterfaceOrientations
    }

    override var preferredInterfaceOrientationForPresentation: UIInterfaceOrientation {
        contentController.preferredInterfaceOrientationForPresentation
    }

    override func pressesBegan(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        if ControllerEventDeliveryCoordinator.shared
            .handleMenuPressesEvent(presses) {
            return
        }
        super.pressesBegan(presses, with: event)
    }

    override func pressesChanged(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        if ControllerEventDeliveryCoordinator.shared.shouldConsumeDirectionalPresses,
           presses.contains(where: { $0.type.isDirectional }) {
            return
        }
        super.pressesChanged(presses, with: event)
    }

    override func pressesEnded(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        if ControllerEventDeliveryCoordinator.shared.shouldConsumeDirectionalPresses,
           presses.contains(where: { $0.type.isDirectional }) {
            return
        }
        super.pressesEnded(presses, with: event)
    }

    override func pressesCancelled(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        if ControllerEventDeliveryCoordinator.shared.shouldConsumeDirectionalPresses,
           presses.contains(where: { $0.type.isDirectional }) {
            return
        }
        super.pressesCancelled(presses, with: event)
    }
}

private extension UIPress.PressType {
    var isDirectional: Bool {
        self == .upArrow || self == .downArrow
            || self == .leftArrow || self == .rightArrow
    }
}

private extension UIKeyboardHIDUsage {
    var menuControllerCommand: MenuControllerCommand? {
        switch rawValue {
        case 82: .up
        case 81: .down
        case 80: .left
        case 79: .right
        default: nil
        }
    }
}

/// A deliberately small bridge between UIKit presses and the SwiftUI-owned
/// menu router, which reads controllers through their GameController profile.
@MainActor
final class ControllerEventDeliveryCoordinator {
    static let shared = ControllerEventDeliveryCoordinator()

    private weak var router: MenuControllerInputRouter?
    private(set) var rightStickScrolling = false
    private var menuActive = false

    private init() {}

    var shouldConsumeDirectionalPresses: Bool {
        menuActive
    }

    /// A controller's UIKit copy of a press must not reach SwiftUI, where a
    /// focused NavigationLink takes a duplicate direction as activation on iPad.
    /// Keyboard arrows are bridged here, for Simulator and hardware keyboards.
    func handleMenuPressesEvent(_ presses: Set<UIPress>) -> Bool {
        guard menuActive else { return false }
        if let keyboardPress = presses.first(where: {
            $0.key?.keyCode.menuControllerCommand != nil
        }) {
            if keyboardPress.phase == .began,
               let command = keyboardPress.key?.keyCode.menuControllerCommand {
                router?.handleKeyboardDirectionalPress(command)
            }
            return true
        }

        // The iPad crash report shows a keyless _UIGameControllerEvent being
        // converted into a UIKit presses event. Its L1 shoulder recognizer then
        // moved native focus and selected a SwiftUI NavigationLink while the
        // custom Settings graph was switching tabs. Profile-driven screens own
        // every such keyless controller press, so UIKit must not see the copy.
        return presses.contains(where: { $0.key == nil })
    }

    func install(router: MenuControllerInputRouter) {
        self.router = router
    }

    func setMenuActive(_ active: Bool) {
        menuActive = active
        if !active { rightStickScrolling = false }
    }

    func setRightStickScrolling(_ active: Bool) {
        rightStickScrolling = active
    }
}

extension View {
    /// A full-screen cover gets its own controller, which answers for the status
    /// bar itself, so it repeats the choice the app's hosting controller makes.
    func appStatusBarHidden() -> some View {
        statusBarHidden(AppState.shared.hideStatusBar)
    }

    /// Turns iPhone Duo's status column off for the full-screen cover this view sits in.
    @ViewBuilder
    func statusColumnDisabled() -> some View {
#if canImport(UIKit, _version: 9127.0.85)
        background(StatusColumnDisabler())
#else
        self
#endif
    }
}

#if canImport(UIKit, _version: 9127.0.85)
private struct StatusColumnDisabler: UIViewControllerRepresentable {
    final class Controller: UIViewController {
        @available(iOS 27.1, *)
        override var preferredVerticalBarBehavior: UIVerticalBarBehavior { .disabled }

        override func didMove(toParent parent: UIViewController?) {
            super.didMove(toParent: parent)
            if #available(iOS 27.1, *) {
                parent?.setNeedsUpdateOfVerticalBarConfiguration()
            }
        }
    }

    func makeUIViewController(context: Context) -> Controller { Controller() }
    func updateUIViewController(_ controller: Controller, context: Context) {}
}
#endif

/// Custom hosting controller that respects fullScreen state for status bar hiding
class ARMSX2HostingController<Content: View>: UIHostingController<Content> {
    override var prefersStatusBarHidden: Bool {
        AppState.shared.hideStatusBar
    }
    override var prefersHomeIndicatorAutoHidden: Bool {
        AppState.shared.hideStatusBar || AppState.shared.hideHomeIndicator
    }
    override var preferredStatusBarUpdateAnimation: UIStatusBarAnimation {
        .fade
    }
#if canImport(UIKit, _version: 9127.0.85)
    // A mounted NavigationStack would answer for the game screen, so no child while playing.
    @available(iOS 27.1, *)
    override var preferredVerticalBarBehavior: UIVerticalBarBehavior {
        AppState.shared.currentScreen == .playing ? .disabled : .automatic
    }
    @available(iOS 27.1, *)
    override var childForPreferredVerticalBarBehavior: UIViewController? {
        AppState.shared.currentScreen == .playing ? nil : super.childForPreferredVerticalBarBehavior
    }
#endif

    override func viewDidLoad() {
        super.viewDidLoad()
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(systemChromeNeedsUpdate),
            name: AppState.systemChromeNeedsUpdateNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(systemChromeNeedsUpdate),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    deinit {
        NotificationCenter.default.removeObserver(
            self,
            name: AppState.systemChromeNeedsUpdateNotification,
            object: nil
        )
        NotificationCenter.default.removeObserver(
            self,
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        applyNativeContentScale(to: view)
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        applyNativeContentScale(to: view)
        // The intro preference is seeded before this child is attached. Ask
        // the complete parent chain again once UIKit has a visible window.
        systemChromeNeedsUpdate()
    }

    /// Force SwiftUI to re-evaluate its layout when the device rotates.
    ///
    /// This hosting controller is a child of SDL's root view controller. While
    /// UIKit does forward `viewWillTransition` to child controllers,
    /// `UIHostingController`'s internal layout engine sometimes fails to
    /// invalidate its `GeometryReader` contents promptly — especially when the
    /// view is pinned via Auto Layout constraints rather than living directly
    /// under the window. Without this nudge, SwiftUI keeps stale geometry after
    /// rotation, producing broken layouts (black bars, cropped viewports,
    /// misplaced touch controls). We force a re-evaluation by toggling the
    /// `rootView` on the animation coordinator so the re-layout rides the
    /// standard rotation animation block.
    override func viewWillTransition(to size: CGSize, with coordinator: any UIViewControllerTransitionCoordinator) {
        super.viewWillTransition(to: size, with: coordinator)
        // Re-assigning rootView forces UIHostingController to invalidate its
        // internal sizing cache and re-measure for the new container size.
        let current = rootView
        coordinator.animate(alongsideTransition: { _ in
            self.rootView = current
            self.view.setNeedsLayout()
            self.view.layoutIfNeeded()
        }, completion: { _ in
            // iPhone Duo's scene reaches the other screen only as the transition completes, after its hinge update.
            AppState.shared.sizeTransitions &+= 1
        })
    }

    @objc private func systemChromeNeedsUpdate() {
        // UIKit asks the window's controller hierarchy for system chrome.
        // Updating only this nested hosting controller can leave the SDL and
        // GCEvent parents with their cached answer for the entire intro.
        // Invalidate every owner up to the window root.
        var controller: UIViewController? = self
        while let current = controller {
            current.setNeedsStatusBarAppearanceUpdate()
            current.setNeedsUpdateOfHomeIndicatorAutoHidden()
#if canImport(UIKit, _version: 9127.0.85)
            if #available(iOS 27.1, *) {
                current.setNeedsUpdateOfVerticalBarConfiguration()
            }
#endif
            controller = current.parent
        }
    }

    private func applyNativeContentScale(to view: UIView) {
        let screen = view.window?.screen ?? UIScreen.main
        let scale = max(screen.nativeScale, screen.scale, 1.0)
        view.contentScaleFactor = scale
        view.layer.contentsScale = scale
        for subview in view.subviews {
            applyNativeContentScale(to: subview)
        }
    }
}


@objc public class SwiftUIHost: NSObject {
    @MainActor
    @objc(handleControllerPressesEvent:)
    public static func handleControllerPressesEvent(
        _ event: UIPressesEvent
    ) -> Bool {
        ControllerEventDeliveryCoordinator.shared
            .handleMenuPressesEvent(event.allPresses)
    }

    @MainActor
    @objc(createControllerEventHostWithContentController:)
    public static func createControllerEventHost(
        contentController: UIViewController
    ) -> UIViewController {
        ARMSX2ControllerEventHostViewController(
            contentController: contentController
        )
    }

    @MainActor
    @objc public static func suppressAutomaticGameStartup() {
        AppState.shared.suppressAutomaticGameStartup()
    }

    @MainActor
    @objc public static func createMenuController() -> UIViewController {
        let hostingController = ARMSX2HostingController(rootView: RootView())
        hostingController.view.backgroundColor = .clear
        hostingController.view.isOpaque = false
#if canImport(UIKit, _version: 9127.0.85)
        if #available(iOS 27.1, *) {
            hostingController.view.addInteraction(UIHingeInteraction { _, update in
                let state = AppState.shared
                let partiallyOpen = update.hinge?.status == .partiallyOpen
                let present = update.hinge.map { $0.status != .unknown } ?? false
                if state.hingePartiallyOpen != partiallyOpen { state.hingePartiallyOpen = partiallyOpen }
                if state.hingePresent != present { state.hingePresent = present }
            })
        }
#endif
        return hostingController
    }

    @MainActor
    @objc public static func createExternalDisplayController() -> UIViewController {
        let hostingController = UIHostingController(rootView: ExternalGameDisplayView())
        hostingController.view.backgroundColor = .black
        return hostingController
    }

    @MainActor
    @objc public static func setExternalDisplayConnected(_ connected: Bool) {
        AppState.shared.externalDisplayConnected = connected
    }

    // Device haptic fallback for game rumble. Called from ARMSX2Bridge on the
    // main queue when no rumble-capable controller is connected.
    @MainActor
    @objc public static func triggerDeviceHaptic(large: UInt, small: UInt) {
        GameEventHaptics.shared.trigger(
            large: UInt16(truncatingIfNeeded: large),
            small: UInt16(truncatingIfNeeded: small)
        )
    }
}

extension UIApplication {
    /// The app's own scene; an AirPlay scene can enumerate first and has a key window too.
    var appWindowScene: UIWindowScene? {
        connectedScenes.first { $0.session.role == .windowApplication } as? UIWindowScene
    }

    /// The screen of the app's own scene.
    var appScreen: UIScreen {
        appWindowScene?.screen ?? UIScreen.main
    }
}

/// iPad, or a phone window whose shorter side is at least 600 pt (iPhone Duo's inner screen).
@MainActor
func usesLargeLayout(windowSize: CGSize? = nil) -> Bool {
    switch UIDevice.current.userInterfaceIdiom {
    case .pad:
        return true
    case .phone:
        let size = windowSize ?? UIApplication.shared.appWindowScene?.coordinateSpace.bounds.size ?? .zero
        return min(size.width, size.height) >= 600
    default:
        return false
    }
}

extension GeometryProxy {
    /// The fold while the hinge is half open, in this proxy's space.
    func activeFold() -> CGRect? {
#if canImport(SwiftUI, _version: 8.0.85)
        guard AppState.shared.hingePartiallyOpen, #available(iOS 27.1, *) else { return nil }
        let bounds = CGRect(origin: .zero, size: size)
        return reservedRegions(kind: .division)
            .first { $0.isActive && $0.frame.intersects(bounds) }?
            .frame
#else
        return nil
#endif
    }

    /// The fold in book pose (running top to bottom) or in tabletop (running across).
    func activeFold(book: Bool) -> CGRect? {
        activeFold().flatMap { (book ? $0.height > $0.width : $0.width > $0.height) ? $0 : nil }
    }
}

extension ToolbarContent {
    /// Moves the item into the vertical bar when one is shown.
    @ToolbarContentBuilder
    func prefersVerticalBar() -> some ToolbarContent {
#if canImport(SwiftUI, _version: 8.0.85)
        if #available(iOS 27.1, *) {
            axisBehavior(.verticalPreferred)
        } else {
            self
        }
#else
        self
#endif
    }
}

extension View {
    /// Centres the view in the leading pane while the fold splits it in book pose.
    func centredInLeadingPaneOnFold() -> some View {
        modifier(LeadingPaneOnFold())
    }
}

private struct LeadingPaneOnFold: ViewModifier {
    @State private var shift: CGFloat = 0

    func body(content: Content) -> some View {
        content
            .offset(x: shift)
            .background {
                GeometryReader { geo in
                    let target = geo.activeFold(book: true).map { $0.minX / 2 - geo.size.width / 2 } ?? 0
                    Color.clear.onChange(of: target, initial: true) { _, target in shift = target }
                }
            }
    }
}
