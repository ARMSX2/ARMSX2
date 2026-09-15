// ShaderCatalogInstaller.swift — downloads one catalogue preset and verifies it before writing
// SPDX-License-Identifier: GPL-3.0+

import CryptoKit
import Foundation

enum ShaderCatalogInstallError: LocalizedError {
    case tooLarge(Int)
    case statedNoBytes
    case unusableLink
    case unreachable
    case sizeMismatch(expected: Int, received: Int)
    case hashMismatch(expected: String)
    case serverRefused(Int)
    case importFailed(String)

    var errorDescription: String? {
        switch self {
        case .tooLarge, .statedNoBytes, .unusableLink:
            return "This shader can't be downloaded."
        case .unreachable:
            return "Can't reach the shader server. Check your connection and try again."
        case .sizeMismatch, .hashMismatch:
            return "The download was damaged, so nothing was installed. Try again."
        case .serverRefused:
            return "The shader server didn't send this shader. Try again later."
        case .importFailed(let reason):
            return reason
        }
    }
}

/// Records the catalogue entry and upstream pin a folder came from. Hidden, so
/// `ShaderPresetLibrary.scan()` skips it.
struct ShaderCatalogMarker: Codable {
    let id: String
    let pin: String
    let zipSHA256: String
    let installed: Date

    static let fileName = ".armsx2-catalogue.json"
}

@MainActor
final class ShaderCatalogInstaller: ObservableObject {
    @Published private(set) var installing: Set<String> = []
    @Published private(set) var errors: [String: String] = [:]
    /// Catalogue id to the folder holding its marker, so a row shows installed after a relaunch.
    @Published private(set) var installed: [String: String] = [:]

    static let stagingPrefix = "shader-download-"
    private static let maxDownloadBytes = 32 * 1024 * 1024

    private let importer = ShaderPackImporter()

    init() {
        refreshInstalled()
    }

    func refreshInstalled() {
        installed = Self.markers()
    }

    func presetToken(for entry: ShaderCatalogEntry) -> String? {
        guard let folder = installed[entry.id], SkinAssetPath.isSafeRelative(entry.id),
              let pack = ShaderPresetLibrary.userRoot?.appendingPathComponent(folder, isDirectory: true)
        else { return nil }
        let path = entry.id + "." + ShaderPresetLibrary.presetExtension
        // The extractor drops a top folder every file shares, so crt/crt-geom lands as crt-geom.slangp.
        let stripped = path.firstIndex(of: "/").map { String(path[path.index(after: $0)...]) }
        return [path, stripped].compactMap { $0 }
            .compactMap { ShaderPresetLibrary.token(for: pack.appendingPathComponent($0)) }
            .first { ShaderPresetLibrary.resolve($0) != nil }
    }

    func install(_ entry: ShaderCatalogEntry, pin: String) async {
        installing.insert(entry.id)
        errors[entry.id] = nil
        do {
            try await perform(entry, pin: pin)
        } catch is CancellationError {
            errors[entry.id] = nil
        } catch {
            errors[entry.id] = error.localizedDescription
        }
        installing.remove(entry.id)
        refreshInstalled()
    }

    private func perform(_ entry: ShaderCatalogEntry, pin: String) async throws {
        // The manifest states the size, so an oversized entry is refused before any transfer.
        guard entry.zip.bytes > 0 else { throw ShaderCatalogInstallError.statedNoBytes }
        guard entry.zip.bytes <= Self.maxDownloadBytes else {
            throw ShaderCatalogInstallError.tooLarge(entry.zip.bytes)
        }
        guard let source = ShaderCatalog.assetURL(entry.zip.path) else {
            throw ShaderCatalogInstallError.unusableLink
        }

        let staged = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(Self.stagingPrefix)\(UUID().uuidString).zip")
        let temporary: URL
        let response: URLResponse
        do {
            (temporary, response) = try await URLSession.shared.download(from: source)
        } catch {
            throw ShaderCatalogInstallError.unreachable
        }
        // URLSession hands over a temp file the caller owns. If the move below throws it is
        // still ours and nothing else will remove it; after a successful move it is gone and
        // this is a no-op.
        defer { try? FileManager.default.removeItem(at: temporary) }
        try? FileManager.default.removeItem(at: staged)
        try FileManager.default.moveItem(at: temporary, to: staged)
        defer { try? FileManager.default.removeItem(at: staged) }
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw ShaderCatalogInstallError.serverRefused(http.statusCode)
        }
        try Task.checkCancellation()

        let received = Int(((try? FileManager.default.attributesOfItem(atPath: staged.path))?[.size]
            as? NSNumber)?.int64Value ?? 0)
        guard received == entry.zip.bytes else {
            throw ShaderCatalogInstallError.sizeMismatch(
                expected: entry.zip.bytes, received: received)
        }
        let digest = try Self.sha256(of: staged)
        guard digest == entry.zip.sha256.lowercased() else {
            throw ShaderCatalogInstallError.hashMismatch(expected: entry.zip.sha256)
        }

        // Not the published property: two rows install at once and it holds only the last.
        let landed = await importer.install(archiveAt: staged, named: entry.name)
        guard let name = landed else {
            if let reason = importer.errors[staged.lastPathComponent] {
                throw ShaderCatalogInstallError.importFailed(reason)
            }
            throw ShaderPackImportError.notAShaderPack
        }
        let folder = ShaderPresetLibrary.userRoot?.appendingPathComponent(name, isDirectory: true)

        // Extraction can't be interrupted, so a cancel during it removes the pack afterwards.
        if Task.isCancelled {
            if let folder { try? FileManager.default.removeItem(at: folder) }
            throw CancellationError()
        }
        guard let folder else { throw ShaderPackImportError.noUserRoot }
        Self.writeMarker(
            ShaderCatalogMarker(
                id: entry.id, pin: pin, zipSHA256: digest, installed: Date()),
            into: folder)
    }

    // MARK: - Markers

    static func markers() -> [String: String] {
        guard let root = ShaderPresetLibrary.userRoot,
              let children = try? FileManager.default.contentsOfDirectory(
                at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
        else { return [:] }
        var found: [String: String] = [:]
        for folder in children {
            let file = folder.appendingPathComponent(ShaderCatalogMarker.fileName)
            guard let data = try? Data(contentsOf: file),
                  let marker = try? JSONDecoder().decode(ShaderCatalogMarker.self, from: data)
            else { continue }
            found[marker.id] = folder.lastPathComponent
        }
        return found
    }

    private static func writeMarker(_ marker: ShaderCatalogMarker, into folder: URL) {
        guard let data = try? JSONEncoder().encode(marker) else { return }
        try? data.write(
            to: folder.appendingPathComponent(ShaderCatalogMarker.fileName), options: .atomic)
    }

    // MARK: - Staging

    /// Removes downloads left in tmp when iOS killed the app mid-download, where defer never ran.
    static func sweepStagedDownloads() {
        let temporary = FileManager.default.temporaryDirectory
        let contents = (try? FileManager.default.contentsOfDirectory(
            at: temporary, includingPropertiesForKeys: nil)) ?? []
        for url in contents where url.lastPathComponent.hasPrefix(stagingPrefix) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    private nonisolated static func sha256(of url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hasher = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
}
