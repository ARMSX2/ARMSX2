// SettingsStore+Ranges.swift — ranges and defaults the setters and screens share
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    static let minTargetFPS: Float = 15.0
    static let maxTargetFPS: Float = 120.0
    static let defaultTargetFPS: Float = 60.0
    static let minFastForwardScalar: Float = 1.25
    static let maxFastForwardScalar: Float = 10.0
    static let defaultFastForwardScalar: Float = 2.0
    static let defaultEmulatorVolumePercent = 100
    static let textureOffsetRange = -4096...4096
    static let skipDrawRange = 0...5000
    static let eeCycleRateRange = -3...3
    // Named so the stepper, the per-game panel and the global codec all agree.
    static let vsyncQueueRange = 2...16
    static let audioBufferMsRange = 10...200
    static let audioOutputLatencyMsRange = 5...200
    static let fastForwardVolumeRange = 0...200
    static let emulatorVolumeRange = 0...150
    static let shadeBoostRange = 1...100
    static let casSharpnessRange = 0...100
    static let cpuSpriteRenderBwRange = 0...10
    static let defaultOsdPerformancePosition = 3
    static let emulationOnlyModeDelayRange = 0...15
    static let defaultEmulationOnlyModeDelaySeconds = 5
    static let autoSaveIntervals = [5, 10, 15, 30]
}
