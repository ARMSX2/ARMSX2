// ChangeDiscPanel.swift — Pause menu Change Disc panel
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Pause → Change Disc, built from the pause menu's own shell, header and cards. Insert
/// and Restart sit side by side when there is room, so each disc's two actions share a line.
struct ChangeDiscPanel: View {
    @Environment(\.menuControllerInputRouter) private var controllerInput

    let settings: SettingsStore
    let variant: PauseLayoutVariant
    let discs: [String]
    let driveDisc: String?
    let onEject: () -> Void
    let onInsert: (String) -> Void
    let onRestart: (String) -> Void
    let onClose: () -> Void

    private static let close = "runtime.disc.close"
    private static let eject = "runtime.disc.eject"
    private static func insert(_ index: Int) -> String { "runtime.disc.insert.\(index)" }
    private static func restart(_ index: Int) -> String { "runtime.disc.restart.\(index)" }

    private func isInDrive(_ disc: String) -> Bool {
        (disc as NSString).lastPathComponent == driveDisc
    }

    // Inserting the disc that is already in the drive does nothing, so its row is left out.
    private var insertIDs: [String] {
        discs.indices.filter { !isInDrive(discs[$0]) }.map(Self.insert)
    }

    private var restartIDs: [String] { discs.indices.map(Self.restart) }

    private var driveTitle: String { driveDisc ?? settings.localized("No disc") }

    var body: some View {
        GeometryReader { geo in
            let landscape = variant == .phoneLandscape
            let twoColumns = !discs.isEmpty
                && variant.supportsTwoColumns(width: geo.size.width, height: geo.size.height)
            let rows = insertIDs + restartIDs
            let order = landscape ? [Self.eject, Self.close] + rows : rows + [Self.eject, Self.close]
            let scopeKey = "runtime.change-disc.\(landscape ? "landscape" : "portrait").\(twoColumns ? 2 : 1)"
            Group {
                if landscape {
                    landscapeBody(width: geo.size.width, twoColumns: twoColumns)
                } else {
                    portraitBody(twoColumns: twoColumns)
                }
            }
            .controllerAccessibilityTargetOrder(order)
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                scopeKey: scopeKey,
                priority: 320,
                orbStyle: .liquidGlass,
                onBack: {
                    onClose()
                    return true
                },
                directionalLinks: directionalLinks(landscape: landscape, twoColumns: twoColumns),
                prioritizesDirectionalLinks: true,
                usesExplicitTargetGeometryOnly: true,
                focusScrollBehavior: .maintainWithinViewport,
                preferredInitialFocusLabel: rows.first ?? Self.close,
                declaredTargetOrder: order
            )
            .task(id: scopeKey) {
                // As in the pause menu, registering is passive, so ask to enter once the rows mount.
                await Task.yield()
                guard !Task.isCancelled else { return }
                _ = controllerInput?.requestNavigationSessionEntry(
                    preferLast: false,
                    matchingScopePrefix: "runtime.change-disc."
                )
            }
        }
        .environment(\.controllerTextAppearance, settings.appearance.controllerQuickMenuTextAppearance)
        .preferredColorScheme(.dark)
        .quickMenuLiquidGlassConfiguration()
    }

    /// Up and Down stay in a column and reach Eject or Back at its end. Left and Right pair a
    /// disc's Insert and Restart rows. One column is a single chain, as in the pause menu.
    private func directionalLinks(landscape: Bool, twoColumns: Bool)
        -> [ControllerAccessibilityDirectionalLink] {
        let eject = Self.eject
        let back = Self.close
        var links: [ControllerAccessibilityDirectionalLink] = [
            .init(fromLabel: eject, direction: .right, toLabel: back),
            .init(fromLabel: back, direction: .left, toLabel: eject),
        ]
        func chain(_ labels: [String]) {
            for (upper, lower) in zip(labels, labels.dropFirst()) {
                links.append(.init(fromLabel: upper, direction: .down, toLabel: lower))
                links.append(.init(fromLabel: lower, direction: .up, toLabel: upper))
            }
        }
        if twoColumns {
            chain(landscape ? [eject] + insertIDs : insertIDs + [eject])
            chain(landscape ? [back] + restartIDs : restartIDs + [back])
            for index in discs.indices where !isInDrive(discs[index]) {
                links.append(.init(fromLabel: Self.insert(index), direction: .right, toLabel: Self.restart(index)))
                links.append(.init(fromLabel: Self.restart(index), direction: .left, toLabel: Self.insert(index)))
            }
        } else {
            let rows = insertIDs + restartIDs
            chain(landscape ? [back] + rows : rows + [back])
            if let edge = landscape ? rows.first : rows.last {
                links.append(.init(fromLabel: eject, direction: landscape ? .down : .up, toLabel: edge))
            }
        }
        // Unlinked directions stay put; geometry sent Left from Eject into the Restart card.
        let linked = Set(links.map { "\($0.fromLabel) \($0.direction)" })
        for label in [eject, back] + insertIDs + restartIDs {
            for direction in [MenuControllerCommand.up, .down, .left, .right]
            where !linked.contains("\(label) \(direction)") {
                links.append(.init(
                    fromLabel: label,
                    direction: direction,
                    toLabel: ControllerAccessibilityDirectionalLink.navigationBoundary
                ))
            }
        }
        return links
    }

    private func landscapeBody(width: CGFloat, twoColumns: Bool) -> some View {
        OverlayPanelScaffold {
            VStack(spacing: 0) {
                LandscapeCommandBar(
                    settings: settings,
                    gameTitle: driveTitle,
                    stopControllerNavigationID: Self.eject,
                    resumeControllerNavigationID: Self.close,
                    onStop: onEject,
                    onResume: onClose,
                    iconOnly: width < 380,
                    systemImage: "opticaldisc",
                    title: settings.localized("Change Disc"),
                    ejects: true,
                    resumeTitle: settings.localized("Back"),
                    resumeImage: "chevron.left"
                )
                scrollingCards(twoColumns: twoColumns)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .environment(\.overlayCompact, true)
    }

    private func portraitBody(twoColumns: Bool) -> some View {
        OverlayPanelScaffold {
            VStack(spacing: 0) {
                OverlayHeader(
                    systemImage: "opticaldisc",
                    title: settings.localized("Change Disc"),
                    subtitle: driveTitle,
                    compact: variant != .phonePortrait
                )
                scrollingCards(twoColumns: twoColumns)
                QuickMenuFooter(
                    settings: settings,
                    compact: variant != .phonePortrait,
                    stopControllerNavigationID: Self.eject,
                    resumeControllerNavigationID: Self.close,
                    onStop: onEject,
                    onResume: onClose,
                    ejects: true,
                    resumeTitle: settings.localized("Back"),
                    resumeImage: "chevron.left"
                )
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
    }

    private func scrollingCards(twoColumns: Bool) -> some View {
        ScrollView {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 320,
                pointsPerSecond: 680
            )
            .frame(height: 0)
            Group {
                if discs.isEmpty {
                    OverlaySectionCard {
                        Text(settings.localized("No disc images found"))
                            .foregroundStyle(
                                settings.appearance.controllerQuickMenuTextAppearance.secondaryColor
                                    ?? OverlayTheme.textSecondary
                            )
                            .frame(minHeight: 44)
                    }
                } else if twoColumns {
                    HStack(alignment: .top, spacing: 12) {
                        insertCard
                        restartCard
                    }
                } else {
                    VStack(spacing: 14) {
                        insertCard
                        restartCard
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, variant == .phoneLandscape ? 8 : 10)
        }
    }

    private var insertCard: some View {
        OverlaySectionCard(title: settings.localized("Insert Disc (No Reboot)")) {
            ForEach(Array(discs.enumerated()), id: \.offset) { index, disc in
                let inDrive = isInDrive(disc)
                OverlayActionRow(
                    controllerNavigationID: Self.insert(index),
                    label: GameListView.cleanGameFileName(disc),
                    systemImage: "opticaldisc",
                    trailingValue: inDrive ? settings.localized("In drive") : nil,
                    keepsTrailingValue: true
                ) {
                    onInsert(disc)
                }
                .disabled(inDrive)
            }
        }
    }

    private var restartCard: some View {
        OverlaySectionCard(title: settings.localized("Restart With Disc")) {
            ForEach(Array(discs.enumerated()), id: \.offset) { index, disc in
                OverlayActionRow(
                    controllerNavigationID: Self.restart(index),
                    label: GameListView.cleanGameFileName(disc),
                    systemImage: "arrow.clockwise"
                ) {
                    onRestart(disc)
                }
            }
        }
    }
}
