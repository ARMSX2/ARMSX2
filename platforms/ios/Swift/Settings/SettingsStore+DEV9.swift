// SettingsStore+DEV9.swift — DEV9/network normalization helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    /// Keeps the multi-gigabyte DEV9 HDD image, but not the rest of inis, out of iCloud backup.
    /// Resolved as the core's GetHDDPath does, and run at launch as well as on enable, since
    /// players copy the image in themselves and the core never creates it on iOS.
    func excludeHddImageFromBackup() {
        let fileName = dev9HddFile.isEmpty ? "DEV9hdd.raw" : dev9HddFile
        let inis = URL.documentsDirectory.appendingPathComponent("inis", isDirectory: true)
        var imageURL = URL(fileURLWithPath: fileName, relativeTo: inis).absoluteURL
        guard FileManager.default.fileExists(atPath: imageURL.path) else { return }
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? imageURL.setResourceValues(values)
    }

    func normalizeDEV9Settings() {
        if dev9HddEnabled {
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile.isEmpty ? "DEV9hdd.raw" : dev9HddFile)
            excludeHddImageFromBackup()
        }

        if dev9EthernetEnabled {
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
        }
    }
}

extension SettingsStore {
    static let _dev9HddFileConfig = Setting<String>(
        section: "DEV9/Hdd", key: "HddFile", default: "DEV9hdd.raw",
        codec: .string)
    static let _dev9InterceptDHCPConfig = Setting<Bool>(
        section: "DEV9/Eth", key: "InterceptDHCP", default: false,
        codec: .bool)
    static let _dev9EthLogDHCPConfig = Setting<Bool>(
        section: "DEV9/Eth", key: "EthLogDHCP", default: false,
        codec: .bool)
    static let _dev9EthLogDNSConfig = Setting<Bool>(
        section: "DEV9/Eth", key: "EthLogDNS", default: false,
        codec: .bool)
    static let _dev9DNS1ModeConfig = Setting<String>(
        section: "DEV9/Eth", key: "ModeDNS1", default: "Auto",
        codec: .string)
    static let _dev9DNS1Config = Setting<String>(
        section: "DEV9/Eth", key: "DNS1", default: "0.0.0.0",
        codec: .string)
    static let _dev9DNS2ModeConfig = Setting<String>(
        section: "DEV9/Eth", key: "ModeDNS2", default: "Auto",
        codec: .string)
    static let _dev9DNS2Config = Setting<String>(
        section: "DEV9/Eth", key: "DNS2", default: "0.0.0.0",
        codec: .string)
}
