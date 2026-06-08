// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import GameController

private let runtimeMenuStateChangedNotification = Notification.Name("ARMSX2iOSRuntimeMenuStateChanged")

struct CompatibilityPreset: Identifiable {
    let id: String
    let title: String
    let systemImage: String
}

let compatibilityPresets: [CompatibilityPreset] = [
    CompatibilityPreset(id: "off", title: "Off / Default", systemImage: "power"),
    CompatibilityPreset(id: "cop1", title: "COP1 Everything Only", systemImage: "function"),
    CompatibilityPreset(id: "loadstore", title: "COP1 + EE Load/Store", systemImage: "arrow.left.arrow.right"),
    CompatibilityPreset(id: "mmi", title: "COP1 + EE MMI", systemImage: "rectangle.3.group"),
    CompatibilityPreset(id: "cop2vu", title: "COP1 + EE COP2/VU Macro", systemImage: "cube.transparent"),
    CompatibilityPreset(id: "multdiv", title: "COP1 + EE Mult/Div", systemImage: "multiply"),
    CompatibilityPreset(id: "shifts", title: "COP1 + EE Shifts", systemImage: "arrow.left.and.right"),
    CompatibilityPreset(id: "moves", title: "COP1 + EE Moves/HI-LO", systemImage: "arrow.triangle.swap"),
    CompatibilityPreset(id: "integeralu", title: "COP1 + EE Integer ALU", systemImage: "plus.forwardslash.minus"),
    CompatibilityPreset(id: "branches", title: "COP1 + EE Branches/Jumps", systemImage: "arrow.triangle.branch"),
]

private struct GameScreenSizePreferenceKey: PreferenceKey {
    static let defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        value = nextValue()
    }
}

struct GameScreenView: View {
    // MARK: - State & Constants

    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var userVirtualPadVisible = true
    @State private var externalControllerConnected = false
    @State private var fullScreen = false
    @State private var menuButtonHidden = false
    @State private var vmMenuAvailable = false
    @State private var gameMenuAvailable = false
    @State private var showSaveStates = false
    @State private var showSpeedControl = false
    @State private var showCompatibilityLab = false
    @State private var showPerGameSettings = false
    @State private var showPNACHImporter = false
    @State private var showPadLayoutEditor = false
    @State private var showResetConfirmation = false
    @State private var runtimePerGameSettingsEntry: ISOEntry?
    @State private var runtimePerGameSettings: [String: Any]?
    @State private var compatibilityPresetKey = "off"
    @State private var compatibilityIdentity = ""
    @State private var compatibilityAutoPresets = true
    @State private var statusMessage: String? = nil
    @State private var statusMessageGeneration = 0
    @State private var statusMessageDismissTask: Task<Void, Never>?
    @State private var runtimeOverlayPauseActive = false
    @State private var previousHideHomeIndicator = false

    @Environment(\.scenePhase) private var scenePhase

    private static let briefStatusDisplayDuration: TimeInterval = 2.2
    private static let importantStatusDisplayDuration: TimeInterval = 6.0

    // MARK: - Body

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height

            Group {
                if isLandscape {
                    // Landscape: full-screen layout so pad coordinates match the layout editor.
                    ZStack {
                        MetalGameView()
                        if effectiveVirtualPadVisible {
                            VirtualControllerView(isLandscape: true)
                        }
                        menuButtonOverlay(isLandscape: true)
                    }
                    .ignoresSafeArea()
                } else {
                    // Portrait: split layout. Full-phone skins are skipped until viewport metadata is parsed.
                    ZStack {
                        VStack(spacing: 0) {
                            MetalGameView()
                                .frame(height: geo.size.height / 2)
                            if effectiveVirtualPadVisible {
                                VirtualControllerView()
                                    .frame(height: geo.size.height / 2)
                            } else {
                                Spacer()
                            }
                        }
                        .overlay(alignment: .topTrailing) {
                            menuButtonOverlay(isLandscape: false)
                        }
                    }
                }
            }
            .preference(key: GameScreenSizePreferenceKey.self, value: geo.size)
        }
        .onPreferenceChange(GameScreenSizePreferenceKey.self) { _ in
            syncFullscreenStateFromWindow()
        }
        .sheet(isPresented: $showSaveStates) {
            SaveStatesPanel { message, isImportant in
                presentStatusMessage(
                    message,
                    displayDuration: isImportant ? Self.importantStatusDisplayDuration : Self.briefStatusDisplayDuration
                )
            }
        }
        .sheet(isPresented: $showSpeedControl) {
            SpeedControlPanel(settings: settings)
                .presentationDetents([.medium])
        }
        .sheet(isPresented: $showCompatibilityLab) {
            compatibilityLabPanel
                .presentationDetents([.medium, .large])
        }
        .fullScreenCover(isPresented: $showPadLayoutEditor) {
            PadLayoutEditView()
        }
        .sheet(isPresented: $showPNACHImporter) {
            ImportDocumentPicker(
                allowedContentTypes: FileImportHandler.pnachContentTypes,
                allowsMultipleSelection: true
            ) { result in
                showPNACHImporter = false
                switch result {
                case .success(let urls):
                    let message = fileImporter.importPNACHURLs(urls, asCheat: true, presentsAlert: false)
                    presentPNACHImportResult(message)
                case .failure(let error):
                    if !FileImportHandler.isUserCancelledPickerError(error) {
                        fileImporter.presentImportResult(FileImportHandler.failedPNACHPickerMessage(errorDescription: error.localizedDescription))
                    }
                }
            }
        }
        .overlay(alignment: .bottom) {
            statusToastOverlay
        }
        .sheet(isPresented: $showPerGameSettings) {
            runtimePerGameSettingsContent
                .presentationDetents([.large])
                .presentationDragIndicator(.visible)
                .presentationBackground(.regularMaterial)
                .presentationCornerRadius(34)
        }
        .alert(settings.localized("Reset ROM?"), isPresented: $showResetConfirmation) {
            Button(settings.localized("Cancel"), role: .cancel) {}
            Button(settings.localized("Reset ROM"), role: .destructive) {
                resetCurrentROM()
            }
        } message: {
            Text(settings.localized("Restart the current game? Unsaved progress will be lost."))
        }
        .onAppear {
            enterGameplaySystemChromeMode()
            syncFullscreenStateFromWindow()
            applyInitialFullscreenPreference()
            refreshExternalControllerConnectionState()
            refreshRuntimeMenuState()
        }
        .onDisappear {
            statusMessageDismissTask?.cancel()
            leaveGameplaySystemChromeMode()
        }
        .simultaneousGesture(
            TapGesture(count: 2).onEnded {
                restoreMenuButtonIfHidden()
            }
        )
        .onChange(of: showSaveStates) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showSpeedControl) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showCompatibilityLab) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPerGameSettings) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPNACHImporter) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPadLayoutEditor) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showResetConfirmation) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: scenePhase) { _, newPhase in
            if newPhase == .active {
                syncFullscreenStateFromWindow()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
            refreshRuntimeMenuState()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            refreshExternalControllerConnectionState()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            refreshExternalControllerConnectionState()
        }
        .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
            refreshRuntimeMenuState()
        }
        .persistentSystemOverlays(.hidden)
    }

    // MARK: - Layout Views

    @ViewBuilder
    private func menuButtonOverlay(isLandscape: Bool) -> some View {
        if !menuButtonHidden {
            VStack {
                HStack {
                    Spacer()
                    menuButton()
                }
                .padding(.top, isLandscape ? 8 : 4)
                .padding(.trailing, isLandscape ? 8 : 4)
                Spacer()
            }
        }
    }

    private func menuButton() -> some View {
        Menu {
            Toggle(isOn: Binding(
                get: { settings.osdPreset != .off },
                set: { newValue in
                    if newValue {
                        settings.osdPreset = settings.lastActiveOsdPreset
                        ARMSX2Bridge.setPerformanceOverlayVisible(true)
                    } else {
                        settings.osdPreset = .off
                        ARMSX2Bridge.setPerformanceOverlayVisible(false)
                    }
                }
            )) {
                Label(settings.localized("OSD"), systemImage: "speedometer")
            }
            Toggle(isOn: $userVirtualPadVisible) {
                Label(settings.localized("Virtual Pad"), systemImage: "gamecontroller")
            }
            if virtualPadHiddenByController {
                Text(settings.localized("Hidden while controller is connected"))
            }
            Toggle(isOn: Binding(
                get: { fullScreen },
                set: { newValue in
                    fullScreen = newValue
                    ARMSX2Bridge.setFullScreen(newValue)
                }
            )) {
                Label(settings.localized("Full Screen"), systemImage: "arrow.up.left.and.arrow.down.right")
            }
            Toggle(isOn: Binding(
                get: { menuButtonHidden || settings.hideMenuButton },
                set: { newValue in
                    settings.hideMenuButton = newValue
                    menuButtonHidden = newValue
                    if newValue {
                        presentStatusMessage(settings.localized("Double-tap empty gameplay space to show the menu button again."))
                    }
                }
            )) {
                Label(settings.localized("Hide Menu Button"), systemImage: "eye.slash")
            }
            Button {
                presentQuickMenuPanel("pad_layout") {
                    showPadLayoutEditor = true
                }
            } label: {
                Label(settings.localized("Edit Virtual Pad Layout"), systemImage: "square.resize")
            }

            Divider()

            Button {
                presentQuickMenuPanel("compatibility_lab") {
                    refreshCompatibilityState()
                    showCompatibilityLab = true
                }
            } label: {
                Label(settings.localized("Compatibility Lab"), systemImage: "wand.and.stars")
            }

            if gameMenuAvailable {
                Button {
                    presentQuickMenuPanel("per_game_settings") {
                        openPerGameSettingsForCurrentGame()
                    }
                } label: {
                    Label(settings.localized("Per-Game Settings"), systemImage: "slider.horizontal.3")
                }
            }

            if vmMenuAvailable {
                Button {
                    presentQuickMenuPanel("speed_control") {
                        showSpeedControl = true
                    }
                } label: {
                    Label(settings.localized("Speed / FPS Target"), systemImage: "speedometer")
                }

                Button {
                    presentQuickMenuPanel("reset_rom") {
                        showResetConfirmation = true
                    }
                } label: {
                    Label(settings.localized("Reset ROM"), systemImage: "arrow.counterclockwise.circle")
                }
            }

            if gameMenuAvailable || vmMenuAvailable {
                Button {
                    presentQuickMenuPanel("save_states") {
                        showSaveStates = true
                    }
                } label: {
                    Label(settings.localized("Save / Load States"), systemImage: "square.stack.3d.up.fill")
                }
            }

            if vmMenuAvailable {
                discSwapMenu
            }

            if gameMenuAvailable {
                Button {
                    presentQuickMenuPanel("pnach_import") {
                        showPNACHImporter = true
                    }
                } label: {
                    Label(settings.localized("Import PNACH / 60 FPS Patch"), systemImage: "wand.and.stars")
                }

                Button {
                    clearCurrentGameCache()
                } label: {
                    Label(settings.localized("Clear Current Game Cache"), systemImage: "trash.slash")
                }
            }

            Divider()
            Button {
                appState.returnToMenu()
            } label: {
                Label(settings.localized("Back to Menu"), systemImage: "list.bullet")
            }
        } label: {
            Image(systemName: "ellipsis.circle.fill")
                .font(.title3)
                .foregroundStyle(.white.opacity(0.5))
                .padding(6)
                .background(.black.opacity(0.15), in: Circle())
        }
    }

    private func presentQuickMenuPanel(_ name: String, _ action: @escaping () -> Void) {
        NSLog("[ARMSX2 iOS QuickMenu] present \(name)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            action()
        }
    }

    private var discSwapMenu: some View {
        Menu {
            Button {
                ejectDisc()
            } label: {
                Label(settings.localized("Eject Disc"), systemImage: "eject")
            }

            let discs = availableDiscSwapNames
            if discs.isEmpty {
                Text(settings.localized("No disc images found"))
            } else {
                Menu {
                    ForEach(discs, id: \.self) { discName in
                        Button {
                            changeDisc(to: discName)
                        } label: {
                            Label(discName, systemImage: "opticaldisc")
                        }
                    }
                } label: {
                    Label(settings.localized("Insert Disc (No Reboot)"), systemImage: "tray.and.arrow.down")
                }

                Menu {
                    ForEach(discs, id: \.self) { discName in
                        Button {
                            restartWithDisc(discName)
                        } label: {
                            Label(discName, systemImage: "arrow.clockwise.circle")
                        }
                    }
                } label: {
                    Label(settings.localized("Restart With Disc"), systemImage: "arrow.clockwise.circle")
                }
            }
        } label: {
            Label(settings.localized("Change Disc"), systemImage: "opticaldisc")
        }
    }

    // MARK: - Runtime Panels

    private var compatibilityLabPanel: some View {
        NavigationStack {
            Form {
                compatibilityStatusSection
                compatibilityResetSection
                compatibilityFlagsSection
                if !compatibilityIdentity.isEmpty {
                    compatibilityForgetSection
                }
            }
            .navigationTitle(settings.localized("Compatibility Lab"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        showCompatibilityLab = false
                    }
                }
            }
            .onAppear(perform: refreshCompatibilityState)
        }
    }

    private var compatibilityStatusSection: some View {
        let currentPreset = compatibilityPreset(for: compatibilityPresetKey)
        return Section {
            Toggle(isOn: Binding(
                get: { compatibilityAutoPresets },
                set: { newValue in
                    compatibilityAutoPresets = newValue
                    ARMSX2Bridge.setCompatibilityAutoGamePresetsEnabled(newValue)
                    refreshCompatibilityState()
                }
            )) {
                Label(settings.localized("Auto Game Presets"), systemImage: "sparkles")
            }

            LabeledContent(settings.localized("Current Mode")) {
                Text(settings.localized(currentPreset.title))
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.trailing)
            }

            if !compatibilityIdentity.isEmpty {
                LabeledContent(settings.localized("Current Game")) {
                    Text(compatibilityIdentity)
                        .foregroundStyle(.secondary)
                }
            } else {
                Text(settings.localized("Start a game to remember presets per title."))
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text(settings.localized("Status"))
        } footer: {
            Text(settings.localized("Auto Game Presets applies known safe defaults. Manual flags below are remembered for the current game when a game is running."))
        }
    }

    private var compatibilityResetSection: some View {
        Section {
            Button {
                applyCompatibilityPreset(compatibilityPreset(for: "off"))
            } label: {
                Label(settings.localized("Use Default / Clear Flags"), systemImage: "power")
            }
            .foregroundStyle(.primary)
        } header: {
            Text(settings.localized("Reset"))
        } footer: {
            Text(settings.localized("Use this when testing is done or a game behaves worse with compatibility flags enabled."))
        }
    }

    private var compatibilityFlagsSection: some View {
        Section {
            compatibilityLabToggle(
                "COP1EverythingOnly",
                title: "COP1 Everything Only",
                systemImage: "function"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusLoadStore",
                title: "COP1 Everything + EE Load/Store",
                systemImage: "arrow.left.arrow.right"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusMMI",
                title: "COP1 Everything + EE MMI",
                systemImage: "rectangle.3.group"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusCOP2VU",
                title: "COP1 Everything + EE COP2/VU Macro",
                systemImage: "cube.transparent"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusMultDiv",
                title: "COP1 Everything + EE Mult/Div",
                systemImage: "multiply"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusShifts",
                title: "COP1 Everything + EE Shifts",
                systemImage: "arrow.left.and.right"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusMoves",
                title: "COP1 Everything + EE Moves/HI-LO",
                systemImage: "arrow.triangle.swap"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusIntegerALU",
                title: "COP1 Everything + EE Integer ALU",
                systemImage: "plus.forwardslash.minus"
            )
            compatibilityLabToggle(
                "COP1EverythingPlusBranches",
                title: "COP1 Everything + EE Branches/Jumps",
                systemImage: "arrow.triangle.branch"
            )
        } header: {
            Text(settings.localized("Manual Compatibility Flags"))
        } footer: {
            Text(settings.localized("Toggle one or more flags when a game needs compatibility help. Changing any flag switches this game to Custom Advanced Flags."))
        }
    }

    private var compatibilityForgetSection: some View {
        Section {
            Button(role: .destructive) {
                let identity = compatibilityIdentity
                ARMSX2Bridge.forgetCompatibilityPresetForCurrentGame()
                refreshCompatibilityState()
                presentStatusMessage("\(settings.localized("Compatibility preset reset for")) \(identity)")
            } label: {
                Label(settings.localized("Forget This Game's Override"), systemImage: "trash")
            }
        }
    }

    @ViewBuilder
    private var runtimePerGameSettingsContent: some View {
        if let runtimePerGameSettingsEntry {
            PerGameSettingsPanel(game: runtimePerGameSettingsEntry, preloadedSettings: runtimePerGameSettings, savesToRunningGame: true) {
                closePerGameSettingsOverlay()
            }
        } else {
            NavigationStack {
                ContentUnavailableView(
                    settings.localized("No Game Active"),
                    systemImage: "gamecontroller",
                    description: Text(settings.localized("Start a game before changing per-game settings."))
                )
                .navigationTitle(settings.localized("Per-Game Settings"))
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button(settings.localized("Done")) {
                            closePerGameSettingsOverlay()
                        }
                    }
                }
            }
        }
    }

    // MARK: - Lifecycle & Events

    private func enterGameplaySystemChromeMode() {
        previousHideHomeIndicator = appState.hideHomeIndicator
        appState.hideHomeIndicator = true
    }

    private func leaveGameplaySystemChromeMode() {
        appState.hideHomeIndicator = previousHideHomeIndicator
    }

    private func applyInitialFullscreenPreference() {
        menuButtonHidden = settings.hideMenuButton
        if settings.autoFullscreen && !ARMSX2Bridge.isSDLFullscreen() {
            fullScreen = true
            ARMSX2Bridge.setFullScreen(true)
        }
    }

    private func syncFullscreenStateFromWindow() {
        let sdlFullscreen = ARMSX2Bridge.isSDLFullscreen()
        if fullScreen != sdlFullscreen {
            fullScreen = sdlFullscreen
        }
    }

    private func restoreMenuButtonIfHidden() {
        guard menuButtonHidden else { return }

        menuButtonHidden = false
        settings.hideMenuButton = false
        presentStatusMessage(settings.localized("Menu button shown"))
    }

    private func updateRuntimeOverlayPause() {
        let shouldPause = showSaveStates || showSpeedControl || showCompatibilityLab || showPerGameSettings || showPNACHImporter || showPadLayoutEditor || showResetConfirmation
        guard runtimeOverlayPauseActive != shouldPause else { return }

        runtimeOverlayPauseActive = shouldPause
        if ARMSX2Bridge.isVMRunning() {
            ARMSX2Bridge.setVMPaused(shouldPause)
        }
    }

    private func refreshRuntimeMenuState() {
        let vmRunning = ARMSX2Bridge.isVMRunning()
        let gameReady = ARMSX2Bridge.hasValidSaveStateGame()
        if vmMenuAvailable != vmRunning {
            vmMenuAvailable = vmRunning
        }
        if gameMenuAvailable != gameReady {
            gameMenuAvailable = gameReady
        }
        refreshCompatibilityState()
    }

    private func refreshExternalControllerConnectionState() {
        let connected = !GCController.controllers().isEmpty
        if externalControllerConnected != connected {
            externalControllerConnected = connected
        }
    }

    // MARK: - Game Identity Helpers

    private func currentRuntimeGameName() -> String? {
        if let gameName = normalizedRuntimeGameName(appState.runningGameName) {
            return gameName
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentGameISOName()) {
            return gameName
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentISOPath()) {
            return gameName
        }

        let bootISO = ARMSX2Bridge.getINIString("GameISO", key: "BootISO", defaultValue: "")
        if let gameName = normalizedRuntimeGameName(bootISO) {
            return gameName
        }

        return gameNameMatchingRuntimeIdentity()
    }

    private func normalizedRuntimeGameName(_ value: String?) -> String? {
        guard var value = value?.trimmingCharacters(in: .whitespacesAndNewlines),
              !value.isEmpty else {
            return nil
        }

        value = value.trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
        let fileName = (value as NSString).lastPathComponent
        guard !fileName.isEmpty,
              fileName != "BIOS",
              fileName != "AutoBoot" else {
            return nil
        }

        return fileName
    }

    private func gameNameMatchingRuntimeIdentity() -> String? {
        let identity = normalizedRuntimeIdentity(ARMSX2Bridge.compatibilityIdentityForCurrentGame())
        guard !identity.isEmpty else {
            return nil
        }

        for gameName in ARMSX2Bridge.availableISOs() {
            let metadata = ARMSX2Bridge.gameMetadata(forISO: gameName)
            let serial = normalizedRuntimeIdentity(metadata["serial"])
            if !serial.isEmpty && serial == identity {
                return gameName
            }

            if let crc = metadata["crc"]?.trimmingCharacters(in: .whitespacesAndNewlines).uppercased(),
               !crc.isEmpty,
               (identity == crc || identity == "CRC-\(crc)") {
                return gameName
            }
        }

        return nil
    }

    private func normalizedRuntimeIdentity(_ value: String?) -> String {
        (value ?? "")
            .replacingOccurrences(of: "_", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased()
    }

    // MARK: - Compatibility Helpers

    private func refreshCompatibilityState() {
        let preset = ARMSX2Bridge.compatibilityPresetForCurrentGame()
        let identity = ARMSX2Bridge.compatibilityIdentityForCurrentGame()
        let autoPresets = ARMSX2Bridge.isCompatibilityAutoGamePresetsEnabled()

        if compatibilityPresetKey != preset {
            compatibilityPresetKey = preset
        }
        if compatibilityIdentity != identity {
            compatibilityIdentity = identity
        }
        if compatibilityAutoPresets != autoPresets {
            compatibilityAutoPresets = autoPresets
        }
    }

    private func compatibilityPreset(for id: String) -> CompatibilityPreset {
        if let preset = compatibilityPresets.first(where: { $0.id == id }) {
            return preset
        }

        return CompatibilityPreset(id: "custom", title: "Custom Advanced Flags", systemImage: "slider.horizontal.3")
    }

    private func applyCompatibilityPreset(_ preset: CompatibilityPreset) {
        let rememberForCurrentGame = !compatibilityIdentity.isEmpty
        ARMSX2Bridge.setCompatibilityPreset(preset.id, rememberForCurrentGame: rememberForCurrentGame)
        refreshCompatibilityState()

        if rememberForCurrentGame {
            presentStatusMessage("\(settings.localized(preset.title)) \(settings.localized("saved for")) \(compatibilityIdentity)")
        } else {
            presentStatusMessage("\(settings.localized("Compatibility preset set to")) \(settings.localized(preset.title))")
        }
    }

    private func compatibilityLabToggle(_ key: String, title: String, systemImage: String) -> some View {
        Toggle(isOn: Binding(
            get: { ARMSX2Bridge.getJITBisectFlag(key, defaultValue: false) },
            set: {
                ARMSX2Bridge.setJITBisectFlag(key, value: $0)
                compatibilityPresetKey = "custom"
                if !compatibilityIdentity.isEmpty {
                    presentStatusMessage("\(settings.localized("Custom compatibility flags saved for")) \(compatibilityIdentity)")
                }
            }
        )) {
            Label(settings.localized(title), systemImage: systemImage)
        }
    }

    // MARK: - Actions

    private func openPerGameSettingsForCurrentGame() {
        // Use the VM-safe bridge path to avoid a disc-image scan while the game is running.
        guard let gameName = currentRuntimeGameName(),
              let info = ARMSX2Bridge.gameSettingsForCurrentGame() else {
            runtimePerGameSettingsEntry = nil
            runtimePerGameSettings = nil
            withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
                showPerGameSettings = true
            }
            presentImportantStatusMessage(settings.localized("Per-game settings need a running game."))
            return
        }

        let serial = (info["serial"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        runtimePerGameSettingsEntry = ISOEntry(
            name: gameName,
            fileURL: nil,
            bootPath: nil,
            coverURL: nil,
            coverSignature: nil,
            metadata: serial.isEmpty ? [:] : ["serial": serial],
            size: 0,
            isFavorite: false
        )
        runtimePerGameSettings = info
        withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
            showPerGameSettings = true
        }
    }

    private func closePerGameSettingsOverlay() {
        withAnimation(.easeInOut(duration: 0.2)) {
            showPerGameSettings = false
        }
        runtimePerGameSettingsEntry = nil
        runtimePerGameSettings = nil
        refreshRuntimeMenuState()
    }

    private func resetCurrentROM() {
        appState.resetCurrentVM()
        presentStatusMessage(settings.localized("Restarting ROM..."))
    }

    private func clearCurrentGameCache() {
        guard let gameName = currentRuntimeGameName() else {
            presentImportantStatusMessage(settings.localized("Cache clear needs a running game."))
            return
        }

        let message = ARMSX2Bridge.clearCache(forISO: gameName)
        presentStatusMessage(message)
    }

    private func changeDisc(to discName: String) {
        presentStatusMessage("Changing disc...")
        ARMSX2Bridge.changeDisc(toISO: discName) { success in
            Task { @MainActor in
                if success {
                    presentStatusMessage("\(discName) inserted. Use the game's disc-swap prompt if needed.")
                } else {
                    presentImportantStatusMessage("Could not change discs. Open the game's disc-swap prompt first, or restart with the target disc.")
                }
            }
        }
    }

    private func restartWithDisc(_ discName: String) {
        presentStatusMessage("Restarting with \(discName)...")
        appState.shutdownAndBoot(isoName: discName)
    }

    private func ejectDisc() {
        presentStatusMessage("Ejecting disc...")
        ARMSX2Bridge.ejectDisc { success in
            Task { @MainActor in
                if success {
                    presentStatusMessage("Disc ejected")
                } else {
                    presentImportantStatusMessage("Could not eject the disc. Try again after the game has finished loading.")
                }
            }
        }
    }

    // MARK: - Toast & Feedback

    @ViewBuilder
    private var statusToastOverlay: some View {
        if let statusMessage {
            Text(statusMessage)
                .font(.callout.weight(.semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(.black.opacity(0.72), in: Capsule())
                .padding(.bottom, 24)
                .transition(.opacity.combined(with: .move(edge: .bottom)))
        }
    }

    private func presentStatusMessage(
        _ message: String,
        displayDuration: TimeInterval = Self.briefStatusDisplayDuration
    ) {
        statusMessageDismissTask?.cancel()
        statusMessageGeneration += 1
        let currentGeneration = statusMessageGeneration
        withAnimation(.easeOut(duration: 0.18)) {
            statusMessage = message
        }
        statusMessageDismissTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(displayDuration))
            guard !Task.isCancelled else { return }
            guard statusMessageGeneration == currentGeneration else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                statusMessage = nil
            }
        }
    }

    private func presentImportantStatusMessage(_ message: String) {
        presentStatusMessage(message, displayDuration: Self.importantStatusDisplayDuration)
    }

    private func presentPNACHImportResult(_ message: String) {
        if Self.isPNACHImportSuccessMessage(message) {
            presentStatusMessage(message)
        } else {
            fileImporter.presentImportResult(message)
        }
    }

    private static func isPNACHImportSuccessMessage(_ message: String) -> Bool {
        let lines = message
            .split(separator: "\n")
            .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        return !lines.isEmpty && lines.allSatisfy { $0.hasPrefix("PNACH imported") }
    }

    // MARK: - Virtual Pad

    private var effectiveVirtualPadVisible: Bool {
        userVirtualPadVisible && (!settings.autoHideVirtualPadWhenControllerConnected || !externalControllerConnected)
    }

    private var virtualPadHiddenByController: Bool {
        userVirtualPadVisible && settings.autoHideVirtualPadWhenControllerConnected && externalControllerConnected
    }

    // MARK: - Disc Helpers

    private var availableDiscSwapNames: [String] {
        ARMSX2Bridge.availableISOs().filter { !$0.lowercased().hasSuffix(".elf") }
    }
}

// MARK: - Save States Panel

private struct SaveStatesPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var slots: [ARMSX2SaveStateSlotInfo] = []
    @State private var busySlot: Int? = nil
    @State private var pendingOverwrite: ARMSX2SaveStateSlotInfo? = nil

    let statusHandler: (String, Bool) -> Void

    var body: some View {
        NavigationStack {
            ScrollView {
                if slots.isEmpty {
                    VStack(spacing: 10) {
                        Image(systemName: "hourglass")
                            .font(.largeTitle)
                            .foregroundStyle(.secondary)
                        Text(settings.localized("Save states are not ready yet."))
                            .font(.headline)
                        Text(settings.localized("Wait until the game has fully identified, then try again."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(32)
                } else {
                    LazyVStack(spacing: 10) {
                        ForEach(slots, id: \.slot) { slot in
                            SaveStateSlotRow(
                                info: slot,
                                isBusy: busySlot == slot.slot,
                                onSave: { save(slot) },
                                onLoad: { load(slot) },
                                onOverwrite: { pendingOverwrite = slot },
                                settings: settings
                            )
                        }
                    }
                    .padding()
                }
            }
            .safeAreaInset(edge: .top) {
                Text(settings.localized("Empty slots can save. Occupied slots can load or overwrite."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                    .background(.regularMaterial)
            }
            .navigationTitle(settings.localized("Save / Load States"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
                refresh()
            }
            .confirmationDialog(
                "\(settings.localized("Overwrite Slot")) \(pendingOverwrite?.slot ?? 0)?",
                isPresented: Binding(
                    get: { pendingOverwrite != nil },
                    set: { newValue in
                        if !newValue {
                            pendingOverwrite = nil
                        }
                    }
                ),
                titleVisibility: .visible
            ) {
                Button(settings.localized("Overwrite"), role: .destructive) {
                    if let pendingOverwrite {
                        save(pendingOverwrite)
                    }
                    pendingOverwrite = nil
                }
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingOverwrite = nil
                }
            }
        }
    }

    private func refresh() {
        slots = ARMSX2Bridge.saveStateSlots()
    }

    private func save(_ slot: ARMSX2SaveStateSlotInfo) {
        let slotNumber = slot.slot
        busySlot = slotNumber
        ARMSX2Bridge.saveState(toSlot: slotNumber) { success in
            Task { @MainActor in
                busySlot = nil
                refresh()
                let message = success
                    ? "\(settings.localized("State saved to slot")) \(slotNumber)"
                    : "\(settings.localized("Could not save slot")) \(slotNumber). \(settings.localized("Try again after gameplay has fully loaded."))"
                statusHandler(message, !success)
            }
        }
    }

    private func load(_ slot: ARMSX2SaveStateSlotInfo) {
        let slotNumber = slot.slot
        busySlot = slotNumber
        ARMSX2Bridge.loadState(fromSlot: slotNumber) { success in
            Task { @MainActor in
                busySlot = nil
                refresh()
                let message = success
                    ? "\(settings.localized("State loaded from slot")) \(slotNumber)"
                    : "\(settings.localized("Could not load slot")) \(slotNumber). \(settings.localized("Make sure it has a saved state first."))"
                statusHandler(message, !success)
                if success {
                    dismiss()
                }
            }
        }
    }
}

// MARK: - Speed Control Panel

private struct SpeedControlPanel: View {
    @Bindable var settings: SettingsStore
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section(settings.localized("Frame Limiter")) {
                    Toggle(settings.localized("Enable Limiter"), isOn: $settings.frameLimiterEnabled)

                    if settings.frameLimiterEnabled {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Text(settings.localized("FPS Target"))
                                Spacer()
                                Text(Self.formatFPS(settings.targetFPS))
                                    .foregroundStyle(.secondary)
                                    .font(.callout.monospacedDigit())
                            }

                            Slider(
                                value: $settings.targetFPS,
                                in: SettingsStore.minTargetFPS...SettingsStore.maxTargetFPS,
                                step: 1.0
                            )

                            HStack {
                                quickTargetButton(30)
                                quickTargetButton(45)
                                quickTargetButton(60)
                                quickTargetButton(90)
                                quickTargetButton(120)
                            }
                        }
                    } else {
                        Text(settings.localized("Limiter is OFF. Games can run above normal speed and may draw more power."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Section(settings.localized("How It Works")) {
                    Text(settings.localized("This controls PCSX2 Normal Speed. On NTSC games, 60 FPS is normal speed and 30 FPS is about 50% speed. It is safe to change while a game is running."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    HStack {
                        Text(settings.localized("Normal Speed"))
                        Spacer()
                        Text(Self.formatPercent(settings.targetFPS / max(settings.ntscFramerate, 1.0)))
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }
                }
            }
            .navigationTitle(settings.localized("Speed / FPS Target"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
        }
    }

    private func quickTargetButton(_ fps: Float) -> some View {
        Button(Self.formatCompactFPS(fps)) {
            settings.frameLimiterEnabled = true
            settings.targetFPS = fps
        }
        .buttonStyle(.bordered)
        .font(.caption.monospacedDigit())
    }

    private static func formatFPS(_ value: Float) -> String {
        String(format: "%.0f FPS", value)
    }

    private static func formatCompactFPS(_ value: Float) -> String {
        String(format: "%.0f", value)
    }

    private static func formatPercent(_ scalar: Float) -> String {
        String(format: "%.0f%%", scalar * 100.0)
    }
}

// MARK: - Save State Slot Row

private struct SaveStateSlotRow: View {
    let info: ARMSX2SaveStateSlotInfo
    let isBusy: Bool
    let onSave: () -> Void
    let onLoad: () -> Void
    let onOverwrite: () -> Void
    let settings: SettingsStore

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                SaveStatePreview(data: info.previewPNGData, occupied: info.occupied)
                    .frame(width: 96, height: 72)

                VStack(alignment: .leading, spacing: 4) {
                    Text("\(settings.localized("Slot")) \(info.slot)")
                        .font(.headline)

                    if info.occupied {
                        if let modifiedDate = info.modifiedDate {
                            Text(Self.dateFormatter.string(from: modifiedDate))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Text(info.fileName)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    } else {
                        Text(settings.localized("Empty"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Spacer(minLength: 8)

                if isBusy {
                    ProgressView()
                        .frame(width: 88)
                } else if !info.occupied {
                    Button(action: onSave) {
                        Label(settings.localized("Save"), systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(.borderedProminent)
                }
            }

            if info.occupied && !isBusy {
                HStack(spacing: 8) {
                    Button(action: onLoad) {
                        Label(settings.localized("Load"), systemImage: "arrow.down.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .frame(maxWidth: .infinity)

                    Button(action: onOverwrite) {
                        Label(settings.localized("Overwrite"), systemImage: "arrow.triangle.2.circlepath")
                    }
                    .buttonStyle(.bordered)
                    .frame(maxWidth: .infinity)
                }
            }
        }
        .padding(12)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }

    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter
    }()
}

// MARK: - Save State Preview

private struct SaveStatePreview: View {
    let data: Data?
    let occupied: Bool

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(.black.opacity(0.12))

            if let data, let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            } else {
                Image(systemName: occupied ? "photo" : "tray")
                    .font(.title2)
                    .foregroundStyle(.secondary)
            }
        }
        .clipped()
    }
}
