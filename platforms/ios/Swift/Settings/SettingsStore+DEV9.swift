// SettingsStore+DEV9.swift — DEV9/network normalization helpers
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct DEV9Host: Identifiable, Equatable {
    let id = UUID()
    var url: String
    var desc: String
    var address: String
    var enabled: Bool
}

extension SettingsStore.Network {
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

    static func loadDEV9Hosts() -> [DEV9Host] {
        let count = Int(ARMSX2Bridge.getINIInt("DEV9/Eth/Hosts", key: "Count", defaultValue: 0))
        var loaded: [DEV9Host] = []
        var i = 0
        while i < count {
            let sec = "DEV9/Eth/Hosts/Host\(i)"
            loaded.append(DEV9Host(
                url: ARMSX2Bridge.getINIString(sec, key: "Url", defaultValue: ""),
                desc: ARMSX2Bridge.getINIString(sec, key: "Desc", defaultValue: ""),
                address: ARMSX2Bridge.getINIString(sec, key: "Address", defaultValue: "0.0.0.0"),
                enabled: ARMSX2Bridge.getINIBool(sec, key: "Enabled", defaultValue: true)
            ))
            i += 1
        }
        return loaded
    }

    func saveDEV9Hosts(_ hosts: [DEV9Host]) {
        let oldCount = Int(ARMSX2Bridge.getINIInt("DEV9/Eth/Hosts", key: "Count", defaultValue: 0))
        var i = 0
        while i < max(oldCount, hosts.count) {
            ARMSX2Bridge.clearINISection("DEV9/Eth/Hosts/Host\(i)")
            i += 1
        }
        ARMSX2Bridge.setINIInt("DEV9/Eth/Hosts", key: "Count", value: Int32(hosts.count))
        for (idx, host) in hosts.enumerated() {
            let sec = "DEV9/Eth/Hosts/Host\(idx)"
            ARMSX2Bridge.setINIString(sec, key: "Url", value: host.url)
            ARMSX2Bridge.setINIString(sec, key: "Desc", value: host.desc.isEmpty ? host.url : host.desc)
            ARMSX2Bridge.setINIString(sec, key: "Address", value: host.address)
            ARMSX2Bridge.setINIBool(sec, key: "Enabled", value: host.enabled)
        }
        dev9Hosts = hosts.map { var written = $0; written.desc = $0.desc.isEmpty ? $0.url : $0.desc; return written }
    }
}

extension SettingsStore.Network {
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
