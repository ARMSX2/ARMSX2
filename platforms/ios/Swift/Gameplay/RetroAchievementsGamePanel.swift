// RetroAchievementsGamePanel.swift — the in-game RetroAchievements list and its rows
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private struct RetroAchievementEntry: Identifiable, Equatable {
    let id: Int
    let title: String
    let description: String
    let badgePath: String?
    let measuredProgress: String
    let points: Int
    let state: Int
    let category: Int
    let bucket: Int
    let measuredPercent: Double

    init?(dictionary: [String: Any]) {
        guard let idNumber = dictionary["id"] as? NSNumber else { return nil }
        id = idNumber.intValue
        title = dictionary["title"] as? String ?? ""
        description = dictionary["description"] as? String ?? ""
        let rawBadgePath = (dictionary["badgePath"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        badgePath = rawBadgePath.isEmpty ? nil : rawBadgePath
        measuredProgress = dictionary["measuredProgress"] as? String ?? ""
        points = (dictionary["points"] as? NSNumber)?.intValue ?? 0
        state = (dictionary["state"] as? NSNumber)?.intValue ?? 0
        category = (dictionary["category"] as? NSNumber)?.intValue ?? 0
        bucket = (dictionary["bucket"] as? NSNumber)?.intValue ?? 0
        measuredPercent = (dictionary["measuredPercent"] as? NSNumber)?.doubleValue ?? 0
    }

    var isUnlocked: Bool { state == 2 }
    var isUnsupported: Bool { state == 3 || bucket == 3 }
    var isUnofficial: Bool { (category & 2) != 0 || bucket == 4 }
    var isActiveChallenge: Bool { bucket == 6 || bucket == 7 }
}

struct RetroAchievementsGamePanel: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.menuControllerInputRouter) private var controllerInput
    let settings: SettingsStore
    var onClose: (() -> Void)? = nil

    @State private var entries: [RetroAchievementEntry] = []
    @State private var state: [String: Any] = [:]

    var body: some View {
        NavigationStack {
            List {
                summarySection

                if entries.isEmpty {
                    emptySection
                } else {
                    ForEach(groupedEntries, id: \.title) { group in
                        if !group.entries.isEmpty {
                            Section(group.title) {
                                ForEach(group.entries) { entry in
                                    RetroAchievementRow(entry: entry, settings: settings)
                                        .controllerAccessibilityActionTarget(
                                            id: "runtime.retro-achievements.\(entry.id)",
                                            label: entry.title,
                                            activationFeedback: .silent
                                        ) {}
                                }
                            }
                        }
                    }
                }
            }
            // Hosted beside the fold, the panel's glass shows through as it does in Cheats.
            .scrollContentBackground(onClose == nil ? .automatic : .hidden)
            .navigationTitle(settings.localized("RetroAchievements"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(settings.localized("Done")) {
                        close()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
                refresh()
            }
            .onReceive(NotificationCenter.default.publisher(for: retroAchievementsToastNotification)) { _ in
                refresh()
            }
        }
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            scopeKey: "runtime.retro-achievements",
            priority: 345,
            onBack: {
                close()
                return true
            },
            usesExplicitTargetGeometryOnly: true
        )
    }

    private func close() {
        if let onClose { onClose() } else { dismiss() }
    }

    private var summarySection: some View {
        Section {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    gameBadge

                    VStack(alignment: .leading, spacing: 4) {
                        Text(gameTitle)
                            .font(.headline)
                            .lineLimit(2)
                        Text(progressText)
                            .font(.subheadline.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }

                ProgressView(value: progressFraction)
                    .tint(.yellow)

                if !richPresence.isEmpty {
                    Label(richPresence, systemImage: "quote.bubble")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, 4)
        }
    }

    @ViewBuilder
    private var emptySection: some View {
        Section {
            VStack(spacing: 10) {
                Image(systemName: "trophy")
                    .font(.largeTitle)
                    .foregroundStyle(.secondary)
                Text(emptyTitle)
                    .font(.headline)
                    .multilineTextAlignment(.center)
                Text(emptySubtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .frame(maxWidth: .infinity)
            .padding(28)
        }
    }

    @ViewBuilder
    private var gameBadge: some View {
        if let path = state["gameIconPath"] as? String,
           !path.isEmpty,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 54, height: 54)
                .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        } else {
            Image(systemName: "trophy.fill")
                .font(.system(size: 26, weight: .semibold))
                .foregroundStyle(.yellow)
                .frame(width: 54, height: 54)
                .background(.yellow.opacity(0.14), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
    }

    private var gameTitle: String {
        let title = state["gameTitle"] as? String ?? ""
        return title.isEmpty ? settings.localized("Current Game") : title
    }

    private var richPresence: String {
        state["richPresence"] as? String ?? ""
    }

    private var unlockedCount: Int {
        (state["unlockedAchievements"] as? NSNumber)?.intValue ?? entries.filter(\.isUnlocked).count
    }

    private var totalCount: Int {
        (state["totalAchievements"] as? NSNumber)?.intValue ?? entries.filter { !$0.isUnofficial }.count
    }

    private var unlockedPoints: Int {
        (state["unlockedPoints"] as? NSNumber)?.intValue ?? entries.filter(\.isUnlocked).reduce(0) { $0 + $1.points }
    }

    private var totalPoints: Int {
        (state["totalPoints"] as? NSNumber)?.intValue ?? entries.filter { !$0.isUnofficial }.reduce(0) { $0 + $1.points }
    }

    private var progressFraction: Double {
        guard totalCount > 0 else { return 0 }
        return min(1, max(0, Double(unlockedCount) / Double(totalCount)))
    }

    private var progressText: String {
        "\(unlockedCount)/\(totalCount) \(settings.localized("achievements")) · \(unlockedPoints)/\(totalPoints) \(settings.localized("points"))"
    }

    private var emptyTitle: String {
        if (state["loggedIn"] as? NSNumber)?.boolValue == false {
            return settings.localized("RetroAchievements is not logged in.")
        }
        if (state["hasActiveGame"] as? NSNumber)?.boolValue == false {
            return settings.localized("No RetroAchievements game is active.")
        }
        return settings.localized("No achievements found for this game.")
    }

    private var emptySubtitle: String {
        settings.localized("Start a supported game with RetroAchievements enabled, then reopen this panel.")
    }

    private var groupedEntries: [(title: String, entries: [RetroAchievementEntry])] {
        let active = entries.filter { $0.isActiveChallenge && !$0.isUnlocked && !$0.isUnsupported && !$0.isUnofficial }
        let locked = entries.filter { !$0.isUnlocked && !$0.isActiveChallenge && !$0.isUnsupported && !$0.isUnofficial }
        let unlocked = entries.filter { $0.isUnlocked && !$0.isUnofficial }
        let unofficial = entries.filter(\.isUnofficial)
        let unsupported = entries.filter(\.isUnsupported)

        return [
            (settings.localized("Active / Almost There"), active),
            (settings.localized("Locked"), locked),
            (settings.localized("Unlocked"), unlocked),
            (settings.localized("Unofficial"), unofficial),
            (settings.localized("Unsupported"), unsupported),
        ]
    }

    private func refresh() {
        state = ARMSX2Bridge.retroAchievementsState()
        entries = ARMSX2Bridge.retroAchievementsForCurrentGame()
            .compactMap { RetroAchievementEntry(dictionary: $0) }
    }
}

private struct RetroAchievementRow: View {
    let entry: RetroAchievementEntry
    let settings: SettingsStore

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            badge

            VStack(alignment: .leading, spacing: 5) {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(entry.title.isEmpty ? settings.localized("Untitled Achievement") : entry.title)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(entry.isUnlocked ? .primary : .secondary)
                        .lineLimit(2)
                    Spacer(minLength: 8)
                    Text("\(entry.points) pts")
                        .font(.caption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.yellow)
                }

                if !entry.description.isEmpty {
                    Text(entry.description)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(3)
                }

                HStack(spacing: 8) {
                    Label(statusText, systemImage: statusIcon)
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(statusColor)

                    if !entry.measuredProgress.isEmpty {
                        Text(entry.measuredProgress)
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    } else if entry.measuredPercent > 0 && entry.measuredPercent < 100 {
                        Text("\(Int(entry.measuredPercent.rounded()))%")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }

                    Spacer(minLength: 0)
                }
            }
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private var badge: some View {
        if let path = entry.badgePath,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 46, height: 46)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .opacity(entry.isUnlocked ? 1 : 0.55)
        } else {
            Image(systemName: entry.isUnlocked ? "trophy.fill" : "lock.fill")
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(entry.isUnlocked ? .yellow : .secondary)
                .frame(width: 46, height: 46)
                .background(.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
    }

    private var statusText: String {
        if entry.isUnsupported { return settings.localized("Unsupported") }
        if entry.isUnofficial { return settings.localized("Unofficial") }
        if entry.isUnlocked { return settings.localized("Unlocked") }
        if entry.isActiveChallenge { return settings.localized("Active") }
        return settings.localized("Locked")
    }

    private var statusIcon: String {
        if entry.isUnsupported { return "exclamationmark.triangle.fill" }
        if entry.isUnofficial { return "sparkles" }
        if entry.isUnlocked { return "checkmark.seal.fill" }
        if entry.isActiveChallenge { return "flame.fill" }
        return "lock.fill"
    }

    private var statusColor: Color {
        if entry.isUnsupported { return .orange }
        if entry.isUnofficial { return .purple }
        if entry.isUnlocked { return .green }
        if entry.isActiveChallenge { return .red }
        return .secondary
    }
}
