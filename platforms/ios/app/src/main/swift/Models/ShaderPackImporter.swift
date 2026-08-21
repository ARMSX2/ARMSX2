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

    /// `named` overrides the archive's own name, for a caller whose staging file is a UUID.
    ///
    /// The name is returned as well as published. `installedName` is one property on a shared
    /// importer, so two installs running at once overwrite each other's answer and a caller can
    /// act on the wrong folder. The published copy stays for the settings row that reports the
    /// last install; anything acting on a specific call reads the return value.
    @discardableResult
    func install(archiveAt source: URL, named: String? = nil) async -> String? {
        await install(source, named: named, writing: Self.extract)
    }

    @discardableResult
    func install(folderAt source: URL) async -> String? {
        await install(source, named: nil, writing: Self.copyTree)
    }

    private func install(
        _ source: URL,
        named: String?,
        writing: @escaping @Sendable (URL, URL) throws -> Void
    ) async -> String? {
        let key = source.lastPathComponent
        installing.insert(key)
        errors[key] = nil
        installedName = nil
        var landed: String?
        do {
            // A full RetroArch pack is thousands of files, so this blocks for long enough
            // to stall the settings screen if it runs on the main actor.
            landed = try await Task.detached(priority: .userInitiated) {
                try Self.perform(source, named, writing)
            }.value
            installedName = landed
        } catch {
            errors[key] = error.localizedDescription
        }
        installing.remove(key)
        return landed
    }

    private nonisolated static func perform(
        _ source: URL,
        _ named: String?,
        _ writing: @Sendable (URL, URL) throws -> Void
    ) throws -> String {
        guard let root = ShaderPresetLibrary.prepareUserRoots() else {
            throw ShaderPackImportError.noUserRoot
        }
        let accessing = source.startAccessingSecurityScopedResource()
        defer { if accessing { source.stopAccessingSecurityScopedResource() } }

        let name = freeName(named.map(sanitised) ?? readableName(source), under: root)
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

    private nonisolated static func freeName(_ base: String, under root: URL) -> String {
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
        return sanitised(name)
    }

    private nonisolated static func sanitised(_ name: String) -> String {
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
