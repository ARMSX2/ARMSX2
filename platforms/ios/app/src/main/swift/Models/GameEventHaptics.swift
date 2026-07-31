// GameEventHaptics.swift — device-level haptic fallback for game rumble events
// when no rumble-capable controller is connected.
// SPDX-License-Identifier: GPL-3.0+

import UIKit

@MainActor
final class GameEventHaptics {
    static let shared = GameEventHaptics()
    private var heavyGenerator: UIImpactFeedbackGenerator?
    private var mediumGenerator: UIImpactFeedbackGenerator?
    private var lastFire = Date.distantPast
    private var hapticsEnabled = true

    private init() {
        hapticsEnabled = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HapticFeedback", defaultValue: true)
    }

    /// Called from the bridge (game rumble path) when no rumble-capable controller is connected.
    /// Respects the user's HapticFeedback setting and throttles to avoid motor fatigue.
    func trigger(large: UInt16, small: UInt16) {
        guard hapticsEnabled else { return }
        guard large > 0 || small > 0 else { return }
        let now = Date()
        // Throttle to ~20 Hz so sustained rumble does not peg the haptic engine.
        guard now.timeIntervalSince(lastFire) > 0.05 else { return }
        lastFire = now

        // The heavy motor is the only one with a speed. The PS2 buzzer is on or off,
        // so taking the larger of the two used to pin this to full every time a game
        // so much as touched it.
        let intensity = large > 0
            ? Float(large) / Float(UInt16.max)
            : 0.55
        // Heavy motor (large) dominates; fall back to medium for small-only rumble.
        let generator: UIImpactFeedbackGenerator
        if large > 0 {
            if let heavyGenerator {
                generator = heavyGenerator
            } else {
                let created = UIImpactFeedbackGenerator(style: .heavy)
                heavyGenerator = created
                generator = created
            }
        } else if let mediumGenerator {
            generator = mediumGenerator
        } else {
            let created = UIImpactFeedbackGenerator(style: .medium)
            mediumGenerator = created
            generator = created
        }
        // No floor. It used to be 0.3, which flattened every weak rumble onto the same
        // knock as a medium one. This path is only reached on hardware with no taptic
        // engine to run the continuous one, so it is a fallback rather than the norm.
        generator.impactOccurred(intensity: CGFloat(min(1.0, intensity)))
    }

    func prepareForGameplaySession() {
        refreshEnabled()
    }

    /// Drops the cached generators. trigger() builds them again on demand, so this is a
    /// resource release and not an off switch: rumble after this still gets through.
    func releaseForEmulationOnlyMode() {
        heavyGenerator = nil
        mediumGenerator = nil
        lastFire = .distantPast
    }

    /// Refresh the enabled flag when the user changes the HapticFeedback setting.
    func refreshEnabled() {
        hapticsEnabled = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HapticFeedback", defaultValue: true)
    }
}
