// PerGameSettingsPanel.swift - Per-game overrides panel
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import UIKit

struct PerGameSettingsPanel: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiContextMenuColour) private var panelTextColour
    @Environment(\.uiContextMenuSecondaryColour) private var panelSecondaryTextColour
    @State private var settings = SettingsStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared

    private struct SaveButtonStyle: ButtonStyle {
        let compact: Bool
        let confirmsSave: Bool
        @Environment(\.isEnabled) private var isEnabled
        @Environment(\.uiAccentColour) private var accentColour

        func makeBody(configuration: Configuration) -> some View {
            configuration.label
                .font(.body.weight(.semibold))
                // Own both colours: inherited theme text and the native
                // disabled prominent style can make this action look black.
                // Save remains visibly disabled after committing.
                .foregroundStyle(
                    Color.white.opacity(isEnabled || confirmsSave ? 1 : 0.6)
                )
                .padding(.horizontal, 18)
                .frame(minHeight: compact ? 36 : 44)
                .background(
                    (confirmsSave ? Color.green : accentColour)
                        .opacity(isEnabled || confirmsSave ? 1 : 0.28),
                    in: Capsule()
                )
                .opacity(configuration.isPressed ? 0.75 : 1)
                .animation(.easeOut(duration: 0.18), value: confirmsSave)
        }
    }

    let game: ISOEntry
    let controllerInput: MenuControllerInputRouter?
    let onDone: (() -> Void)?
    private let initiallySelectsGameController: Bool
    private let initiallySelectsShaders: Bool

    /// Zero in the library, where this is a sheet and the system does its own avoidance.
    @Environment(\.overlayKeyboardOverlap) private var keyboardOverlap

    @State private var draft: PerGameSettingsDraft
    @State private var livePreview: PerGameLivePreviewSession
    @State private var controller: PerGameSettingsControllerCoordinator
    @State private var showPadLayoutEditor = false
    @State private var shaderPresetRequest: ShaderPresetBrowserRequest?
    @State private var showCheatsManager = false
    @State private var saveFeedbackVisible = false
    @State private var shaderWorkspaceCategoryID = "shaders"

    private let livePreviewDismissRequest: UInt64

    init(
        game: ISOEntry,
        preloadedSettings: [String: Any]? = nil,
        savesToRunningGame: Bool = false,
        initiallySelectsGameController: Bool = false,
        initiallySelectsShaders: Bool = false,
        controllerInput: MenuControllerInputRouter? = nil,
        livePreviewDismissRequest: UInt64 = 0,
        onLivePreviewPresentationChange:
            ((PerGameLivePreviewPresentation) -> Void)? = nil,
        onLivePreviewStatusChange:
            ((PerGameLivePreviewStatus) -> Void)? = nil,
        onDone: (() -> Void)? = nil
    ) {
        self.game = game
        self.controllerInput = controllerInput
        self.onDone = onDone
        self.initiallySelectsGameController = initiallySelectsGameController
        self.initiallySelectsShaders = initiallySelectsShaders
        self.livePreviewDismissRequest = livePreviewDismissRequest
        _draft = State(initialValue: PerGameSettingsDraft(
            game: game,
            preloadedSettings: preloadedSettings,
            savesToRunningGame: savesToRunningGame,
            initiallySelectsGameController: initiallySelectsGameController,
            initiallySelectsShaders: initiallySelectsShaders
        ))
        _livePreview = State(initialValue: PerGameLivePreviewSession(
            draft: _draft.wrappedValue,
            onLivePreviewPresentationChange: onLivePreviewPresentationChange,
            onLivePreviewStatusChange: onLivePreviewStatusChange
        ))
        _controller = State(initialValue: PerGameSettingsControllerCoordinator(
            draft: _draft.wrappedValue,
            initiallySelectsGameController: initiallySelectsGameController,
            initiallySelectsShaders: initiallySelectsShaders
        ))
    }

    /// Turns the master toggle off and saves, which deletes every per-game key. The Virtual Pad
    /// layout, skin and stick inversion are saved outside that path, so they are cleared here.
    private func resetAllOverrides() {
        if let padLayoutIdentity = draft.padLayoutIdentity {
            layoutPresets.clearVPadOverrides(for: padLayoutIdentity)
        }
        draft.clearStickInversionOverrides()
        draft.enabled = false
        save()
    }

    var body: some View {
        OverlayPanelScaffold(usesRegularGlass: true) {
            GeometryReader { geo in
                // Rail and detail pane on a wide card, NavigationStack form on a tall one.
                let layout = PerGameSettingsLayout(size: geo.size)
                VStack(spacing: 0) {
                    settingsContent(
                        useCompactLayout: layout.usesSplitView,
                        availableWidth: geo.size.width
                    )
                        .frame(maxHeight: .infinity)
                    saveCancelFooter(compact: layout.usesSplitView)
                }
            // Inside the reader, so a keyboard cannot flip the layout picked above.
            .safeAreaInset(edge: .bottom, spacing: 0) {
                Color.clear.frame(height: keyboardOverlap)
            }
            // Portrait and landscape are different focus graphs, so a rotation rebuilds the probes
            // and the navigation scope at once and Cross cannot activate a control from the old tree.
            .id(layout)
            .onChange(of: layout) { _, newLayout in
                guard AppState.shared.hingePresent else {
                    controller.selectedControllerDetail = nil
                    return
                }
                // The split always shows openCategory, so a fold's stack pushes it again.
                guard !initiallySelectsShaders else { return }
                controller.selectedControllerDetail = nil
                guard !newLayout.usesSplitView else { return }
                controller.pushOpenCategoryAfterLayout()
            }
            .controllerAccessibilityTargetOrder(
                controller.perGameControllerTargetOrder
            )
            // Keep the accessibility graph on the controller confirmation, not the settings under it.
            .accessibilityHidden(
                controller.activeControllerAlertKind != nil
            )
            .environment(
                \.controllerAccessibilityTargetsSuppressed,
                controller.activeControllerAlertKind != nil
            )
            .overlay {
                if let kind = controller.activeControllerAlertKind {
                    ControllerNavigationAlert(
                        title: controller.controllerAlertTitle(for: kind),
                        message: controller.controllerAlertMessage(for: kind),
                        actions: controller.controllerAlertActions(for: kind),
                        selectedIndex: 0,
                        onSelect: { index in
                            performControllerAlertAction(index, for: kind)
                        },
                        onDismiss: {
                            controller.dismissControllerAlert(kind)
                        }
                    )
                    .zIndex(20_000)
                }
            }
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                isActive: !showCheatsManager
                    && !showPadLayoutEditor
                    && shaderPresetRequest == nil
                    && !livePreview.livePreviewSessionIsFinishing,
                scopeKey: controller.perGameControllerScopeKey(layout: layout),
                priority: 220,
                orbStyle: .plain,
                onBack: handleSharedControllerBack,
                directionalLinks: controller.perGameControllerDirectionalLinks,
                prioritizesDirectionalLinks: true,
                confinesHorizontalFocusMovement: true,
                // Per-Game rows all publish stable semantic probes. Keeping
                // this surface explicit-only prevents a nested native control
                // from becoming a second focus owner for the same setting.
                usesExplicitTargetGeometryOnly: true,
                preservesFocusDuringRightStickScrolling: false,
                focusScrollBehavior: .maintainWithinViewport,
                scrollAnimationDuration: 0.16,
                focusTopAlignmentMargin: controller.controllerColumn == .detail ? 84 : 24,
                focusBottomAlignmentMargin: controller.controllerColumn == .detail ? 84 : 24,
                onAdjustFocusedTarget: { _, value, increases in
                    livePreview.publishLivePreviewStatus(
                        value: value,
                        countsDown: !increases
                    )
                },
                preferredInitialFocusLabel:
                    controller.perGamePreferredInitialFocusLabel,
                preferredTrailingFocusLabels: [
                    PerGameSettingsControllerCoordinator.closeControllerTargetID,
                    PerGameSettingsControllerCoordinator.saveControllerTargetID,
                ]
            )
            .task(id: controller.perGameControllerScopeKey(layout: layout)) {
                guard controllerInput?.hasConnectedController == true,
                      controllerInput?.isControllerNavigationEnabled == true else {
                    return
                }
                // Registering the scope is passive; enter the new column once its first row has mounted.
                await Task.yield()
                guard !Task.isCancelled else { return }
                _ = controllerInput?.requestNavigationSessionEntry(
                    preferLast: false,
                    matchingScopePrefix: controller.perGameControllerScopeKey(
                        layout: layout
                    )
                )
            }
            }
        }
        .environment(
            \.clearLiquidGlassUIEnabled,
            false
        )
        .environment(\.menuControllerInputRouter, controllerInput)
        // Embedded portrait pages need a local scheme; the gameplay scene can override a preference.
        .environment(\.colorScheme, .dark)
        .contextMenuPanelTextAppearance()
        .environment(\.perGameOverrideWillActivate) {
            livePreview.suppressNextLivePreviewChange = true
        }
        .allowsHitTesting(!livePreview.livePreviewSessionIsFinishing)
        .tint(accentColour)
        .accessibilityElement(children: .contain)
        .accessibilityAction(.escape) { _ = handleSharedControllerBack() }
        .onChange(of: draft.hasPendingChanges) { _, pending in
            if pending {
                saveFeedbackVisible = false
            }
        }
        .onChange(of: controller.activeControllerAlertKind) { previous, kind in
            guard let kind, kind != previous else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .onAppear {
            livePreview.beginLivePreviewSessionIfNeeded()
            if initiallySelectsGameController { controller.pushOpenCategoryAfterLayout() }
        }
        .onChange(of: draft.perGameLivePreviewFingerprint()) { _, fingerprint in
            livePreview.fingerprintChanged(fingerprint)
        }
        .onChange(of: livePreviewDismissRequest) { _, _ in
            livePreview.dismissRequested()
        }
        .onChange(of: settings.appearance.temporalSaveStateToLivePreviewChanges) {
            _, enabled in
            livePreview.temporalPreviewSettingChanged(enabled)
        }
        .onDisappear {
            livePreview.panelDisappeared()
        }
        .interactiveDismissDisabled(draft.hasPendingChanges)
        .fullScreenCover(isPresented: $showPadLayoutEditor) {
            PadLayoutEditView(
                onDismiss: { showPadLayoutEditor = false },
                context: perGamePadLayoutEditorContext
            )
        }
        .sheet(item: $shaderPresetRequest, onDismiss: {
            if !draft.perGameShaderPresetRef.isEmpty, ShaderPresetLibrary.resolve(draft.perGameShaderPresetRef) == nil {
                draft.perGameShaderPresetRef = ""
            }
        }) { _ in
            NavigationStack {
                ShaderPresetBrowserView(
                    title: settings.localized("Shader Presets"),
                    folder: nil,
                    selectedToken: draft.perGameShaderPresetRef,
                    localized: { settings.localized($0) },
                    onSelect: { token in
                        draft.perGameShaderPresetRef = token
                        shaderPresetRequest = nil
                        ARMSX2Bridge.retryShaderChain()
                    },
                    controllerInput: controllerInput,
                    onClose: { shaderPresetRequest = nil }
                )
            }
        }
        .fullScreenCover(isPresented: $showCheatsManager) {
            CheatsPatchesManagerView(
                isoName: game.bootName,
                gameTitle: game.name,
                launchContext: draft.savesToRunningGame ? .inGame : .library,
                controllerInput: controllerInput
            )
            .appStatusBarHidden()
        }
        .confirmationDialog(settings.localized("Reset all per-game overrides?"),
                            isPresented: nativeControllerDialogBinding($controller.showResetAllConfirmation),
                            titleVisibility: .visible) {
            Button(settings.localized("Reset All"), role: .destructive) {
                resetAllOverrides()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("All per-game overrides for this title are removed; global settings apply on the next boot or reset."))
        }
        .confirmationDialog(settings.localized("Discard your changes?"),
                            isPresented: nativeControllerDialogBinding($controller.showDiscardConfirmation),
                            titleVisibility: .visible) {
            Button(settings.localized("Apply Changes")) {
                applyChangesAndDismiss()
            }
            Button(settings.localized("Discard Changes"), role: .destructive) {
                discardChangesAndDismiss()
            }
            Button(settings.localized("Keep Editing"), role: .cancel) {}
        } message: {
            Text(settings.localized("You have unsaved per-game settings changes."))
        }
        .confirmationDialog(settings.localized("Clear Per-Game Frame Pacing?"),
                            isPresented: nativeControllerDialogBinding($controller.showFramePacingResetConfirmation),
            titleVisibility: .visible) {
            Button(settings.localized("Clear"), role: .destructive) {
                draft.clearFramePacingOverrides()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("This removes your overrides for this game. It will use your global Frame Pacing settings."))
        }
    }

    @ViewBuilder
    private func settingsContent(useCompactLayout: Bool, availableWidth: CGFloat = 0) -> some View {
        if initiallySelectsShaders {
            shaderWorkspaceContent
        } else if useCompactLayout {
            landscapeSettingsSplit(availableWidth: availableWidth)
        } else {
            NavigationStack(path: controller.portraitNavigationPath) {
                PerGameCategoryList(
                    controller: controller,
                    controllerInput: controllerInput,
                    showsRail: false
                )
                    .environment(
                        \.controllerAccessibilityTargetsSuppressed,
                        controller.controllerColumn == .detail
                            || controller.controllerAlertSuppressesContentTargets
                    )
                    .navigationDestination(for: PerGameSettingsCategory.self) { category in
                        categoryDetail(category)
                            // A pushed portrait Form can put an opaque or empty navigation backing over
                            // the panel's glass, so the page gets its own regular Liquid Glass layer.
                            .background {
                                Color.clear
                                    .glassSurface(
                                        clear: false,
                                        cornerRadius: 0
                                    )
                                    .ignoresSafeArea()
                            }
                            .navigationTitle(
                                settings.localized(
                                    initiallySelectsShaders
                                        && category == .graphics
                                        ? "Shaders"
                                        : category.titleKey
                                )
                            )
                            .navigationBarTitleDisplayMode(.inline)
                            .navigationBarBackButtonHidden(true)
                            .toolbar {
                                ToolbarItem(placement: .topBarLeading) {
                                    perGameDetailBackButton
                                }
                            }
                            .environment(
                                \.controllerAccessibilityTargetsSuppressed,
                                controller.controllerColumn == .categories
                                    || controller.controllerAlertSuppressesContentTargets
                            )
                    }
                    .navigationTitle(settings.localized(panelTitleKey))
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbarBackground(.hidden, for: .navigationBar)
            }
            // The destination Forms are transparent. Without clearing the
            // NavigationStack container, UIKit inserts a pale backing view
            // during a portrait push and it reads as a half-white tint.
            .clearNavigationContainerBackground()
            .background(Color.clear)
        }
    }

    private var shaderWorkspaceContent: some View {
        VStack(spacing: 0) {
            landscapeHeader
            OverlayTheme.separator.frame(height: 0.5)
            shaderWorkspace
        }
    }

    private var shaderWorkspace: some View {
        ShaderWorkspaceView(
            enabled: $settings.shaderChainEnabled,
            presetRef: $draft.perGameShaderPresetRef,
            perGameChain: $draft.perGameShaderChain,
            settings: settings,
            controllerInput: controllerInput,
            savesToRunningGame: draft.savesToRunningGame,
            onLivePreviewChange: requestShaderLivePreview,
            selectedCategoryID: $shaderWorkspaceCategoryID,
            detailPresented: shaderWorkspaceDetailPresented,
            reportedControllerTargetOrder:
                $controller.shaderWorkspaceControllerTargetOrder
        )
    }

    private var shaderWorkspaceDetailPresented: Binding<Bool> {
        Binding(
            get: { controller.selectedControllerDetail != nil },
            set: { presented in
                controller.selectedControllerDetail = presented ? .graphics : nil
            }
        )
    }

    @MainActor
    private func requestShaderLivePreview(_ value: String) {
        draft.shaderParameterPreviewSequence &+= 1
        livePreview.publishLivePreviewStatus(value: value, countsDown: false)
    }

    @ViewBuilder
    private func landscapeSettingsSplit(availableWidth: CGFloat) -> some View {
        // Wide enough for "Fixes & Compatibility" (about 190pt at .callout), clamped for narrow panels.
        let railWidth = min(200, max(168, availableWidth * 0.24))
        VStack(spacing: 0) {
            landscapeHeader
            OverlayTheme.separator.frame(height: 0.5)
            HStack(spacing: 0) {
                PerGameCategoryList(
                    controller: controller,
                    controllerInput: controllerInput,
                    showsRail: true
                )
                    .frame(width: railWidth)
                    .background(Color.clear)
                OverlayTheme.separator.frame(width: 0.5)
                detailPane
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .dynamicTypeSize(...DynamicTypeSize.accessibility3)
    }

    @ViewBuilder
    private var landscapeHeader: some View {
        HStack(spacing: 8) {
            perGameDetailBackButton
            Image(systemName: draft.enabled ? "slider.horizontal.3" : "power")
                .font(.system(size: 18))
                .foregroundStyle(accentColour)
            Text(settings.localized(panelTitleKey))
                .font(.callout.weight(.semibold))
                .foregroundStyle(panelTextColour)
            Text(draft.displayName)
                .font(.caption)
                .foregroundStyle(panelSecondaryTextColour)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: 8)
            if draft.hasPendingChanges {
                Circle()
                    .fill(accentColour)
                    .frame(width: 7, height: 7)
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 6)
        .padding(.bottom, 3)
    }

    private var perGameDetailBackButton: some View {
        Button {
            navigateBack(playsSemanticAudio: true)
        } label: {
            Label(settings.localized("Back"), systemImage: "chevron.left")
                .font(.callout.weight(.semibold))
                .padding(.horizontal, 10)
                .frame(minHeight: 34)
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(settings.localized("Back"))
        .controllerAccessibilityActionTarget(
            id: PerGameSettingsControllerCoordinator.detailBackControllerTargetID,
            label: settings.localized("Back"),
            activationFeedback: .silent
        ) {
            navigateBack(playsSemanticAudio: true)
        }
    }

    private var panelTitleKey: String {
        initiallySelectsShaders ? "Shaders" : "Per-Game Settings"
    }

    @ViewBuilder
    private var detailPane: some View {
        categoryDetail(controller.openCategory)
            .pickerStyle(.menu)
            .environment(
                \.controllerAccessibilityTargetsSuppressed,
                controller.controllerColumn == .categories
                    || controller.controllerAlertSuppressesContentTargets
            )
    }

    private func categoryDetail(_ category: PerGameSettingsCategory) -> some View {
        PerGameCategoryDetail(
            category: category,
            draft: draft,
            controller: controller,
            initiallySelectsShaders: initiallySelectsShaders,
            showPadLayoutEditor: $showPadLayoutEditor,
            showCheatsManager: $showCheatsManager,
            shaderPresetRequest: $shaderPresetRequest
        )
    }

    private func saveCancelFooter(compact: Bool) -> some View {
        HStack(spacing: 12) {
            let closeLabel = settings.localized(
                draft.hasPendingChanges ? "Cancel" : "Close"
            )
            Button(closeLabel) {
                attemptCancel()
            }
            .buttonStyle(.bordered)
            .controlSize(compact ? .regular : .large)
            .frame(maxWidth: .infinity)
            .controllerAccessibilityActionTarget(
                id: PerGameSettingsControllerCoordinator.closeControllerTargetID,
                label: closeLabel
            ) { attemptCancel() }

            Button {
                save()
            } label: {
                Text(
                    settings.localized(
                        saveFeedbackVisible ? "Saved" : "Save"
                    )
                )
            }
            .buttonStyle(
                SaveButtonStyle(
                    compact: compact,
                    confirmsSave: saveFeedbackVisible
                )
            )
            .frame(maxWidth: .infinity)
            .controllerAccessibilityActionTarget(
                id: PerGameSettingsControllerCoordinator.saveControllerTargetID,
                label: settings.localized("Save"),
                action: { save() }
            )
            // Keep the controller probe inside the disabled environment. Save
            // unregisters from the active graph immediately after the saved
            // fingerprint catches up with the edited state.
            .disabled(!draft.hasGameSettingsIdentity || !draft.hasPendingChanges)
        }
        .padding(.horizontal, compact ? 16 : 20)
        .padding(.top, compact ? 6 : 10)
        .padding(.bottom, compact ? 8 : 16)
        .background(Color.clear)
        .overlay(alignment: .top) {
            OverlayTheme.separator
                .frame(height: 0.5)
        }
    }

    /// Asks before discarding unsaved edits. Reads `hasPendingChanges` and changes nothing else.
    private func attemptCancel(playsSemanticAudio: Bool = true) {
        if draft.hasPendingChanges {
            if playsSemanticAudio {
                MenuAudioPackManager.shared.playEvent(.uiToast)
            }
            controller.showDiscardConfirmation = true
        } else {
            if playsSemanticAudio {
                MenuAudioPackManager.shared.playEvent(.return)
            }
            dismissPanel()
        }
    }

    /// Back is the sole transition from detail controls to the selected
    /// category item in both layouts. Left remains reserved for adjustable
    /// values and never changes columns.
    @MainActor
    private func handleSharedControllerBack() -> Bool {
        if let kind = controller.activeControllerAlertKind {
            controller.dismissControllerAlert(kind)
            return true
        }
        if controller.controllerColumn == .detail {
            controller.selectedControllerDetail = nil
            return true
        }
        attemptCancel(playsSemanticAudio: false)
        return true
    }

    private func navigateBack(playsSemanticAudio: Bool) {
        if controller.selectedControllerDetail != nil {
            controller.returnToControllerCategories(playsSemanticAudio: playsSemanticAudio)
        } else {
            attemptCancel(playsSemanticAudio: playsSemanticAudio)
        }
    }

    private func nativeControllerDialogBinding(
        _ source: Binding<Bool>
    ) -> Binding<Bool> {
        Binding(
            get: { false },
            set: { presented in
                if !presented,
                   controllerInput?.hasConnectedController != true {
                    source.wrappedValue = false
                }
            }
        )
    }

    private func performControllerAlertAction(
        _ index: Int,
        for kind: PerGameControllerAlertKind
    ) {
        let actions = controller.controllerAlertActions(for: kind)
        guard actions.indices.contains(index) else { return }
        let actionID = actions[index].id
        guard actionID != "cancel" else {
            controller.dismissControllerAlert(kind)
            return
        }
        controller.dismissControllerAlert(kind)
        switch (kind, actionID) {
        case (.discardChanges, "apply"):
            applyChangesAndDismiss()
        case (.resetAll, "reset"):
            resetAllOverrides()
        case (.discardChanges, "discard"):
            discardChangesAndDismiss()
        case (.clearFramePacing, "clear"):
            draft.clearFramePacingOverrides()
        default:
            break
        }
    }

    private func dismissPanel(
        appliesChanges: Bool = false
    ) {
        guard !livePreview.livePreviewSessionIsFinishing else { return }
        livePreview.livePreviewSessionIsFinishing = true
        livePreview.livePreviewPendingFingerprint = nil
        let previewTask = livePreview.livePreviewTask
        let initialization = livePreview.livePreviewInitialization
        previewTask?.cancel()
        // Keep the paused editor visible while restoring; do not flash gameplay
        // and then remount the editor immediately before dismissing.
        livePreview.publishLivePreviewPresentation(.finishing)

        Task { @MainActor in
            // Cancellation cannot interrupt an in-flight native settings write.
            // Join it before saving so it cannot restore the old INI over Apply.
            await previewTask?.value
            let initializedToken = await initialization?.value
            livePreview.livePreviewInitialization = nil
            livePreview.livePreviewToken = livePreview.livePreviewToken ?? initializedToken
            if appliesChanges {
                save(showsConfirmation: false)
                guard !draft.hasPendingChanges else {
                    livePreview.livePreviewSessionIsFinishing = false
                    livePreview.publishLivePreviewPresentation(.editing)
                    return
                }
                MenuAudioPackManager.shared.playEvent(.return)
            }
            guard draft.savesToRunningGame,
                  let token = livePreview.livePreviewToken else {
                completePanelDismissal()
                return
            }
            _ = await ARMSX2Bridge.finishPerGameLivePreview(token: token)
            livePreview.livePreviewToken = nil
            completePanelDismissal()
        }
    }

    private func completePanelDismissal() {
        if let onDone {
            onDone()
        } else {
            dismiss()
        }
    }

    private func discardChangesAndDismiss() {
        MenuAudioPackManager.shared.playEvent(.return)
        dismissPanel()
    }

    private func applyChangesAndDismiss() {
        dismissPanel(appliesChanges: true)
    }

    private var perGamePadLayoutEditorContext: PadLayoutEditorContext {
        let preset = layoutPresets.effectivePreset(for: draft.padLayoutIdentity)
        let editablePresetID = draft.padLayoutIdentity.flatMap { layoutPresets.presetID(for: $0) }
        return PadLayoutEditorContext(
            presetID: editablePresetID,
            gameIdentity: draft.padLayoutIdentity,
            initialSnapshot: preset?.snapshot,
            skinDescriptor: layoutPresets.effectiveSkinDescriptor(for: draft.padLayoutIdentity, using: skinLibrary)
        )
    }

    private func save(showsConfirmation: Bool = true) {
        livePreview.livePreviewTask?.cancel()
        guard draft.writeCurrentSettings(normalizesEditableValues: true) else {
            return
        }
        if let livePreviewToken = livePreview.livePreviewToken {
            ARMSX2Bridge.refreshPerGameLivePreviewBaseline(token: livePreviewToken, afterSave: true)
        }
        draft.statusMessage = draft.postSaveMessage
        draft.savedFingerprint = draft.perGameFingerprint()
        livePreview.lastPreviewedFingerprint = draft.perGameLivePreviewFingerprint()
        if showsConfirmation {
            showSaveConfirmation()
        }
    }

    @MainActor
    private func showSaveConfirmation() {
        UINotificationFeedbackGenerator().notificationOccurred(.success)
        withAnimation(.easeOut(duration: 0.18)) {
            saveFeedbackVisible = true
        }
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(1.4))
            withAnimation(.easeOut(duration: 0.24)) {
                saveFeedbackVisible = false
            }
        }
    }

}
