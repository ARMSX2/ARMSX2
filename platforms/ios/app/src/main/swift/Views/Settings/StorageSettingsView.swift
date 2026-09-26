// StorageSettingsView.swift — cache cleanup tools
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UniformTypeIdentifiers

private struct StorageReport: Sendable, Equatable {
    var appCacheBytes: Int64 = 0
    var textureDumpBytes: Int64 = 0
    var diagnosticLogBytes: Int64 = 0
    var stateSafetyBackupBytes: Int64 = 0

    var totalGeneratedBytes: Int64 {
        appCacheBytes + textureDumpBytes + diagnosticLogBytes + stateSafetyBackupBytes
    }
}

private struct StorageClearResult: Sendable {
    let bytesRemoved: Int64
    let failures: [String]
}

private struct StoragePaths: Sendable {
    let documentsURL: URL
    let emulatorCacheURL: URL
    let systemCachesURL: URL?
    let temporaryURL: URL
    let textureRootURL: URL
    let logsURL: URL
    let stateSafetyBackupsURL: URL
    let rootLogURLs: [URL]

    @MainActor
    static func current() -> StoragePaths {
        let fileManager = FileManager.default
        let documentsURL = URL(fileURLWithPath: ARMSX2Bridge.documentsDirectory(), isDirectory: true)
        let systemCachesURL = fileManager.urls(for: .cachesDirectory, in: .userDomainMask).first
        return StoragePaths(
            documentsURL: documentsURL,
            emulatorCacheURL: documentsURL.appendingPathComponent("cache", isDirectory: true),
            systemCachesURL: systemCachesURL,
            temporaryURL: URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true),
            textureRootURL: documentsURL.appendingPathComponent("textures", isDirectory: true),
            logsURL: documentsURL.appendingPathComponent("logs", isDirectory: true),
            stateSafetyBackupsURL: documentsURL.appendingPathComponent("memcard-state-backups", isDirectory: true),
            rootLogURLs: [
                documentsURL.appendingPathComponent("pcsx2_log.txt", isDirectory: false),
                documentsURL.appendingPathComponent("emulog.txt", isDirectory: false)
            ]
        )
    }
}

private struct StorageExternalGamesAlertCommandListener: View {
    let controllerInput: MenuControllerInputRouter
    let onCommand: (MenuControllerCommand) -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput.latestEvent) { _, event in
                guard let event,
                      event.captureOwner
                        == MenuControllerNavigationCaptureOwner
                            .storageExternalGamesAlert else {
                    return
                }
                onCommand(event.command)
            }
    }
}

private enum StorageClearAction: Identifiable, Sendable {
    case appCache
    case textureDumps
    case diagnosticLogs
    case allGenerated

    var id: String {
        switch self {
        case .appCache: return "appCache"
        case .textureDumps: return "textureDumps"
        case .diagnosticLogs: return "diagnosticLogs"
        case .allGenerated: return "allGenerated"
        }
    }

    var title: String {
        switch self {
        case .appCache: return "Clear App Cache"
        case .textureDumps: return "Clear Texture Dumps"
        case .diagnosticLogs: return "Clear Diagnostic Logs"
        case .allGenerated: return "Clear All Generated Files"
        }
    }

    var confirmationMessage: String {
        switch self {
        case .appCache:
            return "This removes emulator cache, iOS cache, temporary files, and save-state safety backups. Games, BIOS, saves, memory cards, covers, settings, and texture packs are preserved."
        case .textureDumps:
            return "This removes generated texture dump folders only. Replacement texture packs are preserved, and texture dumping will be turned off so storage does not refill immediately."
        case .diagnosticLogs:
            return "This clears pcsx2_log.txt and generated diagnostic logs. It is useful before sending a fresh bug report, but it removes old logs from the app container."
        case .allGenerated:
            return "This removes app cache, temporary files, save-state safety backups, generated texture dumps, and diagnostic logs. Games, BIOS, saves, memory cards, covers, settings, and replacement texture packs are preserved."
        }
    }

    var includesTextureDumps: Bool {
        self == .textureDumps || self == .allGenerated
    }

    var includesDiagnosticLogs: Bool {
        self == .diagnosticLogs || self == .allGenerated
    }
}

private enum StorageCleaner {
    static func report(paths: StoragePaths) async -> StorageReport {
        await Task.detached(priority: .utility) {
            awaitableReport(paths: paths)
        }.value
    }

    static func clear(action: StorageClearAction, paths: StoragePaths) async -> StorageClearResult {
        await Task.detached(priority: .utility) {
            let before = awaitableReport(paths: paths)
            var failures: [String] = []

            if action == .appCache || action == .allGenerated {
                clearContents(of: paths.emulatorCacheURL, recreate: true, failures: &failures)
                clearContents(of: paths.systemCachesURL, recreate: true, failures: &failures)
                clearContents(of: paths.temporaryURL, recreate: true, failures: &failures)
                clearContents(of: paths.stateSafetyBackupsURL, recreate: true, failures: &failures)
            }

            if action.includesTextureDumps {
                for dumpURL in textureDumpDirectories(in: paths.textureRootURL) {
                    removeItem(at: dumpURL, failures: &failures)
                }
            }

            if action.includesDiagnosticLogs {
                clearContents(of: paths.logsURL, recreate: true, failures: &failures)
                for logURL in paths.rootLogURLs {
                    truncateOrRemoveFile(at: logURL, failures: &failures)
                }
            }

            let after = awaitableReport(paths: paths)
            return StorageClearResult(bytesRemoved: max(0, before.totalGeneratedBytes - after.totalGeneratedBytes), failures: failures)
        }.value
    }

    private static func awaitableReport(paths: StoragePaths) -> StorageReport {
        StorageReport(
            appCacheBytes: directorySize(paths.emulatorCacheURL) + directorySize(paths.systemCachesURL) + directorySize(paths.temporaryURL),
            textureDumpBytes: textureDumpDirectories(in: paths.textureRootURL).reduce(Int64(0)) { partial, url in
                partial + directorySize(url)
            },
            diagnosticLogBytes: directorySize(paths.logsURL) + paths.rootLogURLs.reduce(Int64(0)) { partial, url in
                partial + directorySize(url)
            },
            stateSafetyBackupBytes: directorySize(paths.stateSafetyBackupsURL)
        )
    }

    private static func directorySize(_ url: URL?) -> Int64 {
        guard let url else { return 0 }

        let fileManager = FileManager.default
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory) else { return 0 }

        if !isDirectory.boolValue {
            return fileSize(url)
        }

        var total: Int64 = 0
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileAllocatedSizeKey, .totalFileAllocatedSizeKey]
        guard let enumerator = fileManager.enumerator(
            at: url,
            includingPropertiesForKeys: keys,
            options: [.skipsPackageDescendants],
            errorHandler: { _, _ in true }
        ) else {
            return 0
        }

        for case let fileURL as URL in enumerator {
            total += fileSize(fileURL)
        }

        return total
    }

    private static func fileSize(_ url: URL) -> Int64 {
        let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileAllocatedSizeKey, .totalFileAllocatedSizeKey])
        guard values?.isRegularFile == true else { return 0 }
        return Int64(values?.totalFileAllocatedSize ?? values?.fileAllocatedSize ?? 0)
    }

    private static func clearContents(of url: URL?, recreate: Bool, failures: inout [String]) {
        guard let url else { return }

        let fileManager = FileManager.default
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            if recreate {
                do {
                    try fileManager.createDirectory(at: url, withIntermediateDirectories: true)
                } catch {
                    failures.append("\(url.lastPathComponent): \(error.localizedDescription)")
                }
            }
            return
        }

        do {
            for child in try fileManager.contentsOfDirectory(at: url, includingPropertiesForKeys: nil) {
                removeItem(at: child, failures: &failures)
            }
            if recreate {
                try fileManager.createDirectory(at: url, withIntermediateDirectories: true)
            }
        } catch {
            failures.append("\(url.lastPathComponent): \(error.localizedDescription)")
        }
    }

    private static func removeItem(at url: URL, failures: inout [String]) {
        do {
            try FileManager.default.removeItem(at: url)
        } catch {
            failures.append("\(url.lastPathComponent): \(error.localizedDescription)")
        }
    }

    private static func truncateOrRemoveFile(at url: URL, failures: inout [String]) {
        let fileManager = FileManager.default
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
            return
        }

        do {
            let handle = try FileHandle(forWritingTo: url)
            try handle.truncate(atOffset: 0)
            try handle.close()
        } catch {
            removeItem(at: url, failures: &failures)
        }
    }

    private static func textureDumpDirectories(in textureRootURL: URL) -> [URL] {
        let fileManager = FileManager.default
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: textureRootURL.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            return []
        }

        let keys: [URLResourceKey] = [.isDirectoryKey]
        guard let enumerator = fileManager.enumerator(
            at: textureRootURL,
            includingPropertiesForKeys: keys,
            options: [.skipsHiddenFiles, .skipsPackageDescendants],
            errorHandler: { _, _ in true }
        ) else {
            return []
        }

        var dumpDirectories: [URL] = []
        for case let url as URL in enumerator {
            guard url.lastPathComponent == "dumps" else { continue }
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey])
            if values?.isDirectory == true {
                dumpDirectories.append(url)
                enumerator.skipDescendants()
            }
        }

        return dumpDirectories
    }
}

struct StorageSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var externalLibrary = ExternalGameLibrary.shared
    @State private var report = StorageReport()
    @State private var isWorking = false
    @State private var pendingAction: StorageClearAction?
    @State private var resultMessage: String?
    @State private var showResult = false
    @State private var showExternalGameFilePicker = false
    @State private var showExternalFolderPicker = false
    @State private var externalActionMessage: String?
    @Environment(\.menuControllerInputRouter) private var controllerInput

    private var controllerTargetOrder: [String] {
        let cleanupActions = [
                "settings.storage.clear-cache",
                "settings.storage.clear-textures",
                "settings.storage.clear-logs",
                "settings.storage.clear-all",
            ]
        return (isWorking ? [] : ["settings.storage.refresh"])
            + ["settings.storage.add-file", "settings.storage.add-folder"]
            + externalLibrary.directories.map { "settings.storage.remove.\($0.id)" }
            + (isWorking ? [] : cleanupActions)
    }

    private var externalGameFileContentTypes: [UTType] {
        var types: [UTType] = [.item, .data, .content]
        for type in FileImportHandler.gameContentTypes where !types.contains(type) {
            types.append(type)
        }
        return types
    }

    var body: some View {
        ZStack {
            Form {
                Section(settings.localized("Usage")) {
                LabeledContent(settings.localized("App Cache"), value: formatBytes(report.appCacheBytes))
                LabeledContent(settings.localized("Texture Dumps"), value: formatBytes(report.textureDumpBytes))
                LabeledContent(settings.localized("Diagnostic Logs"), value: formatBytes(report.diagnosticLogBytes))
                LabeledContent(settings.localized("State Safety Backups"), value: formatBytes(report.stateSafetyBackupBytes))
                LabeledContent(settings.localized("Generated Total"), value: formatBytes(report.totalGeneratedBytes))

                Button {
                    Task { @MainActor in await refreshReport() }
                } label: {
                    Label(settings.localized("Refresh Storage Usage"), systemImage: "arrow.clockwise")
                }
                .controllerAccessibilityTargetID("settings.storage.refresh")
                .disabled(isWorking)
            }

                Section {
                Button {
                    showExternalGameFilePicker = true
                } label: {
                    Label(settings.localized("Add External Game File"), systemImage: "doc.badge.plus")
                }
                .controllerAccessibilityTargetID("settings.storage.add-file")

                Button {
                    showExternalFolderPicker = true
                } label: {
                    Label(settings.localized("Add External Game Folder"), systemImage: "externaldrive.badge.plus")
                }
                .controllerAccessibilityTargetID("settings.storage.add-folder")

                if externalLibrary.directories.isEmpty {
                    ContentUnavailableView(
                        settings.localized("No External Games"),
                        systemImage: "externaldrive",
                        description: Text(settings.localized("Add a folder or game file from Files to play without copying it into ARMSX2."))
                    )
                    .frame(maxWidth: .infinity)
                } else {
                    ForEach(externalLibrary.directories) { location in
                        HStack(alignment: .top, spacing: 12) {
                            Image(systemName: location.isDirectory ? "externaldrive" : "doc")
                                .foregroundStyle(.secondary)
                                .frame(width: 24)

                            VStack(alignment: .leading, spacing: 4) {
                                Text(location.displayName)
                                    .font(.body.weight(.medium))
                                Text(location.path)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(2)
                                    .textSelection(.enabled)
                            }

                            Spacer()

                            Button(role: .destructive) {
                                externalLibrary.removeDirectory(id: location.id)
                            } label: {
                                Image(systemName: "trash")
                            }
                            .buttonStyle(.borderless)
                            .controllerAccessibilityActionTarget(
                                id: "settings.storage.remove.\(location.id)",
                                label: settings.localized("Remove") + " " + location.displayName
                            ) {
                                externalLibrary.removeDirectory(id: location.id)
                            }
                        }
                        .padding(.vertical, 4)
                    }
                }
            } header: {
                Text(settings.localized("External Games"))
            } footer: {
                Text(settings.localized("USB/SSD folders can be scanned and played directly. Removing an entry only removes ARMSX2's bookmark and does not delete the game."))
            }

                Section(settings.localized("Cleanup")) {
                Button(role: .destructive) {
                    pendingAction = .appCache
                } label: {
                    Label(settings.localized("Clear App Cache"), systemImage: "trash")
                }
                .controllerAccessibilityTargetID("settings.storage.clear-cache")
                .disabled(isWorking)

                Button(role: .destructive) {
                    pendingAction = .textureDumps
                } label: {
                    Label(settings.localized("Clear Texture Dumps"), systemImage: "photo.stack")
                }
                .controllerAccessibilityTargetID("settings.storage.clear-textures")
                .disabled(isWorking)

                Button(role: .destructive) {
                    pendingAction = .diagnosticLogs
                } label: {
                    Label(settings.localized("Clear Diagnostic Logs"), systemImage: "doc.text.magnifyingglass")
                }
                .controllerAccessibilityTargetID("settings.storage.clear-logs")
                .disabled(isWorking)

                Button(role: .destructive) {
                    pendingAction = .allGenerated
                } label: {
                    Label(settings.localized("Clear All Generated Files"), systemImage: "externaldrive.badge.xmark")
                }
                .controllerAccessibilityTargetID("settings.storage.clear-all")
                .disabled(isWorking)

                if isWorking {
                    ProgressView(settings.localized("Cleaning..."))
                }

                Text(settings.localized("This never removes imported games, BIOS files, save states, memory cards, covers, settings, PNACH files, or replacement texture packs."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
            }
            .controllerAccessibilityTargetOrder(controllerTargetOrder)

            if showsExternalGamesControllerAlert,
               let externalActionMessage {
                ControllerNavigationAlert(
                    title: settings.localized("External Games"),
                    message: settings.localized(externalActionMessage),
                    actions: [
                        .init(id: "ok", title: settings.localized("OK")),
                    ],
                    selectedIndex: 0,
                    onSelect: { _ in
                        dismissExternalGamesAlert(feedback: .activate)
                    },
                    onDismiss: {
                        dismissExternalGamesAlert(feedback: .back)
                    }
                )
                // This modal owns its selection while the Settings session is
                // retained underneath it. Do not register the OK action as a
                // second target in that underlying Form graph.
                .environment(\.controllerAccessibilityNavigationActive, false)
                .environment(\.controllerAccessibilityNavigationSession, nil)
                .overlay {
                    if let controllerInput {
                        StorageExternalGamesAlertCommandListener(
                            controllerInput: controllerInput,
                            onCommand: handleExternalGamesAlertCommand
                        )
                    }
                }
                .zIndex(20_000)
            }
        }
        .navigationTitle(settings.localized("Storage"))
        .navigationBarTitleDisplayMode(.inline)
        .task {
            externalLibrary.reload()
            await refreshReport()
        }
        .onChange(of: externalActionMessage) { _, _ in
            updateExternalGamesAlertCapture()
        }
        .onChange(of: controllerInput?.hasConnectedController) { _, _ in
            updateExternalGamesAlertCapture()
        }
        .onDisappear {
            controllerInput?.setNavigationCaptured(
                false,
                owner: MenuControllerNavigationCaptureOwner
                    .storageExternalGamesAlert,
                priority: 1_000
            )
        }
        .sheet(isPresented: $showExternalGameFilePicker) {
            ImportDocumentPicker(
                allowedContentTypes: externalGameFileContentTypes,
                allowsMultipleSelection: false,
                legacyDocumentTypes: ["public.item", "public.data", "public.content"],
                legacyDocumentMode: .open,
                asCopy: false
            ) { result in
                showExternalGameFilePicker = false
                switch result {
                case .success(let urls):
                    guard let url = urls.first else {
                        externalActionMessage = "No external game was selected."
                        return
                    }
                    externalActionMessage = externalLibrary.addLocation(url)
                case .failure(let error):
                    if !FileImportHandler.isUserCancelledPickerError(error) {
                        externalActionMessage = "External game could not be added.\n\(error.localizedDescription)"
                    }
                }
            }
        }
        .sheet(isPresented: $showExternalFolderPicker) {
            ImportDocumentPicker(
                allowedContentTypes: [.folder],
                allowsMultipleSelection: false,
                legacyDocumentTypes: ["public.folder", "public.directory"],
                legacyDocumentMode: .open,
                asCopy: false
            ) { result in
                showExternalFolderPicker = false
                switch result {
                case .success(let urls):
                    guard let url = urls.first else {
                        externalActionMessage = "No external folder was selected."
                        return
                    }
                    externalActionMessage = externalLibrary.addLocation(url)
                case .failure(let error):
                    if !FileImportHandler.isUserCancelledPickerError(error) {
                        externalActionMessage = "External folder could not be added.\n\(error.localizedDescription)"
                    }
                }
            }
        }
        .confirmationDialog(
            settings.localized(pendingAction?.title ?? "Clear Cache"),
            isPresented: Binding(
                get: { pendingAction != nil },
                set: { if !$0 { pendingAction = nil } }
            ),
            titleVisibility: .visible
        ) {
            if let pendingAction {
                Button(settings.localized(pendingAction.title), role: .destructive) {
                    let action = pendingAction
                    self.pendingAction = nil
                    Task { @MainActor in await clear(action) }
                }
            }

            Button(settings.localized("Cancel"), role: .cancel) {
                pendingAction = nil
            }
        } message: {
            Text(settings.localized(pendingAction?.confirmationMessage ?? ""))
        }
        .alert(settings.localized("Storage Cleanup"), isPresented: $showResult) {
            Button(settings.localized("OK")) {}
        } message: {
            Text(resultMessage ?? "")
        }
        .alert(
            settings.localized("External Games"),
            isPresented: Binding(
                get: {
                    externalActionMessage != nil
                        && !showsExternalGamesControllerAlert
                },
                set: { if !$0 { externalActionMessage = nil } }
            )
        ) {
            Button(settings.localized("OK")) {
                externalActionMessage = nil
            }
        } message: {
            Text(externalActionMessage ?? "")
        }
    }

    private var showsExternalGamesControllerAlert: Bool {
        externalActionMessage != nil
            && controllerInput?.hasConnectedController == true
    }

    @MainActor
    private func updateExternalGamesAlertCapture() {
        guard let controllerInput else { return }
        if showsExternalGamesControllerAlert {
            // A document picker is usually touch-owned. Restore controller
            // presentation before claiming the modal so OK is immediately
            // visible and the first press cannot reach Storage underneath.
            controllerInput.claimPresentedNavigationFocus()
        }
        controllerInput.setNavigationCaptured(
            showsExternalGamesControllerAlert,
            owner: MenuControllerNavigationCaptureOwner
                .storageExternalGamesAlert,
            priority: 1_000
        )
    }

    @MainActor
    private func handleExternalGamesAlertCommand(
        _ command: MenuControllerCommand
    ) {
        guard showsExternalGamesControllerAlert else { return }
        switch command {
        case .activate:
            dismissExternalGamesAlert(feedback: .activate)
        case .back:
            dismissExternalGamesAlert(feedback: .back)
        case .up, .upLeft, .left, .downLeft,
             .upRight, .right, .downRight, .down,
             .toggleFavorite, .showContextMenu,
             .previousTab, .nextTab:
            controllerInput?.playFeedback(.boundary)
        }
    }

    @MainActor
    private func dismissExternalGamesAlert(
        feedback: MenuControllerFeedback
    ) {
        guard externalActionMessage != nil else { return }
        controllerInput?.setNavigationCaptured(
            false,
            owner: MenuControllerNavigationCaptureOwner
                .storageExternalGamesAlert,
            priority: 1_000
        )
        externalActionMessage = nil
        controllerInput?.playFeedback(feedback)
    }

    @MainActor
    private func refreshReport() async {
        let paths = StoragePaths.current()
        report = await StorageCleaner.report(paths: paths)
    }

    @MainActor
    private func clear(_ action: StorageClearAction) async {
        isWorking = true
        let paths = StoragePaths.current()
        let result = await StorageCleaner.clear(action: action, paths: paths)

        if action.includesTextureDumps {
            settings.dumpReplaceableTextures = false
        }

        report = await StorageCleaner.report(paths: paths)
        isWorking = false

        var message = String(format: settings.localized("Removed about %@ of generated data."), formatBytes(result.bytesRemoved))
        if !result.failures.isEmpty {
            message += "\n\n\(settings.localized("Some files could not be removed:"))\n\(result.failures.prefix(5).joined(separator: "\n"))"
        }
        resultMessage = message
        showResult = true
    }

    private func formatBytes(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}
