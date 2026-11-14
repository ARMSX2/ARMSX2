// ShaderWorkspaceTargetIDs.swift — the controller target ids the shader workspace builds from a layer, pass or parameter
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

protocol ShaderWorkspaceTargetIDs {}

extension ShaderWorkspaceTargetIDs {
    func passOpenTargetID(_ pass: ShaderPassDescriptor) -> String {
        "shader-workspace.open." + pass.id
    }

    func layerVisibilityTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.visibility." + layer.id
    }

    func layerExpansionTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.expand." + layer.id
    }

    func layerShareTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.share." + layer.id
    }

    func layerMoveTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.move." + layer.id
    }

    func layerRemoveTargetID(
        _ layer: ShaderPresetLayerDescriptor
    ) -> String {
        "shader-workspace.layer.remove." + layer.id
    }

    func passSaveTargetID(_ pass: ShaderPassDescriptor) -> String {
        "shader-workspace.save." + pass.id
    }

    func passResetTargetID(_ pass: ShaderPassDescriptor) -> String {
        "shader-workspace.reset." + pass.id
    }

    func parameterTargetID(
        _ param: ShaderParam,
        pass: ShaderPassDescriptor
    ) -> String {
        "shader-workspace.parameter.\(pass.id).\(param.id)"
    }

    func passTitleRepeatsLayer(
        _ pass: ShaderPassDescriptor,
        layer: ShaderPresetLayerDescriptor
    ) -> Bool {
        normalizedShaderTitle(pass.name) == normalizedShaderTitle(layer.name)
    }

    func normalizedShaderTitle(_ value: String) -> String {
        value.lowercased().filter { $0.isLetter || $0.isNumber }
    }
}
