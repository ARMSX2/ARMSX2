// PadEditorCards.swift — the pad layout editor's bar, drop cards and their surface
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Opaque, like the overlay's Reduce Transparency look: glass would blur the pieces under it into smudges.
struct PadEditorSurface: ViewModifier {
    @Environment(\.uiAccentColour) private var accent

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: 26, style: .continuous)
        content
            .dynamicTypeSize(...DynamicTypeSize.accessibility3)
            .tint(accent)
            .background(
                LinearGradient(colors: [OverlayTheme.shellGradientTop, OverlayTheme.shell], startPoint: .top, endPoint: .bottom),
                in: shape
            )
            .overlay(shape.stroke(OverlayTheme.cardTopHighlight, lineWidth: 1))
            .clipShape(shape)
            .shadow(color: .black.opacity(0.28), radius: 22, y: 12)
    }
}

/// Proposes at most `cap` and takes its content's height; `.frame(maxHeight:)` grows to the cap whatever the content.
struct PadEditorHeightCap: Layout {
    let cap: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        subviews.first?.sizeThatFits(capped(proposal)) ?? .zero
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        subviews.first?.place(at: bounds.origin, proposal: capped(ProposedViewSize(bounds.size)))
    }

    private func capped(_ proposal: ProposedViewSize) -> ProposedViewSize {
        ProposedViewSize(width: proposal.width, height: min(proposal.height ?? cap, cap))
    }
}

/// A card under the bar, so a controller can reach every row; it scrolls when taller than `maxHeight`.
struct PadEditorDropCard<Content: View>: View {
    var maxHeight: CGFloat = .infinity
    @ViewBuilder let content: Content
    @Environment(\.horizontalSizeClass) private var sizeClass

    var body: some View {
        PadEditorHeightCap(cap: maxHeight) {
            ViewThatFits(in: .vertical) {
                stack
                ScrollView { stack }
            }
        }
        .modifier(PadEditorSurface())
        .frame(width: sizeClass == .regular ? 320 : 300)
    }

    private var stack: some View {
        VStack(spacing: 12) { content }.padding(12)
    }
}

extension View {
    /// An open card's controller scope, 10 above the editor's own session; Circle closes it.
    func padEditorDropScope(_ controllerInput: MenuControllerInputRouter?, onClose: @escaping () -> Void) -> some View {
        controllerAccessibilityNavigation(controllerInput: controllerInput, scopeKey: "pad.editor.drop", priority: 810, onBack: {
            onClose()
            return true
        })
    }
}

/// What the controller does right now, on one line or, when that does not fit, two.
struct PadEditorControllerHint: View {
    let parts: [ControllerHintLine.Part]

    var body: some View {
        let half = (parts.count + 1) / 2
        ViewThatFits(in: .horizontal) {
            ControllerHintLine(parts: parts)
            VStack(spacing: 4) {
                ControllerHintLine(parts: Array(parts.prefix(half)))
                ControllerHintLine(parts: Array(parts.dropFirst(half)))
            }
        }
        .font(.subheadline)
        .foregroundStyle(OverlayTheme.textPrimary)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .dynamicTypeSize(...DynamicTypeSize.xxxLarge)
        .background(OverlayFrostBackground().clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous)))
    }
}

/// The More card's toggles, remembered between sessions.
struct PadEditorPreferences: DynamicProperty {
    @AppStorage("ARMSX2iOSPadEditorSnap") var snap = true
    @AppStorage("ARMSX2iOSPadEditorGrid") var grid = false
    @AppStorage("ARMSX2iOSPadEditorMirror") var mirror = false
}

extension PadEditorSlot {
    @MainActor var title: String { SettingsStore.shared.localized(rawValue.capitalized) }

    /// Book only on the Duo.
    @MainActor static var offered: [PadEditorSlot] { allCases.filter { $0 != .book || AppState.shared.hingePresent } }
}

struct PadEditorMoreCard: View {
    let editor: PadEditor
    let maxHeight: CGFloat
    @Binding var snap: Bool
    @Binding var grid: Bool
    @Binding var mirror: Bool
    /// Per session, off when the editor opens.
    @Binding var touchAreas: Bool
    let onLayouts: () -> Void
    let onReset: () -> Void
    let onTry: () -> Void
    private let settings = SettingsStore.shared

    var body: some View {
        PadEditorDropCard(maxHeight: maxHeight) {
            OverlaySectionCard {
                // Turning it on closes the card, so the row only ever shows off.
                OverlayToggleRow(
                    label: settings.localized("Try Buttons"),
                    systemImage: "hand.point.up.left",
                    isOn: Binding(get: { editor.trying }, set: { if $0 { onTry() } })
                )
            }
            OverlaySectionCard {
                OverlayActionRow(label: settings.localized("Undo"), systemImage: "arrow.uturn.backward") { editor.undo() }
                    .disabled(!editor.canUndo)
                OverlayActionRow(label: settings.localized("Redo"), systemImage: "arrow.uturn.forward") { editor.redo() }
                    .disabled(!editor.canRedo)
            }
            OverlaySectionCard(title: settings.localized("Editing")) {
                OverlayToggleRow(label: settings.localized("Snap"), systemImage: "ruler", isOn: $snap)
                OverlayToggleRow(label: settings.localized("Show Grid"), systemImage: "grid", isOn: $grid)
                OverlayToggleRow(label: settings.localized("Mirror"), systemImage: "arrow.left.and.right", isOn: $mirror)
                OverlayToggleRow(label: settings.localized("Show Touch Areas"), systemImage: "square.dashed", isOn: $touchAreas)
            }
            OverlaySectionCard {
                OverlayActionRow(label: settings.localized("Show Hidden Buttons"), systemImage: "eye") { editor.showHidden() }
                    .disabled(!editor.hasHidden)
            }
            OverlaySectionCard {
                OverlayActionRow(label: settings.localized("Layouts…"), systemImage: "list.bullet", action: onLayouts)
                OverlayActionRow(label: settings.localized("Reset…"), systemImage: "arrow.counterclockwise", action: onReset)
            }
        }
    }
}

/// Switching only loads a layout into the draft; Duplicate and Rename write the layouts file at once.
struct PadEditorLayoutsCard: View {
    let presets: [PadLayoutPreset]
    /// Nil is the Current Layout.
    let openID: String?
    let canRename: Bool
    /// The layouts file could not be read, so nothing can be written to it.
    let readOnly: Bool
    let deviceSlot: PadEditorSlot
    let maxHeight: CGFloat
    @Binding var slot: PadEditorSlot
    let onPick: (PadLayoutPreset?) -> Void
    let onDuplicate: () -> Void
    let onRename: () -> Void
    private let settings = SettingsStore.shared

    var body: some View {
        let current = settings.localized("Current Layout")
        // An older version saved the layout as a preset with this same name, so the INI one says where it lives.
        let clash = presets.contains { $0.displayName.localizedCaseInsensitiveCompare(current) == .orderedSame }
        let trailing = [clash ? settings.localized("Settings File") : nil, openID == nil ? "✓" : nil].compactMap { $0 }
        PadEditorDropCard(maxHeight: maxHeight) {
            OverlaySectionCard {
                OverlayActionRow(
                    label: current,
                    trailingValue: trailing.isEmpty ? nil : trailing.joined(separator: " "),
                    keepsTrailingValue: true
                ) { onPick(nil) }
                ForEach(presets) { preset in
                    // By id: two layouts can share a name.
                    OverlayActionRow(
                        controllerNavigationID: preset.id,
                        label: preset.displayName,
                        trailingValue: openID == preset.id ? "✓" : nil,
                        keepsTrailingValue: true
                    ) {
                        onPick(preset)
                    }
                }
            }
            OverlaySectionCard {
                OverlayActionRow(label: settings.localized("Duplicate Layout"), systemImage: "plus.square.on.square", action: onDuplicate)
                    .disabled(readOnly)
                OverlayActionRow(label: settings.localized("Rename Layout"), systemImage: "pencil", action: onRename)
                    .disabled(readOnly || !canRename)
            }
            OverlaySectionCard(title: settings.localized("Orientation")) {
                Picker(settings.localized("Orientation"), selection: $slot) {
                    ForEach(PadEditorSlot.offered) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .controllerAccessibilityActionTarget(label: settings.localized("Orientation")) {
                    let offered = PadEditorSlot.offered
                    slot = offered[((offered.firstIndex(of: slot) ?? -1) + 1) % offered.count]
                }
                footer(settings.localized("Portrait, landscape and book each keep their own layout."))
                if slot != deviceSlot { footer(settings.localized("Turn the device to edit it full size.")) }
            }
        }
    }

    private func footer(_ text: String) -> some View {
        Text(text)
            .font(.subheadline)
            .foregroundStyle(OverlayTheme.textSecondary)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Resets ask first, except for one piece, which Undo covers.
struct PadEditorResetCard: View {
    let editor: PadEditor
    let maxHeight: CGFloat
    let onClose: () -> Void
    private let settings = SettingsStore.shared

    var body: some View {
        let slot = editor.slot
        PadEditorDropCard(maxHeight: maxHeight) {
            OverlaySectionCard {
                if let id = editor.selection {
                    OverlayActionRow(
                        label: String(format: settings.localized("Reset %@"), padPieceName(id)),
                        systemImage: "arrow.counterclockwise"
                    ) { editor.resetPiece(id) }
                        .disabled(editor.isDefault(id))
                }
                OverlayActionRow(
                    label: String(format: settings.localized("Reset %@"), slot.title),
                    systemImage: "arrow.counterclockwise",
                    isDestructive: true
                ) {
                    ask(
                        String(format: settings.localized("Reset the %@ layout?"), slot.title),
                        String(format: settings.localized("Every button in %@ goes back to its default place and size. Undo brings it back."), slot.title),
                        confirm: "Reset Layout", destructive: true
                    ) { editor.resetSlot() }
                }
                .disabled(editor.isDefault(slot))
                OverlayActionRow(
                    label: settings.localized("Reset All Orientations"),
                    systemImage: "arrow.counterclockwise",
                    isDestructive: true
                ) {
                    ask(
                        settings.localized("Reset every orientation?"),
                        settings.localized("Every orientation goes back to the default pad. Undo brings each one back while it's on screen."),
                        confirm: "Reset All Orientations", destructive: true
                    ) { editor.resetAll() }
                }
                .disabled(editor.isAllDefault)
            }
            OverlaySectionCard(title: settings.localized("Copy")) {
                ForEach(PadEditorSlot.offered.filter { $0 != slot }) { source in
                    OverlayActionRow(label: String(format: settings.localized("Copy From %@"), source.title), systemImage: "doc.on.doc") {
                        ask(
                            String(format: settings.localized("Copy %@ into %@?"), source.title, slot.title),
                            String(format: settings.localized("This replaces the %@ layout. Undo brings it back."), slot.title),
                            confirm: "Copy Layout", destructive: false
                        ) { editor.copy(from: source) }
                    }
                    .disabled(!editor.canCopy(from: source))
                }
            }
        }
    }

    private func ask(_ title: String, _ message: String, confirm: String, destructive: Bool, run: @escaping @MainActor () -> Void) {
        onClose()
        ControllerPrompt.shared.ask(title, message: message, actions: [
            .init(title: settings.localized(confirm), isDestructive: destructive, run: run),
            .cancel,
        ])
    }
}

struct PadEditorSaveCard: View {
    /// "Update “<name>”" or "Update Current Layout"; nil hides the row.
    let updateLabel: String?
    let hasGame: Bool
    let globalName: String
    /// The layouts file could not be read, so only the INI working copy can be saved.
    let readOnly: Bool
    let maxHeight: CGFloat
    @Binding var saveAsNew: Bool
    @Binding var scope: PadEditorSaveScope
    let onBack: () -> Void
    let onSave: () -> Void
    private let settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accent

    var body: some View {
        PadEditorDropCard(maxHeight: maxHeight) {
            OverlaySectionCard(title: settings.localized("Save to")) {
                if let updateLabel {
                    OverlayActionRow(label: updateLabel, trailingValue: saveAsNew ? nil : "✓", keepsTrailingValue: true) {
                        saveAsNew = false
                    }
                }
                OverlayActionRow(
                    label: settings.localized("Save as New Layout"),
                    trailingValue: saveAsNew ? "✓" : nil,
                    keepsTrailingValue: true
                ) { saveAsNew = true }
            }
            OverlaySectionCard(title: settings.localized("Use for")) {
                scopeRow(.allGames, "All Games")
                if hasGame { scopeRow(.thisGame, "This Game Only") }
                scopeRow(.keepDefault, "Don't Change Default")
                footer(readOnly
                    ? settings.localized("Layouts can't be saved right now because the layouts file couldn't be read.")
                    : scopeFooter)
            }
            HStack(spacing: 8) {
                Button(action: onBack) {
                    Text(settings.localized("Back")).font(.body).foregroundStyle(accent)
                        .padding(.horizontal, 16).frame(height: 44)
                        .background(accent.opacity(0.16), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                }
                .controllerAccessibilityActionTarget(label: settings.localized("Back"), action: onBack)
                Button(action: onSave) {
                    Text(settings.localized("Save Layout")).font(.headline).foregroundStyle(.white)
                        .frame(maxWidth: .infinity).frame(height: 44)
                        .background(accent, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                }
                .controllerAccessibilityActionTarget(label: settings.localized("Save Layout"), action: onSave)
                .disabled(readOnly)
                .opacity(readOnly ? 0.4 : 1)
            }
            .buttonStyle(.plain)
        }
    }

    private var scopeFooter: String {
        switch scope {
        case .allGames: return settings.localized("Games without their own layout use it.")
        case .thisGame: return String(format: settings.localized("Only this game uses it. Other games keep “%@”."), globalName)
        case .keepDefault: return settings.localized("Games keep the layouts they use now.")
        }
    }

    private func scopeRow(_ value: PadEditorSaveScope, _ label: String) -> some View {
        OverlayActionRow(label: settings.localized(label), trailingValue: scope == value ? "✓" : nil, keepsTrailingValue: true) {
            scope = value
        }
    }

    private func footer(_ text: String) -> some View {
        Text(text)
            .font(.subheadline)
            .foregroundStyle(OverlayTheme.textSecondary)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct PadEditorBar: View {
    let editor: PadEditor
    let title: String
    let subtitle: String
    let onCancel: () -> Void
    let onTitle: () -> Void
    let onReset: () -> Void
    let onMore: () -> Void
    let onDone: () -> Void
    let onStopTrying: () -> Void
    private let settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accent

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 4) {
                cancel
                undoRedo
                // The try line takes its own row when it does not fit, rather than truncating.
                heading.frame(maxWidth: editor.trying ? nil : 220)
                more
                done
            }
            VStack(spacing: 4) {
                heading.frame(maxWidth: .infinity).frame(height: 44)
                Rectangle().fill(OverlayTheme.separator).frame(height: 0.5)
                // At the larger text sizes Cancel and Done get a row of their own rather than truncating.
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: 4) {
                        cancel
                        HStack(spacing: 4) { undoRedo; more }.frame(maxWidth: .infinity)
                        done
                    }
                    VStack(spacing: 4) {
                        HStack(spacing: 4) { undoRedo; more }
                        HStack(spacing: 4) { cancel; Spacer(minLength: 0); done }
                    }
                }
            }
            .frame(maxWidth: 420)
        }
        .padding(4)
        .dynamicTypeSize(...DynamicTypeSize.xxxLarge)
        .background(OverlayFrostBackground().clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous)))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(OverlayTheme.cardTopHighlight, lineWidth: 1))
        .shadow(color: OverlayTheme.cardShadow, radius: 4, y: 2)
        .buttonStyle(.plain)
    }

    private var cancel: some View {
        Button(action: onCancel) {
            Text(settings.localized("Cancel")).font(.body).foregroundStyle(OverlayTheme.textPrimary)
                .padding(.horizontal, 16).frame(height: 44).fixedSize().contentShape(Rectangle())
        }
    }

    @ViewBuilder private var undoRedo: some View {
        glyph("arrow.uturn.backward", label: "Undo", enabled: editor.canUndo && !editor.trying) { editor.undo() }
        glyph("arrow.uturn.forward", label: "Redo", enabled: editor.canRedo && !editor.trying) { editor.redo() }
    }

    @ViewBuilder private var more: some View {
        glyph("arrow.counterclockwise", label: "Reset", enabled: !editor.trying, action: onReset)
        glyph("ellipsis", label: "More", enabled: !editor.trying, action: onMore)
    }

    private func glyph(_ name: String, label: String, enabled: Bool = true, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: name).font(.body).foregroundStyle(OverlayTheme.textPrimary.opacity(enabled ? 1 : 0.35))
                .frame(width: 44, height: 44).contentShape(Rectangle())
        }
        .disabled(!enabled)
        .accessibilityLabel(Text(settings.localized(label)))
    }

    @ViewBuilder private var heading: some View {
        if editor.trying {
            HStack(spacing: 8) {
                Text(settings.localized("Press buttons to try them.")).font(.subheadline).foregroundStyle(OverlayTheme.textPrimary)
                Button(action: onStopTrying) {
                    Text(settings.localized("Stop Trying")).font(.body).foregroundStyle(accent)
                        .frame(minHeight: 44).contentShape(Rectangle())
                }
            }
            .padding(.horizontal, 8)
        } else {
            titleButton
        }
    }

    private var titleButton: some View {
        Button(action: onTitle) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(title).font(.headline).foregroundStyle(OverlayTheme.textPrimary)
                    .lineLimit(1).truncationMode(.middle)
                Image(systemName: "chevron.down").font(.caption).foregroundStyle(OverlayTheme.textSecondary)
                Text(subtitle).font(.subheadline).foregroundStyle(OverlayTheme.textSecondary).lineLimit(1)
            }
            .padding(.horizontal, 8)
            .frame(minHeight: 44)
            .contentShape(Rectangle())
        }
        .accessibilityLabel(Text(settings.localized("Layout")))
        .accessibilityValue(Text(title))
    }

    private var done: some View {
        Button(action: onDone) {
            Text(settings.localized("Done")).font(.headline).foregroundStyle(.white)
                .padding(.horizontal, 16).frame(height: 44).fixedSize()
                .background(accent, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
    }
}
