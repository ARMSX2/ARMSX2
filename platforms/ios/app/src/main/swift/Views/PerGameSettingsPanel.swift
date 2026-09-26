// PerGameSettingsPanel.swift - Per-game overrides panel
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import UIKit

private enum PerGameControllerAlertKind: Equatable {
    case resetAll
    case discardChanges
    case clearFramePacing
}

enum PerGameLivePreviewPresentation: Equatable {
    case editing
    case playingBefore
    case applying
    case playingAfter
    case restoring
    case finishing

    var hidesSettingsUI: Bool { self != .editing && self != .finishing }
    var runsGame: Bool {
        self == .playingBefore || self == .playingAfter
    }
}

/// One stable live-preview toast update. The setting label deliberately stays out of this model:
/// rapid controller repeats should roll the new value inside the existing toast, not replace a
/// longer `Setting -> Value` sentence twice per adjustment.
struct PerGameLivePreviewStatus: Equatable {
    let value: String
    let countsDown: Bool
}
struct PerGameSettingsPanel: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiContextMenuColour) private var panelTextColour
    @Environment(\.uiContextMenuSecondaryColour) private var panelSecondaryTextColour
    @State private var settings = SettingsStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared

    private enum PerGameSettingsCategory: String, CaseIterable, Identifiable, Hashable {
        case general, graphics, framePacing, audio, cpu, gameController, pad, fixes, cheats, retroAchievements

        var id: Self { self }

        var titleKey: String {
            switch self {
            case .general: return "General"
            case .graphics: return "Graphics"
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

    /// Whether this build has librashader, read once for the Graphics tab.
    private static let shaderChainSupported = ARMSX2Bridge.isShaderChainSupported()

    private enum PerGameSettingsLayout: String, Hashable {
        case portrait
        case landscape

        init(size: CGSize) {
            self = size.width > size.height ? .landscape : .portrait
        }

        var usesSplitView: Bool { self == .landscape }
    }

    private enum PerGameControllerColumn: String {
        case categories
        case detail
    }

    private static let closeControllerTargetID = "per-game.footer.close"
    private static let saveControllerTargetID = "per-game.footer.save"
    private static let detailBackControllerTargetID = "per-game.detail.back"

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

    private struct CategoryRailLabel: View {
        let title: String
        let systemImage: String
        let selected: Bool
        @Environment(\.uiAccentColour) private var accentColour
        @Environment(\.controllerTextAppearance) private var textAppearance

        var body: some View {
            HStack(spacing: 10) {
                Image(systemName: systemImage)
                    .frame(width: OverlayTheme.rowIconWidth)
                    .foregroundStyle(accentColour)
                Text(title)
                    .font(.callout)
                    .controllerFocusedTextColor(
                        normal: selected
                            ? (textAppearance.normalColor ?? OverlayTheme.textPrimary)
                            : (textAppearance.secondaryColor ?? OverlayTheme.textSecondary)
                    )
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .frame(minHeight: 40)
            .padding(.horizontal, 12)
            .background(
                selected ? accentColour.opacity(0.16) : Color.clear,
                in: RoundedRectangle(cornerRadius: 8, style: .continuous)
            )
        }
    }
    private static let useGlobalSentinel = -1
    private static let upscaleUseGlobalSentinel: Float = -1.0
    private static let aspectUseGlobalSentinel = ""
    private static let trilinearUseGlobalSentinel = Int(Int32.min)
    private static let eeCycleRateUseGlobalSentinel = Int(Int32.min)
    private static let fastBootUseGlobalSentinel = -1
    private static let fastBootOff = 0
    private static let fastBootOn = 1

    /// A single snapshot of the game's INI. Its presence, including an empty
    /// dictionary, means the bridge already resolved the serial/CRC and loaded
    /// the file. Missing keys therefore mean "Use Global" and must not fall
    /// back to another ISO scan.
    private struct PerGameINISnapshot {
        let values: [String: Any]

        init?(_ info: [String: Any]) {
            guard let values = info["perGameINI"] as? [String: Any] else {
                return nil
            }
            self.values = values
        }

        private func key(_ section: String, _ name: String) -> String {
            "\(section)\n\(name)"
        }

        func number(_ section: String, _ name: String) -> NSNumber? {
            values[key(section, name)] as? NSNumber
        }

        func string(_ section: String, _ name: String) -> String? {
            values[key(section, name)] as? String
        }
    }

    let game: ISOEntry
    let controllerInput: MenuControllerInputRouter?
    let onDone: (() -> Void)?
    let savesToRunningGame: Bool
    private let initiallySelectsShaders: Bool

    /// Zero in the library, where this is a sheet and the system does its own avoidance.
    @Environment(\.overlayKeyboardOverlap) private var keyboardOverlap

    @State private var enabled: Bool
    @State private var upscaleMultiplier: Float
    @State private var aspectRatio: String
    @State private var textureFiltering: Int
    /// Tri-state: useGlobalSentinel, 0 off, 1 on. Was a Bool, which had no room to say "inherit".
    @State private var hardwareMipmapping: Int
    @State private var blendingAccuracy: Int
    @State private var interlaceMode: Int
    @State private var trilinearFiltering: Int
    @State private var halfPixelOffset: Int
    @State private var roundSprite: Int
    @State private var alignSprite: Int
    @State private var mergeSprite: Int
    @State private var wildArmsOffset: Int
    @State private var textureOffsetXOverride: Bool
    @State private var textureOffsetX: Int
    @State private var textureOffsetYOverride: Bool
    @State private var textureOffsetY: Int
    @State private var skipDrawStartOverride: Bool
    @State private var skipDrawStart: Int
    @State private var skipDrawEndOverride: Bool
    @State private var skipDrawEnd: Int
    @State private var globalVolumePercent: Int
    @State private var volumeOverride: Bool
    @State private var volumePercent: Int
    @State private var padLayoutIdentity: PadLayoutGameIdentity?
    @State private var showPadLayoutEditor = false
    @State private var eeCoreType: Int
    @State private var mtvu: Bool
    @State private var globalEECycleRate: Int
    @State private var eeCycleRate: Int
    @State private var globalEECycleSkip: Int
    @State private var eeCycleSkip: Int
    @State private var globalFastBoot: Bool
    @State private var fastBoot: Int
    @State private var hasGameSettingsIdentity: Bool
    @State private var enableCheats: Bool
    @State private var enablePatches: Bool
    @State private var enableGameFixes: Bool
    @State private var enableGameDBHardwareFixes: Bool
    // Per-game compatibility overrides (-1 = use global). Driven by the generic
    // per-game INI helper; -1 clears the per-game key so the global value applies.
    @State private var perGameFixes: [String: Int]
    @State private var perGameAAT: Int
    @State private var perGameTextureInsideRt: Int
    @State private var perGameDisableDepth: Int
    @State private var perGameRenderer: Int
    @State private var perGameFXAA: Int
    @State private var perGameUpscaler: Int
    @State private var perGameShadeBoost: Int
    @State private var perGameTVShader: Int
    @State private var perGameCASMode: Int
    @State private var perGameMaxAnisotropy: Int
    @State private var perGameCASSharpness: Int
    @State private var perGamePCRTCOffsets: Int
    @State private var perGameIntegerScaling: Int
    @State private var perGameSkipDupFrames: Int
    @State private var perGamePCRTCOverscan: Int
    @State private var perGamePCRTCAntiBlur: Int
    @State private var perGameDisableInterlaceOffset: Int
    @State private var perGameWidescreen: Int
    @State private var perGameNoInterlace: Int
    @State private var perGameShadeBoostBrightness: Int
    @State private var perGameShadeBoostContrast: Int
    @State private var perGameShadeBoostSaturation: Int
    @State private var perGameShadeBoostGamma: Int
    @State private var perGameShaderChain: Int
    @State private var perGameShaderPresetRef: String
    @State private var shaderPresetRequest: ShaderPresetBrowserRequest?
    @State private var perGameDithering: Int
    @State private var perGameFastForwardVolume: Int
    @State private var perGameIOP: Int
    @State private var perGameVU0: Int
    @State private var perGameVU1: Int
    @State private var perGameEEFpuRound: Int
    @State private var perGameVU0Round: Int
    @State private var perGameVU1Round: Int
    @State private var perGameEEClamp: Int
    @State private var perGameVUClamp: Int
    @State private var globalEEFpuRound: Int
    @State private var globalVU0Round: Int
    @State private var globalVU1Round: Int
    @State private var globalEEClamp: Int
    @State private var globalVUClamp: Int
    @State private var perGameHWDownloadMode: Int
    @State private var perGameCPUCLUT: Int
    @State private var perGameGPUTargetCLUT: Int
    @State private var perGameVsyncQueue: Int
    @State private var perGameLoadTextureReplacements: Int
    @State private var perGameLoadTextureReplacementsAsync: Int
    @State private var perGamePrecacheTextureReplacements: Int
    @State private var perGameSyncToHostRefresh: Int
    @State private var perGameBufferMS: Int
    @State private var perGameOutputLatencyMS: Int
    // Frame Pacing per-game overrides (-1 = use global). Limiter state remains
    // in Framerate/NominalScalar while presentation cadence has its own key.
    @State private var perGameFramePacingPreset: Int
    @State private var perGameFrameLimiter: Int
    @State private var perGameTargetFPS: Float
    @State private var statusMessage: String?
    @State private var showCheatsManager = false
    @State private var showResetAllConfirmation = false
    @State private var showDiscardConfirmation = false
    @State private var showFramePacingResetConfirmation = false
    @State private var savedFingerprint = 0
    @State private var saveFeedbackVisible = false
    @State private var openCategory: PerGameSettingsCategory = .general
    @State private var selectedControllerDetail: PerGameSettingsCategory?
    @State private var shaderWorkspaceCategoryID = "shaders"
    @State private var shaderWorkspaceControllerTargetOrder: [String] = []
    @State private var shaderParameterPreviewSequence: UInt64 = 0
    @State private var raEnabledOverride: Int
    @State private var raHardcoreOverride: Int
    @State private var livePreviewToken: String?
    @State private var livePreviewTask: Task<Void, Never>?
    @State private var livePreviewPendingFingerprint: Int?
    @State private var livePreviewChangeSequence: UInt64 = 0
    @State private var livePreviewInitialization: Task<String?, Never>?
    @State private var livePreviewSessionIsFinishing = false
    @State private var lastPreviewedFingerprint = 0
    @State private var suppressNextLivePreviewChange = false
    @State private var livePreviewPresentation:
        PerGameLivePreviewPresentation = .editing

    private let livePreviewDismissRequest: UInt64
    private let onLivePreviewPresentationChange:
        ((PerGameLivePreviewPresentation) -> Void)?
    private let onLivePreviewStatusChange:
        ((PerGameLivePreviewStatus) -> Void)?

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
        self.savesToRunningGame = savesToRunningGame
        self.initiallySelectsShaders = initiallySelectsShaders
        self.livePreviewDismissRequest = livePreviewDismissRequest
        self.onLivePreviewPresentationChange =
            onLivePreviewPresentationChange
        self.onLivePreviewStatusChange = onLivePreviewStatusChange
        // The runtime caller passes settings it already loaded through a VM-safe path so
        // this view never re-scans the disc image during init while a game is running.
        let info = preloadedSettings ?? ARMSX2Bridge.gameSettings(forISO: game.bootName)
        let iniSnapshot = PerGameINISnapshot(info)
        _enabled = State(
            initialValue: initiallySelectsGameController
                || initiallySelectsShaders
                || Self.boolValue(info["enabled"], defaultValue: false)
        )
        _openCategory = State(
            initialValue: initiallySelectsGameController
                ? .gameController
                : (initiallySelectsShaders ? .graphics : .general)
        )
        _selectedControllerDetail = State(
            initialValue: initiallySelectsGameController
                ? .gameController
                : nil
        )
        let inheritedVolume = Self.clampedVolume(Self.intValue(info["globalVolumePercent"], defaultValue: SettingsStore.defaultEmulatorVolumePercent))
        let loadedVolume = Self.clampedVolume(Self.intValue(info["volumePercent"], defaultValue: inheritedVolume))
        _globalVolumePercent = State(initialValue: inheritedVolume)
        _volumeOverride = State(initialValue: Self.boolValue(info["hasVolumeOverride"], defaultValue: false))
        _volumePercent = State(initialValue: loadedVolume)
        _padLayoutIdentity = State(initialValue: PadLayoutGameIdentity(
            serial: (info["serial"] as? String) ?? game.metadata["serial"],
            crc: (info["crc"] as? String) ?? game.metadata["crc"]
        ))
        _hasGameSettingsIdentity = State(initialValue: !PadLayoutGameIdentity.normalizedCRC((info["crc"] as? String) ?? game.metadata["crc"]).isEmpty)
        // Sentinel unless the file actually carries the key, so opening and saving the panel
        // cannot invent an override.
        _upscaleMultiplier = State(initialValue: Self.boolValue(info["hasUpscaleMultiplierOverride"], defaultValue: false) ? Self.floatValue(info["upscaleMultiplier"], defaultValue: 1.0) : Self.upscaleUseGlobalSentinel)
        _aspectRatio = State(initialValue: Self.boolValue(info["hasAspectRatioOverride"], defaultValue: false) ? Self.normalizedAspect(info["aspectRatio"] as? String) : Self.aspectUseGlobalSentinel)
        _textureFiltering = State(initialValue: Self.boolValue(info["hasTextureFilteringOverride"], defaultValue: false) ? Self.intValue(info["textureFiltering"], defaultValue: 2) : Self.useGlobalSentinel)
        _hardwareMipmapping = State(initialValue: Self.boolValue(info["hasHardwareMipmappingOverride"], defaultValue: false) ? (Self.boolValue(info["hardwareMipmapping"], defaultValue: true) ? 1 : 0) : Self.useGlobalSentinel)
        _blendingAccuracy = State(initialValue: Self.boolValue(info["hasBlendingAccuracyOverride"], defaultValue: false) ? Self.intValue(info["blendingAccuracy"], defaultValue: 1) : Self.useGlobalSentinel)
        _interlaceMode = State(initialValue: Self.boolValue(info["hasInterlaceModeOverride"], defaultValue: false) ? Self.intValue(info["interlaceMode"], defaultValue: 0) : Self.useGlobalSentinel)
        _trilinearFiltering = State(initialValue: Self.boolValue(info["hasTrilinearFilteringOverride"], defaultValue: false) ? Self.intValue(info["trilinearFiltering"], defaultValue: -1) : Self.trilinearUseGlobalSentinel)
        _halfPixelOffset = State(initialValue: Self.boolValue(info["hasHalfPixelOffsetOverride"], defaultValue: false) ? Self.intValue(info["halfPixelOffset"], defaultValue: 0) : Self.useGlobalSentinel)
        _roundSprite = State(initialValue: Self.boolValue(info["hasRoundSpriteOverride"], defaultValue: false) ? Self.intValue(info["roundSprite"], defaultValue: 0) : Self.useGlobalSentinel)
        _alignSprite = State(initialValue: Self.boolValue(info["hasAlignSpriteOverride"], defaultValue: false) ? (Self.boolValue(info["alignSprite"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel)
        _mergeSprite = State(initialValue: Self.boolValue(info["hasMergeSpriteOverride"], defaultValue: false) ? (Self.boolValue(info["mergeSprite"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel)
        _wildArmsOffset = State(initialValue: Self.boolValue(info["hasWildArmsOffsetOverride"], defaultValue: false) ? (Self.boolValue(info["wildArmsOffset"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel)
        _textureOffsetXOverride = State(initialValue: Self.boolValue(info["hasTextureOffsetXOverride"], defaultValue: false))
        _textureOffsetX = State(initialValue: Self.clampedTextureOffset(Self.intValue(info["textureOffsetX"], defaultValue: 0)))
        _textureOffsetYOverride = State(initialValue: Self.boolValue(info["hasTextureOffsetYOverride"], defaultValue: false))
        _textureOffsetY = State(initialValue: Self.clampedTextureOffset(Self.intValue(info["textureOffsetY"], defaultValue: 0)))
        let hasSkipDrawStartOverride = Self.boolValue(info["hasSkipDrawStartOverride"], defaultValue: false)
        let hasSkipDrawEndOverride = Self.boolValue(info["hasSkipDrawEndOverride"], defaultValue: false)
        let initialSkipDrawStart = Self.clampedSkipDraw(Self.intValue(info["skipDrawStart"], defaultValue: 0))
        let initialSkipDrawEnd = Self.normalizedSkipDrawEnd(
            start: initialSkipDrawStart,
            end: Self.intValue(info["skipDrawEnd"], defaultValue: 0),
            startOverride: hasSkipDrawStartOverride,
            endOverride: hasSkipDrawEndOverride
        )
        _skipDrawStartOverride = State(initialValue: hasSkipDrawStartOverride)
        _skipDrawStart = State(initialValue: initialSkipDrawStart)
        _skipDrawEndOverride = State(initialValue: hasSkipDrawEndOverride)
        _skipDrawEnd = State(initialValue: initialSkipDrawEnd)
        _eeCoreType = State(initialValue: Self.intValue(info["eeCoreType"], defaultValue: 2))
        _mtvu = State(initialValue: Self.boolValue(info["mtvu"], defaultValue: true))
        let inheritedEECycleRate = Self.clampedEECycleRate(Self.intValue(info["globalEECycleRate"], defaultValue: 0))
        _globalEECycleRate = State(initialValue: inheritedEECycleRate)
        _eeCycleRate = State(initialValue: Self.boolValue(info["hasEECycleRateOverride"], defaultValue: false) ? Self.clampedEECycleRate(Self.intValue(info["eeCycleRate"], defaultValue: inheritedEECycleRate)) : Self.eeCycleRateUseGlobalSentinel)
        let inheritedEECycleSkip = SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleSkip", defaultValue: 0)), to: 0...3)
        _globalEECycleSkip = State(initialValue: inheritedEECycleSkip)
        let inheritedFastBoot = Self.boolValue(info["globalFastBoot"], defaultValue: false)
        _globalFastBoot = State(initialValue: inheritedFastBoot)
        _fastBoot = State(initialValue: Self.boolValue(info["hasFastBootOverride"], defaultValue: false) ? (Self.boolValue(info["fastBoot"], defaultValue: inheritedFastBoot) ? Self.fastBootOn : Self.fastBootOff) : Self.fastBootUseGlobalSentinel)
        _enableCheats = State(initialValue: Self.boolValue(info["enableCheats"], defaultValue: false))
        _enablePatches = State(initialValue: Self.boolValue(info["enablePatches"], defaultValue: true))
        _enableGameFixes = State(initialValue: Self.boolValue(info["enableGameFixes"], defaultValue: true))
        _enableGameDBHardwareFixes = State(initialValue: Self.boolValue(info["enableGameDBHardwareFixes"], defaultValue: true))

        // Per-game compatibility overrides. The bridge preloads these from the game
        // settings INI in a single read, so the panel avoids dozens of repeated
        // per-game INI parses on open.
        var loadedFixes: [String: Int] = [:]
        let preloadedFixes = info["perGameFixes"] as? [String: Any] ?? [:]
        for option in SettingsStore.gameFixOptions {
            if let value = preloadedFixes[option.key] {
                loadedFixes[option.key] = Self.intValue(value, defaultValue: 0)
            } else {
                loadedFixes[option.key] = -1
            }
        }
        _perGameFixes = State(initialValue: loadedFixes)
        let hasPerGameAAT = Self.boolValue(info["hasPerGameAAT"], defaultValue: false)
        _perGameAAT = State(initialValue: hasPerGameAAT ? Self.intValue(info["perGameAAT"], defaultValue: 0) : -1)
        let hasPerGameTextureInsideRt = Self.boolValue(info["hasPerGameTextureInsideRt"], defaultValue: false)
        _perGameTextureInsideRt = State(initialValue: hasPerGameTextureInsideRt ? Self.intValue(info["perGameTextureInsideRt"], defaultValue: 0) : -1)
        _perGameDisableDepth = State(initialValue: Self.boolValue(info["hasPerGameDisableDepth"], defaultValue: false) ? (Self.boolValue(info["perGameDisableDepth"], defaultValue: false) ? 1 : 0) : Self.useGlobalSentinel)
        let hasPerGameRenderer = Self.boolValue(info["hasPerGameRenderer"], defaultValue: false)
        _perGameRenderer = State(initialValue: hasPerGameRenderer ? Self.intValue(info["perGameRenderer"], defaultValue: 17) : -1)
        let hasPerGameFXAA = Self.boolValue(info["hasPerGameFXAA"], defaultValue: false)
        _perGameFXAA = State(initialValue: hasPerGameFXAA ? Self.intValue(info["perGameFXAA"], defaultValue: 0) : -1)
        let hasPerGameUpscaler = Self.boolValue(info["hasPerGameUpscaler"], defaultValue: false)
        _perGameUpscaler = State(initialValue: hasPerGameUpscaler ? Self.intValue(info["perGameUpscaler"], defaultValue: 0) : -1)
        let hasPerGameShadeBoost = Self.boolValue(info["hasPerGameShadeBoost"], defaultValue: false)
        _perGameShadeBoost = State(initialValue: hasPerGameShadeBoost ? Self.intValue(info["perGameShadeBoost"], defaultValue: 0) : -1)
        let hasPerGameTVShader = Self.boolValue(info["hasPerGameTVShader"], defaultValue: false)
        _perGameTVShader = State(initialValue: hasPerGameTVShader ? Self.intValue(info["perGameTVShader"], defaultValue: 0) : -1)
        let hasPerGameCASMode = Self.boolValue(info["hasPerGameCASMode"], defaultValue: false)
        _perGameCASMode = State(initialValue: hasPerGameCASMode ? Self.intValue(info["perGameCASMode"], defaultValue: 0) : -1)
        let hasPerGameMaxAnisotropy = Self.boolValue(info["hasPerGameMaxAnisotropy"], defaultValue: false)
        _perGameMaxAnisotropy = State(initialValue: hasPerGameMaxAnisotropy ? Self.intValue(info["perGameMaxAnisotropy"], defaultValue: 0) : -1)
        let hasPerGameCASSharpness = Self.boolValue(info["hasPerGameCASSharpness"], defaultValue: false)
        _perGameCASSharpness = State(initialValue: hasPerGameCASSharpness ? SettingsStore.clamped(Self.intValue(info["perGameCASSharpness"], defaultValue: 50), to: SettingsStore.casSharpnessRange) : -1)
        let hasPerGamePCRTCOffsets = Self.boolValue(info["hasPerGamePCRTCOffsets"], defaultValue: false)
        _perGamePCRTCOffsets = State(initialValue: hasPerGamePCRTCOffsets ? Self.intValue(info["perGamePCRTCOffsets"], defaultValue: 0) : -1)
        let hasPerGameIntegerScaling = Self.boolValue(info["hasPerGameIntegerScaling"], defaultValue: false)
        _perGameIntegerScaling = State(initialValue: hasPerGameIntegerScaling ? Self.intValue(info["perGameIntegerScaling"], defaultValue: 0) : -1)
        let hasPerGameSkipDupFrames = Self.boolValue(info["hasPerGameSkipDupFrames"], defaultValue: false)
        _perGameSkipDupFrames = State(initialValue: hasPerGameSkipDupFrames ? Self.intValue(info["perGameSkipDupFrames"], defaultValue: 1) : -1)

        let hasPerGamePCRTCOverscan = Self.boolValue(info["hasPerGamePCRTCOverscan"], defaultValue: false)
        _perGamePCRTCOverscan = State(initialValue: hasPerGamePCRTCOverscan ? Self.intValue(info["perGamePCRTCOverscan"], defaultValue: 0) : -1)

        let hasPerGamePCRTCAntiBlur = Self.boolValue(info["hasPerGamePCRTCAntiBlur"], defaultValue: false)
        _perGamePCRTCAntiBlur = State(initialValue: hasPerGamePCRTCAntiBlur ? Self.intValue(info["perGamePCRTCAntiBlur"], defaultValue: 1) : -1)

        let hasPerGameDisableInterlaceOffset = Self.boolValue(info["hasPerGameDisableInterlaceOffset"], defaultValue: false)
        _perGameDisableInterlaceOffset = State(initialValue: hasPerGameDisableInterlaceOffset ? Self.intValue(info["perGameDisableInterlaceOffset"], defaultValue: 0) : -1)
        let perGameISO = game.bootName
        let useCurrent = savesToRunningGame
        _eeCycleSkip = State(initialValue: Self.loadedPerGameInt("EmuCore/Speedhacks", "EECycleSkip", globalDefault: Int32(inheritedEECycleSkip), useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameWidescreen = State(initialValue: Self.loadedPerGameBool("EmuCore", "EnableWideScreenPatches", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameNoInterlace = State(initialValue: Self.loadedPerGameBool("EmuCore", "EnableNoInterlacingPatches", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameShadeBoostBrightness = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Brightness", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameShadeBoostContrast = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Contrast", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameShadeBoostSaturation = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Saturation", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameShadeBoostGamma = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "ShadeBoost_Gamma", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameShaderChain = State(initialValue: PerGameShaderSelection.loadedChain(useCurrent: useCurrent, iso: perGameISO))
        _perGameShaderPresetRef = State(initialValue: PerGameShaderSelection.loadedPresetRef(useCurrent: useCurrent, iso: perGameISO))
        _perGameDithering = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "dithering_ps2", globalDefault: 2, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameFastForwardVolume = State(initialValue: Self.clampedPerGameInt(Self.loadedPerGameInt("SPU2/Output", "FastForwardVolume", globalDefault: 100, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.fastForwardVolumeRange))
        _perGameIOP = State(initialValue: Self.loadedPerGameBool("EmuCore/CPU/Recompiler", "EnableIOP", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameVU0 = State(initialValue: Self.loadedPerGameBool("EmuCore/CPU/Recompiler", "EnableVU0", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameVU1 = State(initialValue: Self.loadedPerGameBool("EmuCore/CPU/Recompiler", "EnableVU1", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameEEFpuRound = State(initialValue: Self.loadedPerGameInt("EmuCore/CPU", "FPU.Roundmode", globalDefault: 3, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameVU0Round = State(initialValue: Self.loadedPerGameInt("EmuCore/CPU", "VU0.Roundmode", globalDefault: 3, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameVU1Round = State(initialValue: Self.loadedPerGameInt("EmuCore/CPU", "VU1.Roundmode", globalDefault: 3, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameEEClamp = State(initialValue: Self.loadedPerGameEEClamp(useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameVUClamp = State(initialValue: Self.loadedPerGameVUClamp(useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _globalEEFpuRound = State(initialValue: SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "FPU.Roundmode", defaultValue: 3)), to: 0...3))
        _globalVU0Round = State(initialValue: SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "VU0.Roundmode", defaultValue: 3)), to: 0...3))
        _globalVU1Round = State(initialValue: SettingsStore.clamped(Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "VU1.Roundmode", defaultValue: 3)), to: 0...3))
        _globalEEClamp = State(initialValue: SettingsStore.eeClampModeFromBools(
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuOverflow", defaultValue: true),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuExtraOverflow", defaultValue: false),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "fpuFullMode", defaultValue: false)))
        _globalVUClamp = State(initialValue: SettingsStore.vuClampModeFromBools(
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0Overflow", defaultValue: true),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0ExtraOverflow", defaultValue: false),
            ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "vu0SignOverflow", defaultValue: false)))
        _perGameHWDownloadMode = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "HWDownloadMode", globalDefault: 0, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameCPUCLUT = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "UserHacks_CPUCLUTRender", globalDefault: 0, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameGPUTargetCLUT = State(initialValue: Self.loadedPerGameInt("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", globalDefault: 0, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameVsyncQueue = State(initialValue: Self.clampedPerGameInt(Self.loadedPerGameInt("EmuCore/GS", "VsyncQueueSize", globalDefault: 8, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.vsyncQueueRange))
        _perGameLoadTextureReplacements = State(initialValue: Self.loadedPerGameBool("EmuCore/GS", "LoadTextureReplacements", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameLoadTextureReplacementsAsync = State(initialValue: Self.loadedPerGameBool("EmuCore/GS", "LoadTextureReplacementsAsync", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGamePrecacheTextureReplacements = State(initialValue: Self.loadedPerGameBool("EmuCore/GS", "PrecacheTextureReplacements", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameSyncToHostRefresh = State(initialValue: Self.loadedPerGameBool("EmuCore/GS", "SyncToHostRefreshRate", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _perGameBufferMS = State(initialValue: Self.clampedPerGameInt(Self.loadedPerGameInt("SPU2/Output", "BufferMS", globalDefault: 50, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.audioBufferMsRange))
        _perGameOutputLatencyMS = State(initialValue: Self.clampedPerGameInt(Self.loadedPerGameInt("SPU2/Output", "OutputLatencyMS", globalDefault: 20, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot), to: SettingsStore.audioOutputLatencyMsRange))
        let _globalFramePacingPreset = Int32(SettingsStore.shared.framePacingPreset.rawValue)
        _perGameFramePacingPreset = State(initialValue: Self.loadedPerGameInt("ARMSX2iOS/FramePacing", "Preset", globalDefault: _globalFramePacingPreset, useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        let _fpLimiter = Self.loadedPerGameFrameLimiter(useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot)
        _perGameFrameLimiter = State(initialValue: _fpLimiter.limiter)
        _perGameTargetFPS = State(initialValue: _fpLimiter.fps)
        _raEnabledOverride = State(initialValue: Self.loadedPerGameBool("Achievements", "Enabled", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        _raHardcoreOverride = State(initialValue: Self.loadedPerGameBool("Achievements", "ChallengeMode", useCurrent: useCurrent, iso: perGameISO, snapshot: iniSnapshot))
        let initialFingerprint = perGameFingerprint()
        let initialLivePreviewFingerprint = perGameLivePreviewFingerprint()
        _savedFingerprint = State(initialValue: initialFingerprint)
        _livePreviewToken = State(initialValue: nil)
        _livePreviewTask = State(initialValue: nil)
        _livePreviewPendingFingerprint = State(initialValue: nil)
        _lastPreviewedFingerprint = State(
            initialValue: initialLivePreviewFingerprint
        )
    }

    /// Encodes the current editable per-game state so Save can be gated on real changes.
    /// Virtual Pad values are left out on purpose: that tab writes as you edit it, so
    /// there is never anything of its own left for Save to commit.
    private func perGameFingerprint() -> Int {
        var hasher = Hasher()
        hasher.combine(enabled)
        hasher.combine(textureOffsetXOverride)
        hasher.combine(textureOffsetYOverride)
        hasher.combine(skipDrawStartOverride)
        hasher.combine(skipDrawEndOverride)
        hasher.combine(volumeOverride)
        hasher.combine(perGameLivePreviewFingerprint())
        return hasher.finalize()
    }

    /// The override master switch changes what Save persists, but it is not a
    /// visual/emulation value which should interrupt gameplay for a preview.
    private func perGameLivePreviewFingerprint() -> Int {
        var hasher = Hasher()
        hasher.combine(upscaleMultiplier)
        hasher.combine(aspectRatio)
        hasher.combine(textureFiltering)
        hasher.combine(hardwareMipmapping)
        hasher.combine(blendingAccuracy)
        hasher.combine(interlaceMode)
        hasher.combine(trilinearFiltering)
        hasher.combine(halfPixelOffset)
        hasher.combine(roundSprite)
        hasher.combine(alignSprite)
        hasher.combine(mergeSprite)
        hasher.combine(wildArmsOffset)
        hasher.combine(textureOffsetX)
        hasher.combine(textureOffsetY)
        hasher.combine(skipDrawStart)
        hasher.combine(skipDrawEnd)
        hasher.combine(volumePercent)
        hasher.combine(eeCoreType)
        hasher.combine(mtvu)
        hasher.combine(eeCycleRate)
        hasher.combine(eeCycleSkip)
        hasher.combine(fastBoot)
        hasher.combine(enableCheats)
        hasher.combine(enablePatches)
        hasher.combine(enableGameFixes)
        hasher.combine(enableGameDBHardwareFixes)
        hasher.combine(perGameAAT)
        hasher.combine(perGameTextureInsideRt)
        hasher.combine(perGameDisableDepth)
        hasher.combine(perGameRenderer)
        hasher.combine(perGameFXAA)
        hasher.combine(perGameUpscaler)
        hasher.combine(perGameShadeBoost)
        hasher.combine(perGameTVShader)
        hasher.combine(perGameCASMode)
        hasher.combine(perGameMaxAnisotropy)
        hasher.combine(perGameCASSharpness)
        hasher.combine(perGamePCRTCOffsets)
        hasher.combine(perGameIntegerScaling)
        hasher.combine(perGameSkipDupFrames)
        hasher.combine(perGamePCRTCOverscan)
        hasher.combine(perGamePCRTCAntiBlur)
        hasher.combine(perGameDisableInterlaceOffset)
        hasher.combine(perGameWidescreen)
        hasher.combine(perGameNoInterlace)
        hasher.combine(perGameShadeBoostBrightness)
        hasher.combine(perGameShadeBoostContrast)
        hasher.combine(perGameShadeBoostSaturation)
        hasher.combine(perGameShadeBoostGamma)
        hasher.combine(perGameShaderChain)
        hasher.combine(perGameShaderPresetRef)
        hasher.combine(shaderParameterPreviewSequence)
        hasher.combine(perGameDithering)
        hasher.combine(perGameFastForwardVolume)
        hasher.combine(perGameIOP)
        hasher.combine(perGameVU0)
        hasher.combine(perGameVU1)
        hasher.combine(perGameHWDownloadMode)
        hasher.combine(perGameCPUCLUT)
        hasher.combine(perGameGPUTargetCLUT)
        hasher.combine(perGameVsyncQueue)
        hasher.combine(perGameLoadTextureReplacements)
        hasher.combine(perGameLoadTextureReplacementsAsync)
        hasher.combine(perGamePrecacheTextureReplacements)
        hasher.combine(perGameSyncToHostRefresh)
        hasher.combine(perGameBufferMS)
        hasher.combine(perGameOutputLatencyMS)
        hasher.combine(perGameEEFpuRound)
        hasher.combine(perGameVU0Round)
        hasher.combine(perGameVU1Round)
        hasher.combine(perGameEEClamp)
        hasher.combine(perGameVUClamp)
        hasher.combine(raEnabledOverride)
        hasher.combine(raHardcoreOverride)
        hasher.combine(perGameFramePacingPreset)
        hasher.combine(perGameFrameLimiter)
        hasher.combine(perGameTargetFPS)
        for option in SettingsStore.gameFixOptions {
            hasher.combine(perGameFixes[option.key] ?? -1)
        }
        return hasher.finalize()
    }

    private var hasPendingChanges: Bool {
        perGameFingerprint() != savedFingerprint
    }

    /// Status shown after Save. In-game most settings apply immediately via the
    /// live-apply bridge path; a few (renderer, recompiler toggles, MTVU) need a
    /// reset. From the library nothing is running, so the next boot is the earliest.
    private var postSaveMessage: String {
        if !enabled {
            return settings.localized("Per-game overrides cleared.")
        }
        let serial = game.metadata["serial"] ?? game.name
        let suffix = savesToRunningGame
            ? settings.localized("Saved — changes apply now. Renderer and recompiler settings need a reset.")
            : settings.localized("Reset or relaunch the game to apply.")
        return String(format: settings.localized("Saved for %1$@. %2$@"), serial, suffix)
    }

    /// Clears every per-game override by disabling the master toggle and saving; the
    /// save path deletes all per-game keys so the global values apply on next boot.
    /// Virtual Pad state isn't part of that path — layout and skin live in the preset
    /// store, stick inversion is written as you edit it — so clear those by hand here.
    private func resetAllOverrides() {
        if let padLayoutIdentity {
            layoutPresets.clearVPadOverrides(for: padLayoutIdentity)
        }
        clearStickInversionOverrides()
        enabled = false
        save()
    }

    private func clearStickInversionOverrides() {
        for key in SettingsStore.stickInversionKeys {
            Self.clearPerGameValue("ARMSX2iOS/UI", key, useCurrent: savesToRunningGame, iso: game.bootName)
        }
        if savesToRunningGame {
            settings.reloadStickInversionOverrides()
        }
    }

    /// Whether OPH Flag Hack is effectively on for this game: a per-game override of 1, or
    /// the global value when the per-game override is set to use-global (-1). Used to hide
    /// the higher-resolution OPH suggestion once OPH is in effect. Passed to GraphicsTab.
    private var ophFlagHackEffective: Bool {
        let perGame = perGameFixes["OPHFlagHack"] ?? -1
        if perGame == 1 { return true }
        if perGame == 0 { return false }
        return settings.gameFixEnabled("OPHFlagHack")
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
            // The portrait and landscape presentations are different focus
            // graphs (one NavigationStack versus two independent scroll views).
            // Recreate probes and the navigation scope atomically at rotation
            // so Cross can never activate a retained control from the old tree.
            .id(layout)
            .onChange(of: layout) { _, _ in
                selectedControllerDetail = nil
            }
            .controllerAccessibilityTargetOrder(
                perGameControllerTargetOrder
            )
            // Keep the shared accessibility graph on the controller confirmation
            // itself instead of the still-rendered settings underneath it.
            .accessibilityHidden(
                activeControllerAlertKind != nil
            )
            .environment(
                \.controllerAccessibilityTargetsSuppressed,
                activeControllerAlertKind != nil
            )
            .overlay {
                if let kind = activeControllerAlertKind {
                    ControllerNavigationAlert(
                        title: controllerAlertTitle(for: kind),
                        message: controllerAlertMessage(for: kind),
                        actions: controllerAlertActions(for: kind),
                        selectedIndex: 0,
                        onSelect: { index in
                            performControllerAlertAction(index, for: kind)
                        },
                        onDismiss: {
                            dismissControllerAlert(kind)
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
                    && !livePreviewSessionIsFinishing,
                scopeKey: perGameControllerScopeKey(layout: layout),
                priority: 220,
                orbStyle: .plain,
                onBack: handleSharedControllerBack,
                directionalLinks: perGameControllerDirectionalLinks,
                prioritizesDirectionalLinks: true,
                confinesHorizontalFocusMovement: true,
                usesNativeFocusEngine: false,
                // Per-Game rows all publish stable semantic probes. Keeping
                // this surface explicit-only prevents a nested native control
                // from becoming a second focus owner for the same setting.
                usesExplicitTargetGeometryOnly: true,
                preservesFocusDuringRightStickScrolling: false,
                focusScrollBehavior: .maintainWithinViewport,
                scrollAnimationDuration: 0.16,
                focusTopAlignmentMargin: controllerColumn == .detail ? 84 : 24,
                focusBottomAlignmentMargin: controllerColumn == .detail ? 84 : 24,
                onAdjustFocusedTarget: { _, value, increases in
                    publishLivePreviewStatus(
                        value: value,
                        countsDown: !increases
                    )
                },
                preferredInitialFocusLabel:
                    perGamePreferredInitialFocusLabel,
                preferredTrailingFocusLabels: [
                    Self.closeControllerTargetID,
                    Self.saveControllerTargetID,
                ]
            )
            .task(id: perGameControllerScopeKey(layout: layout)) {
                guard controllerInput?.hasConnectedController == true,
                      controllerInput?.isControllerNavigationEnabled == true else {
                    return
                }
                // Scope registration is passive. Enter the newly selected
                // column exactly once after its first real row has mounted.
                await Task.yield()
                guard !Task.isCancelled else { return }
                _ = controllerInput?.requestNavigationSessionEntry(
                    preferLast: false,
                    matchingScopePrefix: perGameControllerScopeKey(
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
        // Embedded portrait destinations need a local scheme, not a preference
        // which can be overridden by the presenting gameplay scene.
        .environment(\.colorScheme, .dark)
        .contextMenuPanelTextAppearance()
        .environment(\.perGameOverrideWillActivate) {
            suppressNextLivePreviewChange = true
        }
        .allowsHitTesting(!livePreviewSessionIsFinishing)
        .tint(accentColour)
        .onChange(of: hasPendingChanges) { _, pending in
            if pending {
                saveFeedbackVisible = false
            }
        }
        .onChange(of: activeControllerAlertKind) { previous, kind in
            guard let kind, kind != previous else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .onAppear {
            beginLivePreviewSessionIfNeeded()
        }
        .onChange(of: perGameLivePreviewFingerprint()) { _, fingerprint in
            if suppressNextLivePreviewChange {
                suppressNextLivePreviewChange = false
                lastPreviewedFingerprint = fingerprint
                return
            }
            scheduleLivePreview(for: fingerprint)
        }
        .onChange(of: livePreviewDismissRequest) { _, _ in
            guard livePreviewPresentation.hidesSettingsUI else { return }
            // The presentation owner captured this button before it could
            // reach the hidden editor. Cancelling joins any in-flight native
            // apply, restores the temporal state, and remounts the panel.
            livePreviewTask?.cancel()
        }
        .onChange(of: settings.temporalSaveStateToLivePreviewChanges) {
            _, enabled in
            if enabled {
                if livePreviewToken == nil {
                    beginLivePreviewSessionIfNeeded()
                } else {
                    scheduleLivePreview(
                        for: perGameLivePreviewFingerprint()
                    )
                }
            } else {
                livePreviewTask?.cancel()
                if livePreviewTask == nil {
                    publishLivePreviewPresentation(.editing)
                }
            }
        }
        .onDisappear {
            let wasAlreadyFinishing = livePreviewSessionIsFinishing
            livePreviewSessionIsFinishing = true
            let previewTask = livePreviewTask
            let initialization = livePreviewInitialization
            let existingToken = livePreviewToken
            previewTask?.cancel()
            guard savesToRunningGame,
                  !wasAlreadyFinishing else { return }
            // A system-driven dismissal is cancellation, never an implicit save.
            Task { @MainActor in
                await previewTask?.value
                let initializedToken = await initialization?.value
                guard let token = existingToken ?? initializedToken else { return }
                _ = await ARMSX2Bridge.finishPerGameLivePreview(
                    token: token,
                    preserveSettings: false
                )
            }
        }
        .interactiveDismissDisabled(hasPendingChanges)
        .fullScreenCover(isPresented: $showPadLayoutEditor) {
            PadLayoutEditView(
                onDismiss: { showPadLayoutEditor = false },
                context: perGamePadLayoutEditorContext
            )
        }
        .sheet(item: $shaderPresetRequest, onDismiss: {
            if !perGameShaderPresetRef.isEmpty, ShaderPresetLibrary.resolve(perGameShaderPresetRef) == nil {
                perGameShaderPresetRef = ""
            }
        }) { _ in
            NavigationStack {
                ShaderPresetBrowserView(
                    title: settings.localized("Shader Presets"),
                    folder: nil,
                    selectedToken: perGameShaderPresetRef,
                    localized: { settings.localized($0) },
                    onSelect: { token in
                        perGameShaderPresetRef = token
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
                launchContext: savesToRunningGame ? .inGame : .library,
                controllerInput: controllerInput
            )
        }
        .confirmationDialog(settings.localized("Reset all per-game overrides?"),
                            isPresented: nativeControllerDialogBinding($showResetAllConfirmation),
                            titleVisibility: .visible) {
            Button(settings.localized("Reset All"), role: .destructive) {
                resetAllOverrides()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("All per-game overrides for this title are removed; global settings apply on the next boot or reset."))
        }
        .confirmationDialog(settings.localized("Discard your changes?"),
                            isPresented: nativeControllerDialogBinding($showDiscardConfirmation),
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
                            isPresented: nativeControllerDialogBinding($showFramePacingResetConfirmation),
            titleVisibility: .visible) {
            Button(settings.localized("Clear"), role: .destructive) {
                clearFramePacingOverrides()
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
            NavigationStack(path: portraitNavigationPath) {
                portraitCategoryMenu
                    .environment(
                        \.controllerAccessibilityTargetsSuppressed,
                        controllerColumn == .detail
                            || controllerAlertSuppressesContentTargets
                    )
                    .navigationDestination(for: PerGameSettingsCategory.self) { category in
                        detailContent(category)
                            // A pushed portrait Form can otherwise install an
                            // opaque/empty navigation backing above the panel's
                            // glass. Give the destination its own regular Liquid
                            // Glass layer so every sub-setting retains the same
                            // non-clear surface.
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
                                controllerColumn == .categories
                                    || controllerAlertSuppressesContentTargets
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
            presetRef: $perGameShaderPresetRef,
            perGameChain: $perGameShaderChain,
            settings: settings,
            controllerInput: controllerInput,
            savesToRunningGame: savesToRunningGame,
            onLivePreviewChange: requestShaderLivePreview,
            selectedCategoryID: $shaderWorkspaceCategoryID,
            detailPresented: shaderWorkspaceDetailPresented,
            reportedControllerTargetOrder:
                $shaderWorkspaceControllerTargetOrder
        )
    }

    private var shaderWorkspaceDetailPresented: Binding<Bool> {
        Binding(
            get: { selectedControllerDetail != nil },
            set: { presented in
                selectedControllerDetail = presented ? .graphics : nil
            }
        )
    }

    @MainActor
    private func requestShaderLivePreview(_ label: String, _ value: String) {
        shaderParameterPreviewSequence &+= 1
        publishLivePreviewStatus(value: value, countsDown: false)
    }

    @ViewBuilder
    private func landscapeSettingsSplit(availableWidth: CGFloat) -> some View {
        // Adaptive rail: wide enough for the longest category label ("Fixes & Compatibility")
        // on large panels, clamped so narrow panels keep a usable detail pane. At .callout the
        // longest label needs ~190pt including its icon column and padding.
        let railWidth = min(200, max(168, availableWidth * 0.24))
        VStack(spacing: 0) {
            landscapeHeader
            OverlayTheme.separator.frame(height: 0.5)
            HStack(spacing: 0) {
                categoryRail
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
            Image(systemName: enabled ? "slider.horizontal.3" : "power")
                .font(.system(size: 18))
                .foregroundStyle(accentColour)
            Text(settings.localized(panelTitleKey))
                .font(.callout.weight(.semibold))
                .foregroundStyle(panelTextColour)
            Text(displayName)
                .font(.caption)
                .foregroundStyle(panelSecondaryTextColour)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: 8)
            if hasPendingChanges {
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
            id: Self.detailBackControllerTargetID,
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
    private var categoryRail: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(PerGameSettingsCategory.allCases) { category in
                    let selected = openCategory == category
                    Button {
                        openCategory = category
                    } label: {
                        CategoryRailLabel(
                            title: settings.localized(category.titleKey),
                            systemImage: category.systemImage,
                            selected: selected
                        )
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: landscapeCategoryControllerTargetID(category),
                        label: settings.localized(category.titleKey)
                    ) {
                        enterControllerDetail(category)
                    }
                    .focusEffectDisabled()
                }
            }
            .padding(.vertical, 8)
        }
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 220,
                isEnabled: controllerColumn == .categories,
                searchesNearbyScrollViews: true
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
        }
        .environment(
            \.controllerAccessibilityTargetsSuppressed,
            controllerColumn == .detail
                || controllerAlertSuppressesContentTargets
        )
    }

    @ViewBuilder
    private var detailPane: some View {
        detailContent(openCategory)
            .pickerStyle(.menu)
            .environment(
                \.controllerAccessibilityTargetsSuppressed,
                controllerColumn == .categories
                    || controllerAlertSuppressesContentTargets
            )
    }

    private var portraitCategoryMenu: some View {
        Form {
            Section {
                ForEach(PerGameSettingsCategory.allCases) { category in
                    Button {
                        enterControllerDetail(category)
                    } label: {
                        HStack(spacing: 12) {
                            Label(
                                settings.localized(category.titleKey),
                                systemImage: category.systemImage
                            )
                            Spacer(minLength: 8)
                            Image(systemName: "chevron.right")
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(accentColour)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: landscapeCategoryControllerTargetID(category),
                        label: settings.localized(category.titleKey)
                    ) {
                        enterControllerDetail(category)
                    }
                    .focusEffectDisabled()
                }
            }
        }
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 220,
                isEnabled: controllerColumn == .categories,
                searchesNearbyScrollViews: true
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
        }
        .scrollContentBackground(.hidden)
    }

    @ViewBuilder
    private func detailContent(_ category: PerGameSettingsCategory) -> some View {
        switch category {
        case .general:  generalTab
        case .graphics: graphicsTab
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
            enabled: $enabled,
            hasGameSettingsIdentity: $hasGameSettingsIdentity,
            showResetAllConfirmation: $showResetAllConfirmation,
            statusMessage: $statusMessage,
            displayName: displayName,
            hasPendingChanges: hasPendingChanges,
            savesToRunningGame: savesToRunningGame,
            game: game,
            settings: settings
        )
    }

    private var graphicsTab: some View {
        GraphicsTab(
            enabled: $enabled,
            enableGameDBHardwareFixes: $enableGameDBHardwareFixes,
            trilinearUseGlobalSentinel: Self.trilinearUseGlobalSentinel,
            ophFlagHackEffective: ophFlagHackEffective,
            perGameRenderer: $perGameRenderer,
            upscaleMultiplier: $upscaleMultiplier,
            aspectRatio: $aspectRatio,
            textureFiltering: $textureFiltering,
            hardwareMipmapping: $hardwareMipmapping,
            blendingAccuracy: $blendingAccuracy,
            interlaceMode: $interlaceMode,
            trilinearFiltering: $trilinearFiltering,
            halfPixelOffset: $halfPixelOffset,
            roundSprite: $roundSprite,
            alignSprite: $alignSprite,
            mergeSprite: $mergeSprite,
            wildArmsOffset: $wildArmsOffset,
            textureOffsetXOverride: $textureOffsetXOverride,
            textureOffsetX: $textureOffsetX,
            textureOffsetYOverride: $textureOffsetYOverride,
            textureOffsetY: $textureOffsetY,
            skipDrawStartOverride: $skipDrawStartOverride,
            skipDrawStart: $skipDrawStart,
            skipDrawEndOverride: $skipDrawEndOverride,
            skipDrawEnd: $skipDrawEnd,
            perGameFXAA: $perGameFXAA,
            perGameUpscaler: $perGameUpscaler,
            perGameShadeBoost: $perGameShadeBoost,
            perGameShadeBoostBrightness: $perGameShadeBoostBrightness,
            perGameShadeBoostContrast: $perGameShadeBoostContrast,
            perGameShadeBoostSaturation: $perGameShadeBoostSaturation,
            perGameShadeBoostGamma: $perGameShadeBoostGamma,
            perGameShaderChain: $perGameShaderChain,
            perGameShaderPresetRef: $perGameShaderPresetRef,
            perGameDithering: $perGameDithering,
            perGameTVShader: $perGameTVShader,
            perGameCASMode: $perGameCASMode,
            perGameMaxAnisotropy: $perGameMaxAnisotropy,
            perGameCASSharpness: $perGameCASSharpness,
            perGamePCRTCOffsets: $perGamePCRTCOffsets,
            perGameIntegerScaling: $perGameIntegerScaling,
            perGameSkipDupFrames: $perGameSkipDupFrames,
            perGamePCRTCOverscan: $perGamePCRTCOverscan,
            perGamePCRTCAntiBlur: $perGamePCRTCAntiBlur,
            perGameDisableInterlaceOffset: $perGameDisableInterlaceOffset,
            perGameHWDownloadMode: $perGameHWDownloadMode,
            perGameDisableDepth: $perGameDisableDepth,
            perGameCPUCLUT: $perGameCPUCLUT,
            perGameGPUTargetCLUT: $perGameGPUTargetCLUT,
            perGameLoadTextureReplacements: $perGameLoadTextureReplacements,
            perGameLoadTextureReplacementsAsync: $perGameLoadTextureReplacementsAsync,
            perGamePrecacheTextureReplacements: $perGamePrecacheTextureReplacements,
            savesToRunningGame: savesToRunningGame,
            onBrowseShaderPreset: { shaderPresetRequest = ShaderPresetBrowserRequest() },
            settings: settings,
            showsOnlyShaders: initiallySelectsShaders
        )
    }

    private var audioTab: some View {
        AudioTab(
            enabled: $enabled,
            volumeOverride: $volumeOverride,
            volumePercent: $volumePercent,
            globalVolumePercent: $globalVolumePercent,
            perGameFastForwardVolume: $perGameFastForwardVolume,
            settings: settings
        )
    }

    private var framePacingTab: some View {
        FramePacingTab(
            enabled: $enabled,
            settings: settings,
            perGameFramePacingPreset: $perGameFramePacingPreset,
            perGameFrameLimiter: $perGameFrameLimiter,
            perGameTargetFPS: $perGameTargetFPS,
            perGameVsyncQueue: $perGameVsyncQueue,
            perGameSyncToHostRefresh: $perGameSyncToHostRefresh,
            perGameBufferMS: $perGameBufferMS,
            perGameOutputLatencyMS: $perGameOutputLatencyMS,
            showResetConfirmation: $showFramePacingResetConfirmation
        )
    }

    private var cpuTab: some View {
        CPUTab(
            enabled: $enabled,
            eeCoreType: $eeCoreType,
            mtvu: $mtvu,
            eeCycleRate: $eeCycleRate,
            globalEECycleRate: $globalEECycleRate,
            eeCycleSkip: $eeCycleSkip,
            globalEECycleSkip: $globalEECycleSkip,
            fastBoot: $fastBoot,
            globalFastBoot: $globalFastBoot,
            perGameIOP: $perGameIOP,
            perGameVU0: $perGameVU0,
            perGameVU1: $perGameVU1,
            perGameEEFpuRound: $perGameEEFpuRound,
            perGameVU0Round: $perGameVU0Round,
            perGameVU1Round: $perGameVU1Round,
            perGameEEClamp: $perGameEEClamp,
            perGameVUClamp: $perGameVUClamp,
            savesToRunningGame: savesToRunningGame,
            settings: settings,
            eeCycleRateUseGlobalSentinel: Self.eeCycleRateUseGlobalSentinel,
            fastBootUseGlobalSentinel: Self.fastBootUseGlobalSentinel,
            fastBootOff: Self.fastBootOff,
            fastBootOn: Self.fastBootOn,
            globalEEFpuRound: globalEEFpuRound,
            globalVU0Round: globalVU0Round,
            globalVU1Round: globalVU1Round,
            globalEEClamp: globalEEClamp,
            globalVUClamp: globalVUClamp
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
            padLayoutIdentity: $padLayoutIdentity,
            showPadLayoutEditor: $showPadLayoutEditor,
            layoutPresets: layoutPresets,
            skinLibrary: skinLibrary,
            savesToRunningGame: savesToRunningGame,
            iso: game.bootName,
            hasGameSettingsIdentity: hasGameSettingsIdentity
        )
    }

    private var fixesTab: some View {
        FixesTab(
            enabled: $enabled,
            perGameAAT: $perGameAAT,
            perGameTextureInsideRt: $perGameTextureInsideRt,
            perGameFixes: $perGameFixes,
            savesToRunningGame: savesToRunningGame,
            settings: settings
        )
    }

    private var cheatsTab: some View {
        CheatsTab(
            enabled: $enabled,
            enableGameFixes: $enableGameFixes,
            enableGameDBHardwareFixes: $enableGameDBHardwareFixes,
            perGameWidescreen: $perGameWidescreen,
            perGameNoInterlace: $perGameNoInterlace,
            showCheatsManager: $showCheatsManager,
            savesToRunningGame: savesToRunningGame,
            settings: settings
        )
    }

    private var retroAchievementsTab: some View {
        RetroAchievementsTab(
            enabled: $enabled,
            raEnabledOverride: $raEnabledOverride,
            raHardcoreOverride: $raHardcoreOverride,
            settings: settings
        )
    }

    private func saveCancelFooter(compact: Bool) -> some View {
        HStack(spacing: 12) {
            let closeLabel = settings.localized(
                hasPendingChanges ? "Cancel" : "Close"
            )
            Button(closeLabel) {
                attemptCancel()
            }
            .buttonStyle(.bordered)
            .controlSize(compact ? .regular : .large)
            .frame(maxWidth: .infinity)
            .controllerAccessibilityActionTarget(
                id: Self.closeControllerTargetID,
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
                id: Self.saveControllerTargetID,
                label: settings.localized("Save"),
                action: { save() }
            )
            // Keep the controller probe inside the disabled environment. Save
            // unregisters from the active graph immediately after the saved
            // fingerprint catches up with the edited state.
            .disabled(!hasGameSettingsIdentity || !hasPendingChanges)
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

    /// Cancel/dismiss guard: confirm before discarding unsaved edits. Reads `hasPendingChanges`
    /// only — it does not alter Save/Cancel gating or the fingerprint logic.
    private func attemptCancel(playsSemanticAudio: Bool = true) {
        if hasPendingChanges {
            if playsSemanticAudio {
                MenuAudioPackManager.shared.playEvent(.uiToast)
            }
            showDiscardConfirmation = true
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
        if let kind = activeControllerAlertKind {
            dismissControllerAlert(kind)
            return true
        }
        if controllerColumn == .detail {
            selectedControllerDetail = nil
            return true
        }
        attemptCancel(playsSemanticAudio: false)
        return true
    }

    private var activeControllerAlertKind: PerGameControllerAlertKind? {
        if showResetAllConfirmation { return .resetAll }
        if showDiscardConfirmation { return .discardChanges }
        if showFramePacingResetConfirmation { return .clearFramePacing }
        return nil
    }

    private var controllerAlertSuppressesContentTargets: Bool {
        activeControllerAlertKind != nil
    }

    private func perGameControllerScopeKey(
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

    private func landscapeCategoryControllerTargetID(
        _ category: PerGameSettingsCategory
    ) -> String {
        "per-game.category.\(category.rawValue)"
    }

    private func enterControllerDetail(_ category: PerGameSettingsCategory) {
        openCategory = category
        selectedControllerDetail = category
    }

    private func returnToControllerCategories(playsSemanticAudio: Bool) {
        guard selectedControllerDetail != nil else { return }
        if playsSemanticAudio {
            MenuAudioPackManager.shared.playEvent(.return)
        }
        selectedControllerDetail = nil
    }

    private func navigateBack(playsSemanticAudio: Bool) {
        if selectedControllerDetail != nil {
            returnToControllerCategories(playsSemanticAudio: playsSemanticAudio)
        } else {
            attemptCancel(playsSemanticAudio: playsSemanticAudio)
        }
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

    private var perGameControllerTargetOrder: [String] {
        [Self.detailBackControllerTargetID] + activePerGameContentOrder + [
            Self.closeControllerTargetID,
            Self.saveControllerTargetID,
        ]
    }

    /// Per-Game Settings is visually a vertical content column followed by a
    /// two-button horizontal footer. Declare that geometry semantically so the
    /// linear fallback cannot reinterpret Close/Save as another vertical row.
    private var perGameControllerDirectionalLinks:
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
            if hasGameSettingsIdentity {
                order.append("per-game.general.reset-all")
            }
            order.append("per-game.general.live-preview")
            if settings.temporalSaveStateToLivePreviewChanges {
                order.append("per-game.general.live-preview-circle-exit")
                order.append("per-game.general.before-changes-preview-duration")
            }
            return order

        case .graphics:
            guard enabled else {
                return ["per-game.graphics.download-shaders"]
            }
            if initiallySelectsShaders {
                var order = ["per-game.graphics.shader-chain"]
                order.append("per-game.graphics.download-shaders")
                if perGameShaderChain == 1 {
                    order.append("per-game.graphics.shader-preset")
                    if !perGameShaderPresetRef.isEmpty {
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
            if perGameShaderChain == 1 {
                order.append("per-game.graphics.shader-preset")
                if !perGameShaderPresetRef.isEmpty {
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
            if textureOffsetXOverride {
                order.append("per-game.graphics.texture-offset-x")
            }
            order.append("per-game.graphics.texture-offset-y-override")
            if textureOffsetYOverride {
                order.append("per-game.graphics.texture-offset-y")
            }
            if !enableGameDBHardwareFixes {
                order.append("per-game.graphics.skipdraw-start-override")
                if skipDrawStartOverride {
                    order.append("per-game.graphics.skipdraw-start")
                }
                order.append("per-game.graphics.skipdraw-end-override")
                if skipDrawEndOverride {
                    order.append("per-game.graphics.skipdraw-end")
                }
            }
            order += [
                "per-game.graphics.hardware-download-mode",
                "per-game.graphics.disable-depth-emulation",
                "per-game.graphics.cpu-clut-render",
                "per-game.graphics.gpu-target-clut",
                "per-game.graphics.load-replacement-textures",
                "per-game.graphics.async-loading",
                "per-game.graphics.precache-textures",
            ]
            return order

        case .framePacing:
            guard enabled else { return [] }
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
            guard enabled else { return [] }
            var order = ["per-game.audio.custom-volume"]
            if volumeOverride {
                order.append("per-game.audio.emulator-volume")
            }
            order.append("per-game.audio.fast-forward-volume")
            return order

        case .cpu:
            guard enabled else { return [] }
            var order = [
                "per-game.cpu.ee-core",
                "per-game.cpu.mtvu",
                "per-game.cpu.iop-recompiler",
                "per-game.cpu.vu0-recompiler",
                "per-game.cpu.vu1-recompiler",
                "per-game.cpu.ee-cycle-rate",
            ]
            if eeCycleRate != Self.eeCycleRateUseGlobalSentinel {
                order.append("per-game.cpu.reset-ee-cycle-rate")
            }
            order.append("per-game.cpu.ee-cycle-skip")
            if eeCycleSkip != -1 {
                order.append("per-game.cpu.reset-ee-cycle-skip")
            }
            order.append("per-game.cpu.fast-boot")
            if fastBoot != Self.fastBootUseGlobalSentinel {
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
            if padLayoutIdentity != nil {
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
            if hasGameSettingsIdentity {
                order += SettingsStore.stickInversionKeys.map {
                    "per-game.pad.inversion.\($0)"
                }
            }
            return order

        case .fixes:
            guard enabled else { return [] }
            return [
                "per-game.fixes.accurate-alpha-test",
                "per-game.fixes.texture-inside-rt",
            ] + SettingsStore.gameFixOptions.map {
                "per-game.fixes.\($0.key)"
            }

        case .cheats:
            var order = ["per-game.cheats.manager"]
            if enabled {
                order += [
                    "per-game.cheats.core-fixes",
                    "per-game.cheats.graphics-fixes",
                    "per-game.cheats.widescreen",
                    "per-game.cheats.no-interlace",
                ]
            }
            return order

        case .retroAchievements:
            guard enabled else { return [] }
            return [
                "per-game.retro.enabled",
                "per-game.retro.hardcore",
            ]
        }
    }

    private var perGamePadHasLinkedLayout: Bool {
        let descriptor = layoutPresets.effectiveSkinDescriptor(
            for: padLayoutIdentity,
            using: skinLibrary
        )
        guard let linkedID = descriptor.linkedLayoutPresetID else { return false }
        return layoutPresets.preset(id: linkedID) != nil
    }

    private var perGamePreferredInitialFocusLabel: String? {
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

    private var controllerColumn: PerGameControllerColumn {
        selectedControllerDetail == nil ? .categories : .detail
    }

    private var portraitNavigationPath: Binding<[PerGameSettingsCategory]> {
        Binding(
            get: { selectedControllerDetail.map { [$0] } ?? [] },
            set: { path in
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

    private func controllerAlertTitle(
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

    private func controllerAlertMessage(
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

    private func controllerAlertActions(
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

    private func performControllerAlertAction(
        _ index: Int,
        for kind: PerGameControllerAlertKind
    ) {
        let actions = controllerAlertActions(for: kind)
        guard actions.indices.contains(index) else { return }
        let actionID = actions[index].id
        guard actionID != "cancel" else {
            dismissControllerAlert(kind)
            return
        }
        dismissControllerAlert(kind)
        switch (kind, actionID) {
        case (.discardChanges, "apply"):
            applyChangesAndDismiss()
        case (.resetAll, "reset"):
            resetAllOverrides()
        case (.discardChanges, "discard"):
            discardChangesAndDismiss()
        case (.clearFramePacing, "clear"):
            clearFramePacingOverrides()
        default:
            break
        }
    }

    private func dismissControllerAlert(
        _ kind: PerGameControllerAlertKind
    ) {
        switch kind {
        case .resetAll: showResetAllConfirmation = false
        case .discardChanges: showDiscardConfirmation = false
        case .clearFramePacing: showFramePacingResetConfirmation = false
        }
    }

    private func clearFramePacingOverrides() {
        perGameFramePacingPreset = -1
        perGameFrameLimiter = -1
        perGameTargetFPS = -1
        perGameVsyncQueue = -1
        perGameSyncToHostRefresh = -1
        perGameBufferMS = -1
        perGameOutputLatencyMS = -1
    }

    @MainActor
    private func beginLivePreviewSessionIfNeeded() {
        guard savesToRunningGame,
              settings.temporalSaveStateToLivePreviewChanges,
              livePreviewToken == nil,
              livePreviewInitialization == nil,
              !livePreviewSessionIsFinishing else { return }

        let initialization = Task<String?, Never> { @MainActor in
            await withCheckedContinuation { continuation in
                ARMSX2Bridge.beginPerGameLivePreview { token in
                    continuation.resume(returning: token)
                }
            }
        }
        livePreviewInitialization = initialization
        Task { @MainActor in
            let token = await initialization.value
            // Dismissal owns the transaction once closing starts, including
            // a save-state capture that was still in flight.
            guard !livePreviewSessionIsFinishing else { return }
            livePreviewInitialization = nil
            guard settings.temporalSaveStateToLivePreviewChanges else {
                if let token {
                    _ = await ARMSX2Bridge.finishPerGameLivePreview(
                        token: token,
                        preserveSettings: false
                    )
                }
                return
            }
            livePreviewToken = token
            guard token != nil else {
                livePreviewPendingFingerprint = nil
                statusMessage = settings.localized(
                    "Live Preview is unavailable while save states are unavailable."
                )
                return
            }
            if livePreviewPendingFingerprint != nil {
                startLivePreviewTaskIfNeeded()
            }
        }
    }

    @MainActor
    private func scheduleLivePreview(for fingerprint: Int) {
        guard savesToRunningGame,
              settings.temporalSaveStateToLivePreviewChanges,
              !livePreviewSessionIsFinishing else { return }

        let previewIsIdle = livePreviewTask == nil
            && livePreviewPresentation == .editing
        guard !previewIsIdle
                || fingerprint != lastPreviewedFingerprint else { return }

        livePreviewPendingFingerprint = fingerprint
        livePreviewChangeSequence &+= 1

        guard livePreviewToken != nil else {
            beginLivePreviewSessionIfNeeded()
            return
        }

        startLivePreviewTaskIfNeeded()
    }

    @MainActor
    private func startLivePreviewTaskIfNeeded() {
        guard livePreviewTask == nil,
              livePreviewPendingFingerprint != nil,
              livePreviewToken != nil,
              !livePreviewSessionIsFinishing else { return }
        livePreviewTask = Task { @MainActor in
            await runLivePreviewLoop()
        }
    }

    @MainActor
    private func runLivePreviewLoop() async {
        defer {
            livePreviewTask = nil
            if livePreviewPendingFingerprint != nil,
               settings.temporalSaveStateToLivePreviewChanges,
               !livePreviewSessionIsFinishing {
                startLivePreviewTaskIfNeeded()
            }
        }
        guard let token = livePreviewToken else { return }
        let beforeDuration = max(0, settings.perGameBeforeChangesPreviewDuration)

        do {
            try Task.checkCancellation()
            // Zero skips the before phase entirely: only native application
            // of the edited values precedes the changed preview.
            if beforeDuration > 0 {
                let initialSequence = livePreviewChangeSequence
                publishLivePreviewPresentation(.playingBefore)
                let beforeDeadline = Date.timeIntervalSinceReferenceDate
                    + beforeDuration
                while Date.timeIntervalSinceReferenceDate < beforeDeadline,
                      livePreviewChangeSequence == initialSequence {
                    try await Task.sleep(for: .milliseconds(16))
                    try Task.checkCancellation()
                }
            }

            while let fingerprint = livePreviewPendingFingerprint {
                let sequenceBeingApplied = livePreviewChangeSequence

                publishLivePreviewPresentation(.applying)
                guard writeCurrentSettings(normalizesEditableValues: false),
                      await applyLivePreview(token: token) else {
                    throw CancellationError()
                }
                try Task.checkCancellation()

                // A value can change while the CPU thread is applying the
                // previous one. Consume the newest state immediately instead
                // of briefly showing an obsolete frame.
                if livePreviewChangeSequence != sequenceBeingApplied {
                    continue
                }

                publishLivePreviewPresentation(.playingAfter)
                lastPreviewedFingerprint = fingerprint
                // Remain in the changed scene until Circle/touch cancels the
                // preview, but immediately apply a newly adjusted Left/Right
                // value without remounting the editor between changes.
                while livePreviewChangeSequence == sequenceBeingApplied {
                    try await Task.sleep(for: .milliseconds(16))
                    try Task.checkCancellation()
                }
            }
        } catch {
            if !livePreviewSessionIsFinishing {
                await recoverCancelledLivePreview()
            }
        }
    }

    @MainActor
    private func recoverCancelledLivePreview() async {
        guard !livePreviewSessionIsFinishing else { return }
        guard let token = livePreviewToken else {
            livePreviewPendingFingerprint = nil
            publishLivePreviewPresentation(.editing)
            return
        }
        publishLivePreviewPresentation(.restoring)
        _ = await restoreLivePreviewState(token: token)
        guard !livePreviewSessionIsFinishing else { return }
        livePreviewPendingFingerprint = nil
        publishLivePreviewPresentation(.editing)
    }

    @MainActor
    private func publishLivePreviewStatus(
        value: String?,
        countsDown: Bool
    ) {
        guard livePreviewPendingFingerprint != nil
                || livePreviewPresentation != .editing else { return }
        let resolvedValue = value?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard let resolvedValue, !resolvedValue.isEmpty else { return }
        onLivePreviewStatusChange?(
            PerGameLivePreviewStatus(
                value: settings.localized(resolvedValue),
                countsDown: countsDown
            )
        )
    }

    private func applyLivePreview(token: String) async -> Bool {
        await withCheckedContinuation { continuation in
            ARMSX2Bridge.applyPerGameLivePreview(
                token: token
            ) { success in
                continuation.resume(returning: success)
            }
        }
    }

    private func restoreLivePreviewState(token: String) async -> Bool {
        await withCheckedContinuation { continuation in
            ARMSX2Bridge.restorePerGameLivePreviewState(
                token: token
            ) { success in
                continuation.resume(returning: success)
            }
        }
    }

    private func dismissPanel(
        preservingChanges: Bool = true,
        appliesChanges: Bool = false
    ) {
        guard !livePreviewSessionIsFinishing else { return }
        livePreviewSessionIsFinishing = true
        livePreviewPendingFingerprint = nil
        let previewTask = livePreviewTask
        let initialization = livePreviewInitialization
        previewTask?.cancel()
        // Keep the paused editor visible while restoring; do not flash gameplay
        // and then remount the editor immediately before dismissing.
        publishLivePreviewPresentation(.finishing)

        Task { @MainActor in
            // Cancellation cannot interrupt an in-flight native settings write.
            // Join it before saving so it cannot restore the old INI over Apply.
            await previewTask?.value
            let initializedToken = await initialization?.value
            livePreviewInitialization = nil
            livePreviewToken = livePreviewToken ?? initializedToken
            if appliesChanges {
                save(showsConfirmation: false)
                guard !hasPendingChanges else {
                    livePreviewSessionIsFinishing = false
                    publishLivePreviewPresentation(.editing)
                    return
                }
                MenuAudioPackManager.shared.playEvent(.return)
            }
            guard savesToRunningGame,
                  let token = livePreviewToken else {
                completePanelDismissal()
                return
            }
            _ = await ARMSX2Bridge.finishPerGameLivePreview(
                token: token,
                preserveSettings: preservingChanges
            )
            livePreviewToken = nil
            completePanelDismissal()
        }
    }

    @MainActor
    private func publishLivePreviewPresentation(
        _ presentation: PerGameLivePreviewPresentation
    ) {
        guard !livePreviewSessionIsFinishing || presentation == .finishing,
              livePreviewPresentation != presentation else { return }
        livePreviewPresentation = presentation
        onLivePreviewPresentationChange?(presentation)
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
        dismissPanel(preservingChanges: false)
    }

    private func applyChangesAndDismiss() {
        dismissPanel(preservingChanges: true, appliesChanges: true)
    }

    private var perGamePadLayoutEditorContext: PadLayoutEditorContext {
        let preset = layoutPresets.effectivePreset(for: padLayoutIdentity)
        let editablePresetID = padLayoutIdentity.flatMap { layoutPresets.presetID(for: $0) }
        return PadLayoutEditorContext(
            presetID: editablePresetID,
            gameIdentity: padLayoutIdentity,
            initialSnapshot: preset?.snapshot,
            skinDescriptor: layoutPresets.effectiveSkinDescriptor(for: padLayoutIdentity, using: skinLibrary)
        )
    }

    private var displayName: String {
        let name = ((game.name as NSString).deletingPathExtension as String).trimmingCharacters(in: .whitespacesAndNewlines)
        let serial = game.metadata["serial"]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""

        if name.isEmpty {
            return serial.isEmpty ? settings.localized("Current Game") : serial
        }
        if serial.isEmpty {
            return name
        }
        return "\(name) - \(serial)"
    }

    // MARK: - Per-game INI helpers
    // Bridge the generic per-game INI accessor for the compatibility overrides.
    // `useCurrent` selects the VM-safe current-game variant (which live-applies);
    // otherwise the ISO variant writes the per-game file without applying.

    private static func targetISO(useCurrent: Bool, iso: String) -> String? {
        useCurrent ? nil : iso
    }

    /// Reads a per-game int override; returns -1 ("use global") when no per-game key is set.
    private static func loadedPerGameInt(
        _ section: String,
        _ key: String,
        globalDefault: Int32,
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        if let snapshot {
            return snapshot.number(section, key)?.intValue
                ?? useGlobalSentinel
        }
        let target = targetISO(useCurrent: useCurrent, iso: iso)
        guard ARMSX2Bridge.hasPerGameINIValue(section, key: key, forISO: target) else { return -1 }
        return Int(ARMSX2Bridge.getPerGameINIInt(section, key: key, defaultValue: globalDefault, forISO: target))
    }

    /// Pin a loaded value to what its control can show. The sentinel is not a value, so it is
    /// left alone; anything else has to fit or the stepper and the stored number disagree.
    private static func clampedPerGameInt(_ value: Int, to range: ClosedRange<Int>) -> Int {
        value == useGlobalSentinel ? value : SettingsStore.clamped(value, to: range)
    }

    /// Reads a per-game bool override; returns -1 ("use global"), 0 (off), or 1 (on).
    private static func loadedPerGameBool(
        _ section: String,
        _ key: String,
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        if let snapshot {
            guard let value = snapshot.number(section, key) else {
                return useGlobalSentinel
            }
            return value.boolValue ? 1 : 0
        }
        let target = targetISO(useCurrent: useCurrent, iso: iso)
        guard ARMSX2Bridge.hasPerGameINIValue(section, key: key, forISO: target) else { return -1 }
        return ARMSX2Bridge.getPerGameINIBool(section, key: key, defaultValue: false, forISO: target) ? 1 : 0
    }

    private static func loadedPerGameString(
        _ section: String,
        _ key: String,
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> String {
        if let snapshot {
            return snapshot.string(section, key) ?? ""
        }
        return ARMSX2Bridge.getPerGameINIString(
            section,
            key: key,
            defaultValue: "",
            forISO: targetISO(useCurrent: useCurrent, iso: iso)
        )
    }

    private static func setPerGameBoolValue(_ section: String, _ key: String, _ value: Bool, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIBool(section, key: key, value: value, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func setPerGameIntValue(_ section: String, _ key: String, _ value: Int, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIInt(section, key: key, value: Int32(value), forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func setPerGameFloatValue(_ section: String, _ key: String, _ value: Float, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIFloat(section, key: key, value: value, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    /// Reads independent per-game limiter and presentation-cadence overrides.
    /// Legacy files are interpreted correctly until the native runtime or Save
    /// migrates their encoded target into the dedicated cadence key.
    private static func loadedPerGameFrameLimiter(
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> (limiter: Int, fps: Float) {
        let scalarPresent: Bool
        let scalar: Float
        let targetPresent: Bool
        let storedTarget: Float
        if let snapshot {
            let scalarNumber = snapshot.number("Framerate", "NominalScalar")
            scalarPresent = scalarNumber != nil
            scalar = scalarNumber?.floatValue ?? 1.0
            let targetNumber = snapshot.number(
                "ARMSX2iOS/FramePacing",
                "TargetFPS"
            )
            targetPresent = targetNumber != nil
            storedTarget = targetNumber?.floatValue
                ?? SettingsStore.defaultTargetFPS
        } else {
            let target = targetISO(useCurrent: useCurrent, iso: iso)
            scalarPresent = ARMSX2Bridge.hasPerGameINIValue("Framerate", key: "NominalScalar", forISO: target)
            scalar = scalarPresent ? ARMSX2Bridge.getPerGameINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0, forISO: target) : 1.0
            targetPresent = ARMSX2Bridge.hasPerGameINIValue("ARMSX2iOS/FramePacing", key: "TargetFPS", forISO: target)
            storedTarget = targetPresent ? ARMSX2Bridge.getPerGameINIFloat("ARMSX2iOS/FramePacing", key: "TargetFPS", defaultValue: SettingsStore.defaultTargetFPS, forISO: target) : SettingsStore.defaultTargetFPS
        }

        let limiter = scalarPresent ? (SettingsStore.frameLimiterEnabled(fromNominalScalar: scalar) ? 1 : 0) : -1
        if targetPresent {
            let fps = min(
                max((storedTarget * 1_000.0).rounded() / 1_000.0, SettingsStore.minTargetFPS),
                SettingsStore.maxTargetFPS)
            return (limiter, fps)
        }

        // Older per-game files stored target/base directly in NominalScalar.
        if scalarPresent, limiter == 1, abs(scalar - 1.0) >= 0.002 {
            let legacyFPS = SettingsStore.targetFPS(
                fromNominalScalar: scalar,
                baseFramerate: SettingsStore.shared.ntscFramerate)
            return (limiter, max(legacyFPS, SettingsStore.minTargetFPS))
        }
        return (limiter, -1.0)
    }

    /// Writes limiter state and presentation cadence independently so either
    /// picker can continue inheriting its global value.
    private static func savePerGameFrameLimiter(preset: Int, limiter: Int, targetFPS: Float, enabled: Bool, useCurrent: Bool, iso: String) {
        if enabled, let named = FramePacingPreset(rawValue: preset), let v = SettingsStore.framePacingPresetTable[named] {
            setPerGameFloatValue("Framerate", "NominalScalar", SettingsStore.nominalScalarForFrameLimiter(enabled: v.frameLimiterEnabled), useCurrent: useCurrent, iso: iso)
            setPerGameFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", Float(v.targetFPS), useCurrent: useCurrent, iso: iso)
            return
        }

        if enabled, limiter != -1 {
            setPerGameFloatValue("Framerate", "NominalScalar", SettingsStore.nominalScalarForFrameLimiter(enabled: limiter == 1), useCurrent: useCurrent, iso: iso)
        } else {
            clearPerGameValue("Framerate", "NominalScalar", useCurrent: useCurrent, iso: iso)
        }
        if enabled, targetFPS >= SettingsStore.minTargetFPS {
            setPerGameFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", targetFPS, useCurrent: useCurrent, iso: iso)
        } else {
            clearPerGameValue("ARMSX2iOS/FramePacing", "TargetFPS", useCurrent: useCurrent, iso: iso)
        }
    }

    private static func clearPerGameValue(_ section: String, _ key: String, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.deletePerGameINIValue(section, key: key, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func loadedPerGameEEClamp(
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        let overflow = loadedPerGameBool("EmuCore/CPU/Recompiler", "fpuOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot)
        guard overflow != -1 else { return -1 }
        let extra = loadedPerGameBool("EmuCore/CPU/Recompiler", "fpuExtraOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        let full = loadedPerGameBool("EmuCore/CPU/Recompiler", "fpuFullMode", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        return SettingsStore.eeClampModeFromBools(overflow == 1, extra, full)
    }

    private static func savePerGameEEClamp(_ mode: Int, enabled: Bool, useCurrent: Bool, iso: String) {
        if enabled && mode != -1 {
            setPerGameBoolValue("EmuCore/CPU/Recompiler", "fpuOverflow", mode >= 1, useCurrent: useCurrent, iso: iso)
            setPerGameBoolValue("EmuCore/CPU/Recompiler", "fpuExtraOverflow", mode >= 2, useCurrent: useCurrent, iso: iso)
            setPerGameBoolValue("EmuCore/CPU/Recompiler", "fpuFullMode", mode >= 3, useCurrent: useCurrent, iso: iso)
        } else {
            for key in ["fpuOverflow", "fpuExtraOverflow", "fpuFullMode"] {
                clearPerGameValue("EmuCore/CPU/Recompiler", key, useCurrent: useCurrent, iso: iso)
            }
        }
    }

    private static func loadedPerGameVUClamp(
        useCurrent: Bool,
        iso: String,
        snapshot: PerGameINISnapshot? = nil
    ) -> Int {
        let overflow = loadedPerGameBool("EmuCore/CPU/Recompiler", "vu0Overflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot)
        guard overflow != -1 else { return -1 }
        let extra = loadedPerGameBool("EmuCore/CPU/Recompiler", "vu0ExtraOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        let sign = loadedPerGameBool("EmuCore/CPU/Recompiler", "vu0SignOverflow", useCurrent: useCurrent, iso: iso, snapshot: snapshot) == 1
        return SettingsStore.vuClampModeFromBools(overflow == 1, extra, sign)
    }

    private static func savePerGameVUClamp(_ mode: Int, enabled: Bool, useCurrent: Bool, iso: String) {
        if enabled && mode != -1 {
            for prefix in ["vu0", "vu1"] {
                setPerGameBoolValue("EmuCore/CPU/Recompiler", "\(prefix)Overflow", mode >= 1, useCurrent: useCurrent, iso: iso)
                setPerGameBoolValue("EmuCore/CPU/Recompiler", "\(prefix)ExtraOverflow", mode >= 2, useCurrent: useCurrent, iso: iso)
                setPerGameBoolValue("EmuCore/CPU/Recompiler", "\(prefix)SignOverflow", mode >= 3, useCurrent: useCurrent, iso: iso)
            }
        } else {
            for prefix in ["vu0", "vu1"] {
                for suffix in ["Overflow", "ExtraOverflow", "SignOverflow"] {
                    clearPerGameValue("EmuCore/CPU/Recompiler", "\(prefix)\(suffix)", useCurrent: useCurrent, iso: iso)
                }
            }
        }
    }

    private func save(showsConfirmation: Bool = true) {
        livePreviewTask?.cancel()
        guard writeCurrentSettings(normalizesEditableValues: true) else {
            return
        }
        statusMessage = postSaveMessage
        savedFingerprint = perGameFingerprint()
        lastPreviewedFingerprint = perGameLivePreviewFingerprint()
        if showsConfirmation {
            showSaveConfirmation()
        }
    }

    /// Writes the editor snapshot through the existing bridge. During a live preview
    /// the native transaction restores the original INI immediately after the VM has
    /// consumed this snapshot, so this method does not itself mark anything saved.
    @discardableResult
    private func writeCurrentSettings(
        normalizesEditableValues: Bool
    ) -> Bool {
        guard hasGameSettingsIdentity else {
            statusMessage = "Start this game once before saving its settings."
            return false
        }
        let normalizedSkipDraw = normalizedSkipDrawValues()
        if normalizesEditableValues,
           skipDrawStart != normalizedSkipDraw.start {
            skipDrawStart = normalizedSkipDraw.start
        }
        if normalizesEditableValues,
           skipDrawEnd != normalizedSkipDraw.end {
            skipDrawEnd = normalizedSkipDraw.end
        }

        let settingsDict: [String: Any] = [
            "enabled": enabled,
            "upscaleMultiplier": upscaleMultiplier,
            "aspectRatio": aspectRatio,
            "textureFiltering": Int32(textureFiltering),
            "hardwareMipmapping": Int32(hardwareMipmapping),
            "blendingAccuracy": Int32(blendingAccuracy),
            "interlaceMode": Int32(interlaceMode),
            "trilinearFiltering": Int32(trilinearFiltering),
            "halfPixelOffset": Int32(halfPixelOffset),
            "roundSprite": Int32(roundSprite),
            "alignSprite": Int32(alignSprite),
            "mergeSprite": Int32(mergeSprite),
            "wildArmsOffset": Int32(wildArmsOffset),
            "hasTextureOffsetXOverride": textureOffsetXOverride,
            "textureOffsetX": Int32(textureOffsetX),
            "hasTextureOffsetYOverride": textureOffsetYOverride,
            "textureOffsetY": Int32(textureOffsetY),
            "hasSkipDrawStartOverride": skipDrawStartOverride,
            "skipDrawStart": Int32(normalizedSkipDraw.start),
            "hasSkipDrawEndOverride": skipDrawEndOverride,
            "skipDrawEnd": Int32(normalizedSkipDraw.end),
            "hasVolumeOverride": enabled && volumeOverride,
            "volumePercent": Int32(volumePercent),
            "eeCoreType": enabled ? Int32(eeCoreType) : 0,
            "mtvu": enabled && mtvu,
            "hasEECycleRateOverride": enabled && eeCycleRate != Self.eeCycleRateUseGlobalSentinel,
            "eeCycleRate": Int32(Self.clampedEECycleRate(eeCycleRate == Self.eeCycleRateUseGlobalSentinel ? globalEECycleRate : eeCycleRate)),
            "hasFastBootOverride": enabled && fastBoot != Self.fastBootUseGlobalSentinel,
            "fastBoot": fastBoot == Self.fastBootOn,
            "enableCheats": enabled && enableCheats,
            "enablePatches": enabled && enablePatches,
            "enableGameFixes": enabled && enableGameFixes,
            "enableGameDBHardwareFixes": enabled && enableGameDBHardwareFixes
        ]
        ARMSX2Bridge.setGameSettings(settingsDict, forISO: savesToRunningGame ? nil : game.bootName)
        savePerGameCompatibility()
        return true
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

    /// Write the per-game compatibility overrides (game fixes, accurate alpha test,
    /// texture-inside-RT) via the generic per-game INI helper. "Use global" (-1) and
    /// a disabled master toggle both clear the per-game key so the global value wins.
    private func savePerGameCompatibility() {
        let useCurrent = savesToRunningGame
        let iso = game.bootName
        for option in SettingsStore.gameFixOptions {
            let state = perGameFixes[option.key] ?? -1
            if enabled && state != -1 {
                Self.setPerGameBoolValue("EmuCore/Gamefixes", option.key, state == 1, useCurrent: useCurrent, iso: iso)
            } else {
                Self.clearPerGameValue("EmuCore/Gamefixes", option.key, useCurrent: useCurrent, iso: iso)
            }
        }
        if enabled && perGameAAT != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "HWAccurateAlphaTest", perGameAAT == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "HWAccurateAlphaTest", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameTextureInsideRt != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", perGameTextureInsideRt, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_TextureInsideRt", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameDisableDepth != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", perGameDisableDepth == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_DisableDepthSupport", useCurrent: useCurrent, iso: iso)
        }
        // The core keeps a running game on its booted renderer, so this applies next boot.
        if enabled && perGameRenderer != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "Renderer", perGameRenderer, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "Renderer", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameFXAA != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "fxaa", perGameFXAA == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "fxaa", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameUpscaler != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "Upscaler", perGameUpscaler, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "Upscaler", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoost != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "ShadeBoost", perGameShadeBoost == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameTVShader != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "TVShader", perGameTVShader, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "TVShader", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameCASMode != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "CASMode", perGameCASMode, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "CASMode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameMaxAnisotropy != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "MaxAnisotropy", perGameMaxAnisotropy, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "MaxAnisotropy", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameCASSharpness != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "CASSharpness", perGameCASSharpness, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "CASSharpness", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePCRTCOffsets != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "pcrtc_offsets", perGamePCRTCOffsets == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "pcrtc_offsets", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameIntegerScaling != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "IntegerScaling", perGameIntegerScaling == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "IntegerScaling", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameSkipDupFrames != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "SkipDuplicateFrames", perGameSkipDupFrames == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "SkipDuplicateFrames", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePCRTCOverscan != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "pcrtc_overscan", perGamePCRTCOverscan == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "pcrtc_overscan", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePCRTCAntiBlur != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "pcrtc_antiblur", perGamePCRTCAntiBlur == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "pcrtc_antiblur", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameDisableInterlaceOffset != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "disable_interlace_offset", perGameDisableInterlaceOffset == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "disable_interlace_offset", useCurrent: useCurrent, iso: iso)
        }
        // High-value per-game overrides added via the generic helper path.
        if enabled && perGameWidescreen != -1 {
            Self.setPerGameBoolValue("EmuCore", "EnableWideScreenPatches", perGameWidescreen == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore", "EnableWideScreenPatches", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameNoInterlace != -1 {
            Self.setPerGameBoolValue("EmuCore", "EnableNoInterlacingPatches", perGameNoInterlace == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore", "EnableNoInterlacingPatches", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostBrightness != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Brightness", perGameShadeBoostBrightness, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Brightness", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostContrast != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Contrast", perGameShadeBoostContrast, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Contrast", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostSaturation != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Saturation", perGameShadeBoostSaturation, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Saturation", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameShadeBoostGamma != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "ShadeBoost_Gamma", perGameShadeBoostGamma, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "ShadeBoost_Gamma", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameDithering != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "dithering_ps2", perGameDithering, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "dithering_ps2", useCurrent: useCurrent, iso: iso)
        }
        // PerGameShaderSelection writes or clears all three shader keys together.
        if enabled && perGameShaderChain != -1 {
            PerGameShaderSelection.write(chain: perGameShaderChain, presetRef: perGameShaderPresetRef, useCurrent: useCurrent, iso: iso)
        } else {
            PerGameShaderSelection.clear(useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameFastForwardVolume != -1 {
            Self.setPerGameIntValue("SPU2/Output", "FastForwardVolume", perGameFastForwardVolume, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("SPU2/Output", "FastForwardVolume", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameIOP != -1 {
            Self.setPerGameBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", perGameIOP == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU/Recompiler", "EnableIOP", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU0 != -1 {
            Self.setPerGameBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", perGameVU0 == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU/Recompiler", "EnableVU0", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU1 != -1 {
            Self.setPerGameBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", perGameVU1 == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU/Recompiler", "EnableVU1", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameHWDownloadMode != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "HWDownloadMode", perGameHWDownloadMode, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "HWDownloadMode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameCPUCLUT != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "UserHacks_CPUCLUTRender", perGameCPUCLUT, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_CPUCLUTRender", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameGPUTargetCLUT != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", perGameGPUTargetCLUT, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVsyncQueue != -1 {
            Self.setPerGameIntValue("EmuCore/GS", "VsyncQueueSize", perGameVsyncQueue, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "VsyncQueueSize", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameLoadTextureReplacements != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "LoadTextureReplacements", perGameLoadTextureReplacements == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "LoadTextureReplacements", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameLoadTextureReplacementsAsync != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "LoadTextureReplacementsAsync", perGameLoadTextureReplacementsAsync == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "LoadTextureReplacementsAsync", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGamePrecacheTextureReplacements != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "PrecacheTextureReplacements", perGamePrecacheTextureReplacements == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "PrecacheTextureReplacements", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameSyncToHostRefresh != -1 {
            Self.setPerGameBoolValue("EmuCore/GS", "SyncToHostRefreshRate", perGameSyncToHostRefresh == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/GS", "SyncToHostRefreshRate", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameBufferMS != -1 {
            Self.setPerGameIntValue("SPU2/Output", "BufferMS", perGameBufferMS, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("SPU2/Output", "BufferMS", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameOutputLatencyMS != -1 {
            Self.setPerGameIntValue("SPU2/Output", "OutputLatencyMS", perGameOutputLatencyMS, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("SPU2/Output", "OutputLatencyMS", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameFramePacingPreset != -1 {
            Self.setPerGameIntValue("ARMSX2iOS/FramePacing", "Preset", perGameFramePacingPreset, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("ARMSX2iOS/FramePacing", "Preset", useCurrent: useCurrent, iso: iso)
        }
        // Cascade a named preset's own keys. Use Global and .custom both miss the
        // table and fall through. After the individual writes on purpose, so the
        // preset's profile wins over stale per-game picker state.
        if enabled, let preset = FramePacingPreset(rawValue: perGameFramePacingPreset),
           let values = SettingsStore.framePacingPresetTable[preset] {
            Self.setPerGameIntValue("EmuCore/GS", "VsyncQueueSize", values.vsyncQueueSize, useCurrent: useCurrent, iso: iso)
            Self.setPerGameIntValue("SPU2/Output", "OutputLatencyMS", values.audioOutputLatencyMs, useCurrent: useCurrent, iso: iso)
            Self.setPerGameIntValue("SPU2/Output", "BufferMS", values.audioBufferMs, useCurrent: useCurrent, iso: iso)
            Self.setPerGameBoolValue("EmuCore/GS", "SyncToHostRefreshRate", values.syncToHostRefresh, useCurrent: useCurrent, iso: iso)
        }
        // Limiter state and presentation cadence are independent per-game overrides.
        Self.savePerGameFrameLimiter(preset: perGameFramePacingPreset, limiter: perGameFrameLimiter, targetFPS: perGameTargetFPS, enabled: enabled, useCurrent: useCurrent, iso: iso)
        if enabled && eeCycleSkip != -1 {
            Self.setPerGameIntValue("EmuCore/Speedhacks", "EECycleSkip", eeCycleSkip, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/Speedhacks", "EECycleSkip", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameEEFpuRound != -1 {
            Self.setPerGameIntValue("EmuCore/CPU", "FPU.Roundmode", perGameEEFpuRound, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU", "FPU.Roundmode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU0Round != -1 {
            Self.setPerGameIntValue("EmuCore/CPU", "VU0.Roundmode", perGameVU0Round, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU", "VU0.Roundmode", useCurrent: useCurrent, iso: iso)
        }
        if enabled && perGameVU1Round != -1 {
            Self.setPerGameIntValue("EmuCore/CPU", "VU1.Roundmode", perGameVU1Round, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("EmuCore/CPU", "VU1.Roundmode", useCurrent: useCurrent, iso: iso)
        }
        Self.savePerGameEEClamp(perGameEEClamp, enabled: enabled, useCurrent: useCurrent, iso: iso)
        Self.savePerGameVUClamp(perGameVUClamp, enabled: enabled, useCurrent: useCurrent, iso: iso)
        if enabled && raEnabledOverride != -1 {
            Self.setPerGameBoolValue("Achievements", "Enabled", raEnabledOverride == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("Achievements", "Enabled", useCurrent: useCurrent, iso: iso)
        }
        if enabled && raHardcoreOverride != -1 {
            Self.setPerGameBoolValue("Achievements", "ChallengeMode", raHardcoreOverride == 1, useCurrent: useCurrent, iso: iso)
        } else {
            Self.clearPerGameValue("Achievements", "ChallengeMode", useCurrent: useCurrent, iso: iso)
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

    private static func normalizedAspect(_ value: String?) -> String {
        switch value {
        case "Stretch", "4:3", "16:9", "10:7":
            return value ?? "Auto 4:3/3:2"
        default:
            return "Auto 4:3/3:2"
        }
    }

    private static func boolValue(_ value: Any?, defaultValue: Bool) -> Bool {
        if let bool = value as? Bool {
            return bool
        }
        if let number = value as? NSNumber {
            return number.boolValue
        }
        return defaultValue
    }

    private static func intValue(_ value: Any?, defaultValue: Int) -> Int {
        if let number = value as? NSNumber {
            return number.intValue
        }
        return defaultValue
    }

    private static func floatValue(_ value: Any?, defaultValue: Float) -> Float {
        if let number = value as? NSNumber {
            return number.floatValue
        }
        return defaultValue
    }

    private static func clampedTextureOffset(_ offset: Int) -> Int {
        min(max(offset, SettingsStore.textureOffsetRange.lowerBound), SettingsStore.textureOffsetRange.upperBound)
    }

    private static func clampedSkipDraw(_ value: Int) -> Int {
        min(max(value, SettingsStore.skipDrawRange.lowerBound), SettingsStore.skipDrawRange.upperBound)
    }

    private static func clampedVolume(_ value: Int) -> Int {
        SettingsStore.clampedEmulatorVolumePercent(value)
    }

    private static func clampedEECycleRate(_ value: Int) -> Int {
        min(max(value, -3), 3)
    }

    private static func normalizedSkipDrawEnd(start: Int, end: Int, startOverride: Bool, endOverride: Bool) -> Int {
        let clampedEnd = clampedSkipDraw(end)
        guard startOverride && endOverride else {
            return clampedEnd
        }
        return SettingsStore.normalizedSkipDrawEnd(start: start, end: clampedEnd)
    }
}
