// ControllerNavigationScrollFocusCandidate.swift — right stick scroll focus candidates
// SPDX-License-Identifier: GPL-3.0+

import UIKit

/// Geometry-only ranking shared by live analog flow and its final selection.
/// Fully visible controls win; ties keep semantic order.
struct ControllerNavigationScrollFocusCandidate {
    let key: String
    let frame: CGRect
    let order: Int

    static func preferredKey(
        in candidates: [Self],
        viewport: CGRect,
        direction: MenuControllerCommand
    ) -> String? {
        let horizontal = direction == .left || direction == .right
        let towardStart = direction == .up || direction == .left
        let ranked = candidates.compactMap { candidate -> (
            candidate: Self, fullyVisible: Bool, hidden: CGFloat, distance: CGFloat
        )? in
            let frame = candidate.frame
            let intersection = frame.intersection(viewport)
            guard !intersection.isNull, !intersection.isInfinite,
                  intersection.width > 1, intersection.height > 1 else { return nil }
            let extent = horizontal ? frame.width : frame.height
            let visible = horizontal ? intersection.width : intersection.height
            let start = horizontal ? frame.minX : frame.minY
            let end = horizontal ? frame.maxX : frame.maxY
            let viewportStart = horizontal ? viewport.minX : viewport.minY
            let viewportEnd = horizontal ? viewport.maxX : viewport.maxY
            return (
                candidate, visible >= extent - 1, max(0, extent - visible),
                towardStart ? abs(start - viewportStart) : abs(end - viewportEnd)
            )
        }
        return ranked.min { lhs, rhs in
            if lhs.fullyVisible != rhs.fullyVisible { return lhs.fullyVisible }
            if !lhs.fullyVisible, abs(lhs.hidden - rhs.hidden) > 0.5 {
                return lhs.hidden < rhs.hidden
            }
            if abs(lhs.distance - rhs.distance) > 0.5 {
                return lhs.distance < rhs.distance
            }
            return lhs.candidate.order < rhs.candidate.order
        }?.candidate.key
    }
}
