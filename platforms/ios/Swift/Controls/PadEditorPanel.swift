// PadEditorPanel.swift — the pad editor's panel for the selected piece, and where it docks
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@MainActor func padPieceName(_ id: String) -> String {
    let names = [
        "up": "D-Pad Up", "down": "D-Pad Down", "left": "D-Pad Left", "right": "D-Pad Right",
        "triangle": "Triangle", "circle": "Circle", "square": "Square", "cross": "Cross",
        "l1": "L1", "l2": "L2", "r1": "R1", "r2": "R2",
        "lstick": "Left Stick", "rstick": "Right Stick", "select": "Select", "start": "Start",
    ]
    return SettingsStore.shared.localized(names[id] ?? id)
}

struct PadPiecePanel: View {
    let editor: PadEditor
    let id: String
    var controllerInput: MenuControllerInputRouter?
    /// Cross on a selected piece focuses the panel; Circle goes back to moving the piece.
    var focused = false
    /// The room it docks in; taller content, as at the larger text sizes, scrolls.
    var maxHeight: CGFloat = .infinity
    var onLeave: () -> Void = {}
    private let settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accent
    @Environment(\.uiCriticalTextColour) private var critical

    var body: some View {
        PadEditorHeightCap(cap: maxHeight) {
            ViewThatFits(in: .vertical) {
                stack
                ScrollView { stack }
            }
        }
        .buttonStyle(.plain)
        .modifier(PadEditorSurface())
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput, isActive: focused, scopeKey: "pad.editor.panel", priority: 810,
            onBack: {
                onLeave()
                return true
            }
        )
    }

    @ViewBuilder private var stack: some View {
        let p = editor.resolved(id)
        let hidden = editor.isHidden(id)
        let others = hidden ? [] : editor.overlapping(id)
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(padPieceName(id))
                    .font(.headline)
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .lineLimit(1)
                Spacer(minLength: 8)
                Button { editor.selection = nil } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title3)
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel(Text(settings.localized("Close")))
                .controllerAccessibilityActionTarget(label: settings.localized("Close")) { editor.selection = nil }
            }
            .padding(.leading, 4)
            // Two stepper columns, or at the larger text sizes one, with the tiles under it in a row or a column.
            // The tiles stay out of the one-column grid: a ViewThatFits in a grid cell is measured short.
            ViewThatFits(in: .horizontal) {
                grid(p, hidden: hidden, wide: true)
                VStack(alignment: .leading, spacing: 4) {
                    grid(p, hidden: hidden, wide: false)
                    ViewThatFits(in: .horizontal) {
                        tiles(hidden: hidden)
                        tiles(hidden: hidden, AnyLayout(VStackLayout(alignment: .leading, spacing: 4)))
                    }
                }
            }
            if let first = others.first {
                note(others.count == 1
                    ? String(format: settings.localized("Its touch area overlaps %@."), padPieceName(first))
                    : String(format: settings.localized("Its touch area overlaps %d other buttons."), others.count),
                     warning: true)
            }
            if hidden { note(settings.localized("Hidden while you play. Tap Show to bring it back.")) }
            if isStick(id) && editor.effectiveFloating(id) {
                note(settings.localized("The stick appears where your thumb lands in its zone."))
            }
            if id == "rstick" && SettingsStore.shared.dynamicThumbsticks.swipeCamera {
                note(settings.localized("Swipe Camera uses this zone too."))
            }
        }
        .padding(12)
    }

    private func grid(_ p: PadGroupPosition, hidden: Bool, wide: Bool) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
            // A stick touches over the circle it draws from one scale, so it has one Size and no Touch Area.
            if isStick(id) {
                pair(wide, sizeStepper("Size", spoken: "Size", p.scaleX, .visible, .x), opacityStepper(p.opacity))
                if editor.usesZone(id) {
                    let zone = editor.zone(of: id)
                    caption("Zone")
                    pair(wide, zoneStepper("Width", spoken: "Zone width", zone.width, .x),
                         zoneStepper("Height", spoken: "Zone height", zone.height, .y))
                }
                if wide { GridRow { tiles(hidden: hidden).gridCellColumns(2) } }
            } else {
                caption("Size")
                pair(wide, sizeStepper("Width", spoken: "Size width", p.scaleX, .visible, .x),
                     sizeStepper("Height", spoken: "Size height", p.scaleY, .visible, .y))
                caption("Touch Area")
                pair(wide, sizeStepper("Width", spoken: "Touch area width", p.hitScaleX, .hit, .x),
                     sizeStepper("Height", spoken: "Touch area height", p.hitScaleY, .hit, .y))
                if wide {
                    GridRow {
                        opacityStepper(p.opacity)
                        tiles(hidden: hidden)
                    }
                } else {
                    GridRow { opacityStepper(p.opacity) }
                }
            }
        }
    }

    @ViewBuilder private func pair(_ wide: Bool, _ a: some View, _ b: some View) -> some View {
        if wide {
            GridRow { a; b }
        } else {
            GridRow { a }
            GridRow { b }
        }
    }

    private func caption(_ text: String) -> some View {
        Text(settings.localized(text))
            .font(.caption)
            .foregroundStyle(OverlayTheme.textSecondary)
            .padding(.leading, 4)
            .accessibilityHidden(true)
    }

    private func sizeStepper(_ label: String, spoken: String, _ value: CGFloat, _ kind: PadSizeKind, _ axis: PadSizeAxis) -> some View {
        stepper(label, spoken: spoken, value, in: PadLayoutMetrics.minControlScale...PadLayoutMetrics.maxControlScale) { delta in
            editor.setSize(id, kind: kind, axis: axis, value: ((value + delta) * 10).rounded() / 10)
        }
    }

    private func zoneStepper(_ label: String, spoken: String, _ value: CGFloat, _ axis: PadSizeAxis) -> some View {
        stepper(label, spoken: spoken, value, in: PadLayoutMetrics.minimumZoneShare...1) { delta in
            editor.setZoneSize(id, axis: axis, value: ((value + delta) * 10).rounded() / 10)
        }
    }

    private func opacityStepper(_ value: CGFloat) -> some View {
        stepper("Opacity", spoken: "Opacity", value, in: PadLayoutMetrics.opacityRange) { editor.stepOpacity(id, by: $0) }
    }

    /// A caption over the value and −/+ glyphs that repeat while held, in 10% steps.
    private func stepper(
        _ label: String,
        spoken: String,
        _ value: CGFloat,
        in range: ClosedRange<CGFloat>,
        step: @escaping (CGFloat) -> Void
    ) -> some View {
        let percent = "\(Int((value * 100).rounded()))%"
        return VStack(alignment: .leading, spacing: 0) {
            Text(settings.localized(label))
                .font(.caption)
                .foregroundStyle(OverlayTheme.textSecondary)
                .lineLimit(1)
                .padding(.leading, 4)
            HStack(spacing: 4) {
                Text(verbatim: percent)
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .monospacedDigit()
                    .frame(minWidth: 52, alignment: .leading)
                    .padding(.leading, 4)
                glyph("minus.circle.fill", enabled: value > range.lowerBound + 0.001) { step(-0.1) }
                glyph("plus.circle.fill", enabled: value < range.upperBound - 0.001) { step(0.1) }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(settings.localized(spoken)))
        .accessibilityValue(Text(verbatim: percent))
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment: step(0.1)
            case .decrement: step(-0.1)
            @unknown default: break
            }
        }
        .controllerAccessibilityAdjustableTarget(
            label: settings.localized(spoken), value: percent, onActivate: {},
            onIncrement: { step(0.1) }, onDecrement: { step(-0.1) }
        )
    }

    private func glyph(_ symbol: String, enabled: Bool, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.title2)
                .foregroundStyle(accent.opacity(enabled ? 1 : 0.35))
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
        }
        .buttonRepeatBehavior(.enabled)
        .disabled(!enabled)
    }

    private func tiles(hidden: Bool, _ layout: AnyLayout = AnyLayout(HStackLayout(spacing: 4))) -> some View {
        layout {
            if isStick(id) {
                let floating = editor.effectiveFloating(id)
                tile("Floating", spoken: "Floating stick", id == "lstick" ? "l.joystick" : "r.joystick", on: floating) {
                    editor.setFloating(id, !floating)
                }
                .accessibilityValue(Text(settings.localized(floating ? "On" : "Off")))
            }
            if hidden {
                tile("Show", spoken: "Show", "eye.slash", on: true) { editor.toggleHidden(id) }
                    .accessibilityValue(Text(settings.localized("Off")))
            } else {
                tile("Hide", spoken: "Hide", "eye") { editor.toggleHidden(id) }
            }
            tile("Reset Piece", spoken: "Reset this piece", "arrow.counterclockwise") { editor.resetPiece(id) }
                .disabled(editor.isDefault(id))
        }
    }

    /// An icon over a short word; `on` gives the accent look of a lit toggle.
    private func tile(_ label: String, spoken: String, _ symbol: String, on: Bool = false, run: @escaping () -> Void) -> some View {
        Button(action: run) {
            VStack(spacing: 2) {
                Image(systemName: symbol).font(.body).frame(height: 22)
                Text(settings.localized(label)).font(.caption).lineLimit(1)
            }
            .foregroundStyle(on ? accent : OverlayTheme.textPrimary)
            .frame(minWidth: 52, minHeight: 48)
            .background(on ? accent.opacity(0.16) : .clear, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            .contentShape(Rectangle())
        }
        .accessibilityLabel(Text(settings.localized(spoken)))
        .controllerAccessibilityActionTarget(label: settings.localized(spoken), action: run)
    }

    private func note(_ text: String, warning: Bool = false) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            if warning { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(critical) }
            Text(text)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .font(.subheadline)
        .foregroundStyle(OverlayTheme.textSecondary)
        .padding(.horizontal, 4)
    }
}

extension PadPiecePanel {
    /// The spot covering least of `others`, never within 8 pt of `selected` or across `fold`; nearer spots win a tie.
    /// Scans a grid of 8 between the side margins from `top`, and gives `fallback` when no spot fits.
    static func dock(
        _ panel: CGSize,
        top: CGFloat,
        bounds: CGSize,
        selected: CGRect,
        others: [CGRect],
        fold: CGRect?,
        fallback: CGPoint
    ) -> CGPoint {
        var best: (cost: CGFloat, centre: CGPoint)?
        for y in stride(from: top, through: bounds.height - panel.height - 8, by: 8) {
            for x in stride(from: 16, through: bounds.width - panel.width - 16, by: 8) {
                let card = CGRect(x: x, y: y, width: panel.width, height: panel.height)
                guard !card.insetBy(dx: -8, dy: -8).intersects(selected), !(fold.map(card.intersects) ?? false) else { continue }
                let cover = others.reduce(CGFloat(0)) { total, frame in
                    let shared = card.intersection(frame)
                    return shared.isNull ? total : total + shared.width * shared.height
                }
                let cost = cover + hypot(card.midX - selected.midX, card.midY - selected.midY) / 100
                if cost < best?.cost ?? .infinity { best = (cost, CGPoint(x: card.midX, y: card.midY)) }
            }
        }
        return best?.centre ?? fallback
    }
}
