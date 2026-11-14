// MenuEnvironmentKeys.swift — environment values the menu tabs share
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct MenuTabIsActiveEnvironmentKey: EnvironmentKey {
    static let defaultValue = true
}

private struct MenuBackgroundSessionStartEnvironmentKey: EnvironmentKey {
    static let defaultValue = Date()
}

private struct MenuLargeTitleNamespaceEnvironmentKey: EnvironmentKey {
    static let defaultValue: Namespace.ID? = nil
}

private struct MenuLargeTitleMorphActiveEnvironmentKey: EnvironmentKey {
    static let defaultValue = false
}

private struct MenuLargeTitleIsSourceEnvironmentKey: EnvironmentKey {
    static let defaultValue: Bool? = nil
}

private struct MenuTabBarContentMarginEnvironmentKey: EnvironmentKey {
    static let defaultValue: CGFloat = 0
}

extension EnvironmentValues {
    var menuTabIsActive: Bool {
        get { self[MenuTabIsActiveEnvironmentKey.self] }
        set { self[MenuTabIsActiveEnvironmentKey.self] = newValue }
    }

    var menuBackgroundSessionStart: Date {
        get { self[MenuBackgroundSessionStartEnvironmentKey.self] }
        set { self[MenuBackgroundSessionStartEnvironmentKey.self] = newValue }
    }

    var menuLargeTitleNamespace: Namespace.ID? {
        get { self[MenuLargeTitleNamespaceEnvironmentKey.self] }
        set { self[MenuLargeTitleNamespaceEnvironmentKey.self] = newValue }
    }

    var menuLargeTitleMorphActive: Bool {
        get { self[MenuLargeTitleMorphActiveEnvironmentKey.self] }
        set { self[MenuLargeTitleMorphActiveEnvironmentKey.self] = newValue }
    }

    var menuLargeTitleIsSource: Bool? {
        get { self[MenuLargeTitleIsSourceEnvironmentKey.self] }
        set { self[MenuLargeTitleIsSourceEnvironmentKey.self] = newValue }
    }

    /// Room a list needs below its last row to end just above the menu tab bar.
    var menuTabBarContentMargin: CGFloat {
        get { self[MenuTabBarContentMarginEnvironmentKey.self] }
        set { self[MenuTabBarContentMarginEnvironmentKey.self] = newValue }
    }
}
