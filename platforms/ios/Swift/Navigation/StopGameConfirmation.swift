// StopGameConfirmation.swift — stop game confirmation
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Foundation

/// Where a Stop confirmation anchors, in global coordinates. A beside anchor is a
/// whole card, which the panel sits next to rather than across.
struct StopConfirmationAnchor: Equatable {
    var frame: CGRect
    var beside = false
}

struct StopConfirmationAnchorKey: PreferenceKey {
    static let defaultValue: StopConfirmationAnchor? = nil
    static func reduce(value: inout StopConfirmationAnchor?, nextValue: () -> StopConfirmationAnchor?) {
        if let next = nextValue(), value == nil || next.beside {
            value = next
        }
    }
}

private struct StopConfirmationPresentedKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var stopConfirmationPresented: Bool {
        get { self[StopConfirmationPresentedKey.self] }
        set { self[StopConfirmationPresentedKey.self] = newValue }
    }
}

extension View {
    /// Marks the button a Stop confirmation opens from: it reports where the
    /// confirmation anchors and wears a red ring while it is open.
    func stopConfirmationSource() -> some View {
        modifier(StopConfirmationSourceModifier())
    }

    /// Anchors the confirmation beside this whole card instead of under its button.
    func stopConfirmationCard() -> some View {
        background {
            GeometryReader { proxy in
                Color.clear.preference(
                    key: StopConfirmationAnchorKey.self,
                    value: StopConfirmationAnchor(frame: proxy.frame(in: .global), beside: true)
                )
            }
        }
    }
}

private struct StopConfirmationSourceModifier: ViewModifier {
    @Environment(\.stopConfirmationPresented) private var isPresented

    func body(content: Content) -> some View {
        content
            .overlay {
                if isPresented {
                    Capsule()
                        .stroke(
                            Color(red: 1, green: 0.388, blue: 0.412).opacity(0.25),
                            lineWidth: 4
                        )
                        .padding(-2)
                        .allowsHitTesting(false)
                }
            }
            .background {
                GeometryReader { proxy in
                    Color.clear.preference(
                        key: StopConfirmationAnchorKey.self,
                        value: StopConfirmationAnchor(frame: proxy.frame(in: .global))
                    )
                }
            }
    }
}

/// Stop confirmation anchored under the button that opened it: clear Liquid
/// Glass on iOS 26, the frosted graphite panel before that. Its buttons keep
/// the alert's action ids and selection index, so controller input is as before.
struct StopGameConfirmation: View {
    let gameTitle: String
    let anchor: StopConfirmationAnchor?
    let selectedIndex: Int
    var stopFeedback: MenuControllerFeedback = .activate
    let onSelect: (Int) -> Void
    let onDismiss: () -> Void

    @Environment(\.horizontalSizeClass) private var horizontalSizeClass
    @Environment(\.controllerAccessibilityNavigationActive) private var sharedNavigationActive
    @Environment(\.menuControllerInputRouter) private var controllerInput

    private static let stopRed = Color(red: 0.820, green: 0.204, blue: 0.220)

    private var usesGlass: Bool {
        if #available(iOS 26, *) { return true }
        return false
    }

    private var showsControllerSelection: Bool {
        controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
    }

    private func localized(_ key: String) -> String {
        SettingsStore.shared.localized(key)
    }

    var body: some View {
        GeometryReader { proxy in
            let isRegular = horizontalSizeClass == .regular
            let width = isRegular ? 328 : max(0, proxy.size.width - 32)
            let placement = placement(in: proxy, width: width, isRegular: isRegular)
            ZStack(alignment: placement.alignment) {
                OverlayTheme.scrimBase.opacity(OverlayTheme.scrimPad)
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture { cancel() }
                panel(isRegular: isRegular)
                    .frame(width: width)
                    .fixedSize(horizontal: false, vertical: true)
                    .offset(placement.offset)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .dynamicTypeSize(...DynamicTypeSize.accessibility3)
        .zIndex(20_000)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
        .accessibilityAction(.escape) { cancel() }
        .onAppear { controllerInput?.playTouchHaptics(.contextMenu) }
    }

    /// Under the button, above it when it sits in the lower half like the pause menu's,
    /// or beside a card on whichever side has room.
    private func placement(
        in proxy: GeometryProxy,
        width: CGFloat,
        isRegular: Bool
    ) -> (alignment: Alignment, offset: CGSize) {
        guard let anchor else { return (.center, .zero) }
        let origin = proxy.frame(in: .global).origin
        let frame = anchor.frame.offsetBy(dx: -origin.x, dy: -origin.y)
        let size = proxy.size
        let clampX = { (x: CGFloat) in min(max(x, 16), max(16, size.width - width - 16)) }
        if anchor.beside {
            let leading = frame.minX - 12 - width
            let x = leading >= 16 ? leading : frame.maxX + 12
            return (.leading, CGSize(width: clampX(x), height: frame.midY - size.height / 2))
        }
        let x = clampX(isRegular ? frame.maxX - width : 16)
        if frame.midY > size.height / 2 {
            return (.bottomLeading, CGSize(width: x, height: frame.minY - 10 - size.height))
        }
        return (.topLeading, CGSize(width: x, height: frame.maxY + 24))
    }

    @ViewBuilder
    private func panel(isRegular: Bool) -> some View {
        let content = VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 4) {
                Text(String(format: localized("Stop %@?"), gameTitle))
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(usesGlass ? Color.white : OverlayTheme.textPrimary)
                Text(localized("Unsaved progress will be lost."))
                    .font(.system(size: 15))
                    .foregroundStyle(
                        usesGlass ? Color.white.opacity(0.8) : OverlayTheme.textSecondary
                    )
            }
            .shadow(color: .black.opacity(usesGlass ? 0.35 : 0), radius: 1, y: 1)
            .accessibilityElement(children: .combine)
            HStack(spacing: 10) {
                actionButton(0, title: localized("Cancel"), isRegular: isRegular)
                actionButton(1, title: localized("Stop Game"), isRegular: isRegular)
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        if usesGlass {
            content.glassSurface(clear: true, cornerRadius: 28)
        } else {
            content
                .background(OverlayFrostBackground())
                .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
        }
    }

    private func actionButton(_ index: Int, title: String, isRegular: Bool) -> some View {
        let isStop = index == 1
        let height: CGFloat = usesGlass && isRegular ? 46 : 50
        let radius = usesGlass ? height / 2 : 12
        let isSelected = !sharedNavigationActive
            && showsControllerSelection
            && selectedIndex == index
        let feedback: MenuControllerFeedback = isStop ? stopFeedback : .activate
        let label = Text(title)
            .font(.system(size: usesGlass && isRegular ? 16 : 17, weight: .semibold))
            .foregroundStyle(usesGlass || isStop ? Color.white : OverlayTheme.textPrimary)
            .frame(maxWidth: .infinity, minHeight: height)
            .contentShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
        return Button {
            controllerInput?.playFeedback(feedback)
            onSelect(index)
        } label: {
            if usesGlass {
                label.glassSurface(
                    tint: isStop ? Self.stopRed : nil,
                    interactive: true,
                    clear: true,
                    cornerRadius: radius
                )
            } else {
                label.background(
                    isStop ? Self.stopRed : Color.white.opacity(0.12),
                    in: RoundedRectangle(cornerRadius: radius, style: .continuous)
                )
            }
        }
        .buttonStyle(.plain)
        .overlay {
            if isSelected {
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .stroke(Color.white.opacity(0.9), lineWidth: 2)
            }
        }
        .controllerAccessibilityActionTarget(
            id: isStop ? "alert.action.stop" : "alert.action.cancel",
            label: title,
            focusedColor: .white,
            focusedNeonCornerRadius: radius,
            activationFeedback: feedback
        ) {
            onSelect(index)
        }
        .focusEffectDisabled()
        .accessibilityAddTraits(isSelected ? .isSelected : [])
    }

    private func cancel() {
        controllerInput?.playFeedback(.back)
        onDismiss()
    }
}
