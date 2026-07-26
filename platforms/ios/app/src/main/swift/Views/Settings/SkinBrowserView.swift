// SkinBrowserView.swift — browse and install community controller skins
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SkinBrowserView: View {
    @StateObject private var catalog = SkinCatalog()
    @StateObject private var installer = SkinInstaller()
    @State private var searchText = ""
    @State private var errorAlert: String?
    @State private var previewSkin: CatalogSkin?

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
        .alert("Download Failed", isPresented: Binding(
            get: { errorAlert != nil },
            set: { if !$0 { errorAlert = nil } }
        )) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(errorAlert ?? "")
        }
        .sheet(item: $previewSkin) { skin in
            SkinPreviewSheet(skin: skin)
        }
    }

    private var filteredSkins: [CatalogSkin] {
        guard !searchText.isEmpty else { return catalog.skins }
        return catalog.skins.filter { $0.name.localizedCaseInsensitiveContains(searchText) }
    }

    @ViewBuilder
    private func skinRow(_ skin: CatalogSkin) -> some View {
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
            } else if installer.installing.contains(skin.file) {
                ProgressView()
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
