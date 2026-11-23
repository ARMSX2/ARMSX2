// NetworkSettingsView.swift — PS2 HDD, ethernet adapter and DNS hosts
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct NetworkSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var hosts: [DEV9Host] = []
    @State private var lastPersistedHosts: [DEV9Host] = []
    @State private var networkAdapters: [String] = []
    @State private var hasLoadedHosts = false
    @State private var hostSaveTask: Task<Void, Never>?

    static let controllerTargetOrder = [
        "settings.network.hdd-enabled",
        "settings.network.default-hdd",
        "settings.network.ethernet-enabled",
        "settings.network.adapter",
        "settings.network.log-dhcp",
        "settings.network.log-dns",
        "settings.network.dns1",
        "settings.network.dns2",
        "settings.network.add-host",
    ]

    var body: some View {
        Form {
            Section(settings.localized("PS2 HDD")) {
                Toggle(settings.localized("Enable DEV9 Virtual HDD"), isOn: Bindable(settings.network).dev9HddEnabled)
                    .controllerAccessibilityTargetID("settings.network.hdd-enabled")

                HStack {
                    Text(settings.localized("Image"))
                    Spacer()
                    Text(settings.network.dev9HddFile.isEmpty ? "DEV9hdd.raw" : settings.network.dev9HddFile)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }

                Button(settings.localized("Use Default HDD Image")) {
                    settings.network.dev9HddFile = "DEV9hdd.raw"
                }
                .controllerAccessibilityTargetID("settings.network.default-hdd")

                Text(settings.localized("If a compatible HDD image is present in app storage, games that expect an internal hard drive can use it. Requires a VM restart. The image is kept out of iCloud backups."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Online / Ethernet")) {
                Toggle(settings.localized("Enable DEV9 Ethernet"), isOn: Bindable(settings.network).dev9EthernetEnabled)
                    .controllerAccessibilityTargetID("settings.network.ethernet-enabled")

                Group {
                    HStack {
                        Text(settings.localized("Mode"))
                        Spacer()
                        Text(settings.localized("Sockets"))
                            .foregroundStyle(.secondary)
                    }

                    Picker(settings.localized("Adapter"), selection: Bindable(settings.network).dev9EthDevice) {
                        ForEach(networkAdapters, id: \.self) { adapter in
                            Text(adapter).tag(adapter)
                        }
                    }
                    .controllerAccessibilityOptionsPickerTarget(
                        id: "settings.network.adapter",
                        label: settings.localized("Adapter"),
                        selection: Bindable(settings.network).dev9EthDevice,
                        options: networkAdapters.map {
                            (id: $0, title: $0)
                        }
                    )

                    Toggle(settings.localized("Log DHCP"), isOn: Bindable(settings.network).dev9EthLogDHCP)
                        .controllerAccessibilityTargetID("settings.network.log-dhcp")
                    Toggle(settings.localized("Log DNS"), isOn: Bindable(settings.network).dev9EthLogDNS)
                        .controllerAccessibilityTargetID("settings.network.log-dns")
                }
                // Ethernet options may be prepared before the adapter is
                // enabled. Disabling this Group made the focus graph end at
                // Enable DEV9 Ethernet.

                Text(settings.localized("Sockets is the iOS-safe DEV9 Ethernet mode exposed here. PCAP bridged/switched modes are compiled out of this iOS build, so they are intentionally not selectable until a real iOS backend exists."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("DNS")) {
                dnsRow("DNS1", id: "settings.network.dns1", mode: Bindable(settings.network).dev9DNS1Mode, address: Bindable(settings.network).dev9DNS1)
                dnsRow("DNS2", id: "settings.network.dns2", mode: Bindable(settings.network).dev9DNS2Mode, address: Bindable(settings.network).dev9DNS2)
            }

            Section(settings.localized("Internal DNS")) {
                ForEach(hosts) { host in
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            TextField(settings.localized("Hostname"), text: hostBinding(host).url)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                            Toggle("", isOn: hostBinding(host).enabled).labelsHidden()
                        }
                        TextField("0.0.0.0", text: hostBinding(host).address)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .keyboardType(.numbersAndPunctuation)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .onDelete { hosts.remove(atOffsets: $0) }

                Button {
                    hosts.append(DEV9Host(url: "", desc: "", address: "0.0.0.0", enabled: true))
                } label: {
                    Label(settings.localized("Add Host"), systemImage: "plus")
                }
                .controllerAccessibilityTargetID("settings.network.add-host")

                Text(settings.localized("Maps a hostname to an IP. Set a DNS mode to Internal to use these entries."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Tester Notes")) {
                Text(settings.localized("Games still need their in-game PS2 network setup. After changing DEV9 settings, use Reset ROM or restart the VM before testing."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Network"))
        .onAppear {
            loadNetworkAdaptersIfNeeded()
            loadHosts()
        }
        .onChange(of: hosts) { _, newHosts in
            guard hasLoadedHosts, newHosts != lastPersistedHosts else {
                return
            }
            scheduleHostSave()
        }
        .onDisappear {
            flushPendingHostSave()
        }
#if canImport(UIKit)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            flushPendingHostSave()
        }
#endif
    }

    @ViewBuilder
    private func ipRow(_ label: String, _ value: Binding<String>) -> some View {
        HStack {
            Text(label)
            Spacer()
            TextField("0.0.0.0", text: value)
                .multilineTextAlignment(.trailing)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.numbersAndPunctuation)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func dnsRow(_ label: String, id: String, mode: Binding<String>, address: Binding<String>) -> some View {
        Picker(label, selection: mode) {
            Text(settings.localized("Auto")).tag("Auto")
            Text(settings.localized("Manual")).tag("Manual")
            Text(settings.localized("Internal")).tag("Internal")
        }
        .controllerAccessibilityOptionsPickerTarget(
            id: id,
            label: label,
            selection: mode,
            options: [
                ("Auto", settings.localized("Auto")),
                ("Manual", settings.localized("Manual")),
                ("Internal", settings.localized("Internal")),
            ]
        )
        ipRow(label + " " + settings.localized("Address"), address)
            .disabled(mode.wrappedValue != "Manual")
    }

    private func hostBinding(_ host: DEV9Host) -> Binding<DEV9Host> {
        Binding(
            get: { hosts.first { $0.id == host.id } ?? host },
            set: { newValue in
                if let index = hosts.firstIndex(where: { $0.id == host.id }) {
                    hosts[index] = newValue
                }
            }
        )
    }

    private func loadHosts() {
        hosts = settings.network.dev9Hosts
        lastPersistedHosts = settings.network.dev9Hosts
        hasLoadedHosts = true
    }

    private func loadNetworkAdaptersIfNeeded() {
        guard networkAdapters.isEmpty else { return }
        var loaded = ARMSX2Bridge.dev9NetworkAdapters()
        if !settings.network.dev9EthDevice.isEmpty,
           !loaded.contains(settings.network.dev9EthDevice) {
            loaded.insert(settings.network.dev9EthDevice, at: 0)
        }
        networkAdapters = loaded
    }

    /// Text fields can publish on every keystroke. Coalescing those writes
    /// avoids repeatedly rewriting every DEV9 host section while typing.
    private func scheduleHostSave() {
        hostSaveTask?.cancel()
        hostSaveTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            saveHosts()
            hostSaveTask = nil
        }
    }

    private func flushPendingHostSave() {
        hostSaveTask?.cancel()
        hostSaveTask = nil
        guard hasLoadedHosts, hosts != lastPersistedHosts else { return }
        saveHosts()
    }

    private func saveHosts() {
        settings.network.saveDEV9Hosts(hosts)
        lastPersistedHosts = hosts
    }
}
