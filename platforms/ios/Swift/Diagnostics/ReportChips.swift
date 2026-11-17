// ReportChips.swift — wrapping chip rows for the report sheet
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ChipFlow: Layout {
    var spacing: CGFloat = 8

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        arrange(width: proposal.width ?? .infinity, subviews: subviews).size
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let origins = arrange(width: bounds.width, subviews: subviews).origins
        for (subview, origin) in zip(subviews, origins) {
            subview.place(at: CGPoint(x: bounds.minX + origin.x, y: bounds.minY + origin.y), proposal: .unspecified)
        }
    }

    private func arrange(width: CGFloat, subviews: Subviews) -> (origins: [CGPoint], size: CGSize) {
        var origins: [CGPoint] = []
        var x: CGFloat = 0
        var y: CGFloat = 0
        var rowHeight: CGFloat = 0
        var widest: CGFloat = 0
        for subview in subviews {
            let size = subview.sizeThatFits(.unspecified)
            if x > 0, x + size.width > width {
                x = 0
                y += rowHeight + spacing
                rowHeight = 0
            }
            origins.append(CGPoint(x: x, y: y))
            x += size.width + spacing
            widest = max(widest, x - spacing)
            rowHeight = max(rowHeight, size.height)
        }
        return (origins, CGSize(width: widest, height: y + rowHeight))
    }
}

struct ReportChips: View {
    struct Option {
        let raw: String
        let label: String
    }

    let options: [Option]
    let selected: Set<String>
    let idPrefix: String
    let onTap: (String) -> Void

    var body: some View {
        ChipFlow {
            ForEach(Array(options.enumerated()), id: \.offset) { index, option in
                let isSelected = selected.contains(option.raw)
                Button { onTap(option.raw) } label: {
                    Text(option.label)
                        .font(.subheadline)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 7)
                        .foregroundStyle(isSelected ? Color.white : Color.primary)
                        .background(Capsule().fill(isSelected ? Color.accentColor : Color(.tertiarySystemFill)))
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(isSelected ? .isSelected : [])
                .controllerAccessibilityActionTarget(id: "\(idPrefix).\(index)", label: option.label) { onTap(option.raw) }
            }
        }
        .padding(.vertical, 4)
    }
}
