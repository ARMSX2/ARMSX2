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
        let scriptParam = (SettingsStore.shared.jitScriptProtocol == .legacy) ? "ZnVuY3Rpb24gbGl0dGxlRW5kaWFuSGV4U3RyaW5nVG9OdW1iZXIoaGV4U3RyKSB7CiAgICBjb25zdCBieXRlcyA9IFtdOwogICAgZm9yIChsZXQgaSA9IDA7IGkgPCBoZXhTdHIubGVuZ3RoOyBpICs9IDIpIHsKICAgICAgICBieXRlcy5wdXNoKHBhcnNlSW50KGhleFN0ci5zdWJzdHIoaSwgMiksIDE2KSk7CiAgICB9CiAgICBsZXQgbnVtID0gMG47CiAgICBmb3IgKGxldCBpID0gNDsgaSA+PSAwOyBpLS0pIHsKICAgICAgICBudW0gPSAobnVtIDw8IDhuKSB8IEJpZ0ludChieXRlc1tpXSk7CiAgICB9CiAgICByZXR1cm4gbnVtOwp9CgpmdW5jdGlvbiBudW1iZXJUb0xpdHRsZUVuZGlhbkhleFN0cmluZyhudW0pIHsKICAgIGNvbnN0IGJ5dGVzID0gW107CiAgICBmb3IgKGxldCBpID0gMDsgaSA8IDU7IGkrKykgewogICAgICAgIGJ5dGVzLnB1c2goTnVtYmVyKG51bSAmIDB4RkZuKSk7CiAgICAgICAgbnVtID4+PSA4bjsKICAgIH0KICAgIHdoaWxlIChieXRlcy5sZW5ndGggPCA4KSB7CiAgICAgICAgYnl0ZXMucHVzaCgwKTsKICAgIH0KICAgIHJldHVybiBieXRlcy5tYXAoYiA9PiBiLnRvU3RyaW5nKDE2KS5wYWRTdGFydCgyLCAnMCcpKS5qb2luKCcnKTsKfQoKZnVuY3Rpb24gbGl0dGxlRW5kaWFuSGV4VG9VMzIoaGV4U3RyKSB7CiAgICByZXR1cm4gcGFyc2VJbnQoaGV4U3RyLm1hdGNoKC8uLi9nKS5yZXZlcnNlKCkuam9pbignJyksIDE2KTsKfQoKZnVuY3Rpb24gZXh0cmFjdEJya0ltbWVkaWF0ZSh1MzIpIHsKICAgIHJldHVybiAodTMyID4+IDUpICYgMHhGRkZGOwp9CgpmdW5jdGlvbiBhdHRhY2goYnJlYWtwb2ludGNvdW50KSB7CiAgICBsZXQgcGlkID0gZ2V0X3BpZCgpOwogICAgbG9nKGBwaWQgPSAke3BpZH1gKTsKICAgIGxldCBhdHRhY2hSZXNwb25zZSA9IHNlbmRfY29tbWFuZChgdkF0dGFjaDske3BpZC50b1N0cmluZygxNil9YCk7CiAgICBsb2coYGF0dGFjaF9yZXNwb25zZSA9ICR7YXR0YWNoUmVzcG9uc2V9YCk7CiAgICAKICAgIGxldCB2YWxpZEJyZWFrcG9pbnRzID0gMDsKICAgIGxldCB0b3RhbEJyZWFrcG9pbnRzID0gMDsKCiAgICB3aGlsZSAodmFsaWRCcmVha3BvaW50cyA8IGJyZWFrcG9pbnRjb3VudCkgewogICAgICAgIHRvdGFsQnJlYWtwb2ludHMrKzsKICAgICAgICBsb2coYEhhbmRsaW5nIGJyZWFrcG9pbnQgJHt0b3RhbEJyZWFrcG9pbnRzfSAobG9va2luZyBmb3IgdmFsaWQgYnJlYWtwb2ludCAke3ZhbGlkQnJlYWtwb2ludHMgKyAxfS8ke2JyZWFrcG9pbnRjb3VudH0pYCk7CiAgICAgICAgCiAgICAgICAgbGV0IGJya1Jlc3BvbnNlID0gc2VuZF9jb21tYW5kKGBjYCk7CiAgICAgICAgbG9nKGBicmtSZXNwb25zZSA9ICR7YnJrUmVzcG9uc2V9YCk7CiAgICAgICAgCiAgICAgICAgbGV0IHRpZE1hdGNoID0gL1RbMC05YS1mXSt0aHJlYWQ6KD88dGlkPlswLTlhLWZdKyk7Ly5leGVjKGJya1Jlc3BvbnNlKTsKICAgICAgICBsZXQgdGlkID0gdGlkTWF0Y2ggPyB0aWRNYXRjaC5ncm91cHNbJ3RpZCddIDogbnVsbDsKICAgICAgICBsZXQgcGNNYXRjaCA9IC8yMDooPzxyZWc+WzAtOWEtZl17MTZ9KTsvLmV4ZWMoYnJrUmVzcG9uc2UpOwogICAgICAgIGxldCBwYyA9IHBjTWF0Y2ggPyBwY01hdGNoLmdyb3Vwc1sncmVnJ10gOiBudWxsOwogICAgICAgIGxldCB4ME1hdGNoID0gLzAwOig/PHJlZz5bMC05YS1mXXsxNn0pOy8uZXhlYyhicmtSZXNwb25zZSk7CiAgICAgICAgbGV0IHgwID0geDBNYXRjaCA/IHgwTWF0Y2guZ3JvdXBzWydyZWcnXSA6IG51bGw7CiAgICAgICAgbGV0IHgxTWF0Y2ggPSAvMDE6KD88cmVnPlswLTlhLWZdezE2fSk7Ly5leGVjKGJya1Jlc3BvbnNlKTsKICAgICAgICBsZXQgeDEgPSB4MU1hdGNoID8geDFNYXRjaC5ncm91cHNbJ3JlZyddIDogbnVsbDsKICAgICAgICAKICAgICAgICBpZiAoIXRpZCB8fCAhcGMgfHwgIXgwIHx8ICF4MSkgewogICAgICAgICAgICBsb2coYEZhaWxlZCB0byBleHRyYWN0IHJlZ2lzdGVyczogdGlkPSR7dGlkfSwgcGM9JHtwY30sIHgwPSR7eDB9LCB4MT0ke3gxfWApOwogICAgICAgICAgICBjb250aW51ZTsKICAgICAgICB9CiAgICAgICAgCiAgICAgICAgY29uc3QgcGNOdW0gPSBsaXR0bGVFbmRpYW5IZXhTdHJpbmdUb051bWJlcihwYyk7CiAgICAgICAgY29uc3QgeDBOdW0gPSBsaXR0bGVFbmRpYW5IZXhTdHJpbmdUb051bWJlcih4MCk7CiAgICAgICAgY29uc3QgeDFOdW0gPSBsaXR0bGVFbmRpYW5IZXhTdHJpbmdUb051bWJlcih4MSk7CiAgICAgICAgbG9nKGB0aWQgPSAke3RpZH0sIHBjID0gJHtwY051bS50b1N0cmluZygxNil9LCB4MCA9ICR7eDBOdW0udG9TdHJpbmcoMTYpfSwgeDEgPSAke3gxTnVtLnRvU3RyaW5nKDE2KX1gKTsKICAgICAgICAKICAgICAgICBsZXQgaW5zdHJ1Y3Rpb25SZXNwb25zZSA9IHNlbmRfY29tbWFuZChgbSR7cGNOdW0udG9TdHJpbmcoMTYpfSw0YCk7CiAgICAgICAgbG9nKGBpbnN0cnVjdGlvbiBhdCBwYzogJHtpbnN0cnVjdGlvblJlc3BvbnNlfWApOwogICAgICAgIGxldCBpbnN0clUzMiA9IGxpdHRsZUVuZGlhbkhleFRvVTMyKGluc3RydWN0aW9uUmVzcG9uc2UpOwogICAgICAgIGxldCBicmtJbW1lZGlhdGUgPSBleHRyYWN0QnJrSW1tZWRpYXRlKGluc3RyVTMyKTsKICAgICAgICBsb2coYEJSSyBpbW1lZGlhdGU6IDB4JHticmtJbW1lZGlhdGUudG9TdHJpbmcoMTYpfSAoJHticmtJbW1lZGlhdGV9KWApOwogICAgICAgIAogICAgICAgIGlmIChicmtJbW1lZGlhdGUgIT09IDB4NjkpIHsKICAgICAgICAgICAgbG9nKGBTa2lwcGluZyBicmVha3BvaW50OiBicmsgaW1tZWRpYXRlIHdhcyBub3QgMHg2OSAod2FzIDB4JHticmtJbW1lZGlhdGUudG9TdHJpbmcoMTYpfSlgKTsKICAgICAgICAgICAgY29udGludWU7CiAgICAgICAgfQogICAgICAgIAogICAgICAgIGxvZyhgQlJLIGltbWVkaWF0ZSBtYXRjaGVzIGV4cGVjdGVkIHZhbHVlIDB4NjkgLSBwcm9jZXNzaW5nIHZhbGlkIGJyZWFrcG9pbnQgJHt2YWxpZEJyZWFrcG9pbnRzICsgMX0vJHticmVha3BvaW50Y291bnR9YCk7CiAgICAgICAgCiAgICAgICAgbG9nKGBBbGxvY2F0ZWQgSklUIHBhZ2UgYXQgYWRkcmVzczogMHgke3gwTnVtLnRvU3RyaW5nKDE2KX1gKTsKICAgICAgICAKICAgICAgICBsZXQgcHJlcGFyZUpJVFBhZ2VSZXNwb25zZSA9IHByZXBhcmVfbWVtb3J5X3JlZ2lvbih4ME51bSwgeDFOdW0pOwogICAgICAgIGxvZyhgcHJlcGFyZUpJVFBhZ2VSZXNwb25zZSA9ICR7cHJlcGFyZUpJVFBhZ2VSZXNwb25zZX1gKTsKICAgICAgICAKICAgICAgICBsZXQgcGNQbHVzNCA9IG51bWJlclRvTGl0dGxlRW5kaWFuSGV4U3RyaW5nKHBjTnVtICsgNG4pOwogICAgICAgIGxldCBwY1BsdXM0UmVzcG9uc2UgPSBzZW5kX2NvbW1hbmQoYFAyMD0ke3BjUGx1czR9O3RocmVhZDoke3RpZH07YCk7CiAgICAgICAgbG9nKGBwY1BsdXM0UmVzcG9uc2UgPSAke3BjUGx1czRSZXNwb25zZX1gKTsKICAgICAgICAKICAgICAgICB2YWxpZEJyZWFrcG9pbnRzKys7CiAgICAgICAgbG9nKGBDb21wbGV0ZWQgdmFsaWQgYnJlYWtwb2ludCAke3ZhbGlkQnJlYWtwb2ludHN9LyR7YnJlYWtwb2ludGNvdW50fWApOwogICAgfQogICAgCiAgICBsZXQgZGV0YWNoUmVzcG9uc2UgPSBzZW5kX2NvbW1hbmQoYERgKTsKICAgIGxvZyhgZGV0YWNoUmVzcG9uc2UgPSAke2RldGFjaFJlc3BvbnNlfWApOwp9CgphdHRhY2goMSk7Cg==" : "universal"
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
