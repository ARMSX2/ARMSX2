// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.
// Dynamic Background sources are intentionally excluded.
import Combine
import CoreHaptics
import Foundation
@preconcurrency import GameController
import UIKit

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

  var directionName: String {
    switch self {
    case .square: "left"
    case .cross: "bottom"
    case .circle: "right"
    case .triangle: "top"
    }
  }

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

  var title: String {
    switch self {
    case .letters: "abc"
    case .shifted: "ABC"
    case .numbers: "123"
    }
  }

  var accessibilityName: String {
    switch self {
    case .letters: "lowercase letters"
    case .shifted: "uppercase letters and alternate punctuation"
    case .numbers: "numbers and special characters"
    }
  }
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

@MainActor
final class OrbitKeysHapticManager {
  enum Feedback: CaseIterable, Hashable {
    case selection
    case key
    case space
    case delete
    case mode
    case success
  }

  private let selectionGenerator = UISelectionFeedbackGenerator()
  private let lightGenerator = UIImpactFeedbackGenerator(style: .light)
  private let rigidGenerator = UIImpactFeedbackGenerator(style: .rigid)
  private let softGenerator = UIImpactFeedbackGenerator(style: .soft)
  private let notificationGenerator = UINotificationFeedbackGenerator()
  private var controllerEngines: [CHHapticEngine] = []
  private var controllerPlayers: [Feedback: [any CHHapticPatternPlayer]] = [:]

  init() {
    selectionGenerator.prepare()
    lightGenerator.prepare()
  }

  func attach(to controller: GCController?) {
    for engine in controllerEngines {
      engine.stop(completionHandler: nil)
    }
    controllerPlayers.removeAll(keepingCapacity: false)
    controllerEngines.removeAll(keepingCapacity: false)
    guard let haptics = controller?.haptics else { return }

    let supported = haptics.supportedLocalities
    let localities: [GCHapticsLocality]
    if supported.contains(.leftHandle), supported.contains(.rightHandle) {
      localities = [.leftHandle, .rightHandle]
    } else if supported.contains(.handles) {
      localities = [.handles]
    } else if supported.contains(.default) {
      localities = [.default]
    } else if let locality = supported.first {
      localities = [locality]
    } else {
      return
    }

    for locality in localities {
      guard let engine = haptics.createEngine(withLocality: locality) else { continue }
      do {
        engine.isAutoShutdownEnabled = false
        try engine.start()
        controllerEngines.append(engine)
        for feedback in Feedback.allCases {
          let pattern = try CHHapticPattern(
            events: controllerEvents(for: feedback),
            parameters: []
          )
          let player = try engine.makePlayer(with: pattern)
          controllerPlayers[feedback, default: []].append(player)
        }
      } catch {
        continue
      }
    }
  }

  func play(_ feedback: Feedback) {
    playDeviceFeedback(feedback)
    playControllerFeedback(feedback)
  }

  private func playDeviceFeedback(_ feedback: Feedback) {
    switch feedback {
    case .selection:
      selectionGenerator.selectionChanged()
      selectionGenerator.prepare()
    case .key:
      rigidGenerator.impactOccurred(intensity: 0.72)
    case .space:
      softGenerator.impactOccurred(intensity: 0.66)
    case .delete:
      rigidGenerator.impactOccurred(intensity: 0.92)
    case .mode:
      lightGenerator.impactOccurred(intensity: 0.55)
    case .success:
      notificationGenerator.notificationOccurred(.success)
    }
  }

  private func playControllerFeedback(_ feedback: Feedback) {
    guard let players = controllerPlayers[feedback] else { return }
    for player in players {
      try? player.start(atTime: CHHapticTimeImmediate)
    }
  }

  private func controllerEvents(for feedback: Feedback) -> [CHHapticEvent] {
    switch feedback {
    case .selection:
      [transient(intensity: 0.1, sharpness: 0.5, time: 0)]
    case .key:
      [transient(intensity: 0.24, sharpness: 0.58, time: 0)]
    case .space:
      [transient(intensity: 0.16, sharpness: 0.18, time: 0)]
    case .delete:
      [transient(intensity: 0.28, sharpness: 0.68, time: 0)]
    case .mode:
      [transient(intensity: 0.16, sharpness: 0.34, time: 0)]
    case .success:
      [
        transient(intensity: 0.16, sharpness: 0.3, time: 0),
        transient(intensity: 0.24, sharpness: 0.52, time: 0.055),
      ]
    }
  }

  private func transient(
    intensity: Float,
    sharpness: Float,
    time: TimeInterval
  ) -> CHHapticEvent {
    CHHapticEvent(
      eventType: .hapticTransient,
      parameters: [
        CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
        CHHapticEventParameter(parameterID: .hapticSharpness, value: sharpness),
      ],
      relativeTime: time
    )
  }

}

// MARK: - Keyboard Model

@MainActor
final class OrbitKeysModel: ObservableObject {
  @Published private(set) var selectedPosition: OrbitKeysGridPosition = .center
  @Published private(set) var characters: [String] = []
  @Published private(set) var cursor = 0
  @Published private(set) var isKeyboardVisible = false
  @Published private(set) var committedText: String
  @Published private(set) var pressedFace: OrbitKeysFaceButton?
  @Published private(set) var stickX: Float = 0
  @Published private(set) var stickY: Float = 0
  @Published private(set) var rightStickX: Float = 0
  @Published private(set) var rightStickY: Float = 0
  @Published private(set) var rightStickFace: OrbitKeysFaceButton?
  @Published private(set) var isLeftShoulderHeld = false
  @Published private(set) var isRightShoulderHeld = false
  @Published private(set) var usesFullQwerty =
    UserDefaults.standard.object(forKey: "OrbitKeys.keyboard.usesFullQwerty") as? Bool
    ?? true
  @Published private(set) var loopsFullQwertyRows =
    UserDefaults.standard.object(forKey: "OrbitKeys.keyboard.loopsFullQwertyRows") as? Bool
    ?? true
  @Published private(set) var usesLegacyTheme =
    UserDefaults.standard.object(forKey: "OrbitKeys.keyboard.usesLegacyTheme") as? Bool
    ?? true
  @Published private(set) var fullQwertySelection = OrbitKeysFullQwertyKeyPosition.home
  @Published private(set) var fullQwertyToolbarSelection:
    OrbitKeysFullQwertyToolbarControl?
  @Published private(set) var fullQwertyFeedbackSequence = 0
  @Published private(set) var lastActivatedFullQwertyPosition: OrbitKeysFullQwertyKeyPosition?
  @Published private(set) var keyboardArrangement: OrbitKeysArrangement = .alphabetical
  @Published private(set) var showsFaceIcons = true
  @Published private(set) var showsFaceIconsOnAllButtons = false
  @Published private(set) var showsColorsOnAllBoxes = false
  @Published private(set) var showsOrbs = false
  @Published private(set) var orbsInForeground = true
  @Published private(set) var clearGlassLevel = 1.0
  @Published private(set) var isKeyboardSettingsVisible = false
  @Published private(set) var selectedKeyboardSettingsOption: OrbitKeysSettingsOption =
    .normalKeyboard
  @Published private(set) var feedbackSequence = 0
  @Published private(set) var selectionSequence = 0
  @Published private(set) var characterSequence = 0
  @Published private(set) var lastActivatedFace: OrbitKeysFaceButton?
  @Published private(set) var lastActivatedPosition: OrbitKeysGridPosition?
  @Published private(set) var toastMessage: String?
  @Published private(set) var lastSubmittedText: String?

  @Published private var isL2Held = false
  @Published private var isR2Held = false
  @Published private var touchNumbersLocked = false
  @Published private var touchShiftLocked = false
  private var activeController: GCController?
  private var isRightStickLatched = false
  private var isL3Pressed = false
  private var isR3Pressed = false
  private var backspaceRepeatTask: Task<Void, Never>?
  private var faceRepeatTask: Task<Void, Never>?
  private var pressedFaceReleaseTask: Task<Void, Never>?
  private var repeatingFace: OrbitKeysFaceButton?
  private var cursorRepeatTask: Task<Void, Never>?
  private var cursorRepeatOffset: Int?
  private var fullQwertyMoveRepeatTask: Task<Void, Never>?
  private var fullQwertyHeldDirection: OrbitKeysFullQwertyDirection?
  private var fullQwertyKeyRepeatTask: Task<Void, Never>?
  private var fullQwertyTouchBackspaceRepeatTask: Task<Void, Never>?
  private var isFullQwertySelectHeld = false
  private var keyboardSettingsDpadRepeatTask: Task<Void, Never>?
  private var keyboardSettingsHeldDirection: OrbitKeysFullQwertyDirection?
  private var observers: [NSObjectProtocol] = []
  private let haptics = OrbitKeysHapticManager()
  private let onCommit: (String) -> Void

  var mode: OrbitKeysKeyboardMode {
    if isL2Held || touchNumbersLocked {
      return .numbers
    }
    if isR2Held || touchShiftLocked {
      return .shifted
    }
    return .letters
  }

  var text: String {
    characters.joined()
  }

  var showsKeyboardSwitchAction: Bool {
    text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
  }

  var cursorUTF16Offset: Int {
    characters.prefix(cursor).joined().utf16.count
  }

  var highlightedFace: OrbitKeysFaceButton? { rightStickFace ?? pressedFace }

  var usesClearGlass: Bool {
    clearGlassLevel > 0.001
  }

  init(
    initialText: String = "",
    startsControllerDiscovery: Bool = true,
    startsInNormalKeyboard: Bool = false,
    onCommit: @escaping (String) -> Void = { _ in }
  ) {
    committedText = initialText
    if startsInNormalKeyboard {
      // Rename always opens on the conventional QWERTY layout. The alternate
      // OrbitKeys grid remains available from keyboard Settings.
      usesFullQwerty = true
    }
    self.onCommit = onCommit
    observeControllers()
    guard startsControllerDiscovery else { return }

    if let controller = GCController.current ?? GCController.controllers().first {
      connect(controller)
    } else {
      GCController.startWirelessControllerDiscovery(completionHandler: nil)
    }
  }

  func select(_ position: OrbitKeysGridPosition) {
    select(position, playsAudio: true)
  }

  private func select(_ position: OrbitKeysGridPosition, playsAudio: Bool) {
    guard position != selectedPosition else { return }
    selectedPosition = position
    selectionSequence += 1
    haptics.play(.selection)
    if playsAudio {
      MenuAudioPackManager.shared.playEvent(.navigation)
    }
  }

  func activate(_ face: OrbitKeysFaceButton) {
    guard isKeyboardVisible else {
      if face == .cross {
        openKeyboard()
      }
      return
    }
    pressedFace = face
    let key = OrbitKeysLayout.key(
      at: selectedPosition,
      face: face,
      mode: mode,
      arrangement: keyboardArrangement
    )

    switch key.command {
    case .insert(let value):
      lastActivatedFace = face
      lastActivatedPosition = selectedPosition
      characterSequence += 1
      insert(value)
      haptics.play(value == " " ? .space : .key)
      MenuAudioPackManager.shared.playEvent(.navigation)
    case .forwardDelete:
      if deleteAtCursor() {
        haptics.play(.delete)
        MenuAudioPackManager.shared.playEvent(.return)
      }
    }

    feedbackSequence += 1
    schedulePressedFaceRelease(face, after: 115, unlessRepeating: true)
  }

  func activateSwipe(in position: OrbitKeysGridPosition, face: OrbitKeysFaceButton) {
    select(position, playsAudio: false)
    activate(face)
  }

  func backspace() {
    guard isKeyboardVisible, cursor > 0 else { return }
    characters.remove(at: cursor - 1)
    cursor -= 1
    feedbackSequence += 1
    haptics.play(.delete)
    MenuAudioPackManager.shared.playEvent(.return)
  }

  func setBackspaceHeld(_ isHeld: Bool) {
    guard isLeftShoulderHeld != isHeld else { return }
    isLeftShoulderHeld = isHeld
    if isHeld {
      guard backspaceRepeatTask == nil else { return }
      backspace()
      backspaceRepeatTask = makeRepeatTask(initialDelay: 420, repeatDelay: 82) { model in
        model.backspace()
        return true
      }
    } else {
      backspaceRepeatTask?.cancel()
      backspaceRepeatTask = nil
    }
  }

  func setSpaceHeld(_ isHeld: Bool) {
    guard isRightShoulderHeld != isHeld else { return }
    isRightShoulderHeld = isHeld
    if isHeld {
      insertSpace()
    }
  }

  func toggleKeyboardStyle() {
    setFullQwertyEnabled(!usesFullQwerty)
  }

  func performPrimaryEditorAction() {
    if showsKeyboardSwitchAction {
      toggleKeyboardStyle()
      return
    }

    commitAndClose()
  }

  func openKeyboardSettings() {
    if !isKeyboardVisible {
      openKeyboard(playsAudio: false)
    }
    guard !isKeyboardSettingsVisible else { return }
    setKeyboardSettingsVisible(true)
    MenuAudioPackManager.shared.playEvent(.contextMenu)
  }

  func closeKeyboardSettings() {
    guard isKeyboardSettingsVisible else { return }
    setKeyboardSettingsVisible(false)
    MenuAudioPackManager.shared.playEvent(.return)
  }

  func toggleKeyboardSettings() {
    if isKeyboardSettingsVisible {
      closeKeyboardSettings()
    } else {
      openKeyboardSettings()
    }
  }

  func selectKeyboardSettingsOption(_ option: OrbitKeysSettingsOption) {
    guard selectedKeyboardSettingsOption != option else { return }
    selectedKeyboardSettingsOption = option
    selectionSequence += 1
    haptics.play(.selection)
    MenuAudioPackManager.shared.playEvent(.navigation)
  }

  func moveKeyboardSettingsSelection(by offset: Int) {
    let options = OrbitKeysSettingsOption.allCases
    guard let currentIndex = options.firstIndex(of: selectedKeyboardSettingsOption) else {
      selectedKeyboardSettingsOption = .normalKeyboard
      return
    }

    let nextIndex = min(
      max(currentIndex + offset, options.startIndex),
      options.index(before: options.endIndex)
    )
    selectKeyboardSettingsOption(options[nextIndex])
  }

  func activateKeyboardSettingsSelection() {
    switch selectedKeyboardSettingsOption {
    case .normalKeyboard:
      toggleKeyboardStyle()
    case .loopFullKeyboard:
      setFullQwertyRowLoopEnabled(!loopsFullQwertyRows)
    case .legacyTheme:
      setLegacyThemeEnabled(!usesLegacyTheme)
    case .layout:
      toggleKeyboardArrangement()
    case .faceIcons:
      toggleFaceIcons()
    case .iconsOnAllKeys:
      setFaceIconsOnAllButtons(!showsFaceIconsOnAllButtons)
    case .colorsOnAllBoxes:
      setColorsOnAllBoxes(!showsColorsOnAllBoxes)
    case .orbitField:
      setOrbsVisible(!showsOrbs)
    case .orbsInForeground:
      setOrbsInForeground(!orbsInForeground)
    case .clearGlass:
      setClearGlassEnabled(!usesClearGlass)
    case .transparency:
      setClearGlassEnabled(!usesClearGlass)
    }
  }

  func adjustKeyboardSettingsSelection(by direction: Int) {
    guard direction != 0 else { return }

    switch selectedKeyboardSettingsOption {
    case .normalKeyboard:
      setFullQwertyEnabled(direction > 0)
    case .loopFullKeyboard:
      setFullQwertyRowLoopEnabled(direction > 0)
    case .legacyTheme:
      setLegacyThemeEnabled(direction > 0)
    case .layout:
      setKeyboardArrangement(direction > 0 ? .qaw : .alphabetical)
    case .faceIcons:
      setFaceIconsVisible(direction > 0)
    case .iconsOnAllKeys:
      setFaceIconsOnAllButtons(direction > 0)
    case .colorsOnAllBoxes:
      setColorsOnAllBoxes(direction > 0)
    case .orbitField:
      setOrbsVisible(direction > 0)
    case .orbsInForeground:
      setOrbsInForeground(direction > 0)
    case .clearGlass:
      setClearGlassEnabled(direction > 0)
    case .transparency:
      setClearGlassLevel(clearGlassLevel + Double(direction) * 0.01)
    }
  }

  func setFullQwertyEnabled(_ isEnabled: Bool) {
    guard usesFullQwerty != isEnabled else { return }
    usesFullQwerty = isEnabled
    fullQwertySelection = OrbitKeysFullQwertyLayout.clampedPosition(
      fullQwertySelection,
      mode: mode
    )
    stopFaceRepeat()
    stopCursorRepeat()
    stopFullQwertyRepeats()
    rightStickFace = nil
    isRightStickLatched = false
    UserDefaults.standard.set(
      isEnabled,
      forKey: "OrbitKeys.keyboard.usesFullQwerty"
    )
    feedbackSequence += 1
    selectionSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isEnabled))
  }

  func setFullQwertyRowLoopEnabled(_ isEnabled: Bool) {
    guard loopsFullQwertyRows != isEnabled else { return }
    loopsFullQwertyRows = isEnabled
    UserDefaults.standard.set(
      isEnabled,
      forKey: "OrbitKeys.keyboard.loopsFullQwertyRows"
    )
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isEnabled))
  }

  func setLegacyThemeEnabled(_ isEnabled: Bool) {
    guard usesLegacyTheme != isEnabled else { return }
    usesLegacyTheme = isEnabled
    UserDefaults.standard.set(
      isEnabled,
      forKey: "OrbitKeys.keyboard.usesLegacyTheme"
    )
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isEnabled))
  }

  func selectFullQwertyKey(_ position: OrbitKeysFullQwertyKeyPosition) {
    selectFullQwertyKey(position, playsAudio: true)
  }

  private func selectFullQwertyKey(
    _ position: OrbitKeysFullQwertyKeyPosition,
    playsAudio: Bool
  ) {
    let nextPosition = OrbitKeysFullQwertyLayout.clampedPosition(position, mode: mode)
    guard nextPosition != fullQwertySelection || fullQwertyToolbarSelection != nil else {
      return
    }
    fullQwertyToolbarSelection = nil
    fullQwertySelection = nextPosition
    selectionSequence += 1
    haptics.play(.selection)
    if playsAudio {
      MenuAudioPackManager.shared.playEvent(.navigation)
    }
  }

  func activateFullQwertyKey(at position: OrbitKeysFullQwertyKeyPosition) {
    selectFullQwertyKey(position, playsAudio: false)
    activateFullQwertySelection()
  }

  func activateFullQwertySelection() {
    guard isKeyboardVisible else { return }

    if let toolbarSelection = fullQwertyToolbarSelection {
      switch toolbarSelection {
      case .send:
        performPrimaryEditorAction()
      case .settings:
        openKeyboardSettings()
      case .close:
        cancelAndClose()
      }
      return
    }

    guard
      let key = OrbitKeysFullQwertyLayout.key(at: fullQwertySelection, mode: mode)
    else {
      return
    }

    performFullQwertyKey(key)
  }

  func setFullQwertyTouchBackspaceHeld(_ isHeld: Bool) {
    if isHeld {
      guard fullQwertyTouchBackspaceRepeatTask == nil else { return }
      performFullQwertyTouchBackspace()
      fullQwertyTouchBackspaceRepeatTask = makeRepeatTask(
        initialDelay: 420,
        repeatDelay: 82
      ) { model in
        model.performFullQwertyTouchBackspace()
        return true
      }
    } else {
      stopFullQwertyTouchBackspaceRepeat()
    }
  }

  func insertSpace() {
    guard isKeyboardVisible else { return }
    insert(" ")
    feedbackSequence += 1
    haptics.play(.space)
    MenuAudioPackManager.shared.playEvent(.navigation)
  }

  func moveCursor(by offset: Int) {
    guard !characters.isEmpty else { return }
    let nextCursor = min(max(cursor + offset, 0), characters.count)
    guard nextCursor != cursor else { return }
    cursor = nextCursor
    selectionSequence += 1
    haptics.play(.selection)
    MenuAudioPackManager.shared.playEvent(.navigation)
  }

  func moveCursorToStart() {
    guard cursor != 0 else { return }
    cursor = 0
    selectionSequence += 1
    haptics.play(.selection)
    MenuAudioPackManager.shared.playEvent(.navigation)
  }

  func toggleShiftLock() {
    touchShiftLocked.toggle()
    if touchShiftLocked {
      touchNumbersLocked = false
    }
    modeDidChange()
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: touchShiftLocked))
  }

  func toggleNumbersLock() {
    touchNumbersLocked.toggle()
    if touchNumbersLocked {
      touchShiftLocked = false
    }
    modeDidChange()
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: touchNumbersLocked))
  }

  func toggleKeyboardArrangement() {
    setKeyboardArrangement(
      keyboardArrangement == .alphabetical ? .qaw : .alphabetical
    )
  }

  func setKeyboardArrangement(_ arrangement: OrbitKeysArrangement) {
    guard keyboardArrangement != arrangement else { return }
    keyboardArrangement = arrangement
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.tabTransition)
  }

  func toggleFaceIcons() {
    setFaceIconsVisible(!showsFaceIcons)
  }

  func setFaceIconsVisible(_ isVisible: Bool) {
    guard showsFaceIcons != isVisible else { return }
    showsFaceIcons = isVisible
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isVisible))
  }

  func setFaceIconsOnAllButtons(_ showsOnAllButtons: Bool) {
    guard showsFaceIconsOnAllButtons != showsOnAllButtons else { return }
    showsFaceIconsOnAllButtons = showsOnAllButtons
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: showsOnAllButtons))
  }

  func setColorsOnAllBoxes(_ showsColors: Bool) {
    guard showsColorsOnAllBoxes != showsColors else { return }
    showsColorsOnAllBoxes = showsColors
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: showsColors))
  }

  func setOrbsVisible(_ isVisible: Bool) {
    guard showsOrbs != isVisible else { return }
    showsOrbs = isVisible
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isVisible))
  }

  func setOrbsInForeground(_ isForeground: Bool) {
    guard orbsInForeground != isForeground else { return }
    orbsInForeground = isForeground
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isForeground))
  }

  func setClearGlassEnabled(_ isEnabled: Bool) {
    let nextLevel = isEnabled ? 1.0 : 0.0
    guard abs(clearGlassLevel - nextLevel) > 0.001 else { return }
    clearGlassLevel = nextLevel
    feedbackSequence += 1
    haptics.play(.mode)
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isEnabled))
  }

  func setClearGlassLevel(_ level: Double) {
    let nextLevel = min(max(level, -1), 1)
    guard abs(clearGlassLevel - nextLevel) > 0.001 else { return }
    clearGlassLevel = nextLevel
    MenuAudioPackManager.shared.playEvent(.navigation)
  }

  func openKeyboard() {
    openKeyboard(playsAudio: true)
  }

  private func openKeyboard(playsAudio: Bool) {
    guard !isKeyboardVisible else { return }
    resetTransientModeHolds()
    haptics.attach(to: activeController)
    characters = committedText.map(String.init)
    cursor = characters.count
    fullQwertySelection = OrbitKeysFullQwertyLayout.clampedPosition(
      fullQwertySelection,
      mode: mode
    )
    fullQwertyToolbarSelection = nil
    isKeyboardVisible = true
    haptics.play(.mode)
    if playsAudio {
      MenuAudioPackManager.shared.playEvent(.tabTransition)
    }
  }

  func commitAndClose() {
    guard isKeyboardVisible else { return }
    committedText = text
    lastSubmittedText = text
    onCommit(text)
    showToast(text.isEmpty ? "Text cleared" : "Text inserted")
    closeKeyboard(clearsActivation: false, feedback: .success)
  }

  func cancelAndClose() {
    guard isKeyboardVisible else { return }
    characters = committedText.map(String.init)
    cursor = characters.count
    closeKeyboard(clearsActivation: true, feedback: .mode)
  }

  func clearAndClose() {
    guard isKeyboardVisible else { return }
    characters = []
    cursor = 0
    committedText = ""
    lastSubmittedText = ""
    onCommit("")
    showToast("Text cleared")
    closeKeyboard(clearsActivation: true, feedback: .delete)
  }

  // Drops the closed keyboard's duplicate text buffer, repeat tasks, and haptic engines.
  // Controller input remains connected so the keyboard can still be opened from the menu.
  func releaseClosedKeyboardResources() {
    guard !isKeyboardVisible else { return }

    backspaceRepeatTask?.cancel()
    backspaceRepeatTask = nil
    isLeftShoulderHeld = false
    isRightShoulderHeld = false
    stopFaceRepeat()
    stopCursorRepeat()
    stopFullQwertyRepeats()
    stopKeyboardSettingsDpadRepeat()
    resetTransientModeHolds()
    resetPublishedStickVector()
    resetPublishedCharacterStickVector()
    pressedFace = nil
    rightStickFace = nil
    isRightStickLatched = false
    lastActivatedFace = nil
    lastActivatedPosition = nil
    lastActivatedFullQwertyPosition = nil
    characters.removeAll(keepingCapacity: false)
    cursor = 0
    haptics.attach(to: nil)
  }

  /// Final teardown for an embedded keyboard presentation. Per-element
  /// GameController callbacks otherwise outlive the SwiftUI cover and keep
  /// allocating main-actor tasks for unrelated menu/gameplay input.
  func releaseKeyboardSessionResources() {
    if isKeyboardVisible {
      isKeyboardVisible = false
    }
    releaseClosedKeyboardResources()
    stopKeyboardSettingsDpadRepeat()
    removeControllerHandlers(from: activeController)
    activeController = nil
    haptics.attach(to: nil)
    for observer in observers {
      NotificationCenter.default.removeObserver(observer)
    }
    observers.removeAll(keepingCapacity: false)
    GCController.stopWirelessControllerDiscovery()
  }

  private func removeControllerHandlers(from controller: GCController?) {
    guard let controller, let gamepad = controller.extendedGamepad else {
      return
    }
    gamepad.leftThumbstick.valueChangedHandler = nil
    gamepad.rightThumbstick.valueChangedHandler = nil
    gamepad.leftThumbstickButton?.pressedChangedHandler = nil
    gamepad.rightThumbstickButton?.pressedChangedHandler = nil
    controller.physicalInputProfile.buttons[
      GCInputLeftThumbstickButton
    ]?.pressedChangedHandler = nil
    controller.physicalInputProfile.buttons[
      GCInputRightThumbstickButton
    ]?.pressedChangedHandler = nil
    gamepad.buttonX.pressedChangedHandler = nil
    gamepad.buttonA.pressedChangedHandler = nil
    gamepad.buttonB.pressedChangedHandler = nil
    gamepad.buttonY.pressedChangedHandler = nil
    gamepad.buttonMenu.pressedChangedHandler = nil
    gamepad.buttonOptions?.pressedChangedHandler = nil
    gamepad.leftShoulder.pressedChangedHandler = nil
    gamepad.rightShoulder.pressedChangedHandler = nil
    gamepad.leftTrigger.valueChangedHandler = nil
    gamepad.rightTrigger.valueChangedHandler = nil
    gamepad.dpad.left.pressedChangedHandler = nil
    gamepad.dpad.right.pressedChangedHandler = nil
    gamepad.dpad.up.pressedChangedHandler = nil
    gamepad.dpad.down.pressedChangedHandler = nil
  }

  // Leaves only the background renderer active while the main interface is hidden.
  func prepareForBackgroundOnlyDisplay() {
    if isKeyboardVisible {
      commitAndClose()
    }
    setKeyboardSettingsVisible(false, playsHaptic: false)
    toastMessage = nil
    releaseClosedKeyboardResources()
  }

  func clearText() {
    characters = []
    cursor = 0
    committedText = ""
    lastSubmittedText = ""
    onCommit("")
    showToast("Text cleared")
    feedbackSequence += 1
    haptics.play(.delete)
    MenuAudioPackManager.shared.playEvent(.uiToast)
  }

  func updateExternalText(_ value: String) {
    guard !isKeyboardVisible, value != committedText else { return }
    committedText = value
  }

  private func insert(_ value: String) {
    let insertedCharacters = value.map(String.init)
    characters.insert(contentsOf: insertedCharacters, at: cursor)
    cursor += insertedCharacters.count
  }

  @discardableResult
  private func deleteAtCursor() -> Bool {
    if cursor < characters.count {
      characters.remove(at: cursor)
      return true
    } else if cursor > 0 {
      characters.remove(at: cursor - 1)
      cursor -= 1
      return true
    }
    return false
  }

  // Applies a full-keyboard key to the shared text buffer and keeps the visual feedback in sync.
  private func performFullQwertyKey(_ key: OrbitKeysFullQwertyKey) {
    switch key.action {
    case .character(let value):
      lastActivatedFullQwertyPosition = fullQwertySelection
      insert(value)
      characterSequence += 1
      feedbackSequence += 1
      fullQwertyFeedbackSequence += 1
      haptics.play(.key)
      MenuAudioPackManager.shared.playEvent(.navigation)
    case .shift:
      toggleShiftLock()
      fullQwertyFeedbackSequence += 1
    case .numbers:
      toggleNumbersLock()
      fullQwertyFeedbackSequence += 1
    case .space:
      lastActivatedFullQwertyPosition = fullQwertySelection
      insert(" ")
      characterSequence += 1
      feedbackSequence += 1
      fullQwertyFeedbackSequence += 1
      haptics.play(.space)
      MenuAudioPackManager.shared.playEvent(.navigation)
    case .backspace:
      lastActivatedFullQwertyPosition = fullQwertySelection
      backspace()
      fullQwertyFeedbackSequence += 1
    case .returnKey:
      lastActivatedFullQwertyPosition = fullQwertySelection
      insert("\n")
      characterSequence += 1
      feedbackSequence += 1
      fullQwertyFeedbackSequence += 1
      haptics.play(.key)
      MenuAudioPackManager.shared.playEvent(.navigation)
    }
  }

  private func performFullQwertyTouchBackspace() {
    backspace()
    fullQwertyFeedbackSequence += 1
  }

  private func showToast(_ message: String) {
    toastMessage = message
    let currentMessage = message
    Task { @MainActor [weak self] in
      try? await Task.sleep(for: .seconds(1.6))
      guard self?.toastMessage == currentMessage else { return }
      self?.toastMessage = nil
    }
  }

  private func closeKeyboard(
    clearsActivation: Bool,
    feedback: OrbitKeysHapticManager.Feedback
  ) {
    setBackspaceHeld(false)
    setSpaceHeld(false)
    stopFaceRepeat()
    stopCursorRepeat()
    stopFullQwertyRepeats()
    resetTransientModeHolds()
    setKeyboardSettingsVisible(false, playsHaptic: false)
    isKeyboardVisible = false
    fullQwertyToolbarSelection = nil
    pressedFace = nil
    rightStickFace = nil
    isRightStickLatched = false
    lastActivatedFullQwertyPosition = nil

    if clearsActivation {
      lastActivatedFace = nil
      lastActivatedPosition = nil
    }

    feedbackSequence += 1
    haptics.play(feedback)
    MenuAudioPackManager.shared.playEvent(.tabTransition)
  }

  private func makeRepeatTask(
    initialDelay: Int,
    repeatDelay: Int,
    action: @escaping @MainActor (OrbitKeysModel) -> Bool
  ) -> Task<Void, Never> {
    Task { @MainActor [weak self] in
      try? await Task.sleep(for: .milliseconds(initialDelay))
      while !Task.isCancelled {
        guard let self, action(self) else { break }
        try? await Task.sleep(for: .milliseconds(repeatDelay))
      }
    }
  }

  private func schedulePressedFaceRelease(
    _ face: OrbitKeysFaceButton,
    after delay: Int,
    unlessRepeating: Bool = false
  ) {
    pressedFaceReleaseTask?.cancel()
    pressedFaceReleaseTask = Task { @MainActor [weak self] in
      try? await Task.sleep(for: .milliseconds(delay))
      guard let self, self.pressedFace == face else { return }
      guard !unlessRepeating || self.repeatingFace != face else { return }
      self.pressedFace = nil
    }
  }

  private func resetTransientModeHolds() {
    guard isL2Held || isR2Held else { return }
    isL2Held = false
    isR2Held = false
    fullQwertySelection = OrbitKeysFullQwertyLayout.clampedPosition(
      fullQwertySelection,
      mode: mode
    )
    feedbackSequence += 1
    fullQwertyFeedbackSequence += 1
  }

  private func setKeyboardSettingsVisible(_ isVisible: Bool, playsHaptic: Bool = true) {
    guard isKeyboardSettingsVisible != isVisible else { return }
    isKeyboardSettingsVisible = isVisible

    if isVisible {
      resetTransientModeHolds()
      selectedKeyboardSettingsOption = .normalKeyboard
      stopFaceRepeat()
      stopCursorRepeat()
      stopFullQwertyRepeats()
      stopKeyboardSettingsDpadRepeat()
      setBackspaceHeld(false)
      setSpaceHeld(false)
      rightStickFace = nil
      isRightStickLatched = false
    } else {
      resetTransientModeHolds()
      stopKeyboardSettingsDpadRepeat()
    }

    feedbackSequence += 1
    if playsHaptic {
      haptics.play(.mode)
    }
  }

  private func modeDidChange() {
    fullQwertySelection = OrbitKeysFullQwertyLayout.clampedPosition(
      fullQwertySelection,
      mode: mode
    )
    feedbackSequence += 1
    fullQwertyFeedbackSequence += 1
    haptics.play(.mode)
  }

  private func observeControllers() {
    let center = NotificationCenter.default
    observers.append(
      center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) {
        [weak self] notification in
        guard let controller = notification.object as? GCController else { return }
        Task { @MainActor in self?.connect(controller) }
      }
    )
    observers.append(
      center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) {
        [weak self] notification in
        guard let controller = notification.object as? GCController else { return }
        Task { @MainActor in self?.disconnect(controller) }
      }
    )
  }

  private func connect(_ controller: GCController) {
    guard activeController == nil || activeController === controller else { return }
    guard let gamepad = controller.extendedGamepad else { return }
    activeController = controller
    GCController.stopWirelessControllerDiscovery()
    if isKeyboardVisible {
      haptics.attach(to: controller)
    }

    gamepad.leftThumbstick.valueChangedHandler = { [weak self] _, x, y in
      Task { @MainActor in self?.handleStick(x: x, y: y) }
    }
    gamepad.rightThumbstick.valueChangedHandler = { [weak self] _, x, y in
      Task { @MainActor in self?.handleCharacterStick(x: x, y: y) }
    }
    gamepad.leftThumbstickButton?.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in self?.setL3Pressed(pressed) }
    }
    gamepad.rightThumbstickButton?.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in self?.setR3Pressed(pressed) }
    }
    // ARMSX2 owns the aggregate profile callback. OrbitKeys uses the
    // per-element handlers above so both input systems can coexist while the
    // keyboard's modal capture prevents the library from receiving commands.
    controller.physicalInputProfile.buttons[
      GCInputLeftThumbstickButton
    ]?.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in self?.setL3Pressed(pressed) }
    }
    controller.physicalInputProfile.buttons[
      GCInputRightThumbstickButton
    ]?.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in self?.setR3Pressed(pressed) }
    }

    bind(gamepad.buttonX, to: .square)
    bind(gamepad.buttonA, to: .cross)
    bind(gamepad.buttonB, to: .circle)
    bind(gamepad.buttonY, to: .triangle)
    gamepad.buttonMenu.pressedChangedHandler = { [weak self] _, _, pressed in
      guard pressed else { return }
      Task { @MainActor in self?.commitAndClose() }
    }
    gamepad.buttonOptions?.pressedChangedHandler = { [weak self] _, _, pressed in
      guard pressed else { return }
      Task { @MainActor in self?.toggleKeyboardStyle() }
    }

    gamepad.leftShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in self?.setBackspaceHeld(pressed) }
    }
    gamepad.rightShoulder.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in self?.setSpaceHeld(pressed) }
    }

    gamepad.leftTrigger.valueChangedHandler = { [weak self] _, value, _ in
      Task { @MainActor in self?.setL2Held(value > 0.45) }
    }
    gamepad.rightTrigger.valueChangedHandler = { [weak self] _, value, _ in
      Task { @MainActor in self?.setR2Held(value > 0.45) }
    }

    gamepad.dpad.left.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in
        guard let self else { return }
        if self.handleKeyboardSettingsDpad(.left, isHeld: pressed) { return }
        if self.usesFullQwerty {
          self.setFullQwertyMoveHeld(.left, isHeld: pressed)
        } else {
          self.setCursorHeld(-1, isHeld: pressed)
        }
      }
    }
    gamepad.dpad.right.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in
        guard let self else { return }
        if self.handleKeyboardSettingsDpad(.right, isHeld: pressed) { return }
        if self.usesFullQwerty {
          self.setFullQwertyMoveHeld(.right, isHeld: pressed)
        } else {
          self.setCursorHeld(1, isHeld: pressed)
        }
      }
    }
    gamepad.dpad.up.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in
        guard let self else { return }
        if self.handleKeyboardSettingsDpad(.up, isHeld: pressed) { return }
        if self.usesFullQwerty {
          if self.isKeyboardVisible {
            self.setFullQwertyMoveHeld(.up, isHeld: pressed)
          } else if pressed {
            self.openKeyboard()
          }
          return
        }
        guard pressed else { return }
        self.isKeyboardVisible ? self.commitAndClose() : self.openKeyboard()
      }
    }
    gamepad.dpad.down.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in
        guard let self else { return }
        if self.handleKeyboardSettingsDpad(.down, isHeld: pressed) { return }
        if self.usesFullQwerty {
          self.setFullQwertyMoveHeld(.down, isHeld: pressed)
          return
        }
        guard pressed else { return }
        self.commitAndClose()
      }
    }
  }

  private func handleKeyboardSettingsDpad(
    _ direction: OrbitKeysFullQwertyDirection,
    isHeld: Bool
  ) -> Bool {
    guard isKeyboardSettingsVisible else { return false }
    setKeyboardSettingsDpadHeld(direction, isHeld: isHeld)
    return true
  }

  private func setKeyboardSettingsDpadHeld(
    _ direction: OrbitKeysFullQwertyDirection,
    isHeld: Bool
  ) {
    guard isHeld else {
      if keyboardSettingsHeldDirection == direction {
        stopKeyboardSettingsDpadRepeat()
      }
      return
    }
    guard keyboardSettingsHeldDirection != direction else { return }

    stopKeyboardSettingsDpadRepeat()
    keyboardSettingsHeldDirection = direction
    applyKeyboardSettingsDpad(direction)

    guard shouldRepeatKeyboardSettingsDpad(direction) else { return }
    let repeatDelay = keyboardSettingsRepeatDelay(for: direction)
    keyboardSettingsDpadRepeatTask = makeRepeatTask(
      initialDelay: 260,
      repeatDelay: repeatDelay
    ) { model in
      guard model.keyboardSettingsHeldDirection == direction else { return false }
      model.applyKeyboardSettingsDpad(direction)
      return true
    }
  }

  private func applyKeyboardSettingsDpad(_ direction: OrbitKeysFullQwertyDirection) {
    switch direction {
    case .up:
      moveKeyboardSettingsSelection(by: -1)
    case .down:
      moveKeyboardSettingsSelection(by: 1)
    case .left:
      adjustKeyboardSettingsSelection(by: -1)
    case .right:
      adjustKeyboardSettingsSelection(by: 1)
    }
  }

  private func shouldRepeatKeyboardSettingsDpad(_ direction: OrbitKeysFullQwertyDirection) -> Bool {
    switch direction {
    case .up, .down:
      return true
    case .left, .right:
      return selectedKeyboardSettingsOption == .transparency
    }
  }

  private func keyboardSettingsRepeatDelay(for direction: OrbitKeysFullQwertyDirection) -> Int {
    switch direction {
    case .up, .down:
      return 76
    case .left, .right:
      return 34
    }
  }

  private func stopKeyboardSettingsDpadRepeat() {
    keyboardSettingsDpadRepeatTask?.cancel()
    keyboardSettingsDpadRepeatTask = nil
    keyboardSettingsHeldDirection = nil
  }

  private func bind(_ input: GCControllerButtonInput, to face: OrbitKeysFaceButton) {
    input.pressedChangedHandler = { [weak self] _, _, pressed in
      Task { @MainActor in
        self?.setFaceHeld(face, isHeld: pressed)
      }
    }
  }

  private func disconnect(_ controller: GCController) {
    guard controller === activeController else { return }
    activeController = nil
    setBackspaceHeld(false)
    setSpaceHeld(false)
    stopFaceRepeat()
    stopCursorRepeat()
    stopFullQwertyRepeats()
    stopKeyboardSettingsDpadRepeat()
    resetTransientModeHolds()
    resetPublishedStickVector()
    resetPublishedCharacterStickVector()
    rightStickFace = nil
    isRightStickLatched = false
    isL3Pressed = false
    isR3Pressed = false
    haptics.attach(to: nil)

    if let nextController = GCController.controllers().first(where: { $0 !== controller }) {
      connect(nextController)
    }
  }

  private func handleStick(x: Float, y: Float) {
    guard isKeyboardVisible else { return }
    guard !isKeyboardSettingsVisible else {
      resetPublishedStickVector()
      stopFullQwertyMoveRepeat()
      handleKeyboardSettingsStick(x: x, y: y)
      return
    }
    if usesFullQwerty {
      resetPublishedStickVector()
      handleFullQwertyStick(x: x, y: y)
      return
    }
    publishStickVector(x: x, y: y)
    select(OrbitKeysGridPosition.from(stickX: x, y: y))
  }

  private func publishStickVector(x: Float, y: Float) {
    let resolvedX: Float = abs(x) < 0.05 ? 0 : x
    let resolvedY: Float = abs(y) < 0.05 ? 0 : y
    guard abs(resolvedX - stickX) >= 0.06
      || abs(resolvedY - stickY) >= 0.06
    else {
      return
    }
    stickX = resolvedX
    stickY = resolvedY
  }

  private func resetPublishedStickVector() {
    guard stickX != 0 || stickY != 0 else { return }
    stickX = 0
    stickY = 0
  }

  private func handleKeyboardSettingsStick(x: Float, y: Float) {
    let deadZone: Float = 0.46
    guard max(abs(x), abs(y)) >= deadZone else {
      stopKeyboardSettingsDpadRepeat()
      return
    }

    let direction: OrbitKeysFullQwertyDirection =
      abs(x) > abs(y)
      ? (x < 0 ? .left : .right)
      : (y > 0 ? .up : .down)
    setKeyboardSettingsDpadHeld(direction, isHeld: true)
  }

  private func handleCharacterStick(x: Float, y: Float) {
    if isKeyboardSettingsVisible {
      resetPublishedCharacterStickVector()
      rightStickFace = nil
      isRightStickLatched = false
      return
    }

    if usesFullQwerty {
      resetPublishedCharacterStickVector()
      rightStickFace = nil
      isRightStickLatched = false
      return
    }

    publishCharacterStickVector(x: x, y: y)

    if max(abs(x), abs(y)) < 0.32 {
      rightStickFace = nil
      isRightStickLatched = false
      return
    }

    guard isKeyboardVisible,
      let face = OrbitKeysFaceButton.from(characterStickX: x, y: y)
    else {
      return
    }

    rightStickFace = face
    guard !isRightStickLatched else { return }
    isRightStickLatched = true
    activate(face)
  }

  private func publishCharacterStickVector(x: Float, y: Float) {
    let resolvedX: Float = abs(x) < 0.05 ? 0 : x
    let resolvedY: Float = abs(y) < 0.05 ? 0 : y
    guard abs(resolvedX - rightStickX) >= 0.06
      || abs(resolvedY - rightStickY) >= 0.06
    else {
      return
    }
    rightStickX = resolvedX
    rightStickY = resolvedY
  }

  private func resetPublishedCharacterStickVector() {
    guard rightStickX != 0 || rightStickY != 0 else { return }
    rightStickX = 0
    rightStickY = 0
  }

  private func setFaceHeld(_ face: OrbitKeysFaceButton, isHeld: Bool) {
    if isKeyboardSettingsVisible {
      handleKeyboardSettingsFace(face, isHeld: isHeld)
      return
    }

    if usesFullQwerty, isKeyboardVisible {
      setFullQwertyFaceHeld(face, isHeld: isHeld)
      return
    }

    guard isHeld else {
      if repeatingFace == face {
        stopFaceRepeat()
      }
      if pressedFace == face {
        pressedFaceReleaseTask?.cancel()
        pressedFaceReleaseTask = nil
        pressedFace = nil
      }
      return
    }

    guard isKeyboardVisible else {
      activate(face)
      return
    }
    guard repeatingFace != face else { return }

    stopFaceRepeat()
    repeatingFace = face
    activate(face)
    faceRepeatTask = makeRepeatTask(initialDelay: 420, repeatDelay: 82) { model in
      guard model.repeatingFace == face else { return false }
      model.activate(face)
      return true
    }
  }

  private func handleKeyboardSettingsFace(_ face: OrbitKeysFaceButton, isHeld: Bool) {
    guard isHeld else {
      if pressedFace == face {
        pressedFace = nil
      }
      return
    }

    pressedFace = face
    switch face {
    case .cross:
      activateKeyboardSettingsSelection()
    case .circle, .triangle:
      closeKeyboardSettings()
    case .square:
      clearText()
    }

    schedulePressedFaceRelease(face, after: 110)
  }

  private func stopFaceRepeat() {
    faceRepeatTask?.cancel()
    faceRepeatTask = nil
    repeatingFace = nil
    pressedFaceReleaseTask?.cancel()
    pressedFaceReleaseTask = nil
    pressedFace = nil
  }

  private func setCursorHeld(_ offset: Int, isHeld: Bool) {
    guard isHeld else {
      if cursorRepeatOffset == offset {
        stopCursorRepeat()
      }
      return
    }
    guard isKeyboardVisible, cursorRepeatOffset != offset else {
      return
    }

    stopCursorRepeat()
    cursorRepeatOffset = offset
    moveCursor(by: offset)
    cursorRepeatTask = makeRepeatTask(initialDelay: 320, repeatDelay: 55) { model in
      guard model.cursorRepeatOffset == offset else { return false }
      model.moveCursor(by: offset)
      return true
    }
  }

  private func stopCursorRepeat() {
    cursorRepeatTask?.cancel()
    cursorRepeatTask = nil
    cursorRepeatOffset = nil
  }

  // Converts analog stick movement into repeatable row-key navigation for the full keyboard.
  private func handleFullQwertyStick(x: Float, y: Float) {
    let deadZone: Float = 0.42
    guard max(abs(x), abs(y)) >= deadZone else {
      stopFullQwertyMoveRepeat()
      return
    }

    let direction: OrbitKeysFullQwertyDirection =
      abs(x) > abs(y)
      ? (x < 0 ? .left : .right)
      : (y > 0 ? .up : .down)
    setFullQwertyMoveHeld(direction, isHeld: true)
  }

  // Starts or stops fast repeated movement across the QWERTY rows.
  private func setFullQwertyMoveHeld(
    _ direction: OrbitKeysFullQwertyDirection,
    isHeld: Bool
  ) {
    guard isHeld else {
      if fullQwertyHeldDirection == direction {
        stopFullQwertyMoveRepeat()
      }
      return
    }
    guard isKeyboardVisible, fullQwertyHeldDirection != direction else {
      return
    }

    stopFullQwertyMoveRepeat()
    fullQwertyHeldDirection = direction
    moveFullQwertySelection(direction, permitsToolbarEntry: true)
    let initialDelay = fullQwertyMoveDelay(for: direction)
    let repeatDelay = fullQwertyRepeatDelay(for: direction)
    fullQwertyMoveRepeatTask = makeRepeatTask(
      initialDelay: initialDelay,
      repeatDelay: repeatDelay
    ) { model in
      guard model.fullQwertyHeldDirection == direction else { return false }
      model.moveFullQwertySelection(direction, permitsToolbarEntry: false)
      return true
    }
  }

  private func fullQwertyMoveDelay(for direction: OrbitKeysFullQwertyDirection) -> Int {
    switch direction {
    case .up, .down:
      return 440
    case .left, .right:
      return 220
    }
  }

  private func fullQwertyRepeatDelay(for direction: OrbitKeysFullQwertyDirection) -> Int {
    switch direction {
    case .up, .down:
      return 124
    case .left, .right:
      return 62
    }
  }

  private func moveFullQwertySelection(
    _ direction: OrbitKeysFullQwertyDirection,
    permitsToolbarEntry: Bool
  ) {
    if let toolbarSelection = fullQwertyToolbarSelection {
      let nextToolbarSelection: OrbitKeysFullQwertyToolbarControl?
      switch direction {
      case .left:
        nextToolbarSelection = OrbitKeysFullQwertyToolbarControl(
          rawValue: max(toolbarSelection.rawValue - 1, 0)
        )
      case .right:
        nextToolbarSelection = OrbitKeysFullQwertyToolbarControl(
          rawValue: min(
            toolbarSelection.rawValue + 1,
            OrbitKeysFullQwertyToolbarControl.allCases.count - 1
          )
        )
      case .up:
        nextToolbarSelection = toolbarSelection
      case .down:
        nextToolbarSelection = nil
      }

      if direction == .down {
        // The selected key remains unchanged while Send/Settings/Close owns
        // focus, so leaving the toolbar can restore that exact key.
        selectFullQwertyKey(fullQwertySelection)
      } else if let nextToolbarSelection,
                nextToolbarSelection != toolbarSelection {
        fullQwertyToolbarSelection = nextToolbarSelection
        selectionSequence += 1
        haptics.play(.selection)
        MenuAudioPackManager.shared.playEvent(.navigation)
      }
      return
    }

    if direction == .up,
       fullQwertySelection.row == 0,
       permitsToolbarEntry {
      fullQwertyToolbarSelection = .send
      selectionSequence += 1
      haptics.play(.selection)
      MenuAudioPackManager.shared.playEvent(.navigation)
      return
    }

    let nextPosition = OrbitKeysFullQwertyLayout.movedPosition(
      from: fullQwertySelection,
      direction: direction,
      mode: mode,
      loopsHorizontally: loopsFullQwertyRows
    )
    selectFullQwertyKey(nextPosition)
  }

  private func stopFullQwertyMoveRepeat() {
    fullQwertyMoveRepeatTask?.cancel()
    fullQwertyMoveRepeatTask = nil
    fullQwertyHeldDirection = nil
  }

  // Maps the PlayStation face buttons to common full-keyboard actions.
  private func setFullQwertyFaceHeld(_ face: OrbitKeysFaceButton, isHeld: Bool) {
    guard isHeld else {
      if face == .cross {
        stopFullQwertyKeyRepeat()
      }
      if face == .square {
        setBackspaceHeld(false)
      }
      if pressedFace == face {
        pressedFaceReleaseTask?.cancel()
        pressedFaceReleaseTask = nil
        pressedFace = nil
      }
      return
    }

    pressedFace = face
    switch face {
    case .cross:
      guard !isFullQwertySelectHeld else { return }
      isFullQwertySelectHeld = true
      activateFullQwertySelection()
      startFullQwertyKeyRepeatIfNeeded()
    case .square:
      setBackspaceHeld(true)
    case .circle:
      insertSpace()
    case .triangle:
      insertSpace()
      fullQwertyFeedbackSequence += 1
    }
  }

  private func startFullQwertyKeyRepeatIfNeeded() {
    guard fullQwertyToolbarSelection == nil,
      let key = OrbitKeysFullQwertyLayout.key(at: fullQwertySelection, mode: mode),
      key.repeatsWhileHeld,
      fullQwertyKeyRepeatTask == nil
    else {
      return
    }

    fullQwertyKeyRepeatTask = makeRepeatTask(
      initialDelay: 420,
      repeatDelay: 82
    ) { model in
      guard model.isFullQwertySelectHeld,
        let key = OrbitKeysFullQwertyLayout.key(at: model.fullQwertySelection, mode: model.mode),
        key.repeatsWhileHeld
      else {
        return false
      }

      model.performFullQwertyKey(key)
      return true
    }
  }

  private func stopFullQwertyKeyRepeat() {
    fullQwertyKeyRepeatTask?.cancel()
    fullQwertyKeyRepeatTask = nil
    isFullQwertySelectHeld = false
  }

  private func stopFullQwertyTouchBackspaceRepeat() {
    fullQwertyTouchBackspaceRepeatTask?.cancel()
    fullQwertyTouchBackspaceRepeatTask = nil
  }

  private func stopFullQwertyRepeats() {
    stopFullQwertyMoveRepeat()
    stopFullQwertyKeyRepeat()
    stopFullQwertyTouchBackspaceRepeat()
  }

  private func setL3Pressed(_ isPressed: Bool) {
    guard isL3Pressed != isPressed else { return }
    isL3Pressed = isPressed
    if isPressed {
      if usesFullQwerty, isKeyboardVisible {
        setFullQwertyEnabled(false)
      } else {
        toggleKeyboardArrangement()
      }
    }
  }

  private func setR3Pressed(_ isPressed: Bool) {
    guard isR3Pressed != isPressed else { return }
    isR3Pressed = isPressed
    if isPressed {
      toggleFaceIcons()
    }
  }

  private func setL2Held(_ isHeld: Bool) {
    guard isL2Held != isHeld else { return }
    isL2Held = isHeld
    modeDidChange()
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isHeld))
  }

  private func setR2Held(_ isHeld: Bool) {
    guard isR2Held != isHeld else { return }
    isR2Held = isHeld
    modeDidChange()
    MenuAudioPackManager.shared.playEvent(.toggle(isOn: isHeld))
  }
}
