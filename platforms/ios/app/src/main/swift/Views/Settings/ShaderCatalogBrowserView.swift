// ShaderCatalogBrowserView.swift — browse the published RetroArch presets and install one
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ShaderCatalogGroup: Identifiable {
    let id: String
    let entries: [ShaderCatalogEntry]
}

struct ShaderCatalogBrowserView: View {
    let localized: @MainActor (String) -> String
    let onSelect: @MainActor (String) -> Void

    @Environment(\.dismiss) private var dismiss
    @StateObject private var catalog = ShaderCatalog()
    @StateObject private var installer = ShaderCatalogInstaller()
    @State private var searchText = ""
    @State private var expanded: Set<String> = []

    var body: some View {
        List {
            if catalog.isStale, let updated = catalog.lastUpdated {
                Text(String(format: localized("Last updated %@"),
                            updated.formatted(.relative(presentation: .named))))
                    .font(.caption)
                    .foregroundStyle(.secondary)
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

            if !catalog.entries.isEmpty && groups.isEmpty {
                Text(localized("Nothing here matches that search."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                ForEach(groups) { group in
                    DisclosureGroup(isExpanded: isExpanded(group.id)) {
                        ForEach(group.entries) { entry in
                            row(entry)
                        }
                    } label: {
                        LabeledContent(group.id, value: "\(group.entries.count)")
                    }
                }
            }
        }
        .searchable(
            text: $searchText,
            placement: .navigationBarDrawer(displayMode: .always),
            prompt: localized("Search shaders")
        )
        .navigationTitle(localized("Download Shaders"))
        .navigationBarTitleDisplayMode(.inline)
        .task { await catalog.load() }
        .refreshable { await catalog.load(force: true) }
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

    private func isExpanded(_ category: String) -> Binding<Bool> {
        Binding(
            get: { !searchText.isEmpty || expanded.contains(category) },
            set: { open in
                if open { expanded.insert(category) } else { expanded.remove(category) }
            }
        )
    }

    @ViewBuilder
    private func row(_ entry: ShaderCatalogEntry) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.name).font(.body)
                Text(Int64(entry.zip.bytes).formatted(.byteCount(style: .file)))
                    .font(.caption).foregroundStyle(.secondary)
                if let failure = installer.errors[entry.id] {
                    Text(failure).font(.caption2).foregroundStyle(.orange)
                }
            }

            Spacer()

            if installer.installing.contains(entry.id) {
                ProgressView()
            } else if let token = installer.presetToken(for: entry) {
                Button(localized("Use")) {
                    onSelect(token)
                    dismiss()
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            } else if installer.installed[entry.id] != nil {
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
}
