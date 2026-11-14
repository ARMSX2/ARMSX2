// ISOEntry.swift — A library game entry, its region flag and file type label
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// Region-string to flag emoji mapping, shared by library cards and the game
/// info detail view. Returns an empty string for unknown regions so callers can
/// omit the flag without rendering a broken placeholder.
enum RegionFlag {
    static func emoji(for region: String?) -> String {
        guard let region, !region.isEmpty else { return "" }
        let value = region.lowercased()
        if value.contains("japan") || value.contains("ntsc-j") {
            return "🇯🇵"
        }
        if value.contains("usa") || value.contains("america") || value.contains("ntsc-u") {
            return "🇺🇸"
        }
        if value.contains("europe") || value.contains("pal") {
            return "🇪🇺"
        }
        if value.contains("korea") || value.contains("ntsc-k") {
            return "🇰🇷"
        }
        if value.contains("china") || value.contains("ntsc-c") {
            return "🇨🇳"
        }
        if value.contains("hong kong") || value.contains("ntsc-hk") {
            return "🇭🇰"
        }
        if value.contains("australia") {
            return "🇦🇺"
        }
        return ""
    }
}

struct ISOEntry: Identifiable, Equatable, Sendable {
	var id: String { bootPath ?? fileURL?.path ?? name }
	let name: String
	let fileURL: URL?
	let bootPath: String?
	let coverURL: URL?
	let coverSignature: String?
	let metadata: [String: String]
	let size: UInt64
	var isFavorite: Bool
    var isExternal: Bool = false
    var sourceName: String? = nil
    let displayName: String
    let sizeLabel: String
    let regionFlag: String?
    let normalizedIdentifiers: Set<String>

    init(
        name: String,
        fileURL: URL?,
        bootPath: String?,
        coverURL: URL?,
        coverSignature: String?,
        metadata: [String: String],
        size: UInt64,
        isFavorite: Bool,
        isExternal: Bool = false,
        sourceName: String? = nil,
        displayNameOverride: String? = nil
    ) {
        self.name = name
        self.fileURL = fileURL
        self.bootPath = bootPath
        self.coverURL = coverURL
        self.coverSignature = coverSignature
        self.metadata = metadata
        self.size = size
        self.isFavorite = isFavorite
        self.isExternal = isExternal
        self.sourceName = sourceName
        let fallbackDisplayName = URL(fileURLWithPath: name)
            .deletingPathExtension()
            .lastPathComponent
        let override = displayNameOverride?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if let override, !override.isEmpty {
            displayName = override
        } else {
            displayName = fallbackDisplayName
        }

        let gigabytes = Double(size) / 1_073_741_824
        if gigabytes >= 1 {
            sizeLabel = String(format: "%.1f GB", gigabytes)
        } else {
            sizeLabel = String(
                format: "%.0f MB",
                Double(size) / 1_048_576
            )
        }

        let region = metadata["region"]?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let flag = RegionFlag.emoji(for: region)
        regionFlag = flag.isEmpty ? nil : flag

        let identifierCandidates: [String?] = [
            bootPath ?? fileURL?.path ?? name,
            name,
            fileURL?.path,
        ]
        let identifiers: [String] = identifierCandidates.compactMap { $0 }
        normalizedIdentifiers = identifiers.reduce(into: Set<String>()) {
            result, identifier in
            result.formUnion(Self.normalizedIdentifierKeys(for: identifier))
        }
    }

	var bootName: String {
		isExternal ? (bootPath ?? fileURL?.path ?? name) : name
	}

	var isELF: Bool {
		(bootPath ?? fileURL?.path ?? name).lowercased().hasSuffix(".elf")
	}

	var coverInfo: CoverGameInfo {
		CoverGameInfo(name: name, fileURL: fileURL, metadata: metadata, hasCover: coverURL != nil)
	}

    static func normalizedIdentifierKeys(
        for value: String
    ) -> Set<String> {
        let normalized = (value.removingPercentEncoding ?? value)
            .replacingOccurrences(of: "\\", with: "/")
            .lowercased()
        guard !normalized.isEmpty else { return [] }
        let fileName = (normalized as NSString).lastPathComponent
        return fileName.isEmpty ? [normalized] : [normalized, fileName]
    }
}

extension String {
    var pathExtensionLabel: String {
        let ext = (self as NSString).pathExtension.uppercased()
        return ext.isEmpty ? "FILE" : ext
    }
}
