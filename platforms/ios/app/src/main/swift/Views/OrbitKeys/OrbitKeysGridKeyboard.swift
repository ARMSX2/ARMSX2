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

  var body: some View {
    GeometryReader { proxy in
      let size = min(proxy.size.width, proxy.size.height)
      let inset = max(17, size * 0.18)
      let isTablet = UIDevice.current.userInterfaceIdiom == .pad
      let baseKeyDiameter = min(
        isTablet ? 110 : 44,
        max(isTablet ? 56 : 29, size * (isTablet ? 0.42 : 0.34))
      )
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

// MARK: - Orbit Field

struct OrbitKeysOrbitalOrbField: View {
  let color: Color
  let accentColor: Color
  let stickX: Float
  let stickY: Float
  let selectedPosition: OrbitKeysGridPosition
  let characterFace: OrbitKeysFaceButton?
  let characterPosition: OrbitKeysGridPosition?
  let characterTrigger: Int
  let cellSide: CGFloat
  var isPaused = false
  var customSelectedAnchor: CGSize? = nil
  var customCharacterAnchor: CGSize? = nil

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var orbs = OrbitKeysOrbParticle.randomSet()
  @State private var anchorTransitions: [OrbitKeysOrbAnchorTransition] = []
  @State private var characterAnchorTransitions: [OrbitKeysOrbAnchorTransition] = []
  @State private var focusPrograms: [OrbitKeysOrbFocusProgram] = []
  @State private var spinPrograms: [OrbitKeysOrbSpinProgram] = []
  @State private var orbitSpeedPrograms: [OrbitKeysOrbOrbitSpeedProgram] = []
  @State private var orbitRadiusPrograms: [OrbitKeysOrbRadiusProgram] = []
  @State private var trailTransitionStart: TimeInterval?
  @State private var trailInitialOpacity: CGFloat = 1
  @State private var trailSampleResetTime: TimeInterval?
  @State private var lastSettledPosition: OrbitKeysGridPosition?

  var body: some View {
    GeometryReader { proxy in
      TimelineView(
        .animation(
          minimumInterval: 1 / 30,
          paused: reduceMotion || isPaused
        )
      ) { timeline in
        let time = reduceMotion
          ? 0
          : timeline.date.timeIntervalSinceReferenceDate

        ZStack {
          Canvas(rendersAsynchronously: true) { context, size in
            let trailCount = reduceMotion || isPaused ? 1 : 7
            let trailOpacity = trailVisibility(at: time)
            let earliestSampleTime = trailEarliestSampleTime(at: time)

            for orb in orbs {
              guard trailCount > 1, trailOpacity > 0.001 else { continue }
              let laserColor = particleColor(orb.colorIndex)
              let points = trailPoints(
                for: orb,
                time: time,
                size: size,
                sampleCount: trailCount,
                earliestSampleTime: earliestSampleTime
              )

              for trailIndex in 0..<(points.count - 1) {
                let fade = (1 - Double(trailIndex) / Double(points.count - 1)) * trailOpacity
                let segment = bezierTrailSegment(
                  points: points,
                  index: trailIndex
                )

                context.stroke(
                  segment,
                  with: .color(laserColor.opacity(fade * 0.16)),
                  style: StrokeStyle(lineWidth: 5, lineCap: .round)
                )
                context.stroke(
                  segment,
                  with: .color(laserColor.opacity(fade * 0.92)),
                  style: StrokeStyle(
                    lineWidth: CGFloat(0.55 + fade * 1.15),
                    lineCap: .round
                  )
                )
              }
            }
          }

          glassOrbLayer(time: time, size: proxy.size)
        }
      }
    }
    .allowsHitTesting(false)
    .onAppear {
      let now = Date.timeIntervalSinceReferenceDate
      anchorTransitions = [
        OrbitKeysOrbAnchorTransition(
          from: targetStickVector,
          to: targetStickVector,
          startTime: now,
          duration: 0
        )
      ]
      characterAnchorTransitions = [
        OrbitKeysOrbAnchorTransition(
          from: .zero,
          to: .zero,
          startTime: now,
          duration: 0
        )
      ]
      orbitSpeedPrograms = [
        OrbitKeysOrbOrbitSpeedProgram(
          startTime: now,
          initialState: OrbitKeysOrbOrbitSpeedState(
            phaseTime: now,
            multiplier: targetOrbitSpeedMultiplier
          ),
          targetMultiplier: targetOrbitSpeedMultiplier,
          duration: 0
        )
      ]
      orbitRadiusPrograms = [
        OrbitKeysOrbRadiusProgram(
          startTime: now,
          initialProgress: targetOrbitRadiusProgress,
          targetProgress: targetOrbitRadiusProgress,
          duration: 0
        )
      ]
      lastSettledPosition = selectedPosition
    }
    .onChange(of: targetStickVector) { _, newValue in
      let now = Date.timeIntervalSinceReferenceDate
      let currentValue = anchorVector(at: now)
      anchorTransitions.append(
        OrbitKeysOrbAnchorTransition(
          from: currentValue,
          to: newValue,
          startTime: now,
          duration: reduceMotion ? 0 : 0.34
        )
      )
      if anchorTransitions.count > 80 {
        anchorTransitions.removeFirst(anchorTransitions.count - 80)
      }
    }
    .onChange(of: selectedPosition) { _, _ in
      updateOrbitFocusForSelection()
    }
    .onChange(of: customSelectedAnchor) { _, _ in
      updateOrbitFocusForSelection()
    }
    .task(id: selectedPosition) {
      guard let lastSettledPosition else {
        self.lastSettledPosition = selectedPosition
        return
      }
      guard lastSettledPosition != selectedPosition else { return }

      do {
        try await Task.sleep(for: .milliseconds(420))
      } catch {
        return
      }
      guard !Task.isCancelled else { return }

      let now = Date.timeIntervalSinceReferenceDate
      if let trailTransitionStart, now - trailTransitionStart < 1.25 {
        self.lastSettledPosition = selectedPosition
        return
      }
      trailInitialOpacity = trailVisibility(at: now)
      trailTransitionStart = now
      trailSampleResetTime = now
      self.lastSettledPosition = selectedPosition
    }
    .onChange(of: characterTrigger) { _, _ in
      guard !reduceMotion,
        customCharacterAnchor != nil || (characterFace != nil && characterPosition != nil)
      else {
        return
      }
      let now = Date.timeIntervalSinceReferenceDate
      let currentSpin = spinState(at: now)
      let currentFocus = burstFocus(at: now)
      spinPrograms.append(
        .burst(startTime: now, initialState: currentSpin)
      )
      trimSpinPrograms()
      focusPrograms.append(
        .burst(startTime: now, initialFocus: currentFocus)
      )
      trimFocusPrograms()
      let currentCharacterAnchor = characterAnchor(at: now)
      characterAnchorTransitions.append(
        OrbitKeysOrbAnchorTransition(
          from: currentCharacterAnchor,
          to: resolvedCharacterAnchor(),
          startTime: now,
          duration: 0.28
        )
      )
      trimCharacterAnchorTransitions()
    }
  }

  @ViewBuilder
  private func glassOrbLayer(time: TimeInterval, size: CGSize) -> some View {
    if #available(iOS 26.0, *) {
      GlassEffectContainer(spacing: 3) {
        orbLayer(time: time, size: size, usesGlass: true)
      }
    } else {
      orbLayer(time: time, size: size, usesGlass: false)
    }
  }

  private func orbLayer(time: TimeInterval, size: CGSize, usesGlass: Bool) -> some View {
    ZStack {
      ForEach(orbs) { orb in
        let offset = orbOffset(
          orb,
          time: time,
          size: size,
          anchor: anchorVector(at: time)
        )
        OrbitKeysGlassOrb(
          color: particleColor(orb.colorIndex),
          diameter: orb.diameter,
          usesGlass: usesGlass
        )
        .offset(offset)
      }
    }
    .frame(width: size.width, height: size.height)
  }

  private func orbOffset(
    _ orb: OrbitKeysOrbParticle,
    time: TimeInterval,
    size: CGSize,
    anchor: CGSize
  ) -> CGSize {
    let characterFocus = burstFocus(at: time)
    let burstPhase = spinState(at: time).phaseTurns * .pi * 2 * orb.direction
    let animationTime = reduceMotion ? 0 : orbitSpeedState(at: time).phaseTime
    let angle = animationTime * orb.speed * orb.direction + orb.phase + burstPhase
    let radiusFocus = CGFloat(orbitRadiusProgress(at: time))
    let normalAnchor = normalAnchorOffset(from: anchor)
    let characterAnchor = characterAnchor(at: time)
    let anchorX = mix(normalAnchor.width, characterAnchor.width, characterFocus)
    let anchorY = mix(normalAnchor.height, characterAnchor.height, characterFocus)
    let normalRadiusX = mix(
      size.width * orb.radiusX,
      cellSide * orb.localRadiusX,
      radiusFocus
    )
    let normalRadiusY = mix(
      size.height * orb.radiusY,
      cellSide * orb.localRadiusY,
      radiusFocus
    )
    let ring = CGFloat(orb.id)
    let burstRadiusX = cellSide * (0.12 + ring * 0.055 + orb.localRadiusX * 0.04)
    let burstRadiusY = cellSide * (0.105 + ring * 0.048 + orb.localRadiusY * 0.04)
    let radiusX = mix(normalRadiusX, burstRadiusX, characterFocus)
    let radiusY = mix(normalRadiusY, burstRadiusY, characterFocus)
    let wobbleX = sin(angle * orb.wobbleFrequency + orb.phase) * orb.wobble
    let wobbleY = cos(angle * (orb.wobbleFrequency + 0.31)) * orb.wobble

    return CGSize(
      width: anchorX + cos(angle) * radiusX + wobbleX,
      height: anchorY + sin(angle) * radiusY + wobbleY
    )
  }

  private func trailPoints(
    for orb: OrbitKeysOrbParticle,
    time: TimeInterval,
    size: CGSize,
    sampleCount: Int,
    earliestSampleTime: TimeInterval?
  ) -> [CGPoint] {
    let maximumLength = cellSide * 0.58
    let maximumSegmentLength = cellSide * 0.2
    var points: [CGPoint] = []
    var accumulatedLength: CGFloat = 0

    for index in 0..<sampleCount {
      let sampleTime = time - Double(index) * 0.028
      if let earliestSampleTime, sampleTime < earliestSampleTime {
        break
      }
      let offset = orbOffset(
        orb,
        time: sampleTime,
        size: size,
        anchor: anchorVector(at: sampleTime)
      )
      let point = CGPoint(
        x: size.width / 2 + offset.width,
        y: size.height / 2 + offset.height
      )

      guard let previousPoint = points.last else {
        points.append(point)
        continue
      }

      let segmentLength = hypot(
        point.x - previousPoint.x,
        point.y - previousPoint.y
      )
      let remainingLength = maximumLength - accumulatedLength
      guard remainingLength > 0 else { break }

      let allowedLength = min(
        segmentLength,
        maximumSegmentLength,
        remainingLength
      )
      if segmentLength > allowedLength, segmentLength > 0 {
        let progress = allowedLength / segmentLength
        points.append(
          CGPoint(
            x: previousPoint.x + (point.x - previousPoint.x) * progress,
            y: previousPoint.y + (point.y - previousPoint.y) * progress
          )
        )
        break
      }

      points.append(point)
      accumulatedLength += segmentLength
    }

    return points
  }

  private func bezierTrailSegment(points: [CGPoint], index: Int) -> Path {
    let segment = OrbitKeysOrbTrailGeometry.segment(
      points: points,
      index: index,
      maximumTangentLength: cellSide * 0.18
    )

    var path = Path()
    path.move(to: segment.start)
    path.addCurve(
      to: segment.end,
      control1: segment.control1,
      control2: segment.control2
    )
    return path
  }

  private func trailVisibility(at time: TimeInterval) -> Double {
    guard
      let trailTransitionStart,
      let trailSampleResetTime,
      time >= trailTransitionStart
    else {
      return 1
    }
    let fadeOutElapsed = time - trailTransitionStart

    if fadeOutElapsed < 0.14 {
      let progress = OrbitKeysCubicBezierMotion.value(
        fadeOutElapsed / 0.14,
        control1: 0.12,
        control2: 0.82
      )
      return Double(trailInitialOpacity) * (1 - progress)
    }

    let fadeInStart = trailSampleResetTime + 0.16
    if time < fadeInStart {
      return 0
    }
    if time < fadeInStart + 0.28 {
      return OrbitKeysCubicBezierMotion.value(
        (time - fadeInStart) / 0.28,
        control1: 0.08,
        control2: 0.88
      )
    }
    return 1
  }

  private func trailEarliestSampleTime(at time: TimeInterval) -> TimeInterval? {
    guard let trailSampleResetTime, time >= trailSampleResetTime else {
      return nil
    }
    return trailSampleResetTime
  }

  private func anchorVector(at time: TimeInterval) -> CGSize {
    OrbitKeysOrbMotionResolver.anchorVector(
      transitions: anchorTransitions,
      fallback: targetStickVector,
      at: time
    )
  }

  private func spinState(at time: TimeInterval) -> OrbitKeysOrbSpinState {
    OrbitKeysOrbSpinResolver.state(programs: spinPrograms, at: time)
  }

  private func trimSpinPrograms() {
    if spinPrograms.count > 32 {
      spinPrograms.removeFirst(spinPrograms.count - 32)
    }
  }

  private func orbitSpeedState(at time: TimeInterval) -> OrbitKeysOrbOrbitSpeedState {
    OrbitKeysOrbOrbitSpeedResolver.state(
      programs: orbitSpeedPrograms,
      fallbackTime: time,
      at: time
    )
  }

  private func trimOrbitSpeedPrograms() {
    if orbitSpeedPrograms.count > 32 {
      orbitSpeedPrograms.removeFirst(orbitSpeedPrograms.count - 32)
    }
  }

  private func orbitRadiusProgress(at time: TimeInterval) -> Double {
    OrbitKeysOrbRadiusResolver.progress(
      programs: orbitRadiusPrograms,
      fallback: targetOrbitRadiusProgress,
      at: time
    )
  }

  private func trimOrbitRadiusPrograms() {
    if orbitRadiusPrograms.count > 32 {
      orbitRadiusPrograms.removeFirst(orbitRadiusPrograms.count - 32)
    }
  }

  private func characterAnchor(at time: TimeInterval) -> CGSize {
    OrbitKeysOrbMotionResolver.anchorVector(
      transitions: characterAnchorTransitions,
      fallback: .zero,
      at: time
    )
  }

  private func trimCharacterAnchorTransitions() {
    if characterAnchorTransitions.count > 32 {
      characterAnchorTransitions.removeFirst(characterAnchorTransitions.count - 32)
    }
  }

  private func burstFocus(at time: TimeInterval) -> CGFloat {
    OrbitKeysOrbFocusResolver.focus(programs: focusPrograms, at: time)
  }

  private func trimFocusPrograms() {
    if focusPrograms.count > 32 {
      focusPrograms.removeFirst(focusPrograms.count - 32)
    }
  }

  private func characterAnchor(
    for face: OrbitKeysFaceButton,
    position: OrbitKeysGridPosition
  ) -> CGSize {
    let boxAnchor = CGSize(
      width: CGFloat(position.column - 1) * (cellSide + 8),
      height: CGFloat(position.row - 1) * (cellSide + 8)
    )
    let distance = cellSide * 0.32

    switch face {
    case .square:
      return CGSize(width: boxAnchor.width - distance, height: boxAnchor.height)
    case .cross:
      return CGSize(width: boxAnchor.width, height: boxAnchor.height + distance)
    case .circle:
      return CGSize(width: boxAnchor.width + distance, height: boxAnchor.height)
    case .triangle:
      return CGSize(width: boxAnchor.width, height: boxAnchor.height - distance)
    }
  }

  private func normalAnchorOffset(from anchor: CGSize) -> CGSize {
    if customSelectedAnchor != nil {
      return anchor
    }

    return CGSize(
      width: anchor.width * (cellSide + 8),
      height: -anchor.height * (cellSide + 8)
    )
  }

  private func resolvedCharacterAnchor() -> CGSize {
    if let customCharacterAnchor {
      return customCharacterAnchor
    }
    guard let characterFace, let characterPosition else {
      return .zero
    }

    return characterAnchor(for: characterFace, position: characterPosition)
  }

  private func updateOrbitFocusForSelection() {
    let now = Date.timeIntervalSinceReferenceDate
    let currentOrbitSpeed = orbitSpeedState(at: now)
    orbitSpeedPrograms.append(
      OrbitKeysOrbOrbitSpeedProgram(
        startTime: now,
        initialState: currentOrbitSpeed,
        targetMultiplier: targetOrbitSpeedMultiplier,
        duration: reduceMotion
          ? 0
          : (isUsingCenterOrbit ? 0.78 : 0.44)
      )
    )
    trimOrbitSpeedPrograms()

    let currentRadiusProgress = orbitRadiusProgress(at: now)
    orbitRadiusPrograms.append(
      OrbitKeysOrbRadiusProgram(
        startTime: now,
        initialProgress: currentRadiusProgress,
        targetProgress: targetOrbitRadiusProgress,
        duration: reduceMotion
          ? 0
          : (isUsingCenterOrbit ? 0.82 : 0.68)
      )
    )
    trimOrbitRadiusPrograms()

    let currentFocus = burstFocus(at: now)
    let currentSpin = spinState(at: now)
    if currentFocus > 0.001 || abs(currentSpin.velocityTurnsPerSecond) > 0.001 {
      spinPrograms.append(
        .deceleration(
          startTime: now,
          initialState: currentSpin,
          duration: 0.78
        )
      )
      trimSpinPrograms()
      focusPrograms.append(
        .deceleration(
          startTime: now,
          initialFocus: currentFocus,
          duration: 0.78
        )
      )
      trimFocusPrograms()
    }
  }

  private var targetStickVector: CGSize {
    if let customSelectedAnchor {
      return customSelectedAnchor
    }

    guard selectedPosition == .center else {
      return CGSize(
        width: CGFloat(selectedPosition.column - 1),
        height: CGFloat(1 - selectedPosition.row)
      )
    }
    return stickVector
  }

  private var targetOrbitSpeedMultiplier: Double {
    isUsingCenterOrbit ? 1 : 2.2
  }

  private var targetOrbitRadiusProgress: Double {
    isUsingCenterOrbit ? 0 : 1
  }

  private var isUsingCenterOrbit: Bool {
    customSelectedAnchor == nil && selectedPosition == .center
  }

  private var stickVector: CGSize {
    let x = CGFloat(stickX)
    let y = CGFloat(stickY)
    let radialMagnitude = min(1, hypot(x, y))
    let largestAxis = max(abs(x), abs(y))
    guard largestAxis > 0.0001 else { return .zero }

    let edgeStretch = radialMagnitude / largestAxis
    let squareX = min(max(x * edgeStretch, -1), 1)
    let squareY = min(max(y * edgeStretch, -1), 1)
    return CGSize(
      width: snappedAxis(squareX),
      height: snappedAxis(squareY)
    )
  }

  private func snappedAxis(_ value: CGFloat) -> CGFloat {
    if abs(value) < 0.12 {
      return 0
    }
    if abs(value) > 0.88 {
      return value < 0 ? -1 : 1
    }
    return value
  }

  private func mix(_ start: CGFloat, _ end: CGFloat, _ progress: CGFloat) -> CGFloat {
    start + (end - start) * progress
  }

  private func particleColor(_ index: Int) -> Color {
    switch index % 4 {
    case 0: color
    case 1: accentColor
    case 2: .cyan
    default: .white
    }
  }

}

struct OrbitKeysOrbTrailBezierSegment {
  let start: CGPoint
  let end: CGPoint
  let control1: CGPoint
  let control2: CGPoint
}

enum OrbitKeysOrbTrailGeometry {
  static func segment(
    points: [CGPoint],
    index: Int,
    maximumTangentLength: CGFloat
  ) -> OrbitKeysOrbTrailBezierSegment {
    let start = points[index]
    let end = points[index + 1]
    let startTangent = tangent(
      points: points,
      index: index,
      maximumLength: maximumTangentLength
    )
    let endTangent = tangent(
      points: points,
      index: index + 1,
      maximumLength: maximumTangentLength
    )

    return OrbitKeysOrbTrailBezierSegment(
      start: start,
      end: end,
      control1: CGPoint(
        x: start.x + startTangent.dx / 3,
        y: start.y + startTangent.dy / 3
      ),
      control2: CGPoint(
        x: end.x - endTangent.dx / 3,
        y: end.y - endTangent.dy / 3
      )
    )
  }

  private static func tangent(
    points: [CGPoint],
    index: Int,
    maximumLength: CGFloat
  ) -> CGVector {
    let point = points[index]
    let previous = points[max(0, index - 1)]
    let following = points[min(points.count - 1, index + 1)]
    let raw = CGVector(
      dx: (following.x - previous.x) * 0.5,
      dy: (following.y - previous.y) * 0.5
    )
    let rawLength = hypot(raw.dx, raw.dy)
    guard rawLength > 0 else { return .zero }

    let adjacentLengths = [
      hypot(point.x - previous.x, point.y - previous.y),
      hypot(following.x - point.x, following.y - point.y),
    ].filter { $0 > 0 }
    guard let shortestAdjacent = adjacentLengths.min() else {
      return .zero
    }

    let allowedLength = min(maximumLength, shortestAdjacent * 1.25)
    let scale = min(1, allowedLength / rawLength)
    return CGVector(dx: raw.dx * scale, dy: raw.dy * scale)
  }
}

struct OrbitKeysOrbAnchorTransition {
  let from: CGSize
  let to: CGSize
  let startTime: TimeInterval
  let duration: TimeInterval

  var endTime: TimeInterval {
    startTime + duration
  }

  func value(at time: TimeInterval) -> CGSize {
    guard duration > 0 else { return to }
    let linearProgress = min(max((time - startTime) / duration, 0), 1)
    let progress = OrbitKeysCubicBezierMotion.value(
      linearProgress,
      control1: 0.08,
      control2: 0.92
    )
    return CGSize(
      width: from.width + (to.width - from.width) * progress,
      height: from.height + (to.height - from.height) * progress
    )
  }
}

struct OrbitKeysOrbSpinState: Equatable {
  let phaseTurns: Double
  let velocityTurnsPerSecond: Double

  static let zero = OrbitKeysOrbSpinState(
    phaseTurns: 0,
    velocityTurnsPerSecond: 0
  )
}

struct OrbitKeysOrbFocusProgram {
  enum Kind {
    case burst
    case deceleration(duration: TimeInterval)
  }

  let startTime: TimeInterval
  let initialFocus: CGFloat
  let kind: Kind

  static func burst(
    startTime: TimeInterval,
    initialFocus: CGFloat
  ) -> OrbitKeysOrbFocusProgram {
    OrbitKeysOrbFocusProgram(
      startTime: startTime,
      initialFocus: initialFocus,
      kind: .burst
    )
  }

  static func deceleration(
    startTime: TimeInterval,
    initialFocus: CGFloat,
    duration: TimeInterval
  ) -> OrbitKeysOrbFocusProgram {
    OrbitKeysOrbFocusProgram(
      startTime: startTime,
      initialFocus: initialFocus,
      kind: .deceleration(duration: duration)
    )
  }

  func focus(at time: TimeInterval) -> CGFloat {
    let elapsed = max(0, time - startTime)

    switch kind {
    case .burst:
      if elapsed < 0.24 {
        let progress = OrbitKeysCubicBezierMotion.value(
          elapsed / 0.24,
          control1: 0.08,
          control2: 0.9
        )
        return initialFocus
          + (1 - initialFocus) * CGFloat(progress)
      }
      if elapsed < 0.74 {
        return 1
      }
      if elapsed < 1.79 {
        return CGFloat(
          1
            - OrbitKeysCubicBezierMotion.value(
              (elapsed - 0.74) / 1.05,
              control1: 0.08,
              control2: 0.86
            )
        )
      }
      return 0

    case .deceleration(let duration):
      guard duration > 0 else { return 0 }
      let progress = min(elapsed / duration, 1)
      return initialFocus
        * CGFloat(
          1
            - OrbitKeysCubicBezierMotion.value(
              progress,
              control1: 0.08,
              control2: 0.9
            )
        )
    }
  }
}

enum OrbitKeysOrbFocusResolver {
  static func focus(
    programs: [OrbitKeysOrbFocusProgram],
    at time: TimeInterval
  ) -> CGFloat {
    for program in programs.reversed() where time >= program.startTime {
      return program.focus(at: time)
    }
    return 0
  }
}

struct OrbitKeysOrbSpinProgram {
  enum Kind {
    case burst
    case deceleration(duration: TimeInterval)
  }

  let startTime: TimeInterval
  let initialState: OrbitKeysOrbSpinState
  let kind: Kind

  static func burst(
    startTime: TimeInterval,
    initialState: OrbitKeysOrbSpinState
  ) -> OrbitKeysOrbSpinProgram {
    OrbitKeysOrbSpinProgram(
      startTime: startTime,
      initialState: initialState,
      kind: .burst
    )
  }

  static func deceleration(
    startTime: TimeInterval,
    initialState: OrbitKeysOrbSpinState,
    duration: TimeInterval
  ) -> OrbitKeysOrbSpinProgram {
    OrbitKeysOrbSpinProgram(
      startTime: startTime,
      initialState: initialState,
      kind: .deceleration(duration: duration)
    )
  }

  func state(at time: TimeInterval) -> OrbitKeysOrbSpinState {
    let elapsed = max(0, time - startTime)

    switch kind {
    case .burst:
      return burstState(elapsed: elapsed)
    case .deceleration(let duration):
      return velocitySegment(
        start: initialState,
        endVelocity: 0,
        duration: duration,
        elapsed: elapsed
      )
    }
  }

  private func burstState(elapsed: TimeInterval) -> OrbitKeysOrbSpinState {
    let accelerationDuration = 0.32
    let holdDuration = 0.42
    let decelerationDuration = 1.05
    let peakVelocity = max(0.82, initialState.velocityTurnsPerSecond)

    let accelerated = velocitySegment(
      start: initialState,
      endVelocity: peakVelocity,
      duration: accelerationDuration,
      elapsed: min(elapsed, accelerationDuration)
    )
    guard elapsed > accelerationDuration else { return accelerated }

    let heldElapsed = min(elapsed - accelerationDuration, holdDuration)
    let held = OrbitKeysOrbSpinState(
      phaseTurns: accelerated.phaseTurns + peakVelocity * heldElapsed,
      velocityTurnsPerSecond: peakVelocity
    )
    guard elapsed > accelerationDuration + holdDuration else { return held }

    return velocitySegment(
      start: held,
      endVelocity: 0,
      duration: decelerationDuration,
      elapsed: min(
        elapsed - accelerationDuration - holdDuration,
        decelerationDuration
      )
    )
  }

  private func velocitySegment(
    start: OrbitKeysOrbSpinState,
    endVelocity: Double,
    duration: TimeInterval,
    elapsed: TimeInterval
  ) -> OrbitKeysOrbSpinState {
    guard duration > 0 else {
      return OrbitKeysOrbSpinState(
        phaseTurns: start.phaseTurns,
        velocityTurnsPerSecond: endVelocity
      )
    }

    let progress = min(max(elapsed / duration, 0), 1)
    let curve = OrbitKeysCubicBezierMotion.value(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let integratedCurve = OrbitKeysCubicBezierMotion.integral(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let velocityDelta = endVelocity - start.velocityTurnsPerSecond

    return OrbitKeysOrbSpinState(
      phaseTurns: start.phaseTurns
        + duration
        * (start.velocityTurnsPerSecond * progress
          + velocityDelta * integratedCurve),
      velocityTurnsPerSecond: start.velocityTurnsPerSecond
        + velocityDelta * curve
    )
  }
}

enum OrbitKeysOrbSpinResolver {
  static func state(
    programs: [OrbitKeysOrbSpinProgram],
    at time: TimeInterval
  ) -> OrbitKeysOrbSpinState {
    for program in programs.reversed() where time >= program.startTime {
      return program.state(at: time)
    }
    return .zero
  }
}

enum OrbitKeysOrbMotionResolver {
  static func anchorVector(
    transitions: [OrbitKeysOrbAnchorTransition],
    fallback: CGSize,
    at time: TimeInterval
  ) -> CGSize {
    guard let firstTransition = transitions.first else {
      return fallback
    }

    for transition in transitions.reversed() where time >= transition.startTime {
      return transition.value(at: time)
    }
    return firstTransition.from
  }
}

struct OrbitKeysOrbOrbitSpeedState: Equatable {
  let phaseTime: TimeInterval
  let multiplier: Double
}

struct OrbitKeysOrbOrbitSpeedProgram {
  let startTime: TimeInterval
  let initialState: OrbitKeysOrbOrbitSpeedState
  let targetMultiplier: Double
  let duration: TimeInterval

  func state(at time: TimeInterval) -> OrbitKeysOrbOrbitSpeedState {
    let elapsed = max(0, time - startTime)
    guard duration > 0 else {
      return OrbitKeysOrbOrbitSpeedState(
        phaseTime: initialState.phaseTime + targetMultiplier * elapsed,
        multiplier: targetMultiplier
      )
    }

    let transitionElapsed = min(elapsed, duration)
    let progress = transitionElapsed / duration
    let curve = OrbitKeysCubicBezierMotion.value(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let integratedCurve = OrbitKeysCubicBezierMotion.integral(
      progress,
      control1: 0.1,
      control2: 0.9
    )
    let multiplierDelta = targetMultiplier - initialState.multiplier
    var phaseTime =
      initialState.phaseTime
      + duration
      * (initialState.multiplier * progress
        + multiplierDelta * integratedCurve)

    if elapsed > duration {
      phaseTime += targetMultiplier * (elapsed - duration)
    }

    return OrbitKeysOrbOrbitSpeedState(
      phaseTime: phaseTime,
      multiplier: initialState.multiplier + multiplierDelta * curve
    )
  }
}

enum OrbitKeysOrbOrbitSpeedResolver {
  static func state(
    programs: [OrbitKeysOrbOrbitSpeedProgram],
    fallbackTime: TimeInterval,
    at time: TimeInterval
  ) -> OrbitKeysOrbOrbitSpeedState {
    for program in programs.reversed() where time >= program.startTime {
      return program.state(at: time)
    }
    return OrbitKeysOrbOrbitSpeedState(
      phaseTime: fallbackTime,
      multiplier: 1
    )
  }
}

struct OrbitKeysOrbRadiusProgram {
  let startTime: TimeInterval
  let initialProgress: Double
  let targetProgress: Double
  let duration: TimeInterval

  func progress(at time: TimeInterval) -> Double {
    guard duration > 0 else { return targetProgress }
    let linearProgress = min(max((time - startTime) / duration, 0), 1)
    let curve = OrbitKeysCubicBezierMotion.value(
      linearProgress,
      control1: 0.06,
      control2: 0.88
    )
    return initialProgress + (targetProgress - initialProgress) * curve
  }
}

enum OrbitKeysOrbRadiusResolver {
  static func progress(
    programs: [OrbitKeysOrbRadiusProgram],
    fallback: Double,
    at time: TimeInterval
  ) -> Double {
    for program in programs.reversed() where time >= program.startTime {
      return program.progress(at: time)
    }
    return programs.first?.initialProgress ?? fallback
  }
}

enum OrbitKeysCubicBezierMotion {
  static func value(
    _ progress: Double,
    control1: Double,
    control2: Double
  ) -> Double {
    let t = min(max(progress, 0), 1)
    let inverse = 1 - t
    return 3 * inverse * inverse * t * control1
      + 3 * inverse * t * t * control2
      + t * t * t
  }

  static func integral(
    _ progress: Double,
    control1: Double,
    control2: Double
  ) -> Double {
    let t = min(max(progress, 0), 1)
    return 1.5 * control1 * t * t
      + (-2 * control1 + control2) * t * t * t
      + (0.75 * control1 - 0.75 * control2 + 0.25)
      * t * t * t * t
  }
}

private struct OrbitKeysGlassOrb: View {
  let color: Color
  let diameter: CGFloat
  let usesGlass: Bool

  private var core: some View {
    Circle()
      .fill(.white.opacity(0.08))
      .frame(width: diameter, height: diameter)
      .overlay {
        Circle()
          .stroke(.white.opacity(0.62), lineWidth: 0.65)
      }
      .overlay(alignment: .topLeading) {
        Circle()
          .fill(.white.opacity(0.38))
          .frame(width: diameter * 0.24, height: diameter * 0.24)
          .padding(diameter * 0.18)
      }
      .shadow(color: color.opacity(0.72), radius: diameter * 0.85)
  }

  @ViewBuilder
  var body: some View {
    if #available(iOS 26.0, *), usesGlass {
      core
        .glassEffect(.regular.tint(color.opacity(0.34)), in: .circle)
        .opacity(0.75)
    } else {
      core
        .background(.ultraThinMaterial, in: Circle())
        .overlay {
          Circle().fill(color.opacity(0.2))
        }
        .opacity(0.75)
    }
  }
}

private struct OrbitKeysOrbParticle: Identifiable {
  let id: Int
  let radiusX: CGFloat
  let radiusY: CGFloat
  let localRadiusX: CGFloat
  let localRadiusY: CGFloat
  let speed: Double
  let direction: Double
  let phase: Double
  let wobble: CGFloat
  let wobbleFrequency: Double
  let diameter: CGFloat
  let colorIndex: Int

  static func randomSet(count: Int = 4) -> [OrbitKeysOrbParticle] {
    (0..<count).map { index in
      OrbitKeysOrbParticle(
        id: index,
        radiusX: .random(in: 0.24...0.44),
        radiusY: .random(in: 0.22...0.43),
        localRadiusX: .random(in: 0.12...0.34),
        localRadiusY: .random(in: 0.1...0.32),
        speed: .random(in: 0.95...1.75),
        direction: Bool.random() ? 1 : -1,
        phase: .random(in: 0...(.pi * 2)),
        wobble: .random(in: 1.5...5),
        wobbleFrequency: .random(in: 1.2...2.4),
        diameter: .random(in: 7...13),
        colorIndex: Int.random(in: 0...3)
      )
    }
  }
}
