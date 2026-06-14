// AppearanceSettingsView.swift — Library background customization
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UIKit

struct AppearanceSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var selectedPhotoItem: PhotosPickerItem?
    @State private var showPhotoPicker = false
    @State private var showLoadError = false
    @State private var selectedBackgroundRole = LibraryBackgroundRole.main

    private var hasBackground: Bool {
        !settings.libraryBackgroundPath.isEmpty && FileManager.default.fileExists(atPath: settings.libraryBackgroundPath)
    }

    private var hasLandscapeBackground: Bool {
        !settings.libraryLandscapeBackgroundPath.isEmpty && FileManager.default.fileExists(atPath: settings.libraryLandscapeBackgroundPath)
    }

    var body: some View {
        Form {
            Section(settings.localized("Library Background")) {
                Text(settings.localized("Use a custom image behind your game library."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Button {
                    selectedBackgroundRole = .main
                    showPhotoPicker = true
                } label: {
                    Label(settings.localized("Choose Background"), systemImage: "photo")
                }

                if hasBackground {
                    Button(role: .destructive) {
                        removeLibraryBackground()
                    } label: {
                        Label(settings.localized("Remove Background"), systemImage: "trash")
                    }

                    VStack(alignment: .leading, spacing: 6) {
                        Text(settings.localized("Background Dim"))
                            .font(.subheadline)
                        Slider(
                            value: $settings.libraryBackgroundDim,
                            in: 0.0...0.8,
                            step: 0.05
                        )
                        .accessibilityLabel(settings.localized("Background Dim"))
                        .accessibilityValue(String(format: "%.0f%%", settings.libraryBackgroundDim * 100))
                    }
                    .padding(.vertical, 4)
                }
            }

            if hasBackground {
                Section(settings.localized("Landscape Background")) {
                    Text(settings.localized("Uses the main background when no landscape image is selected."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Button {
                        selectedBackgroundRole = .landscape
                        showPhotoPicker = true
                    } label: {
                        Label(settings.localized("Choose Landscape Background"), systemImage: "rectangle.landscape")
                    }

                    if hasLandscapeBackground {
                        Button(role: .destructive) {
                            removeLandscapeBackground()
                        } label: {
                            Label(settings.localized("Remove Landscape Background"), systemImage: "trash")
                        }
                    }
                }
            }
        }
        .navigationTitle(settings.localized("Appearance"))
        .photosPicker(
            isPresented: $showPhotoPicker,
            selection: $selectedPhotoItem,
            matching: .images
        )
        .onChange(of: selectedPhotoItem) { _, newItem in
            guard let newItem else { return }
            let role = selectedBackgroundRole
            selectedPhotoItem = nil
            Task {
                await importLibraryBackground(from: newItem, role: role)
            }
        }
        .alert(
            settings.localized("Background image could not be loaded."),
            isPresented: $showLoadError
        ) {
            Button(settings.localized("OK")) {}
        }
    }

    private func importLibraryBackground(from photoItem: PhotosPickerItem, role: LibraryBackgroundRole) async {
        do {
            guard let data = try await photoItem.loadTransferable(type: Data.self),
                  let image = UIImage(data: data) else {
                showLoadError = true
                return
            }
            let savedPath = try LibraryBackgroundHelper.save(image, for: role)
            switch role {
            case .main:
                settings.libraryBackgroundPath = savedPath
            case .landscape:
                settings.libraryLandscapeBackgroundPath = savedPath
            }
            settings.libraryBackgroundRevision &+= 1
        } catch {
            showLoadError = true
            NSLog("[ARMSX2 iOS LibraryBackground] import failed: %@", error.localizedDescription)
        }
    }

    private func removeLibraryBackground() {
        LibraryBackgroundHelper.remove(.main)
        LibraryBackgroundHelper.remove(.landscape)
        settings.libraryBackgroundPath = ""
        settings.libraryLandscapeBackgroundPath = ""
        settings.libraryBackgroundDim = 0.35
        settings.libraryBackgroundRevision &+= 1
    }

    private func removeLandscapeBackground() {
        LibraryBackgroundHelper.remove(.landscape)
        settings.libraryLandscapeBackgroundPath = ""
        settings.libraryBackgroundRevision &+= 1
    }
}

private enum LibraryBackgroundRole {
    case main
    case landscape

    var fileName: String {
        switch self {
        case .main:
            return "library_background.jpg"
        case .landscape:
            return "library_background_landscape.jpg"
        }
    }
}

private enum LibraryBackgroundHelper {
    static let maxDimension: CGFloat = 1920
    static let jpegQuality: CGFloat = 0.85

    static var storageDirectory: URL {
        let fm = FileManager.default
        let urls = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)
        let dir = urls.first?.appendingPathComponent("ARMSX2", isDirectory: true) ?? fm.temporaryDirectory.appendingPathComponent("ARMSX2", isDirectory: true)
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static func fileURL(for role: LibraryBackgroundRole) -> URL {
        storageDirectory.appendingPathComponent(role.fileName)
    }

    static func save(_ image: UIImage, for role: LibraryBackgroundRole) throws -> String {
        let resized = resize(image, maxDimension: maxDimension)
        guard let data = resized.jpegData(compressionQuality: jpegQuality) else {
            throw NSError(domain: "LibraryBackground", code: 1, userInfo: [NSLocalizedDescriptionKey: "JPEG encoding failed"])
        }
        let url = fileURL(for: role)
        try data.write(to: url, options: .atomic)
        return url.path
    }

    static func remove(_ role: LibraryBackgroundRole) {
        let url = fileURL(for: role)
        if FileManager.default.fileExists(atPath: url.path) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    private static func resize(_ image: UIImage, maxDimension: CGFloat) -> UIImage {
        let size = image.size
        let maxSide = max(size.width, size.height)
        guard maxSide > maxDimension else { return image }
        let scale = maxDimension / maxSide
        let newSize = CGSize(width: size.width * scale, height: size.height * scale)
        let renderer = UIGraphicsImageRenderer(size: newSize)
        return renderer.image { _ in
            image.draw(in: CGRect(origin: .zero, size: newSize))
        }
    }
}
