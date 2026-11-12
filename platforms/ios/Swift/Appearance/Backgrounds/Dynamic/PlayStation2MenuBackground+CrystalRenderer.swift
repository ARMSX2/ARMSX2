// PlayStation2MenuBackground+CrystalRenderer.swift — PS2 menu crystal renderer
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

// MARK: - PlayStation2MenuCrystalRenderer

extension PlayStation2MenuBackground {
  func drawCrystals(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    time: TimeInterval,
    pass: PlayStation2MenuDepthPass,
    configurations: [PlayStation2MenuCrystalConfiguration]
  ) {
    for (index, configuration) in configurations.enumerated() {
      guard pass.contains(configuration.center.z) else { continue }

      drawCrystal(
        context: &context,
        camera: camera,
        configuration: configuration,
        time: time,
        index: index,
        pass: pass
      )
    }
  }

  func drawCrystal(
    context: inout GraphicsContext,
    camera: PlayStation2MenuCamera,
    configuration: PlayStation2MenuCrystalConfiguration,
    time: TimeInterval,
    index: Int,
    pass: PlayStation2MenuDepthPass
  ) {
    let rotation = PlayStation2MenuRotation(
      x: configuration.phase + time * configuration.rotationSpeed * 0.68,
      y: configuration.phase * 0.73 + time * configuration.rotationSpeed,
      z: configuration.phase * 0.41 + time * configuration.rotationSpeed * 0.38
    )
    let half = PlayStation2MenuVector3(
      x: configuration.side / 2,
      y: configuration.side / 2,
      z: configuration.side / 2
    )
    let projected = cuboidVertices(
      center: configuration.center,
      halfSize: half,
      rotation: rotation,
      camera: camera
    )
    let faces = PlayStation2MenuGeometry.cubeFaces.enumerated().sorted { lhs, rhs in
      averageDepth(lhs.element, vertices: projected)
        < averageDepth(rhs.element, vertices: projected)
    }
    let shared = theme.sharedColor(index: configuration.tintIndex, time: time)
    let accent = theme.ribbonColor(index: configuration.tintIndex + 1, time: time)
    let opacity: Double

    switch pass {
    case .far:
      opacity = 0.1
    case .middle:
      opacity = 0.18
    case .near:
      opacity = 0.27
    }

    for (sortedIndex, faceEntry) in faces.enumerated() {
      let faceIndex = faceEntry.offset
      let face = faceEntry.element
      let points = face.map { projected[$0].point }
      let path = polygonPath(points)
      let bounds = path.boundingRect
      let depth = averageDepth(face, vertices: projected)
      let sheen = 0.45 + max(-0.25, min(0.45, depth))

      context.fill(
        path,
        with: .linearGradient(
          Gradient(colors: [
            .white.opacity(opacity * (0.24 + sheen * 0.2)),
            shared.opacity(opacity * (0.42 + sheen * 0.18)),
            accent.opacity(opacity * 0.5),
            .black.opacity(opacity * 0.28),
          ]),
          startPoint: CGPoint(x: bounds.minX, y: bounds.minY),
          endPoint: CGPoint(x: bounds.maxX, y: bounds.maxY)
        )
      )

      let reflectiveFaceCount = pass == .far ? 2 : 3
      if sortedIndex >= faces.count - reflectiveFaceCount {
        let reflectionIdentity =
          index * PlayStation2MenuGeometry.cubeFaces.count + faceIndex
        drawCrystalCloudReflection(
          context: &context,
          clippingPath: path,
          bounds: bounds,
          time: time,
          index: reflectionIdentity,
          opacity: opacity
        )
        drawCrystalOrbReflections(
          context: &context,
          clippingPath: path,
          bounds: bounds,
          time: time,
          index: reflectionIdentity,
          reflectionCount: pass == .near ? 2 : 1,
          opacity: opacity
        )
      }

      if let first = points.first, points.count > 2 {
        var facet = Path()
        facet.move(to: first)
        facet.addLine(to: averagedPoint(points))
        facet.addLine(to: points[2])
        facet.closeSubpath()
        context.fill(
          facet,
          with: .color(.white.opacity(opacity * 0.055))
        )
      }

      context.stroke(
        path,
        with: .color(.white.opacity(opacity * 0.7)),
        lineWidth: pass == .near ? 1 : 0.65
      )
    }

    context.drawLayer { facets in
      facets.blendMode = .plusLighter
      for edge in PlayStation2MenuGeometry.crystalCrossEdges {
        var line = Path()
        line.move(to: projected[edge.0].point)
        line.addLine(to: projected[edge.1].point)
        facets.stroke(
          line,
          with: .color(accent.opacity(opacity * 0.18)),
          lineWidth: 0.55
        )
      }
    }
  }

  func drawCrystalCloudReflection(
    context: inout GraphicsContext,
    clippingPath: Path,
    bounds: CGRect,
    time: TimeInterval,
    index: Int,
    opacity: Double
  ) {
    guard bounds.width > 5, bounds.height > 5 else { return }

    let phase = time * (0.018 + Double(index % 4) * 0.004) + Double(index) * 1.37
    let color = theme.sharedColor(index: index + 1, time: time)
    let center = CGPoint(
      x: bounds.midX + CGFloat(sin(phase)) * bounds.width * 0.31,
      y: bounds.midY + CGFloat(cos(phase * 0.83)) * bounds.height * 0.24
    )
    let rect = CGRect(
      x: center.x - bounds.width * 0.52,
      y: center.y - bounds.height * 0.28,
      width: bounds.width * 1.04,
      height: bounds.height * 0.56
    )

    context.drawLayer { reflection in
      reflection.clip(to: clippingPath)
      reflection.blendMode = .plusLighter
      reflection.addFilter(.blur(radius: max(7, min(28, bounds.width * 0.12))))
      reflection.fill(
        Path(ellipseIn: rect),
        with: .color(color.opacity(opacity * 0.72))
      )
    }
  }

  func drawCrystalOrbReflections(
    context: inout GraphicsContext,
    clippingPath: Path,
    bounds: CGRect,
    time: TimeInterval,
    index: Int,
    reflectionCount: Int,
    opacity: Double
  ) {
    guard bounds.width > 8, bounds.height > 8 else { return }

    for reflectionIndex in 0..<reflectionCount {
      let orbIndex = (index + reflectionIndex * 2) % 4
      let color = theme.ribbonColor(index: orbIndex + 2, time: time)
      let moment = time * (0.055 + Double(orbIndex) * 0.006) + Double(index) * 0.71
      let yOffset = CGFloat(sin(moment + Double(reflectionIndex))) * bounds.height * 0.22
      let start = CGPoint(
        x: bounds.minX - bounds.width * 0.14,
        y: bounds.midY + yOffset
      )
      let control = CGPoint(
        x: bounds.midX + CGFloat(cos(moment * 0.77)) * bounds.width * 0.18,
        y: bounds.midY - CGFloat(cos(moment)) * bounds.height * 0.42
      )
      let end = CGPoint(
        x: bounds.maxX + bounds.width * 0.14,
        y: bounds.midY - yOffset * 0.7
      )
      var reflectionPath = Path()
      reflectionPath.move(to: start)
      reflectionPath.addQuadCurve(to: end, control: control)

      context.drawLayer { glow in
        glow.clip(to: clippingPath)
        glow.blendMode = .plusLighter
        glow.addFilter(.blur(radius: 3.5))
        glow.stroke(
          reflectionPath,
          with: .color(color.opacity(opacity * 0.95)),
          style: StrokeStyle(lineWidth: 2.2, lineCap: .round)
        )
      }

      context.drawLayer { reflection in
        reflection.clip(to: clippingPath)
        reflection.blendMode = .plusLighter
        reflection.stroke(
          reflectionPath,
          with: .color(color.opacity(opacity * 1.25)),
          style: StrokeStyle(lineWidth: 0.75, lineCap: .round)
        )

        let travelPhase =
          time * (0.1 + Double(orbIndex) * 0.008)
          + Double(index) * 0.43
          + Double(reflectionIndex) * 1.4
        let travel = 0.5 + sin(travelPhase) * 0.5
        let orbPoint = quadraticPoint(
          from: start,
          control: control,
          to: end,
          progress: CGFloat(travel)
        )
        let diameter = max(2.2, min(5.5, bounds.width * 0.045))
        reflection.fill(
          Path(
            ellipseIn: CGRect(
              x: orbPoint.x - diameter / 2,
              y: orbPoint.y - diameter / 2,
              width: diameter,
              height: diameter
            )
          ),
          with: .color(.white.opacity(opacity * 2.1))
        )
      }
    }
  }
}
