// GameLibraryPanels.swift — The library's floating panels: per-game settings, Game Info and Disc Path
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Uses the same bounded command-deck presentation as the in-emulation Quick
/// Menu route. Keeping this in a full-screen presentation removes the sheet's
/// detent scroll view and isolates the retained Game Library from touch and
/// right-stick scrolling while per-game controls are active.
struct LibraryPerGameSettingsOverlay: View {
    let game: ISOEntry
    let preloadedSettings: [String: Any]?
    let initiallySelectsShaders: Bool
    let controllerInput: MenuControllerInputRouter?
    let onDone: () -> Void

    var body: some View {
        GameOverlayContainer(
            frameMode: .landscapePanel,
            dimsBackground: false
        ) { _ in
            PerGameSettingsPanel(
                game: game,
                preloadedSettings: preloadedSettings,
                initiallySelectsShaders: initiallySelectsShaders,
                controllerInput: controllerInput,
                onDone: onDone
            )
        }
        .background(Color.clear)
        .lightweightLiveWallpaperWhilePresented()
    }
}

/// Library destinations use a transparent full-screen presentation so the
/// system sheet dimmer cannot darken the game library. The invisible outer
/// surface still blocks and dismisses touch input; only the destination window
/// is visible.
struct GameLibraryForegroundPanel<Content: View>: View {
    let maximumWidth: CGFloat
    let maximumHeight: CGFloat
    let isContentVisible: Bool
    let onDismiss: () -> Void
    private let content: Content

    init(
        maximumWidth: CGFloat,
        maximumHeight: CGFloat,
        isContentVisible: Bool = true,
        onDismiss: @escaping () -> Void,
        @ViewBuilder content: () -> Content
    ) {
        self.maximumWidth = maximumWidth
        self.maximumHeight = maximumHeight
        self.isContentVisible = isContentVisible
        self.onDismiss = onDismiss
        self.content = content()
    }

    var body: some View {
        GeometryReader { proxy in
            let width = min(maximumWidth, max(280, proxy.size.width - 32))
            let height = min(maximumHeight, max(300, proxy.size.height - 32))

            ZStack {
                Color.clear
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture(perform: onDismiss)

                content
                    .frame(width: width, height: height)
                    .clipShape(
                        RoundedRectangle(cornerRadius: 26, style: .continuous)
                    )
                    .overlay {
                        RoundedRectangle(
                            cornerRadius: 26,
                            style: .continuous
                        )
                        .stroke(Color.primary.opacity(0.12), lineWidth: 0.7)
                    }
                    .shadow(color: .black.opacity(0.24), radius: 22, y: 10)
                    // Keep the controls mounted while previewing. An invisible
                    // Slider can finish the same touch drag, and the controller
                    // graph retains its focused row.
                    .opacity(isContentVisible ? 1 : 0)
                    .animation(
                        .easeOut(duration: isContentVisible ? 0.18 : 0.12),
                        value: isContentVisible
                    )
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .ignoresSafeArea(.container, edges: .all)
    }
}

struct GameInfoPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    let game: ISOEntry

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 14) {
                        CoverThumbnailView(
                            gameName: game.name,
                            coverURL: game.coverURL,
                            coverSignature: game.coverSignature,
                            width: 84,
                            height: 126
                        )

                        VStack(alignment: .leading, spacing: 6) {
                            Text(game.displayName)
                                .font(.headline)
                            Text(game.name)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section(settings.localized("Disc")) {
                    LabeledContent(settings.localized("Region")) {
                        Text(regionDisplay)
                    }
                    LabeledContent(settings.localized("Serial")) {
                        Text(metadataValue("serial"))
                            .textSelection(.enabled)
                    }
                    LabeledContent(settings.localized("CRC")) {
                        Text(metadataValue("crc"))
                            .textSelection(.enabled)
                    }
                    LabeledContent(settings.localized("Format")) {
                        Text(game.name.pathExtensionLabel)
                    }
                    LabeledContent(settings.localized("Size")) {
                        Text(game.sizeLabel)
                    }
                }

                Section(settings.localized("File")) {
                    Text(game.fileURL?.path ?? settings.localized("File path unavailable"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
            }
            .scrollContentBackground(.hidden)
            .navigationTitle(settings.localized("Game Info"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
        }
        .glassSurface(clear: false, cornerRadius: 26)
    }

    private var regionDisplay: String {
        let region = metadataValue("region")
        let flag = RegionFlag.emoji(for: region)
        return flag.isEmpty ? region : "\(flag) \(region)"
    }

    private func metadataValue(_ key: String) -> String {
        let value = game.metadata[key]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? settings.localized("Unknown") : value
    }
}

struct DiscLinkPicker: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    let discs: [ISOEntry]
    let hasLinkedDisc: Bool
    let onSelect: (ISOEntry?) -> Void

    var body: some View {
        NavigationStack {
            List {
                if hasLinkedDisc {
                    Button(settings.localized("Remove Disc Link"), role: .destructive) {
                        onSelect(nil)
                        dismiss()
                    }
                }
                if discs.isEmpty {
                    Text(settings.localized("No disc images found. Import an ISO first, then link it here."))
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(discs) { disc in
                        Button {
                            onSelect(disc)
                            dismiss()
                        } label: {
                            Text(disc.name).lineLimit(1)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .navigationTitle(settings.localized("Disc Path"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(settings.localized("Cancel")) { dismiss() }
                }
            }
        }
        .glassSurface(clear: false, cornerRadius: 26)
    }
}
