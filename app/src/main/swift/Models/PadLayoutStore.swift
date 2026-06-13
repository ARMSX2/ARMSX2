// PadLayoutStore.swift — INI-backed virtual pad layout positions
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadGroupPosition {
    var x: CGFloat
    var y: CGFloat
    var scale: CGFloat
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

@Observable
final class PadLayoutStore: @unchecked Sendable {
    static let shared = PadLayoutStore()

    static let actionButtonIDs = ["cross", "circle", "square", "triangle"]
    static let perButtonIDs = ["triangle", "circle", "square", "cross", "up", "down", "left", "right"]
    static let groupIDs = ["dpad", "action", "l1", "l2", "r1", "r2", "lstick", "rstick", "select", "start"]

    var portrait: [String: PadGroupPosition] = [:]
    var landscape: [String: PadGroupPosition] = [:]

    // Per-button overrides — only populated when the user moves an individual button.
    var perButtonPortrait: [String: PadGroupPosition] = [:]
    var perButtonLandscape: [String: PadGroupPosition] = [:]

    // Group-level control visibility. Keys are group IDs; value `false` means hidden.
    // Absent key means visible (default). Stored globally, not per-orientation.
    var controlVisibility: [String: Bool] = [:]

    // MARK: - Default positions (derived from current hardcoded layout)

    // Portrait: relative to controller area (0.0-1.0)
    // U002: action x adjusted from 0.88/0.92 to 0.85/0.88 to prevent ○ button clipping
    static let defaultPortrait: [String: PadGroupPosition] = [
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
    static let defaultLandscape: [String: PadGroupPosition] = [
        "dpad":   PadGroupPosition(x: 0.14, y: 0.72, scale: 1.0),
        "action": PadGroupPosition(x: 0.84, y: 0.72, scale: 1.0),
        "l2":     PadGroupPosition(x: 0.14, y: 0.22, scale: 1.0),
        "l1":     PadGroupPosition(x: 0.14, y: 0.34, scale: 1.0),
        "r2":     PadGroupPosition(x: 0.86, y: 0.22, scale: 1.0),
        "r1":     PadGroupPosition(x: 0.86, y: 0.34, scale: 1.0),
        "select": PadGroupPosition(x: 0.43, y: 0.90, scale: 1.0),
        "start":  PadGroupPosition(x: 0.57, y: 0.90, scale: 1.0),
        "lstick": PadGroupPosition(x: 0.26, y: 0.86, scale: 1.0),
        "rstick": PadGroupPosition(x: 0.74, y: 0.86, scale: 1.0),
    ]

    private init() {
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
        let scaledOffsetX = offset.width * groupPos.scale
        let scaledOffsetY = offset.height * groupPos.scale
        return PadGroupPosition(
            x: groupPos.x + scaledOffsetX / areaW,
            y: groupPos.y + scaledOffsetY / areaH,
            scale: groupPos.scale
        )
    }

    func groupID(for perButtonID: String) -> String {
        if ["triangle", "circle", "square", "cross"].contains(perButtonID) { return "action" }
        if ["up", "down", "left", "right"].contains(perButtonID) { return "dpad" }
        return perButtonID
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

    func setControlVisible(_ id: String, visible: Bool) {
        if visible {
            let group = groupID(for: id)
            if id == group {
                controlVisibility.removeValue(forKey: id)
            } else if let groupVisible = controlVisibility[group], !groupVisible {
                controlVisibility[id] = true
            } else {
                controlVisibility.removeValue(forKey: id)
            }
        } else {
            controlVisibility[id] = false
        }
    }

    func resetControlVisibility() {
        controlVisibility.removeAll()
    }

    // MARK: - INI persistence

    func save() {
        for id in Self.groupIDs {
            if let pos = portrait[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_x", value: Float(pos.x))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_y", value: Float(pos.y))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_scale", value: Float(pos.scale))
            }
            if let pos = landscape[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_x", value: Float(pos.x))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_y", value: Float(pos.y))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_scale", value: Float(pos.scale))
            }
        }
        // Per-button positions: write override if present, sentinel -1 otherwise.
        for id in Self.perButtonIDs {
            if let pos = perButtonPortrait[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_x", value: Float(pos.x))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_y", value: Float(pos.y))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_scale", value: Float(pos.scale))
            } else {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_x", value: -1.0)
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_y", value: -1.0)
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonPortrait", key: "\(id)_scale", value: -1.0)
            }
            if let pos = perButtonLandscape[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_x", value: Float(pos.x))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_y", value: Float(pos.y))
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_scale", value: Float(pos.scale))
            } else {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_x", value: -1.0)
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_y", value: -1.0)
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/PerButtonLandscape", key: "\(id)_scale", value: -1.0)
            }
        }
        // Visibility: `0` = hidden, `1` = visible. Absent means visible (default).
        for id in Self.groupIDs {
            let isVisible = isControlVisible(id)
            ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, value: isVisible ? 1.0 : 0.0)
        }
        // Per-button visibility overrides (only action buttons in this pass).
        for id in Self.actionButtonIDs {
            if let explicit = controlVisibility[id] {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, value: explicit ? 1.0 : 0.0)
            } else {
                ARMSX2Bridge.setINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, value: -1.0)
            }
        }
    }

    func load() {
        for id in Self.groupIDs {
            // Portrait
            let px = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_x", defaultValue: -1)
            if px >= 0 {
                let py = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_y", defaultValue: 0.5)
                let ps = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Portrait", key: "\(id)_scale", defaultValue: 1.0)
                portrait[id] = PadGroupPosition(x: CGFloat(px), y: CGFloat(py), scale: CGFloat(ps))
            }
            // Landscape
            let lx = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_x", defaultValue: -1)
            if lx >= 0 {
                let ly = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_y", defaultValue: 0.5)
                let ls = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/Landscape", key: "\(id)_scale", defaultValue: 1.0)
                landscape[id] = PadGroupPosition(x: CGFloat(lx), y: CGFloat(ly), scale: CGFloat(ls))
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
            let ps = ARMSX2Bridge.getINIFloat(
                "ARMSX2iOS/PadLayout/PerButtonPortrait",
                key: "\(id)_scale",
                defaultValue: -1
            )
            if px >= 0 && py >= 0 && ps > 0 {
                perButtonPortrait[id] = PadGroupPosition(
                    x: CGFloat(px),
                    y: CGFloat(py),
                    scale: CGFloat(ps)
                )
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
            let ls = ARMSX2Bridge.getINIFloat(
                "ARMSX2iOS/PadLayout/PerButtonLandscape",
                key: "\(id)_scale",
                defaultValue: -1
            )
            if lx >= 0 && ly >= 0 && ls > 0 {
                perButtonLandscape[id] = PadGroupPosition(
                    x: CGFloat(lx),
                    y: CGFloat(ly),
                    scale: CGFloat(ls)
                )
            }
        }
        // Visibility: `0` = hidden, `1` = visible, absent = visible (default).
        for id in Self.groupIDs {
            let value = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, defaultValue: -1)
            if value >= 0 {
                controlVisibility[id] = (value > 0.5)
            }
        }
        // Per-button visibility overrides.
        for id in Self.actionButtonIDs {
            let value = ARMSX2Bridge.getINIFloat("ARMSX2iOS/PadLayout/ControlVisibility", key: id, defaultValue: -1)
            if value >= 0 {
                controlVisibility[id] = (value > 0.5)
            }
        }
    }

    func resetPortrait() {
        portrait = Self.defaultPortrait
    }

    func resetLandscape() {
        landscape = Self.defaultLandscape
    }

    func reset(isLandscape: Bool) {
        if isLandscape { resetLandscape() } else { resetPortrait() }
    }

    func resetPerButtonActionButtons(isLandscape: Bool) {
        if isLandscape {
            for id in ["triangle", "circle", "square", "cross"] {
                perButtonLandscape.removeValue(forKey: id)
            }
        } else {
            for id in ["triangle", "circle", "square", "cross"] {
                perButtonPortrait.removeValue(forKey: id)
            }
        }
    }

    func resetPerButtonDPad(isLandscape: Bool) {
        if isLandscape {
            for id in ["up", "down", "left", "right"] {
                perButtonLandscape.removeValue(forKey: id)
            }
        } else {
            for id in ["up", "down", "left", "right"] {
                perButtonPortrait.removeValue(forKey: id)
            }
        }
    }

    func resetAll() {
        portrait = Self.defaultPortrait
        landscape = Self.defaultLandscape
        perButtonPortrait.removeAll()
        perButtonLandscape.removeAll()
        // Note: controlVisibility is intentionally NOT reset here.
        // Use resetControlVisibility() for that.
    }
}
