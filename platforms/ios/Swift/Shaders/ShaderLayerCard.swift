// ShaderLayerCard.swift — one shader layer in the workspace, with its passes and parameters
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderLayerCard: View, ShaderWorkspaceTargetIDs {
    let layer: ShaderPresetLayerDescriptor
    let model: ShaderWorkspaceModel
    let host: ShaderWorkspaceHost
    @ObservedObject var params: ShaderParams
    let showsFilenames: Bool
    let controllerInput: MenuControllerInputRouter?
    let parameters: (ShaderPassDescriptor) -> [ShaderParam]
    let onShare: (ShaderPresetLayerDescriptor) -> Void

    @State private var settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiContextMenuColour) private var panelTextColour
    @Environment(\.uiContextMenuSecondaryColour)
    private var panelSecondaryTextColour

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Button {
                    performTouchAction(.activate) {
                        model.toggleLayerExpansion(layer)
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
                                Text(model.displayName(for: layer.token))
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
                                    model.expandedLayerIDs.contains(layer.id)
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
                    model.toggleLayerExpansion(layer)
                }

                Spacer(minLength: 4)

                shaderActionButton(
                    systemImage: model.isLayerHidden(layer)
                        ? "eye.slash.fill" : "eye.fill",
                    color: model.isLayerHidden(layer) ? .secondary : accentColour,
                    label: settings.localized(
                        model.isLayerHidden(layer) ? "Enable Layer" : "Disable Layer"
                    ),
                    id: layerVisibilityTargetID(layer)
                ) {
                    model.toggleLayerVisibility(layer, host: host)
                }

                shaderActionButton(
                    systemImage: "square.and.arrow.up",
                    color: accentColour,
                    label: settings.localized("Share Shader Layer"),
                    id: layerShareTargetID(layer)
                ) {
                    onShare(layer)
                }

                ShaderPassReorderButton(
                    canMoveUp: model.canMove(layer, offset: -1),
                    canMoveDown: model.canMove(layer, offset: 1),
                    label: settings.localized("Move Shader Layer"),
                    targetID: layerMoveTargetID(layer),
                    controllerInput: controllerInput,
                    onMoveUp: { model.move(layer, offset: -1, host: host) },
                    onMoveDown: { model.move(layer, offset: 1, host: host) }
                )

                shaderActionButton(
                    systemImage: "minus.circle.fill",
                    color: .red,
                    label: settings.localized("Remove Shader Layer"),
                    id: layerRemoveTargetID(layer)
                ) {
                    model.removeLayer(layer, host: host)
                }
            }

            // The conditional is deliberately outside ForEach. Collapsed
            // layers therefore do not instantiate NumberRows, controller
            // geometry probes, or any parameter sub-tree.
            if model.expandedLayerIDs.contains(layer.id) {
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
        // Own the glass surface inside the stable card identity: a Form cell
        // resized by the accordion can lose its row decorator for a frame, so
        // this surface stays mounted while the parameters change.
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
                    model.togglePassExpansion(pass, host: host)
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
                    let count = parameters(pass).count
                    if count > 0 {
                        Text("\(count)")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(panelSecondaryTextColour)
                    }
                    Image(systemName: model.expandedPassIDs.contains(pass.id)
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
                model.togglePassExpansion(pass, host: host)
            }

            if model.expandedPassIDs.contains(pass.id) {
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
        } else if parameters(pass).isEmpty {
            Text(settings.localized("No adjustable parameters"))
                .font(.caption)
                .foregroundStyle(panelSecondaryTextColour)
                .padding(.leading, leadingPadding + 34)
        } else {
            ForEach(parameters(pass)) { param in
                ShaderParameterRow(
                    param: param,
                    pass: pass,
                    params: params,
                    settings: settings,
                    onLivePreviewChange: host.onLivePreviewChange
                )
                    .padding(.leading, leadingPadding)
            }
        }
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

    private func performTouchAction(
        _ feedback: MenuControllerFeedback,
        action: () -> Void
    ) {
        action()
        MenuAudioPackManager.shared.play(feedback)
        controllerInput?.playTouchHaptics(feedback)
    }
}
