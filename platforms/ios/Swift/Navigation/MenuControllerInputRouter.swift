// MenuControllerInputRouter.swift — physical-controller menu navigation
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import GameController
import CoreHaptics
import Observation
import SwiftUI
import UIKit

/// Held face button for controller feedback; like OrbitKeys' `pressedFace`, it stays until release.
enum MenuControllerFaceButton: Equatable, Sendable {
    case cross
    case circle
    case triangle
}

/// Controller focus regions, apart from the selected tab: only the primary button changes content.
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
    static let biosPrompt = "menu.bios.prompt"
    static let gameLibraryPresentation = "game-library.presentation"
    static let cheatsPatchesManager = "cheats-patches-manager"
    static let themePresetShortcut = "menu.theme-preset-shortcut"
    static let perGameLivePreview = "per-game.live-preview"
    static let orbitKeysKeyboard = "orbit-keys.keyboard"
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

struct MenuControllerThemePresetRequest: Equatable, Sendable {
    let sequence: UInt64
    let step: Int
}

struct MenuControllerScrollVector: Equatable, Sendable {
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

/// Owns the profile callbacks of the retained SwiftUI menu. SDL polls the same profiles in gameplay
/// and the iOS fallback owns each D-pad handler, so only the aggregate valueChangedHandler is used.
@MainActor
@Observable
final class MenuControllerInputRouter {
    /// While a save-state Undo is on screen, the macro chord also works over menus.
    var saveStateUndoPending = false
    @ObservationIgnored var undoChordHeld = false
    var saveStateUndoRequest: UInt64 = 0
    struct NavigationSessionTarget {
        let scopeKey: String
        let priority: Int
        let registrationOrder: UInt64
        var isReady: Bool
        var scrollWindow: UIWindow?
        var scrollFocusFrame: CGRect?
        let handler: @MainActor (MenuControllerCommand) -> Bool
        let entryHandler: @MainActor (Bool) -> Bool
        let scrollHandler: @MainActor (UIScrollView, MenuControllerCommand) -> Void
        let scrollStateHandler: @MainActor (Bool) -> Void
    }

    struct RightStickTarget {
        let priority: Int
        let registrationOrder: UInt64
        let ownerSessionID: UUID?
        let manualCaptureOwner: String?
        let handler: @MainActor (MenuControllerScrollVector) -> Bool
    }

    struct ManualNavigationCapture {
        let priority: Int
        let registrationOrder: UInt64
    }

    struct PendingNavigationSessionCommand {
        let sequence: UInt64
        let scopeKey: String
        let command: MenuControllerCommand
        let isRepeat: Bool
        let repeatAcceleration: Double
    }

    struct PendingNavigationSessionEntryRequest {
        let id = UUID()
        let preferLast: Bool
        let scopePrefix: String?
        var ownerSessionID: UUID?
        var ownerScopeKey: String?
    }

    enum Button: Hashable {
        case primary
        case secondary
        case square
        case triangle
        case previousTab
        case nextTab
        case previousTheme
        case nextTheme
    }

    private(set) var latestEvent: MenuControllerInputEvent?
    private(set) var latestEventTimestamp: TimeInterval?
    private(set) var latestLibraryEntryRequest: MenuControllerLibraryEntryRequest?
    var latestQuickPauseRequest: MenuControllerQuickPauseRequest?
    var latestEmulationShortcutRequest:
        EmulationControllerShortcutRequest?
    var latestThemePresetRequest: MenuControllerThemePresetRequest?
    var pendingNavigationModeSwitchRequest:
        MenuNavigationModeSwitchRequest?
    /// Window-space frame from the real tab bar. RootView portals it into the presentation-wide orb
    /// host, so crossing a safe-area boundary never replaces the active particle field.
    var tabBarOrbFocusFrameInWindow: CGRect?
    var tabBarOrbFocusedIndex: Int?
    private(set) var tabBarOrbSelectedIndex: Int?
    private(set) var focusReleaseSequence: UInt64 = 0
    var rightStickScrollEndSequence: UInt64 = 0
    var isMenuActive = false
    private(set) var isNavigationCaptured = false
    var hasConnectedController = false
    var isControllerNavigationEnabled = false
    var activeNavigationInputMode: MenuNavigationInputMode = .touch
    var navigationZone: MenuControllerNavigationZone = .library
    var pressedFaceButton: MenuControllerFaceButton?

    @ObservationIgnored var sequence: UInt64 = 0
    @ObservationIgnored var connectObserver: NSObjectProtocol?
    @ObservationIgnored var disconnectObserver: NSObjectProtocol?
    @ObservationIgnored var registeredExtendedProfiles = Set<ObjectIdentifier>()
    @ObservationIgnored var registeredMicroProfiles = Set<ObjectIdentifier>()
    @ObservationIgnored var heldDirections: [ObjectIdentifier: MenuControllerCommand] = [:]
    @ObservationIgnored var directionalHoldStartTimes: [
        ObjectIdentifier: TimeInterval
    ] = [:]
    @ObservationIgnored var lastDirectionEdges: [
        ObjectIdentifier: (command: MenuControllerCommand, timestamp: TimeInterval)
    ] = [:]
    /// Some wireless pads send one D-pad edge through GameController and a keyboard-style HID
    /// path. emit() drops that duplicate globally and leaves intentional held repeats alone.
    @ObservationIgnored var lastPublishedDirectionalEdge: (
        command: MenuControllerCommand,
        timestamp: TimeInterval
    )?
    @ObservationIgnored var directionSamplingTasks: [
        ObjectIdentifier: Task<Void, Never>
    ] = [:]
    @ObservationIgnored var libraryEntrySuppressedEventSequence: UInt64?
    @ObservationIgnored var pressedButtons: [ObjectIdentifier: Set<Button>] = [:]
    @ObservationIgnored var emulationMacroLatches: [
        ObjectIdentifier: Set<ControllerMacroAction>
    ] = [:]
    @ObservationIgnored var emulationMacroRepeatTasks: [
        ObjectIdentifier: [ControllerMacroAction: Task<Void, Never>]
    ] = [:]
    @ObservationIgnored var navigationSessions: [UUID: NavigationSessionTarget] = [:]
    /// Session input goes to. Nil while a screen like the pause menu captures it; all rings stay.
    private(set) var navigationInputSessionID: UUID?
    @ObservationIgnored private var rememberedNavigationFocusKeys: [String: String] = [:]
    @ObservationIgnored private var navigationSessionSequence: UInt64 = 0
    @ObservationIgnored var pendingNavigationSessionCommands: [
        UUID: PendingNavigationSessionCommand
    ] = [:]
    @ObservationIgnored var pendingNavigationSessionEntryRequest:
        PendingNavigationSessionEntryRequest?
    @ObservationIgnored var navigationSessionEntryTask: Task<Void, Never>?
    @ObservationIgnored var launchInputInterceptor: (@MainActor () -> Void)?
    @ObservationIgnored var manuallyCapturedNavigation = false
    @ObservationIgnored var manualNavigationCaptures: [
        String: ManualNavigationCapture
    ] = [:]
    @ObservationIgnored var manualNavigationCaptureSequence: UInt64 = 0
    @ObservationIgnored var repeatTasks: [
        ObjectIdentifier: Task<Void, Never>
    ] = [:]
    @ObservationIgnored var themePresetRepeatTasks: [
        ObjectIdentifier: [Button: Task<Void, Never>]
    ] = [:]
    @ObservationIgnored var touchThemePresetShortcutSuppressedUntil = 0.0
    @ObservationIgnored var rightStickVector = MenuControllerScrollVector.zero
    @ObservationIgnored var lastRightStickScrollDirection: MenuControllerCommand?
    @ObservationIgnored private(set) var isRepeatingDirectionCommand = false
    /// Multiplier of the repeat in flight; sessions keep focus scrolls shorter than its cadence.
    @ObservationIgnored private(set) var directionalRepeatAcceleration = 1.0
    @ObservationIgnored var isRightStickScrolling = false
    @ObservationIgnored var rightStickHoldStartTime: CFTimeInterval?
    @ObservationIgnored var rightStickSessionOwner: (id: UUID, scopeKey: String)?
    @ObservationIgnored var rightStickTargets: [UUID: RightStickTarget] = [:]
    @ObservationIgnored var activeRightStickTargetID: UUID?
    @ObservationIgnored var rightStickRegistrationSequence: UInt64 = 0
    @ObservationIgnored var rightStickRedispatchTask: Task<Void, Never>?
    @ObservationIgnored let globalScrollDriver = MenuControllerGlobalScrollDriver()
    @ObservationIgnored let feedbackPlayer = MenuControllerFeedbackPlayer()

    func requestFocusRelease() {
        focusReleaseSequence &+= 1
    }

    // This launch only: when saved across launches, a Settings page first opened on the old row.
    func rememberNavigationFocusKey(_ key: String, forScope scopeKey: String) {
        guard !scopeKey.isEmpty, !key.isEmpty else { return }
        rememberedNavigationFocusKeys[scopeKey] = key
    }

    func rememberedNavigationFocusKey(forScope scopeKey: String) -> String? {
        rememberedNavigationFocusKeys[scopeKey]
    }

    /// Registers a screen's command consumer. Highest priority, then newest, wins, as sheet and
    /// full-screen cover z-order do, without broadcasting state into every retained tab.
    func registerNavigationSession(
        id: UUID,
        scopeKey: String,
        priority: Int,
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
                // A NavigationStack push keeps the UUID, not the scope: retire the old stick owner.
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
            isReady: isReady,
            scrollWindow: navigationSessions[id]?.scrollWindow,
            scrollFocusFrame: navigationSessions[id]?.scrollFocusFrame,
            handler: handler,
            entryHandler: entryHandler,
            scrollHandler: scrollHandler,
            scrollStateHandler: scrollStateHandler
        )
        updateNavigationCaptureState()
        schedulePendingNavigationSessionCommandIfReady(id: id)
        schedulePendingNavigationSessionEntryIfReady()
        // Bookkeeping only, no zone change: a List/Form can register while the tab bar owns focus,
        // and Down would then enter its first lazy row. requestNavigationSessionEntry owns entry.
        if scopeChanged, !rightStickVector.isZero { scheduleRightStickRedispatch() }
    }

    func setNavigationSessionReady(id: UUID, isReady: Bool) {
        guard var target = navigationSessions[id],
              target.isReady != isReady else { return }
        target.isReady = isReady
        navigationSessions[id] = target
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
        // Focus release lands while the first analog vector is dispatching. Keep an actively
        // scrolling pane target instead of recursively resolving the same Form again.
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
        updateNavigationCaptureState()
        activeRightStickTargetID = nil
        scheduleRightStickRedispatch()
        schedulePendingNavigationSessionEntryIfReady()
    }

    func updateNavigationCaptureState() {
        // The same owner emit() picks when no screen holds a manual capture.
        let inputSession = manualNavigationCaptures.isEmpty ? frontmostNavigationSessionEntry?.key : nil
        if navigationInputSessionID != inputSession { navigationInputSessionID = inputSession }
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
        // A command queued while the old focus owner mounted must never replay into the new zone.
        pendingNavigationSessionCommands.removeAll(keepingCapacity: true)
        navigationZone = zone
        activeRightStickTargetID = nil
        dispatchRightStickVector()
    }

    /// Modal screens (Quick Menu, Per-Game Settings, their children) ignore the toolbar and tab bar
    /// zones and claim content on registration, so a controller edge re-enables focus after touch.
    func claimPresentedNavigationFocus() {
        guard isMenuActive, isControllerNavigationEnabled else { return }
        setNavigationZone(.library)
    }

    /// The live preview's hidden Per-Game Settings session still owns the focused adjustable row.
    @discardableResult
    func adjustPerGameLivePreview(_ command: MenuControllerCommand) -> Bool {
        adjustCapturedLivePreview(
            command,
            owner: MenuControllerNavigationCaptureOwner.perGameLivePreview
        )
    }

    /// Forwards a horizontal command from a live-preview shield to the session mounted under it,
    /// without releasing the capture or publishing a second event, so hidden sliders still adjust.
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
        // The Game Library also sees the event that leaves the tab bar. Suppress only that event,
        // not the held direction, or the first upward repeat sticks on its card until release.
        libraryEntrySuppressedEventSequence = latestEvent?.sequence
        // The zone change moves a held right stick off the pre-rotation/tab target to the library.
        setNavigationZone(.library)
        sequence &+= 1
        latestLibraryEntryRequest = MenuControllerLibraryEntryRequest(
            sequence: sequence,
            preferLast: preferLast
        )
    }

    /// Moves focus from the tab bar into the frontmost navigation surface. Unlike library entry, it
    /// also works for Form/List screens that materialize late, such as BIOS and Settings.
    @discardableResult
    func requestNavigationSessionEntry(
        preferLast: Bool,
        matchingScopePrefix scopePrefix: String? = nil
    ) -> Bool {
        guard isMenuActive else { return false }
        // The tab bar took the entry edge; repeats run until endpoint rules stop at Language/Help.
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

    func schedulePendingNavigationSessionEntryIfReady() {
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
            // Readiness comes from UIViewRepresentable probes; wait for it and the List layout.
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

    /// In the boot movie the first press only dismisses it; native focus and the menu never see it.
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
    }

    func suppressesLibraryEvent(_ event: MenuControllerInputEvent) -> Bool {
        event.sequence == libraryEntrySuppressedEventSequence
    }

    func emit(
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

        // Resolve the owner once: publishing can mount Settings and change the zone, so a second
        // lookup could route the same press into Language or another newly mounted target.
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
            // An entry wait owns only its page, never a modal above it, another tab or the tab bar.
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
        // The frontmost presentation owns the command while its lazy controls load: never leak it
        // to a retained screen underneath or run an empty focus graph.
        if !target.isReady {
            // Shoulders and Back are presentation-level and need no lazy rows in the graph yet.
            if command == .previousTab
                || command == .nextTab
                || command == .back {
                return target.handler(command)
            }
            // Activation and context commands are edges; dropping them with no target is
            // safer than replaying them after entry has focused the first Settings row.
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

    /// Matches an entry request only to the new page: after a tab change the old session can stay
    /// registered for a render pass, and falling back to it would focus hidden controls under it.
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

    var frontmostNavigationSessionEntry: (
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

}
