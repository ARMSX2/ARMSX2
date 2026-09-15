// ShaderPresetLibrary.swift — the two RetroArch preset roots, and names that outlive an install
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct ShaderPresetFile: Identifiable, Hashable {
    let name: String
    let url: URL
    let token: String

    var id: String { token }
}

struct ShaderPresetFolder: Identifiable, Hashable {
    let name: String
    let url: URL

    var id: URL { url }
}

struct ShaderPresetListing: Hashable {
    let folders: [ShaderPresetFolder]
    let presets: [ShaderPresetFile]

    static let empty = ShaderPresetListing(folders: [], presets: [])
}

/// The two preset roots, and tokens that name a preset across reinstalls.
enum ShaderPresetLibrary {
    static let presetExtension = "slangp"
    static let rootFolderName = "shaders"
    static let savedPresetFolderName = "My Presets"

    // Stored as a root marker plus a relative path, because both roots sit under a container
    // UUID that changes on every install.
    static let bundleMarker = "bundle"
    static let userMarker = "data"
    // Files doesn't allow a colon in names and it isn't a path separator, so the relative half
    // needs no escaping. Decoding splits on the first one.
    static let markerSeparator: Character = ":"

    // The bundle's shaders/ also holds the core's GLSL and Metal sources, so only these are listed.
    private static let bundlePresetFolders = ["presets", "armsx2-tracer"]
    private static let maxScanDepth = 12

    // MARK: - Roots

    /// Read-only. Flat iOS bundles have no Resources/ component, so this is the .app root.
    static var bundleRoot: URL? {
        Bundle.main.resourceURL?.appendingPathComponent(rootFolderName, isDirectory: true)
    }

    static var userRoot: URL? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?
            .appendingPathComponent(rootFolderName, isDirectory: true)
    }

    // Inside the scanned root, so saved presets list like any other.
    static var savedPresetRoot: URL? {
        userRoot?.appendingPathComponent(savedPresetFolderName, isDirectory: true)
    }

    static let basePackFolderName = "shaders_slang"

    static var basePackRoot: URL? {
        userRoot?.appendingPathComponent(basePackFolderName, isDirectory: true)
    }

    @discardableResult
    static func prepareUserRoots() -> URL? {
        guard let root = userRoot, let saved = savedPresetRoot else { return nil }
        try? FileManager.default.createDirectory(at: saved, withIntermediateDirectories: true)
        return root
    }

    // MARK: - Tokens

    static func token(for url: URL) -> String? {
        for (marker, root) in markedRoots() {
            if let relative = relativePath(of: url, under: root) {
                return marker + String(markerSeparator) + relative
            }
        }
        return nil
    }

    static func resolve(_ token: String) -> URL? {
        guard let separator = token.firstIndex(of: markerSeparator) else { return nil }
        let relative = String(token[token.index(after: separator)...])
        guard let root = root(forMarker: String(token[..<separator])),
              contains(relative) else { return nil }
        let url = root.appendingPathComponent(relative).standardizedFileURL
        guard url.path.hasPrefix(root.standardizedFileURL.path + "/"),
              FileManager.default.fileExists(atPath: url.path) else { return nil }
        return url
    }

    static func displayName(for token: String) -> String? {
        guard let separator = token.firstIndex(of: markerSeparator) else { return nil }
        let name = URL(fileURLWithPath: String(token[token.index(after: separator)...]))
            .deletingPathExtension().lastPathComponent
        return name.isEmpty ? nil : name
    }

    static func token(forLegacyPath path: String) -> String? {
        guard path.hasPrefix("/") else { return nil }
        if let token = token(for: URL(fileURLWithPath: path)), resolve(token) != nil {
            return token
        }
        // The prefix holds an older container UUID, so only the part below shaders/ still matches.
        guard let tail = pathBelowRootFolder(path) else { return nil }
        return markedRoots()
            .map { $0.marker + String(markerSeparator) + tail }
            .first { resolve($0) != nil }
    }

    /// A preset saved from a built-in one names it by absolute path, which every install moves.
    static func repairSavedReferences() {
        let head = "#reference \""
        guard let saved = savedPresetRoot, let files = try? FileManager.default.contentsOfDirectory(
            at: saved, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles]) else { return }
        for url in files where url.pathExtension.lowercased() == presetExtension {
            guard let text = try? String(contentsOf: url, encoding: .utf8),
                  let end = text.firstIndex(of: "\n") else { continue }
            let line = text[..<end]
            guard line.hasPrefix(head + "/"), line.hasSuffix("\"") else { continue }
            let old = String(line.dropFirst(head.count).dropLast())
            guard !FileManager.default.fileExists(atPath: old),
                  let token = token(forLegacyPath: old), let base = resolve(token) else { continue }
            try? (head + base.path + "\"" + String(text[end...]))
                .write(to: url, atomically: true, encoding: .utf8)
        }
    }

    private static func markedRoots() -> [(marker: String, root: URL)] {
        [(bundleMarker, bundleRoot), (userMarker, userRoot)].compactMap { marker, root in
            root.map { (marker, $0) }
        }
    }

    private static func root(forMarker marker: String) -> URL? {
        markedRoots().first { $0.marker == marker }?.root
    }

    private static func contains(_ relative: String) -> Bool {
        guard !relative.isEmpty, !relative.hasPrefix("/") else { return false }
        return !relative.components(separatedBy: "/").contains("..")
    }

    private static func relativePath(of url: URL, under root: URL) -> String? {
        let prefix = root.standardizedFileURL.path + "/"
        let path = url.standardizedFileURL.path
        guard path.hasPrefix(prefix) else { return nil }
        return String(path.dropFirst(prefix.count))
    }

    private static func pathBelowRootFolder(_ path: String) -> String? {
        let components = URL(fileURLWithPath: path).pathComponents
        guard let root = components.lastIndex(of: rootFolderName),
              root + 1 < components.count else { return nil }
        return components[(root + 1)...].joined(separator: "/")
    }

    // MARK: - Scanning

    static func scan() -> ShaderPresetListing {
        Self.prepareUserRoots()
        var folders: [ShaderPresetFolder] = []
        var presets: [ShaderPresetFile] = []
        if let bundle = Self.bundleRoot {
            for name in Self.bundlePresetFolders {
                let level = listing(at: bundle.appendingPathComponent(name, isDirectory: true))
                folders += level.folders
                presets += level.presets
            }
        }
        if let user = Self.userRoot {
            let level = listing(at: user)
            folders += level.folders
            presets += level.presets
        }
        return ShaderPresetListing(folders: Self.sorted(folders), presets: Self.sorted(presets))
    }

    static func listing(at folder: ShaderPresetFolder) -> ShaderPresetListing {
        listing(at: folder.url)
    }

    /// A pack row can be a promoted inner folder, so its name, not its URL, finds what to delete.
    static func deletableURL(for folder: ShaderPresetFolder) -> URL? {
        guard folder.name != savedPresetFolderName, let root = userRoot?.standardizedFileURL else { return nil }
        let pack = root.appendingPathComponent(folder.name, isDirectory: true)
        let path = folder.url.standardizedFileURL.path
        return path == pack.path || path.hasPrefix(pack.path + "/") ? pack : nil
    }

    static func deletableURL(for preset: ShaderPresetFile) -> URL? {
        let parent = preset.url.deletingLastPathComponent().standardizedFileURL.path
        return parent == savedPresetRoot?.standardizedFileURL.path ? preset.url : nil
    }

    private static func listing(at directory: URL) -> ShaderPresetListing {
        let level = Self.children(of: directory)
        let folders = level.directories.compactMap {
            Self.folder(at: $0, pinned: Self.isSavedPresetRoot($0))
        }
        return ShaderPresetListing(
            folders: Self.sorted(folders),
            presets: Self.sorted(level.presets.compactMap { Self.preset(at: $0) }))
    }

    /// Skips a single wrapping folder so a pack's categories list at its root, and drops folders
    /// with no preset under them, such as a pack's shaders/ stage folder.
    private static func folder(at url: URL, pinned: Bool = false) -> ShaderPresetFolder? {
        let level = children(of: url)
        if !pinned, level.presets.isEmpty, level.directories.count == 1,
           let promoted = folder(at: level.directories[0]) {
            return ShaderPresetFolder(name: url.lastPathComponent, url: promoted.url)
        }
        guard pinned || !level.presets.isEmpty || containsPreset(url) else { return nil }
        return ShaderPresetFolder(name: url.lastPathComponent, url: url)
    }

    private static func containsPreset(_ directory: URL) -> Bool {
        guard let walk = FileManager.default.enumerator(
            at: directory, includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]) else { return false }
        while let url = walk.nextObject() as? URL {
            if walk.level >= maxScanDepth { walk.skipDescendants() }
            if url.pathExtension.lowercased() == presetExtension { return true }
        }
        return false
    }

    private static func isSavedPresetRoot(_ url: URL) -> Bool {
        url.standardizedFileURL.path == savedPresetRoot?.standardizedFileURL.path
    }

    private static func preset(at url: URL) -> ShaderPresetFile? {
        guard let token = token(for: url) else { return nil }
        return ShaderPresetFile(
            name: url.deletingPathExtension().lastPathComponent, url: url, token: token)
    }

    /// Standardised on the way out, so a scanned row compares equal to a resolved selection.
    private static func children(of directory: URL) -> (directories: [URL], presets: [URL]) {
        let contents = (try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles])) ?? []
        var directories: [URL] = []
        var presets: [URL] = []
        for url in contents {
            if (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
                directories.append(url.standardizedFileURL)
            } else if url.pathExtension.lowercased() == presetExtension {
                presets.append(url.standardizedFileURL)
            }
        }
        return (directories, presets)
    }

    private static func sorted(_ folders: [ShaderPresetFolder]) -> [ShaderPresetFolder] {
        folders.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private static func sorted(_ presets: [ShaderPresetFile]) -> [ShaderPresetFile] {
        presets.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
}
