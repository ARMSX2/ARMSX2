// SkinInstaller.swift — downloads a skin zip and installs it via VPadSkinLibraryStore
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// Downloads a skin zip from the catalog repo, extracts it via the bridge, and
/// installs through VPadSkinLibraryStore.importSkin(from: directoryURL).
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

            // Give the temp file a .zip name so the bridge recognises it.
            let zipFile = FileManager.default.temporaryDirectory
                .appendingPathComponent("\(skin.name)-\(UUID().uuidString).zip")
            try FileManager.default.moveItem(at: tempURL, to: zipFile)
            defer { try? FileManager.default.removeItem(at: zipFile) }

            // Extract the zip to a temp directory — importSkin expects a
            // directory of loose files, not a raw zip archive.
            let extractDir = FileManager.default.temporaryDirectory
                .appendingPathComponent("skin-import-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: extractDir, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: extractDir) }

            let extracted = ARMSX2Bridge.extractControllerSkinArchive(at: zipFile, to: extractDir)
            guard !extracted.isEmpty else {
                throw URLError(.cannotDecodeContentData)
            }

            _ = try VPadSkinLibraryStore.shared.importSkin(
                from: extractDir,
                originalImportName: skin.name,
                layoutPresets: .shared
            )
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
