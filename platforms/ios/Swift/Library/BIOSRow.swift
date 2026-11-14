// BIOSRow.swift — one BIOS file in the BIOS list
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct BIOSRow: View {
    let bios: BIOSLibraryEntry
    let isDefault: Bool
    let backgroundActive: Bool
    let onSelect: () -> Void
    @State private var settings = SettingsStore.shared
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCardSubtitleColour) private var secondaryTextColour

    var body: some View {
        Button {
            onSelect()
        } label: {
            HStack(spacing: 12) {
                regionBadge(for: bios)

                VStack(alignment: .leading, spacing: 4) {
                    Text(bios.fileName)
                        .font(.body)
                    Text(bios.valid ? "\(bios.regionName) BIOS" : settings.localized("Not a boot BIOS"))
                        .font(.caption)
                        .foregroundStyle(secondaryTextColour)
                    if bios.valid && !bios.descriptionText.isEmpty {
                        Text(bios.descriptionText)
                            .font(.caption2)
                            .foregroundStyle(secondaryTextColour.opacity(0.78))
                            .lineLimit(1)
                    } else if !bios.valid {
                        Text(settings.localized("Companion ROM or unsupported BIOS dump"))
                            .font(.caption2)
                            .foregroundStyle(secondaryTextColour.opacity(0.78))
                            .lineLimit(1)
                    }
                }
                Spacer()
                if isDefault {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(accentColour)
                }
            }
        }
        .buttonStyle(.plain)
        .foregroundStyle(contentTextColour)
        .opacity(bios.valid ? 1 : 0.65)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(biosControllerLabel(for: bios))
        .controllerAccessibilityActionTarget(
            id: bios.filePath,
            label: biosControllerLabel(for: bios)
        ) { onSelect() }
    }

    private func biosControllerLabel(for bios: BIOSLibraryEntry) -> String {
        if bios.valid {
            return "\(bios.fileName), \(bios.regionName) BIOS"
        }
        return "\(bios.fileName), \(settings.localized("Not a boot BIOS"))"
    }

    @ViewBuilder
    private func regionBadge(for bios: BIOSLibraryEntry) -> some View {
        if backgroundActive {
            badgeContent(for: bios)
                .frame(width: 44, height: 44)
                .glassSurface(clear: true, cornerRadius: 12)
                .accessibilityHidden(true)
        } else {
            badgeContent(for: bios)
                .frame(width: 44, height: 44)
                .background(
                    Color(.secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                )
                .accessibilityHidden(true)
        }
    }

    @ViewBuilder
    private func badgeContent(for bios: BIOSLibraryEntry) -> some View {
        if let flag = flagEmoji(for: bios.countryCode) {
            Text(flag)
                .font(.title2)
        } else {
            Image(systemName: "globe")
                .font(.title3)
                .foregroundStyle(.secondary)
        }
    }

    private func flagEmoji(for countryCode: String) -> String? {
        let scalars = countryCode.uppercased().unicodeScalars
        guard scalars.count == 2 else { return nil }

        var unicodeScalars = String.UnicodeScalarView()
        for scalar in scalars {
            guard scalar.value >= 65, scalar.value <= 90,
                  let regional = UnicodeScalar(0x1F1E6 + scalar.value - 65) else {
                return nil
            }
            unicodeScalars.append(regional)
        }

        return String(unicodeScalars)
    }
}
