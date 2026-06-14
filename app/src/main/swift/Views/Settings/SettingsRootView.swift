// SettingsRootView.swift — Settings navigation root
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

private enum SettingsPane: String, CaseIterable, Identifiable {
    case language
    case appearance
    case emulator
    case graphics
    case network
    case memoryCards
    case storage
    case retroAchievements
    case overlay
    case gameController
    case localMultiplayer
    case virtualPad
    case licenses
    case about

    var id: String { rawValue }

    var title: String {
        switch self {
        case .language:
            return "Language"
        case .appearance:
            return "Appearance"
        case .emulator:
            return "Emulator"
        case .graphics:
            return "Graphics"
        case .network:
            return "Network"
        case .memoryCards:
            return "Memory Cards"
        case .storage:
            return "Storage"
        case .retroAchievements:
            return "RetroAchievements"
        case .overlay:
            return "Overlay (OSD)"
        case .gameController:
            return "Game Controller"
        case .localMultiplayer:
            return "Local Multiplayer"
        case .virtualPad:
            return "Virtual Pad"
        case .licenses:
            return "Licenses & Credits"
        case .about:
            return "About"
        }
    }

    var icon: String {
        switch self {
        case .language:
            return "globe"
        case .appearance:
            return "paintpalette"
        case .emulator:
            return "cpu"
        case .graphics:
            return "paintbrush"
        case .network:
            return "network"
        case .memoryCards:
            return "memorychip"
        case .storage:
            return "internaldrive"
        case .retroAchievements:
            return "trophy"
        case .overlay:
            return "text.below.photo"
        case .gameController:
            return "gamecontroller"
        case .localMultiplayer:
            return "person.3"
        case .virtualPad:
            return "hand.draw"
        case .licenses:
            return "doc.text"
        case .about:
            return "info.circle"
        }
    }
}

struct SettingsRootView: View {
    @State private var settings = SettingsStore.shared
    @State private var jitAvailable = ARMSX2Bridge.isJITAvailable()
    @State private var noJITFallbackActive = ARMSX2Bridge.isNoJITFallbackActive()
    @State private var stikDebugOpenFailed = false
    @State private var stikDebugOpenInProgress = false
#if targetEnvironment(macCatalyst)
    @State private var selectedPane: SettingsPane? = .emulator
#endif

    var body: some View {
#if targetEnvironment(macCatalyst)
        NavigationSplitView {
            List(SettingsPane.allCases, selection: $selectedPane) { pane in
                Label(settings.localized(pane.title), systemImage: pane.icon)
                    .tag(pane)
            }
            .navigationTitle(settings.localized("Settings"))
            .listStyle(.sidebar)
        } detail: {
            settingsDetail(for: selectedPane)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .navigationSplitViewStyle(.balanced)
#else
        List {
            Section {
                jitStatusRow

                Button {
                    stikDebugOpenInProgress = true
                    stikDebugOpenFailed = false
                    StikDebugLauncher.open(reason: "settings-root") { success in
                        stikDebugOpenInProgress = false
                        stikDebugOpenFailed = !success
                        refreshJITStatus()
                    }
                } label: {
                    Label(settings.localized("Open StikDebug"), systemImage: "bolt.horizontal.circle")
                }
                .disabled(stikDebugOpenInProgress)

                Text(settings.localized("JIT Access means iOS currently allows executable memory. Confirm the real runtime state in-game: the OSD should show EE:JIT, IOP:JIT, and VU:JIT. Match the StikDebug script to the JIT Script setting in Emulator settings."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("JIT Status"))
            }

            Section {
                NavigationLink {
                    LanguageSettingsView()
                } label: {
                    Label(settings.localized("Language"), systemImage: "globe")
                }
                NavigationLink {
                    AppearanceSettingsView()
                } label: {
                    Label(settings.localized("Appearance"), systemImage: "paintpalette")
                }
                NavigationLink {
                    EmulatorSettingsView()
                } label: {
                    Label(settings.localized("Emulator"), systemImage: "cpu")
                }
                NavigationLink {
                    GraphicsSettingsView()
                } label: {
                    Label(settings.localized("Graphics"), systemImage: "paintbrush")
                }
                NavigationLink {
                    NetworkSettingsView()
                } label: {
                    Label(settings.localized("Network"), systemImage: "network")
                }
                NavigationLink {
                    MemoryCardSettingsView()
                } label: {
                    Label(settings.localized("Memory Cards"), systemImage: "memorychip")
                }
                NavigationLink {
                    StorageSettingsView()
                } label: {
                    Label(settings.localized("Storage"), systemImage: "internaldrive")
                }
                NavigationLink {
                    RetroAchievementsSettingsView()
                } label: {
                    Label(settings.localized("RetroAchievements"), systemImage: "trophy")
                }
                NavigationLink {
                    OverlaySettingsView()
                } label: {
                    Label(settings.localized("Overlay (OSD)"), systemImage: "text.below.photo")
                }
                NavigationLink {
                    GamepadSettingsView()
                } label: {
                    Label(settings.localized("Game Controller"), systemImage: "gamecontroller")
                }
                NavigationLink {
                    LocalMultiplayerSettingsView()
                } label: {
                    Label(settings.localized("Local Multiplayer"), systemImage: "person.3")
                }
                NavigationLink {
                    VirtualPadSettingsView()
                } label: {
                    Label(settings.localized("Virtual Pad"), systemImage: "hand.draw")
                }
            }

            Section {
                NavigationLink {
                    LicenseView()
                } label: {
                    Label(settings.localized("Licenses & Credits"), systemImage: "doc.text")
                }
            }

            Section(settings.localized("About")) {
                HStack {
                    Text(settings.localized("Version"))
                    Spacer()
                    Text(ARMSX2Bridge.buildVersion())
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
            }
        }
        .navigationTitle(settings.localized("Settings"))
        .navigationBarTitleDisplayMode(.inline)
        .safeAreaInset(edge: .top) {
            Color.clear.frame(height: 6)
        }
        .onAppear(perform: refreshJITStatus)
#if canImport(UIKit)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in
            refreshJITStatus()
        }
#endif
#endif
    }

    private var jitStatusRow: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(jitStatusColor)
                .frame(width: 12, height: 12)
                .shadow(color: jitStatusColor.opacity(0.45), radius: 5)

            VStack(alignment: .leading, spacing: 3) {
                Text(settings.localized(jitStatusTitle))
                    .font(.body)
                Text(settings.localized(jitStatusSubtitle))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Text(settings.localized(jitStatusBadge))
                .font(.caption.weight(.semibold))
                .foregroundStyle(jitStatusColor)
        }
        .accessibilityElement(children: .combine)
    }

    private var jitStatusColor: Color {
        if stikDebugOpenFailed {
            return .orange
        }
        if jitAvailable {
            return .blue
        }
        if noJITFallbackActive {
            return .orange
        }
        return .red
    }

    private var jitStatusTitle: String {
        if stikDebugOpenFailed {
            return "StikDebug Did Not Open"
        }
        if jitAvailable {
            return "JIT Access Detected"
        }
        if noJITFallbackActive {
            return "JIT Off"
        }
        return "JIT Not Detected"
    }

    private var jitStatusSubtitle: String {
        if stikDebugOpenFailed {
            return "Open StikDebug manually, then run the selected script and relaunch ARMSX2."
        }
        if jitAvailable {
            return "Access is available. Confirm EE:JIT / IOP:JIT / VU:JIT in the in-game OSD. Current script: \(settings.jitScriptProtocol.label)."
        }
        if noJITFallbackActive {
            return "No-JIT fallback is active. Use StikDebug/\(settings.jitScriptProtocol.label) for dynarec."
        }
        return "Launch with StikDebug using the \(settings.jitScriptProtocol.label) script to enable JIT."
    }

    private var jitStatusBadge: String {
        if stikDebugOpenFailed {
            return "Manual Open"
        }
        return jitAvailable ? "Check OSD" : "Needs StikDebug"
    }

    private func refreshJITStatus() {
        jitAvailable = ARMSX2Bridge.isJITAvailable()
        noJITFallbackActive = ARMSX2Bridge.isNoJITFallbackActive()
    }

    @ViewBuilder
    private func settingsDetail(for pane: SettingsPane?) -> some View {
        switch pane {
        case .language:
            LanguageSettingsView()
        case .appearance:
            AppearanceSettingsView()
        case .emulator:
            EmulatorSettingsView()
        case .graphics:
            GraphicsSettingsView()
        case .network:
            NetworkSettingsView()
        case .memoryCards:
            MemoryCardSettingsView()
        case .storage:
            StorageSettingsView()
        case .retroAchievements:
            RetroAchievementsSettingsView()
        case .overlay:
            OverlaySettingsView()
        case .gameController:
            GamepadSettingsView()
        case .localMultiplayer:
            LocalMultiplayerSettingsView()
        case .virtualPad:
            VirtualPadSettingsView()
        case .licenses:
            LicenseView()
        case .about:
            SettingsAboutView()
        case .none:
            VStack(spacing: 12) {
                Image(systemName: "gearshape")
                    .font(.system(size: 42))
                    .foregroundStyle(.secondary)
                Text(settings.localized("Select a setting"))
                    .font(.title2)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

private struct LanguageSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("Interface Language")) {
                Picker(settings.localized("App Language"), selection: $settings.appLanguage) {
                    ForEach(AppLanguage.allCases) { language in
                        Text(settings.localized(language.label)).tag(language)
                    }
                }
                Text(settings.localized("ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(settings.localized("Language"))
    }
}

private struct SettingsAboutView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("App")) {
                HStack {
                    Text(settings.localized("Version"))
                    Spacer()
                    Text(ARMSX2Bridge.buildVersion())
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
            }
        }
        .navigationTitle(settings.localized("About"))
    }
}

private struct NetworkSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("PS2 HDD")) {
                Toggle(settings.localized("Enable DEV9 Virtual HDD"), isOn: $settings.dev9HddEnabled)

                HStack {
                    Text(settings.localized("Image"))
                    Spacer()
                    Text(settings.dev9HddFile.isEmpty ? "DEV9hdd.raw" : settings.dev9HddFile)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }

                Button(settings.localized("Use Default HDD Image")) {
                    settings.dev9HddFile = "DEV9hdd.raw"
                }

                Text(settings.localized("Matches ARMSX2 Android's DEV9 HDD option. Requires a VM restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Online / Ethernet")) {
                Toggle(settings.localized("Enable DEV9 Ethernet"), isOn: $settings.dev9EthernetEnabled)

                HStack {
                    Text(settings.localized("Mode"))
                    Spacer()
                    Text(settings.localized("Sockets"))
                        .foregroundStyle(.secondary)
                }

                Picker(settings.localized("Adapter"), selection: $settings.dev9EthDevice) {
                    ForEach(ARMSX2Bridge.dev9NetworkAdapters(), id: \.self) { adapter in
                        Text(adapter).tag(adapter)
                    }
                }

                Toggle(settings.localized("Intercept DHCP"), isOn: $settings.dev9InterceptDHCP)
                Toggle(settings.localized("Log DHCP"), isOn: $settings.dev9EthLogDHCP)
                Toggle(settings.localized("Log DNS"), isOn: $settings.dev9EthLogDNS)

                Text(settings.localized("Sockets is the iOS-safe DEV9 Ethernet mode exposed here. PCAP bridged/switched modes are compiled out of this iOS build, so they are intentionally not selectable until a real iOS backend exists."))
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
    }
}
