// DynamicColours.swift — Dynamic background colour and effects editor
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// MARK: - ThemePaletteEditorComponents

struct ThemePaletteEditorSnapshot: Equatable {
  let sharedPalette: ThemePalette
  let sharedCustomColor: SavedPaletteColor?
  let sharedMultiColor: ThemeMultiColorSelection
  let ribbonPalette: ThemePalette
  let ribbonCustomColor: SavedPaletteColor?
  let ribbonMultiColor: ThemeMultiColorSelection
  let particleSettings: DynamicParticleSettings
  let isPlayStation3XMBPresetExplicit: Bool
  let savedColorsJSON: String

  var visualState: ThemePaletteEditorVisualState {
    ThemePaletteEditorVisualState(
      sharedPalette: sharedPalette,
      sharedCustomColor: sharedCustomColor,
      sharedMultiColor: sharedMultiColor,
      ribbonPalette: ribbonPalette,
      ribbonCustomColor: ribbonCustomColor,
      ribbonMultiColor: ribbonMultiColor,
      particleSettings: particleSettings
    )
  }
}

struct ThemePaletteEditorVisualState: Equatable {
  let sharedPalette: ThemePalette
  let sharedCustomColor: SavedPaletteColor?
  let sharedMultiColor: ThemeMultiColorSelection
  let ribbonPalette: ThemePalette
  let ribbonCustomColor: SavedPaletteColor?
  let ribbonMultiColor: ThemeMultiColorSelection
  let particleSettings: DynamicParticleSettings
}

struct ThemePaletteEditorHeader: View {
  let canUndo: Bool
  let canRedo: Bool
  let cancel: () -> Void
  let undo: () -> Void
  let redo: () -> Void
  let apply: () -> Void

  var body: some View {
    HStack(spacing: 10) {
      Text("Colours")
        .font(.title2.weight(.bold))

      Spacer(minLength: 8)

      HStack(spacing: 2) {
        Button("Cancel", action: cancel)
          .padding(.horizontal, 7)

        Button(action: undo) {
          Image(systemName: "arrow.uturn.backward")
            .frame(width: 28, height: 32)
        }
        .disabled(!canUndo)
        .accessibilityLabel(Text("Undo"))
        .help(Text("Undo"))

        Button(action: redo) {
          Image(systemName: "arrow.uturn.forward")
            .frame(width: 28, height: 32)
        }
        .disabled(!canRedo)
        .accessibilityLabel(Text("Redo"))
        .help(Text("Redo"))

        Button("Apply", action: apply)
          .fontWeight(.semibold)
          .padding(.horizontal, 7)
      }
      .font(.subheadline.weight(.medium))
      .padding(.horizontal, 5)
      .frame(height: 42)
      .glassSurface(interactive: true, cornerRadius: 21)
      .buttonStyle(.plain)
    }
  }
}

struct ThemePaletteEditorSliderReadout: View {
  let title: String
  let value: String

  var body: some View {
    VStack {
      HStack {
        Spacer()
        HStack(spacing: 8) {
          Text(title)
            .foregroundStyle(.white.opacity(0.72))
          Text(value)
            .foregroundStyle(.white)
            .monospacedDigit()
        }
        .font(.caption.weight(.semibold))
        .lineLimit(1)
        .padding(.horizontal, 14)
        .frame(height: 36)
        .glassSurface(cornerRadius: 18)
      }
      Spacer()
    }
    .padding(18)
    .allowsHitTesting(false)
    .accessibilityElement(children: .combine)
  }
}

// MARK: - DynamicSettingsEditor

struct ThemePaletteEditor: View {
  @Environment(\.dismiss) private var dismiss
  @Environment(\.layoutDirection) private var layoutDirection

  @Binding var target: ThemePaletteTarget
  @Binding var sharedPalette: ThemePalette
  @Binding var sharedCustomColor: SavedPaletteColor?
  @Binding var sharedMultiColor: ThemeMultiColorSelection
  @Binding var ribbonPalette: ThemePalette
  @Binding var ribbonCustomColor: SavedPaletteColor?
  @Binding var ribbonMultiColor: ThemeMultiColorSelection
  @Binding var particleSettings: DynamicParticleSettings
  @Binding var isShowingBackgroundOnly: Bool
  @Binding var isPlayStation3XMBPresetExplicit: Bool
  let dynamicBackground: DynamicBackgroundStyle
  let onSaveAppearance: () -> Void

  @AppStorage("ARMSX2iOSDynamicSavedPaletteColors") private var savedColorsJSON = "[]"
  @State private var previewDebounceTask: Task<Void, Never>?
  @State private var backgroundPreviewTask: Task<Void, Never>?
  @State private var previewFadeTask: Task<Void, Never>?
  @State private var backgroundPreviewOpacity = 0.0
  @State private var lastThemeSnapshot: DynamicBackgroundTheme?
  @State private var pendingOriginalTheme: DynamicBackgroundTheme?
  @State private var pendingUpdatedTheme: DynamicBackgroundTheme?
  @State private var activeUpdatedTheme: DynamicBackgroundTheme?
  @State private var previewThemeSnapshot: DynamicBackgroundTheme?
  @State private var previewShowsUpdatedTheme = false
  @State private var activeSliderTitle: String?
  @State private var activeSliderValue: String?
  @State private var lastSliderInteractionEnd = Date.distantPast
  @State private var tabTransitionMovesForward = true
  @State private var scrollToTopRequest = 0
  @State private var undoSnapshots: [ThemePaletteEditorSnapshot] = []
  @State private var redoSnapshots: [ThemePaletteEditorSnapshot] = []
  @State private var initialEditorSnapshot: ThemePaletteEditorSnapshot?
  @State private var lastEditorSnapshot: ThemePaletteEditorSnapshot?
  @State private var pendingUndoSnapshot: ThemePaletteEditorSnapshot?
  @State private var historyCommitTask: Task<Void, Never>?
  @State private var isClosingEditor = false

  init(
    target: Binding<ThemePaletteTarget>,
    preferences: Binding<DynamicAppearancePreferences>,
    isShowingBackgroundOnly: Binding<Bool>,
    dynamicBackground: DynamicBackgroundStyle,
    onSaveAppearance: @escaping () -> Void
  ) {
    _target = target
    _sharedPalette = preferences.sharedPalette
    _sharedCustomColor = preferences.sharedCustomColor
    _sharedMultiColor = preferences.sharedMultiColor
    _ribbonPalette = preferences.ribbonPalette
    _ribbonCustomColor = preferences.ribbonCustomColor
    _ribbonMultiColor = preferences.ribbonMultiColor
    _particleSettings = preferences.particleSettings
    _isShowingBackgroundOnly = isShowingBackgroundOnly
    _isPlayStation3XMBPresetExplicit = preferences.isPlayStation3XMBPresetExplicit
    self.dynamicBackground = dynamicBackground
    self.onSaveAppearance = onSaveAppearance
  }

  var body: some View {
    editorSurface
      .preferredColorScheme(.dark)
      .presentationBackground(.clear)
      .presentationDragIndicator(isShowingBackgroundOnly ? .hidden : .visible)
      .onAppear(perform: prepareEditor)
      .onChange(of: currentEditorSnapshot) { oldSnapshot, newSnapshot in
        handleEditorSnapshotChange(from: oldSnapshot, to: newSnapshot)
      }
      .onDisappear(perform: tearDownEditor)
  }

  // The explicit erasure keeps the release runtime from decoding the entire sheet's
  // deeply nested SwiftUI type when the presentation bridge creates its host.
  private var editorSurface: AnyView {
    AnyView(
      editorNavigation
        .overlay {
          if isShowingBackgroundOnly {
            backgroundPreview
              .opacity(backgroundPreviewOpacity)
          }
        }
        .overlay {
          if isShowingBackgroundOnly {
            if let activeSliderTitle,
              let activeSliderValue
            {
              ThemePaletteEditorSliderReadout(
                title: activeSliderTitle,
                value: activeSliderValue
              )
              .opacity(backgroundPreviewOpacity)
            }
          }
        }
    )
  }

  private var editorNavigation: AnyView {
    AnyView(
      NavigationStack {
        editorScroll
      }
      .simultaneousGesture(tabSwipeGesture)
      .opacity(1 - backgroundPreviewOpacity)
      .allowsHitTesting(!isShowingBackgroundOnly || activeSliderTitle != nil)
      .environment(
        \.numberRowActivity,
        NumberRowActivity(update: updateSliderActivity)
      )
      // The editor packs a few hundred rows into a sheet; Form metrics would double its height.
      .numberRowStyle(.compact)
    )
  }

  private var editorScroll: AnyView {
    AnyView(
      ScrollViewReader { proxy in
        ScrollView {
          editorSections
        }
        .onChange(of: scrollToTopRequest) { _, _ in
          var transaction = Transaction(animation: nil)
          transaction.disablesAnimations = true
          withTransaction(transaction) {
            proxy.scrollTo("colours-editor-top", anchor: .top)
          }
        }
      }
      .background(
        Color.clear
          .glassSurface(cornerRadius: 0)
          .ignoresSafeArea()
      )
    )
  }

  private var editorSections: AnyView {
    AnyView(
      VStack(alignment: .leading, spacing: 20) {
        ThemePaletteEditorHeader(
          canUndo: canRevert,
          canRedo: !redoSnapshots.isEmpty,
          cancel: cancelChanges,
          undo: revertLastChange,
          redo: redoLastChange,
          apply: applyChanges
        )
        .id("colours-editor-top")

        Picker("Color target", selection: $target) {
          ForEach(ThemePaletteTarget.allCases) { target in
            Text(target.title).tag(target)
          }
        }
        .pickerStyle(.segmented)

        selectedTabSections
          .id(target)
          .transition(tabTransition)
      }
      .padding(18)
      .clipped()
    )
  }

  @ViewBuilder
  private var selectedTabSections: some View {
    if target == .particles {
      DynamicParticleSettingsControls(
        particleSettings: $particleSettings,
        isPlayStation3XMBPresetExplicit: $isPlayStation3XMBPresetExplicit,
        dynamicBackground: dynamicBackground,
        resetAllSettingsAndPalettes: resetDynamicBackgroundSettingsAndPalettes
      )
    } else {
      ThemePaletteControls(
        target: $target,
        sharedPalette: $sharedPalette,
        sharedCustomColor: $sharedCustomColor,
        sharedMultiColor: $sharedMultiColor,
        ribbonPalette: $ribbonPalette,
        ribbonCustomColor: $ribbonCustomColor,
        ribbonMultiColor: $ribbonMultiColor,
        particleSettings: $particleSettings,
        isPlayStation3XMBPresetExplicit: $isPlayStation3XMBPresetExplicit,
        savedColorsJSON: $savedColorsJSON,
        dynamicBackground: dynamicBackground,
        onSaveAppearance: onSaveAppearance
      )
    }
  }

  private var tabTransition: AnyTransition {
    let insertionEdge: Edge = tabTransitionMovesForward ? .trailing : .leading
    let removalEdge: Edge = tabTransitionMovesForward ? .leading : .trailing
    return .asymmetric(
      insertion: .move(edge: insertionEdge).combined(with: .opacity),
      removal: .move(edge: removalEdge).combined(with: .opacity)
    )
  }

  private var tabSwipeGesture: some Gesture {
    DragGesture(minimumDistance: 28)
      .onEnded(handleTabSwipe)
  }

  private func prepareEditor() {
    initialEditorSnapshot = currentEditorSnapshot
    lastThemeSnapshot = currentTheme
    lastEditorSnapshot = currentEditorSnapshot
  }

  private func handleTabSwipe(_ value: DragGesture.Value) {
    guard
      !isShowingBackgroundOnly,
      activeSliderTitle == nil,
      Date().timeIntervalSince(lastSliderInteractionEnd) > 0.2
    else {
      return
    }

    let horizontalDistance = value.translation.width
    let verticalDistance = value.translation.height
    guard
      abs(horizontalDistance) >= 64,
      abs(horizontalDistance) > abs(verticalDistance) * 1.25
    else {
      return
    }

    let isRTL = layoutDirection == .rightToLeft
    let forward = isRTL ? horizontalDistance > 0 : horizontalDistance < 0
    moveToAdjacentTab(forward: forward)
  }

  private func moveToAdjacentTab(forward: Bool) {
    let tabs = ThemePaletteTarget.allCases
    guard let currentIndex = tabs.firstIndex(of: target) else { return }
    let nextIndex = currentIndex + (forward ? 1 : -1)
    guard tabs.indices.contains(nextIndex) else { return }

    tabTransitionMovesForward = forward
    withAnimation(.easeInOut(duration: 0.28)) {
      target = tabs[nextIndex]
      scrollToTopRequest += 1
    }
  }

  private var backgroundPreview: AnyView {
    AnyView(
      ZStack {
        previewBackgroundLayer
        previewDismissLayer
      }
    )
  }

  private var previewBackgroundLayer: AnyView {
    AnyView(
      ResolutionAwareDynamicBackgroundContentView(
        style: dynamicBackground,
        theme: previewTheme
      )
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .ignoresSafeArea()
        .clipped()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    )
  }

  private var previewDismissLayer: AnyView {
    AnyView(
      Color.clear
        .contentShape(Rectangle())
        .ignoresSafeArea()
        .allowsHitTesting(activeSliderTitle == nil)
        .onTapGesture(perform: endBackgroundPreview)
        .accessibilityLabel(Text("Show controls"))
    )
  }

  private var currentTheme: DynamicBackgroundTheme {
    DynamicBackgroundTheme(
      sharedPalette: sharedPalette,
      sharedCustomColor: sharedCustomColor,
      sharedMultiColor: sharedMultiColor,
      ribbonPalette: ribbonPalette,
      ribbonCustomColor: ribbonCustomColor,
      ribbonMultiColor: ribbonMultiColor,
      particleSettings: particleSettings
    )
  }

  private var previewTheme: DynamicBackgroundTheme {
    previewThemeSnapshot ?? currentTheme
  }

  private func scheduleBackgroundPreview() {
    previewDebounceTask?.cancel()

    let beforeDuration = particleSettings.backgroundPreviewBeforeApplyingDuration
    let afterDuration = particleSettings.backgroundPreviewAfterApplyingDuration
    guard beforeDuration > 0 || afterDuration > 0 else {
      endBackgroundPreview()
      lastThemeSnapshot = currentTheme
      return
    }

    let updatedTheme = currentTheme
    let previousTheme = lastThemeSnapshot ?? updatedTheme
    lastThemeSnapshot = updatedTheme

    if isShowingBackgroundOnly {
      previewFadeTask?.cancel()
      activeUpdatedTheme = updatedTheme
      pendingUpdatedTheme = updatedTheme

      if backgroundPreviewOpacity < 1 {
        withAnimation(.easeInOut(duration: 0.1)) {
          backgroundPreviewOpacity = 1
        }
      }

      if previewShowsUpdatedTheme {
        var themeTransaction = Transaction(animation: nil)
        themeTransaction.disablesAnimations = true
        withTransaction(themeTransaction) {
          previewThemeSnapshot = updatedTheme
        }
        restartBackgroundPreviewHold(afterDuration: afterDuration)
      }
      return
    }

    backgroundPreviewTask?.cancel()
    if pendingOriginalTheme == nil {
      pendingOriginalTheme = previousTheme
    }
    pendingUpdatedTheme = updatedTheme

    previewDebounceTask = Task { @MainActor in
      await Task.yield()
      guard !Task.isCancelled else { return }
      beginBackgroundPreview(
        beforeDuration: beforeDuration,
        afterDuration: afterDuration
      )
    }
  }

  private func beginBackgroundPreview(
    beforeDuration: Double,
    afterDuration: Double
  ) {
    let originalTheme = pendingOriginalTheme ?? lastThemeSnapshot ?? currentTheme
    previewFadeTask?.cancel()
    activeUpdatedTheme = pendingUpdatedTheme ?? currentTheme
    pendingOriginalTheme = nil
    pendingUpdatedTheme = nil
    previewShowsUpdatedTheme = beforeDuration <= 0
    previewThemeSnapshot =
      beforeDuration > 0
      ? originalTheme
      : activeUpdatedTheme ?? currentTheme

    backgroundPreviewTask?.cancel()
    var visibilityTransaction = Transaction(animation: nil)
    visibilityTransaction.disablesAnimations = true
    withTransaction(visibilityTransaction) {
      backgroundPreviewOpacity = 0
      isShowingBackgroundOnly = true
    }
    withAnimation(.easeInOut(duration: 0.1)) {
      backgroundPreviewOpacity = 1
    }

    backgroundPreviewTask = Task { @MainActor in
      if beforeDuration > 0 {
        try? await Task.sleep(for: .seconds(beforeDuration))
        guard !Task.isCancelled else { return }

        var themeTransaction = Transaction(animation: nil)
        themeTransaction.disablesAnimations = true
        withTransaction(themeTransaction) {
          previewShowsUpdatedTheme = true
          previewThemeSnapshot = activeUpdatedTheme ?? currentTheme
        }
      }

      guard activeSliderTitle == nil else { return }
      if afterDuration > 0 {
        try? await Task.sleep(for: .seconds(afterDuration))
        guard !Task.isCancelled else { return }
      }
      endBackgroundPreview()
    }
  }

  private func restartBackgroundPreviewHold(afterDuration: Double) {
    backgroundPreviewTask?.cancel()
    guard activeSliderTitle == nil else { return }

    backgroundPreviewTask = Task { @MainActor in
      if afterDuration > 0 {
        try? await Task.sleep(for: .seconds(afterDuration))
        guard !Task.isCancelled else { return }
      }
      endBackgroundPreview()
    }
  }

  private func updateSliderActivity(
    title: String,
    value: String,
    isEditing: Bool
  ) {
    if isEditing {
      let beginsInteraction = activeSliderTitle == nil
      activeSliderTitle = title
      activeSliderValue = value
      if beginsInteraction {
        beginSliderBackgroundPreviewIfNeeded()
      }
      return
    }

    guard activeSliderTitle == title else { return }
    activeSliderTitle = nil
    activeSliderValue = nil
    lastSliderInteractionEnd = Date()

    if isShowingBackgroundOnly, previewShowsUpdatedTheme {
      restartBackgroundPreviewHold(
        afterDuration: particleSettings.backgroundPreviewAfterApplyingDuration
      )
    }
  }

  private func beginSliderBackgroundPreviewIfNeeded() {
    guard !isShowingBackgroundOnly else { return }

    let beforeDuration = particleSettings.backgroundPreviewBeforeApplyingDuration
    let afterDuration = particleSettings.backgroundPreviewAfterApplyingDuration
    guard beforeDuration > 0 || afterDuration > 0 else { return }

    previewDebounceTask?.cancel()
    backgroundPreviewTask?.cancel()
    let unchangedTheme = lastThemeSnapshot ?? currentTheme
    pendingOriginalTheme = unchangedTheme
    pendingUpdatedTheme = currentTheme
    beginBackgroundPreview(
      beforeDuration: beforeDuration,
      afterDuration: afterDuration
    )
  }

  private func endBackgroundPreview() {
    previewDebounceTask?.cancel()
    backgroundPreviewTask?.cancel()
    previewFadeTask?.cancel()
    pendingOriginalTheme = nil
    pendingUpdatedTheme = nil
    withAnimation(.easeInOut(duration: 0.2)) {
      backgroundPreviewOpacity = 0
    }

    previewFadeTask = Task { @MainActor in
      try? await Task.sleep(for: .milliseconds(100))
      guard !Task.isCancelled, backgroundPreviewOpacity == 0 else { return }

      activeUpdatedTheme = nil
      previewThemeSnapshot = nil
      previewShowsUpdatedTheme = false
      var visibilityTransaction = Transaction(animation: nil)
      visibilityTransaction.disablesAnimations = true
      withTransaction(visibilityTransaction) {
        isShowingBackgroundOnly = false
      }
    }
  }

  private var currentEditorSnapshot: ThemePaletteEditorSnapshot {
    ThemePaletteEditorSnapshot(
      sharedPalette: sharedPalette,
      sharedCustomColor: sharedCustomColor,
      sharedMultiColor: sharedMultiColor,
      ribbonPalette: ribbonPalette,
      ribbonCustomColor: ribbonCustomColor,
      ribbonMultiColor: ribbonMultiColor,
      particleSettings: particleSettings,
      isPlayStation3XMBPresetExplicit: isPlayStation3XMBPresetExplicit,
      savedColorsJSON: savedColorsJSON
    )
  }

  private var canRevert: Bool {
    pendingUndoSnapshot != nil || !undoSnapshots.isEmpty
  }

  private func handleEditorSnapshotChange(
    from oldSnapshot: ThemePaletteEditorSnapshot,
    to newSnapshot: ThemePaletteEditorSnapshot
  ) {
    guard !isClosingEditor else { return }
    recordEditorChange()
    guard oldSnapshot.visualState != newSnapshot.visualState else { return }

    if isOnlyBackgroundPreviewTimingChange(from: oldSnapshot, to: newSnapshot) {
      lastThemeSnapshot = currentTheme
      return
    }

    scheduleBackgroundPreview()
  }

  private func isOnlyBackgroundPreviewTimingChange(
    from oldSnapshot: ThemePaletteEditorSnapshot,
    to newSnapshot: ThemePaletteEditorSnapshot
  ) -> Bool {
    guard
      oldSnapshot.sharedPalette == newSnapshot.sharedPalette,
      oldSnapshot.sharedCustomColor == newSnapshot.sharedCustomColor,
      oldSnapshot.sharedMultiColor == newSnapshot.sharedMultiColor,
      oldSnapshot.ribbonPalette == newSnapshot.ribbonPalette,
      oldSnapshot.ribbonCustomColor == newSnapshot.ribbonCustomColor,
      oldSnapshot.ribbonMultiColor == newSnapshot.ribbonMultiColor,
      oldSnapshot.particleSettings.backgroundPreviewBeforeApplyingDuration
        != newSnapshot.particleSettings.backgroundPreviewBeforeApplyingDuration
        || oldSnapshot.particleSettings.backgroundPreviewAfterApplyingDuration
          != newSnapshot.particleSettings.backgroundPreviewAfterApplyingDuration
    else {
      return false
    }

    var normalizedSettings = oldSnapshot.particleSettings
    normalizedSettings.backgroundPreviewBeforeApplyingDuration =
      newSnapshot.particleSettings.backgroundPreviewBeforeApplyingDuration
    normalizedSettings.backgroundPreviewAfterApplyingDuration =
      newSnapshot.particleSettings.backgroundPreviewAfterApplyingDuration
    return normalizedSettings == newSnapshot.particleSettings
  }

  private func tearDownEditor() {
    if !isClosingEditor {
      onSaveAppearance()
      isClosingEditor = true
    }

    previewDebounceTask?.cancel()
    backgroundPreviewTask?.cancel()
    previewFadeTask?.cancel()
    historyCommitTask?.cancel()
    pendingOriginalTheme = nil
    pendingUpdatedTheme = nil
    activeUpdatedTheme = nil
    previewThemeSnapshot = nil
    previewShowsUpdatedTheme = false
    activeSliderTitle = nil
    activeSliderValue = nil
    backgroundPreviewOpacity = 0
    isShowingBackgroundOnly = false
  }

  private func recordEditorChange() {
    let updatedSnapshot = currentEditorSnapshot
    let previousSnapshot = lastEditorSnapshot ?? updatedSnapshot
    guard updatedSnapshot != previousSnapshot else { return }

    if pendingUndoSnapshot == nil {
      pendingUndoSnapshot = previousSnapshot
    }
    lastEditorSnapshot = updatedSnapshot
    redoSnapshots.removeAll()

    historyCommitTask?.cancel()
    historyCommitTask = Task { @MainActor in
      try? await Task.sleep(for: .milliseconds(300))
      guard !Task.isCancelled else { return }
      commitPendingUndoSnapshot()
    }
  }

  private func commitPendingUndoSnapshot() {
    guard let snapshot = pendingUndoSnapshot else { return }
    pendingUndoSnapshot = nil
    undoSnapshots.append(snapshot)
    if undoSnapshots.count > 64 {
      undoSnapshots.removeFirst(undoSnapshots.count - 64)
    }
  }

  private func revertLastChange() {
    historyCommitTask?.cancel()
    commitPendingUndoSnapshot()
    guard let snapshot = undoSnapshots.popLast() else { return }

    redoSnapshots.append(currentEditorSnapshot)
    applyEditorSnapshot(snapshot)
  }

  private func redoLastChange() {
    historyCommitTask?.cancel()
    guard let snapshot = redoSnapshots.popLast() else { return }

    undoSnapshots.append(currentEditorSnapshot)
    applyEditorSnapshot(snapshot)
  }

  // restoringSavedColors is false only for Cancel. Undo and redo do want the swatch list
  // to move with everything else; Cancel does not, because saving a swatch is its own
  // deliberate act and rolling it back reads as losing work.
  private func applyEditorSnapshot(
    _ snapshot: ThemePaletteEditorSnapshot,
    restoringSavedColors: Bool = true
  ) {
    pendingUndoSnapshot = nil
    sharedPalette = snapshot.sharedPalette
    sharedCustomColor = snapshot.sharedCustomColor
    sharedMultiColor = snapshot.sharedMultiColor
    ribbonPalette = snapshot.ribbonPalette
    ribbonCustomColor = snapshot.ribbonCustomColor
    ribbonMultiColor = snapshot.ribbonMultiColor
    particleSettings = snapshot.particleSettings
    isPlayStation3XMBPresetExplicit = snapshot.isPlayStation3XMBPresetExplicit
    if restoringSavedColors {
      savedColorsJSON = snapshot.savedColorsJSON
    }
    lastEditorSnapshot = snapshot
  }

  private func resetDynamicBackgroundSettingsAndPalettes() {
    let defaults = DynamicAppearancePreferences.standard
    sharedPalette = defaults.sharedPalette
    sharedCustomColor = defaults.sharedCustomColor
    sharedMultiColor = defaults.sharedMultiColor
    ribbonPalette = defaults.ribbonPalette
    ribbonCustomColor = defaults.ribbonCustomColor
    ribbonMultiColor = defaults.ribbonMultiColor
    particleSettings = defaults.particleSettings
    isPlayStation3XMBPresetExplicit = defaults.isPlayStation3XMBPresetExplicit
  }

  private func cancelChanges() {
    isClosingEditor = true
    endBackgroundPreview()
    if let initialEditorSnapshot {
      applyEditorSnapshot(initialEditorSnapshot, restoringSavedColors: false)
      // Restoring the bindings is not enough. Saving a swatch mid session calls
      // onSaveAppearance, which persists the whole preferences struct, so everything
      // edited before that point is already committed and would outlive Cancel.
      onSaveAppearance()
    }
    dismiss()
  }

  private func applyChanges() {
    isClosingEditor = true
    endBackgroundPreview()
    onSaveAppearance()
    dismiss()
  }
}
