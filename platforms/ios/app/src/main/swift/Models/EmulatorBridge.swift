// EmulatorBridge.swift — SwiftUI ↔ C++ emulator bridge
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

@MainActor
enum StikDebugLauncher {
    private static let lastAutoOpenKey = "ARMSX2iOSLastStikDebugAutoOpenTime"
    private static let autoOpenCooldown: TimeInterval = 120
    private static func log(_ message: String) {
        print("[ARMSX2 iOS] StikDebug \(message)")
    }

    static func open(reason: String = "manual", completion: ((Bool) -> Void)? = nil) {
#if canImport(UIKit)
        let bundleID = Bundle.main.bundleIdentifier ?? "com.armsx2.ios"
        let encodedBundleID = bundleID.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? bundleID
        let appName = Bundle.main.infoDictionary?["CFBundleDisplayName"] as? String ?? "ARMSX2iOS"
        let scriptParam = (SettingsStore.shared.jitScriptProtocol == .legacy) ? "classic" : "universal"
        let encodedScript = scriptParam.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? scriptParam
        let candidates = [
            "stikdebug://enable-jit?bundle-id=\(encodedBundleID)",
            "stikjit://enable-jit?bundle-id=\(encodedBundleID)",
            "stikdebug://",
            "stosdebug://enableJIT?bundleId=\(encodedBundleID)&appName=\(appName)&script=\(encodedScript)",
            "stosdebug://"
                    ].compactMap(URL.init(string:))

        guard !candidates.isEmpty else {
            log("open failed: no valid launch URLs")
            completion?(false)
            return
        }

        openFirstAvailableURL(candidates, reason: reason, completion: completion)
#else
        log("open skipped: UIKit unavailable reason=\(reason)")
        completion?(false)
#endif
    }

#if canImport(UIKit)
    private static func openFirstAvailableURL(_ urls: [URL], reason: String, completion: ((Bool) -> Void)?) {
        guard let url = urls.first else {
            log("open failed reason=\(reason): no URL scheme accepted")
            completion?(false)
            return
        }

        UIApplication.shared.open(url, options: [:]) { success in
            log("open \(success ? "succeeded" : "failed") reason=\(reason) url=\(url.absoluteString)")
            if success {
                completion?(true)
            } else {
                openFirstAvailableURL(Array(urls.dropFirst()), reason: reason, completion: completion)
            }
        }
    }
#endif

    static func autoOpenIfNeeded(reason: String) {
        guard SettingsStore.shared.autoOpenStikDebug else { return }
        guard !ARMSX2Bridge.isJITAvailable() else { return }

        let now = Date().timeIntervalSince1970
        let last = UserDefaults.standard.double(forKey: lastAutoOpenKey)
        guard now - last >= autoOpenCooldown else {
            log("auto-open throttled reason=\(reason)")
            return
        }

        UserDefaults.standard.set(now, forKey: lastAutoOpenKey)
        open(reason: "auto-\(reason)")
    }
}

enum EmulatorState: String {
    case stopped = "Stopped"
    case running = "Running"
    case paused = "Paused"
    case saving = "Saving"
    case suspended = "Suspended"
}

@Observable
final class EmulatorBridge: @unchecked Sendable {
    static let shared = EmulatorBridge()

    var state: EmulatorState = .stopped
    var lastSaveDate: Date? = nil
    var lastSaveSuccess: Bool = true
    var biosName: String = "Unknown"
    var buildVersion: String = ""

    @ObservationIgnored private var virtualRightTouchX: Float = 0
    @ObservationIgnored private var virtualRightTouchY: Float = 0
    @ObservationIgnored private var virtualRightMotionX: Float = 0
    @ObservationIgnored private var virtualRightMotionY: Float = 0

    private init() {
        biosName = ARMSX2Bridge.biosName()
        buildVersion = ARMSX2Bridge.buildVersion()
    }

    func saveAll() {
        state = .saving
        ARMSX2Bridge.saveAllState()
        lastSaveDate = Date()
        lastSaveSuccess = true
        state = .running
    }

    func setPadButton(_ button: ARMSX2PadButton, pressed: Bool) {
        ARMSX2Bridge.setPadButton(button, pressed: pressed)
    }

    @MainActor
    func setLeftStick(x: Float, y: Float) {
        let dynamicSettings = DynamicThumbstickSettings.shared
        let sensitivity = Float(dynamicSettings.movementSensitivity)
        let clamped = Self.radiallyClamped(x: x * sensitivity, y: y * sensitivity)
        let output = Self.applyingNegativeDeadzone(
            to: clamped,
            enabled: dynamicSettings.leftInstantDeadzoneEnabled,
            configuredDeadzone: dynamicSettings.leftNegativeDeadzone
        )
        let inv = SettingsStore.shared.stickInversion(for: .left)
        ARMSX2Bridge.setLeftStickX(inv.x ? -output.x : output.x, y: inv.y ? -output.y : output.y)
    }

    @MainActor
    func setRightStick(x: Float, y: Float) {
        let sensitivity = Float(DynamicThumbstickSettings.shared.lookSensitivity)
        virtualRightTouchX = x * sensitivity
        virtualRightTouchY = y * sensitivity
        applyVirtualRightStick()
    }

    @MainActor
    func setRightStickMotion(x: Float, y: Float) {
        virtualRightMotionX = x
        virtualRightMotionY = y
        applyVirtualRightStick()
    }

    @MainActor
    func resetVirtualPadAnalogInput() {
        virtualRightTouchX = 0
        virtualRightTouchY = 0
        virtualRightMotionX = 0
        virtualRightMotionY = 0
        ARMSX2Bridge.setLeftStickX(0, y: 0)
        ARMSX2Bridge.setRightStickX(0, y: 0)
    }

    @MainActor
    private func applyVirtualRightStick() {
        let dynamicSettings = DynamicThumbstickSettings.shared
        let clamped = Self.radiallyClamped(
            x: virtualRightTouchX + virtualRightMotionX,
            y: virtualRightTouchY + virtualRightMotionY
        )
        let output = Self.applyingNegativeDeadzone(
            to: clamped,
            enabled: dynamicSettings.rightInstantDeadzoneEnabled,
            configuredDeadzone: dynamicSettings.rightNegativeDeadzone
        )
        let inv = SettingsStore.shared.stickInversion(for: .right)
        ARMSX2Bridge.setRightStickX(inv.x ? -output.x : output.x, y: inv.y ? -output.y : output.y)
    }

    private static func radiallyClamped(x: Float, y: Float) -> (x: Float, y: Float) {
        let magnitude = hypotf(x, y)
        guard magnitude > 1 else { return (x, y) }
        return (x / magnitude, y / magnitude)
    }

    /// Applies a radial minimum-output floor without rescaling larger values.
    /// This preserves the Dynamic Thumbstick's progressive deadzone curve.
    private static func applyingNegativeDeadzone(
        to input: (x: Float, y: Float),
        enabled: Bool,
        configuredDeadzone: Double
    ) -> (x: Float, y: Float) {
        guard enabled else { return input }
        let magnitude = hypotf(input.x, input.y)
        guard magnitude > 0 else { return input }

        let floor = min(max(Float(-configuredDeadzone), 0), 0.95)
        guard floor > 0, magnitude < floor else { return input }
        let scale = floor / magnitude
        return (input.x * scale, input.y * scale)
    }

    var isOsdVisible: Bool {
        get { ARMSX2Bridge.isPerformanceOverlayVisible() }
        set { ARMSX2Bridge.setPerformanceOverlayVisible(newValue) }
    }
}
