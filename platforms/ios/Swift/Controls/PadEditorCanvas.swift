// PadEditorCanvas.swift — the pad layout editor's grab areas, selection, grid, snap guides and overlaps
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Grab areas for every piece, over a pad drawn with hit testing off.
struct PadEditorPieces: View {
    static let space = "ARMSX2iOSPadEditorPad"

    let editor: PadEditor
    @GestureState private var pinching = false

    var body: some View {
        let geometries = editor.geometries()
        ZStack {
            // An empty spot clears the selection.
            Color.clear
                .contentShape(Rectangle())
                .onTapGesture { editor.selection = nil }
            let partner = editor.mirrorEnabled ? editor.selection.flatMap { PadEditor.mirrorPartner[$0] } : nil
            ForEach(geometries, id: \.id) { geometry in
                PadPieceEditOverlay(
                    editor: editor,
                    geometry: geometry,
                    isSelected: editor.selection == geometry.id,
                    isPartner: partner == geometry.id,
                    isHidden: editor.isHidden(geometry.id)
                ) { translation in
                    editor.drag(geometry.id, translation: translation)
                } onEnd: {
                    editor.endDrag(geometry.id)
                }
            }
            if let selected = geometries.first(where: { $0.id == editor.selection }) {
                PadResizeHandle(editor: editor, geometry: selected)
                if editor.zoneFrame(selected.id) != nil { PadZoneHandle(editor: editor, id: selected.id) }
                if editor.resizing { readout(selected) }
            }
        }
        .frame(width: editor.area.size.width, height: editor.area.size.height)
        .contentShape(Rectangle())
        // On a piece or on empty canvas, a pinch resizes the selected piece.
        .simultaneousGesture(
            MagnifyGesture()
                .updating($pinching) { _, pinching, _ in pinching = true }
                .onChanged { value in
                    if let id = editor.selection { editor.resize(id, by: value.magnification) }
                }
        )
        .onChange(of: pinching) { _, pinching in
            if !pinching { editor.endResize() }
        }
        .coordinateSpace(.named(Self.space))
    }

    private func readout(_ geometry: PadEditorControlGeometry) -> some View {
        let p = editor.resolved(geometry.id)
        return Text(verbatim: "W \(Int((p.scaleX * 100).rounded()))% · H \(Int((p.scaleY * 100).rounded()))%")
            .font(.caption)
            .monospacedDigit()
            .foregroundStyle(OverlayTheme.textPrimary)
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(OverlayFrostBackground().clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous)))
            .position(x: geometry.center.x, y: max(12, geometry.visibleFrame.minY - 8 - 12))
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

/// One VoiceOver element per piece, whose actions stand in for dragging and pinching.
struct PadPieceEditOverlay: View {
    let editor: PadEditor
    let geometry: PadEditorControlGeometry
    let isSelected: Bool
    let isPartner: Bool
    let isHidden: Bool
    let onDrag: (CGSize) -> Void
    let onEnd: () -> Void
    private let settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accent

    var body: some View {
        let frame = geometry.visibleFrame, id = geometry.id
        Color.clear
            .frame(width: max(frame.width, 44), height: max(frame.height, 44))
            .contentShape(Rectangle())
            .overlay {
                if isHidden {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .stroke(Color.white.opacity(0.4), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
                        .frame(width: frame.width, height: frame.height)
                }
            }
            .overlay {
                if isSelected || isPartner {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .stroke(accent.opacity(isSelected ? 1 : 0.5), style: StrokeStyle(lineWidth: 1, dash: isSelected ? [] : [4, 4]))
                        .frame(width: frame.width, height: frame.height)
                        .padding(-4)
                }
            }
            .accessibilityElement()
            .accessibilityLabel(Text(isHidden ? String(format: settings.localized("%@, hidden"), padPieceName(id)) : padPieceName(id)))
            .accessibilityValue(Text(spokenValue))
            .accessibilityHint(Text(settings.localized("Double-tap to select.")))
            .accessibilityAddTraits(isSelected ? [.isButton, .isSelected] : .isButton)
            .accessibilityAction { editor.selection = id }
            .accessibilityAction(named: Text(settings.localized("Move Up"))) { editor.nudge(id, dx: 0, dy: -8) }
            .accessibilityAction(named: Text(settings.localized("Move Down"))) { editor.nudge(id, dx: 0, dy: 8) }
            .accessibilityAction(named: Text(settings.localized("Move Left"))) { editor.nudge(id, dx: -8, dy: 0) }
            .accessibilityAction(named: Text(settings.localized("Move Right"))) { editor.nudge(id, dx: 8, dy: 0) }
            .accessibilityAction(named: Text(settings.localized("Make Bigger"))) { resize(by: 1.1) }
            .accessibilityAction(named: Text(settings.localized("Make Smaller"))) { resize(by: 0.9) }
            .position(geometry.center)
            // In the pad's space: the piece moves under the finger, which would skew a local translation.
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .named(PadEditorPieces.space))
                    .onChanged { onDrag($0.translation) }
                    .onEnded { _ in onEnd() }
            )
    }

    /// "<place>, width <W>%, height <H>%", the place by thirds of the pad.
    private var spokenValue: String {
        let size = editor.area.size
        // Int() traps on nan, which a layout with no finite place would give.
        let third = { (v: CGFloat) in v.isNaN ? 1 : Int(min(max(3 * v, 0), 2)) }
        let column = third(geometry.center.x / max(size.width, 1))
        let row = third(geometry.center.y / max(size.height, 1))
        let places = [["top left", "top", "top right"], ["left", "centre", "right"], ["bottom left", "bottom", "bottom right"]]
        let p = editor.resolved(geometry.id)
        return String(
            format: settings.localized("%@, width %d%%, height %d%%"),
            settings.localized(places[row][column]), Int((p.scaleX * 100).rounded()), Int((p.scaleY * 100).rounded())
        )
    }

    private func resize(by factor: CGFloat) {
        editor.resize(geometry.id, by: factor)
        editor.endResize()
    }
}

/// Scales the selected piece by one factor on both axes.
private struct PadResizeHandle: View {
    let editor: PadEditor
    let geometry: PadEditorControlGeometry
    // The corner's offset from the centre when the drag began, which the factor is measured along.
    @State private var reach: CGVector?
    @Environment(\.uiAccentColour) private var accent

    var body: some View {
        let frame = geometry.visibleFrame, size = editor.area.size
        // Near an edge the bottom-trailing corner would leave the pad, so the corner facing the middle takes it.
        let nearEdge = frame.maxX + 22 > size.width || frame.maxY + 22 > size.height
        let corner = CGPoint(
            x: !nearEdge || frame.midX < size.width / 2 ? frame.maxX : frame.minX,
            y: !nearEdge || frame.midY < size.height / 2 ? frame.maxY : frame.minY
        )
        Circle()
            .fill(accent)
            .frame(width: 14, height: 14)
            .overlay(Circle().stroke(OverlayTheme.shell, lineWidth: 2))
            .frame(width: 44, height: 44)
            .contentShape(Rectangle())
            .position(corner)
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .named(PadEditorPieces.space))
                    .onChanged { value in
                        let start = reach ?? CGVector(dx: corner.x - frame.midX, dy: corner.y - frame.midY)
                        reach = start
                        let length = start.dx * start.dx + start.dy * start.dy
                        guard length > 0 else { return }
                        let along = (start.dx + value.translation.width) * start.dx + (start.dy + value.translation.height) * start.dy
                        editor.resize(geometry.id, by: max(along / length, 0.01))
                    }
                    .onEnded { _ in
                        reach = nil
                        editor.endResize()
                    }
            )
            .accessibilityHidden(true)
    }
}

/// Resizes the selected stick's zone from its inner top corner.
private struct PadZoneHandle: View {
    let editor: PadEditor
    let id: String
    @Environment(\.uiAccentColour) private var accent

    var body: some View {
        let corner = editor.zoneCorner(id), size = editor.area.size
        Circle()
            .fill(accent)
            .frame(width: 14, height: 14)
            .overlay(Circle().stroke(OverlayTheme.shell, lineWidth: 2))
            .frame(width: 44, height: 44)
            .contentShape(Rectangle())
            .position(x: corner.x * size.width, y: corner.y * size.height)
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .named(PadEditorPieces.space))
                    .onChanged { editor.moveZoneCorner(id, to: $0.location) }
            )
            .accessibilityHidden(true)
    }
}

/// The grid, zones, touch areas, overlaps and snap guides; none takes a touch.
struct PadEditorMarks: View {
    let editor: PadEditor
    let showGrid: Bool
    let showTouchAreas: Bool
    @Environment(\.uiAccentColour) private var accent
    @Environment(\.uiCriticalTextColour) private var critical

    var body: some View {
        let size = editor.area.size
        ZStack(alignment: .topLeading) {
            if showGrid {
                Path { path in
                    for x in stride(from: CGFloat(0), through: size.width, by: 8) {
                        path.move(to: CGPoint(x: x, y: 0))
                        path.addLine(to: CGPoint(x: x, y: size.height))
                    }
                    for y in stride(from: CGFloat(0), through: size.height, by: 8) {
                        path.move(to: CGPoint(x: 0, y: y))
                        path.addLine(to: CGPoint(x: size.width, y: y))
                    }
                }
                .stroke(Color.white.opacity(0.08), lineWidth: 0.5)
            }
            zones
            touchAreas
            ForEach(Array(editor.guides), id: \.self) { guide in
                Rectangle()
                    .fill(accent)
                    .frame(width: guide.axis == .x ? 1 : size.width, height: guide.axis == .y ? 1 : size.height)
                    .position(
                        x: guide.axis == .x ? guide.position : size.width / 2,
                        y: guide.axis == .y ? guide.position : size.height / 2
                    )
            }
        }
        .frame(width: size.width, height: size.height, alignment: .topLeading)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private var zones: some View {
        let swipe = SettingsStore.shared.dynamicThumbsticks.swipeCamera
        let labels = ["lstick": "Left stick zone", "rstick": swipe ? "Right stick and camera zone" : "Right stick zone"]
        return ForEach(["lstick", "rstick"].filter { !editor.isHidden($0) }, id: \.self) { id in
            if let zone = editor.zoneFrame(id) {
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(accent.opacity(0.06))
                    .overlay(
                        RoundedRectangle(cornerRadius: 14, style: .continuous)
                            .stroke(accent.opacity(0.35), style: StrokeStyle(lineWidth: 1, dash: [6, 4]))
                    )
                    .overlay(alignment: id == "lstick" ? .topLeading : .topTrailing) {
                        Text(SettingsStore.shared.localized(labels[id] ?? id))
                            .font(.caption)
                            .foregroundStyle(OverlayTheme.textSecondary)
                            .padding(8)
                    }
                    .frame(width: zone.width, height: zone.height)
                    .position(x: zone.midX, y: zone.midY)
            }
        }
    }

    /// The selected piece's own area always shows, and every colliding one in the destructive colour.
    @ViewBuilder private var touchAreas: some View {
        let pairs = editor.overlaps()
        let colliding = Set(pairs.flatMap { [$0.0, $0.1] })
        let regions = editor.geometries().filter { g in
            g.id == editor.selection || colliding.contains(g.id) || showTouchAreas && !editor.isHidden(g.id)
        }
        ForEach(regions, id: \.id) { g in
            let red = colliding.contains(g.id)
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .stroke(
                    red ? critical : accent.opacity(g.id == editor.selection ? 1 : 0.5),
                    style: StrokeStyle(lineWidth: red ? 1.5 : 1, dash: [6, 4])
                )
                .frame(width: g.hitFrame.width, height: g.hitFrame.height)
                .position(x: g.hitFrame.midX, y: g.hitFrame.midY)
        }
        let frames = Dictionary(uniqueKeysWithValues: regions.map { ($0.id, $0.hitFrame) })
        ForEach(pairs.indices, id: \.self) { index in
            if let a = frames[pairs[index].0], let b = frames[pairs[index].1] {
                let shared = a.intersection(b)
                Rectangle()
                    .fill(critical.opacity(0.2))
                    .frame(width: shared.width, height: shared.height)
                    .position(x: shared.midX, y: shared.midY)
            }
        }
    }
}

/// An orientation the device is not in, drawn full size and scaled into the screen inside a margin.
struct PadEditorPreview<Content: View>: View {
    let slot: PadEditorSlot
    let pad: CGSize
    let scale: CGFloat
    @ViewBuilder let content: Content

    /// The pad area that orientation plays in on this screen; a portrait game takes the top as with no fold.
    static func padSize(_ slot: PadEditorSlot, screen: CGSize) -> CGSize {
        let long = max(screen.width, screen.height), short = min(screen.width, screen.height)
        guard !slot.isLandscape else { return CGSize(width: long, height: short) }
        return CGSize(width: short, height: long - min(short * 3 / 4, long * 0.6))
    }

    static func scale(_ pad: CGSize, screen: CGSize) -> CGFloat {
        guard pad.width > 0, pad.height > 0 else { return 1 }
        return max(0.1, min((screen.width - 48) / pad.width, (screen.height - 48) / pad.height, 1))
    }

    var body: some View {
        content
            .frame(width: pad.width, height: pad.height)
            .scaleEffect(scale)
            .frame(width: pad.width * scale, height: pad.height * scale)
            .overlay { Rectangle().strokeBorder(OverlayTheme.separator, lineWidth: 1).allowsHitTesting(false) }
            .overlay(alignment: .topLeading) {
                Text(String(format: SettingsStore.shared.localized("%@ preview"), slot.title))
                    .font(.caption)
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(OverlayFrostBackground().clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous)))
                    .padding(8)
                    .allowsHitTesting(false)
            }
    }
}

/// The game area behind the pad: empty in game so the paused frame shows, else the cover dimmed over the outline.
struct PadEditorGameArea: View {
    let backdrop: PadEditorBackdrop
    var outlined = true
    /// Where the bar ends, so the caption sits under it.
    var labelTop: CGFloat = 0
    @State private var cover: UIImage?
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    private var url: URL? {
        if case .cover(let url) = backdrop { return url }
        return nil
    }

    var body: some View {
        ZStack {
            if outlined && backdrop != .pausedGame {
                Rectangle()
                    .strokeBorder(OverlayTheme.separator, lineWidth: 1)
                    .overlay(alignment: .topLeading) {
                        Text(SettingsStore.shared.localized("Game area"))
                            .font(.caption)
                            .foregroundStyle(OverlayTheme.textSecondary)
                            .padding(8)
                            .padding(.top, labelTop)
                    }
            }
            if let cover {
                Color.clear
                    .overlay { Image(uiImage: cover).resizable().scaledToFill() }
                    .clipped()
                    .overlay(OverlayTheme.scrimBase.opacity(reduceTransparency ? 0.8 : 0.6))
                    .transition(.opacity)
            }
        }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.2), value: cover != nil)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .task(id: url) {
            guard let url else {
                cover = nil
                return
            }
            let screen = UIApplication.shared.appScreen
            let side = max(screen.bounds.width, screen.bounds.height)
            cover = await CoverThumbnailCache.shared.thumbnail(for: url, signature: nil, width: side, height: side, scale: screen.scale)
        }
    }
}

// MARK: Overlaps

extension PadEditor {
    /// The arrows share one hit test, and so do the face buttons, which settle by nearness; the rest stand alone.
    func hitGroup(_ id: String) -> Int {
        switch PadLayoutStore.groupID(for: id) {
        case "dpad": return 0
        case "action": return 1
        default: return 2 + (Self.pieceIDs.firstIndex(of: id) ?? Self.pieceIDs.count)
        }
    }

    /// Pairs of pieces in play from different hit groups whose touch areas share some area.
    func overlaps() -> [(String, String)] {
        let shown = geometries().filter { !isHidden($0.id) }
        var pairs: [(String, String)] = []
        for (index, a) in shown.enumerated() {
            for b in shown[(index + 1)...] where hitGroup(a.id) != hitGroup(b.id) && Self.clash(a, b) && !defaultShoulders(a.id, b.id) {
                pairs.append((a.id, b.id))
            }
        }
        return pairs
    }

    // The built-in L1/L2 and R1/R2 touch areas meet, so that pair warns only once one of them is moved or resized.
    private func defaultShoulders(_ a: String, _ b: String) -> Bool {
        let pair = [a, b].sorted()
        guard pair == ["l1", "l2"] || pair == ["r1", "r2"] else { return false }
        let defaults = slot.isLandscape ? PadLayoutStore.defaultLandscape : PadLayoutStore.defaultPortrait
        return pair.allSatisfy { id in
            guard var stored = position(of: id), let builtIn = defaults[id] else { return true }
            stored.opacity = builtIn.opacity
            return stored == builtIn
        }
    }

    /// The pieces `id` overlaps, top to bottom, then left to right.
    func overlapping(_ id: String) -> [String] {
        let centres = Dictionary(uniqueKeysWithValues: geometries().map { ($0.id, $0.center) })
        return overlaps().compactMap { $0.0 == id ? $0.1 : $0.1 == id ? $0.0 : nil }.sorted { a, b in
            let p = centres[a] ?? .zero, q = centres[b] ?? .zero
            return (p.y, p.x, a) < (q.y, q.x, b)
        }
    }

    /// A stick takes touches only inside its circle, so a piece in its frame's corner is clear of it.
    private static func clash(_ a: PadEditorControlGeometry, _ b: PadEditorControlGeometry) -> Bool {
        let shared = a.hitFrame.intersection(b.hitFrame)
        guard !shared.isNull, shared.width > 0, shared.height > 0 else { return false }
        switch (isStick(a.id), isStick(b.id)) {
        case (false, false):
            return true
        case (true, true):
            return hypot(a.center.x - b.center.x, a.center.y - b.center.y) < (a.hitFrame.width + b.hitFrame.width) / 2
        default:
            let (stick, other) = isStick(a.id) ? (a, b.hitFrame) : (b, a.hitFrame)
            let nearest = CGPoint(
                x: min(max(stick.center.x, other.minX), other.maxX),
                y: min(max(stick.center.y, other.minY), other.maxY)
            )
            return hypot(nearest.x - stick.center.x, nearest.y - stick.center.y) < stick.hitFrame.width / 2
        }
    }
}
