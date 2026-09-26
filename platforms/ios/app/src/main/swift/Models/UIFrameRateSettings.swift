// UIFrameRateSettings.swift — user-selectable UI and effect frame pacing
// SPDX-License-Identifier: GPL-3.0+

import Observation
import QuartzCore
import SwiftUI
import UIKit

enum UIFrameRateDomain: Sendable {
    case touchNavigation
    case controllerNavigation
    case controllerEffects
    case dynamicBackground
    case dynamicParticles
    case dynamicFaceButtons
}

enum DynamicWallpaperResolution: String, CaseIterable, Identifiable, Hashable, Sendable {
    case minimum
    case sixth
    case quarter
    case third
    case performance
    case balanced
    case normal
    case high

    var id: String { rawValue }

    var title: String {
        switch self {
        case .minimum: return "Minimum (12.5%)"
        case .sixth: return "Ultra Performance (16.7%)"
        case .quarter: return "Very Low (25%)"
        case .third: return "Low (33.3%)"
        case .performance: return "Performance (50%)"
        case .balanced: return "Balanced (60%)"
        case .normal: return "Normal Quality (75%)"
        case .high: return "High Quality (100%)"
        }
    }

    var renderScale: Double {
        switch self {
        case .minimum: return 0.125
        case .sixth: return 1.0 / 6.0
        case .quarter: return 0.25
        case .third: return 1.0 / 3.0
        case .performance: return 0.50
        case .balanced: return 0.60
        case .normal: return 0.75
        case .high: return 1.00
        }
    }

    /// Pixel resolution and scene complexity are related, but not linearly.
    /// Keep enough ambient detail for every preset to retain its identity while
    /// reducing the number of independently animated objects at low quality.
    var effectQuantityScale: Double {
        switch self {
        case .minimum: return 0.25
        case .sixth: return 0.30
        case .quarter: return 0.40
        case .third: return 0.50
        case .performance: return 0.65
        case .balanced: return 0.75
        case .normal: return 0.88
        case .high: return 1.00
        }
    }

    /// Structural geometry is reduced more conservatively than particles so
    /// ribbons and tower scenes remain recognizable at very low resolution.
    var geometryDetailScale: Double {
        switch self {
        case .minimum: return 0.40
        case .sixth: return 0.45
        case .quarter: return 0.52
        case .third: return 0.60
        case .performance: return 0.72
        case .balanced: return 0.80
        case .normal: return 0.90
        case .high: return 1.00
        }
    }
}

struct UIFrameRateConfiguration: Equatable, Sendable {
    let displayMaximum: Double
    let touchNavigation: Double
    let controllerNavigation: Double
    let controllerEffects: Double
    let dynamicBackground: Double
    let dynamicParticles: Double
    let dynamicFaceButtons: Double
    let dynamicWallpaperResolution: DynamicWallpaperResolution
    let dynamicParticleAmount: Double
    let dynamicFaceButtonResolution: DynamicWallpaperResolution
    let dynamicFaceButtonAmount: Double
    let usesOriginalFullQualityLiveWallpapers: Bool

    var dynamicWallpaperRenderScale: Double {
        usesOriginalFullQualityLiveWallpapers
            ? 1
            : dynamicWallpaperResolution.renderScale
    }

    var effectiveDynamicParticleAmount: Double {
        usesOriginalFullQualityLiveWallpapers
            ? 1
            : dynamicParticleAmount
    }

    var dynamicGeometryDetailScale: Double {
        usesOriginalFullQualityLiveWallpapers
            ? 1
            : dynamicWallpaperResolution.geometryDetailScale
    }

    var dynamicFaceButtonRenderScale: Double {
        usesOriginalFullQualityLiveWallpapers
            ? 1
            : dynamicFaceButtonResolution.renderScale
    }

    var effectiveDynamicFaceButtonAmount: Double {
        usesOriginalFullQualityLiveWallpapers
            ? 1
            : dynamicFaceButtonAmount
    }

    func scaledDynamicEffectCount(_ count: Double) -> Int {
        max(0, Int((count * effectiveDynamicParticleAmount).rounded()))
    }

    func scaledDynamicFaceButtonCount(_ count: Double) -> Int {
        max(0, Int((count * effectiveDynamicFaceButtonAmount).rounded()))
    }

    func scaledDynamicGeometryCount(
        _ count: Double,
        minimum: Int = 1
    ) -> Int {
        max(minimum, Int((count * dynamicGeometryDetailScale).rounded()))
    }

    func reducedDynamicGeometry<Element>(
        _ elements: [Element],
        minimum: Int = 1
    ) -> [Element] {
        guard !elements.isEmpty else { return [] }
        let targetCount = min(
            elements.count,
            scaledDynamicGeometryCount(Double(elements.count), minimum: minimum)
        )
        guard targetCount < elements.count else { return elements }
        guard targetCount > 1 else { return [elements[elements.count / 2]] }

        return (0..<targetCount).map { index in
            let progress = Double(index) / Double(targetCount - 1)
            let sourceIndex = Int(
                (progress * Double(elements.count - 1)).rounded()
            )
            return elements[sourceIndex]
        }
    }

    func framesPerSecond(
        for domain: UIFrameRateDomain,
        maximum: Double? = nil
    ) -> Double {
        let requested: Double
        if usesOriginalFullQualityLiveWallpapers {
            switch domain {
            case .touchNavigation, .controllerNavigation,
                 .controllerEffects:
                requested = displayMaximum
            case .dynamicBackground, .dynamicParticles,
                 .dynamicFaceButtons:
                // Before the performance controls were introduced, the live
                // wallpaper and ambient overlays used an uncapped 30 FPS
                // presentation cadence. Per-background maximums still apply.
                requested = 30
            }
        } else {
            switch domain {
            case .touchNavigation: requested = touchNavigation
            case .controllerNavigation: requested = controllerNavigation
            case .controllerEffects: requested = controllerEffects
            case .dynamicBackground: requested = dynamicBackground
            case .dynamicParticles: requested = dynamicParticles
            case .dynamicFaceButtons: requested = dynamicFaceButtons
            }
        }
        return max(1, min(requested, maximum ?? .greatestFiniteMagnitude, displayMaximum))
    }

    func interval(
        for domain: UIFrameRateDomain,
        maximum: Double? = nil
    ) -> TimeInterval {
        1 / framesPerSecond(for: domain, maximum: maximum)
    }

    func apply(
        to displayLink: CADisplayLink,
        domain: UIFrameRateDomain,
        maximum: Double? = nil
    ) {
        let rate = Float(framesPerSecond(for: domain, maximum: maximum))
        displayLink.preferredFrameRateRange = CAFrameRateRange(
            minimum: rate,
            maximum: rate,
            preferred: rate
        )
    }
}

@MainActor
@Observable
final class UIFrameRateSettings {
    static let shared = UIFrameRateSettings()

    static let displayRefreshRateRange: ClosedRange<Double> = 30...120
    static let navigationRange: ClosedRange<Double> = 30...120
    static let effectRange: ClosedRange<Double> = 15...60
    static let dynamicRange: ClosedRange<Double> = 15...120
    static let dynamicParticleAmountRange: ClosedRange<Double> = 0.25...1.0
    static let dynamicFaceButtonAmountRange: ClosedRange<Double> = 0.25...2.0

    var displayRefreshRateFramesPerSecond: Double {
        didSet {
            persist(
                displayRefreshRateFramesPerSecond,
                key: Keys.displayRefreshRate,
                range: Self.displayRefreshRateRange
            )
        }
    }
    var touchNavigationFramesPerSecond: Double {
        didSet { persist(touchNavigationFramesPerSecond, key: Keys.touchNavigation, range: Self.navigationRange) }
    }
    var controllerNavigationFramesPerSecond: Double {
        didSet { persist(controllerNavigationFramesPerSecond, key: Keys.controllerNavigation, range: Self.navigationRange) }
    }
    var controllerEffectsFramesPerSecond: Double {
        didSet { persist(controllerEffectsFramesPerSecond, key: Keys.controllerEffects, range: Self.effectRange) }
    }
    var dynamicBackgroundFramesPerSecond: Double {
        didSet { persist(dynamicBackgroundFramesPerSecond, key: Keys.dynamicBackground, range: Self.dynamicRange) }
    }
    var dynamicParticleFramesPerSecond: Double {
        didSet { persist(dynamicParticleFramesPerSecond, key: Keys.dynamicParticles, range: Self.effectRange) }
    }
    var dynamicFaceButtonFramesPerSecond: Double {
        didSet { persist(dynamicFaceButtonFramesPerSecond, key: Keys.dynamicFaceButtons, range: Self.effectRange) }
    }
    var dynamicWallpaperResolution: DynamicWallpaperResolution {
        didSet {
            UserDefaults.standard.set(
                dynamicWallpaperResolution.rawValue,
                forKey: Keys.dynamicWallpaperResolution
            )
        }
    }
    var dynamicParticleAmount: Double {
        didSet {
            persist(
                dynamicParticleAmount,
                key: Keys.dynamicParticleAmount,
                range: Self.dynamicParticleAmountRange
            )
        }
    }
    var dynamicFaceButtonResolution: DynamicWallpaperResolution {
        didSet {
            UserDefaults.standard.set(
                dynamicFaceButtonResolution.rawValue,
                forKey: Keys.dynamicFaceButtonResolution
            )
        }
    }
    var dynamicFaceButtonAmount: Double {
        didSet {
            persist(
                dynamicFaceButtonAmount,
                key: Keys.dynamicFaceButtonAmount,
                range: Self.dynamicFaceButtonAmountRange
            )
        }
    }
    var usesOriginalFullQualityLiveWallpapers: Bool {
        didSet {
            UserDefaults.standard.set(
                usesOriginalFullQualityLiveWallpapers,
                forKey: Keys.originalFullQualityLiveWallpapers
            )
        }
    }
    var usesLightweightQualityOnHotTemperature: Bool {
        didSet {
            UserDefaults.standard.set(
                usesLightweightQualityOnHotTemperature,
                forKey: Keys.lightweightQualityOnHotTemperature
            )
        }
    }
    /// Presentation-only quality suppression used by dense foreground editors.
    /// This deliberately is not persisted and never rewrites the user's full-
    /// quality preference.
    private(set) var lightweightLiveWallpaperOverrideCount = 0
    var asksBeforeControllerNavigation: Bool {
        didSet { UserDefaults.standard.set(asksBeforeControllerNavigation, forKey: Keys.askController) }
    }
    var asksBeforeTouchNavigation: Bool {
        didSet { UserDefaults.standard.set(asksBeforeTouchNavigation, forKey: Keys.askTouch) }
    }

    var displayMaximumFramesPerSecond: Int {
        max(30, UIScreen.main.maximumFramesPerSecond)
    }

    var effectiveDisplayRefreshRate: Double {
        min(
            displayRefreshRateFramesPerSecond,
            Double(displayMaximumFramesPerSecond)
        )
    }

    func wallpaperResolutionTitle(
        _ resolution: DynamicWallpaperResolution
    ) -> String {
        let nativeSize = UIScreen.main.nativeBounds.size
        let width = max(1, Int((nativeSize.width * resolution.renderScale).rounded()))
        let height = max(1, Int((nativeSize.height * resolution.renderScale).rounded()))
        return "\(resolution.title) · \(width) × \(height) px"
    }

    var configuration: UIFrameRateConfiguration {
        configuration(usingLightweightLiveWallpaperQuality: false)
    }

    func configuration(
        usingLightweightLiveWallpaperQuality: Bool
    ) -> UIFrameRateConfiguration {
        let originalQuality = usesOriginalFullQualityLiveWallpapers
            && !usingLightweightLiveWallpaperQuality
            && lightweightLiveWallpaperOverrideCount == 0
        return UIFrameRateConfiguration(
            displayMaximum: originalQuality
                ? Double(displayMaximumFramesPerSecond)
                : effectiveDisplayRefreshRate,
            touchNavigation: touchNavigationFramesPerSecond,
            controllerNavigation: controllerNavigationFramesPerSecond,
            controllerEffects: controllerEffectsFramesPerSecond,
            dynamicBackground: dynamicBackgroundFramesPerSecond,
            dynamicParticles: dynamicParticleFramesPerSecond,
            dynamicFaceButtons: dynamicFaceButtonFramesPerSecond,
            dynamicWallpaperResolution: dynamicWallpaperResolution,
            dynamicParticleAmount: dynamicParticleAmount,
            dynamicFaceButtonResolution: dynamicFaceButtonResolution,
            dynamicFaceButtonAmount: dynamicFaceButtonAmount,
            usesOriginalFullQualityLiveWallpapers: originalQuality
        )
    }

    @MainActor
    func beginLightweightLiveWallpaperPresentation() {
        lightweightLiveWallpaperOverrideCount += 1
    }

    @MainActor
    func endLightweightLiveWallpaperPresentation() {
        lightweightLiveWallpaperOverrideCount = max(
            0,
            lightweightLiveWallpaperOverrideCount - 1
        )
    }

    func resetToDefaults() {
        displayRefreshRateFramesPerSecond = 60
        touchNavigationFramesPerSecond = 60
        controllerNavigationFramesPerSecond = 60
        controllerEffectsFramesPerSecond = 30
        dynamicBackgroundFramesPerSecond = 15
        dynamicParticleFramesPerSecond = 15
        dynamicFaceButtonFramesPerSecond = 30
        dynamicWallpaperResolution = .sixth
        dynamicParticleAmount = 0.5
        dynamicFaceButtonResolution = .performance
        dynamicFaceButtonAmount = 0.75
        usesOriginalFullQualityLiveWallpapers = true
        usesLightweightQualityOnHotTemperature = true
        asksBeforeControllerNavigation = true
        asksBeforeTouchNavigation = true
    }

    private enum Keys {
        static let displayRefreshRate = "ARMSX2iOSDisplayRefreshRate"
        static let touchNavigation = "ARMSX2iOSTouchNavigationFPS"
        static let controllerNavigation = "ARMSX2iOSControllerNavigationFPS"
        static let controllerEffects = "ARMSX2iOSControllerEffectsFPS"
        static let dynamicBackground = "ARMSX2iOSDynamicBackgroundFPS"
        static let dynamicParticles = "ARMSX2iOSDynamicParticleFPS"
        static let dynamicFaceButtons = "ARMSX2iOSDynamicFaceButtonFPS"
        static let dynamicWallpaperResolution = "ARMSX2iOSDynamicWallpaperResolution"
        static let dynamicParticleAmount = "ARMSX2iOSDynamicParticleAmount"
        static let dynamicFaceButtonResolution =
            "ARMSX2iOSDynamicFaceButtonResolution"
        static let dynamicFaceButtonAmount = "ARMSX2iOSDynamicFaceButtonAmount"
        static let originalFullQualityLiveWallpapers =
            "ARMSX2iOSUseOriginalFullQualityLiveWallpapers"
        static let lightweightQualityOnHotTemperature =
            "ARMSX2iOSUseLightweightQualityOnHotTemperature"
        static let askController = "ARMSX2iOSAskBeforeControllerNavigation"
        static let askTouch = "ARMSX2iOSAskBeforeTouchNavigation"
    }

    private init() {
        let defaults = UserDefaults.standard
        displayRefreshRateFramesPerSecond = Self.load(
            defaults, key: Keys.displayRefreshRate,
            fallback: 60, range: Self.displayRefreshRateRange
        )
        touchNavigationFramesPerSecond = Self.load(
            defaults, key: Keys.touchNavigation,
            fallback: 60, range: Self.navigationRange
        )
        controllerNavigationFramesPerSecond = Self.load(
            defaults, key: Keys.controllerNavigation,
            fallback: 60, range: Self.navigationRange
        )
        controllerEffectsFramesPerSecond = Self.load(
            defaults, key: Keys.controllerEffects,
            fallback: 30, range: Self.effectRange
        )
        dynamicBackgroundFramesPerSecond = Self.load(
            defaults, key: Keys.dynamicBackground,
            fallback: 15, range: Self.dynamicRange
        )
        dynamicParticleFramesPerSecond = Self.load(
            defaults, key: Keys.dynamicParticles,
            fallback: 15, range: Self.effectRange
        )
        dynamicFaceButtonFramesPerSecond = Self.load(
            defaults, key: Keys.dynamicFaceButtons,
            fallback: 30, range: Self.effectRange
        )
        dynamicWallpaperResolution = DynamicWallpaperResolution(
            rawValue: defaults.string(forKey: Keys.dynamicWallpaperResolution) ?? ""
        ) ?? .sixth
        dynamicParticleAmount = Self.load(
            defaults, key: Keys.dynamicParticleAmount,
            fallback: 0.5, range: Self.dynamicParticleAmountRange
        )
        dynamicFaceButtonResolution = DynamicWallpaperResolution(
            rawValue: defaults.string(forKey: Keys.dynamicFaceButtonResolution) ?? ""
        ) ?? .performance
        dynamicFaceButtonAmount = Self.load(
            defaults, key: Keys.dynamicFaceButtonAmount,
            fallback: 0.75, range: Self.dynamicFaceButtonAmountRange
        )
        usesOriginalFullQualityLiveWallpapers = defaults.object(
            forKey: Keys.originalFullQualityLiveWallpapers
        ) as? Bool ?? true
        usesLightweightQualityOnHotTemperature = defaults.object(
            forKey: Keys.lightweightQualityOnHotTemperature
        ) as? Bool ?? true
        asksBeforeControllerNavigation = defaults.object(forKey: Keys.askController) as? Bool ?? true
        asksBeforeTouchNavigation = defaults.object(forKey: Keys.askTouch) as? Bool ?? true
    }

    private static func load(
        _ defaults: UserDefaults,
        key: String,
        fallback: Double,
        range: ClosedRange<Double>
    ) -> Double {
        let value = defaults.object(forKey: key) as? Double ?? fallback
        return min(max(value, range.lowerBound), range.upperBound)
    }

    private func persist(
        _ value: Double,
        key: String,
        range: ClosedRange<Double>
    ) {
        let clamped = min(max(value, range.lowerBound), range.upperBound)
        UserDefaults.standard.set(clamped, forKey: key)
    }
}

private struct UIFrameRateConfigurationEnvironmentKey: EnvironmentKey {
    static let defaultValue = UIFrameRateConfiguration(
        displayMaximum: 60,
        touchNavigation: 60,
        controllerNavigation: 60,
        controllerEffects: 30,
        dynamicBackground: 15,
        dynamicParticles: 15,
        dynamicFaceButtons: 30,
        dynamicWallpaperResolution: .sixth,
        dynamicParticleAmount: 0.5,
        dynamicFaceButtonResolution: .performance,
        dynamicFaceButtonAmount: 0.75,
        usesOriginalFullQualityLiveWallpapers: true
    )
}

extension EnvironmentValues {
    var uiFrameRateConfiguration: UIFrameRateConfiguration {
        get { self[UIFrameRateConfigurationEnvironmentKey.self] }
        set { self[UIFrameRateConfigurationEnvironmentKey.self] = newValue }
    }
}

struct AdaptiveAnimationTimelineContext {
    let date: Date
}

struct AdaptiveAnimationTimeline<Content: View>: View {
    typealias Context = AdaptiveAnimationTimelineContext

    let domain: UIFrameRateDomain
    var maximumFramesPerSecond: Double? = nil
    var paused = false
    @ViewBuilder let content: (Context) -> Content

    @Environment(\.uiFrameRateConfiguration) private var configuration

    init(
        domain: UIFrameRateDomain,
        maximumFramesPerSecond: Double? = nil,
        paused: Bool = false,
        @ViewBuilder content: @escaping (Context) -> Content
    ) {
        self.domain = domain
        self.maximumFramesPerSecond = maximumFramesPerSecond
        self.paused = paused
        self.content = content
    }

    var body: some View {
        TimelineView(
            .animation(
                minimumInterval: configuration.interval(
                    for: domain,
                    maximum: maximumFramesPerSecond
                ),
                paused: paused
            )
        ) { timeline in
            content(Context(date: timeline.date))
        }
    }
}
