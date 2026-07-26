// MenuBackgroundSupport.swift — Shared menu-tab background helpers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private struct MenuBackgroundHostEnvironmentKey: EnvironmentKey {
    static let defaultValue: PersistentMenuBackgroundHost? = nil
}

extension EnvironmentValues {
    var menuBackgroundHost: PersistentMenuBackgroundHost? {
        get { self[MenuBackgroundHostEnvironmentKey.self] }
        set { self[MenuBackgroundHostEnvironmentKey.self] = newValue }
    }
}

/// Owns the lifecycle state for the single background renderer installed above
/// the complete menu TabView hierarchy.
@MainActor
final class PersistentMenuBackgroundHost: ObservableObject {
    @Published private(set) var sessionStart = Date()

    @Published private(set) var rendererMounted: Bool
    @Published private(set) var presentationVisible = true
    @Published private var exclusivePreviewDepth = 0
    private var menuBackgroundAvailable = true
    private var releasedForGameplay = false

    init() {
        let hasBackground = SettingsStore.shared.hasCustomBackground
        menuBackgroundAvailable = hasBackground
        rendererMounted = hasBackground
    }

    /// Creates or destroys the menu renderer only when the user adds/removes
    /// the configured background itself. Tab selection never calls this.
    func setMenuBackgroundAvailable(_ isAvailable: Bool) {
        menuBackgroundAvailable = isAvailable
        rendererMounted = isAvailable
            && !(exclusivePreviewDepth > 0 && SettingsStore.shared.dynamicBackgroundsEnabled)
    }

    /// Starts a fresh renderer session only after gameplay released the previous
    /// one. Normal tab changes retain the same session and renderer identity.
    func reactivateForMenu(isAvailable: Bool) {
        if releasedForGameplay {
            releasedForGameplay = false
            sessionStart = Date()
        }
        setMenuBackgroundAvailable(isAvailable)
    }

    /// Hides the persistent renderer without removing it from SwiftUI. Video
    /// playback position and unmuted audio continue on background-disabled tabs.
    func setPresentationVisible(_ isVisible: Bool) {
        presentationVisible = isVisible
    }

    /// The Appearance screen uses the same expensive renderer in its preview.
    /// Dynamic backgrounds still yield to that preview to avoid two live
    /// animation/Metal renderers. Imported image/video backgrounds remain
    /// mounted but hidden so unmuted video audio and playback time are retained.
    func beginExclusivePreview() {
        exclusivePreviewDepth += 1
        if exclusivePreviewDepth == 1 && SettingsStore.shared.dynamicBackgroundsEnabled {
            rendererMounted = false
        }
    }

    func endExclusivePreview() {
        exclusivePreviewDepth = max(0, exclusivePreviewDepth - 1)
        if exclusivePreviewDepth == 0 {
            rendererMounted = menuBackgroundAvailable
        }
    }

    /// Stops the renderer when the complete menu hierarchy is being released.
    /// Normal tab changes and per-tab visibility toggles do not call this.
    func suspend() {
        rendererMounted = false
    }

    /// The surrounding MenuTabView is also removed at gameplay start, ensuring
    /// SwiftUI dismantles video, animated-image, display-link, and Metal views.
    func release() {
        releasedForGameplay = true
        menuBackgroundAvailable = false
        exclusivePreviewDepth = 0
        suspend()
    }

    var shouldShowRenderer: Bool {
        rendererMounted && presentationVisible && exclusivePreviewDepth == 0
    }
}

struct PersistentMenuBackgroundLayer: View {
    @ObservedObject var host: PersistentMenuBackgroundHost

    @ViewBuilder
    var body: some View {
        if host.rendererMounted {
            GeometryReader { geometry in
                BackgroundContainerView(size: geometry.size)
            }
            .environment(\.menuBackgroundSessionStart, host.sessionStart)
            .opacity(host.shouldShowRenderer ? 1 : 0)
            .ignoresSafeArea()
            .accessibilityHidden(true)
            .allowsHitTesting(false)
        }
    }
}

struct MenuBackgroundLayer: View {
    var isActive = true
    @Environment(\.menuBackgroundHost) private var persistentHost

    @ViewBuilder
    var body: some View {
        if persistentHost != nil {
            Color.clear
                .ignoresSafeArea()
                .accessibilityHidden(true)
                .allowsHitTesting(false)
        } else if isActive {
            GeometryReader { geometry in
                BackgroundContainerView(size: geometry.size)
            }
            .ignoresSafeArea()
            .accessibilityHidden(true)
            .allowsHitTesting(false)
        }
    }
}

/// Lets Games and BIOS publish their existing navigation/toolbar preferences
/// into MenuTabView's one persistent NavigationStack. Standalone previews and
/// Catalyst call sites retain their original self-contained navigation stack.
struct OptionalMenuNavigationStack<Content: View>: View {
    let embedded: Bool
    let content: Content

    init(embedded: Bool, @ViewBuilder content: () -> Content) {
        self.embedded = embedded
        self.content = content()
    }

    @ViewBuilder
    var body: some View {
        if embedded {
            content
        } else {
            NavigationStack {
                content
            }
        }
    }
}

private struct OptionalMenuNavigationChromeModifier: ViewModifier {
    let title: String
    let backgroundHidden: Bool
    let embedded: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if embedded {
            content
        } else {
            content
                .navigationTitle(title)
                .toolbarBackground(
                    backgroundHidden ? .hidden : .automatic,
                    for: .navigationBar
                )
        }
    }
}

private struct ClearNavigationContainerBackgroundModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.containerBackground(Color.clear, for: .navigation)
        } else {
            content
        }
    }
}

/// Keeps a tab's custom glass scene attached below its NavigationStack chrome.
/// MenuTabView retains every tab page, so this container is not dismantled when
/// another tab is selected.
private struct StableMenuContentGlassContainerModifier: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: 0) {
                content
            }
        } else {
            content
        }
    }
}

struct MenuBackgroundListRowModifier: ViewModifier {
    let isEnabled: Bool
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(reduceTransparency ? AnyShapeStyle(.background) : AnyShapeStyle(.regularMaterial), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .listRowInsets(EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 12))
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
        } else {
            content
        }
    }
}

/// Gives non-library menu rows the clear Liquid Glass tint used by Games cards
/// without changing the Games list-mode treatment.
struct GameCardTintMenuBackgroundListRowModifier: ViewModifier {
    let isEnabled: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if isEnabled {
            content
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .glassSurface(clear: true, cornerRadius: 16)
                .listRowInsets(EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 12))
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
        } else {
            content
        }
    }
}

extension View {
    func optionalMenuNavigationChrome(
        title: String,
        backgroundHidden: Bool,
        embedded: Bool
    ) -> some View {
        modifier(
            OptionalMenuNavigationChromeModifier(
                title: title,
                backgroundHidden: backgroundHidden,
                embedded: embedded
            )
        )
    }

    func stableMenuContentGlassContainer() -> some View {
        modifier(StableMenuContentGlassContainerModifier())
    }

    func clearNavigationContainerBackground() -> some View {
        modifier(ClearNavigationContainerBackgroundModifier())
    }

    func menuBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(MenuBackgroundListRowModifier(isEnabled: isEnabled))
    }

    func gameCardTintMenuBackgroundListRow(_ isEnabled: Bool) -> some View {
        modifier(GameCardTintMenuBackgroundListRowModifier(isEnabled: isEnabled))
    }
}
