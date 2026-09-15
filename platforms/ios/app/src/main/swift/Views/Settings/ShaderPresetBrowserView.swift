// ShaderPresetBrowserView.swift — one shader location at a time, across both preset roots
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderPresetBrowserView: View {
    let title: String
    let folder: ShaderPresetFolder?
    let selectedToken: String
    let localized: @MainActor (String) -> String
    let onSelect: @MainActor (String) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var listing = ShaderPresetListing.empty
    @State private var scanned = false
    @State private var searchText = ""

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
            }

            ForEach(presets) { preset in
                Button {
                    onSelect(preset.token)
                    dismiss()
                } label: {
                    presetRow(preset)
                }
                .buttonStyle(.plain)
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

    private func rescan() async {
        let target = folder
        listing = await Task.detached(priority: .userInitiated) { () -> ShaderPresetListing in
            let library = ShaderPresetLibrary()
            return target.map { library.listing(at: $0) } ?? library.scan()
        }.value
        scanned = true
    }
}
