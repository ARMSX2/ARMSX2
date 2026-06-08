// SettingsStore.swift — INI-backed settings for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

/// [P51] OSD preset levels
enum OsdPreset: Int, CaseIterable {
    case off = 0
    case simple = 1    // FPS + speed + CPU usage + device stats
    case detail = 2    // All except frame times graph
    case full = 3      // Everything

    var label: String {
        switch self {
        case .off: return "OFF"
        case .simple: return "Simple"
        case .detail: return "Detail"
        case .full: return "Full"
        }
    }
}

@Observable
final class SettingsStore: @unchecked Sendable {
    static let shared = SettingsStore()
    static let minTargetFPS: Float = 15.0
    static let maxTargetFPS: Float = 120.0
    static let defaultTargetFPS: Float = 60.0
    static let textureOffsetRange = -4096...4096
    static let skipDrawRange = 0...5000
    static let defaultOsdPerformancePosition = 3

    @ObservationIgnored private var suppressINIWrites = false

    // ── Emulator / CPU ──
    var eeCoreType: Int {
        didSet {
            ARMSX2Bridge.setINIInt("EmuCore/CPU", key: "CoreType", value: Int32(eeCoreType))
            ARMSX2Bridge.setINIBool("EmuCore/CPU", key: "UseArm64Dynarec", value: eeCoreType == 2)
        }
    }
    var iopRecompiler: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableIOP", value: iopRecompiler) }
    }
    var vu0Recompiler: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableVU0", value: vu0Recompiler) }
    }
    var vu1Recompiler: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableVU1", value: vu1Recompiler) }
    }
    var fastBoot: Bool {
        didSet {
            ARMSX2Bridge.setINIBool("GameISO", key: "FastBoot", value: fastBoot)
            ARMSX2Bridge.setINIBool("EmuCore", key: "EnableFastBoot", value: fastBoot)
        }
    }
    var fastmem: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", value: fastmem) }
    }
    var frameLimiterEnabled: Bool {
        didSet { applyFrameLimiterSettings() }
    }
    var targetFPS: Float {
        didSet {
            let normalized = Self.clampedTargetFPS(targetFPS)
            guard abs(targetFPS - normalized) <= 0.001 else {
                targetFPS = normalized
                return
            }
            applyFrameLimiterSettings()
        }
    }
    var ntscFramerate: Float {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "FramerateNTSC", value: ntscFramerate)
            applyFrameLimiterSettings()
        }
    }
    var palFramerate: Float {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "FrameratePAL", value: palFramerate)
        }
    }

    // ── Boot ──
    var fastCDVD: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "fastCDVD", value: fastCDVD) }
    }

    // ── Advanced Speedhacks ──
    var eeCycleRate: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/Speedhacks", key: "EECycleRate", value: Int32(eeCycleRate)) }
    }
    var vu1Instant: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vu1Instant", value: vu1Instant) }
    }
    var mtvu: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vuThread", value: mtvu) }
    }
    var waitLoop: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "WaitLoop", value: waitLoop) }
    }
    var intcStat: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "IntcStat", value: intcStat) }
    }
    var enableCheats: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableCheats", value: enableCheats) }
    }
    var enablePatches: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnablePatches", value: enablePatches) }
    }
    var enableGameFixes: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableGameFixes", value: enableGameFixes) }
    }
    var enableGameDBHardwareFixes: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "UserHacks", value: !enableGameDBHardwareFixes) }
    }
    var enableWidescreenPatches: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableWideScreenPatches", value: enableWidescreenPatches) }
    }
    var enableNoInterlacingPatches: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableNoInterlacingPatches", value: enableNoInterlacingPatches) }
    }
    var hostFilesystem: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "HostFs", value: hostFilesystem) }
    }

    // ── Graphics ──
    var renderer: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(renderer)) }
    }
    var upscaleMultiplier: Float {
        didSet { ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "upscale_multiplier", value: upscaleMultiplier) }
    }
    var vsyncQueueSize: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "VsyncQueueSize", value: Int32(vsyncQueueSize)) }
    }
    var textureFiltering: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "filter", value: Int32(textureFiltering)) }
    }
    var hardwareMipmapping: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "hw_mipmap", value: hardwareMipmapping) }
    }
    var fxaa: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "fxaa", value: fxaa) }
    }
    var casMode: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "CASMode", value: Int32(casMode)) }
    }
    var casSharpness: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "CASSharpness", value: Int32(casSharpness)) }
    }
    var interlaceMode: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "deinterlace_mode", value: Int32(interlaceMode)) }
    }
    var aspectRatio: Int {
        didSet { ARMSX2Bridge.setINIString("EmuCore/GS", key: "AspectRatio", value: Self.aspectRatioName(for: aspectRatio)) }
    }
    var blendingAccuracy: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "accurate_blending_unit", value: Int32(blendingAccuracy)) }
    }
    var dithering: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "dithering_ps2", value: Int32(dithering)) }
    }
    var trilinearFiltering: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "TriFilter", value: Int32(trilinearFiltering)) }
    }
    var halfPixelOffset: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_HalfPixelOffset", value: Int32(halfPixelOffset)) }
    }
    var roundSprite: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_round_sprite_offset", value: Int32(roundSprite)) }
    }
    var alignSprite: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "UserHacks_align_sprite_X", value: alignSprite) }
    }
    var mergeSprite: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "UserHacks_merge_pp_sprite", value: mergeSprite) }
    }
    var wildArmsOffset: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "UserHacks_ForceEvenSpritePosition", value: wildArmsOffset) }
    }
    var textureOffsetX: Int {
        didSet {
            let normalized = Self.clampedTextureOffset(textureOffsetX)
            guard textureOffsetX == normalized else {
                textureOffsetX = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_TCOffsetX", value: Int32(textureOffsetX))
        }
    }
    var textureOffsetY: Int {
        didSet {
            let normalized = Self.clampedTextureOffset(textureOffsetY)
            guard textureOffsetY == normalized else {
                textureOffsetY = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_TCOffsetY", value: Int32(textureOffsetY))
        }
    }
    var skipDrawStart: Int {
        didSet {
            let normalized = Self.clampedSkipDraw(skipDrawStart)
            guard skipDrawStart == normalized else {
                skipDrawStart = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_Start", value: Int32(skipDrawStart))
        }
    }
    var skipDrawEnd: Int {
        didSet {
            let normalized = Self.clampedSkipDraw(skipDrawEnd)
            guard skipDrawEnd == normalized else {
                skipDrawEnd = normalized
                return
            }
            ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_End", value: Int32(skipDrawEnd))
        }
    }
    var loadTextureReplacements: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "LoadTextureReplacements", value: loadTextureReplacements) }
    }
    var loadTextureReplacementsAsync: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "LoadTextureReplacementsAsync", value: loadTextureReplacementsAsync) }
    }
    var precacheTextureReplacements: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "PrecacheTextureReplacements", value: precacheTextureReplacements) }
    }
    var texturePreloading: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "texture_preloading", value: Int32(texturePreloading)) }
    }
    var dumpReplaceableTextures: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpReplaceableTextures", value: dumpReplaceableTextures) }
    }
    var dumpReplaceableMipmaps: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpReplaceableMipmaps", value: dumpReplaceableMipmaps) }
    }
    var dumpTexturesWithFMVActive: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpTexturesWithFMVActive", value: dumpTexturesWithFMVActive) }
    }
    var dumpDirectTextures: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpDirectTextures", value: dumpDirectTextures) }
    }
    var dumpPaletteTextures: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpPaletteTextures", value: dumpPaletteTextures) }
    }

    // ── OSD Overlay ──
    var osdPreset: OsdPreset {
        didSet {
            ARMSX2Bridge.setINIInt("ARMSX2iOS/UI", key: "OsdPreset", value: Int32(osdPreset.rawValue))
            if osdPreset == .off {
                if oldValue != .off {
                    lastActiveOsdPreset = oldValue
                }
            } else {
                lastActiveOsdPreset = osdPreset
            }
            applyOsdPreset(osdPreset)
        }
    }
    var lastActiveOsdPreset: OsdPreset {
        didSet {
            ARMSX2Bridge.setINIInt("ARMSX2iOS/UI", key: "LastActiveOsdPreset", value: Int32(lastActiveOsdPreset.rawValue))
        }
    }
    var osdPerformancePosition: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "OsdPerformancePos", value: Int32(osdPerformancePosition)) }
    }
    var osdShowFPS: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowFPS", value: osdShowFPS) }
    }
    var osdShowVPS: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowVPS", value: osdShowVPS) }
    }
    var osdShowSpeed: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowSpeed", value: osdShowSpeed) }
    }
    var osdShowCPU: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowCPU", value: osdShowCPU) }
    }
    var osdShowGPU: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowGPU", value: osdShowGPU) }
    }
    var osdShowResolution: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowResolution", value: osdShowResolution) }
    }
    var osdShowGSStats: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowGSStats", value: osdShowGSStats) }
    }
    var osdShowIndicators: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowIndicators", value: osdShowIndicators) }
    }
    var osdShowSettings: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowSettings", value: osdShowSettings) }
    }
    var osdShowInputs: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowInputs", value: osdShowInputs) }
    }
    var osdShowFrameTimes: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowFrameTimes", value: osdShowFrameTimes) }
    }
    var osdShowVersion: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowVersion", value: osdShowVersion) }
    }
    var osdShowHardwareInfo: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", value: osdShowHardwareInfo) }
    }
    var osdShowDeviceStats: Bool {
        didSet { ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "OsdShowDeviceStats", value: osdShowDeviceStats) }
    }

    // ── Gamepad / UI ──
    var padOpacity: Float {
        didSet { ARMSX2Bridge.setINIFloat("ARMSX2iOS/UI", key: "PadOpacity", value: padOpacity) }
    }
    var hapticFeedback: Bool {
        didSet { ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "HapticFeedback", value: hapticFeedback) }
    }
    var virtualPadSkin: VirtualPadSkin {
        didSet { ARMSX2Bridge.setINIInt("ARMSX2iOS/UI", key: "VirtualPadSkin", value: Int32(virtualPadSkin.rawValue)) }
    }
    var autoHideVirtualPadWhenControllerConnected: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "AutoHideVirtualPadWhenControllerConnected", value: autoHideVirtualPadWhenControllerConnected)
        }
    }
    var autoFullscreen: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "AutoFullscreen", value: autoFullscreen)
        }
    }
    var hideMenuButton: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "HideMenuButton", value: hideMenuButton)
        }
    }
    var analogStickScale: Float {
        didSet {
            let clamped = Self.clampedAnalogStickScale(analogStickScale)
            guard abs(analogStickScale - clamped) <= 0.001 else {
                analogStickScale = clamped
                return
            }
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", value: analogStickScale)
        }
    }
    var appLanguage: AppLanguage {
        didSet { ARMSX2Bridge.setINIString("ARMSX2iOS/UI", key: "AppLanguage", value: appLanguage.rawValue) }
    }
    var controllerMultitapMode: Int {
        didSet { ARMSX2Bridge.setINIInt("ARMSX2iOS/Gamepad", key: "MultitapMode", value: Int32(controllerMultitapMode)) }
    }
    var autoOpenStikDebug: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("ARMSX2iOS/JIT", key: "AutoOpenStikDebug", value: autoOpenStikDebug)
        }
    }

    // DEV9 / Network
    var dev9HddEnabled: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Hdd", key: "HddEnable", value: dev9HddEnabled)
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile)
        }
    }
    var dev9HddFile: String {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile)
        }
    }
    var dev9EthernetEnabled: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthEnable", value: dev9EthernetEnabled)
            if dev9EthernetEnabled {
                ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
                ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
            }
        }
    }
    var dev9EthDevice: String {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
        }
    }
    var dev9InterceptDHCP: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "InterceptDHCP", value: dev9InterceptDHCP)
        }
    }
    var dev9EthLogDHCP: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthLogDHCP", value: dev9EthLogDHCP)
        }
    }
    var dev9EthLogDNS: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthLogDNS", value: dev9EthLogDNS)
        }
    }

    private static func aspectRatioName(for value: Int) -> String {
        switch value {
        case 0: return "Stretch"
        case 1: return "Auto 4:3/3:2"
        case 2: return "4:3"
        case 3: return "16:9"
        case 4: return "10:7"
        default: return "Auto 4:3/3:2"
        }
    }

    private static func aspectRatioValue(from name: String) -> Int {
        switch name {
        case "Stretch", "0": return 0
        case "Auto 4:3/3:2", "1": return 1
        case "4:3", "2": return 2
        case "16:9", "3": return 3
        case "10:7", "4": return 4
        default: return 1
        }
    }

    private static func loadedFastBoot() -> Bool {
        let coreFastBoot = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableFastBoot", defaultValue: true)
        return ARMSX2Bridge.getINIBool("GameISO", key: "FastBoot", defaultValue: coreFastBoot)
    }

    // ── Init from INI ──
    private init() {
        // CPU
        eeCoreType = Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "CoreType", defaultValue: 2))
        iopRecompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableIOP", defaultValue: true)
        vu0Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU0", defaultValue: true)
        vu1Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU1", defaultValue: true)
        fastBoot = Self.loadedFastBoot()
        fastmem = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: true)
        let loadedNTSCFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FramerateNTSC", defaultValue: 59.94)
        ntscFramerate = loadedNTSCFramerate
        palFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FrameratePAL", defaultValue: 50.0)
        let nominalScalar = ARMSX2Bridge.getINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0)
        frameLimiterEnabled = Self.frameLimiterEnabled(fromNominalScalar: nominalScalar)
        targetFPS = Self.targetFPS(fromNominalScalar: nominalScalar, baseFramerate: loadedNTSCFramerate)
        Self.sanitizeNominalScalarIfNeeded(nominalScalar)
        // Boot
        fastCDVD = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "fastCDVD", defaultValue: false)
        // Advanced Speedhacks
        eeCycleRate = Int(ARMSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleRate", defaultValue: 0))
        vu1Instant = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vu1Instant", defaultValue: true)
        mtvu = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vuThread", defaultValue: false)
        waitLoop = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "WaitLoop", defaultValue: true)
        intcStat = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "IntcStat", defaultValue: true)
        enableCheats = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableCheats", defaultValue: false)
        enablePatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnablePatches", defaultValue: true)
        enableGameFixes = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableGameFixes", defaultValue: true)
        enableGameDBHardwareFixes = !ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks", defaultValue: false)
        enableWidescreenPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableWideScreenPatches", defaultValue: false)
        enableNoInterlacingPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableNoInterlacingPatches", defaultValue: false)
        hostFilesystem = ARMSX2Bridge.getINIBool("EmuCore", key: "HostFs", defaultValue: false)
        // Graphics
#if targetEnvironment(macCatalyst)
        renderer = 17
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(17))
#else
        let initialRenderer = Self.supportedIOSRenderer(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17)))
        renderer = initialRenderer
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(initialRenderer))
#endif
        upscaleMultiplier = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "upscale_multiplier", defaultValue: 1.0)
        vsyncQueueSize = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "VsyncQueueSize", defaultValue: 8))
        textureFiltering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "filter", defaultValue: 2))
        hardwareMipmapping = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "hw_mipmap", defaultValue: true)
        fxaa = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "fxaa", defaultValue: false)
        casMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASMode", defaultValue: 0))
        casSharpness = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASSharpness", defaultValue: 50))
        interlaceMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "deinterlace_mode", defaultValue: 7))
        aspectRatio = Self.aspectRatioValue(from: ARMSX2Bridge.getINIString("EmuCore/GS", key: "AspectRatio", defaultValue: "Auto 4:3/3:2"))
        blendingAccuracy = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "accurate_blending_unit", defaultValue: 1))
        dithering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "dithering_ps2", defaultValue: 2))
        trilinearFiltering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "TriFilter", defaultValue: -1))
        halfPixelOffset = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_HalfPixelOffset", defaultValue: 0))
        roundSprite = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_round_sprite_offset", defaultValue: 0))
        alignSprite = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks_align_sprite_X", defaultValue: false)
        mergeSprite = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks_merge_pp_sprite", defaultValue: false)
        wildArmsOffset = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks_ForceEvenSpritePosition", defaultValue: false)
        textureOffsetX = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetX", defaultValue: 0)))
        textureOffsetY = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetY", defaultValue: 0)))
        let loadedSkipDrawStart = Self.clampedSkipDraw(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_Start", defaultValue: 0)))
        skipDrawStart = loadedSkipDrawStart
        skipDrawEnd = Self.normalizedSkipDrawEnd(
            start: loadedSkipDrawStart,
            end: Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_End", defaultValue: 0))
        )
        loadTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacements", defaultValue: false)
        loadTextureReplacementsAsync = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacementsAsync", defaultValue: true)
        precacheTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "PrecacheTextureReplacements", defaultValue: false)
        texturePreloading = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "texture_preloading", defaultValue: 2))
        dumpReplaceableTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableTextures", defaultValue: false)
        dumpReplaceableMipmaps = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableMipmaps", defaultValue: false)
        dumpTexturesWithFMVActive = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpTexturesWithFMVActive", defaultValue: false)
        dumpDirectTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpDirectTextures", defaultValue: true)
        dumpPaletteTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpPaletteTextures", defaultValue: true)
        // OSD
        let loadedOsdPreset = OsdPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "OsdPreset", defaultValue: 0))) ?? .off
        osdPreset = loadedOsdPreset
        let loadedLastActiveOsdPresetRaw = ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "LastActiveOsdPreset", defaultValue: -1)
        if loadedLastActiveOsdPresetRaw >= 0 {
            lastActiveOsdPreset = OsdPreset(rawValue: Int(loadedLastActiveOsdPresetRaw)) ?? .simple
        } else {
            lastActiveOsdPreset = loadedOsdPreset != .off ? loadedOsdPreset : .simple
        }
        osdPerformancePosition = Self.normalizedOsdPerformancePosition(
            Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "OsdPerformancePos", defaultValue: Int32(Self.defaultOsdPerformancePosition)))
        )
        osdShowFPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFPS", defaultValue: false)
        osdShowVPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVPS", defaultValue: false)
        osdShowSpeed = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSpeed", defaultValue: false)
        osdShowCPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowCPU", defaultValue: false)
        osdShowGPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGPU", defaultValue: false)
        osdShowResolution = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowResolution", defaultValue: false)
        osdShowGSStats = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGSStats", defaultValue: false)
        osdShowIndicators = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowIndicators", defaultValue: false)
        osdShowSettings = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSettings", defaultValue: false)
        osdShowInputs = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowInputs", defaultValue: false)
        osdShowFrameTimes = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFrameTimes", defaultValue: false)
        osdShowVersion = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVersion", defaultValue: false)
        osdShowHardwareInfo = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", defaultValue: false)
        osdShowDeviceStats = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "OsdShowDeviceStats", defaultValue: loadedOsdPreset != .off)
        // UI
        padOpacity = ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "PadOpacity", defaultValue: 0.6)
        hapticFeedback = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HapticFeedback", defaultValue: true)
        virtualPadSkin = VirtualPadSkin(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "VirtualPadSkin", defaultValue: 0))) ?? .armsx2Refresh
        autoHideVirtualPadWhenControllerConnected = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "AutoHideVirtualPadWhenControllerConnected", defaultValue: true)
        autoFullscreen = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "AutoFullscreen", defaultValue: true)
        hideMenuButton = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HideMenuButton", defaultValue: false)
        analogStickScale = Self.clampedAnalogStickScale(ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", defaultValue: 1.0))
        appLanguage = AppLanguage(rawValue: ARMSX2Bridge.getINIString("ARMSX2iOS/UI", key: "AppLanguage", defaultValue: AppLanguage.system.rawValue)) ?? .system
        controllerMultitapMode = Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/Gamepad", key: "MultitapMode", defaultValue: 0))
        autoOpenStikDebug = ARMSX2Bridge.getINIBool("ARMSX2iOS/JIT", key: "AutoOpenStikDebug", defaultValue: false)
        dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
        dev9HddFile = ARMSX2Bridge.getINIString("DEV9/Hdd", key: "HddFile", defaultValue: "DEV9hdd.raw")
        dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
        dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        dev9InterceptDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "InterceptDHCP", defaultValue: false)
        dev9EthLogDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDHCP", defaultValue: false)
        dev9EthLogDNS = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDNS", defaultValue: false)
        normalizeDEV9Settings()
        ARMSX2Bridge.setINIString("EmuCore/GS", key: "AspectRatio", value: Self.aspectRatioName(for: aspectRatio))
        // Apply OSD preset
        ARMSX2Bridge.applyOsdPreset(Int32(osdPreset.rawValue))
    }

    /// Reload ALL settings from INI (call on VM start/stop)
    func reload() {
        suppressINIWrites = true
        defer { suppressINIWrites = false }

        eeCoreType = Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "CoreType", defaultValue: 2))
        iopRecompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableIOP", defaultValue: true)
        vu0Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU0", defaultValue: true)
        vu1Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU1", defaultValue: true)
        fastBoot = Self.loadedFastBoot()
        fastmem = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: true)
        ntscFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FramerateNTSC", defaultValue: 59.94)
        palFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FrameratePAL", defaultValue: 50.0)
        let nominalScalar = ARMSX2Bridge.getINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0)
        frameLimiterEnabled = Self.frameLimiterEnabled(fromNominalScalar: nominalScalar)
        targetFPS = Self.targetFPS(fromNominalScalar: nominalScalar, baseFramerate: ntscFramerate)
        Self.sanitizeNominalScalarIfNeeded(nominalScalar)
        fastCDVD = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "fastCDVD", defaultValue: false)
        eeCycleRate = Int(ARMSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleRate", defaultValue: 0))
        vu1Instant = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vu1Instant", defaultValue: true)
        mtvu = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vuThread", defaultValue: false)
        waitLoop = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "WaitLoop", defaultValue: true)
        intcStat = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "IntcStat", defaultValue: true)
        enableCheats = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableCheats", defaultValue: false)
        enablePatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnablePatches", defaultValue: true)
        enableGameFixes = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableGameFixes", defaultValue: true)
        enableGameDBHardwareFixes = !ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks", defaultValue: false)
        enableWidescreenPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableWideScreenPatches", defaultValue: false)
        enableNoInterlacingPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableNoInterlacingPatches", defaultValue: false)
        hostFilesystem = ARMSX2Bridge.getINIBool("EmuCore", key: "HostFs", defaultValue: false)
#if targetEnvironment(macCatalyst)
        renderer = 17
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(17))
#else
        renderer = Self.supportedIOSRenderer(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17)))
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(renderer))
#endif
        upscaleMultiplier = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "upscale_multiplier", defaultValue: 1.0)
        vsyncQueueSize = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "VsyncQueueSize", defaultValue: 8))
        textureFiltering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "filter", defaultValue: 2))
        hardwareMipmapping = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "hw_mipmap", defaultValue: true)
        fxaa = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "fxaa", defaultValue: false)
        casMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASMode", defaultValue: 0))
        casSharpness = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASSharpness", defaultValue: 50))
        interlaceMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "deinterlace_mode", defaultValue: 7))
        aspectRatio = Self.aspectRatioValue(from: ARMSX2Bridge.getINIString("EmuCore/GS", key: "AspectRatio", defaultValue: "Auto 4:3/3:2"))
        blendingAccuracy = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "accurate_blending_unit", defaultValue: 1))
        dithering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "dithering_ps2", defaultValue: 2))
        trilinearFiltering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "TriFilter", defaultValue: -1))
        halfPixelOffset = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_HalfPixelOffset", defaultValue: 0))
        roundSprite = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_round_sprite_offset", defaultValue: 0))
        alignSprite = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks_align_sprite_X", defaultValue: false)
        mergeSprite = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks_merge_pp_sprite", defaultValue: false)
        wildArmsOffset = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks_ForceEvenSpritePosition", defaultValue: false)
        textureOffsetX = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetX", defaultValue: 0)))
        textureOffsetY = Self.clampedTextureOffset(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_TCOffsetY", defaultValue: 0)))
        let loadedSkipDrawStart = Self.clampedSkipDraw(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_Start", defaultValue: 0)))
        skipDrawStart = loadedSkipDrawStart
        skipDrawEnd = Self.normalizedSkipDrawEnd(
            start: loadedSkipDrawStart,
            end: Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "UserHacks_SkipDraw_End", defaultValue: 0))
        )
        loadTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacements", defaultValue: false)
        loadTextureReplacementsAsync = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacementsAsync", defaultValue: true)
        precacheTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "PrecacheTextureReplacements", defaultValue: false)
        texturePreloading = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "texture_preloading", defaultValue: 2))
        dumpReplaceableTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableTextures", defaultValue: false)
        dumpReplaceableMipmaps = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableMipmaps", defaultValue: false)
        dumpTexturesWithFMVActive = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpTexturesWithFMVActive", defaultValue: false)
        dumpDirectTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpDirectTextures", defaultValue: true)
        dumpPaletteTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpPaletteTextures", defaultValue: true)
        osdPreset = OsdPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "OsdPreset", defaultValue: 0))) ?? .off
        let loadedLastActiveOsdPresetRaw = ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "LastActiveOsdPreset", defaultValue: -1)
        if loadedLastActiveOsdPresetRaw >= 0 {
            lastActiveOsdPreset = OsdPreset(rawValue: Int(loadedLastActiveOsdPresetRaw)) ?? .simple
        } else {
            lastActiveOsdPreset = osdPreset != .off ? osdPreset : .simple
        }
        osdPerformancePosition = Self.normalizedOsdPerformancePosition(
            Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "OsdPerformancePos", defaultValue: Int32(Self.defaultOsdPerformancePosition)))
        )
        osdShowFPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFPS", defaultValue: false)
        osdShowVPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVPS", defaultValue: false)
        osdShowSpeed = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSpeed", defaultValue: false)
        osdShowCPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowCPU", defaultValue: false)
        osdShowGPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGPU", defaultValue: false)
        osdShowResolution = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowResolution", defaultValue: false)
        osdShowGSStats = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGSStats", defaultValue: false)
        osdShowIndicators = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowIndicators", defaultValue: false)
        osdShowSettings = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSettings", defaultValue: false)
        osdShowInputs = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowInputs", defaultValue: false)
        osdShowFrameTimes = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFrameTimes", defaultValue: false)
        osdShowVersion = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVersion", defaultValue: false)
        osdShowHardwareInfo = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", defaultValue: false)
        osdShowDeviceStats = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "OsdShowDeviceStats", defaultValue: osdPreset != .off)
        padOpacity = ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "PadOpacity", defaultValue: 0.6)
        hapticFeedback = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HapticFeedback", defaultValue: true)
        virtualPadSkin = VirtualPadSkin(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "VirtualPadSkin", defaultValue: 0))) ?? .armsx2Refresh
        autoHideVirtualPadWhenControllerConnected = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "AutoHideVirtualPadWhenControllerConnected", defaultValue: true)
        autoFullscreen = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "AutoFullscreen", defaultValue: true)
        hideMenuButton = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HideMenuButton", defaultValue: false)
        analogStickScale = Self.clampedAnalogStickScale(ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "AnalogStickScale", defaultValue: 1.0))
        appLanguage = AppLanguage(rawValue: ARMSX2Bridge.getINIString("ARMSX2iOS/UI", key: "AppLanguage", defaultValue: AppLanguage.system.rawValue)) ?? .system
        controllerMultitapMode = Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/Gamepad", key: "MultitapMode", defaultValue: 0))
        autoOpenStikDebug = ARMSX2Bridge.getINIBool("ARMSX2iOS/JIT", key: "AutoOpenStikDebug", defaultValue: false)
        dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
        dev9HddFile = ARMSX2Bridge.getINIString("DEV9/Hdd", key: "HddFile", defaultValue: "DEV9hdd.raw")
        dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
        dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        dev9InterceptDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "InterceptDHCP", defaultValue: false)
        dev9EthLogDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDHCP", defaultValue: false)
        dev9EthLogDNS = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDNS", defaultValue: false)
        normalizeDEV9Settings()
    }

    private static func frameLimiterEnabled(fromNominalScalar scalar: Float) -> Bool {
        scalar < 5.0
    }

    private static func sanitizedNominalScalar(_ scalar: Float) -> Float {
        guard scalar.isFinite else { return 1.0 }
        return min(max(scalar, 0.05), 10.0)
    }

    private static func clampedTargetFPS(_ fps: Float) -> Float {
        guard fps.isFinite else { return defaultTargetFPS }
        return min(max(fps.rounded(), minTargetFPS), maxTargetFPS)
    }

    private static func clampedAnalogStickScale(_ scale: Float) -> Float {
        guard scale.isFinite else { return 1.0 }
        return min(max(scale, 0.8), 1.6)
    }

    private static func clampedTextureOffset(_ offset: Int) -> Int {
        min(max(offset, textureOffsetRange.lowerBound), textureOffsetRange.upperBound)
    }

    private static func clampedSkipDraw(_ value: Int) -> Int {
        min(max(value, skipDrawRange.lowerBound), skipDrawRange.upperBound)
    }

    static func normalizedSkipDrawEnd(start: Int, end: Int) -> Int {
        let clampedStart = clampedSkipDraw(start)
        let clampedEnd = clampedSkipDraw(end)
        return clampedStart > 0 && clampedEnd < clampedStart ? clampedStart : clampedEnd
    }

    private static func targetFPS(fromNominalScalar scalar: Float, baseFramerate: Float) -> Float {
        guard frameLimiterEnabled(fromNominalScalar: scalar) else { return defaultTargetFPS }
        return clampedTargetFPS(sanitizedNominalScalar(scalar) * max(baseFramerate, 1.0))
    }

    private static func sanitizeNominalScalarIfNeeded(_ scalar: Float) {
        let sanitized = sanitizedNominalScalar(scalar)
        guard abs(scalar - sanitized) > 0.001 else { return }

        NSLog("[ARMSX2 iOS Settings] Clamping unsupported NominalScalar %.3f -> %.3f", scalar, sanitized)
        ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: sanitized)
    }

    private static func normalizedOsdPerformancePosition(_ value: Int) -> Int {
        switch value {
        case 0, 1, 3:
            return value
        case 2:
            return defaultOsdPerformancePosition
        default:
            return defaultOsdPerformancePosition
        }
    }

    private func applyFrameLimiterSettings() {
        guard !suppressINIWrites else { return }
        let scalar: Float = frameLimiterEnabled ? Self.sanitizedNominalScalar(targetFPS / max(ntscFramerate, 1.0)) : 10.0
        NSLog("[ARMSX2 iOS Settings] Frame limiter %@ targetFPS=%.0f NominalScalar=%.3f",
              frameLimiterEnabled ? "ON" : "OFF", targetFPS, scalar)
        ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: scalar)
    }

    private func normalizeDEV9Settings() {
        if dev9HddEnabled {
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile.isEmpty ? "DEV9hdd.raw" : dev9HddFile)
        }

        if dev9EthernetEnabled {
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
        }
    }

    private static func supportedIOSRenderer(_ value: Int) -> Int {
        switch value {
        case 17, 13, 11:
            return value
        default:
            return 17
        }
    }

    func localized(_ key: String) -> String {
        appLanguage.localized(key)
    }

    var localizedLayoutDirection: LayoutDirection {
        appLanguage.layoutDirection
    }

    /// Apply OSD preset — writes ALL OSD flags to INI + GSConfig
    private func applyOsdPreset(_ preset: OsdPreset) {
        ARMSX2Bridge.applyOsdPreset(Int32(preset.rawValue))
        if preset == .off {
            osdPerformancePosition = 0
        } else if osdPerformancePosition == 0 {
            osdPerformancePosition = Self.defaultOsdPerformancePosition
        }
        let isSimple = preset == .simple
        let isDetail = preset == .detail
        let isFull = preset == .full
        osdShowFPS = isSimple || isDetail || isFull
        osdShowVPS = isDetail || isFull
        osdShowSpeed = isSimple || isDetail || isFull
        osdShowCPU = isSimple || isDetail || isFull
        osdShowGPU = isDetail || isFull
        osdShowResolution = isDetail || isFull
        osdShowGSStats = isFull
        osdShowIndicators = isDetail || isFull
        osdShowSettings = isFull
        osdShowInputs = isFull
        osdShowFrameTimes = isFull
        osdShowVersion = isSimple || isDetail || isFull
        osdShowHardwareInfo = isFull
        osdShowDeviceStats = isSimple || isDetail || isFull
    }

    /// Reset emulator settings to ARMSX2 iOS defaults
    func resetEmulatorDefaults() {
        eeCoreType = 2          // ARM64 JIT
        iopRecompiler = true
        vu0Recompiler = true
        vu1Recompiler = true
        fastBoot = false
        fastmem = true
        targetFPS = Self.defaultTargetFPS
        frameLimiterEnabled = true
        ntscFramerate = 59.94
        palFramerate = 50.0
        fastCDVD = false
        eeCycleRate = 0
        vu1Instant = true
        mtvu = false
        waitLoop = true
        intcStat = true
        enableCheats = false
        enablePatches = true
        enableGameFixes = true
        enableGameDBHardwareFixes = true
        enableWidescreenPatches = false
        enableNoInterlacingPatches = false
        hostFilesystem = false
    }

    /// Keep EE/IOP/VU0 fast while isolating suspected VU1 JIT regressions.
    func applyVU1CompatibilityPreset() {
        eeCoreType = 2
        iopRecompiler = true
        vu0Recompiler = true
        vu1Recompiler = false
        vu1Instant = false
        mtvu = false
        fastmem = false
    }

    /// Slow diagnostic preset for crash isolation when dynarec state is suspect.
    func applyFullInterpreterPreset() {
        eeCoreType = 1
        iopRecompiler = false
        vu0Recompiler = false
        vu1Recompiler = false
        vu1Instant = false
        mtvu = false
        fastmem = false
    }

    /// Reset graphics settings to ARMSX2 iOS defaults
    func resetGraphicsDefaults() {
        renderer = 17           // Metal
        upscaleMultiplier = 1.0 // Native PS2
        vsyncQueueSize = 8
        textureFiltering = 2    // Bilinear (PS2)
        hardwareMipmapping = true
        fxaa = false
        casMode = 0             // Disabled
        casSharpness = 50
        interlaceMode = 7       // Adaptive
        aspectRatio = 1         // Auto 4:3/3:2
        blendingAccuracy = 1    // Basic
        dithering = 2           // Scaled
        trilinearFiltering = -1 // Automatic
        halfPixelOffset = 0
        roundSprite = 0
        alignSprite = false
        mergeSprite = false
        wildArmsOffset = false
        textureOffsetX = 0
        textureOffsetY = 0
        skipDrawStart = 0
        skipDrawEnd = 0
        // Texture pack and dump toggles are intentionally preserved.
    }
}
