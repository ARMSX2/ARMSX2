// DynamicAimCrosshairOverlay.swift — dynamic aim crosshair overlay
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct DynamicAimCrosshairOverlay: View {
    let settings: DynamicThumbstickSettings
    let leftRuntime: DynamicCrosshairRuntimeState
    let rightRuntime: DynamicCrosshairRuntimeState

    private var activeRuntime: DynamicCrosshairRuntimeState? {
        if rightRuntime.isAiming { return rightRuntime }
        if leftRuntime.isAiming { return leftRuntime }
        if settings.showCrosshairWhileHoldingSwipe,
           rightRuntime.isSwipeCrosshairVisible {
            return rightRuntime
        }
        return nil
    }

    var body: some View {
        ZStack {
            if settings.dynamicCrosshairEnabled, let activeRuntime {
                DynamicAimCrosshairView(
                    type: settings.dynamicCrosshairType,
                    animation: settings.dynamicCrosshairAnimation,
                    configuredSize: CGFloat(settings.dynamicCrosshairSize),
                    configuredOpacity: CGFloat(settings.dynamicCrosshairOpacity),
                    runtime: activeRuntime
                )
                .transition(.scale(scale: 0.76).combined(with: .opacity))
            }
        }
        .animation(
            .spring(response: 0.22, dampingFraction: 0.78),
            value: settings.dynamicCrosshairEnabled && activeRuntime != nil
        )
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

private struct DynamicAimCrosshairView: View {
    let type: DynamicCrosshairType
    let animation: DynamicCrosshairAnimation
    let configuredSize: CGFloat
    let configuredOpacity: CGFloat
    let runtime: DynamicCrosshairRuntimeState

    var body: some View {
        let isAnimating = runtime.isCameraMoving ||
            runtime.isCameraSettling ||
            runtime.isShooting ||
            runtime.isRapidFiring
        AdaptiveAnimationTimeline(
            domain: .touchNavigation,
            paused: !isAnimating
        ) { timeline in
            Canvas(rendersAsynchronously: true) { context, canvasSize in
                let metrics = animationMetrics(at: timeline.date.timeIntervalSinceReferenceDate)
                var transformed = context
                transformed.translateBy(
                    x: canvasSize.width / 2 + metrics.offset.width,
                    y: canvasSize.height / 2 + metrics.offset.height
                )
                transformed.rotate(by: .radians(Double(metrics.rotation)))
                transformed.scaleBy(
                    x: metrics.scale * metrics.scaleX,
                    y: metrics.scale * metrics.scaleY
                )
                drawCrosshair(
                    in: &transformed,
                    radius: configuredSize / 2,
                    spread: metrics.spread,
                    opacity: metrics.opacity * min(max(configuredOpacity, 0), 1),
                    movementReaction: metrics.movementReaction,
                    shotReaction: metrics.shotReaction,
                    rapidFireReaction: metrics.rapidFireReaction,
                    motion: metrics.motion
                )
            }
        }
        .frame(width: configuredSize * 2.2, height: configuredSize * 2.2)
    }

    private func animationMetrics(at time: TimeInterval) -> CrosshairAnimationMetrics {
        let moving = runtime.isCameraMoving
        let settling = runtime.isCameraSettling
        let shooting = runtime.isShooting
        let rapid = runtime.isRapidFiring
        let movementPhase = max(time - runtime.movementStartedAt, 0)
        let settlingProgress = settling
            ? min(max((time - runtime.movementEndedAt) / 0.26, 0), 1)
            : 0
        let movementDecay: CGFloat = moving ? 1 : (settling ? CGFloat(1 - settlingProgress) : 0)
        let motion = DynamicThumbstickVector(
            x: runtime.cameraMotion.x * movementDecay,
            y: runtime.cameraMotion.y * movementDecay
        )
        let acceleration = DynamicThumbstickVector(
            x: runtime.cameraAcceleration.x * movementDecay,
            y: runtime.cameraAcceleration.y * movementDecay
        )
        let speed = min(motion.magnitude, 1.25)
        let accelerationStrength = min(acceleration.magnitude, 1)
        let normalizedSpeed = min(speed, 1)
        let direction = atan2(Double(motion.y), Double(motion.x))
        let shotProgress = shooting ? min(max((time - runtime.shotStartedAt) / 0.16, 0), 1) : 1
        let shotImpulse = shooting ? CGFloat(1 - shotProgress) : 0
        let rapidPhase = max(time - runtime.rapidFireStartedAt, 0)
        let liveFrequency = 8 + Double(normalizedSpeed) * 14
        let moveWave = movementDecay * CGFloat(sin(movementPhase * liveFrequency))
        let rapidWave = rapid ? CGFloat(sin(rapidPhase * 34)) : 0
        let perpendicular = DynamicThumbstickVector(x: -motion.y, y: motion.x)

        var result = CrosshairAnimationMetrics()
        switch animation {
        case .reactive:
            result.spread = 1 + speed * 0.20 + moveWave * 0.025 + accelerationStrength * 0.05 + shotImpulse * 0.30
            result.scale = 1 - shotImpulse * 0.10 + (rapid ? rapidWave * 0.045 : 0)
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.030,
                height: motion.y * configuredSize * 0.030
            )
        case .pulse:
            result.scale = 1 + moveWave * (0.025 + speed * 0.06) + speed * 0.025 + shotImpulse * 0.22 +
                (rapid ? rapidWave * 0.10 : 0)
            result.opacity = 1 - abs(moveWave) * speed * 0.08 - (rapid ? abs(rapidWave) * 0.08 : 0)
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.018,
                height: motion.y * configuredSize * 0.018
            )
        case .expand:
            result.spread = 1 + speed * 0.34 + abs(moveWave) * speed * 0.09 + shotImpulse * 0.48
            if rapid { result.spread += 0.14 + abs(rapidWave) * 0.16 }
            result.scaleX = 1 + abs(motion.x) * 0.09
            result.scaleY = 1 + abs(motion.y) * 0.09
        case .rotate:
            result.rotation = CGFloat(movementPhase * (0.45 + Double(speed) * 1.1)) * movementDecay
            result.spread = 1 + speed * 0.14 + shotImpulse * 0.28
            result.scale = 1 + shotImpulse * 0.08
        case .recoil:
            result.offset.height = motion.y * configuredSize * 0.042 - configuredSize * shotImpulse * 0.16
            result.offset.width = motion.x * configuredSize * 0.042 +
                (rapid ? configuredSize * rapidWave * 0.055 : 0)
            result.spread = 1 + speed * 0.14 + shotImpulse * 0.20
        case .orbit:
            let phase = rapid ? rapidPhase * 7 : movementPhase * (2.5 + Double(speed) * 3)
            let activity: CGFloat = rapid ? 1 : speed
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.035 +
                    CGFloat(cos(phase + direction)) * configuredSize * 0.055 * activity,
                height: motion.y * configuredSize * 0.035 +
                    CGFloat(sin(phase + direction)) * configuredSize * 0.055 * activity
            )
            result.spread = 1 + shotImpulse * 0.32
        case .focus:
            result.spread = 0.90 + speed * 0.36 + abs(moveWave) * speed * 0.05
            if shooting { result.spread -= shotImpulse * 0.20 }
            if rapid { result.spread += rapidWave * 0.08 }
            result.scale = 1 + shotImpulse * 0.08
            result.offset = CGSize(
                width: -motion.x * configuredSize * 0.020,
                height: -motion.y * configuredSize * 0.020
            )
        case .wave:
            result.scale = 1 + moveWave * speed * 0.075 + (rapid ? rapidWave * 0.08 : 0)
            result.spread = 1 + speed * 0.10 + shotImpulse * 0.36
            result.opacity = 1 - CGFloat(abs(rapidWave)) * 0.10
            result.offset = CGSize(
                width: perpendicular.x * configuredSize * moveWave * 0.035,
                height: perpendicular.y * configuredSize * moveWave * 0.035
            )
        case .directional:
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.085,
                height: motion.y * configuredSize * 0.085
            )
            result.scaleX = 1 + abs(motion.x) * 0.14
            result.scaleY = 1 + abs(motion.y) * 0.14
            result.spread = 1 + speed * 0.13 + shotImpulse * 0.30
        case .elastic:
            result.offset = CGSize(
                width: (-motion.x * 0.075 + acceleration.x * 0.055) * configuredSize,
                height: (-motion.y * 0.075 + acceleration.y * 0.055) * configuredSize
            )
            result.scaleX = 1 + abs(motion.x) * 0.10 - abs(motion.y) * 0.025
            result.scaleY = 1 + abs(motion.y) * 0.10 - abs(motion.x) * 0.025
            result.spread = 1 + speed * 0.12 + accelerationStrength * 0.12 + shotImpulse * 0.28
        case .parallax:
            result.offset = CGSize(
                width: -motion.x * configuredSize * 0.095,
                height: -motion.y * configuredSize * 0.095
            )
            result.scaleX = 1 + abs(motion.y) * 0.045
            result.scaleY = 1 + abs(motion.x) * 0.045
            result.spread = 1 + speed * 0.18 + shotImpulse * 0.30
        case .velocity:
            result.spread = 1 + speed * 0.52 + accelerationStrength * 0.08 + shotImpulse * 0.35
            result.scale = 1 + speed * 0.055 - shotImpulse * 0.08
            result.scaleX = 1 + abs(motion.x) * 0.12
            result.scaleY = 1 + abs(motion.y) * 0.12
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.025,
                height: motion.y * configuredSize * 0.025
            )
        case .stabilizer:
            result.offset = CGSize(
                width: -motion.x * configuredSize * 0.042,
                height: -motion.y * configuredSize * 0.042
            )
            result.spread = 1 - speed * 0.10 + accelerationStrength * 0.04 + shotImpulse * 0.26
            result.scaleX = 1 + abs(motion.y) * 0.025
            result.scaleY = 1 + abs(motion.x) * 0.025
        case .snap:
            result.offset = CGSize(
                width: acceleration.x * configuredSize * 0.095,
                height: acceleration.y * configuredSize * 0.095
            )
            result.spread = 1 + speed * 0.10 + accelerationStrength * 0.30 + shotImpulse * 0.34
            result.scale = 1 + accelerationStrength * 0.08
        case .drift:
            result.offset = CGSize(
                width: (motion.x * 0.050 + perpendicular.x * moveWave * 0.030) * configuredSize,
                height: (motion.y * 0.050 + perpendicular.y * moveWave * 0.030) * configuredSize
            )
            result.spread = 1 + speed * 0.15 + shotImpulse * 0.28
        case .tilt:
            result.rotation = runtime.shotTiltRadians * shotImpulse * 1.35
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.025,
                height: motion.y * configuredSize * 0.025
            )
            result.spread = 1 + speed * 0.16 + shotImpulse * 0.30
        case .bloom:
            result.spread = 1 + speed * 0.42 + abs(moveWave) * speed * 0.10 + shotImpulse * 0.45
            result.scale = 1 + speed * 0.07 + moveWave * speed * 0.035
            result.opacity = 1 - speed * 0.12 + abs(moveWave) * speed * 0.04
            result.scaleX = 1 + abs(motion.x) * 0.08
            result.scaleY = 1 + abs(motion.y) * 0.08
        case .directionLock:
            result.offset = CGSize(
                width: motion.x * configuredSize * 0.045,
                height: motion.y * configuredSize * 0.045
            )
            result.scaleY = 1 + speed * 0.18
            result.scaleX = 1 - min(speed * 0.045, 0.045)
            result.spread = 1 + speed * 0.20 + shotImpulse * 0.32
        }

        // Every type receives a small live deformation even when its selected
        // animation emphasizes only one axis or behavior.
        result.scaleX *= 1 + abs(motion.x) * 0.025
        result.scaleY *= 1 + abs(motion.y) * 0.025
        result.offset.width += motion.x * configuredSize * 0.008
        result.offset.height += motion.y * configuredSize * 0.008
        if animation != .tilt {
            result.rotation += runtime.shotTiltRadians * shotImpulse
        }
        result.movementReaction = min(speed + accelerationStrength * 0.22, 1.35)
        result.shotReaction = shotImpulse
        result.rapidFireReaction = rapid ? 1 + rapidWave * 0.12 : 0
        result.motion = motion
        return result
    }

    private func drawCrosshair(
        in context: inout GraphicsContext,
        radius: CGFloat,
        spread: CGFloat,
        opacity: CGFloat,
        movementReaction: CGFloat,
        shotReaction: CGFloat,
        rapidFireReaction: CGFloat,
        motion: DynamicThumbstickVector
    ) {
        let lineWidth = max(radius / 28, 0.75)
        let gap = radius * 0.27 * spread
        let armEnd = radius * min(max(spread, 0.72), 1.55)
        let fireReaction = max(shotReaction, rapidFireReaction)
        let isFiring = fireReaction > 0
        let reactiveColour = isFiring ? Color(red: 1, green: 0.16, blue: 0.10) : .white
        let reactiveOpacity = isFiring ? min(opacity, 0.40) : opacity

        switch type {
        case .classic:
            stroke(
                directionalRadialLines(
                    count: 4,
                    innerRadius: gap,
                    outerRadius: armEnd,
                    motion: motion,
                    reaction: movementReaction,
                    radius: radius
                ),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .dot:
            motionTrailDots(
                motion: motion,
                radius: radius,
                reaction: movementReaction,
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            dot(
                at: .zero,
                radius: max(radius * 0.075, 1.25),
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .circle:
            stroke(
                circle(radius: radius * 0.52 * spread),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .circleDot:
            stroke(
                circle(radius: radius * 0.52 * spread),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            motionTrailDots(
                motion: motion,
                radius: radius,
                reaction: movementReaction,
                in: &context,
                opacity: reactiveOpacity * 0.75,
                foreground: reactiveColour
            )
            dot(
                at: .zero,
                radius: max(radius * 0.06, 1.1),
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .cross:
            stroke(
                directionalRadialLines(
                    count: 4,
                    innerRadius: 0,
                    outerRadius: armEnd,
                    motion: motion,
                    reaction: movementReaction,
                    radius: radius
                ),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .chevron:
            var path = Path()
            path.move(to: CGPoint(x: -radius * 0.54 * spread, y: radius * 0.28))
            path.addLine(to: CGPoint(x: 0, y: -radius * 0.24 * spread))
            path.addLine(to: CGPoint(x: radius * 0.54 * spread, y: radius * 0.28))
            stroke(
                path,
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .brackets:
            stroke(
                cornerBrackets(radius: radius * spread),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .diamond:
            var path = Path()
            path.move(to: CGPoint(x: 0, y: -radius * 0.62 * spread))
            path.addLine(to: CGPoint(x: radius * 0.62 * spread, y: 0))
            path.addLine(to: CGPoint(x: 0, y: radius * 0.62 * spread))
            path.addLine(to: CGPoint(x: -radius * 0.62 * spread, y: 0))
            path.closeSubpath()
            stroke(
                path,
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .shotgun:
            stroke(
                circle(radius: radius * 0.48 * spread),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            for index in 0..<6 {
                let angle = Double(index) * .pi / 3
                dot(
                    at: CGPoint(
                        x: CGFloat(cos(angle)) * radius * 0.72 * spread,
                        y: CGFloat(sin(angle)) * radius * 0.72 * spread
                    ),
                    radius: max(radius * 0.045, 1),
                    in: &context,
                    opacity: reactiveOpacity,
                    foreground: reactiveColour
                )
            }
        case .sniper:
            stroke(
                circle(radius: radius * 0.56 * spread),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            stroke(
                directionalRadialLines(
                    count: 4,
                    innerRadius: radius * 0.68 * spread,
                    outerRadius: armEnd,
                    motion: motion,
                    reaction: movementReaction,
                    radius: radius
                ),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            dot(
                at: .zero,
                radius: max(radius * 0.05, 1),
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .tactical:
            stroke(
                directionalRadialLines(
                    count: 4,
                    innerRadius: gap * 1.08,
                    outerRadius: armEnd * 0.88,
                    motion: motion,
                    reaction: movementReaction,
                    radius: radius
                ),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            dot(
                at: .zero,
                radius: max(radius * 0.055, 1.1),
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            var marker = Path()
            marker.move(to: CGPoint(x: 0, y: -radius * 0.98 * spread))
            marker.addLine(to: CGPoint(x: -radius * 0.11, y: -radius * 0.80 * spread))
            marker.addLine(to: CGPoint(x: radius * 0.11, y: -radius * 0.80 * spread))
            marker.closeSubpath()
            fill(
                marker,
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .burst:
            stroke(
                directionalRadialLines(
                    count: 8,
                    innerRadius: gap,
                    outerRadius: armEnd,
                    motion: motion,
                    reaction: movementReaction,
                    radius: radius
                ),
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .fourBoxes:
            let boxDistance = radius * (
                0.52 +
                0.13 * movementReaction +
                0.24 * fireReaction
            )
            let boxHalfSide = max(radius * 0.105, 1.25)
            for point in [
                CGPoint(x: 0, y: -boxDistance),
                CGPoint(x: boxDistance, y: 0),
                CGPoint(x: 0, y: boxDistance),
                CGPoint(x: -boxDistance, y: 0)
            ] {
                square(
                    at: point,
                    halfSide: boxHalfSide,
                    in: &context,
                    opacity: reactiveOpacity,
                    foreground: reactiveColour
                )
            }

            let restingLineLength = radius * 0.28
            let lineLength = restingLineLength +
                radius * 0.26 * movementReaction +
                radius * 0.24 * abs(motion.x) +
                radius * 0.18 * fireReaction
            let lineInner = boxDistance + boxHalfSide + radius * 0.14
            var horizontalLines = Path()
            horizontalLines.move(to: CGPoint(x: lineInner, y: 0))
            horizontalLines.addLine(to: CGPoint(x: lineInner + lineLength, y: 0))
            horizontalLines.move(to: CGPoint(x: -lineInner, y: 0))
            horizontalLines.addLine(to: CGPoint(x: -(lineInner + lineLength), y: 0))
            stroke(
                horizontalLines,
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .triad:
            let outward = 1 + movementReaction * 0.20 + fireReaction * 0.36
            let baseSegmentCentre = radius * 0.58 * outward
            let segmentHalfLength = radius * (
                0.12 +
                0.08 * movementReaction +
                0.06 * fireReaction
            )
            var segments = Path()
            for angle in [-Double.pi / 6, -5 * Double.pi / 6, Double.pi / 2] {
                let direction = CGPoint(x: CGFloat(cos(angle)), y: CGFloat(sin(angle)))
                let directionalPush = (direction.x * motion.x + direction.y * motion.y) * radius * 0.13
                let segmentCentre = baseSegmentCentre + directionalPush
                segments.move(to: CGPoint(
                    x: direction.x * (segmentCentre - segmentHalfLength),
                    y: direction.y * (segmentCentre - segmentHalfLength)
                ))
                segments.addLine(to: CGPoint(
                    x: direction.x * (segmentCentre + segmentHalfLength),
                    y: direction.y * (segmentCentre + segmentHalfLength)
                ))
            }
            stroke(
                segments,
                in: &context,
                lineWidth: lineWidth,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        case .reactiveDot:
            let dotScale = 1 + movementReaction * 0.08 + fireReaction * 0.30
            motionTrailDots(
                motion: motion,
                radius: radius,
                reaction: movementReaction,
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
            dot(
                at: .zero,
                radius: max(radius * 0.055, 0.95) * dotScale,
                in: &context,
                opacity: reactiveOpacity,
                foreground: reactiveColour
            )
        }
    }

    private func directionalRadialLines(
        count: Int,
        innerRadius: CGFloat,
        outerRadius: CGFloat,
        motion: DynamicThumbstickVector,
        reaction: CGFloat,
        radius: CGFloat
    ) -> Path {
        var path = Path()
        for index in 0..<count {
            let angle = Double(index) * 2 * .pi / Double(count)
            let direction = CGPoint(x: CGFloat(cos(angle)), y: CGFloat(sin(angle)))
            let projection = direction.x * motion.x + direction.y * motion.y
            let directionalExtension = projection * radius * 0.14 * reaction
            let directionalGap = max(projection, 0) * radius * 0.025 * reaction
            path.move(to: CGPoint(
                x: direction.x * (innerRadius + directionalGap),
                y: direction.y * (innerRadius + directionalGap)
            ))
            path.addLine(to: CGPoint(
                x: direction.x * max(outerRadius + directionalExtension, innerRadius + 1),
                y: direction.y * max(outerRadius + directionalExtension, innerRadius + 1)
            ))
        }
        return path
    }

    private func motionTrailDots(
        motion: DynamicThumbstickVector,
        radius: CGFloat,
        reaction: CGFloat,
        in context: inout GraphicsContext,
        opacity: CGFloat,
        foreground: Color = .white
    ) {
        guard reaction > 0.015 else { return }
        for index in 1...2 {
            let progress = CGFloat(index) / 2
            dot(
                at: CGPoint(
                    x: -motion.x * radius * 0.34 * progress,
                    y: -motion.y * radius * 0.34 * progress
                ),
                radius: max(radius * 0.045 * (1 - progress * 0.34), 0.7),
                in: &context,
                opacity: opacity * (0.34 - progress * 0.12),
                foreground: foreground
            )
        }
    }

    private func cornerBrackets(radius: CGFloat) -> Path {
        let inner = radius * 0.38
        let outer = radius * 0.72
        var path = Path()
        for x: CGFloat in [-1, 1] {
            for y: CGFloat in [-1, 1] {
                path.move(to: CGPoint(x: x * inner, y: y * outer))
                path.addLine(to: CGPoint(x: x * outer, y: y * outer))
                path.addLine(to: CGPoint(x: x * outer, y: y * inner))
            }
        }
        return path
    }

    private func circle(radius: CGFloat) -> Path {
        Path(ellipseIn: CGRect(x: -radius, y: -radius, width: radius * 2, height: radius * 2))
    }

    private func stroke(
        _ path: Path,
        in context: inout GraphicsContext,
        lineWidth: CGFloat,
        opacity: CGFloat,
        foreground: Color = .white
    ) {
        context.stroke(path, with: .color(.black.opacity(0.72 * Double(opacity))), lineWidth: lineWidth + 1.15)
        context.stroke(path, with: .color(foreground.opacity(0.96 * Double(opacity))), lineWidth: lineWidth)
    }

    private func dot(
        at center: CGPoint,
        radius: CGFloat,
        in context: inout GraphicsContext,
        opacity: CGFloat,
        foreground: Color = .white
    ) {
        let rect = CGRect(
            x: center.x - radius,
            y: center.y - radius,
            width: radius * 2,
            height: radius * 2
        )
        context.fill(
            Path(ellipseIn: rect.insetBy(dx: -0.65, dy: -0.65)),
            with: .color(.black.opacity(0.72 * Double(opacity)))
        )
        context.fill(Path(ellipseIn: rect), with: .color(foreground.opacity(0.96 * Double(opacity))))
    }

    private func square(
        at center: CGPoint,
        halfSide: CGFloat,
        in context: inout GraphicsContext,
        opacity: CGFloat,
        foreground: Color
    ) {
        let rect = CGRect(
            x: center.x - halfSide,
            y: center.y - halfSide,
            width: halfSide * 2,
            height: halfSide * 2
        )
        context.fill(
            Path(rect.insetBy(dx: -0.65, dy: -0.65)),
            with: .color(.black.opacity(0.72 * Double(opacity)))
        )
        context.fill(Path(rect), with: .color(foreground.opacity(0.96 * Double(opacity))))
    }

    private func fill(
        _ path: Path,
        in context: inout GraphicsContext,
        opacity: CGFloat,
        foreground: Color = .white
    ) {
        context.stroke(path, with: .color(.black.opacity(0.72 * Double(opacity))), lineWidth: 1.15)
        context.fill(path, with: .color(foreground.opacity(0.96 * Double(opacity))))
    }
}

private struct CrosshairAnimationMetrics {
    var scale: CGFloat = 1
    var scaleX: CGFloat = 1
    var scaleY: CGFloat = 1
    var spread: CGFloat = 1
    var offset = CGSize.zero
    var rotation: CGFloat = 0
    var opacity: CGFloat = 1
    var movementReaction: CGFloat = 0
    var shotReaction: CGFloat = 0
    var rapidFireReaction: CGFloat = 0
    var motion = DynamicThumbstickVector.zero
}
