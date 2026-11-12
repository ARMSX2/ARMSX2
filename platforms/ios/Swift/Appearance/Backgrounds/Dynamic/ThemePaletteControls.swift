// ThemePaletteControls.swift — theme palette controls
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - ThemePaletteControls

struct ThemePaletteControls: View {
  @Binding var target: ThemePaletteTarget
  @Binding var sharedPalette: ThemePalette
  @Binding var sharedCustomColor: SavedPaletteColor?
  @Binding var sharedMultiColor: ThemeMultiColorSelection
  @Binding var ribbonPalette: ThemePalette
  @Binding var ribbonCustomColor: SavedPaletteColor?
  @Binding var ribbonMultiColor: ThemeMultiColorSelection
  @Binding var particleSettings: DynamicParticleSettings
  @Binding var isPlayStation3XMBPresetExplicit: Bool
  @Binding var savedColorsJSON: String
  let dynamicBackground: DynamicBackgroundStyle
  let onSaveAppearance: () -> Void

  private let columns = Array(
    repeating: GridItem(.flexible(), spacing: 10),
    count: 4
  )

  @State private var draftColor = Color(red: 0.95, green: 0.2, blue: 0.82)
  @State private var draftPaletteColors = [
    Color(red: 0.08, green: 0.42, blue: 1),
    Color(red: 0.16, green: 0.88, blue: 0.92),
    Color(red: 0.74, green: 0.2, blue: 0.94),
  ]
  @State private var draftDarkEffectColors = [Color.black, Color.black]
  @State private var isDraftDarkEffectEnabled = true

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      paletteSection
      customColorSection
      customPaletteSection
      multiColorControls
      paletteEffectsSection
    }
  }

  private var paletteSection: some View {
    let usesMultiColor = activeMultiColorSelection.wrappedValue.isEnabled

    return VStack(alignment: .leading, spacing: 10) {
      Label("Dynamic Palettes", systemImage: "sparkles")
        .font(.headline)

      paletteGrid(
        ThemePalette.primaryPalettes,
        usesMultiColor: usesMultiColor
      )

      Label("Plain Palettes", systemImage: "paintpalette.fill")
        .font(.headline)
        .padding(.top, 8)

      paletteGrid(
        ThemePalette.morePalettes,
        usesMultiColor: usesMultiColor
      )
    }
  }

  private func paletteGrid(
    _ palettes: [ThemePalette],
    usesMultiColor: Bool
  ) -> some View {
    LazyVGrid(columns: columns, spacing: 12) {
      ForEach(palettes) { palette in
        Button {
          if usesMultiColor {
            toggleMultiColorPalette(palette)
          } else {
            apply(palette)
          }
        } label: {
          VStack(spacing: 6) {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
              .fill(
                LinearGradient(
                  colors: palettePreviewColors(for: palette),
                  startPoint: palettePreviewGradientPoints.start,
                  endPoint: palettePreviewGradientPoints.end
                )
              )
              .frame(height: 54)
              .overlay {
                paletteSelectionOverlay(
                  for: palette,
                  usesMultiColor: usesMultiColor
                )
              }

            Text(palette.title)
              .font(.caption2.weight(.semibold))
              .foregroundStyle(.white.opacity(0.86))
              .lineLimit(1)
              .minimumScaleFactor(0.7)
          }
        }
        .buttonStyle(.plain)
        .controllerAccessibilityActionTarget(
          id: "palette." + target.id + "." + palette.id, label: palette.title
        ) {
          if usesMultiColor { toggleMultiColorPalette(palette) }
          else { apply(palette) }
        }
        .controllerAccessibilityTargetID("palette." + target.id + "." + palette.id)
      }
    }
  }

  private var multiColorControls: some View {
    let selection = activeMultiColorSelection

    return VStack(alignment: .leading, spacing: 10) {
      Toggle("Multi-Color", isOn: multiColorEnabledBinding)

      Toggle("Animate", isOn: selection.animates)
        .disabled(!selection.wrappedValue.isEnabled)
        .opacity(selection.wrappedValue.isEnabled ? 1 : 0.48)
    }
    .font(.subheadline.weight(.semibold))
    .padding(12)
    .glassSurface(tint: .white.opacity(0.06), cornerRadius: 14)
  }

  @ViewBuilder
  private func paletteSelectionOverlay(
    for palette: ThemePalette,
    usesMultiColor: Bool
  ) -> some View {
    if usesMultiColor {
      if let number = multiColorNumber(for: palette) {
        Text("\(number)")
          .font(.caption.weight(.black))
          .foregroundStyle(.black)
          .frame(width: 24, height: 24)
          .background(Circle().fill(.white.opacity(0.94)))
          .shadow(radius: 4)
      }
    } else if isSelected(palette) {
      Image(systemName: "checkmark.circle.fill")
        .font(.title3)
        .foregroundStyle(.white)
        .shadow(radius: 4)
    }
  }

  private var customColorSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Custom colours")
        .font(.headline)

      HStack(spacing: 12) {
        ColorPicker(
          "Choose a new colour",
          selection: $draftColor,
          supportsOpacity: false
        )

        Button {
          saveDraftColor()
        } label: {
          Label("Save", systemImage: "plus")
            .font(.subheadline.weight(.bold))
            .padding(.horizontal, 14)
            .frame(height: 36)
            .glassSurface(
              tint: draftColor.opacity(0.2),
              interactive: true,
              cornerRadius: 18
            )
        }
        .buttonStyle(.plain)
        .controllerAccessibilityActionTarget(
          id: "palette.save-colour", label: "Save Custom Colour", action: saveDraftColor
        )
      }

      if !savedSolidColors.isEmpty {
        LazyVGrid(columns: columns, spacing: 10) {
          ForEach(savedSolidColors) { savedColor in
            Button {
              apply(savedColor)
            } label: {
              RoundedRectangle(cornerRadius: 11, style: .continuous)
                .fill(
                  LinearGradient(
                    colors: palettePreviewColors(for: savedColor),
                    startPoint: customColorPreviewGradientPoints.start,
                    endPoint: customColorPreviewGradientPoints.end
                  )
                )
                .frame(height: 52)
                .overlay {
                  if isSelected(savedColor) {
                    Image(systemName: "checkmark.circle.fill")
                      .foregroundStyle(.white)
                  }
                }
            }
            .buttonStyle(.plain)
            .contextMenu {
              Button(role: .destructive) {
                delete(savedColor)
              } label: {
                Label("Delete", systemImage: "trash")
              }
            }
            .accessibilityLabel("Saved colour \(savedColor.hex)")
            .controllerAccessibilityActionTarget(
              id: "palette.saved." + savedColor.id, label: "Saved colour \(savedColor.hex)"
            ) { apply(savedColor) }
            .controllerAccessibilityTargetID("palette.saved." + savedColor.id)
            .accessibilityAction(named: "Delete") {
              delete(savedColor)
            }
          }
        }
      }
    }
  }

  private var customPaletteSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Custom palette")
        .font(.headline)

      PaletteGradientEditor(
        title: "Palette gradient",
        colors: $draftPaletteColors
      )

      PaletteGradientEditor(
        title: "Dark effect",
        colors: $draftDarkEffectColors,
        isEnabled: $isDraftDarkEffectEnabled
      )

      HStack {
        Spacer()
        Button {
          saveDraftPalette()
        } label: {
          Label("Save palette", systemImage: "plus")
            .font(.subheadline.weight(.bold))
            .padding(.horizontal, 14)
            .frame(height: 36)
            .glassSurface(
              tint: draftPaletteColors[draftPaletteColors.count / 2].opacity(0.18),
              interactive: true,
              cornerRadius: 18
            )
        }
        .buttonStyle(.plain)
        .controllerAccessibilityActionTarget(
          id: "palette.save-gradient", label: "Save Palette", action: saveDraftPalette
        )
      }

      if !savedCustomPalettes.isEmpty {
        LazyVGrid(columns: columns, spacing: 10) {
          ForEach(savedCustomPalettes) { palette in
            Button {
              apply(palette)
            } label: {
              RoundedRectangle(cornerRadius: 11, style: .continuous)
                .fill(
                  LinearGradient(
                    colors: palettePreviewColors(for: palette),
                    startPoint: palettePreviewGradientPoints.start,
                    endPoint: palettePreviewGradientPoints.end
                  )
                )
                .frame(height: 52)
                .overlay {
                  if isSelected(palette) {
                    Image(systemName: "checkmark.circle.fill")
                      .foregroundStyle(.white)
                      .shadow(radius: 4)
                  }
                }
            }
            .buttonStyle(.plain)
            .contextMenu {
              Button(role: .destructive) {
                delete(palette)
              } label: {
                Label("Delete", systemImage: "trash")
              }
            }
            .accessibilityLabel("Saved custom palette")
            .controllerAccessibilityActionTarget(
              id: "palette.saved." + palette.id, label: "Saved Custom Palette"
            ) { apply(palette) }
            .controllerAccessibilityTargetID("palette.saved." + palette.id)
            .accessibilityAction(named: "Delete") {
              delete(palette)
            }
          }
        }
      }
    }
  }

  // DynamicBackgroundTheme zeroes the dark gradient for two reasons: this section's switch and
  // the eye button on a saved palette, which lives in another section. Both grey the slider out
  // so it never sits at full strength doing nothing.
  private var darkGradientIsInert: Bool {
    particleSettings.disablesDarkPaletteEffects || sharedCustomColor?.usesDarkEffect == false
  }

  private var darkGradientInertReason: String? {
    if particleSettings.disablesDarkPaletteEffects {
      return "Dark effects on palettes are switched off above."
    }
    if sharedCustomColor?.usesDarkEffect == false {
      return "This saved palette has its dark effect turned off, so the strength is ignored."
    }
    return nil
  }

  private var paletteEffectsSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      Toggle(
        "Disable dark effects on palettes",
        isOn: $particleSettings.disablesDarkPaletteEffects
      )
      .font(.subheadline.weight(.semibold))

      particleSlider(
        "Dark gradient strength",
        value: $particleSettings.paletteDarkEffectIntensity,
        range: 0...2,
        step: 0.01,
        formattedValue: "\(Int(particleSettings.paletteDarkEffectIntensity * 100))%",
        resetValue: 1
      )
      .disabled(darkGradientIsInert)
      .opacity(darkGradientIsInert ? 0.45 : 1)
      if let reason = darkGradientInertReason {
        Text(reason)
          .font(.caption)
          .foregroundStyle(.orange)
      }

      if target == .shared {
        particleSlider(
          "Palette tilt gradient",
          value: $particleSettings.sharedPaletteGradientTilt,
          range: -180...180,
          step: 1,
          formattedValue: gradientTiltDescription(
            particleSettings.sharedPaletteGradientTilt
          ),
          resetValue: 0
        )

        particleSlider(
          "Y-Axis gradient",
          value: $particleSettings.sharedPaletteGradientOffsetY,
          range: -1...1,
          step: 0.01,
          formattedValue: gradientOffsetDescription(
            particleSettings.sharedPaletteGradientOffsetY
          ),
          resetValue: 0
        )

        particleSlider(
          "X-Axis gradient",
          value: $particleSettings.sharedPaletteGradientOffsetX,
          range: -1...1,
          step: 0.01,
          formattedValue: gradientOffsetDescription(
            particleSettings.sharedPaletteGradientOffsetX
          ),
          resetValue: 0
        )

        particleSlider(
          "Gradient width",
          value: $particleSettings.sharedPaletteGradientWidth,
          range: 0.25...3,
          step: 0.01,
          formattedValue: String(
            format: "%.2fx",
            particleSettings.sharedPaletteGradientWidth
          ),
          resetValue: 1
        )

        particleSlider(
          "Gradient curvature",
          value: $particleSettings.sharedPaletteGradientCurvature,
          range: -1...1,
          step: 0.01,
          formattedValue: gradientOffsetDescription(
            particleSettings.sharedPaletteGradientCurvature
          ),
          resetValue: 0
        )
      }
    }
    .padding(12)
    .glassSurface(tint: .white.opacity(0.06), cornerRadius: 14)
  }

  private func palettePreviewColors(for palette: ThemePalette) -> [Color] {
    palette.colors
  }

  private func palettePreviewColors(for palette: SavedPaletteColor) -> [Color] {
    palette.gradientColors
  }

  private var activePaletteGradientTilt: Double {
    target == .shared ? particleSettings.sharedPaletteGradientTilt : 0
  }

  private var palettePreviewGradientPoints: (start: UnitPoint, end: UnitPoint) {
    rotatedPreviewGradientPoints(
      from: .topLeading,
      to: .bottomTrailing,
      degrees: activePaletteGradientTilt
    )
  }

  private var customColorPreviewGradientPoints: (start: UnitPoint, end: UnitPoint) {
    rotatedPreviewGradientPoints(
      from: .bottomLeading,
      to: .topTrailing,
      degrees: activePaletteGradientTilt
    )
  }

  private func gradientTiltDescription(_ degrees: Double) -> String {
    String(format: "%+.0f°", degrees)
  }

  private func gradientOffsetDescription(_ value: Double) -> String {
    String(format: "%+.0f%%", value * 100)
  }

  private func rotatedPreviewGradientPoints(
    from startPoint: UnitPoint,
    to endPoint: UnitPoint,
    degrees: Double
  ) -> (start: UnitPoint, end: UnitPoint) {
    let radians = degrees * .pi / 180
    let cosine = CGFloat(cos(radians))
    let sine = CGFloat(sin(radians))
    let appliesSharedGeometry = target == .shared
    let width =
      appliesSharedGeometry
      ? CGFloat(max(0.05, particleSettings.sharedPaletteGradientWidth))
      : 1
    let center = CGPoint(
      x: (startPoint.x + endPoint.x) * 0.5
        + (appliesSharedGeometry
          ? CGFloat(particleSettings.sharedPaletteGradientOffsetX) : 0),
      y: (startPoint.y + endPoint.y) * 0.5
        + (appliesSharedGeometry
          ? CGFloat(particleSettings.sharedPaletteGradientOffsetY) : 0)
    )

    func rotate(_ point: UnitPoint) -> UnitPoint {
      let sourceCenter = CGPoint(
        x: (startPoint.x + endPoint.x) * 0.5,
        y: (startPoint.y + endPoint.y) * 0.5
      )
      let x = (point.x - sourceCenter.x) * width
      let y = (point.y - sourceCenter.y) * width
      return UnitPoint(
        x: center.x + x * cosine - y * sine,
        y: center.y + x * sine + y * cosine
      )
    }

    return (rotate(startPoint), rotate(endPoint))
  }

  private func particleSlider(
    _ title: String,
    value: Binding<Double>,
    range: ClosedRange<Double>,
    step: Double? = nil,
    formattedValue: String,
    resetValue: Double
  ) -> some View {
    NumberRow(
      title,
      value: value,
      in: range,
      format: .opaque(formattedValue),
      step: step,
      default: resetValue,
      settings: SettingsStore.shared
    )
  }

  // Decodes the saved custom colors each time the editor reads AppStorage.
  private var savedColors: [SavedPaletteColor] {
    DynamicBackgroundCoding.decode([SavedPaletteColor].self, from: savedColorsJSON) ?? []
  }

  private var savedSolidColors: [SavedPaletteColor] {
    savedColors.filter { !$0.isGradient }
  }

  private var savedCustomPalettes: [SavedPaletteColor] {
    savedColors.filter(\.isGradient)
  }

  private var activeMultiColorSelection: Binding<ThemeMultiColorSelection> {
    Binding(
      get: {
        switch target {
        case .shared:
          return sharedMultiColor
        case .ribbons:
          return ribbonMultiColor
        case .particles:
          return ThemeMultiColorSelection()
        }
      },
      set: { selection in
        switch target {
        case .shared:
          sharedMultiColor = selection
        case .ribbons:
          ribbonMultiColor = selection
        case .particles:
          break
        }
      }
    )
  }

  private var multiColorEnabledBinding: Binding<Bool> {
    Binding(
      get: {
        activeMultiColorSelection.wrappedValue.isEnabled
      },
      set: { enabled in
        setMultiColorEnabled(enabled)
      }
    )
  }

  private var isMultiColorEnabled: Bool {
    activeMultiColorSelection.wrappedValue.isEnabled
  }

  // Enables or disables multi-color mode for the active color target.
  private func setMultiColorEnabled(_ enabled: Bool) {
    switch target {
    case .shared:
      sharedMultiColor.setEnabled(enabled, fallback: sharedPalette)
      if enabled {
        sharedCustomColor = nil
        synchronizeRibbonPaletteAfterSharedSelection(sharedPalette)
      }
      useSharedThemeForXMBMart()
    case .ribbons:
      ribbonMultiColor.setEnabled(enabled, fallback: ribbonPalette)
      if enabled {
        ribbonCustomColor = nil
      }
      useSharedThemeForXMBMart()
    case .particles:
      break
    }
  }

  // Toggles a built-in palette in the active multi-color selection.
  private func toggleMultiColorPalette(_ palette: ThemePalette) {
    switch target {
    case .shared:
      sharedCustomColor = nil
      sharedMultiColor.toggle(palette, fallback: sharedPalette)
      synchronizeRibbonPaletteAfterSharedSelection(palette)
      useSharedThemeForXMBMart()
    case .ribbons:
      ribbonCustomColor = nil
      ribbonMultiColor.toggle(palette, fallback: ribbonPalette)
      useSharedThemeForXMBMart()
    case .particles:
      break
    }
  }

  // Returns the number displayed on selected multi-color palettes.
  private func multiColorNumber(for palette: ThemePalette) -> Int? {
    switch target {
    case .shared:
      return sharedMultiColor.selectionNumber(for: palette)
    case .ribbons:
      return ribbonMultiColor.selectionNumber(for: palette)
    case .particles:
      return nil
    }
  }

  // Applies a built-in palette to the currently selected target.
  private func apply(_ palette: ThemePalette) {
    switch target {
    case .shared:
      sharedMultiColor.isEnabled = false
      sharedPalette = palette
      sharedCustomColor = nil
      synchronizeRibbonPaletteAfterSharedSelection(palette)
      useSharedThemeForXMBMart()
    case .ribbons:
      ribbonMultiColor.isEnabled = false
      ribbonPalette = palette
      ribbonCustomColor = nil
      useSharedThemeForXMBMart()
    case .particles:
      break
    }
  }

  private func synchronizeRibbonPalette(with palette: ThemePalette) {
    ribbonMultiColor.isEnabled = false
    ribbonPalette = palette
    ribbonCustomColor = nil
  }

  private func synchronizeRibbonPaletteAfterSharedSelection(
    _ palette: ThemePalette
  ) {
    if dynamicBackground == .playStation3XMBByMart {
      synchronizeRibbonPalette(
        with: palette.isMorePalette ? palette.lightRibbonPaletteForMart : .cyan
      )
    } else if palette.isMorePalette {
      synchronizeRibbonPalette(with: palette)
    }
  }

  // Applies a saved custom color to the currently selected target.
  private func apply(_ color: SavedPaletteColor) {
    switch target {
    case .shared:
      sharedMultiColor.isEnabled = false
      sharedCustomColor = color
      if dynamicBackground == .playStation3XMBByMart, color.isGradient {
        synchronizeRibbonPalette(with: .cyan)
      }
      useSharedThemeForXMBMart()
    case .ribbons:
      ribbonMultiColor.isEnabled = false
      ribbonCustomColor = color
      useSharedThemeForXMBMart()
    case .particles:
      break
    }
  }

  private func useSharedThemeForXMBMart() {
    guard !isPlayStation3XMBPresetExplicit else { return }
    particleSettings.playStation3XMB.gradientPreset = .theme
  }

  // Checks whether a built-in palette is active for the selected target.
  private func isSelected(_ palette: ThemePalette) -> Bool {
    guard !isMultiColorEnabled else { return false }

    switch target {
    case .shared:
      return sharedCustomColor == nil && sharedPalette == palette
    case .ribbons:
      return ribbonCustomColor == nil && ribbonPalette == palette
    case .particles:
      return false
    }
  }

  // Checks whether a saved custom color is active for the selected target.
  private func isSelected(_ color: SavedPaletteColor) -> Bool {
    guard !isMultiColorEnabled else { return false }

    switch target {
    case .shared:
      return sharedCustomColor == color
    case .ribbons:
      return ribbonCustomColor == color
    case .particles:
      return false
    }
  }

  // Saves the draft color once and applies it immediately to the active target.
  private func saveDraftColor() {
    guard let color = SavedPaletteColor(color: draftColor) else { return }
    save(color)
    apply(color)
    onSaveAppearance()
  }

  private func saveDraftPalette() {
    guard
      let palette = SavedPaletteColor(
        colors: draftPaletteColors,
        darkEffectColors: isDraftDarkEffectEnabled ? draftDarkEffectColors : nil,
        darkEffectEnabled: isDraftDarkEffectEnabled
      )
    else {
      return
    }
    save(palette)
    apply(palette)
    onSaveAppearance()
  }

  private func save(_ color: SavedPaletteColor) {
    var colors = savedColors
    guard !colors.contains(color) else { return }
    colors.append(color)
    persistSavedColors(colors)
  }

  private func delete(_ color: SavedPaletteColor) {
    let colors = savedColors.filter { $0 != color }
    guard colors.count != savedColors.count else { return }

    if sharedCustomColor == color {
      sharedCustomColor = nil
    }
    if ribbonCustomColor == color {
      ribbonCustomColor = nil
    }

    persistSavedColors(colors)
  }

  private func persistSavedColors(_ colors: [SavedPaletteColor]) {
    guard let json = DynamicBackgroundCoding.encodeJSONString(colors) else { return }
    savedColorsJSON = json
  }
}
