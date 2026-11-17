// LaunchReportOffer.swift — one prompt after a bad session, then the report sheet
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import os

struct LaunchReportOffer: ViewModifier {
    let menuScreenActive: Bool
    let showBootSplash: Bool
    let menuControllerInput: MenuControllerInputRouter

    @State private var settings = SettingsStore.shared
    @State private var appState = AppState.shared
    @State private var session = GameSession.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var reportRequest: ReportSheetRequest?

    private static let scanned = OSAllocatedUnfairLock(initialState: false)

    // The sources RootView.activeControllerAlertKind reads: ControllerPrompt has one slot and shows over gameplay too.
    private var gateOpen: Bool {
        menuScreenActive && !showBootSplash && !session.automaticGameStartupPending
            && session.pendingJITGameBoot == nil && session.pendingRestartGame == nil
            && !fileImporter.showImportAlert && session.bootDisclaimerMessage == nil
            && appState.pendingLibraryExport == nil
            && menuControllerInput.pendingNavigationModeSwitchRequest == nil
            && ControllerPrompt.shared.request == nil
    }

    func body(content: Content) -> some View {
        let direction = settings.localizedLayoutDirection
        let locale = settings.appLanguage == .system ? Locale.autoupdatingCurrent : Locale(identifier: settings.appLanguage.bcp47Code)
        content
            .task(id: gateOpen) { await offerIfQuiet() }
            // This modifier sits outside the environment RootView injects, so the sheet restates it.
            .sheet(item: $reportRequest) { request in
                ReportSheet(request: request)
                    .environment(\.menuControllerInputRouter, menuControllerInput)
                    .environment(\.layoutDirection, direction)
                    .environment(\.locale, locale)
            }
    }

    @MainActor private func offerIfQuiet() async {
        guard gateOpen, !Self.scanned.withLock({ $0 }) else { return }
        let current = SessionStore.currentID
        let target = await Task.detached { LaunchOffer.pick(current: current) }.value
        // A cancelled task means the menu inputs changed while the scan ran.
        guard !Task.isCancelled, gateOpen else { return }
        Self.scanned.withLock { $0 = true }
        guard let target else { return }
        let ending = settings.localized(target.ending.label)
        let message = target.title.isEmpty
            ? String(format: settings.localized("Last time: %@. Send a report?"), ending)
            : String(format: settings.localized("Last time, %1$@: %2$@. Send a report?"), target.title, ending)
        ControllerPrompt.shared.ask(settings.localized("Report a Problem"), message: message, actions: [
            .init(title: settings.localized("Not Now"), isCancel: true),
            .init(title: settings.localized("Report")) { reportRequest = ReportSheetRequest(sessionID: target.id) },
        ])
        SessionStore.markHandled(target: target.id, current: current)
    }
}
