// MenuThemePresetShortcutOverlay.swift — Main-menu theme shortcut feedback
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Foundation

/// The toast uses the same travel curve as the focus box without adding a
/// second display link or animating the entire theme transaction.
private struct ThemePresetFocusAnimation: CustomAnimation {
    let style: ControllerNavigationFocusTravelStyle

    func animate<V: VectorArithmetic>(
        value: V,
        time: TimeInterval,
        context: inout AnimationContext<V>
    ) -> V? {
        guard style.duration > 0, time < style.duration else { return nil }
        return value.scaled(by: style.progress(time / style.duration))
    }
}

struct MenuThemePresetShortcutOverlay: View {
    let controllerInput: MenuControllerInputRouter
    let isEnabled: Bool

    private struct Presentation: Equatable {
        let sequence: UInt64
        let title: String
        let step: Int
    }

    private struct PendingChange: Equatable {
        let sequence: UInt64
        let selection: AppearanceThemeSelection
        let title: String
        let step: Int
        let canSaveCustomTheme: Bool
    }

    @State private var settings = SettingsStore.shared
    @State private var themeGallery = ThemeGalleryStore.shared
    @State private var presentation: Presentation?
    @State private var pendingChange: PendingChange?
    @State private var selectedActionIndex = 0
    @State private var showsThemeNameKeyboard = false
    @State private var themeSaveError = false
    @AppStorage("ARMSX2iOSSkipThemeShortcutConfirmation")
    private var skipsThemeShortcutConfirmation = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var focusAnimation: Animation? {
        guard !reduceMotion,
              settings.controllerNavigationFocusAnimation != .immediate else { return nil }
        return Animation(ThemePresetFocusAnimation(
            style: settings.controllerNavigationFocusAnimation
        ))
    }

    var body: some View {
        ZStack {
            if let pendingChange {
                ControllerNavigationAlert(
                    title: settings.localized("Change Theme?"),
                    usesClearGlass: false,
                    showsActionButtonBackgrounds: true,
                    dimsBackground: false,
                    message: String(
                        format: settings.localized("Change to %@?"),
                        settings.localized(pendingChange.title)
                    ),
                    actions: confirmationActions(for: pendingChange),
                    selectedIndex: selectedActionIndex,
                    onSelect: performConfirmationAction,
                    onDismiss: { cancelPendingChange() }
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
            } else if let presentation {
                HStack(spacing: 12) {
                    Image(systemName: "paintpalette.fill")
                        .font(.title2)
                        .foregroundStyle(.white)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(settings.localized("Theme Preset"))
                            .font(.caption2.weight(.medium))
                            .foregroundStyle(.white.opacity(0.8))
                        Text(settings.localized(presentation.title))
                            .font(.headline)
                            .foregroundStyle(.white)
                            .lineLimit(2)
                            .id(presentation.title)
                            .transition(.opacity.combined(with: .offset(
                                x: presentation.step > 0 ? 14 : -14
                            )))
                    }
                    Image(systemName: presentation.step > 0 ? "chevron.right" : "chevron.left")
                        .font(.caption.weight(.bold))
                        .foregroundStyle(.white.opacity(0.8))
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
                .glassSurface(
                    tint: settings.controllerNavigationAccentColor.opacity(0.18),
                    clear: true,
                    forceClear: true,
                    materializeTransition: true,
                    cornerRadius: 30
                )
                .overlay {
                    ControllerFocusBox(cornerRadius: 30, performanceOptimized: true)
                }
                .shadow(color: .black.opacity(0.2), radius: 12, y: 5)
                .id(presentation.sequence)
                .transition(
                    .opacity.animation(.easeInOut(duration: 0.24))
                )
            }
        }
        .environment(\.colorScheme, .dark)
        .allowsHitTesting(pendingChange != nil)
        .accessibilityElement(children: .combine)
        .animation(focusAnimation, value: presentation)
        .onChange(of: controllerInput.latestThemePresetRequest) { _, request in
            guard let request, isEnabled, controllerInput.canChangeMainMenuTheme else { return }
            let selections = themeGallery.orderedSelections
            let current = themeGallery.currentSelection(for: settings)
            guard !selections.isEmpty,
                  let index = selections.firstIndex(of: current) else { return }
            let count = selections.count
            let nextIndex = (index + (request.step % count) + count) % count
            let selection = selections[nextIndex]
            let title = themeGallery.title(for: selection)
            let pending = PendingChange(
                sequence: request.sequence,
                selection: selection,
                title: title,
                step: request.step,
                canSaveCustomTheme: themeGallery.hasUnsavedCustomTheme(settings)
            )
            if skipsThemeShortcutConfirmation {
                apply(pending)
                return
            }
            selectedActionIndex = 0
            pendingChange = pending
            controllerInput.setNavigationCaptured(
                true,
                owner: MenuControllerNavigationCaptureOwner.themePresetShortcut,
                priority: 400
            )
            controllerInput.playFeedback(.contextMenu)
        }
        .onChange(of: controllerInput.latestEvent) { _, event in
            guard let event,
                  event.captureOwner
                    == MenuControllerNavigationCaptureOwner.themePresetShortcut,
                  pendingChange != nil,
                  !showsThemeNameKeyboard else { return }
            handleControllerCommand(event.command)
        }
        .task(id: presentation?.sequence) {
            guard presentation != nil else { return }
            do {
                try await Task.sleep(for: .seconds(2))
                presentation = nil
            } catch { }
        }
        .onChange(of: isEnabled) { _, enabled in
            if !enabled {
                presentation = nil
                cancelPendingChange(playsFeedback: false)
            }
        }
        .fullScreenCover(isPresented: $showsThemeNameKeyboard) {
            OrbitKeysKeyboardView(
                title: "Save Custom Theme",
                initialText: "",
                startsInNormalKeyboard: true,
                onCommit: saveCustomTheme,
                onCancel: {
                    showsThemeNameKeyboard = false
                }
            )
            .presentationBackground(.clear)
        }
        .alert("Theme Not Saved", isPresented: $themeSaveError) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Enter a name for your theme.")
        }
        .onDisappear {
            controllerInput.setNavigationCaptured(
                false,
                owner: MenuControllerNavigationCaptureOwner.themePresetShortcut,
                priority: 400
            )
        }
    }

    private func confirmationActions(
        for pending: PendingChange
    ) -> [ControllerNavigationAlertAction] {
        var actions = [
            ControllerNavigationAlertAction(
                id: "cancel",
                title: settings.localized("Cancel")
            ),
            ControllerNavigationAlertAction(
                id: "ok",
                title: settings.localized("OK"),
                activationFeedback: .tabTransition
            ),
            ControllerNavigationAlertAction(
                id: "do-not-show-again",
                title: settings.localized("Do Not Show Again"),
                activationFeedback: .tabTransition
            ),
        ]
        if pending.canSaveCustomTheme {
            actions.append(
                ControllerNavigationAlertAction(
                    id: "save-custom-theme",
                    title: settings.localized("Save Custom Theme"),
                    activationFeedback: .destination
                )
            )
        }
        return actions
    }

    private func performConfirmationAction(_ index: Int) {
        guard let pendingChange else { return }
        let actions = confirmationActions(for: pendingChange)
        guard actions.indices.contains(index) else { return }
        switch actions[index].id {
        case "ok":
            applyPendingChange()
        case "do-not-show-again":
            skipsThemeShortcutConfirmation = true
            applyPendingChange()
        case "save-custom-theme":
            showsThemeNameKeyboard = true
        default:
            cancelPendingChange()
        }
    }

    private func applyPendingChange() {
        guard let pendingChange else { return }
        apply(pendingChange)
    }

    private func apply(_ change: PendingChange) {
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            themeGallery.apply(change.selection, to: settings)
        }
        presentation = Presentation(
            sequence: change.sequence,
            title: change.title,
            step: change.step
        )
        self.pendingChange = nil
        selectedActionIndex = 0
        controllerInput.setNavigationCaptured(
            false,
            owner: MenuControllerNavigationCaptureOwner.themePresetShortcut,
            priority: 400
        )
        controllerInput.playFeedback(.tabTransition)
    }

    private func cancelPendingChange(playsFeedback: Bool = true) {
        guard pendingChange != nil else { return }
        pendingChange = nil
        selectedActionIndex = 0
        showsThemeNameKeyboard = false
        controllerInput.setNavigationCaptured(
            false,
            owner: MenuControllerNavigationCaptureOwner.themePresetShortcut,
            priority: 400
        )
        if playsFeedback {
            controllerInput.playFeedback(.back)
        }
    }

    private func saveCustomTheme(_ name: String) {
        guard pendingChange != nil else {
            showsThemeNameKeyboard = false
            return
        }
        guard ThemeGalleryStore.shared.save(name: name, settings: settings) else {
            themeSaveError = true
            return
        }
        showsThemeNameKeyboard = false
        applyPendingChange()
    }

    private func handleControllerCommand(_ command: MenuControllerCommand) {
        guard let pendingChange else { return }
        let actions = confirmationActions(for: pendingChange)
        switch command {
        case .up, .upLeft, .left, .downLeft:
            let next = max(0, selectedActionIndex - 1)
            guard next != selectedActionIndex else {
                controllerInput.playFeedback(.boundary)
                return
            }
            selectedActionIndex = next
            controllerInput.playFeedback(.move(.up))
        case .down, .downRight, .right, .upRight:
            let next = min(actions.count - 1, selectedActionIndex + 1)
            guard next != selectedActionIndex else {
                controllerInput.playFeedback(.boundary)
                return
            }
            selectedActionIndex = next
            controllerInput.playFeedback(.move(.down))
        case .activate:
            performConfirmationAction(selectedActionIndex)
        case .back:
            cancelPendingChange()
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            controllerInput.playFeedback(.boundary)
        }
    }
}
