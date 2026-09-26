// SwiftUIHost.swift — ObjC-callable helper to create SwiftUI hosting controllers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
#if canImport(GameController)
import GameController
#endif

/// Owns the controller delivery boundary for the complete SDL/SwiftUI window.
/// Gameplay keeps profile delivery, while the menu can opt into UIKit press
/// delivery on systems which can also preserve the analog profile callbacks.
@MainActor
@objc(ARMSX2ControllerEventHostViewController)
final class ARMSX2ControllerEventHostViewController: GCEventViewController {
    private let contentController: UIViewController

    @objc init(contentController: UIViewController) {
        self.contentController = contentController
        super.init(nibName: nil, bundle: nil)
        controllerUserInteractionEnabled = false
        ControllerEventDeliveryCoordinator.shared.install(host: self)
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

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        ControllerEventDeliveryCoordinator.shared.hostDidEnterWindow()
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

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        contentController.supportedInterfaceOrientations
    }

    override var preferredInterfaceOrientationForPresentation: UIInterfaceOrientation {
        contentController.preferredInterfaceOrientationForPresentation
    }

    func setNativeControllerDeliveryEnabled(_ enabled: Bool) {
        controllerUserInteractionEnabled = enabled
        if #available(iOS 26.0, *), let loadedView = viewIfLoaded {
            for case let interaction as GCEventInteraction in loadedView.interactions {
                interaction.receivesEventsInView = enabled
            }
        }
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
        if presses.contains(where: { $0.type == .menu }),
           ControllerEventDeliveryCoordinator.shared.handleBackPress() {
            return
        }
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

    var menuControllerCommand: MenuControllerCommand? {
        switch self {
        case .upArrow: .up
        case .downArrow: .down
        case .leftArrow: .left
        case .rightArrow: .right
        default: nil
        }
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

/// A deliberately small bridge between GameController's process-wide delivery
/// mode and the SwiftUI-owned menu router. iOS 26 can deliver gamepad events to
/// both UIKit and profile callbacks; older systems keep the deterministic
/// profile-driven fallback so right-stick scrolling is never lost.
@MainActor
final class ControllerEventDeliveryCoordinator {
    static let shared = ControllerEventDeliveryCoordinator()
    nonisolated static let nativeFocusModeDidChange = Notification.Name(
        "ARMSX2ControllerNativeFocusModeDidChange"
    )

    private weak var host: ARMSX2ControllerEventHostViewController?
    private weak var router: MenuControllerInputRouter?
    private(set) var nativeFocusEnabled = false
    private(set) var rightStickScrolling = false
    private var menuActive = false
    // The app's menu is profile-driven unless the foreground navigation
    // session explicitly opts into UIKit focus. A scene merely supporting a
    // focus system (which is normal on iPad) is not enough to transfer input
    // ownership away from the custom controller graph.
    private var nativeFocusAuthorized = false

    private init() {}

    var shouldConsumeDirectionalPresses: Bool {
        menuActive && (!nativeFocusEnabled || rightStickScrolling)
    }

    /// Profile-driven screens must not also forward the controller's UIKit
    /// press copy into SwiftUI. A focused NavigationLink treats that duplicate
    /// direction as activation on iPad. Physical controllers continue through
    /// their GCController profile; keyboard arrows are bridged here so the
    /// same deterministic graph remains usable in Simulator and on hardware.
    func handleMenuPressesEvent(_ presses: Set<UIPress>) -> Bool {
        guard menuActive else { return false }
        if nativeFocusEnabled {
            return rightStickScrolling
                && presses.contains(where: { $0.type.isDirectional })
        }

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

    func install(host: ARMSX2ControllerEventHostViewController) {
        self.host = host
        updateDeliveryMode()
    }

    func install(router: MenuControllerInputRouter) {
        self.router = router
    }

    func hostDidEnterWindow() {
        updateDeliveryMode()
    }

    func setMenuActive(_ active: Bool) {
        menuActive = active
        if !active { rightStickScrolling = false }
        updateDeliveryMode()
    }

    func setRightStickScrolling(_ active: Bool) {
        rightStickScrolling = active
    }

    func handleBackPress() -> Bool {
        guard nativeFocusEnabled else { return false }
        return router?.handleNativeBackPress() == true
    }

    func setNativeFocusAuthorized(_ authorized: Bool) {
        guard authorized != nativeFocusAuthorized else { return }
        nativeFocusAuthorized = authorized
        updateDeliveryMode()
    }

    private func updateDeliveryMode() {
        guard let host else {
            nativeFocusEnabled = false
            return
        }

        let previousMode = nativeFocusEnabled
        if #available(iOS 26.0, *),
           menuActive,
           nativeFocusAuthorized,
           host.viewIfLoaded?.window?.windowScene?.focusSystem != nil {
            nativeFocusEnabled = true
            host.setNativeControllerDeliveryEnabled(true)
        } else {
            nativeFocusEnabled = false
            host.setNativeControllerDeliveryEnabled(false)
        }
        if nativeFocusEnabled != previousMode {
            NotificationCenter.default.post(
                name: Self.nativeFocusModeDidChange,
                object: nil
            )
        }
    }
}

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
}
