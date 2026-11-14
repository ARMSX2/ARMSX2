// ShaderParameterRow.swift — one shader parameter as a number row
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderParameterRow: View, ShaderWorkspaceTargetIDs {
    let param: ShaderParam
    let pass: ShaderPassDescriptor
    @ObservedObject var params: ShaderParams
    let settings: SettingsStore
    let onLivePreviewChange: (@MainActor (String) -> Void)?

    @Environment(\.uiContextMenuSecondaryColour)
    private var panelSecondaryTextColour

    var body: some View {
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

    private func presentParameterPreview(_ param: ShaderParam) {
        let value = params.value(for: param)
        onLivePreviewChange?(String(format: "%.*f", param.decimals, Double(value)))
    }
}
