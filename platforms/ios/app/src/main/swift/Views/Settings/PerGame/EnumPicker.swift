// EnumPicker.swift — Reusable picker for a static list of labeled options.
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Picker backed by a fixed list of `(value, title)` options.
///
/// Replaces the repeated `Text("...").tag(value)` clusters in the per-game
/// graphics tab (internal resolution, aspect ratio, texture filtering, blending
/// accuracy). Each option's title is rendered verbatim — these pickers used
/// untranslated literal labels, so no localization hook is applied here. The
/// caller is responsible for any `.disabled(...)` modifier, matching the
/// surrounding per-game rows.
struct EnumPicker<Selection: Hashable, Label: View>: View {
    let options: [(id: Selection, title: String)]
    @Binding var selection: Selection
    let controllerLabel: String
    @ViewBuilder let label: Label

    init(_ options: [(id: Selection, title: String)],
         selection: Binding<Selection>,
         controllerLabel: String,
         @ViewBuilder label: () -> Label) {
        self.options = options
        self._selection = selection
        self.controllerLabel = controllerLabel
        self.label = label()
    }

    var body: some View {
        Picker(selection: $selection) {
            ForEach(options, id: \.id) { option in
                Text(option.title).tag(option.id)
            }
        } label: {
            label
        }
        .controllerAccessibilityOptionsPickerTarget(
            label: controllerLabel,
            selection: $selection,
            options: options
        )
    }
}
