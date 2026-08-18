// ShaderCatalogBrowserView.swift — browse the published RetroArch presets and install one
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ShaderCatalogGroup: Identifiable {
    let id: String
    let entries: [ShaderCatalogEntry]
}

struct ShaderCatalogBrowserView: View {
    let localized: @MainActor (String) -> String

    @StateObject private var catalog = ShaderCatalog()
    @StateObject private var installer = ShaderCatalogInstaller()
    @State private var searchText = ""

    var body: some View {
        List {
            if catalog.isStale {
                staleBanner
            }

            if catalog.isLoading && catalog.entries.isEmpty {
                HStack { Spacer(); ProgressView(); Spacer() }
            }

            if let error = catalog.lastError {
                VStack(alignment: .leading, spacing: 8) {
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button(localized("Retry")) { Task { await catalog.load(force: true) } }
                }
            }

            // Nothing cached and nothing reachable is a different sentence from a search that
            // matched none of what is on screen.
            if catalog.entries.isEmpty && !catalog.isLoading && catalog.lastError == nil {
                Text(localized("No shader catalogue has been downloaded yet."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else if !catalog.entries.isEmpty && groups.isEmpty {
                Text(localized("Nothing here matches that search."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            ForEach(groups) { group in
                Section(group.id) {
                    ForEach(group.entries) { entry in
                        row(entry)
                    }
                }
            }
        }
        .searchable(
            text: $searchText,
            placement: .navigationBarDrawer(displayMode: .always),
            prompt: localized("Search shaders")
        )
        .navigationTitle(localized("Shader Catalogue"))
        .navigationBarTitleDisplayMode(.inline)
        .task { await catalog.load() }
        .refreshable { await catalog.load(force: true) }
    }

    private var staleBanner: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(localized("Showing the last catalogue this device saw."))
            if let updated = catalog.lastUpdated {
                Text(String(format: localized("Last updated %@"),
                            updated.formatted(.relative(presentation: .named))))
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
    }

    private var groups: [ShaderCatalogGroup] {
        let matches = searchText.isEmpty ? catalog.entries : catalog.entries.filter { entry in
            entry.name.localizedStandardContains(searchText)
                || entry.category.localizedStandardContains(searchText)
        }
        return Dictionary(grouping: matches, by: \.category)
            .map { ShaderCatalogGroup(id: $0.key, entries: $0.value) }
            .sorted { $0.id.localizedStandardCompare($1.id) == .orderedAscending }
    }

    @ViewBuilder
    private func row(_ entry: ShaderCatalogEntry) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.name).font(.body)
                Text(subtitle(for: entry)).font(.caption).foregroundStyle(.secondary)
                if let failure = installer.errors[entry.id] {
                    Text(failure).font(.caption2).foregroundStyle(.orange)
                }
            }

            Spacer()

            if installer.installing.contains(entry.id) {
                ProgressView()
            } else if installer.installed.contains(entry.id) {
                Text(localized("Installed"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Button(localized("Get")) {
                    Task { await installer.install(entry, pin: catalog.pin) }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }
        }
    }

    private func subtitle(for entry: ShaderCatalogEntry) -> String {
        let passes = entry.passes == 1
            ? localized("1 pass")
            : String(format: localized("%@ passes"), "\(entry.passes)")
        let size = Int64(entry.zip.bytes).formatted(.byteCount(style: .file))
        return "\(passes) · \(size)"
    }
}
