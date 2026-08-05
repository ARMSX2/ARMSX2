// GameOverlayContainer.swift — Shared in-game overlay shell
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// How an in-game overlay should arrange its content for the current device class.
/// Replaces a single broad isLandscape/useWideLayout boolean so iPad, iPhone
/// landscape, and iPhone portrait can each get the right layout without one
/// device's needs compromising another's.
enum PauseLayoutVariant {
    /// iPad, any orientation: two side-by-side columns inside one unified scroll
    /// surface.
    case ipadTwoColumn
    /// iPhone landscape: one or two columns according to the panel's measured size.
    /// Covers every iPhone in landscape, including Pro Max / Plus models that report
    /// a regular width size class.
    case phoneLandscape
    /// iPhone portrait: one single column.
    case phonePortrait
}

/// Adaptive sizing values for an in-game overlay card, derived from the available
/// geometry, the device class (iPad vs iPhone), and the host's safe-area insets.
///
/// Device classes:
/// - iPad: a compact floating panel that leaves most gameplay visible.
/// - iPhone landscape: a wide, height-relieved band so the section content and pinned
///   Resume action are not cramped.
/// - iPhone portrait: the liked bounded card.
struct OverlayMetrics {
    /// Maximum card size for the overlay content.
    let cardMaxWidth: CGFloat
    let cardMaxHeight: CGFloat
    /// How the hosted content should arrange itself for the current device class.
    let variant: PauseLayoutVariant
    /// Deterministic dim behind the card. A plain opacity scrim is used instead of a
    /// SwiftUI Material, which collapses to a flat grey over a paused Metal frame
    /// (worst on iPad and under Reduce Transparency). Bumped for Reduce Transparency
    /// users while remaining light enough for the paused game to show through glass.
    let scrimOpacity: Double

    /// `size` is already the safe region, insets removed. Do not take them off again.
    init(size: CGSize, isIPad: Bool, reduceTransparency: Bool) {
        let isLandscape = size.width > size.height

        if isIPad {
            // iPad. Landscape gets a wider/taller card than portrait so the deck and Per-Game
            // Settings fill more of the wide screen instead of reading as a small centered island.
            variant = .ipadTwoColumn
            let widthMargin: CGFloat = isLandscape ? 96 : 64
            let heightMargin: CGFloat = isLandscape ? 88 : 72
            let widthCap: CGFloat = isLandscape ? 980 : 620
            let heightCap: CGFloat = isLandscape ? 760 : 640
            cardMaxWidth = max(0, min(widthCap, size.width - widthMargin))
            cardMaxHeight = max(0, min(heightCap, size.height - heightMargin))
            scrimOpacity = reduceTransparency ? OverlayTheme.scrimPadReduceTransparency : OverlayTheme.scrimPad
        } else if isLandscape {
            // iPhone landscape: a tall floating command panel. Bounds leave a real margin
            // beyond the safe-area insets so the panel clears the notch/Dynamic Island and
            // the virtual pad instead of reading as an edge-to-edge slab. Caps keep wide
            // phones from stretching past a comfortable reading width/height.
            variant = .phoneLandscape
            cardMaxWidth = max(0, min(760, size.width - 40))
            cardMaxHeight = max(0, min(468, size.height - 36))
            scrimOpacity = reduceTransparency ? OverlayTheme.scrimPhoneLandscapeReduceTransparency : OverlayTheme.scrimPhoneLandscape
        } else {
            // iPhone portrait: the liked compact card, slightly roomier.
            variant = .phonePortrait
            cardMaxWidth = max(0, min(480, size.width - 32))
            cardMaxHeight = max(0, min(620, size.height - 32))
            scrimOpacity = reduceTransparency ? OverlayTheme.scrimPhonePortraitReduceTransparency : OverlayTheme.scrimPhonePortrait
        }
    }
}

/// Reusable in-game overlay shell: a deterministic dim behind a bounded, centered
/// card. Owns the backdrop, card sizing, clipping, shadow, safe-area clearance,
/// tap-outside behavior, and orientation-aware metrics so the pause menu and other
/// in-game panels share one coherent presentation instead of one-off inline overlays.
///
/// The host presents this inside an `.overlay { if isPresented { ... } }` and drives an
/// `.animation(_:value:)` on the presenting bool; this view supplies its own transitions.
/// Clipping and shadow are applied here so every hosted card looks identical.
enum OverlayFrameMode {
    /// Centered, bounded card with rounded corners and a drop shadow. Used for
    /// portrait, iPad, and the polished iPhone-landscape panel.
    case bounded
    /// Polished iPhone-landscape panel: keeps the deck's tall, usable shape but
    /// renders as a floating rounded card with margins, corner radius, and shadow
    /// instead of a full-bleed slab.
    case landscapePanel
    /// Full-bleed edge-to-edge deck (no corner clip, no shadow). Retained for
    /// reversibility; not selected by the current overlay hosts.
    case landscapeDeck
}

struct GameOverlayContainer<Content: View>: View {
    var onTapOutside: (() -> Void)? = nil
    var frameMode: OverlayFrameMode = .bounded
    @ViewBuilder let content: (OverlayMetrics) -> Content

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

    // iPad vs iPhone is decided by device idiom, not the horizontal size class: the
    // largest iPhones (Pro Max / Plus) report a regular width size class in landscape,
    // which previously misrouted them onto the bounded iPad card and starved the
    // landscape deck. GeometryReader still re-evaluates on orientation/size change.
    private var isIPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    var body: some View {
        // Outer reader sees the keyboard, inner one does not. Card is sized off the inner
        // one so it cannot collapse; the difference is how much the keyboard covers.
        GeometryReader { keyboardGeo in
            GeometryReader { geo in
                // Gameplay scene geometry, so rotation and Stage Manager come for free.
                let metrics = OverlayMetrics(
                    size: geo.size,
                    isIPad: isIPad,
                    reduceTransparency: reduceTransparency
                )
                let keyboardOverlap = max(0, geo.size.height - keyboardGeo.size.height)
                // The card is centred, so the keyboard covers less of it than of the region.
                let cardMargin = max(0, (geo.size.height - metrics.cardMaxHeight) / 2)

                ZStack {
                    backdrop(metrics: metrics)

                    // Told, not inset here: an inset shrinks the box the panel measures itself in.
                    let hosted = content(metrics)
                        .environment(\.overlayKeyboardOverlap, max(0, keyboardOverlap - cardMargin))

                    if frameMode == .landscapeDeck && metrics.variant == .phoneLandscape {
                        let deckGutter: CGFloat = 8
                        hosted
                            .padding(deckGutter)
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            .transition(reduceMotion ? .opacity : .scale(scale: 0.97).combined(with: .opacity))
                    } else {
                        // One arm, so a flip cannot swap it and take the panel's state along.
                        let popScale: CGFloat = frameMode == .landscapePanel && metrics.variant == .phoneLandscape ? 0.97 : 0.96
                        hosted
                            .frame(maxWidth: metrics.cardMaxWidth, maxHeight: metrics.cardMaxHeight)
                            .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
                            .shadow(color: .black.opacity(0.28), radius: 22, x: 0, y: 12)
                            .transition(reduceMotion ? .opacity : .scale(scale: popScale).combined(with: .opacity))
                    }
                }
            }
            .ignoresSafeArea(.keyboard)
        }
        .transition(.opacity)
    }

    /// Plain opacity scrim, not a Material, which washes out to flat grey over a paused
    /// Metal frame. Near-black and light enough that the game still reads through. Eats
    /// taps, so gameplay gets none while an overlay is up.
    @ViewBuilder
    private func backdrop(metrics: OverlayMetrics) -> some View {
        let scrim = OverlayTheme.scrimBase
            .opacity(metrics.scrimOpacity)
            .ignoresSafeArea()
            .contentShape(Rectangle())

        if let onTapOutside {
            scrim.onTapGesture { onTapOutside() }
        } else {
            scrim
        }
    }
}
