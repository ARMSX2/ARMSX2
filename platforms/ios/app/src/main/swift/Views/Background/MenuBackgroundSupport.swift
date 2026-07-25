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

/// Owns exactly one live menu background renderer and moves its UIKit surface
/// between the selected tab's background attachment point. Reparenting preserves
/// video/display-link/Metal state instead of rebuilding those resources whenever
/// the user selects another tab.
@MainActor
final class PersistentMenuBackgroundHost: ObservableObject {
    let sessionStart = Date()

    private var hostingController: UIHostingController<AnyView>?
    private weak var activeAttachment: UIView?
    private var hasLoadedRenderer = false
    private var selectedTabAllowsBackground = true
    private var exclusivePreviewDepth = 0

    func attach(to attachment: UIView) {
        activeAttachment = attachment
        guard selectedTabAllowsBackground, exclusivePreviewDepth == 0 else {
            return
        }

        let controller = makeHostingControllerIfNeeded()
        loadRendererIfNeeded(in: controller)

        controller.loadViewIfNeeded()
        guard let hostedView = controller.view else { return }
        if hostedView.superview !== attachment {
            UIView.performWithoutAnimation {
                hostedView.removeFromSuperview()
                hostedView.frame = attachment.bounds
                hostedView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
                attachment.insertSubview(hostedView, at: 0)
                attachment.layoutIfNeeded()
                hostedView.layoutIfNeeded()
            }
        }
        hostedView.isHidden = false
    }

    func setSelectedTabAllowsBackground(_ isAllowed: Bool) {
        selectedTabAllowsBackground = isAllowed
        guard isAllowed else {
            // A disabled destination has no valid background attachment.
            // Clearing it also prevents a later enable from briefly restoring
            // the renderer into an off-screen tab retained by TabView.
            activeAttachment = nil
            unloadRenderer()
            return
        }

        // TabView may update the destination's UIViewRepresentable before this
        // selection permission changes. In that order attach(to:) records the
        // new view but correctly refuses to load while permission is still
        // false. Retry that recorded attachment now so disabled → enabled tab
        // transitions cannot remain blank.
        guard exclusivePreviewDepth == 0, let activeAttachment else { return }
        attach(to: activeAttachment)
    }

    /// The Appearance screen uses the same expensive renderer in its preview.
    /// Temporarily unload the full-screen copy so only one background renderer
    /// can exist while that preview is visible.
    func beginExclusivePreview() {
        exclusivePreviewDepth += 1
        if exclusivePreviewDepth == 1 {
            unloadRenderer()
        }
    }

    func endExclusivePreview() {
        exclusivePreviewDepth = max(0, exclusivePreviewDepth - 1)
        guard exclusivePreviewDepth == 0,
              selectedTabAllowsBackground,
              let activeAttachment else {
            return
        }
        attach(to: activeAttachment)
    }

    /// Stops the renderer only when the selected destination is configured not
    /// to show a background. Normal tab changes between enabled destinations do
    /// not call this method.
    func suspend() {
        activeAttachment = nil
        unloadRenderer()
    }

    private func unloadRenderer() {
        hostingController?.view.removeFromSuperview()
        guard let hostingController, hasLoadedRenderer else { return }
        hostingController.rootView = AnyView(Color.clear)
        hasLoadedRenderer = false
    }

    /// Destroys the hosted SwiftUI tree when the complete menu hierarchy leaves
    /// the root (for example, when gameplay begins).
    func release() {
        suspend()
        hostingController = nil
    }

    private func makeHostingControllerIfNeeded() -> UIHostingController<AnyView> {
        if let hostingController {
            return hostingController
        }

        let controller = UIHostingController(rootView: AnyView(Color.clear))
        controller.view.backgroundColor = .clear
        controller.view.isOpaque = false
        controller.view.isUserInteractionEnabled = false
        hostingController = controller
        return controller
    }

    private func loadRendererIfNeeded(in controller: UIHostingController<AnyView>) {
        guard !hasLoadedRenderer else { return }
        controller.rootView = AnyView(
            GeometryReader { geometry in
                BackgroundContainerView(size: geometry.size)
            }
            .environment(\.menuBackgroundSessionStart, sessionStart)
            .ignoresSafeArea()
            .accessibilityHidden(true)
            .allowsHitTesting(false)
        )
        hasLoadedRenderer = true
    }
}

private final class MenuBackgroundAttachmentView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isOpaque = false
        isUserInteractionEnabled = false
        clipsToBounds = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

private struct PersistentMenuBackgroundAttachment: UIViewRepresentable {
    let host: PersistentMenuBackgroundHost
    let isActive: Bool

    func makeUIView(context: Context) -> MenuBackgroundAttachmentView {
        MenuBackgroundAttachmentView()
    }

    func updateUIView(_ uiView: MenuBackgroundAttachmentView, context: Context) {
        if isActive {
            host.attach(to: uiView)
        }
    }
}

struct MenuBackgroundLayer: View {
    var isActive = true
    @Environment(\.menuBackgroundHost) private var persistentHost

    @ViewBuilder
    var body: some View {
        if let persistentHost {
            PersistentMenuBackgroundAttachment(
                host: persistentHost,
                isActive: isActive
            )
                .ignoresSafeArea()
                .accessibilityHidden(true)
                .allowsHitTesting(false)
                .transaction { transaction in
                    transaction.animation = nil
                    transaction.disablesAnimations = true
                }
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
    func menuBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(MenuBackgroundListRowModifier(isEnabled: isEnabled))
    }

    func gameCardTintMenuBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(GameCardTintMenuBackgroundListRowModifier(isEnabled: isEnabled))
    }
}
