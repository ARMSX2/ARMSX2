// RuntimeControllerSkinPicker.swift — the pause menu's per-game controller skin picker
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct RuntimeControllerSkinPicker: View {
    let serial: String
    let identity: PadLayoutGameIdentity?
    let currentSkinID: String
    let gameName: String?
    let controllerInput: MenuControllerInputRouter?
    let feedback: GameplayFeedback
    let onApplied: (VPadSkinDescriptor) -> Void
    let onDismiss: (Bool) -> Void

    @State private var settings = SettingsStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var runtimeControllerSkinProposals: [AutomaticCustomSkinProposal]
    @State private var runtimeControllerSkinSelectionIndex: Int
    @State private var runtimeControllerSkinSetsLayout: Bool
    @State private var runtimeControllerSkinCatalogTask: Task<Void, Never>?

    init(
        serial: String,
        identity: PadLayoutGameIdentity?,
        currentSkinID: String,
        gameName: String?,
        controllerInput: MenuControllerInputRouter?,
        feedback: GameplayFeedback,
        onApplied: @escaping (VPadSkinDescriptor) -> Void,
        onDismiss: @escaping (Bool) -> Void
    ) {
        self.serial = serial
        self.identity = identity
        self.currentSkinID = currentSkinID
        self.gameName = gameName
        self.controllerInput = controllerInput
        self.feedback = feedback
        self.onApplied = onApplied
        self.onDismiss = onDismiss
        // The view opens this only when the list is not empty, so the first frame has a skin.
        let proposals = AutomaticCustomSkinManager.shared.installedProposals(forSerial: serial)
        let index = proposals.firstIndex { $0.skinID == currentSkinID } ?? 0
        _runtimeControllerSkinProposals = State(initialValue: proposals)
        _runtimeControllerSkinSelectionIndex = State(initialValue: index)
        _runtimeControllerSkinSetsLayout = State(initialValue: proposals.indices.contains(index) && proposals[index].shouldApplyCustomLayout)
    }

    var body: some View {
        runtimeControllerSkinPicker
            .onAppear { refreshCatalogProposals() }
            .onDisappear { runtimeControllerSkinCatalogTask?.cancel() }
    }

    @ViewBuilder
    private var runtimeControllerSkinPicker: some View {
        if let proposal = selectedRuntimeControllerSkinProposal {
            let usesDefaultText = settings.controllerUIThemePreset == .defaultTheme
            let targetOrder = [
                "alert.detail.skin",
                "alert.detail.setCustomLayout",
                "alert.action.apply",
                "alert.action.cancel",
            ]

            ControllerNavigationAlert(
                title: settings.localized("Set New Per-Game Custom Skin?"),
                titleColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
                contentColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
                secondaryContentColour: usesDefaultText ? .white : settings.controllerContextMenuSecondaryColor,
                detailValueColour: usesDefaultText
                    ? settings.controllerNavigationAccentColor
                    : nil,
                previewImageURL: AutomaticCustomSkinManager.shared
                    .previewURL(for: proposal),
                previewSkinDescriptor: VPadSkinLibraryStore.shared
                    .descriptor(id: proposal.skinID),
                previewAccessibilityLabel:
                    "\(settings.localized("Preview")): \(settings.localized(proposal.skinName))",
                details: runtimeControllerSkinDetails(proposal: proposal),
                emphasizesPreview: true,
                usesClearGlass: false,
                usesLargeLandscapePanel: true,
                usesCompactPreviewPanel: true,
                showsActionButtonBackgrounds: true,
                dimsBackground: false,
                message: settings.localized(
                    "Choose the skin and optional linked controller layout for this game."
                ),
                actions: [
                    .init(id: "apply", title: settings.localized("Apply")),
                    .init(
                        id: "cancel",
                        title: settings.localized("Cancel"),
                        activationFeedback: .back
                    ),
                ],
                selectedIndex: 0,
                onSelect: { index in
                    if index == 0 {
                        applyRuntimeControllerSkinSelection()
                    } else {
                        dismissRuntimeControllerSkinPicker(playsSound: false)
                    }
                },
                onDismiss: {
                    dismissRuntimeControllerSkinPicker(playsSound: true)
                }
            )
            .controllerAccessibilityTargetOrder(targetOrder)
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                scopeKey: "runtime.controller-skin",
                priority: 360,
                orbStyle: .plain,
                onBack: {
                    dismissRuntimeControllerSkinPicker(playsSound: true)
                    return true
                },
                directionalLinks: Self.runtimeControllerSkinActionLinks,
                prioritizesDirectionalLinks: true,
                confinesHorizontalFocusMovement: true,
                usesExplicitTargetGeometryOnly: true,
                preferredInitialFocusLabel: "alert.detail.skin",
                declaredTargetOrder: targetOrder
            )
            .environment(\.clearLiquidGlassUIEnabled, false)
            .contextMenuPanelTextAppearance()
        }
    }

    // Apply and Cancel sit side by side, so Left and Right move between them.
    private static let runtimeControllerSkinActionLinks: [ControllerAccessibilityDirectionalLink] = [
        .init(fromLabel: "alert.action.apply", direction: .right, toLabel: "alert.action.cancel"),
        .init(fromLabel: "alert.action.cancel", direction: .left, toLabel: "alert.action.apply"),
        .init(fromLabel: "alert.action.cancel", direction: .up, toLabel: "alert.detail.setCustomLayout"),
        .init(
            fromLabel: "alert.action.apply",
            direction: .down,
            toLabel: ControllerAccessibilityDirectionalLink.navigationBoundary
        ),
        .init(
            fromLabel: "alert.action.cancel",
            direction: .down,
            toLabel: ControllerAccessibilityDirectionalLink.navigationBoundary
        ),
    ]

    private var selectedRuntimeControllerSkinProposal:
        AutomaticCustomSkinProposal? {
        guard runtimeControllerSkinProposals.indices.contains(
            runtimeControllerSkinSelectionIndex
        ) else {
            return nil
        }
        return runtimeControllerSkinProposals[
            runtimeControllerSkinSelectionIndex
        ]
    }

    private func runtimeControllerSkinDetails(
        proposal: AutomaticCustomSkinProposal
    ) -> [ControllerNavigationAlertDetail] {
        [
            .init(
                id: "skin",
                label: settings.localized("Skin"),
                value: settings.localized(proposal.skinName),
                onPrevious: { selectRuntimeControllerSkin(offset: -1) },
                onNext: { selectRuntimeControllerSkin(offset: 1) },
                onActivate: { selectRuntimeControllerSkin(offset: 1) }
            ),
            .init(
                id: "game",
                label: settings.localized("Game"),
                value: gameName.map {
                    ($0 as NSString).deletingPathExtension
                } ?? settings.localized("Current Game")
            ),
            .init(
                id: "setCustomLayout",
                label: settings.localized("Set custom layout"),
                value: runtimeControllerSkinSetsLayout
                    ? settings.localized("On")
                    : settings.localized("Off"),
                isOn: runtimeControllerSkinSetsLayout,
                onPrevious: { setRuntimeControllerSkinLayout(false) },
                onNext: { setRuntimeControllerSkinLayout(true) },
                onActivate: {
                    setRuntimeControllerSkinLayout(
                        !runtimeControllerSkinSetsLayout
                    )
                }
            ),
        ]
    }

    private func refreshCatalogProposals() {
        runtimeControllerSkinCatalogTask?.cancel()
        runtimeControllerSkinCatalogTask = Task { @MainActor in
            let refreshed = await AutomaticCustomSkinManager.shared
                .catalogProposals(forSerial: serial)
            guard !Task.isCancelled,
                  !refreshed.isEmpty else { return }
            let selectedSkinID = selectedRuntimeControllerSkinProposal?.skinID
            runtimeControllerSkinProposals = refreshed
            runtimeControllerSkinSelectionIndex = selectedSkinID.flatMap {
                selectedID in
                refreshed.firstIndex { $0.skinID == selectedID }
            } ?? 0
            runtimeControllerSkinCatalogTask = nil
        }
    }

    private func selectRuntimeControllerSkin(offset: Int) {
        guard runtimeControllerSkinProposals.count > 1 else { return }
        let count = runtimeControllerSkinProposals.count
        runtimeControllerSkinSelectionIndex = (
            runtimeControllerSkinSelectionIndex + offset + count
        ) % count
    }

    private func setRuntimeControllerSkinLayout(_ enabled: Bool) {
        guard runtimeControllerSkinSetsLayout != enabled else { return }
        runtimeControllerSkinSetsLayout = enabled
    }

    private func applyRuntimeControllerSkinSelection() {
        guard let picked = selectedRuntimeControllerSkinProposal,
              picked.isCatalogListing else {
            applyRuntimeControllerSkin(selectedRuntimeControllerSkinProposal)
            return
        }
        // A listed catalog skin downloads now that it is picked.
        runtimeControllerSkinCatalogTask?.cancel()
        runtimeControllerSkinCatalogTask = Task { @MainActor in
            let installed = await AutomaticCustomSkinManager.shared
                .installIfNeeded(picked)
            guard !Task.isCancelled else { return }
            applyRuntimeControllerSkin(installed ?? picked)
        }
    }

    private func applyRuntimeControllerSkin(_ proposal: AutomaticCustomSkinProposal?) {
        guard let proposal,
              let descriptor = skinLibrary.descriptor(id: proposal.skinID)
        else {
            feedback.presentImportantStatusMessage(
                settings.localized("The selected controller skin is unavailable.")
            )
            return
        }

        if runtimeControllerSkinSetsLayout,
           let layoutPresetID = proposal.layoutPresetID {
            if let identity {
                layoutPresets.setPreset(layoutPresetID, for: identity)
            }
            if !serial.isEmpty {
                layoutPresets.setAutomaticAssignment(
                    skinID: descriptor.id,
                    layoutPresetID: layoutPresetID,
                    forSerial: serial,
                    using: skinLibrary
                )
            }
        }

        onApplied(descriptor)
        feedback.presentStatusMessage(
            "\(settings.localized("Controller Skin")): "
                + settings.localized(descriptor.displayName)
        )
        dismissRuntimeControllerSkinPicker(playsSound: false)
    }

    private func dismissRuntimeControllerSkinPicker(playsSound: Bool) {
        releaseControllerSkinPresentationResources()
        onDismiss(playsSound)
    }

    @MainActor
    private func releaseControllerSkinPresentationResources() {
        runtimeControllerSkinCatalogTask?.cancel()
        runtimeControllerSkinCatalogTask = nil
        runtimeControllerSkinProposals.removeAll(keepingCapacity: false)
        runtimeControllerSkinSelectionIndex = 0
        runtimeControllerSkinSetsLayout = false
    }
}
