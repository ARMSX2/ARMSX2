// SettingsStoreNetwork.swift — DEV9 network adapter and HDD image settings
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SettingsStore {
    @MainActor
    @Observable
    final class Network {
        @ObservationIgnored private(set) var suppressINIWrites = false

        // DEV9 / Network
        var dev9HddEnabled: Bool {
            didSet {
                guard !suppressINIWrites else { return }
                ARMSX2Bridge.setINIBool("DEV9/Hdd", key: "HddEnable", value: dev9HddEnabled)
                ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile)
                if dev9HddEnabled {
                    // Keep a large HDD image out of iCloud and iTunes backups from the start.
                    excludeHddImageFromBackup()
                }
            }
        }
        var dev9HddFile: String = "DEV9hdd.raw" { didSet { commit(Self._dev9HddFileConfig, dev9HddFile) } }
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
        var dev9InterceptDHCP: Bool = false { didSet { commit(Self._dev9InterceptDHCPConfig, dev9InterceptDHCP) } }
        var dev9EthLogDHCP: Bool = false { didSet { commit(Self._dev9EthLogDHCPConfig, dev9EthLogDHCP) } }
        var dev9EthLogDNS: Bool = false { didSet { commit(Self._dev9EthLogDNSConfig, dev9EthLogDNS) } }
        var dev9DNS1Mode: String = "Auto" { didSet { commit(Self._dev9DNS1ModeConfig, dev9DNS1Mode) } }
        var dev9DNS1: String = "0.0.0.0" { didSet { commit(Self._dev9DNS1Config, dev9DNS1) } }
        var dev9DNS2Mode: String = "Auto" { didSet { commit(Self._dev9DNS2ModeConfig, dev9DNS2Mode) } }
        var dev9DNS2: String = "0.0.0.0" { didSet { commit(Self._dev9DNS2Config, dev9DNS2) } }

        init() {
            suppressINIWrites = true
            defer { suppressINIWrites = false }
            dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
            dev9HddFile = Self._dev9HddFileConfig.load()
            dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
            dev9InterceptDHCP = Self._dev9InterceptDHCPConfig.load()
            dev9EthLogDHCP = Self._dev9EthLogDHCPConfig.load()
            dev9EthLogDNS = Self._dev9EthLogDNSConfig.load()
            dev9DNS1Mode = Self._dev9DNS1ModeConfig.load()
            dev9DNS1 = Self._dev9DNS1Config.load()
            dev9DNS2Mode = Self._dev9DNS2ModeConfig.load()
            dev9DNS2 = Self._dev9DNS2Config.load()
            dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        }

        private func commit<T>(_ setting: Setting<T>, _ value: T) {
            guard !(setting.suppressible && suppressINIWrites) else { return }
            setting.codec.write(setting.section, setting.key, value)
        }
    }
}
