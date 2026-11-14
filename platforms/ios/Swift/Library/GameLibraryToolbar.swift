// GameLibraryToolbar.swift — the library's Boot BIOS, Import, Layout and More toolbar items
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameLibraryToolbar: ToolbarContent {
    let coordinator: GameLibraryControllerCoordinator
    let library: GameLibraryModel
    @Bindable var appearance: GameLibraryAppearance
    let coverStore: CoverStore
    let settings: SettingsStore
    let logoStore: ARMSX2LogoStore
    let useGamesLogo: Bool
    let libraryIsEmpty: Bool
    let accentColour: Color
    let controllerInput: MenuControllerInputRouter?
    let menuTabIsActive: Bool

    var body: some ToolbarContent {
        ToolbarItem(id: "menu.gamesBrand", placement: .topBarLeading) {
            if useGamesLogo, let image = logoStore.image {
                Button(action: coordinator.performBootBIOSAction) {
                    Image(uiImage: image)
                        .resizable()
                        .interpolation(.high)
                        .scaledToFit()
                        .frame(width: 154, height: 22)
                        .padding(.horizontal, 18)
                        .frame(minHeight: 42)
                        .contentShape(Capsule())
                        .modifier(coordinator.controllerToolbarFocus(
                            .bootBIOS,
                            controllerInput: controllerInput,
                            menuTabIsActive: menuTabIsActive,
                            minimumWidth: 190
                        ))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(settings.localized("Boot BIOS"))
            } else {
                Button(action: coordinator.performBootBIOSAction) {
                    Text(settings.localized("Boot BIOS"))
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(accentColour)
                        .lineLimit(1)
                        .fixedSize(horizontal: true, vertical: false)
                        .modifier(coordinator.controllerToolbarFocus(
                            .bootBIOS,
                            controllerInput: controllerInput,
                            menuTabIsActive: menuTabIsActive,
                            baseHorizontalPadding: 14,
                            minimumWidth: 112
                        ))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(settings.localized("Boot BIOS"))
                .fixedSize()
            }
        }
        ToolbarItem(id: "menu.import", placement: .topBarTrailing) {
            Button {
                coordinator.openGameImporter()
            } label: {
                Image(systemName: "plus")
                    .modifier(coordinator.controllerToolbarFocus(
                        .importGames,
                        controllerInput: controllerInput,
                        menuTabIsActive: menuTabIsActive
                    ))
            }
            .buttonStyle(.plain)
            .accessibilityLabel(settings.localized("Import Games"))
        }
        .prefersVerticalBar()
        ToolbarItem(id: "menu.layout", placement: .topBarTrailing) {
            Menu {
                Button {
                    coordinator.selectAlternateLibraryLayout()
                } label: {
                    Label(
                        settings.localized(appearance.libraryLayout == "grid" ? "Show List" : "Show Grid"),
                        systemImage: appearance.libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2"
                    )
                }

                if appearance.libraryLayout == "grid",
                   coordinator.usesLandscapeGameCardScale {
                    Toggle(isOn: $appearance.landscapeCoverFlowEnabled) {
                        Label(settings.localized("Landscape Cover Flow"), systemImage: "rectangle.landscape.rotate")
                    }
                }

                Divider()

                Button {
                    coordinator.presentGameLibraryViewOptions()
                } label: {
                    Label(
                        settings.localized(
                            "Game Library View Options"
                        ),
                        systemImage: "slider.horizontal.3"
                    )
                }
            } label: {
                Image(systemName: appearance.libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2")
                    .modifier(coordinator.controllerToolbarFocus(
                        .layout,
                        controllerInput: controllerInput,
                        menuTabIsActive: menuTabIsActive
                    ))
            }
            .buttonStyle(.plain)
            .accessibilityLabel(settings.localized("Library Layout"))
        }
        ToolbarItem(id: "menu.more", placement: .topBarTrailing) {
            Menu {
                Button {
                    coordinator.presentMenuPanel {
                        coordinator.pendingCoverGameName = nil
                        coordinator.showCoverImporter = true
                    }
                } label: {
                    Label(settings.localized("Import Local Covers"), systemImage: "photo.badge.plus")
                }

                Button {
                    library.downloadMissingCovers()
                } label: {
                    Label(settings.localized("Download Missing Covers"), systemImage: "icloud.and.arrow.down")
                }
                .disabled(coverStore.isDownloadingCovers || libraryIsEmpty)

                Button {
                    coordinator.presentMenuPanel {
                        coordinator.coverTemplateDraft = coverStore.coverURLTemplate
                        coordinator.showCoverTemplateEditor = true
                    }
                } label: {
                    Label(settings.localized("Cover Source"), systemImage: "link")
                }

                Button {
                    coordinator.presentMenuPanel {
                        coverStore.resetCoverURLTemplate()
                    }
                } label: {
                    Label(settings.localized("Reset Cover Template"), systemImage: "arrow.counterclockwise")
                }
                Divider()
                Button {
                    library.loadGames(forceMetadataRefresh: true)
                } label: {
                    Label(settings.localized("Refresh"), systemImage: "arrow.clockwise")
                }
            } label: {
                Image(systemName: "ellipsis")
                    .modifier(coordinator.controllerToolbarFocus(
                        .more,
                        controllerInput: controllerInput,
                        menuTabIsActive: menuTabIsActive
                    ))
            }
            .buttonStyle(.plain)
            .accessibilityLabel(settings.localized("More"))
        }
    }
}
