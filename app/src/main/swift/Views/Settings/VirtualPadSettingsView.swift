// VirtualPadSettingsView.swift — Virtual pad opacity, haptic, layout editing
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct VirtualPadSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var showLayoutEditor = false
    @State private var showSkinImporter = false
    @State private var showSkinImportAlert = false
    @State private var skinImportMessage = ""

    var body: some View {
        Form {
            Section(settings.localized("Appearance")) {
                Picker(settings.localized("Button Skin"), selection: $settings.virtualPadSkin) {
                    ForEach(VirtualPadSkin.allCases) { skin in
                        Text(settings.localized(skin.label)).tag(skin)
                    }
                }

                Text(settings.localized(settings.virtualPadSkin.detail))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                VStack(alignment: .leading) {
                    Text("\(settings.localized("Opacity")): \(Int(settings.padOpacity * 100))%")
                    Slider(value: $settings.padOpacity, in: 0.1...1.0, step: 0.05)
                }
            }

            Section(settings.localized("Gameplay")) {
                Toggle(settings.localized("Hide Virtual Pad When Controller Is Connected"), isOn: $settings.autoHideVirtualPadWhenControllerConnected)
                Text(settings.localized("Automatically hides the on-screen controls while an external controller is connected."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Auto Full Screen"), isOn: $settings.autoFullscreen)
                Toggle(settings.localized("Hide Menu Button"), isOn: $settings.hideMenuButton)

                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text(settings.localized("Analog Stick Size"))
                        Spacer()
                        Text("\(Int((settings.analogStickScale * 100).rounded()))%")
                            .foregroundStyle(.secondary)
                    }
                    Slider(
                        value: Binding(
                            get: { Double(settings.analogStickScale) },
                            set: { settings.analogStickScale = Float($0) }
                        ),
                        in: 0.8...1.6,
                        step: 0.05
                    )
                }

                Text(settings.localized("Double-tap empty gameplay space to show the menu button again."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Custom Skin")) {
                Button {
                    showSkinImporter = true
                } label: {
                    Label(settings.localized("Import Button Images"), systemImage: "paintpalette")
                }

                Text("Import loose PNG/JPG/WebP button images, a full portrait/landscape controller image, or a zipped skin pack. Button files can be named cross, circle, square, triangle, up, down, left, right, L1, R1, L2, R2, start, select, analog_base, or analog_stick.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Feedback")) {
                Toggle(settings.localized("Haptic Feedback"), isOn: $settings.hapticFeedback)
            }

            Section(settings.localized("Layout")) {
                Button {
                    showLayoutEditor = true
                } label: {
                    Label(settings.localized("Edit Layout"), systemImage: "square.resize")
                }
                Text(settings.localized("Drag buttons to reposition. Pinch to resize."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text("Simple custom pad skins are shown behind the blue hit boxes in Edit Layout. Full-phone Manic skins need layout metadata support before they can be used in gameplay.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Virtual Pad"))
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $showSkinImporter) {
            ImportDocumentPicker(
                allowedContentTypes: [
                    .image,
                    UTType(filenameExtension: "zip") ?? .data,
                    UTType(filenameExtension: "skin") ?? .data,
                    UTType(filenameExtension: "manic") ?? .data,
                    .data
                ],
                allowsMultipleSelection: true
            ) { result in
                switch result {
                case .success(let urls):
                    skinImportMessage = importCustomSkinImages(urls)
                case .failure(let error):
                    skinImportMessage = "Skin import failed: \(error.localizedDescription)"
                }
                showSkinImportAlert = true
            }
        }
        .alert(settings.localized("Custom Skin"), isPresented: $showSkinImportAlert) {
            Button(settings.localized("OK"), role: .cancel) {}
        } message: {
            Text(skinImportMessage)
        }
        .fullScreenCover(isPresented: $showLayoutEditor) {
            PadLayoutEditView(onDismiss: { showLayoutEditor = false })
        }
    }

    private func importCustomSkinImages(_ urls: [URL]) -> String {
        guard let directory = VirtualPadSkin.customSkinDirectory(create: true) else {
            return "Could not create the custom skin folder."
        }

        let stagingDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("ARMSX2SkinImport-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: stagingDirectory, withIntermediateDirectories: true)
        defer {
            try? FileManager.default.removeItem(at: stagingDirectory)
        }

        var importedButtons: [String] = []
        var importedFullSkins: [String] = []
        var extractedImageCount = 0
        var skipped: [String] = []

        for sourceURL in urls {
            let accessGranted = sourceURL.startAccessingSecurityScopedResource()
            defer {
                if accessGranted {
                    sourceURL.stopAccessingSecurityScopedResource()
                }
            }

            let candidates = expandedSkinImportSources(from: sourceURL, stagingDirectory: stagingDirectory)
            extractedImageCount += max(0, candidates.count - 1)
            if candidates.isEmpty {
                skipped.append(sourceURL.lastPathComponent)
                continue
            }

            for candidate in candidates {
                importSkinImage(
                    candidate,
                    to: directory,
                    importedButtons: &importedButtons,
                    importedFullSkins: &importedFullSkins,
                    skipped: &skipped
                )
            }
        }

        if !importedButtons.isEmpty || !importedFullSkins.isEmpty {
            settings.virtualPadSkin = .custom
        }

        var lines: [String] = []
        if extractedImageCount > 0 {
            lines.append("Extracted \(extractedImageCount) image(s) from skin archive(s).")
        }
        if !importedButtons.isEmpty {
            lines.append(importedButtons.count == 1 ? "Imported \(importedButtons[0])." : "Imported \(importedButtons.count) button images.")
        }
        if !importedFullSkins.isEmpty {
            lines.append(importedFullSkins.count == 1 ? "Imported full skin image: \(importedFullSkins[0])." : "Imported \(importedFullSkins.count) full skin images.")
            lines.append("Simple pad skins are drawn behind transparent touch zones. Full-phone Manic skins are imported but kept inactive until their layout metadata is supported.")
        }
        if !skipped.isEmpty {
            lines.append("Skipped: \(skipped.joined(separator: ", "))")
        }
        if lines.isEmpty {
            return "No usable skin images were imported. Use loose button PNGs/JPGs/WebPs, a portrait/landscape controller image, or a zip skin pack containing image files."
        }
        return lines.joined(separator: "\n")
    }

    private func expandedSkinImportSources(from sourceURL: URL, stagingDirectory: URL) -> [URL] {
        if isSkinArchive(sourceURL) {
            let archiveDirectory = stagingDirectory
                .appendingPathComponent(sourceURL.deletingPathExtension().lastPathComponent, isDirectory: true)
            let extracted = ARMSX2Bridge.extractControllerSkinArchive(at: sourceURL, to: archiveDirectory)
            if !extracted.isEmpty {
                return extracted
            }
        }

        return [sourceURL]
    }

    private func importSkinImage(
        _ sourceURL: URL,
        to directory: URL,
        importedButtons: inout [String],
        importedFullSkins: inout [String],
        skipped: inout [String]
    ) {
        guard let image = UIImage(contentsOfFile: sourceURL.path),
              let data = image.pngData() else {
            skipped.append(sourceURL.lastPathComponent)
            return
        }

        let destinationName: String
        let isFullSkin: Bool
        if let canonicalName = canonicalSkinFileName(for: sourceURL) {
            destinationName = canonicalName
            isFullSkin = false
        } else {
            destinationName = fullSkinFileName(for: sourceURL, image: image)
            isFullSkin = true
        }

        do {
            try data.write(to: directory.appendingPathComponent(destinationName), options: .atomic)
            if isFullSkin {
                importedFullSkins.append(destinationName)
            } else {
                importedButtons.append(destinationName)
            }
        } catch {
            skipped.append(sourceURL.lastPathComponent)
        }
    }

    private func isSkinArchive(_ url: URL) -> Bool {
        let ext = url.pathExtension.lowercased()
        return ext == "zip" || ext == "skin" || ext == "manic"
    }

    private func fullSkinFileName(for url: URL, image: UIImage) -> String {
        let stem = url.deletingPathExtension().lastPathComponent
            .lowercased()
            .replacingOccurrences(of: "[^a-z0-9]+", with: "_", options: .regularExpression)
        let looksLikeDeviceSkin = stem.contains("edge") || stem.contains("iphone") || stem.contains("ipad")

        if stem.contains("landscape") || stem.contains("horizontal") {
            if looksLikeDeviceSkin {
                return "controller_edgetoedge_landscape.png"
            }
            return "controller_landscape.png"
        }
        if stem.contains("portrait") || stem.contains("vertical") {
            if looksLikeDeviceSkin {
                return "controller_edgetoedge_portrait.png"
            }
            return "controller_portrait.png"
        }

        return image.size.width > image.size.height ? "controller_landscape.png" : "controller_portrait.png"
    }

    private func canonicalSkinFileName(for url: URL) -> String? {
        let exact = url.lastPathComponent.lowercased()
        let expected = Set([
            "ic_controller_up_button.png",
            "ic_controller_down_button.png",
            "ic_controller_left_button.png",
            "ic_controller_right_button.png",
            "ic_controller_cross_button.png",
            "ic_controller_circle_button.png",
            "ic_controller_square_button.png",
            "ic_controller_triangle_button.png",
            "ic_controller_l1_button.png",
            "ic_controller_r1_button.png",
            "ic_controller_l2_button.png",
            "ic_controller_r2_button.png",
            "ic_controller_start_button.png",
            "ic_controller_select_button.png",
            "ic_controller_l3_button.png",
            "ic_controller_r3_button.png",
            "ic_controller_analog_base.png",
            "ic_controller_analog_stick.png",
            "ic_controller_analog_button.png"
        ])
        if expected.contains(exact) {
            return exact
        }

        let stem = url.deletingPathExtension().lastPathComponent
            .lowercased()
            .replacingOccurrences(of: "[^a-z0-9]+", with: "_", options: .regularExpression)

        let pairs: [(String, String)] = [
            ("analog_base", "ic_controller_analog_base.png"),
            ("analog_stick", "ic_controller_analog_stick.png"),
            ("analog_button", "ic_controller_analog_button.png"),
            ("stick", "ic_controller_analog_stick.png"),
            ("base", "ic_controller_analog_base.png"),
            ("l1", "ic_controller_l1_button.png"),
            ("r1", "ic_controller_r1_button.png"),
            ("l2", "ic_controller_l2_button.png"),
            ("r2", "ic_controller_r2_button.png"),
            ("l3", "ic_controller_l3_button.png"),
            ("r3", "ic_controller_r3_button.png"),
            ("start", "ic_controller_start_button.png"),
            ("select", "ic_controller_select_button.png"),
            ("triangle", "ic_controller_triangle_button.png"),
            ("circle", "ic_controller_circle_button.png"),
            ("square", "ic_controller_square_button.png"),
            ("cross", "ic_controller_cross_button.png"),
            ("up", "ic_controller_up_button.png"),
            ("down", "ic_controller_down_button.png"),
            ("left", "ic_controller_left_button.png"),
            ("right", "ic_controller_right_button.png")
        ]

        return pairs.first { stem.contains($0.0) }?.1
    }
}
