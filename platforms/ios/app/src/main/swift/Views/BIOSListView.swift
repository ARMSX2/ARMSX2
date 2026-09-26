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

private enum BIOSControllerToolbarAction: Int, CaseIterable {
    case boot
    case importBIOS
    case refresh
}

private let biosControllerContentMemoryScope = "menu.bios.content"
private let biosControllerToolbarMemoryScope = "menu.bios.toolbar"

@MainActor
@Observable
private final class BIOSControllerToolbarFocusState {
    var selectedAction: BIOSControllerToolbarAction = .boot
}

@MainActor
@Observable
private final class BIOSControllerItemFocusState {
    var isFocused = false
}

@MainActor
@Observable
private final class BIOSControllerFocusState {
    @ObservationIgnored private var storedSelectedID: String?
    @ObservationIgnored private var itemStates: [String: BIOSControllerItemFocusState] = [:]
    @ObservationIgnored private var isEnabled = false
    @ObservationIgnored private var isLibraryZone = true
    @ObservationIgnored private var isVisible = true
    @ObservationIgnored private var selectionChangeHandler: ((String?) -> Void)?

    var selectedID: String? {
        get { storedSelectedID }
        set {
            guard storedSelectedID != newValue else { return }
            let previous = storedSelectedID
            storedSelectedID = newValue
            refresh(previous)
            refresh(newValue)
            selectionChangeHandler?(newValue)
        }
    }

    func setSelectionChangeHandler(_ handler: ((String?) -> Void)?) {
        selectionChangeHandler = handler
        if let handler {
            handler(storedSelectedID)
        }
    }

    func setEnabled(_ enabled: Bool) {
        guard isEnabled != enabled else { return }
        isEnabled = enabled
        refresh(storedSelectedID)
    }

    func setLibraryZoneActive(_ active: Bool) {
        guard isLibraryZone != active else { return }
        isLibraryZone = active
        refresh(storedSelectedID)
    }

    func setVisible(_ visible: Bool) {
        guard isVisible != visible else { return }
        isVisible = visible
        refresh(storedSelectedID)
    }

    func updateIDs(_ ids: [String]) {
        let retained = Set(ids)
        itemStates = itemStates.filter { retained.contains($0.key) }
        if let storedSelectedID, !retained.contains(storedSelectedID) {
            selectedID = nil
        }
    }

    func itemState(for id: String) -> BIOSControllerItemFocusState {
        if let state = itemStates[id] { return state }
        let state = BIOSControllerItemFocusState()
        itemStates[id] = state
        state.isFocused = isFocused(id)
        return state
    }

    private func isFocused(_ id: String) -> Bool {
        isEnabled && isLibraryZone && isVisible && storedSelectedID == id
    }

    private func refresh(_ id: String?) {
        guard let id, let state = itemStates[id] else { return }
        state.isFocused = isFocused(id)
    }
}

private struct BIOSControllerItemFocusModifier: ViewModifier {
    let id: String
    let state: BIOSControllerItemFocusState
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func body(content: Content) -> some View {
        content
            .focusEffectDisabled()
            .background {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(state.isFocused ? Color.black.opacity(0.2) : .clear)
            }
            // The outer row owns Liquid Glass. Publish the pill-shaped depth
            // anchor to that surface, and keep only the foreground pill in
            // this content layer.
            .controllerFocusDepthAnchor(
                id: "bios.focus.\(id)",
                isFocused: state.isFocused
            )
            .controllerFocusBoxForegroundPresentation(
                isVisible: state.isFocused,
                cornerRadius: 12
            )
            .controllerNavigationOrbTarget(
                id: "bios.focus.\(id)",
                isActive: state.isFocused,
                palette: .blue,
                style: .plain,
                inset: 2,
                orbScale: 0.78,
                priority: 20
            )
            .scaleEffect(state.isFocused && !reduceMotion ? 1.01 : 1)
            .shadow(
                color: .black.opacity(state.isFocused ? 0.1 : 0),
                radius: state.isFocused ? 4 : 0,
                y: state.isFocused ? 2 : 0
            )
            .zIndex(state.isFocused ? 20 : 0)
            .animation(
                reduceMotion
                    ? .linear(duration: 0.1)
                    : .smooth(duration: 0.16, extraBounce: 0),
                value: state.isFocused
            )
    }
}

private struct BIOSControllerFocusedTextModifier: ViewModifier {
    let state: BIOSControllerItemFocusState
    @Environment(\.controllerTextAppearance) private var textAppearance
    @Environment(\.uiCardTitleColour) private var contentTextColour

    func body(content: Content) -> some View {
        content
            .foregroundStyle(
                state.isFocused
                    ? textAppearance.focusedColor
                    : contentTextColour
            )
            .shadow(
                color: .black.opacity(state.isFocused ? 0.1 : 0),
                radius: state.isFocused ? 4 : 0,
                y: state.isFocused ? 2 : 0
            )
            .animation(
                ControllerFocusVisualAnimation.textFade,
                value: state.isFocused
            )
    }
}

private struct BIOSControllerCommandListener: View {
    let controllerInput: MenuControllerInputRouter?
    let focusState: BIOSControllerFocusState
    let isVisible: Bool
    let onCommand: (MenuControllerInputEvent) -> Void
    let onLibraryEntry: (Bool) -> Void
    let onAvailabilityChanged: () -> Void

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .onChange(of: controllerInput?.latestEvent) { _, event in
                guard let event else { return }
                onCommand(event)
            }
            .onChange(of: controllerInput?.latestLibraryEntryRequest) { _, request in
                guard let request else { return }
                onLibraryEntry(request.preferLast)
            }
            .onChange(of: controllerInput?.focusReleaseSequence) { _, _ in
                focusState.setEnabled(false)
                focusState.selectedID = nil
            }
            .onChange(of: controllerInput?.navigationZone, initial: true) { _, zone in
                focusState.setLibraryZoneActive(zone == .library)
                onAvailabilityChanged()
            }
            .onChange(
                of: controllerInput?.isControllerNavigationEnabled,
                initial: true
            ) { _, _ in
                onAvailabilityChanged()
            }
            .onChange(of: isVisible, initial: true) { _, visible in
                focusState.setVisible(visible)
                onAvailabilityChanged()
            }
    }
}

private struct BIOSControllerToolbarFocusModifier: ViewModifier {
    let state: BIOSControllerToolbarFocusState
    let action: BIOSControllerToolbarAction
    let controllerInput: MenuControllerInputRouter?
    let isVisible: Bool
    var baseHorizontalPadding: CGFloat = 0
    var minimumWidth: CGFloat = 36
    var foregroundColour: Color? = nil

    func body(content: Content) -> some View {
        let isFocused = isVisible
            && controllerInput?.isControllerNavigationEnabled == true
            && controllerInput?.navigationZone == .topToolbar
            && state.selectedAction == action
        content.modifier(
            ControllerToolbarFocusVisualModifier(
                isFocused: isFocused,
                focusID: "bios.toolbar.\(action.rawValue)",
                baseHorizontalPadding: baseHorizontalPadding,
                minimumWidth: minimumWidth,
                foregroundColour: foregroundColour
            )
        )
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
    @State private var controllerFocusState = BIOSControllerFocusState()
    @State private var controllerToolbarFocusState =
        BIOSControllerToolbarFocusState()
    @State private var controllerEntryPrefersLast = false
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
            isEnabled: controllerLibraryNavigationAllowed
                && controllerInput?.navigationZone == .library,
            onReachedLeadingEdge: controllerRightStickReachedTop,
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
            ? ["bios.content.import"]
            : bioses.map(\.filePath)
    }

    private var controllerLibraryNavigationAllowed: Bool {
        menuTabIsActive
            && appState.currentScreen == .menu
            && (controllerInput?.isControllerNavigationEnabled ?? false)
            && !(controllerInput?.isNavigationCaptured ?? false)
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
                    ScrollViewReader { proxy in
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
                        .onAppear {
                            controllerFocusState.setSelectionChangeHandler { id in
                                guard let id else { return }
                                let acceleration = controllerInput?
                                    .directionalRepeatAcceleration ?? 1
                                let duration = controllerInput?
                                    .isRepeatingDirectionCommand == true
                                    ? max(0.04, 0.12 / acceleration)
                                    : 0.18
                                withAnimation(
                                    .smooth(duration: duration, extraBounce: 0)
                                ) {
                                    proxy.scrollTo(id, anchor: .center)
                                }
                                controllerInput?.rememberNavigationFocusKey(
                                    id,
                                    forScope: biosControllerContentMemoryScope
                                )
                            }
                        }
                        .onDisappear {
                            controllerFocusState.setSelectionChangeHandler(nil)
                        }
                    }
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
                    .zIndex(20_000)
                }
            }
            .stableMenuContentGlassContainer()
            .overlay {
                BIOSControllerCommandListener(
                    controllerInput: controllerInput,
                    focusState: controllerFocusState,
                    isVisible: menuTabIsActive,
                    onCommand: handleControllerCommand,
                    onLibraryEntry: handleControllerLibraryEntry,
                    onAvailabilityChanged: synchronizeControllerAvailability
                )
            }
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
                            .modifier(
                                biosControllerToolbarFocus(
                                    .boot,
                                    baseHorizontalPadding: 14,
                                    minimumWidth: 112
                                )
                            )
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Boot BIOS"))
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
                            .modifier(biosControllerToolbarFocus(.importBIOS))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Import BIOS"))
                    .simultaneousGesture(
                        TapGesture().onEnded {
                            MenuAudioPackManager.shared.playEvent(.contextMenu)
                        }
                    )
                }
                ToolbarItem(id: "menu.refresh", placement: .topBarTrailing) {
                    Button { loadBIOSes() } label: {
                        Image(systemName: "arrow.clockwise")
                            .menuToolbarMorphElement("refresh")
                            .modifier(
                                biosControllerToolbarFocus(.refresh)
                            )
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(settings.localized("Refresh"))
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
                    allowedContentTypes: FileImportHandler.biosContentTypes,
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
            restoreControllerToolbarFocus()
            if menuTabIsActive {
                scheduleBIOSRefresh(after: .milliseconds(0))
            }
            synchronizeControllerItems()
            synchronizeControllerAvailability()
        }
        .onChange(of: menuTabIsActive) { _, isActive in
            if isActive {
                restoreControllerToolbarFocus()
                scheduleBIOSRefresh(after: .milliseconds(0))
            }
            synchronizeControllerAvailability()
        }
        .onChange(of: controllerInput?.navigationZone) { _, zone in
            if zone == .topToolbar {
                rememberControllerToolbarFocus()
            }
        }
        .onChange(of: controllerToolbarFocusState.selectedAction) { _, _ in
            if controllerInput?.navigationZone == .topToolbar {
                rememberControllerToolbarFocus()
            }
        }
        .onChange(of: activePrompt) { previous, prompt in
            promptSelectedIndex = 0
            guard let prompt, prompt != previous else { return }
            MenuAudioPackManager.shared.playEvent(.uiToast)
        }
        .onChange(of: bioses) { _, _ in
            synchronizeControllerItems()
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
        }
    }

    private func biosControllerToolbarFocus(
        _ action: BIOSControllerToolbarAction,
        baseHorizontalPadding: CGFloat = 0,
        minimumWidth: CGFloat = 36,
        foregroundColour: Color? = nil
    ) -> BIOSControllerToolbarFocusModifier {
        BIOSControllerToolbarFocusModifier(
            state: controllerToolbarFocusState,
            action: action,
            controllerInput: controllerInput,
            isVisible: menuTabIsActive,
            baseHorizontalPadding: baseHorizontalPadding,
            minimumWidth: minimumWidth,
            foregroundColour: foregroundColour
        )
    }

    private func synchronizeControllerItems() {
        controllerFocusState.updateIDs(biosControllerContentIDs)
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library,
              controllerFocusState.selectedID == nil else { return }
        _ = selectInitialControllerItem(preferLast: controllerEntryPrefersLast)
    }

    private func synchronizeControllerAvailability() {
        controllerFocusState.setEnabled(controllerLibraryNavigationAllowed)
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library,
              controllerFocusState.selectedID == nil else { return }
        _ = selectInitialControllerItem(preferLast: controllerEntryPrefersLast)
    }

    private func handleControllerLibraryEntry(preferLast: Bool) {
        guard controllerLibraryNavigationAllowed else { return }
        controllerEntryPrefersLast = preferLast
        controllerFocusState.setEnabled(true)
        _ = selectInitialControllerItem(preferLast: preferLast)
    }

    @discardableResult
    private func selectInitialControllerItem(preferLast: Bool) -> Bool {
        if let rememberedID = controllerInput?.rememberedNavigationFocusKey(
            forScope: biosControllerContentMemoryScope
        ) {
            if biosControllerContentIDs.contains(rememberedID) {
                controllerFocusState.selectedID = rememberedID
                return true
            }
        }
        guard let id = preferLast
                ? biosControllerContentIDs.last
                : biosControllerContentIDs.first else { return false }
        controllerFocusState.selectedID = id
        return true
    }

    private func rememberControllerToolbarFocus() {
        controllerInput?.rememberNavigationFocusKey(
            String(controllerToolbarFocusState.selectedAction.rawValue),
            forScope: biosControllerToolbarMemoryScope
        )
    }

    private func restoreControllerToolbarFocus() {
        guard let rememberedAction = controllerInput?.rememberedNavigationFocusKey(
            forScope: biosControllerToolbarMemoryScope
        ), let rawValue = Int(rememberedAction),
           let action = BIOSControllerToolbarAction(rawValue: rawValue) else {
            return
        }
        controllerToolbarFocusState.selectedAction = action
    }

    private func controllerRightStickReachedTop() {
        guard controllerLibraryNavigationAllowed,
              controllerInput?.navigationZone == .library else { return }
        controllerFocusState.setEnabled(false)
        controllerInput?.setNavigationZone(.topToolbar)
        controllerInput?.playFeedback(.move(.up))
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

    private func handleControllerCommand(_ event: MenuControllerInputEvent) {
        guard !event.isNavigationSessionRouted,
              event.captureOwner == nil else { return }
        if let prompt = activePrompt {
            handlePromptCommand(event.command, prompt: prompt)
            return
        }
        guard controllerLibraryNavigationAllowed else { return }
        let command = event.command
        if controllerInput?.navigationZone == .tabBar { return }
        if controllerInput?.navigationZone == .topToolbar {
            handleControllerToolbarCommand(command)
            return
        }
        if controllerInput?.suppressesLibraryEvent(event) == true { return }

        let ids = biosControllerContentIDs
        guard !ids.isEmpty else { return }
        let index = controllerFocusState.selectedID.flatMap {
            ids.firstIndex(of: $0)
        }
            ?? (controllerEntryPrefersLast ? ids.index(before: ids.endIndex) : ids.startIndex)

        switch command {
        case .up, .upLeft, .upRight:
            guard index > ids.startIndex else {
                controllerFocusState.setEnabled(false)
                controllerInput?.setNavigationZone(.topToolbar)
                controllerInput?.playFeedback(.move(.up))
                return
            }
            controllerFocusState.selectedID = ids[index - 1]
            controllerInput?.playFeedback(.move(.up))
        case .down, .downLeft, .downRight:
            guard index < ids.index(before: ids.endIndex) else {
                controllerFocusState.setEnabled(false)
                if onControllerBoundary(.down) {
                    controllerInput?.playFeedback(.move(.down))
                } else {
                    controllerInput?.playFeedback(.boundary)
                }
                return
            }
            controllerFocusState.selectedID = ids[index + 1]
            controllerInput?.playFeedback(.move(.down))
        case .activate:
            activateControllerContent(id: ids[index])
        case .back:
            controllerFocusState.selectedID = nil
            controllerFocusState.setEnabled(false)
            controllerInput?.playFeedback(.back)
        case .left, .right, .toggleFavorite, .showContextMenu:
            controllerInput?.playFeedback(.boundary)
        case .previousTab, .nextTab:
            break
        }
    }

    private func handleControllerToolbarCommand(
        _ command: MenuControllerCommand
    ) {
        let actions = BIOSControllerToolbarAction.allCases
        let index = actions.firstIndex(
            of: controllerToolbarFocusState.selectedAction
        ) ?? actions.startIndex
        switch command {
        case .left:
            let next = max(actions.startIndex, index - 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerToolbarFocusState.selectedAction = actions[next]
            controllerInput?.playFeedback(.move(.left))
        case .right:
            let next = min(actions.index(before: actions.endIndex), index + 1)
            guard next != index else {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerToolbarFocusState.selectedAction = actions[next]
            controllerInput?.playFeedback(.move(.right))
        case .down, .downLeft, .downRight:
            controllerInput?.setNavigationZone(.library)
            controllerFocusState.setEnabled(true)
            if !selectInitialControllerItem(preferLast: false) {
                controllerInput?.playFeedback(.boundary)
                return
            }
            controllerInput?.playFeedback(.move(.down))
        case .up, .upLeft, .upRight:
            controllerInput?.playFeedback(.boundary)
        case .activate:
            performControllerToolbarAction(
                controllerToolbarFocusState.selectedAction
            )
            controllerInput?.playFeedback(.activate)
        case .back:
            controllerInput?.setNavigationZone(.library)
            controllerFocusState.setEnabled(true)
            _ = selectInitialControllerItem(preferLast: false)
            controllerInput?.playFeedback(.back)
        case .toggleFavorite, .showContextMenu, .previousTab, .nextTab:
            break
        }
    }

    private func performControllerToolbarAction(
        _ action: BIOSControllerToolbarAction
    ) {
        switch action {
        case .boot:
            performBootBIOSAction()
        case .importBIOS:
            openBIOSImporter()
        case .refresh:
            loadBIOSes()
        }
    }

    private func activateControllerContent(id: String) {
        if id == "bios.content.import" {
            openBIOSImporter()
            controllerInput?.playFeedback(.destination)
            return
        }
        guard let bios = bioses.first(where: { $0.filePath == id }) else {
            controllerInput?.playFeedback(.boundary)
            return
        }
        selectBIOS(bios)
        controllerInput?.playFeedback(.activate)
    }

    private func presentMenuPanel(_ name: String, _ action: @escaping () -> Void) {
        NSLog("[ARMSX2 iOS BIOSMenu] present \(name)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            action()
        }
    }

    private func biosRow(_ bios: BIOSLibraryEntry) -> some View {
        let focusState = controllerFocusState.itemState(for: bios.filePath)
        return Button {
            selectBIOS(bios)
        } label: {
            HStack(spacing: 12) {
                regionBadge(for: bios)

                VStack(alignment: .leading, spacing: 4) {
                    Text(bios.fileName)
                        .font(.body)
                        .modifier(
                            BIOSControllerFocusedTextModifier(state: focusState)
                        )
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
        // BIOS uses its own stable-ID controller graph. Keeping these rows in
        // UIKit's native focus graph as well can deliver Cross to the first
        // native row after the custom highlight has moved elsewhere.
        .focusable(false)
        .foregroundStyle(contentTextColour)
        .opacity(bios.valid ? 1 : 0.65)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(biosControllerLabel(for: bios))
        .modifier(
            BIOSControllerItemFocusModifier(
                id: bios.filePath,
                state: focusState
            )
        )
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
        let focusState = controllerFocusState.itemState(
            for: "bios.content.import"
        )
        return VStack(spacing: 16) {
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
            .modifier(
                BIOSControllerItemFocusModifier(
                    id: "bios.content.import",
                    state: focusState
                )
            )
            .id("bios.content.import")
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
