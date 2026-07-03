// MemoryCardSettingsView.swift — iOS memory card creation and slot assignment
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct MemoryCardSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var availableCards: [String] = []
    @State private var slot1Card = ""
    @State private var slot2Card = ""
    @State private var newCardName = "Mcd003"
    @State private var newCardSizeMB = 8
    @State private var createFolderCard = false
    @State private var resultMessage: String?
    @State private var showResult = false
    @State private var pendingDeleteCard: String?

    private let cardSizes = [8, 16, 32, 64]
    private let pathLikeCharacters: [Character] = ["/", "\\", ":", "*", "?", "\"", "<", ">", "|"]

    var body: some View {
        Form {
            Section(settings.localized("Directory")) {
                Text(ARMSX2Bridge.memoryCardDirectory())
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Section(settings.localized("Assigned Cards")) {
                Picker(settings.localized("Slot 1"), selection: $slot1Card) {
                    Text(settings.localized("Unplugged")).tag("")
                    ForEach(availableCards, id: \.self) { card in
                        Text(card).tag(card)
                    }
                }
                .onChange(of: slot1Card) { _, newValue in
                    ARMSX2Bridge.setMemoryCard(name: newValue, forSlot: 1, enabled: !newValue.isEmpty)
                }

                Picker(settings.localized("Slot 2"), selection: $slot2Card) {
                    Text(settings.localized("Unplugged")).tag("")
                    ForEach(availableCards, id: \.self) { card in
                        Text(card).tag(card)
                    }
                }
                .onChange(of: slot2Card) { _, newValue in
                    ARMSX2Bridge.setMemoryCard(name: newValue, forSlot: 2, enabled: !newValue.isEmpty)
                }

                Text(settings.localized("Slot changes take effect on the next VM boot."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                TextField(settings.localized("Card name"), text: $newCardName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                Toggle(settings.localized("Folder Memory Card"), isOn: $createFolderCard)

                if !createFolderCard {
                    Picker(settings.localized("Size"), selection: $newCardSizeMB) {
                        ForEach(cardSizes, id: \.self) { size in
                            Text("\(size) MB").tag(size)
                        }
                    }
                }

                Button {
                    createCard()
                } label: {
                    Label(settings.localized(createFolderCard ? "Create Folder Card" : "Create Card"), systemImage: "memorychip")
                }
            } header: {
                Text(settings.localized("Create Memory Card"))
            } footer: {
                Text(settings.localized("File cards support 8 MB, 16 MB, 32 MB, and 64 MB. Folder cards match the ARMSX2/PCSX2 folder-card behavior and are useful for game-specific saves."))
            }

            Section(settings.localized("Available Cards")) {
                if availableCards.isEmpty {
                    Text(settings.localized("No cards found."))
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(availableCards, id: \.self) { card in
                        HStack {
                            Image(systemName: (slot1Card == card || slot2Card == card) ? "checkmark.circle.fill" : "circle")
                                .foregroundStyle((slot1Card == card || slot2Card == card) ? .green : .secondary)
                            Text(card)
                            Spacer()
                            Button {
                                pendingDeleteCard = card
                            } label: {
                                Image(systemName: "trash")
                                    .foregroundStyle(.red)
                            }
                            .buttonStyle(.borderless)
                        }
                    }
                }
            }
        }
        .navigationTitle(settings.localized("Memory Cards"))
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: refresh)
        .alert(settings.localized("Memory Cards"), isPresented: $showResult) {
            Button(settings.localized("OK")) {}
        } message: {
            Text(settings.localized(resultMessage ?? ""))
        }
        .confirmationDialog(
            settings.localized("Delete Memory Card?"),
            isPresented: Binding(
                get: { pendingDeleteCard != nil },
                set: { if !$0 { pendingDeleteCard = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button(settings.localized("Delete"), role: .destructive) {
                if let card = pendingDeleteCard {
                    let success = ARMSX2Bridge.deleteMemoryCard(named: card)
                    refresh()
                    resultMessage = success ? "Memory card deleted." : "Could not delete the memory card. It may be in use."
                    showResult = true
                }
                pendingDeleteCard = nil
            }
            Button(settings.localized("Cancel"), role: .cancel) {
                pendingDeleteCard = nil
            }
        } message: {
            if let card = pendingDeleteCard {
                Text(settings.localized("Delete \"\(card)\"? Saves on it will be lost, and it will be removed from any slot it is assigned to."))
            }
        }
    }

    private func refresh() {
        let cards = ARMSX2Bridge.availableMemoryCards()
        availableCards = cards
        let s1 = ARMSX2Bridge.memoryCardName(forSlot: 1) ?? ""
        let s2 = ARMSX2Bridge.memoryCardName(forSlot: 2) ?? ""
        // Self-heal: a slot may still point at a card that was removed out-of-app
        // (e.g. via the Files app). Clear it so the slot does not reference a stale
        // filename, which previously made the other slot's card mislabel at boot.
        if !s1.isEmpty && !cards.contains(s1) {
            ARMSX2Bridge.setMemoryCard(name: "", forSlot: 1, enabled: false)
            slot1Card = ""
        } else {
            slot1Card = s1
        }
        if !s2.isEmpty && !cards.contains(s2) {
            ARMSX2Bridge.setMemoryCard(name: "", forSlot: 2, enabled: false)
            slot2Card = ""
        } else {
            slot2Card = s2
        }
    }

    private func createCard() {
        if let validationMessage = validateNewMemoryCardName(newCardName) {
            resultMessage = validationMessage
            showResult = true
            return
        }

        let success = ARMSX2Bridge.createMemoryCard(named: newCardName, sizeMB: newCardSizeMB, folder: createFolderCard)
        refresh()
        resultMessage = success ? "Memory card created." : "Could not create memory card. Check the name, size, or whether it already exists."
        showResult = true
    }

    private func validateNewMemoryCardName(_ name: String) -> String? {
        let trimmedName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedName.isEmpty {
            return "Enter a name for the memory card first."
        }

        if trimmedName.contains(where: { pathLikeCharacters.contains($0) }) {
            return "Memory card names cannot contain folder or path characters like / or \\."
        }

        if availableCards.contains(where: { $0.caseInsensitiveCompare(trimmedName) == .orderedSame }) {
            return "A memory card with this name already exists."
        }

        return nil
    }
}
