// PaletteGradientField.swift — palette gradient field and editor
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

// MARK: - PaletteGradientField

struct PaletteGradientField: View {
  let colors: [Color]
  let startPoint: UnitPoint
  let endPoint: UnitPoint
  let curvature: Double

  var body: some View {
    if abs(curvature) < 0.001 {
      LinearGradient(
        colors: colors,
        startPoint: startPoint,
        endPoint: endPoint
      )
    } else {
      Canvas(opaque: false, colorMode: .linear) { context, size in
        drawCurvedGradient(context: &context, size: size)
      }
    }
  }

  private func drawCurvedGradient(
    context: inout GraphicsContext,
    size: CGSize
  ) {
    guard size.width > 0, size.height > 0, let firstColor = colors.first else {
      return
    }

    let resolvedColors = colors.map(PaletteGradientColor.init)
    let start = CGPoint(
      x: startPoint.x * size.width,
      y: startPoint.y * size.height
    )
    let end = CGPoint(
      x: endPoint.x * size.width,
      y: endPoint.y * size.height
    )
    let vector = CGVector(dx: end.x - start.x, dy: end.y - start.y)
    let length = max(1, hypot(vector.dx, vector.dy))
    let direction = CGVector(dx: vector.dx / length, dy: vector.dy / length)
    let perpendicular = CGVector(dx: -direction.dy, dy: direction.dx)
    let center = CGPoint(x: (start.x + end.x) * 0.5, y: (start.y + end.y) * 0.5)
    let diagonal = hypot(size.width, size.height)
    let clampedCurvature = CGFloat(min(1, max(-1, curvature)))
    let sampleCount = 28
    let bandCount = 144

    context.fill(
      Path(CGRect(origin: .zero, size: size)),
      with: .color(firstColor)
    )

    func boundary(_ progress: CGFloat) -> [CGPoint] {
      (0...sampleCount).map { sample in
        let crossProgress = CGFloat(sample) / CGFloat(sampleCount)
        let crossDistance = (crossProgress * 2 - 1) * diagonal
        let normalizedCrossDistance = crossDistance / diagonal
        let bend =
          clampedCurvature
          * normalizedCrossDistance * normalizedCrossDistance
          * length * 0.78
        let alongDistance = (progress - 0.5) * length - bend
        return CGPoint(
          x: center.x
            + direction.dx * alongDistance
            + perpendicular.dx * crossDistance,
          y: center.y
            + direction.dy * alongDistance
            + perpendicular.dy * crossDistance
        )
      }
    }

    let lastBoundary = boundary(1)
    var terminalPath = Path()
    if let first = lastBoundary.first {
      terminalPath.move(to: first)
      for point in lastBoundary.dropFirst() {
        terminalPath.addLine(to: point)
      }
      for point in lastBoundary.reversed() {
        terminalPath.addLine(
          to: CGPoint(
            x: point.x + direction.dx * diagonal * 4,
            y: point.y + direction.dy * diagonal * 4
          )
        )
      }
      terminalPath.closeSubpath()
      context.fill(
        terminalPath,
        with: .color(colors.last ?? firstColor)
      )
    }

    for band in 0..<bandCount {
      let lowerProgress = CGFloat(band) / CGFloat(bandCount)
      let upperProgress = CGFloat(band + 1) / CGFloat(bandCount)
      let lowerBoundary = boundary(lowerProgress)
      let upperBoundary = boundary(upperProgress + 0.001)
      guard let first = lowerBoundary.first else { continue }

      var bandPath = Path()
      bandPath.move(to: first)
      for point in lowerBoundary.dropFirst() {
        bandPath.addLine(to: point)
      }
      for point in upperBoundary.reversed() {
        bandPath.addLine(to: point)
      }
      bandPath.closeSubpath()

      let progress = Double(lowerProgress + upperProgress) * 0.5
      context.fill(
        bandPath,
        with: .color(
          PaletteGradientColor.interpolated(
            colors: resolvedColors,
            progress: progress
          ).color
        )
      )
    }
  }
}

private struct PaletteGradientColor {
  let red: CGFloat
  let green: CGFloat
  let blue: CGFloat
  let alpha: CGFloat

  init(_ color: Color) {
    let uiColor = UIColor(color)
    var red: CGFloat = 0
    var green: CGFloat = 0
    var blue: CGFloat = 0
    var alpha: CGFloat = 0
    if uiColor.getRed(&red, green: &green, blue: &blue, alpha: &alpha) {
      self.red = red
      self.green = green
      self.blue = blue
      self.alpha = alpha
    } else {
      self.red = 0
      self.green = 0
      self.blue = 0
      self.alpha = 0
    }
  }

  private init(red: CGFloat, green: CGFloat, blue: CGFloat, alpha: CGFloat) {
    self.red = red
    self.green = green
    self.blue = blue
    self.alpha = alpha
  }

  var color: Color {
    Color(red: red, green: green, blue: blue, opacity: alpha)
  }

  static func interpolated(
    colors: [PaletteGradientColor],
    progress: Double
  ) -> PaletteGradientColor {
    guard let first = colors.first else {
      return PaletteGradientColor(red: 0, green: 0, blue: 0, alpha: 0)
    }
    guard colors.count > 1 else { return first }

    let position = min(1, max(0, progress)) * Double(colors.count - 1)
    let lowerIndex = min(colors.count - 1, Int(floor(position)))
    let upperIndex = min(colors.count - 1, lowerIndex + 1)
    let amount = CGFloat(position - Double(lowerIndex))
    let lower = colors[lowerIndex]
    let upper = colors[upperIndex]
    return PaletteGradientColor(
      red: lower.red + (upper.red - lower.red) * amount,
      green: lower.green + (upper.green - lower.green) * amount,
      blue: lower.blue + (upper.blue - lower.blue) * amount,
      alpha: lower.alpha + (upper.alpha - lower.alpha) * amount
    )
  }
}

struct PaletteGradientEditor: View {
  let title: String
  @Binding var colors: [Color]
  var isEnabled: Binding<Bool>?

  private let minimumStopCount = 2
  private let maximumStopCount = 8

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(spacing: 10) {
        Text(title)
          .font(.subheadline.weight(.semibold))

        Spacer()

        if let isEnabled {
          Button {
            isEnabled.wrappedValue.toggle()
          } label: {
            Image(
              systemName: isEnabled.wrappedValue ? "eye.fill" : "eye.slash"
            )
          }
          .accessibilityLabel(
            isEnabled.wrappedValue ? "Disable \(title)" : "Enable \(title)"
          )
        }

        Button {
          resampleColors(to: colors.count - 1)
        } label: {
          Image(systemName: "minus")
        }
        .disabled(colors.count <= minimumStopCount)
        .accessibilityLabel("Remove \(title) gradient stop")

        Button {
          resampleColors(to: colors.count + 1)
        } label: {
          Image(systemName: "plus")
        }
        .disabled(colors.count >= maximumStopCount)
        .accessibilityLabel("Add \(title) gradient stop")
      }
      .buttonStyle(.borderless)

      GeometryReader { proxy in
        let handleDiameter: CGFloat = 40
        let trackWidth = max(0, proxy.size.width - handleDiameter)

        ZStack {
          RoundedRectangle(cornerRadius: 16, style: .continuous)
            .fill(
              LinearGradient(
                colors: colors,
                startPoint: .leading,
                endPoint: .trailing
              )
            )
            .frame(height: 32)
            .padding(.horizontal, handleDiameter / 2)

          ForEach(colors.indices, id: \.self) { index in
            ColorPicker(
              "\(title) stop \(index + 1)",
              selection: colorBinding(at: index),
              supportsOpacity: false
            )
            .labelsHidden()
            .frame(width: handleDiameter, height: handleDiameter)
            .background(Circle().fill(colors[index]))
            .overlay {
              Circle()
                .stroke(.black.opacity(0.85), lineWidth: 3)
                .allowsHitTesting(false)
            }
            .clipShape(Circle())
            .position(
              x: handleDiameter / 2
                + trackWidth * CGFloat(index) / CGFloat(colors.count - 1),
              y: proxy.size.height / 2
            )
          }
        }
        .opacity(isEnabled?.wrappedValue == false ? 0.35 : 1)
        .allowsHitTesting(isEnabled?.wrappedValue != false)
      }
      .frame(height: 46)
    }
    .padding(12)
    .glassSurface(tint: .white.opacity(0.06), cornerRadius: 14)
  }

  private func colorBinding(at index: Int) -> Binding<Color> {
    Binding(
      get: { colors[index] },
      set: { colors[index] = $0 }
    )
  }

  private func resampleColors(to count: Int) {
    guard minimumStopCount...maximumStopCount ~= count else { return }
    let resolvedColors = colors.map(PaletteGradientColor.init)
    colors = (0..<count).map { index in
      PaletteGradientColor.interpolated(
        colors: resolvedColors,
        progress: Double(index) / Double(count - 1)
      ).color
    }
  }
}
