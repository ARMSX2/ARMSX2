// PadEditor.swift — the pad layout editor's engine: a draft layout, the piece in hand and saving
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// A snap line the dragged piece meets, in pad points.
struct PadGuide: Hashable {
    enum Axis { case x, y }
    let axis: Axis
    let position: CGFloat
}

/// One orientation's part of the draft, which its undo steps restore without touching the others.
struct PadSlotSlice: Equatable {
    var groups: [String: PadGroupPosition]?
    var perButton: [String: PadGroupPosition]?
    /// The visibility keys a hide or show changed, with the values to restore. Visibility is one set for every
    /// orientation, so a step restores only its own keys and leaves later hides made elsewhere alone.
    var visibility: [String: Bool?]? = nil
}

/// The pad area the editor draws in, which the engine measures every piece against.
struct PadArea: Equatable {
    var size: CGSize = .zero
    var fit: PadPortraitFit?
    var analogStickScale: CGFloat = 1
    /// The book-pose fold in pad points, a snap line on both of its edges.
    var fold: CGRect?
    /// Screen points per pad point, under 1 in an orientation preview.
    var scale: CGFloat = 1
}

enum PadEditorSlot: String, CaseIterable, Identifiable {
    case portrait, landscape, book

    var id: String { rawValue }
    var isLandscape: Bool { self != .portrait }

    /// The slot a window plays: book on the Duo's open inner screen.
    @MainActor static func forWindow(_ size: CGSize) -> PadEditorSlot {
        guard size.width > size.height else { return .portrait }
        return AppState.shared.hingePresent && usesLargeLayout(windowSize: size) ? .book : .landscape
    }
}

enum PadEditorSaveTarget: Equatable {
    case update
    case saveAsNew(name: String)
}

enum PadEditorSaveScope {
    case allGames, thisGame, keepDefault
}

@MainActor
@Observable
final class PadEditor {
    static let pieceIDs = PadLayoutStore.perButtonIDs + PadLayoutStore.groupIDs.filter { $0 != "action" && $0 != "dpad" }
    static let snapReach = 6 as CGFloat
    static let margin = 16 as CGFloat
    static let undoDepth = 100
    static let mirrorPartner: [String: String] = {
        let pairs = [("left", "circle"), ("right", "square"), ("up", "triangle"), ("down", "cross"),
                     ("l1", "r1"), ("l2", "r2"), ("lstick", "rstick"), ("select", "start")]
        return Dictionary(uniqueKeysWithValues: pairs.flatMap { [($0.0, $0.1), ($0.1, $0.0)] })
    }()

    let deviceClass: PadDeviceClass
    private(set) var draft: PadLayoutSnapshot
    private(set) var original: PadLayoutSnapshot
    private(set) var slot: PadEditorSlot
    var selection: String?
    var area = PadArea()
    var snapEnabled = true
    var mirrorEnabled = false
    /// Try mode: the pad takes touches and nothing moves.
    var trying = false
    private(set) var dragging = false
    /// The dragged piece's frame in pad points, which the bar steps aside from.
    private(set) var dragFrame: CGRect?
    private(set) var guides: Set<PadGuide> = []
    private(set) var resizing = false
    // Per slot, so Undo never changes an orientation that is not on screen.
    private var undoStacks: [PadEditorSlot: [PadSlotSlice]] = [:]
    private var redoStacks: [PadEditorSlot: [PadSlotSlice]] = [:]
    // Bumped when the whole slot changes, so a held portrait fit refits.
    private(set) var refitGeneration = 0

    // The drag in flight and where its piece started, so a second finger cannot move it twice.
    // After a pinch the rest of the drag is ignored, since its start frame no longer holds.
    @ObservationIgnored private var gesture: (id: String, frame: CGRect, moved: Bool, pinched: Bool)?
    @ObservationIgnored private var sizing: (id: String, start: PadGroupPosition)?
    @ObservationIgnored private var lastStep: (id: String, setting: String, at: Date)?
    @ObservationIgnored private let tick = UISelectionFeedbackGenerator()

    init(snapshot: PadLayoutSnapshot, slot: PadEditorSlot, deviceClass: PadDeviceClass) {
        draft = snapshot
        original = snapshot
        self.slot = slot
        self.deviceClass = deviceClass
    }

    var isDirty: Bool { draft != original }

    /// The old slot's views go away with a drag, pinch or held stepper in flight, so it ends here.
    func switchSlot(to s: PadEditorSlot) {
        endDrag()
        endResize()
        lastStep = nil
        slot = s
    }

    var renderSnapshot: PadLayoutSnapshot { slot == .book ? draft.inBook() : draft }

    /// The snapshot as play lays it out in the area, fitted on the Duo's portrait deck.
    func shownSnapshot() -> PadLayoutSnapshot {
        area.fit.map { renderSnapshot.fittedToPortraitDeck($0, size: area.size, analogStickScale: area.analogStickScale) } ?? renderSnapshot
    }

    /// What the editor draws: hidden pieces too, faded to 0.25 so they can still be picked.
    func drawnSnapshot() -> PadLayoutSnapshot {
        var s = shownSnapshot()
        guard area.size.width > 0, area.size.height > 0 else { return s }
        let landscape = slot.isLandscape
        for id in Self.pieceIDs where isHidden(id) {
            if Self.isPerButton(id) {
                var p = s.perButtonPosition(for: id, landscape: landscape, areaW: area.size.width, areaH: area.size.height)
                p.opacity = 0.25
                if landscape { s.perButtonLandscape[id] = p } else { s.perButtonPortrait[id] = p }
            } else {
                var p = s.position(for: id, landscape: landscape)
                p.opacity = 0.25
                if landscape { s.landscape[id] = p } else { s.portrait[id] = p }
            }
        }
        s.controlVisibility = [:]
        return s
    }

    /// On-screen frames of every piece, hidden ones included.
    func geometries() -> [PadEditorControlGeometry] {
        // Until the pad is measured, a button that follows its group divides by a zero area and lands at NaN.
        guard area.size.width > 0, area.size.height > 0 else { return [] }
        return shownSnapshot().controlGeometries(
            size: area.size, landscape: slot.isLandscape, analogStickScale: area.analogStickScale, includeHidden: true
        )
    }

    func isHidden(_ id: String) -> Bool { !draft.isControlVisible(id) }

    private static func isPerButton(_ id: String) -> Bool { PadLayoutStore.perButtonIDs.contains(id) }

    /// The stored entry for `id` in this slot; nil while it still follows its default or group.
    func position(of id: String) -> PadGroupPosition? {
        let perButton = Self.isPerButton(id)
        switch slot {
        case .portrait: return (perButton ? draft.perButtonPortrait : draft.portrait)[id]
        case .landscape: return (perButton ? draft.perButtonLandscape : draft.landscape)[id]
        case .book: return (perButton ? draft.perButtonBook ?? draft.perButtonLandscape : draft.book ?? draft.landscape)[id]
        }
    }

    /// The stored entry, or the one `id` is drawn from while it follows its default or group.
    func resolved(_ id: String) -> PadGroupPosition {
        if let stored = position(of: id) { return stored }
        guard Self.isPerButton(id) else { return renderSnapshot.position(for: id, landscape: slot.isLandscape) }
        // Derived in the deck the stored layout was drawn for, so it fits back to the drop point.
        let deck = area.fit?.referenceArea(deckWidth: area.size.width) ?? area.size
        return renderSnapshot.perButtonPosition(
            for: id, landscape: slot.isLandscape, areaW: max(deck.width, 1), areaH: max(deck.height, 1)
        )
    }

    /// With Mirror on, the left/right partner takes the same size at the mirrored place.
    func place(_ id: String, _ p: PadGroupPosition) {
        write(id, p)
        guard mirrorEnabled, let partner = Self.mirrorPartner[id] else { return }
        var mirrored = p
        mirrored.x = 1 - p.x
        write(partner, mirrored)
    }

    /// Nil drops the entry, so the piece follows its default or group again.
    private func write(_ id: String, _ p: PadGroupPosition?) {
        let perButton = Self.isPerButton(id)
        switch slot {
        case .portrait:
            if perButton { draft.perButtonPortrait[id] = p } else { draft.portrait[id] = p }
        case .landscape:
            if perButton { draft.perButtonLandscape[id] = p } else { draft.landscape[id] = p }
        case .book:
            // Book starts as a copy of landscape on its first edit.
            if draft.book == nil { draft.book = draft.landscape }
            if draft.perButtonBook == nil { draft.perButtonBook = draft.perButtonLandscape }
            if perButton { draft.perButtonBook?[id] = p } else { draft.book?[id] = p }
        }
    }

    // MARK: Undo

    var canUndo: Bool { !(undoStacks[slot] ?? []).isEmpty }
    var canRedo: Bool { !(redoStacks[slot] ?? []).isEmpty }

    private var slice: PadSlotSlice {
        get { slice(of: slot) }
        set { setSlice(newValue, for: slot) }
    }

    private func slice(of s: PadEditorSlot) -> PadSlotSlice {
        switch s {
        case .portrait: return PadSlotSlice(groups: draft.portrait, perButton: draft.perButtonPortrait)
        case .landscape: return PadSlotSlice(groups: draft.landscape, perButton: draft.perButtonLandscape)
        case .book: return PadSlotSlice(groups: draft.book, perButton: draft.perButtonBook)
        }
    }

    /// The slice now, with the current values of the keys `step` restores, for the other stack.
    private func current(like step: PadSlotSlice) -> PadSlotSlice {
        var now = slice
        now.visibility = step.visibility.map { Dictionary(uniqueKeysWithValues: $0.keys.map { ($0, draft.controlVisibility[$0]) }) }
        return now
    }

    private func setSlice(_ newValue: PadSlotSlice, for s: PadEditorSlot) {
        switch s {
        case .portrait:
            draft.portrait = newValue.groups ?? [:]
            draft.perButtonPortrait = newValue.perButton ?? [:]
        case .landscape:
            draft.landscape = newValue.groups ?? [:]
            draft.perButtonLandscape = newValue.perButton ?? [:]
        case .book:
            draft.book = newValue.groups
            draft.perButtonBook = newValue.perButton
        }
        for (id, visible) in newValue.visibility ?? [:] { draft.controlVisibility[id] = visible }
    }

    /// Call before a change that is its own undo step.
    func record() {
        push(slice, to: slot)
    }

    /// The keys `change` sets, with their values before it.
    private func visibilityChange(_ change: () -> Void) -> [String: Bool?] {
        let before = draft.controlVisibility
        change()
        let keys = Set(before.keys).union(draft.controlVisibility.keys).filter { before[$0] != draft.controlVisibility[$0] }
        return Dictionary(uniqueKeysWithValues: keys.map { ($0, before[$0]) })
    }

    /// A hide or show as its own undo step.
    private func recordingVisibility(_ change: () -> Void) {
        var step = slice
        step.visibility = visibilityChange(change)
        push(step, to: slot)
    }

    private func push(_ step: PadSlotSlice, to s: PadEditorSlot) {
        lastStep = nil
        undoStacks[s, default: []].append(step)
        if undoStacks[s]!.count > Self.undoDepth { undoStacks[s]!.removeFirst() }
        redoStacks[s] = []
    }

    /// Changes to the same setting of one piece within 1 s, such as a held stepper, undo as one step.
    func stepping(_ id: String, setting: String, _ apply: () -> Void) {
        let before = slice
        apply()
        guard slice != before else { return }
        let now = Date()
        if let last = lastStep, last.id == id, last.setting == setting, now.timeIntervalSince(last.at) < 1 {
            lastStep?.at = now
            return
        }
        push(before, to: slot)
        lastStep = (id, setting, now)
    }

    func undo() {
        guard let last = undoStacks[slot]?.popLast() else { return }
        redoStacks[slot, default: []].append(current(like: last))
        restore(last)
    }

    func redo() {
        guard let next = redoStacks[slot]?.popLast() else { return }
        undoStacks[slot, default: []].append(current(like: next))
        restore(next)
    }

    private func restore(_ step: PadSlotSlice) {
        slice = step
        restarted()
    }

    // MARK: Dragging

    func drag(_ id: String, translation: CGSize) {
        let size = area.size
        guard size.width > 0, size.height > 0, sizing == nil else { return }
        if gesture == nil {
            guard let start = geometries().first(where: { $0.id == id }) else { return }
            selection = id
            gesture = (id, start.visibleFrame, false, false)
            tick.prepare()
        }
        // Under 4 pt is a tap's wobble: it selects and leaves the piece where it is.
        guard let held = gesture, held.id == id, !held.pinched, held.moved || hypot(translation.width, translation.height) >= 4 / area.scale else { return }
        if !held.moved {
            record()
            gesture?.moved = true
        }
        dragging = true
        var frame = keepInside(held.frame.offsetBy(dx: translation.width, dy: translation.height), size)
        var found = Set<PadGuide>()
        if snapEnabled {
            let others = geometries()
                .filter { $0.id != id && !isHidden($0.id) && !(mirrorEnabled && Self.mirrorPartner[id] == $0.id) }
            (frame, found) = snapped(frame, to: targets(size: size, fold: area.fold, others: others))
        }
        if found != guides, !found.isEmpty { tick.selectionChanged() }
        guides = found
        dragFrame = frame
        place(id, positioned(id, at: CGPoint(x: frame.midX, y: frame.midY)))
    }

    /// Ends the drag of `id`, or with nil whatever is in flight, as on a rotation.
    func endDrag(_ id: String? = nil) {
        if let id, gesture.map({ $0.id != id }) ?? false { return }
        gesture = nil
        dragging = false
        dragFrame = nil
        guides = []
    }

    /// In the order ties go: the margins, the pad's middle, the fold, then the other pieces in `pieceIDs` order.
    private func targets(size: CGSize, fold: CGRect?, others: [PadEditorControlGeometry]) -> (x: [CGFloat], y: [CGFloat]) {
        var x = [Self.margin, size.width - Self.margin, size.width / 2]
        var y = [Self.margin, size.height - Self.margin, size.height / 2]
        if let fold { x += [fold.minX, fold.maxX] }
        let frames = Dictionary(uniqueKeysWithValues: others.map { ($0.id, $0.visibleFrame) })
        for id in Self.pieceIDs {
            guard let frame = frames[id] else { continue }
            x += [frame.minX, frame.midX, frame.maxX]
            y += [frame.minY, frame.midY, frame.maxY]
        }
        return (x, y)
    }

    private func snapped(_ frame: CGRect, to lines: (x: [CGFloat], y: [CGFloat])) -> (CGRect, Set<PadGuide>) {
        // The reach is on screen, so a scaled-down preview snaps from as far as the full-size pad.
        let reach = Self.snapReach / area.scale
        func nearest(_ edges: [CGFloat], _ lines: [CGFloat]) -> (shift: CGFloat, line: CGFloat)? {
            var best: (shift: CGFloat, line: CGFloat)?
            for line in lines {
                for edge in edges where abs(line - edge) <= reach && abs(line - edge) < abs(best?.shift ?? .infinity) {
                    best = (line - edge, line)
                }
            }
            return best
        }
        var frame = frame
        var found = Set<PadGuide>()
        if let hit = nearest([frame.minX, frame.midX, frame.maxX], lines.x) {
            frame.origin.x += hit.shift
            found.insert(PadGuide(axis: .x, position: hit.line))
        }
        if let hit = nearest([frame.minY, frame.midY, frame.maxY], lines.y) {
            frame.origin.y += hit.shift
            found.insert(PadGuide(axis: .y, position: hit.line))
        }
        return (frame, found)
    }

    /// `id` with its centre moved to `centre` in the area.
    private func positioned(_ id: String, at centre: CGPoint) -> PadGroupPosition {
        var p = resolved(id)
        p.x = min(max(centre.x / area.size.width, 0), 1)
        p.y = area.fit?.storedY(centre.y / area.size.height) ?? min(max(centre.y / area.size.height, 0), 1)
        return p
    }

    private func keepInside(_ frame: CGRect, _ size: CGSize) -> CGRect {
        var frame = frame
        frame.origin.x = min(max(frame.minX, 0), max(0, size.width - frame.width))
        frame.origin.y = min(max(frame.minY, 0), max(0, size.height - frame.height))
        return frame
    }

    // MARK: Piece options

    /// Changes `id`, and with Mirror on its partner the same way, then nudges each back inside the pad.
    private func edit(_ id: String, _ change: (inout PadGroupPosition) -> Void) {
        guard area.size.width > 0, area.size.height > 0 else { return }
        for target in [id] + (mirrorEnabled ? [Self.mirrorPartner[id]].compactMap { $0 } : []) {
            var p = resolved(target)
            change(&p)
            write(target, p)
            guard let frame = geometries().first(where: { $0.id == target })?.visibleFrame else { continue }
            let inside = keepInside(frame, area.size)
            if inside.origin != frame.origin { write(target, positioned(target, at: CGPoint(x: inside.midX, y: inside.midY))) }
        }
    }

    /// `factor` multiplies the size the pinch or corner drag started from; each axis clamps on its own.
    func resize(_ id: String, by factor: CGFloat) {
        guard factor.isFinite, factor > 0 else { return }
        if sizing?.id != id { sizing = (id, resolved(id)) }
        guard let start = sizing?.start else { return }
        gesture?.pinched = true
        selection = id
        resizing = true
        stepping(id, setting: "resize") {
            edit(id) {
                $0.scaleX = PadLayoutMetrics.clampedScale(start.scaleX * factor)
                $0.scaleY = PadLayoutMetrics.clampedScale(start.scaleY * factor)
            }
        }
    }

    func endResize() {
        sizing = nil
        resizing = false
    }

    /// A stick is drawn round from one scale, so its size sets both axes.
    func setSize(_ id: String, kind: PadSizeKind, axis: PadSizeAxis, value: CGFloat) {
        let value = PadLayoutMetrics.clampedScale(value)
        stepping(id, setting: "\(kind).\(axis)") {
            edit(id) { p in
                switch (kind, axis) {
                case (.visible, _) where isStick(id): p.scale = value
                case (.visible, .x): p.scaleX = value
                case (.visible, .y): p.scaleY = value
                case (.hit, .x): p.hitScaleX = value
                case (.hit, .y): p.hitScaleY = value
                }
            }
        }
    }

    func stepOpacity(_ id: String, by delta: CGFloat) {
        let value = PadLayoutMetrics.clampedOpacity(((resolved(id).opacity + delta) * 10).rounded() / 10)
        stepping(id, setting: "opacity") { edit(id) { $0.opacity = value } }
    }

    // MARK: Floating sticks

    /// The stick's stored mode, else the global Dynamic Thumbsticks setting it follows.
    func effectiveFloating(_ id: String) -> Bool {
        resolved(id).floating ?? SettingsStore.shared.dynamicThumbsticks.dynamicThumbsticks
    }

    /// Stores an explicit mode, which the partner stick takes too with Mirror on.
    func setFloating(_ id: String, _ value: Bool) {
        guard isStick(id) else { return }
        stepping(id, setting: "floating") { edit(id) { $0.floating = value } }
    }

    /// True where the stick takes touches in a zone: floating, or the right stick under Swipe Camera.
    func usesZone(_ id: String) -> Bool {
        isStick(id) && (effectiveFloating(id) || id == "rstick" && SettingsStore.shared.dynamicThumbsticks.swipeCamera)
    }

    func zone(of id: String) -> CGRect {
        resolved(id).zone ?? PadLayoutMetrics.defaultZone(isLeft: id == "lstick")
    }

    func zoneFrame(_ id: String) -> CGRect? {
        guard usesZone(id) else { return nil }
        let z = zone(of: id), size = area.size
        return CGRect(x: z.minX * size.width, y: z.minY * size.height, width: z.width * size.width, height: z.height * size.height)
    }

    /// Sizes the zone from its outer and bottom edges, 25% to 100% of the pad.
    func setZoneSize(_ id: String, axis: PadSizeAxis, value: CGFloat) {
        let v = min(max(value, PadLayoutMetrics.minimumZoneShare), 1)
        var z = zone(of: id)
        switch axis {
        case .x:
            z.origin.x = id == "lstick" ? min(z.minX, 1 - v) : max(z.maxX - v, 0)
            z.size.width = v
        case .y:
            z.origin.y = max(z.maxY - v, 0)
            z.size.height = v
        }
        setZone(id, z, setting: "zone.\(axis)")
    }

    /// The handle's corner, inner top: the left stick's right edge, the right stick's left edge.
    func zoneCorner(_ id: String) -> CGPoint {
        let z = zone(of: id)
        return CGPoint(x: id == "lstick" ? z.maxX : z.minX, y: z.minY)
    }

    /// Moves the inner top corner to `point` in pad points; the outer bottom corner stays.
    func moveZoneCorner(_ id: String, to point: CGPoint) {
        guard area.size.width > 0, area.size.height > 0 else { return }
        let z = zone(of: id), m = PadLayoutMetrics.minimumZoneShare
        let x = min(max(point.x / area.size.width, 0), 1), y = min(max(point.y / area.size.height, 0), z.maxY - m)
        let next = id == "lstick"
            ? CGRect(x: z.minX, y: y, width: max(x - z.minX, m), height: z.maxY - y)
            : CGRect(x: min(x, z.maxX - m), y: y, width: max(z.maxX - x, m), height: z.maxY - y)
        setZone(id, next, setting: "zone.corner")
    }

    /// With Mirror on, the partner stick takes the mirrored zone.
    private func setZone(_ id: String, _ zone: CGRect, setting: String) {
        guard isStick(id), area.size.width > 0, let z = PadLayoutMetrics.clampedZone(zone) else { return }
        stepping(id, setting: setting) {
            for target in [id] + (mirrorEnabled ? [Self.mirrorPartner[id]].compactMap { $0 } : []) {
                var p = resolved(target)
                p.zone = target == id ? z : CGRect(x: 1 - z.maxX, y: z.minY, width: z.width, height: z.height)
                write(target, p)
            }
        }
    }

    /// Hidden pieces are one set for every orientation.
    func toggleHidden(_ id: String) {
        recordingVisibility {
            let group = PadLayoutStore.groupID(for: id)
            // The old editor hid the D-pad or face buttons as a group; its pieces keep their look on their own keys.
            if group != id, draft.controlVisibility[group] != nil {
                for member in PadLayoutStore.perButtonIDs where PadLayoutStore.groupID(for: member) == group {
                    draft.controlVisibility[member] = !isHidden(member)
                }
                draft.controlVisibility[group] = nil
            }
            draft.controlVisibility[id] = isHidden(id)
        }
    }

    /// True while `id` sits at its built-in place and size, or follows its group.
    func isDefault(_ id: String) -> Bool {
        guard let stored = position(of: id) else { return true }
        return !Self.isPerButton(id) && stored == (slot.isLandscape ? PadLayoutStore.defaultLandscape : PadLayoutStore.defaultPortrait)[id]
    }

    func resetPiece(_ id: String) {
        guard !isDefault(id) else { return }
        record()
        write(id, nil)
    }

    var hasHidden: Bool { Self.pieceIDs.contains(where: isHidden) }

    func showHidden() {
        guard hasHidden else { return }
        recordingVisibility { draft.controlVisibility = shownAll }
    }

    // Every piece shows on its own key, so the old editor's group keys can go.
    private var shownAll: [String: Bool] {
        var visibility = draft.controlVisibility
        for id in Self.pieceIDs where isHidden(id) { visibility[id] = true }
        visibility["dpad"] = nil
        visibility["action"] = nil
        return visibility
    }

    // MARK: Resets and copies

    /// Book has no layout of its own until edited, and plays landscape's.
    private func defaultSlice(_ s: PadEditorSlot) -> PadSlotSlice {
        switch s {
        case .portrait: return PadSlotSlice(groups: PadLayoutStore.defaultPortrait, perButton: [:])
        case .landscape: return PadSlotSlice(groups: PadLayoutStore.defaultLandscape, perButton: [:])
        case .book: return PadSlotSlice(groups: nil, perButton: nil)
        }
    }

    func isDefault(_ s: PadEditorSlot) -> Bool {
        let current = slice(of: s)
        guard s != .book else { return current.groups == nil && current.perButton == nil }
        let defaults = s == .portrait ? PadLayoutStore.defaultPortrait : PadLayoutStore.defaultLandscape
        return (current.perButton ?? [:]).isEmpty && (current.groups ?? [:]).allSatisfy { defaults[$0.key] == $0.value }
    }

    var isAllDefault: Bool { PadEditorSlot.allCases.allSatisfy(isDefault) && !hasHidden }

    /// Hidden pieces are not per orientation, so they stay as they are.
    func resetSlot() {
        guard !isDefault(slot) else { return }
        record()
        slice = defaultSlice(slot)
        restarted()
    }

    /// Every orientation and every hidden piece, with an undo step in each orientation that changed.
    func resetAll() {
        let shown = hasHidden ? visibilityChange { draft.controlVisibility = shownAll } : nil
        for s in PadEditorSlot.allCases where shown != nil || !isDefault(s) {
            var step = slice(of: s)
            step.visibility = shown
            push(step, to: s)
            setSlice(defaultSlice(s), for: s)
        }
        restarted()
    }

    func canCopy(from source: PadEditorSlot) -> Bool {
        source != slot && copied(from: source) != slice
    }

    func copy(from source: PadEditorSlot) {
        guard canCopy(from: source) else { return }
        record()
        slice = copied(from: source)
        restarted()
    }

    private func copied(from source: PadEditorSlot) -> PadSlotSlice {
        let from = slice(of: source)
        var into = slice
        into.groups = from.groups ?? draft.landscape
        into.perButton = from.perButton ?? draft.perButtonLandscape
        return into
    }

    private func restarted() {
        endDrag()
        endResize()
        lastStep = nil
        selection = nil
        refitGeneration += 1
    }

    // MARK: Controller and VoiceOver

    /// Every piece, hidden ones too: top to bottom, then left to right, ties by id.
    func readingOrder() -> [String] {
        geometries().sorted { ($0.center.y, $0.center.x, $0.id) < ($1.center.y, $1.center.x, $1.id) }.map(\.id)
    }

    /// The nearest piece within 45° of `direction` from `point`; the first in reading order wins a tie.
    func neighbour(from point: CGPoint, direction: CGVector) -> String? {
        let length = hypot(direction.dx, direction.dy)
        guard length > 0 else { return nil }
        let centres = Dictionary(uniqueKeysWithValues: geometries().map { ($0.id, $0.center) })
        var best: (id: String, distance: CGFloat)?
        for id in readingOrder() {
            guard let c = centres[id] else { continue }
            let dx = c.x - point.x, dy = c.y - point.y, distance = hypot(dx, dy)
            guard distance > 0, (dx * direction.dx + dy * direction.dy) / length >= distance * 0.7071 else { continue }
            if distance < best?.distance ?? .infinity { best = (id, distance) }
        }
        return best?.id
    }

    /// The next or previous piece in reading order; with nothing selected, the last or the first.
    func cycle(forward: Bool) {
        let order = readingOrder()
        guard !order.isEmpty else { return }
        guard let current = selection, let index = order.firstIndex(of: current) else {
            selection = forward ? order.last : order.first
            return
        }
        selection = order[(index + (forward ? 1 : order.count - 1)) % order.count]
    }

    /// Moves `id` by screen points with no snapping; moves within 1 s undo as one step.
    func nudge(_ id: String, dx: CGFloat, dy: CGFloat) {
        guard area.size.width > 0, area.size.height > 0,
              let frame = geometries().first(where: { $0.id == id })?.visibleFrame else { return }
        let moved = keepInside(frame.offsetBy(dx: dx / area.scale, dy: dy / area.scale), area.size)
        stepping(id, setting: "nudge") { place(id, positioned(id, at: CGPoint(x: moved.midX, y: moved.midY))) }
    }

    // MARK: Layouts

    /// Another layout becomes the draft, with no history.
    func load(_ snapshot: PadLayoutSnapshot) {
        draft = snapshot
        original = snapshot
        undoStacks = [:]
        redoStacks = [:]
        restarted()
    }

    /// Written at once, outside the draft, so Discard keeps it.
    func rename(_ presetID: String, to name: String, store: PadLayoutPresetStore) throws {
        guard !store.readOnly else { throw PadLayoutPresetStoreError.readOnly }
        guard store.preset(id: presetID).map({ $0.source != .builtIn }) ?? false else { throw PadLayoutPresetStoreError.missingPreset }
        try store.reportingWriteFailure { try store.renamePreset(id: presetID, to: name) }
    }

    /// Copies the stored layout, not the draft, at once; nil copies the Current Layout's INI working copy.
    @discardableResult
    func duplicate(_ presetID: String?, named name: String, store: PadLayoutPresetStore, layout: PadLayoutStore) throws -> PadLayoutPreset {
        guard !store.readOnly else { throw PadLayoutPresetStoreError.readOnly }
        return try store.reportingWriteFailure {
            guard let presetID else { return store.createPreset(named: name, snapshot: layout.snapshot()) }
            return try store.duplicatePreset(id: presetID, named: name)
        }
    }

    // MARK: Saving

    /// The draft's only write; Rename and Duplicate write at once. Returns the preset the layout went to, nil for the Current Layout.
    @discardableResult
    func save(
        target: PadEditorSaveTarget,
        scope: PadEditorSaveScope,
        context: PadLayoutEditorContext,
        store: PadLayoutPresetStore,
        layout: PadLayoutStore
    ) throws -> String? {
        let savedID = try store.reportingWriteFailure { () throws -> String? in
            let saved: String?
            switch target {
            case .update:
                if let id = context.presetID {
                    guard !store.readOnly else { throw PadLayoutPresetStoreError.readOnly }
                    try store.updatePreset(id: id, snapshot: draft, for: deviceClass)
                    saved = id
                } else if context.initialSnapshot == nil {
                    layout.apply(snapshot: draft)
                    layout.save()
                    saved = nil
                } else {
                    throw PadLayoutPresetStoreError.missingPreset
                }
            case .saveAsNew(let name):
                guard !store.readOnly else { throw PadLayoutPresetStoreError.readOnly }
                saved = store.createPreset(named: name, snapshot: draft).id
            }
            switch scope {
            case .allGames:
                store.globalPresetID = saved
            case .thisGame:
                if let identity = context.gameIdentity { store.setPreset(saved, for: identity) }
            case .keepDefault:
                break
            }
            return saved
        }
        original = draft
        return savedID
    }
}
