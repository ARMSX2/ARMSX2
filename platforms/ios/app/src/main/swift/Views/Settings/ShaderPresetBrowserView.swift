// ShaderPresetBrowserView.swift — one shader location at a time, across both preset roots
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ShaderBrowserContents: Sendable {
    let listing: ShaderPresetListing
    let passes: [String: Int]

    static let empty = ShaderBrowserContents(listing: .empty, passes: [:])
}

struct ShaderPresetBrowserView: View {
    let title: String
    let folder: ShaderPresetFolder?
    let selectedToken: String
    let localized: @MainActor (String) -> String
    let onSelect: @MainActor (String) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var contents = ShaderBrowserContents.empty
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
        guard !searchText.isEmpty else { return contents.listing.folders }
        return contents.listing.folders.filter { $0.name.localizedStandardContains(searchText) }
    }

    private var presets: [ShaderPresetFile] {
        guard !searchText.isEmpty else { return contents.listing.presets }
        return contents.listing.presets.filter { $0.name.localizedStandardContains(searchText) }
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
            VStack(alignment: .leading, spacing: 2) {
                Text(preset.name)
                // Reported and nothing else. A pass count does not predict what a preset
                // costs, so it never orders, groups, filters or badges a row.
                if let passes = contents.passes[preset.token] {
                    Text(passLabel(passes))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            if preset.token == selectedToken {
                Image(systemName: "checkmark").foregroundStyle(.tint)
            }
        }
        .contentShape(Rectangle())
    }

    private func passLabel(_ count: Int) -> String {
        count == 1 ? localized("1 pass") : "\(count) " + localized("passes")
    }

    private func rescan() async {
        let target = folder
        let scan = await Task.detached(priority: .userInitiated) { () -> ShaderBrowserContents in
            let library = ShaderPresetLibrary()
            let listing = target.map { library.listing(at: $0) } ?? library.scan()
            var passes: [String: Int] = [:]
            for preset in listing.presets {
                passes[preset.token] = library.passCount(for: preset.url)
            }
            return ShaderBrowserContents(listing: listing, passes: passes)
        }.value
        contents = scan
        scanned = true
    }
}
