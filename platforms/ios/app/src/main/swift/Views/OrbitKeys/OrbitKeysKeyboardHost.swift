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

  init(
    title: String,
    initialText: String,
    game: ISOEntry? = nil,
    startsInNormalKeyboard: Bool = false,
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
        startsInNormalKeyboard: startsInNormalKeyboard
      )
    )
  }

  var body: some View {
    GeometryReader { proxy in
      let isWide = proxy.size.width > proxy.size.height * 1.18
      let showsGamePreview = game != nil
      let titleHeight = showsGamePreview ? renameTitleHeight : 0
      let previewHeight =
        showsGamePreview && !isWide
        ? gamePreviewHeight(
            in: proxy.size,
            safeAreaInsets: proxy.safeAreaInsets,
            titleHeight: titleHeight
          )
        : 0
      let presentationSpacing: CGFloat = previewHeight > 0 ? 10 : 0

      ZStack {
        Color.clear
          .ignoresSafeArea()
          .contentShape(Rectangle())
          .onTapGesture(perform: model.commitAndClose)

        if isWide {
          wideKeyboardLayout(
            availableSize: proxy.size
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

            keyboardLayout(
              isWide: false,
              availableSize: proxy.size
            )
          }
          .padding(.horizontal, 14)
          .padding(
            .top,
            max(proxy.safeAreaInsets.top, 14) + titleHeight
          )
          .padding(.bottom, max(proxy.safeAreaInsets.bottom, 10))
          .frame(
            maxWidth: .infinity,
            maxHeight: .infinity,
            alignment: .bottom
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
            safeAreaInsets: proxy.safeAreaInsets
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
    UIDevice.current.userInterfaceIdiom == .pad
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
  private func keyboardLayout(
    isWide: Bool,
    availableSize: CGSize
  ) -> some View {
    if isWide {
      wideKeyboardLayout(availableSize: availableSize)
    } else {
      portraitKeyboardLayout(availableSize: availableSize)
    }
  }

  @ViewBuilder
  private func wideKeyboardLayout(availableSize: CGSize) -> some View {
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

      ZStack(alignment: .bottom) {
        fullQwertyDeck(
          width: keyboardWidth,
          height: keyboardHeight,
          compact: !usesTabletLayout
        )

        HStack(spacing: usesTabletLayout ? 18 : 12) {
          if let game, previewHeight > 0 {
            gamePreview(game, height: previewHeight)
          }

          editor(compact: !usesTabletLayout)
            .frame(
              maxWidth: min(usesTabletLayout ? 760 : 620, keyboardWidth)
            )
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

      HStack(alignment: .center, spacing: 16) {
        VStack(spacing: 8) {
          if let game, previewHeight > 0 {
            gamePreview(game, height: previewHeight)
          }

          editor(compact: false)
        }
        .frame(
          width: editorColumnWidth,
          height: max(0, gridSide - renameTitleHeight),
          alignment: .center
        )
        .padding(.top, renameTitleHeight)

        orbitKeysDeck(side: gridSide)
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
  private func portraitKeyboardLayout(availableSize: CGSize) -> some View {
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

      VStack(spacing: 8) {
        editor(compact: false)
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

      VStack(spacing: 6) {
        editor(compact: false)
          .frame(maxWidth: usesTabletLayout ? 900 : 620)
        orbitKeysDeck(side: gridSide)
      }
      .frame(maxWidth: usesTabletLayout ? 960 : 680)
    }
  }

  private func editor(compact: Bool) -> some View {
    OrbitKeysEditorPanel(
      model: model,
      compact: compact,
      usesExpandedMetrics: usesTabletLayout,
      onSettingsTap: model.toggleKeyboardSettings
    )
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
    compact: Bool
  ) -> some View {
    OrbitKeysFullQwerty(model: model, compact: compact)
      .frame(width: width, height: height)
      .contentShape(Rectangle())
      .onTapGesture {}
      .transition(.scale(scale: 0.94).combined(with: .opacity))
  }

  private func settingsLayer(
    isWide: Bool,
    availableSize: CGSize,
    safeAreaInsets: EdgeInsets
  ) -> some View {
    let sidePadding = isWide ? CGFloat(16) : CGFloat(14)
    let topPadding = max(safeAreaInsets.top + 6, 10)
    let bottomPadding = max(safeAreaInsets.bottom + 10, 12)
    let availableHeight = max(
      260,
      availableSize.height - topPadding - bottomPadding
    )
    let panelWidth = isWide
      ? min(410, max(350, availableSize.width * 0.38))
      : min(330, max(280, availableSize.width * 0.78))
    let panelHeight = isWide
      ? min(320, availableHeight * 0.86)
      : min(290, availableHeight * 0.5)

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
    model.openKeyboard()
  }

  private func handleVisibilityChange(
    _ oldValue: Bool,
    _ isVisible: Bool
  ) {
    guard hasOpened, oldValue, !isVisible, !hasCompleted else { return }
    hasCompleted = true
    if let submitted = model.lastSubmittedText {
      onCommit(submitted)
    } else {
      onCancel()
    }
  }

  private func releaseResources() {
    if !hasCompleted {
      hasCompleted = true
      onCancel()
    }
    model.releaseKeyboardSessionResources()
  }
}

/// Replaces OrbitKeys' standalone `@main` demo with an embeddable sample that
/// can be used elsewhere in ARMSX2 without creating a second app entry point.
struct OrbitKeysEmbeddedDemoView: View {
  @State private var text = ""

  var body: some View {
    OrbitKeysKeyboardView(
      title: "OrbitKeys",
      initialText: text,
      onCommit: { text = $0 },
      onCancel: {}
    )
  }
}
