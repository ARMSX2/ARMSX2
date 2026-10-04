// ARMSX2LogoStore.swift — Persistent user-imported ARMSX2 logo
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import Observation
import UIKit

enum ARMSX2LogoStoreError: LocalizedError {
    case notPNG
    case invalidImage
    case imageTooLarge

    var errorDescription: String? {
        switch self {
        case .notPNG:
            return "Select a PNG image named logo.png."
        case .invalidImage:
            return "The selected file is not a readable PNG image."
        case .imageTooLarge:
            return "The logo image is too large. Choose a PNG smaller than 16 MB and 8192 pixels per side."
        }
    }
}

@MainActor
@Observable
final class ARMSX2LogoStore {
    static let shared = ARMSX2LogoStore()
    static let didChangeNotification = Notification.Name("ARMSX2iOSLogoDidChange")

    private static let maximumFileSize = 16 * 1024 * 1024
    private static let maximumPixelDimension: CGFloat = 8_192

    private(set) var image: UIImage?

    var hasLogo: Bool { image != nil }

    private init() {
        image = Self.loadStoredImage()
    }

    func importLogo(from sourceURL: URL) throws {
        guard sourceURL.pathExtension.caseInsensitiveCompare("png") == .orderedSame else {
            throw ARMSX2LogoStoreError.notPNG
        }

        let data = try Data(contentsOf: sourceURL, options: [.mappedIfSafe])
        guard data.count <= Self.maximumFileSize else {
            throw ARMSX2LogoStoreError.imageTooLarge
        }
        guard let decoded = UIImage(data: data),
              decoded.size.width > 0,
              decoded.size.height > 0 else {
            throw ARMSX2LogoStoreError.invalidImage
        }
        guard decoded.size.width <= Self.maximumPixelDimension,
              decoded.size.height <= Self.maximumPixelDimension else {
            throw ARMSX2LogoStoreError.imageTooLarge
        }

        let manager = FileManager.default
        try manager.createDirectory(
            at: Self.storageDirectoryURL,
            withIntermediateDirectories: true
        )
        try data.write(to: Self.logoURL, options: [.atomic])

        // Decode from the installed copy so the in-memory image never retains
        // a security-scoped provider or temporary picker URL.
        image = UIImage(contentsOfFile: Self.logoURL.path)
        NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
    }

    func deleteLogo() throws {
        let manager = FileManager.default
        if manager.fileExists(atPath: Self.logoURL.path) {
            try manager.removeItem(at: Self.logoURL)
        }
        image = nil
        NotificationCenter.default.post(name: Self.didChangeNotification, object: nil)
    }

    private static func loadStoredImage() -> UIImage? {
        UIImage(contentsOfFile: logoURL.path)
    }

    private nonisolated static var storageDirectoryURL: URL {
        let manager = FileManager.default
        let base = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? manager.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? manager.temporaryDirectory
        return base
            .appendingPathComponent("ARMSX2", isDirectory: true)
            .appendingPathComponent("Branding", isDirectory: true)
    }

    private nonisolated static var logoURL: URL {
        storageDirectoryURL.appendingPathComponent("logo.png", isDirectory: false)
    }
}
