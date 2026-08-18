// ShaderPackImporter.swift — installs a picked .zip or folder into Documents/shaders
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ShaderPackImportError: LocalizedError {
    case noUserRoot
    case notAShaderPack

    var errorDescription: String? {
        switch self {
        case .noUserRoot:
            return "The Documents shader folder could not be opened."
        case .notAShaderPack:
            return "That contained no shader presets, so nothing was installed."
        }
    }
}

/// Installs a shader pack under a free name in the user root, then reports what happened.
@MainActor
final class ShaderPackImporter: ObservableObject {
    @Published private(set) var installing: Set<String> = []
    @Published private(set) var errors: [String: String] = [:]
    @Published private(set) var installedName: String?

    var isBusy: Bool { !installing.isEmpty }

    func install(archiveAt source: URL) async {
        await install(source, writing: Self.extract)
    }

    func install(folderAt source: URL) async {
        await install(source, writing: Self.copyTree)
    }

    private func install(
        _ source: URL,
        writing: @escaping @Sendable (URL, URL) throws -> Void
    ) async {
        let key = source.lastPathComponent
        installing.insert(key)
        errors[key] = nil
        installedName = nil
        do {
            // A full RetroArch pack is thousands of files, so this blocks for long enough
            // to stall the settings screen if it runs on the main actor.
            installedName = try await Task.detached(priority: .userInitiated) {
                try Self.perform(source, writing)
            }.value
        } catch {
            errors[key] = error.localizedDescription
        }
        installing.remove(key)
    }

    private nonisolated static func perform(
        _ source: URL,
        _ writing: @Sendable (URL, URL) throws -> Void
    ) throws -> String {
        guard let root = ShaderPresetLibrary.prepareUserRoots() else {
            throw ShaderPackImportError.noUserRoot
        }
        let accessing = source.startAccessingSecurityScopedResource()
        defer { if accessing { source.stopAccessingSecurityScopedResource() } }

        let name = freeName(for: source, under: root)
        let destination = root.appendingPathComponent(name, isDirectory: true)
        do {
            try writing(source, destination)
        } catch {
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
        // A pack folder with nothing selectable in it stays in the browser forever as a
        // dead end, so a refusal the user can read beats keeping what they handed over.
        guard presetCount(under: destination) > 0 else {
            try? FileManager.default.removeItem(at: destination)
            throw ShaderPackImportError.notAShaderPack
        }
        return name
    }

    private nonisolated static func extract(_ source: URL, _ destination: URL) throws {
        let staged = FileManager.default.temporaryDirectory
            .appendingPathComponent("shaderpack-\(UUID().uuidString).zip")
        defer { try? FileManager.default.removeItem(at: staged) }
        try FileManager.default.copyItem(at: source, to: staged)

        var failure: NSError?
        let written = ARMSX2Bridge.extractShaderPackArchive(
            at: staged, to: destination, error: &failure)
        if written.isEmpty {
            if let failure { throw failure }
            throw ShaderPackImportError.notAShaderPack
        }
    }

    /// Copied as it stands, because a .slangp names its stages by relative path and the
    /// tree is therefore part of the pack rather than an arrangement of it.
    private nonisolated static func copyTree(_ source: URL, _ destination: URL) throws {
        try FileManager.default.copyItem(at: source, to: destination)
    }

    private nonisolated static func freeName(for source: URL, under root: URL) -> String {
        let base = readableName(source)
        var candidate = base
        var suffix = 2
        while FileManager.default.fileExists(
            atPath: root.appendingPathComponent(candidate, isDirectory: true).path) {
            candidate = "\(base) (\(suffix))"
            suffix += 1
        }
        return candidate
    }

    private nonisolated static func readableName(_ source: URL) -> String {
        var name = source.lastPathComponent
        if name.lowercased().hasSuffix(".zip") {
            name = String(name.dropLast(4))
        }
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: " _.-"))
        let scalars = name.unicodeScalars.map { allowed.contains($0) ? $0 : Unicode.Scalar("_") }
        let cleaned = String(String.UnicodeScalarView(scalars))
            .trimmingCharacters(in: .whitespaces)
        return cleaned.isEmpty ? "Shader Pack" : cleaned
    }

    private nonisolated static func presetCount(under directory: URL) -> Int {
        guard let walk = FileManager.default.enumerator(
            at: directory, includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]) else { return 0 }
        var found = 0
        while let url = walk.nextObject() as? URL {
            if url.pathExtension.lowercased() == ShaderPresetLibrary.presetExtension {
                found += 1
            }
        }
        return found
    }
}
