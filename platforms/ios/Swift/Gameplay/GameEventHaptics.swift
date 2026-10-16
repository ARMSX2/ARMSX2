// GameEventHaptics.swift — device haptics for the pad, dynamic actions and the rumble fallback.
// SPDX-License-Identifier: GPL-3.0+

import UIKit

@MainActor
final class GameEventHaptics {
    static let shared = GameEventHaptics()
    private var heavyGenerator: UIImpactFeedbackGenerator?
    private var mediumGenerator: UIImpactFeedbackGenerator?
    private var lightGenerator: UIImpactFeedbackGenerator?
    private var lastFire = Date.distantPast

    private init() {}

    var heavy: UIImpactFeedbackGenerator {
        if let heavyGenerator { return heavyGenerator }
        let generator = UIImpactFeedbackGenerator(style: .heavy)
        generator.prepare()
        heavyGenerator = generator
        return generator
    }

    var medium: UIImpactFeedbackGenerator {
        if let mediumGenerator { return mediumGenerator }
        let generator = UIImpactFeedbackGenerator(style: .medium)
        generator.prepare()
        mediumGenerator = generator
        return generator
    }

    var light: UIImpactFeedbackGenerator {
        if let lightGenerator { return lightGenerator }
        let generator = UIImpactFeedbackGenerator(style: .light)
        generator.prepare()
        lightGenerator = generator
        return generator
    }

    /// Game rumble from the bridge when no rumble-capable controller is connected.
    func trigger(large: UInt16, small: UInt16) {
        guard SettingsStore.shared.hapticFeedback else { return }
        guard large > 0 || small > 0 else { return }
        let now = Date()
        // Throttle to ~20 Hz so sustained rumble does not peg the haptic engine.
        guard now.timeIntervalSince(lastFire) > 0.05 else { return }
        lastFire = now

        // Only the heavy motor has a speed; the PS2 buzzer is on or off.
        let intensity = large > 0
            ? Float(large) / Float(UInt16.max)
            : 0.55
        // Heavy motor (large) dominates; fall back to medium for small-only rumble.
        let generator = large > 0 ? heavy : medium
        // No floor, so a weak rumble stays weaker than a medium knock.
        generator.impactOccurred(intensity: CGFloat(min(1.0, intensity)))
    }

    func dynamicActionAim() {
        guard SettingsStore.shared.hapticFeedback else { return }
        light.impactOccurred(intensity: 0.55)
    }

    func dynamicActionShot() {
        guard SettingsStore.shared.hapticFeedback else { return }
        medium.impactOccurred(intensity: 0.75)
    }

    func dynamicActionRapidFire() {
        guard SettingsStore.shared.hapticFeedback else { return }
        medium.impactOccurred(intensity: 0.9)
    }

    /// Drops the generators; the next use rebuilds them, so this is not an off switch.
    func releaseForEmulationOnlyMode() {
        heavyGenerator = nil
        mediumGenerator = nil
        lightGenerator = nil
        lastFire = .distantPast
    }
}
