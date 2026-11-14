// PerGameCategoryDetail.swift — the settings page of one per-game category
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PerGameCategoryDetail: View {
    let category: PerGameSettingsCategory
    @Bindable var draft: PerGameSettingsDraft
    @Bindable var controller: PerGameSettingsControllerCoordinator
    let initiallySelectsShaders: Bool
    @Binding var showPadLayoutEditor: Bool
    @Binding var showCheatsManager: Bool
    @Binding var shaderPresetRequest: ShaderPresetBrowserRequest?
    @State private var settings = SettingsStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared

    private var game: ISOEntry { draft.game }

    var body: some View {
        detailContent(category)
    }

    /// Whether OPH Flag Hack is effectively on for this game: a per-game override of 1, or
    /// the global value when the per-game override is set to use-global (-1). Used to hide
    /// the higher-resolution OPH suggestion once OPH is in effect. Passed to GraphicsTab.
    private var ophFlagHackEffective: Bool {
        let perGame = draft.perGameFixes["OPHFlagHack"] ?? -1
        if perGame == 1 { return true }
        if perGame == 0 { return false }
        return settings.gameFixEnabled("OPHFlagHack")
    }

    @ViewBuilder
    private func detailContent(_ category: PerGameSettingsCategory) -> some View {
        switch category {
        case .general:  generalTab
        case .graphics: graphicsTab
        case .textures: texturesTab
        case .framePacing: framePacingTab
        case .audio:    audioTab
        case .cpu:      cpuTab
        case .gameController: gameControllerTab
        case .pad:      padTab
        case .fixes:    fixesTab
        case .cheats:   cheatsTab
        case .retroAchievements: retroAchievementsTab
        }
    }

    // MARK: - Tab construction

    private var generalTab: some View {
        GeneralTab(
            enabled: $draft.enabled,
            hasGameSettingsIdentity: $draft.hasGameSettingsIdentity,
            showResetAllConfirmation: $controller.showResetAllConfirmation,
            statusMessage: $draft.statusMessage,
            displayName: draft.displayName,
            hasPendingChanges: draft.hasPendingChanges,
            savesToRunningGame: draft.savesToRunningGame,
            game: game,
            settings: settings
        )
    }

    private var graphicsTab: some View {
        GraphicsTab(
            enabled: $draft.enabled,
            enableGameDBHardwareFixes: $draft.enableGameDBHardwareFixes,
            trilinearUseGlobalSentinel: PerGameSettingsDraft.trilinearUseGlobalSentinel,
            ophFlagHackEffective: ophFlagHackEffective,
            perGameRenderer: $draft.perGameRenderer,
            upscaleMultiplier: $draft.upscaleMultiplier,
            aspectRatio: $draft.aspectRatio,
            textureFiltering: $draft.textureFiltering,
            hardwareMipmapping: $draft.hardwareMipmapping,
            blendingAccuracy: $draft.blendingAccuracy,
            interlaceMode: $draft.interlaceMode,
            trilinearFiltering: $draft.trilinearFiltering,
            halfPixelOffset: $draft.halfPixelOffset,
            roundSprite: $draft.roundSprite,
            alignSprite: $draft.alignSprite,
            mergeSprite: $draft.mergeSprite,
            wildArmsOffset: $draft.wildArmsOffset,
            textureOffsetXOverride: $draft.textureOffsetXOverride,
            textureOffsetX: $draft.textureOffsetX,
            textureOffsetYOverride: $draft.textureOffsetYOverride,
            textureOffsetY: $draft.textureOffsetY,
            skipDrawStartOverride: $draft.skipDrawStartOverride,
            skipDrawStart: $draft.skipDrawStart,
            skipDrawEndOverride: $draft.skipDrawEndOverride,
            skipDrawEnd: $draft.skipDrawEnd,
            perGameFXAA: $draft.perGameFXAA,
            perGameUpscaler: $draft.perGameUpscaler,
            perGameShadeBoost: $draft.perGameShadeBoost,
            perGameShadeBoostBrightness: $draft.perGameShadeBoostBrightness,
            perGameShadeBoostContrast: $draft.perGameShadeBoostContrast,
            perGameShadeBoostSaturation: $draft.perGameShadeBoostSaturation,
            perGameShadeBoostGamma: $draft.perGameShadeBoostGamma,
            perGameShaderChain: $draft.perGameShaderChain,
            perGameShaderPresetRef: $draft.perGameShaderPresetRef,
            perGameDithering: $draft.perGameDithering,
            perGameTVShader: $draft.perGameTVShader,
            perGameCASMode: $draft.perGameCASMode,
            perGameMaxAnisotropy: $draft.perGameMaxAnisotropy,
            perGameCASSharpness: $draft.perGameCASSharpness,
            perGamePCRTCOffsets: $draft.perGamePCRTCOffsets,
            perGameIntegerScaling: $draft.perGameIntegerScaling,
            perGameSkipDupFrames: $draft.perGameSkipDupFrames,
            perGamePCRTCOverscan: $draft.perGamePCRTCOverscan,
            perGamePCRTCAntiBlur: $draft.perGamePCRTCAntiBlur,
            perGameDisableInterlaceOffset: $draft.perGameDisableInterlaceOffset,
            perGameHWDownloadMode: $draft.perGameHWDownloadMode,
            perGameDisableDepth: $draft.perGameDisableDepth,
            perGameCPUCLUT: $draft.perGameCPUCLUT,
            perGameGPUTargetCLUT: $draft.perGameGPUTargetCLUT,
            savesToRunningGame: draft.savesToRunningGame,
            onBrowseShaderPreset: { shaderPresetRequest = ShaderPresetBrowserRequest() },
            settings: settings,
            showsOnlyShaders: initiallySelectsShaders
        )
    }

    private var audioTab: some View {
        AudioTab(
            enabled: $draft.enabled,
            volumeOverride: $draft.volumeOverride,
            volumePercent: $draft.volumePercent,
            globalVolumePercent: .constant(draft.globalVolumePercent),
            perGameFastForwardVolume: $draft.perGameFastForwardVolume,
            settings: settings
        )
    }

    private var texturesTab: some View {
        TexturesTab(
            enabled: $draft.enabled,
            perGameLoadTextureReplacements: $draft.perGameLoadTextureReplacements,
            perGameLoadTextureReplacementsAsync: $draft.perGameLoadTextureReplacementsAsync,
            perGamePrecacheTextureReplacements: $draft.perGamePrecacheTextureReplacements,
            controllerTargets: $controller.textureControllerTargets,
            serial: game.metadata["serial"]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "",
            settings: settings
        )
    }

    private var framePacingTab: some View {
        FramePacingTab(
            enabled: $draft.enabled,
            settings: settings,
            perGameFramePacingPreset: $draft.perGameFramePacingPreset,
            perGameFrameLimiter: $draft.perGameFrameLimiter,
            perGameTargetFPS: $draft.perGameTargetFPS,
            perGameVsyncQueue: $draft.perGameVsyncQueue,
            perGameSyncToHostRefresh: $draft.perGameSyncToHostRefresh,
            perGameBufferMS: $draft.perGameBufferMS,
            perGameOutputLatencyMS: $draft.perGameOutputLatencyMS,
            showResetConfirmation: $controller.showFramePacingResetConfirmation
        )
    }

    private var cpuTab: some View {
        CPUTab(
            enabled: $draft.enabled,
            eeCoreType: $draft.eeCoreType,
            mtvu: $draft.mtvu,
            eeCycleRate: $draft.eeCycleRate,
            globalEECycleRate: .constant(draft.globalEECycleRate),
            eeCycleSkip: $draft.eeCycleSkip,
            globalEECycleSkip: .constant(draft.globalEECycleSkip),
            fastBoot: $draft.fastBoot,
            globalFastBoot: .constant(draft.globalFastBoot),
            perGameIOP: $draft.perGameIOP,
            perGameVU0: $draft.perGameVU0,
            perGameVU1: $draft.perGameVU1,
            perGameEEFpuRound: $draft.perGameEEFpuRound,
            perGameVU0Round: $draft.perGameVU0Round,
            perGameVU1Round: $draft.perGameVU1Round,
            perGameEEClamp: $draft.perGameEEClamp,
            perGameVUClamp: $draft.perGameVUClamp,
            savesToRunningGame: draft.savesToRunningGame,
            settings: settings,
            eeCycleRateUseGlobalSentinel: PerGameSettingsDraft.eeCycleRateUseGlobalSentinel,
            fastBootUseGlobalSentinel: PerGameSettingsDraft.fastBootUseGlobalSentinel,
            fastBootOff: PerGameSettingsDraft.fastBootOff,
            fastBootOn: PerGameSettingsDraft.fastBootOn,
            globalEEFpuRound: draft.globalEEFpuRound,
            globalVU0Round: draft.globalVU0Round,
            globalVU1Round: draft.globalVU1Round,
            globalEEClamp: draft.globalEEClamp,
            globalVUClamp: draft.globalVUClamp
        )
    }

    /// The controller mappings, inversion, dead-zone, rumble and macro controls
    /// intentionally share the global store with Settings > Game Controller.
    /// Reusing the same form keeps both entry points behaviorally identical.
    private var gameControllerTab: some View {
        GamepadSettingsView(presentsLinkedPanesModally: true)
    }

    private var padTab: some View {
        PadTab(
            padLayoutIdentity: $draft.padLayoutIdentity,
            showPadLayoutEditor: $showPadLayoutEditor,
            layoutPresets: layoutPresets,
            skinLibrary: skinLibrary,
            savesToRunningGame: draft.savesToRunningGame,
            iso: game.bootName,
            hasGameSettingsIdentity: draft.hasGameSettingsIdentity
        )
    }

    private var fixesTab: some View {
        FixesTab(
            enabled: $draft.enabled,
            perGameAAT: $draft.perGameAAT,
            perGameTextureInsideRt: $draft.perGameTextureInsideRt,
            perGameFixes: $draft.perGameFixes,
            savesToRunningGame: draft.savesToRunningGame,
            settings: settings
        )
    }

    private var cheatsTab: some View {
        CheatsTab(
            enabled: $draft.enabled,
            enableGameFixes: $draft.enableGameFixes,
            enableGameDBHardwareFixes: $draft.enableGameDBHardwareFixes,
            perGameWidescreen: $draft.perGameWidescreen,
            perGameNoInterlace: $draft.perGameNoInterlace,
            showCheatsManager: $showCheatsManager,
            savesToRunningGame: draft.savesToRunningGame,
            settings: settings
        )
    }

    private var retroAchievementsTab: some View {
        RetroAchievementsTab(
            enabled: $draft.enabled,
            raEnabledOverride: $draft.raEnabledOverride,
            raHardcoreOverride: $draft.raHardcoreOverride,
            settings: settings
        )
    }
}
