// SaveStateCards.swift — save state rename, auto-save and undo cards
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Up to 32 characters. With a controller the name is typed on OrbitKeys, which has no limit
/// of its own, so the name is cut when it comes back.
struct SaveStateRenameCard: View {
    let settings: SettingsStore
    let title: String
    let placeholder: String
    let wide: Bool
    @Binding var text: String
    let ids: (name: String, clear: String, cancel: String, confirm: String)
    let onEdit: () -> Void
    let onCancel: () -> Void
    let onConfirm: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(OverlayTheme.textPrimary)
                .lineLimit(1)
            if wide {
                HStack(alignment: .top, spacing: 10) {
                    field
                    buttons
                }
            } else {
                field
                HStack(spacing: 10) { buttons }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(OverlayTheme.card, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).stroke(Color.white.opacity(0.14)))
        .shadow(color: .black.opacity(0.5), radius: 25, y: 20)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
    }

    private var field: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(settings.localized("Name"))
                .font(.caption)
                .foregroundStyle(OverlayTheme.textSecondary)
            HStack(spacing: 0) {
                TextField(placeholder, text: $text)
                    .font(.callout)
                    .foregroundStyle(OverlayTheme.textPrimary)
                    .submitLabel(.done)
                    .onSubmit(onConfirm)
                    .onChange(of: text) { _, value in
                        if value.count > SaveStateSlot.nameLimit {
                            text = String(value.prefix(SaveStateSlot.nameLimit))
                        }
                    }
                    .padding(.leading, 12)
                    .frame(minHeight: 44)
                    .controllerAccessibilityActionTarget(
                        id: ids.name,
                        label: ids.name,
                        focusedNeonCornerRadius: 12,
                        action: onEdit
                    )
                Button { text = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(settings.localized("Clear name"))
                .controllerAccessibilityActionTarget(
                    id: ids.clear,
                    label: ids.clear,
                    focusedNeonCornerRadius: 22
                ) { text = "" }
            }
            .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(accentColour, lineWidth: 2))
            HStack {
                Text(String(format: settings.localized("Empty shows “%@”. Saving over keeps the name."), placeholder))
                Spacer(minLength: 8)
                Text(String(format: settings.localized("%1$d/%2$d"), text.count, SaveStateSlot.nameLimit))
                    .monospacedDigit()
            }
            .font(.caption)
            .foregroundStyle(OverlayTheme.textSecondary)
        }
    }

    @ViewBuilder
    private var buttons: some View {
        Button(settings.localized("Cancel"), action: onCancel)
            .buttonStyle(.bordered)
            .controlSize(.large)
            .controllerAccessibilityActionTarget(id: ids.cancel, label: ids.cancel, action: onCancel)
        Button(settings.localized("Rename"), action: onConfirm)
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .controllerAccessibilityActionTarget(id: ids.confirm, label: ids.confirm, action: onConfirm)
    }
}

/// The same settings as in Settings > Emulator.
struct SaveStateAutoSaveCard: View {
    @Bindable var settings: SettingsStore
    let ids: (enabled: String, interval: String, leave: String, battery: String, done: String)
    let onDone: () -> Void

    @Environment(\.uiAccentColour) private var accentColour

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(settings.localized("Auto-save Settings"))
                .font(.subheadline.weight(.semibold))
                .padding(.horizontal, 8)
            Toggle(settings.localized("Auto-save"), isOn: $settings.autoSaveEnabled)
                .padding(8)
                .controllerAccessibilityToggleTarget(id: ids.enabled, label: ids.enabled, isOn: $settings.autoSaveEnabled)
            Group {
                VStack(alignment: .leading, spacing: 6) {
                    Text(settings.localized("Save every"))
                        .font(.caption)
                        .foregroundStyle(OverlayTheme.textSecondary)
                        .padding(.horizontal, 8)
                    Picker(settings.localized("Save every"), selection: $settings.autoSaveIntervalMinutes) {
                        ForEach(SettingsStore.autoSaveIntervals, id: \.self) { minutes in
                            Text(String(format: settings.localized("%d min"), minutes)).tag(minutes)
                        }
                    }
                    .pickerStyle(.segmented)
                    .controllerAccessibilityAdjustableTarget(
                        id: ids.interval,
                        label: ids.interval,
                        value: String(format: settings.localized("%d min"), settings.autoSaveIntervalMinutes),
                        onActivate: { stepInterval(1, wraps: true) },
                        onIncrement: { stepInterval(1) },
                        onDecrement: { stepInterval(-1) }
                    )
                }
                Toggle(settings.localized("Save when leaving the game"), isOn: $settings.autoSaveOnLeave)
                    .padding(8)
                    .controllerAccessibilityToggleTarget(id: ids.leave, label: ids.leave, isOn: $settings.autoSaveOnLeave)
                Toggle(settings.localized("Save when battery is low"), isOn: $settings.autoSaveOnLowBattery)
                    .padding(8)
                    .controllerAccessibilityToggleTarget(id: ids.battery, label: ids.battery, isOn: $settings.autoSaveOnLowBattery)
            }
            .disabled(!settings.autoSaveEnabled)
            Button(settings.localized("Done"), action: onDone)
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .controllerAccessibilityActionTarget(id: ids.done, label: ids.done, action: onDone)
                .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .foregroundStyle(OverlayTheme.textPrimary)
        .tint(accentColour)
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(OverlayTheme.card, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).stroke(Color.white.opacity(0.14)))
        .shadow(color: .black.opacity(0.5), radius: 25, y: 20)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
    }

    private func stepInterval(_ offset: Int, wraps: Bool = false) {
        let all = SettingsStore.autoSaveIntervals
        let next = (all.firstIndex(of: settings.autoSaveIntervalMinutes) ?? 1) + offset
        settings.autoSaveIntervalMinutes = wraps
            ? all[(next + all.count) % all.count] : all[min(max(next, 0), all.count - 1)]
    }
}

/// After a load, delete or save-over: what happened, and an Undo that lasts as long as Settings says.
struct SaveStateUndoToast: View {
    let undo: SaveStateUndoModel
    let settings: SettingsStore
    let hint: ControllerHintLine?
    let onUndo: () -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @AccessibilityFocusState private var focused: Bool

    var body: some View {
        if let item = undo.item {
            HStack(spacing: 12) {
                ZStack {
                    Color.black
                    if let data = item.preview, let image = UIImage(data: data) {
                        Image(uiImage: image).resizable().scaledToFill()
                    }
                }
                .frame(width: 56, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.caption)
                        .font(.caption)
                        .foregroundStyle(OverlayTheme.textSecondary)
                    Text(item.name)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(OverlayTheme.textPrimary)
                        .lineLimit(2)
                    if let hint {
                        hint
                            .font(.caption)
                            .foregroundStyle(OverlayTheme.textSecondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Button(action: onUndo) {
                    Label(settings.localized("Undo"), systemImage: "arrow.uturn.backward")
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(.white)
                        .frame(width: 104, height: 44)
                        .background(Color.white.opacity(0.14), in: Capsule())
                        .overlay(Capsule().stroke(Color.white.opacity(0.2)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(item.undoLabel)
            }
            .padding(EdgeInsets(top: 8, leading: 10, bottom: 11, trailing: 10))
            .overlay(alignment: .bottom) {
                // Reduce Motion leaves the draining bar out; the window is just as long.
                if !reduceMotion {
                    GeometryReader { proxy in
                        Capsule().fill(Color.white.opacity(0.14))
                            .overlay(alignment: .leading) {
                                Capsule().fill(Color.white.opacity(0.75))
                                    .frame(width: proxy.size.width * max(0, undo.remaining) / undo.duration)
                            }
                    }
                    .frame(height: 3)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 3)
                    .accessibilityHidden(true)
                }
            }
            .background(Color(red: 0.133, green: 0.149, blue: 0.180), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).stroke(Color.white.opacity(0.14)))
            .shadow(color: .black.opacity(0.5), radius: 18, y: 14)
            .frame(maxWidth: 460)
            .accessibilityElement(children: .contain)
            .accessibilityFocused($focused)
            .accessibilityAction(.escape) { undo.finish() }
            .onChange(of: focused) { _, value in undo.focusHeld = value }
        }
    }
}
