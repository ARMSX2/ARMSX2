// ControllerFocusBox.swift — focus box, bottom bar glow and depth modifiers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// One visual implementation for controller focus across Forms, libraries,
/// alerts, toolbars, and the bottom navigation bar. Animated styles own only
/// this lightweight outline timeline; they never invalidate the focused row.
enum ControllerFocusBoxShape {
    case roundedRectangle
    case pill
}

func controllerFocusCornerRadius(
    size: CGSize,
    baseline: CGFloat,
    shape: ControllerFocusBoxShape
) -> CGFloat {
    guard size.width > 0, size.height > 0 else { return baseline }
    let halfHeight = size.height / 2
    switch shape {
    case .roundedRectangle:
        // Explicit rectangular surfaces retain a stable continuous radius.
        return min(halfHeight, max(6, baseline))
    case .pill:
        if size.width >= size.height * 1.6 {
            return halfHeight
        }
        // Tall cards must never become vertical ovals. They retain a softened
        // continuous rectangle while horizontal controls become true pills.
        return min(
            halfHeight,
            max(baseline, min(22, size.height * 0.28))
        )
    }
}

struct ControllerFocusBox: View {
    var cornerRadius: CGFloat = 12
    var performanceOptimized = false
    var animatesArtwork = true
    var shape: ControllerFocusBoxShape = .pill

    @State private var settings = SettingsStore.shared
    @State private var gameCoverThemePreview = GameCoverThemePreviewStore.shared
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        GeometryReader { proxy in
            AdaptiveAnimationTimeline(
                domain: .controllerEffects,
                paused: reduceMotion
                    || !animatesArtwork
                    || !settings.controllerFocusBoxStyle.isAnimated
            ) { timeline in
                ControllerFocusBoxArtwork(
                    style: settings.controllerFocusBoxStyle,
                    palette: settings.controllerFocusBoxPalette,
                    customColors: gameCoverThemePreview.preview.map {
                        $0.backgroundPalette.gradientColors
                    } ?? settings.controllerFocusBoxCustomColor.map { [$0.color] },
                    animationSpeed: settings.controllerFocusBoxAnimationSpeed,
                    glowIntensity: settings.controllerFocusBoxGlowIntensity,
                    cornerRadius: controllerFocusCornerRadius(
                        size: proxy.size,
                        baseline: cornerRadius,
                        shape: shape
                    ),
                    time: reduceMotion || !animatesArtwork
                        ? 0
                        : timeline.date.timeIntervalSinceReferenceDate,
                    performanceOptimized: performanceOptimized
                )
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// The persistent bottom navigation bar intentionally uses a restrained focus
/// treatment. Theme artwork such as waves, particles, blood, or filled
/// gradients can obscure the compact tab labels, so this variant keeps only a
/// palette-aware neon outline and its glow. Other focus surfaces continue to
/// use `ControllerFocusBox` and therefore retain the selected artwork style.
struct ControllerBottomNavigationFocusGlow: View {
    var cornerRadius: CGFloat = 18

    @State private var settings = SettingsStore.shared
    @State private var gameCoverThemePreview = GameCoverThemePreviewStore.shared

    var body: some View {
        GeometryReader { proxy in
            let paletteColors = gameCoverThemePreview.preview.map {
                $0.backgroundPalette.gradientColors
            } ?? settings.controllerFocusBoxCustomColor.map { [$0.color] }
                ?? settings.controllerFocusBoxPalette.colors
            let primary = paletteColors.first ?? .blue
            let secondary = paletteColors.dropFirst().first ?? primary
            let intensity = min(
                max(settings.controllerFocusBoxGlowIntensity, 0),
                2
            )
            let shape = RoundedRectangle(
                cornerRadius: controllerFocusCornerRadius(
                    size: proxy.size,
                    baseline: cornerRadius,
                    shape: .pill
                ),
                style: .continuous
            )

            shape
                .strokeBorder(
                    LinearGradient(
                        colors: [primary, secondary, primary],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    ),
                    lineWidth: 2
                )
                .shadow(
                    color: primary.opacity(0.82 * intensity),
                    radius: CGFloat(5 * intensity)
                )
                .shadow(
                    color: secondary.opacity(0.52 * intensity),
                    radius: CGFloat(10 * intensity)
                )
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

private struct ControllerFocusBoxDepthPresentationModifier: ViewModifier {
    let isVisible: Bool
    let cornerRadius: CGFloat
    let performanceOptimized: Bool
    @Environment(\.controllerNavigationDepthEffectEnabled)
    private var depthEffectEnabled

    func body(content: Content) -> some View {
        content
            .background {
                if isVisible && depthEffectEnabled {
                    ControllerFocusBox(
                        cornerRadius: cornerRadius,
                        performanceOptimized: performanceOptimized,
                        animatesArtwork: false,
                        shape: .pill
                    )
                }
            }
    }
}

struct ControllerFocusDepthAnchorTarget {
    let id: String
    let bounds: Anchor<CGRect>
}

struct ControllerFocusDepthAnchorPreferenceKey: PreferenceKey {
    static let defaultValue: [ControllerFocusDepthAnchorTarget] = []

    static func reduce(
        value: inout [ControllerFocusDepthAnchorTarget],
        nextValue: () -> [ControllerFocusDepthAnchorTarget]
    ) {
        value.append(contentsOf: nextValue())
    }
}

private struct ControllerFocusDepthAnchorModifier: ViewModifier {
    let id: String
    let isFocused: Bool
    @Environment(\.controllerNavigationDepthEffectEnabled)
    private var depthEffectEnabled

    func body(content: Content) -> some View {
        content.anchorPreference(
            key: ControllerFocusDepthAnchorPreferenceKey.self,
            value: .bounds
        ) { anchor in
            isFocused && depthEffectEnabled
                ? [ControllerFocusDepthAnchorTarget(id: id, bounds: anchor)]
                : []
        }
    }
}

private struct ControllerFocusDepthBehindGlassModifier: ViewModifier {
    let isEnabled: Bool
    let force: Bool
    let cornerRadius: CGFloat
    @Environment(\.controllerNavigationDepthEffectEnabled)
    private var depthEffectEnabled
    @Environment(\.controllerFocusDepthHandledByAncestor)
    private var handledByAncestor
    @Environment(\.menuControllerInputRouter)
    private var controllerInput
    @Environment(\.controllerAccessibilityNavigationSession)
    private var navigationSession
    @Environment(\.uiAccentColour)
    private var accentColour

    func body(content: Content) -> some View {
        content.backgroundPreferenceValue(
            ControllerFocusDepthAnchorPreferenceKey.self
        ) { anchors in
            GeometryReader { proxy in
                if isEnabled,
                   depthEffectEnabled,
                   (force || !handledByAncestor),
                   let target = anchors.last {
                    let frame = depthFrame(for: target, in: proxy)
                    // The session's foreground rectangle, inset and scroll
                    // position included, keeps the behind-glass pill locked
                    // to the focus box instead of a second raw row anchor.
                    ControllerNavigationAnimatedOrbField(
                        targetID: target.id,
                        targetFrame: frame,
                        primaryColor: accentColour,
                        style: .plain,
                        controllerInput: controllerInput,
                        showsOrbs: false,
                        showsNeonOutline: true,
                        tracksTargetFrameDirectly:
                            navigationSession?.isScrollPresentationActive
                                == true,
                        neonCornerRadius: cornerRadius,
                        animatesNeonArtwork: false,
                        neonShape: .pill
                    )
                    .opacity(
                        navigationSession?
                            .focusPresentationIsHiddenForScrolling == true
                            ? 0
                            : 1
                    )
                    .animation(
                        .easeOut(duration: 0.14),
                        value: navigationSession?
                            .focusPresentationIsHiddenForScrolling
                    )
                }
            }
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
    }

    private func depthFrame(
        for target: ControllerFocusDepthAnchorTarget,
        in proxy: GeometryProxy
    ) -> CGRect {
        guard let navigationSession,
              focusedTargetMatches(target.id, in: navigationSession),
              let foregroundFrame = navigationSession.focusedFrame else {
            return proxy[target.bounds]
        }

        // `focusedFrame` and global geometry share the window origin.
        let ownerFrame = proxy.frame(in: .global)
        return foregroundFrame.offsetBy(
            dx: -ownerFrame.minX,
            dy: -ownerFrame.minY
        )
    }

    private func focusedTargetMatches(
        _ targetID: String,
        in navigationSession: ControllerAccessibilityNavigationSession
    ) -> Bool {
        guard let focusedID = navigationSession.focusedElementID else {
            return false
        }
        return focusedID == targetID || focusedID.hasPrefix("\(targetID)#")
    }
}

private struct ControllerFocusBoxPresentationModifier: ViewModifier {
    let isVisible: Bool
    let cornerRadius: CGFloat
    let performanceOptimized: Bool

    func body(content: Content) -> some View {
        content
            .modifier(
                ControllerFocusBoxDepthPresentationModifier(
                    isVisible: isVisible,
                    cornerRadius: cornerRadius,
                    performanceOptimized: performanceOptimized
                )
            )
            .overlay {
                if isVisible {
                    ControllerFocusBox(
                        cornerRadius: cornerRadius,
                        performanceOptimized: performanceOptimized,
                        animatesArtwork: true,
                        shape: .pill
                    )
                }
            }
    }
}

extension View {
    func controllerFocusDepthAnchor(
        id: String,
        isFocused: Bool
    ) -> some View {
        modifier(
            ControllerFocusDepthAnchorModifier(
                id: id,
                isFocused: isFocused
            )
        )
    }

    func controllerFocusDepthBehindGlass(
        isEnabled: Bool = true,
        force: Bool = false,
        cornerRadius: CGFloat = 12
    ) -> some View {
        modifier(
            ControllerFocusDepthBehindGlassModifier(
                isEnabled: isEnabled,
                force: force,
                cornerRadius: cornerRadius
            )
        )
    }

    func controllerFocusBoxPresentation(
        isVisible: Bool,
        cornerRadius: CGFloat = 12,
        performanceOptimized: Bool = false
    ) -> some View {
        modifier(
            ControllerFocusBoxPresentationModifier(
                isVisible: isVisible,
                cornerRadius: cornerRadius,
                performanceOptimized: performanceOptimized
            )
        )
    }

}
