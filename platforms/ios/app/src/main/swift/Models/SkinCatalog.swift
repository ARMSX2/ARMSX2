// SkinCatalog.swift — fetches the community skin catalog from the ARMSX2 skins repo
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct CatalogSkin: Identifiable, Codable, Equatable {
    var id: String { name }
    let name: String
    let file: String
    let preview: String?
    let author: String?
    let buttons: Int?
    let sizeBytes: Int?
    let iosLayout: String?

    var isIOSReady: Bool { iosLayout != nil }
}

struct SkinCatalogManifest: Codable {
    let skins: [CatalogSkin]
}

enum SkinCatalogError: LocalizedError {
    case unreachable
    case serverError(Int)
    case malformed

    var errorDescription: String? {
        switch self {
        case .unreachable:
            return "Can't reach the skin catalog. Check your connection and pull down to try again."
        case .serverError(let code):
            return "The skin catalog server answered with \(code). Pull down to try again."
        case .malformed:
            return "The skin catalog downloaded fine but can't be read. The file itself is broken, so this needs fixing in the skins repo rather than here."
        }
    }
}

/// Fetches the community skin catalog (manifest.json) from the ARMSX2 skins repo.
@MainActor
final class SkinCatalog: ObservableObject {
    static let repo = "bagasromadon/ARMSX2-CustomControllerSkins"
    static let manifestURL = URL(string: "https://raw.githubusercontent.com/\(repo)/main/manifest.json")!
    static let rawBase = "https://raw.githubusercontent.com/\(repo)/main"

    @Published private(set) var skins: [CatalogSkin] = []
    @Published private(set) var isLoading = false
    @Published private(set) var lastUpdated: Date?
    @Published var lastError: String?

    private var inFlight: Task<Void, Never>?

    func fetch(force: Bool = false) async {
        inFlight?.cancel()
        let task = Task { await self.load(force: force) }
        inFlight = task
        await task.value
        // Only the newest fetch owns the spinner; a superseded one leaves it be.
        if inFlight == task {
            inFlight = nil
            isLoading = false
        }
    }

    private func load(force: Bool) async {
        isLoading = true
        lastError = nil
        do {
            var request = URLRequest(url: Self.manifestURL)
            // raw.githubusercontent.com serves max-age=300, so an ordinary
            // refresh inside that window never leaves the URL cache and newly
            // published skins stay invisible for five minutes.
            if force {
                request.cachePolicy = .reloadIgnoringLocalCacheData
            }
            let (data, response) = try await URLSession.shared.data(for: request)
            if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                throw SkinCatalogError.serverError(http.statusCode)
            }
            guard !Task.isCancelled else { return }
            // Reaching the file and being able to read it are different failures. One
            // missing comma upstream used to come out as "check your connection", which
            // sent people hunting a network problem they did not have.
            let manifest: SkinCatalogManifest
            do {
                manifest = try JSONDecoder().decode(SkinCatalogManifest.self, from: data)
            } catch {
                throw SkinCatalogError.malformed
            }
            skins = manifest.skins
            lastUpdated = Date()
        } catch {
            guard !Task.isCancelled else { return }
            lastError = (error as? SkinCatalogError ?? .unreachable).localizedDescription
        }
    }

    static func zipURL(for skin: CatalogSkin) -> URL? {
        assetURL(skin.file)
    }

    static func previewURL(for skin: CatalogSkin) -> URL? {
        guard let preview = skin.preview else { return nil }
        return assetURL(preview)
    }

    /// `..` and `/` both survive percent-encoding for `.urlPathAllowed`, so a
    /// hostile manifest entry could otherwise walk the path off the repo.
    private static func assetURL(_ path: String) -> URL? {
        guard SkinAssetPath.isSafeRelative(path) else { return nil }
        let encoded = path.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? path
        return URL(string: "\(rawBase)/\(encoded)")
    }
}
