// ShaderChainSection.swift — RetroArch shader chain rows, host-agnostic
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

private struct ShaderPackPickerSource: Identifiable {
    let isFolder: Bool

    var id: Bool { isFolder }
}

/// Persistence arrives from the caller, so these rows can serve the in-game pause surface
/// later without knowing which settings tier is writing underneath them.
struct ShaderChainSection: View {
    @Binding var enabled: Bool
    @Binding var presetRef: String
    let localized: @MainActor (String) -> String

    @StateObject private var importer = ShaderPackImporter()
    @StateObject private var params = ShaderParams()
    @State private var pickerSource: ShaderPackPickerSource?
    @State private var saveRequest: ShaderPresetSaveRequest?

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
            Toggle(localized("Shader Chain"), isOn: $enabled)
                .sheet(item: $pickerSource) { source in
                    picker(for: source)
                }
                .task(id: presetRef) {
                    await params.load(token: presetRef)
                }

            if enabled {
                NavigationLink {
                    ShaderPresetBrowserView(
                        title: localized("Shader Presets"),
                        folder: nil,
                        selectedToken: presetRef,
                        localized: localized,
                        onSelect: { presetRef = $0 }
                    )
                } label: {
                    HStack {
                        Text(localized("Preset"))
                        Spacer()
                        Text(presetName)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
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
                } label: {
                    Label(localized("Install Shader Pack"), systemImage: "square.and.arrow.down")
                }
                .disabled(importer.isBusy)

                if importer.isBusy {
                    ProgressView(localized("Installing..."))
                }

                if let installed = importer.installedName {
                    Text(localized("Installed") + " " + installed)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                ForEach(importer.errors.sorted { $0.key < $1.key }, id: \.key) { entry in
                    Text(entry.value)
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
            }
        } header: {
            Text(localized("Shader Chain"))
        } footer: {
            Text(localized("RetroArch .slangp presets, applied to the finished image after the other post-processing steps. A preset compiles the first time you select it, so that one frame can hitch."))
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
                // Item-bound, not isPresented: the host holds an observed SettingsStore, so an
                // isPresented content closure belongs to a body that re-runs on every settings
                // change and the name field would lose the keyboard on each keystroke.
                .sheet(item: $saveRequest) { request in
                    ShaderPresetSaveSheet(request: request, localized: localized) { name in
                        Task { await params.save(as: name) }
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

            if let saved = params.savedName {
                Text(localized("Saved") + " " + saved)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let failure = params.errorText {
                Text(failure)
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        } header: {
            Text(localized("Parameters"))
        } footer: {
            Text(localized("What this preset's author chose to expose, in the order they declared it. A saved preset lands in My Presets and is selectable from the browser; it points at the base pack by path, so removing that pack breaks it, and Reset on a saved preset returns to the values you saved."))
        }
    }

    @ViewBuilder
    private func parameterRow(_ param: ShaderParam) -> some View {
        if param.isAdjustable {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(param.name)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer()
                    if params.overrides[param.name] != nil {
                        Button {
                            params.reset(param)
                        } label: {
                            Image(systemName: "arrow.uturn.backward")
                        }
                        .buttonStyle(.borderless)
                        .accessibilityLabel(localized("Reset"))
                    }
                    Text(param.format(params.value(for: param)))
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }

                if !param.description.isEmpty, param.description != param.name {
                    Text(param.description)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Slider(
                    value: Binding(
                        get: { Double(param.index(of: params.value(for: param))) },
                        set: { params.setValue(param.value(at: Int($0.rounded())), for: param) }
                    ),
                    in: 0...Double(param.stepCount),
                    step: 1
                )
            }
        } else {
            Text(param.name)
                .font(.footnote.weight(.semibold))
                .foregroundStyle(.secondary)
        }
    }

    private var presetName: String {
        guard let separator = presetRef.firstIndex(of: ShaderPresetLibrary.markerSeparator) else {
            return localized("None")
        }
        let relative = presetRef[presetRef.index(after: separator)...]
        let name = URL(fileURLWithPath: String(relative))
            .deletingPathExtension().lastPathComponent
        return name.isEmpty ? localized("None") : name
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
