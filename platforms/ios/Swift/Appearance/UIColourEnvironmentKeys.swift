// UIColourEnvironmentKeys.swift — interface colour environment values and label styles
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct UIAccentColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.blue
}

private struct UISecondaryTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UITitleTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIContentTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UITabTitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UITabSubtitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UIBottomTabBarColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.blue
}

private struct UIBottomTabBarUnselectedColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UICardTitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UICardSubtitleColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UIContextMenuColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIContextMenuSecondaryColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.secondary
}

private struct UIContextMenuFocusedColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIImportActionColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.primary
}

private struct UIToolbarColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.blue
}

private struct UICriticalTextColourEnvironmentKey: EnvironmentKey {
    static let defaultValue = Color.red
}

extension EnvironmentValues {
    var uiAccentColour: Color {
        get { self[UIAccentColourEnvironmentKey.self] }
        set { self[UIAccentColourEnvironmentKey.self] = newValue }
    }

    var uiSecondaryTextColour: Color {
        get { self[UISecondaryTextColourEnvironmentKey.self] }
        set { self[UISecondaryTextColourEnvironmentKey.self] = newValue }
    }

    var uiTitleTextColour: Color {
        get { self[UITitleTextColourEnvironmentKey.self] }
        set { self[UITitleTextColourEnvironmentKey.self] = newValue }
    }

    var uiContentTextColour: Color {
        get { self[UIContentTextColourEnvironmentKey.self] }
        set { self[UIContentTextColourEnvironmentKey.self] = newValue }
    }

    var uiTabTitleColour: Color {
        get { self[UITabTitleColourEnvironmentKey.self] }
        set { self[UITabTitleColourEnvironmentKey.self] = newValue }
    }

    var uiTabSubtitleColour: Color {
        get { self[UITabSubtitleColourEnvironmentKey.self] }
        set { self[UITabSubtitleColourEnvironmentKey.self] = newValue }
    }

    var uiBottomTabBarColour: Color {
        get { self[UIBottomTabBarColourEnvironmentKey.self] }
        set { self[UIBottomTabBarColourEnvironmentKey.self] = newValue }
    }

    var uiBottomTabBarUnselectedColour: Color {
        get { self[UIBottomTabBarUnselectedColourEnvironmentKey.self] }
        set { self[UIBottomTabBarUnselectedColourEnvironmentKey.self] = newValue }
    }

    var uiCardTitleColour: Color {
        get { self[UICardTitleColourEnvironmentKey.self] }
        set { self[UICardTitleColourEnvironmentKey.self] = newValue }
    }

    var uiCardSubtitleColour: Color {
        get { self[UICardSubtitleColourEnvironmentKey.self] }
        set { self[UICardSubtitleColourEnvironmentKey.self] = newValue }
    }

    var uiContextMenuColour: Color {
        get { self[UIContextMenuColourEnvironmentKey.self] }
        set { self[UIContextMenuColourEnvironmentKey.self] = newValue }
    }

    var uiContextMenuSecondaryColour: Color {
        get { self[UIContextMenuSecondaryColourEnvironmentKey.self] }
        set { self[UIContextMenuSecondaryColourEnvironmentKey.self] = newValue }
    }

    var uiContextMenuFocusedColour: Color {
        get { self[UIContextMenuFocusedColourEnvironmentKey.self] }
        set { self[UIContextMenuFocusedColourEnvironmentKey.self] = newValue }
    }

    var uiImportActionColour: Color {
        get { self[UIImportActionColourEnvironmentKey.self] }
        set { self[UIImportActionColourEnvironmentKey.self] = newValue }
    }

    var uiToolbarColour: Color {
        get { self[UIToolbarColourEnvironmentKey.self] }
        set { self[UIToolbarColourEnvironmentKey.self] = newValue }
    }

    var uiCriticalTextColour: Color {
        get { self[UICriticalTextColourEnvironmentKey.self] }
        set { self[UICriticalTextColourEnvironmentKey.self] = newValue }
    }
}

/// Preserves the master UI's semantic split: label copy inherits its normal
/// primary/secondary colour while symbols use the active theme accent.
struct UIAccentIconLabelStyle: LabelStyle {
    @Environment(\.uiAccentColour) private var accentColour

    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 8) {
            configuration.icon
                .symbolRenderingMode(.monochrome)
                .foregroundStyle(accentColour)
            configuration.title
        }
    }
}

private struct UICriticalForegroundStyleModifier: ViewModifier {
    @Environment(\.uiCriticalTextColour) private var criticalTextColour

    func body(content: Content) -> some View {
        content.foregroundStyle(criticalTextColour)
    }
}

extension View {
    /// Keeps destructive, error, and other critical labels independent from
    /// the user's ordinary and secondary text palettes.
    func uiCriticalForegroundStyle() -> some View {
        modifier(UICriticalForegroundStyleModifier())
    }
}

private struct UISecondaryForegroundStyleModifier: ViewModifier {
    let color: Color?

    func body(content: Content) -> some View {
        // One view identity across system and palette colours: a conditional
        // branch here replaced RootView's subtree, and the retiring branch's
        // cleanup then stopped controller delivery with the menu still up.
        content.foregroundColor(color)
    }
}

extension View {
    /// Colours interface labels and symbols that do not own an accent or
    /// controller-focus style. Keeping this optional preserves system defaults
    /// when Secondary Text Colour is disabled.
    func uiSecondaryForegroundStyle(_ color: Color?) -> some View {
        modifier(UISecondaryForegroundStyleModifier(color: color))
    }
}
