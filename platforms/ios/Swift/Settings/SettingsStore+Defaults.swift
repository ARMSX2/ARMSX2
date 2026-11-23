// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Reset workflows are kept in a separate primary file so changes
// to defaults can compile independently from SettingsStore's stored properties.
extension SettingsStore {
    /// Restore the emulator's audio controls without deleting imported packs or custom files.
    func resetAudioDefaults() {
        emulatorVolumePercent = Self.defaultEmulatorVolumePercent
        audioTimeStretch = true
        audioBufferMs = 50
        audioOutputLatencyMs = 20
        audioFastForwardVolume = 100
        audioSwapChannels = false
    }

    /// Reset emulator settings to ARMSX2 iOS defaults
    func resetEmulatorDefaults() {
        eeCoreType = 2          // ARM64 JIT
        // Core uses EnableEE (not CoreType) to select interpreter vs recompiler.
        // Restore EnableEE=true so the core actually uses the recompiler again,
        // undoing any prior applyFullInterpreterPreset() that forced the interpreter.
        ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableEE", value: true)
        iopRecompiler = true
        vu0Recompiler = true
        vu1Recompiler = true
        fastBoot = false
        automaticLoadLastSaveState = false
        autoSaveEnabled = true
        autoSaveIntervalMinutes = 10
        autoSaveOnLeave = true
        autoSaveOnLowBattery = true
        undoSeconds = 5
        automaticLoadLastGame = false
        appearance.temporalSaveStateToLivePreviewChanges = true
        appearance.perGameLivePreviewDuration = 2
        appearance.perGameBeforeChangesPreviewDuration = 0
        appearance.perGameLivePreviewStopsWithCircle = true
        fastmem = true
        emulationOnlyModeEnabled = false
        emulationOnlyDisablePatches = true
        emulationOnlyDisablePINE = true
        emulationOnlyDisableRetroAchievements = true
        emulationOnlyDisableInputRecording = true
        emulationOnlyDisableOSD = true
        emulationOnlyDisableFramePacing = true
        emulationOnlyDisableVirtualControls = true
        emulationOnlyDisableQuickMenu = true
        emulationOnlyClearNetworkCache = true
        emulationOnlyModeDelaySeconds = Self.defaultEmulationOnlyModeDelaySeconds
        eeFpuRoundMode = 3      // Chop (Zero)
        vu0RoundMode = 3
        vu1RoundMode = 3
        eeClampMode = 1         // Normal
        vuClampMode = 1
        targetFPS = Self.defaultTargetFPS
        frameLimiterEnabled = true
        fastForwardRuntimeEnabled = false
        fastForwardScalar = Self.defaultFastForwardScalar
        emulatorVolumePercent = Self.defaultEmulatorVolumePercent
        audioTimeStretch = true
        audioFastForwardVolume = 100
        audioSwapChannels = false
        ntscFramerate = 59.94
        palFramerate = 50.0
        fastCDVD = false
        eeCycleRate = 0
        vu1Instant = true
        mtvu = true
        waitLoop = true
        intcStat = true
        eeCycleSkip = 0
        vuFlagHack = true
        enableCheats = false
        enablePatches = true
        enableGameFixes = true
        enableGameDBHardwareFixes = true
        enableWidescreenPatches = false
        enableNoInterlacingPatches = false
        hostFilesystem = false
        autoTune = false
        for option in Self.gameFixOptions {
            gameFixes[option.key] = false
            ARMSX2Bridge.setINIBool("EmuCore/Gamefixes", key: option.key, value: false)
        }
        jitScriptProtocol = JITScriptProtocol.defaultValue
    }

    /// Keep EE/IOP/VU0 fast while isolating suspected VU1 JIT regressions.
    func applyVU1CompatibilityPreset() {
        eeCoreType = 2
        // Core uses EnableEE (not CoreType) to select interpreter vs recompiler.
        // Restore EnableEE=true so the core actually uses the EE recompiler again,
        // undoing any prior applyFullInterpreterPreset() that forced the interpreter.
        ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableEE", value: true)
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
        // Core uses EnableEE (not CoreType) to select interpreter vs recompiler.
        // Must write EnableEE=false to actually force the EE interpreter.
        ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableEE", value: false)
        iopRecompiler = false
        vu0Recompiler = false
        vu1Recompiler = false
        vu1Instant = false
        mtvu = false
        fastmem = false
    }

    /// Reset graphics settings to ARMSX2 iOS defaults
    func resetGraphicsDefaults() {
        // Hand the hacks back to the game database, otherwise a reset leaves whatever was
        // claimed still overriding it.
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "UserHackOverrides", value: 0)
        renderer = 17           // Metal
        upscaleMultiplier = 1.0 // Native PS2
        textureFiltering = 2    // Bilinear (PS2)
        backThreadMode = 0            // Disabled
        hardwareMipmapping = true
        fxaa = false
        casMode = 0             // Disabled
        casSharpness = 50
        shaderChainEnabled = false
        shaderChainPresetRef = ""
        interlaceMode = 0       // GSInterlaceMode::Automatic
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
        // GS hardware fixes
        hwAccurateAlphaTest = false
        textureInsideRt = 0
        limit24BitDepth = 0
        nativeScaling = 0
        cpuClutRender = 0
        cpuSpriteRenderBw = 0
        cpuSpriteRenderLevel = 0
        gpuTargetClut = 0
        bilinearUpscaleHack = 0
        maxAnisotropy = 0
        hardwareDownloadMode = 0
        tvShader = 0
        upscaler = 0
        // Defaults hand the hacks back to the game database, so the claim goes with them.
        for option in Self.gsBoolHackOptions {
            setGSBoolHack(option.key, false)
            setGraphicsHackPinned(option.key, false)
        }
        // Screen / PCRTC and Shade Boost
        pcrtcOffsets = false
        pcrtcOverscan = false
        pcrtcAntiBlur = true
        disableInterlaceOffset = false
        skipDuplicateFrames = true
        integerScaling = false
        shadeBoost = false
        shadeBoostBrightness = 50
        shadeBoostContrast = 50
        shadeBoostSaturation = 50
        shadeBoostGamma = 50
        // Texture pack and dump toggles are intentionally preserved.
    }

    /// Restores the stock Appearance presentation without deleting imported
    /// backgrounds or user-saved themes. The active selection remains the
    /// canonical Default preset so its semantic white-text readability rules
    /// are not replaced by Custom's palette fallbacks.
    func resetAppearanceDefaults() {
        UIFrameRateSettings.shared.resetToDefaults()
        let gallery = ThemeGalleryStore.shared
        gallery.clearActiveSavedTheme()

        appearance.backgroundPrimaryAsset = nil
        appearance.backgroundLandscapeAsset = nil
        appearance.backgroundFitMode = .fill
        appearance.backgroundLandscapeFitMode = .fill
        appearance.backgroundVideoMuted = true
        appearance.backgroundDim = 0
        appearance.backgroundEnabledInBIOS = true
        appearance.backgroundEnabledInHelp = true
        appearance.backgroundEnabledInSettings = true

        appearance.clearLiquidGlassUI = true
        appearance.clearLiquidGlassUISubSettings = true
        appearance.clearLiquidGlassUIQuickMenu = false
        appearance.clearLiquidGlassUIPerGameSettingsLibrary = false
        appearance.clearLiquidGlassUIPerGameSettingsEmulation = false
        appearance.gameCardZoomAnimationEnabled = true
        appearance.favoriteGlowingEffectEnabled = false
        appearance.gameMenuCoverColoursEnabled = true
        appearance.hideGameplayStatusBar = true
        appearance.hideIntroStatusBar = true
        appearance.hideMenuStatusBar = false
        appearance.controllerNavigationDepthEffectEnabled = true
        appearance.focusOrbsEnabled = false

        // Apply the canonical preset rather than duplicating its palette and
        // dynamic-background defaults here. This keeps Reset synchronized as
        // the stock theme evolves.
        appearance.applyControllerUIThemePreset(
            .defaultTheme,
            preservingCustomTheme: false
        )
        appearance.controllerNavigationFocusAnimation = .easeInOut
        appearance.controllerCustomThemeBase = .defaultTheme
        appearance.controllerUIThemePreset = .defaultTheme
    }

    /// Restores application configuration without deleting imported content,
    /// user-created presets, memory cards, skins, or account credentials.
    func resetAllDefaults() {
        resetEmulatorDefaults()
        resetGraphicsDefaults()

        // Texture replacement settings are intentionally preserved by the
        // graphics-only reset, but a full reset returns them to fresh-install values.
        loadTextureReplacements = false
        loadTextureReplacementsAsync = true
        precacheTextureReplacements = false
        texturePreloading = 2
        dumpReplaceableTextures = false
        dumpReplaceableMipmaps = false
        dumpTexturesWithFMVActive = false
        dumpDirectTextures = true
        dumpPaletteTextures = true

        // The graphics and emulator resets no longer touch the pacing keys, so this is the only
        // thing restoring them. Apply the values first, the way the Frame Pacing screen's own
        // reset does, rather than leaning on the preset's didSet to do it.
        applyFramePacingPreset(.optimal)
        framePacingPreset = .optimal
        adaptiveResolutionEnabled = false

        osdPreset = .off
        lastActiveOsdPreset = .simple
        osdPerformancePosition = Self.defaultOsdPerformancePosition
        osdShowMessages = true
        osdShowTextureReplacements = false
        snapshotCustomOsd()

        controller.padOpacity = 0.6
        controller.phoneRumbleStrength = 0.25
        controller.increaseRumbleDurationAndInterpolation = true
        controller.hapticFeedback = true
        controller.gameRumbleStrength = 1
        controller.uiRumbleStrength = 0.5
        controller.dpadDiagonalsEnabled = true
        controller.faceComboZonesEnabled = true
        controller.automaticDownloadCustomSkin = false
        controller.autoHideVirtualPadWhenControllerConnected = true
        controller.autoFullscreen = true
        controller.hideMenuButton = false
        controller.analogStickScale = 1.0
        controller.gameControllerDeadZone = 0.15
        controller.gameControllerLeftInstantDeadzoneEnabled = false
        controller.gameControllerLeftNegativeDeadzone = -0.08
        controller.gameControllerRightInstantDeadzoneEnabled = false
        controller.gameControllerRightNegativeDeadzone = -0.08
        controller.invertLeftStickX = false
        controller.invertLeftStickY = false
        controller.invertRightStickX = false
        controller.invertRightStickY = false
        appLanguage = .system
        controller.controllerMultitapMode = 0
        controller.resetControllerMacros()
        autoOpenStikDebug = false
        jitScriptProtocol = JITScriptProtocol.defaultValue

        network.dev9HddEnabled = false
        network.dev9HddFile = "DEV9hdd.raw"
        network.dev9EthernetEnabled = false
        network.dev9EthDevice = "Auto"
        network.dev9InterceptDHCP = false
        network.dev9EthLogDHCP = false
        network.dev9EthLogDNS = false
        network.dev9DNS1Mode = "Auto"
        network.dev9DNS1 = "0.0.0.0"
        network.dev9DNS2Mode = "Auto"
        network.dev9DNS2 = "0.0.0.0"

        resetAppearanceDefaults()

        dynamicThumbsticks.restoreDefaults()
        let padLayout = PadLayoutStore.shared
        padLayout.resetAll()
        padLayout.resetControlVisibility()
        padLayout.save()
        ARMSX2Bridge.resetButtonMappings()
        ARMSX2Bridge.flushINISettings()
    }
}
