// ControllerAccessibilityNavigationModifier.swift — the navigation scope modifier and its overlays
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private final class ControllerAccessibilityScopeProbeView: UIView {
    weak var session: ControllerAccessibilityNavigationSession?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        session?.updateScope(view: self)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        session?.updateScope(view: self)
    }
}

private struct ControllerAccessibilityScopeProbe: UIViewRepresentable {
    let session: ControllerAccessibilityNavigationSession

    func makeUIView(context: Context) -> ControllerAccessibilityScopeProbeView {
        let view = ControllerAccessibilityScopeProbeView()
        view.session = session
        return view
    }

    func updateUIView(
        _ uiView: ControllerAccessibilityScopeProbeView,
        context: Context
    ) {
        uiView.session = session
        uiView.setNeedsLayout()
    }
}

private struct ControllerAccessibilityRegisteringButtonStyle: PrimitiveButtonStyle {
    let isActive: Bool
    @Environment(\.controllerAccessibilityAutomaticTargetSuppressed)
    private var isSuppressed

    @ViewBuilder
    func makeBody(configuration: Configuration) -> some View {
        if isActive, !isSuppressed {
            Button(configuration)
                .buttonStyle(.automatic)
                .controllerAccessibilityActionTarget(label: "") {
                    configuration.trigger()
                }
        } else { Button(configuration) }
    }
}

private struct ControllerAccessibilityRegisteringToggleStyle: ToggleStyle {
    let isActive: Bool
    @Environment(\.controllerAccessibilityAutomaticTargetSuppressed)
    private var isSuppressed

    @ViewBuilder
    func makeBody(configuration: Configuration) -> some View {
        if isActive, !isSuppressed {
            Toggle(configuration)
                .toggleStyle(.switch)
                .controllerAccessibilityToggleTarget(
                    label: "",
                    isOn: configuration.$isOn
                )
        } else { Toggle(configuration) }
    }
}

private struct ControllerAccessibilityFocusOverlay: View {
    let session: ControllerAccessibilityNavigationSession
    let controllerInput: MenuControllerInputRouter?
    let style: ControllerNavigationOrbStyle
    let focusNeonExclusionLabels: [String]
    @State private var settings = SettingsStore.shared

    var body: some View {
        if controllerInput?.navigationZone != .tabBar,
           session.ownsInput(in: controllerInput),
           let frame = session.focusedOverlayFrame {
            let usesWindowFocusVisual = focusNeonExclusionLabels.contains(
                session.focusedAccessibilityLabel ?? ""
            )
            Group {
                if !usesWindowFocusVisual {
                    ControllerNavigationAnimatedOrbField(
                        targetID: session.focusedElementID ?? "focused-element",
                        targetFrame: frame,
                        primaryColor: settings.controllerNavigationAccentColor,
                        style: style,
                        controllerInput: controllerInput,
                        interactionOverride: session.latestOrbInteraction,
                        inset: 2,
                        orbScale: 0.72,
                        showsOrbs: settings.focusOrbsEnabled,
                        showsNeonOutline: true,
                        tracksTargetFrameDirectly:
                            session.isScrollPresentationActive,
                        neonCornerRadius:
                            session.focusedNeonCornerRadius ?? 12
                    )
                }
            }
            .opacity(
                session.focusPresentationIsHiddenForScrolling ? 0 : 1
            )
            .animation(
                .easeOut(duration: 0.14),
                value: session.focusPresentationIsHiddenForScrolling
            )
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            // The frame is in physical points; right-to-left would mirror the ring off its row.
            .environment(\.layoutDirection, .leftToRight)
            .zIndex(90_000)
        }
    }
}

/// Toolbar targets can live in a `UINavigationBar`, which is composited above
/// the SwiftUI content hierarchy. Install their focus presentation directly in
/// the window so the neon and orbs cannot be hidden behind the navigation bar.
private struct ControllerAccessibilityWindowFocusOverlay: View {
    let session: ControllerAccessibilityNavigationSession
    let controllerInput: MenuControllerInputRouter?
    let style: ControllerNavigationOrbStyle
    let includedLabels: [String]
    @State private var settings = SettingsStore.shared

    var body: some View {
        ZStack(alignment: .topLeading) {
            if includedLabels.contains(
                session.focusedAccessibilityLabel ?? ""
            ), controllerInput?.isNavigationCaptured != true,
               controllerInput?.navigationZone != .tabBar,
               session.ownsInput(in: controllerInput),
               let frame = session.focusedFrame {
                ControllerNavigationAnimatedOrbField(
                    targetID: session.focusedElementID ?? "focused-toolbar-element",
                    targetFrame: frame,
                    primaryColor: settings.controllerNavigationAccentColor,
                    style: style,
                    controllerInput: controllerInput,
                    interactionOverride: session.latestOrbInteraction,
                    inset: 2,
                    orbScale: 0.72,
                    showsOrbs: settings.focusOrbsEnabled,
                    showsNeonOutline: true,
                    tracksTargetFrameDirectly:
                        session.isScrollPresentationActive
                )
                .opacity(
                    session.focusPresentationIsHiddenForScrolling ? 0 : 1
                )
                .animation(
                    .easeOut(duration: 0.14),
                    value: session.focusPresentationIsHiddenForScrolling
                )
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .environment(\.layoutDirection, .leftToRight)
    }
}

@MainActor
private final class ControllerAccessibilityWindowFocusOverlayInstallerView:
    UIView
{
    private weak var installedWindow: UIWindow?
    private var hostingController:
        UIHostingController<ControllerAccessibilityWindowFocusOverlay>?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        installIfNeeded()
    }

    func configure(
        session: ControllerAccessibilityNavigationSession,
        controllerInput: MenuControllerInputRouter?,
        style: ControllerNavigationOrbStyle,
        includedLabels: [String]
    ) {
        let overlay = ControllerAccessibilityWindowFocusOverlay(
            session: session,
            controllerInput: controllerInput,
            style: style,
            includedLabels: includedLabels
        )
        if let hostingController {
            hostingController.rootView = overlay
        } else {
            let hostingController = UIHostingController(rootView: overlay)
            hostingController.view.backgroundColor = .clear
            hostingController.view.isUserInteractionEnabled = false
            hostingController.view.accessibilityElementsHidden = true
            self.hostingController = hostingController
        }
        installIfNeeded()
    }

    func uninstall() {
        hostingController?.view.removeFromSuperview()
        hostingController = nil
        installedWindow = nil
    }

    private func installIfNeeded() {
        guard let window, let hostedView = hostingController?.view else {
            if self.window == nil {
                hostedViewIfPresent?.removeFromSuperview()
                installedWindow = nil
            }
            return
        }
        if installedWindow !== window || hostedView.superview !== window {
            hostedView.removeFromSuperview()
            hostedView.translatesAutoresizingMaskIntoConstraints = false
            window.addSubview(hostedView)
            NSLayoutConstraint.activate([
                hostedView.leadingAnchor.constraint(equalTo: window.leadingAnchor),
                hostedView.trailingAnchor.constraint(equalTo: window.trailingAnchor),
                hostedView.topAnchor.constraint(equalTo: window.topAnchor),
                hostedView.bottomAnchor.constraint(equalTo: window.bottomAnchor),
            ])
            installedWindow = window
        } else {
            window.bringSubviewToFront(hostedView)
        }
    }

    private var hostedViewIfPresent: UIView? { hostingController?.view }
}

private struct ControllerAccessibilityWindowFocusOverlayInstaller:
    UIViewRepresentable
{
    let session: ControllerAccessibilityNavigationSession
    let controllerInput: MenuControllerInputRouter?
    let style: ControllerNavigationOrbStyle
    let includedLabels: [String]

    func makeUIView(context: Context) ->
        ControllerAccessibilityWindowFocusOverlayInstallerView
    {
        let view = ControllerAccessibilityWindowFocusOverlayInstallerView()
        view.isHidden = true
        view.isUserInteractionEnabled = false
        return view
    }

    func updateUIView(
        _ uiView: ControllerAccessibilityWindowFocusOverlayInstallerView,
        context: Context
    ) {
        uiView.configure(
            session: session,
            controllerInput: controllerInput,
            style: style,
            includedLabels: includedLabels
        )
    }

    static func dismantleUIView(
        _ uiView: ControllerAccessibilityWindowFocusOverlayInstallerView,
        coordinator: Void
    ) {
        uiView.uninstall()
    }
}

private struct ControllerAccessibilityNavigationModifier: ViewModifier {
    let controllerInput: MenuControllerInputRouter?
    let isActive: Bool
    let scopeKey: String
    let priority: Int
    let orbStyle: ControllerNavigationOrbStyle
    let focusNeonExclusionLabels: [String]
    let onBack: (@MainActor () -> Bool)?
    let onContextMenu: (@MainActor (String) -> Bool)?
    let onPreviousTab: (@MainActor () -> Bool)?
    let onNextTab: (@MainActor () -> Bool)?
    let onBoundary: (@MainActor (MenuControllerCommand) -> Bool)?
    let boundaryRules: [ControllerAccessibilityBoundaryRule]
    let directionalLinks: [ControllerAccessibilityDirectionalLink]
    let prioritizesDirectionalLinks: Bool
    let confinesHorizontalFocusMovement: Bool
    let wrapsAtListEnds: Bool
    let usesExplicitTargetGeometryOnly: Bool
    let preservesFocusDuringRightStickScrolling: Bool
    let focusScrollBehavior: ControllerAccessibilityFocusScrollBehavior
    let scrollAnimationDuration: Double
    let focusViewportEdgeMargin: CGFloat
    let focusTopAlignmentMargin: CGFloat
    let focusBottomAlignmentMargin: CGFloat
    let onActivateFocusedLabel: (@MainActor (String) -> Bool)?
    let onAdjustFocusedTarget: (@MainActor (String, String?, Bool) -> Void)?
    let preferredInitialFocusLabel: String?
    let preferredLastEntryLabel: String?
    let preferredTrailingFocusLabels: [String]
    let declaredTargetOrder: [String]?

    @State private var registrationID = UUID()
    @State private var session = ControllerAccessibilityNavigationSession()

    func body(content: Content) -> some View {
        ScrollViewReader { proxy in
            inputLifecycle(
                configurationLifecycle(navigationContent(content)),
                proxy: proxy
            )
        }
    }

    private func configurationLifecycle<Body: View>(
        _ content: Body
    ) -> some View {
        content
            .onChange(of: isActive) { _, _ in updateRegistration() }
            .onChange(of: scopeKey) { _, _ in updateRegistration() }
            .onChange(of: boundaryRules) { _, _ in updateRegistration() }
            .onChange(of: directionalLinks) { _, _ in updateRegistration() }
            .onChange(of: prioritizesDirectionalLinks) { _, _ in
                updateRegistration()
            }
            .onChange(of: confinesHorizontalFocusMovement) { _, _ in
                updateRegistration()
            }
            .onChange(of: wrapsAtListEnds) { _, _ in updateRegistration() }
            .onChange(of: focusScrollBehavior) { _, _ in updateRegistration() }
            .onChange(of: focusViewportEdgeMargin) { _, _ in
                updateRegistration()
            }
            .onChange(of: focusTopAlignmentMargin) { _, _ in
                updateRegistration()
            }
            .onChange(of: focusBottomAlignmentMargin) { _, _ in
                updateRegistration()
            }
            .onChange(of: preferredInitialFocusLabel) { _, _ in
                updateRegistration()
            }
            .onChange(of: preferredLastEntryLabel) { _, _ in
                updateRegistration()
            }
            .onChange(of: preferredTrailingFocusLabels) { _, _ in
                updateRegistration()
            }
            .onChange(of: declaredTargetOrder) { _, _ in updateRegistration() }
    }

    private func inputLifecycle<Body: View>(
        _ content: Body,
        proxy: ScrollViewProxy
    ) -> some View {
        content
            .onAppear {
                session.setScrollAction { id, anchor in
                    if scrollAnimationDuration > 0 {
                        let acceleration = max(
                            1,
                            controllerInput?.directionalRepeatAcceleration ?? 1
                        )
                        let duration = controllerInput?
                            .isRepeatingDirectionCommand == true
                                ? min(
                                    scrollAnimationDuration
                                        / (acceleration * acceleration),
                                    0.085 / acceleration
                                )
                                : scrollAnimationDuration
                        withAnimation(
                            .easeOut(duration: duration)
                        ) {
                            proxy.scrollTo(id, anchor: anchor)
                        }
                    } else {
                        proxy.scrollTo(id, anchor: anchor)
                    }
                }
                updateRegistration()
            }
            .onChange(of: controllerInput?.hasConnectedController) { _, _ in
                updateRegistration()
            }
            .onChange(of: controllerInput?.isMenuActive) { _, _ in
                updateRegistration()
            }
            // A touch drops every session, and a screen that keeps its router
            // gets no other signal when the pad takes over again.
            .onChange(of: controllerInput?.isControllerNavigationEnabled) { _, enabled in
                if enabled == true { updateRegistration() }
            }
            .onChange(of: controllerInput?.focusReleaseSequence) { _, _ in
                // Analog begin/end is delivered synchronously to its owner.
                // A delayed SwiftUI observation must not hide a newer focus.
                if navigationIsActive,
                   controllerInput?.isControllerNavigationEnabled == false {
                    session.suspendFocusForScrolling()
                }
            }
            .onDisappear {
                controllerInput?.unregisterNavigationSession(
                    id: registrationID
                )
                session.clearScrollAction()
                session.deactivate()
            }
    }

    private var navigationIsActive: Bool {
        isActive && controllerInput?.isMenuActive == true
            && controllerInput?.hasConnectedController == true
    }

    // One chain in every state. A branch here gave the screen a second identity, so
    // a controller connecting or sleeping rebuilt it, the Settings stack included.
    private func navigationContent(_ content: Content) -> some View {
        content
            .environment(\.controllerAccessibilityNavigationActive, navigationIsActive)
            .environment(
                \.controllerAccessibilityNavigationSession,
                navigationIsActive ? session : nil
            )
            .environment(
                \.controllerAccessibilityNavigationRegistrationID,
                navigationIsActive ? registrationID : nil
            )
            .environment(
                \.controllerAccessibilityUsesWindowFocusVisual,
                !focusNeonExclusionLabels.isEmpty
            )
            .environment(
                \.controllerAccessibilityAutomaticTargetSuppressed,
                !registersAutomaticTargets
            )
            .buttonStyle(
                ControllerAccessibilityRegisteringButtonStyle(
                    isActive: registersAutomaticTargets
                )
            )
            .toggleStyle(
                ControllerAccessibilityRegisteringToggleStyle(
                    isActive: registersAutomaticTargets
                )
            )
            .background {
                if navigationIsActive {
                    ZStack {
                        ControllerAccessibilityScopeProbe(session: session)
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                        // One exact scroll owner serves every managed surface. The focused
                        // probe supplies its actual UIScrollView, so Settings, Appearance,
                        // Per-Game, Quick Menu, and overlays share Emulator's path.
                        ControllerRightStickScrollTarget(
                            controllerInput: controllerInput,
                            axes: .vertical,
                            // This is a fallback for screens without a concrete List/Form
                            // marker. Screen-local owners know their private SwiftUI scroll
                            // hierarchy and must be offered the gesture first.
                            priority: priority - 10_000,
                            isEnabled: navigationIsActive,
                            preferredScrollViewProvider: {
                                session.preferredRightStickScrollView()
                            },
                            // `session.configure` runs on appearance. Use the modifier's
                            // stable ID immediately so this target cannot mount ownerless
                            // and be removed by the router's active-session filter.
                            ownerSessionIDOverride: registrationID
                        )
                        .frame(width: 0, height: 0)
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                        if !focusNeonExclusionLabels.isEmpty {
                            ControllerAccessibilityWindowFocusOverlayInstaller(
                                session: session,
                                controllerInput: controllerInput,
                                style: orbStyle,
                                includedLabels: focusNeonExclusionLabels
                            )
                            .frame(width: 0, height: 0)
                        }
                    }
                }
            }
            .overlay {
                if navigationIsActive {
                    ControllerAccessibilityFocusOverlay(
                        session: session,
                        controllerInput: controllerInput,
                        style: orbStyle,
                        focusNeonExclusionLabels: focusNeonExclusionLabels
                    )
                }
            }
    }

    // The nearest scope decides for its buttons. The styles fall through to an outer
    // scope's style when inactive, so this also keeps that one from registering them.
    private var registersAutomaticTargets: Bool {
        navigationIsActive && !usesExplicitTargetGeometryOnly
    }

    private func updateRegistration() {
        session.configure(
            controllerInput: controllerInput,
            scopeKey: scopeKey,
            registrationID: registrationID,
            onBack: onBack,
            onContextMenu: onContextMenu,
            onPreviousTab: onPreviousTab,
            onNextTab: onNextTab,
            onBoundary: onBoundary,
            boundaryRules: boundaryRules,
            directionalLinks: directionalLinks,
            prioritizesDirectionalLinks: prioritizesDirectionalLinks,
            confinesHorizontalFocusMovement: confinesHorizontalFocusMovement,
            wrapsAtListEnds: wrapsAtListEnds,
            focusScrollBehavior: focusScrollBehavior,
            scrollAnimationDuration: scrollAnimationDuration,
            focusViewportEdgeMargin: focusViewportEdgeMargin,
            focusTopAlignmentMargin: focusTopAlignmentMargin,
            focusBottomAlignmentMargin: focusBottomAlignmentMargin,
            onActivateFocusedLabel: onActivateFocusedLabel,
            onAdjustFocusedTarget: onAdjustFocusedTarget,
            preferredInitialFocusLabel: preferredInitialFocusLabel,
            preferredLastEntryLabel: preferredLastEntryLabel,
            preferredTrailingFocusLabels: preferredTrailingFocusLabels
        )
        // Screens with a known semantic order must publish it before their
        // first lazy row marks the session ready. Relying only on a descendant
        // `onAppear` lets iPad List mounting accept entry with a one-row graph.
        session.ownerDeclaresOrder = declaredTargetOrder != nil
        if let declaredTargetOrder {
            session.setDeclaredOrder(declaredTargetOrder)
        }
        guard navigationIsActive, let controllerInput else {
            controllerInput?.unregisterNavigationSession(id: registrationID)
            session.deactivate()
            return
        }
        let preservesPresentation = preservesFocusDuringRightStickScrolling
        controllerInput.registerNavigationSession(
            id: registrationID,
            scopeKey: scopeKey,
            priority: priority,
            isReady: session.hasNavigableElements,
            handler: { [weak session] command in
                session?.handle(command) ?? false
            },
            entryHandler: { [weak session] preferLast in
                session?.focusContent(preferLast: preferLast) ?? false
            },
            scrollHandler: { [weak session] scrollView, direction in
                session?.followRightStickScroll(in: scrollView, direction: direction)
            },
            scrollStateHandler: { [weak session] scrolling in
                if scrolling {
                    session?.suspendFocusForScrolling(
                        preservingPresentation: preservesPresentation
                    )
                } else {
                    session?.resumeFocusAfterRightStickScrolling()
                }
            }
        )
        session.publishScrollFocus()
    }
}

extension View {
    func controllerAccessibilityNavigation(
        controllerInput: MenuControllerInputRouter?,
        isActive: Bool = true,
        scopeKey: String,
        priority: Int = 100,
        orbStyle: ControllerNavigationOrbStyle = .plain,
        focusNeonExclusionLabels: [String] = [],
        onBack: (@MainActor () -> Bool)? = nil,
        onContextMenu: (@MainActor (String) -> Bool)? = nil,
        onPreviousTab: (@MainActor () -> Bool)? = nil,
        onNextTab: (@MainActor () -> Bool)? = nil,
        onBoundary: (@MainActor (MenuControllerCommand) -> Bool)? = nil,
        boundaryRules: [ControllerAccessibilityBoundaryRule] = [],
        directionalLinks: [ControllerAccessibilityDirectionalLink] = [],
        prioritizesDirectionalLinks: Bool = false,
        confinesHorizontalFocusMovement: Bool = false,
        wrapsAtListEnds: Bool = false,
        usesExplicitTargetGeometryOnly: Bool = false,
        preservesFocusDuringRightStickScrolling: Bool = false,
        focusScrollBehavior: ControllerAccessibilityFocusScrollBehavior =
            .revealIfNeeded,
        scrollAnimationDuration: Double = 0.22,
        focusViewportEdgeMargin: CGFloat = 12,
        focusTopAlignmentMargin: CGFloat = 0,
        focusBottomAlignmentMargin: CGFloat = 0,
        onActivateFocusedLabel: (@MainActor (String) -> Bool)? = nil,
        onAdjustFocusedTarget:
            (@MainActor (String, String?, Bool) -> Void)? = nil,
        preferredInitialFocusLabel: String? = nil,
        preferredLastEntryLabel: String? = nil,
        preferredTrailingFocusLabels: [String] = [],
        declaredTargetOrder: [String]? = nil
    ) -> some View {
        modifier(
            ControllerAccessibilityNavigationModifier(
                controllerInput: controllerInput,
                isActive: isActive,
                scopeKey: scopeKey,
                priority: priority,
                orbStyle: orbStyle,
                focusNeonExclusionLabels: focusNeonExclusionLabels,
                onBack: onBack,
                onContextMenu: onContextMenu,
                onPreviousTab: onPreviousTab,
                onNextTab: onNextTab,
                onBoundary: onBoundary,
                boundaryRules: boundaryRules,
                directionalLinks: directionalLinks,
                prioritizesDirectionalLinks: prioritizesDirectionalLinks,
                confinesHorizontalFocusMovement: confinesHorizontalFocusMovement,
                wrapsAtListEnds: wrapsAtListEnds,
                usesExplicitTargetGeometryOnly: usesExplicitTargetGeometryOnly,
                preservesFocusDuringRightStickScrolling:
                    preservesFocusDuringRightStickScrolling,
                focusScrollBehavior: focusScrollBehavior,
                scrollAnimationDuration: scrollAnimationDuration,
                focusViewportEdgeMargin: focusViewportEdgeMargin,
                focusTopAlignmentMargin: focusTopAlignmentMargin,
                focusBottomAlignmentMargin: focusBottomAlignmentMargin,
                onActivateFocusedLabel: onActivateFocusedLabel,
                onAdjustFocusedTarget: onAdjustFocusedTarget,
                preferredInitialFocusLabel: preferredInitialFocusLabel,
                preferredLastEntryLabel: preferredLastEntryLabel,
                preferredTrailingFocusLabels: preferredTrailingFocusLabels,
                declaredTargetOrder: declaredTargetOrder
            )
        )
    }
}
