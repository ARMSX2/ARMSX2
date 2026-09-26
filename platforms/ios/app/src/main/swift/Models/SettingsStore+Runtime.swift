// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Runtime normalization, OSD, and frame-pacing workflows compile
// independently from the large observable stored-property declaration.
extension SettingsStore {
    static func frameLimiterEnabled(fromNominalScalar scalar: Float) -> Bool {
        !scalar.isFinite || scalar < 5.0
    }

    static func sanitizedNominalScalar(_ scalar: Float) -> Float {
        guard scalar.isFinite else { return 1.0 }
        return min(max(scalar, 0.05), 10.0)
    }

    static func clampedTargetFPS(_ fps: Float) -> Float {
        guard fps.isFinite else { return defaultTargetFPS }
        let millisecondPrecision = (fps * 1_000.0).rounded() / 1_000.0
        return min(max(millisecondPrecision, minTargetFPS), maxTargetFPS)
    }

    static func clampedSpeedScalar(_ scalar: Float) -> Float {
        guard scalar.isFinite else { return defaultFastForwardScalar }
        let stepped = (scalar * 4.0).rounded() / 4.0
        return min(max(stepped, minFastForwardScalar), maxFastForwardScalar)
    }

    // clampedEmulatorVolumePercent — see SettingsStore+Audio.swift.

    static func clampedAnalogStickScale(_ scale: Float) -> Float {
        guard scale.isFinite else { return 1.0 }
        return min(max(scale, 0.8), 1.6)
    }

    static func clampedBackgroundDim(_ value: Double) -> Double {
        guard value.isFinite else { return 0.0 }
        return min(max(value, 0.0), 1.0)
    }

    static func loadBackgroundAsset(forKey key: String) -> BackgroundAsset? {
        guard let data = UserDefaults.standard.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(BackgroundAsset.self, from: data)
    }

    static func loadSavedPaletteColor(
        forKey key: String
    ) -> SavedPaletteColor? {
        guard let data = UserDefaults.standard.data(forKey: key) else {
            return nil
        }
        return try? JSONDecoder().decode(SavedPaletteColor.self, from: data)
    }

    static func persistOptionalThemePalette(
        _ palette: ThemePalette?,
        forKey key: String
    ) {
        if let palette {
            UserDefaults.standard.set(palette.rawValue, forKey: key)
        } else {
            UserDefaults.standard.removeObject(forKey: key)
        }
    }

    static func persistSavedPaletteColor(
        _ color: SavedPaletteColor?,
        forKey key: String
    ) {
        guard let color else {
            UserDefaults.standard.removeObject(forKey: key)
            return
        }
        UserDefaults.standard.set(
            try? JSONEncoder().encode(color),
            forKey: key
        )
    }

    /// Removes background assets whose files no longer exist. Returns `true`
    /// when at least one stale asset was cleared so callers can surface a notice.
    @discardableResult
    func sanitizeBackgroundAssets() -> Bool {
        var removed = false
        if let primary = backgroundPrimaryAsset, !BackgroundStorage.exists(primary) {
            backgroundPrimaryAsset = nil
            removed = true
        }
        if let landscape = backgroundLandscapeAsset, !BackgroundStorage.exists(landscape) {
            backgroundLandscapeAsset = nil
            removed = true
        }
        return removed
    }

    static func clampedTextureOffset(_ offset: Int) -> Int {
        min(max(offset, textureOffsetRange.lowerBound), textureOffsetRange.upperBound)
    }

    static func clampedSkipDraw(_ value: Int) -> Int {
        min(max(value, skipDrawRange.lowerBound), skipDrawRange.upperBound)
    }

    static func normalizedSkipDrawEnd(start: Int, end: Int) -> Int {
        let clampedStart = clampedSkipDraw(start)
        let clampedEnd = clampedSkipDraw(end)
        return clampedStart > 0 && clampedEnd < clampedStart ? clampedStart : clampedEnd
    }

    // MARK: - CPU rounding/clamp writers

    /// Write the EE clamp level to the three FPU recompiler keys.
    static func applyEEClampMode(_ mode: Int) {
        ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "fpuOverflow", value: mode >= 1)
        ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "fpuExtraOverflow", value: mode >= 2)
        ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "fpuFullMode", value: mode >= 3)
    }

    /// Write the VU clamp level to both VU0 and VU1 recompiler keys (six booleans total).
    static func applyVUClampMode(_ mode: Int) {
        for prefix in ["vu0", "vu1"] {
            ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "\(prefix)Overflow", value: mode >= 1)
            ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "\(prefix)ExtraOverflow", value: mode >= 2)
            ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "\(prefix)SignOverflow", value: mode >= 3)
        }
    }

    static func targetFPS(fromNominalScalar scalar: Float, baseFramerate: Float) -> Float {
        guard frameLimiterEnabled(fromNominalScalar: scalar) else { return defaultTargetFPS }
        return clampedTargetFPS(sanitizedNominalScalar(scalar) * max(baseFramerate, 1.0))
    }

    static func normalSpeedScalar(frameLimiterEnabled: Bool) -> Float {
        frameLimiterEnabled ? 1.0 : 10.0
    }

    static func loadedTargetFPS(fromNominalScalar scalar: Float, baseFramerate: Float) -> Float {
        let normalizedScalar = sanitizedNominalScalar(scalar)
        let derivedTarget = abs(normalizedScalar - 1.0) < 0.002
            ? defaultTargetFPS
            : targetFPS(fromNominalScalar: scalar, baseFramerate: baseFramerate)
        let rawStoredTarget = ARMSX2Bridge.getINIFloat(
            "ARMSX2iOS/FramePacing", key: "TargetFPS", defaultValue: -1.0)
        let hasStoredTarget = rawStoredTarget.isFinite && rawStoredTarget >= minTargetFPS
        let target = hasStoredTarget ? clampedTargetFPS(rawStoredTarget) : derivedTarget

        // Older iOS builds encoded the presentation cadence in NominalScalar,
        // slowing CPU and audio timing together with video. Move that cadence
        // to its dedicated key and restore normal emulation speed.
        if frameLimiterEnabled(fromNominalScalar: scalar) && abs(normalizedScalar - 1.0) >= 0.002 {
            ARMSX2Bridge.setINIFloat("ARMSX2iOS/FramePacing", key: "TargetFPS", value: target)
            ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: 1.0)
        }

        return target
    }

    static func sanitizeNominalScalarIfNeeded(_ scalar: Float) {
        let sanitized = sanitizedNominalScalar(scalar)
        guard abs(scalar - sanitized) > 0.001 else { return }

        ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: sanitized)
    }

    // excludeHddImageFromBackup / normalizeDEV9Settings — see SettingsStore+DEV9.swift.

    static func normalizedOsdPerformancePosition(_ value: Int) -> Int {
        switch value {
        case 0, 1, 3:
            return value
        case 2:
            return defaultOsdPerformancePosition
        default:
            return defaultOsdPerformancePosition
        }
    }

    func applyFrameLimiterSettings() {
        guard !suppressINIWrites else { return }
        let scalar = Self.normalSpeedScalar(frameLimiterEnabled: frameLimiterEnabled)
        ARMSX2Bridge.setINIFloat("ARMSX2iOS/FramePacing", key: "TargetFPS", value: targetFPS)
        ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: scalar)
        ARMSX2Bridge.setPresentFPSCap(frameLimiterEnabled ? targetFPS : 0.0)
    }

    /// Frame targets control presentation cadence rather than VM speed. The
    /// selected cadence is stored separately while NominalScalar remains 1.0.
    static func nominalScalarForFrameLimiter(enabled: Bool) -> Float {
        enabled ? 1.0 : 10.0
    }

    func setRuntimeFastForwardEnabled(_ enabled: Bool) {
        fastForwardRuntimeEnabled = enabled
        let speedPercent = Int32((fastForwardScalar * 100).rounded())
        ARMSX2Bridge.setRuntimeFastForward(
            enabled: enabled,
            speedPercent: speedPercent
        )
    }

    // supportedIOSRenderer — see SettingsStore+Graphics.swift.
    // localized / localizedLayoutDirection — see SettingsStore+UI.swift.

    /// Apply OSD preset — writes ALL OSD flags to INI + GSConfig
    func applyOsdPreset(_ preset: OsdPreset) {
        guard preset != .custom else { return }
        ARMSX2Bridge.applyOsdPreset(Int32(preset.rawValue))
        if preset == .off {
            osdPerformancePosition = 0
        } else {
            revealOsdPerformancePositionIfHidden()
        }
        let isSimple = preset == .simple
        let isDetail = preset == .detail
        let isFull = preset == .full
        isProgrammaticOsdFlagChange = true
        defer { isProgrammaticOsdFlagChange = false }
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

    /// If the perf overlay is at the hidden position (None/0), restore it to the default
    /// so newly-enabled perf stats become visible. Shared by applyOsdPreset + restoreCustomOsd.
    func revealOsdPerformancePositionIfHidden() {
        if osdPerformancePosition == 0 {
            osdPerformancePosition = Self.defaultOsdPerformancePosition
        }
    }

    private static let osdCustomFlagKeyPaths: [(ReferenceWritableKeyPath<SettingsStore, Bool>, String)] = [
        (\.osdShowFPS, "OsdCustomShowFPS"),
        (\.osdShowVPS, "OsdCustomShowVPS"),
        (\.osdShowSpeed, "OsdCustomShowSpeed"),
        (\.osdShowCPU, "OsdCustomShowCPU"),
        (\.osdShowGPU, "OsdCustomShowGPU"),
        (\.osdShowResolution, "OsdCustomShowResolution"),
        (\.osdShowGSStats, "OsdCustomShowGSStats"),
        (\.osdShowIndicators, "OsdCustomShowIndicators"),
        (\.osdShowSettings, "OsdCustomShowSettings"),
        (\.osdShowInputs, "OsdCustomShowInputs"),
        (\.osdShowFrameTimes, "OsdCustomShowFrameTimes"),
        (\.osdShowVersion, "OsdCustomShowVersion"),
        (\.osdShowHardwareInfo, "OsdCustomShowHardwareInfo"),
        (\.osdShowDeviceStats, "OsdCustomShowDeviceStats"),
    ]

    func snapshotCustomOsd() {
        for (keyPath, key) in Self.osdCustomFlagKeyPaths {
            ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: key, value: self[keyPath: keyPath])
        }
    }

    func restoreCustomOsd() {
        revealOsdPerformancePositionIfHidden()
        isProgrammaticOsdFlagChange = true
        for (keyPath, key) in Self.osdCustomFlagKeyPaths {
            self[keyPath: keyPath] = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: key, defaultValue: self[keyPath: keyPath])
        }
        isProgrammaticOsdFlagChange = false
    }

    func markOsdCustom() {
        guard !suppressINIWrites, !isProgrammaticOsdFlagChange else { return }
        isAutoMarkingCustom = true
        if osdPreset != .custom {
            osdPreset = .custom
        }
        isAutoMarkingCustom = false
        snapshotCustomOsd()
    }

    /// Apply a preset via the individual clamped setters. Never writes
    /// Framerate/NominalScalar directly — frameLimiterEnabled + targetFPS go
    /// through applyFrameLimiterSettings. Non-Framerate keys first so the
    /// limiter flag is set before targetFPS fires applyFrameLimiterSettings.
    func applyFramePacingPreset(_ preset: FramePacingPreset) {
        guard preset != .custom else { return }
        isProgrammaticFramePacingFlagChange = true
        defer { isProgrammaticFramePacingFlagChange = false }
        guard let values = Self.framePacingPresetTable[preset] else { return }
        // Spelled out rather than looped, because the order above is the point.
        vsyncQueueSize = values.vsyncQueueSize
        audioOutputLatencyMs = values.audioOutputLatencyMs
        audioBufferMs = values.audioBufferMs
        syncToHostRefresh = values.syncToHostRefresh
        frameLimiterEnabled = values.frameLimiterEnabled
        targetFPS = Float(values.targetFPS)
    }

    /// Hook for restoring individual pacing values when cycling back to
    /// .custom. Currently a no-op; kept so the preset didSet stays symmetric
    /// with the OSD preset handling.
    func restoreCustomFramePacing() {
    }

    /// Mark the preset .custom when the user edits any individual pacing
    /// control directly.
    func markFramePacingCustom() {
        guard !suppressINIWrites, !isProgrammaticFramePacingFlagChange else { return }
        isAutoMarkingFramePacingCustom = true
        if framePacingPreset != .custom {
            framePacingPreset = .custom
        }
        isAutoMarkingFramePacingCustom = false
    }
}
