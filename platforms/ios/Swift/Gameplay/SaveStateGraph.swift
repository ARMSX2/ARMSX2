// SaveStateGraph.swift — save state graph
// SPDX-License-Identifier: GPL-3.0+

/// Up and Down keep the column (Save, Load), or take the nearest button the next row has.
/// The first and last rows wrap on a new press; a held direction stops there.
struct SaveStateGraph {
    struct Target {
        let id: String
        let column: Int
    }

    let order: [String]
    let links: [ControllerAccessibilityDirectionalLink]
    let freshPressRules: [ControllerAccessibilityBoundaryRule]

    init(rows: [[Target]], wraps: Bool = true) {
        let boundary = ControllerAccessibilityDirectionalLink.navigationBoundary
        var links: [ControllerAccessibilityDirectionalLink] = []
        var rules: [ControllerAccessibilityBoundaryRule] = []
        func nearest(in row: [Target], to column: Int) -> String {
            row.min { (abs($0.column - column), $0.column) < (abs($1.column - column), $1.column) }?.id ?? boundary
        }
        for (index, row) in rows.enumerated() {
            let above = index > 0 ? rows[index - 1] : rows[rows.count - 1]
            let below = index + 1 < rows.count ? rows[index + 1] : rows[0]
            for (position, target) in row.enumerated() {
                let left = position > 0 ? row[position - 1].id : boundary
                let right = position + 1 < row.count ? row[position + 1].id : boundary
                links.append(.init(fromLabel: target.id, direction: .left, toLabel: left))
                links.append(.init(fromLabel: target.id, direction: .right, toLabel: right))
                let first = index == 0
                let last = index == rows.count - 1
                let upWraps = first && (!wraps || rows.count < 2)
                let downWraps = last && (!wraps || rows.count < 2)
                links.append(.init(
                    fromLabel: target.id, direction: .up,
                    toLabel: upWraps ? boundary : nearest(in: above, to: target.column)
                ))
                links.append(.init(
                    fromLabel: target.id, direction: .down,
                    toLabel: downWraps ? boundary : nearest(in: below, to: target.column)
                ))
                if wraps, first {
                    rules.append(.init(direction: .up, fromLabel: target.id, requiresFreshPress: true))
                }
                if wraps, last {
                    rules.append(.init(direction: .down, fromLabel: target.id, requiresFreshPress: true))
                }
            }
        }
        order = rows.flatMap { $0.map(\.id) }
        self.links = links
        freshPressRules = rules
    }
}
