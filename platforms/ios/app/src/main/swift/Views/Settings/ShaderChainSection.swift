// ShaderChainSection.swift — RetroArch shader chain rows, host-agnostic
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

private struct ShaderPackPickerSource: Identifiable {
    let isFolder: Bool

    var id: Bool { isFolder }
}

/// Persistence comes from the caller, so Settings and the pause card share these rows.
struct ShaderChainSection: View {
    @Binding var enabled: Bool
    @Binding var presetRef: String
    let localized: @MainActor (String) -> String

    @StateObject private var importer = ShaderPackImporter()
    @StateObject private var params = ShaderParams()
    @State private var pickerSource: ShaderPackPickerSource?
    @State private var browseRequest: ShaderPresetBrowserRequest?
    @State private var saveRequest: ShaderPresetSaveRequest?

    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        Group {
            chainSection
            if enabled, !presetRef.isEmpty, params.isLoading || !params.params.isEmpty {
                parameterSection
            }
        }
    }

    private var chainSection: some View {
        Section {
            Button {
                browseRequest = ShaderPresetBrowserRequest()
            } label: {
                HStack {
                    Text(localized("Preset"))
                    Spacer()
                    Text(presetName)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Image(systemName: "chevron.right")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.tertiary)
                }
            }
            .tint(.primary)
            .sheet(item: $browseRequest, onDismiss: {
                if !presetRef.isEmpty, ShaderPresetLibrary.resolve(presetRef) == nil { presetRef = "" }
            }) { _ in
                NavigationStack {
                    ShaderPresetBrowserView(
                        title: localized("Shader Presets"),
                        folder: nil,
                        selectedToken: presetRef,
                        localized: localized,
                        onSelect: { token in
                            select(token)
                            browseRequest = nil
                        }
                    )
                }
            }
            .task(id: presetRef) {
                await params.load(token: presetRef)
            }

            if let failure = params.loadFailure {
                problem(failure)
            }

            if !presetRef.isEmpty {
                Toggle(localized("Shaders"), isOn: $enabled)
            }

            NavigationLink {
                ShaderCatalogBrowserView(localized: localized, onSelect: select)
            } label: {
                Label(localized("Download Shaders"), systemImage: "arrow.down.circle")
            }

            Menu {
                Button {
                    pickerSource = ShaderPackPickerSource(isFolder: false)
                } label: {
                    Label(localized("From a Zip Archive"), systemImage: "doc.zipper")
                }
                Button {
                    pickerSource = ShaderPackPickerSource(isFolder: true)
                } label: {
                    Label(localized("From a Folder"), systemImage: "folder")
                }
                Button(action: getBasePack) {
                    basePackLabel
                }
            } label: {
                Label(localized("Install Shader Pack"), systemImage: "square.and.arrow.down")
            }
            .disabled(importer.isBusy)
            .sheet(item: $pickerSource) { source in
                picker(for: source)
            }

            if importer.installing.contains(ShaderPresetLibrary.basePackFolderName) {
                ProgressView(localized("Downloading RetroArch Slang Shaders..."))
            } else if importer.isBusy {
                ProgressView(localized("Installing..."))
            }

            if let installed = importer.installedName {
                Text(installed == ShaderPresetLibrary.basePackFolderName
                     ? localized("RetroArch Slang Shaders are installed.")
                     : String(format: localized("Installed %@. Pick a preset from it under Preset."), installed))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let failure = importer.installProblem, params.loadFailure == nil {
                problem(failure)
            }

            ForEach(importer.errors.sorted { $0.key < $1.key }, id: \.key) { entry in
                Text(localized(entry.value))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            if !presetRef.isEmpty {
                Button(role: .destructive) {
                    presetRef = ""
                } label: {
                    Text(localized("Clear Preset"))
                }
            }
        } footer: {
            Text(localized("Filters like CRT scanlines or LCD grids, drawn over the game. The first frame can stutter while one loads."))
        }
    }

    private var parameterSection: some View {
        Section {
            if params.isLoading {
                ProgressView(localized("Reading parameters..."))
            }

            ForEach(params.params) { param in
                parameterRow(param)
            }

            if !params.params.isEmpty {
                Button {
                    saveRequest = ShaderPresetSaveRequest(
                        token: presetRef, suggestedName: presetName)
                } label: {
                    Label(localized("Save as New Preset"), systemImage: "square.and.arrow.down")
                }
                // Item-bound: an isPresented sheet re-runs with the host's settings body and drops
                // the keyboard on each keystroke.
                .sheet(item: $saveRequest) { request in
                    ShaderPresetSaveSheet(request: request, localized: localized) { name in
                        Task { if let token = await params.save(as: name) { select(token) } }
                    }
                }
            }

            if params.hasOverrides {
                Button(role: .destructive) {
                    params.resetAll()
                } label: {
                    Text(localized("Reset All Parameters"))
                }
            }

            if let failure = params.errorText {
                Text(localized(failure))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        } header: {
            Text(localized("Parameters"))
        } footer: {
            Text(localized("Changes show right away. Save as New Preset keeps them in My Presets. A saved preset stops working if you delete the shader it came from."))
        }
    }

    @ViewBuilder
    private func parameterRow(_ param: ShaderParam) -> some View {
        if param.isAdjustable {
            // setValue clamps before storing, and NaN becomes the author's initial.
            NumberRow(
                param.label,
                value: Binding(
                    get: { params.value(for: param) },
                    set: { params.setValue($0, for: param) }
                ),
                in: param.minimum...param.maximum,
                format: NumberFormat.plain.decimals(param.decimals),
                step: Double(param.increment),
                detents: NumberRow.stops(in: Double(param.minimum)...Double(param.maximum),
                                         step: Double(param.increment)),
                accessory: NumberRowAccessory(
                    systemImage: "arrow.counterclockwise",
                    label: "Reset %@",
                    isVisible: params.overrides[param.name] != nil,
                    action: { params.reset(param) }
                ),
                settings: settings
            )
        } else {
            Text(param.label)
                .font(.footnote.weight(.semibold))
                .foregroundStyle(.secondary)
        }
    }

    private func select(_ token: String) {
        enabled = true
        presetRef = token
    }

    private func getBasePack() {
        Task {
            await importer.installBasePack()
            await params.load(token: presetRef)
        }
    }

    private var basePackLabel: some View {
        Label {
            Text(localized("RetroArch Slang Shaders") + " (" + ShaderPackImporter.basePackBytes.formatted(.byteCount(style: .file)) + ")")
        } icon: {
            Image(systemName: ShaderPresetLibrary.hasBasePack ? "checkmark.circle" : "arrow.down.circle")
        }
    }

    @ViewBuilder
    private func problem(_ failure: ShaderPresetFailure) -> some View {
        switch failure {
        case .needsBasePack:
            Text(localized("It needs RetroArch Slang Shaders."))
                .font(.caption)
                .foregroundStyle(.orange)
            Button(action: getBasePack) { basePackLabel }
                .disabled(importer.isBusy)
        case .missing(let file):
            Text(String(format: localized("It can't load because %@ is missing or broken."), file))
                .font(.caption)
                .foregroundStyle(.orange)
        case .needsReimport:
            Text(localized("It needs its shader pack installed again."))
                .font(.caption)
                .foregroundStyle(.orange)
        }
    }

    private var presetName: String {
        ShaderPresetLibrary.displayName(for: presetRef) ?? localized("None")
    }

    @ViewBuilder
    private func picker(for source: ShaderPackPickerSource) -> some View {
        if source.isFolder {
            ImportDocumentPicker(
                allowedContentTypes: [.folder],
                allowsMultipleSelection: false,
                legacyDocumentTypes: ["public.folder", "public.directory"],
                legacyDocumentMode: .open,
                asCopy: false
            ) { result in
                install(result, isFolder: true)
            }
        } else {
            ImportDocumentPicker(
                allowedContentTypes: [UTType(filenameExtension: "zip") ?? .data, .archive, .data],
                allowsMultipleSelection: false,
                legacyDocumentTypes: ["public.zip-archive", "com.pkware.zip-archive", "public.archive"],
                legacyDocumentMode: .import,
                asCopy: true
            ) { result in
                install(result, isFolder: false)
            }
        }
    }

    private func install(_ result: Result<[URL], Error>, isFolder: Bool) {
        pickerSource = nil
        guard case .success(let urls) = result, let url = urls.first else { return }
        Task {
            if isFolder {
                await importer.install(folderAt: url)
            } else {
                await importer.install(archiveAt: url)
            }
        }
    }
}
