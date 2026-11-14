// QuickMenuDestination.swift — Pause menu destinations and fold memory
// SPDX-License-Identifier: GPL-3.0+

enum QuickMenuDestination: Equatable {
    case perGame
    case gameController
    case controllerSkin
    case speed
    case shaders
    case saveStates
    case cheats
    case retroAchievements
    case padLayout
    case resetROM
    case changeDisc
}

struct QuickMenuFoldMemory: Equatable {
    var scope: String
    var large: Bool
    var pose = ""
    var folded = false
}
