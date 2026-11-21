// ControllerAccessibilityExplicitTargetModifier.swift — explicit action, toggle, adjustable and picker targets
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

@MainActor
final class ControllerAccessibilityActionProbeView: UIControl {
    private weak var navigationSession: ControllerAccessibilityNavigationSession?
    private var registrationID: UUID?
    private var baseKey = ""
    private(set) var resolvedKey = ""
    private var navigationID: String?
    private var targetLabel = ""
    private var targetValue: String?
    private var targetFocusState: ControllerAccessibilityTargetFocusState?
    private var targetEnabled = true
    private var targetPersistent = false
    private var targetFeedback: MenuControllerFeedback = .activate
    private var targetFocusedNeonCornerRadius: CGFloat?
    private var onActivate: (() -> Void)?
    private var onIncrement: (() -> Void)?
    private var onDecrement: (() -> Void)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isAccessibilityElement = false
        isUserInteractionEnabled = true
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool { false }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { unregisterWhileDetached() }
        else { synchronizeRegistration() }
    }

    override func didMoveToSuperview() {
        super.didMoveToSuperview()
        if superview == nil { unregisterWhileDetached() }
        else { synchronizeRegistration() }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let registrationID else { return }
        navigationSession?.targetGeometryDidChange(
            registrationID: registrationID,
            owner: self
        )
    }

    @discardableResult
    func configure(
        registrationID: UUID,
        session: ControllerAccessibilityNavigationSession,
        baseKey: String,
        navigationID: String?,
        label: String,
        value: String?,
        focusState: ControllerAccessibilityTargetFocusState,
        isEnabled: Bool,
        persistent: Bool,
        activationFeedback: MenuControllerFeedback,
        focusedNeonCornerRadius: CGFloat?,
        onActivate: (() -> Void)?,
        onIncrement: (() -> Void)?,
        onDecrement: (() -> Void)?
    ) -> Bool {
        let needsGeometryRefresh = self.registrationID != registrationID
            || navigationSession !== session
            || self.baseKey != baseKey
            || targetLabel != label
            || targetValue != value
            || targetEnabled != isEnabled
            || targetFocusedNeonCornerRadius != focusedNeonCornerRadius
        if self.registrationID != registrationID
            || navigationSession !== session
            || self.baseKey != baseKey {
            disconnect()
        }
        self.registrationID = registrationID
        navigationSession = session
        self.baseKey = baseKey
        self.navigationID = navigationID
        targetLabel = label
        targetValue = value
        targetFocusState = focusState
        targetEnabled = isEnabled
        targetPersistent = persistent
        targetFeedback = activationFeedback
        targetFocusedNeonCornerRadius = focusedNeonCornerRadius
        self.onActivate = onActivate
        self.onIncrement = onIncrement
        self.onDecrement = onDecrement
        synchronizeRegistration()
        return needsGeometryRefresh
    }

    func disconnect() {
        unregisterWhileDetached()
        navigationSession = nil
        registrationID = nil
        resolvedKey = ""
    }

    private func unregisterWhileDetached() {
        guard let registrationID else { return }
        navigationSession?.unregisterTarget(
            registrationID: registrationID,
            owner: self
        )
        resolvedKey = ""
    }

    private func synchronizeRegistration() {
        guard window != nil, superview != nil,
              let registrationID, let navigationSession,
              let targetFocusState else { return }
        resolvedKey = navigationSession.registerTarget(
            registrationID: registrationID,
            view: self,
            baseKey: baseKey,
            navigationID: navigationID,
            label: targetLabel,
            value: targetValue,
            focusState: targetFocusState,
            isEnabled: targetEnabled,
            persistent: targetPersistent,
            activationFeedback: targetFeedback,
            focusedNeonCornerRadius: targetFocusedNeonCornerRadius,
            onActivate: onActivate,
            onIncrement: onIncrement,
            onDecrement: onDecrement
        )
    }
}

private struct ControllerAccessibilityActionProbe: UIViewRepresentable {
    let registrationID: UUID
    let session: ControllerAccessibilityNavigationSession
    let scopeRevision: UInt64
    let baseKey: String
    let navigationID: String?
    let label: String
    let value: String?
    let focusState: ControllerAccessibilityTargetFocusState
    let isEnabled: Bool
    let persistent: Bool
    let activationFeedback: MenuControllerFeedback
    let focusedNeonCornerRadius: CGFloat?
    let onActivate: (() -> Void)?
    let onIncrement: (() -> Void)?
    let onDecrement: (() -> Void)?

    func makeUIView(context: Context) -> ControllerAccessibilityActionProbeView {
        ControllerAccessibilityActionProbeView()
    }

    func updateUIView(
        _ uiView: ControllerAccessibilityActionProbeView,
        context: Context
    ) {
        let needsGeometryRefresh = uiView.configure(
            registrationID: registrationID,
            session: session,
            baseKey: baseKey,
            navigationID: navigationID,
            label: label,
            value: value,
            focusState: focusState,
            isEnabled: isEnabled,
            persistent: persistent,
            activationFeedback: activationFeedback,
            focusedNeonCornerRadius: focusedNeonCornerRadius,
            onActivate: onActivate,
            onIncrement: onIncrement,
            onDecrement: onDecrement
        )
        if needsGeometryRefresh {
            // A changed Picker value or an Override-to-editor swap can resize its
            // Form row without moving this representable in the hierarchy. Avoid
            // forcing another layout pass for unrelated observation updates.
            uiView.setNeedsLayout()
        }
    }

    static func dismantleUIView(
        _ uiView: ControllerAccessibilityActionProbeView,
        coordinator: Void
    ) { uiView.disconnect() }
}

private struct ControllerAccessibilityExplicitTargetModifier: ViewModifier {
    let navigationID: String?
    let label: String
    let value: String?
    let focusedColor: Color?
    let focusedNeonCornerRadius: CGFloat?
    let activationFeedback: MenuControllerFeedback
    let onActivate: (() -> Void)?
    let onIncrement: (() -> Void)?
    let onDecrement: (() -> Void)?

    @Environment(\.controllerAccessibilityNavigationActive) private var isActive
    @Environment(\.controllerAccessibilityNavigationSession) private var session
    @Environment(\.controllerAccessibilityTargetsSuppressed) private var isSuppressed
    @Environment(\.controllerAccessibilityInheritedTargetID) private var inheritedID
    @Environment(\.isEnabled) private var isEnabled
    @State private var registrationID = UUID()
    @State private var focusState = ControllerAccessibilityTargetFocusState()

    private var trimmedID: String? {
        guard let source = navigationID ?? inheritedID else { return nil }
        let value = source.trimmingCharacters(in: .whitespacesAndNewlines)
        return value.isEmpty ? nil : value
    }

    private var trimmedLabel: String {
        label.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private var baseKey: String {
        if let trimmedID {
            return ControllerAccessibilityNavigationSession.explicitKey(trimmedID)
        }
        if !trimmedLabel.isEmpty {
            // A declared order is expressed in stable target IDs. Treat an
            // explicit target's nonempty label as that ID when the caller did
            // not provide one, so lazy Form rows can be sought before mount.
            return ControllerAccessibilityNavigationSession.explicitKey(trimmedLabel)
        }
        return "controller.focus.auto.\(registrationID.uuidString)"
    }

    func body(content: Content) -> some View {
        ControllerAccessibilityFocusedTextTint(
            content: content.focusable(false),
            focusedColor: focusedColor
        )
        // An explicit row owns the logical target. Prevent a nested SwiftUI
        // Button/Toggle (Picker included) from synthesizing a second UUID
        // probe over the same geometry through the navigation-wide styles.
        .environment(\.controllerAccessibilityAutomaticTargetSuppressed, true)
        .environment(\.controllerAccessibilityTargetFocused, focusState.isFocused)
        .controllerFocusDepthAnchor(
            id: baseKey,
            isFocused: focusState.isFocused
        )
        .modifier(
            ControllerAccessibilityScrollIdentityModifier(
                identity: navigationID == nil && inheritedID != nil
                    ? nil
                    : baseKey
            )
        )
        .background {
            if isActive, !isSuppressed, let session {
                ControllerAccessibilityActionProbe(
                    registrationID: registrationID,
                    session: session,
                    // Retained NavigationStack rows must update their probes
                    // after configure clears the preceding screen's registry.
                    scopeRevision: session.scopeRevision,
                    baseKey: baseKey,
                    navigationID: trimmedID,
                    label: trimmedLabel,
                    value: value,
                    focusState: focusState,
                    isEnabled: isEnabled,
                    persistent: trimmedID != nil || !trimmedLabel.isEmpty,
                    activationFeedback: activationFeedback,
                    focusedNeonCornerRadius: focusedNeonCornerRadius,
                    onActivate: onActivate,
                    onIncrement: onIncrement,
                    onDecrement: onDecrement
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }
}

private struct ControllerAccessibilityScrollIdentityModifier: ViewModifier {
    let identity: String?

    @ViewBuilder
    func body(content: Content) -> some View {
        if let identity { content.id(identity) }
        else { content }
    }
}

private struct ControllerAccessibilityFocusedTextTint<Content: View>: View {
    let content: Content
    let focusedColor: Color?
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.menuControllerInputRouter) private var controllerInput

    var body: some View {
        let controllerOwnsAppearance =
            controllerInput?.isControllerNavigationEnabled != false
        let visuallyFocused = isFocused && controllerOwnsAppearance
        let shadowStrength = visuallyFocused
            ? textAppearance.focusedShadowStrength
            : textAppearance.normalShadowStrength
        content
        // Input changes focus ownership, not the screen's theme palette.
        .foregroundColor(
            visuallyFocused
                ? (focusedColor ?? textAppearance.focusedColor)
                : textAppearance.normalColor
        )
        .shadow(
            color: (visuallyFocused
                ? textAppearance.focusedShadowColor
                : textAppearance.normalShadowColor
            ).opacity(shadowStrength),
            radius: CGFloat(5 * shadowStrength),
            y: CGFloat(2 * shadowStrength)
        )
        .animation(
            ControllerFocusVisualAnimation.textFade,
            value: visuallyFocused
        )
    }
}

extension View {
    func controllerAccessibilityActionTarget(
        id: String? = nil,
        label: String,
        focusedColor: Color? = nil,
        focusedNeonCornerRadius: CGFloat? = nil,
        activationFeedback: MenuControllerFeedback = .activate,
        action: @escaping () -> Void
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: nil,
                focusedColor: focusedColor,
                focusedNeonCornerRadius: focusedNeonCornerRadius,
                activationFeedback: activationFeedback,
                onActivate: action,
                onIncrement: nil,
                onDecrement: nil
            )
        )
    }

    func controllerAccessibilityMenuTarget(id: String, label: String) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: nil,
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .activate,
                onActivate: nil,
                onIncrement: nil,
                onDecrement: nil
            )
        )
    }

    func controllerAccessibilityToggleTarget(
        id: String? = nil,
        label: String,
        isOn: Binding<Bool>
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: isOn.wrappedValue ? "On" : "Off",
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .toggle(isOn: !isOn.wrappedValue),
                onActivate: { isOn.wrappedValue.toggle() },
                onIncrement: { isOn.wrappedValue = true },
                onDecrement: { isOn.wrappedValue = false }
            )
        )
    }

    func controllerAccessibilityAdjustableTarget(
        id: String? = nil,
        label: String,
        value: String? = nil,
        onActivate: @escaping () -> Void,
        onIncrement: @escaping () -> Void,
        onDecrement: @escaping () -> Void
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: value,
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .activate,
                onActivate: onActivate,
                onIncrement: onIncrement,
                onDecrement: onDecrement
            )
        )
    }

    func controllerAccessibilityPickerTarget(
        id: String? = nil,
        label: String,
        value: String? = nil,
        onActivate: (() -> Void)? = nil,
        onIncrement: @escaping () -> Void,
        onDecrement: @escaping () -> Void
    ) -> some View {
        modifier(
            ControllerAccessibilityExplicitTargetModifier(
                navigationID: id,
                label: label,
                value: value,
                focusedColor: nil,
                focusedNeonCornerRadius: nil,
                activationFeedback: .activate,
                onActivate: onActivate,
                onIncrement: onIncrement,
                onDecrement: onDecrement
            )
        )
    }
}
