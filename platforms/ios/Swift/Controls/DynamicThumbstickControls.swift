// DynamicThumbstickControls.swift — Floating sticks, swipe camera, gyro, and touch actions
// SPDX-License-Identifier: GPL-3.0+

import CoreMotion
import QuartzCore
import SwiftUI
import UIKit

struct DynamicThumbstickVector: Equatable, Sendable {
    var x: CGFloat
    var y: CGFloat

    static let zero = DynamicThumbstickVector(x: 0, y: 0)

    var magnitude: CGFloat {
        hypot(x, y)
    }

    func limited(to maximumMagnitude: CGFloat) -> DynamicThumbstickVector {
        let magnitude = magnitude
        guard magnitude > maximumMagnitude, magnitude > 0 else { return self }
        let scale = maximumMagnitude / magnitude
        return DynamicThumbstickVector(x: x * scale, y: y * scale)
    }
}

enum DynamicCrosshairMotionSource: Hashable {
    case swipe
    case thumbstick
    case gyroscope
}

struct DynamicThumbstickSample {
    let input: DynamicThumbstickVector
    let rawDistance: CGFloat
}

enum DynamicThumbstickMath {
    static func sample(
        translation: CGSize,
        maximumRadius: CGFloat,
        deadZone: CGFloat,
        deadZoneProgressRadius: CGFloat? = nil
    ) -> DynamicThumbstickSample {
        guard maximumRadius > 0 else {
            return DynamicThumbstickSample(input: .zero, rawDistance: 0)
        }

        let rawDistance = hypot(translation.width, translation.height)
        guard rawDistance > 0 else {
            return DynamicThumbstickSample(input: .zero, rawDistance: 0)
        }

        let safeDeadZone = min(max(deadZone, 0), 0.95)
        let cappedDistance = min(rawDistance, maximumRadius)
        let normalizedDistance = cappedDistance / maximumRadius
        // Input saturation and deadzone progression can use different radii.
        // This preserves the larger movement area while letting the left stick
        // reach its configured deadzone at the visible Maximum Radius.
        let progressRadius = max(deadZoneProgressRadius ?? maximumRadius, 1)
        let deadZoneProgress = min(rawDistance / progressRadius, 1)
        let progressRadiusScale = min(progressRadius / maximumRadius, 1)
        let adaptiveDeadZone =
            safeDeadZone * deadZoneProgress * progressRadiusScale
        let magnitude = max(normalizedDistance - adaptiveDeadZone, 0) /
            (1 - adaptiveDeadZone)

        return DynamicThumbstickSample(
            input: DynamicThumbstickVector(
                x: translation.width / rawDistance * magnitude,
                // ARMSX2's existing virtual sticks use screen-space positive Y.
                y: translation.height / rawDistance * magnitude
            ),
            rawDistance: rawDistance
        )
    }

    static func trailDotScale(index: Int, rawDistance: CGFloat, maximumRadius: CGFloat, maximumDots: Int = 7) -> CGFloat {
        guard index >= 0, index < maximumDots, rawDistance > 0, maximumRadius > 0 else { return 0 }
        let progress = min(rawDistance / maximumRadius, 1) * CGFloat(maximumDots)
        return min(max(progress - CGFloat(index), 0), 1)
    }

    static func trailDotPositionProgress(index: Int, rawDistance: CGFloat, maximumRadius: CGFloat, maximumDots: Int = 7) -> CGFloat {
        let scale = trailDotScale(index: index, rawDistance: rawDistance, maximumRadius: maximumRadius, maximumDots: maximumDots)
        guard scale > 0 else { return 0 }
        return CGFloat(index + 1) / CGFloat(maximumDots + 1) * scale
    }

    /// Keeps the maximum-pull feedback stable near the boundary so tiny finger
    /// jitter cannot repeatedly retrigger its haptic.
    static func overextensionState(
        distance: CGFloat,
        maximumRadius: CGFloat,
        currentlyOverextended: Bool
    ) -> Bool {
        currentlyOverextended
            ? distance > maximumRadius * 0.95
            : distance > maximumRadius
    }
}

struct DynamicThumbstickView: View {
    let isLeft: Bool
    let radius: CGFloat
    let maximumRadius: CGFloat
    var deadZoneProgressRadius: CGFloat? = nil
    let deadZone: CGFloat
    let hapticsEnabled: Bool
    let thumbstickOpacity: Double
    let baseOpacity: Double
    let trailOpacity: Double
    var tapActionsEnabled = false
    var maximumTapDuration: TimeInterval = 0.30
    var tapTravelTolerance: CGFloat = 12
    var onVector: (DynamicThumbstickVector) -> Void
    var onInteractionBegan: () -> Void = {}
    var onInteractionActivity: () -> Void = {}
    var onInteractionTap: () -> Void = {}
    var onInteractionEnded: () -> Void = {}
    // Fires on every classified tap, unlike onInteractionTap which needs tap actions on.
    var onAnyTap: () -> Void = {}

    @State private var origin = CGPoint.zero
    @State private var dragOffset = CGSize.zero
    @State private var dragDistance: CGFloat = 0
    @State private var isActive = false
    @State private var isOverextended = false
    @State private var maximumPullPulseToken = 0
    @State private var gestureStartedAt: Date?
    @State private var maximumTravel: CGFloat = 0

    var body: some View {
        GeometryReader { _ in
            ZStack(alignment: .topLeading) {
                Color.clear
                    .contentShape(Rectangle())

                if isActive {
                    DynamicThumbstickVisual(
                        radius: radius,
                        maximumRadius: maximumRadius,
                        dragOffset: dragOffset,
                        dragDistance: dragDistance,
                        maximumPullPulseToken: maximumPullPulseToken,
                        thumbstickOpacity: thumbstickOpacity,
                        baseOpacity: baseOpacity,
                        trailOpacity: trailOpacity
                    )
                    .position(origin)
                    .transition(.opacity)
                    .allowsHitTesting(false)
                }
            }
            .gesture(dragGesture)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(isLeft ? Text("Dynamic left thumbstick area") : Text("Dynamic right thumbstick area"))
        .accessibilityHint(Text("Press and drag anywhere in this area"))
        .onDisappear(perform: reset)
    }

    private var dragGesture: some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                if !isActive {
                    origin = value.startLocation
                    dragOffset = .zero
                    dragDistance = 0
                    maximumTravel = 0
                    gestureStartedAt = value.time
                    if tapActionsEnabled { onInteractionBegan() }
                    withAnimation(.spring(response: 0.20, dampingFraction: 0.78)) {
                        isActive = true
                    }
                    if hapticsEnabled && SettingsStore.shared.hapticFeedback {
                        GameEventHaptics.shared.light.impactOccurred(intensity: 0.72)
                        GameEventHaptics.shared.medium.prepare()
                    }
                }

                maximumTravel = max(maximumTravel, hypot(value.translation.width, value.translation.height))
                if tapActionsEnabled { onInteractionActivity() }
                let sample = DynamicThumbstickMath.sample(
                    translation: value.translation,
                    maximumRadius: maximumRadius,
                    deadZone: deadZone,
                    deadZoneProgressRadius: deadZoneProgressRadius
                )
                dragOffset = value.translation
                dragDistance = sample.rawDistance
                updateOverextension(sample.rawDistance)
                onVector(sample.input)
            }
            .onEnded { value in
                let duration = value.time.timeIntervalSince(gestureStartedAt ?? value.time)
                let travel = max(maximumTravel, hypot(value.translation.width, value.translation.height))
                if duration <= maximumTapDuration && travel <= tapTravelTolerance {
                    onAnyTap()
                    if tapActionsEnabled {
                        onInteractionTap()
                    } else {
                        pulseStickButton()
                    }
                }
                reset()
            }
    }

    private func pulseStickButton() {
        let button: ARMSX2PadButton = isLeft ? .L3 : .R3
        EmulatorBridge.shared.setPadButton(button, pressed: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.10) {
            EmulatorBridge.shared.setPadButton(button, pressed: false)
        }
    }

    private func updateOverextension(_ distance: CGFloat) {
        let overextended = DynamicThumbstickMath.overextensionState(
            distance: distance,
            maximumRadius: maximumRadius,
            currentlyOverextended: isOverextended
        )
        guard overextended != isOverextended else { return }
        isOverextended = overextended
        if overextended {
            maximumPullPulseToken &+= 1
            if hapticsEnabled && SettingsStore.shared.hapticFeedback {
                GameEventHaptics.shared.medium.impactOccurred(intensity: 1)
                GameEventHaptics.shared.medium.prepare()
            }
        }
    }

    private func reset() {
        guard isActive || gestureStartedAt != nil else {
            onVector(.zero)
            return
        }
        onVector(.zero)
        if tapActionsEnabled && gestureStartedAt != nil { onInteractionEnded() }
        gestureStartedAt = nil
        maximumTravel = 0
        isOverextended = false
        maximumPullPulseToken = 0
        withAnimation(.easeOut(duration: 0.14)) {
            isActive = false
        }
    }
}

private struct DynamicThumbstickVisual: View {
    let radius: CGFloat
    let maximumRadius: CGFloat
    let dragOffset: CGSize
    let dragDistance: CGFloat
    let maximumPullPulseToken: Int
    let thumbstickOpacity: Double
    let baseOpacity: Double
    let trailOpacity: Double

    @State private var maximumPullPulseProgress: CGFloat = 0

    var body: some View {
        ZStack {
            ForEach(0..<7, id: \.self) { index in
                let scale = DynamicThumbstickMath.trailDotScale(index: index, rawDistance: dragDistance, maximumRadius: maximumRadius)
                let progress = DynamicThumbstickMath.trailDotPositionProgress(index: index, rawDistance: dragDistance, maximumRadius: maximumRadius)
                if scale > 0 {
                    Circle()
                        .fill(Color(white: 0.82).opacity(trailOpacity * Double(scale)))
                        .frame(width: 7.5, height: 7.5)
                        .scaleEffect(scale)
                        .offset(x: dragOffset.width * progress, y: dragOffset.height * progress)
                        .animation(.spring(response: 0.18, dampingFraction: 0.72), value: scale)
                }
            }

            Circle()
                .fill(Color(white: 0.72).opacity(baseOpacity))
                .frame(width: radius * 0.42, height: radius * 0.42)
                .overlay {
                    Circle()
                        .stroke(
                            Color.white.opacity(0.55 * maximumPullPulseProgress),
                            lineWidth: 1.5
                        )
                        .scaleEffect(1 + 0.45 * maximumPullPulseProgress)
                }
                .scaleEffect(1 + 0.28 * maximumPullPulseProgress)

            Circle()
                .fill(Color(white: 0.88).opacity(thumbstickOpacity))
                .frame(width: radius * 0.64, height: radius * 0.64)
                .offset(dragOffset)
        }
        .frame(width: radius * 2, height: radius * 2)
        .task(id: maximumPullPulseToken) {
            guard maximumPullPulseToken > 0 else {
                maximumPullPulseProgress = 0
                return
            }
            maximumPullPulseProgress = 0
            withAnimation(.spring(response: 0.18, dampingFraction: 0.58)) {
                maximumPullPulseProgress = 1
            }
            try? await Task.sleep(for: .milliseconds(170))
            guard !Task.isCancelled else { return }
            withAnimation(.easeOut(duration: 0.22)) {
                maximumPullPulseProgress = 0
            }
        }
    }
}

struct VirtualPadCameraSwipeView: View {
    let maximumTapDuration: TimeInterval
    let tapTravelTolerance: CGFloat
    let convertsToDynamicJoystick: Bool
    let thumbstickRadius: CGFloat
    let maximumThumbstickRadius: CGFloat
    let thumbstickDeadZone: CGFloat
    let convertIntoDynamicThumbstickThreshold: CGFloat
    let pullingBackDistance: CGFloat
    let thumbstickOpacity: Double
    let baseOpacity: Double
    let trailOpacity: Double
    let hapticsEnabled: Bool
    let onDelta: (CGSize) -> Void
    let onDynamicJoystick: (DynamicThumbstickVector?) -> Void
    let onBegan: () -> Void
    let onActivity: () -> Void
    let onTap: () -> Void
    let onReleased: () -> Void
    let onEnded: () -> Void

    @State private var lastLocation: CGPoint?
    @State private var gestureStartedAt: Date?
    @State private var maximumTravel: CGFloat = 0
    @State private var origin = CGPoint.zero
    @State private var dragOffset = CGSize.zero
    @State private var dragDistance: CGFloat = 0
    @State private var isDynamicJoystickMode = false
    @State private var isOverextended = false
    @State private var maximumPullPulseToken = 0
    @State private var activeDynamicThumbstickDistance: CGFloat?
    @State private var dynamicJoystickPeakTravel: CGFloat = 0
    @State private var previousRadialTravel: CGFloat = 0

    var body: some View {
        GeometryReader { _ in
            ZStack(alignment: .topLeading) {
                Color.clear
                    .contentShape(Rectangle())

                if isDynamicJoystickMode {
                    DynamicThumbstickVisual(
                        radius: thumbstickRadius,
                        maximumRadius: maximumThumbstickRadius,
                        dragOffset: dragOffset,
                        dragDistance: dragDistance,
                        maximumPullPulseToken: maximumPullPulseToken,
                        thumbstickOpacity: thumbstickOpacity,
                        baseOpacity: baseOpacity,
                        trailOpacity: trailOpacity
                    )
                    .position(origin)
                    .transition(.opacity.combined(with: .scale(scale: 0.9)))
                    .allowsHitTesting(false)
                }
            }
            .gesture(swipeGesture)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Camera swipe area"))
        .accessibilityHint(Text("Swipe to move the emulated right analog stick"))
        .onDisappear(perform: endInteractionIfNeeded)
    }

    private var swipeGesture: some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                if gestureStartedAt == nil {
                    gestureStartedAt = value.time
                    maximumTravel = 0
                    origin = value.startLocation
                    dragOffset = .zero
                    dragDistance = 0
                    activeDynamicThumbstickDistance = thumbstickRadius * min(
                        max(convertIntoDynamicThumbstickThreshold, 0.01),
                        3
                    )
                    dynamicJoystickPeakTravel = 0
                    previousRadialTravel = 0
                    onBegan()
                    if convertsToDynamicJoystick &&
                        hapticsEnabled &&
                        SettingsStore.shared.hapticFeedback {
                        GameEventHaptics.shared.medium.prepare()
                    }
                }
                let rawTravel = hypot(value.translation.width, value.translation.height)
                maximumTravel = max(maximumTravel, rawTravel)
                dragOffset = value.translation
                dragDistance = rawTravel
                onActivity()
                defer {
                    lastLocation = value.location
                    previousRadialTravel = rawTravel
                }

                if convertsToDynamicJoystick {
                    updateOverextension(rawTravel)
                    let entryDistance = activeDynamicThumbstickDistance ??
                        thumbstickRadius * min(max(convertIntoDynamicThumbstickThreshold, 0.01), 3)
                    let pullbackDistance = thumbstickRadius *
                        min(max(pullingBackDistance, 0.01), 1)

                    if isDynamicJoystickMode {
                        dynamicJoystickPeakTravel = max(dynamicJoystickPeakTravel, rawTravel)
                        if dynamicJoystickPeakTravel - rawTravel >= pullbackDistance {
                            onDynamicJoystick(nil)
                            activeDynamicThumbstickDistance = rawTravel
                            dynamicJoystickPeakTravel = 0
                            withAnimation(.spring(response: 0.20, dampingFraction: 0.82)) {
                                isDynamicJoystickMode = false
                            }
                        } else {
                            let sample = DynamicThumbstickMath.sample(
                                translation: value.translation,
                                maximumRadius: maximumThumbstickRadius,
                                deadZone: thumbstickDeadZone
                            )
                            onDynamicJoystick(sample.input)
                        }
                        return
                    }

                    let movingOutward = rawTravel > previousRadialTravel + 0.5
                    if movingOutward && rawTravel >= entryDistance {
                        let sample = DynamicThumbstickMath.sample(
                            translation: value.translation,
                            maximumRadius: maximumThumbstickRadius,
                            deadZone: thumbstickDeadZone
                        )
                        withAnimation(.spring(response: 0.20, dampingFraction: 0.82)) {
                            isDynamicJoystickMode = true
                        }
                        dynamicJoystickPeakTravel = rawTravel
                        onDynamicJoystick(sample.input)
                        return
                    }
                }

                guard let lastLocation else { return }
                let delta = CGSize(
                    width: value.location.x - lastLocation.x,
                    height: value.location.y - lastLocation.y
                )
                if abs(delta.width) > 0.01 || abs(delta.height) > 0.01 {
                    onDelta(delta)
                }
            }
            .onEnded { value in
                let duration = value.time.timeIntervalSince(gestureStartedAt ?? value.time)
                let travel = max(maximumTravel, hypot(value.translation.width, value.translation.height))
                if duration <= maximumTapDuration && travel <= tapTravelTolerance { onTap() }
                onReleased()
                onEnded()
                reset()
            }
    }

    private func endInteractionIfNeeded() {
        if gestureStartedAt != nil { onEnded() }
        reset()
    }

    private func updateOverextension(_ distance: CGFloat) {
        let overextended = DynamicThumbstickMath.overextensionState(
            distance: distance,
            maximumRadius: maximumThumbstickRadius,
            currentlyOverextended: isOverextended
        )
        guard overextended != isOverextended else { return }
        isOverextended = overextended
        if overextended {
            maximumPullPulseToken &+= 1
            if hapticsEnabled && SettingsStore.shared.hapticFeedback {
                GameEventHaptics.shared.medium.impactOccurred(intensity: 1)
                GameEventHaptics.shared.medium.prepare()
            }
        }
    }

    private func reset() {
        if isDynamicJoystickMode {
            onDynamicJoystick(nil)
        }
        lastLocation = nil
        gestureStartedAt = nil
        maximumTravel = 0
        dragOffset = .zero
        dragDistance = 0
        isOverextended = false
        maximumPullPulseToken = 0
        activeDynamicThumbstickDistance = nil
        dynamicJoystickPeakTravel = 0
        previousRadialTravel = 0
        withAnimation(.easeOut(duration: 0.14)) {
            isDynamicJoystickMode = false
        }
    }
}

@MainActor
final class SwipeCameraInputDriver: NSObject {
    private var displayLink: CADisplayLink?
    private var pendingDelta = CGSize.zero
    private var pendingDeltaIsAiming = false
    private var heldJoystickVector: DynamicThumbstickVector?
    private var lastTimestamp: CFTimeInterval?
    private var outputWasActive = false
    var onCameraMotion: ((DynamicThumbstickVector) -> Void)?

    func start() {
        guard displayLink == nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(update(_:)))
        UIFrameRateSettings.shared.configuration.apply(
            to: link,
            domain: .touchNavigation
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    func add(delta: CGSize, isAiming: Bool) {
        guard heldJoystickVector == nil else { return }
        pendingDelta.width += delta.width
        pendingDelta.height += delta.height
        pendingDeltaIsAiming = isAiming
    }

    func setDynamicJoystick(_ vector: DynamicThumbstickVector?) {
        pendingDelta = .zero
        pendingDeltaIsAiming = false

        if let vector {
            let limitedVector = vector.limited(to: 1)
            heldJoystickVector = limitedVector
            publish(limitedVector)
        } else if heldJoystickVector != nil {
            heldJoystickVector = nil
            publish(.zero)
        }
    }

    func stop() {
        displayLink?.invalidate()
        displayLink = nil
        pendingDelta = .zero
        pendingDeltaIsAiming = false
        heldJoystickVector = nil
        lastTimestamp = nil
        publish(.zero)
    }

    @objc private func update(_ link: CADisplayLink) {
        let previous = lastTimestamp ?? (link.timestamp - 1.0 / 60.0)
        let deltaTime = min(max(link.timestamp - previous, 1.0 / 240.0), 1.0 / 20.0)
        lastTimestamp = link.timestamp

        guard heldJoystickVector == nil else { return }

        let delta = pendingDelta
        let isAiming = pendingDeltaIsAiming
        pendingDelta = .zero
        guard delta != .zero else {
            if outputWasActive {
                publish(.zero)
            }
            return
        }

        let settings = DynamicThumbstickSettings.shared
        let sensitivity = settings.effectiveSwipeSensitivity(isAiming: isAiming)
        let degreesPerSecondX = Double(delta.width) * sensitivity.horizontal / deltaTime
        let degreesPerSecondY = Double(delta.height) * sensitivity.vertical / deltaTime
        let referenceDegreesPerSecond = 105.0
        let motion = DynamicThumbstickVector(
            x: CGFloat(degreesPerSecondX / referenceDegreesPerSecond),
            y: CGFloat(degreesPerSecondY / referenceDegreesPerSecond)
        ).limited(to: 1)
        publish(motion)
    }

    private func publish(_ motion: DynamicThumbstickVector) {
        outputWasActive = motion != .zero
        onCameraMotion?(motion)
        EmulatorBridge.shared.setRightStick(
            x: Float(motion.x),
            y: Float(motion.y)
        )
    }
}

@MainActor
final class VirtualPadGyroscopeController {
    private let motionManager = CMMotionManager()
    private var filteredRate = DynamicThumbstickVector.zero
    var onCameraMotion: ((DynamicThumbstickVector) -> Void)?

    init() {
        motionManager.gyroUpdateInterval = 1.0 / 60.0
    }

    func setEnabled(_ enabled: Bool) {
        guard enabled, motionManager.isGyroAvailable else {
            stop()
            return
        }
        guard !motionManager.isGyroActive else { return }

        motionManager.startGyroUpdates(to: .main) { [weak self] data, _ in
            guard let rotationRate = data?.rotationRate else { return }
            Task { @MainActor [weak self] in
                self?.consume(x: rotationRate.x, y: rotationRate.y)
            }
        }
    }

    func stop() {
        motionManager.stopGyroUpdates()
        filteredRate = .zero
        onCameraMotion?(.zero)
        EmulatorBridge.shared.setRightStickMotion(x: 0, y: 0)
    }

    private func consume(x: Double, y: Double) {
        let raw = Self.screenRate(x: x, y: y, orientation: interfaceOrientation)
        let settings = DynamicThumbstickSettings.shared
        let retained = min(max(settings.gyroSmoothing, 0), 0.95)
        let blend = CGFloat(1 - retained)
        filteredRate = DynamicThumbstickVector(
            x: filteredRate.x + (raw.x - filteredRate.x) * blend,
            y: filteredRate.y + (raw.y - filteredRate.y) * blend
        )

        let processed = Self.processedRate(
            filteredRate,
            sensitivity: settings.gyroSensitivity,
            acceleration: settings.gyroAcceleration,
            deadZone: settings.gyroDeadZone,
            maximumRate: settings.gyroMaximumRate,
            invertHorizontal: settings.invertGyroHorizontal,
            invertVertical: settings.invertGyroVertical
        )
        let referenceRadiansPerSecond = CGFloat(105.0 * Double.pi / 180.0)
        let motion = DynamicThumbstickVector(
            x: processed.x / referenceRadiansPerSecond,
            y: processed.y / referenceRadiansPerSecond
        )
        onCameraMotion?(motion)
        EmulatorBridge.shared.setRightStickMotion(
            x: Float(motion.x),
            y: Float(motion.y)
        )
    }

    private var interfaceOrientation: UIInterfaceOrientation {
        UIApplication.shared.appWindowScene?.interfaceOrientation ?? .landscapeRight
    }

    private static func screenRate(x: Double, y: Double, orientation: UIInterfaceOrientation) -> DynamicThumbstickVector {
        switch orientation {
        case .landscapeLeft: return DynamicThumbstickVector(x: x, y: -y)
        case .landscapeRight: return DynamicThumbstickVector(x: -x, y: y)
        case .portraitUpsideDown: return DynamicThumbstickVector(x: y, y: x)
        default: return DynamicThumbstickVector(x: -y, y: -x)
        }
    }

    private static func processedRate(
        _ raw: DynamicThumbstickVector,
        sensitivity: Double,
        acceleration: Double,
        deadZone: Double,
        maximumRate: Double,
        invertHorizontal: Bool,
        invertVertical: Bool
    ) -> DynamicThumbstickVector {
        let magnitude = raw.magnitude
        let deadZone = CGFloat(max(deadZone, 0))
        guard magnitude > deadZone, magnitude > 0 else { return .zero }

        let limit = CGFloat(max(maximumRate, 0.01))
        let adjustedMagnitude = min(magnitude - deadZone, limit)
        let speed = min(adjustedMagnitude / limit, 1)
        let gain = 1 + CGFloat(max(acceleration, 0)) * speed
        let outputMagnitude = adjustedMagnitude * CGFloat(max(sensitivity, 0)) * gain
        return DynamicThumbstickVector(
            x: raw.x / magnitude * outputMagnitude * (invertHorizontal ? -1 : 1),
            y: raw.y / magnitude * outputMagnitude * (invertVertical ? -1 : 1)
        )
    }
}

