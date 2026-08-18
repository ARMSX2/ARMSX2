// ShaderPresetSaveSheet.swift — naming a tweaked preset
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct ShaderPresetSaveRequest: Identifiable {
    let token: String
    let suggestedName: String

    var id: String { token }
}

struct ShaderPresetSaveSheet: View {
    let request: ShaderPresetSaveRequest
    let localized: @MainActor (String) -> String
    let onSave: @MainActor (String) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var name: String

    init(
        request: ShaderPresetSaveRequest,
        localized: @escaping @MainActor (String) -> String,
        onSave: @escaping @MainActor (String) -> Void
    ) {
        self.request = request
        self.localized = localized
        self.onSave = onSave
        _name = State(initialValue: request.suggestedName)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField(localized("Preset name"), text: $name)
                        .autocorrectionDisabled()
                } footer: {
                    Text(localized("Saved into My Presets, where it is selectable like any other preset. It points at the base pack by path, so removing or moving that pack breaks it, and Reset on the saved preset returns to the values you saved rather than to the base's own."))
                }
            }
            .navigationTitle(localized("Save Preset"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(localized("Cancel")) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(localized("Save")) {
                        onSave(name)
                        dismiss()
                    }
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }
}
