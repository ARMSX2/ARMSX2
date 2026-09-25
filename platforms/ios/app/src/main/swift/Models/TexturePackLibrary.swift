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
