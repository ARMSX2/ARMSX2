// AppState.swift — App screen state management
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

@MainActor
@Observable
final class AppState {
    static let shared = AppState()
    nonisolated static let systemChromeNeedsUpdateNotification = Notification.Name("ARMSX2iOSSystemChromeNeedsUpdate")
    static let releaseMenuBackgroundResourcesNotification = Notification.Name("ARMSX2iOSReleaseMenuBackgroundResources")

    enum Screen {
        case menu
        case playing
    }

    var currentScreen: Screen = .menu {
        didSet {
            if systemChromeNotificationsEnabled, oldValue != currentScreen {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }
    var pendingLibraryExport: String?
    var externalDisplayConnected = false
    var sizeTransitions = 0
    // Written by the hinge interaction on the menu hosting view.
    var hingePartiallyOpen = false
    var hingePresent = false
    var hideStatusBar: Bool = false {
        didSet {
            if systemChromeNotificationsEnabled, oldValue != hideStatusBar {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }
    var hideHomeIndicator: Bool = false {
        didSet {
            if systemChromeNotificationsEnabled, oldValue != hideHomeIndicator {
                NotificationCenter.default.post(name: Self.systemChromeNeedsUpdateNotification, object: nil)
            }
        }
    }

    @ObservationIgnored private var systemChromeNotificationsEnabled = false

    private init() {
        let automaticGameStartup = ARMSX2Bridge.getINIBool(
            "ARMSX2iOS/Boot", key: "AutomaticLoadLastGame", defaultValue: false
        )
        // Seed the intro status bar before UIKit first asks for it, not in onAppear.
        // Automatic game startup skips the intro and leaves the bar to GameScreenView.
        if !automaticGameStartup {
            hideStatusBar = UserDefaults.standard.object(
                forKey: "ARMSX2iOSHideIntroStatusBar"
            ) as? Bool ?? true
        }
        systemChromeNotificationsEnabled = true
    }

    func restoreMenuSystemChrome() {
        // SDL's root controller owns gameplay fullscreen, so clear it before the menu chrome.
        ARMSX2Bridge.setFullScreen(false)
        hideStatusBar = false
        hideHomeIndicator = false
    }
}
