// SkinCatalog.swift — fetches the community skin catalog from the ARMSX2 skins repo
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct CatalogSkin: Identifiable, Codable, Equatable {
    /// The catalog currently labels the game-specific BLACK package as
    /// universal. Keep that one package scoped to Criterion's first-person
    /// shooter without affecting generic skins such as Black Gold. These are
    /// the retail and demo serials in PCSX2's GameIndex.
    static let blackGameSerials: Set<String> = [
        "SLAJ-25078",
        "SLED-53937",
        "SLES-53886",
        "SLES-54030",
        "SLPM-66354",
        "SLPM-66731",
        "SLPM-66961",
        "SLUS-21376",
        "SLUS-29180",
    ]

    var id: String { file }
    let name: String
    let file: String
    let preview: String?
    let author: String?
    let serials: [String]
    let buttons: Int?
    let sizeBytes: Int?
    let iosLayout: String?

    var isIOSReady: Bool { iosLayout != nil }

    var isBlackGameSkin: Bool {
        Self.isBlackGameSkin(name: name, catalogID: file)
    }

    var isUniversal: Bool {
        !isBlackGameSkin
            && serials.contains {
                $0.caseInsensitiveCompare("any") == .orderedSame
            }
    }

    func explicitlyMatches(serial rawSerial: String) -> Bool {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty else { return false }
        if isBlackGameSkin {
            return Self.blackGameSerials.contains(serial)
        }
        return serials.contains {
            PadLayoutGameIdentity.normalizedSerial($0) == serial
        }
    }

    func isCompatible(withSerial rawSerial: String) -> Bool {
        explicitlyMatches(serial: rawSerial) || isUniversal
    }

    static func isBlackGameSkin(
        name: String,
        catalogID: String?
    ) -> Bool {
        let normalizedName = name.trimmingCharacters(
            in: .whitespacesAndNewlines
        ).lowercased()
        let normalizedCatalogFile = catalogID.map {
            URL(fileURLWithPath: $0).lastPathComponent.lowercased()
        }
        return normalizedName == "black"
            && (normalizedCatalogFile == nil
                || normalizedCatalogFile == "black.zip")
    }

    static func blackGameSkinIsCompatible(
        name: String,
        catalogID: String?,
        serial rawSerial: String
    ) -> Bool {
        guard isBlackGameSkin(name: name, catalogID: catalogID) else {
            return true
        }
        return blackGameSerials.contains(
            PadLayoutGameIdentity.normalizedSerial(rawSerial)
        )
    }

    private enum CodingKeys: String, CodingKey {
        case name
        case file
        case preview
        case author
        case serial
        case buttons
        case sizeBytes
        case iosLayout
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        name = try container.decode(String.self, forKey: .name)
        file = try container.decode(String.self, forKey: .file)
        preview = try container.decodeIfPresent(String.self, forKey: .preview)
        author = try container.decodeIfPresent(String.self, forKey: .author)
        buttons = try container.decodeIfPresent(Int.self, forKey: .buttons)
        sizeBytes = try container.decodeIfPresent(Int.self, forKey: .sizeBytes)
        iosLayout = try container.decodeIfPresent(String.self, forKey: .iosLayout)
        if let serial = try? container.decode(String.self, forKey: .serial) {
            serials = [serial]
        } else {
            serials = try container.decodeIfPresent(
                [String].self,
                forKey: .serial
            ) ?? []
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(name, forKey: .name)
        try container.encode(file, forKey: .file)
        try container.encodeIfPresent(preview, forKey: .preview)
        try container.encodeIfPresent(author, forKey: .author)
        if serials.count == 1 {
            try container.encode(serials[0], forKey: .serial)
        } else if !serials.isEmpty {
            try container.encode(serials, forKey: .serial)
        }
        try container.encodeIfPresent(buttons, forKey: .buttons)
        try container.encodeIfPresent(sizeBytes, forKey: .sizeBytes)
        try container.encodeIfPresent(iosLayout, forKey: .iosLayout)
    }
}

struct SkinCatalogManifest: Codable {
    let skins: [CatalogSkin]
}

enum SkinCatalogError: LocalizedError {
    case unreachable
    case serverError(Int)
    case malformed

    var errorDescription: String? {
        switch self {
        case .unreachable:
            return "Can't reach the skin catalog. Check your connection and pull down to try again."
        case .serverError(let code):
            return "The skin catalog server answered with \(code). Pull down to try again."
        case .malformed:
            return "The skin catalog downloaded fine but can't be read. The file itself is broken, so this needs fixing in the skins repo rather than here."
        }
    }
}

/// Fetches the community skin catalog (manifest.json) from the ARMSX2 skins repo.
@MainActor
final class SkinCatalog: ObservableObject {
    static let repo = "henyckma/ARMSX2-CustomControllerSkins"
    static let manifestURL = URL(string: "https://raw.githubusercontent.com/\(repo)/main/manifest.json")!
    static let rawBase = "https://raw.githubusercontent.com/\(repo)/main"

    @Published private(set) var skins: [CatalogSkin] = []
    @Published private(set) var isLoading = false
    @Published private(set) var lastUpdated: Date?
    @Published var lastError: String?

    private var inFlight: Task<Void, Never>?

    func fetch(force: Bool = false) async {
        inFlight?.cancel()
        let task = Task { await self.load(force: force) }
        inFlight = task
        await task.value
        // Only the newest fetch owns the spinner; a superseded one leaves it be.
        if inFlight == task {
            inFlight = nil
            isLoading = false
        }
    }

    private func load(force: Bool) async {
        isLoading = true
        lastError = nil
        do {
            var request = URLRequest(url: Self.manifestURL)
            // raw.githubusercontent.com serves max-age=300, so an ordinary
            // refresh inside that window never leaves the URL cache and newly
            // published skins stay invisible for five minutes.
            if force {
                request.cachePolicy = .reloadIgnoringLocalCacheData
            }
            let (data, response) = try await URLSession.shared.data(for: request)
            if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                throw SkinCatalogError.serverError(http.statusCode)
            }
            guard !Task.isCancelled else { return }
            // Reaching the file and being able to read it are different failures. One
            // missing comma upstream used to come out as "check your connection", which
            // sent people hunting a network problem they did not have.
            let manifest: SkinCatalogManifest
            do {
                manifest = try JSONDecoder().decode(SkinCatalogManifest.self, from: data)
            } catch {
                throw SkinCatalogError.malformed
            }
            skins = manifest.skins
            lastUpdated = Date()
        } catch {
            guard !Task.isCancelled else { return }
            lastError = (error as? SkinCatalogError ?? .unreachable).localizedDescription
        }
    }

    static func zipURL(for skin: CatalogSkin) -> URL? {
        assetURL(skin.file)
    }

    static func previewURL(for skin: CatalogSkin) -> URL? {
        guard let preview = skin.preview else { return nil }
        return assetURL(preview)
    }

    /// `..` and `/` both survive percent-encoding for `.urlPathAllowed`, so a
    /// hostile manifest entry could otherwise walk the path off the repo.
    private static func assetURL(_ path: String) -> URL? {
        guard SkinAssetPath.isSafeRelative(path) else { return nil }
        let encoded = path.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? path
        return URL(string: "\(rawBase)/\(encoded)")
    }
}
