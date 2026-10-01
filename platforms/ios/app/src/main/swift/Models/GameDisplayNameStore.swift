// GameDisplayNameStore.swift — persistent user-assigned library titles
// SPDX-License-Identifier: GPL-3.0+

import Foundation

@MainActor
final class GameDisplayNameStore {
    static let shared = GameDisplayNameStore()

    private static let defaultsKey = "ARMSX2iOSGameDisplayNames"
    private var namesByIdentifier: [String: String]

    private init(defaults: UserDefaults = .standard) {
        namesByIdentifier = defaults.dictionary(forKey: Self.defaultsKey)?
            .compactMapValues { $0 as? String } ?? [:]
    }

    func displayName(
        for identifier: String,
        fallback: String
    ) -> String {
        namesByIdentifier[identifier] ?? fallback
    }

    func setDisplayName(
        _ proposedName: String,
        for identifier: String,
        fallback: String
    ) {
        let name = sanitizedName(proposedName)
        if name.isEmpty || name == fallback {
            namesByIdentifier.removeValue(forKey: identifier)
        } else {
            namesByIdentifier[identifier] = name
        }
        UserDefaults.standard.set(
            namesByIdentifier,
            forKey: Self.defaultsKey
        )
    }

    private func sanitizedName(_ value: String) -> String {
        value
            .components(separatedBy: .newlines)
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .prefix(120)
            .description
    }
}
