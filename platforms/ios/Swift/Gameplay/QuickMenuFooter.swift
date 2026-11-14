// QuickMenuFooter.swift — Pause menu footer and landscape command bar
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct LandscapeCommandBar: View {
    let settings: SettingsStore
    let gameTitle: String?
    let stopControllerNavigationID: String
    /// No target means Circle is the pad's way back.
    let resumeControllerNavigationID: String?
    let onStop: () -> Void
    let onResume: () -> Void
    let iconOnly: Bool
    var systemImage = "pause.circle.fill"
    var title: String?
    var ejects = false
    var showsStopButton = true
    var resumeTitle: String?
    var resumeImage = "play.fill"
    var hint: ControllerHintLine?
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: systemImage)
                    .font(.system(size: 18))
                    .foregroundStyle(accentColour)
                Text(title ?? settings.localized("Paused"))
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(
                        textAppearance.normalColor ?? OverlayTheme.textPrimary
                    )
                    .layoutPriority(1)
                if let title = gameTitle, !title.isEmpty {
                    Text(title)
                        .font(.caption)
                        .foregroundStyle(
                            textAppearance.secondaryColor
                                ?? OverlayTheme.textSecondary
                        )
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .layoutPriority(-1)
                }
                Spacer(minLength: 8)
                if let hint {
                    hint
                        .font(.caption)
                        .foregroundStyle(textAppearance.secondaryColor ?? OverlayTheme.textSecondary)
                        .lineLimit(1)
                }
                if showsStopButton {
                    QuickMenuStopButton(
                        controllerNavigationID: stopControllerNavigationID,
                        accessibilityLabel: settings.localized(ejects ? "Eject Disc" : "Stop"),
                        compact: true,
                        ejects: ejects,
                        action: onStop
                    )
                }
                Button(action: onResume) {
                    if iconOnly {
                        Image(systemName: resumeImage)
                            .foregroundStyle(.white)
                    } else {
                        Label {
                            Text(resumeTitle ?? settings.localized("Resume"))
                        } icon: {
                            Image(systemName: resumeImage)
                                .foregroundStyle(.white)
                        }
                    }
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.regular)
                .quickMenuResumeTarget(resumeControllerNavigationID, action: onResume)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 6)
            OverlayTheme.separator
                .frame(height: 0.5)
        }
    }
}

struct QuickMenuFooter: View {
    let settings: SettingsStore
    let compact: Bool
    let stopControllerNavigationID: String
    let resumeControllerNavigationID: String?
    let onStop: () -> Void
    let onResume: () -> Void
    var ejects = false
    var showsStopButton = true
    var resumeTitle: String?
    var resumeImage = "play.fill"
    var hint: ControllerHintLine?
    var hintTiles: HintTiles?

    enum HintTiles {
        /// Tiles under the first column, Stop and Resume under the second.
        case halves
        /// Tiles, then Stop, then Resume, in one row.
        case row
        /// Only the tiles, on the leaf of iPhone Duo's opened card that has no buttons.
        case alone
        /// Only Stop and Resume, at least as tall as the tiles on the other leaf.
        case beside
    }

    @Environment(\.menuControllerInputRouter) private var controllerInput

    var body: some View {
        VStack(spacing: 0) {
            OverlayTheme.separator
                .frame(height: 0.5)
            Group {
                switch hintTiles {
                case .halves:
                    HStack(spacing: 12) {
                        tiles.frame(maxWidth: .infinity)
                        buttons.frame(maxWidth: .infinity)
                    }
                    .padding(.horizontal, 16)
                case .row:
                    HStack(spacing: compact ? 10 : 12) {
                        tiles.frame(width: 3 * 110 + 8)
                        buttons
                    }
                    .padding(.horizontal, 16)
                case .alone:
                    tiles.padding(.horizontal, 16)
                case .beside:
                    buttons
                        .frame(minHeight: GameplayControllerShortcutHelpOverlay.height)
                        .padding(.horizontal, 16)
                case nil:
                    buttons.padding(.horizontal, compact ? 18 : 20)
                }
            }
            .padding(.top, 8)
            .padding(.bottom, compact ? 10 : 14)
        }
    }

    private var tiles: some View {
        GameplayControllerShortcutHelpOverlay(settings: settings, controllerInput: controllerInput, inCard: true)
    }

    private var buttons: some View {
        HStack(spacing: compact ? 10 : 12) {
            if showsStopButton {
                QuickMenuStopButton(
                    controllerNavigationID: stopControllerNavigationID,
                    accessibilityLabel: settings.localized(ejects ? "Eject Disc" : "Stop"),
                    compact: compact,
                    ejects: ejects,
                    action: onStop
                )
            }
            if let hint {
                hint
                    .font(.footnote)
                    .foregroundStyle(OverlayTheme.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Button(action: onResume) {
                Label {
                    Text(resumeTitle ?? settings.localized("Resume"))
                } icon: {
                    Image(systemName: resumeImage)
                        .foregroundStyle(.white)
                }
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(compact ? .regular : .large)
            .quickMenuResumeTarget(resumeControllerNavigationID, action: onResume)
        }
    }
}

private extension View {
    @ViewBuilder
    func quickMenuResumeTarget(_ id: String?, action: @escaping () -> Void) -> some View {
        if let id {
            controllerAccessibilityActionTarget(id: id, label: id, focusedColor: .white, action: action)
        } else {
            self
        }
    }
}

private struct QuickMenuStopButton: View {
    let controllerNavigationID: String
    let accessibilityLabel: String
    let compact: Bool
    /// Change Disc puts Eject in this slot: neutral, since ejecting loses nothing.
    var ejects = false
    let action: () -> Void

    private var diameter: CGFloat { compact ? 36 : 46 }

    var body: some View {
        Button(action: action) {
            Image(systemName: ejects ? "eject.fill" : "stop.fill")
                .font(.system(size: compact ? 13 : 16, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: diameter, height: diameter)
                .background(ejects ? OverlayTheme.separator : Color.red, in: Circle())
        }
        .buttonStyle(.plain)
        .contentShape(Circle())
        .stopConfirmationSource()
        .accessibilityLabel(accessibilityLabel)
        .controllerAccessibilityActionTarget(
            id: controllerNavigationID,
            label: controllerNavigationID,
            focusedColor: .white,
            focusedNeonCornerRadius: diameter / 2,
            action: action
        )
    }
}
