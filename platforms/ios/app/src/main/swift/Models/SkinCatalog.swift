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

/// Fetches the community skin catalog (manifest.json) from the ARMSX2 skins repo
/// and filters to iOS-ready skins (those with an `iosLayout` field).
@MainActor
final class SkinCatalog: ObservableObject {
    static let repo = "bagasromadon/ARMSX2-CustomControllerSkins"
    static let manifestURL = URL(string: "https://raw.githubusercontent.com/\(repo)/main/manifest.json")!
    static let rawBase = "https://raw.githubusercontent.com/\(repo)/main"

    @Published private(set) var skins: [CatalogSkin] = []
    @Published private(set) var isLoading = false
    @Published var lastError: String?

    func fetch() async {
        isLoading = true
        lastError = nil
        do {
            let (data, response) = try await URLSession.shared.data(from: Self.manifestURL)
            if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                throw URLError(.badServerResponse)
            }
            let manifest = try JSONDecoder().decode(SkinCatalogManifest.self, from: data)
            skins = manifest.skins.filter { $0.isIOSReady }
        } catch {
            lastError = error.localizedDescription
        }
        isLoading = false
    }

    func zipURL(for skin: CatalogSkin) -> URL {
        let encoded = skin.file.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? skin.file
        return URL(string: "\(Self.rawBase)/\(encoded)")!
    }

    func previewURL(for skin: CatalogSkin) -> URL? {
        guard let preview = skin.preview else { return nil }
        let encoded = preview.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? preview
        return URL(string: "\(Self.rawBase)/\(encoded)")
    }
}
