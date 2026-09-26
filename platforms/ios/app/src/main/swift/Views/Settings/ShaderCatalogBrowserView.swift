// ShaderCatalogBrowserView.swift — browse the published RetroArch presets and install one
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private struct ShaderCatalogGroup: Identifiable {
    let id: String
    let entries: [ShaderCatalogEntry]
}

struct ShaderCatalogBrowserView: View {
    let localized: @MainActor (String) -> String
    let onSelect: @MainActor (String) -> Void
    var controllerInput: MenuControllerInputRouter? = nil

    @Environment(\.dismiss) private var dismiss
    @StateObject private var catalog = ShaderCatalog()
    @StateObject private var installer = ShaderCatalogInstaller()
    @State private var searchText = ""
    @State private var expanded: Set<String> = []
    @State private var installTasks: [String: Task<Void, Never>] = [:]
    @State private var downloadAllTask: Task<Void, Never>?
    @State private var downloadAllProgress: (completed: Int, total: Int)?
    @State private var copiedLinkID: String?

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
                    Text(localized(error))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button(localized("Retry")) { Task { await catalog.load(force: true) } }
                        .controllerAccessibilityActionTarget(
                            id: "shader-catalog.retry",
                            label: localized("Retry")
                        ) {
                            Task { await catalog.load(force: true) }
                        }
                }
            }

            if !catalog.entries.isEmpty && groups.isEmpty {
                Text(localized("Nothing here matches that search."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if !catalog.entries.isEmpty {
                Section {
                    Button {
                        toggleDownloadAll()
                    } label: {
                        Label(
                            downloadAllProgress == nil
                                ? localized("Download All")
                                : localized("Stop Downloading"),
                            systemImage: downloadAllProgress == nil
                                ? "arrow.down.circle.fill"
                                : "stop.circle.fill"
                        )
                    }
                    .controllerAccessibilityActionTarget(
                        id: "shader-catalog.download-all",
                        label: downloadAllProgress == nil
                            ? localized("Download All")
                            : localized("Stop Downloading")
                    ) {
                        toggleDownloadAll()
                    }

                    if let progress = downloadAllProgress {
                        ProgressView(
                            value: Double(progress.completed),
                            total: Double(max(progress.total, 1))
                        ) {
                            Text(
                                String(
                                    format: localized("Downloading %@ of %@"),
                                    "\(progress.completed)",
                                    "\(progress.total)"
                                )
                            )
                        }
                    }
                }
            }

            Section {
                ForEach(groups) { group in
                    DisclosureGroup(isExpanded: isExpanded(group.id)) {
                        ForEach(group.entries) { entry in
                            row(entry)
                        }
                    } label: {
                        LabeledContent(group.id, value: "\(group.entries.count)")
                            .controllerAccessibilityActionTarget(
                                label: group.id
                            ) {
                                toggleGroup(group.id)
                            }
                    }
                    .controllerAccessibilityTargetID(groupTargetID(group))
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
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "shader-catalog-browser",
            priority: 720,
            onBack: {
                dismiss()
                return true
            },
            usesNativeFocusEngine: false,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            focusTopAlignmentMargin: 20,
            focusBottomAlignmentMargin: 20,
            preferredInitialFocusLabel: controllerTargetOrder.first,
            declaredTargetOrder: controllerTargetOrder
        )
        .task { await catalog.load() }
        .refreshable { await catalog.load(force: true) }
        .onDisappear {
            catalog.cancelPresentationWork()
            downloadAllTask?.cancel()
            downloadAllTask = nil
            for task in installTasks.values {
                task.cancel()
            }
            installTasks.removeAll(keepingCapacity: false)
        }
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
                    Text(localized(failure)).font(.caption2).foregroundStyle(.orange)
                }
            }

            Spacer()

            if let sourceURL = ShaderCatalog.assetURL(entry.zip.path) {
                copyLinkButton(for: entry, url: sourceURL)
            }

            if installer.installing.contains(entry.id) {
                ProgressView()
            } else if let token = installer.presetToken(for: entry) {
                Button(localized("Use")) {
                    onSelect(token)
                    dismiss()
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .controllerAccessibilityActionTarget(
                    id: "shader-catalog.use." + entry.id,
                    label: String(format: localized("Use %@"), entry.name)
                ) {
                    onSelect(token)
                    dismiss()
                }
            } else if installer.installed[entry.id] != nil {
                Text(localized("Installed"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Button(localized("Get")) {
                    install(entry)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .controllerAccessibilityActionTarget(
                    id: targetID(entry),
                    label: entry.name
                ) {
                    install(entry)
                }
            }
        }
    }
    private var controllerTargetOrder: [String] {
        let retry = catalog.lastError == nil ? [] : ["shader-catalog.retry"]
        let downloadAll = catalog.entries.isEmpty
            ? [] : ["shader-catalog.download-all"]
        return retry + downloadAll + groups.flatMap { group in
            var ids = [groupTargetID(group)]
            guard groupIsExpanded(group.id) else { return ids }
            for entry in group.entries {
                if ShaderCatalog.assetURL(entry.zip.path) != nil {
                    ids.append(linkTargetID(entry))
                }
                if !installer.installing.contains(entry.id) {
                    if installer.presetToken(for: entry) != nil {
                        ids.append("shader-catalog.use." + entry.id)
                    } else if installer.installed[entry.id] == nil {
                        ids.append(targetID(entry))
                    }
                }
            }
            return ids
        }
    }

    private func groupTargetID(_ group: ShaderCatalogGroup) -> String {
        "shader-catalog.group." + group.id
    }

    private func groupIsExpanded(_ category: String) -> Bool {
        !searchText.isEmpty || expanded.contains(category)
    }

    private func toggleGroup(_ category: String) {
        if expanded.contains(category) {
            expanded.remove(category)
        } else {
            expanded.insert(category)
        }
    }

    private func targetID(_ entry: ShaderCatalogEntry) -> String {
        "shader-catalog.get." + entry.id
    }

    private func linkTargetID(_ entry: ShaderCatalogEntry) -> String {
        "shader-catalog.copy-link." + entry.id
    }

    private func copyLinkButton(
        for entry: ShaderCatalogEntry,
        url: URL
    ) -> some View {
        Button {
            copyLink(url, for: entry)
        } label: {
            Image(systemName: copiedLinkID == entry.id ? "checkmark" : "link")
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
        .accessibilityLabel(localized("Copy Shader Link"))
        .controllerAccessibilityActionTarget(
            id: linkTargetID(entry),
            label: localized("Copy Shader Link")
        ) {
            copyLink(url, for: entry)
        }
    }

    private func copyLink(_ url: URL, for entry: ShaderCatalogEntry) {
        UIPasteboard.general.url = url
        copiedLinkID = entry.id
    }

    private func install(_ entry: ShaderCatalogEntry) {
        installTasks[entry.id]?.cancel()
        installTasks[entry.id] = Task { @MainActor in
            await installer.install(entry, pin: catalog.pin)
            guard !Task.isCancelled else { return }
            installTasks[entry.id] = nil
        }
    }

    private func toggleDownloadAll() {
        if downloadAllTask != nil {
            downloadAllTask?.cancel()
            downloadAllTask = nil
            downloadAllProgress = nil
            return
        }

        let pending = catalog.entries.filter {
            installer.presetToken(for: $0) == nil
        }
        guard !pending.isEmpty else { return }
        downloadAllProgress = (0, pending.count)
        downloadAllTask = Task { @MainActor in
            for (index, entry) in pending.enumerated() {
                guard !Task.isCancelled else { break }
                await installer.install(entry, pin: catalog.pin)
                guard !Task.isCancelled else { break }
                downloadAllProgress = (index + 1, pending.count)
            }
            downloadAllTask = nil
            downloadAllProgress = nil
        }
    }
}
