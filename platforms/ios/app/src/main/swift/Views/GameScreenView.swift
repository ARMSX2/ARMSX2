// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import GameController
import Observation

private let runtimeMenuStateChangedNotification = Notification.Name("ARMSX2iOSRuntimeMenuStateChanged")
private let retroAchievementsToastNotification = Notification.Name("ARMSX2RetroAchievementsNotification")

private extension View {
    func gameplayLaunchChrome(visible: Bool) -> some View {
        opacity(visible ? 1 : 0)
            .allowsHitTesting(visible)
            .accessibilityHidden(!visible)
            .animation(.easeOut(duration: 0.30), value: visible)
    }
}

private enum EmulationOnlyNativeReleaseFlag {
    // Keep these bit positions synchronized with VMManager.h.
    static let patches: UInt = 1 << 0
    static let discordPresence: UInt = 1 << 1
    static let pine: UInt = 1 << 2
    static let achievements: UInt = 1 << 3
    static let inputRecording: UInt = 1 << 4
    static let osd: UInt = 1 << 5
}

private struct RetroAchievementsToast: Equatable {
    let categoryTitle: String
    let title: String
    let message: String
    let badgePath: String?
    let symbolName: String?
    let imageData: Data?
}

/// Isolates rapid Per-Game value updates from `GameScreenView`'s large render tree. The toast is
/// the only view which observes this object, so holding Left/Right no longer invalidates Metal,
/// virtual-pad, Quick Menu, and overlay layout on every repeated percentage step.
@MainActor
@Observable
private final class PerGameLivePreviewToastState {
    private(set) var status: PerGameLivePreviewStatus?

    func present(_ next: PerGameLivePreviewStatus) {
        guard status?.value != next.value else { return }
        withAnimation(.easeOut(duration: 0.14)) {
            status = next
        }
    }

    func clear() {
        guard status != nil else { return }
        withAnimation(.easeIn(duration: 0.14)) {
            status = nil
        }
    }
}

private struct GameScreenStatusToastOverlay: View {
    let livePreview: PerGameLivePreviewToastState?
    @ObservedObject var fallback: TransientBannerController<String>
    let bottomPadding: CGFloat
    let horizontalPadding: CGFloat

    var body: some View {
        if let message = livePreview?.status?.value ?? fallback.content {
            Text(message)
                .font(.callout.weight(.semibold))
                .foregroundStyle(.white)
                // The same Text remains mounted while its number rolls in the direction of the
                // adjustment. This avoids repeatedly transitioning an entire toast in and out.
                .contentTransition(
                    .numericText(
                        countsDown: livePreview?.status?.countsDown ?? false
                    )
                )
                .animation(.easeOut(duration: 0.14), value: message)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(.black.opacity(0.72), in: Capsule())
                .padding(.bottom, bottomPadding)
                .padding(.horizontal, horizontalPadding)
                .transition(.opacity.combined(with: .move(edge: .bottom)))
        }
    }
}

/// Owns all input while the live game image is exposed. The first command is
/// consumed here, rather than being delivered to either gameplay or the
/// invisible Per-Game Settings graph, and requests restoration of the editor.
private struct PerGameLivePreviewExitOverlay: View {
    let controllerInput: MenuControllerInputRouter?
    let stopsWithCircle: Bool
    let onExit: () -> Void

    @Environment(\.uiContentTextColour) private var textColour
    @State private var exitRequested = false

    var body: some View {
        ZStack(alignment: .top) {
            Color.clear
                .contentShape(Rectangle())
                .onTapGesture {
                    requestExit(fromController: false)
                }

            Label(
                stopsWithCircle
                    ? "Tap or press Circle to return"
                    : "Tap or press any button to return",
                systemImage: "gamecontroller.fill"
            )
            .font(.callout.weight(.semibold))
            .foregroundStyle(textColour)
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .glassSurface(
                clear: false,
                forceClear: false,
                cornerRadius: 22
            )
            .padding(.horizontal, 20)
            .padding(.top, 22)
            .allowsHitTesting(false)
        }
        .ignoresSafeArea()
        .onAppear {
            controllerInput?.setNavigationCaptured(
                true,
                owner: MenuControllerNavigationCaptureOwner
                    .perGameLivePreview,
                priority: 1_000
            )
        }
        .onDisappear {
            controllerInput?.setNavigationCaptured(
                false,
                owner: MenuControllerNavigationCaptureOwner
                    .perGameLivePreview,
                priority: 1_000
            )
        }
        .onChange(of: controllerInput?.latestEvent) { _, event in
            guard let event,
                  event.captureOwner
                    == MenuControllerNavigationCaptureOwner
                        .perGameLivePreview else { return }
            if event.command == .back {
                requestExit(fromController: true)
            } else if stopsWithCircle,
                      event.command.horizontalComponent != nil {
                _ = controllerInput?.adjustPerGameLivePreview(event.command)
            } else if !stopsWithCircle {
                requestExit(fromController: true)
            }
        }
    }

    private func requestExit(fromController: Bool) {
        guard !exitRequested else { return }
        exitRequested = true
        if fromController {
            controllerInput?.playFeedback(.back)
        } else {
            MenuAudioPackManager.shared.playEvent(.return)
            controllerInput?.playTouchHaptics(.back)
        }
        onExit()
    }
}

private struct RetroAchievementEntry: Identifiable, Equatable {
    let id: Int
    let title: String
    let description: String
    let badgePath: String?
    let measuredProgress: String
    let points: Int
    let unlockTime: Int
    let state: Int
    let category: Int
    let bucket: Int
    let unlocked: Int
    let measuredPercent: Double
    let rarity: Double
    let rarityHardcore: Double

    init?(dictionary: [String: Any]) {
        guard let idNumber = dictionary["id"] as? NSNumber else { return nil }
        id = idNumber.intValue
        title = dictionary["title"] as? String ?? ""
        description = dictionary["description"] as? String ?? ""
        let rawBadgePath = (dictionary["badgePath"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        badgePath = rawBadgePath.isEmpty ? nil : rawBadgePath
        measuredProgress = dictionary["measuredProgress"] as? String ?? ""
        points = (dictionary["points"] as? NSNumber)?.intValue ?? 0
        unlockTime = (dictionary["unlockTime"] as? NSNumber)?.intValue ?? 0
        state = (dictionary["state"] as? NSNumber)?.intValue ?? 0
        category = (dictionary["category"] as? NSNumber)?.intValue ?? 0
        bucket = (dictionary["bucket"] as? NSNumber)?.intValue ?? 0
        unlocked = (dictionary["unlocked"] as? NSNumber)?.intValue ?? 0
        measuredPercent = (dictionary["measuredPercent"] as? NSNumber)?.doubleValue ?? 0
        rarity = (dictionary["rarity"] as? NSNumber)?.doubleValue ?? 0
        rarityHardcore = (dictionary["rarityHardcore"] as? NSNumber)?.doubleValue ?? 0
    }

    var isUnlocked: Bool { state == 2 }
    var isUnsupported: Bool { state == 3 || bucket == 3 }
    var isUnofficial: Bool { (category & 2) != 0 || bucket == 4 }
    var isActiveChallenge: Bool { bucket == 6 || bucket == 7 }
}

private struct GameScreenSizePreferenceKey: PreferenceKey {
    static let defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        value = nextValue()
    }
}

/// Single source of truth for the in-game overlay stack, replacing the previous set of
/// independent per-screen `@State` booleans.
///
/// - `.hidden`: gameplay; the VM is running and no overlay is up.
/// - `.paused`: the pause-menu card is visible.
/// - `.pausedPresenting`: the pause menu is logically open *underneath* a child screen (a
///   sheet, the pad-layout overlay, the per-game settings overlay, or the reset alert). The
///   pause card itself is not rendered in this state; the child covers the screen.
///
/// Dismissing any pause-launched child returns to `.paused` (never to `.hidden`), so closing
/// Save States / Per-Game Settings / Cheats / RetroAchievements / Pad Layout / Reset ROM lands
/// back on the pause menu instead of resuming gameplay. `Resume`, Back to Menu, Reset ROM and
/// restart-with-disc are the only intentional paths to `.hidden`.
private enum OverlayRoute: Equatable {
    case hidden
    case paused
    case pausedPresenting(QuickMenuDestination)
}

private enum SaveStateShortcutPrompt: Equatable {
    case create(slot: Int)
    case replace(slot: Int)
    case load(slot: Int)
}

private enum SaveStateSlotPickerPurpose: String, Equatable, Identifiable {
    case save
    case load

    var id: String { rawValue }
}

/// Gameplay presentation used after Emulation-Only Mode finishes startup cleanup.
/// With every release switch enabled, this keeps only the existing Metal surface.
struct EmulationOnlyGameView: View {
    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = DynamicThumbstickSettings.shared
    @State private var touchActionSession = VirtualPadTouchActionSession()

    @ViewBuilder
    var body: some View {
        if appState.emulationOnlyPresentation == .minimal {
            PhoneGameSurface()
                .ignoresSafeArea()
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text("Game display"))
                .accessibilityAddTraits(.isImage)
                .persistentSystemOverlays(
                    settings.hideGameplayStatusBar ? .hidden : .automatic
                )
                .onAppear(perform: preparePresentation)
                .onDisappear(perform: releasePresentation)
        } else {
            retainedGameplayView
                .persistentSystemOverlays(
                    settings.hideGameplayStatusBar ? .hidden : .automatic
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
                if appState.emulationOnlyPresentation.showsVirtualControls && !isLandscape {
                    VStack(spacing: 0) {
                        let deckHeight = screen.height - geometry.safeAreaInsets.top
                        let gameHeight = min(geometry.size.width * 3 / 4, deckHeight * 0.6)
                        accessibleMetalSurface
                            .frame(height: gameHeight)
                            .clipped()
                            .overlay { dynamicCrosshairOverlay }

                        ZStack {
                            Color.black
                            retainedVirtualControls(isLandscape: false)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    .ignoresSafeArea([.container, .keyboard], edges: .bottom)
                    // Same top safe-area strip as the full game screen, same reason.
                    .background(Color.black.ignoresSafeArea())
                } else {
                    ZStack {
                        accessibleMetalSurface
                        if appState.emulationOnlyPresentation.showsVirtualControls {
                            retainedVirtualControls(isLandscape: true)
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

    private func retainedVirtualControls(isLandscape: Bool) -> some View {
        VirtualControllerView(
            isLandscape: isLandscape,
            layoutSnapshot: appState.emulationOnlyPresentation.padLayoutSnapshot,
            skinDescriptor: appState.emulationOnlyPresentation.padSkinDescriptor,
            touchActionSession: touchActionSession
        )
        .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
    }

    private var dynamicCrosshairOverlay: some View {
        DynamicAimCrosshairOverlay(
            settings: dynamicSettings,
            leftRuntime: touchActionSession.left.crosshairState,
            rightRuntime: touchActionSession.right.crosshairState
        )
        .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
    }

    private func preparePresentation() {
        appState.hideStatusBar = settings.hideGameplayStatusBar
        appState.hideHomeIndicator = true
        UIApplication.shared.isIdleTimerDisabled = true

        // GameScreenView's onDisappear can run later in the same SwiftUI update.
        // Reassert the retained presentation state after that teardown completes.
        DispatchQueue.main.async {
            guard appState.isEmulationOnlyMode else { return }
            appState.hideStatusBar = settings.hideGameplayStatusBar
            appState.hideHomeIndicator = true
            UIApplication.shared.isIdleTimerDisabled = true
        }
    }

    private func releasePresentation() {
        if case .menu = appState.currentScreen {
            appState.hideStatusBar = settings.hideMenuStatusBar
            appState.hideHomeIndicator = false
        }
        UIApplication.shared.isIdleTimerDisabled = false
    }
}

struct GameScreenView: View {
    // MARK: - State & Constants

    let showsGameplayControllerShortcutHelp: Bool

    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var dynamicSettings = DynamicThumbstickSettings.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var touchActionSession = VirtualPadTouchActionSession()
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var userVirtualPadVisible = true
    @State private var externalControllerConnected = false
    @State private var fullScreen = false
    @State private var menuButtonHidden = false
    @State private var vmMenuAvailable = false
    @State private var gameMenuAvailable = false
    @State private var noJITFallbackActive = false
    // MARK: Overlay Route
    // The pause card + every screen launched from it are driven by one FSM. Opening a child
    // transitions `.paused -> .pausedPresenting(child)` without tearing the card down; the child
    // covers the screen and dismissing it returns to `.paused` (the pause menu), not gameplay.
    @State private var overlayRoute: OverlayRoute = .hidden
    @State private var runtimePerGameSettingsEntry: ISOEntry?
    @State private var runtimePerGameSettings: [String: Any]?
    @State private var runtimePerGameStartsInGameController = false
    @State private var runtimePerGameStartsInShaders = false
    @State private var runtimePerGameLivePreviewPresentation:
        PerGameLivePreviewPresentation = .editing
    @State private var runtimePerGameLivePreviewDismissRequest: UInt64 = 0
    // Per-game editing is an optional presentation resource. Keeping this nil
    // during gameplay avoids retaining its observable state after the panel is
    // dismissed.
    @State private var runtimePerGameLivePreviewToast:
        PerGameLivePreviewToastState?
    @State private var resumeGameplayTask: Task<Void, Never>?
    @State private var quickMenuResourceReleaseTask: Task<Void, Never>?
    @State private var runtimePadLayoutIdentity: PadLayoutGameIdentity?
    @State private var runtimeControllerSkinProposals:
        [AutomaticCustomSkinProposal] = []
    @State private var runtimeControllerSkinSelectionIndex = 0
    @State private var runtimeControllerSkinSetsLayout = false
    @State private var runtimeControllerSkinCatalogTask: Task<Void, Never>?
    // Auto-dismissing banner controllers. Status uses the brief duration as its
    // default (important messages override per-call); achievements use 5s. Both
    // preserve the original easeOut/easeIn 0.18s show/hide and generation-bump
    // cancel semantics (see TransientBannerController).
    @StateObject private var statusBanner = TransientBannerController<String>(defaultDisplayDuration: Self.briefStatusDisplayDuration)
    @StateObject private var achievementsBanner = TransientBannerController<RetroAchievementsToast>(defaultDisplayDuration: Self.retroAchievementsToastDisplayDuration, queuesConcurrentPresentations: true)
    @State private var runtimeOverlayPauseActive = false
    @State private var saveStateShortcutPrompt: SaveStateShortcutPrompt?
    @State private var saveStateShortcutSlotPicker: SaveStateSlotPickerPurpose?
    @State private var saveStateShortcutOperationActive = false
    @State private var runtimeShortcutSpeedPercent: Int?
    @State private var previousHideHomeIndicator = false
    @State private var previousHideStatusBar = false
    @State private var wasBackgrounded = false
    // Bumped whenever the in-game pad editor dismisses, so the gameplay controller is
    // rebuilt from scratch (fresh UIKit press surfaces) instead of diffed. This avoids
    // stale UIControl/hosting-controller state left behind by visibility edits.
    @State private var padRebuildToken = 0
    // A tap on the game view shows the hidden menu button for a moment. The
    // setting itself only changes from the quick menu or settings.
    @State private var menuButtonRevealed = false
    @State private var menuRevealTask: Task<Void, Never>?
    // Only the pause menu is keyed on this. The per-game panel holds unsaved edits.
    @State private var screenIsLandscape = true
    @State private var emulationOnlyTransitionTask: Task<Void, Never>?
    @State private var emulationOnlyActivationInFlight = false
    @State private var pendingEmulationOnlyPresentation: EmulationOnlyPresentation?

    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private static let briefStatusDisplayDuration: TimeInterval = 2.2
    private static let importantStatusDisplayDuration: TimeInterval = 6.0
    private static let retroAchievementsToastDisplayDuration: TimeInterval = 5.0
    private static let replaceLastSaveStateAlwaysKey =
        "ARMSX2iOSReplaceLastSaveStateAlways"
    private static let loadLastSaveStateAlwaysKey =
        "ARMSX2iOSLoadLastSaveStateAlways"

    private var displaySafeAreaInsets: UIEdgeInsets {
        UIApplication.shared.appWindowScene?.windows.first?.safeAreaInsets ?? .zero
    }

    @ViewBuilder
    private var pauseMenuOverlay: some View {
        if case .paused = overlayRoute {
            // The pause card renders only in `.paused`. While a child is up
            // (`.pausedPresenting`) the card is omitted so it cannot z-order over the
            // child overlay; the child covers the screen and the card reappears on dismiss.
            GameOverlayContainer(
                onTapOutside: { overlayRoute = .hidden },
                frameMode: .landscapePanel
            ) { metrics in
                QuickMenuView(
                    settings: settings,
                    padVisible: $userVirtualPadVisible,
                    fullScreen: $fullScreen,
                    menuButtonHidden: $menuButtonHidden,
                    vmMenuAvailable: vmMenuAvailable,
                    gameMenuAvailable: gameMenuAvailable,
                    virtualPadHiddenByController: virtualPadHiddenByController,
                    // Downloading, selecting, and editing presets are frontend
                    // capabilities and must not disappear when an optional
                    // renderer was omitted from a particular build.
                    shaderChainAvailable: true,
                    gameTitle: currentRuntimeGameName(),
                    variant: metrics.variant,
                    activePadLayoutName: activePadLayoutDisplayName,
                    activeControllerSkinName:
                        effectivePadSkinDescriptor.displayName,
                    onCycleOSD: { cycleOsdPreset() },
                    onOpen: { destination in
                        // Transition to `.pausedPresenting` without closing the card; the
                        // child covers the screen and dismissing it returns to `.paused`.
                        openPauseMenuChild(destination)
                    },
                    onClearCache: {
                        overlayRoute = .hidden
                        clearCurrentGameCache()
                    },
                    onBackToMenu: {
                        appState.returnToMenu()
                    },
                    onStop: {
                        MenuAudioPackManager.shared
                            .playStoppingGameOnMainInterface()
                        if settings.hapticFeedback { HapticManager.medium.impactOccurred() }
                        overlayRoute = .hidden
                        // Leave now rather than waiting on the shutdown notification, so nobody
                        // watches live gameplay through the card and NVRAM flush.
                        appState.returnToMenu()
                        appState.stopGame()
                    },
                    onResume: {
                        if settings.hapticFeedback { HapticManager.light.impactOccurred() }
                        resumeGameplayAfterControllerRelease()
                    }
                )
            }
        }
    }

    /// One conditional host owns the complete Quick Menu presentation tree.
    /// When gameplay is visible this resolves to `EmptyView`, so none of the
    /// menu's Liquid Glass, focus overlays, alerts, or child panel layouts are
    /// mounted or receive updates from the running VM.
    @ViewBuilder
    private var runtimeQuickMenuOverlay: some View {
        switch overlayRoute {
        case .hidden:
            EmptyView()
        case .paused:
            // Rebuild the card on rotation so its GeometryReader cannot retain
            // stale landscape metrics.
            pauseMenuOverlay.id(screenIsLandscape)
        case .pausedPresenting(let destination):
            switch destination {
            case .padLayout:
                PadLayoutEditView(
                    onDismiss: { finishRuntimePadLayoutEditing() },
                    context: runtimePadLayoutEditorContext
                )
                .frame(
                    maxWidth: .infinity,
                    maxHeight: .infinity,
                    alignment: .top
                )
            case .perGame, .gameController, .shaders:
                ZStack {
                    // Do not key this panel: rebuilding would discard unsaved
                    // per-game edits.
                    GameOverlayContainer(frameMode: .landscapePanel) { _ in
                        runtimePerGameSettingsContent
                    }
                    .opacity(
                        runtimePerGameLivePreviewPresentation.hidesSettingsUI
                            ? 0
                            : 1
                    )
                    .allowsHitTesting(
                        runtimePerGameLivePreviewPresentation == .editing
                    )
                    .accessibilityHidden(
                        runtimePerGameLivePreviewPresentation.hidesSettingsUI
                    )

                    if runtimePerGameLivePreviewPresentation.hidesSettingsUI {
                        PerGameLivePreviewExitOverlay(
                            controllerInput: controllerInput,
                            stopsWithCircle:
                                settings.perGameLivePreviewStopsWithCircle
                        ) {
                            runtimePerGameLivePreviewDismissRequest &+= 1
                        }
                        .transition(.opacity)
                        .zIndex(20_100)
                    }
                }
            case .controllerSkin:
                runtimeControllerSkinPicker
            case .changeDisc:
                GameOverlayContainer(frameMode: .landscapePanel) { _ in
                    RuntimeDiscSwapPanel(
                        settings: settings,
                        controllerInput: controllerInput,
                        discs: availableDiscSwapNames,
                        onEject: {
                            ejectDisc()
                            overlayRoute = .paused
                        },
                        onInsert: { disc in
                            changeDisc(to: disc)
                            overlayRoute = .paused
                        },
                        onRestart: restartWithDisc,
                        onClose: { overlayRoute = .paused }
                    )
                }
            case .resetROM:
                runtimeResetROMAlert
            case .speed, .saveStates, .cheats,
                 .retroAchievements:
                // These destinations are lazy sheet contents. Their route is
                // retained only so dismissal returns to the pause card.
                EmptyView()
            }
        }
    }

    private var runtimeResetROMAlert: some View {
        ControllerNavigationAlert(
            title: settings.localized("Reset ROM?"),
            message: settings.localized(
                "Restart the current game? Unsaved progress will be lost."
            ),
            actions: [
                .init(
                    id: "cancel",
                    title: settings.localized("Cancel")
                ),
                .init(
                    id: "reset",
                    title: settings.localized("Reset ROM"),
                    isDestructive: true
                ),
            ],
            selectedIndex: 0,
            onSelect: { index in
                if index == 0 {
                    overlayRoute = .paused
                } else {
                    resetCurrentROM()
                }
            },
            onDismiss: {
                overlayRoute = .paused
            }
        )
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "runtime.reset-rom-confirmation",
            priority: 320,
            orbStyle: .plain,
            onBack: {
                overlayRoute = .paused
                return true
            },
            usesExplicitTargetGeometryOnly: true,
            preferredInitialFocusLabel: settings.localized("Cancel")
        )
    }

    // MARK: - Body

    var body: some View {
        GeometryReader { geo in
            // The window. A keyboard shrinks the safe region until iPad portrait reads wide.
            let screen = CGSize(
                width: geo.size.width + geo.safeAreaInsets.leading + geo.safeAreaInsets.trailing,
                height: geo.size.height + geo.safeAreaInsets.top + geo.safeAreaInsets.bottom
            )
            let isLandscape = screen.width > screen.height

            Group {
                if isLandscape {
                    // Landscape: full-screen layout so pad coordinates match the layout editor.
                    ZStack {
                        PhoneGameSurface()
                            .onTapGesture { revealMenuButtonBriefly() }
                            .accessibilityElement(children: .ignore)
                            .accessibilityLabel(Text("Game display"))
                            .accessibilityAddTraits(.isImage)
                            .accessibilityHint(Text("VoiceOver image recognition can read on-screen text."))
                            .overlay { menuRevealTapCatcher }
                        AccessibilityHUDMirror()
                        if effectiveVirtualPadVisible {
                            VirtualControllerView(
                                isLandscape: true,
                                layoutSnapshot: effectivePadLayoutSnapshot,
                                skinDescriptor: effectivePadSkinDescriptor,
                                touchActionSession: touchActionSession
                            )
                            .id(padRebuildToken)
                            .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
                        }
                        dynamicCrosshairOverlay
                        menuButtonOverlay(isLandscape: true)
                            .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
                    }
                    .ignoresSafeArea()
                } else {
                    // Portrait: top game viewport, bottom controller deck.
                    // Game respects the top safe area so OSD stays below the Dynamic Island.
                    // Controller ignores the bottom safe area so buttons remain usable near the home indicator.
                    VStack(spacing: 0) {
                        // The deck ignores the bottom inset, so it runs to the foot of the window.
                        let deckHeight = screen.height - geo.safeAreaInsets.top
                        let gameHeight = min(geo.size.width * 3 / 4, deckHeight * 0.6)
                        PhoneGameSurface()
                            .frame(height: gameHeight)
                            .clipped()
                            .onTapGesture { revealMenuButtonBriefly() }
                            .accessibilityElement(children: .ignore)
                            .accessibilityLabel(Text("Game display"))
                            .accessibilityAddTraits(.isImage)
                            .accessibilityHint(Text("VoiceOver image recognition can read on-screen text."))
                            .overlay {
                                ZStack {
                                    menuRevealTapCatcher
                                    AccessibilityHUDMirror()
                                    dynamicCrosshairOverlay
                                }
                            }

                        if effectiveVirtualPadVisible {
                            ZStack {
                                Color.black
                                VirtualControllerView(
                                    layoutSnapshot: effectivePadLayoutSnapshot,
                                    skinDescriptor: effectivePadSkinDescriptor,
                                    touchActionSession: touchActionSession
                                )
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                                .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
                            }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            .id(padRebuildToken)
                        }
                    }
                    .overlay(alignment: .topTrailing) {
                        if !menuButtonHidden || menuButtonRevealed {
                            menuButtonCluster()
                                .padding(.top, 8)
                                .padding(.trailing, 4)
                                .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
                        }
                    }
                    // `.keyboard` too: gameplay must not move when an overlay raises one.
                    .ignoresSafeArea([.container, .keyboard], edges: .bottom)
                    // The game stays out of the top safe area on purpose, so something has
                    // to fill it. Black rather than leaving it to whatever is behind: the
                    // root controller is only black because a boot notification made it so.
                    .background(Color.black.ignoresSafeArea())
                }
            }
            .preference(key: GameScreenSizePreferenceKey.self, value: screen)
            // Off the safe region, not the preference: the status bar moves one, not the other.
            .onChange(of: geo.size) { _, _ in syncFullscreenStateFromWindow() }
        }
        // A presented gameplay overlay owns controller and accessibility focus.
        // Hide the emulation surface beneath it so window-level native fallback
        // discovery cannot select covered virtual-pad or menu-button controls.
        .accessibilityHidden(
            overlayRoute != .hidden
                || saveStateShortcutPrompt != nil
                || saveStateShortcutSlotPicker != nil
        )
        .onPreferenceChange(GameScreenSizePreferenceKey.self) { size in
            // The window, so only a real rotation reaches this.
            let landscape = size.width > size.height
            if screenIsLandscape != landscape {
                screenIsLandscape = landscape
            }
        }
        .sheet(isPresented: childPresentedBinding(.saveStates)) {
            SaveStatesPanel { message, isImportant in
                presentStatusMessage(
                    message,
                    displayDuration: isImportant ? Self.importantStatusDisplayDuration : Self.briefStatusDisplayDuration
                )
            }
        }
        .sheet(
            item: $saveStateShortcutSlotPicker,
            onDismiss: {
                updateRuntimeOverlayPause()
                updateRuntimeControllerMenuOwnership()
            }
        ) { purpose in
            SaveStatesPanel(mode: purpose) { message, isImportant in
                presentStatusMessage(
                    message,
                    displayDuration: isImportant
                        ? Self.importantStatusDisplayDuration
                        : Self.briefStatusDisplayDuration
                )
            }
        }
        .sheet(isPresented: childPresentedBinding(.speed)) {
            SpeedControlPanel(settings: settings)
                .presentationDetents([.medium, .large])
        }
        .sheet(isPresented: childPresentedBinding(.retroAchievements)) {
            RetroAchievementsGamePanel(settings: settings)
                .presentationDetents([.medium, .large])
        }
        .sheet(isPresented: childPresentedBinding(.cheats)) {
            CheatsPatchesManagerView(
                isoName: ARMSX2Bridge.currentGameISOName() ?? "",
                gameTitle: "",
                launchContext: .inGame,
                controllerInput: controllerInput
            )
            .presentationDetents([.medium, .large])
        }
        .overlay(alignment: .bottom) {
            statusToastOverlay
        }
        .overlay(alignment: .top) {
            retroAchievementsToastOverlay
        }
        .overlay {
            if showsGameplayControllerShortcutHelp {
                GameplayControllerShortcutHelpOverlay(
                    settings: settings,
                    controllerInput: controllerInput
                )
                    .transition(.opacity.combined(with: .scale(scale: 0.96)))
            }
        }
        .overlay {
            runtimeQuickMenuOverlay
        }
        .overlay {
            if let prompt = saveStateShortcutPrompt {
                ControllerNavigationAlert(
                    title: saveStateShortcutPromptTitle(prompt),
                    message: saveStateShortcutPromptMessage(prompt),
                    actions: saveStateShortcutPromptActions(prompt),
                    selectedIndex: saveStateShortcutPromptInitialIndex(prompt),
                    onSelect: { index in
                        handleSaveStateShortcutPromptSelection(
                            prompt,
                            index: index
                        )
                    },
                    onDismiss: cancelSaveStateShortcutPrompt
                )
                .controllerAccessibilityNavigation(
                    controllerInput: controllerInput,
                    scopeKey: "runtime.last-save-state-confirmation",
                    priority: 340,
                    orbStyle: .plain,
                    onBack: {
                        cancelSaveStateShortcutPrompt()
                        return true
                    },
                    usesExplicitTargetGeometryOnly: true,
                    preferredInitialFocusLabel: saveStateShortcutPromptInitialFocusLabel(prompt)
                )
            }
        }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.2), value: overlayRoute)
        .onAppear {
            let nativeEmulationOnlyMode = ARMSX2Bridge.isEmulationOnlyModeActive()
            // Returning to the same stripped VM must not recreate services that
            // Emulation-Only Mode already released.
            if !appState.isEmulationOnlyMode && nativeEmulationOnlyMode {
                restoreEmulationOnlyPresentation()
            } else if !appState.isEmulationOnlyMode {
                FrameTimeDynamicResolutionController.shared.resumeAfterEmulationOnlyMode()
                GameEventHaptics.shared.prepareForGameplaySession()
            }
            enterGameplaySystemChromeMode()
            syncFullscreenStateFromWindow()
            applyInitialFullscreenPreference()
            refreshExternalControllerConnectionState()
            refreshRuntimeMenuState()
            consumePendingRetroAchievementsToast()
            enterEmulationOnlyModeIfReady()
        }
        .onDisappear {
            resumeGameplayTask?.cancel()
            resumeGameplayTask = nil
            releaseAllQuickMenuPresentationResources()
            cancelEmulationOnlyTransition()
            statusBanner.cancelDismiss()
            achievementsBanner.cancelDismiss()
            cancelMenuButtonReveal()
            leaveGameplaySystemChromeMode()
        }
        // Single chokepoint for runtime pause: VM pause derives only from `overlayRoute`
        // (any non-hidden route keeps the VM paused), so one observer covers every child
        // open/dismiss regardless of which screen it was. This replaces the seven per-screen
        // observers that existed for the old independent booleans.
        .onChange(of: overlayRoute) { _, route in
            if route == .paused {
                // Child dismissal and background re-entry can expose the pause
                // card without going through either explicit open action.
                refreshRuntimeMenuState()
            }
            updateRuntimeOverlayPause()
            updateRuntimeControllerMenuOwnership()
            scheduleInactiveQuickMenuResourceRelease(for: route)
        }
        .onChange(of: saveStateShortcutPrompt) { _, _ in
            updateRuntimeOverlayPause()
            updateRuntimeControllerMenuOwnership()
        }
        .onChange(of: saveStateShortcutSlotPicker) { _, _ in
            updateRuntimeOverlayPause()
            updateRuntimeControllerMenuOwnership()
        }
        .onChange(of: saveStateShortcutOperationActive) { _, _ in
            updateRuntimeOverlayPause()
        }
        .onChange(of: controllerInput?.latestQuickPauseRequest) { _, request in
            guard request != nil,
                  overlayRoute == .hidden,
                  saveStateShortcutPrompt == nil,
                  saveStateShortcutSlotPicker == nil,
                  !saveStateShortcutOperationActive else { return }
            if settings.hapticFeedback { HapticManager.medium.impactOccurred() }
            controllerInput?.setMenuActive(true)
            controllerInput?.playFeedback(.tabTransition)
            refreshRuntimeMenuState()
            overlayRoute = .paused
        }
        .onChange(of: controllerInput?.latestEmulationShortcutRequest) { _, request in
            guard let request else { return }
            handleEmulationControllerShortcut(request.shortcut)
        }
        .onChange(of: scenePhase) { _, newPhase in
            if newPhase == .background {
                wasBackgrounded = true
            } else if newPhase == .active {
                syncFullscreenStateFromWindow()
                // Returning to a running game from the background: open the pause menu
                // so resuming is deliberate, not a drop straight back into gameplay.
                if wasBackgrounded && overlayRoute == .hidden {
                    refreshRuntimeMenuState()
                    overlayRoute = .paused
                }
                wasBackgrounded = false
            }
        }
        .onChange(of: fullScreen) { _, isEnabled in
            applyFullscreenState(isEnabled)
        }
        .onChange(of: appState.gameplayLaunchTransition?.id) { previous, current in
            // The gameplay view mounts underneath the live card transition.
            // Keep system chrome stable until that transition has completely
            // faded away, then apply the user's gameplay preference.
            guard previous != nil, current == nil else { return }
            applyFullscreenState(fullScreen)
        }
        .onChange(of: settings.hideGameplayStatusBar) { _, _ in
            updateGameplayStatusBar(forFullscreen: fullScreen)
        }
        .onChange(of: settings.hideMenuButton) { _, isHidden in
            // Keep the runtime menu-button flag in lockstep with the persisted setting so
            // re-enabling it (from the quick menu or settings) restores the button at once.
            if menuButtonHidden != isHidden {
                menuButtonHidden = isHidden
            }
            cancelMenuButtonReveal()
        }
        .onChange(of: settings.emulationOnlyModeEnabled) { _, isEnabled in
            if isEnabled {
                enterEmulationOnlyModeIfReady()
            } else {
                cancelEmulationOnlyTransition()
            }
        }
        .onChange(of: appState.emulationOnlyStartupReady) { _, isReady in
            if isReady {
                enterEmulationOnlyModeIfReady()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
            refreshRuntimeMenuState()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: AppState.emulationOnlyResourcesReleasedNotification
            )
        ) { _ in
            finishEmulationOnlyActivation()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            refreshExternalControllerConnectionState()
            enterEmulationOnlyModeIfReady()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            refreshExternalControllerConnectionState()
        }
        .onReceive(NotificationCenter.default.publisher(for: gameplaySurfaceTapNotification)) { _ in
            revealMenuButtonBriefly()
        }
        .onReceive(NotificationCenter.default.publisher(for: retroAchievementsToastNotification)) { _ in
            consumePendingRetroAchievementsToast()
        }
        .onChange(of: statusBanner.generation) { oldGeneration, newGeneration in
            guard newGeneration > oldGeneration else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .onChange(of: achievementsBanner.generation) { oldGeneration, newGeneration in
            guard newGeneration > oldGeneration else { return }
            MenuAudioPackManager.shared.playEvent(.achievementToast)
        }
        .persistentSystemOverlays(.hidden)
    }

    // MARK: - Layout Views

    @ViewBuilder
    private func menuButtonOverlay(isLandscape: Bool) -> some View {
        if !menuButtonHidden || menuButtonRevealed {
            VStack {
                HStack {
                    Spacer()
                    menuButtonCluster()
                }
                .padding(.top, isLandscape ? 8 : 4)
                .padding(.trailing, isLandscape ? 8 : 4)
                Spacer()
            }
        }
    }

    private func menuButton() -> some View {
        Button {
            controllerInput?.setMenuActive(true)
            MenuAudioPackManager.shared.playEvent(.tabTransition)
            if settings.hapticFeedback { HapticManager.light.impactOccurred() }
            refreshRuntimeMenuState()
            overlayRoute = .paused
        } label: {
            menuButtonLabel
        }
        .accessibilityLabel(settings.localized("Pause Menu"))
        .accessibilityHint(settings.localized("Opens the pause menu"))
    }

    private func menuButtonCluster() -> some View {
        HStack(spacing: 6) {
            if noJITFallbackActive {
                Text(settings.localized("No JIT"))
                    .font(.caption.weight(.semibold))
                    .uiCriticalForegroundStyle()
                    .padding(.horizontal, 9)
                    .padding(.vertical, 6)
                    .background(.black.opacity(0.40), in: Capsule())
                    .accessibilityLabel(settings.localized("No JIT mode"))
            }
            menuButton()
        }
    }

    @MainActor
    private func enterEmulationOnlyModeIfReady() {
        guard settings.emulationOnlyModeEnabled,
              appState.emulationOnlyStartupReady,
              !appState.isEmulationOnlyMode,
              ARMSX2Bridge.isVMRunning(),
              emulationOnlyTransitionTask == nil,
              !emulationOnlyActivationInFlight
        else {
            return
        }

        let delaySeconds = settings.emulationOnlyModeDelaySeconds
        guard delaySeconds > 0 else {
            activateEmulationOnlyModeIfReady()
            return
        }

        emulationOnlyTransitionTask = Task { @MainActor in
            do {
                try await Task.sleep(nanoseconds: UInt64(delaySeconds) * 1_000_000_000)
            } catch {
                return
            }

            guard !Task.isCancelled else { return }
            emulationOnlyTransitionTask = nil
            activateEmulationOnlyModeIfReady()
        }
    }

    @MainActor
    private func activateEmulationOnlyModeIfReady() {
        guard settings.emulationOnlyModeEnabled,
              appState.emulationOnlyStartupReady,
              !appState.isEmulationOnlyMode,
              ARMSX2Bridge.isVMRunning(),
              !emulationOnlyActivationInFlight
        else {
            return
        }

        pendingEmulationOnlyPresentation = makeEmulationOnlyPresentation()
        emulationOnlyActivationInFlight = true
        ARMSX2Bridge.releaseNonEmulationResources(emulationOnlyNativeReleaseFlags)
    }

    @MainActor
    private func restoreEmulationOnlyPresentation() {
        guard ARMSX2Bridge.isVMRunning(),
              ARMSX2Bridge.isEmulationOnlyModeActive(),
              !appState.isEmulationOnlyMode
        else {
            return
        }

        pendingEmulationOnlyPresentation = makeEmulationOnlyPresentation()
        emulationOnlyActivationInFlight = true
        finishEmulationOnlyActivation()
    }

    @MainActor
    private func makeEmulationOnlyPresentation() -> EmulationOnlyPresentation {
        let hasExternalController = !GCController.controllers().isEmpty
        let keepsVirtualControls =
            !hasExternalController ||
            (!settings.emulationOnlyDisableVirtualControls && effectiveVirtualPadVisible)
        let keepsQuickMenu = !settings.emulationOnlyDisableQuickMenu
        return EmulationOnlyPresentation(
            showsVirtualControls: keepsVirtualControls,
            showsQuickMenu: keepsQuickMenu,
            padLayoutSnapshot: keepsVirtualControls ? effectivePadLayoutSnapshot : nil,
            padSkinDescriptor: keepsVirtualControls ? effectivePadSkinDescriptor : nil
        )
    }

    @MainActor
    private func finishEmulationOnlyActivation() {
        guard emulationOnlyActivationInFlight,
              let presentation = pendingEmulationOnlyPresentation
        else {
            return
        }

        emulationOnlyActivationInFlight = false
        pendingEmulationOnlyPresentation = nil
        guard ARMSX2Bridge.isVMRunning(),
              appState.emulationOnlyStartupReady
        else {
            return
        }

        overlayRoute = .hidden
        if presentation.showsQuickMenu {
            menuButtonHidden = false
        }
        statusBanner.cancelDismiss()
        achievementsBanner.cancelDismiss()
        cancelMenuButtonReveal()

        releaseAllQuickMenuPresentationResources()
        runtimePadLayoutIdentity = nil

        if !presentation.showsVirtualControls {
            ARMSX2VirtualPadMaskImageCache.releaseForEmulationOnlyMode()
            HapticManager.releaseForEmulationOnlyMode()
        }
        GameEventHaptics.shared.releaseForEmulationOnlyMode()
        PatchStore.shared.releasePresentationResources()
        if settings.emulationOnlyClearNetworkCache {
            URLCache.shared.removeAllCachedResponses()
        }
        if settings.emulationOnlyDisableFramePacing {
            FrameTimeDynamicResolutionController.shared.suspendForEmulationOnlyMode()
        }

        ARMSX2Bridge.setVMPaused(false)
        appState.enterEmulationOnlyMode(presentation: presentation)
    }

    @MainActor
    private func cancelEmulationOnlyTransition() {
        emulationOnlyTransitionTask?.cancel()
        emulationOnlyTransitionTask = nil
    }

    private var emulationOnlyNativeReleaseFlags: UInt {
        var flags: UInt = 0
        if settings.emulationOnlyDisablePatches { flags |= EmulationOnlyNativeReleaseFlag.patches }
        if settings.emulationOnlyDisableDiscordPresence { flags |= EmulationOnlyNativeReleaseFlag.discordPresence }
        if settings.emulationOnlyDisablePINE { flags |= EmulationOnlyNativeReleaseFlag.pine }
        if settings.emulationOnlyDisableRetroAchievements { flags |= EmulationOnlyNativeReleaseFlag.achievements }
        if settings.emulationOnlyDisableInputRecording { flags |= EmulationOnlyNativeReleaseFlag.inputRecording }
        if settings.emulationOnlyDisableOSD { flags |= EmulationOnlyNativeReleaseFlag.osd }
        return flags
    }

    private var dynamicCrosshairOverlay: some View {
        DynamicAimCrosshairOverlay(
            settings: dynamicSettings,
            leftRuntime: touchActionSession.left.crosshairState,
            rightRuntime: touchActionSession.right.crosshairState
        )
        .gameplayLaunchChrome(visible: appState.gameplayLaunchControlsVisible)
    }

    // The render view is non-interactive on iOS 27, so the reveal tap needs a SwiftUI surface.
    private var menuRevealTapCatcher: some View {
        Color.clear
            .contentShape(Rectangle())
            .onTapGesture { revealMenuButtonBriefly() }
            .accessibilityHidden(true)
    }

    @ViewBuilder
    private var menuButtonLabel: some View {
        // Always-rendered SF Symbol mark. A loose PNG was previously loaded here, but
        // it loaded successfully (so the fallback never ran) while rendering nearly
        // invisible at 30pt over gameplay. Using a template SF Symbol with a strong
        // white foreground guarantees the icon is readable on both dark and bright
        // gameplay regardless of any bundled asset, so the button is never iconless.
        Image(systemName: "pause.circle.fill")
            .font(.system(size: 22, weight: .semibold))
            .symbolRenderingMode(.hierarchical)
            .foregroundStyle(.white)
            .frame(width: 30, height: 30)
            .padding(7)
            .background(.black.opacity(0.40), in: Circle())
    }

    @ViewBuilder
    private var runtimeControllerSkinPicker: some View {
        if let proposal = selectedRuntimeControllerSkinProposal {
            let usesDefaultText = settings.controllerUIThemePreset == .defaultTheme
            let targetOrder = [
                "alert.detail.skin",
                "alert.detail.setCustomLayout",
                "alert.action.apply",
                "alert.action.cancel",
            ]

            ControllerNavigationAlert(
                title: settings.localized("Set New Per-Game Custom Skin?"),
                titleColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
                contentColour: usesDefaultText ? .white : settings.controllerContextMenuColor,
                secondaryContentColour: usesDefaultText ? .white : settings.controllerContextMenuSecondaryColor,
                detailValueColour: usesDefaultText
                    ? settings.controllerNavigationAccentColor
                    : nil,
                previewImageURL: AutomaticCustomSkinManager.shared
                    .previewURL(for: proposal),
                previewSkinDescriptor: VPadSkinLibraryStore.shared
                    .descriptor(id: proposal.skinID),
                previewAccessibilityLabel:
                    "\(settings.localized("Preview")): \(settings.localized(proposal.skinName))",
                details: runtimeControllerSkinDetails(proposal: proposal),
                emphasizesPreview: true,
                usesClearGlass: false,
                usesLargeLandscapePanel: true,
                usesCompactPreviewPanel: true,
                showsActionButtonBackgrounds: true,
                dimsBackground: false,
                message: settings.localized(
                    "Choose the skin and optional linked controller layout for this game."
                ),
                actions: [
                    .init(id: "apply", title: settings.localized("Apply")),
                    .init(
                        id: "cancel",
                        title: settings.localized("Cancel"),
                        activationFeedback: .back
                    ),
                ],
                selectedIndex: 0,
                onSelect: { index in
                    if index == 0 {
                        applyRuntimeControllerSkinSelection()
                    } else {
                        dismissRuntimeControllerSkinPicker(playsSound: false)
                    }
                },
                onDismiss: {
                    dismissRuntimeControllerSkinPicker(playsSound: true)
                }
            )
            .controllerAccessibilityTargetOrder(targetOrder)
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                scopeKey: "runtime.controller-skin",
                priority: 360,
                orbStyle: .plain,
                onBack: {
                    dismissRuntimeControllerSkinPicker(playsSound: true)
                    return true
                },
                confinesHorizontalFocusMovement: true,
                usesExplicitTargetGeometryOnly: true,
                preferredInitialFocusLabel: "alert.detail.skin",
                declaredTargetOrder: targetOrder
            )
            .environment(\.clearLiquidGlassUIEnabled, false)
            .contextMenuPanelTextAppearance()
        }
    }

    private var selectedRuntimeControllerSkinProposal:
        AutomaticCustomSkinProposal? {
        guard runtimeControllerSkinProposals.indices.contains(
            runtimeControllerSkinSelectionIndex
        ) else {
            return nil
        }
        return runtimeControllerSkinProposals[
            runtimeControllerSkinSelectionIndex
        ]
    }

    private func runtimeControllerSkinDetails(
        proposal: AutomaticCustomSkinProposal
    ) -> [ControllerNavigationAlertDetail] {
        [
            .init(
                id: "skin",
                label: settings.localized("Skin"),
                value: settings.localized(proposal.skinName),
                onPrevious: { selectRuntimeControllerSkin(offset: -1) },
                onNext: { selectRuntimeControllerSkin(offset: 1) },
                onActivate: { selectRuntimeControllerSkin(offset: 1) }
            ),
            .init(
                id: "game",
                label: settings.localized("Game"),
                value: currentRuntimeGameName().map {
                    ($0 as NSString).deletingPathExtension
                } ?? settings.localized("Current Game")
            ),
            .init(
                id: "setCustomLayout",
                label: settings.localized("Set custom layout"),
                value: runtimeControllerSkinSetsLayout
                    ? settings.localized("On")
                    : settings.localized("Off"),
                isOn: runtimeControllerSkinSetsLayout,
                onPrevious: { setRuntimeControllerSkinLayout(false) },
                onNext: { setRuntimeControllerSkinLayout(true) },
                onActivate: {
                    setRuntimeControllerSkinLayout(
                        !runtimeControllerSkinSetsLayout
                    )
                }
            ),
        ]
    }

    private func applyControllerSkin(_ skin: VPadSkinDescriptor) {
        if let identity = runtimePadLayoutIdentity {
            layoutPresets.setSkin(
                skin.id,
                for: identity,
                using: skinLibrary
            )
        }

        let serial = PadLayoutGameIdentity.normalizedSerial(
            runtimePadLayoutIdentity?.serial ?? appState.gameplayPadSerial
        )
        if !serial.isEmpty {
            layoutPresets.setAutomaticSkin(
                skin.id,
                forSerial: serial,
                using: skinLibrary
            )
        } else {
            // BIOS and serial-less ELF sessions have no per-game assignment
            // key, so preserve the existing Global Default behaviour.
            skinLibrary.selectSkin(id: skin.id)
            settings.virtualPadSkin = skin.virtualPadSkin
        }

        // UIKit-backed controls cache their artwork and hit masks. Rebuild the
        // pad immediately after the effective descriptor changes.
        padRebuildToken &+= 1
    }

    // MARK: - Runtime Panels

    @ViewBuilder
    private var runtimePerGameSettingsContent: some View {
        if let runtimePerGameSettingsEntry {
            PerGameSettingsPanel(
                game: runtimePerGameSettingsEntry,
                preloadedSettings: runtimePerGameSettings,
                savesToRunningGame: true,
                initiallySelectsGameController:
                    runtimePerGameStartsInGameController,
                initiallySelectsShaders: runtimePerGameStartsInShaders,
                controllerInput: controllerInput,
                livePreviewDismissRequest:
                    runtimePerGameLivePreviewDismissRequest,
                onLivePreviewPresentationChange: { presentation in
                    guard case .pausedPresenting(let destination) = overlayRoute,
                          destination == .perGame
                            || destination == .gameController
                            || destination == .shaders else { return }
                    runtimePerGameLivePreviewPresentation = presentation
                    ARMSX2Bridge.setPerGameLivePreviewAudioMuted(
                        presentation != .editing
                    )
                    if presentation == .editing || presentation == .finishing {
                        runtimePerGameLivePreviewToast?.clear()
                    }
                    updateRuntimeOverlayPause()
                },
                onLivePreviewStatusChange: { status in
                    runtimePerGameLivePreviewToast?.present(status)
                }
            ) {
                closePerGameSettingsOverlay()
            }
        } else {
            NavigationStack {
                ContentUnavailableView(
                    settings.localized("No Game Active"),
                    systemImage: "gamecontroller",
                    description: Text(settings.localized("Start a game before changing per-game settings."))
                )
                .navigationTitle(settings.localized("Per-Game Settings"))
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button(settings.localized("Done")) {
                            closePerGameSettingsOverlay()
                        }
                    }
                }
            }
        }
    }

    // MARK: - Lifecycle & Events

    private func enterGameplaySystemChromeMode() {
        previousHideHomeIndicator = appState.hideHomeIndicator
        previousHideStatusBar = appState.hideStatusBar
        appState.hideHomeIndicator = true
        // Keep the display awake while a game is on screen so it does not sleep mid-play.
        UIApplication.shared.isIdleTimerDisabled = true
    }

    private func leaveGameplaySystemChromeMode() {
        if case .menu = appState.currentScreen {
            appState.hideHomeIndicator = false
            appState.hideStatusBar = settings.hideMenuStatusBar
        } else {
            appState.hideHomeIndicator = previousHideHomeIndicator
            appState.hideStatusBar = previousHideStatusBar
        }
        // Allow the screen to auto-sleep again once gameplay ends.
        UIApplication.shared.isIdleTimerDisabled = false
    }

    /// Single source of truth for applying the runtime fullscreen state. Keeps the
    /// SDL window, the SwiftUI status bar policy, and the local toggle in lockstep so
    /// the quick-menu Full Screen toggle takes effect immediately instead of only on
    /// the next app launch.
    private func applyFullscreenState(_ enabled: Bool) {
        // SDL's fullscreen call can update UIKit's system chrome itself. Delay
        // that call—not only the SwiftUI preference—while the selected card is
        // zooming so the status bar does not vanish under the animation.
        if !enabled || appState.gameplayLaunchTransition == nil {
            ARMSX2Bridge.setFullScreen(enabled)
        }
        updateGameplayStatusBar(forFullscreen: enabled)
    }

    private func applyInitialFullscreenPreference() {
        menuButtonHidden = settings.hideMenuButton
        // Reconcile to the Auto Full Screen preference on every game entry so that
        // changing the setting takes effect on the next boot without an app restart.
        // Previously a stale fullscreen window persisted until the app was relaunched.
        let desired = settings.autoFullscreen
        if fullScreen != desired {
            fullScreen = desired
        }
        applyFullscreenState(desired)
    }

    private func syncFullscreenStateFromWindow() {
        let sdlFullscreen = ARMSX2Bridge.isSDLFullscreen()
        if fullScreen != sdlFullscreen {
            fullScreen = sdlFullscreen
        }
        updateGameplayStatusBar(forFullscreen: sdlFullscreen)
    }

    private func updateGameplayStatusBar(forFullscreen isFullscreen: Bool) {
        let shouldHide = settings.hideGameplayStatusBar
            && isFullscreen
            && appState.gameplayLaunchTransition == nil
        if appState.hideStatusBar != shouldHide {
            appState.hideStatusBar = shouldHide
        }
    }

    private func revealMenuButtonBriefly() {
        guard menuButtonHidden else { return }
        menuRevealTask?.cancel()
        withAnimation(.easeOut(duration: 0.18)) { menuButtonRevealed = true }
        menuRevealTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(4))
            guard !Task.isCancelled else { return }
            withAnimation(.easeIn(duration: 0.18)) { menuButtonRevealed = false }
        }
    }

    private func cancelMenuButtonReveal() {
        menuRevealTask?.cancel()
        menuRevealTask = nil
        menuButtonRevealed = false
    }

    private func updateRuntimeOverlayPause() {
        // Pause derives centrally from the visible overlay or a save-state
        // shortcut transaction. Keeping the operation flag active until its
        // completion prevents gameplay from resuming between confirmation and
        // the CPU-thread save/load work.
        let perGameEditorIsVisible: Bool
        if case .pausedPresenting(let destination) = overlayRoute {
            perGameEditorIsVisible = destination == .perGame
                || destination == .gameController
                || destination == .shaders
        } else {
            perGameEditorIsVisible = false
        }
        let previewRunsGame = perGameEditorIsVisible
            && runtimePerGameLivePreviewPresentation.runsGame
        let shouldPause = (overlayRoute != .hidden && !previewRunsGame)
            || saveStateShortcutPrompt != nil
            || saveStateShortcutSlotPicker != nil
            || saveStateShortcutOperationActive
        guard runtimeOverlayPauseActive != shouldPause else { return }

        runtimeOverlayPauseActive = shouldPause
        if ARMSX2Bridge.isVMRunning() {
            ARMSX2Bridge.setVMPaused(shouldPause)
        }
    }

    /// Quick Menu destinations are mounted conditionally, but their input data
    /// lives on the longer-lived gameplay view. Release that data once the old
    /// destination's removal animation has completed so an invisible editor,
    /// preview model, skin proposal list, or patch catalogue cannot remain in
    /// memory while the VM is running.
    @MainActor
    private func scheduleInactiveQuickMenuResourceRelease(
        for route: OverlayRoute
    ) {
        quickMenuResourceReleaseTask?.cancel()
        quickMenuResourceReleaseTask = Task { @MainActor in
            if reduceMotion {
                await Task.yield()
            } else {
                try? await Task.sleep(for: .milliseconds(220))
            }
            guard !Task.isCancelled, overlayRoute == route else { return }
            releaseInactiveQuickMenuPresentationResources(for: route)
            quickMenuResourceReleaseTask = nil
        }
    }

    @MainActor
    private func releaseInactiveQuickMenuPresentationResources(
        for route: OverlayRoute
    ) {
        let activeDestination: QuickMenuDestination?
        if case .pausedPresenting(let destination) = route {
            activeDestination = destination
        } else {
            activeDestination = nil
        }

        if activeDestination != .perGame
            && activeDestination != .gameController
            && activeDestination != .shaders {
            releasePerGamePresentationResources()
        }
        if activeDestination != .controllerSkin {
            releaseControllerSkinPresentationResources()
        }
        if activeDestination != .cheats {
            PatchStore.shared.releasePresentationResources()
        }
    }

    @MainActor
    private func releasePerGamePresentationResources() {
        ARMSX2Bridge.setPerGameLivePreviewAudioMuted(false)
        runtimePerGameSettingsEntry = nil
        runtimePerGameSettings = nil
        runtimePerGameStartsInGameController = false
        runtimePerGameStartsInShaders = false
        runtimePerGameLivePreviewPresentation = .editing
        runtimePerGameLivePreviewDismissRequest = 0
        runtimePerGameLivePreviewToast?.clear()
        runtimePerGameLivePreviewToast = nil
    }

    @MainActor
    private func releaseControllerSkinPresentationResources() {
        runtimeControllerSkinCatalogTask?.cancel()
        runtimeControllerSkinCatalogTask = nil
        runtimeControllerSkinProposals.removeAll(keepingCapacity: false)
        runtimeControllerSkinSelectionIndex = 0
        runtimeControllerSkinSetsLayout = false
    }

    @MainActor
    private func releaseAllQuickMenuPresentationResources() {
        quickMenuResourceReleaseTask?.cancel()
        quickMenuResourceReleaseTask = nil
        releasePerGamePresentationResources()
        releaseControllerSkinPresentationResources()
        PatchStore.shared.releasePresentationResources()
    }

    @MainActor
    private func resumeGameplayAfterControllerRelease() {
        resumeGameplayTask?.cancel()
        guard controllerInput?.pressedFaceButton == .cross else {
            overlayRoute = .hidden
            return
        }

        // Keep the overlay's input ownership until Cross is physically up.
        // Otherwise the same edge that activates Resume reaches the resumed VM.
        resumeGameplayTask = Task { @MainActor in
            while controllerInput?.pressedFaceButton == .cross {
                try? await Task.sleep(for: .milliseconds(8))
                guard !Task.isCancelled else { return }
            }
            try? await Task.sleep(for: .milliseconds(16))
            guard !Task.isCancelled else { return }
            resumeGameplayTask = nil
            overlayRoute = .hidden
        }
    }

    private func updateRuntimeControllerMenuOwnership() {
        controllerInput?.setMenuActive(
            overlayRoute != .hidden
                || saveStateShortcutPrompt != nil
                || saveStateShortcutSlotPicker != nil
        )
    }

    /// Routes a pause-menu destination to the overlay FSM. `.perGame` needs the VM-safe
    /// settings load, so it goes through `openPerGameSettingsForCurrentGame`; every other
    /// destination simply transitions to `.pausedPresenting` so the card stays logically open
    /// underneath the child and reappears when the child is dismissed.
    private func openPauseMenuChild(_ destination: QuickMenuDestination) {
        // Exhaustive (no `default`): adding a new QuickMenuDestination case without a matching
        // presentation would fail to compile here, so a destination can never silently route to
        // `.pausedPresenting` with no view presenting it.
        switch destination {
        case .perGame:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            openPerGameSettingsForCurrentGame()
        case .gameController:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            openPerGameSettingsForCurrentGame(
                initiallySelectingGameController: true
            )
        case .cheats:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            overlayRoute = .pausedPresenting(destination)
        case .controllerSkin:
            openRuntimeControllerSkinPicker()
        case .shaders:
            MenuAudioPackManager.shared.playEvent(.uiToast)
            openPerGameSettingsForCurrentGame(
                initiallySelectingShaders: true
            )
        case .speed, .saveStates, .retroAchievements, .padLayout, .resetROM,
             .changeDisc:
            overlayRoute = .pausedPresenting(destination)
        }
    }

    private func openRuntimeControllerSkinPicker() {
        let serial = PadLayoutGameIdentity.normalizedSerial(
            runtimePadLayoutIdentity?.serial ?? appState.gameplayPadSerial
        )
        let proposals = AutomaticCustomSkinManager.shared
            .installedProposals(forSerial: serial)
        guard !proposals.isEmpty else {
            presentImportantStatusMessage(
                settings.localized(
                    "Controller skins need a running game with a valid serial."
                )
            )
            return
        }

        runtimeControllerSkinProposals = proposals
        runtimeControllerSkinSelectionIndex = proposals.firstIndex {
            $0.skinID == effectivePadSkinDescriptor.id
        } ?? 0
        runtimeControllerSkinSetsLayout = proposals[
            runtimeControllerSkinSelectionIndex
        ].shouldApplyCustomLayout
        MenuAudioPackManager.shared.playEvent(.uiToast)
        overlayRoute = .pausedPresenting(.controllerSkin)

        runtimeControllerSkinCatalogTask?.cancel()
        runtimeControllerSkinCatalogTask = Task { @MainActor in
            let refreshed = await AutomaticCustomSkinManager.shared
                .downloadAvailableProposals(forSerial: serial)
            guard !Task.isCancelled,
                  overlayRoute == .pausedPresenting(.controllerSkin),
                  !refreshed.isEmpty else { return }
            let selectedSkinID = selectedRuntimeControllerSkinProposal?.skinID
            runtimeControllerSkinProposals = refreshed
            runtimeControllerSkinSelectionIndex = selectedSkinID.flatMap {
                selectedID in
                refreshed.firstIndex { $0.skinID == selectedID }
            } ?? 0
            runtimeControllerSkinCatalogTask = nil
        }
    }

    private func selectRuntimeControllerSkin(offset: Int) {
        guard runtimeControllerSkinProposals.count > 1 else { return }
        let count = runtimeControllerSkinProposals.count
        runtimeControllerSkinSelectionIndex = (
            runtimeControllerSkinSelectionIndex + offset + count
        ) % count
        runtimeControllerSkinSetsLayout = selectedRuntimeControllerSkinProposal?
            .shouldApplyCustomLayout ?? false
    }

    private func setRuntimeControllerSkinLayout(_ enabled: Bool) {
        guard runtimeControllerSkinSetsLayout != enabled else { return }
        runtimeControllerSkinSetsLayout = enabled
    }

    private func applyRuntimeControllerSkinSelection() {
        guard let proposal = selectedRuntimeControllerSkinProposal,
              let descriptor = skinLibrary.descriptor(id: proposal.skinID)
        else {
            presentImportantStatusMessage(
                settings.localized("The selected controller skin is unavailable.")
            )
            return
        }

        if runtimeControllerSkinSetsLayout,
           let layoutPresetID = proposal.layoutPresetID {
            if let identity = runtimePadLayoutIdentity {
                layoutPresets.setPreset(layoutPresetID, for: identity)
            }
            let serial = PadLayoutGameIdentity.normalizedSerial(
                runtimePadLayoutIdentity?.serial ?? appState.gameplayPadSerial
            )
            if !serial.isEmpty {
                layoutPresets.setAutomaticAssignment(
                    skinID: descriptor.id,
                    layoutPresetID: layoutPresetID,
                    forSerial: serial,
                    using: skinLibrary
                )
            }
        }

        applyControllerSkin(descriptor)
        presentStatusMessage(
            "\(settings.localized("Controller Skin")): "
                + settings.localized(descriptor.displayName)
        )
        dismissRuntimeControllerSkinPicker(playsSound: false)
    }

    private func dismissRuntimeControllerSkinPicker(playsSound: Bool) {
        guard overlayRoute == .pausedPresenting(.controllerSkin) else { return }
        if playsSound {
            MenuAudioPackManager.shared.playEvent(.return)
        }
        releaseControllerSkinPresentationResources()
        overlayRoute = .paused
    }

    /// Drives a `.sheet` / `.alert(isPresented:)` from the single overlay route. `get`
    /// presents the child when the route is `.pausedPresenting(child)`; `set(false)` on
    /// dismissal returns to `.paused` — *unless* a teardown path (Reset ROM / restart-with-disc)
    /// has already moved the route to `.hidden`, in which case the dismissal is a no-op so it
    /// does not clobber the intentional return to gameplay.
    private func childPresentedBinding(_ child: QuickMenuDestination) -> Binding<Bool> {
        Binding(
            get: { overlayRoute == .pausedPresenting(child) },
            set: { isPresented in
                guard !isPresented else { return }
                if case .pausedPresenting(let active) = overlayRoute, active == child {
                    overlayRoute = .paused
                }
            }
        )
    }

    private func refreshRuntimeMenuState() {
        let vmRunning = ARMSX2Bridge.isVMRunning()
        let runtimeSettings = vmRunning
            ? ARMSX2Bridge.gameSettingsForCurrentGame()
            : nil
        let identity = runtimePadLayoutIdentity(from: runtimeSettings)
        // Save-state eligibility is intentionally not the owner of game tools
        // or per-game virtual-pad identity. Some titles expose their canonical
        // settings serial before the save-state bridge reports ready.
        let gameReady = identity != nil || ARMSX2Bridge.hasValidSaveStateGame()
        let noJITActive = ARMSX2Bridge.isNoJITFallbackActive()
        if vmMenuAvailable != vmRunning {
            vmMenuAvailable = vmRunning
        }
        if gameMenuAvailable != gameReady {
            gameMenuAvailable = gameReady
        }
        if noJITFallbackActive != noJITActive {
            noJITFallbackActive = noJITActive
        }
        if let identity {
            AutomaticCustomSkinManager.shared.materializeAcceptedAssignment(
                for: identity
            )
        }
        if runtimePadLayoutIdentity != identity {
            runtimePadLayoutIdentity = identity
        }
    }

    private func refreshExternalControllerConnectionState() {
        let connected = !GCController.controllers().isEmpty
        if externalControllerConnected != connected {
            externalControllerConnected = connected
        }
    }

    // MARK: - Game Identity Helpers

    private func currentRuntimeGameName() -> String? {
        if let gameName = normalizedRuntimeGameName(appState.runningGameName) {
            return gameName
        }

        // A BIOS-only session has no game identity. Avoid falling through to the
        // library-matching path, which synchronously opens every local disc image.
        // Once a disc is inserted, gameMenuAvailable becomes true and the normal
        // game-name resolution path resumes.
        if appState.runningGameName == "BIOS" && !gameMenuAvailable {
            return nil
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentGameISOName()) {
            return gameName
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentISOPath()) {
            return gameName
        }

        let bootISO = ARMSX2Bridge.getINIString("GameISO", key: "BootISO", defaultValue: "")
        if let gameName = normalizedRuntimeGameName(bootISO) {
            return gameName
        }

        return gameNameMatchingRuntimeIdentity()
    }

    private func normalizedRuntimeGameName(_ value: String?) -> String? {
        guard var value = value?.trimmingCharacters(in: .whitespacesAndNewlines),
              !value.isEmpty else {
            return nil
        }

        value = value.trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
        let fileName = (value as NSString).lastPathComponent
        guard !fileName.isEmpty,
              fileName != "BIOS",
              fileName != "AutoBoot" else {
            return nil
        }

        return fileName
    }

    private func gameNameMatchingRuntimeIdentity() -> String? {
        let identity = normalizedRuntimeIdentity(ARMSX2Bridge.currentDiscIdentity())
        guard !identity.isEmpty else {
            return nil
        }

        for gameName in ARMSX2Bridge.availableISOs() {
            let metadata = ARMSX2Bridge.gameMetadata(forISO: gameName)
            let serial = normalizedRuntimeIdentity(metadata["serial"])
            if !serial.isEmpty && serial == identity {
                return gameName
            }

            if let crc = metadata["crc"]?.trimmingCharacters(in: .whitespacesAndNewlines).uppercased(),
               !crc.isEmpty,
               (identity == crc || identity == "CRC-\(crc)") {
                return gameName
            }
        }

        return nil
    }

    private func normalizedRuntimeIdentity(_ value: String?) -> String {
        (value ?? "")
            .replacingOccurrences(of: "_", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased()
    }

    // MARK: - Actions

    private func openPerGameSettingsForCurrentGame(
        initiallySelectingGameController: Bool = false,
        initiallySelectingShaders: Bool = false
    ) {
        runtimePerGameStartsInGameController =
            initiallySelectingGameController
        runtimePerGameStartsInShaders = initiallySelectingShaders
        let destination: QuickMenuDestination = initiallySelectingShaders
            ? .shaders
            : (initiallySelectingGameController ? .gameController : .perGame)
        ARMSX2Bridge.setPerGameLivePreviewAudioMuted(false)
        runtimePerGameLivePreviewPresentation = .editing
        if runtimePerGameLivePreviewToast == nil {
            runtimePerGameLivePreviewToast = PerGameLivePreviewToastState()
        } else {
            runtimePerGameLivePreviewToast?.clear()
        }
        // Use the VM-safe bridge path to avoid a disc-image scan while the game is running.
        guard let gameName = currentRuntimeGameName(),
              let info = ARMSX2Bridge.gameSettingsForCurrentGame() else {
            runtimePerGameSettingsEntry = nil
            runtimePerGameSettings = nil
            withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
                overlayRoute = .pausedPresenting(destination)
            }
            presentImportantStatusMessage(settings.localized("Per-game settings need a running game."))
            return
        }

        let serial = (info["serial"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        runtimePerGameSettingsEntry = ISOEntry(
            name: gameName,
            fileURL: nil,
            bootPath: nil,
            coverURL: nil,
            coverSignature: nil,
            metadata: ["serial": serial, "crc": (info["crc"] as? String) ?? ""].filter { !$0.value.isEmpty },
            size: 0,
            isFavorite: false
        )
        runtimePerGameSettings = info
        withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
            overlayRoute = .pausedPresenting(destination)
        }
    }

    private func closePerGameSettingsOverlay() {
        ARMSX2Bridge.setPerGameLivePreviewAudioMuted(false)
        // Save/Cancel from Per-Game Settings return to the pause menu, not gameplay.
        withAnimation(.easeInOut(duration: 0.2)) {
            overlayRoute = .paused
        }
        // Payload cleanup is deferred until the removal animation completes;
        // clearing it here would replace the outgoing editor with "No Game
        // Active" during the fade.
        runtimePerGameLivePreviewToast?.clear()
    }

    /// The editor owns exactly one dismissal path. Its former `onDisappear`
    /// notification could arrive after Resume hid the pause UI and set the
    /// route back to `.paused`, leaving an invisible overlay intercepting every
    /// touch. Reset input before rebuilding the controller surface.
    private func finishRuntimePadLayoutEditing() {
        guard overlayRoute == .pausedPresenting(.padLayout) else { return }
        touchActionSession.reset()
        EmulatorBridge.shared.resetVirtualPadAnalogInput()
        padRebuildToken &+= 1
        overlayRoute = .paused
    }

    private func resetCurrentROM() {
        // Drop out of the overlay before tearing the VM down so the pause state cannot
        // re-pause a resetting VM.
        overlayRoute = .hidden
        appState.resetCurrentVM()
        presentStatusMessage(settings.localized("Restarting ROM..."))
    }

    private func clearCurrentGameCache() {
        guard let gameName = currentRuntimeGameName() else {
            presentImportantStatusMessage(settings.localized("Cache clear needs a running game."))
            return
        }

        let message = ARMSX2Bridge.clearCache(forISO: gameName)
        presentStatusMessage(message)
    }

    private func changeDisc(to discName: String) {
        presentStatusMessage("Changing disc...")
        ARMSX2Bridge.changeDisc(toISO: discName) { success in
            Task { @MainActor in
                if success {
                    presentStatusMessage("\(discName) inserted. Use the game's disc-swap prompt if needed.")
                } else {
                    presentImportantStatusMessage("Could not change discs. Open the game's disc-swap prompt first, or restart with the target disc.")
                }
            }
        }
    }

    private func restartWithDisc(_ discName: String) {
        // Drop out of the overlay before the VM shutdown/boot so the pause state cannot
        // fight the teardown.
        overlayRoute = .hidden
        presentStatusMessage("Restarting with \(discName)...")
        appState.shutdownAndBoot(isoName: discName)
    }

    private func ejectDisc() {
        presentStatusMessage("Ejecting disc...")
        ARMSX2Bridge.ejectDisc { success in
            Task { @MainActor in
                if success {
                    presentStatusMessage("Disc ejected")
                } else {
                    presentImportantStatusMessage("Could not eject the disc. Try again after the game has finished loading.")
                }
            }
        }
    }

    // MARK: - Toast & Feedback

    @ViewBuilder
    private var statusToastOverlay: some View {
        GameScreenStatusToastOverlay(
            livePreview: runtimePerGameLivePreviewToast,
            fallback: statusBanner,
            bottomPadding: 24,
            horizontalPadding: max(
                max(
                    displaySafeAreaInsets.left,
                    displaySafeAreaInsets.right
                ),
                14
            )
        )
    }

    @ViewBuilder
    private var retroAchievementsToastOverlay: some View {
        if let retroAchievementsToast = achievementsBanner.content {
            HStack(spacing: 12) {
                retroAchievementsBadge(
                    path: retroAchievementsToast.badgePath,
                    imageData: retroAchievementsToast.imageData,
                    fallbackSystemImage: retroAchievementsToast.symbolName ?? "trophy.fill"
                )

                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 4) {
                        if let symbolName = retroAchievementsToast.symbolName {
                            Image(systemName: symbolName)
                        }
                        Text(retroAchievementsToast.categoryTitle)
                    }
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.yellow)
                    Text(retroAchievementsToast.title)
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(.white)
                        .lineLimit(1)
                    if !retroAchievementsToast.message.isEmpty {
                        Text(retroAchievementsToast.message)
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.82))
                            .lineLimit(2)
                    }
                }

                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(maxWidth: 390)
            .background(.black.opacity(0.78), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(.white.opacity(0.12), lineWidth: 1)
            }
            .padding(.top, max(displaySafeAreaInsets.top, 54))
            .padding(.horizontal, max(max(displaySafeAreaInsets.left, displaySafeAreaInsets.right), 14))
            .allowsHitTesting(false)
            .transition(.opacity.combined(with: .move(edge: .top)))
        }
    }

    @ViewBuilder
    private func retroAchievementsBadge(
        path: String?,
        imageData: Data? = nil,
        fallbackSystemImage: String = "trophy.fill"
    ) -> some View {
        if let imageData,
           let image = UIImage(data: imageData) {
            Image(uiImage: image)
                .resizable()
                .scaledToFill()
                .frame(width: 58, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        } else if let path,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 42, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        } else {
            Image(systemName: fallbackSystemImage)
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(.yellow)
                .frame(width: 42, height: 42)
                .background(.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
    }

    private func presentStatusMessage(
        _ message: String,
        displayDuration: TimeInterval = Self.briefStatusDisplayDuration
    ) {
        statusBanner.present(message, displayDuration: displayDuration)
    }

    private func cycleOsdPreset() {
        let allPresets: [OsdPreset] = OsdPreset.allCases
        guard let currentIndex = allPresets.firstIndex(of: settings.osdPreset) else {
            return
        }
        let nextIndex = (currentIndex + 1) % allPresets.count
        let nextPreset = allPresets[nextIndex]

        settings.osdPreset = nextPreset
        ARMSX2Bridge.setPerformanceOverlayVisible(nextPreset != .off)

        let label = nextPreset != .off
            ? settings.localized(nextPreset.label)
            : settings.localized("OFF")
        presentStatusMessage("OSD: \(label)")
    }

    private func presentRetroAchievementsToast(
        title rawTitle: String,
        message rawMessage: String,
        badgePath rawBadgePath: String,
        duration: TimeInterval?,
        categoryTitle: String = "RetroAchievements",
        symbolName: String? = nil,
        imageData: Data? = nil
    ) {
        let title = rawTitle.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty else { return }

        let message = rawMessage.trimmingCharacters(in: .whitespacesAndNewlines)
        let badgePathValue = rawBadgePath.trimmingCharacters(in: .whitespacesAndNewlines)
        let toast = RetroAchievementsToast(
            categoryTitle: categoryTitle,
            title: title,
            message: message,
            badgePath: badgePathValue.isEmpty ? nil : badgePathValue,
            symbolName: symbolName,
            imageData: imageData
        )

        achievementsBanner.present(toast, displayDuration: duration)
    }

    private func consumePendingRetroAchievementsToast() {
        guard let pending = ARMSX2Bridge.consumePendingRetroAchievementsNotification() else { return }
        presentRetroAchievementsToast(
            title: pending.title,
            message: pending.message,
            badgePath: pending.badgePath,
            duration: pending.duration > 0 ? pending.duration : nil
        )
    }

    private func presentImportantStatusMessage(_ message: String) {
        presentStatusMessage(message, displayDuration: Self.importantStatusDisplayDuration)
    }

    // MARK: - Gameplay Controller Shortcuts

    private func handleEmulationControllerShortcut(
        _ shortcut: EmulationControllerShortcut
    ) {
        guard overlayRoute == .hidden,
              saveStateShortcutPrompt == nil,
              saveStateShortcutSlotPicker == nil,
              !saveStateShortcutOperationActive,
              ARMSX2Bridge.isVMRunning() else { return }

        switch shortcut {
        case .saveLastState:
            requestSaveToLastState()
        case .loadLastState:
            requestLoadFromLastState()
        case .increaseSpeed:
            if let percent = adjustActiveEmulationSpeed(byPercent: 25) {
                presentStatusMessage(
                    "\(settings.localized("Emulation Speed")): \(percent)%"
                )
            }
        case .decreaseSpeed:
            if let percent = adjustActiveEmulationSpeed(byPercent: -25) {
                presentStatusMessage(
                    "\(settings.localized("Emulation Speed")): \(percent)%"
                )
            }
        case .enableFastForward:
            settings.fastForwardScalar = 10.0
            settings.setRuntimeFastForwardEnabled(true)
            presentStatusMessage(settings.localized("Fast Forward: ON (1000%)"))
        case .disableFastForward:
            runtimeShortcutSpeedPercent = 100
            settings.setRuntimeFastForwardEnabled(false)
            presentStatusMessage(settings.localized("Fast Forward: OFF (100%)"))
        }
    }

    @discardableResult
    private func adjustActiveEmulationSpeed(byPercent delta: Int) -> Int? {
        if ARMSX2Bridge.limiterMode() == 1 {
            let currentPercent = Int((settings.fastForwardScalar * 100).rounded())
            let nextPercent = min(
                max(currentPercent + delta, 125),
                1_000
            )
            guard nextPercent != currentPercent else { return nil }
            settings.fastForwardScalar = Float(nextPercent) / 100.0
            settings.setRuntimeFastForwardEnabled(true)
            return nextPercent
        }

        let storedPercent = Int((ARMSX2Bridge.getINIFloat(
            "Framerate",
            key: "NominalScalar",
            defaultValue: 1.0
        ) * 100).rounded())
        let currentPercent = runtimeShortcutSpeedPercent ?? storedPercent
        let minimumPercent = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
            ? 100
            : 25
        let nextPercent = min(
            max(currentPercent + delta, minimumPercent),
            1_000
        )
        guard nextPercent != currentPercent else { return nil }
        runtimeShortcutSpeedPercent = nextPercent
        ARMSX2Bridge.setRuntimeEmulationSpeedPercent(Int32(nextPercent))
        return nextPercent
    }

    private func requestSaveToLastState() {
        let slots = ARMSX2Bridge.saveStateSlots()
        guard let firstSlot = slots.first else {
            presentRetroAchievementsToast(
                title: settings.localized("Save states are not ready yet."),
                message: settings.localized(
                    "Wait until the game has fully identified, then try again."
                ),
                badgePath: "",
                duration: nil
            )
            return
        }

        if let latest = latestOccupiedSaveState(in: slots) {
            if UserDefaults.standard.bool(
                forKey: Self.replaceLastSaveStateAlwaysKey
            ) {
                performSaveStateShortcut(slot: latest.slot, replacing: true)
            } else {
                presentSaveStateShortcutPrompt(.replace(slot: latest.slot))
            }
        } else {
            presentSaveStateShortcutPrompt(.create(slot: firstSlot.slot))
        }
    }

    private func requestLoadFromLastState() {
        guard !ARMSX2Bridge.isRetroAchievementsHardcoreActive() else {
            presentRetroAchievementsToast(
                title: settings.localized("Save State Unavailable"),
                message: settings.localized(
                    "Hardcore mode blocks loading save states."
                ),
                badgePath: "",
                duration: nil
            )
            return
        }

        let slots = ARMSX2Bridge.saveStateSlots()
        guard let latest = latestOccupiedSaveState(in: slots) else {
            presentRetroAchievementsToast(
                title: settings.localized("No saved states found."),
                message: settings.localized("Create one by pressing")
                    + " \(settings.controllerMacroSaveGameState.title).",
                badgePath: "",
                duration: nil
            )
            return
        }

        if UserDefaults.standard.bool(forKey: Self.loadLastSaveStateAlwaysKey) {
            performLoadStateShortcut(slot: latest.slot)
        } else {
            presentSaveStateShortcutPrompt(.load(slot: latest.slot))
        }
    }

    private func latestOccupiedSaveState(
        in slots: [ARMSX2SaveStateSlotInfo]
    ) -> ARMSX2SaveStateSlotInfo? {
        slots.filter(\.occupied).max { lhs, rhs in
            let leftDate = lhs.modifiedDate ?? .distantPast
            let rightDate = rhs.modifiedDate ?? .distantPast
            if leftDate == rightDate { return lhs.slot < rhs.slot }
            return leftDate < rightDate
        }
    }

    private func presentSaveStateShortcutPrompt(
        _ prompt: SaveStateShortcutPrompt
    ) {
        saveStateShortcutPrompt = prompt
        updateRuntimeOverlayPause()
        updateRuntimeControllerMenuOwnership()
    }

    private func cancelSaveStateShortcutPrompt() {
        saveStateShortcutPrompt = nil
        updateRuntimeOverlayPause()
        updateRuntimeControllerMenuOwnership()
    }

    private func saveStateShortcutPromptTitle(
        _ prompt: SaveStateShortcutPrompt
    ) -> String {
        switch prompt {
        case .create:
            settings.localized("Create a Saved State?")
        case .replace:
            settings.localized("Are you sure to replace last Saved State?")
        case .load:
            settings.localized("Load Last Saved State?")
        }
    }

    private func saveStateShortcutPromptMessage(
        _ prompt: SaveStateShortcutPrompt
    ) -> String {
        switch prompt {
        case .create(let slot):
            "\(settings.localized("No saved states exist. Create one in slot")) \(slot)?"
        case .replace(let slot):
            "\(settings.localized("This replaces the saved state in slot")) \(slot)."
        case .load(let slot):
            "\(settings.localized("Load the most recently saved state from slot")) \(slot)?"
        }
    }

    private func saveStateShortcutPromptActions(
        _ prompt: SaveStateShortcutPrompt
    ) -> [ControllerNavigationAlertAction] {
        switch prompt {
        case .create:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "create", title: settings.localized("Create")),
                .init(
                    id: "save-another",
                    title: settings.localized("Save Another")
                ),
            ]
        case .replace:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "replace", title: settings.localized("Replace")),
                .init(
                    id: "save-another",
                    title: settings.localized("Save Another")
                ),
                .init(
                    id: "replace-always",
                    title: settings.localized("Replace Always"),
                    isDestructive: true
                ),
            ]
        case .load:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(id: "load", title: settings.localized("Load")),
                .init(
                    id: "load-another",
                    title: settings.localized("Load Another")
                ),
                .init(
                    id: "load-always",
                    title: settings.localized("Load Always"),
                    isDestructive: true
                ),
            ]
        }
    }

    private func saveStateShortcutPromptInitialIndex(
        _ prompt: SaveStateShortcutPrompt
    ) -> Int {
        switch prompt {
        case .create, .load:
            1
        case .replace:
            0
        }
    }

    private func saveStateShortcutPromptInitialFocusLabel(
        _ prompt: SaveStateShortcutPrompt
    ) -> String {
        switch prompt {
        case .create:
            settings.localized("Create")
        case .load:
            settings.localized("Load")
        case .replace:
            settings.localized("Cancel")
        }
    }

    private func handleSaveStateShortcutPromptSelection(
        _ prompt: SaveStateShortcutPrompt,
        index: Int
    ) {
        guard index > 0 else {
            cancelSaveStateShortcutPrompt()
            return
        }

        switch prompt {
        case .create(let slot):
            if index == 1 {
                performSaveStateShortcut(slot: slot, replacing: false)
            } else if index == 2 {
                presentSaveStateSlotPicker(.save)
            }
        case .replace(let slot):
            if index == 2 {
                presentSaveStateSlotPicker(.save)
                return
            }
            if index == 3 {
                UserDefaults.standard.set(
                    true,
                    forKey: Self.replaceLastSaveStateAlwaysKey
                )
            }
            if index == 1 || index == 3 {
                performSaveStateShortcut(slot: slot, replacing: true)
            }
        case .load(let slot):
            if index == 2 {
                presentSaveStateSlotPicker(.load)
                return
            }
            if index == 3 {
                UserDefaults.standard.set(
                    true,
                    forKey: Self.loadLastSaveStateAlwaysKey
                )
            }
            if index == 1 || index == 3 {
                performLoadStateShortcut(slot: slot)
            }
        }
    }

    private func presentSaveStateSlotPicker(
        _ purpose: SaveStateSlotPickerPurpose
    ) {
        saveStateShortcutPrompt = nil
        saveStateShortcutSlotPicker = purpose
        updateRuntimeOverlayPause()
        updateRuntimeControllerMenuOwnership()
    }

    private func beginSaveStateShortcutOperation() {
        saveStateShortcutOperationActive = true
        saveStateShortcutPrompt = nil
        updateRuntimeOverlayPause()
        updateRuntimeControllerMenuOwnership()
    }

    private func finishSaveStateShortcutOperation() {
        saveStateShortcutOperationActive = false
        updateRuntimeOverlayPause()
    }

    private func performSaveStateShortcut(slot: Int, replacing: Bool) {
        beginSaveStateShortcutOperation()
        ARMSX2Bridge.saveState(toSlot: slot) { success in
            Task { @MainActor in
                finishSaveStateShortcutOperation()
                if success {
                    let previewData = saveStatePreviewPNGData(for: slot)
                    presentRetroAchievementsToast(
                        title: settings.localized(
                            replacing ? "Saved State Replaced" : "Saved State Created"
                        ),
                        message: "\(settings.localized("Saved to slot")) \(slot).",
                        badgePath: "",
                        duration: nil,
                        categoryTitle: settings.localized("Save Game State"),
                        symbolName: "square.stack.3d.up.fill",
                        imageData: previewData
                    )
                } else {
                    presentImportantStatusMessage(
                        "\(settings.localized("Could not save slot")) \(slot). \(settings.localized("Try again after gameplay has fully loaded."))"
                    )
                }
            }
        }
    }

    private func performLoadStateShortcut(slot: Int) {
        beginSaveStateShortcutOperation()
        ARMSX2Bridge.loadState(fromSlot: slot) { success in
            Task { @MainActor in
                finishSaveStateShortcutOperation()
                if success {
                    let previewData = saveStatePreviewPNGData(for: slot)
                    // The completion toast remains visible for the persistent
                    // Load Always path, so an automatic load is never silent.
                    presentRetroAchievementsToast(
                        title: settings.localized("Saved State Loaded"),
                        message: "\(settings.localized("Loaded from slot")) \(slot).",
                        badgePath: "",
                        duration: nil,
                        categoryTitle: settings.localized("Load Game State"),
                        symbolName: "square.stack.3d.up.fill",
                        imageData: previewData
                    )
                } else {
                    presentImportantStatusMessage(
                        "\(settings.localized("Could not load slot")) \(slot). \(settings.localized("Make sure it has a saved state first."))"
                    )
                }
            }
        }
    }

    private func saveStatePreviewPNGData(for slot: Int) -> Data? {
        ARMSX2Bridge.saveStateSlots()
            .first(where: { $0.slot == slot })?
            .previewPNGData
    }

    // MARK: - Virtual Pad

    private var effectiveVirtualPadVisible: Bool {
        if appState.isEmulationOnlyMode {
            return appState.emulationOnlyPresentation.showsVirtualControls
        }
        return userVirtualPadVisible &&
            (!settings.autoHideVirtualPadWhenControllerConnected || !externalControllerConnected) &&
            overlayRoute != .pausedPresenting(.padLayout)
    }

    private var effectivePadLayoutSnapshot: PadLayoutSnapshot? {
        layoutPresets.effectiveSnapshot(
            for: runtimePadLayoutIdentity ?? appState.gameplayPadIdentity,
            fallbackSerial: appState.gameplayPadSerial
        )
    }

    /// Friendly name for the virtual pad layout currently in effect, shown beside the
    /// Edit Virtual Pad Layout row so the active value is visible at a glance.
    private var activePadLayoutDisplayName: String {
        if let preset = layoutPresets.effectivePreset(
            for: runtimePadLayoutIdentity ?? appState.gameplayPadIdentity,
            fallbackSerial: appState.gameplayPadSerial
        ) {
            return preset.displayName
        }
        return settings.localized("Current Layout")
    }

    private var effectivePadSkinDescriptor: VPadSkinDescriptor {
        layoutPresets.effectiveSkinDescriptor(
            for: runtimePadLayoutIdentity ?? appState.gameplayPadIdentity,
            fallbackSerial: appState.gameplayPadSerial,
            using: skinLibrary
        )
    }

    private var runtimePadLayoutEditorContext: PadLayoutEditorContext {
        let effectiveIdentity = runtimePadLayoutIdentity
            ?? appState.gameplayPadIdentity
        let preset = layoutPresets.effectivePreset(
            for: effectiveIdentity,
            fallbackSerial: appState.gameplayPadSerial
        )
        let editablePresetID = effectiveIdentity.flatMap {
            layoutPresets.presetID(for: $0)
        } ?? (effectiveIdentity == nil ? layoutPresets.globalPresetID : nil)
        return PadLayoutEditorContext(
            presetID: editablePresetID,
            gameIdentity: effectiveIdentity,
            initialSnapshot: preset?.snapshot,
            skinDescriptor: effectivePadSkinDescriptor
        )
    }

    private func runtimePadLayoutIdentityForCurrentGame() -> PadLayoutGameIdentity? {
        guard let info = ARMSX2Bridge.perGameIdentityForCurrentGame() else {
            return nil
        }
        return runtimePadLayoutIdentity(from: info)
    }

    private func runtimePadLayoutIdentity(
        from info: [String: Any]?
    ) -> PadLayoutGameIdentity? {
        guard let info else { return nil }
        return PadLayoutGameIdentity(
            serial: info["serial"] as? String,
            crc: info["crc"] as? String
        )
    }

    private var virtualPadHiddenByController: Bool {
        userVirtualPadVisible && settings.autoHideVirtualPadWhenControllerConnected && externalControllerConnected
    }

    // MARK: - Disc Helpers

    private var availableDiscSwapNames: [String] {
        ARMSX2Bridge.availableISOs().filter { !$0.lowercased().hasSuffix(".elf") }
    }
}

/// Controller-owned replacement for the nested native Change Disc `Menu`.
/// Native menus can take UIKit focus ownership and leave the emulation overlay
/// without a GameController handler after dismissal. This list keeps every
/// operation inside the same explicit navigation session.
private struct RuntimeDiscSwapPanel: View {
    let settings: SettingsStore
    let controllerInput: MenuControllerInputRouter?
    let discs: [String]
    let onEject: () -> Void
    let onInsert: (String) -> Void
    let onRestart: (String) -> Void
    let onClose: () -> Void

    private var controllerTargetOrder: [String] {
        ["runtime.disc.close", "runtime.disc.eject"]
            + discs.indices.map { "runtime.disc.insert.\($0)" }
            + discs.indices.map { "runtime.disc.restart.\($0)" }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button(action: onEject) {
                        Label(
                            settings.localized("Eject Disc"),
                            systemImage: "eject"
                        )
                    }
                    .controllerAccessibilityActionTarget(
                        id: "runtime.disc.eject",
                        label: settings.localized("Eject Disc"),
                        action: onEject
                    )
                    .gameCardTintMenuBackgroundListRow(true)
                }

                if discs.isEmpty {
                    Section {
                        Text(settings.localized("No disc images found"))
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Section(settings.localized("Insert Disc (No Reboot)")) {
                        ForEach(Array(discs.enumerated()), id: \.offset) { index, disc in
                            Button {
                                onInsert(disc)
                            } label: {
                                Label(disc, systemImage: "opticaldisc")
                            }
                            .controllerAccessibilityActionTarget(
                                id: "runtime.disc.insert.\(index)",
                                label: disc
                            ) {
                                onInsert(disc)
                            }
                            .gameCardTintMenuBackgroundListRow(true)
                        }
                    }

                    Section(settings.localized("Restart With Disc")) {
                        ForEach(Array(discs.enumerated()), id: \.offset) { index, disc in
                            Button {
                                onRestart(disc)
                            } label: {
                                Label(disc, systemImage: "arrow.clockwise.circle")
                            }
                            .controllerAccessibilityActionTarget(
                                id: "runtime.disc.restart.\(index)",
                                label: disc
                            ) {
                                onRestart(disc)
                            }
                            .gameCardTintMenuBackgroundListRow(true)
                        }
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .navigationTitle(settings.localized("Change Disc"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button(action: onClose) {
                        Label(
                            settings.localized("Back"),
                            systemImage: "chevron.left"
                        )
                    }
                    .controllerAccessibilityActionTarget(
                        id: "runtime.disc.close",
                        label: settings.localized("Back"),
                        activationFeedback: .back,
                        action: onClose
                    )
                }
            }
            .controllerAccessibilityTargetOrder(controllerTargetOrder)
            .controllerAccessibilityNavigation(
                controllerInput: controllerInput,
                scopeKey: "runtime.change-disc",
                priority: 320,
                orbStyle: .liquidGlass,
                onBack: {
                    onClose()
                    return true
                },
                usesNativeFocusEngine: false,
                usesExplicitTargetGeometryOnly: true,
                focusScrollBehavior: .maintainWithinViewport,
                preferredInitialFocusLabel: "runtime.disc.eject",
                declaredTargetOrder: controllerTargetOrder
            )
        }
    }
}

// MARK: - RetroAchievements Panel

private struct RetroAchievementsGamePanel: View {
    @Environment(\.dismiss) private var dismiss
    let settings: SettingsStore

    @State private var entries: [RetroAchievementEntry] = []
    @State private var state: [String: Any] = [:]

    var body: some View {
        NavigationStack {
            List {
                summarySection

                if entries.isEmpty {
                    emptySection
                } else {
                    ForEach(groupedEntries, id: \.title) { group in
                        if !group.entries.isEmpty {
                            Section(group.title) {
                                ForEach(group.entries) { entry in
                                    RetroAchievementRow(entry: entry, settings: settings)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle(settings.localized("RetroAchievements"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
                refresh()
            }
            .onReceive(NotificationCenter.default.publisher(for: retroAchievementsToastNotification)) { _ in
                refresh()
            }
        }
    }

    private var summarySection: some View {
        Section {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    gameBadge

                    VStack(alignment: .leading, spacing: 4) {
                        Text(gameTitle)
                            .font(.headline)
                            .lineLimit(2)
                        Text(progressText)
                            .font(.subheadline.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }

                ProgressView(value: progressFraction)
                    .tint(.yellow)

                if !richPresence.isEmpty {
                    Label(richPresence, systemImage: "quote.bubble")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, 4)
        }
    }

    @ViewBuilder
    private var emptySection: some View {
        Section {
            VStack(spacing: 10) {
                Image(systemName: "trophy")
                    .font(.largeTitle)
                    .foregroundStyle(.secondary)
                Text(emptyTitle)
                    .font(.headline)
                    .multilineTextAlignment(.center)
                Text(emptySubtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .frame(maxWidth: .infinity)
            .padding(28)
        }
    }

    @ViewBuilder
    private var gameBadge: some View {
        if let path = state["gameIconPath"] as? String,
           !path.isEmpty,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 54, height: 54)
                .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        } else {
            Image(systemName: "trophy.fill")
                .font(.system(size: 26, weight: .semibold))
                .foregroundStyle(.yellow)
                .frame(width: 54, height: 54)
                .background(.yellow.opacity(0.14), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
    }

    private var gameTitle: String {
        let title = state["gameTitle"] as? String ?? ""
        return title.isEmpty ? settings.localized("Current Game") : title
    }

    private var richPresence: String {
        state["richPresence"] as? String ?? ""
    }

    private var unlockedCount: Int {
        (state["unlockedAchievements"] as? NSNumber)?.intValue ?? entries.filter(\.isUnlocked).count
    }

    private var totalCount: Int {
        (state["totalAchievements"] as? NSNumber)?.intValue ?? entries.filter { !$0.isUnofficial }.count
    }

    private var unlockedPoints: Int {
        (state["unlockedPoints"] as? NSNumber)?.intValue ?? entries.filter(\.isUnlocked).reduce(0) { $0 + $1.points }
    }

    private var totalPoints: Int {
        (state["totalPoints"] as? NSNumber)?.intValue ?? entries.filter { !$0.isUnofficial }.reduce(0) { $0 + $1.points }
    }

    private var progressFraction: Double {
        guard totalCount > 0 else { return 0 }
        return min(1, max(0, Double(unlockedCount) / Double(totalCount)))
    }

    private var progressText: String {
        "\(unlockedCount)/\(totalCount) \(settings.localized("achievements")) · \(unlockedPoints)/\(totalPoints) \(settings.localized("points"))"
    }

    private var emptyTitle: String {
        if (state["loggedIn"] as? NSNumber)?.boolValue == false {
            return settings.localized("RetroAchievements is not logged in.")
        }
        if (state["hasActiveGame"] as? NSNumber)?.boolValue == false {
            return settings.localized("No RetroAchievements game is active.")
        }
        return settings.localized("No achievements found for this game.")
    }

    private var emptySubtitle: String {
        settings.localized("Start a supported game with RetroAchievements enabled, then reopen this panel.")
    }

    private var groupedEntries: [(title: String, entries: [RetroAchievementEntry])] {
        let active = entries.filter { $0.isActiveChallenge && !$0.isUnlocked && !$0.isUnsupported && !$0.isUnofficial }
        let locked = entries.filter { !$0.isUnlocked && !$0.isActiveChallenge && !$0.isUnsupported && !$0.isUnofficial }
        let unlocked = entries.filter { $0.isUnlocked && !$0.isUnofficial }
        let unofficial = entries.filter(\.isUnofficial)
        let unsupported = entries.filter(\.isUnsupported)

        return [
            (settings.localized("Active / Almost There"), active),
            (settings.localized("Locked"), locked),
            (settings.localized("Unlocked"), unlocked),
            (settings.localized("Unofficial"), unofficial),
            (settings.localized("Unsupported"), unsupported),
        ]
    }

    private func refresh() {
        state = ARMSX2Bridge.retroAchievementsState()
        entries = ARMSX2Bridge.retroAchievementsForCurrentGame()
            .compactMap { RetroAchievementEntry(dictionary: $0) }
    }
}

private struct RetroAchievementRow: View {
    let entry: RetroAchievementEntry
    let settings: SettingsStore

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            badge

            VStack(alignment: .leading, spacing: 5) {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(entry.title.isEmpty ? settings.localized("Untitled Achievement") : entry.title)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(entry.isUnlocked ? .primary : .secondary)
                        .lineLimit(2)
                    Spacer(minLength: 8)
                    Text("\(entry.points) pts")
                        .font(.caption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.yellow)
                }

                if !entry.description.isEmpty {
                    Text(entry.description)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(3)
                }

                HStack(spacing: 8) {
                    Label(statusText, systemImage: statusIcon)
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(statusColor)

                    if !entry.measuredProgress.isEmpty {
                        Text(entry.measuredProgress)
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    } else if entry.measuredPercent > 0 && entry.measuredPercent < 100 {
                        Text("\(Int(entry.measuredPercent.rounded()))%")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }

                    Spacer(minLength: 0)
                }
            }
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private var badge: some View {
        if let path = entry.badgePath,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 46, height: 46)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .opacity(entry.isUnlocked ? 1 : 0.55)
        } else {
            Image(systemName: entry.isUnlocked ? "trophy.fill" : "lock.fill")
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(entry.isUnlocked ? .yellow : .secondary)
                .frame(width: 46, height: 46)
                .background(.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
    }

    private var statusText: String {
        if entry.isUnsupported { return settings.localized("Unsupported") }
        if entry.isUnofficial { return settings.localized("Unofficial") }
        if entry.isUnlocked { return settings.localized("Unlocked") }
        if entry.isActiveChallenge { return settings.localized("Active") }
        return settings.localized("Locked")
    }

    private var statusIcon: String {
        if entry.isUnsupported { return "exclamationmark.triangle.fill" }
        if entry.isUnofficial { return "sparkles" }
        if entry.isUnlocked { return "checkmark.seal.fill" }
        if entry.isActiveChallenge { return "flame.fill" }
        return "lock.fill"
    }

    private var statusColor: Color {
        if entry.isUnsupported { return .orange }
        if entry.isUnofficial { return .purple }
        if entry.isUnlocked { return .green }
        if entry.isActiveChallenge { return .red }
        return .secondary
    }
}

// MARK: - Save States Panel

private struct SaveStatesPanel: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var settings = SettingsStore.shared
    @State private var slots: [ARMSX2SaveStateSlotInfo] = []
    @State private var busySlot: Int? = nil
    @State private var pendingOverwrite: ARMSX2SaveStateSlotInfo? = nil
    @State private var hardcoreActive = false

    private let mode: SaveStateSlotPickerPurpose?
    let statusHandler: (String, Bool) -> Void

    init(
        mode: SaveStateSlotPickerPurpose? = nil,
        statusHandler: @escaping (String, Bool) -> Void
    ) {
        self.mode = mode
        self.statusHandler = statusHandler
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                if slots.isEmpty {
                    VStack(spacing: 10) {
                        Image(systemName: "hourglass")
                            .font(.largeTitle)
                            .foregroundStyle(.secondary)
                        Text(settings.localized("Save states are not ready yet."))
                            .font(.headline)
                        Text(settings.localized("Wait until the game has fully identified, then try again."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(32)
                } else {
                    // Save-state lists are small and every action must exist in
                    // the controller graph before focus seeks beyond the
                    // current viewport. A LazyVStack only mounted roughly four
                    // slots, leaving the next semantic target with no scroll
                    // anchor.
                    VStack(spacing: 10) {
                        ForEach(slots, id: \.slot) { slot in
                            SaveStateSlotRow(
                                info: slot,
                                isBusy: busySlot == slot.slot,
                                onSave: { save(slot) },
                                onLoad: { load(slot) },
                                onOverwrite: { pendingOverwrite = slot },
                                loadDisabled: hardcoreActive,
                                showsSaveAction: mode != .load,
                                showsLoadAction: mode != .save,
                                settings: settings
                            )
                        }
                    }
                    .padding()
                }
            }
            .safeAreaInset(edge: .top) {
                Text(settings.localized(hardcoreActive ?
                    "Hardcore mode allows saving states for debugging, but loading states is blocked." :
                    "Empty slots can save. Occupied slots can load or overwrite."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                    .background(.regularMaterial)
            }
            .navigationTitle(settings.localized("Save / Load States"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
                refresh()
            }
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
                refresh()
            }
            .confirmationDialog(
                "\(settings.localized("Overwrite Slot")) \(pendingOverwrite?.slot ?? 0)?",
                isPresented: Binding(
                    get: { pendingOverwrite != nil },
                    set: { newValue in
                        if !newValue {
                            pendingOverwrite = nil
                        }
                    }
                ),
                titleVisibility: .visible
            ) {
                Button(settings.localized("Overwrite"), role: .destructive) {
                    if let pendingOverwrite {
                        save(pendingOverwrite)
                    }
                    pendingOverwrite = nil
                }
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingOverwrite = nil
                }
            }
        }
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "runtime.save-state-picker.\(mode?.rawValue ?? "all")",
            priority: 345,
            orbStyle: .plain,
            onBack: {
                dismiss()
                return true
            },
            directionalLinks: controllerDirectionalLinks,
            prioritizesDirectionalLinks: true,
            // Slot rows are a vertical list with an optional two-button
            // horizontal action group. Confining the graph makes imperfect
            // left-stick diagonals continue vertically; explicit Left/Right
            // links still move between Load and Overwrite.
            confinesHorizontalFocusMovement: true,
            usesNativeFocusEngine: false,
            usesExplicitTargetGeometryOnly: true,
            preferredInitialFocusLabel: preferredControllerTargetID
        )
    }

    private var controllerTargetRows: [[String]] {
        slots.compactMap { slot -> [String]? in
            var targets: [String] = []
            if slot.occupied {
                if mode != .save, !hardcoreActive {
                    targets.append(SaveStateSlotRow.loadTargetID(slot.slot))
                }
                if mode != .load {
                    targets.append(SaveStateSlotRow.overwriteTargetID(slot.slot))
                }
            } else if mode != .load {
                targets.append(SaveStateSlotRow.saveTargetID(slot.slot))
            }
            return targets.isEmpty ? nil : targets
        }
    }

    private var controllerTargetOrder: [String] {
        controllerTargetRows.flatMap { $0 }
    }

    private var controllerDirectionalLinks: [ControllerAccessibilityDirectionalLink] {
        var links: [ControllerAccessibilityDirectionalLink] = []
        let rows = controllerTargetRows

        for (rowIndex, row) in rows.enumerated() {
            for (columnIndex, target) in row.enumerated() {
                if columnIndex > row.startIndex {
                    links.append(ControllerAccessibilityDirectionalLink(
                        fromLabel: target,
                        direction: .left,
                        toLabel: row[columnIndex - 1]
                    ))
                }
                if columnIndex + 1 < row.endIndex {
                    links.append(ControllerAccessibilityDirectionalLink(
                        fromLabel: target,
                        direction: .right,
                        toLabel: row[columnIndex + 1]
                    ))
                }

                let semanticColumn = saveStateActionColumn(target)
                if rowIndex > rows.startIndex,
                   let destination = nearestSaveStateTarget(
                       in: rows[rowIndex - 1],
                       toColumn: semanticColumn
                   ) {
                    links.append(ControllerAccessibilityDirectionalLink(
                        fromLabel: target,
                        direction: .up,
                        toLabel: destination
                    ))
                }
                if rowIndex + 1 < rows.endIndex,
                   let destination = nearestSaveStateTarget(
                       in: rows[rowIndex + 1],
                       toColumn: semanticColumn
                   ) {
                    links.append(ControllerAccessibilityDirectionalLink(
                        fromLabel: target,
                        direction: .down,
                        toLabel: destination
                    ))
                }
            }
        }
        return links
    }

    /// Load is the leading action; Save and Overwrite occupy the trailing
    /// action position. Keeping that semantic column across rows prevents
    /// vertical stick movement from stepping sideways within one slot.
    private func saveStateActionColumn(_ targetID: String) -> Int {
        targetID.hasSuffix(".load") ? 0 : 1
    }

    private func nearestSaveStateTarget(
        in row: [String],
        toColumn column: Int
    ) -> String? {
        row.min {
            abs(saveStateActionColumn($0) - column)
                < abs(saveStateActionColumn($1) - column)
        }
    }

    private var preferredControllerTargetID: String? {
        switch mode {
        case .load:
            guard let latest = slots.filter(\.occupied).max(by: { lhs, rhs in
                (lhs.modifiedDate ?? .distantPast) < (rhs.modifiedDate ?? .distantPast)
            }) else { return nil }
            return SaveStateSlotRow.loadTargetID(latest.slot)
        case .save:
            guard let slot = slots.first(where: { !$0.occupied }) ?? slots.first else {
                return nil
            }
            return slot.occupied
                ? SaveStateSlotRow.overwriteTargetID(slot.slot)
                : SaveStateSlotRow.saveTargetID(slot.slot)
        case nil:
            return controllerTargetOrder.first
        }
    }

    private func refresh() {
        slots = ARMSX2Bridge.saveStateSlots()
        hardcoreActive = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
    }

    private func save(_ slot: ARMSX2SaveStateSlotInfo) {
        let slotNumber = slot.slot
        busySlot = slotNumber
        ARMSX2Bridge.saveState(toSlot: slotNumber) { success in
            Task { @MainActor in
                busySlot = nil
                refresh()
                let message = success
                    ? "\(settings.localized("State saved to slot")) \(slotNumber)"
                    : "\(settings.localized("Could not save slot")) \(slotNumber). \(settings.localized("Try again after gameplay has fully loaded."))"
                statusHandler(message, !success)
                if success, mode == .save {
                    dismiss()
                }
            }
        }
    }

    private func load(_ slot: ARMSX2SaveStateSlotInfo) {
        guard !hardcoreActive else {
            statusHandler(settings.localized("Hardcore mode blocks loading save states."), true)
            return
        }

        let slotNumber = slot.slot
        busySlot = slotNumber
        ARMSX2Bridge.loadState(fromSlot: slotNumber) { success in
            Task { @MainActor in
                busySlot = nil
                refresh()
                let message = success
                    ? "\(settings.localized("State loaded from slot")) \(slotNumber)"
                    : "\(settings.localized("Could not load slot")) \(slotNumber). \(settings.localized("Make sure it has a saved state first."))"
                statusHandler(message, !success)
                if success {
                    dismiss()
                }
            }
        }
    }
}

// MARK: - Speed Control Panel

private struct SpeedControlPanel: View {
    @Bindable var settings: SettingsStore
    @Environment(\.dismiss) private var dismiss
    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var hardcoreActive = false

    private static let controllerTargetOrder = [
        "runtime.speed.fast-forward",
        "runtime.speed.scalar",
        "runtime.speed.done",
    ]

    var body: some View {
        NavigationStack {
            Form {
                Section(settings.localized("Fast Forward")) {
                    Toggle(
                        settings.localized("Enable Fast Forward"),
                        isOn: fastForwardEnabledBinding
                    )
                    .controllerAccessibilityToggleTarget(
                        id: "runtime.speed.fast-forward",
                        label: settings.localized("Enable Fast Forward"),
                        isOn: fastForwardEnabledBinding
                    )

                    NumberRow(.fastForwardSpeed, value: $settings.fastForwardScalar,
                              settings: settings)
                        .controllerAccessibilityTargetID(
                            "runtime.speed.scalar"
                        )
                }

                if hardcoreActive {
                    Section(settings.localized("Hardcore Mode")) {
                        Text(settings.localized("Hardcore mode blocks slowdown and frame advance. Fast forward stays available; Normal Speed is locked at 100% or higher."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Section(settings.localized("How It Works")) {
                    Text(settings.localized("The FPS Target changes display presentation without slowing CPU, audio, or game timing. Fast Forward remains a separate emulation-speed control."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    HStack {
                        Text(settings.localized("Normal Speed"))
                        Spacer()
                        Text(Self.formatPercent(SettingsStore.normalSpeedScalar(
                            frameLimiterEnabled: settings.frameLimiterEnabled)))
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }
                }
            }
            .navigationTitle(settings.localized("Speed / Fast Forward"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                    .controllerAccessibilityActionTarget(
                        id: "runtime.speed.done",
                        label: settings.localized("Done"),
                        activationFeedback: .back
                    ) {
                        dismiss()
                    }
                }
            }
            .onAppear {
                refreshRuntimeState()
            }
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
                refreshRuntimeState()
            }
        }
        .controllerAccessibilityTargetOrder(Self.controllerTargetOrder)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "runtime.speed-control",
            priority: 345,
            orbStyle: .plain,
            onBack: {
                dismiss()
                return true
            },
            confinesHorizontalFocusMovement: true,
            usesNativeFocusEngine: false,
            usesExplicitTargetGeometryOnly: true,
            focusScrollBehavior: .maintainWithinViewport,
            preferredInitialFocusLabel: "runtime.speed.fast-forward",
            preferredTrailingFocusLabels: ["runtime.speed.done"],
            declaredTargetOrder: Self.controllerTargetOrder
        )
    }

    private var fastForwardEnabledBinding: Binding<Bool> {
        Binding(
            get: { settings.fastForwardRuntimeEnabled },
            set: { enabled in
                settings.setRuntimeFastForwardEnabled(enabled)
            }
        )
    }

    private func refreshRuntimeState() {
        settings.fastForwardRuntimeEnabled = ARMSX2Bridge.limiterMode() == 1
        hardcoreActive = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
    }

    private static func formatPercent(_ scalar: Float) -> String {
        String(format: "%.0f%%", scalar * 100.0)
    }
}

// MARK: - Save State Slot Row

private struct SaveStateSlotRow: View {
    let info: ARMSX2SaveStateSlotInfo
    let isBusy: Bool
    let onSave: () -> Void
    let onLoad: () -> Void
    let onOverwrite: () -> Void
    let loadDisabled: Bool
    let showsSaveAction: Bool
    let showsLoadAction: Bool
    let settings: SettingsStore

    static func saveTargetID(_ slot: Int) -> String {
        "save-state.slot.\(slot).save"
    }

    static func loadTargetID(_ slot: Int) -> String {
        "save-state.slot.\(slot).load"
    }

    static func overwriteTargetID(_ slot: Int) -> String {
        "save-state.slot.\(slot).overwrite"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                SaveStatePreview(data: info.previewPNGData, occupied: info.occupied)
                    .frame(width: 96, height: 72)

                VStack(alignment: .leading, spacing: 4) {
                    Text("\(settings.localized("Slot")) \(info.slot)")
                        .font(.headline)

                    if info.occupied {
                        if let modifiedDate = info.modifiedDate {
                            Text(Self.dateFormatter.string(from: modifiedDate))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Text(info.fileName)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    } else {
                        Text(settings.localized("Empty"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Spacer(minLength: 8)

                if isBusy {
                    ProgressView()
                        .frame(width: 88)
                } else if !info.occupied && showsSaveAction {
                    Button(action: onSave) {
                        Label(settings.localized("Save"), systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(.borderedProminent)
                    .controllerAccessibilityActionTarget(
                        id: Self.saveTargetID(info.slot),
                        label: settings.localized("Save to Slot \(info.slot)")
                    ) {
                        onSave()
                    }
                }
            }

            if info.occupied && !isBusy && (showsLoadAction || showsSaveAction) {
                HStack(spacing: 8) {
                    if showsLoadAction {
                        Button(action: onLoad) {
                            Label(settings.localized("Load"), systemImage: "arrow.down.circle")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(loadDisabled)
                        .frame(maxWidth: .infinity)
                        .controllerAccessibilityActionTarget(
                            id: Self.loadTargetID(info.slot),
                            label: settings.localized("Load Slot \(info.slot)")
                        ) {
                            onLoad()
                        }
                    }

                    if showsSaveAction {
                        Button(action: onOverwrite) {
                            Label(settings.localized("Overwrite"), systemImage: "arrow.triangle.2.circlepath")
                        }
                        .buttonStyle(.bordered)
                        .frame(maxWidth: .infinity)
                        .controllerAccessibilityActionTarget(
                            id: Self.overwriteTargetID(info.slot),
                            label: settings.localized("Overwrite Slot \(info.slot)")
                        ) {
                            onOverwrite()
                        }
                    }
                }
            }
        }
        .padding(12)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }

    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter
    }()
}

// MARK: - Save State Preview

private struct SaveStatePreview: View {
    let data: Data?
    let occupied: Bool

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(.black.opacity(0.12))

            if let data, let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            } else {
                Image(systemName: occupied ? "photo" : "tray")
                    .font(.title2)
                    .foregroundStyle(.secondary)
            }
        }
        .clipped()
    }
}
