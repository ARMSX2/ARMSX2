// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.
// Dynamic Background sources are intentionally excluded.
import SwiftUI

// MARK: - OrbitKeys Grid

struct OrbitKeysGridView: View {
  @ObservedObject var model: OrbitKeysModel
  @Namespace private var glassNamespace
  @Namespace private var dotNamespace
  @Environment(\.uiAccentColour) private var themeAccent

  private let columns = Array(
    repeating: GridItem(.flexible(minimum: 60), spacing: 8),
    count: 3
  )

  /// How far the side keys' circles reach past a grid square `gridSide` points wide.
  static func keyOverhang(gridSide: CGFloat) -> CGFloat {
    let key = OrbitKeysCellView.keyMetrics(size: max(1, (gridSide - 16) / 3))
    return max(0, key.diameter / 2 - key.inset)
  }

  var body: some View {
    GeometryReader { proxy in
      let gridSide = min(proxy.size.width, proxy.size.height)
      let cellSide = max(1, (gridSide - 16) / 3)

      ZStack(alignment: .topLeading) {
        if model.showsOrbs, !model.orbsInForeground {
          orbitalLayer(
            gridSide: gridSide,
            cellSide: cellSide,
            containerSize: proxy.size
          )
        }

        glassGrid
          .frame(width: gridSide, height: gridSide)
          .position(x: proxy.size.width / 2, y: proxy.size.height / 2)
          .zIndex(1)

        if model.showsOrbs, model.orbsInForeground {
          orbitalLayer(
            gridSide: gridSide,
            cellSide: cellSide,
            containerSize: proxy.size
          )
          .zIndex(2)
        }
      }
      .animation(.easeInOut(duration: 0.25), value: model.showsOrbs)
      .animation(.easeInOut(duration: 0.25), value: model.orbsInForeground)
    }
  }

  private func orbitalLayer(
    gridSide: CGFloat,
    cellSide: CGFloat,
    containerSize: CGSize
  ) -> some View {
    OrbitKeysOrbitalOrbField(
      color: themeAccent,
      accentColor: model.highlightedFace?.color ?? themeAccent,
      stickX: model.stickX,
      stickY: model.stickY,
      selectedPosition: model.selectedPosition,
      characterFace: model.lastActivatedFace,
      characterPosition: model.lastActivatedPosition,
      characterTrigger: model.characterSequence,
      cellSide: cellSide,
      isPaused: model.isKeyboardSettingsVisible
    )
    .frame(width: gridSide, height: gridSide)
    .position(x: containerSize.width / 2, y: containerSize.height / 2)
    .transition(.opacity)
  }

  @ViewBuilder
  private var glassGrid: some View {
    if model.usesLegacyTheme {
      if #available(iOS 26.0, *) {
        GlassEffectContainer(spacing: 4) {
          grid(usesLegacyLiquidGlass: true)
        }
      } else {
        grid(usesLegacyLiquidGlass: false)
      }
    } else {
      grid(usesLegacyLiquidGlass: false)
    }
  }

  private func grid(usesLegacyLiquidGlass: Bool) -> some View {
    LazyVGrid(columns: columns, spacing: 8) {
      ForEach(OrbitKeysGridPosition.allCases) { position in
        let isSelected = position == model.selectedPosition
        let content = OrbitKeysCellView(
          position: position,
          mode: model.mode,
          arrangement: model.keyboardArrangement,
          showsFaceIcons: model.showsFaceIcons,
          showsFaceIconsOnAllButtons: model.showsFaceIconsOnAllButtons,
          showsColorsOnAllBoxes: model.showsColorsOnAllBoxes,
          clearGlassLevel: model.clearGlassLevel,
          usesLegacyTheme: model.usesLegacyTheme,
          isSelected: isSelected,
          pressedFace: isSelected ? model.highlightedFace : nil,
          rightStickX: model.rightStickX,
          rightStickY: model.rightStickY,
          dotNamespace: dotNamespace,
          dotTrigger: isSelected
            ? model.selectionSequence &* 10_000 &+ model.feedbackSequence
            : 0,
          onSelect: { model.select(position) },
          onKey: { face in
            model.select(position)
            model.activate(face)
          },
          onSwipe: { face in
            model.activateSwipe(in: position, face: face)
          }
        )
        .aspectRatio(1, contentMode: .fit)

        if model.usesLegacyTheme {
          if #available(iOS 26.0, *), usesLegacyLiquidGlass {
            content
              .glassEffect(
                .regular,
                in: .rect(cornerRadius: 22)
              )
              .glassEffectID(position.id, in: glassNamespace)
              .overlay {
                RoundedRectangle(cornerRadius: 22, style: .continuous)
                  .stroke(.white.opacity(0.08), lineWidth: 0.7)
              }
          } else {
            content
              .background(
                .ultraThinMaterial,
                in: RoundedRectangle(cornerRadius: 22, style: .continuous)
              )
              .overlay {
                RoundedRectangle(cornerRadius: 22, style: .continuous)
                  .stroke(.white.opacity(0.08), lineWidth: 0.7)
              }
          }
        } else {
          content.keyboardGlassStyle(
            clearGlassLevel: model.clearGlassLevel,
            cornerRadius: 22
          )
        }
      }
    }
    .animation(.smooth(duration: 0.3), value: model.mode)
    .animation(.smooth(duration: 0.3), value: model.keyboardArrangement)
  }
}

private struct OrbitKeysCellView: View {
  let position: OrbitKeysGridPosition
  let mode: OrbitKeysKeyboardMode
  let arrangement: OrbitKeysArrangement
  let showsFaceIcons: Bool
  let showsFaceIconsOnAllButtons: Bool
  let showsColorsOnAllBoxes: Bool
  let clearGlassLevel: Double
  let usesLegacyTheme: Bool
  let isSelected: Bool
  let pressedFace: OrbitKeysFaceButton?
  let rightStickX: Float
  let rightStickY: Float
  let dotNamespace: Namespace.ID
  let dotTrigger: Int
  let onSelect: () -> Void
  let onKey: (OrbitKeysFaceButton) -> Void
  let onSwipe: (OrbitKeysFaceButton) -> Void

  @State private var swipeOffset = CGSize.zero
  @State private var isSwiping = false
  @State private var suppressKeyTap = false
  @Environment(\.uiAccentColour) private var themeAccent

  /// A side key's centre inset and circle diameter in a cell `size` points wide.
  static func keyMetrics(size: CGFloat) -> (inset: CGFloat, diameter: CGFloat) {
    let isTablet = usesLargeLayout()
    return (
      max(17, size * 0.18),
      min(isTablet ? 110 : 44, max(isTablet ? 56 : 29, size * (isTablet ? 0.42 : 0.34)))
    )
  }

  var body: some View {
    GeometryReader { proxy in
      let size = min(proxy.size.width, proxy.size.height)
      let (inset, baseKeyDiameter) = Self.keyMetrics(size: size)
      let keyDiameter = position == .center ? baseKeyDiameter * 0.9 : baseKeyDiameter
      let dotLimit = size * 0.27
      let joystickOffset = CGSize(
        width: CGFloat(rightStickX) * dotLimit,
        height: -CGFloat(rightStickY) * dotLimit
      )
      let faceButtonOffset =
        pressedFace.map {
          faceOffset(for: $0, distance: dotLimit)
        } ?? .zero
      let hasAnalogInput = rightStickX != 0 || rightStickY != 0
      let dotOffset =
        isSwiping
        ? swipeOffset
        : (hasAnalogInput ? joystickOffset : faceButtonOffset)

      ZStack {
        if isSelected {
          RoundedRectangle(cornerRadius: 22, style: .continuous)
            .stroke(.white.opacity(0.22), lineWidth: 1)
            .shadow(color: themeAccent.opacity(0.42), radius: 8)
            .padding(1)

        } else {
          RoundedRectangle(cornerRadius: 22, style: .continuous)
            .stroke(.white.opacity(0.08), lineWidth: 0.7)
        }

        directionalKey(.triangle, diameter: keyDiameter)
          .position(x: proxy.size.width / 2, y: inset)
        directionalKey(.square, diameter: keyDiameter)
          .position(x: inset, y: proxy.size.height / 2)
        directionalKey(.circle, diameter: keyDiameter)
          .position(x: proxy.size.width - inset, y: proxy.size.height / 2)
        directionalKey(.cross, diameter: keyDiameter)
          .position(x: proxy.size.width / 2, y: proxy.size.height - inset)

        if isSelected {
          OrbitKeysSelectionDot(trigger: dotTrigger)
            .offset(dotOffset)
            .matchedGeometryEffect(
              id: "selection-dot",
              in: dotNamespace,
              properties: .position
            )
            .animation(
              .interpolatingSpring(mass: 0.62, stiffness: 210, damping: 18),
              value: joystickOffset
            )
            .animation(
              .interpolatingSpring(mass: 0.62, stiffness: 210, damping: 18),
              value: faceButtonOffset
            )
        } else {
          Circle()
            .fill(.white.opacity(0.18))
            .frame(width: 3, height: 3)
        }

      }
      .contentShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
      .onTapGesture(perform: onSelect)
      .simultaneousGesture(
        DragGesture(minimumDistance: 6)
          .onChanged { value in
            onSelect()
            isSwiping = true
            suppressKeyTap = true
            swipeOffset = clampedOffset(value.translation, limit: dotLimit)
          }
          .onEnded { value in
            let x = Float(value.translation.width)
            let y = Float(-value.translation.height)
            if let face = OrbitKeysFaceButton.from(
              characterStickX: x,
              y: y,
              threshold: 14
            ) {
              onSwipe(face)
            }

            withAnimation(.spring(duration: 0.22, bounce: 0.28)) {
              isSwiping = false
              swipeOffset = .zero
            }

            Task { @MainActor in
              try? await Task.sleep(for: .milliseconds(160))
              suppressKeyTap = false
            }
          }
      )
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel("\(position.spokenName) symbol group")
    .accessibilityValue(isSelected ? "Selected" : "Not selected")
  }

  private func directionalKey(_ face: OrbitKeysFaceButton, diameter: CGFloat) -> some View {
    let key = OrbitKeysLayout.key(
      at: position,
      face: face,
      mode: mode,
      arrangement: arrangement
    )
    let isPressed = pressedFace == face
    let showsGlyph =
      showsFaceIcons && (isSelected || showsFaceIconsOnAllButtons)
    let usesFaceColor = isSelected || showsColorsOnAllBoxes
    let keyColor = usesFaceColor ? face.color : Color.white.opacity(0.58)

    return Button {
      guard !suppressKeyTap else { return }
      onKey(face)
    } label: {
      ZStack(alignment: .topTrailing) {
        directionalKeyBackground(
          face: face,
          keyColor: keyColor,
          isPressed: isPressed
        )

        ZStack {
          keyLabel(key, diameter: diameter)
            .foregroundStyle(Color.white.opacity(0.58))
            .opacity(usesFaceColor ? 0 : 1)

          keyLabel(key, diameter: diameter)
            .foregroundStyle(isPressed ? .white : face.color)
            .opacity(usesFaceColor ? 1 : 0)
        }
        .animation(.easeOut(duration: 0.16), value: usesFaceColor)

        OrbitKeysFaceGlyph(face: face, color: keyColor)
          .scaleEffect(showsGlyph ? 0.62 : 0.18)
          .offset(x: 3, y: -3)
          .opacity(showsGlyph ? 0.9 : 0)
          .animation(.spring(duration: 0.28, bounce: 0.34), value: isSelected)
          .animation(.spring(duration: 0.28, bounce: 0.34), value: showsFaceIcons)
          .animation(
            .spring(duration: 0.28, bounce: 0.34),
            value: showsFaceIconsOnAllButtons
          )
          .animation(
            .easeInOut(duration: 0.25),
            value: showsColorsOnAllBoxes
          )
      }
      .frame(width: diameter, height: diameter)
      .shadow(color: isPressed ? face.color.opacity(0.9) : .clear, radius: 10)
      .scaleEffect(isPressed ? 1.17 : 1)
      .zIndex(isPressed ? 1 : 0)
      .animation(.spring(duration: 0.24, bounce: 0.62), value: isPressed)
      .contentTransition(.numericText())
    }
    .buttonStyle(.plain)
    .buttonRepeatBehavior(isSelected ? .enabled : .disabled)
    .accessibilityLabel("\(face.rawValue), \(key.label)")
    .accessibilityHint("Selects \(key.label) in the \(position.spokenName) group")
  }

  @ViewBuilder
  private func directionalKeyBackground(
    face: OrbitKeysFaceButton,
    keyColor: Color,
    isPressed: Bool
  ) -> some View {
    if usesLegacyTheme {
      Circle()
        .fill(
          isPressed
            ? face.color.opacity(0.5)
            : .black.opacity(0.2)
        )
        .overlay {
          Circle()
            .stroke(
              keyColor.opacity(isSelected ? 0.42 : 0.18),
              lineWidth: isPressed ? 1.7 : 0.7
            )
        }
    } else {
      Circle()
        .fill(.clear)
        .keyboardGlassCircleStyle(clearGlassLevel: clearGlassLevel, lineWidth: 0)
        .overlay {
          Circle()
            .stroke(
              circleStrokeColor(keyColor: keyColor, isPressed: isPressed),
              lineWidth: isPressed ? 1.7 : 0.7
            )
        }
    }
  }

  private func circleStrokeColor(keyColor: Color, isPressed: Bool) -> Color {
    let level = min(max(clearGlassLevel, -1), 1)
    let clearLevel = max(level, 0)
    let opaqueLevel = 1 - clearLevel
    let clearOpacity = isPressed ? 0.5 : 0.24
    let opaqueOpacity = isPressed ? 0.42 : (isSelected ? 0.42 : 0.18)
    return keyColor.opacity(
      clearOpacity * clearLevel
        + opaqueOpacity * opaqueLevel
    )
  }

  private func keyLabel(_ key: OrbitKeysKeyboardKey, diameter: CGFloat) -> some View {
    Text(key.label)
      .font(
        .system(
          size: key.isAction ? diameter * 0.23 : diameter * 0.4,
          weight: key.isAction ? .bold : .semibold,
          design: .rounded
        )
      )
      .lineLimit(1)
      .minimumScaleFactor(0.62)
      .padding(key.isAction ? 2 : 0)
      .frame(maxWidth: .infinity, maxHeight: .infinity)
  }

  private func clampedOffset(_ offset: CGSize, limit: CGFloat) -> CGSize {
    let distance = hypot(offset.width, offset.height)
    guard distance > limit, distance > 0 else { return offset }
    let scale = limit / distance
    return CGSize(width: offset.width * scale, height: offset.height * scale)
  }

  private func faceOffset(
    for face: OrbitKeysFaceButton,
    distance: CGFloat
  ) -> CGSize {
    switch face {
    case .square:
      return CGSize(width: -distance, height: 0)
    case .cross:
      return CGSize(width: 0, height: distance)
    case .circle:
      return CGSize(width: distance, height: 0)
    case .triangle:
      return CGSize(width: 0, height: -distance)
    }
  }
}

private struct OrbitKeysSelectionDot: View {
  let trigger: Int

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @Environment(\.uiAccentColour) private var themeAccent
  @State private var isExpanded = false
  @State private var pulseTask: Task<Void, Never>?

  var body: some View {
    Circle()
      .fill(themeAccent)
      .frame(width: 6, height: 6)
      .clipShape(Circle())
      .contentShape(Circle())
      .scaleEffect(isExpanded ? 2.35 : 1)
      .opacity(isExpanded ? 0.82 : 1)
      .shadow(color: themeAccent, radius: isExpanded ? 11 : 7)
      .onAppear {
        pulse(for: trigger)
      }
      .onChange(of: trigger) { _, newValue in
        pulse(for: newValue)
      }
      .onDisappear {
        pulseTask?.cancel()
        pulseTask = nil
      }
  }

  private func pulse(for trigger: Int) {
    guard trigger > 0, !reduceMotion else { return }

    pulseTask?.cancel()
    isExpanded = false
    pulseTask = Task { @MainActor in
      try? await Task.sleep(for: .milliseconds(16))
      guard !Task.isCancelled else { return }
      withAnimation(.spring(duration: 0.16, bounce: 0.28)) {
        isExpanded = true
      }

      try? await Task.sleep(for: .milliseconds(150))
      guard !Task.isCancelled else { return }
      withAnimation(.spring(duration: 0.22, bounce: 0.2)) {
        isExpanded = false
      }
    }
  }
}

