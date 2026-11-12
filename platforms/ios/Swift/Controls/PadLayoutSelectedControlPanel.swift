// PadLayoutSelectedControlPanel.swift — the layout editor's panel for the selected control
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadLayoutSelectedControlPanel: View {
    let id: String
    let areaW: CGFloat
    let areaH: CGFloat
    let isLandscape: Bool
    @Binding var unlinkedControls: Set<String>
    let pushSnapshot: () -> Void

    @State private var layout = PadLayoutStore.shared

    var body: some View {
        selectedControlPanel(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
    }

    /// A control is shown unlinked when explicitly toggled or when its stored
    /// axes already differ (inferred). Sticks are always linked (no per-axis).
    private func isUnlinked(id: String, position: PadGroupPosition) -> Bool {
        guard !isStick(id) else { return false }
        if unlinkedControls.contains(id) { return true }
        return position.scaleX != position.scaleY || position.hitScaleX != position.hitScaleY
    }

    @ViewBuilder
    private func selectedControlPanel(id: String, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) -> some View {
        let position = selectedPosition(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
        let unlinked = isUnlinked(id: id, position: position)
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                Text(editorLabel(for: id))
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.white)
                    .frame(minWidth: 54, alignment: .leading)

                if !isStick(id) {
                    aspectLockButton(id: id, unlinked: unlinked, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
                }

                if !unlinked {
                    // Linked: one Visible + one Hit stepper drive both axes.
                    scaleStepper(
                        title: "Visible",
                        value: position.scale,
                        canDecrease: position.scale > PadLayoutMetrics.minControlScale,
                        canIncrease: position.scale < PadLayoutMetrics.maxControlScale,
                        onDecrease: { adjustSelectedVisibleScale(id: id, by: -0.1, areaW: areaW, areaH: areaH, isLandscape: isLandscape) },
                        onIncrease: { adjustSelectedVisibleScale(id: id, by: 0.1, areaW: areaW, areaH: areaH, isLandscape: isLandscape) }
                    )

                    if !isStick(id) {
                        scaleStepper(
                            title: "Hit",
                            value: position.hitScale,
                            canDecrease: position.hitScale > PadLayoutMetrics.minControlScale,
                            canIncrease: position.hitScale < PadLayoutMetrics.maxControlScale,
                            onDecrease: { adjustSelectedHitScale(id: id, by: -0.1, areaW: areaW, areaH: areaH, isLandscape: isLandscape) },
                            onIncrease: { adjustSelectedHitScale(id: id, by: 0.1, areaW: areaW, areaH: areaH, isLandscape: isLandscape) }
                        )
                    }
                }
            }

            if unlinked {
                // Unlinked: width/height steppers for visible and hit, two rows.
                HStack(spacing: 10) {
                    axisStepper(title: "Vis W", value: position.scaleX, id: id, kind: .visible, axis: .x, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
                    axisStepper(title: "Vis H", value: position.scaleY, id: id, kind: .visible, axis: .y, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
                }
                HStack(spacing: 10) {
                    axisStepper(title: "Hit W", value: position.hitScaleX, id: id, kind: .hit, axis: .x, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
                    axisStepper(title: "Hit H", value: position.hitScaleY, id: id, kind: .hit, axis: .y, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 7)
        .background(.black.opacity(0.58), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private func aspectLockButton(id: String, unlinked: Bool, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) -> some View {
        Button {
            if unlinked {
                // Re-lock: collapse to width (width-wins) and record link state.
                pushSnapshot()
                let perButton = PadLayoutStore.perButtonIDs.contains(id)
                layout.relinkAxes(id, perButton: perButton, landscape: isLandscape, areaW: areaW, areaH: areaH)
                unlinkedControls.remove(id)
            } else {
                // Unlock: reveal per-axis steppers. No data change yet.
                unlinkedControls.insert(id)
            }
        } label: {
            Image(systemName: unlinked ? "lock.open" : "lock")
                .font(.caption)
                .foregroundStyle(unlinked ? .yellow : .white)
                .frame(width: 24, height: 24)
        }
        .accessibilityLabel(unlinked ? "Unlinked width and height" : "Linked width and height")
    }

    private func axisStepper(title: String, value: CGFloat, id: String, kind: PadSizeKind, axis: PadSizeAxis, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) -> some View {
        scaleStepper(
            title: title,
            value: value,
            canDecrease: value > PadLayoutMetrics.minControlScale,
            canIncrease: value < PadLayoutMetrics.maxControlScale,
            onDecrease: { adjustSelectedAxis(id: id, kind: kind, axis: axis, by: -0.1, areaW: areaW, areaH: areaH, isLandscape: isLandscape) },
            onIncrease: { adjustSelectedAxis(id: id, kind: kind, axis: axis, by: 0.1, areaW: areaW, areaH: areaH, isLandscape: isLandscape) }
        )
    }

    private func scaleStepper(
        title: String,
        value: CGFloat,
        canDecrease: Bool,
        canIncrease: Bool,
        onDecrease: @escaping () -> Void,
        onIncrease: @escaping () -> Void
    ) -> some View {
        HStack(spacing: 5) {
            Text(title)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.white.opacity(0.75))
            Button(action: onDecrease) {
                Image(systemName: "minus.circle.fill")
                    .font(.body)
                    .foregroundStyle(canDecrease ? .white : .white.opacity(0.3))
                    .frame(width: 24, height: 24)
            }
            .disabled(!canDecrease)
            Text(String(format: "%.1f", Double(value)))
                .font(.caption.monospacedDigit().weight(.semibold))
                .foregroundStyle(.white)
                .frame(width: 30)
            Button(action: onIncrease) {
                Image(systemName: "plus.circle.fill")
                    .font(.body)
                    .foregroundStyle(canIncrease ? .white : .white.opacity(0.3))
                    .frame(width: 24, height: 24)
            }
            .disabled(!canIncrease)
        }
    }

    private func selectedPosition(id: String, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) -> PadGroupPosition {
        if PadLayoutStore.perButtonIDs.contains(id) {
            return layout.perButtonPosition(for: id, landscape: isLandscape, areaW: areaW, areaH: areaH)
        }
        return layout.position(for: id, landscape: isLandscape)
    }

    private func adjustSelectedVisibleScale(id: String, by delta: CGFloat, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) {
        let current = selectedPosition(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape).scale
        let next = PadLayoutMetrics.clampedScale(current + delta)
        guard next != current else { return }
        pushSnapshot()
        if PadLayoutStore.perButtonIDs.contains(id) {
            layout.updatePerButtonScale(id, scale: next, landscape: isLandscape, areaW: areaW, areaH: areaH)
        } else {
            layout.updateGroupScale(id, scale: next, landscape: isLandscape)
        }
    }

    private func adjustSelectedHitScale(id: String, by delta: CGFloat, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) {
        guard !isStick(id) else { return }
        let current = selectedPosition(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape).hitScale
        let next = PadLayoutMetrics.clampedScale(current + delta)
        guard next != current else { return }
        pushSnapshot()
        if PadLayoutStore.perButtonIDs.contains(id) {
            layout.updatePerButtonHitScale(id, hitScale: next, landscape: isLandscape, areaW: areaW, areaH: areaH)
        } else {
            layout.updateGroupHitScale(id, hitScale: next, landscape: isLandscape)
        }
    }

    private func adjustSelectedAxis(id: String, kind: PadSizeKind, axis: PadSizeAxis, by delta: CGFloat, areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) {
        guard !isStick(id) else { return }
        let position = selectedPosition(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape)
        let current = axisValue(of: position, kind: kind, axis: axis)
        let next = PadLayoutMetrics.clampedScale(current + delta)
        guard next != current else { return }
        pushSnapshot()
        if PadLayoutStore.perButtonIDs.contains(id) {
            layout.updatePerButtonSize(id, kind: kind, axis: axis, scale: next, landscape: isLandscape, areaW: areaW, areaH: areaH)
        } else {
            layout.updateGroupSize(id, kind: kind, axis: axis, scale: next, landscape: isLandscape)
        }
    }

    private func axisValue(of position: PadGroupPosition, kind: PadSizeKind, axis: PadSizeAxis) -> CGFloat {
        switch (kind, axis) {
        case (.visible, .x): return position.scaleX
        case (.visible, .y): return position.scaleY
        case (.hit, .x):     return position.hitScaleX
        case (.hit, .y):     return position.hitScaleY
        }
    }
}

private func editorLabel(for id: String) -> String {
    switch id {
    case "up": return "UP"
    case "down": return "DN"
    case "left": return "LT"
    case "right": return "RT"
    case "triangle": return "TRI"
    case "circle": return "O"
    case "square": return "SQR"
    case "cross": return "X"
    case "select": return "SEL"
    default: return id.uppercased()
    }
}
