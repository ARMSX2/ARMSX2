// PlayStation3XMBByMartBackground.swift — XMB by Mart background
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import MetalKit
import SwiftUI
import UIKit

// MARK: - PlayStation3XMBMartGradient

struct PlayStation3XMBMartGradient {
  let startColor: Color
  let endColor: Color
  let angleDegrees: Double
  let offsetX: Double
  let offsetY: Double
  let width: Double
  let curvature: Double

  init(
    startColor: Color,
    endColor: Color,
    angleDegrees: Double,
    offsetX: Double = 0,
    offsetY: Double = 0,
    width: Double = 1,
    curvature: Double = 0
  ) {
    self.startColor = startColor
    self.endColor = endColor
    self.angleDegrees = angleDegrees
    self.offsetX = offsetX
    self.offsetY = offsetY
    self.width = width
    self.curvature = curvature
  }

}

struct PlayStation3XMBMartRGB {
  let red: Double
  let green: Double
  let blue: Double

  init(_ red: Int, _ green: Int, _ blue: Int) {
    self.red = Double(red) / 255
    self.green = Double(green) / 255
    self.blue = Double(blue) / 255
  }

  var color: Color {
    Color(red: red, green: green, blue: blue)
  }
}

extension PlayStation3XMBGradientPreset {
  var sourceGradient:
    (angleDegrees: Double, start: PlayStation3XMBMartRGB, end: PlayStation3XMBMartRGB)
  {
    switch self {
    case .theme, .original:
      return (90, PlayStation3XMBMartRGB(3, 8, 24), PlayStation3XMBMartRGB(37, 89, 179))
    case .januaryDay:
      return (90.25, PlayStation3XMBMartRGB(197, 197, 197), PlayStation3XMBMartRGB(201, 201, 201))
    case .januaryNight:
      return (89.75, PlayStation3XMBMartRGB(181, 181, 181), PlayStation3XMBMartRGB(0, 0, 0))
    case .februaryDay:
      return (67, PlayStation3XMBMartRGB(203, 158, 13), PlayStation3XMBMartRGB(219, 214, 41))
    case .februaryNight:
      return (93.75, PlayStation3XMBMartRGB(198, 188, 128), PlayStation3XMBMartRGB(0, 0, 0))
    case .marchDay:
      return (106, PlayStation3XMBMartRGB(142, 190, 40), PlayStation3XMBMartRGB(104, 168, 22))
    case .marchNight:
      return (90.25, PlayStation3XMBMartRGB(152, 170, 113), PlayStation3XMBMartRGB(0, 0, 0))
    case .aprilDay:
      return (136.75, PlayStation3XMBMartRGB(216, 182, 182), PlayStation3XMBMartRGB(231, 66, 117))
    case .aprilNight:
      return (90.25, PlayStation3XMBMartRGB(212, 174, 182), PlayStation3XMBMartRGB(10, 8, 8))
    case .mayDay:
      return (1.5, PlayStation3XMBMartRGB(19, 108, 19), PlayStation3XMBMartRGB(24, 156, 24))
    case .mayNight:
      return (116, PlayStation3XMBMartRGB(48, 118, 48), PlayStation3XMBMartRGB(11, 3, 11))
    case .juneDay:
      return (148.75, PlayStation3XMBMartRGB(198, 120, 238), PlayStation3XMBMartRGB(103, 77, 161))
    case .juneNight:
      return (91, PlayStation3XMBMartRGB(209, 163, 225), PlayStation3XMBMartRGB(0, 0, 0))
    case .julyDay:
      return (26.5, PlayStation3XMBMartRGB(0, 167, 146), PlayStation3XMBMartRGB(10, 240, 239))
    case .julyNight:
      return (109.75, PlayStation3XMBMartRGB(16, 129, 124), PlayStation3XMBMartRGB(17, 0, 0))
    case .augustDay:
      return (62.5, PlayStation3XMBMartRGB(0, 0, 95), PlayStation3XMBMartRGB(33, 217, 255))
    case .augustNight:
      return (69.5, PlayStation3XMBMartRGB(20, 159, 176), PlayStation3XMBMartRGB(0, 0, 31))
    case .septemberDay:
      return (148.5, PlayStation3XMBMartRGB(146, 44, 155), PlayStation3XMBMartRGB(217, 98, 236))
    case .septemberNight:
      return (51, PlayStation3XMBMartRGB(116, 0, 153), PlayStation3XMBMartRGB(12, 0, 11))
    case .octoberDay:
      return (128.5, PlayStation3XMBMartRGB(227, 151, 15), PlayStation3XMBMartRGB(224, 187, 2))
    case .octoberNight:
      return (89.75, PlayStation3XMBMartRGB(216, 142, 0), PlayStation3XMBMartRGB(0, 0, 0))
    case .novemberDay:
      return (90, PlayStation3XMBMartRGB(115, 68, 20), PlayStation3XMBMartRGB(154, 118, 47))
    case .novemberNight:
      return (90, PlayStation3XMBMartRGB(131, 86, 32), PlayStation3XMBMartRGB(18, 20, 17))
    case .decemberDay:
      return (170.5, PlayStation3XMBMartRGB(236, 68, 45), PlayStation3XMBMartRGB(214, 63, 43))
    case .decemberNight:
      return (118.25, PlayStation3XMBMartRGB(157, 59, 44), PlayStation3XMBMartRGB(0, 0, 3))
    }
  }
}

// MARK: - PlayStation3XMBMartMetalSurface

enum PlayStation3XMBMartRenderMode: Equatable {
  case fullBackground
  case foregroundOnly
  case particlesOnly
}

struct PlayStation3XMBMartParticleControls {
  var amount = 1.0
  var speed = 1.0
  var direction = 0.0
  var dispersion = 1.0
  var verticalSpread = 1.0
  var outerDispersion = 0.0
  var verticalLevel = 0.5
  var size = 1.0
  var brightness = 1.0
  var opacity = 1.0
  var depthVariation = 0.0
}

/// `MTKView.autoResizeDrawable` only follows view-size changes. Wallpaper
/// resolution changes leave the bounds untouched, so this subclass explicitly
/// updates the drawable's pixel dimensions whenever its scale or layout changes.
private final class DynamicWallpaperMTKView: MTKView {
  var wallpaperRenderScale: CGFloat = 1 {
    didSet {
      guard abs(wallpaperRenderScale - oldValue) > 0.0001 else { return }
      updateDrawableResolution()
    }
  }

  override func layoutSubviews() {
    super.layoutSubviews()
    updateDrawableResolution()
  }

  override func didMoveToWindow() {
    super.didMoveToWindow()
    updateDrawableResolution()
  }

  func updateDrawableResolution() {
    let nativeScale = window?.screen.scale ?? UIScreen.main.scale
    let pixelsPerPoint = nativeScale * wallpaperRenderScale

    // Keep UIKit layout in native point coordinates. Only the Metal drawable
    // is reduced; changing contentScaleFactor lets SwiftUI/MTKView restore a
    // native-sized drawable during a later layout pass.
    if abs(contentScaleFactor - nativeScale) > 0.0001 {
      contentScaleFactor = nativeScale
    }

    guard bounds.width > 0, bounds.height > 0 else { return }
    let targetSize = CGSize(
      width: max(1, (bounds.width * pixelsPerPoint).rounded()),
      height: max(1, (bounds.height * pixelsPerPoint).rounded())
    )
    guard abs(drawableSize.width - targetSize.width) > 0.5
        || abs(drawableSize.height - targetSize.height) > 0.5
    else {
      return
    }
    drawableSize = targetSize
    (layer as? CAMetalLayer)?.drawableSize = targetSize
  }
}

struct PlayStation3XMBMartMetalSurface: UIViewRepresentable {
  let settings: PlayStation3XMBSettings
  let theme: DynamicBackgroundTheme
  let sessionStartTime: TimeInterval
  var renderMode: PlayStation3XMBMartRenderMode = .fullBackground
  var particleControls = PlayStation3XMBMartParticleControls()
  @Environment(\.uiFrameRateConfiguration) private var frameRates

  func makeCoordinator() -> Coordinator {
    Coordinator(
      renderMode: renderMode,
      sessionStartTime: sessionStartTime,
      geometryDetailScale: frameRates.dynamicGeometryDetailScale
    )
  }

  func makeUIView(context: Context) -> MTKView {
    let view = DynamicWallpaperMTKView(
      frame: .zero,
      device: MTLCreateSystemDefaultDevice()
    )
    view.colorPixelFormat = .bgra8Unorm
    let rendersTransparentSurface = renderMode != .fullBackground
    view.clearColor = MTLClearColor(
      red: 0,
      green: 0,
      blue: 0,
      alpha: rendersTransparentSurface ? 0 : 1
    )
    view.framebufferOnly = true
    view.autoResizeDrawable = false
    updateDrawableResolution(for: view)
    view.preferredFramesPerSecond = preferredFramesPerSecond
    view.enableSetNeedsDisplay = false
    view.isPaused = false
    view.isOpaque = !rendersTransparentSurface
    view.layer.isOpaque = !rendersTransparentSurface
    view.backgroundColor = .clear
    context.coordinator.attach(to: view)
    context.coordinator.renderer?.update(
      settings: settings,
      theme: theme,
      particleControls: particleControls,
      effectDensity: frameRates.effectiveDynamicParticleAmount
    )
    return view
  }

  func updateUIView(_ view: MTKView, context: Context) {
    if view.preferredFramesPerSecond != preferredFramesPerSecond {
      view.preferredFramesPerSecond = preferredFramesPerSecond
    }
    updateDrawableResolution(for: view)
    context.coordinator.renderer?.update(
      settings: settings,
      theme: theme,
      particleControls: particleControls,
      effectDensity: frameRates.effectiveDynamicParticleAmount
    )
  }

  private func updateDrawableResolution(for view: MTKView) {
    guard let view = view as? DynamicWallpaperMTKView else { return }
    view.wallpaperRenderScale = CGFloat(
      frameRates.dynamicWallpaperRenderScale
    )
    view.updateDrawableResolution()
  }

  private var preferredFramesPerSecond: Int {
    let domain: UIFrameRateDomain = renderMode == .particlesOnly
      ? .dynamicParticles
      : .dynamicBackground
    return Int(frameRates.framesPerSecond(for: domain).rounded())
  }

  static func dismantleUIView(_ view: MTKView, coordinator: Coordinator) {
    view.delegate = nil
    view.isPaused = true
    view.enableSetNeedsDisplay = true
    view.releaseDrawables()
    coordinator.detach()
    view.device = nil
  }

  final class Coordinator {
    fileprivate var renderer: PlayStation3XMBMartMetalRenderer?
    private let renderMode: PlayStation3XMBMartRenderMode
    private let sessionStartTime: TimeInterval
    private let geometryDetailScale: Double
    private var retainsShaderLibrary = false

    init(
      renderMode: PlayStation3XMBMartRenderMode,
      sessionStartTime: TimeInterval,
      geometryDetailScale: Double
    ) {
      self.renderMode = renderMode
      self.sessionStartTime = sessionStartTime
      self.geometryDetailScale = geometryDetailScale
    }

    @MainActor
    func attach(to view: MTKView) {
      guard renderer == nil else { return }
      guard let renderer = PlayStation3XMBMartMetalRenderer(
        view: view,
        renderMode: renderMode,
        sessionStartTime: sessionStartTime,
        geometryDetailScale: geometryDetailScale
      ) else {
        PlayStation3XMBByMartShaderLibrary.releaseIfUnused()
        return
      }
      self.renderer = renderer
      PlayStation3XMBByMartShaderLibrary.retainForRenderer()
      retainsShaderLibrary = true
      view.delegate = renderer
    }

    @MainActor
    func detach() {
      renderer = nil
      guard retainsShaderLibrary else {
        PlayStation3XMBByMartShaderLibrary.releaseIfUnused()
        return
      }
      retainsShaderLibrary = false
      PlayStation3XMBByMartShaderLibrary.releaseForRenderer()
    }
  }
}

// MARK: - PlayStation3XMBByMartBackground

// Native SwiftUI adaptation of Mart's MIT-licensed PlayStation 3 XMB recreation.
struct PlayStation3XMBByMartBackground: View {
  let theme: DynamicBackgroundTheme
  @Environment(\.menuBackgroundSessionStart) private var menuBackgroundSessionStart

  @ViewBuilder
  var body: some View {
    if theme.usesPlayStation4PaletteBackdrop {
      ZStack {
        PlayStation4WavesBackground(theme: theme, showsWaves: false)
          .dynamicWallpaperSwiftUIRenderSurface(opaque: true)

        PlayStation3XMBMartMetalSurface(
          settings: settings,
          theme: theme,
          sessionStartTime: menuBackgroundSessionStart.timeIntervalSinceReferenceDate,
          renderMode: .foregroundOnly
        )
      }
      .ignoresSafeArea()
      .accessibilityHidden(true)
    } else {
      PlayStation3XMBMartMetalSurface(
        settings: settings,
        theme: theme,
        sessionStartTime: menuBackgroundSessionStart.timeIntervalSinceReferenceDate
      )
      .ignoresSafeArea()
      .accessibilityHidden(true)
    }
  }

  private var settings: PlayStation3XMBSettings {
    theme.particleSettings.playStation3XMB
  }
}

extension DynamicBackgroundTheme {
  /// Mart's XMB draws the PS4 waves behind itself for a theme gradient on a
  /// shared palette, and that backdrop does not blend a palette change.
  var usesPlayStation4PaletteBackdrop: Bool {
    particleSettings.playStation3XMB.gradientPreset == .theme
      && usesDynamicSharedPalette
  }
}
