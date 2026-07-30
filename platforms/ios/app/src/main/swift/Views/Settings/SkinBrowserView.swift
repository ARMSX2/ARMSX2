// SkinBrowserView.swift — browse and install community controller skins
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SkinBrowserView: View {
    /// Ready means the skin ships a layout for iOS. The rest still install, they
    /// just leave you to place the buttons.
    private enum Filter: Hashable, CaseIterable {
        case all, installed, ready

        var title: String {
            switch self {
            case .all: return "All"
            case .installed: return "Installed"
            case .ready: return "Ready"
            }
        }
    }

    @StateObject private var catalog = SkinCatalog()
    @StateObject private var installer = SkinInstaller()
    // Held directly so the rows invalidate off the library itself rather than
    // off whatever the installer happens to be publishing.
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var searchText = ""
    @State private var filter: Filter = .all
    @State private var detailAlert: String?
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
                    Text(error)
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

            if !catalog.skins.isEmpty {
                Picker("Show", selection: $filter) {
                    ForEach(Filter.allCases, id: \.self) { option in
                        Text(option.title).tag(option)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .listRowSeparator(.hidden)
            }

            if !catalog.skins.isEmpty && filteredSkins.isEmpty {
                Text(emptyResultMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            ForEach(filteredSkins) { skin in
                skinRow(skin)
            }
        }
        // Pin this to the drawer. Left alone, iOS 26 puts the field at the
        // bottom of the screen, which is where our tab bar lives, and the bar
        // wins on z order. Always rather than automatic, so the field is
        // sitting there instead of needing a pull down to find it.
        .searchable(
            text: $searchText,
            placement: .navigationBarDrawer(displayMode: .always),
            prompt: "Search skins"
        )
        .navigationTitle("Skins")
        .navigationBarTitleDisplayMode(.inline)
        .task { await catalog.fetch() }
        .refreshable { await catalog.fetch(force: true) }
        .alert("Skin Install", isPresented: Binding(
            get: { detailAlert != nil },
            set: { if !$0 { detailAlert = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(detailAlert ?? "")
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

    /// Everything the filter allows, before the search query narrows it. Kept
    /// apart so the empty state can tell which of the two emptied the list.
    private var filteredByCategory: [CatalogSkin] {
        let installed = installedFiles
        switch filter {
        case .all: return catalog.skins
        case .installed: return catalog.skins.filter { installed.contains($0.file) }
        case .ready: return catalog.skins.filter(\.isIOSReady)
        }
    }

    private var filteredSkins: [CatalogSkin] {
        let installed = installedFiles
        // localizedStandard rather than localizedCaseInsensitive so an accent
        // in the catalog doesn't hide a skin from someone typing without one.
        let matches = searchText.isEmpty ? filteredByCategory : filteredByCategory.filter { skin in
            skin.name.localizedStandardContains(searchText)
                || (skin.author?.localizedStandardContains(searchText) ?? false)
        }
        return matches.filter { installed.contains($0.file) }
            + matches.filter { !installed.contains($0.file) }
    }

    /// Blame whichever one actually emptied the list. Telling someone their
    /// search found nothing when it was the filter is just misleading.
    private var emptyResultMessage: String {
        if filteredByCategory.isEmpty {
            switch filter {
            case .all: return "No skins match that search."
            case .installed: return "You haven't installed any skins yet."
            case .ready: return "No skins ship a recommended layout yet."
            }
        }
        return "No skins match that search."
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
                    detailAlert = error
                } label: {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Show the error from \(skin.name)")
            } else if let notice = installer.notices[skin.file] {
                Button {
                    detailAlert = notice
                } label: {
                    Image(systemName: "exclamationmark.circle")
                        .foregroundStyle(.yellow)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Show what \(skin.name) reported during install")
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
