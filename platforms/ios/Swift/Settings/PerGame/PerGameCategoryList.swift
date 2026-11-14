// PerGameCategoryList.swift — the per-game category rail and portrait menu
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PerGameCategoryList: View {
    let controller: PerGameSettingsControllerCoordinator
    let controllerInput: MenuControllerInputRouter?
    let showsRail: Bool
    @Environment(\.uiAccentColour) private var accentColour
    @State private var settings = SettingsStore.shared

    var body: some View {
        if showsRail {
            categoryRail
        } else {
            portraitCategoryMenu
        }
    }

    @ViewBuilder
    private var categoryRail: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(PerGameSettingsCategory.allCases) { category in
                    let selected = controller.openCategory == category
                    Button {
                        controller.openCategory = category
                    } label: {
                        CategoryRailLabel(
                            title: settings.localized(category.titleKey),
                            systemImage: category.systemImage,
                            selected: selected
                        )
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: controller.landscapeCategoryControllerTargetID(category),
                        label: settings.localized(category.titleKey)
                    ) {
                        controller.enterControllerDetail(category)
                    }
                    .focusEffectDisabled()
                }
            }
            .padding(.vertical, 8)
        }
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 220,
                isEnabled: controller.controllerColumn == .categories,
                searchesNearbyScrollViews: true
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
        }
        .environment(
            \.controllerAccessibilityTargetsSuppressed,
            controller.controllerColumn == .detail
                || controller.controllerAlertSuppressesContentTargets
        )
    }

    private var portraitCategoryMenu: some View {
        Form {
            Section {
                ForEach(PerGameSettingsCategory.allCases) { category in
                    Button {
                        controller.enterControllerDetail(category)
                    } label: {
                        HStack(spacing: 12) {
                            Label(
                                settings.localized(category.titleKey),
                                systemImage: category.systemImage
                            )
                            Spacer(minLength: 8)
                            Image(systemName: "chevron.right")
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(accentColour)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: controller.landscapeCategoryControllerTargetID(category),
                        label: settings.localized(category.titleKey)
                    ) {
                        controller.enterControllerDetail(category)
                    }
                    .focusEffectDisabled()
                }
            }
        }
        .background {
            ControllerRightStickScrollTarget(
                controllerInput: controllerInput,
                axes: .vertical,
                priority: 220,
                isEnabled: controller.controllerColumn == .categories,
                searchesNearbyScrollViews: true
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .allowsHitTesting(false)
        }
        .scrollContentBackground(.hidden)
    }
}

private struct CategoryRailLabel: View {
    let title: String
    let systemImage: String
    let selected: Bool
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.controllerTextAppearance) private var textAppearance

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: systemImage)
                .frame(width: OverlayTheme.rowIconWidth)
                .foregroundStyle(accentColour)
            Text(title)
                .font(.callout)
                .controllerFocusedTextColor(
                    normal: selected
                        ? (textAppearance.normalColor ?? OverlayTheme.textPrimary)
                        : (textAppearance.secondaryColor ?? OverlayTheme.textSecondary)
                )
                .lineLimit(2)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(minHeight: 40)
        .padding(.horizontal, 12)
        .background(
            selected ? accentColour.opacity(0.16) : Color.clear,
            in: RoundedRectangle(cornerRadius: 8, style: .continuous)
        )
    }
}
