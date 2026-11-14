// MenuTabBarPillViews.swift — controller focus pill on the UIKit tab bar
// SPDX-License-Identifier: GPL-3.0+

import UIKit

#if !targetEnvironment(macCatalyst)
final class ControllerFocusPillView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false
        backgroundColor = .clear
        layer.borderWidth = 0 // SwiftUI draws the focus box from this view's frame
        layer.shadowOpacity = 0
    }

    required init?(coder: NSCoder) { nil }

    func updateCornerRadius(_ radius: CGFloat) {
        layer.cornerRadius = radius
    }
}
#endif
