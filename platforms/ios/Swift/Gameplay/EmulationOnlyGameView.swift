// EmulationOnlyGameView.swift — Emulation-Only Mode gameplay and the layout helpers it shares
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

extension View {
    func gameplayLaunchChrome(visible: Bool) -> some View {
        opacity(visible ? 1 : 0)
            .allowsHitTesting(visible)
            .accessibilityHidden(!visible)
            .animation(.easeOut(duration: 0.30), value: visible)
    }
}

/// The portrait game height, and with `refitting` (the Duo) the pad layout fitted to the deck below.
/// The outer screen's game gives up height so the pad keeps `PadPortraitFit.outerMinimumScale`.
@MainActor
func portraitGameSplit(
    _ geo: GeometryProxy,
    deckHeight: CGFloat,
    fold: CGRect?,
    refitting snapshot: PadLayoutSnapshot?,
    analogStickScale: Float
) -> (gameHeight: CGFloat, fitted: PadLayoutSnapshot?, fit: PadPortraitFit?) {
    let barBase = geo.safeAreaInsets.bottom - GameplayControllerShortcutHelpOverlay.homeIndicatorSink(geo.safeAreaInsets.bottom)
    let stickScale = CGFloat(SettingsStore.clampedAnalogStickScale(analogStickScale))
    let envelope = snapshot?.portraitEnvelope(analogStickScale: stickScale)
    let padFloor = envelope.map {
        max(0, deckHeight - ($0.height * PadPortraitFit.outerMinimumScale + PadPortraitFit.topGap + barBase))
    } ?? .infinity
    let gameHeight = fold?.minY ?? min(geo.size.width * 3 / 4, deckHeight * 0.6, padFloor)
    let deck = CGSize(width: geo.size.width, height: deckHeight - gameHeight - (fold?.height ?? 0))
    let fit = envelope.flatMap { PadPortraitFit(envelope: $0, deck: deck, barBase: barBase) }
    let fitted = fit.flatMap { snapshot?.fittedToPortraitDeck($0, size: deck, analogStickScale: stickScale) } ?? snapshot
    return (gameHeight, fitted, fit)
}

/// The Duo's inner screen keeps the landscape pad and menu button inside the safe area.
@MainActor
func landscapePadInsets(_ geo: GeometryProxy, screen: CGSize) -> EdgeInsets {
    AppState.shared.hingePresent && usesLargeLayout(windowSize: screen)
        ? EdgeInsets(top: 0, leading: geo.safeAreaInsets.leading, bottom: 0, trailing: geo.safeAreaInsets.trailing)
        : EdgeInsets()
}

/// Gameplay presentation used after Emulation-Only Mode finishes startup cleanup.
/// With every release switch enabled, this keeps only the existing Metal surface.
struct EmulationOnlyGameView: View {
    @State private var appState = AppState.shared
    @State private var session = GameSession.shared
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = SettingsStore.shared.dynamicThumbsticks
    @State private var touchActionSession = VirtualPadTouchActionSession()

    @ViewBuilder
    var body: some View {
        if session.emulationOnlyPresentation == .minimal {
            PhoneGameSurface()
                .ignoresSafeArea()
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text("Game display"))
                .accessibilityAddTraits(.isImage)
                .persistentSystemOverlays(
                    settings.appearance.hideGameplayStatusBar ? .hidden : .automatic
                )
                .onAppear(perform: preparePresentation)
                .onDisappear(perform: releasePresentation)
        } else {
            retainedGameplayView
                .persistentSystemOverlays(
                    settings.appearance.hideGameplayStatusBar ? .hidden : .automatic
                )
                .onAppear(perform: preparePresentation)
                .onDisappear(perform: releasePresentation)
        }
    }

    private var retainedGameplayView: some View {
        GeometryReader { geometry in
            // Same screen-not-safe-region measurement as the full game screen, same reason.
            let screen = CGSize(
                width: geometry.size.width + geometry.safeAreaInsets.leading + geometry.safeAreaInsets.trailing,
                height: geometry.size.height + geometry.safeAreaInsets.top + geometry.safeAreaInsets.bottom
            )
            let isLandscape = screen.width > screen.height

            Group {
                if session.emulationOnlyPresentation.showsVirtualControls && !isLandscape {
                    VStack(spacing: 0) {
                        let deckHeight = screen.height - geometry.safeAreaInsets.top
                        let fold = geometry.activeFold(book: false)
                        let (gameHeight, fittedSnapshot, fit) = portraitGameSplit(
                            geometry,
                            deckHeight: deckHeight,
                            fold: fold,
                            refitting: appState.hingePresent
                                ? session.emulationOnlyPresentation.padLayoutSnapshot ?? PadLayoutStore.shared.snapshot() : nil,
                            analogStickScale: settings.controller.analogStickScale
                        )
                        accessibleMetalSurface
                            .frame(height: gameHeight)
                            .clipped()
                            .overlay { dynamicCrosshairOverlay }
                        if let fold {
                            Color.black.frame(height: fold.height)
                        }

                        ZStack {
                            Color.black
                            retainedVirtualControls(isLandscape: false, screen: screen, fittedSnapshot: fittedSnapshot, fitScale: fittedSnapshot == nil ? nil : fit?.scale ?? 1)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    .ignoresSafeArea([.container, .keyboard], edges: .bottom)
                    // Same top safe-area strip as the full game screen, same reason.
                    .background(Color.black.ignoresSafeArea())
                } else {
                    let padInsets = landscapePadInsets(geometry, screen: screen)
                    ZStack {
                        accessibleMetalSurface
                        if session.emulationOnlyPresentation.showsVirtualControls {
                            retainedVirtualControls(
                                isLandscape: true,
                                screen: screen,
                                fittedSnapshot: appState.hingePresent && usesLargeLayout(windowSize: screen)
                                    ? (session.emulationOnlyPresentation.padLayoutSnapshot ?? PadLayoutStore.shared.snapshot()).inBook() : nil
                            )
                                .padding(padInsets)
                        }
                        dynamicCrosshairOverlay
                    }
                    .ignoresSafeArea()
                }
            }
        }
    }

    private var accessibleMetalSurface: some View {
        PhoneGameSurface()
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text("Game display"))
            .accessibilityAddTraits(.isImage)
    }

    private func retainedVirtualControls(isLandscape: Bool, screen: CGSize, fittedSnapshot: PadLayoutSnapshot? = nil, fitScale: CGFloat? = nil) -> some View {
        VirtualControllerView(
            isLandscape: isLandscape,
            layoutSnapshot: fittedSnapshot ?? session.emulationOnlyPresentation.padLayoutSnapshot,
            skinDescriptor: session.emulationOnlyPresentation.padSkinDescriptor,
            touchActionSession: touchActionSession,
            largeLayout: usesLargeLayout(windowSize: screen),
            fitScale: fitScale
        )
        .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
    }

    private var dynamicCrosshairOverlay: some View {
        DynamicAimCrosshairOverlay(
            settings: dynamicSettings,
            leftRuntime: touchActionSession.left.crosshairState,
            rightRuntime: touchActionSession.right.crosshairState
        )
        .gameplayLaunchChrome(visible: session.gameplayLaunchControlsVisible)
    }

    private func preparePresentation() {
        appState.hideStatusBar = settings.appearance.hideGameplayStatusBar
        appState.hideHomeIndicator = true
        UIApplication.shared.isIdleTimerDisabled = true

        // GameScreenView's onDisappear can run later in the same SwiftUI update.
        // Reassert the retained presentation state after that teardown completes.
        DispatchQueue.main.async {
            guard session.isEmulationOnlyMode else { return }
            appState.hideStatusBar = settings.appearance.hideGameplayStatusBar
            appState.hideHomeIndicator = true
            UIApplication.shared.isIdleTimerDisabled = true
        }
    }

    private func releasePresentation() {
        if case .menu = appState.currentScreen {
            appState.hideStatusBar = settings.appearance.hideMenuStatusBar
            appState.hideHomeIndicator = false
        }
        UIApplication.shared.isIdleTimerDisabled = false
    }
}
