// ShaderPackImporter.swift — installs a picked .zip or folder into Documents/shaders
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ShaderPackImportError: LocalizedError {
    case noUserRoot
    case notAShaderPack

    var errorDescription: String? {
        switch self {
        case .noUserRoot:
            return "Your shader folder couldn't be opened."
        case .notAShaderPack:
            return "No shader presets were found in it, so nothing was installed."
        }
    }
}

/// Installs a shader pack into the user root under a free folder name.
@MainActor
final class ShaderPackImporter: ObservableObject {
    @Published private(set) var installing: Set<String> = []
    @Published private(set) var errors: [String: String] = [:]
    @Published private(set) var installedName: String?

    var isBusy: Bool { !installing.isEmpty }

    /// `named` replaces the archive's own name, for a staging file named by UUID. Callers use the
    /// returned folder name, since concurrent installs overwrite `installedName`.
    @discardableResult
    func install(archiveAt source: URL, named: String? = nil) async -> String? {
        await install(source, named: named, writing: Self.extract)
    }

    @discardableResult
    func install(folderAt source: URL) async -> String? {
        await install(source, named: nil, writing: Self.copyTree)
    }

    static let basePackURL = URL(string: "https://buildbot.libretro.com/assets/frontend/shaders_slang.zip")!
    static let basePackBytes: Int64 = 54_000_000

    func installBasePack() async {
        let key = ShaderPresetLibrary.basePackFolderName
        installing.insert(key)
        errors[key] = nil
        defer { installing.remove(key) }
        do {
            let staged = try await ShaderCatalogInstaller.stage(Self.basePackURL)
            defer { try? FileManager.default.removeItem(at: staged) }
            guard let landed = await install(archiveAt: staged, named: key) else {
                errors[key] = errors.removeValue(forKey: staged.lastPathComponent)
                    ?? ShaderPackImportError.notAShaderPack.localizedDescription
                return
            }
            installedName = nil
            try await Task.detached(priority: .userInitiated) { try Self.replaceBasePack(with: landed) }.value
            installedName = key
        } catch {
            errors[key] = error.localizedDescription
        }
    }

    private nonisolated static func replaceBasePack(with landed: String) throws {
        let name = ShaderPresetLibrary.basePackFolderName
        guard landed != name, let root = ShaderPresetLibrary.userRoot else { return }
        let base = root.appendingPathComponent(name, isDirectory: true)
        try FileManager.default.removeItem(at: base)
        try FileManager.default.moveItem(at: root.appendingPathComponent(landed, isDirectory: true), to: base)
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
            // A full RetroArch pack is thousands of files, too slow for the main actor.
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
        // A folder with no presets would sit in the browser with nothing to pick, so it is refused.
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

    /// Copied as is, because a .slangp names its stages by relative path.
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
