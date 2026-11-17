// ReportSheet+Attachments.swift — the photo row, the crash file steps and the one file importer of the report sheet
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import ImageIO
import UniformTypeIdentifiers

enum FilePickMode {
    case photo, crash
}

// The copies are deleted when the sheet's state goes away, however the sheet is dismissed.
@Observable
final class CrashCopies {
    var urls: [URL] = []

    deinit {
        for url in urls { try? FileManager.default.removeItem(at: url) }
    }
}

enum ReportAttachments {
    static let maxCrashFiles = 3

    // Validation and staging both read this copy, so the picked file changing on disk later cannot change the bytes.
    static func privateCopy(of url: URL) -> URL? {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]), values.isRegularFile == true,
              let size = values.fileSize, size > 0, size <= CrashFileCheck.sizeLimit else { return nil }
        let copy = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("ARMSX2-report-attach-\(UUID().uuidString).ips")
        do { try FileManager.default.copyItem(at: url, to: copy) } catch { return nil }
        return copy
    }

    static func photoData(from url: URL) -> Data? {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }
        guard let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize, size <= ReportPhoto.maxInputBytes else { return nil }
        return try? Data(contentsOf: url)
    }

    static func thumbnail(of jpeg: Data) -> UIImage? {
        let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                        kCGImageSourceCreateThumbnailWithTransform: true,
                                        kCGImageSourceThumbnailMaxPixelSize: 320]
        guard let source = CGImageSourceCreateWithData(jpeg as CFData, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        return UIImage(cgImage: image)
    }
}

extension View {
    func attachmentPickers(of sheet: ReportSheet) -> some View { sheet.attachmentPickers(self) }
}

extension ReportSheet {
    var attachmentBusy: Bool { working || attachmentLoading }

    @ViewBuilder var frameSection: some View {
        if let frameThumb {
            Section(settings.localized("Paused frame")) {
                Image(uiImage: frameThumb)
                    .resizable()
                    .scaledToFit()
                    .frame(maxHeight: 120)
                    .accessibilityHidden(true)
                Button(settings.localized("Remove"), role: .destructive) { self.frameThumb = nil }
                    .controllerAccessibilityTargetID("report.frame-remove")
                    .disabled(working)
            }
        }
    }

    var photoSection: some View {
        Section(settings.localized("Photo (optional)")) {
            if let photoThumb {
                Image(uiImage: photoThumb)
                    .resizable()
                    .scaledToFit()
                    .frame(maxHeight: 120)
                    .accessibilityHidden(true)
            }
            Button(settings.localized(photoThumb == nil ? "Add Photo" : "Replace Photo")) { photoChoice = true }
                .controllerAccessibilityTargetID("report.photo")
                .disabled(attachmentBusy)
            if photoThumb != nil {
                Button(settings.localized("Remove"), role: .destructive) {
                    photoJPEG = nil
                    photoThumb = nil
                }
                .controllerAccessibilityTargetID("report.photo.remove")
                .disabled(attachmentBusy)
            }
            if attachmentLoading { ProgressView() }
        }
    }

    @ViewBuilder var crashSection: some View {
        if let hasCrashPayload {
            Section {
                if hasCrashPayload {
                    Text(settings.localized("A crash report from iOS is included."))
                } else {
                    let steps = ["Open the Settings app, then Privacy & Security > Analytics & Improvements > Analytics Data.",
                                 "Find the newest entry whose name starts with ARMSX2iOS-.",
                                 "Tap it, tap Share, then Save to Files.",
                                 "Come back here and tap Attach Crash File."]
                    ForEach(Array(steps.enumerated()), id: \.offset) { index, step in
                        Text("\(index + 1). \(settings.localized(step))")
                    }
                    Button(settings.localized("Attach Crash File")) { pick(.crash) }
                        .controllerAccessibilityTargetID("report.attach-crash")
                        .disabled(attachmentBusy)
                    if !crashCopies.urls.isEmpty { Text(settings.localized("Crash file attached.")) }
                }
            } header: {
                Text(settings.localized("Crash report"))
            } footer: {
                if !hasCrashPayload {
                    Text(settings.localized("Attach only files that start with ARMSX2iOS-. Other entries can belong to other apps."))
                }
            }
        }
    }

    // One importer serves both picks: two on one view node leave only one of them working.
    func attachmentPickers(_ content: some View) -> some View {
        content
            .controllerPrompt(settings.localized("Photo (optional)"), isPresented: $photoChoice, actions: [
                .cancel,
                .init(title: settings.localized("Choose from Photos")) { showPhotoPicker = true },
                .init(title: settings.localized("Choose from Files")) { pick(.photo) },
            ])
            .photosPicker(isPresented: $showPhotoPicker, selection: $photoItem, matching: .images)
            .onChange(of: photoItem) { _, item in
                guard let item else { return }
                photoItem = nil
                setPhoto { try? await item.loadTransferable(type: Data.self) }
            }
            .fileImporter(
                isPresented: $showImporter,
                allowedContentTypes: pickMode == .crash ? [UTType(filenameExtension: "ips") ?? .json, .json, .plainText] : [.image],
                allowsMultipleSelection: false
            ) { result in
                let mode = pickMode
                pickMode = nil
                guard let url = try? result.get().first else { return }
                if mode == .photo { setPhoto { ReportAttachments.photoData(from: url) } } else { attachCrash(url) }
            }
            .task {
                // The thumbnail decides what ships: a frame that can't be shown is not sent.
                frameThumb = request.pausedFramePNG.flatMap(ReportAttachments.thumbnail(of:))
                let id = request.sessionID
                hasCrashPayload = await Task.detached { CrashFileCheck.sessionHasPayload(id: id) }.value
            }
    }

    private func pick(_ mode: FilePickMode) {
        pickMode = mode
        showImporter = true
    }

    private func setPhoto(_ read: @escaping @Sendable () async -> Data?) {
        attachmentLoading = true
        Task {
            defer { attachmentLoading = false }
            let jpeg = await Task.detached { await read().flatMap(ReportPhoto.reencode) }.value
            guard let jpeg else { return refuse("That file could not be used as a photo.") }
            photoJPEG = jpeg
            photoThumb = ReportAttachments.thumbnail(of: jpeg)
        }
    }

    private func attachCrash(_ url: URL) {
        guard crashCopies.urls.count < ReportAttachments.maxCrashFiles else {
            return refuse("Up to three crash files can be attached.")
        }
        attachmentLoading = true
        Task {
            defer { attachmentLoading = false }
            let copy = await Task.detached { ReportAttachments.privateCopy(of: url) }.value
            let accepted = await Task.detached { copy.map(CrashFileCheck.isOwnCrashFile) ?? false }.value
            guard let copy, accepted else {
                if let copy { try? FileManager.default.removeItem(at: copy) }
                return refuse("Only crash reports from ARMSX2 are accepted. Their file names start with ARMSX2iOS-.",
                              title: "That is not a crash report from ARMSX2")
            }
            crashCopies.urls.append(copy)
        }
    }

    private func refuse(_ message: String, title: String = "Report a Problem") {
        ControllerPrompt.shared.ask(settings.localized(title), message: settings.localized(message), actions: [.ok])
    }
}
