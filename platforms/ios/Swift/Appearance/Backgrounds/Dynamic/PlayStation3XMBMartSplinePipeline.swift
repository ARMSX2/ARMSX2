// PlayStation3XMBMartSplinePipeline.swift — XMB by Mart spline pipeline
// SPDX-License-Identifier: GPL-3.0+

import Foundation

// MARK: - PlayStation3XMBMartSplinePipeline

// Direct Swift port of spline-reverse.js's synthetic descriptor and texture pipeline.
final class PlayStation3XMBMartSplinePipeline {
  private static let tableEntryCount = 0x169
  private static let loopIterations = 8
  private static let storesPerIteration = 8
  let textureWidth: Int
  let textureHeight: Int
  private static let descriptorByteCount = 0x2200
  private static let normA = [0.39584, -0.0052389996, -0.58664495, 0.189007]
  private static let normB = [-0.003751, -0.57536095, 0.161975, 0.417137]
  private static let registerWords = [0x00, 0x11, 0x22, 0x33]

  private var cachedDescriptorSeed: Int?
  private var descriptor = [UInt8](
    repeating: 0,
    count: PlayStation3XMBMartSplinePipeline.descriptorByteCount
  )
  private var runtimeInput = [Double](repeating: 0, count: 16)
  private var coefficients = [Double](
    repeating: 0,
    count: PlayStation3XMBMartSplinePipeline.tableEntryCount * 16
  )
  private var table = [Double](
    repeating: 0,
    count: PlayStation3XMBMartSplinePipeline.tableEntryCount * 4
  )
  private var kernelPacked = [Double](
    repeating: 0,
    count: PlayStation3XMBMartSplinePipeline.loopIterations
      * PlayStation3XMBMartSplinePipeline.storesPerIteration * 4
  )
  private var previousKernelPacked = [Double](
    repeating: 0,
    count: PlayStation3XMBMartSplinePipeline.loopIterations
      * PlayStation3XMBMartSplinePipeline.storesPerIteration * 4
  )
  private var controlPoints: [Double]
  private var displacement: [Double]

  init(detailScale: Double = 1) {
    let clampedScale = min(1, max(0.25, detailScale))
    textureWidth = max(64, Int((256 * clampedScale).rounded()))
    textureHeight = max(16, Int((64 * clampedScale).rounded()))
    controlPoints = Array(
      repeating: 0,
      count: max(12, Int((28 * clampedScale).rounded()))
    )
    displacement = Array(
      repeating: 0,
      count: textureWidth * textureHeight
    )
  }

  func update(settings: PlayStation3XMBSettings, time: TimeInterval) {
    buildRuntimeInputs(settings: settings)
    buildDescriptor(settings: settings)
    decodeCoefficients(settings: settings, time: time)
    buildSplineTable(settings: settings)
    runKernel(settings: settings, time: time)
    writeDisplacementTexture(settings: settings, time: time)
  }

  func copyDisplacement(into values: inout [Float]) {
    if values.count != displacement.count {
      values = Array(repeating: 0, count: displacement.count)
    }
    for index in displacement.indices {
      values[index] = Float(displacement[index])
    }
  }

  private func buildRuntimeInputs(settings: PlayStation3XMBSettings) {
    runtimeInput[0] = settings.damping
    runtimeInput[1] = settings.length
    runtimeInput[2] = settings.tension
    runtimeInput[3] = settings.spacing * 0.001
    runtimeInput[4] = settings.waveCosineAmplitude
    runtimeInput[5] = settings.waveBias
    runtimeInput[6] = settings.timeStep
    runtimeInput[7] = settings.perturbation
    runtimeInput[8] = settings.perturbationScale
    runtimeInput[9] = settings.flowSpeed
    runtimeInput[10] = settings.ffdScale1X
    runtimeInput[11] = settings.ffdScale2X
    runtimeInput[12] = settings.ffdOffsetY
    runtimeInput[13] = settings.fresnelScale
    runtimeInput[14] = settings.waveHeightScale
    runtimeInput[15] = 1
  }

  private func buildDescriptor(settings: PlayStation3XMBSettings) {
    let seed = Int(settings.reverseSyntheticDescriptorSeed)
    guard seed != cachedDescriptorSeed else { return }
    cachedDescriptorSeed = seed

    for index in descriptor.indices {
      let fraction = Double(index) / Double(descriptor.count)
      let wave =
        sin((fraction * 97) * (0.3 + settings.length)) * 0.5
        + sin((fraction * 211) * (0.15 + settings.tension * 3)) * 0.3
        + sin((fraction * 17) * (0.2 + settings.perturbationScale * 2)) * 0.2
      let noise = hash01(Double(index) * 13.37 + Double(seed) * 0.01) * 2 - 1
      let value = clamp(
        (wave * settings.reverseDescriptorStrength + noise * 0.35 + 1) * 0.5,
        lower: 0,
        upper: 1
      )
      descriptor[index] = UInt8(Int(value * 255))
    }
  }

  private func decodeCoefficients(
    settings: PlayStation3XMBSettings,
    time: TimeInterval
  ) {
    let descriptorStride = 0x130
    let phase =
      time * settings.flowSpeed * settings.timeStep
      * settings.reverseSyntheticDescriptorMotion

    for entry in 0..<Self.tableEntryCount {
      let entryBase = entry * 16
      let descriptorBase = (entry * descriptorStride) % descriptor.count

      for block in 0..<4 {
        for lane in 0..<4 {
          let byteIndex = (descriptorBase + block * 0x10 + lane * 4 + entry % 7) % descriptor.count
          let byteValue = Double(descriptor[byteIndex]) / 255
          let centered = byteValue * 2 - 1
          let harmonic = sin(
            Double(entry) * 0.07
              + Double(block) * 0.91
              + Double(lane) * 1.37
              + phase * 0.23
          )
          coefficients[entryBase + block * 4 + lane] =
            centered * 0.75 + harmonic * 0.25
        }
      }
    }
  }

  private func buildSplineTable(settings: PlayStation3XMBSettings) {
    for entry in 0..<Self.tableEntryCount {
      let coefficientBase = entry * 16
      let tableBase = entry * 4

      for lane in 0..<4 {
        let raw =
          coefficients[coefficientBase + lane] * runtimeInput[0]
          + coefficients[coefficientBase + 4 + lane] * runtimeInput[1]
          + coefficients[coefficientBase + 8 + lane] * runtimeInput[2]
          + coefficients[coefficientBase + 12 + lane] * runtimeInput[3]
        let denominatorMagnitude = max(abs(Self.normB[lane]), 0.05)
        let denominator =
          Self.normB[lane] < 0
          ? -denominatorMagnitude
          : denominatorMagnitude
        let normalized =
          ((raw - Self.normA[lane]) / denominator)
          * settings.reverseNormalizeGain
        table[tableBase + lane] = tanh(normalized)
      }
    }
  }

  private func runKernel(
    settings: PlayStation3XMBSettings,
    time: TimeInterval
  ) {
    for iteration in 0..<Self.loopIterations {
      let phase =
        time * settings.reverseKernelPhaseStep
        + Double(iteration) * 0.37
      var indices = [Double](repeating: 0, count: 4)

      for lane in 0..<4 {
        let word = (Self.registerWords[lane] + iteration * 0x13) & 0xff
        let baseIndex = tableIndex(from: word)
        let offset =
          sin(phase + Double(lane) * 0.77)
          * settings.reverseIndexJitter * Double(Self.tableEntryCount)
        indices[lane] = wrappedIndex(
          Double(baseIndex) + offset,
          length: Self.tableEntryCount
        )
      }

      let value0 = sampleTableVector(at: indices[0])
      let value1 = sampleTableVector(at: indices[1])
      let value2 = sampleTableVector(at: indices[2])
      let value3 = sampleTableVector(at: indices[3])
      let mixA = 0.5 + 0.5 * sin(phase * 0.7)
      let mixB = 0.5 + 0.5 * cos(phase * 0.9)
      let mixed12 = mixVectors(value0, value1, amount: mixA)
      let mixed13 = mixVectors(value1, value2, amount: mixB)
      let mixed14 = mixVectors(value2, value3, amount: mixA)
      let mixed15 = mixVectors(value3, value0, amount: mixB)
      let delta9 = subtractVectors(value1, value0)
      let delta4 = subtractVectors(value2, value1)
      let delta87 = subtractVectors(value3, value2)
      let delta89 = subtractVectors(value0, value3)
      let stores = [
        mixed12, mixed13, mixed14, mixed15,
        delta9, delta4, delta87, delta89,
      ]

      for storeIndex in 0..<stores.count {
        let packedBase = (iteration * Self.storesPerIteration + storeIndex) * 4
        for lane in 0..<4 {
          kernelPacked[packedBase + lane] = stores[storeIndex][lane]
        }
      }
    }

    let temporal = clamp(settings.reverseTemporalSmooth, lower: 0, upper: 0.999)
    for index in kernelPacked.indices {
      let smoothed =
        previousKernelPacked[index] * temporal
        + kernelPacked[index] * (1 - temporal)
      kernelPacked[index] = smoothed
      previousKernelPacked[index] = smoothed
    }
  }

  private func writeDisplacementTexture(
    settings: PlayStation3XMBSettings,
    time: TimeInterval
  ) {
    let controlPointCount = controlPoints.count
    let kernelVectorCount = Self.loopIterations * Self.storesPerIteration
    let flow = time * settings.flowSpeed * settings.timeStep

    for row in 0..<textureHeight {
      let depth = Double(row) / Double(max(1, textureHeight - 1)) * 2 - 1
      let rowBase = row * textureWidth
      let rowPhase = flow * 0.25 + depth * 1.7

      for index in 0..<controlPointCount {
        let x = Double(index) / Double(max(1, controlPointCount - 1))
        let kernelPosition = Double(row) * 0.93 + Double(index) * 0.61 + flow * 0.35
        let lowerVector = positiveModulo(
          Int(floor(kernelPosition)),
          kernelVectorCount
        )
        let upperVector = (lowerVector + 1) % kernelVectorCount
        let vectorBlend = kernelPosition - floor(kernelPosition)
        let lowerBase = lowerVector * 4
        let upperBase = upperVector * 4
        let kernelX = mix(
          kernelPacked[lowerBase],
          kernelPacked[upperBase],
          amount: vectorBlend
        )
        let kernelY = mix(
          kernelPacked[lowerBase + 1],
          kernelPacked[upperBase + 1],
          amount: vectorBlend
        )
        let kernelZ = mix(
          kernelPacked[lowerBase + 2],
          kernelPacked[upperBase + 2],
          amount: vectorBlend
        )
        let kernelW = mix(
          kernelPacked[lowerBase + 3],
          kernelPacked[upperBase + 3],
          amount: vectorBlend
        )

        let reverseCore =
          (kernelX * 0.45
            + kernelY * 0.25
            + kernelZ * 0.2
            + kernelW * 0.1) * settings.reverseKernelGain
          + sin(rowPhase + x * 6.2) * settings.bandAmplitude
          + cos(
            depth * settings.bandSecondaryFrequency
              + x * 4.8
              + flow * 0.09
          ) * settings.bandSecondaryAmplitude
        let legacy =
          sin(x * .pi * 1.3 + depth * 0.8 - flow * settings.travelSpeed1)
          * settings.travelAmplitude1 * settings.tension
          + sin(x * .pi * 2.8 - depth * 1.2 + flow * settings.travelSpeed2)
          * settings.travelAmplitude2
          + settings.perturbation * settings.perturbationScale
          * sin(
            (x * (4 + settings.length * 2) + depth * 4 - flow * 0.6)
              * (settings.spacing * 0.01)
          )
        controlPoints[index] =
          reverseCore * settings.reversePipelineBlend
          + legacy * (1 - settings.reversePipelineBlend)
      }

      for xIndex in 0..<textureWidth {
        displacement[rowBase + xIndex] = evaluateSpline(
          controlPoints: controlPoints,
          position: Double(xIndex) / Double(max(1, textureWidth - 1))
        )
      }
    }
  }

  private func evaluateSpline(
    controlPoints: [Double],
    position: Double
  ) -> Double {
    let segmentCount = controlPoints.count - 3
    guard segmentCount >= 1 else { return 0 }
    let scaled = max(
      0,
      min(position * Double(segmentCount), Double(segmentCount) - 0.000_001)
    )
    let segment = Int(floor(scaled))
    let local = scaled - Double(segment)
    let squared = local * local
    let cubed = squared * local
    let basis = [
      (1 - 3 * local + 3 * squared - cubed) / 6,
      (4 - 6 * squared + 3 * cubed) / 6,
      (1 + 3 * local + 3 * squared - 3 * cubed) / 6,
      cubed / 6,
    ]
    return basis[0] * controlPoints[segment]
      + basis[1] * controlPoints[segment + 1]
      + basis[2] * controlPoints[segment + 2]
      + basis[3] * controlPoints[segment + 3]
  }

  private func tableIndex(from word: Int) -> Int {
    let high = (word >> 4) & 0xff
    let low = word & 0xf
    return (19 * high + low) % Self.tableEntryCount
  }

  private func wrappedIndex(_ value: Double, length: Int) -> Double {
    var result = value.truncatingRemainder(dividingBy: Double(length))
    if result < 0 {
      result += Double(length)
    }
    return result
  }

  private func sampleTableVector(at index: Double) -> [Double] {
    let lower = Int(floor(index)) % Self.tableEntryCount
    let upper = (lower + 1) % Self.tableEntryCount
    let blend = index - floor(index)
    let lowerBase = lower * 4
    let upperBase = upper * 4
    return (0..<4).map { lane in
      mix(
        table[lowerBase + lane],
        table[upperBase + lane],
        amount: blend
      )
    }
  }

  private func mixVectors(
    _ first: [Double],
    _ second: [Double],
    amount: Double
  ) -> [Double] {
    (0..<4).map { lane in
      mix(first[lane], second[lane], amount: amount)
    }
  }

  private func subtractVectors(
    _ first: [Double],
    _ second: [Double]
  ) -> [Double] {
    (0..<4).map { lane in first[lane] - second[lane] }
  }

  private func positiveModulo(_ value: Int, _ divisor: Int) -> Int {
    ((value % divisor) + divisor) % divisor
  }

  private func hash01(_ value: Double) -> Double {
    DynamicBackgroundMath.unit(sin(value) * 43_758.5453123)
  }
  private func mix(_ first: Double, _ second: Double, amount: Double) -> Double {
    first + (second - first) * amount
  }

  private func clamp(_ value: Double, lower: Double, upper: Double) -> Double {
    min(upper, max(lower, value))
  }
}
