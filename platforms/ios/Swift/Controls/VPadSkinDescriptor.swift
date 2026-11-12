// VPadSkinDescriptor.swift — virtual pad skin descriptor types
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum VPadSkinSource: String, Codable, Equatable {
    case builtIn
    case imported
}

struct VPadSkinDescriptor: Codable, Equatable, Identifiable {
    var id: String
    var displayName: String
    var source: VPadSkinSource
    var storageFolderName: String?
    var linkedLayoutPresetID: String?
    var manifestVersion: Int?
    var originalImportName: String?
    var builtInSkinRawValue: Int?
    // Repo-relative zip path of the catalog entry this came from, nil for
    // anything the user imported by hand. Must stay Optional: the library is
    // decoded all-or-nothing and a required key would wipe every legacy entry.
    var catalogID: String?
    var createdAt: Date
    var updatedAt: Date

    init(
        id: String,
        displayName: String,
        source: VPadSkinSource,
        storageFolderName: String? = nil,
        linkedLayoutPresetID: String? = nil,
        manifestVersion: Int? = nil,
        originalImportName: String? = nil,
        builtInSkinRawValue: Int? = nil,
        catalogID: String? = nil,
        createdAt: Date = Date(),
        updatedAt: Date = Date()
    ) {
        self.id = id
        self.displayName = displayName
        self.source = source
        self.storageFolderName = storageFolderName
        self.linkedLayoutPresetID = linkedLayoutPresetID
        self.manifestVersion = manifestVersion
        self.originalImportName = originalImportName
        self.builtInSkinRawValue = builtInSkinRawValue
        self.catalogID = catalogID
        self.createdAt = createdAt
        self.updatedAt = updatedAt
    }

    var virtualPadSkin: VirtualPadSkin {
        if source == .builtIn,
           let raw = builtInSkinRawValue,
           let skin = VirtualPadSkin(rawValue: raw) {
            return skin
        }
        return .custom
    }
}

struct VPadSkinImportResult {
    var descriptor: VPadSkinDescriptor
    var warnings: [String]

    var includesLinkedLayout: Bool {
        descriptor.linkedLayoutPresetID != nil
    }

    var message: String {
        var lines = ["Imported '\(descriptor.displayName)'."]
        if includesLinkedLayout {
            lines.append("This skin includes a recommended layout.")
        }
        lines.append(contentsOf: warnings)
        return lines.joined(separator: "\n")
    }
}

enum VPadSkinLibraryStoreError: LocalizedError {
    case missingSkin
    case noUsableSkinImages

    var errorDescription: String? {
        switch self {
        case .missingSkin:
            return "That skin is no longer in the library."
        case .noUsableSkinImages:
            return "No usable button images were found."
        }
    }
}
