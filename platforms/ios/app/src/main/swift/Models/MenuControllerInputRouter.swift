// MenuControllerInputRouter.swift — physical-controller menu navigation
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import GameController
import CoreHaptics
import Observation
import SwiftUI
import UIKit

enum MenuControllerCommand: Hashable, Sendable {
    case up
    case upRight
    case right
    case downRight
    case down
    case downLeft
    case left
    case upLeft
    case activate
    case back
    case toggleFavorite
    case showContextMenu
    case previousTab
    case nextTab
}

extension MenuControllerCommand {
    var isSpatialDirection: Bool {
        switch self {
        case .up, .upRight, .right, .downRight,
             .down, .downLeft, .left, .upLeft:
            true
        case .activate, .back, .toggleFavorite, .showContextMenu,
             .previousTab, .nextTab:
            false
        }
    }

    /// Cardinal components are used only by one-dimensional controls and
    /// scrolling. The shared 2D focus engine retains the complete diagonal.
    var horizontalComponent: MenuControllerCommand? {
        switch self {
        case .left, .upLeft, .downLeft: .left
        case .right, .upRight, .downRight: .right
        default: nil
        }
    }

    var verticalComponent: MenuControllerCommand? {
        switch self {
        case .up, .upLeft, .upRight: .up
        case .down, .downLeft, .downRight: .down
        default: nil
        }
    }

    var isDiagonalDirection: Bool {
        switch self {
        case .upLeft, .upRight, .downLeft, .downRight: true
        default: false
        }
    }
}

/// Face-button state used only by the presentation-level controller feedback.
/// Keeping the held state here mirrors OrbitKeys' `pressedFace` lifecycle: the
/// color remains active until the physical button is released.
enum MenuControllerFaceButton: Equatable, Sendable {
    case cross
    case circle
    case triangle
}

/// The retained menu has three controller-only focus regions. Keeping this
/// independent from the selected tab means moving focus never changes content
/// until the player explicitly presses the primary button.
enum MenuControllerNavigationZone: Equatable, Sendable {
    case library
    case topToolbar
    case tabBar
}

enum MenuControllerFeedback: Hashable, Sendable {
    case move(MenuControllerCommand)
    case boundary
    case activate
    case toggle(isOn: Bool)
    case tabTransition
    case back
    case favorite(isFavorite: Bool)
    case contextMenu
    case submenu
    case destination
    case previousTab
    case nextTab
    case silent
}

struct MenuControllerInputEvent: Equatable, Sendable {
    let sequence: UInt64
    let command: MenuControllerCommand
    let captureOwner: String?
    let isNavigationSessionRouted: Bool
}

enum MenuControllerNavigationCaptureOwner {
    static let rootAlert = "root.controller-alert"
    static let storageExternalGamesAlert = "settings.storage.external-games-alert"
    static let gameLibraryPresentation = "game-library.presentation"
    static let cheatsPatchesManager = "cheats-patches-manager"
    static let themePresetShortcut = "menu.theme-preset-shortcut"
    static let perGameLivePreview = "per-game.live-preview"
    static let shaderLivePreview = "shader.live-preview"
}

struct MenuControllerLibraryEntryRequest: Equatable, Sendable {
    let sequence: UInt64
    let preferLast: Bool
}

struct MenuControllerQuickPauseRequest: Equatable, Sendable {
    let sequence: UInt64
}

enum MenuNavigationInputMode: String, Equatable, Sendable {
    case touch
    case controller
}

struct MenuNavigationModeSwitchRequest: Equatable, Sendable {
    let sequence: UInt64
    let mode: MenuNavigationInputMode
}

enum ControllerMacroButton: String, CaseIterable, Identifiable, Sendable {
    case select
    case pause
    case start
    case l3
    case r3
    case l1
    case r1
    case l2
    case r2
    case cross
    case circle
    case square
    case triangle
    case dpadUp
    case dpadDown
    case dpadLeft
    case dpadRight

    var id: String { rawValue }

    var title: String {
        switch self {
        case .select: "Select"
        case .pause: "Pause"
        case .start: "Start"
        case .l3: "L3"
        case .r3: "R3"
        case .l1: "L1"
        case .r1: "R1"
        case .l2: "L2"
        case .r2: "R2"
        case .cross: "Cross"
        case .circle: "Circle"
        case .square: "Square"
        case .triangle: "Triangle"
        case .dpadUp: "D-Pad Up"
        case .dpadDown: "D-Pad Down"
        case .dpadLeft: "D-Pad Left"
        case .dpadRight: "D-Pad Right"
        }
    }

    /// Bit shared with the SDL gameplay-input gate. Pause and Start are the
    /// same physical `buttonMenu` control on Apple's extended profile.
    var gameplayInputMask: UInt32 {
        switch self {
        case .dpadUp: 1 << 0
        case .dpadDown: 1 << 1
        case .dpadLeft: 1 << 2
        case .dpadRight: 1 << 3
        case .cross: 1 << 4
        case .circle: 1 << 5
        case .square: 1 << 6
        case .triangle: 1 << 7
        case .l1: 1 << 8
        case .r1: 1 << 9
        case .l2: 1 << 10
        case .r2: 1 << 11
        case .pause, .start: 1 << 12
        case .select: 1 << 13
        case .l3: 1 << 14
        case .r3: 1 << 15
        }
    }

    fileprivate func isPressed(on gamepad: GCExtendedGamepad) -> Bool {
        switch self {
        case .select:
            gamepad.buttonOptions?.isPressed == true
        case .pause:
            gamepad.buttonMenu.isPressed
        case .start:
            gamepad.buttonMenu.isPressed
        case .l3:
            gamepad.leftThumbstickButton?.isPressed == true
        case .r3:
            gamepad.rightThumbstickButton?.isPressed == true
        case .l1:
            gamepad.leftShoulder.isPressed
        case .r1:
            gamepad.rightShoulder.isPressed
        case .l2:
            gamepad.leftTrigger.isPressed
        case .r2:
            gamepad.rightTrigger.isPressed
        case .cross:
            gamepad.buttonA.isPressed
        case .circle:
            gamepad.buttonB.isPressed
        case .square:
            gamepad.buttonX.isPressed
        case .triangle:
            gamepad.buttonY.isPressed
        case .dpadUp:
            gamepad.dpad.up.isPressed
                || gamepad.leftThumbstick.yAxis.value >= 0.58
        case .dpadDown:
            gamepad.dpad.down.isPressed
                || gamepad.leftThumbstick.yAxis.value <= -0.58
        case .dpadLeft:
            gamepad.dpad.left.isPressed
                || gamepad.leftThumbstick.xAxis.value <= -0.58
        case .dpadRight:
            gamepad.dpad.right.isPressed
                || gamepad.leftThumbstick.xAxis.value >= 0.58
        }
    }
}

struct ControllerMacroBinding: RawRepresentable, Hashable, Sendable {
    let first: ControllerMacroButton
    let second: ControllerMacroButton

    init(first: ControllerMacroButton, second: ControllerMacroButton) {
        self.first = first
        self.second = second
    }

    init?(rawValue: String) {
        let components = rawValue.split(separator: "+", omittingEmptySubsequences: true)
        guard components.count == 2,
              let first = ControllerMacroButton(rawValue: String(components[0])),
              let second = ControllerMacroButton(rawValue: String(components[1])),
              first != second else { return nil }
        self.init(first: first, second: second)
    }

    var rawValue: String { "\(first.rawValue)+\(second.rawValue)" }
    var title: String { "\(first.title) + \(second.title)" }
    var gameplayInputMask: UInt32 {
        first.gameplayInputMask | second.gameplayInputMask
    }
    /// Start/Pause is the only gameplay input allowed to wait for chord resolution. Every other
    /// button keeps the direct SDL-to-emulated-pad delivery used by master when pressed alone.
    /// Custom chords without Start still trigger their action, but do not add latency to either
    /// constituent input.
    var gameplayModifierMask: UInt32 {
        let startMask = ControllerMacroButton.start.gameplayInputMask
        return gameplayInputMask & startMask
    }

    fileprivate func isPressed(on gamepad: GCExtendedGamepad) -> Bool {
        first.isPressed(on: gamepad) && second.isPressed(on: gamepad)
    }
}

enum ControllerMacroAction: String, CaseIterable, Identifiable, Sendable {
    case quickMenu
    case saveGameState
    case loadGameState
    case increaseSpeed
    case decreaseSpeed
    case enableFastForward
    case disableFastForward

    var id: String { rawValue }

    var title: String {
        switch self {
        case .quickMenu: "Quick Menu"
        case .saveGameState: "Save Game State"
        case .loadGameState: "Load Game State"
        case .increaseSpeed: "Increase Emulation Speed"
        case .decreaseSpeed: "Decrease Emulation Speed"
        case .enableFastForward: "Enable Fast Forward (1000%)"
        case .disableFastForward: "Disable Fast Forward (100%)"
        }
    }

    var detail: String {
        switch self {
        case .quickMenu:
            "Opens the Quick Menu while a game is running."
        case .saveGameState:
            "Creates or replaces the most recent save state."
        case .loadGameState:
            "Loads the most recent save state."
        case .increaseSpeed:
            "Adds 25% while pressed or held."
        case .decreaseSpeed:
            "Subtracts 25% while pressed or held."
        case .enableFastForward:
            "Enables Fast Forward and sets speed to 1000%."
        case .disableFastForward:
            "Disables Fast Forward and restores speed to 100%."
        }
    }

    var defaultBinding: ControllerMacroBinding {
        switch self {
        case .quickMenu:
            ControllerMacroBinding(first: .select, second: .pause)
        case .saveGameState:
            ControllerMacroBinding(first: .start, second: .r2)
        case .loadGameState:
            ControllerMacroBinding(first: .start, second: .l2)
        case .increaseSpeed:
            ControllerMacroBinding(first: .start, second: .dpadRight)
        case .decreaseSpeed:
            ControllerMacroBinding(first: .start, second: .dpadLeft)
        case .enableFastForward:
            ControllerMacroBinding(first: .start, second: .dpadUp)
        case .disableFastForward:
            ControllerMacroBinding(first: .start, second: .dpadDown)
        }
    }

    fileprivate var emulationShortcut: EmulationControllerShortcut? {
        switch self {
        case .quickMenu: nil
        case .saveGameState: .saveLastState
        case .loadGameState: .loadLastState
        case .increaseSpeed: .increaseSpeed
        case .decreaseSpeed: .decreaseSpeed
        case .enableFastForward: .enableFastForward
        case .disableFastForward: .disableFastForward
        }
    }

    fileprivate var repeatsWhileHeld: Bool {
        self == .increaseSpeed || self == .decreaseSpeed
    }
}

enum EmulationControllerShortcut: Hashable, Sendable {
    case saveLastState
    case loadLastState
    case increaseSpeed
    case decreaseSpeed
    case enableFastForward
    case disableFastForward
}

struct EmulationControllerShortcutRequest: Equatable, Sendable {
    let sequence: UInt64
    let shortcut: EmulationControllerShortcut
}

struct MenuControllerTabBarOrbEntryRequest: Equatable, Sendable {
    let sequence: UInt64
    /// Window-space frame of the focus surface which handed control to the
    /// persistent tab bar. The local bar overlay converts it into its own
    /// coordinates before beginning the shared OrbitKeys-style travel.
    let sourceFrame: CGRect?
}

struct MenuControllerThemePresetRequest: Equatable, Sendable {
    let sequence: UInt64
    let step: Int
}

private struct MenuControllerScrollVector: Equatable, Sendable {
    let x: Float
    let y: Float

    static let zero = MenuControllerScrollVector(x: 0, y: 0)
    var isZero: Bool { x == 0 && y == 0 }
}

struct MenuControllerScrollAxes: OptionSet, Sendable {
    let rawValue: UInt8

    static let horizontal = MenuControllerScrollAxes(rawValue: 1 << 0)
    static let vertical = MenuControllerScrollAxes(rawValue: 1 << 1)
}

private struct MenuControllerInputRouterEnvironmentKey: EnvironmentKey {
    static let defaultValue: MenuControllerInputRouter? = nil
}

extension EnvironmentValues {
    var menuControllerInputRouter: MenuControllerInputRouter? {
        get { self[MenuControllerInputRouterEnvironmentKey.self] }
        set { self[MenuControllerInputRouterEnvironmentKey.self] = newValue }
    }
}

/// Owns the profile-level callbacks used by the retained SwiftUI menu tree.
/// SDL polls the same profiles for gameplay and the native iOS fallback owns
/// individual D-pad handlers, so this deliberately uses only each profile's
/// aggregate valueChangedHandler.
@MainActor
@Observable
final class MenuControllerInputRouter: @unchecked Sendable {
    private struct NavigationSessionTarget {
        let scopeKey: String
        let priority: Int
        let registrationOrder: UInt64
        let usesNativeFocusEngine: Bool
        var isReady: Bool
        var scrollWindow: UIWindow?
        var scrollFocusFrame: CGRect?
        let handler: @MainActor (MenuControllerCommand) -> Bool
        let entryHandler: @MainActor (Bool) -> Bool
        let scrollHandler: @MainActor (UIScrollView, MenuControllerCommand) -> Void
        let scrollStateHandler: @MainActor (Bool) -> Void
    }

    private struct RightStickTarget {
        let priority: Int
        let registrationOrder: UInt64
        let ownerSessionID: UUID?
        let manualCaptureOwner: String?
        let handler: @MainActor (MenuControllerScrollVector) -> Bool
    }

    private struct ManualNavigationCapture {
        let priority: Int
        let registrationOrder: UInt64
    }

    private struct PendingNavigationSessionCommand {
        let sequence: UInt64
        let scopeKey: String
        let command: MenuControllerCommand
        let isRepeat: Bool
        let repeatAcceleration: Double
    }

    private struct PendingNavigationSessionEntryRequest {
        let id = UUID()
        let preferLast: Bool
        let scopePrefix: String?
        var ownerSessionID: UUID?
        var ownerScopeKey: String?
    }

    private enum Button: Hashable {
        case primary
        case secondary
        case favorite
        case info
        case previousTab
        case nextTab
        case previousTheme
        case nextTheme
    }

    private(set) var latestEvent: MenuControllerInputEvent?
    private(set) var latestEventTimestamp: TimeInterval?
    private(set) var latestLibraryEntryRequest: MenuControllerLibraryEntryRequest?
    private(set) var latestQuickPauseRequest: MenuControllerQuickPauseRequest?
    private(set) var latestEmulationShortcutRequest:
        EmulationControllerShortcutRequest?
    private(set) var latestTabBarOrbEntryRequest: MenuControllerTabBarOrbEntryRequest?
    private(set) var latestThemePresetRequest: MenuControllerThemePresetRequest?
    private(set) var pendingNavigationModeSwitchRequest:
        MenuNavigationModeSwitchRequest?
    /// Window-space geometry published by the real tab bar. RootView portals
    /// this frame into the presentation-wide orb host so crossing a safe-area
    /// boundary never replaces the active particle field.
    private(set) var tabBarOrbFocusFrameInWindow: CGRect?
    private(set) var tabBarOrbFocusedIndex: Int?
    private(set) var tabBarOrbSelectedIndex: Int?
    private(set) var focusReleaseSequence: UInt64 = 0
    private(set) var rightStickScrollEndSequence: UInt64 = 0
    private(set) var isMenuActive = false
    private(set) var isNavigationCaptured = false
    private(set) var hasNavigationSession = false
    private(set) var hasConnectedController = false
    private(set) var isControllerNavigationEnabled = false
    private(set) var activeNavigationInputMode: MenuNavigationInputMode = .touch
    private(set) var navigationZone: MenuControllerNavigationZone = .library
    private(set) var pressedFaceButton: MenuControllerFaceButton?

    @ObservationIgnored private var sequence: UInt64 = 0
    @ObservationIgnored private var connectObserver: NSObjectProtocol?
    @ObservationIgnored private var disconnectObserver: NSObjectProtocol?
    @ObservationIgnored private var registeredExtendedProfiles = Set<ObjectIdentifier>()
    @ObservationIgnored private var registeredMicroProfiles = Set<ObjectIdentifier>()
    @ObservationIgnored private var heldDirections: [ObjectIdentifier: MenuControllerCommand] = [:]
    @ObservationIgnored private var directionalHoldStartTimes: [
        ObjectIdentifier: TimeInterval
    ] = [:]
    @ObservationIgnored private var lastDirectionEdges: [
        ObjectIdentifier: (command: MenuControllerCommand, timestamp: TimeInterval)
    ] = [:]
    /// Some wireless controllers expose one physical D-pad edge through both
    /// GameController and a keyboard-style HID path. Coalesce that duplicate
    /// globally while leaving intentional held-repeat events untouched.
    @ObservationIgnored private var lastPublishedDirectionalEdge: (
        command: MenuControllerCommand,
        timestamp: TimeInterval
    )?
    @ObservationIgnored private var directionSamplingTasks: [
        ObjectIdentifier: Task<Void, Never>
    ] = [:]
    @ObservationIgnored private var libraryEntrySuppressedEventSequence: UInt64?
    @ObservationIgnored private var pendingTabBarOrbSourceFrame: CGRect?
    @ObservationIgnored private var pressedButtons: [ObjectIdentifier: Set<Button>] = [:]
    @ObservationIgnored private var emulationMacroLatches: [
        ObjectIdentifier: Set<ControllerMacroAction>
    ] = [:]
    @ObservationIgnored private var emulationMacroRepeatTasks: [
        ObjectIdentifier: [ControllerMacroAction: Task<Void, Never>]
    ] = [:]
    @ObservationIgnored private var navigationSessions: [UUID: NavigationSessionTarget] = [:]
    @ObservationIgnored private var rememberedNavigationFocusKeys: [String: String] = [:]
    @ObservationIgnored private var navigationSessionSequence: UInt64 = 0
    @ObservationIgnored private var pendingNavigationSessionCommands: [
        UUID: PendingNavigationSessionCommand
    ] = [:]
    @ObservationIgnored private var pendingNavigationSessionEntryRequest:
        PendingNavigationSessionEntryRequest?
    @ObservationIgnored private var navigationSessionEntryTask: Task<Void, Never>?
    @ObservationIgnored private var launchInputInterceptor: (@MainActor () -> Void)?
    @ObservationIgnored private var manuallyCapturedNavigation = false
    @ObservationIgnored private var manualNavigationCaptures: [
        String: ManualNavigationCapture
    ] = [:]
    @ObservationIgnored private var manualNavigationCaptureSequence: UInt64 = 0
    @ObservationIgnored private var repeatTasks: [
        ObjectIdentifier: Task<Void, Never>
    ] = [:]
    @ObservationIgnored private var themePresetRepeatTasks: [
        ObjectIdentifier: [Button: Task<Void, Never>]
    ] = [:]
    @ObservationIgnored private var touchThemePresetShortcutSuppressedUntil = 0.0
    @ObservationIgnored private var rightStickVector = MenuControllerScrollVector.zero
    @ObservationIgnored private(set) var lastRightStickScrollDirection: MenuControllerCommand?
    @ObservationIgnored private(set) var isRepeatingDirectionCommand = false
    /// Transient multiplier for the directional command currently being
    /// delivered. Navigation sessions use it to keep focus-scroll animations
    /// shorter than the accelerating repeat cadence.
    @ObservationIgnored private(set) var directionalRepeatAcceleration = 1.0
    @ObservationIgnored private(set) var isRightStickScrolling = false
    @ObservationIgnored private var rightStickHoldStartTime: CFTimeInterval?
    @ObservationIgnored private var rightStickSessionOwner: (id: UUID, scopeKey: String)?
    @ObservationIgnored private var rightStickTargets: [UUID: RightStickTarget] = [:]
    @ObservationIgnored private var activeRightStickTargetID: UUID?
    @ObservationIgnored private var rightStickRegistrationSequence: UInt64 = 0
    @ObservationIgnored private var rightStickRedispatchTask: Task<Void, Never>?
    @ObservationIgnored private let globalScrollDriver = MenuControllerGlobalScrollDriver()
    @ObservationIgnored private let feedbackPlayer = MenuControllerFeedbackPlayer()

    func start() {
        ControllerEventDeliveryCoordinator.shared.install(router: self)
        guard connectObserver == nil else {
            refreshConnectedControllers()
            return
        }

        let center = NotificationCenter.default
        connectObserver = center.addObserver(
            forName: .GCControllerDidConnect,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshConnectedControllers()
            }
        }
        disconnectObserver = center.addObserver(
            forName: .GCControllerDidDisconnect,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshConnectedControllers()
            }
        }
        refreshConnectedControllers()
    }

    func stop() {
        let center = NotificationCenter.default
        if let connectObserver {
            center.removeObserver(connectObserver)
        }
        if let disconnectObserver {
            center.removeObserver(disconnectObserver)
        }
        connectObserver = nil
        disconnectObserver = nil
        setMenuActive(false)
        manuallyCapturedNavigation = false
        manualNavigationCaptures.removeAll(keepingCapacity: false)
        navigationSessions.removeAll(keepingCapacity: false)
        navigationSessionEntryTask?.cancel()
        navigationSessionEntryTask = nil
        pendingNavigationSessionEntryRequest = nil
        launchInputInterceptor = nil
        cancelEmulationShortcutInput()
        cancelThemePresetRepeats()
        ControllerEventDeliveryCoordinator.shared.setNativeFocusAuthorized(false)
        pendingNavigationSessionCommands.removeAll(keepingCapacity: false)
        updateNavigationCaptureState()
        registeredExtendedProfiles.removeAll(keepingCapacity: false)
        registeredMicroProfiles.removeAll(keepingCapacity: false)
        hasConnectedController = false
        pendingNavigationModeSwitchRequest = nil
        isControllerNavigationEnabled = false
        activeNavigationInputMode = .touch
        updateRightStick(x: 0, y: 0)
    }

    func setMenuActive(_ active: Bool) {
        MenuAudioPackManager.shared.setInterfaceActive(active)
        ControllerEventDeliveryCoordinator.shared.setMenuActive(active)
        guard active != isMenuActive else { return }
        isMenuActive = active
        updateControllerDeliveryPreference()
        cancelRepeat()
        cancelThemePresetRepeats()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        lastPublishedDirectionalEdge = nil
        pressedButtons.removeAll(keepingCapacity: true)
        pressedFaceButton = nil
        cancelEmulationShortcutInput()
        if active {
            refreshConnectedControllers()
        } else {
            latestThemePresetRequest = nil
            rightStickRedispatchTask?.cancel()
            rightStickRedispatchTask = nil
            manuallyCapturedNavigation = false
            manualNavigationCaptures.removeAll(keepingCapacity: true)
            navigationSessions.removeAll(keepingCapacity: true)
            navigationSessionEntryTask?.cancel()
            navigationSessionEntryTask = nil
            pendingNavigationSessionEntryRequest = nil
            ControllerEventDeliveryCoordinator.shared.setNativeFocusAuthorized(false)
            pendingNavigationSessionCommands.removeAll(keepingCapacity: true)
            hasNavigationSession = false
            updateNavigationCaptureState()
            navigationZone = .library
            libraryEntrySuppressedEventSequence = nil
            updateRightStick(x: 0, y: 0)
            feedbackPlayer.releaseResources()
        }
    }

    /// UIKit can temporarily replace GameController value handlers while
    /// Control Center owns the foreground. `setMenuActive(true)` intentionally
    /// does nothing when the Quick Menu is already open, so foreground return
    /// needs a separate rebind path that preserves the active navigation
    /// session while discarding stale held-button state.
    func applicationDidBecomeActive() {
        guard isMenuActive else { return }
        cancelRepeat()
        cancelThemePresetRepeats()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        lastPublishedDirectionalEdge = nil
        pressedButtons.removeAll(keepingCapacity: true)
        pressedFaceButton = nil
        updateRightStick(x: 0, y: 0)
        refreshConnectedControllers()
        updateControllerDeliveryPreference()
        updateNavigationCaptureState()
        schedulePendingNavigationSessionEntryIfReady()
    }

    func setNavigationCaptured(
        _ captured: Bool,
        owner: String,
        priority: Int
    ) {
        if captured, isMenuActive {
            manualNavigationCaptureSequence &+= 1
            manualNavigationCaptures[owner] = ManualNavigationCapture(
                priority: priority,
                registrationOrder: manualNavigationCaptureSequence
            )
            pendingNavigationSessionCommands.removeAll(keepingCapacity: true)
        } else {
            manualNavigationCaptures.removeValue(forKey: owner)
        }
        manuallyCapturedNavigation = !manualNavigationCaptures.isEmpty
        updateControllerDeliveryPreference()
        updateNavigationCaptureState()
        activeRightStickTargetID = nil
        dispatchRightStickVector()
        schedulePendingNavigationSessionEntryIfReady()
    }

    private var frontmostManualNavigationCapture: (
        key: String,
        value: ManualNavigationCapture
    )? {
        manualNavigationCaptures.max { lhs, rhs in
            if lhs.value.priority != rhs.value.priority {
                return lhs.value.priority < rhs.value.priority
            }
            return lhs.value.registrationOrder < rhs.value.registrationOrder
        }
    }

    /// Requests an explicit ownership handoff when touch interrupts an active
    /// controller session. The first touch-down publishes the prompt before
    /// its eventual touch-up, so both navigation systems never silently own
    /// the same screen.
    func noteTouchInput() {
        guard isMenuActive else { return }
        guard pendingNavigationModeSwitchRequest == nil else { return }
        guard isControllerNavigationEnabled else { return }
        if isControllerNavigationEnabled, hasConnectedController,
           UIFrameRateSettings.shared.asksBeforeTouchNavigation {
            publishNavigationModeSwitchRequest(.touch)
            return
        }
        activateTouchNavigation()
    }

    func acceptNavigationModeSwitch(
        _ mode: MenuNavigationInputMode,
        doNotAskAgain: Bool
    ) {
        guard pendingNavigationModeSwitchRequest?.mode == mode else { return }
        pendingNavigationModeSwitchRequest = nil
        if doNotAskAgain {
            switch mode {
            case .touch:
                UIFrameRateSettings.shared.asksBeforeTouchNavigation = false
            case .controller:
                UIFrameRateSettings.shared.asksBeforeControllerNavigation = false
            }
        }
        switch mode {
        case .touch: activateTouchNavigation()
        case .controller: activateControllerNavigation()
        }
    }

    func cancelNavigationModeSwitch(_ mode: MenuNavigationInputMode) {
        guard pendingNavigationModeSwitchRequest?.mode == mode else { return }
        pendingNavigationModeSwitchRequest = nil
        switch mode {
        case .touch: activateControllerNavigation()
        case .controller: activateTouchNavigation()
        }
    }

    private func publishNavigationModeSwitchRequest(
        _ mode: MenuNavigationInputMode
    ) {
        sequence &+= 1
        pendingNavigationModeSwitchRequest = MenuNavigationModeSwitchRequest(
            sequence: sequence,
            mode: mode
        )
        if mode == .controller {
            // The confirmation itself must be controller navigable. Mount the
            // controller presentation only for the prompt; Cancel retires it
            // again immediately.
            isControllerNavigationEnabled = true
            cancelRepeat()
            cancelThemePresetRepeats()
            cancelDirectionSampling()
            heldDirections.removeAll(keepingCapacity: true)
            directionalHoldStartTimes.removeAll(keepingCapacity: true)
            rightStickVector = .zero
            rightStickHoldStartTime = nil
        }
    }

    private func prepareControllerNavigationForInput() -> Bool {
        if isControllerNavigationEnabled { return true }
        guard pendingNavigationModeSwitchRequest == nil else { return false }
        if UIFrameRateSettings.shared.asksBeforeControllerNavigation {
            publishNavigationModeSwitchRequest(.controller)
            return false
        }
        activateControllerNavigation()
        return true
    }

    private func activateTouchNavigation() {
        activeNavigationInputMode = .touch
        isControllerNavigationEnabled = false
        cancelRepeat()
        cancelThemePresetRepeats()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        lastPublishedDirectionalEdge = nil
        rightStickVector = .zero
        rightStickHoldStartTime = nil
        finishRightStickScrolling()
        ControllerEventDeliveryCoordinator.shared.setNativeFocusAuthorized(false)
        feedbackPlayer.releaseControllerResources()
        requestFocusRelease()
        tabBarOrbFocusFrameInWindow = nil
        tabBarOrbFocusedIndex = nil
        pendingTabBarOrbSourceFrame = nil
        updateNavigationCaptureState()
        dispatchRightStickVector()
        retireControllerNavigationResourcesAfterHandoff()
    }

    private func activateControllerNavigation() {
        activeNavigationInputMode = .controller
        isControllerNavigationEnabled = true
        hasNavigationSession = !navigationSessions.isEmpty
        updateNavigationCaptureState()
        feedbackPlayer.attach(to: GCController.controllers())
        updateControllerDeliveryPreference()
    }

    private func retireControllerNavigationResourcesAfterHandoff() {
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self,
                  self.activeNavigationInputMode == .touch,
                  self.pendingNavigationModeSwitchRequest == nil else { return }
            self.navigationSessionEntryTask?.cancel()
            self.navigationSessionEntryTask = nil
            self.pendingNavigationSessionEntryRequest = nil
            self.pendingNavigationSessionCommands.removeAll(keepingCapacity: false)
            self.navigationSessions.removeAll(keepingCapacity: false)
            self.hasNavigationSession = false
            self.manuallyCapturedNavigation = false
            self.manualNavigationCaptures.removeAll(keepingCapacity: false)
            self.rightStickRedispatchTask?.cancel()
            self.rightStickRedispatchTask = nil
            self.rightStickTargets.removeAll(keepingCapacity: false)
            self.activeRightStickTargetID = nil
            self.updateNavigationCaptureState()
            self.updateControllerDeliveryPreference()
        }
    }

    private func requestFocusRelease() {
        focusReleaseSequence &+= 1
    }

    func rememberNavigationFocusKey(_ key: String, forScope scopeKey: String) {
        guard !scopeKey.isEmpty, !key.isEmpty else { return }
        rememberedNavigationFocusKeys[scopeKey] = key
        if scopeKey.hasPrefix("settings.") {
            UserDefaults.standard.set(
                key,
                forKey: "ARMSX2iOSControllerFocus.\(scopeKey)"
            )
        }
    }

    func rememberedNavigationFocusKey(forScope scopeKey: String) -> String? {
        if let remembered = rememberedNavigationFocusKeys[scopeKey] {
            return remembered
        }
        guard scopeKey.hasPrefix("settings."),
              let persisted = UserDefaults.standard.string(
                forKey: "ARMSX2iOSControllerFocus.\(scopeKey)"
              ),
              !persisted.isEmpty else { return nil }
        rememberedNavigationFocusKeys[scopeKey] = persisted
        return persisted
    }

    /// Registers a screen-scoped consumer with the one controller command
    /// source. Highest-priority/most-recent presentation wins, matching sheet
    /// and full-screen-cover z-order without broadcasting state into every
    /// retained tab.
    func registerNavigationSession(
        id: UUID,
        scopeKey: String,
        priority: Int,
        usesNativeFocusEngine: Bool,
        isReady: Bool,
        handler: @escaping @MainActor (MenuControllerCommand) -> Bool,
        entryHandler: @escaping @MainActor (Bool) -> Bool = { _ in false },
        scrollHandler: @escaping @MainActor (UIScrollView, MenuControllerCommand) -> Void = { _, _ in },
        scrollStateHandler: @escaping @MainActor (Bool) -> Void = { _ in }
    ) {
        let scopeChanged = navigationSessions[id]?.scopeKey != scopeKey
        let order: UInt64
        if let existing = navigationSessions[id] {
            order = existing.registrationOrder
            if existing.scopeKey != scopeKey {
                pendingNavigationSessionCommands.removeValue(forKey: id)
                if pendingNavigationSessionEntryRequest?.ownerSessionID == id {
                    cancelNavigationSessionEntry()
                }
                // The UUID can survive a NavigationStack push; its scope cannot.
                // Retire the old analog owner before registering the new page.
                if rightStickSessionOwner?.id == id {
                    finishRightStickScrolling(restoresFocus: false)
                }
            }
        } else {
            navigationSessionSequence &+= 1
            order = navigationSessionSequence
        }
        navigationSessions[id] = NavigationSessionTarget(
            scopeKey: scopeKey,
            priority: priority,
            registrationOrder: order,
            usesNativeFocusEngine: usesNativeFocusEngine,
            isReady: isReady,
            scrollWindow: navigationSessions[id]?.scrollWindow,
            scrollFocusFrame: navigationSessions[id]?.scrollFocusFrame,
            handler: handler,
            entryHandler: entryHandler,
            scrollHandler: scrollHandler,
            scrollStateHandler: scrollStateHandler
        )
        hasNavigationSession = !navigationSessions.isEmpty
        updateNavigationCaptureState()
        schedulePendingNavigationSessionCommandIfReady(id: id)
        schedulePendingNavigationSessionEntryIfReady()
        // Registration is lifecycle bookkeeping only. A List/Form can register
        // while the persistent tab bar still owns controller focus; changing
        // zones here makes the next Down press enter the first lazy row.
        // `requestNavigationSessionEntry` is the sole content-entry owner.
        updateControllerDeliveryPreference()
        if scopeChanged, !rightStickVector.isZero { scheduleRightStickRedispatch() }
    }

    func setNavigationSessionReady(id: UUID, isReady: Bool) {
        guard var target = navigationSessions[id],
              target.isReady != isReady else { return }
        target.isReady = isReady
        navigationSessions[id] = target
        updateControllerDeliveryPreference()
        updateNavigationCaptureState()
        schedulePendingNavigationSessionCommandIfReady(id: id)
        schedulePendingNavigationSessionEntryIfReady()
    }

    private func schedulePendingNavigationSessionCommandIfReady(id: UUID) {
        guard navigationSessions[id]?.isReady == true,
              !hasPendingEntry(for: id),
              let pending = pendingNavigationSessionCommands[id] else { return }
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self,
                  self.navigationZone != .tabBar,
                  !self.hasPendingEntry(for: id),
                  let queued = self.pendingNavigationSessionCommands[id],
                  queued.sequence == pending.sequence,
                  let current = self.navigationSessions[id],
                  current.isReady,
                  current.scopeKey == pending.scopeKey,
                  self.frontmostNavigationSessionEntry?.key == id,
                  self.frontmostManualNavigationCapture == nil else { return }
            self.pendingNavigationSessionCommands.removeValue(forKey: id)
            let previousRepeat = self.isRepeatingDirectionCommand
            let previousAcceleration = self.directionalRepeatAcceleration
            self.isRepeatingDirectionCommand = pending.isRepeat
            self.directionalRepeatAcceleration = pending.repeatAcceleration
            defer {
                self.isRepeatingDirectionCommand = previousRepeat
                self.directionalRepeatAcceleration = previousAcceleration
            }
            _ = current.handler(pending.command)
        }
    }

    func setNavigationSessionScrollFocus(
        id: UUID,
        window: UIWindow?,
        frame: CGRect?
    ) {
        guard var target = navigationSessions[id] else { return }
        let unchanged = target.scrollWindow === window
            && target.scrollFocusFrame == frame
        guard !unchanged else { return }
        target.scrollWindow = window
        target.scrollFocusFrame = frame
        navigationSessions[id] = target
        // Focus release is published while the first analog vector is already
        // being dispatched. Preserve an explicit pane target that is actively
        // scrolling instead of recursively resolving the same Form again.
        if !rightStickVector.isZero, activeRightStickTargetID == nil {
            scheduleRightStickRedispatch()
        }
    }

    func unregisterNavigationSession(id: UUID) {
        guard let removed = navigationSessions.removeValue(forKey: id) else { return }
        if rightStickSessionOwner?.id == id { finishRightStickScrolling() }
        if let request = pendingNavigationSessionEntryRequest,
           request.ownerSessionID == id
            || (request.ownerSessionID == nil
                && request.scopePrefix.map(removed.scopeKey.hasPrefix) == true) {
            cancelNavigationSessionEntry()
        }
        pendingNavigationSessionCommands.removeValue(forKey: id)
        hasNavigationSession = !navigationSessions.isEmpty
        updateControllerDeliveryPreference()
        updateNavigationCaptureState()
        activeRightStickTargetID = nil
        scheduleRightStickRedispatch()
        schedulePendingNavigationSessionEntryIfReady()
    }

    private func updateNavigationCaptureState() {
        let next = isMenuActive
            && isControllerNavigationEnabled
            && activeNavigationInputMode == .controller
            && (manuallyCapturedNavigation
                || !navigationSessions.isEmpty)
        guard next != isNavigationCaptured else { return }
        isNavigationCaptured = next
    }

    func setNavigationZone(_ zone: MenuControllerNavigationZone) {
        guard isMenuActive else { return }
        if zone != .library { cancelNavigationSessionEntry() }
        guard zone != navigationZone else { return }
        // A command captured while the previous focus owner was mounting must
        // never be replayed into the newly selected tab or chrome region.
        pendingNavigationSessionCommands.removeAll(keepingCapacity: true)
        if zone == .tabBar {
            let sourceFrame = pendingTabBarOrbSourceFrame
                ?? frontmostNavigationSession?.scrollFocusFrame
            pendingTabBarOrbSourceFrame = nil
            sequence &+= 1
            latestTabBarOrbEntryRequest = MenuControllerTabBarOrbEntryRequest(
                sequence: sequence,
                sourceFrame: sourceFrame
            )
        } else {
            pendingTabBarOrbSourceFrame = nil
        }
        navigationZone = zone
        updateControllerDeliveryPreference()
        activeRightStickTargetID = nil
        dispatchRightStickVector()
    }

    /// A modal controller surface (Quick Menu, Per-Game Settings, and their
    /// children) has no relationship with the retained menu's toolbar/tab-bar
    /// zones. Claim the content zone as soon as that presentation registers.
    /// This also makes a controller edge re-enable focus after touch input.
    func claimPresentedNavigationFocus() {
        guard isMenuActive, isControllerNavigationEnabled else { return }
        setNavigationZone(.library)
    }

    /// A live preview owns the visible controller surface, but its hidden
    /// Per-Game Settings session still owns the focused adjustable row. Route
    /// horizontal changes directly to that session without releasing the
    /// preview capture or publishing a second controller event.
    @discardableResult
    func adjustPerGameLivePreview(_ command: MenuControllerCommand) -> Bool {
        adjustCapturedLivePreview(
            command,
            owner: MenuControllerNavigationCaptureOwner.perGameLivePreview
        )
    }

    /// Forwards a horizontal command from a live-preview input shield to the
    /// still-mounted navigation session underneath it. This keeps sliders and
    /// pickers adjustable while their panel is visually hidden.
    @discardableResult
    func adjustCapturedLivePreview(
        _ command: MenuControllerCommand,
        owner: String
    ) -> Bool {
        guard isMenuActive,
              frontmostManualNavigationCapture?.key
                == owner,
              command.horizontalComponent != nil,
              navigationZone != .tabBar,
              let session = frontmostNavigationSessionEntry?.value,
              session.isReady else { return false }
        return session.handler(command)
    }

    /// Supplies exact geometry for custom controller surfaces that don't use
    /// the shared accessibility session, such as Game Library cards.
    func prepareTabBarOrbTransition(sourceFrame: CGRect?) {
        pendingTabBarOrbSourceFrame = sourceFrame
    }

    func updateTabBarOrbFocusFrame(
        _ frame: CGRect?,
        focusedIndex: Int,
        selectedIndex: Int
    ) {
        guard let frame,
              !frame.isNull,
              !frame.isInfinite,
              frame.width > 1,
              frame.height > 1 else { return }
        guard tabBarOrbFocusFrameInWindow != frame
                || tabBarOrbFocusedIndex != focusedIndex
                || tabBarOrbSelectedIndex != selectedIndex else { return }
        tabBarOrbFocusFrameInWindow = frame
        tabBarOrbFocusedIndex = focusedIndex
        tabBarOrbSelectedIndex = selectedIndex
    }

    func requestLibraryEntry(preferLast: Bool) {
        guard isMenuActive else { return }
        cancelNavigationSessionEntry()
        // The event that leaves the tab bar is also observed by the retained
        // Game Library. Suppress only that exact event, not the held direction:
        // the first repeat must continue upward from the card that received
        // focus instead of leaving the selection stuck until button release.
        libraryEntrySuppressedEventSequence = latestEvent?.sequence
        // Route this through the normal zone transition so a held right stick
        // is detached from the pre-rotation/tab target and immediately offered
        // to the newly mounted library scroll view.
        setNavigationZone(.library)
        sequence &+= 1
        latestLibraryEntryRequest = MenuControllerLibraryEntryRequest(
            sequence: sequence,
            preferLast: preferLast
        )
    }

    /// Moves focus from the persistent bottom tab bar into the frontmost
    /// shared-navigation surface. Unlike the Game Library request, this works
    /// for dynamically materialized Form/List screens such as BIOS/Settings.
    @discardableResult
    func requestNavigationSessionEntry(
        preferLast: Bool,
        matchingScopePrefix scopePrefix: String? = nil
    ) -> Bool {
        guard isMenuActive else { return false }
        // The entry edge already belongs to the tab bar. Repeats may continue
        // through the content; endpoint rules stop them at Language/Help.
        setNavigationZone(.library)
        cancelNavigationSessionEntry()
        pendingNavigationSessionEntryRequest =
            PendingNavigationSessionEntryRequest(
                preferLast: preferLast,
                scopePrefix: scopePrefix
            )
        schedulePendingNavigationSessionEntryIfReady()
        return true
    }

    private func cancelNavigationSessionEntry() {
        navigationSessionEntryTask?.cancel()
        navigationSessionEntryTask = nil
        pendingNavigationSessionEntryRequest = nil
    }

    private func hasPendingEntry(for id: UUID) -> Bool {
        guard let request = pendingNavigationSessionEntryRequest,
              let session = navigationSessions[id] else { return false }
        if let owner = request.ownerSessionID {
            return owner == id && request.ownerScopeKey == session.scopeKey
        }
        return request.scopePrefix.map(session.scopeKey.hasPrefix) ?? true
    }

    private func schedulePendingNavigationSessionEntryIfReady() {
        guard navigationSessionEntryTask == nil,
              var request = pendingNavigationSessionEntryRequest,
              frontmostManualNavigationCapture == nil,
              let target = navigationSessionEntry(matchingScopePrefix: request.scopePrefix),
              target.key == frontmostNavigationSessionEntry?.key else { return }
        request.ownerSessionID = target.key
        request.ownerScopeKey = target.value.scopeKey
        pendingNavigationSessionEntryRequest = request
        guard target.value.isReady else { return }
        navigationSessionEntryTask = Task { @MainActor [weak self] in
            // Session readiness is published by UIViewRepresentable probes.
            // Enter focus only after that update and its List layout complete.
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.navigationSessionEntryTask = nil
            guard let current = self.pendingNavigationSessionEntryRequest,
                  current.id == request.id,
                  self.frontmostManualNavigationCapture == nil,
                  let target = self.navigationSessionEntry(
                      matchingScopePrefix: current.scopePrefix
                  ), target.value.isReady,
                  target.key == current.ownerSessionID,
                  target.value.scopeKey == current.ownerScopeKey,
                  target.key == self.frontmostNavigationSessionEntry?.key else { return }
            if target.value.entryHandler(current.preferLast) {
                guard self.pendingNavigationSessionEntryRequest?.id == current.id else { return }
                self.pendingNavigationSessionEntryRequest = nil
                self.schedulePendingNavigationSessionCommandIfReady(
                    id: target.key
                )
            }
        }
    }

    /// While the boot movie covers the menu, profile callbacks stay installed
    /// solely so the first physical controller engagement can dismiss it. The
    /// event is consumed before native focus or the retained menu sees it.
    func setLaunchInputInterceptor(
        _ interceptor: (@MainActor () -> Void)?
    ) {
        launchInputInterceptor = interceptor
        cancelRepeat()
        cancelDirectionSampling()
        heldDirections.removeAll(keepingCapacity: true)
        directionalHoldStartTimes.removeAll(keepingCapacity: true)
        lastDirectionEdges.removeAll(keepingCapacity: true)
        pressedButtons.removeAll(keepingCapacity: true)
        pressedFaceButton = nil
        updateControllerDeliveryPreference()
    }

    func suppressesLibraryEvent(_ event: MenuControllerInputEvent) -> Bool {
        event.sequence == libraryEntrySuppressedEventSequence
    }

    fileprivate func registerRightStickTarget(
        id: UUID,
        priority: Int,
        ownerSessionID: UUID?,
        manualCaptureOwner: String?,
        handler: @escaping @MainActor (MenuControllerScrollVector) -> Bool
    ) {
        rightStickRegistrationSequence &+= 1
        rightStickTargets[id] = RightStickTarget(
            priority: priority,
            registrationOrder: rightStickRegistrationSequence,
            ownerSessionID: ownerSessionID,
            manualCaptureOwner: manualCaptureOwner,
            handler: handler
        )
        scheduleRightStickRedispatch()
    }

    fileprivate func unregisterRightStickTarget(id: UUID) {
        if let target = rightStickTargets.removeValue(forKey: id) {
            _ = target.handler(.zero)
        }
        if activeRightStickTargetID == id {
            finishRightStickScrolling(restoresFocus: false)
            activeRightStickTargetID = nil
        }
        scheduleRightStickRedispatch()
    }

    fileprivate func rightStickTargetBecameUnavailable(id: UUID) {
        guard activeRightStickTargetID == id else { return }
        finishRightStickScrolling()
        activeRightStickTargetID = nil
        scheduleRightStickRedispatch()
    }

    fileprivate func rightStickTargetHierarchyDidChange() {
        finishRightStickScrolling(restoresFocus: false)
        activeRightStickTargetID = nil
        scheduleRightStickRedispatch()
    }

    private func scheduleRightStickRedispatch() {
        guard rightStickRedispatchTask == nil else { return }
        rightStickRedispatchTask = Task { @MainActor [weak self] in
            // UIViewRepresentable registration and focus release can arrive in
            // the same layout transaction. Resolve the scroll owner once after
            // that transaction, rather than recursively walking UIKit here.
            await Task.yield()
            guard let self, !Task.isCancelled else { return }
            self.rightStickRedispatchTask = nil
            self.dispatchRightStickVector()
        }
    }

    func playFeedback(_ feedback: MenuControllerFeedback) {
        guard isMenuActive else { return }
        // Focus publication and row layout should finish before audio/haptic
        // feedback performs any framework work on the main actor.
        Task { @MainActor [weak self] in
            await Task.yield()
            guard let self, self.isMenuActive else { return }
            self.feedbackPlayer.play(feedback)
        }
    }

    /// Touch controls use their existing event-specific audio, but share the
    /// same phone and connected-controller haptic patterns as controller input.
    func playTouchHaptics(_ feedback: MenuControllerFeedback) {
        guard isMenuActive else { return }
        feedbackPlayer.playHaptics(feedback)
    }

    private func refreshConnectedControllers() {
        var liveExtendedProfiles = Set<ObjectIdentifier>()
        var liveMicroProfiles = Set<ObjectIdentifier>()

        for controller in GCController.controllers() {
            // Game Controller callbacks use this queue. Keeping menu state on
            // main also makes the SwiftUI event publication deterministic.
            controller.handlerQueue = .main

            if let gamepad = controller.extendedGamepad {
                let profileID = ObjectIdentifier(gamepad)
                liveExtendedProfiles.insert(profileID)
                _ = registeredExtendedProfiles.insert(profileID)
                installExtendedProfile(gamepad)
            } else if let gamepad = controller.microGamepad {
                let profileID = ObjectIdentifier(gamepad)
                liveMicroProfiles.insert(profileID)
                _ = registeredMicroProfiles.insert(profileID)
                installMicroProfile(gamepad)
            }
        }

        registeredExtendedProfiles.formIntersection(liveExtendedProfiles)
        registeredMicroProfiles.formIntersection(liveMicroProfiles)
        hasConnectedController = !liveExtendedProfiles.isEmpty
            || !liveMicroProfiles.isEmpty
        if !hasConnectedController {
            pendingNavigationModeSwitchRequest = nil
            if activeNavigationInputMode != .touch
                || isControllerNavigationEnabled {
                activateTouchNavigation()
            } else {
                feedbackPlayer.releaseControllerResources()
            }
        }
        let liveProfiles = liveExtendedProfiles.union(liveMicroProfiles)
        let disconnectedSamplingProfiles = directionSamplingTasks.keys.filter {
            !liveProfiles.contains($0)
        }
        for profileID in disconnectedSamplingProfiles {
            directionSamplingTasks.removeValue(forKey: profileID)?.cancel()
        }
        heldDirections = heldDirections.filter { liveProfiles.contains($0.key) }
        directionalHoldStartTimes = directionalHoldStartTimes.filter {
            liveProfiles.contains($0.key)
        }
        lastDirectionEdges = lastDirectionEdges.filter {
            liveProfiles.contains($0.key)
        }
        pressedButtons = pressedButtons.filter { liveProfiles.contains($0.key) }
        emulationMacroLatches = emulationMacroLatches.filter {
            liveProfiles.contains($0.key)
        }
        let disconnectedEmulationRepeatProfiles = emulationMacroRepeatTasks.keys.filter {
                !liveProfiles.contains($0)
            }
        for profileID in disconnectedEmulationRepeatProfiles {
            cancelEmulationMacroRepeats(profileID: profileID)
        }
        refreshPressedFaceButton()
        let disconnectedRepeatProfiles = repeatTasks.keys.filter {
            !liveProfiles.contains($0)
        }
        for profileID in disconnectedRepeatProfiles {
            cancelRepeat(profileID: profileID)
        }
        if liveExtendedProfiles.isEmpty {
            updateRightStick(x: 0, y: 0)
        }
        if isMenuActive, isControllerNavigationEnabled {
            feedbackPlayer.attach(to: GCController.controllers())
        } else {
            feedbackPlayer.releaseControllerResources()
        }
    }

    private func installExtendedProfile(_ gamepad: GCExtendedGamepad) {
        gamepad.valueChangedHandler = { [weak self] gamepad, element in
            MainActor.assumeIsolated {
                self?.handleExtendedInput(gamepad, element: element)
            }
        }
    }

    private func installMicroProfile(_ gamepad: GCMicroGamepad) {
        gamepad.valueChangedHandler = { [weak self] gamepad, element in
            MainActor.assumeIsolated {
                self?.handleMicroInput(gamepad, element: element)
            }
        }
    }

    private func handleExtendedInput(
        _ gamepad: GCExtendedGamepad,
        element: GCControllerElement
    ) {
        if consumeLaunchInputIfNeeded(element) { return }
        let profileID = ObjectIdentifier(gamepad)

        // Gameplay macros are sampled from every profile callback so a custom
        // shoulder, trigger, face-button, stick-click, or direction binding is
        // recognized on the same physical edge. SDL polls the shared profile,
        // so a recognized chord is also marked consumed in the gameplay gate.
        if updateEmulationControllerMacros(gamepad, profileID: profileID) {
            return
        }

        guard isMenuActive else { return }

        if isDirectionalElement(element, of: gamepad.dpad)
            || isDirectionalElement(element, of: gamepad.leftThumbstick) {
            if ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled {
                return
            }
            scheduleDirectionUpdate(gamepad, profileID: profileID)
            return
        }

        if isDirectionalElement(element, of: gamepad.rightThumbstick) {
            updateRightStick(
                x: gamepad.rightThumbstick.xAxis.value,
                y: gamepad.rightThumbstick.yAxis.value
            )
            return
        }

        if element === gamepad.buttonA {
            if ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled {
                return
            }
            updateButton(.primary, pressed: gamepad.buttonA.isPressed, command: .activate, profileID: profileID)
        } else if element === gamepad.buttonB {
            if ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled {
                return
            }
            updateButton(.secondary, pressed: gamepad.buttonB.isPressed, command: .back, profileID: profileID)
        } else if element === gamepad.buttonX {
            updateButton(.favorite, pressed: gamepad.buttonX.isPressed, command: .toggleFavorite, profileID: profileID)
        } else if element === gamepad.buttonY {
            updateButton(.info, pressed: gamepad.buttonY.isPressed, command: .showContextMenu, profileID: profileID)
        } else if element === gamepad.leftShoulder {
            updateButton(.previousTab, pressed: gamepad.leftShoulder.isPressed, command: .previousTab, profileID: profileID)
        } else if element === gamepad.rightShoulder {
            updateButton(.nextTab, pressed: gamepad.rightShoulder.isPressed, command: .nextTab, profileID: profileID)
        } else if element === gamepad.leftTrigger {
            updateThemePresetButton(.previousTheme, pressed: gamepad.leftTrigger.isPressed, step: -1, profileID: profileID)
        } else if element === gamepad.rightTrigger {
            updateThemePresetButton(.nextTheme, pressed: gamepad.rightTrigger.isPressed, step: 1, profileID: profileID)
        }
    }

    /// Theme shortcuts belong to the main tabs, never a gameplay overlay,
    /// keyboard, picker, or dialog. Keep them out of directional routing.
    var canChangeMainMenuTheme: Bool {
        isMenuActive
            && AppState.shared.currentScreen == .menu
            && AppState.shared.gameplayLaunchTransition == nil
            && launchInputInterceptor == nil
            && frontmostManualNavigationCapture == nil
            && (frontmostNavigationSessionEntry?.value.priority ?? 0) < 100
    }

    private func updateThemePresetButton(
        _ button: Button,
        pressed: Bool,
        step: Int,
        profileID: ObjectIdentifier
    ) {
        let wasPressed = pressedButtons[profileID, default: []].contains(button)
        if pressed {
            pressedButtons[profileID, default: []].insert(button)
        } else {
            pressedButtons[profileID, default: []].remove(button)
            themePresetRepeatTasks[profileID]?[button]?.cancel()
            themePresetRepeatTasks[profileID]?.removeValue(forKey: button)
            if themePresetRepeatTasks[profileID]?.isEmpty == true {
                themePresetRepeatTasks.removeValue(forKey: profileID)
            }
            return
        }
        guard pressed, !wasPressed, canChangeMainMenuTheme else { return }
        guard publishThemePresetRequest(step: step) else { return }

        themePresetRepeatTasks[profileID]?[button]?.cancel()
        let task = Task { @MainActor [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(420))
            } catch {
                return
            }
            var repeatInterval = 250
            while !Task.isCancelled {
                guard let self,
                      self.pressedButtons[profileID]?.contains(button) == true,
                      self.canChangeMainMenuTheme else { return }
                guard self.publishThemePresetRequest(step: step) else { return }
                do {
                    try await Task.sleep(
                        for: .milliseconds(repeatInterval)
                    )
                } catch {
                    return
                }
                // Deliberate trigger holds become a quick gallery scrub while
                // the first press remains a precise single-step selection.
                repeatInterval = max(70, Int(Double(repeatInterval) * 0.84))
            }
        }
        themePresetRepeatTasks[profileID, default: [:]][button] = task
    }

    private func cancelThemePresetRepeats() {
        for tasks in themePresetRepeatTasks.values {
            for task in tasks.values {
                task.cancel()
            }
        }
        themePresetRepeatTasks.removeAll(keepingCapacity: true)
    }

    @discardableResult
    private func publishThemePresetRequest(step: Int) -> Bool {
        guard prepareControllerNavigationForInput() else { return false }
        sequence &+= 1
        latestThemePresetRequest = .init(sequence: sequence, step: step)
        return true
    }

    /// Mirrors an L2/R2 theme step for a direct-touch gesture without making
    /// controller focus visible. The shared overlay still owns selection,
    /// confirmation, wrapping, audio, and haptics.
    func requestThemePresetChangeFromTouch(step: Int) {
        guard step != 0,
              Date.timeIntervalSinceReferenceDate
                >= touchThemePresetShortcutSuppressedUntil,
              canChangeMainMenuTheme else { return }
        noteTouchInput()
        guard pendingNavigationModeSwitchRequest == nil,
              activeNavigationInputMode == .touch else { return }
        sequence &+= 1
        latestThemePresetRequest = .init(sequence: sequence, step: step)
    }

    /// A SwiftUI game card owns its custom long press without exposing a
    /// `UIContextMenuInteraction` to RootView's window recognizer. Suppress the
    /// deferred empty-space shortcut for this gesture transaction only.
    func suppressTouchThemePresetShortcut() {
        touchThemePresetShortcutSuppressedUntil = max(
            touchThemePresetShortcutSuppressedUntil,
            // Card recognizers suppress from touch-down, before their 420 ms
            // stationary-hold threshold. Keep the claim through the release
            // transaction so RootView's window recognizer cannot interpret
            // the same hold as an empty-space L2/R2 shortcut.
            Date.timeIntervalSinceReferenceDate + 1.0
        )
    }

    private func handleMicroInput(
        _ gamepad: GCMicroGamepad,
        element: GCControllerElement
    ) {
        if consumeLaunchInputIfNeeded(element) { return }
        guard isMenuActive else { return }
        let profileID = ObjectIdentifier(gamepad)

        if isDirectionalElement(element, of: gamepad.dpad) {
            if ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled {
                return
            }
            scheduleDirectionUpdate(gamepad, profileID: profileID)
        } else if element === gamepad.buttonA {
            if ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled {
                return
            }
            updateButton(.primary, pressed: gamepad.buttonA.isPressed, command: .activate, profileID: profileID)
        } else if element === gamepad.buttonX {
            if ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled {
                return
            }
            updateButton(.secondary, pressed: gamepad.buttonX.isPressed, command: .back, profileID: profileID)
        }
    }

    /// Some controllers publish the aggregate direction pad or an axis, while
    /// others publish the individual up/down/left/right button. Treat all of
    /// those callbacks as one directional source. This also makes simulator
    /// virtual-controller input match physical D-pads and thumbsticks.
    private func isDirectionalElement(
        _ element: GCControllerElement,
        of directionPad: GCControllerDirectionPad
    ) -> Bool {
        element === directionPad
            || element === directionPad.xAxis
            || element === directionPad.yAxis
            || element === directionPad.up
            || element === directionPad.down
            || element === directionPad.left
            || element === directionPad.right
    }

    private func consumeLaunchInputIfNeeded(
        _ element: GCControllerElement
    ) -> Bool {
        guard let launchInputInterceptor else { return false }
        let isEngaged: Bool
        if let button = element as? GCControllerButtonInput {
            isEngaged = button.isPressed
        } else if let axis = element as? GCControllerAxisInput {
            isEngaged = abs(axis.value) >= 0.5
        } else if let directionPad = element as? GCControllerDirectionPad {
            isEngaged = abs(directionPad.xAxis.value) >= 0.5
                || abs(directionPad.yAxis.value) >= 0.5
        } else {
            isEngaged = false
        }
        if isEngaged { launchInputInterceptor() }
        return true
    }

    private func extendedDirection(
        for gamepad: GCExtendedGamepad
    ) -> MenuControllerCommand? {
        let dpadX = gamepad.dpad.xAxis.value
        let dpadY = gamepad.dpad.yAxis.value
        if abs(dpadX) >= 0.5 || abs(dpadY) >= 0.5 {
            return direction(x: dpadX, y: dpadY)
        }
        return direction(
            x: gamepad.leftThumbstick.xAxis.value,
            y: gamepad.leftThumbstick.yAxis.value
        )
    }

    /// GameController may publish X, Y, and aggregate callbacks separately for
    /// one physical diagonal. Sample once after that small callback burst so a
    /// diagonal press cannot first emit a cardinal move and then a second
    /// diagonal move.
    private func scheduleDirectionUpdate(
        _ gamepad: GCExtendedGamepad,
        profileID: ObjectIdentifier
    ) {
        directionSamplingTasks[profileID]?.cancel()
        directionSamplingTasks[profileID] = Task { @MainActor [weak self, weak gamepad] in
            await Task.yield()
            guard let self, let gamepad, !Task.isCancelled else { return }
            self.directionSamplingTasks[profileID] = nil
            self.updateDirection(
                self.extendedDirection(for: gamepad),
                profileID: profileID
            )
        }
    }

    private func scheduleDirectionUpdate(
        _ gamepad: GCMicroGamepad,
        profileID: ObjectIdentifier
    ) {
        directionSamplingTasks[profileID]?.cancel()
        directionSamplingTasks[profileID] = Task { @MainActor [weak self, weak gamepad] in
            await Task.yield()
            guard let self, let gamepad, !Task.isCancelled else { return }
            self.directionSamplingTasks[profileID] = nil
            self.updateDirection(
                self.direction(
                    x: gamepad.dpad.xAxis.value,
                    y: gamepad.dpad.yAxis.value
                ),
                profileID: profileID
            )
        }
    }

    private func cancelDirectionSampling() {
        for task in directionSamplingTasks.values { task.cancel() }
        directionSamplingTasks.removeAll(keepingCapacity: true)
    }

    private func direction(x: Float, y: Float) -> MenuControllerCommand? {
        let horizontal = abs(x)
        let vertical = abs(y)
        guard max(horizontal, vertical) >= 0.58 else { return nil }

        // Split the plane into eight 45-degree sectors. tan(22.5 degrees)
        // is the boundary between a cardinal sector and its diagonal neighbor.
        // D-pad diagonals and analog diagonals therefore reach the same focus
        // geometry instead of being collapsed to whichever axis is fractionally
        // larger on the latest hardware callback.
        let diagonalRatio: Float = 0.414_213_56
        let isDiagonal = min(horizontal, vertical)
            / max(horizontal, vertical) >= diagonalRatio
        if isDiagonal {
            switch (x < 0, y < 0) {
            case (true, true): return .downLeft
            case (false, true): return .downRight
            case (true, false): return .upLeft
            case (false, false): return .upRight
            }
        }
        if horizontal > vertical { return x < 0 ? .left : .right }
        return y < 0 ? .down : .up
    }

    private func updateDirection(
        _ direction: MenuControllerCommand?,
        profileID: ObjectIdentifier
    ) {
        let previous = heldDirections[profileID]
        guard previous != direction else { return }
        heldDirections[profileID] = direction

        cancelRepeat(profileID: profileID)
        guard let direction else {
            directionalHoldStartTimes.removeValue(forKey: profileID)
            return
        }

        // Analog sticks can briefly cross the dead-zone while held and some
        // controllers publish both the aggregate D-pad and its axis edge. Treat
        // a rapid same-direction re-entry as the same physical detent so one
        // press cannot skip an otherwise reachable row.
        let now = ProcessInfo.processInfo.systemUptime
        directionalHoldStartTimes[profileID] = now
        let isDuplicateEdge = lastDirectionEdges[profileID].map {
            $0.command == direction && now - $0.timestamp < 0.12
        } ?? false
        if !isDuplicateEdge {
            lastDirectionEdges[profileID] = (direction, now)
            emit(direction)
        }
        repeatTasks[profileID] = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(320))
            guard let self, !Task.isCancelled else { return }

            while !Task.isCancelled,
                  self.isMenuActive,
                  self.heldDirections[profileID] == direction {
                let heldDuration = ProcessInfo.processInfo.systemUptime
                    - (self.directionalHoldStartTimes[profileID]
                        ?? ProcessInfo.processInfo.systemUptime)
                let acceleration = self.directionalAcceleration(
                    heldDuration: heldDuration
                )
                self.emit(
                    direction,
                    isRepeat: true,
                    repeatAcceleration: acceleration
                )
                let interval = max(0.04, 0.10 / acceleration)
                try? await Task.sleep(
                    for: .milliseconds(Int((interval * 1_000).rounded()))
                )
            }
        }
    }

    /// Accelerates a sustained focus traversal without changing the precise
    /// first press. Smoothstep avoids a sudden jump when key repeat begins,
    /// then reaches 25 moves/second after a deliberate long hold.
    private func directionalAcceleration(
        heldDuration: TimeInterval
    ) -> Double {
        let progress = min(1, max(0, (heldDuration - 0.32) / 1.35))
        let eased = progress * progress * (3 - (2 * progress))
        return 1 + (1.5 * eased)
    }

    private func updateButton(
        _ button: Button,
        pressed: Bool,
        command: MenuControllerCommand,
        profileID: ObjectIdentifier
    ) {
        var buttons = pressedButtons[profileID, default: []]
        let wasPressed = buttons.contains(button)
        if pressed {
            buttons.insert(button)
        } else {
            buttons.remove(button)
        }
        pressedButtons[profileID] = buttons
        refreshPressedFaceButton()

        if pressed && !wasPressed {
            emit(command)
        }
    }

    /// Gameplay macros never participate in menu focus routing. This is
    /// important: opening a confirmation surface from a save/load chord must
    /// not also deliver its face button into the newly mounted alert.
    @discardableResult
    private func updateEmulationControllerMacros(
        _ gamepad: GCExtendedGamepad,
        profileID: ObjectIdentifier
    ) -> Bool {
        guard !isMenuActive, ARMSX2Bridge.isVMRunning() else {
            emulationMacroLatches.removeValue(forKey: profileID)
            cancelEmulationMacroRepeats(profileID: profileID)
            return false
        }

        let settings = SettingsStore.shared
        var active = Set<ControllerMacroAction>()
        for action in ControllerMacroAction.allCases
        where settings.controllerMacroBinding(for: action).isPressed(on: gamepad) {
            active.insert(action)
        }

        let previous = emulationMacroLatches[profileID, default: []]
        for action in active.subtracting(previous) {
            let binding = settings.controllerMacroBinding(for: action)
            ARMSX2Bridge.consumeControllerMacroInput(mask: binding.gameplayInputMask)
            publishControllerMacro(action)
            if action.repeatsWhileHeld {
                beginEmulationMacroRepeat(
                    action,
                    gamepad: gamepad,
                    profileID: profileID
                )
            }
        }
        for action in previous.subtracting(active) {
            cancelEmulationMacroRepeat(action, profileID: profileID)
        }

        if active.isEmpty {
            emulationMacroLatches.removeValue(forKey: profileID)
        } else {
            emulationMacroLatches[profileID] = active
        }
        return !active.isEmpty
    }

    private func publishControllerMacro(_ action: ControllerMacroAction) {
        if action == .quickMenu {
            sequence &+= 1
            latestQuickPauseRequest = MenuControllerQuickPauseRequest(
                sequence: sequence
            )
        } else if let shortcut = action.emulationShortcut {
            publishEmulationShortcut(shortcut)
        }
    }

    private func beginEmulationMacroRepeat(
        _ action: ControllerMacroAction,
        gamepad: GCExtendedGamepad,
        profileID: ObjectIdentifier
    ) {
        cancelEmulationMacroRepeat(action, profileID: profileID)
        let task = Task { @MainActor [weak self, weak gamepad] in
            try? await Task.sleep(for: .milliseconds(320))
            guard let self, let gamepad, !Task.isCancelled else { return }
            while !Task.isCancelled,
                  !self.isMenuActive,
                  ARMSX2Bridge.isVMRunning(),
                  SettingsStore.shared.controllerMacroBinding(for: action)
                    .isPressed(on: gamepad),
                  self.emulationMacroLatches[profileID]?.contains(action) == true {
                self.publishControllerMacro(action)
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
        emulationMacroRepeatTasks[profileID, default: [:]][action] = task
    }

    private func publishEmulationShortcut(
        _ shortcut: EmulationControllerShortcut
    ) {
        sequence &+= 1
        latestEmulationShortcutRequest = EmulationControllerShortcutRequest(
            sequence: sequence,
            shortcut: shortcut
        )
    }

    private func cancelEmulationMacroRepeat(
        _ action: ControllerMacroAction,
        profileID: ObjectIdentifier
    ) {
        emulationMacroRepeatTasks[profileID]?.removeValue(forKey: action)?.cancel()
        if emulationMacroRepeatTasks[profileID]?.isEmpty == true {
            emulationMacroRepeatTasks.removeValue(forKey: profileID)
        }
    }

    private func cancelEmulationMacroRepeats(profileID: ObjectIdentifier) {
        guard let tasks = emulationMacroRepeatTasks
            .removeValue(forKey: profileID)?.values else { return }
        for task in tasks {
            task.cancel()
        }
    }

    private func cancelEmulationShortcutInput() {
        for tasks in emulationMacroRepeatTasks.values {
            for task in tasks.values { task.cancel() }
        }
        emulationMacroRepeatTasks.removeAll(keepingCapacity: true)
        emulationMacroLatches.removeAll(keepingCapacity: true)
    }

    private func refreshPressedFaceButton() {
        let allButtons = pressedButtons.values.reduce(into: Set<Button>()) {
            $0.formUnion($1)
        }
        if allButtons.contains(.info) {
            pressedFaceButton = .triangle
        } else if allButtons.contains(.secondary) {
            pressedFaceButton = .circle
        } else if allButtons.contains(.primary) {
            pressedFaceButton = .cross
        } else {
            pressedFaceButton = nil
        }
    }

    private func emit(
        _ command: MenuControllerCommand,
        isRepeat: Bool = false,
        repeatAcceleration: Double = 1
    ) {
        guard isMenuActive else { return }
        guard prepareControllerNavigationForInput() else { return }
        if command.isSpatialDirection, !isRepeat {
            let now = ProcessInfo.processInfo.systemUptime
            if let previous = lastPublishedDirectionalEdge,
               previous.command == command,
               now - previous.timestamp < 0.12 {
                return
            }
            lastPublishedDirectionalEdge = (command, now)
        }
        let previousRepeat = isRepeatingDirectionCommand
        let previousAcceleration = directionalRepeatAcceleration
        isRepeatingDirectionCommand = isRepeat
        directionalRepeatAcceleration = max(1, repeatAcceleration)
        defer {
            isRepeatingDirectionCommand = previousRepeat
            directionalRepeatAcceleration = previousAcceleration
        }

        sequence &+= 1
        let eventSequence = sequence

        // Resolve one immutable owner before delivering the event. Publishing
        // into SwiftUI can mount Settings and change the navigation zone, so a
        // second owner lookup afterward could route the same physical press
        // into Language or another newly mounted target.
        if let captureOwner = frontmostManualNavigationCapture?.key {
            publishEvent(
                command,
                sequence: eventSequence,
                captureOwner: captureOwner
            )
            return
        }

        if navigationZone != .tabBar,
           let sessionEntry = frontmostNavigationSessionEntry {
            // An entry wait only owns its intended page, never a modal above
            // it, another tab, or the tab bar.
            if command.isSpatialDirection, hasPendingEntry(for: sessionEntry.key) {
                schedulePendingNavigationSessionEntryIfReady()
                return
            }
            _ = dispatchToNavigationSession(
                command,
                sequence: eventSequence,
                entry: sessionEntry
            )
            return
        }

        publishEvent(command, sequence: eventSequence, captureOwner: nil)
    }

    private func publishEvent(
        _ command: MenuControllerCommand,
        sequence: UInt64,
        captureOwner: String?
    ) {
        latestEventTimestamp = Date.timeIntervalSinceReferenceDate
        latestEvent = MenuControllerInputEvent(
            sequence: sequence,
            command: command,
            captureOwner: captureOwner,
            isNavigationSessionRouted: false
        )
    }

    private func dispatchToNavigationSession(
        _ command: MenuControllerCommand,
        sequence: UInt64,
        entry: (key: UUID, value: NavigationSessionTarget)
    ) -> Bool {
        let target = entry.value
        // The frontmost presentation still owns the command while its lazy
        // controls materialize. Do not leak that press to a retained screen
        // underneath it, and do not invoke an empty focus graph.
        if !target.isReady {
            // Shoulder navigation is presentation-level and does not depend on
            // lazy Form rows having entered the accessibility graph yet.
            if command == .previousTab
                || command == .nextTab
                || command == .back {
                return target.handler(command)
            }
            // Activation and contextual commands are edge-triggered actions.
            // Dropping them while no target exists is safer than replaying them
            // after the entry request has focused the first Settings row.
            guard command.isSpatialDirection else { return true }
            pendingNavigationSessionCommands[entry.key] = PendingNavigationSessionCommand(
                sequence: sequence,
                scopeKey: target.scopeKey,
                command: command,
                isRepeat: isRepeatingDirectionCommand,
                repeatAcceleration: directionalRepeatAcceleration
            )
            return true
        }
        return target.handler(command)
    }

    private var frontmostNavigationSession: NavigationSessionTarget? {
        frontmostNavigationSessionEntry?.value
    }

    /// Resolves an entry request only against the newly selected retained
    /// page. A tab transition can briefly leave the previous session
    /// registered for one render pass; falling back to that session would put
    /// focus on invisible controls underneath the destination tab.
    private func navigationSessionEntry(
        matchingScopePrefix scopePrefix: String?
    ) -> (key: UUID, value: NavigationSessionTarget)? {
        guard let scopePrefix else { return frontmostNavigationSessionEntry }
        return navigationSessions
            .filter { $0.value.scopeKey.hasPrefix(scopePrefix) }
            .max { lhs, rhs in
                if lhs.value.priority != rhs.value.priority {
                    return lhs.value.priority < rhs.value.priority
                }
                return lhs.value.registrationOrder
                    < rhs.value.registrationOrder
            }
    }

    private var frontmostNavigationSessionEntry: (
        key: UUID,
        value: NavigationSessionTarget
    )? {
        navigationSessions.max { lhs, rhs in
            if lhs.value.priority != rhs.value.priority {
                return lhs.value.priority < rhs.value.priority
            }
            return lhs.value.registrationOrder < rhs.value.registrationOrder
        }
    }

    private func updateRightStick(x: Float, y: Float) {
        let vector = MenuControllerScrollVector(
            x: normalizedRightStickAxis(x),
            y: normalizedRightStickAxis(y)
        )
        guard vector != rightStickVector else { return }
        let beganScrolling = rightStickVector.isZero && !vector.isZero
        let endedScrolling = !rightStickVector.isZero && vector.isZero
        let changedDirection = !rightStickVector.isZero && !vector.isZero
            && (rightStickVector.x * vector.x
                + rightStickVector.y * vector.y) <= 0
        if !vector.isZero {
            guard prepareControllerNavigationForInput() else {
                rightStickVector = .zero
                return
            }
        }
        rightStickVector = vector
        if beganScrolling || changedDirection {
            rightStickHoldStartTime = CACurrentMediaTime()
        }
        if beganScrolling {
            requestFocusRelease()
        }
        dispatchRightStickVector()
        if endedScrolling {
            rightStickHoldStartTime = nil
            // Publish after every registered target has consumed `.zero`, so
            // focus restoration observes the final UIKit content offset.
            rightStickScrollEndSequence &+= 1
        }
    }

    /// Right-stick scrolling is continuous, so accelerate velocity instead of
    /// producing more input events. The first quarter-second remains precise;
    /// a sustained deflection then ramps smoothly to 3.25x speed.
    fileprivate func rightStickScrollAcceleration(
        at timestamp: CFTimeInterval
    ) -> CGFloat {
        guard let rightStickHoldStartTime else { return 1 }
        let progress = min(
            1,
            max(0, (timestamp - rightStickHoldStartTime - 0.25) / 1.25)
        )
        let eased = progress * progress * (3 - (2 * progress))
        return CGFloat(1 + (2.25 * eased))
    }

    func handleNativeBackPress() -> Bool {
        guard isMenuActive else { return false }
        emit(.back)
        return true
    }

    /// Keyboard arrows arrive through UIKit instead of a GCController profile.
    /// Route them into the same graph while custom navigation owns the menu;
    /// controller-originated UIKit duplicates are consumed by the event host.
    func handleKeyboardDirectionalPress(_ command: MenuControllerCommand) {
        guard isMenuActive,
              command.isSpatialDirection,
              !ControllerEventDeliveryCoordinator.shared.nativeFocusEnabled
        else {
            return
        }
        emit(command)
    }

    private func updateControllerDeliveryPreference() {
        let foregroundSession = frontmostNavigationSessionEntry?.value
        let nativeFocusOwnerIsReady = isMenuActive
            && isControllerNavigationEnabled
            && activeNavigationInputMode == .controller
            && launchInputInterceptor == nil
            && frontmostManualNavigationCapture == nil
            && navigationZone != .tabBar
            && foregroundSession?.usesNativeFocusEngine == true
            && foregroundSession?.isReady == true
        ControllerEventDeliveryCoordinator.shared.setNativeFocusAuthorized(
            nativeFocusOwnerIsReady
        )
    }

    private func normalizedRightStickAxis(_ value: Float) -> Float {
        let deadZone: Float = 0.16
        let magnitude = abs(value)
        guard magnitude > deadZone else { return 0 }
        let normalized = min(1, (magnitude - deadZone) / (1 - deadZone))
        return value < 0 ? -normalized : normalized
    }

    private func dispatchRightStickVector() {
        globalScrollDriver.accelerationProvider = { [weak self] timestamp in
            self?.rightStickScrollAcceleration(at: timestamp) ?? 1
        }
        let activeCaptureOwner = frontmostManualNavigationCapture?.key
        let activeSessionEntry = activeCaptureOwner == nil && navigationZone != .tabBar
            ? frontmostNavigationSessionEntry
            : nil
        if isRightStickScrolling,
           rightStickSessionOwner?.id != activeSessionEntry?.key
            || rightStickSessionOwner?.scopeKey != activeSessionEntry?.value.scopeKey {
            finishRightStickScrolling(restoresFocus: false)
        }
        let globalOwner = activeSessionEntry.map { (id: $0.key, scopeKey: $0.value.scopeKey) }
        globalScrollDriver.onScroll = { [weak self] scrollView, direction in
            self?.didScrollRightStick(
                in: scrollView, direction: direction, targetID: nil,
                ownerSessionID: globalOwner?.id, ownerScopeKey: globalOwner?.scopeKey
            )
        }
        globalScrollDriver.onStopped = { [weak self] in
            self?.rightStickDriverStopped(targetID: nil)
        }
        if let session = activeSessionEntry?.value {
            globalScrollDriver.setPreferredFocus(
                window: session.scrollWindow,
                frame: session.scrollFocusFrame,
                requiresPreferredFocus: true
            )
        } else {
            globalScrollDriver.setPreferredFocus(
                window: nil,
                frame: nil,
                requiresPreferredFocus: false
            )
        }
        guard isMenuActive, !rightStickVector.isZero else {
            activeRightStickTargetID = nil
            for target in rightStickTargets.values {
                _ = target.handler(.zero)
            }
            _ = globalScrollDriver.consume(.zero)
            finishRightStickScrolling()
            return
        }

        let activeSessionID = activeSessionEntry?.key
        let eligibleTargets = rightStickTargets.filter { _, target in
            if let activeCaptureOwner {
                return target.manualCaptureOwner == activeCaptureOwner
            }
            if let activeSessionID {
                return target.ownerSessionID == activeSessionID
            }
            return target.ownerSessionID == nil
                && target.manualCaptureOwner == nil
        }
        let sortedTargets = eligibleTargets.sorted { lhs, rhs in
            if let activeSessionID {
                let lhsOwned = lhs.value.ownerSessionID == activeSessionID
                let rhsOwned = rhs.value.ownerSessionID == activeSessionID
                if lhsOwned != rhsOwned { return lhsOwned }
            }
            if lhs.value.priority != rhs.value.priority {
                return lhs.value.priority > rhs.value.priority
            }
            if lhs.value.registrationOrder != rhs.value.registrationOrder {
                return lhs.value.registrationOrder
                    > rhs.value.registrationOrder
            }
            return lhs.key.uuidString < rhs.key.uuidString
        }
        var selectedTargetID: UUID?
        for (id, target) in sortedTargets {
            if target.handler(rightStickVector) {
                selectedTargetID = id
                break
            }
        }
        if activeRightStickTargetID != selectedTargetID {
            finishRightStickScrolling(restoresFocus: selectedTargetID == nil)
        }
        activeRightStickTargetID = selectedTargetID

        // Keep the selected target's display link alive as analog values
        // change. Only inactive or newly-covered surfaces receive a stop.
        for (id, target) in rightStickTargets where id != selectedTargetID {
            _ = target.handler(.zero)
        }
        let driverIsRunning: Bool
        if selectedTargetID != nil || activeCaptureOwner != nil || activeSessionEntry != nil {
            // A managed pane at its boundary must not fall through to a global
            // search and scroll a sibling column or retained page. Its local
            // markers will redispatch once a replacement surface mounts.
            _ = globalScrollDriver.consume(.zero)
            driverIsRunning = selectedTargetID != nil
        } else {
            driverIsRunning = globalScrollDriver.consume(rightStickVector)
        }
        if driverIsRunning {
            if !isRightStickScrolling {
                isRightStickScrolling = true
                rightStickSessionOwner = globalOwner
                lastRightStickScrollDirection = nil
                ControllerEventDeliveryCoordinator.shared.setRightStickScrolling(true)
                activeSessionEntry?.value.scrollStateHandler(true)
            }
        } else {
            finishRightStickScrolling()
        }
    }

    private func finishRightStickScrolling(restoresFocus: Bool = true) {
        let owner = rightStickSessionOwner
        let wasScrolling = isRightStickScrolling
        isRightStickScrolling = false
        rightStickSessionOwner = nil
        ControllerEventDeliveryCoordinator.shared.setRightStickScrolling(false)
        // Clear ownership before reconciliation: the final corridor reveal is
        // now directional scrolling, not a competing analog animation.
        if wasScrolling, restoresFocus, isControllerNavigationEnabled,
           navigationZone != .tabBar, frontmostManualNavigationCapture == nil,
           let owner, let target = navigationSessions[owner.id],
           target.scopeKey == owner.scopeKey,
           frontmostNavigationSessionEntry?.key == owner.id {
            target.scrollStateHandler(false)
        }
    }

    fileprivate func rightStickDriverStopped(targetID: UUID?) {
        guard activeRightStickTargetID == targetID else { return }
        finishRightStickScrolling()
    }

    fileprivate func navigationScope(for id: UUID?) -> String? {
        id.flatMap { navigationSessions[$0]?.scopeKey }
    }

    fileprivate func ownsRightStickScroll(
        targetID: UUID?, ownerSessionID: UUID?, ownerScopeKey: String?
    ) -> Bool {
        guard isRightStickScrolling, activeRightStickTargetID == targetID else { return false }
        guard let ownerSessionID else { return rightStickSessionOwner == nil }
        return navigationZone != .tabBar && frontmostManualNavigationCapture == nil
            && frontmostNavigationSessionEntry?.key == ownerSessionID
            && navigationSessions[ownerSessionID]?.scopeKey == ownerScopeKey
            && rightStickSessionOwner?.id == ownerSessionID
            && rightStickSessionOwner?.scopeKey == ownerScopeKey
    }

    private func cancelRepeat() {
        for task in repeatTasks.values { task.cancel() }
        repeatTasks.removeAll(keepingCapacity: true)
    }

    fileprivate func didScrollRightStick(
        in scrollView: UIScrollView,
        direction: MenuControllerCommand,
        targetID: UUID?,
        ownerSessionID: UUID?,
        ownerScopeKey: String?
    ) {
        guard isMenuActive, isControllerNavigationEnabled,
              isRightStickScrolling, activeRightStickTargetID == targetID else { return }
        lastRightStickScrollDirection = direction
        guard navigationZone != .tabBar,
              frontmostManualNavigationCapture == nil,
              let entry = frontmostNavigationSessionEntry, entry.value.isReady,
              entry.key == ownerSessionID,
              entry.value.scopeKey == ownerScopeKey,
              rightStickSessionOwner?.id == entry.key,
              rightStickSessionOwner?.scopeKey == entry.value.scopeKey else { return }
        entry.value.scrollHandler(scrollView, direction)
    }

    private func cancelRepeat(profileID: ObjectIdentifier) {
        repeatTasks.removeValue(forKey: profileID)?.cancel()
    }
}

/// Fallback for screens that do not need controller-specific focus plumbing.
/// It discovers the frontmost visible UIKit scroll surface and drives it with
/// the same display-link velocity as explicit SwiftUI scroll targets. Explicit
/// targets still win, which keeps retained content underneath sheets passive.
@MainActor
private final class MenuControllerGlobalScrollDriver {
    var onScroll: (@MainActor (UIScrollView, MenuControllerCommand) -> Void)?
    var onStopped: (@MainActor () -> Void)?
    var accelerationProvider: (@MainActor (CFTimeInterval) -> CGFloat)?
    private weak var scrollView: UIScrollView?
    private weak var preferredWindow: UIWindow?
    private var preferredFocusFrame: CGRect?
    private var requiresPreferredFocus = false
    private var vector = MenuControllerScrollVector.zero
    private var displayLink: CADisplayLink?
    private var lastTimestamp: CFTimeInterval?
    private let pointsPerSecond: CGFloat = 760

    func setPreferredFocus(
        window: UIWindow?,
        frame: CGRect?,
        requiresPreferredFocus: Bool
    ) {
        let unchanged = preferredWindow === window
            && preferredFocusFrame == frame
            && self.requiresPreferredFocus == requiresPreferredFocus
        guard !unchanged else { return }
        preferredWindow = window
        preferredFocusFrame = frame
        self.requiresPreferredFocus = requiresPreferredFocus
        guard let scrollView, let window, let frame else {
            self.scrollView = nil
            return
        }
        let focusPoint = CGPoint(x: frame.midX, y: frame.midY)
        let scrollFrame = scrollView.convert(scrollView.bounds, to: window)
        if !scrollFrame.contains(focusPoint) {
            self.scrollView = nil
        }
    }

    func consume(_ vector: MenuControllerScrollVector) -> Bool {
        if vector.isZero {
            stopScrolling()
            return false
        }
        let target: UIScrollView
        if let scrollView,
           scrollView.window != nil,
           canScroll(scrollView, for: vector) {
            target = scrollView
        } else if let resolved = resolveScrollView(for: vector) {
            target = resolved
        } else {
            stopScrolling()
            return false
        }
        scrollView = target
        self.vector = vector
        startDisplayLinkIfNeeded()
        return true
    }

    private func startDisplayLinkIfNeeded() {
        guard displayLink == nil else { return }
        lastTimestamp = nil
        let link = CADisplayLink(target: self, selector: #selector(step(_:)))
        UIFrameRateSettings.shared.configuration.apply(
            to: link,
            domain: .controllerNavigation
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private func stopScrolling() {
        vector = .zero
        scrollView = nil
        displayLink?.invalidate()
        displayLink = nil
        lastTimestamp = nil
    }

    @objc private func step(_ link: CADisplayLink) {
        guard !vector.isZero else {
            stopScrolling()
            onStopped?()
            return
        }
        if scrollView?.window == nil || !(scrollView.map {
            canScroll($0, for: vector)
        } ?? false) {
            scrollView = resolveScrollView(for: vector)
        }
        guard let scrollView else {
            stopScrolling()
            onStopped?()
            return
        }

        let previousTimestamp = lastTimestamp ?? link.timestamp
        let elapsed = min(1.0 / 30.0, link.timestamp - previousTimestamp)
        lastTimestamp = link.timestamp

        let limits = contentOffsetLimits(for: scrollView)
        let delta = scrollDelta(for: scrollView, vector: vector)
        let acceleration = accelerationProvider?(link.timestamp) ?? 1
        var offset = scrollView.contentOffset
        offset.x = min(
            limits.maximumX,
            max(
                limits.minimumX,
                offset.x + delta.x * pointsPerSecond * acceleration * elapsed
            )
        )
        offset.y = min(
            limits.maximumY,
            max(
                limits.minimumY,
                offset.y + delta.y * pointsPerSecond * acceleration * elapsed
            )
        )
        scrollView.setContentOffset(offset, animated: false)
        onScroll?(scrollView, abs(delta.y) >= abs(delta.x)
            ? (delta.y < 0 ? .up : .down)
            : (delta.x < 0 ? .left : .right))
        if !canScroll(scrollView, for: vector) {
            stopScrolling()
            onStopped?()
        }
    }

    private func resolveScrollView(
        for vector: MenuControllerScrollVector
    ) -> UIScrollView? {
        let scenes = UIApplication.shared.connectedScenes.compactMap {
            $0 as? UIWindowScene
        }
        let windows = scenes.flatMap(\.windows)
            .filter { !$0.isHidden && $0.alpha >= 0.01 }
            .sorted { lhs, rhs in
                if lhs.isKeyWindow != rhs.isKeyWindow {
                    return lhs.isKeyWindow
                }
                return lhs.windowLevel.rawValue > rhs.windowLevel.rawValue
            }

        if let preferredWindow,
           windows.contains(where: { $0 === preferredWindow }) {
            let visible = visibleScrollViews(
                in: preferredWindow,
                window: preferredWindow
            )
            let scrollable = visible.filter { canScroll($0, for: vector) }
            if let preferredFocusFrame {
                let point = CGPoint(
                    x: preferredFocusFrame.midX,
                    y: preferredFocusFrame.midY
                )
                let containing = scrollable.filter {
                    $0.convert($0.bounds, to: preferredWindow).contains(point)
                }
                if let target = containing.min(by: {
                    let lhs = $0.convert($0.bounds, to: preferredWindow)
                    let rhs = $1.convert($1.bounds, to: preferredWindow)
                    return lhs.width * lhs.height < rhs.width * rhs.height
                }) {
                    return target
                }
            }

            // Releasing controller focus for analog scrolling intentionally
            // leaves only the active session/window as an ownership boundary.
            // Use its frontmost visible scroll surface rather than failing when
            // the last focused row was remounted or lived in navigation chrome.
            if let target = scrollable.first {
                return target
            }
        }

        // A registered navigation surface must never fall through to a scroll
        // view in another retained tab or obscured presentation.
        if requiresPreferredFocus { return nil }

        for window in windows {
            if let target = visibleScrollViews(in: window, window: window)
                .first(where: { canScroll($0, for: vector) }) {
                return target
            }
        }
        return nil
    }

    private func visibleScrollViews(
        in view: UIView,
        window: UIWindow
    ) -> [UIScrollView] {
        guard !view.isHidden, view.alpha >= 0.01,
              !view.accessibilityElementsHidden,
              view.window != nil,
              view.bounds.width > 0, view.bounds.height > 0 else {
            return []
        }
        let visibleFrame = view.convert(view.bounds, to: window)
        guard visibleFrame.intersects(window.bounds) else { return [] }

        var result: [UIScrollView] = []
        for subview in view.subviews.reversed() {
            result.append(
                contentsOf: visibleScrollViews(in: subview, window: window)
            )
        }
        if let candidate = view as? UIScrollView,
           candidate.isScrollEnabled,
           candidate.isUserInteractionEnabled {
            result.append(candidate)
        }
        return result
    }

    private func canScroll(
        _ scrollView: UIScrollView,
        for vector: MenuControllerScrollVector
    ) -> Bool {
        let limits = contentOffsetLimits(for: scrollView)
        let epsilon: CGFloat = 0.5
        let delta = scrollDelta(for: scrollView, vector: vector)

        if delta.x != 0 {
            return delta.x < 0
                ? scrollView.contentOffset.x > limits.minimumX + epsilon
                : scrollView.contentOffset.x < limits.maximumX - epsilon
        }
        guard delta.y != 0 else { return false }
        return delta.y < 0
            ? scrollView.contentOffset.y > limits.minimumY + epsilon
            : scrollView.contentOffset.y < limits.maximumY - epsilon
    }

    private func scrollDelta(
        for scrollView: UIScrollView,
        vector: MenuControllerScrollVector
    ) -> CGPoint {
        let limits = contentOffsetLimits(for: scrollView)
        let hasHorizontalRange = limits.maximumX - limits.minimumX > 0.5
        let hasVerticalRange = limits.maximumY - limits.minimumY > 0.5

        if hasHorizontalRange,
           (!hasVerticalRange || abs(vector.x) > abs(vector.y)) {
            let amount = abs(vector.x) >= abs(vector.y)
                ? vector.x
                : -vector.y
            return CGPoint(x: CGFloat(amount), y: 0)
        }
        if hasVerticalRange {
            let amount = abs(vector.y) >= abs(vector.x)
                ? -vector.y
                : vector.x
            return CGPoint(x: 0, y: CGFloat(amount))
        }
        return .zero
    }

    private func contentOffsetLimits(
        for scrollView: UIScrollView
    ) -> (
        minimumX: CGFloat,
        maximumX: CGFloat,
        minimumY: CGFloat,
        maximumY: CGFloat
    ) {
        let inset = scrollView.adjustedContentInset
        let minimumX = -inset.left
        let minimumY = -inset.top
        return (
            minimumX,
            max(
                minimumX,
                scrollView.contentSize.width - scrollView.bounds.width
                    + inset.right
            ),
            minimumY,
            max(
                minimumY,
                scrollView.contentSize.height - scrollView.bounds.height
                    + inset.bottom
            )
        )
    }
}

/// A marker which either lives inside SwiftUI scroll content or covers one
/// scroll surface. It finds that surface's UIKit scroll view and applies
/// right-stick velocity without frame-by-frame Observation publications.
@MainActor
struct ControllerRightStickScrollTarget: UIViewRepresentable {
    let controllerInput: MenuControllerInputRouter?
    let axes: MenuControllerScrollAxes
    var manualCaptureOwner: String? = nil
    var priority = 0
    var isEnabled = true
    var pointsPerSecond: CGFloat = 760
    var onReachedLeadingEdge: (@MainActor () -> Void)? = nil
    var searchesNearbyScrollViews = false
    var preferredScrollViewProvider: (@MainActor () -> UIScrollView?)? = nil
    var ownerSessionIDOverride: UUID? = nil
    @Environment(\.controllerAccessibilityNavigationSession)
    private var navigationSession
    @Environment(\.controllerAccessibilityNavigationRegistrationID)
    private var navigationRegistrationID

    func makeUIView(context: Context) -> UIView {
        let view = RightStickScrollTargetView()
        view.configure(
            controllerInput: controllerInput,
            navigationSession: navigationSession,
            ownerSessionID: ownerSessionIDOverride
                ?? navigationRegistrationID
                ?? navigationSession?.registrationIdentifier,
            manualCaptureOwner: manualCaptureOwner,
            axes: axes,
            priority: priority,
            isEnabled: isEnabled,
            pointsPerSecond: pointsPerSecond,
            onReachedLeadingEdge: onReachedLeadingEdge,
            searchesNearbyScrollViews: searchesNearbyScrollViews,
            preferredScrollViewProvider: preferredScrollViewProvider
        )
        return view
    }

    func updateUIView(
        _ uiView: UIView,
        context: Context
    ) {
        guard let uiView = uiView as? RightStickScrollTargetView else { return }
        uiView.configure(
            controllerInput: controllerInput,
            navigationSession: navigationSession,
            ownerSessionID: ownerSessionIDOverride
                ?? navigationRegistrationID
                ?? navigationSession?.registrationIdentifier,
            manualCaptureOwner: manualCaptureOwner,
            axes: axes,
            priority: priority,
            isEnabled: isEnabled,
            pointsPerSecond: pointsPerSecond,
            onReachedLeadingEdge: onReachedLeadingEdge,
            searchesNearbyScrollViews: searchesNearbyScrollViews,
            preferredScrollViewProvider: preferredScrollViewProvider
        )
    }

    static func dismantleUIView(
        _ uiView: UIView,
        coordinator: Void
    ) {
        (uiView as? RightStickScrollTargetView)?.disconnect()
    }
}

@MainActor
private final class RightStickScrollTargetView: UIView, ControllerAccessibilityScrollSurface {
    private let registrationID = UUID()
    private weak var controllerInput: MenuControllerInputRouter?
    private weak var navigationSession: ControllerAccessibilityNavigationSession?
    private var ownerSessionID: UUID?
    private var manualCaptureOwner: String?
    private weak var scrollView: UIScrollView?
    private weak var focusOwnerScrollView: UIScrollView?
    private let associatedFocusTargets = NSMapTable<UIView, UIView>.weakToWeakObjects()
    private var scrollingScopeKey: String?
    private var axes: MenuControllerScrollAxes = .vertical
    private var isEnabled = true
    private var pointsPerSecond: CGFloat = 760
    private var vector = MenuControllerScrollVector.zero
    private var displayLink: CADisplayLink?
    private var lastTimestamp: CFTimeInterval?
    private var registeredPriority: Int?
    private var onReachedLeadingEdge: (@MainActor () -> Void)?
    private var notifiedLeadingEdge = false
    private var searchesNearbyScrollViews = false
    private var preferredScrollViewProvider:
        (@MainActor () -> UIScrollView?)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToSuperview() {
        super.didMoveToSuperview()
        scrollView = nil
        controllerInput?.rightStickTargetHierarchyDidChange()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        scrollView = nil
        controllerInput?.rightStickTargetHierarchyDidChange()
    }

    func configure(
        controllerInput: MenuControllerInputRouter?,
        navigationSession: ControllerAccessibilityNavigationSession?,
        ownerSessionID: UUID?,
        manualCaptureOwner: String?,
        axes: MenuControllerScrollAxes,
        priority: Int,
        isEnabled: Bool,
        pointsPerSecond: CGFloat,
        onReachedLeadingEdge: (@MainActor () -> Void)?,
        searchesNearbyScrollViews: Bool,
        preferredScrollViewProvider:
            (@MainActor () -> UIScrollView?)?
    ) {
        if self.navigationSession !== navigationSession {
            self.navigationSession?.unregisterScrollSurface(self)
            self.navigationSession = navigationSession
        }
        // Passive registration only: never focus or scroll during a SwiftUI
        // representable update. Resolution is lazy and shares the analog cache.
        navigationSession?.registerScrollSurface(self)
        let routerChanged = self.controllerInput !== controllerInput
        let axesChanged = self.axes != axes
        let scrollLookupChanged = self.searchesNearbyScrollViews
            != searchesNearbyScrollViews
        let registrationChanged = routerChanged
            || registeredPriority != priority
            || self.ownerSessionID != ownerSessionID
            || self.manualCaptureOwner != manualCaptureOwner
            || self.isEnabled != isEnabled
            || axesChanged
            || scrollLookupChanged
        if routerChanged {
            self.controllerInput?.unregisterRightStickTarget(
                id: registrationID
            )
            self.controllerInput = controllerInput
        }
        if axesChanged || scrollLookupChanged {
            scrollView = nil
            stopScrolling()
        }
        if self.ownerSessionID != ownerSessionID || self.isEnabled != isEnabled {
            associatedFocusTargets.removeAllObjects()
            focusOwnerScrollView = nil
            scrollView = nil
        }
        self.axes = axes
        self.ownerSessionID = ownerSessionID
        self.manualCaptureOwner = manualCaptureOwner
        self.isEnabled = isEnabled
        self.pointsPerSecond = pointsPerSecond
        self.onReachedLeadingEdge = onReachedLeadingEdge
        self.searchesNearbyScrollViews = searchesNearbyScrollViews
        self.preferredScrollViewProvider = preferredScrollViewProvider

        if registrationChanged {
            registeredPriority = priority
            controllerInput?.registerRightStickTarget(
                id: registrationID,
                priority: priority,
                ownerSessionID: ownerSessionID,
                manualCaptureOwner: manualCaptureOwner
            ) { [weak self] vector in
                self?.consume(vector) ?? false
            }
        }
        if !isEnabled {
            stopScrolling()
        }
    }

    func disconnect() {
        navigationSession?.unregisterScrollSurface(self)
        navigationSession = nil
        controllerInput?.unregisterRightStickTarget(id: registrationID)
        controllerInput = nil
        registeredPriority = nil
        stopScrolling()
    }

    func scrollViewForFocusTarget(_ target: UIView) -> UIScrollView? {
        guard isEnabled, axes.contains(.vertical), searchesNearbyScrollViews,
              preferredScrollViewProvider == nil,
              let window, target.window === window, isEffectivelyVisible(self),
              let candidate = enclosingScrollView(for: nil) else { return nil }
        if focusOwnerScrollView !== candidate {
            associatedFocusTargets.removeAllObjects()
            focusOwnerScrollView = candidate
        }
        let markerFrame = convert(bounds, to: window)
        let viewport = markerFrame.intersection(
            candidate.convert(candidate.bounds, to: window)
        )
        // A fixed footer or a target in another Per-Game column must not acquire
        // this Form's scroll owner just because both share the same UIWindow.
        let frame = target.convert(target.bounds, to: window)
        guard !viewport.isNull, let parent = target.superview,
              frame.maxX > viewport.minX, frame.minX < viewport.maxX else { return nil }
        if frame.intersects(viewport) {
            associatedFocusTargets.setObject(parent, forKey: target)
        } else if associatedFocusTargets.object(forKey: target) !== parent {
            return nil
        }
        // Remember the association independently of vertical visibility. A
        // glass portal can remain mounted after its row leaves the viewport;
        // scrolling it back still needs this owner. Weak parent identity and
        // column bounds prevent reuse by a reparented or sibling control.
        return candidate
    }

    private func consume(_ vector: MenuControllerScrollVector) -> Bool {
        if vector.isZero {
            stopScrolling()
            return false
        }
        let resolvedVector = mappedVector(vector)
        guard isEnabled,
              !resolvedVector.isZero,
              let scrollView = enclosingScrollView(for: resolvedVector),
              isEffectivelyVisible(scrollView),
              canScroll(scrollView, for: resolvedVector) else {
            stopScrolling()
            return false
        }

        self.vector = resolvedVector
        scrollingScopeKey = controllerInput?.navigationScope(for: ownerSessionID)
        notifiedLeadingEdge = false
        startDisplayLinkIfNeeded()
        return true
    }

    private func isEffectivelyVisible(_ candidate: UIView) -> Bool {
        guard let window = candidate.window else { return false }
        var view: UIView? = candidate
        while let current = view {
            if current.isHidden
                || current.alpha < 0.01
                || current.accessibilityElementsHidden {
                return false
            }
            view = current.superview
        }
        let visibleFrame = candidate.convert(candidate.bounds, to: window)
        return visibleFrame.width > 0
            && visibleFrame.height > 0
            && visibleFrame.intersects(window.bounds)
    }

    private func enclosingScrollView(
        for vector: MenuControllerScrollVector?
    ) -> UIScrollView? {
        // A managed navigation session knows the exact scroll ancestor of its
        // focused probe. Prefer it over hierarchy-wide searches, which can
        // select a sibling Form/List after SwiftUI lazily recycles rows.
        if let preferred = preferredScrollViewProvider?(),
           preferred.window != nil,
           isEffectivelyVisible(preferred),
           canScroll(preferred, for: nil) {
            scrollView = preferred
            return preferred
        }
        if let scrollView,
           scrollView.window != nil,
           isEffectivelyVisible(scrollView),
           canScroll(scrollView, for: nil),
           (isDescendant(of: scrollView)
                || isNearbyScrollView(scrollView)) {
            return scrollView
        }
        scrollView = nil
        var ancestorCandidates: [UIScrollView] = []
        var view = superview
        while let current = view {
            if let candidate = current as? UIScrollView,
               isEffectivelyVisible(candidate),
               canScroll(candidate, for: vector) {
                ancestorCandidates.append(candidate)
            }
            view = current.superview
        }
        if let resolved = preferredCandidate(from: ancestorCandidates) {
            scrollView = resolved
            return resolved
        }
        guard searchesNearbyScrollViews else { return nil }
        let resolved = nearbyScrollView(for: vector)
        scrollView = resolved
        return resolved
    }

    /// SwiftUI installs a Form/List background beside its private scroll view,
    /// not inside it. Walk only the marker's nearest visible hosting ancestors
    /// and select the vertical surface with the greatest viewport overlap.
    private func nearbyScrollView(
        for vector: MenuControllerScrollVector?
    ) -> UIScrollView? {
        guard searchesNearbyScrollViews, let window else { return nil }
        let markerFrame = convert(bounds, to: window)
        guard markerFrame.width > 1, markerFrame.height > 1 else { return nil }
        let markerCenter = CGPoint(x: markerFrame.midX, y: markerFrame.midY)
        var visited = Set<ObjectIdentifier>()
        var ancestor = superview

        for _ in 0..<8 {
            guard let root = ancestor, root !== window else { break }
            var candidates: [UIScrollView] = []
            func visit(_ view: UIView) {
                guard visited.insert(ObjectIdentifier(view)).inserted,
                      !view.isHidden,
                      view.alpha >= 0.01,
                      !view.accessibilityElementsHidden,
                      view.window === window else { return }
                let frame = view.convert(view.bounds, to: window)
                guard frame.intersects(markerFrame) else { return }
                for subview in view.subviews.reversed() { visit(subview) }
                if let candidate = view as? UIScrollView,
                   candidate.isScrollEnabled,
                   candidate.isUserInteractionEnabled,
                   canScroll(candidate, for: vector) {
                    candidates.append(candidate)
                }
            }
            visit(root)

            if let target = candidates.max(by: { lhs, rhs in
                let lhsFrame = lhs.convert(lhs.bounds, to: window)
                let rhsFrame = rhs.convert(rhs.bounds, to: window)
                let lhsContains = lhsFrame.contains(markerCenter)
                let rhsContains = rhsFrame.contains(markerCenter)
                if lhsContains != rhsContains { return !lhsContains }
                let lhsIntersection = lhsFrame.intersection(markerFrame)
                let rhsIntersection = rhsFrame.intersection(markerFrame)
                let lhsArea = lhsIntersection.isNull
                    ? 0 : lhsIntersection.width * lhsIntersection.height
                let rhsArea = rhsIntersection.isNull
                    ? 0 : rhsIntersection.width * rhsIntersection.height
                return lhsArea < rhsArea
            }) {
                return target
            }
            ancestor = root.superview
        }
        return nil
    }

    private func preferredCandidate(
        from candidates: [UIScrollView]
    ) -> UIScrollView? {
        guard let window else { return candidates.last }
        return candidates.max { lhs, rhs in
            let lhsFrame = lhs.convert(lhs.bounds, to: window)
                .intersection(window.bounds)
            let rhsFrame = rhs.convert(rhs.bounds, to: window)
                .intersection(window.bounds)
            let lhsArea = lhsFrame.isNull ? 0 : lhsFrame.width * lhsFrame.height
            let rhsArea = rhsFrame.isNull ? 0 : rhsFrame.width * rhsFrame.height
            if abs(lhsArea - rhsArea) > 0.5 { return lhsArea < rhsArea }
            return scrollableRange(of: lhs) < scrollableRange(of: rhs)
        }
    }

    private func isNearbyScrollView(_ candidate: UIScrollView) -> Bool {
        guard searchesNearbyScrollViews,
              let window,
              candidate.window === window else { return false }
        let markerFrame = convert(bounds, to: window)
        let candidateFrame = candidate.convert(candidate.bounds, to: window)
        return markerFrame.intersects(candidateFrame)
    }

    private func startDisplayLinkIfNeeded() {
        if let displayLink {
            displayLink.isPaused = false
            return
        }
        lastTimestamp = nil
        let link = CADisplayLink(target: self, selector: #selector(step(_:)))
        UIFrameRateSettings.shared.configuration.apply(
            to: link,
            domain: .controllerNavigation
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private func stopScrolling() {
        vector = .zero
        displayLink?.invalidate()
        displayLink = nil
        lastTimestamp = nil
    }

    private func pauseScrollingAtBoundary() {
        displayLink?.isPaused = true
        lastTimestamp = nil
        controllerInput?.rightStickDriverStopped(targetID: registrationID)
    }

    @objc private func step(_ link: CADisplayLink) {
        guard isEnabled,
              controllerInput?.ownsRightStickScroll(
                  targetID: registrationID, ownerSessionID: ownerSessionID,
                  ownerScopeKey: scrollingScopeKey
              ) == true,
              let scrollView = enclosingScrollView(for: nil),
              isEffectivelyVisible(scrollView) else {
            stopScrolling()
            controllerInput?.rightStickTargetBecameUnavailable(
                id: registrationID
            )
            return
        }

        let previousTimestamp = lastTimestamp ?? link.timestamp
        let elapsed = min(1.0 / 30.0, link.timestamp - previousTimestamp)
        lastTimestamp = link.timestamp

        let inset = scrollView.adjustedContentInset
        let minimumX = -inset.left
        let minimumY = -inset.top
        let maximumX = max(
            minimumX,
            scrollView.contentSize.width - scrollView.bounds.width + inset.right
        )
        let maximumY = max(
            minimumY,
            scrollView.contentSize.height - scrollView.bounds.height + inset.bottom
        )
        let acceleration = controllerInput?.rightStickScrollAcceleration(
            at: link.timestamp
        ) ?? 1
        var offset = scrollView.contentOffset
        if axes.contains(.horizontal) {
            offset.x = min(
                maximumX,
                max(
                    minimumX,
                    offset.x + CGFloat(vector.x) * pointsPerSecond
                        * acceleration * elapsed
                )
            )
        }
        if axes.contains(.vertical) {
            offset.y = min(
                maximumY,
                max(
                    minimumY,
                    offset.y - CGFloat(vector.y) * pointsPerSecond
                        * acceleration * elapsed
                )
            )
        }
        scrollView.setContentOffset(offset, animated: false)

        controllerInput?.didScrollRightStick(
            in: scrollView,
            direction: axes.contains(.vertical) && vector.y != 0
                ? (vector.y > 0 ? .up : .down)
                : (vector.x < 0 ? .left : .right),
            targetID: registrationID,
            ownerSessionID: ownerSessionID,
            ownerScopeKey: scrollingScopeKey
        )

        let movingTowardLeadingEdge = (
            axes.contains(.horizontal)
                && CGFloat(vector.x) < 0
                && offset.x <= minimumX + 0.75
        ) || (
            axes.contains(.vertical)
                && CGFloat(vector.y) > 0
                && offset.y <= minimumY + 0.75
        )
        let movingTowardTrailingEdge = (
            axes.contains(.horizontal)
                && CGFloat(vector.x) > 0
                && offset.x >= maximumX - 0.75
        ) || (
            axes.contains(.vertical)
                && CGFloat(vector.y) < 0
                && offset.y >= maximumY - 0.75
        )
        if movingTowardLeadingEdge, !notifiedLeadingEdge {
            notifiedLeadingEdge = true
            pauseScrollingAtBoundary()
            onReachedLeadingEdge?()
        } else if movingTowardTrailingEdge {
            pauseScrollingAtBoundary()
        } else if !movingTowardLeadingEdge {
            notifiedLeadingEdge = false
        }
    }

    private func mappedVector(
        _ input: MenuControllerScrollVector
    ) -> MenuControllerScrollVector {
        if axes == .horizontal {
            let horizontal = abs(input.x) >= abs(input.y)
                ? input.x
                : -input.y
            return MenuControllerScrollVector(x: horizontal, y: 0)
        }
        if axes == .vertical {
            let vertical = abs(input.y) >= abs(input.x)
                ? input.y
                : -input.x
            return MenuControllerScrollVector(x: 0, y: vertical)
        }
        return input
    }

    private func hasScrollableRange(_ scrollView: UIScrollView) -> Bool {
        let inset = scrollView.adjustedContentInset
        let horizontalRange = scrollView.contentSize.width
            - scrollView.bounds.width + inset.left + inset.right
        let verticalRange = scrollView.contentSize.height
            - scrollView.bounds.height + inset.top + inset.bottom
        return (axes.contains(.horizontal) && horizontalRange > 0.5)
            || (axes.contains(.vertical) && verticalRange > 0.5)
    }

    private func scrollableRange(of scrollView: UIScrollView) -> CGFloat {
        let inset = scrollView.adjustedContentInset
        let horizontalRange = max(
            0,
            scrollView.contentSize.width - scrollView.bounds.width
                + inset.left + inset.right
        )
        let verticalRange = max(
            0,
            scrollView.contentSize.height - scrollView.bounds.height
                + inset.top + inset.bottom
        )
        if axes == .horizontal { return horizontalRange }
        if axes == .vertical { return verticalRange }
        return max(horizontalRange, verticalRange)
    }

    private func canScroll(
        _ scrollView: UIScrollView,
        for vector: MenuControllerScrollVector?
    ) -> Bool {
        guard hasScrollableRange(scrollView) else { return false }
        // Focus geometry still needs the owner at either content boundary,
        // even when that owner cannot consume another analog step.
        guard let vector else { return true }
        let inset = scrollView.adjustedContentInset
        let minimumX = -inset.left
        let minimumY = -inset.top
        let maximumX = max(
            minimumX,
            scrollView.contentSize.width - scrollView.bounds.width + inset.right
        )
        let maximumY = max(
            minimumY,
            scrollView.contentSize.height - scrollView.bounds.height + inset.bottom
        )
        let epsilon: CGFloat = 0.5

        if axes.contains(.horizontal), vector.x != 0 {
            return vector.x < 0
                ? scrollView.contentOffset.x > minimumX + epsilon
                : scrollView.contentOffset.x < maximumX - epsilon
        }
        guard axes.contains(.vertical), vector.y != 0 else { return false }
        // Vertical targets subtract the stick vector from contentOffset in
        // `step`: positive moves toward the leading edge, negative toward the
        // trailing edge.
        return vector.y > 0
            ? scrollView.contentOffset.y > minimumY + epsilon
            : scrollView.contentOffset.y < maximumY - epsilon
    }
}

/// Menu-only feedback modeled after OrbitKeys: UIKit generators give the
/// device a reliable tactile response while short Core Haptics patterns add a
/// matching sensation to every connected controller that supports rumble.
/// Controller engines exist only while the menu owns input.
@MainActor
private final class MenuControllerFeedbackPlayer {
    private final class ControllerEngine {
        let engine: CHHapticEngine
        var players: [MenuControllerFeedback: any CHHapticPatternPlayer] = [:]

        init(engine: CHHapticEngine) {
            self.engine = engine
        }
    }

    private var controllerEngines: [ControllerEngine] = []
    private var controllerIDs = Set<ObjectIdentifier>()
    private var selectionGenerator: UISelectionFeedbackGenerator?
    private var lightGenerator: UIImpactFeedbackGenerator?
    private var mediumGenerator: UIImpactFeedbackGenerator?
    private var rigidGenerator: UIImpactFeedbackGenerator?
    private var softGenerator: UIImpactFeedbackGenerator?
    private var notificationGenerator: UINotificationFeedbackGenerator?
    private var delayedDeviceFeedbackTask: Task<Void, Never>?
    private var controllerPatterns: [MenuControllerFeedback: CHHapticPattern] = [:]
    private var controllerPatternRumbleGain: Float?
    private var lastHapticTimes: [MenuControllerFeedback: TimeInterval] = [:]

    func attach(to controllers: [GCController]) {
        let nextIDs = Set(controllers.map(ObjectIdentifier.init))
        guard nextIDs != controllerIDs else { return }
        releaseControllerEngines()
        controllerIDs = nextIDs

        for controller in controllers {
            guard let haptics = controller.haptics else { continue }
            for locality in preferredLocalities(for: haptics) {
                guard let engine = haptics.createEngine(withLocality: locality) else {
                    continue
                }
                do {
                    engine.isAutoShutdownEnabled = false
                    try engine.start()
                    controllerEngines.append(ControllerEngine(engine: engine))
                } catch {
                    engine.stop(completionHandler: nil)
                }
            }
        }
    }

    func play(_ feedback: MenuControllerFeedback) {
        MenuAudioPackManager.shared.play(feedback)
        playHaptics(feedback)
    }

    func playHaptics(_ feedback: MenuControllerFeedback) {
        guard SettingsStore.shared.hapticFeedback, uiRumbleGain > 0,
              shouldEmitHaptic(feedback) else { return }
        refreshControllerPatternsForCurrentStrength()
        playDeviceFeedback(feedback)
        playControllerFeedback(feedback)
    }

    private func shouldEmitHaptic(_ feedback: MenuControllerFeedback) -> Bool {
        guard feedback != .silent else { return false }
        let now = ProcessInfo.processInfo.systemUptime
        let minimumInterval: TimeInterval
        switch feedback {
        case .move:
            minimumInterval = 0.025
        case .toggle:
            minimumInterval = 0.04
        default:
            minimumInterval = 0.08
        }
        if let previous = lastHapticTimes[feedback],
           now - previous < minimumInterval {
            return false
        }
        lastHapticTimes[feedback] = now
        return true
    }

    func releaseResources() {
        delayedDeviceFeedbackTask?.cancel()
        delayedDeviceFeedbackTask = nil
        selectionGenerator = nil
        lightGenerator = nil
        mediumGenerator = nil
        rigidGenerator = nil
        softGenerator = nil
        notificationGenerator = nil
        controllerPatterns.removeAll(keepingCapacity: false)
        controllerPatternRumbleGain = nil
        lastHapticTimes.removeAll(keepingCapacity: false)
        releaseControllerEngines()
    }

    /// Keep UIKit touch feedback lazy while releasing the substantially more
    /// expensive controller haptic engines and cached controller patterns.
    func releaseControllerResources() {
        controllerPatterns.removeAll(keepingCapacity: false)
        controllerPatternRumbleGain = nil
        releaseControllerEngines()
    }

    private var uiRumbleGain: Float {
        min(max(SettingsStore.shared.uiRumbleStrength, 0), 1) * 2
    }

    private func refreshControllerPatternsForCurrentStrength() {
        let gain = uiRumbleGain
        guard controllerPatternRumbleGain != gain else { return }
        controllerPatternRumbleGain = gain
        controllerPatterns.removeAll(keepingCapacity: true)
        for engine in controllerEngines {
            engine.players.removeAll(keepingCapacity: true)
        }
    }

    private func preferredLocalities(
        for haptics: GCDeviceHaptics
    ) -> [GCHapticsLocality] {
        let supported = haptics.supportedLocalities
        if supported.contains(.leftHandle), supported.contains(.rightHandle) {
            return [.leftHandle, .rightHandle]
        }
        if supported.contains(.handles) {
            return [.handles]
        }
        if supported.contains(.default) {
            return [.default]
        }
        return supported.first.map { [$0] } ?? []
    }

    private func playDeviceFeedback(_ feedback: MenuControllerFeedback) {
        delayedDeviceFeedbackTask?.cancel()
        delayedDeviceFeedbackTask = nil

        switch feedback {
        case .silent:
            return
        case .move(.up):
            impact(.light, intensity: 0.38)
        case .move(.down):
            impact(.soft, intensity: 0.48)
        case .move(.left):
            selection().selectionChanged()
            selection().prepare()
        case .move(.right):
            impact(.rigid, intensity: 0.30)
        case .move:
            impact(.light, intensity: 0.35)
        case .boundary:
            notification().notificationOccurred(.warning)
            notification().prepare()
        case .activate:
            impact(.rigid, intensity: 0.74)
        case .toggle(let isOn):
            impact(isOn ? .rigid : .soft, intensity: isOn ? 0.48 : 0.38)
        case .tabTransition:
            impact(.medium, intensity: 0.50)
        case .back:
            impact(.soft, intensity: 0.62)
        case .favorite(let isFavorite):
            notification().notificationOccurred(isFavorite ? .success : .warning)
            notification().prepare()
        case .contextMenu:
            impact(.medium, intensity: 0.62)
            scheduleDeviceImpact(.soft, intensity: 0.42, after: .milliseconds(58))
        case .submenu:
            impact(.light, intensity: 0.46)
            scheduleDeviceImpact(.rigid, intensity: 0.36, after: .milliseconds(48))
        case .destination:
            impact(.rigid, intensity: 0.66)
            scheduleDeviceImpact(.soft, intensity: 0.48, after: .milliseconds(62))
        case .previousTab:
            impact(.medium, intensity: 0.50)
            scheduleDeviceImpact(.light, intensity: 0.30, after: .milliseconds(52))
        case .nextTab:
            impact(.light, intensity: 0.30)
            scheduleDeviceImpact(.medium, intensity: 0.50, after: .milliseconds(52))
        }
    }

    private func playControllerFeedback(_ feedback: MenuControllerFeedback) {
        guard !controllerEngines.isEmpty else { return }
        let events = controllerEvents(for: feedback)
        guard !events.isEmpty else { return }
        let pattern: CHHapticPattern
        if let cached = controllerPatterns[feedback] {
            pattern = cached
        } else if let created = try? CHHapticPattern(events: events, parameters: []) {
            controllerPatterns[feedback] = created
            pattern = created
        } else {
            return
        }

        for controllerEngine in controllerEngines {
            do {
                let player: any CHHapticPatternPlayer
                if let cached = controllerEngine.players[feedback] {
                    player = cached
                } else {
                    let created = try controllerEngine.engine.makePlayer(with: pattern)
                    controllerEngine.players[feedback] = created
                    player = created
                }
                try player.start(atTime: CHHapticTimeImmediate)
            } catch {
                // Engines are normally kept running from attach(). Rebuild only
                // after an interruption/reset instead of restarting every move.
                controllerEngine.players.removeValue(forKey: feedback)
                guard (try? controllerEngine.engine.start()) != nil,
                      let replacement = try? controllerEngine.engine.makePlayer(
                          with: pattern
                      ) else { continue }
                controllerEngine.players[feedback] = replacement
                try? replacement.start(atTime: CHHapticTimeImmediate)
            }
        }
    }

    private func controllerEvents(
        for feedback: MenuControllerFeedback
    ) -> [CHHapticEvent] {
        switch feedback {
        case .silent:
            return []
        case .move(.up):
            return [transient(intensity: 0.10, sharpness: 0.72, time: 0)]
        case .move(.down):
            return [transient(intensity: 0.11, sharpness: 0.24, time: 0)]
        case .move(.left):
            return [transient(intensity: 0.08, sharpness: 0.44, time: 0)]
        case .move(.right):
            return [
                transient(intensity: 0.07, sharpness: 0.48, time: 0),
                transient(intensity: 0.09, sharpness: 0.58, time: 0.035),
            ]
        case .move:
            return [transient(intensity: 0.09, sharpness: 0.50, time: 0)]
        case .boundary:
            return [
                transient(intensity: 0.14, sharpness: 0.20, time: 0),
                transient(intensity: 0.08, sharpness: 0.18, time: 0.07),
            ]
        case .activate:
            return [transient(intensity: 0.30, sharpness: 0.64, time: 0)]
        case .toggle(let isOn):
            return isOn
                ? [
                    transient(intensity: 0.12, sharpness: 0.42, time: 0),
                    transient(intensity: 0.18, sharpness: 0.66, time: 0.045),
                ]
                : [
                    transient(intensity: 0.17, sharpness: 0.46, time: 0),
                    transient(intensity: 0.10, sharpness: 0.24, time: 0.045),
                ]
        case .tabTransition:
            return [
                transient(intensity: 0.15, sharpness: 0.36, time: 0),
                transient(intensity: 0.22, sharpness: 0.58, time: 0.055),
            ]
        case .back:
            return [transient(intensity: 0.17, sharpness: 0.16, time: 0)]
        case .favorite(let isFavorite):
            if isFavorite {
                return [
                    transient(intensity: 0.12, sharpness: 0.30, time: 0),
                    transient(intensity: 0.20, sharpness: 0.48, time: 0.055),
                    transient(intensity: 0.27, sharpness: 0.66, time: 0.11),
                ]
            }
            return [
                transient(intensity: 0.22, sharpness: 0.58, time: 0),
                transient(intensity: 0.12, sharpness: 0.30, time: 0.065),
            ]
        case .contextMenu:
            return [
                transient(intensity: 0.17, sharpness: 0.30, time: 0),
                transient(intensity: 0.24, sharpness: 0.50, time: 0.06),
            ]
        case .submenu:
            return [
                transient(intensity: 0.11, sharpness: 0.42, time: 0),
                transient(intensity: 0.18, sharpness: 0.62, time: 0.045),
            ]
        case .destination:
            return [
                transient(intensity: 0.20, sharpness: 0.48, time: 0),
                transient(intensity: 0.30, sharpness: 0.68, time: 0.065),
            ]
        case .previousTab:
            return [
                transient(intensity: 0.20, sharpness: 0.50, time: 0),
                transient(intensity: 0.10, sharpness: 0.34, time: 0.055),
            ]
        case .nextTab:
            return [
                transient(intensity: 0.10, sharpness: 0.34, time: 0),
                transient(intensity: 0.20, sharpness: 0.50, time: 0.055),
            ]
        }
    }

    private enum DeviceImpactStyle {
        case light
        case medium
        case rigid
        case soft
    }

    private func impact(_ style: DeviceImpactStyle, intensity: CGFloat) {
        let generator: UIImpactFeedbackGenerator
        switch style {
        case .light:
            generator = light()
        case .medium:
            generator = medium()
        case .rigid:
            generator = rigid()
        case .soft:
            generator = soft()
        }
        generator.impactOccurred(
            intensity: min(1, intensity * CGFloat(uiRumbleGain))
        )
        generator.prepare()
    }

    private func scheduleDeviceImpact(
        _ style: DeviceImpactStyle,
        intensity: CGFloat,
        after delay: Duration
    ) {
        delayedDeviceFeedbackTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: delay)
            guard let self, !Task.isCancelled else { return }
            self.impact(style, intensity: intensity)
            self.delayedDeviceFeedbackTask = nil
        }
    }

    private func transient(
        intensity: Float,
        sharpness: Float,
        time: TimeInterval
    ) -> CHHapticEvent {
        let scaledIntensity = min(1, max(0, intensity * uiRumbleGain))
        return CHHapticEvent(
            eventType: .hapticTransient,
            parameters: [
                CHHapticEventParameter(
                    parameterID: .hapticIntensity,
                    value: scaledIntensity
                ),
                CHHapticEventParameter(
                    parameterID: .hapticSharpness,
                    value: sharpness
                ),
            ],
            relativeTime: time
        )
    }

    private func releaseControllerEngines() {
        for controllerEngine in controllerEngines {
            controllerEngine.engine.stop(completionHandler: nil)
        }
        controllerEngines.removeAll(keepingCapacity: false)
        controllerIDs.removeAll(keepingCapacity: false)
    }

    private func selection() -> UISelectionFeedbackGenerator {
        if let selectionGenerator { return selectionGenerator }
        let generator = UISelectionFeedbackGenerator()
        generator.prepare()
        selectionGenerator = generator
        return generator
    }

    private func light() -> UIImpactFeedbackGenerator {
        if let lightGenerator { return lightGenerator }
        let generator = UIImpactFeedbackGenerator(style: .light)
        generator.prepare()
        lightGenerator = generator
        return generator
    }

    private func medium() -> UIImpactFeedbackGenerator {
        if let mediumGenerator { return mediumGenerator }
        let generator = UIImpactFeedbackGenerator(style: .medium)
        generator.prepare()
        mediumGenerator = generator
        return generator
    }

    private func rigid() -> UIImpactFeedbackGenerator {
        if let rigidGenerator { return rigidGenerator }
        let generator = UIImpactFeedbackGenerator(style: .rigid)
        generator.prepare()
        rigidGenerator = generator
        return generator
    }

    private func soft() -> UIImpactFeedbackGenerator {
        if let softGenerator { return softGenerator }
        let generator = UIImpactFeedbackGenerator(style: .soft)
        generator.prepare()
        softGenerator = generator
        return generator
    }

    private func notification() -> UINotificationFeedbackGenerator {
        if let notificationGenerator { return notificationGenerator }
        let generator = UINotificationFeedbackGenerator()
        generator.prepare()
        notificationGenerator = generator
        return generator
    }
}
