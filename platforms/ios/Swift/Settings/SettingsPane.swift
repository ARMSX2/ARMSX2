// SettingsPane.swift — the panes the Settings root opens
// SPDX-License-Identifier: GPL-3.0+

enum SettingsPane: String, CaseIterable, Identifiable {
    case language
    case appearance
    case appIcon
    case emulator
    case graphics
    case shaders
    case texturePacks
    case framePacing
    case audio
    case network
    case memoryCards
    case storage
    case settingsPresets
    case retroAchievements
    case customSkin
    case overlay
    case gameController
    case controllerMacros
    case localMultiplayer
    case virtualPad
    case help
    case licenses
    case about
    case skinBrowser

    var id: String { rawValue }

    static var rootCases: [SettingsPane] {
        allCases.filter {
            $0 != .skinBrowser && $0 != .controllerMacros
        }
    }

    var title: String {
        switch self {
        case .language:
            return "Language"
        case .appearance:
            return "Appearance"
        case .appIcon:
            return "App Icon"
        case .emulator:
            return "Emulator"
        case .graphics:
            return "Graphics"
        case .shaders:
            return "Shaders"
        case .texturePacks:
            return "Texture Packs"
        case .framePacing:
            return "Frame Pacing"
        case .audio:
            return "Audio"
        case .network:
            return "Network"
        case .memoryCards:
            return "Memory Cards"
        case .storage:
            return "Storage"
        case .settingsPresets:
            return "Settings Presets"
        case .retroAchievements:
            return "RetroAchievements"
        case .customSkin:
            return "Custom Skin"
        case .overlay:
            return "Overlay (OSD)"
        case .gameController:
            return "Game Controller"
        case .controllerMacros:
            return "Controller Macros"
        case .localMultiplayer:
            return "Local Multiplayer"
        case .virtualPad:
            return "Virtual Pad"
        case .help:
            return "Help"
        case .licenses:
            return "Licenses & Credits"
        case .about:
            return "About"
        case .skinBrowser:
            return "Browse Skins"
        }
    }

    var icon: String {
        switch self {
        case .language:
            return "globe"
        case .appearance:
            return "paintpalette"
        case .appIcon:
            return "app.badge"
        case .emulator:
            return "cpu"
        case .graphics:
            return "paintbrush"
        case .shaders:
            return "camera.filters"
        case .texturePacks:
            return "photo.stack"
        case .framePacing:
            return "speedometer"
        case .audio:
            return "speaker.wave.2"
        case .network:
            return "network"
        case .memoryCards:
            return "memorychip"
        case .storage:
            return "internaldrive"
        case .settingsPresets:
            return "slider.horizontal.3"
        case .retroAchievements:
            return "trophy"
        case .customSkin:
            return "paintpalette"
        case .overlay:
            return "text.below.photo"
        case .gameController:
            return "gamecontroller"
        case .controllerMacros:
            return "command"
        case .localMultiplayer:
            return "person.3"
        case .virtualPad:
            return "hand.draw"
        case .help:
            return "questionmark.circle"
        case .licenses:
            return "doc.text"
        case .about:
            return "info.circle"
        case .skinBrowser:
            return "square.grid.2x2"
        }
    }
}
