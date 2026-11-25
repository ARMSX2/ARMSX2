// PadLayoutStore.swift — INI-backed virtual pad layout positions
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadGroupPosition: Codable, Equatable {
    var x: CGFloat
    var y: CGFloat
    // Per-axis sizing is the source of truth. `scale`/`hitScale` are uniform
    // accessors retained for backward compatibility: reading returns the X axis
    // (the representative value), writing sets BOTH axes (the linked behavior).
    var scaleX: CGFloat
    var scaleY: CGFloat
    var hitScaleX: CGFloat
    var hitScaleY: CGFloat
    var opacity: CGFloat = 1
    // Sticks only: nil follows the global Dynamic Thumbsticks setting, and a nil zone is the stick's half.
    var floating: Bool? = nil
    var zone: CGRect? = nil

    enum CodingKeys: String, CodingKey {
        case x, y, scaleX, scaleY, hitScaleX, hitScaleY, opacity, floating, zone
    }

    var scale: CGFloat {
        get { scaleX }
        set { scaleX = newValue; scaleY = newValue }
    }
    var hitScale: CGFloat {
        get { hitScaleX }
        set { hitScaleX = newValue; hitScaleY = newValue }
    }

    /// Uniform initializer — all four axes take the same scale (and hit scale).
    /// Existing call sites keep using this and behave exactly as before.
    init(x: CGFloat, y: CGFloat, scale: CGFloat, hitScale: CGFloat? = nil) {
        self.x = x
        self.y = y
        self.scaleX = scale
        self.scaleY = scale
        let h = hitScale ?? scale
        self.hitScaleX = h
        self.hitScaleY = h
    }

    /// Per-axis initializer for independently sized controls.
    init(x: CGFloat, y: CGFloat, scaleX: CGFloat, scaleY: CGFloat, hitScaleX: CGFloat, hitScaleY: CGFloat) {
        self.x = x
        self.y = y
        self.scaleX = scaleX
        self.scaleY = scaleY
        self.hitScaleX = hitScaleX
        self.hitScaleY = hitScaleY
    }

    // Files written before per-piece opacity and floating sticks lack those keys.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        x = try c.decode(CGFloat.self, forKey: .x)
        y = try c.decode(CGFloat.self, forKey: .y)
        scaleX = try c.decode(CGFloat.self, forKey: .scaleX)
        scaleY = try c.decode(CGFloat.self, forKey: .scaleY)
        hitScaleX = try c.decode(CGFloat.self, forKey: .hitScaleX)
        hitScaleY = try c.decode(CGFloat.self, forKey: .hitScaleY)
        opacity = PadLayoutMetrics.clampedOpacity((try? c.decodeIfPresent(CGFloat.self, forKey: .opacity)) ?? 1)
        floating = try? c.decodeIfPresent(Bool.self, forKey: .floating)
        zone = (try? c.decodeIfPresent(CGRect.self, forKey: .zone)).flatMap(PadLayoutMetrics.clampedZone)
    }
}

enum PadSizeKind {
    case visible
    case hit
}

enum PadSizeAxis {
    case x
    case y
}

enum PadLayoutMetrics {
    static let minimumTouchLength: CGFloat = 55
    static let minControlScale: CGFloat = 0.5
    static let maxControlScale: CGFloat = 7.0
    static let opacityRange: ClosedRange<CGFloat> = 0.1...1

    static func visibleLength(baseLength: CGFloat, visibleScale: CGFloat) -> CGFloat {
        baseLength * visibleScale
    }

    static func touchLength(baseLength: CGFloat, hitScale: CGFloat) -> CGFloat {
        max(baseLength, minimumTouchLength) * hitScale
    }

    static func clampedScale(_ scale: CGFloat) -> CGFloat {
        guard scale.isFinite else { return 1.0 }
        return min(max(scale, minControlScale), maxControlScale)
    }

    static func clampedOpacity(_ value: CGFloat) -> CGFloat {
        guard value.isFinite else { return 1 }
        return min(max(value, opacityRange.lowerBound), opacityRange.upperBound)
    }

    /// The share of the pad a floating stick takes when its layout sets no zone.
    static func defaultZone(isLeft: Bool) -> CGRect {
        CGRect(x: isLeft ? 0 : 0.5, y: 0, width: 0.5, height: 1)
    }

    static let minimumZoneShare: CGFloat = 0.25

    /// A zone kept inside the pad; nil (the half) when it is not finite or too small on an axis.
    static func clampedZone(_ zone: CGRect) -> CGRect? {
        guard [zone.origin.x, zone.origin.y, zone.width, zone.height].allSatisfy(\.isFinite) else { return nil }
        let z = zone.standardized.intersection(CGRect(x: 0, y: 0, width: 1, height: 1))
        let floor = minimumZoneShare - 0.001
        return !z.isNull && z.width >= floor && z.height >= floor ? z : nil
    }
}

// MARK: - Shared per-button offset constants
// These must match the exact offset math used in DPadView and ActionButtonsView.
enum VirtualPadButtonOffset {
    static let dpadPortraitSize: CGFloat = 100
    static let dpadLandscapeSize: CGFloat = 110
    static let actionButtonSize: CGFloat = 42

    static func dpadButtonWidth(isLandscape: Bool) -> CGFloat {
        (isLandscape ? dpadLandscapeSize : dpadPortraitSize) * 0.42
    }

    static func dpadOffset(isLandscape: Bool) -> CGFloat {
        (isLandscape ? dpadLandscapeSize : dpadPortraitSize) * 0.29
    }

    static let actionOffset: CGFloat = actionButtonSize * 1.1

    static func offset(for buttonID: String, isLandscape: Bool) -> CGSize {
        let dpadOff = dpadOffset(isLandscape: isLandscape)
        let actionOff = actionOffset
        switch buttonID {
        case "up":       return CGSize(width: 0, height: -dpadOff)
        case "down":     return CGSize(width: 0, height: dpadOff)
        case "left":     return CGSize(width: -dpadOff, height: 0)
        case "right":    return CGSize(width: dpadOff, height: 0)
        case "triangle": return CGSize(width: 0, height: -actionOff)
        case "cross":    return CGSize(width: 0, height: actionOff)
        case "square":   return CGSize(width: -actionOff, height: 0)
        case "circle":   return CGSize(width: actionOff, height: 0)
        default:         return .zero
        }
    }
}

@MainActor
@Observable
final class PadLayoutStore {
    static let shared = PadLayoutStore()

    nonisolated static let perButtonIDs = ["triangle", "circle", "square", "cross", "up", "down", "left", "right"]
    nonisolated static let clusterMembers = ["dpad": ["up", "down", "left", "right"], "action": ["triangle", "circle", "square", "cross"]]
    nonisolated static let groupIDs = ["dpad", "action", "l1", "l2", "r1", "r2", "lstick", "rstick", "select", "start"]

    var portrait: [String: PadGroupPosition] = [:]
    var landscape: [String: PadGroupPosition] = [:]

    // Per-button overrides — only populated when the user moves an individual button.
    var perButtonPortrait: [String: PadGroupPosition] = [:]
    var perButtonLandscape: [String: PadGroupPosition] = [:]
    var book: [String: PadGroupPosition]?
    var perButtonBook: [String: PadGroupPosition]?

    // Group-level control visibility. Keys are group IDs; value `false` means hidden.
    // Absent key means visible (default). Stored globally, not per-orientation.
    var controlVisibility: [String: Bool] = [:]

    // MARK: - Default positions (derived from current hardcoded layout)

    // Portrait: relative to controller area (0.0-1.0)
    // U002: action x adjusted from 0.88/0.92 to 0.85/0.88 to prevent ○ button clipping
    nonisolated static let defaultPortrait: [String: PadGroupPosition] = [
        "l2":     PadGroupPosition(x: 0.16, y: 0.06, scale: 1.0),
        "l1":     PadGroupPosition(x: 0.16, y: 0.14, scale: 1.0),
        "r2":     PadGroupPosition(x: 0.84, y: 0.06, scale: 1.0),
        "r1":     PadGroupPosition(x: 0.84, y: 0.14, scale: 1.0),
        "select": PadGroupPosition(x: 0.43, y: 0.20, scale: 1.0),
        "start":  PadGroupPosition(x: 0.57, y: 0.20, scale: 1.0),
        "dpad":   PadGroupPosition(x: 0.16, y: 0.48, scale: 1.0),
        "action": PadGroupPosition(x: 0.82, y: 0.44, scale: 1.0),
        "lstick": PadGroupPosition(x: 0.28, y: 0.78, scale: 1.0),
        "rstick": PadGroupPosition(x: 0.72, y: 0.78, scale: 1.0),
    ]

    // Landscape: relative to full screen — positions kept well inside safe area
    // to avoid clipping on notch/Dynamic Island devices
    nonisolated static let defaultLandscape: [String: PadGroupPosition] = [
        "dpad":   PadGroupPosition(x: 0.14, y: 0.72, scale: 1.0),
        "action": PadGroupPosition(x: 0.84, y: 0.72, scale: 1.0),
        "l2":     PadGroupPosition(x: 0.14, y: 0.22, scale: 1.0),
        "l1":     PadGroupPosition(x: 0.14, y: 0.34, scale: 1.0),
        "r2":     PadGroupPosition(x: 0.86, y: 0.22, scale: 1.0),
        "r1":     PadGroupPosition(x: 0.86, y: 0.34, scale: 1.0),
        "select": PadGroupPosition(x: 0.43, y: 0.90, scale: 1.0),
        "start":  PadGroupPosition(x: 0.57, y: 0.90, scale: 1.0),
        "lstick": PadGroupPosition(x: 0.26, y: 0.86, scale: 1.0),
        "rstick": PadGroupPosition(x: 0.68, y: 0.86, scale: 1.0),
    ]

    init() {
        portrait = Self.defaultPortrait
        landscape = Self.defaultLandscape
        load()
    }

    func position(for id: String, landscape isLandscape: Bool) -> PadGroupPosition {
        let dict = isLandscape ? landscape : portrait
        let defaults = isLandscape ? Self.defaultLandscape : Self.defaultPortrait
        return dict[id] ?? defaults[id] ?? PadGroupPosition(x: 0.5, y: 0.5, scale: 1.0)
    }

    // MARK: - Per-button position lookup

    /// Returns the position for an individual button.
    /// If a per-button override exists, it is returned.
    /// Otherwise, the default is computed from the legacy group position using
    /// the same offset math as the current grouped layout.
    func perButtonPosition(for id: String, landscape: Bool, areaW: CGFloat, areaH: CGFloat) -> PadGroupPosition {
        let dict = landscape ? perButtonLandscape : perButtonPortrait
        if let pos = dict[id] {
            return pos
        }
        let groupID = (id == "triangle" || id == "circle" || id == "square" || id == "cross") ? "action" : "dpad"
        let groupPos = position(for: groupID, landscape: landscape)
        return defaultPerButtonPosition(for: id, groupPos: groupPos, isLandscape: landscape, areaW: areaW, areaH: areaH)
    }

    private func defaultPerButtonPosition(for id: String, groupPos: PadGroupPosition, isLandscape: Bool, areaW: CGFloat, areaH: CGFloat) -> PadGroupPosition {
        let offset = VirtualPadButtonOffset.offset(for: id, isLandscape: isLandscape)
        let scaledOffsetX = offset.width * groupPos.scaleX
        let scaledOffsetY = offset.height * groupPos.scaleY
        var p = PadGroupPosition(
            x: groupPos.x + scaledOffsetX / areaW,
            y: groupPos.y + scaledOffsetY / areaH,
            scaleX: groupPos.scaleX,
            scaleY: groupPos.scaleY,
            hitScaleX: groupPos.hitScaleX,
            hitScaleY: groupPos.hitScaleY
        )
        p.opacity = groupPos.opacity
        return p
    }

    nonisolated static func groupID(for perButtonID: String) -> String {
        if ["triangle", "circle", "square", "cross"].contains(perButtonID) { return "action" }
        if ["up", "down", "left", "right"].contains(perButtonID) { return "dpad" }
        return perButtonID
    }

    func groupID(for perButtonID: String) -> String {
        Self.groupID(for: perButtonID)
    }

    // MARK: - Visibility helpers

    /// Returns whether a control or button is visible.
    /// If an explicit per-button key exists, it wins.
    /// Otherwise, fall back to the group visibility (default: visible).
    func isControlVisible(_ id: String) -> Bool {
        if let explicit = controlVisibility[id] {
            return explicit
        }
        let group = groupID(for: id)
        return controlVisibility[group] ?? true
    }

    func resetControlVisibility() {
        controlVisibility.removeAll()
    }

    // MARK: - INI persistence

    /// Writes the six sizing keys for a control. Legacy `*_scale`/`*_hitScale`
    /// carry the X axis (representative) so older readers still get a sane value;
    /// the four `*_scaleX/Y`/`*_hitScaleX/Y` keys carry the full per-axis state.
    private func writeSizeKeys(_ pos: PadGroupPosition, section: String, id: String) {
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_scale", value: Float(pos.scaleX))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_hitScale", value: Float(pos.hitScaleX))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_scaleX", value: Float(pos.scaleX))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_scaleY", value: Float(pos.scaleY))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_hitScaleX", value: Float(pos.hitScaleX))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_hitScaleY", value: Float(pos.hitScaleY))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_opacity", value: Float(pos.opacity))
        guard isStick(id) else { return }
        let mode: Float = pos.floating.map { $0 ? 1 : 0 } ?? -1
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_floating", value: mode)
        let zone: [CGFloat] = pos.zone.map { [$0.minX, $0.minY, $0.width, $0.height] } ?? [-1, -1, -1, -1]
        for (key, value) in zip(Self.zoneKeys, zone) {
            ARMSX2Bridge.setINIFloat(section, key: "\(id)\(key)", value: Float(value))
        }
    }

    private static let zoneKeys = ["_zoneX", "_zoneY", "_zoneW", "_zoneH"]

    private func writeSizeKeysSentinel(section: String, id: String) {
        for suffix in ["scale", "hitScale", "scaleX", "scaleY", "hitScaleX", "hitScaleY", "opacity"] {
            ARMSX2Bridge.setINIFloat(section, key: "\(id)_\(suffix)", value: -1.0)
        }
        for key in isStick(id) ? ["_floating"] + Self.zoneKeys : [] {
            ARMSX2Bridge.setINIFloat(section, key: "\(id)\(key)", value: -1.0)
        }
    }

    func save() {
        for id in Self.groupIDs {
            writePosition(portrait[id], section: "ARMSX2iOS/PadLayout/Portrait", id: id)
            writePosition(landscape[id], section: "ARMSX2iOS/PadLayout/Landscape", id: id)
        }
        // Per-button positions: write override if present, sentinel -1 otherwise.
        for id in Self.perButtonIDs {
            if let pos = perButtonPortrait[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_x", value: Float(pos.x))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_y", value: Float(pos.y))
                writeSizeKeys(pos, section: "ARMSX2iOS/PadLayout/PerButtonPortrait", id: id)
            } else {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_x", value: -1.0)
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_y", value: -1.0)
                writeSizeKeysSentinel(section: "ARMSX2iOS/PadLayout/PerButtonPortrait", id: id)
            }
            if let pos = perButtonLandscape[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_x", value: Float(pos.x))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_y", value: Float(pos.y))
                writeSizeKeys(pos, section: "ARMSX2iOS/PadLayout/PerButtonLandscape", id: id)
            } else {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_x", value: -1.0)
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_y", value: -1.0)
                writeSizeKeysSentinel(section: "ARMSX2iOS/PadLayout/PerButtonLandscape", id: id)
            }
        }
        writePositions(book, section: "ARMSX2iOS/PadLayout/Book", ids: Self.groupIDs)
        writePositions(perButtonBook, section: "ARMSX2iOS/PadLayout/PerButtonBook", ids: Self.perButtonIDs)
        // Visibility: `0` = hidden, `1` = visible. Absent means visible (default).
        for id in Self.groupIDs {
            let isVisible = isControlVisible(id)
            ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, value: isVisible ? 1.0 : 0.0)
        }
        for id in Self.perButtonIDs {
            if let explicit = controlVisibility[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, value: explicit ? 1.0 : 0.0)
            } else {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, value: -1.0)
            }
        }
    }

    /// Resolves the four sizing axes for a control using the fallback chain:
    ///   scaleX    = *_scaleX    ?? *_scale    ?? 1.0
    ///   scaleY    = *_scaleY    ?? *_scale    ?? 1.0
    ///   hitScaleX = *_hitScaleX ?? *_hitScale ?? scaleX
    ///   hitScaleY = *_hitScaleY ?? *_hitScale ?? scaleY
    /// Scales are clamped to a positive range, so a non-positive read means absent.
    private func readSizeAxes(section: String, id: String) -> (scaleX: CGFloat, scaleY: CGFloat, hitScaleX: CGFloat, hitScaleY: CGFloat) {
        let legacyScale = ARMSX2Bridge.getINIFloat(section, key: "\(id)_scale", defaultValue: -1)
        let legacyHit = ARMSX2Bridge.getINIFloat(section, key: "\(id)_hitScale", defaultValue: -1)
        let sx = ARMSX2Bridge.getINIFloat(section, key: "\(id)_scaleX", defaultValue: -1)
        let sy = ARMSX2Bridge.getINIFloat(section, key: "\(id)_scaleY", defaultValue: -1)
        let hx = ARMSX2Bridge.getINIFloat(section, key: "\(id)_hitScaleX", defaultValue: -1)
        let hy = ARMSX2Bridge.getINIFloat(section, key: "\(id)_hitScaleY", defaultValue: -1)

        let baseScale = PadLayoutMetrics.clampedScale(CGFloat(legacyScale > 0 ? legacyScale : 1.0))
        let scaleX = PadLayoutMetrics.clampedScale(sx > 0 ? CGFloat(sx) : baseScale)
        let scaleY = PadLayoutMetrics.clampedScale(sy > 0 ? CGFloat(sy) : baseScale)
        let legacyHitScale = legacyHit > 0 ? PadLayoutMetrics.clampedScale(CGFloat(legacyHit)) : nil
        let hitScaleX = PadLayoutMetrics.clampedScale(hx > 0 ? CGFloat(hx) : (legacyHitScale ?? scaleX))
        let hitScaleY = PadLayoutMetrics.clampedScale(hy > 0 ? CGFloat(hy) : (legacyHitScale ?? scaleY))
        return (scaleX, scaleY, hitScaleX, hitScaleY)
    }

    private func readOpacity(section: String, id: String) -> CGFloat {
        let value = ARMSX2Bridge.getINIFloat(section, key: "\(id)_opacity", defaultValue: -1)
        return value < 0 ? 1 : PadLayoutMetrics.clampedOpacity(CGFloat(value))
    }

    private func readStickMode(_ pos: inout PadGroupPosition, section: String, id: String) {
        guard isStick(id) else { return }
        let floating = ARMSX2Bridge.getINIFloat(section, key: "\(id)_floating", defaultValue: -1)
        pos.floating = floating < 0 ? nil : floating > 0.5
        let z = Self.zoneKeys.map { CGFloat(ARMSX2Bridge.getINIFloat(section, key: "\(id)\($0)", defaultValue: -1)) }
        pos.zone = z.contains { $0 < 0 } ? nil : PadLayoutMetrics.clampedZone(CGRect(x: z[0], y: z[1], width: z[2], height: z[3]))
    }

    private func writePosition(_ pos: PadGroupPosition?, section: String, id: String) {
        guard let pos else {
            ARMSX2Bridge.setINIFloat(section, key: "\(id)_x", value: -1.0)
            ARMSX2Bridge.setINIFloat(section, key: "\(id)_y", value: -1.0)
            writeSizeKeysSentinel(section: section, id: id)
            return
        }
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_x", value: Float(pos.x))
        ARMSX2Bridge.setINIFloat(section, key: "\(id)_y", value: Float(pos.y))
        writeSizeKeys(pos, section: section, id: id)
    }

    // An empty book layout differs from none, which plays landscape's, so the section records which it holds.
    private func writePositions(_ positions: [String: PadGroupPosition]?, section: String, ids: [String]) {
        ARMSX2Bridge.setINIFloat(section, key: "stored", value: positions == nil ? -1 : 1)
        for id in ids { writePosition(positions?[id], section: section, id: id) }
    }

    private func readPositions(section: String, ids: [String]) -> [String: PadGroupPosition]? {
        var positions: [String: PadGroupPosition] = [:]
        for id in ids {
            let x = ARMSX2Bridge.getINIFloat(section, key: "\(id)_x", defaultValue: -1)
            let y = ARMSX2Bridge.getINIFloat(section, key: "\(id)_y", defaultValue: -1)
            guard x >= 0 && y >= 0 else { continue }
            let axes = readSizeAxes(section: section, id: id)
            var pos = PadGroupPosition(x: CGFloat(x), y: CGFloat(y), scaleX: axes.scaleX, scaleY: axes.scaleY, hitScaleX: axes.hitScaleX, hitScaleY: axes.hitScaleY)
            pos.opacity = readOpacity(section: section, id: id)
            readStickMode(&pos, section: section, id: id)
            positions[id] = pos
        }
        let stored = ARMSX2Bridge.getINIFloat(section, key: "stored", defaultValue: -1) > 0
        return positions.isEmpty && !stored ? nil : positions
    }

    func load() {
        for id in Self.groupIDs {
            // Portrait
            let px = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_x", defaultValue: -1)
            if px >= 0 {
                let py = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_y", defaultValue: 0.5)
                let axes = readSizeAxes(section: "ARMSX2iOS/PadLayout/Portrait", id: id)
                var pos = PadGroupPosition(x: CGFloat(px), y: CGFloat(py), scaleX: axes.scaleX, scaleY: axes.scaleY, hitScaleX: axes.hitScaleX, hitScaleY: axes.hitScaleY)
                pos.opacity = readOpacity(section: "ARMSX2iOS/PadLayout/Portrait", id: id)
                readStickMode(&pos, section: "ARMSX2iOS/PadLayout/Portrait", id: id)
                portrait[id] = pos
            }
            // Landscape
            let lx = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_x", defaultValue: -1)
            if lx >= 0 {
                let ly = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_y", defaultValue: 0.5)
                let axes = readSizeAxes(section: "ARMSX2iOS/PadLayout/Landscape", id: id)
                var pos = PadGroupPosition(x: CGFloat(lx), y: CGFloat(ly), scaleX: axes.scaleX, scaleY: axes.scaleY, hitScaleX: axes.hitScaleX, hitScaleY: axes.hitScaleY)
                pos.opacity = readOpacity(section: "ARMSX2iOS/PadLayout/Landscape", id: id)
                readStickMode(&pos, section: "ARMSX2iOS/PadLayout/Landscape", id: id)
                landscape[id] = pos
            }
        }
        for id in Self.perButtonIDs {
            // Portrait
            let px = ARMSX2Bridge.getINIFloat(
                "ARMSX2iOS/PadLayout/PerButtonPortrait",
                key: "\(id)_x",
                defaultValue: -1
            )
            let py = ARMSX2Bridge.getINIFloat(
                "ARMSX2iOS/PadLayout/PerButtonPortrait",
                key: "\(id)_y",
                defaultValue: -1
            )
            if px >= 0 && py >= 0 {
                let axes = readSizeAxes(section: "ARMSX2iOS/PadLayout/PerButtonPortrait", id: id)
                var pos = PadGroupPosition(
                    x: CGFloat(px),
                    y: CGFloat(py),
                    scaleX: axes.scaleX,
                    scaleY: axes.scaleY,
                    hitScaleX: axes.hitScaleX,
                    hitScaleY: axes.hitScaleY
                )
                pos.opacity = readOpacity(section: "ARMSX2iOS/PadLayout/PerButtonPortrait", id: id)
                perButtonPortrait[id] = pos
            }
            // Landscape
            let lx = ARMSX2Bridge.getINIFloat(
                "ARMSX2iOS/PadLayout/PerButtonLandscape",
                key: "\(id)_x",
                defaultValue: -1
            )
            let ly = ARMSX2Bridge.getINIFloat(
                "ARMSX2iOS/PadLayout/PerButtonLandscape",
                key: "\(id)_y",
                defaultValue: -1
            )
            if lx >= 0 && ly >= 0 {
                let axes = readSizeAxes(section: "ARMSX2iOS/PadLayout/PerButtonLandscape", id: id)
                var pos = PadGroupPosition(
                    x: CGFloat(lx),
                    y: CGFloat(ly),
                    scaleX: axes.scaleX,
                    scaleY: axes.scaleY,
                    hitScaleX: axes.hitScaleX,
                    hitScaleY: axes.hitScaleY
                )
                pos.opacity = readOpacity(section: "ARMSX2iOS/PadLayout/PerButtonLandscape", id: id)
                perButtonLandscape[id] = pos
            }
        }
        book = readPositions(section: "ARMSX2iOS/PadLayout/Book", ids: Self.groupIDs)
        perButtonBook = readPositions(section: "ARMSX2iOS/PadLayout/PerButtonBook", ids: Self.perButtonIDs)
        // Visibility: `0` = hidden, `1` = visible, absent = visible (default).
        for id in Self.groupIDs {
            let value = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, defaultValue: -1)
            if value >= 0 {
                controlVisibility[id] = (value > 0.5)
            }
        }
        // Per-button visibility overrides.
        for id in Self.perButtonIDs {
            let value = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, defaultValue: -1)
            if value >= 0 {
                controlVisibility[id] = (value > 0.5)
            }
        }
        // A hand-edited INI can hold nan or inf, which the editor's Int() conversions trap on.
        apply(snapshot: PadLayoutImportExport.clampedSnapshot(snapshot()))
    }

    func resetAll() {
        portrait = Self.defaultPortrait
        landscape = Self.defaultLandscape
        perButtonPortrait.removeAll()
        perButtonLandscape.removeAll()
        book = nil
        perButtonBook = nil
        // Note: controlVisibility is intentionally NOT reset here.
        // Use resetControlVisibility() for that.
    }

    func snapshot() -> PadLayoutSnapshot {
        PadLayoutSnapshot(
            portrait: portrait,
            landscape: landscape,
            perButtonPortrait: perButtonPortrait,
            perButtonLandscape: perButtonLandscape,
            controlVisibility: controlVisibility,
            book: book,
            perButtonBook: perButtonBook
        )
    }

    func apply(snapshot: PadLayoutSnapshot) {
        portrait = snapshot.portrait
        landscape = snapshot.landscape
        perButtonPortrait = snapshot.perButtonPortrait
        perButtonLandscape = snapshot.perButtonLandscape
        controlVisibility = snapshot.controlVisibility
        book = snapshot.book
        perButtonBook = snapshot.perButtonBook
    }
}
