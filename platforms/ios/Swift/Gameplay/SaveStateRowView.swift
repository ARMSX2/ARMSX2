// SaveStateRowView.swift — save state row view
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

enum SaveStateRowLayout {
    /// iPhone landscape: one line per slot.
    case line
    /// iPad landscape: one line, wider buttons.
    case wideLine
    /// iPhone portrait and the largest text sizes: the buttons get their own line.
    case twoLine
    /// iPad portrait: date and play time on lines of their own.
    case stacked

    var thumbnail: CGSize {
        switch self {
        case .line: CGSize(width: 52, height: 39)
        case .wideLine: CGSize(width: 56, height: 42)
        case .twoLine: CGSize(width: 64, height: 48)
        case .stacked: CGSize(width: 72, height: 54)
        }
    }

    var buttonWidth: CGFloat? {
        switch self {
        case .line: 80
        case .wideLine: 88
        case .twoLine: nil
        case .stacked: 76
        }
    }

    var rowHeight: CGFloat {
        switch self {
        case .line: 50
        case .wideLine: 48
        case .twoLine: 0
        case .stacked: 72
        }
    }
}

struct SaveStateRowView: View {
    let row: SaveStateSlot
    let layout: SaveStateRowLayout
    let settings: SettingsStore
    let isLatest: Bool
    let isBusy: Bool
    let hardcore: Bool
    let primaryID: String?
    let quickSaveLine: ControllerHintLine?
    let onSave: () -> Void
    let onLoad: () -> Void
    let onMore: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    private var title: String { row.title(settings) }

    var body: some View {
        if layout == .twoLine {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    number
                    thumbnail
                    info
                }
                HStack(spacing: 12) {
                    buttons
                }
                .padding(.leading, 26)
            }
            .padding(.top, 8)
            .padding(.bottom, 10)
        } else {
            HStack(spacing: 12) {
                number
                thumbnail
                info
                buttons
            }
            .frame(minHeight: layout.rowHeight)
        }
    }

    private var number: some View {
        Group {
            if let symbol = row.symbol {
                Image(systemName: symbol)
            } else {
                Text("\(row.slot)")
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
            }
        }
        .font(.footnote.weight(.semibold))
        .foregroundStyle(OverlayTheme.textSecondary)
        .frame(width: 18)
        .accessibilityHidden(true)
    }

    private var thumbnail: some View {
        let size = layout.thumbnail
        return ZStack(alignment: .bottomLeading) {
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(row.occupied ? Color.black : Color.white.opacity(0.03))
            if let data = row.file.preview, let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .frame(width: size.width, height: size.height)
            }
        }
        .frame(width: size.width, height: size.height)
        .clipShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(
                    Color.white.opacity(row.occupied ? 0.1 : 0.22),
                    style: StrokeStyle(lineWidth: row.occupied ? 1 : 1.5, dash: row.occupied ? [] : [4, 3])
                )
        }
        .accessibilityHidden(true)
    }

    private var info: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(title)
                    .font(layout == .stacked ? .body.weight(.semibold) : .subheadline.weight(.semibold))
                    .foregroundStyle(row.occupied ? OverlayTheme.textPrimary : OverlayTheme.textSecondary)
                    .lineLimit(1)
                if row.isLocked {
                    Image(systemName: "lock.fill")
                        .font(.caption2)
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .accessibilityLabel(settings.localized("Locked"))
                }
            }
            if layout == .stacked || layout == .twoLine {
                savedLine
                playedLine
            } else {
                HStack(spacing: 10) {
                    savedLine
                    playedLine
                }
            }
            if let chip {
                Text(chip)
                    .font(.caption2)
                    .foregroundStyle(OverlayTheme.textSecondary)
            }
            if row.kind == .quick, let quickSaveLine {
                quickSaveLine
                    .font(.caption2)
                    .foregroundStyle(OverlayTheme.textSecondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder
    private var savedLine: some View {
        if row.occupied, let date = row.modifiedDate {
            let savedAt = SaveStateFormat.savedAt(date, settings: settings)
            if isLatest {
                // Inside the meta style, so the accent wins over its grey.
                Label(settings.localized("Latest") + " · " + savedAt, systemImage: "star.fill")
                    .foregroundStyle(accentColour)
                    .saveStateMeta(layout)
            } else {
                Label(savedAt, systemImage: "clock")
                    .saveStateMeta(layout)
            }
        } else {
            Text(settings.localized("Empty"))
                .saveStateMeta(layout)
        }
    }

    @ViewBuilder
    private var playedLine: some View {
        if row.occupied, let seconds = row.metadata?.playedSeconds {
            Label(SaveStateFormat.played(seconds, settings: settings), systemImage: "hourglass")
                .saveStateMeta(layout)
        }
    }

    private var chip: String? {
        switch row.kind {
        case .auto:
            !settings.autoSaveEnabled ? settings.localized("Off") : String(
                format: settings.localized(settings.autoSaveOnLeave ? "Every %d min and when you exit" : "Every %d min"),
                settings.autoSaveIntervalMinutes
            )
        case .quick, .manual, .older: nil
        }
    }

    @ViewBuilder
    private var buttons: some View {
        let saveID = SaveStatesPanel.saveID(row.slot)
        let loadID = SaveStatesPanel.loadID(row.slot)
        if row.hasSave {
            SaveStateButton(
                id: saveID,
                title: settings.localized("Save"),
                systemImage: row.isLocked ? "lock.fill" : "square.and.arrow.down",
                accessibilityLabel: String(
                    format: settings.localized(
                        row.isLocked ? "Save to %@, locked. Unlock it from More."
                            : row.occupied ? "Save over %@" : "Save to %@"
                    ),
                    title
                ),
                tonal: true,
                accentIcon: !row.occupied,
                isPrimary: primaryID == saveID,
                isBusy: isBusy,
                width: layout.buttonWidth,
                action: onSave
            )
            .disabled(row.isLocked)
        } else {
            placeholder
        }
        if row.occupied {
            SaveStateButton(
                id: loadID,
                title: settings.localized("Load"),
                systemImage: "arrow.counterclockwise",
                accessibilityLabel: String(
                    format: settings.localized(hardcore ? "Load %@. Loading is off in Hardcore mode." : "Load %@"),
                    title
                ),
                tonal: false,
                accentIcon: true,
                isPrimary: false,
                isBusy: false,
                width: layout.buttonWidth,
                action: onLoad
            )
            .disabled(hardcore)
        } else {
            placeholder
        }
        if row.hasMore {
            Button(action: onMore) {
                Image(systemName: "ellipsis")
                    .font(.body.weight(.semibold))
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .frame(width: 44, height: 44)
                    .background(Color.white.opacity(0.10), in: Circle())
                    .overlay(Circle().stroke(Color.white.opacity(0.14), lineWidth: 1))
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(String(format: settings.localized("More for %@"), title))
            .anchorPreference(key: SaveStateMoreAnchorKey.self, value: .bounds) { [row.slot: $0] }
            .controllerAccessibilityActionTarget(
                id: SaveStatesPanel.moreID(row.slot),
                label: SaveStatesPanel.moreID(row.slot),
                focusedNeonCornerRadius: 22,
                action: onMore
            )
        } else {
            Color.clear.frame(width: 44, height: 44)
        }
    }

    @ViewBuilder
    private var placeholder: some View {
        if let width = layout.buttonWidth {
            Color.clear.frame(width: width, height: 44)
        } else {
            Color.clear.frame(maxWidth: .infinity, maxHeight: 44)
        }
    }
}

private extension View {
    func saveStateMeta(_ layout: SaveStateRowLayout) -> some View {
        font(layout == .stacked ? .footnote : .caption)
            .foregroundStyle(OverlayTheme.textSecondary)
            .lineLimit(layout == .line || layout == .wideLine ? 1 : 2)
    }
}

/// Tonal until the pad focuses it; only the focused button, or on touch the one the pad would
/// start on, is filled with the accent.
private struct SaveStateButton: View {
    let id: String
    let title: String
    let systemImage: String
    let accessibilityLabel: String
    let tonal: Bool
    let accentIcon: Bool
    let isPrimary: Bool
    let isBusy: Bool
    let width: CGFloat?
    let action: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        Button(action: action) {
            SaveStateButtonLabel(
                title: title,
                systemImage: systemImage,
                tonal: tonal,
                accentIcon: accentIcon,
                isPrimary: isPrimary,
                isBusy: isBusy,
                width: width
            )
        }
        .buttonStyle(.plain)
        .accessibilityLabel(accessibilityLabel)
        .controllerAccessibilityActionTarget(
            id: id,
            label: id,
            focusedColor: onAccent(accentColour),
            focusedNeonCornerRadius: 22,
            action: action
        )
    }
}

private struct SaveStateButtonLabel: View {
    let title: String
    let systemImage: String
    let tonal: Bool
    let accentIcon: Bool
    let isPrimary: Bool
    let isBusy: Bool
    let width: CGFloat?

    @Environment(\.controllerAccessibilityTargetFocused) private var focused
    @Environment(\.isEnabled) private var isEnabled
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize

    var body: some View {
        let filled = isEnabled && (focused || isPrimary)
        let foreground = filled
            ? onAccent(accentColour)
            : (isEnabled ? OverlayTheme.textPrimary : Color(red: 0.43, green: 0.46, blue: 0.52))
        HStack(spacing: 6) {
            if isBusy {
                ProgressView()
                    .controlSize(.small)
                    .tint(foreground)
            } else if !dynamicTypeSize.isAccessibilitySize {
                Image(systemName: systemImage)
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(filled || !isEnabled || !accentIcon ? foreground : accentColour)
            }
            Text(title)
                .lineLimit(1)
                .minimumScaleFactor(0.75)
        }
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(foreground)
        .padding(.horizontal, 12)
        .frame(minWidth: width ?? 44, maxWidth: width == nil ? .infinity : nil, minHeight: 44)
        .background {
            Capsule()
                .fill(
                    filled ? accentColour
                        : !isEnabled ? Color.white.opacity(0.05)
                        : tonal ? Color.white.opacity(0.10) : accentColour.opacity(0.2)
                )
        }
        .overlay {
            Capsule().stroke(Color.white.opacity(tonal && isEnabled && !filled ? 0.14 : 0), lineWidth: 1)
        }
        .contentShape(Capsule())
    }
}

/// Black or white, whichever reads better on the accent.
private func onAccent(_ accent: Color) -> Color {
    var red: CGFloat = 0, green: CGFloat = 0, blue: CGFloat = 0, alpha: CGFloat = 0
    UIColor(accent).getRed(&red, green: &green, blue: &blue, alpha: &alpha)
    func linear(_ value: CGFloat) -> CGFloat {
        value <= 0.03928 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
    }
    let luminance = 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue)
    return (luminance + 0.05) / 0.05 >= 1.05 / (luminance + 0.05) ? .black : .white
}

struct SaveStateMoreAnchorKey: PreferenceKey {
    static let defaultValue: [Int: Anchor<CGRect>] = [:]
    static func reduce(value: inout [Int: Anchor<CGRect>], nextValue: () -> [Int: Anchor<CGRect>]) {
        value.merge(nextValue()) { _, next in next }
    }
}
