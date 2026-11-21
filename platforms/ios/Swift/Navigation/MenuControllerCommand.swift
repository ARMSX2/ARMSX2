// MenuControllerCommand.swift — menu controller commands
// SPDX-License-Identifier: GPL-3.0+

enum MenuControllerCommand: Hashable, Sendable {
    case up
    case upRight
    case right
    case downRight
    case down
    case downLeft
    case left
    case upLeft
    case activate
    case back
    case toggleFavorite
    case showContextMenu
    case previousTab
    case nextTab
}

extension MenuControllerCommand {
    var isSpatialDirection: Bool {
        switch self {
        case .up, .upRight, .right, .downRight,
             .down, .downLeft, .left, .upLeft:
            true
        case .activate, .back, .toggleFavorite, .showContextMenu,
             .previousTab, .nextTab:
            false
        }
    }

    /// Cardinal components are used only by one-dimensional controls and
    /// scrolling. The shared 2D focus engine retains the complete diagonal.
    var horizontalComponent: MenuControllerCommand? {
        switch self {
        case .left, .upLeft, .downLeft: .left
        case .right, .upRight, .downRight: .right
        default: nil
        }
    }

    var verticalComponent: MenuControllerCommand? {
        switch self {
        case .up, .upLeft, .upRight: .up
        case .down, .downLeft, .downRight: .down
        default: nil
        }
    }
}
