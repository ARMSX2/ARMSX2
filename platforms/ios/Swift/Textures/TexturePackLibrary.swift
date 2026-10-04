// TexturePackLibrary.swift — installed replacement texture packs
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct TexturePack: Identifiable, Equatable, Sendable {
    let serial: String
    let folder: URL
    let bytes: Int64

    var id: String { serial }
}

enum TexturePackLibrary {
    static func installed(in root: URL) -> [TexturePack] {
        let fileManager = FileManager.default
        let serialFolders = (try? fileManager.contentsOfDirectory(
            at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])) ?? []
        return serialFolders.compactMap { serialFolder in
            let folder = serialFolder.appendingPathComponent("replacements", isDirectory: true)
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: folder.path, isDirectory: &isDirectory), isDirectory.boolValue else {
                return nil
            }
            return TexturePack(serial: serialFolder.lastPathComponent, folder: folder, bytes: size(of: folder))
        }
    }

    // Dumps live next to the pack, so the serial folder only goes once nothing else is in it.
    static func remove(_ pack: TexturePack) throws {
        let fileManager = FileManager.default
        try fileManager.removeItem(at: pack.folder)
        let serialFolder = pack.folder.deletingLastPathComponent()
        if (try? fileManager.contentsOfDirectory(atPath: serialFolder.path))?.isEmpty == true {
            try? fileManager.removeItem(at: serialFolder)
        }
    }

    static func catalogIDs(in root: URL) -> Set<String> {
        let serialFolders = (try? FileManager.default.contentsOfDirectory(
            at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])) ?? []
        return Set(serialFolders.flatMap { markedIDs(in: $0.appendingPathComponent("replacements", isDirectory: true)) })
    }

    // Catalog packs can be downloaded again, so they stay out of iCloud backup. An import can't, and
    // a folder that ever took one stays in backup, whichever came first.
    static func markCatalogPack(_ id: String, in folder: URL) throws {
        try Set(markedIDs(in: folder) + [id]).sorted().joined(separator: "\n")
            .write(to: folder.appendingPathComponent(catalogMarker), atomically: true, encoding: .utf8)
        try setExcludedFromBackup(!FileManager.default.fileExists(atPath: folder.appendingPathComponent(importMarker).path), folder)
    }

    static func markImport(in folder: URL) throws {
        try Data().write(to: folder.appendingPathComponent(importMarker))
        try setExcludedFromBackup(false, folder)
    }

    private static func setExcludedFromBackup(_ excluded: Bool, _ folder: URL) throws {
        var values = URLResourceValues()
        values.isExcludedFromBackup = excluded
        var folder = folder
        try folder.setResourceValues(values)
    }

    static func sweepStaging(in root: URL) {
        let leftovers = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? []
        for url in leftovers where url.lastPathComponent.hasPrefix(".import-") {
            try? FileManager.default.removeItem(at: url)
        }
    }

    // GameLibrarySnapshot persists this cache; reading it names packs without opening any disc.
    static func titlesBySerial() -> [String: String] {
        struct Entry: Decodable { let metadata: [String: String] }
        guard let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first,
              let data = try? Data(contentsOf: base.appendingPathComponent("LibraryMetadataCache.json")),
              let entries = try? JSONDecoder().decode([String: Entry].self, from: data) else {
            return [:]
        }
        var titles: [String: String] = [:]
        for entry in entries.values {
            if let serial = entry.metadata["serial"], let title = entry.metadata["title"], !serial.isEmpty, !title.isEmpty {
                titles[serial.uppercased()] = title
            }
        }
        return titles
    }

    private static let catalogMarker = ".armsx2-catalog"
    private static let importMarker = ".armsx2-import"

    private static func markedIDs(in folder: URL) -> [String] {
        let text = (try? String(contentsOf: folder.appendingPathComponent(catalogMarker), encoding: .utf8)) ?? ""
        return text.split(separator: "\n").map(String.init)
    }

    private static func size(of folder: URL) -> Int64 {
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .totalFileAllocatedSizeKey]
        guard let enumerator = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: Array(keys)) else {
            return 0
        }
        var total: Int64 = 0
        for case let url as URL in enumerator {
            let values = try? url.resourceValues(forKeys: keys)
            if values?.isRegularFile == true {
                total += Int64(values?.totalFileAllocatedSize ?? 0)
            }
        }
        return total
    }
}
