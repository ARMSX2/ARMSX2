//
//  ARMSX2App.swift
//  ARMSX2
//
//  Main iOS application entry point
//

import SwiftUI

@main
struct ARMSX2App: App {
    @StateObject private var appState = AppState()

    init() {
        print("[ARMSX2] iOS App Starting - Version 1.0.0")
        print("[ARMSX2] Target: iOS 26+")
        print("[ARMSX2] JIT Compilation: Enabled")

        // Initialize emulator bridge
        let bridge = EmulatorBridge.shared()
        let info = bridge.getEmulatorInfo()
        print("[ARMSX2] Emulator Info: \(info)")
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appState)
                .preferredColorScheme(.dark)
                .statusBar(hidden: true)
        }
    }
}

// App-wide state management
class AppState: ObservableObject {
    @Published var currentView: AppView = .gameLibrary
    @Published var selectedGame: GameInfo?
    @Published var isEmulating: Bool = false
    @Published var showSettings: Bool = false

    enum AppView {
        case gameLibrary
        case emulator
        case settings
    }

    init() {
        print("[ARMSX2-AppState] Initialized")
    }
}

// Game information model
struct GameInfo: Identifiable, Hashable {
    let id = UUID()
    let name: String
    let path: String
    let coverImage: String?
    let region: String
    let lastPlayed: Date?

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }

    static func == (lhs: GameInfo, rhs: GameInfo) -> Bool {
        lhs.id == rhs.id
    }
}
