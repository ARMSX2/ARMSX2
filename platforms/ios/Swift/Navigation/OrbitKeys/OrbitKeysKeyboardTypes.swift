// OrbitKeysKeyboardTypes.swift — key grid, layout table and haptics
// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.

// MARK: - Keyboard Types

enum OrbitKeysGridPosition: Int, CaseIterable, Identifiable, Sendable {
  case topLeft
  case top
  case topRight
  case left
  case center
  case right
  case bottomLeft
  case bottom
  case bottomRight

  var id: Int { rawValue }

  var row: Int {
    switch self {
    case .topLeft, .top, .topRight: 0
    case .left, .center, .right: 1
    case .bottomLeft, .bottom, .bottomRight: 2
    }
  }

  var column: Int {
    switch self {
    case .topLeft, .left, .bottomLeft: 0
    case .top, .center, .bottom: 1
    case .topRight, .right, .bottomRight: 2
    }
  }

  var spokenName: String {
    switch self {
    case .topLeft: "top left"
    case .top: "top"
    case .topRight: "top right"
    case .left: "center left"
    case .center: "center"
    case .right: "center right"
    case .bottomLeft: "bottom left"
    case .bottom: "bottom"
    case .bottomRight: "bottom right"
    }
  }

  static func from(stickX x: Float, y: Float, deadZone: Float = 0.38) -> OrbitKeysGridPosition {
    let column = x < -deadZone ? 0 : (x > deadZone ? 2 : 1)
    let row = y > deadZone ? 0 : (y < -deadZone ? 2 : 1)
    return allCases.first { $0.row == row && $0.column == column } ?? .center
  }
}

enum OrbitKeysFaceButton: String, CaseIterable, Identifiable, Sendable {
  case square
  case cross
  case circle
  case triangle

  var id: String { rawValue }

  static func from(characterStickX x: Float, y: Float, threshold: Float = 0.68) -> OrbitKeysFaceButton? {
    guard max(abs(x), abs(y)) >= threshold else { return nil }

    if abs(x) > abs(y) {
      return x < 0 ? .square : .circle
    }
    return y < 0 ? .cross : .triangle
  }
}

enum OrbitKeysKeyboardMode: String, Sendable {
  case letters
  case shifted
  case numbers
}

enum OrbitKeysArrangement: String, Sendable {
  case alphabetical
  case qaw
}

enum OrbitKeysSettingsOption: Int, CaseIterable, Identifiable, Sendable {
  case normalKeyboard
  case loopFullKeyboard
  case legacyTheme
  case layout
  case faceIcons
  case iconsOnAllKeys
  case colorsOnAllBoxes
  case orbitField
  case orbsInForeground
  case clearGlass
  case transparency

  var id: Int { rawValue }
}

enum OrbitKeysKeyCommand: Equatable, Sendable {
  case insert(String)
  case forwardDelete
}

struct OrbitKeysKeyboardKey: Equatable, Sendable {
  let label: String
  let command: OrbitKeysKeyCommand

  var isAction: Bool {
    switch command {
    case .insert: false
    case .forwardDelete: true
    }
  }
}

enum OrbitKeysLayout {
  private static let base: [[String]] = [
    ["a", "b", "c", ","],
    ["d", "e", "f", "."],
    ["g", "h", "i", "!"],
    ["j", "k", "l", "-"],
    ["m", "␣", "n", "Del"],
    ["o", "p", "q", "?"],
    ["r", "s", "t", "("],
    ["u", "v", "w", ":"],
    ["x", "y", "z", ")"],
  ]

  // Maps the grid to a controller-friendly QWERTY order while keeping center actions fixed.
  private static let qaw: [[String]] = [
    ["q", "a", "w", ","],
    ["s", "e", "d", "."],
    ["r", "f", "t", "!"],
    ["g", "y", "h", "-"],
    ["u", "␣", "j", "Del"],
    ["i", "k", "o", "?"],
    ["l", "p", "z", "("],
    ["x", "c", "v", ":"],
    ["b", "n", "m", ")"],
  ]

  // Maps face buttons as Square, Cross, Circle, Triangle; Circle holds 1-9 and Cross adds 0 in the last cell.
  private static let numeric: [[String]] = [
    ["[", "{", "1", "<"],
    ["]", "}", "2", ">"],
    ["#", "$", "3", "%"],
    ["+", "×", "4", "÷"],
    ["&", "␣", "5", "Del"],
    ["\\", "|", "6", "~"],
    ["€", "£", "7", "¥"],
    ["'", "`", "8", "•"],
    ["°", "0", "9", "§"],
  ]

  private static let shiftedPunctuation: [String: String] = [
    ",": "^",
    ".": "@",
    "!": "*",
    "-": "_",
    "?": "\"",
    "(": "=",
    ":": ";",
    ")": "/",
  ]

  static func key(
    at position: OrbitKeysGridPosition,
    face: OrbitKeysFaceButton,
    mode: OrbitKeysKeyboardMode,
    arrangement: OrbitKeysArrangement = .alphabetical
  ) -> OrbitKeysKeyboardKey {
    if position == .center, face == .cross {
      return OrbitKeysKeyboardKey(label: "␣", command: .insert(" "))
    }
    if position == .center, face == .triangle {
      return OrbitKeysKeyboardKey(label: "Del", command: .forwardDelete)
    }

    let faceIndex = OrbitKeysFaceButton.allCases.firstIndex(of: face) ?? 0
    let rawLabel: String

    switch mode {
    case .letters:
      let layout = arrangement == .qaw ? qaw : base
      rawLabel = layout[position.rawValue][faceIndex]
    case .shifted:
      let layout = arrangement == .qaw ? qaw : base
      let baseLabel = layout[position.rawValue][faceIndex]
      rawLabel = shiftedPunctuation[baseLabel] ?? baseLabel.uppercased()
    case .numbers:
      rawLabel = numeric[position.rawValue][faceIndex]
    }

    return OrbitKeysKeyboardKey(label: rawLabel, command: .insert(rawLabel))
  }
}

// MARK: - Haptic Feedback

/// Plays the keyboard's feedback through the menu router, whose player applies
/// Haptic Feedback and UI Rumble Strength.
@MainActor
struct OrbitKeysHaptics {
  enum Feedback {
    case selection
    case key
    case space
    case delete
    case mode
    case success
  }

  weak var router: MenuControllerInputRouter?

  func play(_ feedback: Feedback) {
    let menuFeedback: MenuControllerFeedback = switch feedback {
    case .selection: .move(.left)
    case .key, .delete: .activate
    case .space: .back
    case .mode: .tabTransition
    case .success: .favorite(isFavorite: true)
    }
    router?.playTouchHaptics(menuFeedback)
  }
}
