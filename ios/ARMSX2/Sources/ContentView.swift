//
//  ContentView.swift
//  ARMSX2
//
//  Main content view and navigation
//

import SwiftUI
import MetalKit

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @StateObject private var emulatorState = EmulatorState()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            switch appState.currentView {
            case .gameLibrary:
                GameLibraryView()
                    .environmentObject(emulatorState)
            case .emulator:
                EmulatorView()
                    .environmentObject(emulatorState)
            case .settings:
                SettingsView()
            }
        }
        .sheet(isPresented: $appState.showSettings) {
            SettingsView()
        }
    }
}

// Game Library View
struct GameLibraryView: View {
    @EnvironmentObject var appState: AppState
    @EnvironmentObject var emulatorState: EmulatorState
    @State private var games: [GameInfo] = []
    @State private var showFilePicker = false
    @State private var showBIOSAlert = false

    var body: some View {
        NavigationView {
            VStack {
                // Header
                HStack {
                    Text("ARMSX2")
                        .font(.largeTitle)
                        .fontWeight(.bold)
                        .foregroundColor(.white)

                    Spacer()

                    Button(action: {
                        appState.showSettings = true
                    }) {
                        Image(systemName: "gearshape.fill")
                            .font(.title2)
                            .foregroundColor(.blue)
                    }
                }
                .padding()

                if games.isEmpty {
                    EmptyLibraryView(showFilePicker: $showFilePicker)
                } else {
                    GameGridView(games: games, onGameSelected: loadGame)
                }
            }
            .background(Color.black)
            .fileImporter(
                isPresented: $showFilePicker,
                allowedContentTypes: [.data],
                allowsMultipleSelection: false
            ) { result in
                handleFileImport(result)
            }
            .alert("BIOS Required", isPresented: $showBIOSAlert) {
                Button("OK", role: .cancel) { }
                Button("Select BIOS") {
                    // Open BIOS selection
                }
            } message: {
                Text("Please configure a PS2 BIOS file in Settings before loading games.")
            }
        }
    }

    private func loadGame(_ game: GameInfo) {
        print("[ARMSX2] Loading game: \(game.name)")

        // Check if BIOS is configured
        if !emulatorState.isBIOSConfigured {
            showBIOSAlert = true
            return
        }

        appState.selectedGame = game
        appState.currentView = .emulator
        emulatorState.loadGame(game)
    }

    private func handleFileImport(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            if let url = urls.first {
                let game = GameInfo(
                    name: url.deletingPathExtension().lastPathComponent,
                    path: url.path,
                    coverImage: nil,
                    region: "Unknown",
                    lastPlayed: nil
                )
                games.append(game)
            }
        case .failure(let error):
            print("[ARMSX2] File import error: \(error)")
        }
    }
}

// Empty Library View
struct EmptyLibraryView: View {
    @Binding var showFilePicker: Bool

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 80))
                .foregroundColor(.gray)

            Text("No Games Yet")
                .font(.title)
                .foregroundColor(.white)

            Text("Add PS2 games to get started")
                .font(.body)
                .foregroundColor(.gray)

            Button(action: {
                showFilePicker = true
            }) {
                Label("Add Game", systemImage: "plus.circle.fill")
                    .font(.headline)
                    .padding()
                    .background(Color.blue)
                    .foregroundColor(.white)
                    .cornerRadius(10)
            }
            .padding(.top, 20)
        }
    }
}

// Game Grid View
struct GameGridView: View {
    let games: [GameInfo]
    let onGameSelected: (GameInfo) -> Void

    let columns = [
        GridItem(.adaptive(minimum: 150))
    ]

    var body: some View {
        ScrollView {
            LazyVGrid(columns: columns, spacing: 20) {
                ForEach(games) { game in
                    GameCardView(game: game)
                        .onTapGesture {
                            onGameSelected(game)
                        }
                }
            }
            .padding()
        }
    }
}

// Game Card View
struct GameCardView: View {
    let game: GameInfo

    var body: some View {
        VStack {
            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .fill(Color.gray.opacity(0.3))
                    .aspectRatio(0.7, contentMode: .fit)

                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 50))
                    .foregroundColor(.white.opacity(0.5))
            }

            Text(game.name)
                .font(.caption)
                .foregroundColor(.white)
                .lineLimit(2)
                .multilineTextAlignment(.center)
        }
    }
}

// Emulator View
struct EmulatorView: View {
    @EnvironmentObject var appState: AppState
    @EnvironmentObject var emulatorState: EmulatorState
    @State private var showControls = true
    @State private var showMenu = false

    var body: some View {
        ZStack {
            // Metal rendering view
            MetalRenderView()
                .edgesIgnoringSafeArea(.all)

            // Overlay controls
            VStack {
                // Top bar
                if showControls {
                    TopBarView(
                        fps: emulatorState.fps,
                        onMenuTap: { showMenu = true },
                        onBackTap: {
                            emulatorState.stop()
                            appState.currentView = .gameLibrary
                        }
                    )
                }

                Spacer()

                // Virtual controller
                if showControls {
                    VirtualControllerView()
                        .environmentObject(emulatorState)
                }
            }

            // Menu overlay
            if showMenu {
                EmulatorMenuView(isPresented: $showMenu)
                    .environmentObject(emulatorState)
            }
        }
        .onAppear {
            emulatorState.start()
        }
        .statusBar(hidden: true)
    }
}

// Metal Render View
struct MetalRenderView: UIViewRepresentable {
    func makeUIView(context: Context) -> MTKView {
        let mtkView = MTKView()
        mtkView.device = MTLCreateSystemDefaultDevice()
        mtkView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        mtkView.preferredFramesPerSecond = 60

        // Setup emulator rendering
        let bridge = EmulatorBridge.shared()
        bridge.setupRenderSurface(mtkView)

        return mtkView
    }

    func updateUIView(_ uiView: MTKView, context: Context) {
        // Update if needed
    }
}

// Top Bar View
struct TopBarView: View {
    let fps: Double
    let onMenuTap: () -> Void
    let onBackTap: () -> Void

    var body: some View {
        HStack {
            Button(action: onBackTap) {
                Image(systemName: "chevron.left")
                    .font(.title2)
                    .foregroundColor(.white)
            }

            Text(String(format: "%.1f FPS", fps))
                .font(.caption)
                .foregroundColor(.white)

            Spacer()

            Button(action: onMenuTap) {
                Image(systemName: "line.3.horizontal")
                    .font(.title2)
                    .foregroundColor(.white)
            }
        }
        .padding()
        .background(Color.black.opacity(0.5))
    }
}

// Virtual Controller View
struct VirtualControllerView: View {
    @EnvironmentObject var emulatorState: EmulatorState

    var body: some View {
        HStack {
            // D-Pad
            DPadView()

            Spacer()

            // Action buttons
            ActionButtonsView()
        }
        .padding()
    }
}

// D-Pad View
struct DPadView: View {
    var body: some View {
        VStack(spacing: 5) {
            Button(action: { pressButton("up") }) {
                Image(systemName: "arrowtriangle.up.fill")
                    .font(.title)
            }

            HStack(spacing: 5) {
                Button(action: { pressButton("left") }) {
                    Image(systemName: "arrowtriangle.left.fill")
                        .font(.title)
                }

                Circle()
                    .fill(Color.clear)
                    .frame(width: 40, height: 40)

                Button(action: { pressButton("right") }) {
                    Image(systemName: "arrowtriangle.right.fill")
                        .font(.title)
                }
            }

            Button(action: { pressButton("down") }) {
                Image(systemName: "arrowtriangle.down.fill")
                    .font(.title)
            }
        }
        .foregroundColor(.white.opacity(0.7))
    }

    private func pressButton(_ button: String) {
        let bridge = EmulatorBridge.shared()
        bridge.pressButton(button)
    }
}

// Action Buttons View
struct ActionButtonsView: View {
    var body: some View {
        VStack(spacing: 10) {
            HStack(spacing: 10) {
                ControlButton(label: "△", action: "triangle")
                ControlButton(label: "○", action: "circle")
            }
            HStack(spacing: 10) {
                ControlButton(label: "□", action: "square")
                ControlButton(label: "✕", action: "cross")
            }
        }
    }
}

// Control Button
struct ControlButton: View {
    let label: String
    let action: String

    var body: some View {
        Button(action: {
            let bridge = EmulatorBridge.shared()
            bridge.pressButton(action)
        }) {
            Text(label)
                .font(.title2)
                .frame(width: 50, height: 50)
                .background(Color.white.opacity(0.3))
                .foregroundColor(.white)
                .clipShape(Circle())
        }
    }
}

// Emulator Menu View
struct EmulatorMenuView: View {
    @Binding var isPresented: Bool
    @EnvironmentObject var emulatorState: EmulatorState

    var body: some View {
        ZStack {
            Color.black.opacity(0.8)
                .edgesIgnoringSafeArea(.all)
                .onTapGesture {
                    isPresented = false
                }

            VStack(spacing: 20) {
                Text("Menu")
                    .font(.largeTitle)
                    .foregroundColor(.white)

                MenuButton(title: "Resume", icon: "play.fill") {
                    isPresented = false
                }

                MenuButton(title: "Save State", icon: "square.and.arrow.down") {
                    emulatorState.saveState(slot: 0)
                    isPresented = false
                }

                MenuButton(title: "Load State", icon: "square.and.arrow.up") {
                    emulatorState.loadState(slot: 0)
                    isPresented = false
                }

                MenuButton(title: "Reset", icon: "arrow.clockwise") {
                    emulatorState.reset()
                    isPresented = false
                }

                MenuButton(title: "Settings", icon: "gearshape") {
                    // Open settings
                }
            }
            .padding()
        }
    }
}

// Menu Button
struct MenuButton: View {
    let title: String
    let icon: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack {
                Image(systemName: icon)
                    .font(.title2)
                Text(title)
                    .font(.title3)
                Spacer()
            }
            .foregroundColor(.white)
            .padding()
            .background(Color.gray.opacity(0.3))
            .cornerRadius(10)
        }
        .frame(maxWidth: 300)
    }
}

// Settings View
struct SettingsView: View {
    @State private var biosPath = ""
    @State private var showFilePicker = false

    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("BIOS")) {
                    HStack {
                        Text("BIOS Path")
                        Spacer()
                        Button("Select") {
                            showFilePicker = true
                        }
                    }
                    if !biosPath.isEmpty {
                        Text(biosPath)
                            .font(.caption)
                            .foregroundColor(.gray)
                    }
                }

                Section(header: Text("Performance")) {
                    Toggle("Enable JIT", isOn: .constant(true))
                    Toggle("Frame Limiting", isOn: .constant(true))
                }

                Section(header: Text("Graphics")) {
                    Picker("Renderer", selection: .constant(0)) {
                        Text("Metal").tag(0)
                        Text("OpenGL").tag(1)
                    }
                }

                Section(header: Text("About")) {
                    let info = EmulatorBridge.shared().getEmulatorInfo() as? [String: Any]
                    Text("Version: \(info?["version"] as? String ?? "Unknown")")
                    Text("Core: \(info?["core"] as? String ?? "Unknown")")
                    Text("JIT Status: \(info?["jit_status"] as? String ?? "Unknown")")
                }
            }
            .navigationTitle("Settings")
        }
        .fileImporter(
            isPresented: $showFilePicker,
            allowedContentTypes: [.data],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                biosPath = url.path

                // Initialize emulator with BIOS
                let bridge = EmulatorBridge.shared()
                do {
                    try bridge.initialize(withBIOSPath: biosPath)
                } catch {
                    print("[ARMSX2] BIOS initialization error: \(error)")
                }
            }
        }
    }
}

// Emulator State Manager
class EmulatorState: NSObject, ObservableObject, EmulatorBridgeDelegate {
    @Published var fps: Double = 0.0
    @Published var isRunning: Bool = false
    @Published var currentGame: GameInfo?
    @Published var isBIOSConfigured: Bool = false

    private let bridge: EmulatorBridge

    override init() {
        bridge = EmulatorBridge.shared()
        super.init()
        bridge.delegate = self
    }

    func loadGame(_ game: GameInfo) {
        currentGame = game
        do {
            try bridge.loadGame(game.path)
        } catch {
            print("[ARMSX2] Error loading game: \(error)")
        }
    }

    func start() {
        bridge.start()
        isRunning = true
    }

    func pause() {
        bridge.pause()
        isRunning = false
    }

    func stop() {
        bridge.stop()
        isRunning = false
    }

    func reset() {
        bridge.reset()
    }

    func saveState(slot: Int) {
        bridge.saveState(slot)
    }

    func loadState(slot: Int) {
        bridge.loadState(slot)
    }

    // MARK: - EmulatorBridgeDelegate

    func emulatorDidUpdateFrame(_ fps: Double) {
        DispatchQueue.main.async {
            self.fps = fps
        }
    }

    func emulatorStateDidChange(_ state: EmulatorState) {
        // Handle state changes
    }

    func emulatorDidEncounterError(_ error: String) {
        print("[ARMSX2] Emulator error: \(error)")
    }
}
