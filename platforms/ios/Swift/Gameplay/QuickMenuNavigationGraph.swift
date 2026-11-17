// QuickMenuNavigationGraph.swift — The pause menu's controller graph
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// The pause menu's controller order and directional links for the rows the card shows.
struct QuickMenuNavigationGraph {
    let vmMenuAvailable: Bool
    let gameMenuAvailable: Bool
    let shaderChainAvailable: Bool
    let hidesPadLayoutEditorForController: Bool
    let layoutDirection: LayoutDirection
    let twoColumns: Bool

    enum ControllerTargetID {
        static let stop = "quick-menu.stop"
        static let resume = "quick-menu.resume"
        static let osd = "quick-menu.osd"
        static let virtualPad = "quick-menu.virtual-pad"
        static let fullScreen = "quick-menu.full-screen"
        static let hideMenuButton = "quick-menu.hide-menu-button"
        static let speed = "quick-menu.speed"
        static let perGame = "quick-menu.per-game"
        static let gameController = "quick-menu.game-controller"
        static let controllerSkin = "quick-menu.controller-skin"
        static let editPadLayout = "quick-menu.edit-pad-layout"
        static let saveStates = "quick-menu.save-states"
        static let shaders = "quick-menu.shaders"
        static let reportProblem = "quick-menu.report-problem"
        static let changeDisc = "quick-menu.change-disc"
        static let retroAchievements = "quick-menu.retro-achievements"
        static let cheats = "quick-menu.cheats"
        static let resetROM = "quick-menu.reset-rom"
        static let clearCache = "quick-menu.clear-cache"
        static let backToMenu = "quick-menu.back-to-menu"
    }

    /// The leading leaf of the book-pose spread, top to bottom.
    var spreadLeadingRows: [String] {
        var rows = [
            ControllerTargetID.osd,
            ControllerTargetID.virtualPad,
            ControllerTargetID.fullScreen,
            ControllerTargetID.hideMenuButton,
        ]
        if vmMenuAvailable { rows.append(ControllerTargetID.speed) }
        if gameMenuAvailable { rows.append(ControllerTargetID.perGame) }
        rows.append(ControllerTargetID.controllerSkin)
        if !hidesPadLayoutEditorForController { rows.append(ControllerTargetID.editPadLayout) }
        return rows
    }

    /// Each leaf is its own column; Left and Right cross the fold to the row beside, and
    /// vertical movement enters the footer through Resume. Mirrored in right-to-left.
    func quickMenuSpreadDirectionalLinks() -> [ControllerAccessibilityDirectionalLink] {
        let leading = spreadLeadingRows
        let trailing = Array(quickMenuPortraitControllerTargetOrder().dropFirst(leading.count).dropLast(2))
        let stop = ControllerTargetID.stop
        let resume = ControllerTargetID.resume
        let boundary = ControllerAccessibilityDirectionalLink.navigationBoundary
        let towardTrailing: MenuControllerCommand = layoutDirection == .rightToLeft ? .left : .right
        let towardLeading: MenuControllerCommand = layoutDirection == .rightToLeft ? .right : .left

        var links: [ControllerAccessibilityDirectionalLink] = []
        for column in [leading, trailing] {
            for (upper, lower) in zip(column, column.dropFirst()) {
                links.append(.init(fromLabel: upper, direction: .down, toLabel: lower))
                links.append(.init(fromLabel: lower, direction: .up, toLabel: upper))
            }
        }
        links += leading.map { .init(fromLabel: $0, direction: towardLeading, toLabel: boundary) }
        links += trailing.map { .init(fromLabel: $0, direction: towardTrailing, toLabel: boundary) }
        // Stop sits at the footer's near end, where a plain spatial lookup can rank it above the other leaf.
        links += leading.map { .init(fromLabel: $0, direction: towardTrailing, toLabel: ControllerAccessibilityDirectionalLink.nearest(of: trailing)) }
        links += trailing.map { .init(fromLabel: $0, direction: towardLeading, toLabel: ControllerAccessibilityDirectionalLink.nearest(of: leading)) }
        if let first = trailing.first {
            links.append(.init(fromLabel: first, direction: .up, toLabel: boundary))
        }
        if let last = leading.last {
            links.append(.init(fromLabel: last, direction: .down, toLabel: resume))
            links.append(.init(fromLabel: stop, direction: towardLeading, toLabel: last))
        }
        links += [
            .init(fromLabel: ControllerTargetID.backToMenu, direction: .down, toLabel: resume),
            .init(fromLabel: stop, direction: .up, toLabel: ControllerTargetID.backToMenu),
            .init(fromLabel: stop, direction: .down, toLabel: boundary),
            .init(fromLabel: stop, direction: towardTrailing, toLabel: resume),
            .init(fromLabel: resume, direction: .up, toLabel: ControllerTargetID.backToMenu),
            .init(fromLabel: resume, direction: towardLeading, toLabel: stop),
            .init(fromLabel: resume, direction: towardTrailing, toLabel: boundary),
        ]
        return links
    }

    /// Portrait keeps its scrolling rows and fixed footer in different
    /// coordinate spaces. Publish their semantic order instead of re-sorting
    /// them from frames that change while the scroll view is moving.
    func quickMenuPortraitControllerTargetOrder() -> [String] {
        var order = spreadLeadingRows
        if gameMenuAvailable {
            order.append(ControllerTargetID.gameController)
        }
        if gameMenuAvailable || vmMenuAvailable {
            order.append(ControllerTargetID.saveStates)
        }
        if vmMenuAvailable { order.append(ControllerTargetID.changeDisc) }
        if gameMenuAvailable {
            order.append(contentsOf: [
                ControllerTargetID.retroAchievements,
                ControllerTargetID.cheats,
            ])
        }
        if shaderChainAvailable { order.append(ControllerTargetID.shaders) }
        order.append(ControllerTargetID.reportProblem)
        if vmMenuAvailable { order.append(ControllerTargetID.resetROM) }
        if gameMenuAvailable { order.append(ControllerTargetID.clearCache) }
        order.append(contentsOf: [
            ControllerTargetID.backToMenu,
            ControllerTargetID.stop,
            ControllerTargetID.resume,
        ])
        return order
    }

    /// Stop is a secondary action beside Resume, not another row between the
    /// bottom card and the primary footer action. Vertical movement enters and
    /// leaves the footer through Resume; horizontal movement selects Stop.
    func quickMenuPortraitDirectionalLinks()
        -> [ControllerAccessibilityDirectionalLink] {
        let towardStop: MenuControllerCommand = layoutDirection == .rightToLeft ? .right : .left
        let towardResume: MenuControllerCommand = layoutDirection == .rightToLeft ? .left : .right
        return [
            .init(
                fromLabel: ControllerTargetID.backToMenu,
                direction: .down,
                toLabel: ControllerTargetID.resume
            ),
            .init(
                fromLabel: ControllerTargetID.resume,
                direction: .up,
                toLabel: ControllerTargetID.backToMenu
            ),
            .init(
                fromLabel: ControllerTargetID.stop,
                direction: .up,
                toLabel: ControllerTargetID.backToMenu
            ),
            .init(
                fromLabel: ControllerTargetID.resume,
                direction: towardStop,
                toLabel: ControllerTargetID.stop
            ),
            .init(
                fromLabel: ControllerTargetID.stop,
                direction: towardResume,
                toLabel: ControllerTargetID.resume
            ),
        ]
    }

    /// Landscape is a command deck, not a free-form proximity surface. Build
    /// its graph from the rows that actually exist so repeated input stays in
    /// the current column and Left/Right always lands on the requested pair.
    /// Missing conditional actions are omitted and their neighbors connect
    /// directly, so the graph remains valid with or without an active VM/game.
    func quickMenuLandscapeDirectionalLinks()
        -> [ControllerAccessibilityDirectionalLink] {
        let stop = ControllerTargetID.stop
        let resume = ControllerTargetID.resume
        let osd = ControllerTargetID.osd
        let virtualPad = ControllerTargetID.virtualPad
        let fullScreen = ControllerTargetID.fullScreen
        let hideMenu = ControllerTargetID.hideMenuButton
        let speed = ControllerTargetID.speed
        let perGame = ControllerTargetID.perGame
        let gameController = ControllerTargetID.gameController
        let controllerSkin = ControllerTargetID.controllerSkin
        let editPad = ControllerTargetID.editPadLayout
        let saveStates = ControllerTargetID.saveStates
        let shaders = ControllerTargetID.shaders
        let reportProblem = ControllerTargetID.reportProblem
        let changeDisc = ControllerTargetID.changeDisc
        let achievements = ControllerTargetID.retroAchievements
        let cheats = ControllerTargetID.cheats
        let resetROM = ControllerTargetID.resetROM
        let clearCache = ControllerTargetID.clearCache
        let backToMenu = ControllerTargetID.backToMenu

        var contentOrder = [osd, virtualPad, fullScreen, hideMenu]
        if vmMenuAvailable { contentOrder.append(speed) }
        if gameMenuAvailable { contentOrder.append(perGame) }
        contentOrder.append(controllerSkin)
        if !hidesPadLayoutEditorForController { contentOrder.append(editPad) }
        if gameMenuAvailable { contentOrder.append(gameController) }
        if gameMenuAvailable || vmMenuAvailable {
            contentOrder.append(saveStates)
        }
        if vmMenuAvailable { contentOrder.append(changeDisc) }
        if gameMenuAvailable {
            contentOrder.append(contentsOf: [achievements, cheats])
        }
        if shaderChainAvailable { contentOrder.append(shaders) }
        contentOrder.append(reportProblem)
        if vmMenuAvailable { contentOrder.append(resetROM) }
        if gameMenuAvailable { contentOrder.append(clearCache) }
        contentOrder.append(backToMenu)

        // The iPhone SE landscape layout renders one visual column. Reusing the
        // two-column graph there made Down from Stop enter the left sequence,
        // while Down from Resume jumped into the former right sequence.
        if !twoColumns {
            var links: [ControllerAccessibilityDirectionalLink] = [
                .init(fromLabel: stop, direction: .right, toLabel: resume),
                .init(fromLabel: resume, direction: .left, toLabel: stop),
                .init(fromLabel: stop, direction: .down, toLabel: osd),
                .init(fromLabel: resume, direction: .down, toLabel: osd),
                .init(fromLabel: osd, direction: .up, toLabel: resume),
            ]
            for (upper, lower) in zip(contentOrder, contentOrder.dropFirst()) {
                links.append(
                    .init(fromLabel: upper, direction: .down, toLabel: lower)
                )
                links.append(
                    .init(fromLabel: lower, direction: .up, toLabel: upper)
                )
            }
            return links
        }

        var leftColumn = [stop, osd, virtualPad, fullScreen, hideMenu]
        if vmMenuAvailable { leftColumn.append(speed) }
        if gameMenuAvailable { leftColumn.append(perGame) }
        leftColumn.append(controllerSkin)
        if !hidesPadLayoutEditorForController { leftColumn.append(editPad) }

        var rightColumn = [resume]
        if gameMenuAvailable { rightColumn.append(gameController) }
        if gameMenuAvailable || vmMenuAvailable {
            rightColumn.append(saveStates)
        }
        if vmMenuAvailable { rightColumn.append(changeDisc) }
        if gameMenuAvailable {
            rightColumn.append(contentsOf: [achievements, cheats])
        }
        if shaderChainAvailable { rightColumn.append(shaders) }
        rightColumn.append(reportProblem)
        if vmMenuAvailable { rightColumn.append(resetROM) }
        if gameMenuAvailable { rightColumn.append(clearCache) }
        rightColumn.append(backToMenu)

        var links: [ControllerAccessibilityDirectionalLink] = []
        // Moving out of the cards always returns to the primary Resume action.
        // Stop remains reachable immediately to its left.
        links.append(
            ControllerAccessibilityDirectionalLink(
                fromLabel: osd,
                direction: .up,
                toLabel: resume
            )
        )
        func appendVerticalLinks(_ labels: [String]) {
            for (upper, lower) in zip(labels, labels.dropFirst()) {
                links.append(
                    ControllerAccessibilityDirectionalLink(
                        fromLabel: upper,
                        direction: .down,
                        toLabel: lower
                    )
                )
                links.append(
                    ControllerAccessibilityDirectionalLink(
                        fromLabel: lower,
                        direction: .up,
                        toLabel: upper
                    )
                )
            }
        }
        appendVerticalLinks(leftColumn)
        appendVerticalLinks(rightColumn)

        let available = Set(leftColumn + rightColumn)
        // Shaders sits in the right column ahead of Report, so everything below it moves down one row.
        let horizontalPairs = [
            (stop, resume),
            (osd, gameController),
            (virtualPad, saveStates),
            (fullScreen, changeDisc),
            (hideMenu, achievements),
            (speed, cheats),
        ] + (shaderChainAvailable ? [
            (perGame, shaders),
            (controllerSkin, reportProblem),
            (editPad, resetROM),
        ] : [
            (perGame, reportProblem),
            (controllerSkin, resetROM),
            (editPad, clearCache),
        ])
        for (left, right) in horizontalPairs
        where available.contains(left) && available.contains(right) {
            links.append(
                ControllerAccessibilityDirectionalLink(
                    fromLabel: left,
                    direction: .right,
                    toLabel: right
                )
            )
            links.append(
                ControllerAccessibilityDirectionalLink(
                    fromLabel: right,
                    direction: .left,
                    toLabel: left
                )
            )
        }
        return links
    }

    /// Fallback ordering uses the same mounted rows as the explicit landscape
    /// links. This keeps a briefly recycled row (notably Shaders) from being
    /// skipped while the scroll view is mounting.
    func quickMenuLandscapeControllerTargetOrder() -> [String] {
        var contentOrder = [
            ControllerTargetID.osd,
            ControllerTargetID.virtualPad,
            ControllerTargetID.fullScreen,
            ControllerTargetID.hideMenuButton,
        ]
        if vmMenuAvailable { contentOrder.append(ControllerTargetID.speed) }
        if gameMenuAvailable { contentOrder.append(ControllerTargetID.perGame) }
        contentOrder.append(ControllerTargetID.controllerSkin)
        if !hidesPadLayoutEditorForController {
            contentOrder.append(ControllerTargetID.editPadLayout)
        }
        if gameMenuAvailable {
            contentOrder.append(ControllerTargetID.gameController)
        }
        if gameMenuAvailable || vmMenuAvailable {
            contentOrder.append(ControllerTargetID.saveStates)
        }
        if vmMenuAvailable { contentOrder.append(ControllerTargetID.changeDisc) }
        if gameMenuAvailable {
            contentOrder.append(contentsOf: [
                ControllerTargetID.retroAchievements,
                ControllerTargetID.cheats,
            ])
        }
        if shaderChainAvailable {
            contentOrder.append(ControllerTargetID.shaders)
        }
        contentOrder.append(ControllerTargetID.reportProblem)
        if vmMenuAvailable { contentOrder.append(ControllerTargetID.resetROM) }
        if gameMenuAvailable { contentOrder.append(ControllerTargetID.clearCache) }
        contentOrder.append(ControllerTargetID.backToMenu)
        return [ControllerTargetID.stop, ControllerTargetID.resume]
            + contentOrder
    }
}
