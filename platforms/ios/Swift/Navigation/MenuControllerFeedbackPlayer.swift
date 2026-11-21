// MenuControllerFeedbackPlayer.swift — menu sounds and haptics
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import GameController
import CoreHaptics
import UIKit

/// Menu-only feedback modeled after OrbitKeys: UIKit generators give the
/// device a reliable tactile response while short Core Haptics patterns add a
/// matching sensation to every connected controller that supports rumble.
/// Controller engines exist only while the menu owns input.
@MainActor
final class MenuControllerFeedbackPlayer {
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
