// ShaderPresetBrowserView.swift — one shader location at a time, across both preset roots
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// A constant id, so a re-running host body keeps the browser's search field and keyboard.
struct ShaderPresetBrowserRequest: Identifiable {
    let id = "shader-preset-browser"
}

/// The host closes its sheet in onSelect, since an outer folder's dismiss is ignored under an inner one.
private struct ShaderDeletionFailure: Identifiable {
    let id = UUID()
    let message: String
}
struct ShaderPresetBrowserView: View {
    let title: String
    let folder: ShaderPresetFolder?
    let selectedToken: String
    let localized: @MainActor (String) -> String
    let onSelect: @MainActor (String) -> Void
    var controllerInput: MenuControllerInputRouter? = nil
    var onClose: (@MainActor () -> Void)? = nil

    @Environment(\.dismiss) private var dismiss
    @State private var listing = ShaderPresetListing.empty
    @State private var scanned = false
    @State private var searchText = ""
    @State private var pendingDelete: (name: String, url: URL)?
    @State private var selectedFolder: ShaderPresetFolder?
    @State private var showsDownloadManager = false

    var body: some View {
        List {
            if !scanned {
                HStack { Spacer(); ProgressView(); Spacer() }
            }

            if folder == nil {
                Button {
                    showsDownloadManager = true
                } label: {
                    Label(localized("Manage Downloaded Shaders"), systemImage: "trash")
                }
                .buttonStyle(.plain)
                .controllerAccessibilityActionTarget(
                    id: "shader-browser.manage-downloads",
                    label: localized("Manage Downloaded Shaders")
                ) {
                    showsDownloadManager = true
                }
            }

            ForEach(folders) { child in
                HStack(spacing: 8) {
                    Button {
                        selectedFolder = child
                    } label: {
                        Label(child.name, systemImage: "folder")
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: folderTargetID(child),
                        label: child.name
                    ) {
                        selectedFolder = child
                    }

                    if controllerNavigationEnabled,
                       let url = folder == nil
                            ? ShaderPresetLibrary.deletableURL(for: child)
                            : nil {
                        controllerDeleteButton(name: child.name, url: url)
                    }
                }
                .swipeActions(edge: .trailing) {
                    deleteAction(child.name, folder == nil ? ShaderPresetLibrary.deletableURL(for: child) : nil)
                }
            }

            ForEach(presets) { preset in
                HStack(spacing: 8) {
                    Button {
                        select(preset)
                    } label: {
                        presetRow(preset)
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: presetTargetID(preset),
                        label: preset.name
                    ) {
                        select(preset)
                    }

                    if controllerNavigationEnabled,
                       let url = ShaderPresetLibrary.deletableURL(for: preset) {
                        controllerDeleteButton(name: preset.name, url: url)
                    }
                }
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
        .navigationDestination(item: $selectedFolder) { child in
            ShaderPresetBrowserView(
                title: child.name,
                folder: child,
                selectedToken: selectedToken,
                localized: localized,
                onSelect: onSelect,
                controllerInput: controllerInput,
                onClose: onClose
            )
        }
        .sheet(isPresented: $showsDownloadManager, onDismiss: {
            Task { await rescan() }
        }) {
            NavigationStack {
                ShaderDownloadManagerView(
                    selectedToken: selectedToken,
                    localized: localized,
                    controllerInput: controllerInput,
                    onDeletedActivePreset: { onSelect("") }
                )
            }
        }
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
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "shader-preset-browser." + (folder?.url.path ?? "root"),
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
        // Rescanned on every appearance: packs land in Documents through the Files app
        // while ARMSX2 is running, so a tree held across presentations goes stale.
        .task(id: folder?.url) { await rescan() }
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
        return localized("Nothing saved yet. Presets you save under Parameters show up here.")
    }

    private var controllerTargetOrder: [String] {
        (folder == nil ? ["shader-browser.manage-downloads"] : [])
            + folders.flatMap { child in
                var ids = [folderTargetID(child)]
                if controllerNavigationEnabled,
                   folder == nil,
                   ShaderPresetLibrary.deletableURL(for: child) != nil {
                    ids.append(deleteTargetID(child.url))
                }
                return ids
            }
            + presets.flatMap { preset in
                var ids = [presetTargetID(preset)]
                if controllerNavigationEnabled,
                   ShaderPresetLibrary.deletableURL(for: preset) != nil {
                    ids.append(deleteTargetID(preset.url))
                }
                return ids
            }
    }

    private var controllerNavigationEnabled: Bool {
        controllerInput?.hasConnectedController == true
            && controllerInput?.isControllerNavigationEnabled == true
    }

    private func folderTargetID(_ folder: ShaderPresetFolder) -> String {
        "shader-browser.folder." + folder.url.absoluteString
    }

    private func presetTargetID(_ preset: ShaderPresetFile) -> String {
        "shader-browser.preset." + preset.token
    }

    private func deleteTargetID(_ url: URL) -> String {
        "shader-browser.delete." + url.absoluteString
    }

    private func select(_ preset: ShaderPresetFile) {
        onSelect(preset.token)
        if let onClose {
            onClose()
        } else {
            dismiss()
        }
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

    private func controllerDeleteButton(name: String, url: URL) -> some View {
        Button {
            pendingDelete = (name, url)
        } label: {
            Image(systemName: "trash")
                .foregroundStyle(.red)
                .frame(width: 36, height: 36)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .controllerAccessibilityActionTarget(
            id: deleteTargetID(url),
            label: String(format: localized("Delete %@"), name)
        ) {
            pendingDelete = (name, url)
        }
    }

    @ViewBuilder
    private func deleteAction(_ name: String, _ url: URL?) -> some View {
        if let url {
            Button(role: .destructive) {
                pendingDelete = (name, url)
            } label: {
                Label(localized("Delete"), systemImage: "trash")
            }
            .foregroundStyle(.red)
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
            target.map { ShaderPresetLibrary.listing(at: $0) } ?? ShaderPresetLibrary.scan()
        }.value
        scanned = true
    }
}

/// Selective removal is kept in a separate presentation so normal preset browsing remains a
/// one-action list. Bundled presets and locally authored My Presets never enter this model.
private struct ShaderDownloadManagerView: View {
    let selectedToken: String
    let localized: @MainActor (String) -> String
    var controllerInput: MenuControllerInputRouter? = nil
    let onDeletedActivePreset: @MainActor () -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var downloads: [ShaderManagedDownload] = []
    @State private var selected: Set<URL> = []
    @State private var isLoading = true
    @State private var isDeleting = false
    @State private var showsDeleteConfirmation = false
    @State private var deletionFailure: ShaderDeletionFailure?
    @State private var completionText: String?

    var body: some View {
        List {
            if isLoading {
                HStack { Spacer(); ProgressView(); Spacer() }
            } else if downloads.isEmpty {
                ContentUnavailableView(
                    localized("No Downloaded Shaders"),
                    systemImage: "square.stack.3d.up.slash",
                    description: Text(localized(
                        "Downloaded and manually installed shader packs will appear here."
                    ))
                )
            } else {
                Section {
                    ForEach(downloads) { download in
                        Button {
                            toggleSelection(download)
                        } label: {
                            HStack(spacing: 12) {
                                Image(systemName: selected.contains(download.url)
                                      ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(selected.contains(download.url)
                                                     ? Color.accentColor : Color.secondary)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(download.name)
                                    Text(localized(download.kind == .pack
                                                   ? "Shader Pack" : "Shader Preset"))
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Image(systemName: download.kind == .pack
                                      ? "folder" : "doc.text")
                                    .foregroundStyle(.secondary)
                            }
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .controllerAccessibilityActionTarget(
                            id: targetID(download),
                            label: download.name
                        ) {
                            toggleSelection(download)
                        }
                    }
                } header: {
                    Text(localized("Downloaded Shaders"))
                } footer: {
                    Text(localized(
                        "Bundled shaders and presets saved in My Presets are protected."
                    ))
                }

                Section {
                    Button(selectionActionTitle) {
                        toggleAll()
                    }
                    .controllerAccessibilityActionTarget(
                        id: "shader-downloads.select-all",
                        label: selectionActionTitle
                    ) {
                        toggleAll()
                    }

                    Button(role: .destructive) {
                        showsDeleteConfirmation = true
                    } label: {
                        Label(deleteActionTitle, systemImage: "trash")
                    }
                    .controllerAccessibilityActionTarget(
                        id: "shader-downloads.delete",
                        label: deleteActionTitle
                    ) {
                        showsDeleteConfirmation = true
                    }
                    .disabled(selected.isEmpty || isDeleting)
                    .foregroundStyle(.red)

                    if let completionText {
                        Label(completionText, systemImage: "checkmark.circle.fill")
                            .font(.footnote.weight(.semibold))
                            .foregroundStyle(.green)
                    }
                }
            }
        }
        .navigationTitle(localized("Manage Downloaded Shaders"))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button(localized("Done")) { dismiss() }
            }
        }
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "shader-download-manager",
            priority: 760,
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
        .task { await reload() }
        .confirmationDialog(
            localized("Delete Selected Shaders?"),
            isPresented: $showsDeleteConfirmation,
            titleVisibility: .visible
        ) {
            Button(deleteActionTitle, role: .destructive) { deleteSelected() }
            Button(localized("Cancel"), role: .cancel) {}
        } message: {
            Text(localized(
                "Deleted shader packs cannot be restored unless they are downloaded or imported again."
            ))
        }
        .alert(item: $deletionFailure) { failure in
            Alert(
                title: Text(localized("Shaders Could Not Be Deleted")),
                message: Text(failure.message),
                dismissButton: .default(Text(localized("OK")))
            )
        }
    }

    private var controllerTargetOrder: [String] {
        guard !downloads.isEmpty else { return [] }
        return downloads.map(targetID)
            + ["shader-downloads.select-all", "shader-downloads.delete"]
    }

    private var selectionActionTitle: String {
        selected.count == downloads.count
            ? localized("Deselect All") : localized("Select All")
    }

    private var deleteActionTitle: String {
        selected.isEmpty
            ? localized("Delete Selected")
            : String(format: localized("Delete Selected (%@)"), "\(selected.count)")
    }

    private func targetID(_ download: ShaderManagedDownload) -> String {
        "shader-downloads.item." + download.url.absoluteString
    }

    private func toggleSelection(_ download: ShaderManagedDownload) {
        if selected.contains(download.url) {
            selected.remove(download.url)
        } else {
            selected.insert(download.url)
        }
    }

    private func toggleAll() {
        if selected.count == downloads.count {
            selected.removeAll(keepingCapacity: true)
        } else {
            selected = Set(downloads.map(\.url))
        }
    }

    private func reload() async {
        let refreshed = await Task.detached(priority: .userInitiated) {
            ShaderPresetLibrary.managedDownloads()
        }.value
        guard !Task.isCancelled else { return }
        downloads = refreshed
        selected.formIntersection(Set(refreshed.map(\.url)))
        isLoading = false
    }

    private func deleteSelected() {
        let removals = downloads.filter { selected.contains($0.url) }
        guard !removals.isEmpty else { return }
        let activeURL = ShaderPresetLibrary.resolve(selectedToken)
        let removesActivePreset = activeURL.map { active in
            removals.contains { $0.contains(active) }
        } ?? false
        isDeleting = true
        completionText = nil
        Task { @MainActor in
            do {
                try await Task.detached(priority: .userInitiated) {
                    try ShaderPresetLibrary.deleteManagedDownloads(removals)
                }.value
                if removesActivePreset {
                    onDeletedActivePreset()
                }
                selected.removeAll(keepingCapacity: true)
                await reload()
                completionText = removals.count == 1
                    ? localized("Shader Deleted")
                    : String(format: localized("%@ Shaders Deleted"), "\(removals.count)")
                MenuAudioPackManager.shared.playEvent(.return)
                controllerInput?.playTouchHaptics(.activate)
            } catch {
                deletionFailure = ShaderDeletionFailure(message: error.localizedDescription)
            }
            isDeleting = false
        }
    }
}
