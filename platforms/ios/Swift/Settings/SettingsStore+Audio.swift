// SettingsStore+Audio.swift — audio-domain helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    static func clampedEmulatorVolumePercent(_ value: Int) -> Int {
        clamped(value, to: emulatorVolumeRange)
    }
}

extension SettingsStore {
    static let _audioTimeStretchConfig = Setting<Bool>(
        section: "SPU2/Output", key: "SyncMode", default: true,
        codec: .timeStretch)
    static let _audioBufferMsConfig = Setting<Int>(
        section: "SPU2/Output", key: "BufferMS", default: 50,
        codec: .int(in: SettingsStore.audioBufferMsRange))
    static let _audioOutputLatencyMsConfig = Setting<Int>(
        section: "SPU2/Output", key: "OutputLatencyMS", default: 20,
        codec: .int(in: SettingsStore.audioOutputLatencyMsRange))
    static let _audioFastForwardVolumeConfig = Setting<Int>(
        section: "SPU2/Output", key: "FastForwardVolume", default: 100,
        codec: .int(in: SettingsStore.fastForwardVolumeRange))
    static let _audioSwapChannelsConfig = Setting<Bool>(
        section: "SPU2/Output", key: "SwapChannels", default: false,
        codec: .bool)
    static let _palFramerateConfig = Setting<Float>(
        section: "EmuCore/GS", key: "FrameratePAL", default: 50.0,
        codec: .float)
}
