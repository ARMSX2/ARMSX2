// Adapted from OrbitKeys-iOS_COMPLETED_FINAL_FIXED.zip for ARMSX2 integration.
// Dynamic Background sources are intentionally excluded.
import SwiftUI

struct OrbitKeysSettingsPanel: View {
  @ObservedObject var model: OrbitKeysModel
  let onClose: () -> Void
  @Environment(\.uiAccentColour) private var themeAccent

  var body: some View {
    GeometryReader { proxy in
      let scrollHeight = max(96, proxy.size.height - 78)

      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 10) {
          Label("Settings", systemImage: "gearshape.fill")
            .font(.subheadline.weight(.bold))

          Spacer()

          Button(action: onClose) {
            Image(systemName: "xmark")
              .font(.system(size: 11, weight: .black))
              .foregroundStyle(themeAccent)
              .frame(width: 28, height: 28)
              .keyboardGlassStyle(clearGlassLevel: model.clearGlassLevel, cornerRadius: 14)
          }
          .buttonStyle(.plain)
          .accessibilityLabel("Close settings")
        }

        Divider().opacity(0.34)

        ScrollViewReader { scrollProxy in
          ScrollView(.vertical, showsIndicators: true) {
            OrbitKeysSettingsControls(model: model)
              .padding(.trailing, 14)
              .padding(.bottom, 18)
          }
          .onChange(of: model.selectedKeyboardSettingsOption) { _, option in
            withAnimation(.easeInOut(duration: 0.18)) {
              scrollProxy.scrollTo(option, anchor: .center)
            }
          }
        }
        .frame(maxWidth: .infinity)
        .frame(height: scrollHeight)
        .contentShape(Rectangle())
      }
      .padding(.leading, 12)
      .padding(.trailing, 16)
      .padding(.vertical, 12)
      .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
    }
    .keyboardKeySurface(
      usesLegacyTheme: model.usesLegacyTheme,
      clearGlassLevel: model.clearGlassLevel,
      cornerRadius: 20
    )
    .font(.footnote.weight(.semibold))
    .tint(themeAccent)
  }
}

private struct OrbitKeysSettingsControls: View {
  @ObservedObject var model: OrbitKeysModel

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      settingsRow(.normalKeyboard) {
        Toggle(
          isOn: settingBinding(\.usesFullQwerty, set: model.setFullQwertyEnabled)
        ) {
          Label("Normal keyboard", systemImage: "keyboard")
        }
      }

      settingsRow(.loopFullKeyboard) {
        Toggle(
          isOn: settingBinding(
            \.loopsFullQwertyRows,
            set: model.setFullQwertyRowLoopEnabled
          )
        ) {
          Label("Loop full keyboard rows", systemImage: "arrow.left.arrow.right")
        }
      }

      settingsRow(.legacyTheme) {
        Toggle(
          isOn: settingBinding(\.usesLegacyTheme, set: model.setLegacyThemeEnabled)
        ) {
          Label("Legacy Theme", systemImage: "circle.lefthalf.filled")
        }
      }

      settingsRow(.layout) {
        Picker(
          "L3 Layout",
          selection: settingBinding(\.keyboardArrangement, set: model.setKeyboardArrangement)
        ) {
          Text("ABC").tag(OrbitKeysArrangement.alphabetical)
          Text("QAW").tag(OrbitKeysArrangement.qaw)
        }
        .pickerStyle(.segmented)
      }

      settingsRow(.faceIcons) {
        Toggle(
          isOn: settingBinding(\.showsFaceIcons, set: model.setFaceIconsVisible)
        ) {
          Label("R3 Face icons", systemImage: "button.programmable")
        }
      }

      settingsRow(.iconsOnAllKeys) {
        Toggle(
          isOn: settingBinding(
            \.showsFaceIconsOnAllButtons,
            set: model.setFaceIconsOnAllButtons
          )
        ) {
          Label("Icons on all keys", systemImage: "square.grid.3x3")
        }
      }

      settingsRow(.colorsOnAllBoxes) {
        Toggle(
          isOn: settingBinding(\.showsColorsOnAllBoxes, set: model.setColorsOnAllBoxes)
        ) {
          Label("Colors on all boxes", systemImage: "paintpalette")
        }
      }

      settingsRow(.orbitField) {
        Toggle(
          isOn: settingBinding(\.showsOrbs, set: model.setOrbsVisible)
        ) {
          Label("Orbit field", systemImage: "circle.hexagongrid")
        }
      }

      settingsRow(.orbsInForeground) {
        Toggle(
          isOn: settingBinding(\.orbsInForeground, set: model.setOrbsInForeground)
        ) {
          Label("Orbs in foreground", systemImage: "square.3.layers.3d.top.filled")
        }
      }

      settingsRow(.clearGlass) {
        Toggle(
          isOn: settingBinding(\.usesClearGlass, set: model.setClearGlassEnabled)
        ) {
          Label("Clear Glass transparency", systemImage: "circle.lefthalf.filled")
        }
      }

      settingsRow(.transparency) {
        VStack(alignment: .leading, spacing: 6) {
          HStack {
            Label("Transparency", systemImage: "slider.horizontal.3")
            Spacer()
            Text("\(Int((model.clearGlassLevel * 100).rounded()))%")
              .font(.caption.monospacedDigit().weight(.bold))
              .foregroundStyle(.secondary)
          }

          Slider(
            value: settingBinding(\.clearGlassLevel, set: model.setClearGlassLevel),
            in: -1...1,
            step: 0.01
          )
        }
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }

  private func settingBinding<Value>(
    _ keyPath: KeyPath<OrbitKeysModel, Value>,
    set: @escaping (Value) -> Void
  ) -> Binding<Value> {
    Binding(
      get: { model[keyPath: keyPath] },
      set: set
    )
  }

  private func settingsRow<Content: View>(
    _ option: OrbitKeysSettingsOption,
    @ViewBuilder content: () -> Content
  ) -> some View {
    let isSelected = model.selectedKeyboardSettingsOption == option

    return content()
      .padding(.horizontal, 8)
      .padding(.vertical, 6)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background {
        RoundedRectangle(cornerRadius: 12, style: .continuous)
          .fill(isSelected ? model.mode.color.opacity(0.17) : Color.white.opacity(0.001))
      }
      .overlay {
        RoundedRectangle(cornerRadius: 12, style: .continuous)
          .stroke(isSelected ? model.mode.color.opacity(0.42) : .clear, lineWidth: 0.8)
          .allowsHitTesting(false)
      }
      .contentShape(Rectangle())
      .simultaneousGesture(
        TapGesture().onEnded {
          model.selectKeyboardSettingsOption(option)
        }
      )
      .id(option)
  }
}
