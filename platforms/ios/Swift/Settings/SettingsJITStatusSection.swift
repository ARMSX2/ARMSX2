// SettingsJITStatusSection.swift — JIT status and the StikDebug button in Settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct SettingsJITStatusSection: View {
    let jitAvailable: Bool
    let noJITFallbackActive: Bool
    let backgroundActive: Bool
    @Binding var stikDebugOpenFailed: Bool
    @Binding var stikDebugOpenInProgress: Bool
    let refreshJITStatus: () -> Void
    @State private var settings = SettingsStore.shared

    var body: some View {
        Section {
            jitStatusRow
                .gameCardTintMenuBackgroundListRow(backgroundActive)

            Button {
                openStikDebug()
            } label: {
                Label(settings.localized("Open StikDebug/StosDebug"), systemImage: "bolt.horizontal.circle")
            }
            .disabled(stikDebugOpenInProgress)
            .controllerAccessibilityActionTarget(
                id: "settings.root.open-stik-debug",
                label: settings.localized("Open StikDebug/StosDebug"),
                action: openStikDebug
            )
            .gameCardTintMenuBackgroundListRow(backgroundActive)

            Text(settings.localized("JIT Access means iOS currently allows executable memory. Confirm the real runtime state in-game: the OSD should show EE:JIT, IOP:JIT, and VU:JIT. Match the StikDebug/StosDebug script to the JIT Script setting in Emulator settings."))
                .font(.caption)
                .foregroundStyle(.secondary)
                .gameCardTintMenuBackgroundListRow(backgroundActive)
        } header: {
            Text(settings.localized("JIT Status"))
        }
    }

    private var jitStatusRow: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(jitStatusColor)
                .frame(width: 12, height: 12)
                .shadow(color: jitStatusColor.opacity(0.45), radius: 5)

            VStack(alignment: .leading, spacing: 3) {
                Text(settings.localized(jitStatusTitle))
                    .font(.body)
                Text(settings.localized(jitStatusSubtitle))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Text(settings.localized(jitStatusBadge))
                .font(.caption.weight(.semibold))
                .foregroundStyle(jitStatusColor)
        }
        .accessibilityElement(children: .combine)
    }

    private var jitStatusColor: Color {
        if stikDebugOpenFailed {
            return .orange
        }
        if noJITFallbackActive {
            return .orange
        }
        if jitAvailable {
            return .blue
        }
        return .red
    }

    private var jitStatusTitle: String {
        if stikDebugOpenFailed {
            return "StikDebug/StosDebug Did Not Open"
        }
        if noJITFallbackActive {
            return "JIT Off"
        }
        if jitAvailable {
            return "JIT Access Detected"
        }
        return "JIT Not Detected"
    }

    private var jitStatusSubtitle: String {
        if stikDebugOpenFailed {
            return "Open StikDebug/StosDebug manually, then run the selected script and relaunch ARMSX2."
        }
        if noJITFallbackActive {
            return "No-JIT fallback is active. Use StikDebug/StosDebug/\(settings.jitScriptProtocol.label) for dynarec."
        }
        if jitAvailable {
            return "Access is available. Confirm EE:JIT / IOP:JIT / VU:JIT in the in-game OSD. Current script: \(settings.jitScriptProtocol.label)."
        }
        return "Launch with StikDebug/StosDebug using the \(settings.jitScriptProtocol.label) script to enable JIT."
    }

    private var jitStatusBadge: String {
        if stikDebugOpenFailed {
            return "Manual Open"
        }
        if noJITFallbackActive {
            return "JIT Off"
        }
        return jitAvailable ? "Check OSD" : "Needs StikDebug/StosDebug"
    }

    private func openStikDebug() {
        stikDebugOpenInProgress = true
        stikDebugOpenFailed = false
        StikDebugLauncher.open(reason: "settings-root") { success in
            stikDebugOpenInProgress = false
            stikDebugOpenFailed = !success
            refreshJITStatus()
        }
    }
}
