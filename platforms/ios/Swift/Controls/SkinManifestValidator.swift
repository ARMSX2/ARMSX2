// SkinManifestValidator.swift — skin manifest validator
// SPDX-License-Identifier: GPL-3.0+

import Foundation

// MARK: - Validator

/// Validates a decoded manifest without installing it. Produces structured
/// errors and warnings with JSON-path-like locations.
enum SkinManifestValidator {
    struct Issue: Equatable {
        let location: String
        let message: String

        var description: String { "\(location): \(message)" }
    }

    struct Result: Equatable {
        var errors: [Issue] = []
        var warnings: [Issue] = []
    }

    /// Validates an already-decoded manifest. When `packageRoot` is supplied,
    /// referenced asset and preview paths are also checked for existence
    /// (reported as warnings); without it, validation is structure-only.
    static func validate(_ manifest: SkinManifestV2, packageRoot: URL? = nil) -> Result {
        var result = Result()

        // formatVersion
        if let version = manifest.formatVersion {
            if !SkinManifestV2.supportedFormatVersions.contains(version) {
                let supported = SkinManifestV2.supportedFormatVersions.sorted().map(String.init).joined(separator: ", ")
                result.errors.append(Issue(location: "formatVersion", message: "unsupported format version \(version); supported: \(supported)"))
            }
        } else {
            result.errors.append(Issue(location: "formatVersion", message: "missing"))
        }

        // name
        if (manifest.name ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            result.errors.append(Issue(location: "name", message: "missing or empty"))
        }

        // identifier
        let identifier = (manifest.identifier ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if identifier.isEmpty {
            result.errors.append(Issue(location: "identifier", message: "missing or empty"))
        } else if !Self.isSafeIdentifier(identifier) {
            result.errors.append(Issue(location: "identifier", message: "must contain only letters, digits, dots, dashes, or underscores (reverse-DNS recommended)"))
        }

        // game type
        let primaryType = (manifest.gameTypeIdentifier ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let supportedTypes = (manifest.supportedGameTypeIdentifiers ?? []).compactMap { entry -> String? in
            let trimmed = entry.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }
        let declaredTypes = ([primaryType] + supportedTypes).filter { !$0.isEmpty }
        if declaredTypes.isEmpty {
            result.errors.append(Issue(location: "gameTypeIdentifier", message: "missing; a PS2 game type is required"))
        } else if !declaredTypes.contains(where: { Self.isPS2GameType($0) }) {
            result.errors.append(Issue(location: "gameTypeIdentifier", message: "not a PS2 skin; expected \"\(SkinManifestV2.ps2GameTypeIdentifier)\" (declared: \(declaredTypes.joined(separator: ", ")))"))
        }

        // preview (optional)
        if let preview = manifest.preview?.trimmingCharacters(in: .whitespacesAndNewlines), !preview.isEmpty {
            if !SkinAssetPath.isSafeRelative(preview) {
                result.errors.append(Issue(location: "preview", message: "preview path \"\(preview)\" is not a safe relative path"))
            } else if let packageRoot, !FileManager.default.fileExists(atPath: packageRoot.appendingPathComponent(preview).path) {
                result.warnings.append(Issue(location: "preview", message: "preview \"\(preview)\" was not found in the package"))
            }
        }

        // representations
        guard let representations = manifest.representations, !representations.isEmpty else {
            result.errors.append(Issue(location: "representations", message: "missing or empty; at least one representation is required"))
            return result
        }

        for deviceKey in representations.keys.sorted() {
            let devicePath = "representations.\(deviceKey)"
            if SkinDevice.from(deviceKey) == .unknown {
                result.warnings.append(Issue(location: devicePath, message: "unsupported device \"\(deviceKey)\"; supported: iphone, ipad"))
            }
            guard let byScreenClass = representations[deviceKey], !byScreenClass.isEmpty else {
                result.errors.append(Issue(location: devicePath, message: "no screen classes defined"))
                continue
            }
            for screenClassKey in byScreenClass.keys.sorted() {
                let screenClassPath = "\(devicePath).\(screenClassKey)"
                let screenClass = SkinScreenClass.from(screenClassKey)
                if screenClass == .unknown {
                    result.warnings.append(Issue(location: screenClassPath, message: "unsupported screen class \"\(screenClassKey)\"; supported: standard, edgeToEdge"))
                } else if screenClass == .splitView {
                    result.warnings.append(Issue(location: screenClassPath, message: "splitView is parsed but reserved; it is ignored in manifest v2.0"))
                }
                guard let orientationSet = byScreenClass[screenClassKey] else { continue }
                if orientationSet.portrait == nil && orientationSet.landscape == nil {
                    result.errors.append(Issue(location: screenClassPath, message: "no orientations defined"))
                    continue
                }
                if let portrait = orientationSet.portrait {
                    validateOrientation(portrait, location: "\(screenClassPath).portrait", packageRoot: packageRoot, into: &result)
                }
                if let landscape = orientationSet.landscape {
                    validateOrientation(landscape, location: "\(screenClassPath).landscape", packageRoot: packageRoot, into: &result)
                }
            }
        }

        return result
    }

    // MARK: Orientation + control checks

    private static func validateOrientation(
        _ representation: SkinOrientationRepresentation,
        location: String,
        packageRoot: URL?,
        into result: inout Result
    ) {
        validateMappingSize(representation.mappingSize, location: location, into: &result)
        validateExtendedEdges(representation.extendedEdges, location: "\(location).extendedEdges", into: &result)

        if let assets = representation.assets {
            validateAssetPaths(assets.referencedPaths, location: "\(location).assets", packageRoot: packageRoot, into: &result)
        }

        if representation.screen != nil {
            result.warnings.append(Issue(location: "\(location).screen", message: "parsed but ignored in manifest v2.0 (screen-frame/bezel placement is not supported yet)"))
        }

        guard let controls = representation.controls, !controls.isEmpty else {
            result.warnings.append(Issue(location: "\(location).controls", message: "no controls defined; this representation is background-only"))
            return
        }

        var seenIdentifiers = Set<String>()
        for (index, control) in controls.enumerated() {
            validateControl(control, location: "\(location).controls[\(index)]", packageRoot: packageRoot, seenIdentifiers: &seenIdentifiers, into: &result)
        }
    }

    private static func validateControl(
        _ control: SkinControl,
        location: String,
        packageRoot: URL?,
        seenIdentifiers: inout Set<String>,
        into result: inout Result
    ) {
        let identifier = (control.id ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if identifier.isEmpty {
            result.errors.append(Issue(location: "\(location).id", message: "missing or empty"))
        } else if !seenIdentifiers.insert(identifier).inserted {
            result.errors.append(Issue(location: "\(location).id", message: "duplicate control id \"\(identifier)\""))
        }

        let kind = control.resolvedKind
        if !kind.isKnown {
            result.warnings.append(Issue(location: "\(location).kind", message: "unsupported kind \"\(control.kind ?? "")\"; supported: button, dpad, thumbstick, function"))
        }

        validateFrame(control.frame, location: "\(location).frame", requirePositiveSize: true, into: &result)
        validateExtendedEdges(control.extendedEdges, location: "\(location).extendedEdges", into: &result)

        if let zIndex = control.zIndex, !zIndex.isFinite {
            result.errors.append(Issue(location: "\(location).zIndex", message: "must be a finite number"))
        }

        if control.resolvedVisibility == .unknown {
            result.warnings.append(Issue(location: "\(location).visibility", message: "unsupported visibility \"\(control.visibility ?? "")\"; supported: always, hidden, autoHideWithPad"))
        }

        // inputs
        if let inputs = control.inputs {
            for inputID in inputs.referencedInputIDs where !SkinInputID.isKnown(inputID) {
                result.errors.append(Issue(location: "\(location).inputs", message: "unknown input \"\(inputID)\""))
            }
            switch inputs {
            case .list:
                if kind == .dpad || kind == .thumbstick {
                    result.warnings.append(Issue(location: "\(location).inputs", message: "a \(kind) control usually maps inputs as {up, down, left, right}"))
                }
            case .map:
                if kind != .dpad && kind != .thumbstick {
                    result.warnings.append(Issue(location: "\(location).inputs", message: "a \(kind) control usually lists inputs as an array"))
                }
            case .single, .unknown:
                break
            }
        }

        // click input (sticks)
        if let click = control.clickInput?.trimmingCharacters(in: .whitespacesAndNewlines), !click.isEmpty, !SkinInputID.isKnown(click) {
            result.errors.append(Issue(location: "\(location).clickInput", message: "unknown input \"\(click)\""))
        }

        // action (future function buttons; never executed here)
        let action = (control.action ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if !action.isEmpty {
            if !SkinAction.isKnown(action) {
                result.errors.append(Issue(location: "\(location).action", message: "action \"\(action)\" is not supported in this version"))
            } else if kind != .function {
                result.warnings.append(Issue(location: "\(location).action", message: "action is ignored unless the control kind is \"function\""))
            }
        } else if kind == .function {
            result.warnings.append(Issue(location: "\(location).action", message: "function control has no action"))
        }

        // thumbstick sizing
        if let thumbstick = control.thumbstick {
            if let width = thumbstick.width, !width.isFinite || width <= 0 {
                result.errors.append(Issue(location: "\(location).thumbstick.width", message: "expected a positive number"))
            }
            if let height = thumbstick.height, !height.isFinite || height <= 0 {
                result.errors.append(Issue(location: "\(location).thumbstick.height", message: "expected a positive number"))
            }
        }

        // assets
        if let asset = control.asset {
            validateAssetPaths(asset.referencedPaths, location: "\(location).asset", packageRoot: packageRoot, into: &result)
            if asset.selected != nil {
                result.warnings.append(Issue(location: "\(location).asset.selected", message: "reserved and ignored in manifest v2.0"))
            }
        }

        // Optional per-direction D-pad art. Safe-relative paths are enforced here;
        // existence of referenced files is a blocking install check in the importer.
        if let directions = control.directionAssets {
            for (name, directional) in [("up", directions.up), ("down", directions.down), ("left", directions.left), ("right", directions.right)] {
                if let directional {
                    validateAssetPaths(directional.referencedPaths, location: "\(location).directionAssets.\(name)", packageRoot: packageRoot, into: &result)
                }
            }
        }

        // reserved fields (parsed, inert)
        if control.haptic != nil {
            result.warnings.append(Issue(location: "\(location).haptic", message: "reserved and ignored in manifest v2.0"))
        }
        if control.sound != nil {
            result.warnings.append(Issue(location: "\(location).sound", message: "reserved and ignored in manifest v2.0"))
        }
        if control.animation != nil {
            result.warnings.append(Issue(location: "\(location).animation", message: "reserved and ignored in manifest v2.0"))
        }
    }

    // MARK: Primitive checks

    private static func validateFrame(_ frame: SkinFrame?, location: String, requirePositiveSize: Bool, into result: inout Result) {
        guard let frame else {
            result.errors.append(Issue(location: location, message: "missing frame"))
            return
        }
        for (name, value) in [("x", frame.x), ("y", frame.y), ("width", frame.width), ("height", frame.height)] {
            guard let value else {
                result.errors.append(Issue(location: "\(location).\(name)", message: "missing"))
                continue
            }
            guard value.isFinite else {
                result.errors.append(Issue(location: "\(location).\(name)", message: "must be a finite number"))
                continue
            }
            if (name == "width" || name == "height") && requirePositiveSize && value <= 0 {
                result.errors.append(Issue(location: "\(location).\(name)", message: "expected a positive number"))
            }
        }
    }

    private static func validateExtendedEdges(_ edges: SkinExtendedEdges?, location: String, into result: inout Result) {
        guard let edges else { return }
        for (name, value) in [("top", edges.top), ("bottom", edges.bottom), ("left", edges.left), ("right", edges.right)] {
            guard let value else { continue }
            if !value.isFinite {
                result.errors.append(Issue(location: "\(location).\(name)", message: "must be a finite number"))
            } else if value < 0 {
                result.errors.append(Issue(location: "\(location).\(name)", message: "must not be negative"))
            }
        }
    }

    private static func validateMappingSize(_ size: SkinSize?, location: String, into result: inout Result) {
        guard let size else {
            result.errors.append(Issue(location: "\(location).mappingSize", message: "missing; a positive point space is required"))
            return
        }
        for (name, value) in [("width", size.width), ("height", size.height)] {
            guard let value else {
                result.errors.append(Issue(location: "\(location).mappingSize.\(name)", message: "missing"))
                continue
            }
            guard value.isFinite, value > 0 else {
                result.errors.append(Issue(location: "\(location).mappingSize.\(name)", message: "expected a positive number"))
                continue
            }
        }
    }

    private static func validateAssetPaths(_ paths: [String], location: String, packageRoot: URL?, into result: inout Result) {
        for path in paths {
            let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { continue }
            if !SkinAssetPath.isSafeRelative(trimmed) {
                result.errors.append(Issue(location: location, message: "asset path \"\(trimmed)\" is not a safe relative path"))
                continue
            }
            if let packageRoot, !FileManager.default.fileExists(atPath: packageRoot.appendingPathComponent(trimmed).path) {
                result.warnings.append(Issue(location: location, message: "asset \"\(trimmed)\" was not found in the package"))
            }
        }
    }

    // MARK: Identifier / game-type checks

    private static func isSafeIdentifier(_ identifier: String) -> Bool {
        guard !identifier.isEmpty else { return false }
        for scalar in identifier.unicodeScalars {
            let value = scalar.value
            let allowed = (value >= 0x30 && value <= 0x39)   // 0-9
                || (value >= 0x41 && value <= 0x5A)           // A-Z
                || (value >= 0x61 && value <= 0x7A)           // a-z
                || value == 0x2E                              // .
                || value == 0x2D                              // -
                || value == 0x5F                              // _
            if !allowed { return false }
        }
        return true
    }

    private static let ps2GameTypeAliases: Set<String> = [
        "com.armsx2.game.ps2",
        "com.armsx2.game.playstation2"
    ]

    private static func isPS2GameType(_ identifier: String) -> Bool {
        ps2GameTypeAliases.contains(identifier.lowercased())
    }
}
