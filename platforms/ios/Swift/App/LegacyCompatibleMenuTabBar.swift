// LegacyCompatibleMenuTabBar.swift — UITabBar behind the menu tab bar
// SPDX-License-Identifier: GPL-3.0+

import UIKit

#if !targetEnvironment(macCatalyst)
final class LegacyCompatibleMenuTabBar: UITabBar {
    var configurationKey = ""
    var onControllerFocusFrameChanged: ((CGRect?) -> Void)?
    var controllerNavigationEnabled = false
    private var stableMeasurementWidth: CGFloat = 0

    var controllerFocusedIndex: Int? {
        didSet {
            guard oldValue != controllerFocusedIndex else { return }
            updateControllerFocusPill(animated: window != nil)
        }
    }

    private let controllerFocusPill = ControllerFocusPillView()

    /// SwiftUI can detach a retained safe-area inset for one reconciliation
    /// pass while changing tabs. During that pass both its proposal and
    /// temporary superview may be the width of one tab item. Keep the last
    /// valid bar width until the UIWindow publishes a new authoritative
    /// width (including after rotation or Stage Manager resizing).
    func resolvedMeasurementWidth(
        windowWidth: CGFloat,
        containerWidth: CGFloat,
        boundsWidth: CGFloat,
        proposalWidth: CGFloat
    ) -> CGFloat {
        if windowWidth > 1 {
            stableMeasurementWidth = windowWidth
            return windowWidth
        }
        if stableMeasurementWidth > 1 {
            return stableMeasurementWidth
        }
        let proposedInitialWidth = [
            containerWidth,
            boundsWidth,
            proposalWidth,
        ].first(where: { $0 > 1 }) ?? 0
        // Measured before it has a window, a one-item proposal makes UIKit cache "G", "B" and "S".
        let minimumBarWidth = min(
            UIApplication.shared.appScreen.bounds.width,
            usesLargeLayout(windowSize: window?.bounds.size) ? 360 : 276
        )
        let initialWidth = max(proposedInitialWidth, minimumBarWidth)
        stableMeasurementWidth = initialWidth
        return initialWidth
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        controllerFocusPill.isHidden = true
        controllerFocusPill.isUserInteractionEnabled = false
        addSubview(controllerFocusPill)
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        controllerFocusPill.isHidden = true
        controllerFocusPill.isUserInteractionEnabled = false
        addSubview(controllerFocusPill)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        setSystemBarBackgroundHidden(false)
        updateControllerFocusPill(animated: false)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard let window, window.bounds.width > 1 else { return }
        stableMeasurementWidth = window.symmetricSafeWidth
        invalidateIntrinsicContentSize()
        setNeedsLayout()
    }

    /// Native floating tab bars can nest their item controls beneath a
    /// private content container. Discover the first control on each
    /// branch and always convert its bounds into this tab bar's coordinate
    /// space before publishing controller-orb geometry.
    private func controllerItemControls() -> [UIControl] {
        var controls: [UIControl] = []

        func collectControls(in view: UIView) {
            for subview in view.subviews {
                if let control = subview as? UIControl {
                    controls.append(control)
                } else {
                    collectControls(in: subview)
                }
            }
        }

        collectControls(in: self)
        let visibleControls = controls.filter { control in
            let frame = control.convert(control.bounds, to: self)
            return !control.isHidden
                && control.alpha > 0.01
                && frame.width > 1
                && frame.height > 1
                && frame.intersects(bounds)
        }
        let expectedCount = items?.count ?? 0

        // Prefer UIKit's accessibility relationship: unlike private view
        // class names, the tab item's title remains stable across native
        // floating-bar layouts and OS releases.
        if expectedCount > 0, let items {
            let semanticControls = items.compactMap { item -> UIControl? in
                guard let title = item.title, !title.isEmpty else {
                    return nil
                }
                return visibleControls
                    .filter { control in
                        guard let label = control.accessibilityLabel else {
                            return false
                        }
                        return label.compare(
                            title,
                            options: [.caseInsensitive, .diacriticInsensitive]
                        ) == .orderedSame
                            || label.localizedCaseInsensitiveContains(title)
                    }
                    .max { lhs, rhs in
                        let leftFrame = lhs.convert(lhs.bounds, to: self)
                        let rightFrame = rhs.convert(rhs.bounds, to: self)
                        return leftFrame.width * leftFrame.height
                            < rightFrame.width * rightFrame.height
                    }
            }
            let uniqueSemanticControls = Set(
                semanticControls.map(ObjectIdentifier.init)
            )
            if semanticControls.count == expectedCount,
               uniqueSemanticControls.count == expectedCount {
                if controllerNavigationEnabled {
                    for control in semanticControls {
                        control.focusEffect = nil
                    }
                }
                return semanticControls
            }
        }

        // Controls sharing a horizontal centre are one tab; keep the largest, or nested ones shift indices.
        let sortedControls = visibleControls.sorted {
            $0.convert($0.bounds, to: self).minX
                < $1.convert($1.bounds, to: self).minX
        }
        var horizontalGroups: [[UIControl]] = []
        for control in sortedControls {
            let centerX = control.convert(control.bounds, to: self).midX
            if let lastGroup = horizontalGroups.last,
               let representative = lastGroup.first {
                let representativeX = representative.convert(
                    representative.bounds,
                    to: self
                ).midX
                if abs(centerX - representativeX) <= 6 {
                    horizontalGroups[horizontalGroups.count - 1]
                        .append(control)
                    continue
                }
            }
            horizontalGroups.append([control])
        }
        let groupedControls = horizontalGroups.compactMap { group in
            group.max { lhs, rhs in
                let leftFrame = lhs.convert(lhs.bounds, to: self)
                let rightFrame = rhs.convert(rhs.bounds, to: self)
                return leftFrame.width * leftFrame.height
                    < rightFrame.width * rightFrame.height
            }
        }
        let resolvedControls = groupedControls.count == expectedCount
            ? groupedControls
            : sortedControls
        if controllerNavigationEnabled {
            for control in resolvedControls {
                control.focusEffect = nil
            }
        }
        return resolvedControls
    }

    private func setSystemBarBackgroundHidden(_ hidden: Bool) {
        for view in subviews {
            let className = NSStringFromClass(type(of: view))
            if className.contains("BarBackground") {
                view.isHidden = hidden
            }
        }
    }

    private func updateControllerFocusPill(animated: Bool) {
        let itemControls = controllerItemControls()
        guard let controllerFocusedIndex else {
            controllerFocusPill.isHidden = true
            onControllerFocusFrameChanged?(nil)
            return
        }
        // UIKit briefly removes its item controls while rotating or
        // reconciling the native bar. Retain the last valid orb anchor
        // during that pass instead of publishing a false disappearance.
        guard itemControls.indices.contains(controllerFocusedIndex) else {
            // Keep the last valid neon pill visible too; UIKit will lay
            // out the requested item again on the next pass.
            return
        }

        controllerFocusPill.isHidden = false
        if let firstItem = itemControls.first {
            insertSubview(controllerFocusPill, belowSubview: firstItem)
        }
        let itemControl = itemControls[controllerFocusedIndex]
        let itemFrame = itemControl.convert(itemControl.bounds, to: self)
        let boundsFrame = bounds.insetBy(dx: 8, dy: 3)
        let visualContentFrame = itemVisualContentFrame(in: itemControl)
        let centerX = visualContentFrame?.midX ?? itemFrame.midX
        let minimumWidth: CGFloat =
            usesLargeLayout(windowSize: window?.bounds.size) ? 120 : 0
        let focusWidth = max(itemFrame.width + 8, minimumWidth)
        let proposedFrame = CGRect(
            x: centerX - (focusWidth / 2),
            y: itemFrame.minY + 3,
            width: focusWidth,
            height: max(1, itemFrame.height - 6)
        )
        let targetFrame = proposedFrame
            .intersection(boundsFrame)
        // UIKit can briefly hand us an empty intersection while the tab
        // bar is being reparented during rotation. Keep the last valid
        // SwiftUI orb anchor until the next layout pass resolves it.
        guard !targetFrame.isNull,
              !targetFrame.isInfinite,
              targetFrame.width > 1,
              targetFrame.height > 1 else {
            // Preserve the preceding valid focus surface during a
            // transient zero/intersecting layout pass.
            return
        }
        onControllerFocusFrameChanged?(
            controllerOrbFrame(
                centeredAt: centerX,
                visualContentFrame: visualContentFrame,
                focusFrame: targetFrame
            )
        )
        controllerFocusPill.updateCornerRadius(
            max(0, targetFrame.height / 2)
        )

        let updates = {
            self.controllerFocusPill.frame = targetFrame
            self.controllerFocusPill.alpha = 1
        }
        if animated {
            UIView.animate(
                withDuration: 0.28,
                delay: 0,
                usingSpringWithDamping: 0.84,
                initialSpringVelocity: 0.12,
                options: [.beginFromCurrentState, .allowUserInteraction],
                animations: updates
            )
        } else {
            updates()
        }
    }

    private func itemVisualContentFrame(in control: UIControl) -> CGRect? {
        var contentFrame: CGRect?

        func collectContent(from view: UIView) {
            for subview in view.subviews
            where !subview.isHidden && subview.alpha > 0.01 {
                if (subview is UILabel || subview is UIImageView),
                   subview.bounds.width > 1,
                   subview.bounds.height > 1 {
                    let frame = subview.convert(subview.bounds, to: self)
                    contentFrame = contentFrame.map { $0.union(frame) }
                        ?? frame
                }
                collectContent(from: subview)
            }
        }

        collectContent(from: control)
        return contentFrame
    }

    private func controllerOrbFrame(
        centeredAt centerX: CGFloat,
        visualContentFrame: CGRect?,
        focusFrame: CGRect
    ) -> CGRect {
        let maximumWidth: CGFloat =
            usesLargeLayout(windowSize: window?.bounds.size) ? 112 : 96
        let contentWidth = (visualContentFrame?.width ?? 64) + 24
        let width = min(
            focusFrame.width,
            min(maximumWidth, max(72, contentWidth))
        )
        let contentHeight = (visualContentFrame?.height ?? 32) + 8
        let height = min(focusFrame.height, max(30, contentHeight))
        let centerY = visualContentFrame?.midY ?? focusFrame.midY
        let proposedFrame = CGRect(
            x: centerX - (width / 2),
            y: centerY - (height / 2),
            width: width,
            height: height
        )
        return proposedFrame.intersection(focusFrame)
    }
}
#endif
