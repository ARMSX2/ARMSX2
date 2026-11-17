// ReportSheet.swift — the Report a Problem sheet
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI

struct ReportSheetRequest: Identifiable, Equatable {
    let id = UUID()
    let sessionID: String
    var pausedFramePNG: Data? = nil
}

struct ReportSheet: View {
    let request: ReportSheetRequest
    var onClose: (() -> Void)? = nil
    // Owned here so the presenting closure never reads it.
    @State var settings = SettingsStore.shared
    @State private var form = ReportForm()
    @State var working = false
    @State private var copied = false
    @State private var lastReportSize: Int?
    @State private var sharing: ShareSheetItem?
    // A controller can't type into the fields, so it types on OrbitKeys.
    @State private var keyboardField: ReportTextField?
    @State var photoJPEG: Data?
    @State var photoThumb: UIImage?
    @State var crashCopies = CrashCopies()
    @State var hasCrashPayload: Bool?
    @State var attachmentLoading = false
    @State var pickMode: FilePickMode?
    @State var showImporter = false
    @State var photoChoice = false
    @State var showPhotoPicker = false
    @State var photoItem: PhotosPickerItem?
    @Environment(\.dismiss) private var dismiss
    @Environment(\.menuControllerInputRouter) private var controllerInput

    private enum ReportAction { case share, copy }

    enum ReportTextField: String, Identifiable {
        case what, scene
        var id: String { rawValue }
    }

    private var controllerTargetOrder: [String] {
        ["report.done"] + ReportKind.allCases.indices.map { "report.chip.kind.\($0)" } + ["report.field.what"]
            + ReportPlace.allCases.indices.map { "report.chip.place.\($0)" }
            + ["report.field.scene", "report.photo"] + (photoThumb == nil ? [] : ["report.photo.remove"])
            + (hasCrashPayload == false ? ["report.attach-crash"] : []) + ["report.share-zip", "report.copy-summary"]
    }

    var body: some View {
        NavigationStack {
            Form {
                Section(settings.localized("What happened")) {
                    ReportChips(
                        options: ReportKind.allCases.map { .init(raw: $0.rawValue, label: settings.localized($0.rawValue)) },
                        selected: Set(form.kinds.map(\.rawValue)),
                        idPrefix: "report.chip.kind",
                        onTap: toggleKind
                    )
                    textRow("Describe it in your own words", text: $form.whatHappened, field: .what, id: "report.field.what")
                }

                Section(settings.localized("Where")) {
                    ReportChips(
                        options: ReportPlace.allCases.map { .init(raw: $0.rawValue, label: settings.localized($0.rawValue)) },
                        selected: Set([form.place?.rawValue].compactMap { $0 }),
                        idPrefix: "report.chip.place",
                        onTap: togglePlace
                    )
                    textRow("Level or scene (optional)", text: $form.scene, field: .scene, id: "report.field.scene")
                }

                photoSection
                crashSection

                Section {
                    Button { start(.share) } label: {
                        Label(settings.localized("Share Zip"), systemImage: "square.and.arrow.up")
                    }
                    .controllerAccessibilityTargetID("report.share-zip")
                    .disabled(attachmentBusy)

                    Button { start(.copy) } label: {
                        Label(settings.localized("Copy Summary"), systemImage: "doc.on.doc")
                    }
                    .controllerAccessibilityTargetID("report.copy-summary")
                    .disabled(attachmentBusy)

                    if working { ProgressView() }
                    if let lastReportSize {
                        let size = ByteCountFormatter.string(fromByteCount: Int64(lastReportSize), countStyle: .file)
                        Text("\(settings.localized("Report size:")) \(size)")
                        Text(settings.localized("Saved in Files > ARMSX2 > Reports."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    if copied { Text(settings.localized("Summary copied.")) }
                } footer: {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(settings.localized("Nothing is sent until you share it. Folder paths are removed and the pairing file is never included."))
                        Text(settings.localized("The RetroAchievements account name and token are removed."))
                    }
                }
            }
            .attachmentPickers(of: self)
            .navigationTitle(settings.localized("Report a Problem"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) { close() }
                        .disabled(working)
                        .controllerAccessibilityActionTarget(
                            id: "report.done",
                            label: settings.localized("Done"),
                            activationFeedback: .back
                        ) {
                            guard !working else { return }
                            close()
                        }
                }
            }
        }
        .interactiveDismissDisabled(working)
        .presentationDetents([.medium, .large])
        .sheet(item: $sharing) { ActivityShareSheet(activityItems: [$0.url]) }
        .fullScreenCover(item: $keyboardField) { field in
            OrbitKeysKeyboardView(
                title: settings.localized(field == .what ? "Describe it in your own words" : "Level or scene (optional)"),
                initialText: field == .what ? form.whatHappened : form.scene,
                startsInNormalKeyboard: true,
                onCommit: { text in
                    if field == .what { form.whatHappened = text } else { form.scene = text }
                    keyboardField = nil
                },
                onCancel: { keyboardField = nil }
            )
            .presentationBackground(.clear)
            .appStatusBarHidden()
        }
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            isActive: keyboardField == nil && !working,
            scopeKey: "report.sheet",
            priority: 345,
            orbStyle: .plain,
            onBack: {
                guard !working else { return true }
                close()
                return true
            },
            confinesHorizontalFocusMovement: true,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            preferredInitialFocusLabel: "report.share-zip",
            declaredTargetOrder: controllerTargetOrder
        )
    }

    private func textRow(_ key: String, text: Binding<String>, field: ReportTextField, id: String) -> some View {
        TextField(settings.localized(key), text: text, axis: .vertical)
            .lineLimit(field == .what ? 3...8 : 1...3)
            .controllerAccessibilityActionTarget(id: id, label: settings.localized(key)) { keyboardField = field }
    }

    private func toggleKind(_ raw: String) {
        guard let kind = ReportKind(rawValue: raw) else { return }
        if let index = form.kinds.firstIndex(of: kind) { form.kinds.remove(at: index) } else { form.kinds.append(kind) }
    }

    private func togglePlace(_ raw: String) {
        let place = ReportPlace(rawValue: raw)
        form.place = form.place == place ? nil : place
    }

    private func close() {
        if let onClose { onClose() } else { dismiss() }
    }

    // `working` is set before the task starts, so a second tap cannot begin another run.
    private func start(_ action: ReportAction) {
        guard !working else { return }
        working = true
        copied = false
        lastReportSize = nil
        let sessionID = request.sessionID
        let displayMaxFPS = UIScreen.main.maximumFramesPerSecond
        let form = form
        let extras = ReportExtras(form: form, photoJPEG: photoJPEG, pausedFramePNG: request.pausedFramePNG,
                                  crashFiles: crashCopies.urls)
        Task {
            defer { working = false }
            do {
                switch action {
                case .share:
                    let url = try await Task.detached {
                        try await ReportBuilder.build(sessionID: sessionID, displayMaxFPS: displayMaxFPS, extras: extras)
                    }.value
                    lastReportSize = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize
                    sharing = ShareSheetItem(url: url)
                case .copy:
                    UIPasteboard.general.string = try await Task.detached {
                        try await ReportBuilder.summaryText(sessionID: sessionID, displayMaxFPS: displayMaxFPS, form: form)
                    }.value
                    copied = true
                }
            } catch {
                ControllerPrompt.shared.ask(settings.localized("Report a Problem"), message: "\(error)", actions: [.ok])
            }
        }
    }
}
