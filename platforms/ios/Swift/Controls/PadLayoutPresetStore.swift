// PadLayoutPresetStore.swift - JSON-backed virtual pad layout presets
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

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
    var classSnapshots: [String: PadLayoutSnapshot]? = nil

    /// The Duo falls back to the phone copy, then every class to the shared base.
    func snapshot(for deviceClass: PadDeviceClass) -> PadLayoutSnapshot {
        classSnapshots?[deviceClass.rawValue]
            ?? (deviceClass == .duo ? classSnapshots?[PadDeviceClass.phone.rawValue] : nil)
            ?? snapshot
    }
}

extension PadLayoutPreset {
    private struct LossySnapshot: Decodable {
        let value: PadLayoutSnapshot?
        init(from decoder: Decoder) throws { value = try? PadLayoutSnapshot(from: decoder) }
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        displayName = try c.decode(String.self, forKey: .displayName)
        createdAt = try c.decode(Date.self, forKey: .createdAt)
        updatedAt = try c.decode(Date.self, forKey: .updatedAt)
        source = try c.decode(PadLayoutPresetSource.self, forKey: .source)
        linkedSkinID = try c.decodeIfPresent(String.self, forKey: .linkedSkinID)
        snapshot = try c.decode(PadLayoutSnapshot.self, forKey: .snapshot)
        let copies = try? c.decodeIfPresent([String: LossySnapshot].self, forKey: .classSnapshots)
        classSnapshots = PadDeviceClass.classCopies(copies?.compactMapValues(\.value))
    }
}

extension PadDeviceClass {
    /// Drops copies for classes this build does not know.
    static func classCopies(_ copies: [String: PadLayoutSnapshot]?) -> [String: PadLayoutSnapshot]? {
        let known = copies?.filter { PadDeviceClass(rawValue: $0.key) != nil } ?? [:]
        return known.isEmpty ? nil : known
    }
}

struct PadLayoutEditorContext: Equatable {
    var presetID: String?
    var gameIdentity: PadLayoutGameIdentity?
    var initialSnapshot: PadLayoutSnapshot?
    var skinDescriptor: VPadSkinDescriptor?
    var effectivePresetID: String?

    init(
        presetID: String? = nil,
        gameIdentity: PadLayoutGameIdentity? = nil,
        initialSnapshot: PadLayoutSnapshot? = nil,
        skinDescriptor: VPadSkinDescriptor? = nil,
        effectivePresetID: String? = nil
    ) {
        self.presetID = presetID
        self.gameIdentity = gameIdentity
        self.initialSnapshot = initialSnapshot
        self.skinDescriptor = skinDescriptor
        self.effectivePresetID = effectivePresetID
    }

    static let current = PadLayoutEditorContext(
        presetID: nil,
        gameIdentity: nil,
        initialSnapshot: nil,
        skinDescriptor: nil
    )

    /// Edits the game's own preset, or the global one when there is no game.
    @MainActor
    static func forGame(
        _ identity: PadLayoutGameIdentity?,
        fallbackSerial: String? = nil,
        store: PadLayoutPresetStore,
        skinDescriptor: VPadSkinDescriptor?
    ) -> PadLayoutEditorContext {
        PadLayoutEditorContext(
            presetID: identity.flatMap { store.presetID(for: $0) } ?? (identity == nil ? store.globalPresetID : nil),
            gameIdentity: identity,
            initialSnapshot: store.effectiveSnapshot(for: identity, fallbackSerial: fallbackSerial),
            skinDescriptor: skinDescriptor,
            effectivePresetID: store.effectivePreset(for: identity, fallbackSerial: fallbackSerial)?.id
        )
    }
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

@MainActor
@Observable
final class PadLayoutPresetStore {
    static let shared = PadLayoutPresetStore()
    static let schemaVersion = 3

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
    private(set) var setAsideName: String?
    // Set when an unreadable file could not be moved aside, so nothing writes over it.
    private(set) var readOnly = false
    @ObservationIgnored private var writeError: Error?

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
            // @Observable runs didSet in init, so the setter would rewrite the file before the assignments load.
            _globalPresetID = validPresetID(library.globalPresetID)
            gameAssignments = sanitizedAssignments(library.gameAssignments)
        case .missing:
            if migrateFromCurrentLayout {
                migrateCurrentLayout(currentLayout)
            }
        case .corrupt:
            let aside = self.libraryURL.deletingPathExtension()
                .appendingPathExtension("unreadable-\(Int(Date().timeIntervalSince1970)).json")
            do {
                try FileManager.default.moveItem(at: self.libraryURL, to: aside)
                setAsideName = aside.lastPathComponent
                if migrateFromCurrentLayout {
                    migrateCurrentLayout(currentLayout)
                }
            } catch {
                readOnly = true
                NSLog("[ARMSX2 iOS VPad] Layouts file unreadable and not moved aside, saving is off: %@", error.localizedDescription)
            }
        }
    }

    @discardableResult
    func createPreset(
        named name: String,
        snapshot: PadLayoutSnapshot,
        source: PadLayoutPresetSource = .user,
        linkedSkinID: String? = nil,
        classSnapshots: [String: PadLayoutSnapshot]? = nil
    ) -> PadLayoutPreset {
        let now = Date()
        let preset = PadLayoutPreset(
            id: UUID().uuidString,
            displayName: sanitizedName(name, fallback: "Layout"),
            createdAt: now,
            updatedAt: now,
            source: source,
            linkedSkinID: linkedSkinID,
            snapshot: snapshot,
            classSnapshots: classSnapshots
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
            snapshot: imported.snapshot,
            classSnapshots: imported.classSnapshots
        )
    }

    /// With a class, saves that class's own copy and leaves the shared base and the other classes alone.
    func updatePreset(id: String, snapshot: PadLayoutSnapshot, for deviceClass: PadDeviceClass? = nil) throws {
        guard let index = presets.firstIndex(where: { $0.id == id }) else {
            throw PadLayoutPresetStoreError.missingPreset
        }
        if let deviceClass {
            var copies = presets[index].classSnapshots ?? [:]
            copies[deviceClass.rawValue] = snapshot
            presets[index].classSnapshots = copies
        } else {
            presets[index].snapshot = snapshot
        }
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
            linkedSkinID: original.linkedSkinID,
            classSnapshots: original.classSnapshots
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

    /// Catalog skins match by disc serial but the runtime identity adds a CRC,
    /// so the assignment stays at serial scope and another CRC never falls back
    /// to Global Default. Accepting a skin replaces stale exact-CRC skin picks;
    /// exact layout picks stay unless a valid catalog layout came with it.
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
        )?.snapshot(for: .current)
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
        guard !readOnly else { throw PadLayoutPresetStoreError.readOnly }
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

    /// Throws when the last write in `body` failed; each write stores the whole library, earlier changes included.
    func reportingWriteFailure<T>(_ body: () throws -> T) throws -> T {
        writeError = nil
        let result = try body()
        if let writeError { throw writeError }
        return result
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
        guard !readOnly else { return }
        do {
            try save()
            writeError = nil
        } catch {
            writeError = error
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
    case readOnly
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }
}
