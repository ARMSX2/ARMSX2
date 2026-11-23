// SkinManifestV2.swift — Advanced skin manifest v2 model
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import CoreGraphics

// MARK: - Top-level manifest

/// Codable mirror of the skin manifest v2 JSON. Every field is optional so
/// ``SkinManifestValidator``, not the decoder, reports missing fields by JSON
/// path, and `representations` stays raw so unknown keys reach it too.
struct SkinManifestV2: Codable, Equatable {
    var formatVersion: Int?
    var name: String?
    var identifier: String?
    var author: String?
    var version: String?
    var gameTypeIdentifier: String?
    var supportedGameTypeIdentifiers: [String]?
    var preview: String?
    var debug: Bool?
    var representations: [String: [String: SkinOrientationSet]]?

    static let supportedFormatVersions: Set<Int> = [2]

    static let ps2GameTypeIdentifier = "com.armsx2.game.ps2"

    static func decode(from data: Data) throws -> SkinManifestV2 {
        try JSONDecoder().decode(SkinManifestV2.self, from: data)
    }
}

// MARK: - Representations

/// A pair of orientations for one device + screen class.
struct SkinOrientationSet: Codable, Equatable {
    var portrait: SkinOrientationRepresentation?
    var landscape: SkinOrientationRepresentation?
}

struct SkinOrientationRepresentation: Codable, Equatable {
    /// Point-based coordinate space that every frame is measured against.
    var mappingSize: SkinSize?
    /// Background art for this orientation.
    var assets: SkinAssets?
    /// Whether this orientation supports the opacity/translucency feature.
    var translucent: Bool?
    /// Reserved: parsed but ignored in manifest v2.0.
    var screen: SkinFrame?
    var controls: [SkinControl]?
    /// Touch-target inflation for every control that does not set its own.
    var extendedEdges: SkinExtendedEdges?
}

/// One scalable asset (typically a vector PDF) or three raster sizes.
struct SkinAssets: Codable, Equatable {
    var resizable: String?
    var small: String?
    var medium: String?
    var large: String?

    var referencedPaths: [String] {
        var paths: [String] = []
        for candidate in [resizable, small, medium, large] {
            if let path = candidate?.trimmingCharacters(in: .whitespacesAndNewlines), !path.isEmpty {
                paths.append(path)
            }
        }
        return paths
    }
}

struct SkinFrame: Codable, Equatable {
    var x: Double?
    var y: Double?
    var width: Double?
    var height: Double?
}

struct SkinSize: Codable, Equatable {
    var width: Double?
    var height: Double?
}

/// Grows where a control can be touched without changing its visual frame.
struct SkinExtendedEdges: Codable, Equatable {
    var top: Double?
    var bottom: Double?
    var left: Double?
    var right: Double?
}

/// Moving knob art for a thumbstick control.
struct SkinThumbstick: Codable, Equatable {
    var name: String?
    var width: Double?
    var height: Double?
}

/// Reserved animation descriptor (parsed, inert in manifest v2.0).
struct SkinAnimation: Codable, Equatable {
    var type: String?
    var begin: SkinFrame?
    var end: SkinFrame?
}

// MARK: - Control

struct SkinControl: Codable, Equatable {
    /// Stable, representation-unique identifier.
    var id: String?
    var kind: String?
    var frame: SkinFrame?
    var extendedEdges: SkinExtendedEdges?
    var asset: SkinControlAsset?
    var directionAssets: SkinDirectionAssets?
    var inputs: SkinInputs?
    /// Optional L3/R3 click input for thumbstick controls.
    var clickInput: String?
    var thumbstick: SkinThumbstick?
    /// Render order; higher draws above lower.
    var zIndex: Double?
    var visibility: String?
    /// haptic, sound and animation are reserved: parsed, inert in manifest v2.0.
    var haptic: String?
    var sound: String?
    var animation: SkinAnimation?
    /// Reserved function-button action, never executed yet.
    var action: String?

    var resolvedKind: SkinControlKind { SkinControlKind.from(kind ?? "") }
    var resolvedVisibility: SkinVisibility { SkinVisibility.from(visibility ?? "") }
}

struct SkinControlAsset: Codable, Equatable {
    var normal: String?
    var pressed: String?
    /// Reserved: parsed, inert in manifest v2.0.
    var selected: String?

    var referencedPaths: [String] {
        var paths: [String] = []
        for candidate in [normal, pressed, selected] {
            if let path = candidate?.trimmingCharacters(in: .whitespacesAndNewlines), !path.isEmpty {
                paths.append(path)
            }
        }
        return paths
    }
}

struct SkinDirectionalAsset: Codable, Equatable {
    var normal: String?
    var pressed: String?

    var referencedPaths: [String] {
        [normal, pressed].compactMap { path in
            guard let path else { return nil }
            let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }
    }
}

/// Per-direction D-pad art; an omitted direction falls back to the whole-D-pad art.
struct SkinDirectionAssets: Codable, Equatable {
    var up: SkinDirectionalAsset?
    var down: SkinDirectionalAsset?
    var left: SkinDirectionalAsset?
    var right: SkinDirectionalAsset?

    var referencedPaths: [String] {
        [up, down, left, right].flatMap { $0?.referencedPaths ?? [] }
    }
}

/// Buttons use a list of input IDs, composite D-pad and thumbstick controls a
/// map of slot to input ID, and some styles a single string. An unrecognized
/// shape decodes to ``unknown`` for the validator to report.
enum SkinInputs: Codable, Equatable {
    case list([String])
    case map([String: String])
    case single(String)
    case unknown

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let string = try? container.decode(String.self) {
            self = .single(string)
        } else if let array = try? container.decode([String].self) {
            self = .list(array)
        } else if let dictionary = try? container.decode([String: String].self) {
            self = .map(dictionary)
        } else {
            self = .unknown
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .list(let array):      try container.encode(array)
        case .map(let dictionary):  try container.encode(dictionary)
        case .single(let string):   try container.encode(string)
        case .unknown:              try container.encodeNil()
        }
    }

    var referencedInputIDs: [String] {
        switch self {
        case .list(let array):     return array
        case .single(let string):  return [string]
        case .map(let dictionary): return Array(dictionary.values)
        case .unknown:             return []
        }
    }
}

// MARK: - Enums

enum SkinControlKind: Equatable {
    case button
    case dpad
    case thumbstick
    case function
    case unknown

    var isKnown: Bool { self != .unknown }

    static func from(_ raw: String) -> SkinControlKind {
        switch raw.lowercased() {
        case "button":                          return .button
        case "dpad", "d-pad":                   return .dpad
        case "thumbstick", "stick", "analog":   return .thumbstick
        case "function", "fn":                  return .function
        default:                                return .unknown
        }
    }
}

enum SkinVisibility: Equatable {
    case always
    case hidden
    case autoHideWithPad
    case unknown

    static func from(_ raw: String) -> SkinVisibility {
        switch raw {
        case "", "always":          return .always
        case "hidden":              return .hidden
        case "autoHideWithPad":     return .autoHideWithPad
        default:                    return .unknown
        }
    }
}

enum SkinDevice: Equatable {
    case iphone
    case ipad
    case unknown

    static func from(_ raw: String) -> SkinDevice {
        switch raw.lowercased() {
        case "iphone":  return .iphone
        case "ipad":    return .ipad
        default:        return .unknown
        }
    }
}

enum SkinScreenClass: Equatable {
    case standard
    case edgeToEdge
    case splitView
    case unknown

    static func from(_ raw: String) -> SkinScreenClass {
        switch raw {
        case "standard":    return .standard
        case "edgeToEdge":  return .edgeToEdge
        case "splitView":   return .splitView
        default:            return .unknown
        }
    }
}

enum SkinOrientation: Equatable {
    case portrait
    case landscape
}

/// Every PS2 input identifier a manifest control may reference.
enum SkinInputID {
    static let known: Set<String> = [
        "up", "down", "left", "right",
        "cross", "circle", "square", "triangle",
        "l1", "l2", "l3", "r1", "r2", "r3",
        "start", "select",
        "leftAnalogUp", "leftAnalogDown", "leftAnalogLeft", "leftAnalogRight",
        "rightAnalogUp", "rightAnalogDown", "rightAnalogLeft", "rightAnalogRight"
    ]

    static func isKnown(_ id: String) -> Bool { known.contains(id) }
}

/// Function-button actions the validator accepts but nothing runs yet. Leaves out
/// `screenshot` (no safe bridge action yet) and renderer actions; one per button.
enum SkinAction: String, Equatable, CaseIterable {
    case quickSave
    case quickLoad
    case fastForward
    case toggleFastForward
    case pauseResume
    case quit
    case restart
    case saveStates
    case cheats
    case settings
    case toggleControls
    case toggleHaptics
    case toggleOverlay
    case mute
    case retroAchievements
    case openSkinPicker
    case openLayoutEditor

    static let knownIDs: Set<String> = Set(allCases.map { $0.rawValue })

    static func isKnown(_ id: String) -> Bool { knownIDs.contains(id) }
}

// MARK: - Path-safety helper

enum SkinAssetPath {
    /// True when `path` is a safe relative reference to a bundled asset.
    static func isSafeRelative(_ path: String) -> Bool {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return false }
        if trimmed.hasPrefix("/") || trimmed.hasPrefix("\\") { return false }
        if trimmed.contains(":") { return false }
        let components = trimmed.split(separator: "/", omittingEmptySubsequences: true)
        for component in components {
            let segment = String(component)
            if segment == ".." || segment == "." { return false }
            if segment.contains("\\") { return false }
            for scalar in segment.unicodeScalars where scalar.value < 0x20 || scalar.value == 0x7F {
                return false
            }
        }
        return true
    }
}

// MARK: - Normalized conversions

/// A frame expressed as fractions (0...1) of a mapping size.
struct NormalizedFrame: Equatable {
    var x: CGFloat
    var y: CGFloat
    var width: CGFloat
    var height: CGFloat
}

/// Per-direction insets expressed as fractions of a mapping size.
struct NormalizedInsets: Equatable {
    var top: CGFloat
    var bottom: CGFloat
    var left: CGFloat
    var right: CGFloat
}

extension SkinFrame {
    /// Nil when the mapping size is not positive or a component is missing or non-finite.
    func normalized(against mappingSize: SkinSize) -> NormalizedFrame? {
        guard let mappingWidth = mappingSize.width,
              let mappingHeight = mappingSize.height,
              mappingWidth > 0, mappingHeight > 0 else { return nil }
        guard let x = self.x, let y = self.y,
              let width = self.width, let height = self.height,
              x.isFinite, y.isFinite, width.isFinite, height.isFinite else { return nil }
        return NormalizedFrame(
            x: CGFloat(x / mappingWidth),
            y: CGFloat(y / mappingHeight),
            width: CGFloat(width / mappingWidth),
            height: CGFloat(height / mappingHeight)
        )
    }
}

extension SkinExtendedEdges {
    /// Missing edges count as zero; nil when the mapping size is not positive.
    func normalizedInsets(against mappingSize: SkinSize) -> NormalizedInsets? {
        guard let mappingWidth = mappingSize.width,
              let mappingHeight = mappingSize.height,
              mappingWidth > 0, mappingHeight > 0 else { return nil }
        let top = self.top ?? 0
        let bottom = self.bottom ?? 0
        let left = self.left ?? 0
        let right = self.right ?? 0
        guard [top, bottom, left, right].allSatisfy({ $0.isFinite }) else { return nil }
        return NormalizedInsets(
            top: CGFloat(top / mappingHeight),
            bottom: CGFloat(bottom / mappingHeight),
            left: CGFloat(left / mappingWidth),
            right: CGFloat(right / mappingWidth)
        )
    }
}

// MARK: - Representation selection

struct SkinManifestQuery: Equatable {
    var device: SkinDevice
    var screenClass: SkinScreenClass
    var orientation: SkinOrientation

    init(
        device: SkinDevice = .iphone,
        screenClass: SkinScreenClass = .standard,
        orientation: SkinOrientation = .landscape
    ) {
        self.device = device
        self.screenClass = screenClass
        self.orientation = orientation
    }
}

extension SkinManifestV2 {
    struct ResolvedRepresentation: Equatable {
        let representation: SkinOrientationRepresentation
    }

    /// Falls back by device, then screen class, then orientation, and finally
    /// takes any representation at all.
    func bestRepresentation(for query: SkinManifestQuery) -> ResolvedRepresentation? {
        let available = representations ?? [:]
        for device in Self.orderedRawDevices(preferred: query.device) {
            guard let byScreenClass = available[device] else { continue }
            for screenClass in Self.orderedRawScreenClasses(preferred: query.screenClass) {
                guard let set = byScreenClass[screenClass] else { continue }
                for orientation in Self.orderedRawOrientations(preferred: query.orientation) {
                    if orientation == "portrait", let portrait = set.portrait {
                        return ResolvedRepresentation(representation: portrait)
                    }
                    if orientation == "landscape", let landscape = set.landscape {
                        return ResolvedRepresentation(representation: landscape)
                    }
                }
            }
        }
        for byScreenClass in available.values {
            for set in byScreenClass.values {
                if let portrait = set.portrait {
                    return ResolvedRepresentation(representation: portrait)
                }
                if let landscape = set.landscape {
                    return ResolvedRepresentation(representation: landscape)
                }
            }
        }
        return nil
    }

    private static func orderedRawDevices(preferred: SkinDevice) -> [String] {
        preferred == .ipad ? ["ipad", "iphone"] : ["iphone", "ipad"]
    }

    private static func orderedRawScreenClasses(preferred: SkinScreenClass) -> [String] {
        preferred == .edgeToEdge ? ["edgeToEdge", "standard"] : ["standard", "edgeToEdge"]
    }

    private static func orderedRawOrientations(preferred: SkinOrientation) -> [String] {
        preferred == .portrait ? ["portrait", "landscape"] : ["landscape", "portrait"]
    }
}
