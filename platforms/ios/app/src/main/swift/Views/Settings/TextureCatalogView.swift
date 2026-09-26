// TextureCatalogView.swift — download texture packs from the shared catalogue
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private enum TextureDownloadError: String, LocalizedError {
    case unreachable = "Can't reach the texture pack server. Check your connection and try again."
    case damaged = "The download was damaged, so nothing was installed. Try again."
    var errorDescription: String? { rawValue }
}

// The async download API reports progress only through the task it creates.
private final class TaskTap: NSObject, URLSessionTaskDelegate {
    let created: @Sendable (URLSessionTask) -> Void

    init(_ created: @escaping @Sendable (URLSessionTask) -> Void) {
        self.created = created
    }

    func urlSession(_ session: URLSession, didCreateTask task: URLSessionTask) {
        created(task)
    }
}

struct TextureCatalogView: View {
    // Set in per-game settings, where only that game's packs are listed.
    private let serial: String?
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var packs: [TextureCatalogPack]?
    @State private var loadFailed = false
    @State private var owned: Set<String> = []
    @State private var installedIDs: Set<String> = []
    @State private var searchText = ""
    @State private var active: String?
    // Nil while the active pack unpacks, which can't be cancelled.
    @State private var progress: Progress?
    @State private var job: Task<Void, Never>?
    @State private var failure: String?

    init(serial: String? = nil) {
        self.serial = serial.map { TextureCatalog.normalizedSerial($0) ?? $0 }
    }

    var body: some View {
        List {
            if let packs {
                let shown = packs.filter { (serial.map($0.serials.contains) ?? true) && matches($0) }
                if shown.isEmpty {
                    Text(settings.localized(searchText.isEmpty ? "No texture packs for this game yet." : "Nothing here matches that search."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                section(settings.localized("Your Games"), shown.filter { !owned.isDisjoint(with: $0.serials) })
                section(settings.localized("Other Games"), shown.filter { owned.isDisjoint(with: $0.serials) })
            } else if loadFailed {
                VStack(alignment: .leading, spacing: 8) {
                    Text(settings.localized("Can't reach the texture pack server. Check your connection and try again."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button(settings.localized("Retry")) { Task { await load() } }
                }
            } else {
                HStack { Spacer(); ProgressView(); Spacer() }
            }
        }
        .searchable(text: $searchText, placement: .navigationBarDrawer(displayMode: .always))
        .navigationTitle(settings.localized("Download Texture Packs"))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button(settings.localized("Done")) { dismiss() }
                    .disabled(active != nil)
            }
        }
        // Closing mid-install would cancel the download or reload the pack list before the pack lands.
        .interactiveDismissDisabled(active != nil)
        .task { await load() }
        .alert(
            settings.localized("Texture Packs"),
            isPresented: Binding(
                get: { failure != nil },
                set: { if !$0 { failure = nil } }
            )
        ) {
            Button(settings.localized("OK")) { failure = nil }
        } message: {
            Text(failure ?? "")
        }
    }

    @ViewBuilder
    private func section(_ title: String, _ packs: [TextureCatalogPack]) -> some View {
        if !packs.isEmpty {
            Section(title) {
                ForEach(packs) { row($0) }
            }
        }
    }

    private func row(_ pack: TextureCatalogPack) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(pack.name)
                    .font(.body.weight(.medium))
                Text("\(pack.serials.joined(separator: ", ")) · \(size(pack.bytes))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Link(pack.authors.joined(separator: ", "), destination: pack.source)
                    .font(.caption)
                    .multilineTextAlignment(.leading)
                    .lineLimit(2)
                if active == pack.id, let progress {
                    ProgressView(progress)
                        .labelsHidden()
                }
            }

            Spacer()

            if active == pack.id {
                if progress != nil {
                    Button(settings.localized("Cancel")) { job?.cancel() }
                } else {
                    ProgressView()
                }
            } else if installedIDs.contains(pack.id) {
                Text(settings.localized("Installed"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Button(settings.localized("Get")) {
                    job = Task { await install(pack) }
                }
                .disabled(active != nil)
            }
        }
        .buttonStyle(.borderless)
        .padding(.vertical, 4)
    }

    private func matches(_ pack: TextureCatalogPack) -> Bool {
        searchText.isEmpty || ([pack.name, pack.gameTitle] + pack.serials + pack.authors)
            .contains { $0.localizedStandardContains(searchText) }
    }

    private func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    private func load() async {
        loadFailed = false
        let root = TexturePackLibrary.root
        let running = ARMSX2Bridge.currentTextureSerial()
        let (titles, ids) = await Task.detached(priority: .utility) {
            (TexturePackLibrary.titlesBySerial(), TexturePackLibrary.catalogIDs(in: root))
        }.value
        owned = serial.map { [$0] } ?? Set((Array(titles.keys) + [running]).compactMap(TextureCatalog.normalizedSerial))
        installedIDs = ids
        guard let loaded = await TextureCatalog.load() else {
            loadFailed = true
            return
        }
        packs = loaded.sorted {
            "\($0.gameTitle) \($0.name)".localizedStandardCompare("\($1.gameTitle) \($1.name)") == .orderedAscending
        }
    }

    private func install(_ pack: TextureCatalogPack) async {
        let root = TexturePackLibrary.root
        let staging = root.appendingPathComponent(".import-\(UUID().uuidString)", isDirectory: true)
        let tracker = Progress(totalUnitCount: 1)
        // A locked phone suspends the app, and the download with it.
        let keepAwake = !UIApplication.shared.isIdleTimerDisabled
        UIApplication.shared.isIdleTimerDisabled = true
        active = pack.id
        progress = tracker
        defer {
            if keepAwake { UIApplication.shared.isIdleTimerDisabled = false }
            try? FileManager.default.removeItem(at: staging)
            active = nil
            progress = nil
        }

        do {
            try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
            // Android's margin: the archive, what it unpacks to (a zip's DDS barely compresses), and 512 MB.
            let needed = pack.bytes + (pack.isTarZstd ? pack.unpackedBytes : pack.bytes) + (512 << 20)
            let available = (try? staging.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
                .volumeAvailableCapacityForImportantUsage ?? .max
            guard available >= needed else {
                failure = String(format: settings.localized("%@ needs %@ of free space and %@ is available."),
                                 pack.name, size(needed), size(available))
                return
            }

            let archive = try await Self.download(pack, into: staging, tracker: tracker)
            progress = nil
            let serial = TextureCatalog.serial(for: pack, owned: owned)
            try await Task.detached(priority: .userInitiated) {
                let received = try archive.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
                guard Int64(received) == pack.bytes, try ShaderCatalogInstaller.sha256(of: archive) == pack.sha256 else {
                    throw TextureDownloadError.damaged
                }
                _ = try ARMSX2Bridge.installTexturePack(at: archive, serial: serial, fallbackSerial: "")
                try TexturePackLibrary.markCatalogPack(pack.id, in: root.appendingPathComponent("\(serial)/replacements"))
            }.value
            installedIDs.insert(pack.id)
            settings.loadTextureReplacements = true
            if serial == ARMSX2Bridge.currentTextureSerial() {
                ARMSX2Bridge.reloadTextureReplacements()
            }
        } catch is CancellationError {
        } catch {
            failure = settings.localized(error.localizedDescription)
        }
    }

    private static func download(_ pack: TextureCatalogPack, into folder: URL, tracker: Progress) async throws -> URL {
        let file: URL
        let response: URLResponse
        do {
            (file, response) = try await URLSession.shared.download(
                from: pack.download, delegate: TaskTap { tracker.addChild($0.progress, withPendingUnitCount: 1) })
        } catch {
            try Task.checkCancellation()
            throw TextureDownloadError.unreachable
        }
        defer { try? FileManager.default.removeItem(at: file) }
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw TextureDownloadError.unreachable }
        // The bridge tells tar.zst from zip by the extension.
        let archive = folder.appendingPathComponent(pack.fileName)
        try FileManager.default.moveItem(at: file, to: archive)
        return archive
    }
}
