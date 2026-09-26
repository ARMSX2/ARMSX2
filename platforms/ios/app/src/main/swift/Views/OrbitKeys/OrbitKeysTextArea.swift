// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.
// Dynamic Background sources are intentionally excluded.
import SwiftUI
import UIKit

// MARK: - Editor Panel

struct OrbitKeysEditorPanel: View {
  @ObservedObject var model: OrbitKeysModel
  let compact: Bool
  let usesExpandedMetrics: Bool
  let onSettingsTap: () -> Void
  @Environment(\.uiAccentColour) private var themeAccent

  var body: some View {
    VStack(spacing: usesExpandedMetrics ? 10 : 6) {
      HStack(spacing: usesExpandedMetrics ? 9 : 5) {
        OrbitKeysTextField(
          model: model,
          compact: compact,
          usesExpandedMetrics: usesExpandedMetrics
        )

        editorActions
      }

      HStack(spacing: usesExpandedMetrics ? 8 : 5) {
        holdBackspaceButton
        modeButton(
          title: "L2",
          subtitle: "123",
          color: themeAccent,
          isActive: model.mode == .numbers,
          action: model.toggleNumbersLock
        )
        modeButton(
          title: "R2",
          subtitle: "ABC",
          color: themeAccent,
          isActive: model.mode == .shifted,
          action: model.toggleShiftLock
        )
        holdSpaceButton
      }
    }
    .frame(maxWidth: .infinity)
    .fixedSize(horizontal: false, vertical: true)
    .padding(usesExpandedMetrics ? 14 : (compact ? 9 : 10))
    .keyboardThemeSurface(
      usesLegacyTheme: model.usesLegacyTheme,
      clearGlassLevel: model.clearGlassLevel,
      cornerRadius: 22
    )
    .contentShape(Rectangle())
    .onTapGesture {}
  }

  private func modeButton(
    title: String,
    subtitle: String,
    color: Color,
    isActive: Bool,
    action: @escaping () -> Void
  ) -> some View {
    Button(action: action) {
      controlLabel(
        title: title,
        subtitle: subtitle,
        color: color,
        isActive: isActive
      )
    }
    .buttonStyle(.plain)
    .accessibilityLabel("\(title), \(subtitle) mode")
    .accessibilityValue(isActive ? "On" : "Off")
  }

  private var holdBackspaceButton: some View {
    controlLabel(
      title: "L1",
      subtitle: "Delete",
      color: themeAccent,
      isActive: model.isLeftShoulderHeld
    )
    .contentShape(Capsule())
    .gesture(
      DragGesture(minimumDistance: 0)
        .onChanged { _ in
          model.setBackspaceHeld(true)
        }
        .onEnded { _ in
          model.setBackspaceHeld(false)
        }
    )
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("L1, Delete")
    .accessibilityHint("Press and hold to repeatedly delete")
  }

  private var holdSpaceButton: some View {
    controlLabel(
      title: "R1",
      subtitle: "Space",
      color: themeAccent,
      isActive: model.isRightShoulderHeld
    )
    .contentShape(Capsule())
    .gesture(
      DragGesture(minimumDistance: 0)
        .onChanged { _ in
          model.setSpaceHeld(true)
        }
        .onEnded { _ in
          model.setSpaceHeld(false)
        }
    )
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("R1, Space")
  }

  private func controlLabel(
    title: String,
    subtitle: String,
    color: Color,
    isActive: Bool
  ) -> some View {
    HStack(spacing: usesExpandedMetrics ? 6 : 4) {
      Text(title)
        .font(
          .system(
            size: usesExpandedMetrics ? 12 : 8.5,
            weight: .black,
            design: .rounded
          )
        )
      Text(subtitle)
        .font(
          .system(
            size: usesExpandedMetrics ? 12 : 8.5,
            weight: .bold,
            design: .rounded
          )
        )
    }
    .foregroundStyle(isActive ? .black : color)
    .frame(maxWidth: .infinity)
    .frame(height: usesExpandedMetrics ? 42 : 28)
    .background(isActive ? color : color.opacity(0.08), in: Capsule())
    .overlay {
      Capsule().stroke(color.opacity(isActive ? 0 : 0.3), lineWidth: 0.8)
    }
  }

  private var editorActions: some View {
    HStack(spacing: usesExpandedMetrics ? 10 : 7) {
      Button(action: model.performPrimaryEditorAction) {
        Image(systemName: model.showsKeyboardSwitchAction ? "keyboard.fill" : "paperplane.fill")
          .font(
            .system(
              size: usesExpandedMetrics ? 18 : 12,
              weight: .bold
            )
          )
          .foregroundStyle(themeAccent)
          .frame(width: editorControlWidth, height: editorControlHeight)
          .editorToolbarFocus(
            color: themeAccent,
            isSelected: model.fullQwertyToolbarSelection == .send
          )
      }
      .buttonStyle(.plain)
      .accessibilityLabel(model.showsKeyboardSwitchAction ? keyboardSwitchLabel : "Send text")
      .accessibilityHint(
        model.showsKeyboardSwitchAction
          ? "Switches between OrbitKeys and the normal keyboard"
          : "Inserts the text and closes the keyboard"
      )

      settingsButton

      Button(action: model.cancelAndClose) {
        Image(systemName: "xmark")
          .font(
            .system(
              size: usesExpandedMetrics ? 18 : 12,
              weight: .black
            )
          )
          .foregroundStyle(themeAccent)
          .frame(width: editorControlWidth, height: editorControlHeight)
          .editorToolbarFocus(
            color: themeAccent,
            isSelected: model.fullQwertyToolbarSelection == .close
          )
      }
      .buttonStyle(.plain)
      .accessibilityLabel("Close keyboard")
      .accessibilityHint("Closes the keyboard without changing the saved text")
    }
    .fixedSize()
  }

  private var keyboardSwitchLabel: String {
    model.usesFullQwerty ? "Switch to OrbitKeys" : "Switch to normal keyboard"
  }

  private var editorControlHeight: CGFloat {
    usesExpandedMetrics ? 42 : (compact ? 22 : 24)
  }

  private var editorControlWidth: CGFloat {
    usesExpandedMetrics ? 44 : 25
  }

  private var settingsButton: some View {
    Button {
      onSettingsTap()
    } label: {
      Image(systemName: "gearshape.fill")
        .font(
          .system(
            size: usesExpandedMetrics ? 18 : 12,
            weight: .bold
          )
        )
        .foregroundStyle(themeAccent)
        .frame(width: editorControlWidth, height: editorControlHeight)
        .editorToolbarFocus(
          color: themeAccent,
          isSelected: model.fullQwertyToolbarSelection == .settings
        )
    }
    .buttonStyle(.plain)
    .accessibilityLabel("Keyboard settings")
    .accessibilityHint("Configure L3 layout, R3 icons, and orbit effects")
  }
}

private extension View {
  func editorToolbarFocus(color: Color, isSelected: Bool) -> some View {
    modifier(OrbitKeysEditorToolbarFocusModifier(color: color, isSelected: isSelected))
  }
}

private struct OrbitKeysEditorToolbarFocusModifier: ViewModifier {
  let color: Color
  let isSelected: Bool
  @Environment(\.uiAccentColour) private var themeAccent

  func body(content: Content) -> some View {
    content
      .background {
        RoundedRectangle(cornerRadius: 8, style: .continuous)
          .fill(isSelected ? color.opacity(0.24) : .clear)
      }
      .overlay {
        RoundedRectangle(cornerRadius: 8, style: .continuous)
          .stroke(isSelected ? themeAccent : .clear, lineWidth: 1.5)
          .shadow(
            color: isSelected ? themeAccent.opacity(0.9) : .clear,
            radius: 5
          )
      }
      .scaleEffect(isSelected ? 1.08 : 1)
      .animation(.easeOut(duration: 0.12), value: isSelected)
  }
}

// MARK: - Keyboard Launcher

struct OrbitKeysLauncherTextField: View {
  @ObservedObject var model: OrbitKeysModel

  var body: some View {
    Button(action: model.openKeyboard) {
      Text(
        model.committedText.isEmpty
          ? "Tap or press Cross"
          : model.committedText
      )
      .font(.system(size: 13, weight: .medium, design: .monospaced))
      .foregroundStyle(model.committedText.isEmpty ? .white.opacity(0.44) : .white)
      .lineLimit(1)
      .truncationMode(.tail)
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(.horizontal, 12)
      .frame(height: 34)
      .contentShape(RoundedRectangle(cornerRadius: 17, style: .continuous))
      .keyboardThemeSurface(
        usesLegacyTheme: model.usesLegacyTheme,
        clearGlassLevel: model.clearGlassLevel,
        cornerRadius: 17
      )
    }
    .buttonStyle(.plain)
    .frame(minWidth: 170, idealWidth: 260, maxWidth: .infinity)
    .accessibilityLabel("Text input")
    .accessibilityHint("Tap or press Cross to open the controller keyboard")
  }
}

// MARK: - Keyboard Text Field

struct OrbitKeysTextField: View {
  @ObservedObject var model: OrbitKeysModel
  let compact: Bool
  let usesExpandedMetrics: Bool
  @Environment(\.uiAccentColour) private var themeAccent

  var body: some View {
    let fieldHeight = usesExpandedMetrics
      ? CGFloat(52)
      : (compact ? CGFloat(22) : 24)

    OrbitKeysThinCaretTextView(
      text: model.text,
      cursorOffset: model.cursorUTF16Offset,
      fontSize: usesExpandedMetrics ? 20 : (compact ? 14 : 15),
      accentColor: UIColor(themeAccent)
    )
    .padding(.horizontal, usesExpandedMetrics ? 14 : 10)
    .padding(.vertical, usesExpandedMetrics ? 5 : 1)
    .frame(
      maxWidth: .infinity,
      minHeight: fieldHeight,
      alignment: .leading
    )
    .fixedSize(horizontal: false, vertical: true)
    .keyboardKeySurface(
      usesLegacyTheme: model.usesLegacyTheme,
      clearGlassLevel: model.clearGlassLevel,
      cornerRadius: 16
    )
    .accessibilityLabel(
      model.text.isEmpty ? "Text field, empty" : "Text field, \(model.text)"
    )
  }
}

// MARK: - Thin Caret Text View

struct OrbitKeysThinCaretTextView: UIViewRepresentable {
  let text: String
  let cursorOffset: Int
  let fontSize: CGFloat
  let accentColor: UIColor

  final class Coordinator {
    var measuredText = ""
    var measuredFontSize: CGFloat = 0
    var measuredWidth: CGFloat = 0
    var measuredSize: CGSize?
  }

  func makeCoordinator() -> Coordinator {
    Coordinator()
  }

  func makeUIView(context: Context) -> OrbitKeysCaretTextView {
    let textView = OrbitKeysCaretTextView()
    textView.backgroundColor = .clear
    textView.isEditable = false
    textView.isSelectable = false
    textView.isScrollEnabled = false
    textView.isUserInteractionEnabled = false
    textView.textContainerInset = .zero
    textView.textContainer.lineFragmentPadding = 0
    textView.textContainer.maximumNumberOfLines = 0
    textView.textContainer.lineBreakMode = .byWordWrapping
    textView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    textView.text = text
    textView.cursorOffset = min(max(cursorOffset, 0), text.utf16.count)
    textView.font = .monospacedSystemFont(ofSize: fontSize, weight: .medium)
    textView.textColor = .white
    textView.caretColor = accentColor
    return textView
  }

  func updateUIView(_ textView: OrbitKeysCaretTextView, context: Context) {
    let resolvedCursorOffset = min(max(cursorOffset, 0), text.utf16.count)
    let textChanged = textView.text != text
    let fontChanged = textView.font?.pointSize != fontSize
    let cursorChanged = textView.cursorOffset != resolvedCursorOffset

    if textChanged {
      textView.text = text
    }
    if fontChanged {
      textView.font = .monospacedSystemFont(ofSize: fontSize, weight: .medium)
    }
    if cursorChanged {
      textView.cursorOffset = resolvedCursorOffset
    }
    if textView.caretColor != accentColor {
      textView.caretColor = accentColor
      textView.setNeedsDisplay()
    }

    if textChanged || fontChanged {
      context.coordinator.measuredSize = nil
      textView.invalidateIntrinsicContentSize()
      textView.setNeedsLayout()
    }
    if textChanged || fontChanged || cursorChanged {
      textView.setNeedsDisplay()
    }
  }

  func sizeThatFits(
    _ proposal: ProposedViewSize,
    uiView textView: OrbitKeysCaretTextView,
    context: Context
  ) -> CGSize? {
    guard let width = proposal.width else { return nil }
    if context.coordinator.measuredText == text,
      context.coordinator.measuredFontSize == fontSize,
      abs(context.coordinator.measuredWidth - width) < 0.5,
      let measuredSize = context.coordinator.measuredSize
    {
      return measuredSize
    }

    let lineHeight = textView.font?.lineHeight ?? fontSize * 1.2
    let measuredHeight = textView.sizeThatFits(
      CGSize(width: width, height: .greatestFiniteMagnitude)
    ).height
    let measuredSize = CGSize(
      width: width,
      height: ceil(max(measuredHeight, lineHeight))
    )
    context.coordinator.measuredText = text
    context.coordinator.measuredFontSize = fontSize
    context.coordinator.measuredWidth = width
    context.coordinator.measuredSize = measuredSize
    return measuredSize
  }
}

final class OrbitKeysCaretTextView: UITextView {
  var cursorOffset = 0
  var caretColor = UIColor.white

  override func draw(_ rect: CGRect) {
    super.draw(rect)

    guard
      let position = position(
        from: beginningOfDocument,
        offset: cursorOffset
      )
    else {
      return
    }

    var caret = caretRect(for: position)
    guard caret.isFinite else { return }
    caret.origin.y += 1
    caret.size.width = 1
    caret.size.height = max(0, caret.height - 2)
    caretColor.setFill()
    UIRectFill(caret)
  }
}

extension CGRect {
  fileprivate var isFinite: Bool {
    origin.x.isFinite && origin.y.isFinite
      && width.isFinite && height.isFinite
  }
}
