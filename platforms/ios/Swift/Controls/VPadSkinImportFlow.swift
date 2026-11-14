// VPadSkinImportFlow.swift — custom skin import and its replace prompt
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import Observation

struct SkinReplacePrompt: Identifiable {
    let id = UUID()
    let name: String
    let existingSkinID: String
}

enum SkinReplaceChoice {
    case replace(String)
    case keepBoth
    case cancel
}

/// Holds the answer the import is waiting on. The view owns one, so leaving the
/// screen with the dialog still up unblocks the import on the way out instead of
/// parking it on a continuation nobody is left to resume.
@MainActor
private final class SkinReplaceGate {
    private var continuation: CheckedContinuation<SkinReplaceChoice, Never>?

    func wait() async -> SkinReplaceChoice {
        await withCheckedContinuation { continuation in
            self.continuation = continuation
        }
    }

    /// Answers at most once, whichever of the buttons or the dismissal gets here first.
    func resume(_ choice: SkinReplaceChoice) {
        guard let continuation else { return }
        self.continuation = nil
        continuation.resume(returning: choice)
    }

    deinit {
        continuation?.resume(returning: .cancel)
    }
}

@MainActor
@Observable
final class VPadSkinImportFlow {
    var skinReplacePrompt: SkinReplacePrompt?
    @ObservationIgnored private var skinReplaceGate = SkinReplaceGate()
    @ObservationIgnored private let skinLibrary = VPadSkinLibraryStore.shared
    @ObservationIgnored private let layoutPresets = PadLayoutPresetStore.shared

    func importCustomSkins(_ urls: [URL]) async -> (message: String, importResult: VPadSkinImportResult?) {
        let stagingDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("ARMSX2SkinImport-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: stagingDirectory, withIntermediateDirectories: true)
        defer {
            try? FileManager.default.removeItem(at: stagingDirectory)
        }

        var messages: [String] = []
        var latestResult: VPadSkinImportResult?
        let looseFiles = urls.filter { !isSkinArchive($0) }
        let archiveFiles = urls.filter { isSkinArchive($0) }

        if !looseFiles.isEmpty {
            let looseDirectory = stagingDirectory.appendingPathComponent("LooseSkin", isDirectory: true)
            try? FileManager.default.createDirectory(at: looseDirectory, withIntermediateDirectories: true)
            for sourceURL in looseFiles {
                let accessGranted = sourceURL.startAccessingSecurityScopedResource()
                defer {
                    if accessGranted {
                        sourceURL.stopAccessingSecurityScopedResource()
                    }
                }
                let destination = looseDirectory.appendingPathComponent(sourceURL.lastPathComponent)
                try? FileManager.default.removeItem(at: destination)
                try? FileManager.default.copyItem(at: sourceURL, to: destination)
            }
            do {
                let result = try await skinLibrary.importSkin(
                    from: looseDirectory,
                    originalImportName: looseFiles.first?.lastPathComponent,
                    layoutPresets: layoutPresets
                )
                latestResult = result
                messages.append(result.message)
            } catch {
                messages.append("Skin import failed: \(error.localizedDescription)")
            }
        }

        for sourceURL in archiveFiles {
            let accessGranted = sourceURL.startAccessingSecurityScopedResource()
            defer {
                if accessGranted {
                    sourceURL.stopAccessingSecurityScopedResource()
                }
            }

            let archiveDirectory = stagingDirectory
                .appendingPathComponent(sourceURL.deletingPathExtension().lastPathComponent, isDirectory: true)
            let isV2Package = SkinManifestImporter.shouldTreatAsV2(
                manifestData: ARMSX2Bridge.peekSkinManifestData(at: sourceURL)
            )
            let extracted: [URL]
            if isV2Package {
                extracted = ARMSX2Bridge.extractSkinPackageArchive(at: sourceURL, to: archiveDirectory)
            } else {
                extracted = ARMSX2Bridge.extractControllerSkinArchive(at: sourceURL, to: archiveDirectory)
            }
            if extracted.isEmpty {
                messages.append("No usable skin files were imported from \(sourceURL.lastPathComponent).")
                continue
            }

            var replacingSkinID: String?
            let intendedName = skinLibrary.intendedDisplayName(forExtractedSkinAt: archiveDirectory)
            if let existing = skinLibrary.existingImportedSkin(matchingName: intendedName) {
                switch await askAboutExistingSkin(named: intendedName, existingSkinID: existing.id) {
                case .replace(let id):
                    replacingSkinID = id
                case .keepBoth:
                    break
                case .cancel:
                    messages.append("Kept the existing '\(intendedName)'.")
                    continue
                }
            }

            do {
                let result = try await skinLibrary.importSkin(
                    from: archiveDirectory,
                    originalImportName: sourceURL.lastPathComponent,
                    replacingSkinID: replacingSkinID,
                    layoutPresets: layoutPresets
                )
                latestResult = result
                messages.append(result.message)
            } catch {
                messages.append("Skin import failed: \(error.localizedDescription)")
            }
        }

        let message = messages.isEmpty
            ? "No usable skin images were imported. Use loose button PNGs/JPGs/WebPs, a portrait/landscape controller image, or a zip skin pack containing image files."
            : messages.joined(separator: "\n\n")
        return (message, latestResult)
    }

    private func askAboutExistingSkin(named name: String, existingSkinID: String) async -> SkinReplaceChoice {
        skinReplacePrompt = SkinReplacePrompt(name: name, existingSkinID: existingSkinID)
        return await skinReplaceGate.wait()
    }

    func resumeSkinReplace(_ choice: SkinReplaceChoice) {
        skinReplaceGate.resume(choice)
    }

    private func isSkinArchive(_ url: URL) -> Bool {
        let ext = url.pathExtension.lowercased()
        return ext == "zip" || ext == "skin" || ext == "manic"
            || ext == "armsx2skin" || ext == "deltaskin" || ext == "manicskin"
    }
}
