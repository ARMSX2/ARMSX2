// ShaderWorkspaceModel.swift — the shader workspace's layers and the edits that rewrite its preset
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderLayerScrollRequest: Equatable {
    let layerID: String
    let sequence: Int
}

struct ShaderWorkspaceHost {
    @Binding var enabled: Bool
    @Binding var presetRef: String
    var perGameChain: Binding<Int>?
    let passLibrary: ShaderPassLibrary
    let params: ShaderParams
    let onLivePreviewChange: (@MainActor (String) -> Void)?
    let dismissPresetBrowser: () -> Void
}

@MainActor
@Observable
final class ShaderWorkspaceModel {
    var workspaceLayers: [ShaderWorkspaceLayerConfiguration] = []
    var hiddenLayerIDs = Set<String>()
    var expandedLayerIDs = Set<String>()
    var expandedPassIDs = Set<String>()
    var layerScrollRequest: ShaderLayerScrollRequest?
    var layerScrollSequence = 0
    @ObservationIgnored private let settings = SettingsStore.shared
    @ObservationIgnored private var host: ShaderWorkspaceHost?

    private var enabled: Bool {
        get { host?.enabled ?? false }
        set { host?.enabled = newValue }
    }
    private var presetRef: String {
        get { host?.presetRef ?? "" }
        set { host?.presetRef = newValue }
    }
    private var perGameChain: Binding<Int>? { host?.perGameChain }
    private var passLibrary: ShaderPassLibrary { host!.passLibrary }
    private var params: ShaderParams { host!.params }
    private var onLivePreviewChange: (@MainActor (String) -> Void)? { host?.onLivePreviewChange }

    func loadChain(token activePresetToken: String, host: ShaderWorkspaceHost) async {
        self.host = host
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
        // Parameter controls are expensive, so every layer starts collapsed.
        expandedLayerIDs.removeAll()
        expandedPassIDs.removeAll()
    }

    func togglePassExpansion(_ pass: ShaderPassDescriptor, host: ShaderWorkspaceHost) {
        self.host = host
        if expandedPassIDs.contains(pass.id) {
            // Animating this removal hides the section and slides it back in.
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                _ = expandedPassIDs.remove(pass.id)
            }
        } else {
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                _ = expandedPassIDs.insert(pass.id)
            }
            if let layer = passLibrary.layers.first(where: {
                $0.passes.contains(where: { $0.id == pass.id })
            }) {
                requestScroll(to: layer.id)
            }
        }
    }

    func toggleLayerExpansion(_ layer: ShaderPresetLayerDescriptor) {
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
                _ = expandedLayerIDs.insert(layer.id)
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

    func isLayerHidden(_ layer: ShaderPresetLayerDescriptor) -> Bool {
        hiddenLayerIDs.contains(layer.id)
    }

    func toggleLayerVisibility(_ layer: ShaderPresetLayerDescriptor, host: ShaderWorkspaceHost) {
        self.host = host
        if hiddenLayerIDs.contains(layer.id) {
            hiddenLayerIDs.remove(layer.id)
        } else {
            hiddenLayerIDs.insert(layer.id)
            expandedLayerIDs.remove(layer.id)
            expandedPassIDs.subtract(layer.passes.map(\.id))
        }
        persistWorkspacePreset(
            previewValue: settings.localized(
                hiddenLayerIDs.contains(layer.id) ? "Off" : "On"
            )
        )
    }

    func canMove(
        _ layer: ShaderPresetLayerDescriptor,
        offset: Int
    ) -> Bool {
        guard let index = workspaceLayers.firstIndex(where: {
            $0.id == layer.id
        }) else { return false }
        return workspaceLayers.indices.contains(index + offset)
    }

    func move(_ layer: ShaderPresetLayerDescriptor, offset: Int, host: ShaderWorkspaceHost) {
        self.host = host
        guard let index = workspaceLayers.firstIndex(where: {
            $0.id == layer.id
        }), workspaceLayers.indices.contains(index + offset) else { return }
        workspaceLayers.swapAt(index, index + offset)
        persistWorkspacePreset(
            previewValue: settings.localized(
                offset < 0 ? "Moved Up" : "Moved Down"
            )
        )
    }

    func removeLayer(_ layer: ShaderPresetLayerDescriptor, host: ShaderWorkspaceHost) {
        self.host = host
        workspaceLayers.removeAll { $0.id == layer.id }
        hiddenLayerIDs.remove(layer.id)
        expandedLayerIDs.remove(layer.id)
        expandedPassIDs.subtract(layer.passes.map(\.id))
        guard !workspaceLayers.isEmpty else {
            selectPreset("", host: host)
            return
        }
        persistWorkspacePreset(
            previewValue: settings.localized("Removed")
        )
    }

    func addLayer(_ token: String, host: ShaderWorkspaceHost) {
        self.host = host
        guard !token.isEmpty,
              ShaderPresetLibrary.resolve(token) != nil else { return }
        workspaceLayers.append(.init(token: token))
        Task { @MainActor [self] in
            await passLibrary.load(layers: workspaceLayers)
            persistWorkspacePreset(
                previewValue: displayName(for: token)
            )
        }
    }

    private func persistWorkspacePreset(previewValue: String) {
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
            onLivePreviewChange?(previewValue)
            ARMSX2Bridge.retryShaderChain()
        } catch {
            validatePreset(host: host!)
        }
    }

    func selectPreset(_ token: String, host: ShaderWorkspaceHost) {
        self.host = host
        if let chain = perGameChain {
            chain.wrappedValue = token.isEmpty ? 0 : 1
        } else if !token.isEmpty {
            enabled = true
        }
        presetRef = token
        host.dismissPresetBrowser()
        onLivePreviewChange?(displayName(for: token))
    }

    func validatePreset(host: ShaderWorkspaceHost) {
        self.host = host
        if !presetRef.isEmpty,
           ShaderPresetLibrary.resolve(presetRef) == nil {
            presetRef = ""
            if let chain = perGameChain { chain.wrappedValue = 0 }
        }
    }

    func displayName(for token: String) -> String {
        guard let separator = token.firstIndex(
            of: ShaderPresetLibrary.markerSeparator
        ) else { return settings.localized("None") }
        let relative = token[token.index(after: separator)...]
        let name = URL(fileURLWithPath: String(relative))
            .deletingPathExtension().lastPathComponent
        return name.isEmpty ? settings.localized("None") : name
    }
}
