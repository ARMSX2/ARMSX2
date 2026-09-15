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

/// Where preset files live and what they are called from one install to the next.
final class ShaderPresetLibrary {
    static let presetExtension = "slangp"
    static let rootFolderName = "shaders"
    static let savedPresetFolderName = "My Presets"

    // A preset is persisted as a root marker plus a root-relative path because both iOS roots
    // sit under a container UUID that changes on every install, and a sideloaded build is
    // reinstalled constantly. An absolute path goes stale within days; this does not.
    static let bundleMarker = "bundle"
    static let userMarker = "data"
    // A colon: Files refuses it in a name and shows any it finds as a slash, and it is not a
    // path separator, so the relative half never needs escaping. Decoding splits on the first.
    static let markerSeparator: Character = ":"

    // shaders/ in the bundle also holds the core's own GLSL and Metal, which must never be listed.
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

    // Saved presets sit INSIDE the scanned root on purpose, so one becomes selectable with no
    // extra plumbing. They are left out of installedPacks() just as deliberately: a pack row
    // carries a Delete button, and this is the one folder here a user cannot re-download.
    static var savedPresetRoot: URL? {
        userRoot?.appendingPathComponent(savedPresetFolderName, isDirectory: true)
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

    static func token(forLegacyPath path: String) -> String? {
        guard path.hasPrefix("/") else { return nil }
        if let token = token(for: URL(fileURLWithPath: path)), resolve(token) != nil {
            return token
        }
        // The prefix carries the container UUID of the install that wrote it, so on the far
        // side of a reinstall only the tail below the shaders folder still matches.
        guard let tail = pathBelowRootFolder(path) else { return nil }
        return markedRoots()
            .map { $0.marker + String(markerSeparator) + tail }
            .first { resolve($0) != nil }
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

    func scan() -> ShaderPresetListing {
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

    func listing(at folder: ShaderPresetFolder) -> ShaderPresetListing {
        listing(at: folder.url)
    }

    static func installedPacks() -> [ShaderPresetFolder] {
        guard let user = userRoot else { return [] }
        return sorted(children(of: user).directories
            .filter { $0.lastPathComponent != savedPresetFolderName }
            .compactMap { folder(at: $0) })
    }

    private func listing(at directory: URL) -> ShaderPresetListing {
        let level = Self.children(of: directory)
        let folders = level.directories.compactMap {
            Self.folder(at: $0, pinned: Self.isSavedPresetRoot($0))
        }
        return ShaderPresetListing(
            folders: Self.sorted(folders),
            presets: Self.sorted(level.presets.compactMap { Self.preset(at: $0) }))
    }

    /// Promoted past a single wrapping install folder so a pack's categories become its root,
    /// and dropped unless a preset lives somewhere under it — a pack keeps its .slang stages in
    /// a shaders/ folder beside the presets, and those are build inputs, not places to browse.
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
