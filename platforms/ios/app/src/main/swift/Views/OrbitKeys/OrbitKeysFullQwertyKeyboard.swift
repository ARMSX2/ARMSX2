// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.
// Dynamic Background sources are intentionally excluded.
import SwiftUI
import UIKit

enum OrbitKeysFullQwertyDirection: Sendable {
  case left
  case right
  case up
  case down
}

enum OrbitKeysFullQwertyToolbarControl: Int, CaseIterable, Sendable {
  case send
  case settings
  case close
}

struct OrbitKeysFullQwertyKeyPosition: Equatable, Sendable {
  var row: Int
  var column: Int

  static let home = OrbitKeysFullQwertyKeyPosition(row: 1, column: 4)
}

enum OrbitKeysFullQwertyKeyAction: Equatable, Sendable {
  case character(String)
  case shift
  case numbers
  case space
  case backspace
  case returnKey
}

struct OrbitKeysFullQwertyKey: Identifiable, Equatable, Sendable {
  let id: String
  let label: String
  let action: OrbitKeysFullQwertyKeyAction
  let width: Double
  let systemImage: String?

  var repeatsWhileHeld: Bool {
    switch action {
    case .character, .space, .backspace:
      return true
    case .shift, .numbers, .returnKey:
      return false
    }
  }

  var isModeKey: Bool {
    switch action {
    case .shift, .numbers:
      return true
    case .character, .space, .backspace, .returnKey:
      return false
    }
  }

  var usesAccentColor: Bool {
    switch action {
    case .shift, .numbers, .space, .backspace, .returnKey:
      return true
    case .character:
      return false
    }
  }
}

enum OrbitKeysFullQwertyLayout {
  static func rows(for mode: OrbitKeysKeyboardMode) -> [[OrbitKeysFullQwertyKey]] {
    switch mode {
    case .letters:
      return letterRows(isShifted: false)
    case .shifted:
      return letterRows(isShifted: true)
    case .numbers:
      return numberRows
    }
  }

  static func key(
    at position: OrbitKeysFullQwertyKeyPosition,
    mode: OrbitKeysKeyboardMode
  ) -> OrbitKeysFullQwertyKey? {
    let rows = rows(for: mode)
    guard rows.indices.contains(position.row),
      rows[position.row].indices.contains(position.column)
    else {
      return nil
    }

    return rows[position.row][position.column]
  }

  static func clampedPosition(
    _ position: OrbitKeysFullQwertyKeyPosition,
    mode: OrbitKeysKeyboardMode
  ) -> OrbitKeysFullQwertyKeyPosition {
    let rows = rows(for: mode)
    guard !rows.isEmpty else { return .home }

    let row = min(max(position.row, 0), rows.count - 1)
    let column = min(max(position.column, 0), rows[row].count - 1)
    return OrbitKeysFullQwertyKeyPosition(row: row, column: column)
  }

  static func movedPosition(
    from position: OrbitKeysFullQwertyKeyPosition,
    direction: OrbitKeysFullQwertyDirection,
    mode: OrbitKeysKeyboardMode,
    loopsHorizontally: Bool = false
  ) -> OrbitKeysFullQwertyKeyPosition {
    let rows = rows(for: mode)
    guard !rows.isEmpty else { return .home }

    let current = clampedPosition(position, mode: mode)
    switch direction {
    case .left:
      let nextColumn =
        loopsHorizontally && current.column == 0
        ? rows[current.row].count - 1
        : max(current.column - 1, 0)
      return OrbitKeysFullQwertyKeyPosition(
        row: current.row,
        column: nextColumn
      )
    case .right:
      let lastColumn = rows[current.row].count - 1
      let nextColumn =
        loopsHorizontally && current.column == lastColumn
        ? 0
        : min(current.column + 1, lastColumn)
      return OrbitKeysFullQwertyKeyPosition(
        row: current.row,
        column: nextColumn
      )
    case .up, .down:
      let nextRow =
        direction == .up
        ? max(current.row - 1, 0)
        : min(current.row + 1, rows.count - 1)
      let currentCenter = normalizedCenter(
        of: current.column,
        in: rows[current.row]
      )
      let nextColumn = nearestColumn(to: currentCenter, in: rows[nextRow])
      return OrbitKeysFullQwertyKeyPosition(row: nextRow, column: nextColumn)
    }
  }

  private static func letterRows(isShifted: Bool) -> [[OrbitKeysFullQwertyKey]] {
    [
      characterRow(["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"], isShifted: isShifted),
      characterRow(["a", "s", "d", "f", "g", "h", "j", "k", "l"], isShifted: isShifted),
      [
        key("shift", isShifted ? "ABC" : "Shift", .shift, width: 1.55, image: "shift.fill")
      ] + characterRow(["z", "x", "c", "v", "b", "n", "m"], isShifted: isShifted) + [
        key("delete", "Del", .backspace, width: 1.55, image: "delete.left.fill")
      ],
      [
        key("numbers", "123", .numbers, width: 1.45),
        key("comma", ",", .character(","), width: 0.9),
        key("space", "space", .space, width: 4.45),
        key("period", ".", .character("."), width: 0.9),
        key("return", "Return", .returnKey, width: 1.75, image: "return"),
      ],
    ]
  }

  private static let numberRows: [[OrbitKeysFullQwertyKey]] = [
    characterRow(["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"], isShifted: false),
    characterRow(["-", "/", ":", ";", "(", ")", "$", "&", "@"], isShifted: false),
    [
      key("shift-number", "Shift", .shift, width: 1.55, image: "shift.fill"),
      key("zero-wide", "0", .character("0")),
      key("quote", "\"", .character("\"")),
      key("apostrophe", "'", .character("'")),
      key("question", "?", .character("?")),
      key("exclamation", "!", .character("!")),
      key("plus", "+", .character("+")),
      key("equals", "=", .character("=")),
      key("delete-number", "Del", .backspace, width: 1.55, image: "delete.left.fill"),
    ],
    [
      key("letters-wide", "ABC", .numbers, width: 1.45),
      key("comma-number", ",", .character(","), width: 0.9),
      key("space-number", "space", .space, width: 4.45),
      key("period-number", ".", .character("."), width: 0.9),
      key("return-number", "Return", .returnKey, width: 1.75, image: "return"),
    ],
  ]

  private static func characterRow(
    _ values: [String],
    isShifted: Bool
  ) -> [OrbitKeysFullQwertyKey] {
    values.map { value in
      let output = isShifted ? value.uppercased() : value
      return key("character-\(output)", output, .character(output))
    }
  }

  private static func key(
    _ id: String,
    _ label: String,
    _ action: OrbitKeysFullQwertyKeyAction,
    width: Double = 1,
    image: String? = nil
  ) -> OrbitKeysFullQwertyKey {
    OrbitKeysFullQwertyKey(
      id: id,
      label: label,
      action: action,
      width: width,
      systemImage: image
    )
  }

  private static func normalizedCenter(
    of column: Int,
    in row: [OrbitKeysFullQwertyKey]
  ) -> Double {
    guard row.indices.contains(column) else { return 0 }

    let totalWidth = row.reduce(0) { $0 + $1.width }
    guard totalWidth > 0 else { return 0 }

    let precedingWidth = row.prefix(column).reduce(0) { $0 + $1.width }
    return (precedingWidth + row[column].width / 2) / totalWidth
  }

  private static func nearestColumn(
    to normalizedCenter: Double,
    in row: [OrbitKeysFullQwertyKey]
  ) -> Int {
    guard !row.isEmpty else { return 0 }

    return row.indices.min { lhs, rhs in
      abs(Self.normalizedCenter(of: lhs, in: row) - normalizedCenter)
        < abs(Self.normalizedCenter(of: rhs, in: row) - normalizedCenter)
    } ?? 0
  }
}

struct OrbitKeysFullQwerty: View {
  @ObservedObject var model: OrbitKeysModel
  let compact: Bool
  @Environment(\.uiAccentColour) private var themeAccent

  private var rows: [[OrbitKeysFullQwertyKey]] {
    OrbitKeysFullQwertyLayout.rows(for: model.mode)
  }

  var body: some View {
    GeometryReader { proxy in
      let metrics = keyboardMetrics(in: proxy.size)

      ZStack {
        if model.showsOrbs, !model.orbsInForeground {
          qwertyOrbLayer(metrics: metrics, size: proxy.size)
            .zIndex(0)
        }

        VStack(spacing: metrics.rowSpacing) {
          ForEach(rows.indices, id: \.self) { rowIndex in
            keyRow(
              rowIndex: rowIndex,
              keys: rows[rowIndex],
              height: metrics.keyHeight,
              spacing: metrics.keySpacing
            )
          }
        }
        .padding(metrics.padding)
        .frame(width: proxy.size.width, height: proxy.size.height, alignment: .center)
        .keyboardThemeSurface(
          usesLegacyTheme: model.usesLegacyTheme,
          clearGlassLevel: model.clearGlassLevel,
          cornerRadius: 20
        )
        .zIndex(1)

        if model.showsOrbs, model.orbsInForeground {
          qwertyOrbLayer(metrics: metrics, size: proxy.size)
            .zIndex(2)
        }
      }
      .frame(width: proxy.size.width, height: proxy.size.height, alignment: .center)
      .contentShape(Rectangle())
      .simultaneousGesture(
        DragGesture(minimumDistance: 24)
          .onEnded { value in
            handleKeyboardSwipe(value.translation)
          }
      )
      .animation(.easeInOut(duration: 0.25), value: model.showsOrbs)
      .animation(.easeInOut(duration: 0.25), value: model.orbsInForeground)
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Full QWERTY keyboard")
  }

  private func handleKeyboardSwipe(_ translation: CGSize) {
    let horizontalDistance = translation.width
    let verticalDistance = abs(translation.height)

    guard abs(horizontalDistance) >= 48,
      abs(horizontalDistance) > verticalDistance * 1.35
    else {
      return
    }

    if horizontalDistance < 0 {
      model.backspace()
    } else {
      model.insertSpace()
    }
  }

  private func keyboardMetrics(in size: CGSize) -> OrbitKeysFullQwertyKeyboardMetrics {
    let isTablet = UIDevice.current.userInterfaceIdiom == .pad
    let padding = isTablet
      ? (compact ? CGFloat(10) : CGFloat(12))
      : (compact ? CGFloat(7) : CGFloat(8))
    let rowSpacing = isTablet
      ? (compact ? CGFloat(7) : CGFloat(8))
      : (compact ? CGFloat(4) : CGFloat(5))
    let keySpacing = isTablet
      ? (compact ? CGFloat(6) : CGFloat(7))
      : (compact ? CGFloat(4) : CGFloat(5))
    let rowCount = max(rows.count, 1)
    let availableHeight =
      size.height
      - padding * 2
      - rowSpacing * CGFloat(max(rowCount - 1, 0))
    let keyHeight = min(
      isTablet
        ? (compact ? CGFloat(78) : CGFloat(84))
        : (compact ? CGFloat(40) : CGFloat(43)),
      max(30, availableHeight / CGFloat(rowCount))
    )
    let contentHeight =
      padding * 2
      + keyHeight * CGFloat(rowCount)
      + rowSpacing * CGFloat(max(rowCount - 1, 0))
    let centeredContentOffsetY = max(0, (size.height - contentHeight) / 2)

    return OrbitKeysFullQwertyKeyboardMetrics(
      padding: padding,
      rowSpacing: rowSpacing,
      keySpacing: keySpacing,
      keyHeight: keyHeight,
      centeredContentOffsetY: centeredContentOffsetY
    )
  }

  private func qwertyOrbLayer(
    metrics: OrbitKeysFullQwertyKeyboardMetrics,
    size: CGSize
  ) -> some View {
    OrbitKeysOrbitalOrbField(
      color: themeAccent,
      accentColor: themeAccent,
      stickX: 0,
      stickY: 0,
      selectedPosition: .center,
      characterFace: nil,
      characterPosition: nil,
      characterTrigger: model.characterSequence,
      cellSide: qwertyOrbCellSide(metrics: metrics, size: size),
      isPaused: model.isKeyboardSettingsVisible,
      customSelectedAnchor: nil,
      customCharacterAnchor: model.lastActivatedFullQwertyPosition.flatMap {
        keyAnchor(for: $0, metrics: metrics, size: size)
      }
    )
    .frame(width: size.width, height: size.height)
    .transition(.opacity)
  }

  private func qwertyOrbCellSide(
    metrics: OrbitKeysFullQwertyKeyboardMetrics,
    size: CGSize
  ) -> CGFloat {
    min(
      max(metrics.keyHeight * 4.2, 132),
      min(size.width, size.height) * 0.9
    )
  }

  private func keyAnchor(
    for position: OrbitKeysFullQwertyKeyPosition,
    metrics: OrbitKeysFullQwertyKeyboardMetrics,
    size: CGSize
  ) -> CGSize? {
    guard rows.indices.contains(position.row),
      rows[position.row].indices.contains(position.column)
    else {
      return nil
    }

    let row = rows[position.row]
    let totalUnits = row.reduce(0) { $0 + $1.width }
    guard totalUnits > 0 else { return nil }

    let totalSpacing = CGFloat(max(row.count - 1, 0)) * metrics.keySpacing
    let availableWidth = max(1, size.width - metrics.padding * 2 - totalSpacing)
    let precedingUnits = row.prefix(position.column).reduce(0) { $0 + $1.width }
    let keyWidth = availableWidth * CGFloat(row[position.column].width / totalUnits)
    let x =
      metrics.padding
      + availableWidth * CGFloat(precedingUnits / totalUnits)
      + metrics.keySpacing * CGFloat(position.column)
      + keyWidth / 2
    let y =
      metrics.centeredContentOffsetY
      + metrics.padding
      + CGFloat(position.row) * (metrics.keyHeight + metrics.rowSpacing)
      + metrics.keyHeight / 2

    return CGSize(width: x - size.width / 2, height: y - size.height / 2)
  }

  private func keyRow(
    rowIndex: Int,
    keys: [OrbitKeysFullQwertyKey],
    height: CGFloat,
    spacing: CGFloat
  ) -> some View {
    GeometryReader { proxy in
      let totalUnits = keys.reduce(0) { $0 + $1.width }
      let totalSpacing = CGFloat(max(keys.count - 1, 0)) * spacing
      let availableWidth = max(1, proxy.size.width - totalSpacing)

      HStack(spacing: spacing) {
        ForEach(keys.indices, id: \.self) { column in
          let key = keys[column]
          let position = OrbitKeysFullQwertyKeyPosition(row: rowIndex, column: column)
          let isSelected = model.fullQwertyToolbarSelection == nil
            && model.fullQwertySelection == position
          let keyWidth = availableWidth * CGFloat(key.width / totalUnits)

          OrbitKeysFullQwertyKeyButton(
            key: key,
            accent: themeAccent,
            isSelected: isSelected,
            feedbackTrigger: isSelected ? model.fullQwertyFeedbackSequence : 0,
            compact: compact,
            clearGlassLevel: model.clearGlassLevel,
            usesLegacyTheme: model.usesLegacyTheme,
            onHoldChanged: { isHeld in
              guard key.action == .backspace else { return }
              if isHeld {
                model.selectFullQwertyKey(position)
              }
              model.setFullQwertyTouchBackspaceHeld(isHeld)
            }
          ) {
            model.activateFullQwertyKey(at: position)
          }
          .frame(width: keyWidth, height: height)
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    .frame(height: height)
  }
}

private struct OrbitKeysFullQwertyKeyboardMetrics {
  let padding: CGFloat
  let rowSpacing: CGFloat
  let keySpacing: CGFloat
  let keyHeight: CGFloat
  let centeredContentOffsetY: CGFloat
}

private struct OrbitKeysFullQwertyKeyButton: View {
  let key: OrbitKeysFullQwertyKey
  let accent: Color
  let isSelected: Bool
  let feedbackTrigger: Int
  let compact: Bool
  let clearGlassLevel: Double
  let usesLegacyTheme: Bool
  let onHoldChanged: (Bool) -> Void
  let action: () -> Void

  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var activationTask: Task<Void, Never>?
  @State private var activationScale = false
  @State private var isTouchHeld = false

  private var usesTabletMetrics: Bool {
    UIDevice.current.userInterfaceIdiom == .pad
  }

  @ViewBuilder
  var body: some View {
    Group {
      if key.action == .backspace {
        keySurface
          .contentShape(RoundedRectangle(cornerRadius: 11, style: .continuous))
          .gesture(
            DragGesture(minimumDistance: 0)
              .onChanged { _ in
                guard !isTouchHeld else { return }
                isTouchHeld = true
                onHoldChanged(true)
                pulse()
              }
              .onEnded { _ in
                guard isTouchHeld else { return }
                isTouchHeld = false
                onHoldChanged(false)
              }
          )
          .accessibilityAddTraits(.isButton)
          .accessibilityAction {
            action()
            pulse()
          }
      } else {
        Button {
          action()
          pulse()
        } label: {
          keySurface
        }
        .buttonStyle(.plain)
      }
    }
    .onChange(of: feedbackTrigger) { _, newValue in
      guard newValue > 0 else { return }
      pulse()
    }
    .onDisappear {
      if isTouchHeld {
        isTouchHeld = false
        onHoldChanged(false)
      }
      activationTask?.cancel()
      activationTask = nil
    }
    .accessibilityLabel(accessibilityLabel)
    .accessibilityValue(isSelected ? "Selected" : "Not selected")
  }

  private var keySurface: some View {
    ZStack {
      RoundedRectangle(cornerRadius: 11, style: .continuous)
        .fill(.clear)
        .overlay {
          RoundedRectangle(cornerRadius: 11, style: .continuous)
            .stroke(
              keyAccent.opacity(isSelected ? 0.78 : 0.18),
              lineWidth: isSelected ? 1.35 : 0.7
            )
        }

      if isSelected {
        RoundedRectangle(cornerRadius: 11, style: .continuous)
          .stroke(.white.opacity(0.18), lineWidth: 1.2)
      }

      keyLabel
        .scaleEffect(activationScale ? 1.12 : 1)
        .shadow(color: keyAccent.opacity(activationScale ? 0.68 : 0), radius: 8)
    }
    .keyboardKeySurface(
      usesLegacyTheme: usesLegacyTheme,
      clearGlassLevel: clearGlassLevel,
      cornerRadius: 11
    )
  }

  @ViewBuilder
  private var keyLabel: some View {
    if let image = key.systemImage {
      Image(systemName: image)
        .font(
          .system(
            size: usesTabletMetrics ? (compact ? 21 : 23) : (compact ? 15 : 17),
            weight: .bold
          )
        )
        .foregroundStyle(labelColor)
    } else {
      Text(key.label)
        .font(labelFont)
        .foregroundStyle(labelColor)
        .lineLimit(1)
        .minimumScaleFactor(0.62)
        .padding(.horizontal, 3)
    }
  }

  private var labelFont: Font {
    let baseSize = usesTabletMetrics
      ? (compact ? CGFloat(21) : CGFloat(23))
      : (compact ? CGFloat(15) : CGFloat(17))
    switch key.action {
    case .space:
      return .system(size: baseSize * 0.82, weight: .semibold, design: .rounded)
    case .shift, .numbers, .backspace, .returnKey:
      return .system(size: baseSize * 0.82, weight: .bold, design: .rounded)
    case .character:
      return .system(size: baseSize, weight: .semibold, design: .rounded)
    }
  }

  private var labelColor: Color {
    if key.usesAccentColor {
      return keyAccent.opacity(isSelected ? 1 : 0.92)
    }
    return .white.opacity(isSelected ? 1 : 0.84)
  }

  private var keyAccent: Color {
    key.usesAccentColor ? accent : .white
  }

  private var accessibilityLabel: String {
    switch key.action {
    case .character(let value):
      return value
    case .shift:
      return "Shift"
    case .numbers:
      return key.label == "ABC" ? "Letters" : "Numbers"
    case .space:
      return "Space"
    case .backspace:
      return "Delete"
    case .returnKey:
      return "Return"
    }
  }

  private func pulse() {
    guard !reduceMotion else { return }

    activationTask?.cancel()
    activationScale = false
    activationTask = Task { @MainActor in
      try? await Task.sleep(for: .milliseconds(8))
      guard !Task.isCancelled else { return }
      withAnimation(.easeOut(duration: 0.08)) {
        activationScale = true
      }

      try? await Task.sleep(for: .milliseconds(95))
      guard !Task.isCancelled else { return }
      withAnimation(.easeOut(duration: 0.14)) {
        activationScale = false
      }
    }
  }
}
