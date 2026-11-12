// PlayStation3XMBMartMetalRenderer.swift — XMB by Mart Metal renderer
// SPDX-License-Identifier: GPL-3.0+

import Foundation
@preconcurrency import MetalKit
import SwiftUI
import UIKit

// MARK: - PlayStation3XMBByMartMetalRenderer

private struct XMBBackgroundUniforms {
  var startColor: SIMD4<Float>
  var endColor: SIMD4<Float>
  var directionRange: SIMD4<Float>
  var geometry: SIMD4<Float>
}

private struct XMBWaveUniforms {
  var timing: SIMD4<Float>
  var geometry: SIMD4<Float>
  var shaping: SIMD4<Float>
  var detail: SIMD4<Float>
  var ffdScale1: SIMD4<Float>
  var ffdScale2: SIMD4<Float>
  var ffdOffset: SIMD4<Float>
  var material: SIMD4<Float>
  var color: SIMD4<Float>
}

private struct XMBParticleUniforms {
  var timing: SIMD4<Float>
  var sizing: SIMD4<Float>
  var color: SIMD4<Float>
  var distribution: SIMD4<Float>
  var waveFollowing: SIMD4<Float>
}

private struct XMBSessionRandomNumberGenerator: RandomNumberGenerator {
  private var state: UInt64

  init(seed: UInt64) {
    state = seed == 0 ? 0x9e37_79b9_7f4a_7c15 : seed
  }

  mutating func next() -> UInt64 {
    state ^= state >> 12
    state ^= state << 25
    state ^= state >> 27
    return state &* 0x2545_f491_4f6c_dd1d
  }
}

final class PlayStation3XMBMartMetalRenderer: NSObject, MTKViewDelegate {
  private static let paletteTransitionDuration: TimeInterval = 0.46

  private let renderMode: PlayStation3XMBMartRenderMode
  private let sessionStartTime: TimeInterval
  private let particleTimeOffset: TimeInterval
  private let particleSeed: UInt64
  private let commandQueue: MTLCommandQueue
  private let backgroundPipeline: MTLRenderPipelineState
  private let wavePipeline: MTLRenderPipelineState
  private let particlePipeline: MTLRenderPipelineState
  private let waveVertexBuffer: MTLBuffer
  private let waveIndexBuffer: MTLBuffer
  private let waveIndexCount: Int
  private let splineTexture: MTLTexture
  private let splinePipeline: PlayStation3XMBMartSplinePipeline

  private var settings = PlayStation3XMBSettings()
  private var theme: DynamicBackgroundTheme?
  private var particleControls = PlayStation3XMBMartParticleControls()
  private var backgroundUniforms = XMBBackgroundUniforms(
    startColor: SIMD4(0, 0, 0, 1),
    endColor: SIMD4(0.1, 0.3, 0.7, 1),
    directionRange: SIMD4(0, 1, 0, 1),
    geometry: SIMD4(0, 0, 1, 0)
  )
  private var waveColor = SIMD4<Float>(1, 1, 1, 1)
  private var paletteTransitionStartTime: TimeInterval?
  private var paletteTransitionSourceBackgroundUniforms: XMBBackgroundUniforms?
  private var paletteTransitionSourceWaveColor: SIMD4<Float>?
  private var particleBuffer: MTLBuffer?
  private var particleCount = 0
  private var displacementValues: [Float]
  private var splineTime: TimeInterval = 0
  private var particleTime: TimeInterval = 0

  @MainActor
  init?(
    view: MTKView,
    renderMode: PlayStation3XMBMartRenderMode,
    sessionStartTime: TimeInterval,
    geometryDetailScale: Double
  ) {
    guard let device = view.device,
      let commandQueue = device.makeCommandQueue(),
      let library = PlayStation3XMBByMartShaderLibrary.makeLibrary(device: device),
      let backgroundVertex = library.makeFunction(name: "xmbBackgroundVertex"),
      let backgroundFragment = library.makeFunction(name: "xmbBackgroundFragment"),
      let waveVertex = library.makeFunction(name: "xmbWaveVertex"),
      let waveFragment = library.makeFunction(name: "xmbWaveFragment"),
      let particleVertex = library.makeFunction(name: "xmbParticleVertex"),
      let particleFragment = library.makeFunction(name: "xmbParticleFragment")
    else {
      return nil
    }

    let backgroundDescriptor = MTLRenderPipelineDescriptor()
    backgroundDescriptor.vertexFunction = backgroundVertex
    backgroundDescriptor.fragmentFunction = backgroundFragment
    backgroundDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat

    let waveDescriptor = MTLRenderPipelineDescriptor()
    waveDescriptor.vertexFunction = waveVertex
    waveDescriptor.fragmentFunction = waveFragment
    waveDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
    waveDescriptor.colorAttachments[0].isBlendingEnabled = true
    waveDescriptor.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
    waveDescriptor.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
    waveDescriptor.colorAttachments[0].sourceAlphaBlendFactor = .one
    waveDescriptor.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha

    let particleDescriptor = MTLRenderPipelineDescriptor()
    particleDescriptor.vertexFunction = particleVertex
    particleDescriptor.fragmentFunction = particleFragment
    particleDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
    particleDescriptor.colorAttachments[0].isBlendingEnabled = true
    particleDescriptor.colorAttachments[0].sourceRGBBlendFactor = .one
    particleDescriptor.colorAttachments[0].destinationRGBBlendFactor = .one
    particleDescriptor.colorAttachments[0].sourceAlphaBlendFactor = .one
    particleDescriptor.colorAttachments[0].destinationAlphaBlendFactor = .one

    guard
      let backgroundPipeline = try? device.makeRenderPipelineState(
        descriptor: backgroundDescriptor
      ),
      let wavePipeline = try? device.makeRenderPipelineState(
        descriptor: waveDescriptor
      ),
      let particlePipeline = try? device.makeRenderPipelineState(
        descriptor: particleDescriptor
      )
    else {
      return nil
    }

    let clampedDetailScale = min(1, max(0.25, geometryDetailScale))
    let gridResolution = max(
      32,
      min(100, Int((100 * clampedDetailScale).rounded()))
    )
    let grid = Self.makeWaveGrid(resolution: gridResolution)
    guard
      let waveVertexBuffer = device.makeBuffer(
        bytes: grid.vertices,
        length: MemoryLayout<SIMD2<Float>>.stride * grid.vertices.count,
        options: .storageModeShared
      ),
      let waveIndexBuffer = device.makeBuffer(
        bytes: grid.indices,
        length: MemoryLayout<UInt16>.stride * grid.indices.count,
        options: .storageModeShared
      )
    else {
      return nil
    }

    let splinePipeline = PlayStation3XMBMartSplinePipeline(
      detailScale: clampedDetailScale
    )
    let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(
      pixelFormat: .r32Float,
      width: splinePipeline.textureWidth,
      height: splinePipeline.textureHeight,
      mipmapped: false
    )
    textureDescriptor.usage = .shaderRead
    textureDescriptor.storageMode = .shared
    guard let splineTexture = device.makeTexture(descriptor: textureDescriptor) else {
      return nil
    }

    self.renderMode = renderMode
    self.sessionStartTime = sessionStartTime
    self.particleTimeOffset =
      sessionStartTime.truncatingRemainder(dividingBy: 1000)
    let renderModeSalt: UInt64 =
      switch renderMode {
      case .fullBackground: 0x1465_0fb0_739d_0383
      case .foregroundOnly: 0x9e37_79b9_7f4a_7c15
      case .particlesOnly: 0xd1b5_4a32_d192_ed03
      }
    self.particleSeed = sessionStartTime.bitPattern ^ renderModeSalt
    self.commandQueue = commandQueue
    self.backgroundPipeline = backgroundPipeline
    self.wavePipeline = wavePipeline
    self.particlePipeline = particlePipeline
    self.waveVertexBuffer = waveVertexBuffer
    self.waveIndexBuffer = waveIndexBuffer
    self.waveIndexCount = grid.indices.count
    self.splineTexture = splineTexture
    self.splinePipeline = splinePipeline
    self.displacementValues = Array(
      repeating: 0,
      count: splinePipeline.textureWidth * splinePipeline.textureHeight
    )
    super.init()

    rebuildParticles(device: device, count: Int(settings.particleCount))
  }

  func update(
    settings: PlayStation3XMBSettings,
    theme: DynamicBackgroundTheme,
    particleControls: PlayStation3XMBMartParticleControls,
    effectDensity: Double
  ) {
    if let currentTheme = self.theme, currentTheme != theme {
      paletteTransitionStartTime = CFAbsoluteTimeGetCurrent()
      paletteTransitionSourceBackgroundUniforms = backgroundUniforms
      paletteTransitionSourceWaveColor = waveColor
    }
    self.settings = settings
    self.theme = theme
    self.particleControls = particleControls

    let density =
      renderMode == .particlesOnly
      ? particleControls.amount
      : settings.particleVerticalDensity
    let desiredCount = Int(
      min(
        4000,
        max(4, (settings.particleCount * density * effectDensity).rounded())
      )
    )
    if desiredCount != particleCount {
      rebuildParticles(device: commandQueue.device, count: desiredCount)
    }
  }

  func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

  func draw(in view: MTKView) {
    let elapsed = max(0, CFAbsoluteTimeGetCurrent() - sessionStartTime)
    splineTime = elapsed
    particleTime = particleTimeOffset + elapsed

    if let theme {
      let paletteTime = CFAbsoluteTimeGetCurrent()
      let progress = paletteTransitionProgress(at: paletteTime)
      if renderMode == .fullBackground {
        let gradient = PlayStation3XMBMartThemeResolver.backgroundGradient(
          theme: theme,
          settings: settings,
          time: paletteTime
        )
        let targetUniforms = makeBackgroundUniforms(gradient: gradient)
        backgroundUniforms = paletteTransitionSourceBackgroundUniforms.map {
          interpolate($0, targetUniforms, progress: progress)
        } ?? targetUniforms
      }
      let targetWaveColor = colorComponents(
        theme.highlightColor(index: 0, time: paletteTime)
      )
      waveColor = paletteTransitionSourceWaveColor.map {
        interpolate($0, targetWaveColor, progress: progress)
      } ?? targetWaveColor

      if progress >= 1 {
        paletteTransitionStartTime = nil
        paletteTransitionSourceBackgroundUniforms = nil
        paletteTransitionSourceWaveColor = nil
      }
    }

    if renderMode != .particlesOnly {
      splinePipeline.update(settings: settings, time: splineTime)
      splinePipeline.copyDisplacement(into: &displacementValues)
      displacementValues.withUnsafeBytes { bytes in
        guard let baseAddress = bytes.baseAddress else { return }
        splineTexture.replace(
          region: MTLRegionMake2D(
            0,
            0,
            splinePipeline.textureWidth,
            splinePipeline.textureHeight
          ),
          mipmapLevel: 0,
          withBytes: baseAddress,
          bytesPerRow: MemoryLayout<Float>.stride
            * splinePipeline.textureWidth
        )
      }
    }

    guard let descriptor = view.currentRenderPassDescriptor,
      let drawable = view.currentDrawable,
      let commandBuffer = commandQueue.makeCommandBuffer(),
      let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor)
    else {
      return
    }

    if renderMode == .fullBackground {
      encodeBackground(encoder: encoder)
    }
    if renderMode != .particlesOnly {
      encodeWave(encoder: encoder)
    }
    encodeParticles(encoder: encoder, drawableSize: view.drawableSize)
    encoder.endEncoding()
    commandBuffer.present(drawable)
    commandBuffer.commit()
  }

  private func encodeBackground(encoder: MTLRenderCommandEncoder) {
    var uniforms = backgroundUniforms
    encoder.setRenderPipelineState(backgroundPipeline)
    encoder.setFragmentBytes(
      &uniforms,
      length: MemoryLayout<XMBBackgroundUniforms>.stride,
      index: 0
    )
    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
  }

  private func encodeWave(encoder: MTLRenderCommandEncoder) {
    var uniforms = makeWaveUniforms()

    encoder.setRenderPipelineState(wavePipeline)
    encoder.setCullMode(.none)
    encoder.setVertexBuffer(waveVertexBuffer, offset: 0, index: 0)
    encoder.setVertexBytes(
      &uniforms,
      length: MemoryLayout<XMBWaveUniforms>.stride,
      index: 1
    )
    encoder.setFragmentBytes(
      &uniforms,
      length: MemoryLayout<XMBWaveUniforms>.stride,
      index: 1
    )
    encoder.setVertexTexture(splineTexture, index: 0)
    encoder.drawIndexedPrimitives(
      type: .triangleStrip,
      indexCount: waveIndexCount,
      indexType: .uint16,
      indexBuffer: waveIndexBuffer,
      indexBufferOffset: 0
    )
  }

  private func encodeParticles(
    encoder: MTLRenderCommandEncoder,
    drawableSize: CGSize
  ) {
    guard particleCount > 0, let particleBuffer else { return }
    let rendersParticleOverlay = renderMode == .particlesOnly
    let aspect = Float(drawableSize.width / max(1, drawableSize.height))
    let ratio = max(1, min(aspect, 2)) * 0.375
    var particleColor = waveColor
    particleColor.w =
      rendersParticleOverlay
      ? Float(particleControls.depthVariation)
      : Float(settings.particleDepthVariation)
    var uniforms = XMBParticleUniforms(
      timing: SIMD4(
        Float(particleTime),
        Float(settings.particleFlowSpeed * particleControls.speed),
        ratio,
        Float(
          settings.particleOpacity
            * particleControls.brightness
            * particleControls.opacity
        )
      ),
      sizing: SIMD4(
        Float(settings.particleSizeBase),
        Float(settings.particleSizeVariance),
        rendersParticleOverlay ? Float(1.5 * particleControls.size) : 1,
        rendersParticleOverlay
          ? Float(1 - particleControls.verticalLevel * 2)
          : 0
      ),
      color: particleColor,
      distribution: SIMD4(
        rendersParticleOverlay
          ? Float(particleControls.direction * 2)
          : 0,
        rendersParticleOverlay
          ? Float(particleControls.dispersion * particleControls.verticalSpread)
          : Float(settings.particleVerticalSpread),
        rendersParticleOverlay ? 1 : 0,
        rendersParticleOverlay
          ? Float(particleControls.outerDispersion)
          : Float(settings.particleOuterDispersion)
      ),
      waveFollowing: SIMD4(
        !rendersParticleOverlay && settings.particlesFollowWaveY ? 1 : 0,
        renderMode == .foregroundOnly ? 1 : 0,
        0,
        0
      )
    )
    var waveUniforms = makeWaveUniforms()

    encoder.setRenderPipelineState(particlePipeline)
    encoder.setVertexBuffer(particleBuffer, offset: 0, index: 0)
    encoder.setVertexBytes(
      &uniforms,
      length: MemoryLayout<XMBParticleUniforms>.stride,
      index: 1
    )
    encoder.setVertexBytes(
      &waveUniforms,
      length: MemoryLayout<XMBWaveUniforms>.stride,
      index: 2
    )
    encoder.setVertexTexture(splineTexture, index: 0)
    encoder.setFragmentBytes(
      &uniforms,
      length: MemoryLayout<XMBParticleUniforms>.stride,
      index: 1
    )
    encoder.drawPrimitives(
      type: .point,
      vertexStart: 0,
      vertexCount: particleCount
    )
  }

  private func makeWaveUniforms() -> XMBWaveUniforms {
    XMBWaveUniforms(
      timing: SIMD4(
        Float(splineTime),
        Float(settings.flowSpeed),
        Float(settings.tension),
        Float(settings.damping)
      ),
      geometry: SIMD4(
        Float(settings.length),
        Float(settings.spacing),
        Float(settings.perturbation),
        Float(settings.perturbationScale)
      ),
      shaping: SIMD4(
        Float(settings.timeStep),
        Float(settings.waveCosineAmplitude),
        Float(settings.waveBias),
        Float(settings.waveHeightScale)
      ),
      detail: SIMD4(
        Float(settings.waveSoftClip),
        Float(settings.ffdYAmplitude),
        Float(settings.ffdZAmplitude),
        Float(settings.zDetailScale)
      ),
      ffdScale1: SIMD4(
        Float(settings.ffdScale1X),
        Float(settings.ffdScale1Y),
        Float(settings.ffdScale1Z),
        0
      ),
      ffdScale2: SIMD4(
        Float(settings.ffdScale2X),
        Float(settings.ffdScale2Y),
        Float(settings.ffdScale2Z),
        0
      ),
      ffdOffset: SIMD4(
        Float(settings.ffdOffsetX),
        Float(settings.ffdOffsetY),
        Float(settings.ffdOffsetZ),
        0
      ),
      material: SIMD4(
        Float(settings.opacity),
        Float(settings.brightness),
        Float(settings.fresnelPower),
        Float(settings.fresnelScale)
      ),
      color: waveColor
    )
  }

  private func rebuildParticles(device: MTLDevice, count: Int) {
    var generator = XMBSessionRandomNumberGenerator(
      seed: particleSeed ^ UInt64(count)
    )
    let seeds = (0..<count).map { _ in
      SIMD3<Float>(
        Float.random(in: 0..<1, using: &generator),
        Float.random(in: 0..<1, using: &generator),
        pow(Float.random(in: 0..<1, using: &generator), 8) + 0.1
      )
    }
    particleBuffer = device.makeBuffer(
      bytes: seeds,
      length: MemoryLayout<SIMD3<Float>>.stride * seeds.count,
      options: .storageModeShared
    )
    particleCount = count
  }

  private func makeBackgroundUniforms(
    gradient: PlayStation3XMBMartGradient
  ) -> XMBBackgroundUniforms {
    let radians = Float(gradient.angleDegrees * .pi / 180)
    let direction = SIMD2<Float>(cos(radians), sin(radians))
    let projections: [Float] = [
      0,
      direction.x,
      direction.y,
      direction.x + direction.y,
    ]
    let minimum = projections.min() ?? 0
    let maximum = projections.max() ?? 1
    return XMBBackgroundUniforms(
      startColor: colorComponents(gradient.startColor),
      endColor: colorComponents(gradient.endColor),
      directionRange: SIMD4(
        direction.x,
        direction.y,
        minimum,
        max(0.000_001, maximum - minimum)
      ),
      geometry: SIMD4(
        Float(gradient.offsetX),
        Float(gradient.offsetY),
        Float(max(0.05, gradient.width)),
        Float(min(1, max(-1, gradient.curvature)))
      )
    )
  }

  private func paletteTransitionProgress(at time: TimeInterval) -> Float {
    guard let startTime = paletteTransitionStartTime else { return 1 }
    let linear = min(
      1,
      max(0, (time - startTime) / Self.paletteTransitionDuration)
    )
    let eased = linear * linear * (3 - 2 * linear)
    return Float(eased)
  }

  private func interpolate(
    _ source: XMBBackgroundUniforms,
    _ target: XMBBackgroundUniforms,
    progress: Float
  ) -> XMBBackgroundUniforms {
    XMBBackgroundUniforms(
      startColor: interpolate(source.startColor, target.startColor, progress: progress),
      endColor: interpolate(source.endColor, target.endColor, progress: progress),
      directionRange: target.directionRange,
      geometry: target.geometry
    )
  }

  private func interpolate(
    _ source: SIMD4<Float>,
    _ target: SIMD4<Float>,
    progress: Float
  ) -> SIMD4<Float> {
    source + (target - source) * progress
  }

  private func colorComponents(_ color: Color) -> SIMD4<Float> {
    let uiColor = UIColor(color)
    var red: CGFloat = 0
    var green: CGFloat = 0
    var blue: CGFloat = 0
    var alpha: CGFloat = 0
    guard uiColor.getRed(&red, green: &green, blue: &blue, alpha: &alpha) else {
      return SIMD4(1, 1, 1, 1)
    }
    return SIMD4(Float(red), Float(green), Float(blue), Float(alpha))
  }

  private static func makeWaveGrid(
    resolution: Int
  ) -> (vertices: [SIMD2<Float>], indices: [UInt16]) {
    let strips = resolution - 1
    let verticesPerStrip = resolution * 2
    var vertices: [SIMD2<Float>] = []
    vertices.reserveCapacity(strips * verticesPerStrip)

    for y in 0..<strips {
      for x in 0..<resolution {
        let horizontal = Float(x) / Float(resolution - 1) * 2 - 1
        vertices.append(
          SIMD2(
            horizontal,
            Float(y + 1) / Float(resolution - 1) * 2 - 1
          )
        )
        vertices.append(
          SIMD2(
            horizontal,
            Float(y) / Float(resolution - 1) * 2 - 1
          )
        )
      }
    }

    var indices: [UInt16] = []
    indices.reserveCapacity(strips * (verticesPerStrip + 2) - 2)
    var base = 0
    for strip in 0..<strips {
      if strip > 0 {
        indices.append(UInt16(base - 1))
        indices.append(UInt16(base))
      }
      for index in 0..<verticesPerStrip {
        indices.append(UInt16(base + index))
      }
      base += verticesPerStrip
    }
    return (vertices, indices)
  }
}

private enum PlayStation3XMBMartThemeResolver {
  static func backgroundGradient(
    theme: DynamicBackgroundTheme,
    settings: PlayStation3XMBSettings,
    time: TimeInterval
  ) -> PlayStation3XMBMartGradient {
    if settings.gradientPreset == .theme {
      return PlayStation3XMBMartGradient(
        startColor: themedGradientColor(
          theme.sharedColor(index: 0, time: time),
          theme: theme,
          multiplier: settings.gradientTopMul,
          blueMultiplier: 1.2
        ),
        endColor: themedGradientColor(
          theme.sharedColor(index: 2, time: time),
          theme: theme,
          multiplier: settings.gradientBotMul
        ),
        angleDegrees: 90 + theme.sharedPaletteGradientTilt,
        offsetX: theme.particleSettings.sharedPaletteGradientOffsetX,
        offsetY: theme.particleSettings.sharedPaletteGradientOffsetY,
        width: theme.particleSettings.sharedPaletteGradientWidth,
        curvature: theme.sharedPaletteGradientCurvature
      )
    }

    if settings.gradientPreset == .original {
      let original = Color(
        red: settings.colorR / 255,
        green: settings.colorG / 255,
        blue: settings.colorB / 255
      )
      return PlayStation3XMBMartGradient(
        startColor: scaledColor(
          original,
          multiplier: settings.gradientTopMul,
          blueMultiplier: 1.2
        ),
        endColor: scaledColor(original, multiplier: settings.gradientBotMul),
        angleDegrees: 90 + theme.sharedPaletteGradientTilt,
        offsetX: theme.particleSettings.sharedPaletteGradientOffsetX,
        offsetY: theme.particleSettings.sharedPaletteGradientOffsetY,
        width: theme.particleSettings.sharedPaletteGradientWidth,
        curvature: theme.sharedPaletteGradientCurvature
      )
    }

    let preset = settings.gradientPreset.sourceGradient
    return PlayStation3XMBMartGradient(
      startColor: preset.start.color,
      endColor: preset.end.color,
      angleDegrees: preset.angleDegrees + theme.sharedPaletteGradientTilt,
      offsetX: theme.particleSettings.sharedPaletteGradientOffsetX,
      offsetY: theme.particleSettings.sharedPaletteGradientOffsetY,
      width: theme.particleSettings.sharedPaletteGradientWidth,
      curvature: theme.sharedPaletteGradientCurvature
    )
  }

  private static func themedGradientColor(
    _ color: Color,
    theme: DynamicBackgroundTheme,
    multiplier: Double,
    blueMultiplier: Double = 1
  ) -> Color {
    let brightenedColor = scaledColor(
      color,
      multiplier: max(1, multiplier),
      blueMultiplier: blueMultiplier
    )
    return theme.paletteBackgroundColor(
      brightenedColor,
      darkness: max(0, 1 - multiplier)
    )
  }

  private static func scaledColor(
    _ color: Color,
    multiplier: Double,
    blueMultiplier: Double = 1
  ) -> Color {
    let uiColor = UIColor(color)
    var red: CGFloat = 0
    var green: CGFloat = 0
    var blue: CGFloat = 0
    var alpha: CGFloat = 0

    guard uiColor.getRed(&red, green: &green, blue: &blue, alpha: &alpha) else {
      return color.opacity(DynamicBackgroundMath.clamp(multiplier, to: 0...1))
    }

    return Color(
      red: DynamicBackgroundMath.clamp(Double(red) * multiplier, to: 0...1),
      green: DynamicBackgroundMath.clamp(Double(green) * multiplier, to: 0...1),
      blue: DynamicBackgroundMath.clamp(
        Double(blue) * multiplier * blueMultiplier,
        to: 0...1
      ),
      opacity: Double(alpha)
    )
  }

}
