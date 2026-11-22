// SettingsStore+Graphics.swift — graphics-domain config and helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    /// Homogeneous bool GS hacks, in display order.
    static let gsBoolHackOptions: [GameFixOption] = [
        .init(key: "paltex", label: "GPU Palette Conversion"),
        .init(key: "UserHacks_CPU_FB_Conversion", label: "CPU Framebuffer Conversion"),
        .init(key: "UserHacks_ReadTCOnClose", label: "Read Targets When Closing"),
        .init(key: "UserHacks_DisableDepthSupport", label: "Disable Depth Emulation"),
        .init(key: "UserHacks_DisablePartialInvalidation", label: "Disable Partial Invalidation"),
        .init(key: "preload_frame_with_gs_data", label: "Preload Frame Data"),
        .init(key: "UserHacks_EstimateTextureRegion", label: "Estimate Texture Region"),
        .init(key: "UserHacks_DrawBuffering", label: "Draw Buffering"),
        .init(key: "UserHacks_NativePaletteDraw", label: "Unscaled Palette Draw")
    ]

    static func aspectRatioName(for value: Int) -> String {
        switch value {
        case 0: return "Stretch"
        case 1: return "Auto 4:3/3:2"
        case 2: return "4:3"
        case 3: return "16:9"
        case 4: return "10:7"
        default: return "Auto 4:3/3:2"
        }
    }

    static func aspectRatioValue(from name: String) -> Int {
        switch name {
        case "Stretch", "0": return 0
        case "Auto 4:3/3:2", "1": return 1
        case "4:3", "2": return 2
        case "16:9", "3": return 3
        case "10:7", "4": return 4
        default: return 1
        }
    }

    static func supportedIOSRenderer(_ value: Int) -> Int {
        switch value {
        case 17, 13, 11:
            return value
        default:
            return 17
        }
    }
}

extension SettingsStore {
    var isMetalFXAvailable: Bool {
        ARMSX2Bridge.isMetalFXSupported()
    }
}

extension SettingsStore {
    // Boot-only on purpose: a live switch is a restart option, so applying it
    // would send GSUpdateConfig down GSreopen and tear the Metal device down
    // under the running game. The picker says "Requires restart".
    static let _rendererConfig = Setting<Int>(
        section: "EmuCore/GS", key: "Renderer", default: 17,
        suppressible: false,
        bootOnly: true,
        codec: .int)
    static let _upscaleMultiplierConfig = Setting<Float>(
        section: "EmuCore/GS", key: "upscale_multiplier", default: 1.0,
        suppressible: false,
        codec: .float)
    static let _vsyncQueueSizeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "VsyncQueueSize", default: 8,
        suppressible: false,
        codec: .int(in: SettingsStore.vsyncQueueRange))
    static let _textureFilteringConfig = Setting<Int>(
        section: "EmuCore/GS", key: "filter", default: 2,
        suppressible: false,
        codec: .int)
    // Boot-only for the same reason as the renderer above: the core counts this in
    // RestartOptionsAreEqual, so applying it live goes down GSreopen and tears the
    // Metal device down under the running game. The picker says "Requires restart".
    static let _backThreadModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "GSBackThreadMode", default: 0,
        suppressible: false,
        bootOnly: true,
        codec: .int)
    static let _hardwareMipmappingConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "hw_mipmap", default: true,
        suppressible: false,
        codec: .bool)
    static let _fxaaConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "fxaa", default: false,
        suppressible: false,
        codec: .bool)
    static let _casModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "CASMode", default: 0,
        suppressible: false,
        codec: .int)
    static let _casSharpnessConfig = Setting<Int>(
        section: "EmuCore/GS", key: "CASSharpness", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.casSharpnessRange))
    static let _shaderChainEnabledConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "ShaderChainEnabled", default: false,
        suppressible: false,
        codec: .bool)
    // Stored as a ShaderPresetLibrary token, since the container path changes on every install;
    // applyShaderChainSelection writes the absolute ShaderChainPreset that core reads.
    static let _shaderChainPresetRefConfig = Setting<String>(
        section: "EmuCore/GS", key: "ShaderChainPresetRef", default: "",
        suppressible: false,
        codec: .string)
    static let _interlaceModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "deinterlace_mode", default: 0,
        suppressible: false,
        codec: .int)
    static let _aspectRatioConfig = Setting<Int>(
        section: "EmuCore/GS", key: "AspectRatio", default: 1,
        suppressible: false,
        codec: .aspectRatio)
    static let _blendingAccuracyConfig = Setting<Int>(
        section: "EmuCore/GS", key: "accurate_blending_unit", default: 1,
        suppressible: false,
        codec: .int)
    static let _ditheringConfig = Setting<Int>(
        section: "EmuCore/GS", key: "dithering_ps2", default: 2,
        suppressible: false,
        codec: .int)
    static let _trilinearFilteringConfig = Setting<Int>(
        section: "EmuCore/GS", key: "TriFilter", default: -1,
        suppressible: false,
        codec: .int)
    static let _halfPixelOffsetConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_HalfPixelOffset", default: 0,
        suppressible: false,
        codec: .int)
    static let _roundSpriteConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_round_sprite_offset", default: 0,
        suppressible: false,
        codec: .int)
    static let _alignSpriteConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks_align_sprite_X", default: false,
        suppressible: false,
        codec: .bool)
    static let _mergeSpriteConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks_merge_pp_sprite", default: false,
        suppressible: false,
        codec: .bool)
    static let _wildArmsOffsetConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "UserHacks_ForceEvenSpritePosition", default: false,
        suppressible: false,
        codec: .bool)
    static let _loadTextureReplacementsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "LoadTextureReplacements", default: false,
        suppressible: false,
        codec: .bool)
    static let _loadTextureReplacementsAsyncConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "LoadTextureReplacementsAsync", default: true,
        suppressible: false,
        codec: .bool)
    static let _precacheTextureReplacementsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "PrecacheTextureReplacements", default: false,
        suppressible: false,
        codec: .bool)
    static let _texturePreloadingConfig = Setting<Int>(
        section: "EmuCore/GS", key: "texture_preloading", default: 2,
        suppressible: false,
        codec: .int)
    static let _dumpReplaceableTexturesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpReplaceableTextures", default: false,
        suppressible: false,
        codec: .bool)
    static let _dumpReplaceableMipmapsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpReplaceableMipmaps", default: false,
        suppressible: false,
        codec: .bool)
    static let _dumpTexturesWithFMVActiveConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpTexturesWithFMVActive", default: false,
        suppressible: false,
        codec: .bool)
    static let _dumpDirectTexturesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpDirectTextures", default: true,
        suppressible: false,
        codec: .bool)
    static let _dumpPaletteTexturesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "DumpPaletteTextures", default: true,
        suppressible: false,
        codec: .bool)
    // Compatibility-oriented hardware-renderer fixes. AAT (HWAccurateAlphaTest) and
    // Texture Inside RT close the GameDB advisory gap added in 2.3.2. Applied live by
    // the GS thread (no VM restart); some may require GameDB Graphics Fixes off.
    static let _hwAccurateAlphaTestConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "HWAccurateAlphaTest", default: false,
        suppressible: false,
        codec: .bool)
    static let _textureInsideRtConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_TextureInsideRt", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    static let _limit24BitDepthConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_Limit24BitDepth", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    static let _nativeScalingConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_native_scaling", default: 0,
        suppressible: false,
        codec: .int(in: 0...4))
    static let _cpuClutRenderConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_CPUCLUTRender", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    static let _cpuSpriteRenderBwConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_CPUSpriteRenderBW", default: 0,
        suppressible: false,
        codec: .int(in: SettingsStore.cpuSpriteRenderBwRange))
    static let _cpuSpriteRenderLevelConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_CPUSpriteRenderLevel", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    static let _gpuTargetClutConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_GPUTargetCLUTMode", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    static let _bilinearUpscaleHackConfig = Setting<Int>(
        section: "EmuCore/GS", key: "UserHacks_BilinearHack", default: 0,
        suppressible: false,
        codec: .int(in: 0...2))
    static let _maxAnisotropyConfig = Setting<Int>(
        section: "EmuCore/GS", key: "MaxAnisotropy", default: 0,
        suppressible: false,
        codec: .int(in: 0...16))
    static let _hardwareDownloadModeConfig = Setting<Int>(
        section: "EmuCore/GS", key: "HWDownloadMode", default: 0,
        suppressible: false,
        codec: .int(in: 0...4))
    static let _tvShaderConfig = Setting<Int>(
        section: "EmuCore/GS", key: "TVShader", default: 0,
        suppressible: false,
        codec: .int(in: 0...7))
    // MetalFX Spatial upscaler (0 = Off / bilinear, 1 = MetalFX Spatial).
    // Hidden in the UI when isMetalFXAvailable is false; default is Off.
    static let _upscalerConfig = Setting<Int>(
        section: "EmuCore/GS", key: "Upscaler", default: 0,
        suppressible: false,
        codec: .int)
    static let _pcrtcOffsetsConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "pcrtc_offsets", default: false,
        suppressible: false,
        codec: .bool)
    static let _pcrtcOverscanConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "pcrtc_overscan", default: false,
        suppressible: false,
        codec: .bool)
    static let _pcrtcAntiBlurConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "pcrtc_antiblur", default: true,
        suppressible: false,
        codec: .bool)
    static let _disableInterlaceOffsetConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "disable_interlace_offset", default: false,
        suppressible: false,
        codec: .bool)
    static let _skipDuplicateFramesConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "SkipDuplicateFrames", default: true,
        suppressible: false,
        codec: .bool)
    static let _syncToHostRefreshConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "SyncToHostRefreshRate", default: false,
        suppressible: false,
        codec: .bool)
    static let _integerScalingConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "IntegerScaling", default: false,
        suppressible: false,
        codec: .bool)
    static let _shadeBoostConfig = Setting<Bool>(
        section: "EmuCore/GS", key: "ShadeBoost", default: false,
        suppressible: false,
        codec: .bool)
    static let _shadeBoostBrightnessConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Brightness", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    static let _shadeBoostContrastConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Contrast", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    static let _shadeBoostSaturationConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Saturation", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
    static let _shadeBoostGammaConfig = Setting<Int>(
        section: "EmuCore/GS", key: "ShadeBoost_Gamma", default: 50,
        suppressible: false,
        codec: .int(in: SettingsStore.shadeBoostRange))
}
