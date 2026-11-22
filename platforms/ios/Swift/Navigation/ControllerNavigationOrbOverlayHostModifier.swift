// ControllerNavigationOrbOverlayHostModifier.swift — orb targets and the moving orb overlay
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ControllerNavigationOrbAnchorTarget {
    let id: String
    let bounds: Anchor<CGRect>
    let palette: ControllerNavigationOrbPalette
    let style: ControllerNavigationOrbStyle
    let inset: CGFloat
    let orbScale: CGFloat
    let segmentIndex: Int?
    let segmentCount: Int?
    let priority: Int
}

private struct ControllerNavigationOrbAnchorPreferenceKey: PreferenceKey {
    static let defaultValue: ControllerNavigationOrbAnchorTarget? = nil

    static func reduce(
        value: inout ControllerNavigationOrbAnchorTarget?,
        nextValue: () -> ControllerNavigationOrbAnchorTarget?
    ) {
        guard let next = nextValue() else { return }
        if value == nil || next.priority >= (value?.priority ?? .min) {
            value = next
        }
    }
}

private struct ControllerNavigationResolvedOrbTarget: Equatable {
    let id: String
    let frame: CGRect
    let palette: ControllerNavigationOrbPalette
    let style: ControllerNavigationOrbStyle
    let inset: CGFloat
    let orbScale: CGFloat
}

/// Keeps one render timeline alive while controller focus moves. OrbitKeys uses
/// the same presentation-level model: a new 0.34-second anchor transition starts
/// from the field's currently displayed position instead of creating a new field.
private struct ControllerNavigationMovingOrbOverlay: View {
    let target: ControllerNavigationResolvedOrbTarget?
    let controllerInput: MenuControllerInputRouter?

    @State private var settings = SettingsStore.shared
    @State private var displayedTarget: ControllerNavigationResolvedOrbTarget?
    @State private var isVisible = false
    @State private var removalTask: Task<Void, Never>?

    var body: some View {
        ZStack(alignment: .topLeading) {
            if let displayedTarget, displayedTarget.id.hasPrefix("tab-bar.") {
                ControllerNavigationAnimatedOrbField(
                    targetID: displayedTarget.id,
                    targetFrame: displayedTarget.frame,
                    primaryColor: displayedTarget.palette.primaryColor,
                    style: displayedTarget.style,
                    controllerInput: controllerInput,
                    inset: displayedTarget.inset,
                    orbScale: displayedTarget.orbScale,
                    showsOrbs: settings.appearance.focusOrbsEnabled,
                    showsNeonOutline: true
                )
                .opacity(isVisible ? 1 : 0)
            } else if settings.appearance.focusOrbsEnabled, let displayedTarget {
                ControllerNavigationAnimatedOrbField(
                    targetID: displayedTarget.id,
                    targetFrame: displayedTarget.frame,
                    primaryColor: displayedTarget.palette.primaryColor,
                    style: displayedTarget.style,
                    controllerInput: controllerInput,
                    inset: displayedTarget.inset,
                    orbScale: displayedTarget.orbScale
                )
                .opacity(isVisible ? 1 : 0)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onAppear {
            updateDisplayedTarget(target)
        }
        .onChange(of: target) { _, next in
            updateDisplayedTarget(next)
        }
        .onDisappear {
            removalTask?.cancel()
            removalTask = nil
        }
    }

    private func updateDisplayedTarget(
        _ next: ControllerNavigationResolvedOrbTarget?
    ) {
        removalTask?.cancel()
        removalTask = nil

        guard let next else {
            // A destination can publish one pass after its source disappears.
            // Keep the current field briefly so that transition remains seamless.
            removalTask = Task { @MainActor in
                // Lazy grids drop the old card a pass before the new one
                // publishes its anchor; wait out the controller scroll so the
                // field never blinks out between two library selections.
                try? await Task.sleep(for: .milliseconds(380))
                guard !Task.isCancelled else { return }
                withAnimation(.easeOut(duration: 0.12)) {
                    isVisible = false
                }
                try? await Task.sleep(for: .milliseconds(130))
                guard !Task.isCancelled else { return }
                displayedTarget = nil
                removalTask = nil
            }
            return
        }

        var transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            displayedTarget = next
        }
        withAnimation(.easeOut(duration: 0.12)) {
            isVisible = true
        }
    }
}

private struct ControllerNavigationOrbTargetModifier: ViewModifier {
    let id: String
    let isActive: Bool
    let isEnabled: Bool
    let palette: ControllerNavigationOrbPalette
    let style: ControllerNavigationOrbStyle
    let inset: CGFloat
    let orbScale: CGFloat
    let segmentIndex: Int?
    let segmentCount: Int?
    let priority: Int

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content.anchorPreference(
                key: ControllerNavigationOrbAnchorPreferenceKey.self,
                value: .bounds
            ) { bounds in
                isActive
                    ? ControllerNavigationOrbAnchorTarget(
                        id: id,
                        bounds: bounds,
                        palette: palette,
                        style: style,
                        inset: inset,
                        orbScale: orbScale,
                        segmentIndex: segmentIndex,
                        segmentCount: segmentCount,
                        priority: priority
                    )
                    : nil
            }
        } else {
            content
        }
    }
}

private struct ControllerNavigationOrbOverlayHostModifier: ViewModifier {
    let controllerInput: MenuControllerInputRouter?

    func body(content: Content) -> some View {
        content.overlayPreferenceValue(
            ControllerNavigationOrbAnchorPreferenceKey.self
        ) { target in
            GeometryReader { proxy in
                ControllerNavigationMovingOrbOverlay(
                    target: resolveActiveTarget(target, in: proxy),
                    controllerInput: controllerInput
                )
            }
            .zIndex(100_000)
        }
    }

    private func resolveActiveTarget(
        _ target: ControllerNavigationOrbAnchorTarget?,
        in proxy: GeometryProxy
    ) -> ControllerNavigationResolvedOrbTarget? {
        if controllerInput?.navigationZone == .tabBar,
           let windowFrame = controllerInput?.tabBarOrbFocusFrameInWindow,
           let focusedIndex = controllerInput?.tabBarOrbFocusedIndex {
            let hostFrame = proxy.frame(in: .global)
            return ControllerNavigationResolvedOrbTarget(
                id: "tab-bar.\(focusedIndex)",
                frame: windowFrame.offsetBy(
                    dx: -hostFrame.minX,
                    dy: -hostFrame.minY
                ),
                palette: focusedIndex
                    == controllerInput?.tabBarOrbSelectedIndex
                    ? .green
                    : .blue,
                style: .liquidGlass,
                inset: 2,
                orbScale: 0.78
            )
        }
        return resolve(target, in: proxy)
    }

    private func resolve(
        _ target: ControllerNavigationOrbAnchorTarget?,
        in proxy: GeometryProxy
    ) -> ControllerNavigationResolvedOrbTarget? {
        guard let target else { return nil }
        var frame = proxy[target.bounds]
        if let index = target.segmentIndex,
           let count = target.segmentCount,
           count > 0,
           (0..<count).contains(index) {
            let segmentWidth = frame.width / CGFloat(count)
            frame.origin.x += CGFloat(index) * segmentWidth
            frame.size.width = segmentWidth
        }
        return ControllerNavigationResolvedOrbTarget(
            id: target.id,
            frame: frame,
            palette: target.palette,
            style: target.style,
            inset: target.inset,
            orbScale: target.orbScale
        )
    }
}

extension View {
    func controllerNavigationOrbTarget(
        id: String,
        isActive: Bool,
        isEnabled: Bool = true,
        palette: ControllerNavigationOrbPalette = .blue,
        style: ControllerNavigationOrbStyle = .liquidGlass,
        inset: CGFloat = 2,
        orbScale: CGFloat = 1,
        segmentIndex: Int? = nil,
        segmentCount: Int? = nil,
        priority: Int = 0
    ) -> some View {
        modifier(
            ControllerNavigationOrbTargetModifier(
                id: id,
                isActive: isActive,
                isEnabled: isEnabled,
                palette: palette,
                style: style,
                inset: inset,
                orbScale: orbScale,
                segmentIndex: segmentIndex,
                segmentCount: segmentCount,
                priority: priority
            )
        )
    }

    func controllerNavigationOrbOverlay(
        controllerInput: MenuControllerInputRouter? = nil
    ) -> some View {
        modifier(
            ControllerNavigationOrbOverlayHostModifier(
                controllerInput: controllerInput
            )
        )
    }
}
