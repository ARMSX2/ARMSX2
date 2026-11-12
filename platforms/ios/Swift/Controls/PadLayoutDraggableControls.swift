// PadLayoutDraggableControls.swift — pad layout editor draggable controls
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

func padEditorBaseSize(for id: String, isLandscape: Bool, analogStickScale: CGFloat) -> CGSize {
    switch id {
    case "up", "down", "left", "right":
        let width = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape)
        return CGSize(width: width, height: width)
    case "triangle", "circle", "square", "cross":
        let size = VirtualPadButtonOffset.actionButtonSize
        return CGSize(width: size, height: size)
    case "l1", "r1":
        return isLandscape ? CGSize(width: 120, height: 32) : CGSize(width: 100, height: 30)
    case "l2", "r2":
        return isLandscape ? CGSize(width: 130, height: 44) : CGSize(width: 110, height: 40)
    case "lstick", "rstick":
        let size = 68 * min(max(analogStickScale, 0.8), 1.6)
        return CGSize(width: size, height: size)
    case "select":
        return CGSize(width: isLandscape ? 40 : 42, height: 22)
    case "start":
        return CGSize(width: 48, height: 22)
    default:
        return CGSize(width: 60, height: 40)
    }
}

// MARK: - Draggable group widget
struct DraggableGroup: View {
    let id: String
    let areaW: CGFloat
    let areaH: CGFloat
    let isLandscape: Bool
    let analogStickScale: CGFloat
    /// The refitted portrait layout the controls are drawn from on the Duo, and its fit; nil elsewhere.
    let fitted: PadLayoutSnapshot?
    let fit: PadPortraitFit?
    @Binding var selectedControlID: String?
    let isSelected: Bool
    let isOverlapping: Bool
    let onBeginEdit: () -> Void

    @State private var layout = PadLayoutStore.shared
    @State private var dragOffset: CGSize = .zero
    @State private var currentScale: CGFloat = 1.0
    @State private var hasPushedSnapshot = false
    @Environment(\.uiAccentColour) private var accentColour

    private var stored: PadGroupPosition {
        layout.position(for: id, landscape: isLandscape)
    }

    private var pos: PadGroupPosition {
        fitted?.position(for: id, landscape: false) ?? stored
    }

    private var currentX: CGFloat { pos.x * areaW + dragOffset.width }
    private var currentY: CGFloat { pos.y * areaH + dragOffset.height }
    // Pinch multiplies both axes by the same factor (ratio-preserving / uniform).
    private var visibleScaleX: CGFloat {
        PadLayoutMetrics.clampedScale(pos.scaleX * currentScale)
    }
    private var visibleScaleY: CGFloat {
        PadLayoutMetrics.clampedScale(pos.scaleY * currentScale)
    }
    private var hitScaleX: CGFloat {
        isStick(id) ? visibleScaleX : pos.hitScaleX
    }
    private var hitScaleY: CGFloat {
        isStick(id) ? visibleScaleY : pos.hitScaleY
    }
    private var visibleSize: CGSize {
        CGSize(
            width: PadLayoutMetrics.visibleLength(baseLength: baseSize.width, visibleScale: visibleScaleX),
            height: PadLayoutMetrics.visibleLength(baseLength: baseSize.height, visibleScale: visibleScaleY)
        )
    }
    private var hitSize: CGSize {
        if isStick(id) {
            return visibleSize
        }
        return CGSize(
            width: PadLayoutMetrics.touchLength(baseLength: baseSize.width, hitScale: hitScaleX),
            height: PadLayoutMetrics.touchLength(baseLength: baseSize.height, hitScale: hitScaleY)
        )
    }

    var body: some View {
        let isVisible = layout.isControlVisible(id)
        ZStack {
            RoundedRectangle(cornerRadius: 8)
                .fill(.white.opacity(isSelected ? 0.10 : 0.04))
                .frame(width: visibleSize.width + 12, height: visibleSize.height + 12)
                .overlay {
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(accentColour.opacity(isSelected ? 0.95 : 0.45), lineWidth: isSelected ? 2.4 : 1.4)
                }

            RoundedRectangle(cornerRadius: 8)
                .stroke((isOverlapping ? Color.red : Color.yellow).opacity(isSelected || isOverlapping ? 0.92 : 0.42), style: StrokeStyle(lineWidth: isSelected || isOverlapping ? 2.0 : 1.2, dash: [6, 4]))
                .frame(width: hitSize.width, height: hitSize.height)

            groupContent
                .allowsHitTesting(false)

            Text(id.uppercased())
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle((isOverlapping ? Color.red : accentColour).opacity(isSelected ? 1.0 : 0.75))
                .offset(y: -(visibleSize.height / 2 + 12))
        }
        .frame(width: hitSize.width, height: hitSize.height)
        .contentShape(Rectangle())
        .position(x: currentX, y: currentY)
        .opacity(isVisible ? 0.8 : 0.25)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { v in
                    selectedControlID = id
                    let hasMoved = abs(v.translation.width) > 1 || abs(v.translation.height) > 1
                    if hasMoved && !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    dragOffset = v.translation
                }
                .onEnded { v in
                    selectedControlID = id
                    let hasMoved = abs(v.translation.width) > 1 || abs(v.translation.height) > 1
                    hasPushedSnapshot = false
                    if hasMoved {
                        let newX = (pos.x * areaW + v.translation.width) / areaW
                        let newY = (pos.y * areaH + v.translation.height) / areaH
                        updatePosition(x: newX.clamped(0, 1), y: newY.clamped(0, 1))
                    }
                    dragOffset = .zero
                }
        )
        .simultaneousGesture(
            MagnifyGesture()
                .onChanged { v in
                    if !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    currentScale = v.magnification
                }
                .onEnded { v in
                    hasPushedSnapshot = false
                    updateScale(magnification: v.magnification)
                    currentScale = 1.0
                }
        )
    }

    private func updatePosition(x: CGFloat, y: CGFloat) {
        var p = stored
        p.x = x
        p.y = fit?.storedY(y) ?? y
        if isLandscape {
            layout.landscape[id] = p
        } else {
            layout.portrait[id] = p
        }
    }

    private func updateScale(magnification: CGFloat) {
        // Uniform pinch: both axes scale by the same factor, keeping the width/height ratio.
        var p = stored
        p.scaleX = PadLayoutMetrics.clampedScale(p.scaleX * magnification)
        p.scaleY = PadLayoutMetrics.clampedScale(p.scaleY * magnification)
        if isLandscape {
            layout.landscape[id] = p
        } else {
            layout.portrait[id] = p
        }
    }

    private var baseSize: CGSize {
        padEditorBaseSize(for: id, isLandscape: isLandscape, analogStickScale: analogStickScale)
    }

    @ViewBuilder
    private var groupContent: some View {
        switch id {
        case "dpad":
            DPadView(size: isLandscape ? VirtualPadButtonOffset.dpadLandscapeSize : VirtualPadButtonOffset.dpadPortraitSize)
        case "action":
            ActionButtonsView(size: VirtualPadButtonOffset.actionButtonSize)
        case "l1":
            PadBtn(label: "L1", w: baseSize.width, h: baseSize.height, btn: .L1, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY)
        case "l2":
            PadBtn(label: "L2", w: baseSize.width, h: baseSize.height, btn: .L2, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY)
        case "r1":
            PadBtn(label: "R1", w: baseSize.width, h: baseSize.height, btn: .R1, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY)
        case "r2":
            PadBtn(label: "R2", w: baseSize.width, h: baseSize.height, btn: .R2, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY)
        case "lstick":
            StickView(isLeft: true, sizeScale: analogStickScale, layoutScale: visibleScaleX, badgeScale: fit?.scale ?? 1)
        case "rstick":
            StickView(isLeft: false, sizeScale: analogStickScale, layoutScale: visibleScaleX, badgeScale: fit?.scale ?? 1)
        case "select":
            PadBtn(label: "SEL", w: baseSize.width, h: baseSize.height, btn: .select, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY)
        case "start":
            PadBtn(label: "START", w: baseSize.width, h: baseSize.height, btn: .start, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY)
        default:
            EmptyView()
        }
    }
}

// MARK: - Draggable individual button widget
struct DraggableButton: View {
    let id: String
    let areaW: CGFloat
    let areaH: CGFloat
    let isLandscape: Bool
    let fitted: PadLayoutSnapshot?
    let fit: PadPortraitFit?
    @Binding var selectedControlID: String?
    let isSelected: Bool
    let isOverlapping: Bool
    let onBeginEdit: () -> Void

    @State private var layout = PadLayoutStore.shared
    @State private var dragOffset: CGSize = .zero
    @State private var currentScale: CGFloat = 1.0
    @State private var hasPushedSnapshot = false
    @Environment(\.uiAccentColour) private var accentColour

    private var stored: PadGroupPosition {
        let area = fit?.referenceArea(deckWidth: areaW) ?? CGSize(width: areaW, height: areaH)
        return layout.perButtonPosition(for: id, landscape: isLandscape, areaW: area.width, areaH: area.height)
    }

    private var pos: PadGroupPosition {
        fitted?.perButtonPosition(for: id, landscape: false, areaW: areaW, areaH: areaH) ?? stored
    }

    private var currentX: CGFloat { pos.x * areaW + dragOffset.width }
    private var currentY: CGFloat { pos.y * areaH + dragOffset.height }

    // Pinch multiplies both axes by the same factor (ratio-preserving / uniform).
    private var visibleScaleX: CGFloat {
        PadLayoutMetrics.clampedScale(pos.scaleX * currentScale)
    }
    private var visibleScaleY: CGFloat {
        PadLayoutMetrics.clampedScale(pos.scaleY * currentScale)
    }
    private var visibleSize: CGSize {
        CGSize(
            width: PadLayoutMetrics.visibleLength(baseLength: baseSize.width, visibleScale: visibleScaleX),
            height: PadLayoutMetrics.visibleLength(baseLength: baseSize.height, visibleScale: visibleScaleY)
        )
    }
    private var hitSize: CGSize {
        CGSize(
            width: PadLayoutMetrics.touchLength(baseLength: baseSize.width, hitScale: pos.hitScaleX),
            height: PadLayoutMetrics.touchLength(baseLength: baseSize.height, hitScale: pos.hitScaleY)
        )
    }

    var body: some View {
        let isVisible = layout.isControlVisible(id)
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(.white.opacity(isSelected ? 0.10 : 0.04))
                .frame(width: visibleSize.width + 10, height: visibleSize.height + 10)
                .overlay {
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(accentColour.opacity(isSelected ? 0.95 : 0.45), lineWidth: isSelected ? 2.3 : 1.3)
                }

            RoundedRectangle(cornerRadius: 6)
                .stroke((isOverlapping ? Color.red : Color.yellow).opacity(isSelected || isOverlapping ? 0.92 : 0.42), style: StrokeStyle(lineWidth: isSelected || isOverlapping ? 2.0 : 1.2, dash: [6, 4]))
                .frame(width: hitSize.width, height: hitSize.height)

            buttonContent
                .allowsHitTesting(false)

            Text(buttonLabel.uppercased())
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle((isOverlapping ? Color.red : accentColour).opacity(isSelected ? 1.0 : 0.75))
                .offset(y: -(visibleSize.height / 2 + 10))
        }
        .frame(width: hitSize.width, height: hitSize.height)
        .contentShape(Rectangle())
        .position(x: currentX, y: currentY)
        .opacity(isVisible ? 0.8 : 0.25)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { v in
                    selectedControlID = id
                    let hasMoved = abs(v.translation.width) > 1 || abs(v.translation.height) > 1
                    if hasMoved && !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    dragOffset = v.translation
                }
                .onEnded { v in
                    selectedControlID = id
                    let hasMoved = abs(v.translation.width) > 1 || abs(v.translation.height) > 1
                    hasPushedSnapshot = false
                    if hasMoved {
                        let newX = (pos.x * areaW + v.translation.width) / areaW
                        let newY = (pos.y * areaH + v.translation.height) / areaH
                        updatePosition(x: newX.clamped(0, 1), y: newY.clamped(0, 1))
                    }
                    dragOffset = .zero
                }
        )
        .simultaneousGesture(
            MagnifyGesture()
                .onChanged { v in
                    if !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    currentScale = v.magnification
                }
                .onEnded { v in
                    hasPushedSnapshot = false
                    updateScale(magnification: v.magnification)
                    currentScale = 1.0
                }
        )
    }

    private var buttonLabel: String {
        switch id {
        case "up": return "UP"
        case "down": return "DN"
        case "left": return "LT"
        case "right": return "RT"
        case "triangle": return "TRI"
        case "circle": return "CIR"
        case "square": return "SQU"
        case "cross": return "CRO"
        default: return id
        }
    }

    private var baseSize: CGSize {
        padEditorBaseSize(for: id, isLandscape: isLandscape, analogStickScale: 1.0)
    }

    private func updatePosition(x: CGFloat, y: CGFloat) {
        var p = stored
        p.x = x
        p.y = fit?.storedY(y) ?? y
        if isLandscape {
            layout.perButtonLandscape[id] = p
        } else {
            layout.perButtonPortrait[id] = p
        }
    }

    private func updateScale(magnification: CGFloat) {
        // Uniform pinch: both axes scale by the same factor, keeping the width/height ratio.
        var p = stored
        p.scaleX = PadLayoutMetrics.clampedScale(p.scaleX * magnification)
        p.scaleY = PadLayoutMetrics.clampedScale(p.scaleY * magnification)
        if isLandscape {
            layout.perButtonLandscape[id] = p
        } else {
            layout.perButtonPortrait[id] = p
        }
    }

    @ViewBuilder
    private var buttonContent: some View {
        switch id {
        case "up":
            PadBtn(label: "▲", w: baseSize.width, h: baseSize.height, btn: .up, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "down":
            PadBtn(label: "▼", w: baseSize.width, h: baseSize.height, btn: .down, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "left":
            PadBtn(label: "◀", w: baseSize.width, h: baseSize.height, btn: .left, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "right":
            PadBtn(label: "▶", w: baseSize.width, h: baseSize.height, btn: .right, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "triangle":
            PSBtn(sym: "△", clr: .green, sz: baseSize.width, btn: .triangle, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "cross":
            PSBtn(sym: "✕", clr: .blue, sz: baseSize.width, btn: .cross, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "square":
            PSBtn(sym: "□", clr: .pink, sz: baseSize.width, btn: .square, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        case "circle":
            PSBtn(sym: "○", clr: .red, sz: baseSize.width, btn: .circle, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: pos.hitScaleX, hitScaleY: pos.hitScaleY)
        default:
            EmptyView()
        }
    }
}

extension PadPortraitFit {
    /// The deck a per-button position is derived in, so a stored edit fits back to its drop point.
    func referenceArea(deckWidth: CGFloat) -> CGSize {
        CGSize(width: deckWidth / scale, height: Self.referenceDeckHeight)
    }

    /// The stored y of a control dropped at `y` in the fitted deck.
    func storedY(_ y: CGFloat) -> CGFloat {
        (((y * deckHeight - Self.topGap) / scale + envelopeTop) / Self.referenceDeckHeight).clamped(0, 1)
    }
}

// MARK: - Clamp helper
private extension CGFloat {
    func clamped(_ lo: CGFloat, _ hi: CGFloat) -> CGFloat {
        Swift.min(Swift.max(self, lo), hi)
    }
}
