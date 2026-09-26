// ShaderChainSection.swift — RetroArch shader chain rows, host-agnostic
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

private struct ShaderPackPickerSource: Identifiable {
    let isFolder: Bool

    var id: Bool { isFolder }
}

private struct ShaderPackInstallOptionsView: View {
    let localized: @MainActor (String) -> String
    let controllerInput: MenuControllerInputRouter?
    let onZip: () -> Void
    let onFolder: () -> Void
    let onBasePack: () -> Void

    @Environment(\.dismiss) private var dismiss

    private let targetOrder = [
        "shader-install.zip",
        "shader-install.folder",
        "shader-install.base-pack",
        "shader-install.cancel",
    ]

    var body: some View {
        NavigationStack {
            List {
                installButton(
                    id: "shader-install.zip",
                    title: localized("From a Zip Archive"),
                    systemImage: "doc.zipper",
                    action: onZip
                )
                installButton(
                    id: "shader-install.folder",
                    title: localized("From a Folder"),
                    systemImage: "folder",
                    action: onFolder
                )
                installButton(
                    id: "shader-install.base-pack",
                    title: localized("Get Base Shader Pack"),
                    systemImage: "arrow.down.circle.fill",
                    action: onBasePack
                )
            }
            .navigationTitle(localized("Install Shader Pack"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button(localized("Cancel")) { dismiss() }
                        .controllerAccessibilityActionTarget(
                            id: "shader-install.cancel",
                            label: localized("Cancel"),
                            activationFeedback: .back
                        ) {
                            dismiss()
                        }
                }
            }
            .controllerAccessibilityTargetOrder(targetOrder)
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                scopeKey: "shader-install-options",
                priority: 760,
                orbStyle: .liquidGlass,
                onBack: {
                    dismiss()
                    return true
                },
                usesNativeFocusEngine: false,
                usesExplicitTargetGeometryOnly: true,
                preferredInitialFocusLabel: targetOrder.first,
                declaredTargetOrder: targetOrder
            )
        }
        .presentationDetents([.medium])
    }

    private func installButton(
        id: String,
        title: String,
        systemImage: String,
        action: @escaping () -> Void
    ) -> some View {
        Button {
            dismissThen(action)
        } label: {
            Label(title, systemImage: systemImage)
        }
        .controllerAccessibilityActionTarget(id: id, label: title) {
            dismissThen(action)
        }
    }

    private func dismissThen(_ action: @escaping () -> Void) {
        dismiss()
        Task { @MainActor in
            await Task.yield()
            action()
        }
    }
}

private struct ShaderCatalogBrowserRequest: Identifiable {
    let id = "shader-catalog-browser"
}

/// Persistence comes from the caller, so Settings and the pause card share these rows.
struct ShaderChainSection: View {
    @Binding var enabled: Bool
    @Binding var presetRef: String
    let localized: @MainActor (String) -> String
    var controllerInput: MenuControllerInputRouter? = nil
    var onLivePreviewChange: (@MainActor (String, String) -> Void)? = nil
    var showsParameters = true
    var showsClearPresetAction = true
    var showsEnabledToggle = true

    @StateObject private var importer = ShaderPackImporter()
    @StateObject private var params = ShaderParams()
    @State private var pickerSource: ShaderPackPickerSource?
    @State private var browseRequest: ShaderPresetBrowserRequest?
    @State private var catalogRequest: ShaderCatalogBrowserRequest?
    @State private var saveRequest: ShaderPresetSaveRequest?
    @State private var showsInstallOptions = false

    private var settings: SettingsStore { SettingsStore.shared }

    var body: some View {
        Group {
            chainSection
            if showsParameters,
               enabled,
               !presetRef.isEmpty,
               params.isLoading || !params.params.isEmpty {
                parameterSection
            }
        }
    }

    private var chainSection: some View {
        Section {
            Button {
                performTouchAction(.submenu) {
                    browseRequest = ShaderPresetBrowserRequest()
                }
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
            .controllerAccessibilityActionTarget(
                id: "shader-chain.preset",
                label: localized("Preset")
            ) {
                browseRequest = ShaderPresetBrowserRequest()
            }
            .sheet(item: $browseRequest, onDismiss: {
                if !presetRef.isEmpty, ShaderPresetLibrary.resolve(presetRef) == nil { presetRef = "" }
            }) { _ in
                NavigationStack {
                    ShaderPresetBrowserView(
                        title: localized("Shader Presets"),
                        folder: nil,
                        selectedToken: presetRef,
                        localized: localized,
                        onSelect: selectPreset,
                        controllerInput: controllerInput,
                        onClose: { browseRequest = nil }
                    )
                }
            }
            .task(id: presetRef) {
                await params.load(token: presetRef)
            }

            if let failure = params.loadFailure {
                problem(failure)
            }

            if showsEnabledToggle, !presetRef.isEmpty {
                Toggle(localized("Shaders"), isOn: touchLiveEnabled)
                    .controllerAccessibilityToggleTarget(
                        id: "shader-chain.enabled",
                        label: localized("Shaders"),
                        isOn: liveEnabled
                    )
            }

            Button {
                performTouchAction(.submenu) {
                    catalogRequest = ShaderCatalogBrowserRequest()
                }
            } label: {
                Label(localized("Download Shaders"), systemImage: "arrow.down.circle")
            }
            .controllerAccessibilityActionTarget(
                id: "shader-chain.download",
                label: localized("Download Shaders")
            ) {
                catalogRequest = ShaderCatalogBrowserRequest()
            }
            .sheet(item: $catalogRequest) { _ in
                NavigationStack {
                    ShaderCatalogBrowserView(
                        localized: localized,
                        onSelect: selectPreset,
                        controllerInput: controllerInput
                    )
                }
                .presentationDetents([.large])
            }

            Button {
                performTouchAction(.submenu) {
                    showsInstallOptions = true
                }
            } label: {
                Label(localized("Install Shader Pack"), systemImage: "square.and.arrow.down")
            }
            .controllerAccessibilityActionTarget(
                id: "shader-chain.install",
                label: localized("Install Shader Pack")
            ) {
                showsInstallOptions = true
            }
            .disabled(importer.isBusy)
            .sheet(item: $pickerSource) { source in
                picker(for: source)
            }
            .sheet(isPresented: $showsInstallOptions) {
                ShaderPackInstallOptionsView(
                    localized: localized,
                    controllerInput: controllerInput,
                    onZip: {
                        pickerSource = ShaderPackPickerSource(isFolder: false)
                    },
                    onFolder: {
                        pickerSource = ShaderPackPickerSource(isFolder: true)
                    },
                    onBasePack: getBasePack
                )
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

            if showsClearPresetAction, !presetRef.isEmpty {
                Button(role: .destructive) {
                    performTouchAction(.activate) {
                        selectPreset("")
                    }
                } label: {
                    Text(localized("Clear Preset"))
                }
                .controllerAccessibilityActionTarget(
                    id: "shader-chain.clear-preset",
                    label: localized("Clear Preset")
                ) {
                    selectPreset("")
                }
                .foregroundStyle(.red)
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
                .controllerAccessibilityActionTarget(
                    id: "shader-chain.save-preset",
                    label: localized("Save as New Preset")
                ) {
                    saveRequest = ShaderPresetSaveRequest(
                        token: presetRef, suggestedName: presetName)
                }
            }

            if params.hasOverrides {
                ConfirmedSettingsResetButton(
                    localized("Reset All Parameters"),
                    confirmationTitle: localized("Reset Shader Parameters?"),
                    confirmationMessage: localized("This restores every parameter exposed by the current shader preset."),
                    completionMessage: localized("Defaults Restored"),
                    controllerTargetID: "shader-chain.reset-parameters"
                ) {
                    params.resetAll()
                    onLivePreviewChange?(
                        localized("Parameters"),
                        localized("Defaults")
                    )
                }
                .uiCriticalForegroundStyle()
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
                    set: { setParameter($0, for: param) }
                ),
                in: param.minimum...param.maximum,
                format: NumberFormat.plain.decimals(param.decimals),
                step: Double(param.increment),
                detents: NumberRow.stops(
                    in: Double(param.minimum)...Double(param.maximum),
                    step: Double(param.increment)
                ),
                accessory: NumberRowAccessory(
                    systemImage: "arrow.counterclockwise",
                    label: "Reset %@",
                    isVisible: params.overrides[param.name] != nil,
                    action: {
                        params.reset(param)
                        presentParameterPreview(param)
                    }
                ),
                settings: settings
            )
            .controllerAccessibilityTargetID(parameterTargetID(param))
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
            ARMSX2Bridge.retryShaderChain()
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

    private var liveEnabled: Binding<Bool> {
        Binding(
            get: { enabled },
            set: { next in
                guard enabled != next else { return }
                enabled = next
                onLivePreviewChange?(
                    localized("Shader Chain"),
                    localized(next ? "On" : "Off")
                )
            }
        )
    }

    private var touchLiveEnabled: Binding<Bool> {
        Binding(
            get: { liveEnabled.wrappedValue },
            set: { next in
                performTouchAction(.toggle(isOn: next)) {
                    liveEnabled.wrappedValue = next
                }
            }
        )
    }

    private func performTouchAction(
        _ feedback: MenuControllerFeedback,
        action: () -> Void
    ) {
        action()
        MenuAudioPackManager.shared.play(feedback)
        controllerInput?.playTouchHaptics(feedback)
    }

    private func selectPreset(_ token: String) {
        guard presetRef != token else { return }
        if !token.isEmpty {
            enabled = true
        }
        presetRef = token
        onLivePreviewChange?(
            localized("Preset"),
            token.isEmpty ? localized("None") : presetDisplayName(token)
        )
    }

    private func setParameter(_ value: Float, for param: ShaderParam) {
        params.setValue(value, for: param)
        presentParameterPreview(param)
    }

    private func presentParameterPreview(_ param: ShaderParam) {
        let value = params.value(for: param)
        onLivePreviewChange?(
            param.name,
            String(format: "%.*f", param.decimals, Double(value))
        )
    }

    private func parameterTargetID(_ param: ShaderParam) -> String {
        "shader-chain.parameter." + param.id
    }

    private func presetDisplayName(_ token: String) -> String {
        guard let separator = token.firstIndex(of: ShaderPresetLibrary.markerSeparator) else {
            return localized("None")
        }
        let relative = token[token.index(after: separator)...]
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
