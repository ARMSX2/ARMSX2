// ShaderCatalog.swift — the downloadable RetroArch preset index, cached for offline browsing
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct ShaderCatalogArchive: Codable, Equatable, Sendable {
    let path: String
    let bytes: Int
    let sha256: String
}

/// The manifest's per-entry `files` array is 96% of its bytes. Not declaring the key is what
/// keeps the cache near 300 KB instead of 8 MB; the zip's own sha256 covers every file in it.
struct ShaderCatalogEntry: Identifiable, Codable, Equatable, Sendable {
    let id: String
    let name: String
    let category: String
    let passes: Int
    let closureBytes: Int
    let zip: ShaderCatalogArchive

    enum CodingKeys: String, CodingKey {
        case id, name, category, passes, zip
        case closureBytes = "closure_bytes"
    }
}

struct ShaderCatalogManifest: Codable, Sendable {
    let schema: Int
    let pin: String
    let generated: String
    let entries: [ShaderCatalogEntry]
}

enum ShaderCatalogError: LocalizedError {
    case unreachable
    case serverError(Int)
    case malformed
    case unsupportedSchema(Int)

    var errorDescription: String? {
        switch self {
        case .unreachable:
            return "Can't reach the shader catalogue. Check your connection and pull down to try again."
        case .serverError(let code):
            return "The shader catalogue server answered with \(code). Pull down to try again."
        case .malformed:
            return "The shader catalogue downloaded fine but can't be read. The file itself is broken, so this needs fixing where it is published rather than here."
        case .unsupportedSchema(let schema):
            return "This catalogue is published in format \(schema), which this build does not read. Update ARMSX2."
        }
    }
}

/// One manifest and one zip per preset. Nothing here walks a preset's file closure over the
/// network — the closure is resolved off-device and shipped inside the zip.
@MainActor
final class ShaderCatalog: ObservableObject {
    static let schema = 1
    /// The repository has to exist before this feature works. Nothing is published there yet.
    nonisolated static let defaultBase = "https://raw.githubusercontent.com/J1coding/ARMSX2-Shaders/main"
    nonisolated static let overrideSection = "EmuCore/GS"
    nonisolated static let overrideKey = "ShaderCatalogueBase"

    /// Read once. No UI writes it; it exists so a simulator can be pointed at a local emit,
    /// which is the only way to exercise this without a host, since ATS refuses plain HTTP.
    nonisolated static let base: URL = resolvedBase()

    @Published private(set) var entries: [ShaderCatalogEntry] = []
    @Published private(set) var isLoading = false
    @Published private(set) var lastUpdated: Date?
    /// The upstream slang-shaders commit every zip was cut from, recorded in each install.
    @Published private(set) var pin = ""
    @Published private(set) var isStale = false
    @Published var lastError: String?

    private var inFlight: Task<Void, Never>?

    func load(force: Bool = false) async {
        inFlight?.cancel()
        let task = Task { await self.run(force: force) }
        inFlight = task
        await task.value
        if inFlight == task {
            inFlight = nil
            isLoading = false
        }
    }

    private func run(force: Bool) async {
        if entries.isEmpty, let cached = Self.readCache() {
            entries = cached.entries
            lastUpdated = cached.written
            pin = cached.pin
        }
        isLoading = true
        lastError = nil
        do {
            let manifest = try await fetch(force: force)
            guard !Task.isCancelled else { return }
            entries = manifest.entries
            pin = manifest.pin
            lastUpdated = Date()
            isStale = false
            Self.writeCache(manifest)
        } catch {
            guard !Task.isCancelled else { return }
            // Entries already on screen came from disk, so a failed refresh ages them rather
            // than emptying the list. It is a banner, not an error state.
            if entries.isEmpty {
                lastError = (error as? ShaderCatalogError ?? .unreachable).localizedDescription
            } else {
                isStale = true
            }
        }
    }

    private func fetch(force: Bool) async throws -> ShaderCatalogManifest {
        var request = URLRequest(url: Self.manifestURL)
        if force {
            request.cachePolicy = .reloadIgnoringLocalCacheData
        }
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch {
            throw ShaderCatalogError.unreachable
        }
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw ShaderCatalogError.serverError(http.statusCode)
        }
        // Reaching it, reading it, and reading a format this build knows are three failures.
        let manifest: ShaderCatalogManifest
        do {
            manifest = try JSONDecoder().decode(ShaderCatalogManifest.self, from: data)
        } catch {
            throw ShaderCatalogError.malformed
        }
        guard manifest.schema == Self.schema else {
            throw ShaderCatalogError.unsupportedSchema(manifest.schema)
        }
        return manifest
    }

    // MARK: - URLs

    static var manifestURL: URL {
        assetURL("manifest.json") ?? base
    }

    /// `..` and `/` both survive percent-encoding for `.urlPathAllowed`, so a hostile manifest
    /// entry could otherwise walk the path off the catalogue root.
    static func assetURL(_ path: String) -> URL? {
        guard SkinAssetPath.isSafeRelative(path) else { return nil }
        let encoded = path.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? path
        var root = base.absoluteString
        while root.hasSuffix("/") { root.removeLast() }
        return URL(string: "\(root)/\(encoded)")
    }

    private nonisolated static func resolvedBase() -> URL {
        let fallback = URL(string: defaultBase)!
        let override = ARMSX2Bridge.getINIString(
            overrideSection, key: overrideKey, defaultValue: ""
        ).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !override.isEmpty, let url = URL(string: override),
              url.scheme == "https" || url.scheme == "file" else { return fallback }
        return url
    }

    // MARK: - Cache

    private struct CachedCatalogue: Codable {
        let schema: Int
        let pin: String
        let generated: String
        let written: Date
        let entries: [ShaderCatalogEntry]
    }

    // Caches, not Documents: this is re-fetchable and must not go to iCloud.
    static var cacheDirectory: URL? {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?
            .appendingPathComponent("shader-catalogue", isDirectory: true)
    }

    static var cacheFile: URL? {
        cacheDirectory?.appendingPathComponent("index.json")
    }

    private static func readCache() -> CachedCatalogue? {
        guard let file = cacheFile, let data = try? Data(contentsOf: file),
              let cached = try? JSONDecoder().decode(CachedCatalogue.self, from: data),
              cached.schema == schema else { return nil }
        return cached
    }

    /// Written as this build's own slim projection rather than the served bytes, so the cache
    /// costs what the app reads and not what the publisher sends.
    private static func writeCache(_ manifest: ShaderCatalogManifest) {
        guard let directory = cacheDirectory, let file = cacheFile else { return }
        let cached = CachedCatalogue(
            schema: manifest.schema, pin: manifest.pin, generated: manifest.generated,
            written: Date(), entries: manifest.entries)
        guard let data = try? JSONEncoder().encode(cached) else { return }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try? data.write(to: file, options: .atomic)
    }
}
