// OrbitKeysKeyboardHost.swift — embeds both OrbitKeys keyboards in ARMSX2.
// The standalone demo app and its Dynamic Background picker are intentionally
// replaced by this feature-scoped presentation host.

import SwiftUI
import UIKit

@MainActor
struct OrbitKeysKeyboardView: View {
  let title: String
  let game: ISOEntry?
  let onCommit: (String) -> Void
  let onCancel: () -> Void

  @StateObject private var model: OrbitKeysModel
  @State private var hasOpened = false
  @State private var hasCompleted = false
  @Environment(\.menuControllerInputRouter) private var controllerInput

  init(
    title: String,
    initialText: String,
    game: ISOEntry? = nil,
    startsInNormalKeyboard: Bool = false,
    masksText: Bool = false,
    onCommit: @escaping (String) -> Void,
    onCancel: @escaping () -> Void
  ) {
    self.title = title
    self.game = game
    self.onCommit = onCommit
    self.onCancel = onCancel
    _model = StateObject(
      wrappedValue: OrbitKeysModel(
        initialText: initialText,
        startsControllerDiscovery: true,
        startsInNormalKeyboard: startsInNormalKeyboard,
        masksText: masksText
      )
    )
  }

  var body: some View {
    GeometryReader { proxy in
      let isWide = proxy.size.width > proxy.size.height * 1.18
      let bookFold = isWide ? proxy.activeFold(book: true) : nil
      let tabletopFold = isWide ? nil : proxy.activeFold(book: false)
      let fieldRowTop = tabletopFold.map {
        $0.minY - 8 - OrbitKeysEditorPanel.splitFieldRowHeight
      }
      let showsGamePreview = game != nil
      let titleHeight = showsGamePreview ? renameTitleHeight : 0
      let previewTop = max(proxy.safeAreaInsets.top, 14) + titleHeight
      let previewHeight: CGFloat =
        !showsGamePreview || isWide
        ? 0
        : fieldRowTop.map { max(118, $0 - 10 - previewTop) }
          ?? gamePreviewHeight(
            in: proxy.size,
            safeAreaInsets: proxy.safeAreaInsets,
            titleHeight: titleHeight
          )
      let presentationSpacing: CGFloat = previewHeight > 0 ? 10 : 0

      ZStack {
        // The cover is see-through, so without a backdrop the panel that opened the keyboard
        // shows between its parts and its text bar lands on that panel's own field.
        Color.black.opacity(0.82)
          .ignoresSafeArea()
          .contentShape(Rectangle())
          .onTapGesture(perform: model.commitAndClose)

        if isWide {
          wideKeyboardLayout(
            availableSize: proxy.size,
            bookFold: bookFold
          )
          .padding(.horizontal, 18)
          .padding(.vertical, 4)
          .frame(
            maxWidth: .infinity,
            maxHeight: .infinity,
            alignment: model.usesFullQwerty ? .bottom : .center
          )
          .offset(y: model.usesFullQwerty ? 0 : 10)
        } else {
          VStack(spacing: presentationSpacing) {
            if let game {
              gamePreview(game, height: previewHeight)
            }

            portraitKeyboardLayout(availableSize: proxy.size, tabletopFold: tabletopFold)
          }
          .padding(.horizontal, 14)
          .padding(
            .top,
            fieldRowTop.map { $0 - previewHeight - presentationSpacing } ?? previewTop
          )
          .padding(.bottom, max(proxy.safeAreaInsets.bottom, 10))
          .frame(
            maxWidth: .infinity,
            maxHeight: .infinity,
            alignment: fieldRowTop == nil ? .bottom : .top
          )
        }

        if showsGamePreview {
          EmbeddedMenuLargeTitle(title: title)
            .padding(.top, proxy.safeAreaInsets.top)
            .frame(
              maxWidth: .infinity,
              maxHeight: .infinity,
              alignment: .topLeading
            )
            .allowsHitTesting(false)
            .zIndex(10)
        }

        if model.isKeyboardSettingsVisible {
          settingsLayer(
            isWide: isWide,
            availableSize: proxy.size,
            safeAreaInsets: proxy.safeAreaInsets,
            fold: bookFold ?? tabletopFold
          )
          .transition(.opacity)
          .zIndex(20)
        }

        if let message = model.toastMessage {
          toast(message, safeAreaTop: proxy.safeAreaInsets.top)
            .transition(.move(edge: .top).combined(with: .opacity))
            .zIndex(30)
        }
      }
      .animation(
        .spring(duration: 0.42, bounce: 0.2),
        value: model.usesFullQwerty
      )
      .animation(
        .easeInOut(duration: 0.22),
        value: model.isKeyboardSettingsVisible
      )
      .animation(
        .spring(duration: 0.38, bounce: 0.26),
        value: model.toastMessage
      )
    }
    .preferredColorScheme(.dark)
    .tint(accentColour)
    .onAppear(perform: openKeyboard)
    .onChange(of: model.isKeyboardVisible, handleVisibilityChange)
    .onDisappear(perform: releaseResources)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(title)
  }

  @Environment(\.uiAccentColour) private var accentColour

  private var usesTabletLayout: Bool {
    usesLargeLayout()
  }

  private func gamePreviewHeight(
    in availableSize: CGSize,
    safeAreaInsets: EdgeInsets,
    titleHeight: CGFloat
  ) -> CGFloat {
    guard game != nil else { return 0 }
    let verticalInsets = max(safeAreaInsets.top, 14)
      + max(safeAreaInsets.bottom, 10)
    let keyboardReserve: CGFloat
    if model.usesFullQwerty {
      let keyboardHeight = min(
        usesTabletLayout ? 420 : 206,
        max(
          usesTabletLayout ? 320 : 164,
          availableSize.height * (usesTabletLayout ? 0.36 : 0.3)
        )
      )
      keyboardReserve = keyboardHeight + (usesTabletLayout ? 150 : 82)
    } else {
      let gridSide = min(
        usesTabletLayout ? 740 : 520,
        max(220, availableSize.width - 28)
      )
      keyboardReserve = gridSide + (usesTabletLayout ? 150 : 82)
    }

    let availableAboveKeyboard = availableSize.height
      - verticalInsets
      - titleHeight
      - keyboardReserve
      - 10
    let maximumHeightForWidth = max(
      118,
      (availableSize.width - 28) * 1.42
    )
    return min(
      maximumHeightForWidth,
      max(118, availableAboveKeyboard)
    )
  }

  private func gamePreview(_ game: ISOEntry, height: CGFloat) -> some View {
    let compact = height < 125
    let titleHeight: CGFloat = compact ? 18 : 34
    let verticalPadding: CGFloat = compact ? 5 : 8
    let spacing: CGFloat = compact ? 3 : 6
    let coverHeight = max(
      48,
      height - titleHeight - spacing - (verticalPadding * 2)
    )
    let coverWidth = coverHeight * (2.0 / 3.0)
    let cardWidth = max(
      coverWidth + (compact ? 12 : 18),
      compact ? 104 : 150
    )

    return VStack(spacing: spacing) {
      CoverThumbnailView(
        gameName: game.name,
        coverURL: game.coverURL,
        coverSignature: game.coverSignature,
        width: coverWidth,
        height: coverHeight
      )

      Text(model.text.isEmpty ? game.displayName : model.text)
        .font(compact ? .caption2.weight(.semibold) : .caption.weight(.semibold))
        .foregroundStyle(.primary)
        .lineLimit(compact ? 1 : 2)
        .multilineTextAlignment(.center)
        .frame(width: cardWidth - 12, height: titleHeight, alignment: .top)
    }
    .padding(.vertical, verticalPadding)
    .frame(width: cardWidth, height: height, alignment: .top)
    .glassSurface(clear: true, cornerRadius: compact ? 14 : 18)
    .allowsHitTesting(false)
    .accessibilityElement(children: .combine)
    .accessibilityLabel(game.displayName)
  }

  @ViewBuilder
  private func wideKeyboardLayout(
    availableSize: CGSize,
    bookFold: CGRect?
  ) -> some View {
    let width = availableSize.width
    let edge: CGFloat = 18
    let foldMargin: CGFloat = 8
    let foldHalf = bookFold.map { fold -> CGFloat in
      min(fold.minX - foldMargin - edge, width - edge - (fold.maxX + foldMargin))
    }
    let leadingPaneShift: CGFloat = bookFold.map { fold -> CGFloat in
      (edge + fold.minX - foldMargin) / 2 - width / 2
    } ?? 0
    let trailingPaneShift: CGFloat = bookFold.map { fold -> CGFloat in
      (fold.maxX + foldMargin + width - edge) / 2 - width / 2
    } ?? 0

    if model.usesFullQwerty {
      let keyboardWidth = min(
        usesTabletLayout ? 1_280 : 760,
        max(430, availableSize.width - (usesTabletLayout ? 32 : 36))
      )
      let keyboardHeight = min(
        usesTabletLayout ? 400 : 196,
        max(
          usesTabletLayout ? 300 : 166,
          availableSize.height * (usesTabletLayout ? 0.40 : 0.42)
        )
      )
      let upperRegionHeight = max(0, availableSize.height - keyboardHeight - 12)
      let upperContentHeight = max(0, upperRegionHeight - renameTitleHeight)
      let previewHeight = landscapePreviewHeight(
        availableHeight: upperContentHeight,
        maximumHeight: 390
      )

      let upperLayout = bookFold == nil
        ? AnyLayout(HStackLayout(spacing: usesTabletLayout ? 18 : 12))
        : AnyLayout(ZStackLayout())

      ZStack(alignment: .bottom) {
        fullQwertyDeck(
          width: foldHalf.map { $0 * 2 + (bookFold?.width ?? 0) + foldMargin * 2 } ?? keyboardWidth,
          height: keyboardHeight,
          compact: !usesTabletLayout,
          splitGap: bookFold.map { $0.width + foldMargin * 2 } ?? 0
        )
        .offset(x: bookFold.map { $0.midX - availableSize.width / 2 } ?? 0)

        upperLayout {
          if let game, previewHeight > 0 {
            gamePreview(game, height: previewHeight)
              .offset(x: trailingPaneShift)
          }

          editor(compact: !usesTabletLayout)
            .frame(
              maxWidth: bookFold.map { $0.minX - foldMargin - edge }
                ?? min(usesTabletLayout ? 760 : 620, keyboardWidth)
            )
            .offset(x: leadingPaneShift)
        }
        .frame(maxWidth: keyboardWidth)
        .frame(height: upperContentHeight, alignment: .center)
        .padding(.top, renameTitleHeight)
        .padding(.bottom, keyboardHeight + 8)
      }
      .frame(
        maxWidth: usesTabletLayout
          ? min(1_280, max(800, availableSize.width - 32))
          : 800,
        maxHeight: .infinity,
        alignment: .bottom
      )
    } else {
      let availableContentWidth = min(
        usesTabletLayout ? 1_320 : 980,
        max(0, availableSize.width - 36)
      )
      let minimumEditorWidth: CGFloat = usesTabletLayout ? 360 : 290
      let maximumGridSide = max(
        220,
        availableContentWidth - minimumEditorWidth - 16
      )
      let gridSide = min(
        usesTabletLayout ? 760 : 560,
        max(220, min(availableSize.height - 8, maximumGridSide))
      )
      let editorColumnWidth = min(
        usesTabletLayout ? 520 : 410,
        max(minimumEditorWidth, availableContentWidth - gridSide - 16)
      )
      let previewHeight = landscapePreviewHeight(
        availableHeight: gridSide - 88 - renameTitleHeight,
        maximumHeight: 390
      )

      let layout = bookFold == nil
        ? AnyLayout(HStackLayout(alignment: .center, spacing: 16))
        : AnyLayout(ZStackLayout())

      layout {
        VStack(spacing: 8) {
          if let game, previewHeight > 0 {
            gamePreview(game, height: previewHeight)
          }

          editor(compact: false)
        }
        .frame(
          width: bookFold.map { $0.minX - foldMargin - edge } ?? editorColumnWidth,
          height: max(0, gridSide - renameTitleHeight),
          alignment: .center
        )
        .padding(.top, renameTitleHeight)
        .offset(x: leadingPaneShift)

        orbitKeysDeck(
          side: foldHalf.map { sideKeysFit(min(740, $0, availableSize.height - 8)) } ?? gridSide
        )
        .offset(x: trailingPaneShift)
      }
      .frame(
        maxWidth: usesTabletLayout ? 1_320 : 980,
        alignment: .center
      )
    }
  }

  private func landscapePreviewHeight(
    availableHeight: CGFloat,
    maximumHeight: CGFloat
  ) -> CGFloat {
    guard game != nil, availableHeight >= 104 else { return 0 }
    return min(maximumHeight, availableHeight)
  }

  /// Matches the fixed footprint of `EmbeddedMenuLargeTitle`, which is also
  /// used by the Settings root. Keeping this space outside the keyboard deck
  /// prevents the title from changing either keyboard's established size.
  private var renameTitleHeight: CGFloat {
    game == nil ? 0 : (usesTabletLayout ? 64 : 52)
  }

  @ViewBuilder
  private func portraitKeyboardLayout(
    availableSize: CGSize,
    tabletopFold: CGRect?
  ) -> some View {
    let rowGap = tabletopFold.map { $0.height + 16 }

    if model.usesFullQwerty {
      let keyboardWidth = min(
        usesTabletLayout ? 1_100 : 700,
        max(292, availableSize.width - 28)
      )
      let keyboardHeight = min(
        usesTabletLayout ? 420 : 206,
        max(
          usesTabletLayout ? 320 : 164,
          availableSize.height * (usesTabletLayout ? 0.36 : 0.3)
        )
      )

      VStack(spacing: rowGap == nil ? 8 : 6) {
        editor(compact: false, rowGap: rowGap)
          .frame(maxWidth: usesTabletLayout ? 900 : 620)
        fullQwertyDeck(
          width: keyboardWidth,
          height: keyboardHeight,
          compact: false
        )
      }
      .frame(maxWidth: .infinity, alignment: .center)
    } else {
      let gridSide = min(
        usesTabletLayout ? 740 : 520,
        max(220, availableSize.width - 28)
      )
      let tabletopGridSide = tabletopFold.map {
        max(0, sideKeysFit(availableSize.height - 8 - ($0.maxY + 8) - OrbitKeysEditorPanel.splitChipsRowHeight - 6))
      }

      let side = min(gridSide, tabletopGridSide ?? gridSide)

      VStack(spacing: 6) {
        editor(compact: false, rowGap: rowGap)
          .frame(maxWidth: usesTabletLayout ? 900 : 620)
        orbitKeysDeck(side: side)
          // The fit above leaves room for the circles' reach; spend it above and below the square.
          .padding(.vertical, tabletopGridSide == nil ? 0 : OrbitKeysGridView.keyOverhang(gridSide: side))
      }
      .frame(maxWidth: usesTabletLayout ? 960 : 680)
    }
  }

  private func editor(compact: Bool, rowGap: CGFloat? = nil) -> some View {
    OrbitKeysEditorPanel(
      model: model,
      compact: compact,
      usesExpandedMetrics: usesTabletLayout,
      onSettingsTap: model.toggleKeyboardSettings,
      rowGap: rowGap
    )
  }

  /// The largest grid side whose side keys' circles, reaching past the square, stay inside `fit`.
  private func sideKeysFit(_ fit: CGFloat) -> CGFloat {
    (0..<3).reduce(fit) { side, _ in fit - 2 * OrbitKeysGridView.keyOverhang(gridSide: side) }
  }

  private func orbitKeysDeck(side: CGFloat) -> some View {
    OrbitKeysGridView(model: model)
      .frame(width: side, height: side)
      .contentShape(Rectangle())
      .onTapGesture {}
      .fixedSize()
  }

  private func fullQwertyDeck(
    width: CGFloat,
    height: CGFloat,
    compact: Bool,
    splitGap: CGFloat = 0
  ) -> some View {
    OrbitKeysFullQwerty(model: model, compact: compact, splitGap: splitGap)
      .frame(width: width, height: height)
      .contentShape(Rectangle())
      .onTapGesture {}
      .transition(.scale(scale: 0.94).combined(with: .opacity))
  }

  private func settingsLayer(
    isWide: Bool,
    availableSize: CGSize,
    safeAreaInsets: EdgeInsets,
    fold: CGRect?
  ) -> some View {
    let sidePadding = isWide ? CGFloat(16) : CGFloat(14)
    let topPadding = max(safeAreaInsets.top + 6, 10)
    let bottomPadding = max(safeAreaInsets.bottom + 10, 12)
    let availableHeight = max(
      260,
      availableSize.height - topPadding - bottomPadding
    )
    var panelWidth = isWide
      ? min(410, max(350, availableSize.width * 0.38))
      : min(330, max(280, availableSize.width * 0.78))
    var panelHeight = isWide
      ? min(320, availableHeight * 0.86)
      : min(290, availableHeight * 0.5)
    if let fold, isWide {
      panelWidth = min(panelWidth, fold.minX - 8 - sidePadding)
    } else if let fold {
      panelHeight = min(panelHeight, fold.minY - 8 - topPadding)
    }

    return ZStack {
      Color.black.opacity(0.28)
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .onTapGesture(perform: model.closeKeyboardSettings)

      OrbitKeysSettingsPanel(
        model: model,
        onClose: model.closeKeyboardSettings
      )
      .frame(width: panelWidth, height: panelHeight)
      .padding(.top, isWide ? 0 : topPadding)
      .padding(.horizontal, sidePadding)
      .frame(
        maxWidth: .infinity,
        maxHeight: .infinity,
        alignment: isWide ? .leading : .top
      )
      .shadow(color: .black.opacity(0.24), radius: 18, y: 10)
    }
  }

  private func toast(_ message: String, safeAreaTop: CGFloat) -> some View {
    Label(
      message,
      systemImage: message == "Text inserted"
        ? "checkmark.circle.fill"
        : "sparkles"
    )
    .font(.subheadline.weight(.bold))
    .foregroundStyle(message == "Text inserted" ? .green : .orange)
    .padding(.horizontal, 16)
    .frame(height: 42)
    .orbitKeysGlassSurface(
      tint: (message == "Text inserted" ? Color.green : .orange).opacity(0.18),
      cornerRadius: 21
    )
    .shadow(color: .black.opacity(0.28), radius: 16, y: 8)
    .padding(.top, max(safeAreaTop + 42, 54))
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    .allowsHitTesting(false)
  }

  private func openKeyboard() {
    guard !hasOpened else { return }
    hasOpened = true
    setNavigationCaptured(true)
    model.haptics.router = controllerInput
    model.openKeyboard()
  }

  // The keyboard reads the pad itself; this keeps the router's copy from the
  // screen underneath. It outranks the root alert, so it goes when the keyboard closes.
  private func setNavigationCaptured(_ captured: Bool) {
    controllerInput?.setNavigationCaptured(
      captured,
      owner: MenuControllerNavigationCaptureOwner.orbitKeysKeyboard,
      priority: 1_100
    )
  }

  private func handleVisibilityChange(
    _ oldValue: Bool,
    _ isVisible: Bool
  ) {
    if !isVisible { setNavigationCaptured(false) }
    guard hasOpened, oldValue, !isVisible, !hasCompleted else { return }
    hasCompleted = true
    if let submitted = model.lastSubmittedText {
      onCommit(submitted)
    } else {
      onCancel()
    }
  }

  private func releaseResources() {
    setNavigationCaptured(false)
    if !hasCompleted {
      hasCompleted = true
      onCancel()
    }
    model.releaseKeyboardSessionResources()
  }
}
