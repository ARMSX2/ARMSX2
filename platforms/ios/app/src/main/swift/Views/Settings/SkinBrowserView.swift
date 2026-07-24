// SkinBrowserView.swift — browse and install community controller skins
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SkinBrowserView: View {
    @StateObject private var catalog = SkinCatalog()
    @StateObject private var installer: SkinInstaller
    @State private var searchText = ""

    init() {
        let catalog = SkinCatalog()
        _catalog = StateObject(wrappedValue: catalog)
        _installer = StateObject(wrappedValue: SkinInstaller(catalog: catalog))
    }

    var body: some View {
        List {
            if catalog.isLoading && catalog.skins.isEmpty {
                HStack { Spacer(); ProgressView(); Spacer() }
            }

            if let error = catalog.lastError, catalog.skins.isEmpty {
                VStack(spacing: 8) {
                    Text(error).font(.caption).foregroundStyle(.secondary)
                    Button("Retry") { Task { await catalog.fetch() } }
                }
            }

            ForEach(filteredSkins) { skin in
                skinRow(skin)
            }
        }
        .searchable(text: $searchText, prompt: "Search skins")
        .navigationTitle("Skins")
        .navigationBarTitleDisplayMode(.inline)
        .task { await catalog.fetch() }
        .refreshable { await catalog.fetch() }
    }

    private var filteredSkins: [CatalogSkin] {
        guard !searchText.isEmpty else { return catalog.skins }
        return catalog.skins.filter { $0.name.localizedCaseInsensitiveContains(searchText) }
    }

    @ViewBuilder
    private func skinRow(_ skin: CatalogSkin) -> some View {
        HStack(spacing: 12) {
            if let url = catalog.previewURL(for: skin) {
                AsyncImage(url: url) { image in
                    image.resizable().aspectRatio(contentMode: .fit)
                } placeholder: {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(.quaternary)
                        .overlay(Image(systemName: "photo").foregroundStyle(.secondary))
                }
                .frame(width: 64, height: 40)
                .cornerRadius(8)
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(skin.name).font(.body)
                if let author = skin.author {
                    Text(author).font(.caption).foregroundStyle(.secondary)
                }
            }

            Spacer()

            if installer.isInstalled(skin) {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.green)
            } else if installer.installingName == skin.name {
                ProgressView()
            } else {
                Button("Get") {
                    Task { await installer.install(skin) }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }

            if let error = installer.errors[skin.name] {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                    .help(error)
            }
        }
        .swipeActions(edge: .trailing) {
            if installer.isInstalled(skin) {
                Button(role: .destructive) {
                    installer.uninstall(skin)
                } label: {
                    Label("Remove", systemImage: "trash")
                }
            }
        }
    }
}
