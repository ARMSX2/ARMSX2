// ControllerAccessibilityOptionsPickerModifier.swift — options picker target and list
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Presents one controller-owned list for a SwiftUI Picker. The Picker keeps
/// its native touch presentation, while Cross enters a deterministic semantic
/// target graph that works the same way in Settings and Per-Game Settings.
private struct ControllerAccessibilityOptionsPickerModifier<Selection: Hashable>: ViewModifier {
    let navigationID: String?
    let label: String
    @Binding var selection: Selection
    let options: [(id: Selection, title: String)]

    @Environment(\.menuControllerInputRouter) private var controllerInput
    @State private var isOptionListPresented = false

    private var scopeID: String {
        let source = navigationID ?? label
        return source.isEmpty ? "controller.options-picker" : source
    }

    func body(content: Content) -> some View {
        content
            .controllerAccessibilityPickerTarget(
                id: navigationID,
                label: label,
                value: options.first(where: {
                    $0.id == selection
                })?.title,
                onActivate: {
                    guard !options.isEmpty else { return }
                    isOptionListPresented = true
                },
                onIncrement: {
                    moveSelection(by: 1)
                },
                onDecrement: {
                    moveSelection(by: -1)
                }
            )
            .sheet(isPresented: $isOptionListPresented) {
                ControllerAccessibilityOptionsList(
                    title: label,
                    scopeID: scopeID,
                    selection: $selection,
                    options: options,
                    controllerInput: controllerInput
                )
                .presentationDetents([.large])
                .presentationDragIndicator(.visible)
            }
    }

    private func moveSelection(by offset: Int) {
        guard let index = options.firstIndex(where: {
            $0.id == selection
        }), !options.isEmpty else { return }
        let nextIndex = min(
            max(index + offset, options.startIndex),
            options.index(before: options.endIndex)
        )
        selection = options[nextIndex].id
    }
}

private struct ControllerAccessibilityOptionsList<Selection: Hashable>: View {
    let title: String
    let scopeID: String
    @Binding var selection: Selection
    let options: [(id: Selection, title: String)]
    let controllerInput: MenuControllerInputRouter?

    @Environment(\.dismiss) private var dismiss
    @Environment(\.uiContentTextColour) private var contentTextColour
    @Environment(\.uiSecondaryTextColour) private var secondaryTextColour

    private var targetIDs: [String] {
        options.indices.map { targetID(for: $0) }
    }

    private var selectedTargetID: String? {
        guard let index = options.firstIndex(where: {
            $0.id == selection
        }) else { return targetIDs.first }
        return targetID(for: index)
    }

    var body: some View {
        NavigationStack {
            List {
                ForEach(Array(options.enumerated()), id: \.offset) { index, option in
                    let isSelected = selection == option.id
                    Button {
                        choose(option.id)
                    } label: {
                        HStack(spacing: 12) {
                            Text(option.title)
                                .foregroundStyle(contentTextColour)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            Image(
                                systemName: isSelected
                                    ? "checkmark.circle.fill"
                                    : "circle"
                            )
                            .foregroundStyle(
                                isSelected ? Color.accentColor : secondaryTextColour
                            )
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .controllerAccessibilityActionTarget(
                        id: targetID(for: index),
                        label: option.title
                    ) {
                        choose(option.id)
                    }
                }
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
        }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "controller.options.\(scopeID)",
            priority: 700,
            onBack: {
                dismiss()
                return true
            },
            usesExplicitTargetGeometryOnly: true,
            preservesFocusDuringRightStickScrolling: false,
            focusScrollBehavior: .maintainWithinViewport,
            focusTopAlignmentMargin: 24,
            focusBottomAlignmentMargin: 24,
            preferredInitialFocusLabel: selectedTargetID,
            declaredTargetOrder: targetIDs
        )
    }

    private func targetID(for index: Int) -> String {
        "\(scopeID).option.\(index)"
    }

    private func choose(_ value: Selection) {
        selection = value
        dismiss()
    }
}

extension View {
    func controllerAccessibilityOptionsPickerTarget<Selection: Hashable>(
        id: String? = nil,
        label: String,
        selection: Binding<Selection>,
        options: [(id: Selection, title: String)]
    ) -> some View {
        modifier(
            ControllerAccessibilityOptionsPickerModifier(
                navigationID: id,
                label: label,
                selection: selection,
                options: options
            )
        )
    }
}
