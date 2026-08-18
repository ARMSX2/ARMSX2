// ShaderCatalogInstaller.swift — downloads one catalogue preset and verifies it before writing
// SPDX-License-Identifier: GPL-3.0+

import CryptoKit
import Foundation

enum ShaderCatalogInstallError: LocalizedError {
    case tooLarge(Int)
    case statedNoBytes
    case unusableLink
    case sizeMismatch(expected: Int, received: Int)
    case hashMismatch(expected: String)
    case serverRefused(Int)
    case importFailed(String)

    var errorDescription: String? {
        switch self {
        case .tooLarge(let bytes):
            return "The catalogue states this preset is \(bytes) bytes, which is past what this build will download. Nothing was fetched."
        case .statedNoBytes:
            return "The catalogue states this preset is empty, so nothing was fetched."
        case .unusableLink:
            return "This preset's catalogue path is not usable, so nothing was fetched."
        case .sizeMismatch(let expected, let received):
            return "The download is \(received) bytes and the catalogue states \(expected). Nothing was installed."
        case .hashMismatch(let expected):
            return "The download does not match its SHA-256 of \(expected). Nothing was installed."
        case .serverRefused(let code):
            return "The catalogue server answered with \(code) for this preset. Nothing was installed."
        case .importFailed(let reason):
            return reason
        }
    }
}

/// Written into every installed folder so a later build can tell what a pack came from and at
/// which upstream pin, and hidden so `ShaderPresetLibrary.scan()` never lists it as content.
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
    @Published var errors: [String: String] = [:]
    /// Catalogue ids with a marker on disk, so a row knows itself installed across relaunches.
    @Published private(set) var installed: Set<String> = []

    static let stagingPrefix = "shader-download-"
    private static let maxDownloadBytes = 32 * 1024 * 1024

    private let importer = ShaderPackImporter()

    init() {
        refreshInstalled()
    }

    func refreshInstalled() {
        installed = Set(Self.markers().keys)
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
        // The manifest states the size in advance, so a refusal costs no transfer. The largest
        // zip in the published run is under a megabyte; this is a fence, not a limit.
        guard entry.zip.bytes > 0 else { throw ShaderCatalogInstallError.statedNoBytes }
        guard entry.zip.bytes <= Self.maxDownloadBytes else {
            throw ShaderCatalogInstallError.tooLarge(entry.zip.bytes)
        }
        guard let source = ShaderCatalog.assetURL(entry.zip.path) else {
            throw ShaderCatalogInstallError.unusableLink
        }

        let staged = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(Self.stagingPrefix)\(UUID().uuidString).zip")
        let (temporary, response) = try await URLSession.shared.download(from: source)
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

        // The extract itself is not interruptible, so cancelling during it removes the pack
        // afterwards rather than stopping it. Better than a lie about when cancelling works.
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

    /// `defer` does not run when iOS kills a backgrounded app mid-download, which is the
    /// ordinary outcome rather than an edge case, so the next launch sweeps what it left.
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
