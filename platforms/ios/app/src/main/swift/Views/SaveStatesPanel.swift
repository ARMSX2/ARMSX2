// SaveStatesPanel.swift — Pause → Save States, built from the pause card's own pieces.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Ten fixed rows with Save and Load on each. The pad moves by row and keeps its column.
struct SaveStatesPanel: View {
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize

    let settings: SettingsStore
    let variant: PauseLayoutVariant
    let landscape: Bool
    let gameTitle: String?
    let statusHandler: (String, Bool) -> Void
    let onLoaded: () -> Void
    let onClose: () -> Void

    @State private var files: [SaveStateFile] = []
    @State private var metadataRevision = 0
    @State private var busySlot: Int?
    @State private var pendingOverwrite: SaveStateSlot?
    @State private var hardcore = false

    static let scopeKey = "runtime.save-states"
    static func saveID(_ slot: Int) -> String { "save-state.slot.\(slot).save" }
    static func loadID(_ slot: Int) -> String { "save-state.slot.\(slot).load" }

    private var rows: [SaveStateSlot] {
        _ = metadataRevision
        return SaveStateSlot.rows(from: files) { SaveStateMetadataStore.shared.metadata(for: $0) }
    }

    private var layout: SaveStateRowLayout {
        if dynamicTypeSize.isAccessibilitySize { return .twoLine }
        switch variant {
        case .phoneLandscape: return .line
        case .phonePortrait: return .twoLine
        case .ipadTwoColumn: return landscape ? .wideLine : .stacked
        }
    }

    var body: some View {
        let graph = SaveStateGraph(rows: targetRows)
        Group {
            if layout == .line || layout == .wideLine {
                OverlayPanelScaffold {
                    VStack(spacing: 0) {
                        LandscapeCommandBar(
                            settings: settings,
                            gameTitle: gameTitle,
                            stopControllerNavigationID: "",
                            resumeControllerNavigationID: nil,
                            onStop: {},
                            onResume: onClose,
                            iconOnly: false,
                            systemImage: "square.stack.3d.up",
                            title: settings.localized("Save States"),
                            showsStopButton: false,
                            resumeTitle: settings.localized("Back"),
                            resumeImage: "chevron.left"
                        )
                        content
                    }
                }
                .environment(\.overlayCompact, true)
            } else {
                OverlayPanelScaffold {
                    VStack(spacing: 0) {
                        OverlayHeader(
                            systemImage: "square.stack.3d.up",
                            title: settings.localized("Save States"),
                            subtitle: gameTitle,
                            compact: variant != .phonePortrait
                        )
                        content
                        QuickMenuFooter(
                            settings: settings,
                            compact: variant != .phonePortrait,
                            stopControllerNavigationID: "",
                            resumeControllerNavigationID: nil,
                            onStop: {},
                            onResume: onClose,
                            showsStopButton: false,
                            resumeTitle: settings.localized("Back"),
                            resumeImage: "chevron.left"
                        )
                    }
                }
            }
        }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: Self.scopeKey,
            priority: 320,
            orbStyle: .liquidGlass,
            onBack: {
                onClose()
                return true
            },
            boundaryRules: graph.freshPressRules,
            directionalLinks: graph.links,
            prioritizesDirectionalLinks: true,
            confinesHorizontalFocusMovement: true,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            // Room for the card header, and in two-line rows for the title above the buttons.
            focusTopAlignmentMargin: layout == .twoLine ? 96 : 40,
            focusBottomAlignmentMargin: 10,
            preferredInitialFocusLabel: initialFocusID,
            declaredTargetOrder: graph.order
        )
        .task(id: files.isEmpty) {
            // Registering is passive, as in the pause menu, so ask to enter once the rows exist.
            guard !files.isEmpty else { return }
            await Task.yield()
            _ = controllerInput?.requestNavigationSessionEntry(
                preferLast: false,
                matchingScopePrefix: Self.scopeKey
            )
        }
        .task { await refresh() }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2iOSRuntimeMenuStateChanged"))) { _ in
            Task { await refresh() }
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
            Task { await refresh() }
        }
        .controllerPrompt(
            "\(settings.localized("Overwrite Slot")) \(pendingOverwrite?.number ?? 0)?",
            isPresented: Binding(
                get: { pendingOverwrite != nil },
                set: { if !$0 { pendingOverwrite = nil } }
            ),
            actions: [
                .cancel,
                .init(title: settings.localized("Overwrite"), isDestructive: true) {
                    if let pendingOverwrite { performSave(pendingOverwrite) }
                },
            ]
        )
        .environment(\.controllerTextAppearance, settings.controllerQuickMenuTextAppearance)
        .preferredColorScheme(.dark)
        .quickMenuLiquidGlassConfiguration()
    }

    @ViewBuilder
    private var content: some View {
        if hardcore {
            Label(
                settings.localized("Hardcore mode: loading save states is off. Saving still works."),
                systemImage: "trophy"
            )
            .font(.footnote.weight(.semibold))
            .foregroundStyle(OverlayTheme.warm)
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(OverlayTheme.warm.opacity(0.14), in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(OverlayTheme.warm.opacity(0.35)))
            .padding(.horizontal, 14)
            .padding(.top, 8)
        }
        if files.isEmpty {
            VStack(spacing: 6) {
                Text(settings.localized("Save states are not ready yet."))
                    .font(.headline)
                Text(settings.localized("Wait until the game has fully identified, then try again."))
                    .font(.caption)
                    .foregroundStyle(OverlayTheme.textSecondary)
            }
            .multilineTextAlignment(.center)
            .foregroundStyle(OverlayTheme.textPrimary)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(24)
        } else {
            list
        }
    }

    private var list: some View {
        let current = rows
        let latest = SaveStateSlot.latest(in: current)
        return ScrollView {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 320,
                pointsPerSecond: 680
            )
            .frame(height: 0)
            VStack(spacing: 12) {
                card(
                    title: settings.localized("Your saves"),
                    detail: settings.localized("Slots 1–8"),
                    rows: current.filter { $0.kind == .manual || $0.kind == .older },
                    latest: latest
                )
                card(
                    title: settings.localized("Auto and quick"),
                    detail: settings.localized("Slots 9 and 10"),
                    rows: current.filter { $0.kind == .auto || $0.kind == .quick },
                    latest: latest
                )
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
        }
        .mask {
            VStack(spacing: 0) {
                LinearGradient(colors: [.clear, .black], startPoint: .top, endPoint: .bottom)
                    .frame(height: 20)
                Color.black
                LinearGradient(colors: [.black, .clear], startPoint: .top, endPoint: .bottom)
                    .frame(height: 18)
            }
        }
    }

    private func card(title: String, detail: String, rows: [SaveStateSlot], latest: Int?) -> some View {
        OverlaySectionCard {
            HStack {
                Text(title)
                    .textCase(.uppercase)
                Spacer()
                Text(detail)
            }
            .font(.caption2)
            .foregroundStyle(OverlayTheme.textSecondary)
            .padding(.horizontal, 4)
            ForEach(rows) { row in
                SaveStateRowView(
                    row: row,
                    layout: layout,
                    settings: settings,
                    isLatest: row.slot == latest,
                    isBusy: busySlot == row.slot,
                    hardcore: hardcore,
                    primaryID: controllerInput?.hasConnectedController == true ? nil : initialFocusID,
                    quickSaveLine: quickSaveLine,
                    onSave: { save(row) },
                    onLoad: { load(row) }
                )
            }
        }
    }

    /// The live macro bindings, shown only to controller players.
    private var quickSaveLine: String? {
        guard controllerInput?.hasConnectedController == true else { return nil }
        let save = settings.controllerMacroSaveGameState.title
        if hardcore {
            return String(format: settings.localized("%@ saves · loading is off in Hardcore"), save)
        }
        return String(
            format: settings.localized("%1$@ saves · %2$@ loads"),
            save,
            settings.controllerMacroLoadGameState.title
        )
    }

    private var targetRows: [[SaveStateGraph.Target]] {
        rows.map { row in
            var targets: [SaveStateGraph.Target] = []
            if row.hasSave { targets.append(.init(id: Self.saveID(row.slot), column: 0)) }
            if row.occupied, !hardcore { targets.append(.init(id: Self.loadID(row.slot), column: 1)) }
            return targets
        }
        .filter { !$0.isEmpty }
    }

    /// The first empty manual slot, else Quick Save.
    private var initialFocusID: String? {
        let current = rows
        if let empty = current.first(where: { $0.kind == .manual && !$0.occupied }) {
            return Self.saveID(empty.slot)
        }
        return current.first(where: { $0.kind == .quick }).map { Self.saveID($0.slot) }
    }

    private func refresh() async {
        let current = await SaveStateFile.current()
        files = current
        hardcore = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
    }

    private func save(_ row: SaveStateSlot) {
        guard busySlot == nil else { return }
        if row.occupied {
            pendingOverwrite = row
        } else {
            performSave(row)
        }
    }

    private func performSave(_ row: SaveStateSlot) {
        let slot = row.slot
        let number = row.number
        busySlot = slot
        ARMSX2Bridge.saveState(toSlot: slot) { success in
            Task { @MainActor in
                let played = ARMSX2Bridge.currentGamePlayedSeconds()
                await refresh()
                if success, let file = files.first(where: { $0.slot == slot }) {
                    SaveStateMetadataStore.shared.recordSave(of: file, playedSeconds: played)
                    metadataRevision &+= 1
                }
                busySlot = nil
                statusHandler(
                    success
                        ? "\(settings.localized("State saved to slot")) \(number)"
                        : "\(settings.localized("Could not save slot")) \(number). \(settings.localized("Try again after gameplay has fully loaded."))",
                    !success
                )
            }
        }
    }

    private func load(_ row: SaveStateSlot) {
        guard busySlot == nil, !hardcore else { return }
        let number = row.number
        busySlot = row.slot
        ARMSX2Bridge.loadState(fromSlot: row.slot) { success in
            Task { @MainActor in
                busySlot = nil
                if success {
                    statusHandler("\(settings.localized("State loaded from slot")) \(number)", false)
                    onLoaded()
                } else {
                    await refresh()
                    statusHandler(
                        "\(settings.localized("Could not load slot")) \(number). \(settings.localized("Make sure it has a saved state first."))",
                        true
                    )
                }
            }
        }
    }
}

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

/// Up and Down keep the column (Save, Load), or take the nearest button the next row has.
/// The first and last rows wrap on a new press; a held direction stops there.
struct SaveStateGraph {
    struct Target {
        let id: String
        let column: Int
    }

    let order: [String]
    let links: [ControllerAccessibilityDirectionalLink]
    let freshPressRules: [ControllerAccessibilityBoundaryRule]

    init(rows: [[Target]]) {
        let boundary = ControllerAccessibilityDirectionalLink.navigationBoundary
        var links: [ControllerAccessibilityDirectionalLink] = []
        var rules: [ControllerAccessibilityBoundaryRule] = []
        func nearest(in row: [Target], to column: Int) -> String {
            row.min { (abs($0.column - column), $0.column) < (abs($1.column - column), $1.column) }?.id ?? boundary
        }
        for (index, row) in rows.enumerated() {
            let above = index > 0 ? rows[index - 1] : rows[rows.count - 1]
            let below = index + 1 < rows.count ? rows[index + 1] : rows[0]
            for (position, target) in row.enumerated() {
                let left = position > 0 ? row[position - 1].id : boundary
                let right = position + 1 < row.count ? row[position + 1].id : boundary
                links.append(.init(fromLabel: target.id, direction: .left, toLabel: left))
                links.append(.init(fromLabel: target.id, direction: .right, toLabel: right))
                let wraps = rows.count > 1
                links.append(.init(
                    fromLabel: target.id, direction: .up,
                    toLabel: wraps ? nearest(in: above, to: target.column) : boundary
                ))
                links.append(.init(
                    fromLabel: target.id, direction: .down,
                    toLabel: wraps ? nearest(in: below, to: target.column) : boundary
                ))
                if index == 0 {
                    rules.append(.init(direction: .up, fromLabel: target.id, requiresFreshPress: true))
                }
                if index == rows.count - 1 {
                    rules.append(.init(direction: .down, fromLabel: target.id, requiresFreshPress: true))
                }
            }
        }
        order = rows.flatMap { $0.map(\.id) }
        self.links = links
        freshPressRules = rules
    }
}

private struct SaveStateRowView: View {
    let row: SaveStateSlot
    let layout: SaveStateRowLayout
    let settings: SettingsStore
    let isLatest: Bool
    let isBusy: Bool
    let hardcore: Bool
    let primaryID: String?
    let quickSaveLine: String?
    let onSave: () -> Void
    let onLoad: () -> Void

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
        Text("\(row.number)")
            .font(.footnote.weight(.semibold))
            .lineLimit(1)
            .minimumScaleFactor(0.6)
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
            if isLatest {
                Text(settings.localized("Latest"))
                    .font(.caption2.weight(.bold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                    // The thumbnail keeps its size, so its badge can't grow with the text.
                    .dynamicTypeSize(...DynamicTypeSize.large)
                    .foregroundStyle(onAccent(accentColour))
                    .padding(.horizontal, 4)
                    .padding(.vertical, 1)
                    .background(accentColour, in: Capsule())
                    .padding(3)
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
            Text(title)
                .font(layout == .stacked ? .body.weight(.semibold) : .subheadline.weight(.semibold))
                .foregroundStyle(row.occupied ? OverlayTheme.textPrimary : OverlayTheme.textSecondary)
                .lineLimit(1)
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
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder
    private var savedLine: some View {
        if row.occupied, let date = row.modifiedDate {
            Label(SaveStateFormat.savedAt(date, settings: settings), systemImage: "clock")
                .saveStateMeta(layout)
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
        case .auto: String(format: settings.localized("Every %d min and when you exit"), 10)
        case .quick: quickSaveLine
        case .manual, .older: nil
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
                systemImage: "square.and.arrow.down",
                accessibilityLabel: String(
                    format: settings.localized(row.occupied ? "Save over %@" : "Save to %@"),
                    title
                ),
                tonal: true,
                accentIcon: !row.occupied,
                isPrimary: primaryID == saveID,
                isBusy: isBusy,
                width: layout.buttonWidth,
                action: onSave
            )
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
