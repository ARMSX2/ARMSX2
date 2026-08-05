// AppIconSettingsView.swift — Alternate app icon picker
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

struct AppIconOption: Identifiable, Hashable {
    // nil selects the primary icon, anything else is a CFBundleAlternateIcons key
    let id: String?
    let displayName: String
    let previewName: String

    var isDefault: Bool { id == nil }

    // The primary icon has no -export twin, so the marketing render stands in
    var exportResourceName: String {
        guard let id else { return "icon-1024" }
        return "\(id)-export"
    }
}

extension AppIconOption {
    // Separate preview files keep the picker off runtime icon file naming
    static let allOptions: [AppIconOption] = [
        AppIconOption(id: nil, displayName: "Default", previewName: "icon-180"),
        AppIconOption(id: "appicon-synthwave", displayName: "Synthwave", previewName: "appicon-synthwave"),
        AppIconOption(id: "appicon-christmas", displayName: "Christmas", previewName: "appicon-christmas"),
        AppIconOption(id: "appicon-dark", displayName: "Dark", previewName: "appicon-dark"),
        AppIconOption(id: "appicon-frosted", displayName: "Frosted", previewName: "appicon-frosted"),
        AppIconOption(id: "appicon-gray", displayName: "Gray", previewName: "appicon-gray"),
        AppIconOption(id: "appicon-light", displayName: "Light", previewName: "appicon-light"),
        AppIconOption(id: "appicon-mystic", displayName: "Mystic Purple", previewName: "appicon-mystic"),
        AppIconOption(id: "appicon-purple", displayName: "Purple", previewName: "appicon-purple"),
        AppIconOption(id: "appicon-pridedark", displayName: "ARMSX2 Pride Dark", previewName: "appicon-pridedark"),
        AppIconOption(id: "appicon-pridelight", displayName: "ARMSX2 Pride Light", previewName: "appicon-pridelight")
    ]
}

private enum AppIconPreview {
    static func image(for option: AppIconOption) -> UIImage? {
        loaded[option.previewName]
    }

    // Loaded once, or a Form pass reopens every PNG per row
    private static let loaded: [String: UIImage] = AppIconOption.allOptions.reduce(into: [:]) { images, option in
        guard let url = Bundle.main.url(forResource: option.previewName, withExtension: "png"),
              let image = UIImage(contentsOfFile: url.path) else { return }
        images[option.previewName] = image
    }
}

// A host container such as LiveContainer relocates the bundle and owns the Home
// Screen icon, so switching cannot work there. A miss still lands on the alert.
private enum AppInstallEnvironment {
    static let isLikelyExternalContainer = Bundle.main.bundlePath
        .range(of: "/Documents/Applications/", options: .caseInsensitive) != nil
}

struct AppIconSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var currentIcon: String? = UIApplication.shared.alternateIconName
    @State private var pendingExport: AppIconOption?
    @State private var shareItem: ShareSheetItem?
    @State private var showExportError = false

    private var inExportMode: Bool { AppInstallEnvironment.isLikelyExternalContainer }

    var body: some View {
        Form {
            if inExportMode {
                Section {
                    externalContainerNotice
                }

                Section {
                    iconRows
                } footer: {
                    Text(settings.localized("How to use this icon: save the exported image, then create or edit a LiveContainer Home Screen shortcut and choose it as the shortcut’s icon. The App Switcher may still show LiveContainer."))
                }
            } else if UIApplication.shared.supportsAlternateIcons {
                Section {
                    iconRows
                } footer: {
                    Text(settings.localized("The Home Screen icon updates after switching. iOS may take a moment to refresh."))
                }
            } else {
                Section {
                    Text(settings.localized("Alternate app icons aren’t supported on this device."))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle(settings.localized("App Icon"))
        .navigationBarTitleDisplayMode(.inline)
        .alert(
            settings.localized("Couldn’t change the app icon."),
            isPresented: Binding(
                get: { pendingExport != nil },
                set: { if !$0 { pendingExport = nil } }
            ),
            presenting: pendingExport
        ) { option in
            Button(settings.localized("Export Icon")) { exportIcon(option) }
            Button(settings.localized("OK"), role: .cancel) {}
        } message: { _ in
            Text(settings.localized("iOS rejected the icon change. This can happen when ARMSX2 runs inside another app’s container. The icons are bundled — you can export one instead."))
        }
        .alert(settings.localized("Couldn’t export the icon."), isPresented: $showExportError) {
            Button(settings.localized("OK"), role: .cancel) {}
        } message: {
            Text(settings.localized("The icon image couldn’t be prepared for sharing."))
        }
        .sheet(item: $shareItem) { item in
            ActivityShareSheet(activityItems: [item.url])
        }
    }

    private var iconRows: some View {
        ForEach(AppIconOption.allOptions) { option in
            appIconRow(option)
        }
    }

    private var externalContainerNotice: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(settings.localized("External container install detected"))
                .font(.subheadline.weight(.semibold))
            Text(settings.localized("Apps running inside a host container such as LiveContainer can’t change their Home Screen icon directly — the icon belongs to the host, not ARMSX2. Export an icon and set it as your LiveContainer Home Screen shortcut icon."))
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }

    private func appIconRow(_ option: AppIconOption) -> some View {
        Button {
            rowTapped(option)
        } label: {
            HStack(spacing: 14) {
                previewThumbnail(for: option)

                Text(rowTitle(option))
                    .foregroundStyle(.primary)

                Spacer()

                trailingAccessory(for: option)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(rowTitle(option))
        .accessibilityHint(settings.localized(inExportMode
            ? "Exports this icon for a Home Screen shortcut."
            : "Sets this as the app icon."))
        .accessibilityAddTraits(currentIcon == option.id && !inExportMode ? .isSelected : [])
    }

    @ViewBuilder
    private func trailingAccessory(for option: AppIconOption) -> some View {
        if inExportMode {
            Image(systemName: "square.and.arrow.up")
                .foregroundStyle(.tint)
                .accessibilityHidden(true)
        } else if currentIcon == option.id {
            Image(systemName: "checkmark")
                .foregroundStyle(.tint)
                .accessibilityHidden(true)
        }
    }

    private func previewThumbnail(for option: AppIconOption) -> some View {
        previewShape
            .fill(Color(.tertiarySystemFill))
            .frame(width: 60, height: 60)
            .overlay {
                if let image = AppIconPreview.image(for: option) {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                } else {
                    Image(systemName: "app")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                }
            }
            .clipShape(previewShape)
    }

    private var previewShape: RoundedRectangle {
        RoundedRectangle(cornerRadius: 14, style: .continuous)
    }

    private func rowTitle(_ option: AppIconOption) -> String {
        option.isDefault ? settings.localized("Default") : option.displayName
    }

    private func rowTapped(_ option: AppIconOption) {
        if inExportMode {
            exportIcon(option)
        } else {
            applyIcon(option)
        }
    }

    private func applyIcon(_ option: AppIconOption) {
        // iOS prompts on every applied change, and re-applying the active name can throw
        guard currentIcon != option.id else { return }
        Task {
            do {
                try await UIApplication.shared.setAlternateIconName(option.id)
                currentIcon = UIApplication.shared.alternateIconName
            } catch {
                NSLog("[ARMSX2 iOS AppIcon] setAlternateIconName failed: %@", error.localizedDescription)
                pendingExport = option
            }
        }
    }

    private func exportIcon(_ option: AppIconOption) {
        guard let url = preparedExportURL(for: option) else {
            showExportError = true
            return
        }
        shareItem = ShareSheetItem(url: url)
    }

    // Copied under a readable name so the share sheet offers a clean save target
    private func preparedExportURL(for option: AppIconOption) -> URL? {
        guard let source = Bundle.main.url(forResource: option.exportResourceName, withExtension: "png") else {
            return nil
        }
        let destination = FileManager.default.temporaryDirectory
            .appendingPathComponent("ARMSX2 Icon - \(option.displayName).png")
        do {
            if FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.removeItem(at: destination)
            }
            try FileManager.default.copyItem(at: source, to: destination)
            return destination
        } catch {
            NSLog("[ARMSX2 iOS AppIcon] export prepare failed: %@", error.localizedDescription)
            return nil
        }
    }
}
