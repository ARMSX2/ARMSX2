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
    /// The region the card sits in is wider than tall. A leaf beside iPhone Duo's fold is not.
    let isLandscape: Bool
    /// Deterministic dim behind the card. A plain opacity scrim is used instead of a
    /// SwiftUI Material, which collapses to a flat grey over a paused Metal frame
    /// (worst on iPad and under Reduce Transparency). Bumped for Reduce Transparency
    /// users while remaining light enough for the paused game to show through glass.
    let scrimOpacity: Double

    /// `size` is already the safe region, insets removed. Do not take them off again.
    init(size: CGSize, isIPad: Bool, reduceTransparency: Bool, ipadPortraitHeightCap: CGFloat = 640) {
        let isLandscape = size.width > size.height
        self.isLandscape = isLandscape

        if isIPad {
            // iPad. Landscape gets a wider/taller card than portrait so the deck and Per-Game
            // Settings fill more of the wide screen instead of reading as a small centered island.
            variant = .ipadTwoColumn
            let widthMargin: CGFloat = isLandscape ? 96 : 64
            let heightMargin: CGFloat = isLandscape ? 88 : 72
            let widthCap: CGFloat = isLandscape ? 980 : 620
            let heightCap: CGFloat = isLandscape ? 760 : ipadPortraitHeightCap
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
    var dimsBackground = true
    /// Room kept free below the card, for the controller shortcut bar.
    var bottomReserve: CGFloat = 0
    /// Save States lists ten rows, so it may use the whole height of an upright iPad.
    var ipadPortraitHeightCap: CGFloat = 640
    /// Spreads the card over both of iPhone Duo's leaves instead of the leaf after the fold.
    var spreadsAcrossFold = false
    @ViewBuilder let content: (OverlayMetrics) -> Content

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.displayScale) private var displayScale

    var body: some View {
        // Outer reader sees the keyboard, inner one does not. Card is sized off the inner
        // one so it cannot collapse; the difference is how much the keyboard covers.
        GeometryReader { keyboardGeo in
            GeometryReader { geo in
                // Gameplay scene geometry, so rotation and Stage Manager come for free.
                let insets = geo.safeAreaInsets
                let window = CGSize(width: geo.size.width + insets.leading + insets.trailing,
                                    height: geo.size.height + insets.top + insets.bottom)
                let fold = geo.activeFold()
                let leaves = fold.flatMap { foldLeaves($0, in: geo.size) }
                let placed = leaves.map { spreadsAcrossFold ? $0.first.union($0.second) : $0.second }
                // Otherwise the pane: trailing in book pose, below the fold in tabletop.
                let region = placed ?? fold.map { fold in
                    fold.height > fold.width
                        ? CGRect(x: fold.maxX, y: 0, width: max(0, geo.size.width - fold.maxX), height: geo.size.height)
                        : CGRect(x: 0, y: fold.maxY, width: geo.size.width, height: max(0, geo.size.height - fold.maxY))
                } ?? CGRect(origin: .zero, size: geo.size)
                let split = !spreadsAcrossFold ? nil : placed.flatMap { placed in
                    leaves.map { leaves in
                        OverlayFoldSplit(
                            vertical: leaves.vertical,
                            first: leaves.first.offsetBy(dx: -placed.minX, dy: -placed.minY),
                            second: leaves.second.offsetBy(dx: -placed.minX, dy: -placed.minY)
                        )
                    }
                }
                // Goes by window size: a Pro Max reports a regular width class in landscape.
                let large = usesLargeLayout(windowSize: window)
                let metrics = OverlayMetrics(
                    size: region.size,
                    isIPad: large,
                    reduceTransparency: reduceTransparency,
                    ipadPortraitHeightCap: ipadPortraitHeightCap
                )
                let keyboardOverlap = max(0, geo.size.height - keyboardGeo.size.height)
                // The card is centred, so the keyboard covers less of it than of the region.
                let cardMargin = placed.map { geo.size.height - $0.maxY }
                    ?? max(0, (region.height - metrics.cardMaxHeight) / 2)
                // What the bottom margin lacks, measured from the screen edge; the top margin stays.
                let barBase = geo.safeAreaInsets.bottom
                    - GameplayControllerShortcutHelpOverlay.homeIndicatorSink(geo.safeAreaInsets.bottom)
                let lift = bottomReserve > 0 && !(AppState.shared.hingePresent && large)
                    ? max(0, bottomReserve + barBase - cardMargin) : 0

                ZStack {
                    backdrop(metrics: metrics)

                    // Told, not inset here: an inset shrinks the box the panel measures itself in.
                    let hosted = content(metrics)
                        .environment(\.overlayKeyboardOverlap, max(0, keyboardOverlap - cardMargin))
                        .environment(\.overlayFoldSplit, split)

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
                            .frame(maxWidth: placed?.width ?? metrics.cardMaxWidth,
                                   maxHeight: (placed?.height ?? metrics.cardMaxHeight) - lift)
                            // Room for the leaves' shadows; padding would shrink the leaves' proposal.
                            .frame(width: split == nil ? nil : geo.size.width,
                                   height: split == nil ? nil : geo.size.height)
                            .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
                            .shadow(color: .black.opacity(split == nil ? 0.28 : 0), radius: 22, x: 0, y: 12)
                            .modifier(LeafShadowMask(foldEdge: !spreadsAcrossFold && placed != nil
                                ? fold.map { $0.height > $0.width ? .leading : .top } : nil))
                            .offset(y: -lift / 2)
                            // .position rounds the origin differently from the stack's centring, so offset from the centre.
                            .offset(x: region.midX - geo.size.width / 2, y: region.midY - geo.size.height / 2)
                            .transition(reduceMotion ? .opacity : .scale(scale: popScale).combined(with: .opacity))
                    }
                }
            }
            .ignoresSafeArea(.keyboard)
        }
        .transition(.opacity)
    }

    private func foldLeaves(_ fold: CGRect, in size: CGSize) -> OverlayFoldSplit? {
        // The fold's edges can fall mid-pixel; round them outward so no leaf or shadow pixel lands on it.
        func snap(_ v: CGFloat, _ rule: FloatingPointRoundingRule) -> CGFloat { (v * displayScale).rounded(rule) / displayScale }
        let fold = CGRect(x: snap(fold.minX, .down), y: snap(fold.minY, .down),
                          width: snap(fold.maxX, .up) - snap(fold.minX, .down),
                          height: snap(fold.maxY, .up) - snap(fold.minY, .down))
        let width = min(980, size.width - 96)
        guard fold.height > fold.width else {
            let x = (size.width - width) / 2
            let bottom = CGRect(x: x, y: fold.maxY + 16, width: width, height: size.height - 32 - fold.maxY - 16)
            guard width >= 360, bottom.height >= 320, fold.minY >= 144 else { return nil }
            return OverlayFoldSplit(
                vertical: false,
                first: CGRect(x: x, y: fold.minY - 144, width: width, height: 128),
                second: bottom
            )
        }
        let leaf = (width - fold.width - 32) / 2
        let height = min(760, size.height - 88)
        guard leaf >= 360, height >= 320 else { return nil }
        let y = (size.height - height) / 2
        return OverlayFoldSplit(
            vertical: true,
            first: CGRect(x: fold.minX - 16 - leaf, y: y, width: leaf, height: height),
            second: CGRect(x: fold.maxX + 16, y: y, width: leaf, height: height)
        )
    }

    /// Plain opacity scrim, not a Material, which washes out to flat grey over a paused
    /// Metal frame. Near-black and light enough that the game still reads through. Eats
    /// taps, so gameplay gets none while an overlay is up.
    @ViewBuilder
    private func backdrop(metrics: OverlayMetrics) -> some View {
        let scrim = Group {
            if dimsBackground {
                OverlayTheme.scrimBase.opacity(metrics.scrimOpacity)
            } else {
                Color.clear
            }
        }
            .ignoresSafeArea()
            .contentShape(Rectangle())

        if let onTapOutside {
            scrim.onTapGesture { onTapOutside() }
        } else {
            scrim
        }
    }
}

/// A leaf panel's shadow stops at the fold's edge.
struct LeafShadowMask: ViewModifier {
    let foldEdge: Edge?

    func body(content: Content) -> some View {
        // Masked everywhere, since a branch on the hinge would rebuild the panel; off iPhone Duo it clears the whole shadow.
        let free: CGFloat = AppState.shared.hingePresent ? -48 : -64
        func reach(_ edge: Edge) -> CGFloat { edge == foldEdge ? -16 : free }
        return content.mask(Rectangle().padding(EdgeInsets(
            top: reach(.top), leading: reach(.leading), bottom: reach(.bottom), trailing: reach(.trailing)
        )))
    }
}
