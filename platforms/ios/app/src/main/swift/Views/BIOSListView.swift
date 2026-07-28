// BIOSListView.swift — BIOS file list with default selection
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct BIOSListView: View {
    let embeddedInMenuNavigation: Bool
    let ownsEmbeddedMenuToolbar: Bool

    init(
        embeddedInMenuNavigation: Bool = false,
        ownsEmbeddedMenuToolbar: Bool = true
    ) {
        self.embeddedInMenuNavigation = embeddedInMenuNavigation
        self.ownsEmbeddedMenuToolbar = ownsEmbeddedMenuToolbar
    }

    @State private var bioses: [ARMSX2BIOSInfo] = []
    @State private var defaultBIOS: String = ""
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBIOSImporter = false
    @State private var showBIOSCompatibilityImporter = false
    @State private var showBIOSReplacementAlert = false
    @State private var showRestartAlert = false
    @State private var pendingBIOSImportURLs: [URL] = []
    @State private var existingBIOSImportFileNames: [String] = []
    @State private var hasLoadedBIOSes = false
    @State private var BIOSRefreshPending = true
    @State private var BIOSRefreshTask: Task<Void, Never>?
    @State private var appState = AppState.shared
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    @Environment(\.verticalSizeClass) private var verticalSizeClass

    private var backgroundConfigured: Bool {
        settings.hasCustomBackground && settings.backgroundEnabledInBIOS
    }

    private var backgroundActive: Bool {
        backgroundConfigured
    }

    private var showsPageOwnedLargeTitle: Bool {
        embeddedInMenuNavigation
            && verticalSizeClass != .compact
            && UIDevice.current.userInterfaceIdiom == .phone
    }

    var body: some View {
        OptionalMenuNavigationStack(embedded: embeddedInMenuNavigation) {
            ZStack {
                if backgroundConfigured {
                    MenuBackgroundLayer(isActive: menuTabIsActive)
                }

                if bioses.isEmpty {
                    GeometryReader { geometry in
                        ScrollView {
                            VStack(spacing: 0) {
                                if showsPageOwnedLargeTitle {
                                    EmbeddedMenuLargeTitle(
                                        title: settings.localized("BIOS")
                                    )
                                }

                                emptyState
                                    .frame(
                                        maxWidth: .infinity,
                                        minHeight: max(
                                            0,
                                            geometry.size.height
                                                - (showsPageOwnedLargeTitle ? 64 : 0)
                                        ),
                                        alignment: .center
                                    )
                                    // Align the BIOS icon with the Games
                                    // empty-state icon in compact height.
                                    .offset(
                                        y: verticalSizeClass == .compact ? -32 : 0
                                    )
                            }
                        }
                        .contentMargins(.top, 0, for: .scrollContent)
                        .scrollBounceBehavior(.always)
                    }
                } else {
                    List {
                        if showsPageOwnedLargeTitle {
                            EmbeddedMenuLargeTitle(
                                title: settings.localized("BIOS")
                            )
                            .listRowInsets(EdgeInsets())
                            .listRowSeparator(.hidden)
                            .listRowBackground(Color.clear)
                        }

                        ForEach(bioses, id: \.filePath) { bios in
                            biosRow(bios)
                                .gameCardTintMenuBackgroundListRow(backgroundActive)
                        }
                    }
                    .contentMargins(.top, 0, for: .scrollContent)
                    .scrollContentBackground(backgroundActive ? .hidden : .automatic)
                    .scrollBounceBehavior(.always)
#if targetEnvironment(macCatalyst)
                    .listStyle(.inset)
#endif
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .stableMenuContentGlassContainer()
            .clearNavigationContainerBackground()
            .optionalMenuNavigationChrome(
                title: settings.localized("BIOS"),
                backgroundHidden: backgroundActive,
                embedded: embeddedInMenuNavigation
            )
            .toolbar {
                if !embeddedInMenuNavigation || ownsEmbeddedMenuToolbar {
                ToolbarItem(id: "menu.bootBIOS", placement: .topBarLeading) {
                    Button {
                        if appState.runningGameName == "BIOS" {
                            appState.returnToGame()
                        } else if appState.runningGameName != nil {
                            showRestartAlert = true
                        } else {
                            appState.bootBIOSOnly()
                        }
                    } label: {
                        Text(settings.localized("Boot BIOS"))
                            .font(.callout.weight(.semibold))
                            .foregroundStyle(.blue)
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                            .padding(.horizontal, 8)
                            .frame(minHeight: 36)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Boot BIOS"))
                }
                ToolbarItem(id: "menu.import", placement: .topBarTrailing) {
                    Menu {
                        Button {
                            presentMenuPanel("bios_import") {
                                NSLog("[ARMSX2 iOS BIOS] opening primary BIOS picker")
                                showBIOSImporter = true
                            }
                        } label: {
                            Label(settings.localized("Import BIOS"), systemImage: "doc.badge.plus")
                        }
                        Button {
                            presentMenuPanel("bios_compatibility_import") {
                                NSLog("[ARMSX2 iOS BIOS] opening compatibility BIOS picker")
                                showBIOSCompatibilityImporter = true
                            }
                        } label: {
                            Label(settings.localized("Compatibility Picker"), systemImage: "folder.badge.plus")
                        }
                    } label: {
                        Image(systemName: "plus")
                            .foregroundStyle(.blue)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Import BIOS"))
                }
                ToolbarItem(id: "menu.refresh", placement: .topBarTrailing) {
                    Button { loadBIOSes() } label: {
                        Image(systemName: "arrow.clockwise")
                            .foregroundStyle(.blue)
                    }
                    .buttonStyle(.plain)
                }
                }
            }
            .sheet(isPresented: $showBIOSImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.biosContentTypes,
                    allowsMultipleSelection: true
                ) { result in
                    showBIOSImporter = false
                    handleBIOSPickerResult(result, source: "primary")
                }
            }
            .sheet(isPresented: $showBIOSCompatibilityImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.biosContentTypes,
                    allowsMultipleSelection: true,
                    legacyDocumentTypes: ["public.item", "public.data", "public.content"]
                ) { result in
                    showBIOSCompatibilityImporter = false
                    handleBIOSPickerResult(result, source: "compatibility")
                }
            }
            .alert(settings.localized("Replace existing files?"), isPresented: $showBIOSReplacementAlert) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    clearPendingBIOSImport()
                }
                Button(settings.localized("Replace"), role: .destructive) {
                    importBIOSFiles(pendingBIOSImportURLs, allowReplacingExistingFiles: true)
                    clearPendingBIOSImport()
                }
            } message: {
                Text(FileImportHandler.replacementConfirmationMessage(for: existingBIOSImportFileNames))
            }
            .alert(settings.localized("Restart VM?"), isPresented: $showRestartAlert) {
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Restart"), role: .destructive) {
                    appState.shutdownAndBootBIOS()
                }
            } message: {
                Text(
                    "\(settings.localized("VM is currently running."))\n" +
                    "\(settings.localized("Shut down and start")) " +
                    "\(settings.localized("Boot BIOS"))?"
                )
            }
        }
        .onAppear {
            if menuTabIsActive {
                scheduleBIOSRefresh(after: .milliseconds(0))
            }
        }
        .onChange(of: menuTabIsActive) { _, isActive in
            guard isActive, BIOSRefreshPending || !hasLoadedBIOSes else {
                return
            }
            scheduleBIOSRefresh(after: .milliseconds(0))
        }
        .onReceive(NotificationCenter.default.publisher(for: InitialContentBootstrap.didChangeNotification)) { _ in
            BIOSRefreshPending = true
            if menuTabIsActive {
                scheduleBIOSRefresh(after: .milliseconds(120))
            }
        }
        .onDisappear {
            BIOSRefreshTask?.cancel()
            BIOSRefreshTask = nil
        }
    }

    private func presentMenuPanel(_ name: String, _ action: @escaping () -> Void) {
        NSLog("[ARMSX2 iOS BIOSMenu] present \(name)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            action()
        }
    }

    private func biosRow(_ bios: ARMSX2BIOSInfo) -> some View {
        Button {
            if bios.valid {
                ARMSX2Bridge.setDefaultBIOS(bios.fileName)
                defaultBIOS = bios.fileName
            } else {
                fileImporter.lastImportMessage = "\(bios.fileName) is visible in your BIOS folder, but it is not a bootable PS2 BIOS. Keep it if it is a companion ROM, and select a valid boot BIOS as default."
                fileImporter.showImportAlert = true
            }
        } label: {
            HStack(spacing: 12) {
                regionBadge(for: bios)

                VStack(alignment: .leading, spacing: 4) {
                    Text(bios.fileName)
                        .font(.body)
                        .foregroundStyle(.primary)
                    Text(bios.valid ? "\(bios.regionName) BIOS" : settings.localized("Not a boot BIOS"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if bios.valid && !bios.descriptionText.isEmpty {
                        Text(bios.descriptionText)
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .lineLimit(1)
                    } else if !bios.valid {
                        Text(settings.localized("Companion ROM or unsupported BIOS dump"))
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .lineLimit(1)
                    }
                }
                Spacer()
                if bios.fileName == defaultBIOS {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.blue)
                }
            }
        }
        .foregroundStyle(.primary)
        .opacity(bios.valid ? 1 : 0.65)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "cpu")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text(settings.localized("No BIOS Found"))
                .font(.title2)
                .fontWeight(.semibold)
            Text(settings.localized("Import a PS2 BIOS dump to enable booting."))
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                NSLog("[ARMSX2 iOS BIOS] opening primary BIOS picker from empty state")
                showBIOSImporter = true
            } label: {
                Label {
                    Text(settings.localized("Import BIOS"))
                } icon: {
                    Image(systemName: "plus")
                        .symbolRenderingMode(.monochrome)
                        .foregroundStyle(.white)
                }
            }
            .buttonStyle(.borderedProminent)
            Text(settings.localized("If one picker refuses to select your .bin/.rom file, try the other."))
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
        }
        .padding()
        .frame(maxWidth: .infinity, minHeight: 240)
    }

    private func handleBIOSPickerResult(_ result: Result<[URL], Error>, source: String) {
        switch result {
        case .success(let urls):
            NSLog("[ARMSX2 iOS BIOS] %@ picker completed with %d URL(s)", source, urls.count)
            prepareBIOSImport(urls)
        case .failure(let error):
            if !FileImportHandler.isUserCancelledPickerError(error) {
                fileImporter.presentImportResult(FileImportHandler.failedBIOSPickerMessage(errorDescription: error.localizedDescription))
            }
        }
    }

    private func prepareBIOSImport(_ urls: [URL]) {
        let existingFileNames = fileImporter.existingFileNames(for: urls, preferredDestination: .bios)
        guard !existingFileNames.isEmpty else {
            importBIOSFiles(urls, allowReplacingExistingFiles: false)
            return
        }

        pendingBIOSImportURLs = urls
        existingBIOSImportFileNames = existingFileNames
        showBIOSReplacementAlert = true
    }

    private func importBIOSFiles(_ urls: [URL], allowReplacingExistingFiles: Bool) {
        fileImporter.handleURLs(
            urls,
            preferredDestination: .bios,
            allowReplacingExistingFiles: allowReplacingExistingFiles
        )
        loadBIOSes()
        if defaultBIOS.isEmpty, let firstBIOS = bioses.first(where: { $0.valid })?.fileName {
            ARMSX2Bridge.setDefaultBIOS(firstBIOS)
            defaultBIOS = firstBIOS
        }
        if let guidance = nonBootableImportGuidance(for: urls) {
            let message = [
                fileImporter.lastImportMessage,
                guidance
            ]
            .compactMap { $0 }
            .joined(separator: "\n")
            fileImporter.presentImportResult(message)
        } else if !bioses.contains(where: { $0.valid }), !urls.isEmpty {
            let message = [
                fileImporter.lastImportMessage,
                "No bootable PS2 BIOS was found. Import a valid PS2 BIOS dump before starting games."
            ]
            .compactMap { $0 }
            .joined(separator: "\n")
            fileImporter.presentImportResult(message)
        }
    }

    private func clearPendingBIOSImport() {
        pendingBIOSImportURLs = []
        existingBIOSImportFileNames = []
    }

    private func loadBIOSes() {
        let loadedBIOSes = ARMSX2Bridge.availableBIOSInfos()
        let loadedDefaultBIOS = ARMSX2Bridge.defaultBIOSName()
        if !BIOSListsMatch(bioses, loadedBIOSes) {
            bioses = loadedBIOSes
        }
        if defaultBIOS != loadedDefaultBIOS {
            defaultBIOS = loadedDefaultBIOS
        }
        hasLoadedBIOSes = true
        BIOSRefreshPending = false
    }

    /// Coalesces bootstrap notifications and avoids opening every BIOS file
    /// while the retained BIOS page is hidden behind another tab.
    private func scheduleBIOSRefresh(after delay: Duration) {
        BIOSRefreshTask?.cancel()
        BIOSRefreshTask = Task { @MainActor in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, menuTabIsActive else {
                BIOSRefreshPending = true
                return
            }
            loadBIOSes()
            BIOSRefreshTask = nil
        }
    }

    private func BIOSListsMatch(
        _ first: [ARMSX2BIOSInfo],
        _ second: [ARMSX2BIOSInfo]
    ) -> Bool {
        guard first.count == second.count else { return false }
        return zip(first, second).allSatisfy { lhs, rhs in
            lhs.filePath == rhs.filePath
                && lhs.fileName == rhs.fileName
                && lhs.valid == rhs.valid
                && lhs.regionName == rhs.regionName
                && lhs.countryCode == rhs.countryCode
                && lhs.descriptionText == rhs.descriptionText
        }
    }

    private func nonBootableImportGuidance(for urls: [URL]) -> String? {
        let selectedFileNames = Set(urls.map(\.lastPathComponent))
        let nonBootableFileNames = bioses
            .filter { !$0.valid && selectedFileNames.contains($0.fileName) }
            .map(\.fileName)

        guard !nonBootableFileNames.isEmpty else { return nil }

        let fileMessage: String
        let setupMessage: String
        if nonBootableFileNames.count == 1 {
            fileMessage = "\(nonBootableFileNames[0]) is in your BIOS folder, but it is not a bootable PS2 BIOS. It may be a companion ROM or unsupported BIOS-related file."
            setupMessage = "A bootable BIOS is already installed, but this selected file cannot be used to boot games."
        } else {
            fileMessage = "These selected files are in your BIOS folder, but they are not bootable PS2 BIOS files: \(nonBootableFileNames.joined(separator: ", ")). They may be companion ROMs or unsupported BIOS-related files."
            setupMessage = "A bootable BIOS is already installed, but these files cannot be used to boot games."
        }

        if bioses.contains(where: { $0.valid }) {
            return "\(fileMessage)\n\(setupMessage)"
        }
        return "\(fileMessage)\nNo bootable PS2 BIOS was found. Import a valid PS2 BIOS dump before starting games."
    }

    @ViewBuilder
    private func regionBadge(for bios: ARMSX2BIOSInfo) -> some View {
        if backgroundActive {
            badgeContent(for: bios)
                .frame(width: 44, height: 44)
                .glassSurface(clear: true, cornerRadius: 12)
                .accessibilityLabel(bios.valid ? "\(bios.regionName) BIOS" : settings.localized("Not a boot BIOS"))
        } else {
            badgeContent(for: bios)
                .frame(width: 44, height: 44)
                .background(
                    Color(.secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                )
                .accessibilityLabel(bios.valid ? "\(bios.regionName) BIOS" : settings.localized("Not a boot BIOS"))
        }
    }

    @ViewBuilder
    private func badgeContent(for bios: ARMSX2BIOSInfo) -> some View {
        if let flag = flagEmoji(for: bios.countryCode) {
            Text(flag)
                .font(.title2)
        } else {
            Image(systemName: "globe")
                .font(.title3)
                .foregroundStyle(.secondary)
        }
    }

    private func flagEmoji(for countryCode: String) -> String? {
        let scalars = countryCode.uppercased().unicodeScalars
        guard scalars.count == 2 else { return nil }

        var unicodeScalars = String.UnicodeScalarView()
        for scalar in scalars {
            guard scalar.value >= 65, scalar.value <= 90,
                  let regional = UnicodeScalar(0x1F1E6 + scalar.value - 65) else {
                return nil
            }
            unicodeScalars.append(regional)
        }

        return String(unicodeScalars)
    }
}
