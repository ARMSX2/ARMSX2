// ShaderPresetBrowserView.swift — one shader location at a time, across both preset roots
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// A constant id, so a host body re-running cannot rebuild the browser's search field under the keyboard.
struct ShaderPresetBrowserRequest: Identifiable {
    let id = "shader-preset-browser"
}

/// The host closes its sheet in onSelect, since an outer folder's dismiss is ignored under an inner one.
struct ShaderPresetBrowserView: View {
    let title: String
    let folder: ShaderPresetFolder?
    let selectedToken: String
    let localized: @MainActor (String) -> String
    let onSelect: @MainActor (String) -> Void

    @State private var listing = ShaderPresetListing.empty
    @State private var scanned = false
    @State private var searchText = ""
    @State private var pendingDelete: (name: String, url: URL)?

    var body: some View {
        List {
            if !scanned {
                HStack { Spacer(); ProgressView(); Spacer() }
            }

            ForEach(folders) { child in
                NavigationLink {
                    ShaderPresetBrowserView(
                        title: child.name, folder: child, selectedToken: selectedToken,
                        localized: localized, onSelect: onSelect)
                } label: {
                    Label(child.name, systemImage: "folder")
                }
                .swipeActions(edge: .trailing) {
                    deleteAction(child.name, folder == nil ? ShaderPresetLibrary.deletableURL(for: child) : nil)
                }
            }

            ForEach(presets) { preset in
                Button {
                    onSelect(preset.token)
                } label: {
                    presetRow(preset)
                }
                .buttonStyle(.plain)
                .swipeActions(edge: .trailing) {
                    deleteAction(preset.name, ShaderPresetLibrary.deletableURL(for: preset))
                }
            }

            if scanned && folders.isEmpty && presets.isEmpty {
                Text(emptyMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .searchable(
            text: $searchText,
            placement: .navigationBarDrawer(displayMode: .always),
            prompt: localized("Search this folder")
        )
        .confirmationDialog(
            String(format: localized("Delete %@?"), pendingDelete?.name ?? ""),
            isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
            titleVisibility: .visible
        ) {
            Button(localized("Delete"), role: .destructive) { deletePending() }
            Button(localized("Cancel"), role: .cancel) { pendingDelete = nil }
        } message: {
            if pendingDelete?.url.hasDirectoryPath == true {
                Text(localized("Presets saved from it stop working."))
            }
        }
        // Rescanned on every appearance: packs land in Documents through the Files app
        // while ARMSX2 is running, so a tree held across presentations goes stale.
        .onAppear { Task { await rescan() } }
    }

    private var folders: [ShaderPresetFolder] {
        guard !searchText.isEmpty else { return listing.folders }
        return listing.folders.filter { $0.name.localizedStandardContains(searchText) }
    }

    private var presets: [ShaderPresetFile] {
        guard !searchText.isEmpty else { return listing.presets }
        return listing.presets.filter { $0.name.localizedStandardContains(searchText) }
    }

    private var emptyMessage: String {
        if !searchText.isEmpty {
            return localized("Nothing here matches that search.")
        }
        return localized("No presets here yet. Shader packs belong in Documents/shaders, or arrive through Install Shader Pack.")
    }

    @ViewBuilder
    private func presetRow(_ preset: ShaderPresetFile) -> some View {
        HStack {
            Text(preset.name)
            Spacer()
            if preset.token == selectedToken {
                Image(systemName: "checkmark").foregroundStyle(.tint)
            }
        }
        .contentShape(Rectangle())
    }

    @ViewBuilder
    private func deleteAction(_ name: String, _ url: URL?) -> some View {
        if let url {
            Button(role: .destructive) {
                pendingDelete = (name, url)
            } label: {
                Label(localized("Delete"), systemImage: "trash")
            }
        }
    }

    private func deletePending() {
        guard let url = pendingDelete?.url else { return }
        pendingDelete = nil
        Task {
            _ = await Task.detached { try? FileManager.default.removeItem(at: url) }.value
            await rescan()
        }
    }

    private func rescan() async {
        let target = folder
        listing = await Task.detached(priority: .userInitiated) { () -> ShaderPresetListing in
            let library = ShaderPresetLibrary()
            return target.map { library.listing(at: $0) } ?? library.scan()
        }.value
        scanned = true
    }
}
