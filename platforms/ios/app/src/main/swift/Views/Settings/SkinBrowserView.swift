// SkinBrowserView.swift — browse and install community controller skins
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SkinBrowserView: View {
    @StateObject private var catalog = SkinCatalog()
    @StateObject private var installer = SkinInstaller()
    // Held directly so the rows invalidate off the library itself rather than
    // off whatever the installer happens to be publishing.
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var searchText = ""
    @State private var errorAlert: String?
    @State private var previewSkin: CatalogSkin?
    @State private var skinPendingRemoval: CatalogSkin?

    var body: some View {
        List {
            if let updated = catalog.lastUpdated {
                HStack(spacing: 4) {
                    Text("Updated")
                    Text(updated, style: .relative)
                    Text("ago")
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            if catalog.isLoading {
                HStack { Spacer(); ProgressView(); Spacer() }
            }

            if let error = catalog.lastError {
                VStack(alignment: .leading, spacing: 8) {
                    Text(catalog.skins.isEmpty
                         ? "Can't reach the skin catalog. Check your connection and pull down to try again."
                         : error)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button("Retry") { Task { await catalog.fetch(force: true) } }
                }
            }

            if catalog.skins.isEmpty && !catalog.isLoading && catalog.lastError == nil {
                Text("No skins are published yet.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if !catalog.skins.isEmpty && filteredSkins.isEmpty {
                Text("No skins match that search.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            ForEach(filteredSkins) { skin in
                skinRow(skin)
            }
        }
        .searchable(text: $searchText, prompt: "Search skins")
        .navigationTitle("Skins")
        .navigationBarTitleDisplayMode(.inline)
        .task { await catalog.fetch() }
        .refreshable { await catalog.fetch(force: true) }
        .alert("Download Failed", isPresented: Binding(
            get: { errorAlert != nil },
            set: { if !$0 { errorAlert = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(errorAlert ?? "")
        }
        .alert(
            "Remove Skin?",
            isPresented: Binding(
                get: { skinPendingRemoval != nil },
                set: { if !$0 { skinPendingRemoval = nil } }
            ),
            presenting: skinPendingRemoval
        ) { skin in
            Button("Remove \(skin.name)", role: .destructive) {
                installer.uninstall(skin)
                skinPendingRemoval = nil
            }
            Button("Cancel", role: .cancel) { skinPendingRemoval = nil }
        } message: { _ in
            Text("This deletes the installed skin. Linked layout presets are kept.")
        }
        .sheet(item: $previewSkin) { skin in
            SkinPreviewSheet(skin: skin)
        }
    }

    /// Read here rather than inside a row closure so the library registers with
    /// the observation tracking that wraps body.
    private var installedFiles: Set<String> {
        Set(skinLibrary.importedDescriptors.compactMap(\.catalogID))
    }

    private var filteredSkins: [CatalogSkin] {
        let installed = installedFiles
        let matches = searchText.isEmpty ? catalog.skins : catalog.skins.filter { skin in
            skin.name.localizedCaseInsensitiveContains(searchText)
                || (skin.author?.localizedCaseInsensitiveContains(searchText) ?? false)
        }
        return matches.filter { installed.contains($0.file) }
            + matches.filter { !installed.contains($0.file) }
    }

    private func subtitle(for skin: CatalogSkin) -> String? {
        var parts: [String] = []
        if let author = skin.author, !author.isEmpty {
            parts.append(author)
        }
        if let size = skin.sizeBytes, size > 0 {
            parts.append(Int64(size).formatted(.byteCount(style: .file)))
        }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    @ViewBuilder
    private func skinRow(_ skin: CatalogSkin) -> some View {
        let isInstalled = installedFiles.contains(skin.file)

        HStack(spacing: 12) {
            if let url = SkinCatalog.previewURL(for: skin) {
                Button {
                    previewSkin = skin
                } label: {
                    AsyncImage(url: url) { image in
                        image.resizable().aspectRatio(contentMode: .fit)
                    } placeholder: {
                        RoundedRectangle(cornerRadius: 8)
                            .fill(.quaternary)
                            .overlay(Image(systemName: "photo").foregroundStyle(.secondary))
                    }
                    .frame(width: 80, height: 50)
                    .cornerRadius(8)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Preview \(skin.name)")
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(skin.name).font(.body)
                if let subtitle = subtitle(for: skin) {
                    Text(subtitle).font(.caption).foregroundStyle(.secondary)
                }
                if !skin.isIOSReady {
                    Text("No recommended layout")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
            }

            Spacer()

            if installer.installing.contains(skin.file) {
                ProgressView()
            } else if isInstalled {
                Menu {
                    Button {
                        Task { await installer.reinstall(skin) }
                    } label: {
                        Label("Reinstall", systemImage: "arrow.clockwise")
                    }
                    Button(role: .destructive) {
                        skinPendingRemoval = skin
                    } label: {
                        Label("Remove", systemImage: "trash")
                    }
                } label: {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                }
                .accessibilityLabel("\(skin.name) is installed. Reinstall or remove it.")
            } else {
                Button("Get") {
                    Task { await installer.install(skin) }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }

            if let error = installer.errors[skin.file] {
                Button {
                    errorAlert = error
                } label: {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Show the error from \(skin.name)")
            }
        }
        .swipeActions(edge: .trailing) {
            if isInstalled {
                Button(role: .destructive) {
                    skinPendingRemoval = skin
                } label: {
                    Label("Remove", systemImage: "trash")
                }
            }
        }
    }
}

private struct SkinPreviewSheet: View {
    let skin: CatalogSkin
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            AsyncImage(url: SkinCatalog.previewURL(for: skin)) { image in
                image.resizable().aspectRatio(contentMode: .fit)
            } placeholder: {
                ProgressView()
            }
            .padding()
            .navigationTitle(skin.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
