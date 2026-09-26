// ShaderWorkspaceView.swift — Per-Game Settings-style shader workspace
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct ShaderLayerBrowserRequest: Identifiable {
    let id = UUID()
}

private struct ShaderLayerScrollRequest: Equatable {
    let layerID: String
    let sequence: Int
}

/// Shared by Settings, the Game Library and the in-game Quick Menu. The host
/// owns persistence; this view owns only shader browsing and presentation.
struct ShaderWorkspaceView: View {
    @Binding var enabled: Bool
    @Binding var presetRef: String
    var perGameChain: Binding<Int>?

    let settings: SettingsStore
    let controllerInput: MenuControllerInputRouter?
    let savesToRunningGame: Bool
    let onLivePreviewChange: (@MainActor (String, String) -> Void)?

    @Binding var selectedCategoryID: String
    @Binding var detailPresented: Bool
    @Binding var reportedControllerTargetOrder: [String]

    @StateObject private var passLibrary = ShaderPassLibrary()
    @StateObject private var params = ShaderParams()
    @State private var browseRequest: ShaderPresetBrowserRequest?
    @State private var addLayerRequest: ShaderLayerBrowserRequest?
    @State private var shareItem: ShareSheetItem?
    @State private var saveRequest: ShaderPresetSaveRequest?
    @State private var workspaceLayers: [ShaderWorkspaceLayerConfiguration] = []
    @State private var hiddenLayerIDs = Set<String>()
    @State private var expandedLayerIDs = Set<String>()
    @State private var expandedPassIDs = Set<String>()
    @State private var layerScrollRequest: ShaderLayerScrollRequest?
    @State private var layerScrollSequence = 0
    @State private var showsExtra = false
    @AppStorage("shaderWorkspace.showFilenames")
    private var showsFilenames = false

    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiContextMenuColour) private var panelTextColour
    @Environment(\.uiContextMenuSecondaryColour)
    private var panelSecondaryTextColour

    private static let managementID = "shaders"
    /// Expansion/collapse changes the Form row immediately; only the following
    /// viewport movement animates, so UIKit cannot slide the whole section.
    private static let layerScrollAnimation = Animation.smooth(
        duration: 0.46,
        extraBounce: 0
    )

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
            let configuration = ShaderWorkspacePresetEditor.configuration(
                for: activePresetToken
            )
            workspaceLayers = configuration.layers
            hiddenLayerIDs = configuration.hiddenLayerIDs
            await passLibrary.load(layers: configuration.layers)
            await params.load(token: activePresetToken)
            params.includeAdditionalParameters(
                passLibrary.layers.flatMap(\.parameters)
            )
            // Shader parameter controls are comparatively expensive (sliders,
            // geometry probes, controller targets). Keep every layer collapsed
            // on entry and build those controls only when the user asks for
            // them.
            expandedLayerIDs.removeAll()
            expandedPassIDs.removeAll()
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
        .onChange(of: expandedPassIDs) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: expandedLayerIDs) { _, _ in
            publishControllerOrder()
        }
        .onChange(of: params.params) { _, _ in
            publishControllerOrder()
        }
        .onAppear { publishControllerOrder() }
        .sheet(item: $browseRequest, onDismiss: validatePreset) { _ in
            NavigationStack {
                ShaderPresetBrowserView(
                    title: settings.localized("Shader Presets"),
                    folder: nil,
                    selectedToken: presetRef,
                    localized: settings.localized,
                    onSelect: selectPreset,
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
                    onSelect: addLayer,
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
                        selectPreset(token)
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
            .onChange(of: layerScrollRequest) { _, request in
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
                            selectPreset("")
                        }
                    } label: {
                        Text(settings.localized("Clear Preset"))
                    }
                    .foregroundStyle(.red)
                    .controllerAccessibilityActionTarget(
                        id: clearPresetTargetID,
                        label: settings.localized("Clear Preset")
                    ) {
                        selectPreset("")
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
                    shaderLayerCard(layer)
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
        .animation(.smooth(duration: 0.3), value: workspaceLayers)
    }

    private func shaderLayerCard(
        _ layer: ShaderPresetLayerDescriptor
    ) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Button {
                    performTouchAction(.activate) {
                        toggleLayerExpansion(layer)
                    }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: ShaderFunctionIcon.systemImage(for: layer.name))
                            .foregroundStyle(accentColour)
                            .frame(width: 24)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(layer.name)
                                .font(.body.weight(.semibold))
                                .foregroundStyle(panelTextColour)
                            if showsFilenames {
                                Text(displayName(for: layer.token))
                                    .font(.caption)
                                    .foregroundStyle(panelSecondaryTextColour)
                                    .lineLimit(1)
                            }
                        }

                        Image(systemName: "chevron.right")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(panelSecondaryTextColour)
                            .rotationEffect(
                                .degrees(
                                    expandedLayerIDs.contains(layer.id)
                                        ? 90 : 0
                                )
                            )
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .controllerAccessibilityActionTarget(
                    id: layerExpansionTargetID(layer),
                    label: layer.name
                ) {
                    toggleLayerExpansion(layer)
                }

                Spacer(minLength: 4)

                shaderActionButton(
                    systemImage: isLayerHidden(layer)
                        ? "eye.slash.fill" : "eye.fill",
                    color: isLayerHidden(layer) ? .secondary : accentColour,
                    label: settings.localized(
                        isLayerHidden(layer) ? "Enable Layer" : "Disable Layer"
                    ),
                    id: layerVisibilityTargetID(layer)
                ) {
                    toggleLayerVisibility(layer)
                }

                shaderActionButton(
                    systemImage: "square.and.arrow.up",
                    color: accentColour,
                    label: settings.localized("Share Shader Layer"),
                    id: layerShareTargetID(layer)
                ) {
                    shareLayer(layer)
                }

                ShaderPassReorderButton(
                    canMoveUp: canMove(layer, offset: -1),
                    canMoveDown: canMove(layer, offset: 1),
                    label: settings.localized("Move Shader Layer"),
                    targetID: layerMoveTargetID(layer),
                    controllerInput: controllerInput,
                    onMoveUp: { move(layer, offset: -1) },
                    onMoveDown: { move(layer, offset: 1) }
                )

                shaderActionButton(
                    systemImage: "minus.circle.fill",
                    color: .red,
                    label: settings.localized("Remove Shader Layer"),
                    id: layerRemoveTargetID(layer)
                ) {
                    removeLayer(layer)
                }
            }

            // The conditional is deliberately outside ForEach. Collapsed
            // layers therefore do not instantiate NumberRows, controller
            // geometry probes, or any parameter sub-tree.
            if expandedLayerIDs.contains(layer.id) {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(layer.passes) { pass in
                        if passTitleRepeatsLayer(pass, layer: layer) {
                            shaderPassParameters(pass, leadingPadding: 18)
                        } else {
                            shaderPassRow(pass)
                        }
                    }
                }
                .transition(.opacity)
            }
        }
        // Own this glass surface inside the stable layer-card identity instead
        // of relying exclusively on Form's native cell background. A Form
        // replaces/resizes its backing UICollectionView cell when this
        // accordion changes height; during that update the shared Settings
        // row decorator can be detached for a frame (or longer for very tall
        // cards). Keeping a non-materializing surface on the outer VStack
        // leaves the glass mounted while only the parameter subtree changes.
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .glassSurface(
            clear: true,
            forceClear: true,
            materializeTransition: false,
            cornerRadius: 16
        )
        .background {
            SettingsOwnedClearGlassMarker()
                .allowsHitTesting(false)
                .accessibilityHidden(true)
        }
        .listRowInsets(
            EdgeInsets(
                top: 6,
                leading: 12,
                bottom: 6,
                trailing: 12
            )
        )
        .listRowSeparator(.hidden)
        .listRowBackground(Color.clear)
    }

    private func shaderPassRow(_ pass: ShaderPassDescriptor) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Button {
                performTouchAction(.activate) {
                    togglePassExpansion(pass)
                }
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: pass.systemImage)
                        .foregroundStyle(accentColour)
                        .frame(width: 24)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(pass.name)
                            .foregroundStyle(panelTextColour)
                        if showsFilenames {
                            Text(pass.fileName)
                                .font(.caption)
                                .foregroundStyle(panelSecondaryTextColour)
                                .lineLimit(1)
                        }
                    }
                    Spacer(minLength: 4)
                    let count = parameters(for: pass).count
                    if count > 0 {
                        Text("\(count)")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(panelSecondaryTextColour)
                    }
                    Image(systemName: expandedPassIDs.contains(pass.id)
                        ? "chevron.down" : "chevron.right")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.tertiary)
                }
                .padding(.leading, 18)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .controllerAccessibilityActionTarget(
                id: passOpenTargetID(pass),
                label: pass.name
            ) {
                togglePassExpansion(pass)
            }

            if expandedPassIDs.contains(pass.id) {
                VStack(alignment: .leading, spacing: 8) {
                    shaderPassParameters(pass, leadingPadding: 18)
                }
                .transition(.opacity)
            }
        }
    }

    @ViewBuilder
    private func shaderPassParameters(
        _ pass: ShaderPassDescriptor,
        leadingPadding: CGFloat
    ) -> some View {
        if params.isLoading {
            ProgressView()
                .padding(.leading, leadingPadding + 34)
        } else if parameters(for: pass).isEmpty {
            Text(settings.localized("No adjustable parameters"))
                .font(.caption)
                .foregroundStyle(panelSecondaryTextColour)
                .padding(.leading, leadingPadding + 34)
        } else {
            ForEach(parameters(for: pass)) { param in
                parameterRow(param, pass: pass)
                    .padding(.leading, leadingPadding)
            }
        }
    }

    private func togglePassExpansion(_ pass: ShaderPassDescriptor) {
        if expandedPassIDs.contains(pass.id) {
            // Collapse without animating the containing Form row. Animating
            // that removal briefly hides the Enabled Shaders section and then
            // slides it back into place on UIKit-backed Forms.
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                expandedPassIDs.remove(pass.id)
            }
        } else {
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                expandedPassIDs.insert(pass.id)
            }
            if let layer = passLibrary.layers.first(where: {
                $0.passes.contains(where: { $0.id == pass.id })
            }) {
                requestScroll(to: layer.id)
            }
        }
    }

    private func toggleLayerExpansion(_ layer: ShaderPresetLayerDescriptor) {
        if expandedLayerIDs.contains(layer.id) {
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                expandedLayerIDs.remove(layer.id)
                expandedPassIDs.subtract(layer.passes.map(\.id))
            }
        } else {
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                expandedLayerIDs.insert(layer.id)
            }
            requestScroll(to: layer.id)
        }
    }

    private func requestScroll(to layerID: String) {
        layerScrollSequence &+= 1
        layerScrollRequest = ShaderLayerScrollRequest(
            layerID: layerID,
            sequence: layerScrollSequence
        )
    }

    private func shaderActionButton(
        systemImage: String,
        color: Color,
        label: String,
        id: String,
        action: @escaping () -> Void
    ) -> some View {
        Button {
            performTouchAction(.activate, action: action)
        } label: {
            Image(systemName: systemImage)
                .foregroundStyle(color)
                .frame(width: 32, height: 32)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .accessibilityLabel(label)
        .controllerAccessibilityActionTarget(
            id: id,
            label: label,
            action: action
        )
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
                        parameterRow(param, pass: pass)
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
                            onLivePreviewChange?(
                                settings.localized("Parameters"),
                                settings.localized("Defaults")
                            )
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
                            onLivePreviewChange?(
                                settings.localized("Parameters"),
                                settings.localized("Defaults")
                            )
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

    @ViewBuilder
    private func parameterRow(
        _ param: ShaderParam,
        pass: ShaderPassDescriptor
    ) -> some View {
        if param.isAdjustable {
            NumberRow(
                param.label,
                value: Binding(
                    get: { params.value(for: param) },
                    set: { value in
                        params.setValue(value, for: param)
                        presentParameterPreview(param)
                    }
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
            .controllerAccessibilityTargetID(
                parameterTargetID(param, pass: pass)
            )
        } else {
            Text(param.label)
                .font(.footnote.weight(.semibold))
                .foregroundStyle(panelSecondaryTextColour)
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
        displayName(for: activePresetToken)
    }

    private var shadersEnabledBinding: Binding<Bool> {
        if let chain = perGameChain {
            return Binding(
                get: { chain.wrappedValue != 0 },
                set: { next in
                    guard (chain.wrappedValue != 0) != next else { return }
                    chain.wrappedValue = next ? -1 : 0
                    onLivePreviewChange?(
                        settings.localized("Shader Chain"),
                        settings.localized(next ? "On" : "Off")
                    )
                }
            )
        }
        return Binding(
            get: { enabled },
            set: { next in
                guard enabled != next else { return }
                enabled = next
                onLivePreviewChange?(
                    settings.localized("Shader Chain"),
                    settings.localized(next ? "On" : "Off")
                )
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
            guard expandedLayerIDs.contains(layer.id) else { continue }
            for pass in layer.passes {
                let repeatsLayerTitle = passTitleRepeatsLayer(
                    pass,
                    layer: layer
                )
                if !repeatsLayerTitle {
                    result.append(passOpenTargetID(pass))
                }
                if repeatsLayerTitle || expandedPassIDs.contains(pass.id) {
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

    private func isLayerHidden(_ layer: ShaderPresetLayerDescriptor) -> Bool {
        hiddenLayerIDs.contains(layer.id)
    }

    private func toggleLayerVisibility(_ layer: ShaderPresetLayerDescriptor) {
        if hiddenLayerIDs.contains(layer.id) {
            hiddenLayerIDs.remove(layer.id)
        } else {
            hiddenLayerIDs.insert(layer.id)
            expandedLayerIDs.remove(layer.id)
            expandedPassIDs.subtract(layer.passes.map(\.id))
        }
        persistWorkspacePreset(
            previewLabel: layer.name,
            previewValue: settings.localized(
                hiddenLayerIDs.contains(layer.id) ? "Off" : "On"
            )
        )
    }

    private func canMove(
        _ layer: ShaderPresetLayerDescriptor,
        offset: Int
    ) -> Bool {
        guard let index = workspaceLayers.firstIndex(where: {
            $0.id == layer.id
        }) else { return false }
        return workspaceLayers.indices.contains(index + offset)
    }

    private func move(_ layer: ShaderPresetLayerDescriptor, offset: Int) {
        guard let index = workspaceLayers.firstIndex(where: {
            $0.id == layer.id
        }), workspaceLayers.indices.contains(index + offset) else { return }
        workspaceLayers.swapAt(index, index + offset)
        persistWorkspacePreset(
            previewLabel: layer.name,
            previewValue: settings.localized(
                offset < 0 ? "Moved Up" : "Moved Down"
            )
        )
    }

    private func removeLayer(_ layer: ShaderPresetLayerDescriptor) {
        workspaceLayers.removeAll { $0.id == layer.id }
        hiddenLayerIDs.remove(layer.id)
        expandedLayerIDs.remove(layer.id)
        expandedPassIDs.subtract(layer.passes.map(\.id))
        guard !workspaceLayers.isEmpty else {
            selectPreset("")
            return
        }
        persistWorkspacePreset(
            previewLabel: layer.name,
            previewValue: settings.localized("Removed")
        )
    }

    private func addLayer(_ token: String) {
        addLayerRequest = nil
        guard !token.isEmpty,
              ShaderPresetLibrary.resolve(token) != nil else { return }
        workspaceLayers.append(.init(token: token))
        Task { @MainActor in
            await passLibrary.load(layers: workspaceLayers)
            persistWorkspacePreset(
                previewLabel: settings.localized("Shader"),
                previewValue: displayName(for: token)
            )
        }
    }

    private func persistWorkspacePreset(
        previewLabel: String,
        previewValue: String
    ) {
        do {
            guard let token = try ShaderWorkspacePresetEditor.write(
                layers: workspaceLayers,
                descriptors: passLibrary.layers,
                hiddenLayerIDs: hiddenLayerIDs
            ) else { return }
            presetRef = token
            let hasVisibleLayer = workspaceLayers.contains {
                !hiddenLayerIDs.contains($0.id)
            }
            if let chain = perGameChain {
                chain.wrappedValue = hasVisibleLayer ? 1 : 0
            } else {
                enabled = hasVisibleLayer
            }
            onLivePreviewChange?(previewLabel, previewValue)
            ARMSX2Bridge.retryShaderChain()
        } catch {
            validatePreset()
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

    private func selectPreset(_ token: String) {
        if let chain = perGameChain {
            chain.wrappedValue = token.isEmpty ? 0 : 1
        } else if !token.isEmpty {
            enabled = true
        }
        presetRef = token
        browseRequest = nil
        onLivePreviewChange?(
            settings.localized("Preset"),
            displayName(for: token)
        )
    }

    private func validatePreset() {
        if !presetRef.isEmpty,
           ShaderPresetLibrary.resolve(presetRef) == nil {
            presetRef = ""
            if let chain = perGameChain { chain.wrappedValue = 0 }
        }
    }

    private func displayName(for token: String) -> String {
        guard let separator = token.firstIndex(
            of: ShaderPresetLibrary.markerSeparator
        ) else { return settings.localized("None") }
        let relative = token[token.index(after: separator)...]
        let name = URL(fileURLWithPath: String(relative))
            .deletingPathExtension().lastPathComponent
        return name.isEmpty ? settings.localized("None") : name
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

    private func passTitleRepeatsLayer(
        _ pass: ShaderPassDescriptor,
        layer: ShaderPresetLayerDescriptor
    ) -> Bool {
        normalizedShaderTitle(pass.name) == normalizedShaderTitle(layer.name)
    }

    private func normalizedShaderTitle(_ value: String) -> String {
        value.lowercased().filter { $0.isLetter || $0.isNumber }
    }

    private func presentParameterPreview(_ param: ShaderParam) {
        let value = params.value(for: param)
        onLivePreviewChange?(
            param.label,
            String(format: "%.*f", param.decimals, Double(value))
        )
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

    private func passOpenTargetID(_ pass: ShaderPassDescriptor) -> String {
        "shader-workspace.open." + pass.id
    }

    private func layerVisibilityTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.visibility." + layer.id
    }

    private func layerExpansionTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.expand." + layer.id
    }

    private func layerScrollAnchorID(_ layerID: String) -> String {
        "shader-workspace.layer.anchor." + layerID
    }

    private func layerShareTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.share." + layer.id
    }

    private func layerMoveTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.move." + layer.id
    }

    private func layerRemoveTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.remove." + layer.id
    }

    private func passSaveTargetID(_ pass: ShaderPassDescriptor) -> String {
        "shader-workspace.save." + pass.id
    }

    private func passResetTargetID(_ pass: ShaderPassDescriptor) -> String {
        "shader-workspace.reset." + pass.id
    }

    private func parameterTargetID(
        _ param: ShaderParam,
        pass: ShaderPassDescriptor
    ) -> String {
        "shader-workspace.parameter.\(pass.id).\(param.id)"
    }
}

private struct ShaderPassReorderButton: View {
    let canMoveUp: Bool
    let canMoveDown: Bool
    let label: String
    let targetID: String
    let controllerInput: MenuControllerInputRouter?
    let onMoveUp: () -> Void
    let onMoveDown: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        HStack(spacing: 0) {
            Button {
                performTouchAction(onMoveUp)
            } label: {
                Image(systemName: "chevron.up")
                    .frame(width: 22, height: 32)
            }
            .disabled(!canMoveUp)

            Button {
                performTouchAction(onMoveDown)
            } label: {
                Image(systemName: "chevron.down")
                    .frame(width: 22, height: 32)
            }
            .disabled(!canMoveDown)
        }
        .buttonStyle(.borderless)
        .foregroundStyle(accentColour)
        .accessibilityElement(children: .ignore)
        .controllerAccessibilityAdjustableTarget(
            id: targetID,
            label: label,
            value: nil,
            onActivate: {
                if canMoveDown { onMoveDown() }
                else if canMoveUp { onMoveUp() }
            },
            onIncrement: { if canMoveDown { onMoveDown() } },
            onDecrement: { if canMoveUp { onMoveUp() } }
        )
    }

    private func performTouchAction(_ action: () -> Void) {
        action()
        MenuAudioPackManager.shared.play(.activate)
        controllerInput?.playTouchHaptics(.activate)
    }
}
