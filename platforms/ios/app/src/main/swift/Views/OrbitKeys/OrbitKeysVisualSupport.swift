// OrbitKeys visual support adapted for the ARMSX2 host application.
// The original project's Dynamic Background types are deliberately omitted.

import SwiftUI

extension OrbitKeysFaceButton {
  var color: Color {
    switch self {
    case .square: .pink
    case .cross: .cyan
    case .circle: .red
    case .triangle: .green
    }
  }
}

extension OrbitKeysKeyboardMode {
  var color: Color {
    switch self {
    case .letters: .cyan
    case .shifted: .pink
    case .numbers: .orange
    }
  }
}

extension View {
  func orbitKeysGlassSurface(
    tint: Color? = nil,
    interactive: Bool = false,
    cornerRadius: CGFloat
  ) -> some View {
    modifier(
      OrbitKeysGlassSurfaceModifier(
        tint: tint,
        interactive: interactive,
        cornerRadius: cornerRadius
      )
    )
  }

  func keyboardGlassStyle(
    clearGlassLevel: Double,
    cornerRadius: CGFloat,
    clearFillOpacity: Double = 0.018,
    clearStrokeOpacity: Double = 0.11,
    opaqueFillOpacity: Double = 0.22,
    opaqueHighlightOpacity: Double = 0.04,
    opaqueStrokeOpacity: Double = 0.16,
    lineWidth: CGFloat = 0.75
  ) -> some View {
    modifier(
      OrbitKeysGlassStyleModifier(
        surface: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous),
        clearGlassLevel: clearGlassLevel,
        clearFillOpacity: clearFillOpacity,
        clearStrokeOpacity: clearStrokeOpacity,
        opaqueFillOpacity: opaqueFillOpacity,
        opaqueHighlightOpacity: opaqueHighlightOpacity,
        opaqueStrokeOpacity: opaqueStrokeOpacity,
        lineWidth: lineWidth
      )
    )
  }

  func keyboardGlassCircleStyle(
    clearGlassLevel: Double,
    clearFillOpacity: Double = 0.018,
    clearStrokeOpacity: Double = 0.11,
    opaqueFillOpacity: Double = 0.22,
    opaqueHighlightOpacity: Double = 0.04,
    opaqueStrokeOpacity: Double = 0.16,
    lineWidth: CGFloat = 0.75
  ) -> some View {
    modifier(
      OrbitKeysGlassStyleModifier(
        surface: Circle(),
        clearGlassLevel: clearGlassLevel,
        clearFillOpacity: clearFillOpacity,
        clearStrokeOpacity: clearStrokeOpacity,
        opaqueFillOpacity: opaqueFillOpacity,
        opaqueHighlightOpacity: opaqueHighlightOpacity,
        opaqueStrokeOpacity: opaqueStrokeOpacity,
        lineWidth: lineWidth
      )
    )
  }

  func keyboardThemeSurface(
    usesLegacyTheme: Bool,
    clearGlassLevel: Double,
    cornerRadius: CGFloat,
    legacyTint: Color? = nil,
    legacyInteractive: Bool = false,
    legacyStrokeColor: Color = .white.opacity(0.08),
    legacyLineWidth: CGFloat = 0.7
  ) -> some View {
    modifier(
      OrbitKeysThemeSurfaceModifier(
        usesLegacyTheme: usesLegacyTheme,
        clearGlassLevel: clearGlassLevel,
        cornerRadius: cornerRadius,
        legacyTint: legacyTint,
        legacyInteractive: legacyInteractive,
        legacyStrokeColor: legacyStrokeColor,
        legacyLineWidth: legacyLineWidth
      )
    )
  }

  func keyboardKeySurface(
    usesLegacyTheme: Bool,
    clearGlassLevel: Double,
    cornerRadius: CGFloat
  ) -> some View {
    modifier(
      OrbitKeysKeySurfaceModifier(
        usesLegacyTheme: usesLegacyTheme,
        clearGlassLevel: clearGlassLevel,
        cornerRadius: cornerRadius
      )
    )
  }
}

private struct OrbitKeysGlassSurfaceModifier: ViewModifier {
  let tint: Color?
  let interactive: Bool
  let cornerRadius: CGFloat

  @ViewBuilder
  func body(content: Content) -> some View {
    if #available(iOS 26.0, *) {
      content
        .glassEffect(
          .regular.tint(tint).interactive(interactive),
          in: .rect(cornerRadius: cornerRadius)
        )
    } else {
      content
        .background(
          .ultraThinMaterial,
          in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        )
    }
  }
}

private struct OrbitKeysGlassStyleModifier<Surface: Shape>: ViewModifier {
  let surface: Surface
  let clearGlassLevel: Double
  let clearFillOpacity: Double
  let clearStrokeOpacity: Double
  let opaqueFillOpacity: Double
  let opaqueHighlightOpacity: Double
  let opaqueStrokeOpacity: Double
  let lineWidth: CGFloat

  func body(content: Content) -> some View {
    let glass = OrbitKeysGlassValues(
      clearGlassLevel: clearGlassLevel,
      clearFillOpacity: clearFillOpacity,
      clearStrokeOpacity: clearStrokeOpacity,
      opaqueFillOpacity: opaqueFillOpacity,
      opaqueHighlightOpacity: opaqueHighlightOpacity,
      opaqueStrokeOpacity: opaqueStrokeOpacity
    )

    content
      .background {
        ZStack {
          surface.fill(.black.opacity(glass.blackOpacity))
          surface.fill(.white.opacity(glass.highlightOpacity))
        }
      }
      .overlay {
        surface.stroke(.white.opacity(glass.strokeOpacity), lineWidth: lineWidth)
      }
      .clipShape(surface)
  }
}

private struct OrbitKeysThemeSurfaceModifier: ViewModifier {
  let usesLegacyTheme: Bool
  let clearGlassLevel: Double
  let cornerRadius: CGFloat
  let legacyTint: Color?
  let legacyInteractive: Bool
  let legacyStrokeColor: Color
  let legacyLineWidth: CGFloat

  @ViewBuilder
  func body(content: Content) -> some View {
    if usesLegacyTheme {
      content
        .orbitKeysGlassSurface(
          tint: legacyTint,
          interactive: legacyInteractive,
          cornerRadius: cornerRadius
        )
        .overlay {
          RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
            .stroke(legacyStrokeColor, lineWidth: legacyLineWidth)
        }
    } else {
      content.keyboardGlassStyle(
        clearGlassLevel: clearGlassLevel,
        cornerRadius: cornerRadius
      )
    }
  }
}

private struct OrbitKeysKeySurfaceModifier: ViewModifier {
  let usesLegacyTheme: Bool
  let clearGlassLevel: Double
  let cornerRadius: CGFloat

  @ViewBuilder
  func body(content: Content) -> some View {
    if usesLegacyTheme {
      content
        .background {
          RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
            .fill(.black.opacity(0.2))
        }
        .clipShape(
          RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        )
    } else {
      content.keyboardGlassStyle(
        clearGlassLevel: clearGlassLevel,
        cornerRadius: cornerRadius
      )
    }
  }
}

private struct OrbitKeysGlassValues {
  let blackOpacity: Double
  let highlightOpacity: Double
  let strokeOpacity: Double

  init(
    clearGlassLevel: Double,
    clearFillOpacity: Double,
    clearStrokeOpacity: Double,
    opaqueFillOpacity: Double,
    opaqueHighlightOpacity: Double,
    opaqueStrokeOpacity: Double
  ) {
    let level = min(max(clearGlassLevel, -1), 1)
    let clearLevel = max(level, 0)
    let extraOpaqueLevel = max(-level, 0)
    let opaqueLevel = 1 - clearLevel

    blackOpacity = opaqueFillOpacity * opaqueLevel
      + opaqueFillOpacity * extraOpaqueLevel
    highlightOpacity = clearFillOpacity * clearLevel
      + opaqueHighlightOpacity * opaqueLevel
    strokeOpacity = clearStrokeOpacity * clearLevel
      + opaqueStrokeOpacity * opaqueLevel
  }
}

struct OrbitKeysTriangleShape: Shape {
  func path(in rect: CGRect) -> Path {
    var path = Path()
    path.move(to: CGPoint(x: rect.midX, y: rect.minY))
    path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
    path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
    path.closeSubpath()
    return path
  }
}

struct OrbitKeysFaceGlyph: View {
  let face: OrbitKeysFaceButton
  var color: Color? = nil

  private var tint: Color {
    color ?? face.color
  }

  var body: some View {
    Group {
      switch face {
      case .square:
        RoundedRectangle(cornerRadius: 2.5, style: .continuous)
          .stroke(tint, lineWidth: 1.7)
      case .cross:
        Image(systemName: "xmark")
          .font(.system(size: 13.5, weight: .black))
          .foregroundStyle(tint)
      case .circle:
        Circle()
          .stroke(tint, lineWidth: 1.7)
      case .triangle:
        OrbitKeysTriangleShape()
          .stroke(tint, lineWidth: 1.7)
      }
    }
    .frame(width: 13, height: 13)
    .accessibilityHidden(true)
  }
}
