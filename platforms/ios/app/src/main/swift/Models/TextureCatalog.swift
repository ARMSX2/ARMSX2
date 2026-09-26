// TextureCatalog.swift — the texture pack catalogue Android downloads from
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct TextureCatalogPack: Identifiable, Equatable, Sendable {
    let id: String
    let name: String
    let gameTitle: String
    let serials: [String]
    let authors: [String]
    let source: URL
    let download: URL
    let bytes: Int64
    let unpackedBytes: Int64
    let sha256: String
    let isTarZstd: Bool

    var fileName: String { id + (isTarZstd ? ".tar.zst" : ".zip") }
}

enum TextureCatalog {
    // Tried in order: bmdhacks' ASTC catalogue, then mirrors of sashkinbro's.
    static let sources = [
        "https://dl.ps2ktxpak.net/textures.json",
        "https://raw.githubusercontent.com/sashkinbro/EmuCoreX-Textures/main/textures.json",
        "https://github.com/sashkinbro/EmuCoreX-Textures/raw/main/textures.json",
        "https://cdn.jsdelivr.net/gh/sashkinbro/EmuCoreX-Textures@main/textures.json",
    ].compactMap { URL(string: $0) }

    static func load() async -> [TextureCatalogPack]? {
        for url in sources {
            guard let result = try? await URLSession.shared.data(from: url),
                  (result.1 as? HTTPURLResponse)?.statusCode == 200, result.0.count <= 8 << 20,
                  let packs = parse(result.0) else { continue }
            return packs
        }
        return nil
    }

    // Android's rules: an unknown schema or nothing usable rejects the source, so the next mirror
    // gets its turn, while one bad entry only drops itself.
    static func parse(_ data: Data) -> [TextureCatalogPack]? {
        guard let root = try? JSONDecoder().decode(Root.self, from: data),
              let schema = root.schemaVersion, (1...2).contains(schema) else { return nil }
        var seen = Set<String>()
        let packs = (root.entries ?? []).compactMap { $0.value.flatMap { pack($0, schema: schema) } }
            .filter { seen.insert($0.id).inserted }
        return packs.isEmpty ? nil : packs
    }

    // Android installs under the first serial, which misses a PAL owner of a multi-region pack.
    static func serial(for pack: TextureCatalogPack, owned: Set<String>) -> String {
        pack.serials.first { owned.contains($0) } ?? pack.serials[0]
    }

    static func normalizedSerial(_ raw: String) -> String? {
        let compact = raw.uppercased().filter { $0.isASCII && ($0.isLetter || $0.isNumber) }
        guard compact.count == 9, compact.prefix(4).allSatisfy(\.isLetter), compact.suffix(5).allSatisfy(\.isNumber) else {
            return nil
        }
        return "\(compact.prefix(4))-\(compact.suffix(5))"
    }

    private static func pack(_ e: Entry, schema: Int) -> TextureCatalogPack? {
        let trimmed = { (s: String?) in s?.trimmingCharacters(in: .whitespaces) ?? "" }
        let https = { (s: String?) in URL(string: trimmed(s)).flatMap { $0.scheme == "https" ? $0 : nil } }
        let format = trimmed(e.format).isEmpty ? "zip" : trimmed(e.format).lowercased()
        let unpacked = e.decompressedSizeBytes ?? 0
        let digest = trimmed(e.sha256).lowercased()
        let serials = (e.serials ?? []).compactMap(normalizedSerial)
        let authors = (e.authors ?? []).map { trimmed($0) }.filter { !$0.isEmpty }

        // The id names the downloaded file and the install marker, so it has to be a plain name.
        let id = trimmed(e.id)
        let plain = id.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "._-".contains($0)) }

        // Split archives need joining, which this build does not do yet; the B2 catalogue has none.
        guard !id.isEmpty, plain, !trimmed(e.name).isEmpty, e.parts == nil,
              let download = https(e.downloadUrl), let source = https(e.sourceUrl),
              let bytes = e.sizeBytes, (1...(4 << 30)).contains(bytes),
              digest.count == 64, digest.allSatisfy(\.isHexDigit), !serials.isEmpty, !authors.isEmpty,
              schema == 1 ? format == "zip" : (format == "tar+zstd" && (1...(16 << 30)).contains(unpacked))
        else { return nil }

        return TextureCatalogPack(
            id: id, name: trimmed(e.name), gameTitle: trimmed(e.gameTitle), serials: serials,
            authors: authors, source: source, download: download, bytes: bytes,
            unpackedBytes: schema == 1 ? 0 : unpacked, sha256: digest, isTarZstd: schema != 1)
    }

    private struct Root: Decodable {
        let schemaVersion: Int?
        let entries: [Lenient<Entry>]?
    }

    private struct Entry: Decodable {
        let id, name, gameTitle, downloadUrl, sourceUrl, sha256, format: String?
        let serials, authors: [String]?
        let sizeBytes, decompressedSizeBytes: Int64?
        let parts: [Ignored]?
    }

    private struct Ignored: Decodable {}

    private struct Lenient<T: Decodable>: Decodable {
        let value: T?
        init(from decoder: Decoder) throws { value = try? T(from: decoder) }
    }
}
