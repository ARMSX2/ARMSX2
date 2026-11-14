// PerGameSettingsControllerCoordinator.swift — the per-game panel's controller focus graph
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

enum PerGameControllerAlertKind: Equatable {
    case resetAll
    case discardChanges
    case clearFramePacing
}

enum PerGameSettingsCategory: String, CaseIterable, Identifiable, Hashable {
    case general, graphics, textures, framePacing, audio, cpu, gameController, pad, fixes, cheats, retroAchievements

    var id: Self { self }

    var titleKey: String {
        switch self {
        case .general: return "General"
        case .graphics: return "Graphics"
        case .textures: return "Texture Packs"
        case .framePacing: return "Frame Pacing"
        case .audio: return "Audio"
        case .cpu: return "CPU & Speedhacks"
        case .gameController: return "Game Controller"
        case .pad: return "Virtual Pad"
        case .fixes: return "Fixes & Compatibility"
        case .cheats: return "Cheats & Patches"
        case .retroAchievements: return "RetroAchievements"
        }
    }

    var systemImage: String {
        switch self {
        case .general: return "slider.horizontal.3"
        case .graphics: return "paintbrush"
        case .textures: return "photo.stack"
        case .framePacing: return "speedometer"
        case .audio: return "speaker.wave.2"
        case .cpu: return "cpu"
        case .gameController: return "gamecontroller"
        case .pad: return "hand.tap"
        case .fixes: return "wrench.and.screwdriver"
        case .cheats: return "rectangle.stack.badge.plus"
        case .retroAchievements: return "trophy"
        }
    }
}

enum PerGameSettingsLayout: String, Hashable {
    case portrait
    case landscape

    init(size: CGSize) {
        self = size.width > size.height ? .landscape : .portrait
    }

    var usesSplitView: Bool { self == .landscape }
}

enum PerGameControllerColumn: String {
    case categories
    case detail
}

@MainActor
@Observable
final class PerGameSettingsControllerCoordinator {
    static let closeControllerTargetID = "per-game.footer.close"
    static let saveControllerTargetID = "per-game.footer.save"
    static let detailBackControllerTargetID = "per-game.detail.back"

    let draft: PerGameSettingsDraft
    let initiallySelectsShaders: Bool
    let settings = SettingsStore.shared
    let layoutPresets = PadLayoutPresetStore.shared
    let skinLibrary = VPadSkinLibraryStore.shared
    var textureControllerTargets: [String] = []
    var showResetAllConfirmation = false
    var showDiscardConfirmation = false
    var showFramePacingResetConfirmation = false
    var openCategory: PerGameSettingsCategory = .general
    var selectedControllerDetail: PerGameSettingsCategory?
    var shaderWorkspaceControllerTargetOrder: [String] = []

    init(
        draft: PerGameSettingsDraft,
        initiallySelectsGameController: Bool,
        initiallySelectsShaders: Bool
    ) {
        self.draft = draft
        self.initiallySelectsShaders = initiallySelectsShaders
        openCategory = initiallySelectsGameController
                ? .gameController
                : (initiallySelectsShaders ? .graphics : .general)
    }

    private var game: ISOEntry { draft.game }

    var activeControllerAlertKind: PerGameControllerAlertKind? {
        if showResetAllConfirmation { return .resetAll }
        if showDiscardConfirmation { return .discardChanges }
        if showFramePacingResetConfirmation { return .clearFramePacing }
        return nil
    }

    var controllerAlertSuppressesContentTargets: Bool {
        activeControllerAlertKind != nil
    }

    func perGameControllerScopeKey(
        layout: PerGameSettingsLayout
    ) -> String {
        let alert: String
        switch activeControllerAlertKind {
        case .resetAll: alert = "reset-alert"
        case .discardChanges: alert = "discard-alert"
        case .clearFramePacing: alert = "frame-pacing-alert"
        case nil: alert = "content"
        }
        return "per-game.\(game.bootName).\(layout.rawValue).\(controllerScopeComponent).\(alert)"
    }

    private var controllerScopeComponent: String {
        selectedControllerDetail.map { "detail.\($0.rawValue)" }
            ?? "categories"
    }

    func landscapeCategoryControllerTargetID(
        _ category: PerGameSettingsCategory
    ) -> String {
        "per-game.category.\(category.rawValue)"
    }

    func enterControllerDetail(_ category: PerGameSettingsCategory) {
        openCategory = category
        selectedControllerDetail = category
    }

    func returnToControllerCategories(playsSemanticAudio: Bool) {
        guard selectedControllerDetail != nil else { return }
        if playsSemanticAudio {
            MenuAudioPackManager.shared.playEvent(.return)
        }
        selectedControllerDetail = nil
    }

    private var activePerGameContentOrder: [String] {
        if initiallySelectsShaders {
            return shaderWorkspaceControllerTargetOrder
        }
        if controllerColumn == .categories {
            return PerGameSettingsCategory.allCases.map {
                landscapeCategoryControllerTargetID($0)
            }
        }
        return perGameDetailControllerTargetOrder(
            selectedControllerDetail ?? openCategory
        )
    }

    var perGameControllerTargetOrder: [String] {
        [Self.detailBackControllerTargetID] + activePerGameContentOrder + [
            Self.closeControllerTargetID,
            Self.saveControllerTargetID,
        ]
    }

    /// Per-Game Settings is visually a vertical content column followed by a
    /// two-button horizontal footer. Declare that geometry semantically so the
    /// linear fallback cannot reinterpret Close/Save as another vertical row.
    var perGameControllerDirectionalLinks:
        [ControllerAccessibilityDirectionalLink] {
        let contentOrder = activePerGameContentOrder
        let entryTarget = contentOrder.first ?? Self.closeControllerTargetID
        let trailingContent = contentOrder.last
            ?? (controllerColumn == .detail
                ? Self.detailBackControllerTargetID
                : Self.closeControllerTargetID)

        var links: [ControllerAccessibilityDirectionalLink] = [
            .init(
                fromLabel: trailingContent,
                direction: .down,
                toLabel: Self.closeControllerTargetID
            ),
            .init(
                fromLabel: Self.closeControllerTargetID,
                direction: .up,
                toLabel: trailingContent
            ),
            .init(
                fromLabel: Self.saveControllerTargetID,
                direction: .up,
                toLabel: trailingContent
            ),
            .init(
                fromLabel: Self.closeControllerTargetID,
                direction: .right,
                toLabel: Self.saveControllerTargetID
            ),
            .init(
                fromLabel: Self.saveControllerTargetID,
                direction: .left,
                toLabel: Self.closeControllerTargetID
            ),
            .init(
                fromLabel: Self.closeControllerTargetID,
                direction: .down,
                toLabel: ControllerAccessibilityDirectionalLink.navigationBoundary
            ),
            .init(
                fromLabel: Self.saveControllerTargetID,
                direction: .down,
                toLabel: ControllerAccessibilityDirectionalLink.navigationBoundary
            ),
        ]

        links.append(contentsOf: [
            .init(
                fromLabel: Self.detailBackControllerTargetID,
                direction: .down,
                toLabel: entryTarget
            ),
            .init(
                fromLabel: entryTarget,
                direction: .up,
                toLabel: Self.detailBackControllerTargetID
            ),
        ])
        return links
    }

    private func perGameDetailControllerTargetOrder(
        _ category: PerGameSettingsCategory
    ) -> [String] {
        switch category {
        case .general:
            var order = ["per-game.general.use-overrides"]
            if draft.hasGameSettingsIdentity {
                order.append("per-game.general.reset-all")
            }
            order.append("per-game.general.live-preview")
            if settings.temporalSaveStateToLivePreviewChanges {
                order.append("per-game.general.live-preview-circle-exit")
                order.append("per-game.general.before-changes-preview-duration")
            }
            return order

        case .graphics:
            guard draft.enabled else {
                return ["per-game.graphics.download-shaders"]
            }
            if initiallySelectsShaders {
                var order = ["per-game.graphics.shader-chain"]
                order.append("per-game.graphics.download-shaders")
                if draft.perGameShaderChain == 1 {
                    order.append("per-game.graphics.shader-preset")
                    if !draft.perGameShaderPresetRef.isEmpty {
                        order.append(
                            "per-game.graphics.clear-shader-preset"
                        )
                    }
                }
                return order
            }
            var order = [
                "per-game.graphics.renderer",
                "per-game.graphics.internal-resolution",
            ]
            if settings.isMetalFXAvailable {
                order.append("per-game.graphics.spatial-upscaler")
            }
            order += [
                "per-game.graphics.aspect-ratio",
                "per-game.graphics.texture-filtering",
                "per-game.graphics.hardware-mipmapping",
                "per-game.graphics.blending-accuracy",
                "per-game.graphics.deinterlace",
                "per-game.graphics.fxaa",
                "per-game.graphics.dithering",
                "per-game.graphics.tv-crt-shader",
                "per-game.graphics.cas-sharpening",
                "per-game.graphics.max-anisotropy",
                "per-game.graphics.cas-sharpness",
                "per-game.graphics.screen-offsets",
                "per-game.graphics.integer-scaling",
                "per-game.graphics.skip-duplicate-frames",
                "per-game.graphics.show-overscan",
                "per-game.graphics.anti-blur",
                "per-game.graphics.disable-interlace-offset",
                "per-game.graphics.shade-boost",
                "per-game.graphics.shade-boost-brightness",
                "per-game.graphics.shade-boost-contrast",
                "per-game.graphics.shade-boost-saturation",
                "per-game.graphics.shade-boost-gamma",
            ]
            order.append("per-game.graphics.shader-chain")
            order.append("per-game.graphics.download-shaders")
            if draft.perGameShaderChain == 1 {
                order.append("per-game.graphics.shader-preset")
                if !draft.perGameShaderPresetRef.isEmpty {
                    order.append("per-game.graphics.clear-shader-preset")
                }
            }
            order += [
                "per-game.graphics.trilinear-filtering",
                "per-game.graphics.half-pixel-offset",
                "per-game.graphics.round-sprite",
                "per-game.graphics.align-sprite",
                "per-game.graphics.merge-sprite",
                "per-game.graphics.wild-arms-offset",
                "per-game.graphics.texture-offset-x-override",
            ]
            if draft.textureOffsetXOverride {
                order.append("per-game.graphics.texture-offset-x")
            }
            order.append("per-game.graphics.texture-offset-y-override")
            if draft.textureOffsetYOverride {
                order.append("per-game.graphics.texture-offset-y")
            }
            if !draft.enableGameDBHardwareFixes {
                order.append("per-game.graphics.skipdraw-start-override")
                if draft.skipDrawStartOverride {
                    order.append("per-game.graphics.skipdraw-start")
                }
                order.append("per-game.graphics.skipdraw-end-override")
                if draft.skipDrawEndOverride {
                    order.append("per-game.graphics.skipdraw-end")
                }
            }
            order += [
                "per-game.graphics.hardware-download-mode",
                "per-game.graphics.disable-depth-emulation",
                "per-game.graphics.cpu-clut-render",
                "per-game.graphics.gpu-target-clut",
            ]
            return order

        case .framePacing:
            guard draft.enabled else { return [] }
            return [
                "per-game.frame-pacing.preset",
                "per-game.frame-pacing.frame-limiter",
                "per-game.frame-pacing.target-fps",
                "per-game.frame-pacing.vsync-queue",
                "per-game.frame-pacing.sync-host-refresh",
                "per-game.frame-pacing.audio-buffer",
                "per-game.frame-pacing.output-latency",
                "per-game.frame-pacing.reset",
            ]

        case .audio:
            guard draft.enabled else { return [] }
            var order = ["per-game.audio.custom-volume"]
            if draft.volumeOverride {
                order.append("per-game.audio.emulator-volume")
            }
            order.append("per-game.audio.fast-forward-volume")
            return order

        case .cpu:
            guard draft.enabled else { return [] }
            var order = [
                "per-game.cpu.ee-core",
                "per-game.cpu.mtvu",
                "per-game.cpu.iop-recompiler",
                "per-game.cpu.vu0-recompiler",
                "per-game.cpu.vu1-recompiler",
                "per-game.cpu.ee-cycle-rate",
            ]
            if draft.eeCycleRate != PerGameSettingsDraft.eeCycleRateUseGlobalSentinel {
                order.append("per-game.cpu.reset-ee-cycle-rate")
            }
            order.append("per-game.cpu.ee-cycle-skip")
            if draft.eeCycleSkip != -1 {
                order.append("per-game.cpu.reset-ee-cycle-skip")
            }
            order.append("per-game.cpu.fast-boot")
            if draft.fastBoot != PerGameSettingsDraft.fastBootUseGlobalSentinel {
                order.append("per-game.cpu.reset-fast-boot")
            }
            order += [
                "per-game.cpu.ee-fpu-round",
                "per-game.cpu.vu0-round",
                "per-game.cpu.vu1-round",
                "per-game.cpu.ee-clamp",
                "per-game.cpu.vu-clamp",
            ]
            return order

        case .gameController:
            return GamepadSettingsView.controllerTargetOrder(
                leftInstantDeadzoneEnabled:
                    settings.gameControllerLeftInstantDeadzoneEnabled,
                rightInstantDeadzoneEnabled:
                    settings.gameControllerRightInstantDeadzoneEnabled
            )

        case .pad:
            var order: [String] = []
            if draft.padLayoutIdentity != nil {
                order += [
                    "per-game.pad.layout",
                    "per-game.pad.skin",
                ]
                if perGamePadHasLinkedLayout {
                    order.append("per-game.pad.apply-linked-layout")
                }
                order += [
                    "per-game.pad.edit-layout",
                    "per-game.pad.reset-layout",
                    "per-game.pad.reset-skin",
                    "per-game.pad.reset-all",
                ]
            }
            if draft.hasGameSettingsIdentity {
                order += SettingsStore.stickInversionKeys.map {
                    "per-game.pad.inversion.\($0)"
                }
            }
            return order

        case .fixes:
            guard draft.enabled else { return [] }
            return [
                "per-game.fixes.accurate-alpha-test",
                "per-game.fixes.texture-inside-rt",
            ] + SettingsStore.gameFixOptions.map {
                "per-game.fixes.\($0.key)"
            }

        case .textures:
            guard draft.enabled else { return textureControllerTargets }
            return textureControllerTargets + [
                "per-game.textures.load-replacement-textures",
                "per-game.textures.async-loading",
                "per-game.textures.precache-textures",
            ]

        case .cheats:
            var order = ["per-game.cheats.manager"]
            if draft.enabled {
                order += [
                    "per-game.cheats.core-fixes",
                    "per-game.cheats.graphics-fixes",
                    "per-game.cheats.widescreen",
                    "per-game.cheats.no-interlace",
                ]
            }
            return order

        case .retroAchievements:
            guard draft.enabled else { return [] }
            return [
                "per-game.retro.enabled",
                "per-game.retro.hardcore",
            ]
        }
    }

    private var perGamePadHasLinkedLayout: Bool {
        let descriptor = layoutPresets.effectiveSkinDescriptor(
            for: draft.padLayoutIdentity,
            using: skinLibrary
        )
        guard let linkedID = descriptor.linkedLayoutPresetID else { return false }
        return layoutPresets.preset(id: linkedID) != nil
    }

    var perGamePreferredInitialFocusLabel: String? {
        if let kind = activeControllerAlertKind {
            // The first action is the non-destructive choice: Keep Editing for
            // discard confirmation, or Cancel for the reset confirmations.
            return controllerAlertActions(for: kind).first?.title
        }
        switch controllerColumn {
        case .categories:
            if initiallySelectsShaders {
                return shaderWorkspaceControllerTargetOrder.first
            }
            return landscapeCategoryControllerTargetID(openCategory)
        case .detail:
            if initiallySelectsShaders {
                return shaderWorkspaceControllerTargetOrder.first
                    ?? "per-game.graphics.shader-chain"
            }
            return landscapeDetailEntryLabel(
                for: selectedControllerDetail ?? openCategory
            )
        }
    }

    var controllerColumn: PerGameControllerColumn {
        selectedControllerDetail == nil ? .categories : .detail
    }

    // A page pushed before the stack's first layout draws its rows without the trailing margin.
    func pushOpenCategoryAfterLayout() {
        DispatchQueue.main.async { [self] in
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                selectedControllerDetail = openCategory
            }
        }
    }

    var portraitNavigationPath: Binding<[PerGameSettingsCategory]> {
        Binding(
            get: { [self] in selectedControllerDetail.map { [$0] } ?? [] },
            set: { [self] path in
                let category = path.last
                guard category != selectedControllerDetail else { return }
                selectedControllerDetail = category
                if let category { openCategory = category }
            }
        )
    }

    private func landscapeDetailEntryLabel(
        for category: PerGameSettingsCategory
    ) -> String {
        perGameDetailControllerTargetOrder(category).first
            ?? Self.closeControllerTargetID
    }

    func controllerAlertTitle(
        for kind: PerGameControllerAlertKind
    ) -> String {
        switch kind {
        case .resetAll:
            settings.localized("Reset all per-game overrides?")
        case .discardChanges:
            settings.localized("Discard your changes?")
        case .clearFramePacing:
            settings.localized("Clear Per-Game Frame Pacing?")
        }
    }

    func controllerAlertMessage(
        for kind: PerGameControllerAlertKind
    ) -> String {
        switch kind {
        case .resetAll:
            settings.localized("All per-game overrides for this title are removed; global settings apply on the next boot or reset.")
        case .discardChanges:
            settings.localized("You have unsaved per-game settings changes.")
        case .clearFramePacing:
            settings.localized("This removes your overrides for this game. It will use your global Frame Pacing settings.")
        }
    }

    func controllerAlertActions(
        for kind: PerGameControllerAlertKind
    ) -> [ControllerNavigationAlertAction] {
        let cancel = ControllerNavigationAlertAction(
            id: "cancel",
            title: settings.localized(
                kind == .discardChanges ? "Keep Editing" : "Cancel"
            )
        )
        switch kind {
        case .resetAll:
            return [
                cancel,
                .init(
                    id: "reset",
                    title: settings.localized("Reset All"),
                    isDestructive: true
                ),
            ]
        case .discardChanges:
            return [
                .init(
                    id: "apply",
                    title: settings.localized("Apply Changes")
                ),
                cancel,
                .init(
                    id: "discard",
                    title: settings.localized("Discard Changes"),
                    isDestructive: true
                ),
            ]
        case .clearFramePacing:
            return [
                cancel,
                .init(
                    id: "clear",
                    title: settings.localized("Clear"),
                    isDestructive: true
                ),
            ]
        }
    }

    func dismissControllerAlert(
        _ kind: PerGameControllerAlertKind
    ) {
        switch kind {
        case .resetAll: showResetAllConfirmation = false
        case .discardChanges: showDiscardConfirmation = false
        case .clearFramePacing: showFramePacingResetConfirmation = false
        }
    }
}
