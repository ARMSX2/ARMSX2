// ShaderWorkspaceView.swift — Per-Game Settings-style shader workspace
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ShaderLayerBrowserRequest: Identifiable {
    let id = UUID()
}

/// Shared by Settings, the Game Library and the in-game Quick Menu. The host
/// owns persistence; this view owns only shader browsing and presentation.
struct ShaderWorkspaceView: View, ShaderWorkspaceTargetIDs {
    @Binding var enabled: Bool
    @Binding var presetRef: String
    var perGameChain: Binding<Int>?

    let settings: SettingsStore
    let controllerInput: MenuControllerInputRouter?
    let savesToRunningGame: Bool
    let onLivePreviewChange: (@MainActor (String) -> Void)?

    @Binding var selectedCategoryID: String
    @Binding var detailPresented: Bool
    @Binding var reportedControllerTargetOrder: [String]

    @StateObject private var passLibrary = ShaderPassLibrary()
    @StateObject private var params = ShaderParams()
    @State private var browseRequest: ShaderPresetBrowserRequest?
    @State private var addLayerRequest: ShaderLayerBrowserRequest?
    @State private var shareItem: ShareSheetItem?
    @State private var saveRequest: ShaderPresetSaveRequest?
    @State private var model = ShaderWorkspaceModel()
    @State private var showsExtra = false
    @AppStorage("shaderWorkspace.showFilenames")
    private var showsFilenames = false

    private static let managementID = "shaders"
    /// Expansion/collapse changes the Form row immediately; only the following
    /// viewport movement animates, so UIKit cannot slide the whole section.
    private static let layerScrollAnimation = Animation.smooth(
        duration: 0.46,
        extraBounce: 0
    )

    private var host: ShaderWorkspaceHost {
        // A binding, not `self`: the model keeps the host, and the view owns the model.
        let browse = $browseRequest
        return ShaderWorkspaceHost(
            enabled: $enabled,
            presetRef: $presetRef,
            perGameChain: perGameChain,
            passLibrary: passLibrary,
            params: params,
            onLivePreviewChange: onLivePreviewChange,
            dismissPresetBrowser: { browse.wrappedValue = nil }
        )
    }

    var body: some View {
        Group {
            if detailPresented, let pass = selectedPass {
                passContent(pass)
            } else {
                managementContent
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .transition(.opacity)
        .animation(.smooth(duration: 0.28), value: detailPresented)
        .task(id: activePresetToken) {
            await model.loadChain(token: activePresetToken, host: host)
            normalizeSelection()
            publishControllerOrder()
        }
        .onChange(of: passLibrary.passes) { _, _ in
            normalizeSelection()
            publishControllerOrder()
        }
        .onChange(of: selectedCategoryID) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: detailPresented) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: showsExtra) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: model.expandedPassIDs) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: model.expandedLayerIDs) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: params.params) { _, _ in
            publishControllerOrder()
        }
        .onAppear { publishControllerOrder() }
        .sheet(item: $browseRequest, onDismiss: { model.validatePreset(host: host) }) { _ in
            NavigationStack {
                ShaderPresetBrowserView(
                    title: settings.localized("Shader Presets"),
                    folder: nil,
                    selectedToken: presetRef,
                    localized: settings.localized,
                    onSelect: { model.selectPreset($0, host: host) },
                    controllerInput: controllerInput,
                    onClose: { browseRequest = nil }
                )
            }
        }
        .sheet(item: $addLayerRequest) { _ in
            NavigationStack {
                ShaderPresetBrowserView(
                    title: settings.localized("Add Shader"),
                    folder: nil,
                    selectedToken: "",
                    localized: settings.localized,
                    onSelect: { token in
                        addLayerRequest = nil
                        model.addLayer(token, host: host)
                    },
                    controllerInput: controllerInput,
                    onClose: { addLayerRequest = nil }
                )
            }
        }
        .sheet(item: $shareItem) { item in
            ActivityShareSheet(activityItems: [item.url])
        }
        .sheet(item: $saveRequest) { request in
            ShaderPresetSaveSheet(
                request: request,
                localized: settings.localized
            ) { name in
                Task {
                    if let token = await params.save(as: name) {
                        model.selectPreset(token, host: host)
                    }
                }
            }
        }
    }

    private var managementContent: some View {
        ScrollViewReader { proxy in
            Form {
                activeShadersSection

                if let chain = perGameChain {
                    PerGameShaderSection(
                        enabled: true,
                        chain: chain,
                        presetRef: $presetRef,
                        settings: settings,
                        onBrowse: {
                            browseRequest = ShaderPresetBrowserRequest()
                        },
                        showsClearPresetAction: false
                    )
                } else {
                    ShaderChainSection(
                        enabled: $enabled,
                        presetRef: $presetRef,
                        localized: settings.localized,
                        controllerInput: controllerInput,
                        onLivePreviewChange: onLivePreviewChange,
                        showsParameters: false,
                        showsClearPresetAction: false,
                        showsEnabledToggle: false
                    )
                }

                workspaceActionsSection

                if showsExtra {
                    Section(settings.localized("Extra")) {
                        Toggle(
                            settings.localized("Show Filenames"),
                            isOn: touchShowsFilenamesBinding
                        )
                        .controllerAccessibilityToggleTarget(
                            id: "shader-workspace.show-filenames",
                            label: settings.localized("Show Filenames"),
                            isOn: $showsFilenames
                        )
                    }
                    PerGameLivePreviewSection(
                        savesToRunningGame: savesToRunningGame,
                        settings: settings
                    )
                }
            }
            .foregroundStyle(
                settings.controllerTextAppearance.contentColor ?? .white
            )
            .tint(settings.controllerNavigationAccentColor)
            .scrollContentBackground(.hidden)
            .background {
                ControllerRightStickScrollTarget(
                    controllerInput: controllerInput,
                    axes: .vertical,
                    priority: 230,
                    isEnabled: !detailPresented,
                    searchesNearbyScrollViews: true
                )
                .allowsHitTesting(false)
            }
            .onChange(of: model.layerScrollRequest) { _, request in
                guard let request else { return }
                Task { @MainActor in
                    // First let Form adopt the expanded row height. The
                    // selected layer then glides into the readable top area in
                    // a separate transaction, avoiding the old competing snap.
                    await Task.yield()
                    withAnimation(Self.layerScrollAnimation) {
                        proxy.scrollTo(
                            layerScrollAnchorID(request.layerID),
                            anchor: .top
                        )
                    }
                }
            }
        }
    }

    private var workspaceActionsSection: some View {
        Section {
            HStack(spacing: 12) {
                Button {
                    performTouchAction(.activate) {
                        withAnimation(.smooth(duration: 0.22)) {
                            showsExtra.toggle()
                        }
                    }
                } label: {
                    Label(
                        settings.localized("Extra"),
                        systemImage: showsExtra
                            ? "chevron.up.circle" : "ellipsis.circle"
                    )
                }
                .controllerAccessibilityActionTarget(
                    id: "shader-workspace.extra",
                    label: settings.localized("Extra")
                ) {
                    withAnimation(.smooth(duration: 0.22)) {
                        showsExtra.toggle()
                    }
                }

                Spacer(minLength: 8)

                if !presetRef.isEmpty {
                    Button(role: .destructive) {
                        performTouchAction(.activate) {
                            model.selectPreset("", host: host)
                        }
                    } label: {
                        Text(settings.localized("Clear Preset"))
                    }
                    .foregroundStyle(.red)
                    .controllerAccessibilityActionTarget(
                        id: clearPresetTargetID,
                        label: settings.localized("Clear Preset")
                    ) {
                        model.selectPreset("", host: host)
                    }
                }
            }
        }
    }

    @ViewBuilder
    private var activeShadersSection: some View {
        Section {
            if passLibrary.isLoading {
                HStack { Spacer(); ProgressView(); Spacer() }
            } else if passLibrary.layers.isEmpty {
                Text(settings.localized(
                    activePresetToken.isEmpty
                        ? "Choose a preset or add a shader."
                        : "This preset does not declare any shader passes."
                ))
                .font(.caption)
                .foregroundStyle(.secondary)
            } else {
                ForEach(passLibrary.layers) { layer in
                    ShaderLayerCard(
                        layer: layer,
                        model: model,
                        host: host,
                        params: params,
                        showsFilenames: showsFilenames,
                        controllerInput: controllerInput,
                        parameters: parameters(for:),
                        onShare: shareLayer
                    )
                        .id(layerScrollAnchorID(layer.id))
                }
            }

            Button {
                performTouchAction(.submenu) {
                    addLayerRequest = ShaderLayerBrowserRequest()
                }
            } label: {
                Label(
                    settings.localized("Add Shader"),
                    systemImage: "plus.circle.fill"
                )
                .frame(maxWidth: .infinity, alignment: .center)
            }
            .controllerAccessibilityActionTarget(
                id: "shader-workspace.add-layer",
                label: settings.localized("Add Shader")
            ) {
                addLayerRequest = ShaderLayerBrowserRequest()
            }
        } header: {
            HStack(spacing: 12) {
                Text(settings.localized("Enabled Shaders"))
                Spacer(minLength: 12)
                Toggle(
                    settings.localized("Shaders"),
                    isOn: touchShadersEnabledBinding
                )
                .labelsHidden()
                .disabled(presetRef.isEmpty)
                .controllerAccessibilityToggleTarget(
                    id: "shader-workspace.enabled",
                    label: settings.localized("Shaders"),
                    isOn: shadersEnabledBinding
                )
            }
            .textCase(nil)
        }
        .animation(.smooth(duration: 0.3), value: model.workspaceLayers)
    }

    private func passContent(_ pass: ShaderPassDescriptor) -> some View {
        Form {
            Section(settings.localized("Parameters")) {
                if params.isLoading {
                    HStack { Spacer(); ProgressView(); Spacer() }
                } else if parameters(for: pass).isEmpty {
                    Text(settings.localized(
                        "This shader pass does not expose adjustable parameters."
                    ))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                } else {
                    ForEach(parameters(for: pass)) { param in
                        ShaderParameterRow(
                            param: param,
                            pass: pass,
                            params: params,
                            settings: settings,
                            onLivePreviewChange: onLivePreviewChange
                        )
                    }
                }
            }

            if !params.params.isEmpty {
                Section {
                    Button {
                        saveRequest = ShaderPresetSaveRequest(
                            token: activePresetToken,
                            suggestedName: activePresetDisplayName
                        )
                    } label: {
                        Label(
                            settings.localized("Save as New Preset"),
                            systemImage: "square.and.arrow.down"
                        )
                    }
                    .controllerAccessibilityActionTarget(
                        id: passSaveTargetID(pass),
                        label: settings.localized("Save as New Preset")
                    ) {
                        saveRequest = ShaderPresetSaveRequest(
                            token: activePresetToken,
                            suggestedName: activePresetDisplayName
                        )
                    }

                    if params.hasOverrides {
                        Button(role: .destructive) {
                            params.resetAll()
                            onLivePreviewChange?(settings.localized("Defaults"))
                        } label: {
                            Label(
                                settings.localized("Reset All Parameters"),
                                systemImage: "arrow.counterclockwise"
                            )
                        }
                        .foregroundStyle(.red)
                        .controllerAccessibilityActionTarget(
                            id: passResetTargetID(pass),
                            label: settings.localized("Reset All Parameters")
                        ) {
                            params.resetAll()
                            onLivePreviewChange?(settings.localized("Defaults"))
                        }
                    }
                }
            }
        }
        .scrollContentBackground(.hidden)
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 230,
                isEnabled: detailPresented,
                searchesNearbyScrollViews: true
            )
            .allowsHitTesting(false)
        }
    }

    private var selectedPass: ShaderPassDescriptor? {
        passLibrary.passes.first { $0.id == selectedCategoryID }
    }

    private var activePresetToken: String {
        if let chain = perGameChain {
            switch chain.wrappedValue {
            case 1: return presetRef
            case -1:
                return settings.shaderChainPresetRef
            default: return presetRef
            }
        }
        return presetRef
    }

    private var activePresetDisplayName: String {
        model.displayName(for: activePresetToken)
    }

    private var shadersEnabledBinding: Binding<Bool> {
        if let chain = perGameChain {
            return Binding(
                get: { chain.wrappedValue != 0 },
                set: { next in
                    guard (chain.wrappedValue != 0) != next else { return }
                    chain.wrappedValue = next ? -1 : 0
                    onLivePreviewChange?(settings.localized(next ? "On" : "Off"))
                }
            )
        }
        return Binding(
            get: { enabled },
            set: { next in
                guard enabled != next else { return }
                enabled = next
                onLivePreviewChange?(settings.localized(next ? "On" : "Off"))
            }
        )
    }

    private var touchShadersEnabledBinding: Binding<Bool> {
        Binding(
            get: { shadersEnabledBinding.wrappedValue },
            set: { next in
                performTouchAction(.toggle(isOn: next)) {
                    shadersEnabledBinding.wrappedValue = next
                }
            }
        )
    }

    private var touchShowsFilenamesBinding: Binding<Bool> {
        Binding(
            get: { showsFilenames },
            set: { next in
                guard showsFilenames != next else { return }
                performTouchAction(.toggle(isOn: next)) {
                    showsFilenames = next
                }
            }
        )
    }

    private var managementControllerOrder: [String] {
        var result = ["shader-workspace.enabled"]
        for layer in passLibrary.layers {
            result.append(layerExpansionTargetID(layer))
            result += [
                layerVisibilityTargetID(layer),
                layerShareTargetID(layer),
                layerMoveTargetID(layer),
                layerRemoveTargetID(layer),
            ]
            guard model.expandedLayerIDs.contains(layer.id) else { continue }
            for pass in layer.passes {
                let repeatsLayerTitle = passTitleRepeatsLayer(
                    pass,
                    layer: layer
                )
                if !repeatsLayerTitle {
                    result.append(passOpenTargetID(pass))
                }
                if repeatsLayerTitle || model.expandedPassIDs.contains(pass.id) {
                    result += parameters(for: pass).map {
                        parameterTargetID($0, pass: pass)
                    }
                }
            }
        }
        result.append("shader-workspace.add-layer")

        if let chain = perGameChain {
            result += [
                "per-game.graphics.shader-chain",
                "per-game.graphics.download-shaders",
            ]
            if chain.wrappedValue == 1 {
                result.append("per-game.graphics.shader-preset")
            }
        } else {
            result.append("shader-chain.preset")
            result += ["shader-chain.download", "shader-chain.install"]
        }
        result.append("shader-workspace.extra")
        if !presetRef.isEmpty {
            result.append(clearPresetTargetID)
        }
        if showsExtra {
            result.append("shader-workspace.show-filenames")
            result.append("per-game.general.live-preview")
            if settings.temporalSaveStateToLivePreviewChanges {
                result += [
                    "per-game.general.live-preview-circle-exit",
                    "per-game.general.before-changes-preview-duration",
                ]
            }
        }
        return result
    }

    private func passControllerOrder(
        _ pass: ShaderPassDescriptor
    ) -> [String] {
        var result = parameters(for: pass).map {
            parameterTargetID($0, pass: pass)
        }
        if !params.params.isEmpty { result.append(passSaveTargetID(pass)) }
        if params.hasOverrides { result.append(passResetTargetID(pass)) }
        return result
    }

    private var currentControllerOrder: [String] {
        if !detailPresented {
            return managementControllerOrder
        }
        guard let pass = selectedPass else { return managementControllerOrder }
        return passControllerOrder(pass)
    }

    private func publishControllerOrder() {
        let order = currentControllerOrder
        guard reportedControllerTargetOrder != order else { return }
        reportedControllerTargetOrder = order
    }

    private func normalizeSelection() {
        guard selectedCategoryID == Self.managementID
                || passLibrary.passes.contains(where: {
                    $0.id == selectedCategoryID
                }) else {
            selectedCategoryID = Self.managementID
            detailPresented = false
            return
        }
    }

    private func performTouchAction(
        _ feedback: MenuControllerFeedback,
        action: () -> Void
    ) {
        action()
        MenuAudioPackManager.shared.play(feedback)
        controllerInput?.playTouchHaptics(feedback)
    }

    private func parameters(
        for pass: ShaderPassDescriptor
    ) -> [ShaderParam] {
        let direct = params.params.filter {
            pass.parameterNames.contains($0.name)
        }
        if !direct.isEmpty { return direct }

        guard let layer = passLibrary.layers.first(where: {
            $0.passes.contains(where: { $0.id == pass.id })
        }) else {
            return passLibrary.passes.count == 1 ? params.params : []
        }
        let layerParameterNames = Set(layer.parameters.map(\.name))
        let layerParameters = params.params.filter {
            layerParameterNames.contains($0.name)
        }
        if layer.passes.count == 1 { return layerParameters }

        let claimedByOtherPasses = layer.passes
            .filter { $0.id != pass.id }
            .reduce(into: Set<String>()) {
                $0.formUnion($1.parameterNames)
            }
        return layerParameters.filter {
            !claimedByOtherPasses.contains($0.name)
        }
    }

    private func shareLayer(_ layer: ShaderPresetLayerDescriptor) {
        guard let url = ShaderPresetLibrary.resolve(layer.token) else { return }
        shareItem = ShareSheetItem(url: url)
    }

    private var clearPresetTargetID: String {
        perGameChain == nil
            ? "shader-chain.clear-preset"
            : "per-game.graphics.clear-shader-preset"
    }

    private func layerScrollAnchorID(_ layerID: String) -> String {
        "shader-workspace.layer.anchor." + layerID
    }
}
