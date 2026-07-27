// SkinInstaller.swift — downloads a skin zip and installs it via VPadSkinLibraryStore
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum SkinInstallError: LocalizedError {
    case unusableLink
    case tooLarge
    case emptyArchive

    var errorDescription: String? {
        switch self {
        case .unusableLink:
            return "This skin's download link is not usable."
        case .tooLarge:
            return "This skin is too large to install."
        case .emptyArchive:
            return "The download did not contain any usable skin files."
        }
    }
}

/// Downloads a skin zip from the catalog repo, extracts it via the bridge, and
/// installs through VPadSkinLibraryStore.importSkin(from: directoryURL).
@MainActor
final class SkinInstaller: ObservableObject {
    /// Keyed by the catalog file path, so two taps on different rows do not
    /// knock each other's spinner out.
    @Published private(set) var installing: Set<String> = []
    @Published var errors: [String: String] = [:]
    /// Warnings the import raised but which are not failures, keyed the same way.
    @Published var notices: [String: String] = [:]

    private static let maxDownloadBytes: Int64 = 32 * 1024 * 1024

    func install(_ skin: CatalogSkin) async {
        await install(skin, replacing: nil)
    }

    func reinstall(_ skin: CatalogSkin) async {
        await install(skin, replacing: installedDescriptor(for: skin)?.id)
    }

    func uninstall(_ skin: CatalogSkin) {
        errors[skin.file] = nil
        notices[skin.file] = nil
        guard let descriptor = installedDescriptor(for: skin) else { return }
        do {
            try VPadSkinLibraryStore.shared.deleteImportedSkin(id: descriptor.id)
            syncSelectedSkin()
        } catch {
            errors[skin.file] = error.localizedDescription
        }
    }

    private func install(_ skin: CatalogSkin, replacing replacingSkinID: String?) async {
        installing.insert(skin.file)
        errors[skin.file] = nil
        notices[skin.file] = nil
        do {
            guard let zipURL = SkinCatalog.zipURL(for: skin) else {
                throw SkinInstallError.unusableLink
            }
            let (tempURL, response) = try await URLSession.shared.download(from: zipURL)
            if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                throw URLError(.badServerResponse)
            }

            // Give the temp file a .zip name so the bridge recognises it. The
            // name is remote-supplied and lands in a path component, so it stays
            // out of this.
            let zipFile = FileManager.default.temporaryDirectory
                .appendingPathComponent("skin-download-\(UUID().uuidString).zip")
            try FileManager.default.moveItem(at: tempURL, to: zipFile)
            defer { try? FileManager.default.removeItem(at: zipFile) }

            // This bounds what gets imported, not what the transfer already wrote
            // to disk — capping mid-transfer needs a URLSessionDownloadDelegate.
            // The largest legitimate catalog zip is a little over 3 MB.
            let size = ((try? FileManager.default.attributesOfItem(atPath: zipFile.path))?[.size] as? NSNumber)?.int64Value ?? 0
            guard size <= Self.maxDownloadBytes else {
                throw SkinInstallError.tooLarge
            }

            // Extract the zip to a temp directory — importSkin expects a
            // directory of loose files, not a raw zip archive.
            let extractDir = FileManager.default.temporaryDirectory
                .appendingPathComponent("skin-import-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: extractDir, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: extractDir) }

            let extracted = ARMSX2Bridge.extractControllerSkinArchive(at: zipFile, to: extractDir)
            guard !extracted.isEmpty else {
                throw SkinInstallError.emptyArchive
            }

            let result = try await VPadSkinLibraryStore.shared.importSkin(
                from: extractDir,
                originalImportName: skin.name,
                catalogID: skin.file,
                replacingSkinID: replacingSkinID,
                layoutPresets: .shared
            )
            if !result.warnings.isEmpty {
                notices[skin.file] = result.warnings.joined(separator: "\n")
            }
            syncSelectedSkin()
        } catch {
            errors[skin.file] = error.localizedDescription
        }
        installing.remove(skin.file)
    }

    private func installedDescriptor(for skin: CatalogSkin) -> VPadSkinDescriptor? {
        VPadSkinLibraryStore.shared.importedDescriptors.first { $0.catalogID == skin.file }
    }

    /// The library resets a dangling selection on its own, but nothing keeps
    /// SettingsStore's mirror in step — and a stale .custom there points the pad
    /// at the old ControllerSkins/Custom folder instead of the stock art.
    private func syncSelectedSkin() {
        SettingsStore.shared.virtualPadSkin = VPadSkinLibraryStore.shared.selectedDescriptor.virtualPadSkin
    }
}
