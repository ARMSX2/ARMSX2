// SkinBrowserView.swift — browse and install community controller skins
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SkinBrowserView: View {
    /// Ready means the skin ships a layout for iOS. The rest still install, they
    /// just leave you to place the buttons. The raw values are the catalog keys.
    private enum Filter: String, Hashable, CaseIterable {
        case all = "All"
        case installed = "Installed"
        case ready = "Ready"
    }

    @State private var settings = SettingsStore.shared
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
    @State private var showsSearchKeyboard = false
    @Environment(\.menuControllerInputRouter) private var controllerInput

    var body: some View {
        List {
            if controllerInput?.isControllerNavigationEnabled == true {
                Button {
                    showsSearchKeyboard = true
                } label: {
                    Label(
                        searchText.isEmpty ? settings.localized("Search skins") : searchText,
                        systemImage: "magnifyingglass"
                    )
                }
                .controllerAccessibilityActionTarget(
                    id: "skin.search",
                    label: settings.localized("Search skins")
                ) {
                    showsSearchKeyboard = true
                }
            }

            lastUpdatedRow

            if catalog.isLoading {
                HStack { Spacer(); ProgressView(); Spacer() }
            }

            if let error = catalog.lastError {
                VStack(alignment: .leading, spacing: 8) {
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button(settings.localized("Retry")) { Task { await catalog.fetch(force: true) } }
                }
            }

            if catalog.skins.isEmpty && !catalog.isLoading && catalog.lastError == nil {
                Text(settings.localized("No skins are published yet."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if !catalog.skins.isEmpty {
                Picker("Show", selection: $filter) {
                    ForEach(Filter.allCases, id: \.self) { option in
                        Text(settings.localized(option.rawValue)).tag(option)
                    }
                }
                .controllerAccessibilityOptionsPickerTarget(
                    id: "skin.filter",
                    label: settings.localized("Skins"),
                    selection: $filter,
                    options: Filter.allCases.map { (id: $0, title: settings.localized($0.rawValue)) }
                )
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
        // Pinned to the drawer, since iOS 26 otherwise puts it at the bottom under our tab bar.
        // Always for touch, so it needs no pull down; automatic in controller mode, where the
        // Search skins row replaces it and a pad can't pull it down.
        .searchable(
            text: $searchText,
            placement: .navigationBarDrawer(
                displayMode: controllerInput?.isControllerNavigationEnabled == true
                    ? .automatic : .always
            ),
            prompt: Text(settings.localized("Search skins"))
        )
        .navigationTitle(settings.localized("Skins"))
        .navigationBarTitleDisplayMode(.inline)
        .controllerAccessibilityTargetOrder(
            controllerColumns.order,
            links: controllerColumns.links
        )
        .task { await catalog.fetch() }
        .refreshable { await catalog.fetch(force: true) }
        .controllerPrompt(
            settings.localized("Skin Install"),
            isPresented: isDetailAlertPresented,
            message: detailAlert ?? "",
            actions: [.ok]
        )
        .controllerPrompt(
            settings.localized("Remove Skin?"),
            isPresented: isRemoveAlertPresented,
            message: settings.localized("This deletes the installed skin. Linked layout presets are kept."),
            actions: [
                .cancel,
                .init(
                    title: String(format: settings.localized("Remove %@"), skinPendingRemoval?.name ?? ""),
                    isDestructive: true
                ) {
                    if let skin = skinPendingRemoval { installer.uninstall(skin) }
                },
            ]
        )
        .sheet(item: $previewSkin) { skin in
            SkinPreviewSheet(skin: skin, controllerInput: controllerInput)
        }
        .fullScreenCover(isPresented: $showsSearchKeyboard) {
            OrbitKeysKeyboardView(
                title: settings.localized("Search skins"),
                initialText: searchText,
                startsInNormalKeyboard: true,
                onCommit: { text in
                    searchText = text
                    showsSearchKeyboard = false
                },
                onCancel: { showsSearchKeyboard = false }
            )
            .presentationBackground(.clear)
            .appStatusBarHidden()
        }
    }

    /// Out of body, and in two steps. A format string wrapped around a
    /// relative-date style wrapped around a locale built from a setting is one
    /// expression, and the type checker charges the time it spends on it to
    /// whatever it is nested in - which was the whole of body.
    @ViewBuilder private var lastUpdatedRow: some View {
        if let updated = catalog.lastUpdated {
            Text(lastUpdatedText(updated))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func lastUpdatedText(_ updated: Date) -> String {
        // appLanguage, and the same shape RootView gives the environment locale:
        // .system means whatever the device is set to, and bcp47Code answers
        // "en" for it, which would have pinned this one line to English.
        let locale = settings.appLanguage == .system
            ? Locale.autoupdatingCurrent
            : Locale(identifier: settings.appLanguage.bcp47Code)
        let relative: String = updated.formatted(.relative(presentation: .named).locale(locale))
        return String(format: settings.localized("Last updated %@"), relative)
    }

    private var isDetailAlertPresented: Binding<Bool> {
        Binding(
            get: { detailAlert != nil },
            set: { if !$0 { detailAlert = nil } }
        )
    }

    private var isRemoveAlertPresented: Binding<Bool> {
        Binding(
            get: { skinPendingRemoval != nil },
            set: { if !$0 { skinPendingRemoval = nil } }
        )
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
            case .all: return settings.localized("No skins match that search.")
            case .installed: return settings.localized("You haven't installed any skins yet.")
            case .ready: return settings.localized("No skins ship a recommended layout yet.")
            }
        }
        return settings.localized("No skins match that search.")
    }

    /// Previews and actions are two columns, so Down stays in the one it started in
    /// and Left or Right crosses over. Only targets that mount are listed.
    private var controllerColumns: (order: [String], links: [ControllerAccessibilityDirectionalLink]) {
        let installed = installedFiles
        var previews: [String] = []
        var actions: [String] = []
        for skin in filteredSkins {
            if SkinCatalog.previewURL(for: skin) != nil {
                previews.append("skin.preview.\(skin.file)")
            }
            if !installer.installing.contains(skin.file), !installed.contains(skin.file) {
                actions.append("skin.get.\(skin.file)")
            }
            if installer.errors[skin.file] != nil || installer.notices[skin.file] != nil {
                actions.append("skin.detail.\(skin.file)")
            }
        }
        let boundary = ControllerAccessibilityDirectionalLink.navigationBoundary
        var links: [ControllerAccessibilityDirectionalLink] = []
        if let last = previews.last {
            links.append(.init(fromLabel: last, direction: .down, toLabel: boundary))
        }
        if let first = actions.first {
            links.append(.init(fromLabel: first, direction: .up, toLabel: boundary))
        }
        var header: [String] = []
        if controllerInput?.isControllerNavigationEnabled == true {
            header.append("skin.search")
        }
        if !catalog.skins.isEmpty {
            header.append("skin.filter")
        }
        return (header + previews + actions, links)
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
                .accessibilityLabel(
                    String(format: settings.localized("Preview %@"), skin.name)
                )
                .controllerAccessibilityActionTarget(
                    id: "skin.preview.\(skin.file)",
                    label: String(format: settings.localized("Preview %@"), skin.name)
                ) {
                    previewSkin = skin
                }
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(skin.name).font(.body)
                if let subtitle = subtitle(for: skin) {
                    Text(subtitle).font(.caption).foregroundStyle(.secondary)
                }
                if !skin.isIOSReady {
                    Text(settings.localized("No recommended layout"))
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
                        Label(settings.localized("Reinstall"), systemImage: "arrow.clockwise")
                    }
                    Button(role: .destructive) {
                        skinPendingRemoval = skin
                    } label: {
                        Label(settings.localized("Remove"), systemImage: "trash")
                    }
                } label: {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                }
                .accessibilityLabel(String(format: settings.localized("%@ is installed. Reinstall or remove it."), skin.name))
            } else {
                Button(settings.localized("Get")) {
                    Task { await installer.install(skin) }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .controllerAccessibilityActionTarget(id: "skin.get.\(skin.file)", label: "Get \(skin.name)") {
                    Task { await installer.install(skin) }
                }
            }

            if let error = installer.errors[skin.file] {
                Button {
                    detailAlert = error
                } label: {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(
                    String(
                        format: settings.localized("Show the error from %@"),
                        skin.name
                    )
                )
                .controllerAccessibilityActionTarget(
                    id: "skin.detail.\(skin.file)",
                    label: String(
                        format: settings.localized("Show the error from %@"),
                        skin.name
                    )
                ) {
                    detailAlert = error
                }
            } else if let notice = installer.notices[skin.file] {
                Button {
                    detailAlert = notice
                } label: {
                    Image(systemName: "exclamationmark.circle")
                        .foregroundStyle(.yellow)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(
                    String(
                        format: settings.localized(
                            "Show what %@ reported during install"
                        ),
                        skin.name
                    )
                )
                .controllerAccessibilityActionTarget(
                    id: "skin.detail.\(skin.file)",
                    label: String(
                        format: settings.localized(
                            "Show what %@ reported during install"
                        ),
                        skin.name
                    )
                ) {
                    detailAlert = notice
                }
            }
        }
        .swipeActions(edge: .trailing) {
            if isInstalled {
                Button(role: .destructive) {
                    skinPendingRemoval = skin
                } label: {
                    Label(settings.localized("Remove"), systemImage: "trash")
                }
            }
        }
    }
}

private struct SkinPreviewSheet: View {
    let skin: CatalogSkin
    let controllerInput: MenuControllerInputRouter?
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
                    Button(SettingsStore.shared.localized("Done")) { dismiss() }
                }
            }
        }
        // Without a scope of its own, Back reached the Settings page underneath and
        // popped the Skins page while the preview stayed up.
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "skin-browser.preview",
            priority: 720,
            onBack: {
                dismiss()
                return true
            },
            usesExplicitTargetGeometryOnly: true
        )
    }
}
