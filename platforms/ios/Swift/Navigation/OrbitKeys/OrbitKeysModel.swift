// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.
// Dynamic Background sources are intentionally excluded.
import Combine
import Foundation
@preconcurrency import GameController
import UIKit

// MARK: - Keyboard Model

@MainActor
final class OrbitKeysModel: ObservableObject {
  @Published private(set) var selectedPosition: OrbitKeysGridPosition = .center
  @Published private(set) var characters: [String] = []
  @Published private(set) var cursor = 0
  @Published private(set) var isKeyboardVisible = false
  @Published private(set) var committedText: String
  @Published var pressedFace: OrbitKeysFaceButton?
  @Published var stickX: Float = 0
  @Published var stickY: Float = 0
  @Published var rightStickX: Float = 0
  @Published var rightStickY: Float = 0
  @Published var rightStickFace: OrbitKeysFaceButton?
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
  @Published var fullQwertySelection = OrbitKeysFullQwertyKeyPosition.home
  @Published var fullQwertyToolbarSelection:
    OrbitKeysFullQwertyToolbarControl?
  @Published var fullQwertyFeedbackSequence = 0
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
  @Published var selectionSequence = 0
  @Published private(set) var characterSequence = 0
  @Published private(set) var lastActivatedFace: OrbitKeysFaceButton?
  @Published private(set) var lastActivatedPosition: OrbitKeysGridPosition?
  @Published private(set) var toastMessage: String?
  @Published private(set) var lastSubmittedText: String?

  @Published var isL2Held = false
  @Published var isR2Held = false
  @Published private var touchNumbersLocked = false
  @Published private var touchShiftLocked = false
  var activeController: GCController?
  var isRightStickLatched = false
  var isL3Pressed = false
  var isR3Pressed = false
  private var backspaceRepeatTask: Task<Void, Never>?
  var faceRepeatTask: Task<Void, Never>?
  var pressedFaceReleaseTask: Task<Void, Never>?
  var repeatingFace: OrbitKeysFaceButton?
  var cursorRepeatTask: Task<Void, Never>?
  var cursorRepeatOffset: Int?
  var fullQwertyMoveRepeatTask: Task<Void, Never>?
  var fullQwertyHeldDirection: OrbitKeysFullQwertyDirection?
  var fullQwertyKeyRepeatTask: Task<Void, Never>?
  var fullQwertyTouchBackspaceRepeatTask: Task<Void, Never>?
  var isFullQwertySelectHeld = false
  var keyboardSettingsDpadRepeatTask: Task<Void, Never>?
  var keyboardSettingsHeldDirection: OrbitKeysFullQwertyDirection?
  var observers: [NSObjectProtocol] = []
  var haptics = OrbitKeysHaptics()
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

  /// A password shows as dots.
  let masksText: Bool

  init(
    initialText: String = "",
    startsControllerDiscovery: Bool = true,
    startsInNormalKeyboard: Bool = false,
    masksText: Bool = false,
    onCommit: @escaping (String) -> Void = { _ in }
  ) {
    committedText = initialText
    self.masksText = masksText
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
  func performFullQwertyKey(_ key: OrbitKeysFullQwertyKey) {
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

  func performFullQwertyTouchBackspace() {
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
    feedback: OrbitKeysHaptics.Feedback
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

  func makeRepeatTask(
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

  func schedulePressedFaceRelease(
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

  func resetTransientModeHolds() {
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

  func modeDidChange() {
    fullQwertySelection = OrbitKeysFullQwertyLayout.clampedPosition(
      fullQwertySelection,
      mode: mode
    )
    feedbackSequence += 1
    fullQwertyFeedbackSequence += 1
    haptics.play(.mode)
  }
}
