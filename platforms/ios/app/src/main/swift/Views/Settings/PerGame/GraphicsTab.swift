// GraphicsTab.swift — Per-game Graphics category tab.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GraphicsTab: View {
    // Master toggle + gating.
    @Binding var enabled: Bool
    @Binding var enableGameDBHardwareFixes: Bool
    let trilinearUseGlobalSentinel: Int
    let ophFlagHackEffective: Bool

    // Core graphics overrides. Each carries a use-global sentinel so an untouched setting is not
    // written to the per-game file at all, plus the global value to label what it inherits.
    @Binding var perGameRenderer: Int
    @Binding var upscaleMultiplier: Float
    @Binding var aspectRatio: String
    @Binding var textureFiltering: Int
    @Binding var hardwareMipmapping: Int
    @Binding var blendingAccuracy: Int
    @Binding var interlaceMode: Int

    // Advanced upscaling hacks.
    @Binding var trilinearFiltering: Int
    @Binding var halfPixelOffset: Int
    @Binding var roundSprite: Int
    @Binding var alignSprite: Int
    @Binding var mergeSprite: Int
    @Binding var wildArmsOffset: Int
    @Binding var textureOffsetXOverride: Bool
    @Binding var textureOffsetX: Int
    @Binding var textureOffsetYOverride: Bool
    @Binding var textureOffsetY: Int
    @Binding var skipDrawStartOverride: Bool
    @Binding var skipDrawStart: Int
    @Binding var skipDrawEndOverride: Bool
    @Binding var skipDrawEnd: Int

    // Per-game compatibility overrides surfaced on this tab.
    @Binding var perGameFXAA: Int
    @Binding var perGameUpscaler: Int
    @Binding var perGameShadeBoost: Int
    @Binding var perGameShadeBoostBrightness: Int
    @Binding var perGameShadeBoostContrast: Int
    @Binding var perGameShadeBoostSaturation: Int
    @Binding var perGameShadeBoostGamma: Int
    @Binding var perGameDithering: Int
    @Binding var perGameTVShader: Int
    @Binding var perGameCASMode: Int
    @Binding var perGameMaxAnisotropy: Int
    @Binding var perGameCASSharpness: Int
    @Binding var perGamePCRTCOffsets: Int
    @Binding var perGameIntegerScaling: Int
    @Binding var perGameSkipDupFrames: Int
    @Binding var perGamePCRTCOverscan: Int
    @Binding var perGamePCRTCAntiBlur: Int
    @Binding var perGameDisableInterlaceOffset: Int
    @Binding var perGameHWDownloadMode: Int
    @Binding var perGameCPUCLUT: Int
    @Binding var perGameGPUTargetCLUT: Int
    @Binding var perGameLoadTextureReplacements: Int
    @Binding var perGameLoadTextureReplacementsAsync: Int
    @Binding var perGamePrecacheTextureReplacements: Int

    let savesToRunningGame: Bool
    let settings: SettingsStore

    // MARK: Static option tables (moved from the panel)

    private static let useGlobalSentinel = -1
    private static let upscaleUseGlobalSentinel: Float = -1.0
    private static let aspectUseGlobalSentinel = ""
    private static let trilinearUseGlobalSentinelLocal = Int(Int32.min)

    // Trilinear needs Int32.min rather than -1, because -1 is a real TriFiltering value.
    private static let trilinearFilteringOptions =
        [(id: trilinearUseGlobalSentinelLocal, title: "Use Global")] + SettingsOptions.trilinearFiltering
    private static let halfPixelOffsetOptions = SettingsOptions.withUseGlobal(SettingsOptions.halfPixelOffset)
    private static let roundSpriteOptions = SettingsOptions.withUseGlobal(SettingsOptions.roundSprite)

    private static let aspectRatioOptions: [(id: String, title: String)] = [
        ("Auto 4:3/3:2", "Auto 4:3 / 3:2"),
        ("4:3", "4:3"),
        ("16:9", "16:9"),
        ("10:7", "10:7"),
        ("Stretch", "Stretch")
    ]
    private static let textureFilteringOptionsEnum: [(id: Int, title: String)] = [
        (0, "Nearest"),
        (1, "Bilinear Forced"),
        (2, "Bilinear PS2 Default"),
        (3, "Bilinear excl. Sprite")
    ]
    private static let blendingAccuracyOptions: [(id: Int, title: String)] = [
        (0, "Minimum"),
        (1, "Basic"),
        (2, "Medium"),
        (3, "High"),
        (4, "Full"),
        (5, "Ultra")
    ]

    var body: some View {
        PerGameTab(title: settings.localized("Graphics")) {
            graphicsContent
        }
    }

    @ViewBuilder
    private var graphicsContent: some View {
        Section(settings.localized("Graphics")) {
            sharedPicker("Renderer", selection: $perGameRenderer,
                         SettingsOptions.withUseGlobal(SettingsOptions.renderer))
                .disabled(!enabled)

            Text(settings.localized("Software Renderer is much slower but can fix games that break on Metal. It applies the next time this game boots."))
                .font(.caption)
                .foregroundStyle(.secondary)

            EnumPicker([(id: Self.upscaleUseGlobalSentinel, title: settings.localized("Use Global"))] + UpscaleOptions.all, selection: $upscaleMultiplier) {
                Text(settings.localized("Internal Resolution"))
            }
            .disabled(!enabled)

            if upscaleMultiplier > 1 && !ophFlagHackEffective {
                Text(settings.localized("Tip: OPH Flag Hack may help reduce slowdowns at higher resolutions."))
                    .font(.caption)
                    .foregroundStyle(OverlayTheme.warm)
            }

            if settings.isMetalFXAvailable {
                Picker(settings.localized("Spatial Upscaler"), selection: $perGameUpscaler) {
                    Text(settings.localized("Use Global")).tag(-1)
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("MetalFX Spatial")).tag(1)
                }
                .disabled(!enabled)
            }

            EnumPicker([(id: Self.aspectUseGlobalSentinel, title: settings.localized("Use Global"))] + Self.aspectRatioOptions, selection: $aspectRatio) {
                Text(settings.localized("Aspect Ratio"))
            }
            .disabled(!enabled)

            EnumPicker([(id: Self.useGlobalSentinel, title: settings.localized("Use Global"))] + Self.textureFilteringOptionsEnum, selection: $textureFiltering) {
                Text(settings.localized("Texture Filtering"))
            }
            .disabled(!enabled)

            Picker(settings.localized("Hardware Mipmapping"), selection: $hardwareMipmapping) {
                Text(settings.localized("Use Global")).tag(Self.useGlobalSentinel)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)
            Text(settings.localized("Turn this off only for games with mipmap-related texture stripes, shimmer, or bad LOD. " + (savesToRunningGame ? "Applies when you save." : "Applies on next boot.")))
                .font(.caption)
                .foregroundStyle(.secondary)

            EnumPicker([(id: Self.useGlobalSentinel, title: settings.localized("Use Global"))] + Self.blendingAccuracyOptions, selection: $blendingAccuracy) {
                Text(settings.localized("Blending Accuracy"))
            }
            .disabled(!enabled)

            sharedPicker("Deinterlace", selection: $interlaceMode,
                         SettingsOptions.withUseGlobal(SettingsOptions.deinterlace))
                .disabled(!enabled)

            Picker(settings.localized("FXAA"), selection: $perGameFXAA) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Dithering"), selection: $perGameDithering) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("Unscaled")).tag(1)
                Text(settings.localized("Scaled")).tag(2)
            }
            .disabled(!enabled)

            sharedPicker("TV/CRT Shader", selection: $perGameTVShader,
                         SettingsOptions.withUseGlobal(SettingsOptions.tvShader))
                .disabled(!enabled)
            Text(settings.localized("Scanline and CRT effects are subtle on high-resolution displays and are more visible at a lower Internal Resolution."))
                .font(.caption)
                .foregroundStyle(.secondary)

            Picker(settings.localized("CAS Sharpening"), selection: $perGameCASMode) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            sharedPicker("Max Anisotropy", selection: $perGameMaxAnisotropy,
                         SettingsOptions.withUseGlobal(SettingsOptions.maxAnisotropy))
                .disabled(!enabled)

            NumberOverrideRow(.casSharpness, value: $perGameCASSharpness,
                              global: settings.casSharpness,
                              settings: settings)
                .disabled(!enabled)

            Picker(settings.localized("Screen Offsets"), selection: $perGamePCRTCOffsets) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Integer Scaling"), selection: $perGameIntegerScaling) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Skip Duplicate Frames"), selection: $perGameSkipDupFrames) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Show Overscan"), selection: $perGamePCRTCOverscan) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Anti-Blur"), selection: $perGamePCRTCAntiBlur) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Disable Interlace Offset"), selection: $perGameDisableInterlaceOffset) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)
        }

        // Its own section, the way the global screen has it. The header is what lets the four rows
        // be called Brightness and Contrast rather than repeating Shade Boost four times.
        Section(settings.localized("Shade Boost")) {
            Picker(settings.localized("Shade Boost"), selection: $perGameShadeBoost) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            NumberOverrideRow(.shadeBoostBrightness, value: $perGameShadeBoostBrightness,
                              global: settings.shadeBoostBrightness,
                              settings: settings)
                .disabled(!enabled)
            NumberOverrideRow(.shadeBoostContrast, value: $perGameShadeBoostContrast,
                              global: settings.shadeBoostContrast,
                              settings: settings)
                .disabled(!enabled)
            NumberOverrideRow(.shadeBoostSaturation, value: $perGameShadeBoostSaturation,
                              global: settings.shadeBoostSaturation,
                              settings: settings)
                .disabled(!enabled)
            NumberOverrideRow(.shadeBoostGamma, value: $perGameShadeBoostGamma,
                              global: settings.shadeBoostGamma,
                              settings: settings)
                .disabled(!enabled)
        }

        Section(settings.localized("Advanced Upscaling Hacks")) {
            Text(settings.localized("A hack you set here outranks the game database for this game, and everything on Use Global stays automatic. " + (savesToRunningGame ? "Changes apply when you save." : "Changes apply on next boot.")))
                .font(.caption)
                .foregroundStyle(.secondary)

            if enabled && enableGameDBHardwareFixes {
                Text(settings.localized("Skipdraw is the exception: it only applies while GameDB Graphics Fixes is off for this game."))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            sharedPicker("Trilinear Filtering", selection: $trilinearFiltering,
                         Self.trilinearFilteringOptions)
                .disabled(!enabled)

            if trilinearFiltering != trilinearUseGlobalSentinel && trilinearFiltering != -1 {
                Text(settings.localized("Non-automatic trilinear filtering may break textures in some games."))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            sharedPicker("Half-pixel Offset", selection: $halfPixelOffset,
                         Self.halfPixelOffsetOptions)
                .disabled(!enabled)

            sharedPicker("Round Sprite", selection: $roundSprite,
                         Self.roundSpriteOptions)
                .disabled(!enabled)

            Picker(settings.localized("Align Sprite"), selection: $alignSprite) {
                Text(settings.localized("Use Global")).tag(Self.useGlobalSentinel)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Merge Sprite"), selection: $mergeSprite) {
                Text(settings.localized("Use Global")).tag(Self.useGlobalSentinel)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Picker(settings.localized("Wild Arms Offset"), selection: $wildArmsOffset) {
                Text(settings.localized("Use Global")).tag(Self.useGlobalSentinel)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)

            Toggle(settings.localized("Override Texture Offset X"), isOn: $textureOffsetXOverride)
                .disabled(!enabled)
            if textureOffsetXOverride {
                NumberRow(.textureOffsetX, value: $textureOffsetX, settings: settings)
                    .disabled(!enabled)
            }

            Toggle(settings.localized("Override Texture Offset Y"), isOn: $textureOffsetYOverride)
                .disabled(!enabled)
            if textureOffsetYOverride {
                NumberRow(.textureOffsetY, value: $textureOffsetY, settings: settings)
                    .disabled(!enabled)
            }

            Toggle(settings.localized("Override Skipdraw Start"), isOn: $skipDrawStartOverride)
                .disabled(!manualAdvancedHacksEnabled)
            if skipDrawStartOverride {
                NumberRow(.skipDrawStart, value: skipDrawStartBinding, settings: settings)
                    .disabled(!manualAdvancedHacksEnabled)
            }

            Toggle(settings.localized("Override Skipdraw End"), isOn: $skipDrawEndOverride)
                .disabled(!manualAdvancedHacksEnabled)
            if skipDrawEndOverride {
                NumberRow(.skipDrawEnd, value: skipDrawEndBinding, settings: settings)
                    .disabled(!manualAdvancedHacksEnabled)
            }
            if skipDrawStartOverride || skipDrawEndOverride {
                Text(settings.localized("For Skipdraw 1, use Start 1 and End 1. " + (savesToRunningGame ? "Changes apply when you save." : "Changes apply on next boot.")))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        }

        Section(settings.localized("Hardware Fixes & Display")) {
            sharedPicker("Hardware Download Mode", selection: $perGameHWDownloadMode,
                         SettingsOptions.withUseGlobal(SettingsOptions.hardwareDownloadMode))
                .disabled(!enabled)
            sharedPicker("CPU CLUT Render", selection: $perGameCPUCLUT,
                         SettingsOptions.withUseGlobal(SettingsOptions.cpuClutRender))
                .disabled(!enabled)
            sharedPicker("GPU Target CLUT", selection: $perGameGPUTargetCLUT,
                         SettingsOptions.withUseGlobal(SettingsOptions.gpuTargetClut))
                .disabled(!enabled)
        }

        Section(settings.localized("Texture Replacement")) {
            Picker(settings.localized("Load Replacement Textures"), selection: $perGameLoadTextureReplacements) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)
            Picker(settings.localized("Async Loading"), selection: $perGameLoadTextureReplacementsAsync) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)
            Picker(settings.localized("Precache Textures"), selection: $perGamePrecacheTextureReplacements) {
                Text(settings.localized("Use Global")).tag(-1)
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("On")).tag(1)
            }
            .disabled(!enabled)
            Text(settings.localized("Texture replacement needs a restart to take effect."))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    /// Picker over a shared option table, localized the way the global screen's own
    /// helper does. The caller adds `.disabled(...)`, since the gate differs per row.
    private func sharedPicker(_ title: String, selection: Binding<Int>,
                              _ options: [(id: Int, title: String)]) -> some View {
        Picker(settings.localized(title), selection: selection) {
            ForEach(options, id: \.id) { option in
                Text(settings.localized(option.title)).tag(option.id)
            }
        }
    }

    private var manualAdvancedHacksEnabled: Bool {
        enabled && !enableGameDBHardwareFixes
    }

    private var skipDrawStartBinding: Binding<Int> {
        Binding(
            get: { skipDrawStart },
            set: { newValue in
                skipDrawStart = Self.clampedSkipDraw(newValue)
                normalizeSkipDrawRangeIfNeeded()
            }
        )
    }

    private var skipDrawEndBinding: Binding<Int> {
        Binding(
            get: { skipDrawEnd },
            set: { newValue in
                skipDrawEnd = Self.normalizedSkipDrawEnd(
                    start: skipDrawStart,
                    end: newValue,
                    startOverride: skipDrawStartOverride,
                    endOverride: skipDrawEndOverride
                )
            }
        )
    }

    private func normalizeSkipDrawRangeIfNeeded() {
        let normalized = normalizedSkipDrawValues()
        if skipDrawStart != normalized.start {
            skipDrawStart = normalized.start
        }
        if skipDrawEnd != normalized.end {
            skipDrawEnd = normalized.end
        }
    }

    private func normalizedSkipDrawValues() -> (start: Int, end: Int) {
        let start = Self.clampedSkipDraw(skipDrawStart)
        let end = Self.normalizedSkipDrawEnd(
            start: start,
            end: skipDrawEnd,
            startOverride: skipDrawStartOverride,
            endOverride: skipDrawEndOverride
        )
        return (start, end)
    }

    private static func clampedSkipDraw(_ value: Int) -> Int {
        min(max(value, SettingsStore.skipDrawRange.lowerBound), SettingsStore.skipDrawRange.upperBound)
    }

    private static func normalizedSkipDrawEnd(start: Int, end: Int, startOverride: Bool, endOverride: Bool) -> Int {
        let clampedEnd = clampedSkipDraw(end)
        guard startOverride && endOverride else {
            return clampedEnd
        }
        return SettingsStore.normalizedSkipDrawEnd(start: start, end: clampedEnd)
    }
}
