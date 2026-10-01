// BIOSListView.swift — BIOS file list with default selection
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct BIOSLibraryEntry: Identifiable, Equatable, Sendable {
    let fileName: String
    let filePath: String
    let regionName: String
    let countryCode: String
    let descriptionText: String
    let regionCode: Int
    let valid: Bool

    var id: String { filePath }

    init(_ info: ARMSX2BIOSInfo) {
        fileName = info.fileName
        filePath = info.filePath
        regionName = info.regionName
        countryCode = info.countryCode
        descriptionText = info.descriptionText
        regionCode = info.regionCode
        valid = info.valid
    }
}

@MainActor
@Observable
final class BIOSLibraryState {
    static let shared = BIOSLibraryState()

    private(set) var entries: [BIOSLibraryEntry] = []
    private(set) var defaultBIOS = ""
    private(set) var hasLoaded = false
    @ObservationIgnored private var refreshTask: Task<Void, Never>?
    @ObservationIgnored private var needsRefresh = true
    @ObservationIgnored private var refreshCompletions:
        [([BIOSLibraryEntry]) -> Void] = []

    private init() {}

    func refreshIfNeeded() {
        guard needsRefresh || !hasLoaded else { return }
        refresh()
    }

    func markNeedsRefresh() {
        needsRefresh = true
    }

    func refresh(
        completion: (([BIOSLibraryEntry]) -> Void)? = nil
    ) {
        if let completion { refreshCompletions.append(completion) }
        guard refreshTask == nil else {
            needsRefresh = true
            return
        }
        needsRefresh = false
        refreshTask = Task { @MainActor [weak self] in
            // BIOS validation reads imported files, so keep it away from tab
            // and focus animations.
            let loadedEntries = await Task.detached(priority: .userInitiated) {
                ARMSX2Bridge.availableBIOSInfos().map(BIOSLibraryEntry.init)
            }.value
            guard let self, !Task.isCancelled else { return }
            let loadedDefault = ARMSX2Bridge.defaultBIOSName()
            entries = loadedEntries
            defaultBIOS = loadedDefault
            hasLoaded = true
            refreshTask = nil
            if defaultBIOS.isEmpty,
               let firstValid = loadedEntries.first(where: \.valid) {
                select(firstValid)
            }
            if needsRefresh {
                refresh()
                return
            }
            let completions = refreshCompletions
            refreshCompletions.removeAll(keepingCapacity: true)
            completions.forEach { $0(loadedEntries) }
        }
    }

    func select(_ entry: BIOSLibraryEntry) {
        guard entry.valid else { return }
        ARMSX2Bridge.setDefaultBIOS(entry.fileName)
        defaultBIOS = entry.fileName
    }
}

private let biosControllerScope = "menu.bios"

private enum BIOSControllerTarget {
    static let boot = "bios.toolbar.boot"
    static let importBIOS = "bios.toolbar.import"
    static let refresh = "bios.toolbar.refresh"
    static let toolbar = [boot, importBIOS, refresh]
    static let emptyImport = "bios.content.import"
}

/// Tells the page which toolbar item has focus, so Back there returns to the list.
private struct BIOSToolbarFocusReporter: ViewModifier {
    let id: String
    @Binding var focusedID: String?
    @Environment(\.controllerAccessibilityTargetFocused) private var isFocused

    func body(content: Content) -> some View {
        content.onChange(of: isFocused, initial: true) { _, focused in
            if focused {
                focusedID = id
            } else if focusedID == id {
                focusedID = nil
            }
        }
    }
}

private struct BIOSPromptCommandListener: View {
    let controllerInput: MenuControllerInputRouter
    let onCommand: (MenuControllerCommand) -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput.latestEvent) { _, event in
                guard let event,
                      event.captureOwner
                        == MenuControllerNavigationCaptureOwner.biosPrompt else {
                    return
                }
                onCommand(event.command)
            }
    }
}

private struct BIOSLoadingView: View {
    var body: some View {
        ProgressView()
            .controlSize(.large)
            .accessibilityLabel("Loading BIOS")
    }
}

private enum BIOSPromptKind: Equatable {
    case replaceFiles
    case restart
}

struct BIOSListView: View {
    let embeddedInMenuNavigation: Bool
    let ownsEmbeddedMenuToolbar: Bool
    let controllerInput: MenuControllerInputRouter?
    let onPreviousControllerTab: @MainActor () -> Bool
    let onNextControllerTab: @MainActor () -> Bool
    let onControllerBoundary: @MainActor (MenuControllerCommand) -> Bool

    init(
        embeddedInMenuNavigation: Bool = false,
        ownsEmbeddedMenuToolbar: Bool = true,
        controllerInput: MenuControllerInputRouter? = nil,
        onPreviousControllerTab: @escaping @MainActor () -> Bool = { false },
        onNextControllerTab: @escaping @MainActor () -> Bool = { false },
        onControllerBoundary: @escaping @MainActor (MenuControllerCommand) -> Bool = { _ in false }
    ) {
        self.embeddedInMenuNavigation = embeddedInMenuNavigation
        self.ownsEmbeddedMenuToolbar = ownsEmbeddedMenuToolbar
        self.controllerInput = controllerInput
        self.onPreviousControllerTab = onPreviousControllerTab
        self.onNextControllerTab = onNextControllerTab
        self.onControllerBoundary = onControllerBoundary
    }

    @State private var library = BIOSLibraryState.shared
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBIOSImporter = false
    @State private var showBIOSCompatibilityImporter = false
    @State private var showBIOSReplacementAlert = false
    @State private var showRestartAlert = false
    @State private var promptSelectedIndex = 0
    @State private var pendingBIOSImportURLs: [URL] = []
    @State private var existingBIOSImportFileNames: [String] = []
    @State private var BIOSRefreshTask: Task<Void, Never>?
    @State private var appState = AppState.shared
    @State private var focusedToolbarTargetID: String?
    @Environment(\.menuTabIsActive) private var menuTabIsActive
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @Environment(\.uiAccentColour) private var accentColour
    @Environment(\.uiCardTitleColour) private var contentTextColour
    @Environment(\.uiCardSubtitleColour) private var secondaryTextColour

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

    private var biosRightStickScrollTarget: some View {
        ControllerRightStickScrollTarget(
            controllerInput: controllerInput,
            axes: .vertical,
            priority: 90,
            isEnabled: menuTabIsActive,
            searchesNearbyScrollViews: true
        )
        .frame(height: 0)
    }

    private var bioses: [BIOSLibraryEntry] {
        library.entries
    }

    private var biosControllerContentIDs: [String] {
        guard library.hasLoaded else { return [] }
        return bioses.isEmpty
            ? [BIOSControllerTarget.emptyImport]
            : bioses.map(\.filePath)
    }

    var body: some View {
        OptionalMenuNavigationStack(embedded: embeddedInMenuNavigation) {
            ZStack {
                if backgroundConfigured {
                    MenuBackgroundLayer(isActive: menuTabIsActive)
                }

                if !library.hasLoaded {
                    BIOSLoadingView()
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if bioses.isEmpty {
                    GeometryReader { geometry in
                        ScrollView {
                            VStack(spacing: 0) {
                                biosRightStickScrollTarget

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
                        // The persistent tab bar already shortens this
                        // viewport through safeAreaInset. Do not inherit the
                        // menu-wide bottom scroll margin as an additional
                        // controller-navigable blank region.
                        .contentMargins(.bottom, 0, for: .scrollContent)
                        .scrollBounceBehavior(.always)
                    }
                } else {
                    List {
                        biosRightStickScrollTarget
                            .listRowInsets(EdgeInsets())
                            .listRowSeparator(.hidden)
                            .listRowBackground(Color.clear)

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
                                .id(bios.filePath)
                                .gameCardTintMenuBackgroundListRow(backgroundActive)
                        }
                    }
                    .contentMargins(.top, 0, for: .scrollContent)
                    .contentMargins(.bottom, 0, for: .scrollContent)
                    .scrollContentBackground(backgroundActive ? .hidden : .automatic)
                    .scrollBounceBehavior(.always)
#if targetEnvironment(macCatalyst)
                    .listStyle(.inset)
#endif
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }

                if let prompt = activePrompt {
                    ControllerNavigationAlert(
                        title: promptTitle(prompt),
                        titleColour: prompt == .restart ? .white : nil,
                        message: promptMessage(prompt),
                        actions: promptActions(prompt),
                        selectedIndex: promptSelectedIndex,
                        onSelect: { performPromptAction($0, prompt: prompt) },
                        onDismiss: { dismissPrompt(prompt) }
                    )
                    // The prompt answers through its own capture, not the page's navigation.
                    .environment(\.controllerAccessibilityNavigationActive, false)
                    .environment(\.controllerAccessibilityNavigationSession, nil)
                    .overlay {
                        if let controllerInput {
                            BIOSPromptCommandListener(
                                controllerInput: controllerInput
                            ) { handlePromptCommand($0, prompt: prompt) }
                        }
                    }
                    .zIndex(20_000)
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
                    Button(action: performBootBIOSAction) {
                        Text(settings.localized("Boot BIOS"))
                            .font(.callout.weight(.semibold))
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                            .padding(.horizontal, 14)
                            .frame(minWidth: 112, minHeight: 36)
                            .controllerAccessibilityToolbarFocusVisual()
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Boot BIOS"))
                    .modifier(
                        BIOSToolbarFocusReporter(
                            id: BIOSControllerTarget.boot,
                            focusedID: $focusedToolbarTargetID
                        )
                    )
                    .controllerAccessibilityActionTarget(
                        id: BIOSControllerTarget.boot,
                        label: BIOSControllerTarget.boot,
                        action: performBootBIOSAction
                    )
                    .fixedSize()
                }
                ToolbarItem(id: "menu.import", placement: .topBarTrailing) {
                    Menu {
                        Button {
                            presentMenuPanel("bios_import") {
                                NSLog("[ARMSX2 iOS BIOS] opening primary BIOS picker")
                                openBIOSImporter()
                            }
                        } label: {
                            Label(settings.localized("Import BIOS"), systemImage: "doc.badge.plus")
                        }
                        Button {
                            presentMenuPanel("bios_compatibility_import") {
                                NSLog("[ARMSX2 iOS BIOS] opening compatibility BIOS picker")
                                openCompatibilityBIOSImporter()
                            }
                        } label: {
                            Label(settings.localized("Compatibility Picker"), systemImage: "folder.badge.plus")
                        }
                    } label: {
                        Image(systemName: "plus")
                            .frame(minWidth: 36, minHeight: 36)
                            .controllerAccessibilityToolbarFocusVisual()
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Import BIOS"))
                    .simultaneousGesture(
                        TapGesture().onEnded {
                            MenuAudioPackManager.shared.playEvent(.contextMenu)
                        }
                    )
                    .modifier(
                        BIOSToolbarFocusReporter(
                            id: BIOSControllerTarget.importBIOS,
                            focusedID: $focusedToolbarTargetID
                        )
                    )
                    .controllerAccessibilityActionTarget(
                        id: BIOSControllerTarget.importBIOS,
                        label: BIOSControllerTarget.importBIOS,
                        action: openBIOSImporter
                    )
                }
                ToolbarItem(id: "menu.refresh", placement: .topBarTrailing) {
                    Button { loadBIOSes() } label: {
                        Image(systemName: "arrow.clockwise")
                            .menuToolbarMorphElement("refresh")
                            .frame(minWidth: 36, minHeight: 36)
                            .controllerAccessibilityToolbarFocusVisual()
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Refresh"))
                    .modifier(
                        BIOSToolbarFocusReporter(
                            id: BIOSControllerTarget.refresh,
                            focusedID: $focusedToolbarTargetID
                        )
                    )
                    .controllerAccessibilityActionTarget(
                        id: BIOSControllerTarget.refresh,
                        label: BIOSControllerTarget.refresh
                    ) { loadBIOSes() }
                }
                }
            }
            .tint(settings.controllerToolbarColor)
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
                    allowsMultipleSelection: true,
                    legacyDocumentTypes: ["public.item", "public.data", "public.content"]
                ) { result in
                    showBIOSCompatibilityImporter = false
                    handleBIOSPickerResult(result, source: "compatibility")
                }
            }
            .alert(
                settings.localized("Replace existing files?"),
                isPresented: Binding(
                    get: { false },
                    set: { if !$0 { showBIOSReplacementAlert = false } }
                )
            ) {
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
            .alert(
                settings.localized("Restart VM?"),
                isPresented: Binding(
                    get: { false },
                    set: { if !$0 { showRestartAlert = false } }
                )
            ) {
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Restart"), role: .destructive) {
                    appState.shutdownAndBootBIOS()
                }
            } message: {
                Text(String(format: settings.localized("VM is currently running.\nShut down and start %@?"), settings.localized("Boot BIOS")))
            }
        }
        .onAppear {
            if menuTabIsActive {
                scheduleBIOSRefresh(after: .milliseconds(0))
            }
        }
        .onChange(of: menuTabIsActive) { _, isActive in
            if isActive {
                scheduleBIOSRefresh(after: .milliseconds(0))
            }
        }
        .onChange(of: activePrompt) { previous, prompt in
            promptSelectedIndex = 0
            updatePromptCapture()
            guard let prompt, prompt != previous else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .onReceive(NotificationCenter.default.publisher(for: InitialContentBootstrap.didChangeNotification)) { _ in
            library.markNeedsRefresh()
            if menuTabIsActive {
                scheduleBIOSRefresh(after: .milliseconds(120))
            }
        }
        .onDisappear {
            BIOSRefreshTask?.cancel()
            BIOSRefreshTask = nil
            controllerInput?.setNavigationCaptured(
                false,
                owner: MenuControllerNavigationCaptureOwner.biosPrompt,
                priority: 1_000
            )
        }
        .environment(\.controllerAccessibilityTargetsSuppressed, !library.hasLoaded)
        .controllerAccessibilityTargetOrder(biosControllerContentIDs)
        .controllerAccessibilityNavigation(
            controllerInput: controllerInput,
            isActive: menuTabIsActive,
            scopeKey: biosControllerScope,
            priority: 80,
            orbStyle: .plain,
            focusNeonExclusionLabels: BIOSControllerTarget.toolbar,
            onBack: handleControllerBack,
            onPreviousTab: onPreviousControllerTab,
            onNextTab: onNextControllerTab,
            onBoundary: onControllerBoundary,
            directionalLinks: biosControllerLinks,
            confinesHorizontalFocusMovement: true,
            usesExplicitTargetGeometryOnly: true
        )
    }

    // Back from the toolbar returns to the list. Back from the list leaves it for
    // the tab bar, where Up comes back in.
    private func handleControllerBack() -> Bool {
        guard focusedToolbarTargetID != nil else {
            return onControllerBoundary(.down)
        }
        return controllerInput?.requestNavigationSessionEntry(
            preferLast: false,
            matchingScopePrefix: biosControllerScope
        ) ?? false
    }

    private var biosControllerLinks: [ControllerAccessibilityDirectionalLink] {
        typealias Link = ControllerAccessibilityDirectionalLink
        let target = BIOSControllerTarget.self
        var links = [
            Link(fromLabel: target.boot, direction: .right, toLabel: target.importBIOS),
            Link(fromLabel: target.importBIOS, direction: .left, toLabel: target.boot),
            Link(fromLabel: target.importBIOS, direction: .right, toLabel: target.refresh),
            Link(fromLabel: target.refresh, direction: .left, toLabel: target.importBIOS),
        ]
        for id in target.toolbar {
            links.append(Link(fromLabel: id, direction: .up, toLabel: Link.navigationBoundary))
        }
        if let first = biosControllerContentIDs.first,
           let last = biosControllerContentIDs.last {
            links.append(Link(fromLabel: first, direction: .up, toLabel: target.boot))
            links.append(Link(fromLabel: last, direction: .down, toLabel: Link.navigationBoundary))
            for id in target.toolbar {
                links.append(Link(fromLabel: id, direction: .down, toLabel: first))
            }
        }
        return links
    }

    private func updatePromptCapture() {
        if activePrompt != nil {
            controllerInput?.claimPresentedNavigationFocus()
        }
        controllerInput?.setNavigationCaptured(
            activePrompt != nil,
            owner: MenuControllerNavigationCaptureOwner.biosPrompt,
            priority: 1_000
        )
    }

    private var activePrompt: BIOSPromptKind? {
        if showBIOSReplacementAlert { return .replaceFiles }
        if showRestartAlert { return .restart }
        return nil
    }

    private func promptTitle(_ prompt: BIOSPromptKind) -> String {
        switch prompt {
        case .replaceFiles:
            settings.localized("Replace existing files?")
        case .restart:
            settings.localized("Restart VM?")
        }
    }

    private func promptMessage(_ prompt: BIOSPromptKind) -> String {
        switch prompt {
        case .replaceFiles:
            FileImportHandler.replacementConfirmationMessage(
                for: existingBIOSImportFileNames
            )
        case .restart:
            "\(settings.localized("VM is currently running."))\n"
                + "\(settings.localized("Shut down and start")) "
                + "\(settings.localized("Boot BIOS"))?"
        }
    }

    private func promptActions(
        _ prompt: BIOSPromptKind
    ) -> [ControllerNavigationAlertAction] {
        switch prompt {
        case .replaceFiles:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(
                    id: "replace",
                    title: settings.localized("Replace"),
                    isDestructive: true
                ),
            ]
        case .restart:
            [
                .init(id: "cancel", title: settings.localized("Cancel")),
                .init(
                    id: "restart",
                    title: settings.localized("Restart"),
                    isDestructive: true
                ),
            ]
        }
    }

    private func performPromptAction(
        _ index: Int,
        prompt: BIOSPromptKind
    ) {
        guard index == 1 else {
            dismissPrompt(prompt)
            return
        }
        switch prompt {
        case .replaceFiles:
            importBIOSFiles(
                pendingBIOSImportURLs,
                allowReplacingExistingFiles: true
            )
            clearPendingBIOSImport()
            showBIOSReplacementAlert = false
        case .restart:
            showRestartAlert = false
            appState.shutdownAndBootBIOS()
        }
    }

    private func dismissPrompt(_ prompt: BIOSPromptKind) {
        switch prompt {
        case .replaceFiles:
            showBIOSReplacementAlert = false
            clearPendingBIOSImport()
        case .restart:
            showRestartAlert = false
        }
    }

    private func handlePromptCommand(
        _ command: MenuControllerCommand,
        prompt: BIOSPromptKind
    ) {
        let finalIndex = promptActions(prompt).index(before: promptActions(prompt).endIndex)
        switch command {
        case .up, .upLeft, .upRight, .left:
            let next = max(0, promptSelectedIndex - 1)
            guard next != promptSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            promptSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .down, .downLeft, .downRight, .right:
            let next = min(finalIndex, promptSelectedIndex + 1)
            guard next != promptSelectedIndex else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            promptSelectedIndex = next
            controllerInput?.playFeedback(.move(command))
        case .activate:
            performPromptAction(promptSelectedIndex, prompt: prompt)
            controllerInput?.playFeedback(.activate)
        case .back:
            dismissPrompt(prompt)
            controllerInput?.playFeedback(.back)
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            controllerInput?.playFeedback(.boundary)
        }
    }

    private func presentMenuPanel(_ name: String, _ action: @escaping () -> Void) {
        NSLog("[ARMSX2 iOS BIOSMenu] present \(name)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            action()
        }
    }

    private func biosRow(_ bios: BIOSLibraryEntry) -> some View {
        Button {
            selectBIOS(bios)
        } label: {
            HStack(spacing: 12) {
                regionBadge(for: bios)

                VStack(alignment: .leading, spacing: 4) {
                    Text(bios.fileName)
                        .font(.body)
                    Text(bios.valid ? "\(bios.regionName) BIOS" : settings.localized("Not a boot BIOS"))
                        .font(.caption)
                        .foregroundStyle(secondaryTextColour)
                    if bios.valid && !bios.descriptionText.isEmpty {
                        Text(bios.descriptionText)
                            .font(.caption2)
                            .foregroundStyle(secondaryTextColour.opacity(0.78))
                            .lineLimit(1)
                    } else if !bios.valid {
                        Text(settings.localized("Companion ROM or unsupported BIOS dump"))
                            .font(.caption2)
                            .foregroundStyle(secondaryTextColour.opacity(0.78))
                            .lineLimit(1)
                    }
                }
                Spacer()
                if bios.fileName == library.defaultBIOS {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(accentColour)
                }
            }
        }
        .buttonStyle(.plain)
        .foregroundStyle(contentTextColour)
        .opacity(bios.valid ? 1 : 0.65)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(biosControllerLabel(for: bios))
        .controllerAccessibilityActionTarget(
            id: bios.filePath,
            label: biosControllerLabel(for: bios)
        ) { selectBIOS(bios) }
    }

    private func selectBIOS(_ bios: BIOSLibraryEntry) {
        if bios.valid {
            library.select(bios)
        } else {
            fileImporter.lastImportMessage = "\(bios.fileName) is visible in your BIOS folder, but it is not a bootable PS2 BIOS. Keep it if it is a companion ROM, and select a valid boot BIOS as default."
            fileImporter.showImportAlert = true
        }
    }

    private func biosControllerLabel(for bios: BIOSLibraryEntry) -> String {
        if bios.valid {
            return "\(bios.fileName), \(bios.regionName) BIOS"
        }
        return "\(bios.fileName), \(settings.localized("Not a boot BIOS"))"
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "cpu")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text(settings.localized("No BIOS Found"))
                .font(.title2)
                .fontWeight(.semibold)
                .foregroundStyle(contentTextColour)
            Text(settings.localized("Import a PS2 BIOS dump to enable booting."))
                .font(.body)
                .foregroundStyle(secondaryTextColour)
                .multilineTextAlignment(.center)
            Button {
                NSLog("[ARMSX2 iOS BIOS] opening primary BIOS picker from empty state")
                openBIOSImporter()
            } label: {
                MenuImportActionLabel(
                    title: settings.localized("Import BIOS")
                )
            }
            .buttonStyle(.plain)
            .focusable(false)
            .focusEffectDisabled()
            .accessibilityElement(children: .combine)
            .accessibilityLabel(settings.localized("Import BIOS"))
            .controllerAccessibilityActionTarget(
                id: BIOSControllerTarget.emptyImport,
                label: settings.localized("Import BIOS"),
                activationFeedback: .destination,
                action: openBIOSImporter
            )
            Text(settings.localized("If one picker refuses to select your .bin/.rom file, try the other."))
                .font(.caption)
                .foregroundStyle(secondaryTextColour.opacity(0.78))
                .multilineTextAlignment(.center)
        }
        .padding()
        .frame(maxWidth: .infinity, minHeight: 240)
    }

    private func performBootBIOSAction() {
        if appState.runningGameName == "BIOS" {
            appState.returnToGame()
        } else if appState.runningGameName != nil {
            showRestartAlert = true
        } else {
            appState.bootBIOSOnly()
        }
    }

    private func openBIOSImporter() {
        MenuAudioPackManager.shared.playEvent(.contextMenu)
        showBIOSImporter = true
    }

    private func openCompatibilityBIOSImporter() {
        MenuAudioPackManager.shared.playEvent(.contextMenu)
        showBIOSCompatibilityImporter = true
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
        let importMessage = fileImporter.lastImportMessage
        loadBIOSes { refreshedEntries in
            if let guidance = nonBootableImportGuidance(for: urls) {
                let message = [importMessage, guidance]
                    .compactMap { $0 }
                    .joined(separator: "\n")
                fileImporter.presentImportResult(message)
            } else if !refreshedEntries.contains(where: { $0.valid }),
                      !urls.isEmpty {
                let message = [
                    importMessage,
                    "No bootable PS2 BIOS was found. Import a valid PS2 BIOS dump before starting games."
                ]
                .compactMap { $0 }
                .joined(separator: "\n")
                fileImporter.presentImportResult(message)
            }
        }
    }

    private func clearPendingBIOSImport() {
        pendingBIOSImportURLs = []
        existingBIOSImportFileNames = []
    }

    private func loadBIOSes(
        completion: (([BIOSLibraryEntry]) -> Void)? = nil
    ) {
        library.refresh(completion: completion)
    }

    /// Coalesces bootstrap notifications and avoids opening every BIOS file
    /// while the retained BIOS page is hidden behind another tab.
    private func scheduleBIOSRefresh(after delay: Duration) {
        BIOSRefreshTask?.cancel()
        BIOSRefreshTask = Task { @MainActor in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, menuTabIsActive else {
                library.markNeedsRefresh()
                return
            }
            library.refreshIfNeeded()
            BIOSRefreshTask = nil
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
    private func regionBadge(for bios: BIOSLibraryEntry) -> some View {
        if backgroundActive {
            badgeContent(for: bios)
                .frame(width: 44, height: 44)
                .glassSurface(clear: true, cornerRadius: 12)
                .accessibilityHidden(true)
        } else {
            badgeContent(for: bios)
                .frame(width: 44, height: 44)
                .background(
                    Color(.secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                )
                .accessibilityHidden(true)
        }
    }

    @ViewBuilder
    private func badgeContent(for bios: BIOSLibraryEntry) -> some View {
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
