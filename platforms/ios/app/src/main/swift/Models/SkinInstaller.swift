// SkinInstaller.swift — downloads a skin zip and installs it via VPadSkinLibraryStore
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// Downloads a skin zip from the catalog repo and installs it through the existing
/// `VPadSkinLibraryStore.importSkin(from:)` path — which handles extraction, images,
/// and the linked layout preset in one call (validated by spike 009).
@MainActor
final class SkinInstaller: ObservableObject {
    @Published private(set) var installingName: String?
    @Published var errors: [String: String] = [:]

    let catalog: SkinCatalog

    init(catalog: SkinCatalog) {
        self.catalog = catalog
    }

    func install(_ skin: CatalogSkin) async {
        installingName = skin.name
        errors[skin.name] = nil
        do {
            let zipURL = catalog.zipURL(for: skin)
            let (tempURL, response) = try await URLSession.shared.download(from: zipURL)
            if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                throw URLError(.badServerResponse)
            }
            _ = try VPadSkinLibraryStore.shared.importSkin(
                from: tempURL,
                originalImportName: skin.name,
                layoutPresets: .shared
            )
            try? FileManager.default.removeItem(at: tempURL)
        } catch {
            errors[skin.name] = error.localizedDescription
        }
        installingName = nil
    }

    func isInstalled(_ skin: CatalogSkin) -> Bool {
        VPadSkinLibraryStore.shared.importedDescriptors.contains { $0.displayName == skin.name }
    }

    func uninstall(_ skin: CatalogSkin) {
        guard let descriptor = VPadSkinLibraryStore.shared.importedDescriptors
            .first(where: { $0.displayName == skin.name }) else { return }
        try? VPadSkinLibraryStore.shared.deleteImportedSkin(id: descriptor.id)
    }
}
