// DiagnosticsView.swift — Settings > Diagnostics: the kept launches, each one reportable
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct DiagnosticsView: View {
    @State private var settings = SettingsStore.shared
    @State private var rows: [SessionRow] = []
    @State private var pending: Set<String> = []
    @State private var reportRequest: ReportSheetRequest?
    @State private var verbose = false

    private var controllerTargetOrder: [String] {
        rows.map { "settings.diagnostics.session.\($0.id)" } + ["settings.diagnostics.verbose"]
    }

    var body: some View {
        Form {
            Section {
                if rows.isEmpty {
                    Text(settings.localized("No sessions yet."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                ForEach(rows) { row in
                    Button {
                        reportRequest = ReportSheetRequest(sessionID: row.id)
                    } label: {
                        sessionLabel(row)
                    }
                    .controllerAccessibilityTargetID("settings.diagnostics.session.\(row.id)")
                }
            } header: {
                Text(settings.localized("Sessions"))
            } footer: {
                Text(settings.localized("Tap a session to report it. The last 10 launches are kept."))
            }

            Section {
                Toggle(settings.localized("Verbose logging"), isOn: verboseBinding)
                    .toggleStyle(.switch)
                    .controllerAccessibilityToggleTarget(
                        id: "settings.diagnostics.verbose",
                        label: settings.localized("Verbose logging"),
                        isOn: verboseBinding
                    )
            } footer: {
                Text(settings.localized("Slows games and writes more detail to emulog.txt until you turn it off."))
            }
        }
        .controllerAccessibilityTargetOrder(controllerTargetOrder)
        .navigationTitle(settings.localized("Diagnostics"))
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $reportRequest) { ReportSheet(request: $0) }
        .task { await loadRows() }
        .onAppear { verbose = ARMSX2Bridge.getINIBool("Logging", key: "EnableVerbose", defaultValue: false) }
    }

    private var verboseBinding: Binding<Bool> {
        Binding(
            get: { verbose },
            set: {
                verbose = $0
                ARMSX2Bridge.setVerboseLogging($0)
            }
        )
    }

    private func sessionLabel(_ row: SessionRow) -> some View {
        let name = row.isNow ? "Now" : (row.title.isEmpty ? "Menu" : row.title)
        let ending = row.isNow ? "Running" : (row.ending?.label ?? "Unknown")
        let detail = [row.started?.formatted(date: .abbreviated, time: .shortened),
                      pending.contains(row.id) ? nil : settings.localized(ending)]
        return HStack {
            VStack(alignment: .leading) {
                Text(row.isNow || row.title.isEmpty ? settings.localized(name) : name)
                    .foregroundStyle(.primary)
                Text(detail.compactMap { $0 }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            let attention = row.ending?.needsAttention == true
            Image(systemName: attention ? "exclamationmark.triangle.fill" : "doc.text")
                .foregroundStyle(attention ? Color.red : Color.secondary)
        }
    }

    private func loadRows() async {
        let current = SessionStore.currentID
        let ids = DiagnosticsRows.order(SessionStore.list(), current: current)
        pending = Set(ids)
        rows = ids.map {
            SessionRow(id: $0, isNow: $0 == current, title: "", started: SessionStore.startedDate(of: $0),
                       ending: nil, logReadable: true)
        }
        for id in ids {
            let row = await Task.detached(priority: .utility) {
                DiagnosticsRows.load(id: id, isNow: id == current)
            }.value
            if let index = rows.firstIndex(where: { $0.id == id }) { rows[index] = row }
            pending.remove(id)
        }
    }
}
