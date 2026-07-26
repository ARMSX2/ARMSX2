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
    case fetchFailed(URL, Error)
    case decodeFailed(Error)

    var errorDescription: String? {
        switch self {
        case .fetchFailed(let url, let error):
            return "Could not load the skin catalog: \(error.localizedDescription)"
        case .decodeFailed(let error):
            return "The skin catalog format changed: \(error.localizedDescription)"
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
                throw URLError(.badServerResponse)
            }
            let manifest = try JSONDecoder().decode(SkinCatalogManifest.self, from: data)
            guard !Task.isCancelled else { return }
            skins = manifest.skins
            lastUpdated = Date()
        } catch {
            guard !Task.isCancelled else { return }
            lastError = error.localizedDescription
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
