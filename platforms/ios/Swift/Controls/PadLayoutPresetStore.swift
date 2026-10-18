// PadLayoutPresetStore.swift - JSON-backed virtual pad layout presets
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

struct PadLayoutSnapshot: Codable, Equatable {
    var portrait: [String: PadGroupPosition]
    var landscape: [String: PadGroupPosition]
    var perButtonPortrait: [String: PadGroupPosition]
    var perButtonLandscape: [String: PadGroupPosition]
    var controlVisibility: [String: Bool]

    func position(for id: String, landscape isLandscape: Bool) -> PadGroupPosition {
        let dict = isLandscape ? landscape : portrait
        let defaults = isLandscape ? PadLayoutStore.defaultLandscape : PadLayoutStore.defaultPortrait
        return dict[id] ?? defaults[id] ?? PadGroupPosition(x: 0.5, y: 0.5, scale: 1.0)
    }

    func perButtonPosition(for id: String, landscape isLandscape: Bool, areaW: CGFloat, areaH: CGFloat) -> PadGroupPosition {
        let dict = isLandscape ? perButtonLandscape : perButtonPortrait
        if let pos = dict[id] {
            return pos
        }

        let groupID = PadLayoutStore.groupID(for: id)
        let groupPos = position(for: groupID, landscape: isLandscape)
        let offset = VirtualPadButtonOffset.offset(for: id, isLandscape: isLandscape)
        let scaledOffsetX = offset.width * groupPos.scaleX
        let scaledOffsetY = offset.height * groupPos.scaleY
        return PadGroupPosition(
            x: groupPos.x + scaledOffsetX / areaW,
            y: groupPos.y + scaledOffsetY / areaH,
            scaleX: groupPos.scaleX,
            scaleY: groupPos.scaleY,
            hitScaleX: groupPos.hitScaleX,
            hitScaleY: groupPos.hitScaleY
        )
    }

    func isControlVisible(_ id: String) -> Bool {
        if let explicit = controlVisibility[id] {
            return explicit
        }
        let group = PadLayoutStore.groupID(for: id)
        return controlVisibility[group] ?? true
    }

    func controlGeometries(size: CGSize, landscape isLandscape: Bool, analogStickScale: CGFloat) -> [PadEditorControlGeometry] {
        func geometry(_ id: String, _ p: PadGroupPosition) -> PadEditorControlGeometry {
            let isStick = id == "lstick" || id == "rstick"
            return PadEditorControlGeometry(
                id: id,
                center: CGPoint(x: p.x * size.width, y: p.y * size.height),
                baseSize: padEditorBaseSize(for: id, isLandscape: isLandscape, analogStickScale: analogStickScale),
                hitScaleX: isStick ? p.scaleX : p.hitScaleX,
                hitScaleY: isStick ? p.scaleY : p.hitScaleY
            )
        }
        let buttons = PadLayoutStore.perButtonIDs.filter(isControlVisible).map {
            geometry($0, perButtonPosition(for: $0, landscape: isLandscape, areaW: size.width, areaH: size.height))
        }
        let groups = PadLayoutStore.groupIDs.filter { $0 != "action" && $0 != "dpad" && isControlVisible($0) }.map {
            geometry($0, position(for: $0, landscape: isLandscape))
        }
        return buttons + groups
    }

    func portraitEnvelope(analogStickScale: CGFloat) -> (top: CGFloat, height: CGFloat) {
        let reference = CGSize(width: PadPortraitFit.referenceDeckWidth, height: PadPortraitFit.referenceDeckHeight)
        let geometries = controlGeometries(size: reference, landscape: false, analogStickScale: analogStickScale)
        // Touch areas count only up to the deck edges, where the fitted layout clips them.
        guard let top = geometries.map({ max($0.hitFrame.minY, 0) }).min() else { return (0, 0) }
        // StickView draws its 18 pt badge right under the stick.
        let bottom = geometries.map {
            min($0.hitFrame.maxY, PadPortraitFit.referenceDeckHeight) + ($0.id.hasSuffix("stick") ? 18 : 0)
        }.max() ?? top
        return (top, bottom - top)
    }

    func fittedToPortraitDeck(_ fit: PadPortraitFit, size: CGSize, analogStickScale: CGFloat) -> PadLayoutSnapshot {
        var fitted = self
        fitted.portrait = Dictionary(uniqueKeysWithValues: PadLayoutStore.groupIDs.map { ($0, fit.fitted(position(for: $0, landscape: false), of: $0)) })
        fitted.perButtonPortrait = Dictionary(uniqueKeysWithValues: perButtonPortrait.map { ($0.key, fit.fitted($0.value, of: $0.key)) })

        for g in fitted.controlGeometries(size: size, landscape: false, analogStickScale: analogStickScale) where !g.id.hasSuffix("stick") {
            let frame = g.hitFrame
            guard frame.minX < 0 || frame.minY < 0 || frame.maxX > size.width || frame.maxY > size.height else { continue }
            let isPerButton = PadLayoutStore.perButtonIDs.contains(g.id)
            var p = isPerButton
                ? fitted.perButtonPosition(for: g.id, landscape: false, areaW: size.width, areaH: size.height)
                : fitted.position(for: g.id, landscape: false)
            let roomX = max(0, min(g.center.x, size.width - g.center.x)) * 2
            let roomY = max(0, min(g.center.y, size.height - g.center.y)) * 2
            p.hitScaleX = min(p.hitScaleX, roomX / max(g.baseSize.width, PadLayoutMetrics.minimumTouchLength))
            p.hitScaleY = min(p.hitScaleY, roomY / max(g.baseSize.height, PadLayoutMetrics.minimumTouchLength))
            if isPerButton {
                fitted.perButtonPortrait[g.id] = p
            } else {
                fitted.portrait[g.id] = p
            }
        }
        return fitted
    }
}

/// Places the reference portrait deck's layout in a shorter deck: x stays, sizes and rows scale by `scale`.
struct PadPortraitFit {
    // The iPhone 18 Pro portrait deck the built-in layout was drawn on.
    static let referenceDeckWidth: CGFloat = 402
    static let referenceDeckHeight: CGFloat = 510.5
    // The smallest pad scale before the game gives up height on a short deck.
    static let outerMinimumScale: CGFloat = 0.70
    static let topGap: CGFloat = 4
    static let minimumHitScale: CGFloat = 44 / PadLayoutMetrics.minimumTouchLength

    let scale: CGFloat
    let envelopeTop: CGFloat
    let deckHeight: CGFloat

    init?(envelope: (top: CGFloat, height: CGFloat), deck: CGSize, barBase: CGFloat) {
        guard envelope.height > 0, deck.width > 0, deck.height > 0 else { return nil }
        scale = max(min(1, (deck.height - Self.topGap - barBase) / envelope.height), PadLayoutMetrics.minControlScale)
        envelopeTop = envelope.top
        deckHeight = deck.height
    }

    func fitted(_ p: PadGroupPosition, of id: String) -> PadGroupPosition {
        // Opposite D-pad arrows sit closer than about 44 pt, so their touch areas shrink with the pad.
        let minimumHit = PadLayoutStore.groupID(for: id) == "dpad" ? 0 : Self.minimumHitScale
        return PadGroupPosition(
            x: p.x,
            y: (Self.topGap + (p.y * Self.referenceDeckHeight - envelopeTop) * scale) / deckHeight,
            scaleX: p.scaleX * scale,
            scaleY: p.scaleY * scale,
            hitScaleX: max(p.hitScaleX * scale, min(p.hitScaleX, minimumHit)),
            hitScaleY: max(p.hitScaleY * scale, min(p.hitScaleY, minimumHit))
        )
    }
}

enum PadLayoutPresetSource: String, Codable, Equatable {
    case user
    case builtIn
    case futureImportedSkin
}

struct PadLayoutPreset: Codable, Equatable, Identifiable {
    var id: String
    var displayName: String
    var createdAt: Date
    var updatedAt: Date
    var source: PadLayoutPresetSource
    var linkedSkinID: String?
    var snapshot: PadLayoutSnapshot
}

struct PadLayoutEditorContext: Equatable {
    var presetID: String?
    var gameIdentity: PadLayoutGameIdentity?
    var initialSnapshot: PadLayoutSnapshot?
    var skinDescriptor: VPadSkinDescriptor?

    init(
        presetID: String? = nil,
        gameIdentity: PadLayoutGameIdentity? = nil,
        initialSnapshot: PadLayoutSnapshot? = nil,
        skinDescriptor: VPadSkinDescriptor? = nil
    ) {
        self.presetID = presetID
        self.gameIdentity = gameIdentity
        self.initialSnapshot = initialSnapshot
        self.skinDescriptor = skinDescriptor
    }

    static let current = PadLayoutEditorContext(
        presetID: nil,
        gameIdentity: nil,
        initialSnapshot: nil,
        skinDescriptor: nil
    )
}

struct PadLayoutGameIdentity: Codable, Equatable, Hashable, Identifiable {
    let serial: String
    let crc: String

    var id: String {
        "\(serial)|\(crc)"
    }

    init?(serial: String?, crc: String?) {
        let normalizedSerial = Self.normalizedSerial(serial)
        let normalizedCRC = Self.normalizedCRC(crc)
        guard !normalizedSerial.isEmpty, !normalizedCRC.isEmpty else {
            return nil
        }
        self.serial = normalizedSerial
        self.crc = normalizedCRC
    }

    static func normalizedSerial(_ value: String?) -> String {
        (value ?? "")
            .replacingOccurrences(of: "_", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased()
    }

    static func normalizedCRC(_ value: String?) -> String {
        var raw = (value ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased()
        if raw.hasPrefix("CRC-") {
            raw.removeFirst(4)
        }
        raw = raw.replacingOccurrences(of: "0X", with: "")
        raw = String(raw.filter { character in
            ("0"..."9").contains(character) || ("A"..."F").contains(character)
        })
        guard !raw.isEmpty, (UInt64(raw, radix: 16) ?? 0) != 0 else {
            return ""
        }
        return raw
    }
}

struct VPadGameAssignment: Codable, Equatable {
    var layoutPresetID: String?
    var skinID: String?

    init(layoutPresetID: String? = nil, skinID: String? = nil) {
        self.layoutPresetID = layoutPresetID
        self.skinID = skinID
    }

    var isEmpty: Bool {
        layoutPresetID == nil && skinID == nil
    }
}

private struct PadLayoutPresetLibrary: Codable {
    var schemaVersion: Int
    var presets: [PadLayoutPreset]
    var globalPresetID: String?
    var gameAssignments: [String: VPadGameAssignment]

    private enum CodingKeys: String, CodingKey {
        case schemaVersion
        case presets
        case globalPresetID
        case gameAssignments
    }

    init(
        schemaVersion: Int,
        presets: [PadLayoutPreset],
        globalPresetID: String?,
        gameAssignments: [String: VPadGameAssignment]
    ) {
        self.schemaVersion = schemaVersion
        self.presets = presets
        self.globalPresetID = globalPresetID
        self.gameAssignments = gameAssignments
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        schemaVersion = try container.decode(Int.self, forKey: .schemaVersion)
        presets = try container.decode([PadLayoutPreset].self, forKey: .presets)
        globalPresetID = try container.decodeIfPresent(String.self, forKey: .globalPresetID)
        if let assignments = try? container.decode([String: VPadGameAssignment].self, forKey: .gameAssignments) {
            gameAssignments = assignments
        } else {
            let oldAssignments = try container.decodeIfPresent([String: String].self, forKey: .gameAssignments) ?? [:]
            gameAssignments = oldAssignments.mapValues { VPadGameAssignment(layoutPresetID: $0, skinID: nil) }
        }
    }
}

@Observable
final class PadLayoutPresetStore: @unchecked Sendable {
    static let shared = PadLayoutPresetStore()
    static let schemaVersion = 2

    private let libraryURL: URL
    private(set) var presets: [PadLayoutPreset] = []
    var globalPresetID: String? {
        didSet {
            if globalPresetID != oldValue {
                persist()
            }
        }
    }
    private var gameAssignments: [String: VPadGameAssignment] = [:]

    init(
        libraryURL: URL? = nil,
        migrateFromCurrentLayout: Bool = true,
        currentLayout: PadLayoutStore = .shared
    ) {
        self.libraryURL = libraryURL ?? Self.defaultLibraryURL()
        let loadResult = loadLibrary()
        switch loadResult {
        case .loaded(let library):
            presets = library.presets
            globalPresetID = validPresetID(library.globalPresetID)
            gameAssignments = sanitizedAssignments(library.gameAssignments)
        case .missing:
            if migrateFromCurrentLayout {
                migrateCurrentLayout(currentLayout)
            }
        case .corrupt:
            presets = []
            globalPresetID = nil
            gameAssignments = [:]
        }
    }

    @discardableResult
    func createPreset(
        named name: String,
        snapshot: PadLayoutSnapshot,
        source: PadLayoutPresetSource = .user,
        linkedSkinID: String? = nil
    ) -> PadLayoutPreset {
        let now = Date()
        let preset = PadLayoutPreset(
            id: UUID().uuidString,
            displayName: sanitizedName(name, fallback: "Layout"),
            createdAt: now,
            updatedAt: now,
            source: source,
            linkedSkinID: linkedSkinID,
            snapshot: snapshot
        )
        presets.append(preset)
        persist()
        return preset
    }

    @discardableResult
    func importLayout(data: Data, fallbackName: String?) throws -> PadLayoutPreset {
        let imported = try PadLayoutImportExport.decodeImport(from: data, fallbackName: fallbackName)
        return createPreset(
            named: uniqueDisplayName(imported.displayName ?? "Imported Layout"),
            snapshot: imported.snapshot
        )
    }

    func updatePreset(id: String, snapshot: PadLayoutSnapshot) throws {
        guard let index = presets.firstIndex(where: { $0.id == id }) else {
            throw PadLayoutPresetStoreError.missingPreset
        }
        presets[index].snapshot = snapshot
        presets[index].updatedAt = Date()
        persist()
    }

    func renamePreset(id: String, to name: String) throws {
        guard let index = presets.firstIndex(where: { $0.id == id }) else {
            throw PadLayoutPresetStoreError.missingPreset
        }
        presets[index].displayName = sanitizedName(name, fallback: presets[index].displayName)
        presets[index].updatedAt = Date()
        persist()
    }

    @discardableResult
    func duplicatePreset(id: String, named name: String? = nil) throws -> PadLayoutPreset {
        guard let original = presets.first(where: { $0.id == id }) else {
            throw PadLayoutPresetStoreError.missingPreset
        }
        return createPreset(
            named: name ?? "\(original.displayName) Copy",
            snapshot: original.snapshot,
            source: .user,
            linkedSkinID: original.linkedSkinID
        )
    }

    func deletePreset(id: String) throws {
        guard presets.contains(where: { $0.id == id }) else {
            throw PadLayoutPresetStoreError.missingPreset
        }
        presets.removeAll { $0.id == id }
        if globalPresetID == id {
            globalPresetID = nil
        }
        gameAssignments = gameAssignments.compactMapValues { assignment in
            var updated = assignment
            if updated.layoutPresetID == id {
                updated.layoutPresetID = nil
            }
            return updated.isEmpty ? nil : updated
        }
        persist()
    }

    func preset(id: String?) -> PadLayoutPreset? {
        guard let id else { return nil }
        return presets.first { $0.id == id }
    }

    func presetID(for identity: PadLayoutGameIdentity) -> String? {
        validPresetID(gameAssignments[identity.id]?.layoutPresetID)
    }

    /// Catalog skins are matched by disc serial, while PCSX2's runtime settings
    /// identity also includes a CRC. Keep the accepted automatic assignment at
    /// serial scope so a different executable/disc CRC cannot silently fall
    /// back to Global Default. Accepting another skin is an explicit choice, so
    /// it also replaces stale exact-CRC skin assignments for the same game.
    /// Existing exact layout choices are preserved unless a valid catalog
    /// layout was selected alongside the skin.
    func setAutomaticAssignment(
        skinID: String,
        layoutPresetID: String?,
        forSerial rawSerial: String,
        using skinLibrary: VPadSkinLibraryStore
    ) {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty,
              skinLibrary.descriptor(id: skinID) != nil else {
            return
        }
        let assignment = VPadGameAssignment(
            layoutPresetID: validPresetID(layoutPresetID),
            skinID: skinID
        )
        gameAssignments[automaticAssignmentKey(forSerial: serial)] = assignment
        let exactAssignmentPrefix = "\(serial)|"
        for key in Array(gameAssignments.keys)
        where key.hasPrefix(exactAssignmentPrefix) {
            var exactAssignment = gameAssignments[key] ?? VPadGameAssignment()
            exactAssignment.skinID = skinID
            if let layoutPresetID = assignment.layoutPresetID {
                exactAssignment.layoutPresetID = layoutPresetID
            }
            gameAssignments[key] = exactAssignment
        }
        persist()
    }

    func automaticSkinID(forSerial rawSerial: String) -> String? {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty else { return nil }
        return gameAssignments[automaticAssignmentKey(forSerial: serial)]?.skinID
    }

    /// Changes only the skin component of a serial-scoped assignment. Quick
    /// Menu skin cycling must not erase a separately chosen controller layout.
    /// Exact runtime identities are updated too so the active game redraws on
    /// the same frame instead of waiting for another launch.
    func setAutomaticSkin(
        _ skinID: String,
        forSerial rawSerial: String,
        using skinLibrary: VPadSkinLibraryStore
    ) {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty,
              skinLibrary.descriptor(id: skinID) != nil else { return }

        let automaticKey = automaticAssignmentKey(forSerial: serial)
        var automatic = gameAssignments[automaticKey] ?? VPadGameAssignment()
        automatic.skinID = skinID
        gameAssignments[automaticKey] = automatic

        let exactAssignmentPrefix = "\(serial)|"
        for key in Array(gameAssignments.keys)
        where key.hasPrefix(exactAssignmentPrefix) {
            var exact = gameAssignments[key] ?? VPadGameAssignment()
            exact.skinID = skinID
            gameAssignments[key] = exact
        }
        persist()
    }

    @MainActor
    func setPreset(_ presetID: String?, for identity: PadLayoutGameIdentity) {
        let presetID = validPresetID(presetID)
        if presetID == nil {
            clearAutomaticAssignment(\.layoutPresetID, forSerial: identity.serial)
        }
        var assignment = gameAssignments[identity.id] ?? VPadGameAssignment()
        assignment.layoutPresetID = presetID
        setAssignment(assignment, for: identity)
        persist()
    }

    func skinID(for identity: PadLayoutGameIdentity) -> String? {
        gameAssignments[identity.id]?.skinID
    }

    func setSkin(_ skinID: String?, for identity: PadLayoutGameIdentity) {
        var assignment = gameAssignments[identity.id] ?? VPadGameAssignment()
        assignment.skinID = skinID?.trimmingCharacters(in: .whitespacesAndNewlines).nilIfEmpty
        setAssignment(assignment, for: identity)
        persist()
    }

    @MainActor
    func setSkin(_ skinID: String?, for identity: PadLayoutGameIdentity, using skinLibrary: VPadSkinLibraryStore) {
        if let skinID, skinLibrary.descriptor(id: skinID) != nil {
            setSkin(skinID, for: identity)
        } else {
            clearSkin(for: identity)
        }
    }

    @MainActor
    func clearSkin(for identity: PadLayoutGameIdentity) {
        clearAutomaticAssignment(\.skinID, forSerial: identity.serial)
        var assignment = gameAssignments[identity.id] ?? VPadGameAssignment()
        assignment.skinID = nil
        setAssignment(assignment, for: identity)
        persist()
    }

    func clearSkinAssignments(forSkinID skinID: String) {
        gameAssignments = gameAssignments.compactMapValues { assignment in
            var updated = assignment
            if updated.skinID == skinID {
                updated.skinID = nil
            }
            return updated.isEmpty ? nil : updated
        }
        persist()
    }

    /// Reinstalling a skin mints a new id, so per-game picks have to follow it
    /// across instead of being cleared.
    func repointSkinAssignments(from oldID: String, to newID: String) {
        gameAssignments = gameAssignments.compactMapValues { assignment in
            var updated = assignment
            if updated.skinID == oldID {
                updated.skinID = newID
            }
            return updated.isEmpty ? nil : updated
        }
        persist()
    }

    func repointLayoutPresetAssignments(from oldID: String, to newID: String) {
        if globalPresetID == oldID {
            globalPresetID = newID
        }
        gameAssignments = gameAssignments.compactMapValues { assignment in
            var updated = assignment
            if updated.layoutPresetID == oldID {
                updated.layoutPresetID = newID
            }
            return updated.isEmpty ? nil : updated
        }
        persist()
    }

    @MainActor
    func clearVPadOverrides(for identity: PadLayoutGameIdentity) {
        clearAutomaticAssignment(\.skinID, \.layoutPresetID, forSerial: identity.serial)
        gameAssignments.removeValue(forKey: identity.id)
        persist()
    }

    func effectivePreset(
        for identity: PadLayoutGameIdentity?,
        fallbackSerial: String? = nil
    ) -> PadLayoutPreset? {
        if let identity,
           let assignedID = presetID(for: identity),
           let preset = preset(id: assignedID) {
            return preset
        }
        let serial = identity?.serial
            ?? PadLayoutGameIdentity.normalizedSerial(fallbackSerial)
        if !serial.isEmpty,
           let assignedID = gameAssignments[
               automaticAssignmentKey(forSerial: serial)
           ]?.layoutPresetID,
           let preset = preset(id: assignedID) {
            return preset
        }
        return preset(id: globalPresetID)
    }

    func effectiveSnapshot(
        for identity: PadLayoutGameIdentity?,
        fallbackSerial: String? = nil
    ) -> PadLayoutSnapshot? {
        effectivePreset(
            for: identity,
            fallbackSerial: fallbackSerial
        )?.snapshot
    }

    func effectiveSkinDescriptor(
        for identity: PadLayoutGameIdentity?,
        fallbackSerial: String? = nil,
        using skinLibrary: VPadSkinLibraryStore
    ) -> VPadSkinDescriptor {
        if let identity,
           let assignedID = skinID(for: identity),
           let descriptor = skinLibrary.descriptor(id: assignedID) {
            return descriptor
        }
        let serial = identity?.serial
            ?? PadLayoutGameIdentity.normalizedSerial(fallbackSerial)
        if !serial.isEmpty,
           let assignedID = automaticSkinID(forSerial: serial),
           let descriptor = skinLibrary.descriptor(id: assignedID) {
            return descriptor
        }
        return skinLibrary.selectedDescriptor
    }

    func save() throws {
        let library = PadLayoutPresetLibrary(
            schemaVersion: Self.schemaVersion,
            presets: presets,
            globalPresetID: validPresetID(globalPresetID),
            gameAssignments: sanitizedAssignments(gameAssignments)
        )
        try FileManager.default.createDirectory(
            at: libraryURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(library).write(to: libraryURL, options: .atomic)
    }

    private enum LoadResult {
        case loaded(PadLayoutPresetLibrary)
        case missing
        case corrupt
    }

    private func loadLibrary() -> LoadResult {
        guard FileManager.default.fileExists(atPath: libraryURL.path) else {
            return .missing
        }
        do {
            let decoder = JSONDecoder()
            decoder.dateDecodingStrategy = .iso8601
            let data = try Data(contentsOf: libraryURL)
            return .loaded(try decoder.decode(PadLayoutPresetLibrary.self, from: data))
        } catch {
            return .corrupt
        }
    }

    private func migrateCurrentLayout(_ currentLayout: PadLayoutStore) {
        let preset = createPreset(named: "Current Layout", snapshot: currentLayout.snapshot())
        globalPresetID = preset.id
        persist()
    }

    private func validPresetID(_ id: String?) -> String? {
        guard let id, presets.contains(where: { $0.id == id }) else {
            return nil
        }
        return id
    }

    private func sanitizedAssignments(_ assignments: [String: VPadGameAssignment]) -> [String: VPadGameAssignment] {
        assignments.compactMapValues { assignment in
            let sanitized = VPadGameAssignment(
                layoutPresetID: validPresetID(assignment.layoutPresetID),
                skinID: assignment.skinID?.trimmingCharacters(in: .whitespacesAndNewlines).nilIfEmpty
            )
            return sanitized.isEmpty ? nil : sanitized
        }
    }

    private func setAssignment(_ assignment: VPadGameAssignment, for identity: PadLayoutGameIdentity) {
        if assignment.isEmpty {
            gameAssignments.removeValue(forKey: identity.id)
        } else {
            gameAssignments[identity.id] = assignment
        }
    }

    private func automaticAssignmentKey(forSerial serial: String) -> String {
        "automatic-serial|\(serial)"
    }

    // A catalog pick is also stored for the whole serial, and one accepted before
    // the game boots is applied again then, so a reset has to clear both.
    @MainActor
    private func clearAutomaticAssignment(
        _ fields: WritableKeyPath<VPadGameAssignment, String?>...,
        forSerial serial: String
    ) {
        AutomaticCustomSkinManager.shared.forgetAcceptedAssignment(forSerial: serial)
        let key = automaticAssignmentKey(forSerial: serial)
        guard var automatic = gameAssignments[key] else { return }
        for field in fields {
            automatic[keyPath: field] = nil
        }
        gameAssignments[key] = automatic.isEmpty ? nil : automatic
    }

    private func sanitizedName(_ name: String, fallback: String) -> String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? fallback : trimmed
    }

    private func uniqueDisplayName(_ name: String) -> String {
        let base = sanitizedName(name, fallback: "Imported Layout")
        let used = Set(presets.map { $0.displayName.lowercased() })
        if !used.contains(base.lowercased()) {
            return base
        }
        var suffix = 2
        while used.contains("\(base) \(suffix)".lowercased()) {
            suffix += 1
        }
        return "\(base) \(suffix)"
    }

    private func persist() {
        do {
            try save()
        } catch {
            NSLog("[ARMSX2 iOS VPad] Failed to save layout presets: %@", error.localizedDescription)
        }
    }

    private static func defaultLibraryURL() -> URL {
        let base = URL.applicationSupportDirectory
        return base
            .appendingPathComponent("ARMSX2", isDirectory: true)
            .appendingPathComponent("VPadLayouts.json")
    }
}

enum PadLayoutPresetStoreError: Error {
    case missingPreset
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }
}
