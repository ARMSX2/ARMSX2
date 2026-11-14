// CheatsAvailableSection.swift — The Cheats & Patches panel's database downloads
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct CheatsAvailableSection: View {
    let controllerInput: MenuControllerInputRouter?
    let controllerTarget: CheatsPatchesManagerView.ControllerTarget
    let onDownload: (_ asCheat: Bool) -> Void

    @State private var settings = SettingsStore.shared
    @State private var store = PatchStore.shared

    private typealias ControllerTarget = CheatsPatchesManagerView.ControllerTarget

    var body: some View {
        Section {
            if store.hasConfiguredPatchDatabase {
                Button {
                    store.dismissMessage()
                    onDownload(false)
                } label: {
                    Label(
                        hasDatabasePatch ? settings.localized("Reinstall Patches") : settings.localized("Download Patches"),
                        systemImage: hasDatabasePatch ? "arrow.clockwise.icloud" : "icloud.and.arrow.down"
                    )
                    .modifier(controllerFocus(.downloadPatches))
                }
                .disabled(!store.identityState.canUseDatabase || !store.canManageInstalledFiles || store.isDownloading)
                .accessibilityHint(settings.localized(store.identityState.canUseDatabase ? "Downloads matching patches from every configured source" : "Requires an identified game CRC"))
                .id(ControllerTarget.downloadPatches)
            } else {
                Text(settings.localized("No patch download source is configured. Add one in Advanced or import a file below."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if store.hasConfiguredCheatDatabase {
                Button {
                    store.dismissMessage()
                    onDownload(true)
                } label: {
                    Label(
                        hasDatabaseCheat ? settings.localized("Reinstall Cheats") : settings.localized("Download Cheats"),
                        systemImage: hasDatabaseCheat ? "arrow.clockwise.icloud" : "icloud.and.arrow.down"
                    )
                    .modifier(controllerFocus(.downloadCheats))
                }
                .disabled(!store.identityState.canUseDatabase || !store.canManageInstalledFiles || store.isDownloading)
                .id(ControllerTarget.downloadCheats)
            }

            if store.isDownloading {
                HStack(spacing: 10) {
                    ProgressView()
                    Text(settings.localized("Downloading…"))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel(settings.localized("Download in progress"))
            }

        } header: {
            Text(settings.localized("Available"))
        } footer: {
            Text(settings.localized("Downloads query every configured source and merge results into one file after making a backup. Patches must match this game’s region and version."))
        }
    }

    private var hasDatabasePatch: Bool {
        store.installed.contains { $0.source == .database && !$0.isCheat }
    }

    private var hasDatabaseCheat: Bool {
        store.installed.contains { $0.source == .database && $0.isCheat }
    }

    private func controllerFocus(_ target: ControllerTarget) -> CheatsControllerFocusModifier {
        cheatsControllerFocus(target, controllerInput: controllerInput, controllerTarget: controllerTarget)
    }
}
