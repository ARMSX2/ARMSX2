// SkinInstaller.swift — downloads a skin zip and installs it via VPadSkinLibraryStore
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum SkinInstallError: LocalizedError {
    case unusableLink
    case tooLarge
    case emptyArchive

    var errorDescription: String? {
        switch self {
        case .unusableLink:
            return "This skin's download link is not usable."
        case .tooLarge:
            return "This skin is too large to install."
        case .emptyArchive:
            return "The download did not contain any usable skin files."
        }
    }
}

/// Downloads a skin zip from the catalog repo, extracts it via the bridge, and
/// installs through VPadSkinLibraryStore.importSkin(from: directoryURL).
@MainActor
final class SkinInstaller: ObservableObject {
    /// Keyed by the catalog file path, so two taps on different rows do not
    /// knock each other's spinner out.
    @Published private(set) var installing: Set<String> = []
    @Published var errors: [String: String] = [:]
    /// Warnings the import raised but which are not failures, keyed the same way.
    @Published var notices: [String: String] = [:]

    private static let maxDownloadBytes: Int64 = 32 * 1024 * 1024

    func install(_ skin: CatalogSkin) async {
        await install(skin, replacing: nil)
    }

    func reinstall(_ skin: CatalogSkin) async {
        await install(skin, replacing: installedDescriptor(for: skin)?.id)
    }

    func uninstall(_ skin: CatalogSkin) {
        errors[skin.file] = nil
        notices[skin.file] = nil
        guard let descriptor = installedDescriptor(for: skin) else { return }
        do {
            try VPadSkinLibraryStore.shared.deleteImportedSkin(id: descriptor.id)
            syncSelectedSkin()
        } catch {
            errors[skin.file] = error.localizedDescription
        }
    }

    private func install(_ skin: CatalogSkin, replacing replacingSkinID: String?) async {
        installing.insert(skin.file)
        errors[skin.file] = nil
        notices[skin.file] = nil
        do {
            let result = try await Self.installCatalogSkin(
                skin,
                replacing: replacingSkinID
            )
            if !result.warnings.isEmpty {
                notices[skin.file] = result.warnings.joined(separator: "\n")
            }
            syncSelectedSkin()
        } catch {
            errors[skin.file] = error.localizedDescription
        }
        installing.remove(skin.file)
    }

    /// The automatic serial matcher uses the same bounded download and import
    /// path as the catalog UI, but leaves the global skin selection untouched.
    static func installForAutomaticAssignment(
        _ skin: CatalogSkin,
        replacing replacingSkinID: String? = nil
    ) async throws -> VPadSkinImportResult {
        try await installCatalogSkin(skin, replacing: replacingSkinID)
    }

    private static func installCatalogSkin(
        _ skin: CatalogSkin,
        replacing replacingSkinID: String?
    ) async throws -> VPadSkinImportResult {
        guard let zipURL = SkinCatalog.zipURL(for: skin) else {
            throw SkinInstallError.unusableLink
        }
        let (tempURL, response) = try await URLSession.shared.download(from: zipURL)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw URLError(.badServerResponse)
        }

        // Give the temporary file a trusted .zip suffix so the bridge recognises it.
        let zipFile = FileManager.default.temporaryDirectory
            .appendingPathComponent("skin-download-\(UUID().uuidString).zip")
        try FileManager.default.moveItem(at: tempURL, to: zipFile)
        defer { try? FileManager.default.removeItem(at: zipFile) }

        // This bounds what gets imported. The largest catalog archive is only a
        // few megabytes, leaving headroom for future controller artwork.
        let size = ((try? FileManager.default.attributesOfItem(
            atPath: zipFile.path
        ))?[.size] as? NSNumber)?.int64Value ?? 0
        guard size <= maxDownloadBytes else {
            throw SkinInstallError.tooLarge
        }

        let extractDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("skin-import-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(
            at: extractDir,
            withIntermediateDirectories: true
        )
        defer { try? FileManager.default.removeItem(at: extractDir) }

        let extracted = ARMSX2Bridge.extractControllerSkinArchive(
            at: zipFile,
            to: extractDir
        )
        guard !extracted.isEmpty else {
            throw SkinInstallError.emptyArchive
        }

        return try await VPadSkinLibraryStore.shared.importSkin(
            from: extractDir,
            originalImportName: skin.name,
            catalogID: skin.file,
            preferredLayoutFileName: skin.iosLayout,
            replacingSkinID: replacingSkinID,
            layoutPresets: .shared
        )
    }

    private func installedDescriptor(for skin: CatalogSkin) -> VPadSkinDescriptor? {
        VPadSkinLibraryStore.shared.importedDescriptors.first { $0.catalogID == skin.file }
    }

    /// The library resets a dangling selection on its own, but nothing keeps
    /// SettingsStore's mirror in step — and a stale .custom there points the pad
    /// at the old ControllerSkins/Custom folder instead of the stock art.
    private func syncSelectedSkin() {
        SettingsStore.shared.virtualPadSkin = VPadSkinLibraryStore.shared.selectedDescriptor.virtualPadSkin
    }
}

struct AutomaticCustomSkinProposal: Codable, Equatable {
    let serial: String
    let catalogID: String
    let skinID: String
    let layoutPresetID: String?
    let skinName: String
    var previewURLString: String? = nil
    /// Optional for compatibility with proposal caches written by older
    /// builds. `default` is the bundled ARMSX2 skin, while `universal` maps to
    /// a catalog entry whose manifest serial is `any`.
    var selectionKind: String? = nil
    /// `nil` preserves the behaviour of assignments accepted by older builds.
    /// New confirmations always write an explicit value from the prompt toggle.
    var appliesCustomLayout: Bool? = nil

    var previewURL: URL? {
        previewURLString.flatMap(URL.init(string:))
    }

    var shouldApplyCustomLayout: Bool {
        appliesCustomLayout ?? true
    }

    var isDefaultSelection: Bool {
        selectionKind == "default"
    }

    var isUniversalSelection: Bool {
        selectionKind == "universal"
    }
}

private struct AutomaticCustomSkinPersistence: Codable {
    var catalogRepository: String?
    var proposals: [String: AutomaticCustomSkinProposal]
    var proposalOptions: [String: [AutomaticCustomSkinProposal]]?
    var resolvedCatalogIDs: [String: String]
    var acceptedAssignments: [String: AutomaticCustomSkinProposal]?
    var declinedCatalogIDs: [String: String]?
    var suppressedSerials: [String]?
}

/// Matches newly added games to catalog serials and keeps a small persisted
/// proposal until the player accepts or declines it at launch. Downloads live
/// here rather than in GameListView so leaving the Games tab cannot cancel one.
@MainActor
final class AutomaticCustomSkinManager {
    static let shared = AutomaticCustomSkinManager()

    private static let persistenceKey = "ARMSX2iOSAutomaticCustomSkinProposalsV1"
    private static let catalogCacheLifetime: TimeInterval = 5 * 60
    private static let defaultSelectionCatalogID =
        "__armsx2_bundled_default_skin__"

    private var proposals: [String: AutomaticCustomSkinProposal] = [:]
    private var proposalOptions: [String: [AutomaticCustomSkinProposal]] = [:]
    private var resolvedCatalogIDs: [String: String] = [:]
    /// Accepted downloads wait here until the running VM publishes its
    /// canonical serial/CRC pair. Library metadata and VM identity can become
    /// available on different frames, so the launch itself is the authority.
    private var acceptedAssignments: [String: AutomaticCustomSkinProposal] = [:]
    private var declinedCatalogIDs: [String: String] = [:]
    /// A durable game-level choice. Clearing a proposal alone is insufficient:
    /// the catalog worker can recreate it on a later cold launch.
    private var suppressedSerials = Set<String>()
    private var queuedSerials = Set<String>()
    private var worker: Task<Void, Never>?
    private var cachedSkins: [CatalogSkin] = []
    private var catalogFetchedAt: Date?

    private init() {
        guard let data = UserDefaults.standard.data(forKey: Self.persistenceKey),
              let stored = try? JSONDecoder().decode(
                AutomaticCustomSkinPersistence.self,
                from: data
              ) else {
            return
        }
        if stored.catalogRepository == SkinCatalog.repo {
            proposals = stored.proposals
            proposalOptions = stored.proposalOptions
                ?? stored.proposals.mapValues { [$0] }
            resolvedCatalogIDs = stored.resolvedCatalogIDs
            declinedCatalogIDs = stored.declinedCatalogIDs ?? [:]
        }
        acceptedAssignments = stored.acceptedAssignments ?? [:]
        suppressedSerials = Set(
            (stored.suppressedSerials ?? [])
                .map(PadLayoutGameIdentity.normalizedSerial)
                .filter { !$0.isEmpty }
        )
        // `resolvedCatalogIDs` and `declinedCatalogIDs` were the old implicit
        // form of "do not show again". Preserve that decision while migrating.
        suppressedSerials.formUnion(stored.resolvedCatalogIDs.keys)
        suppressedSerials.formUnion((stored.declinedCatalogIDs ?? [:]).keys)

        // Repair choices accepted by builds which recorded the proposal but
        // could not make it visible unless the same skin was Global Default.
        for proposal in acceptedAssignments.values
        where VPadSkinLibraryStore.shared.descriptor(id: proposal.skinID) != nil {
            storeAutomaticAssignment(proposal)
        }
    }

    func enqueueMatches(for games: [CoverGameInfo]) {
        guard SettingsStore.shared.automaticDownloadCustomSkin else { return }
        for game in games {
            let serial = PadLayoutGameIdentity.normalizedSerial(
                game.metadata["serial"]
            )
            if !serial.isEmpty, !suppressedSerials.contains(serial) {
                queuedSerials.insert(serial)
            }
        }
        startWorkerIfNeeded()
    }

    func proposal(forSerial rawSerial: String?) -> AutomaticCustomSkinProposal? {
        availableProposals(forSerial: rawSerial).first
    }

    func availableProposals(
        forSerial rawSerial: String?
    ) -> [AutomaticCustomSkinProposal] {
        guard SettingsStore.shared.automaticDownloadCustomSkin else { return [] }
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty, !suppressedSerials.contains(serial) else {
            return []
        }
        let storedOptions = proposalOptions[serial]
            ?? proposals[serial].map { [$0] }
            ?? []
        var available = storedOptions.filter { proposal in
                guard let descriptor = VPadSkinLibraryStore.shared.descriptor(
                    id: proposal.skinID
                ) else {
                    return false
                }
                guard CatalogSkin.blackGameSkinIsCompatible(
                    name: proposal.skinName,
                    catalogID: proposal.catalogID,
                    serial: serial
                ) else {
                    return false
                }
                if proposal.isDefaultSelection {
                    return descriptor.source == .builtIn
                }
                return descriptor.catalogID == proposal.catalogID
            }
        let hasCatalogSkin = available.contains { !$0.isDefaultSelection }
        guard hasCatalogSkin else {
            proposals[serial] = nil
            proposalOptions[serial] = nil
            persist()
            return []
        }

        if !available.contains(where: \.isDefaultSelection) {
            let defaultProposal = makeDefaultProposal(forSerial: serial)
            let universalIndex = available.firstIndex { proposal in
                if proposal.isUniversalSelection { return true }
                return cachedSkins.first(where: {
                    $0.file == proposal.catalogID
                })?.isUniversal == true
            } ?? 0
            available.insert(defaultProposal, at: universalIndex)
            proposalOptions[serial] = available
            persist()
        }
        return proposalsWithCatalogPreviews(available)
    }

    /// Manual Per-Game Custom Skin pickers opt into the same catalog download
    /// work as Automatic Download Custom Skin. This remains available when the
    /// background automatic-download preference is off because opening the
    /// picker is an explicit user request.
    @MainActor
    func downloadAvailableProposals(
        forSerial rawSerial: String?
    ) async -> [AutomaticCustomSkinProposal] {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty else { return [] }

        do {
            let skins = try await fetchCatalogSkins()
            guard !Task.isCancelled else { return [] }
            let catalogProposals = await installCatalogProposals(
                from: skins,
                forSerial: serial
            )
            guard !Task.isCancelled else { return [] }
            if let recommendation = catalogProposals.first {
                proposals[serial] = recommendation
                proposalOptions[serial] = catalogProposals
                persist()
            }
        } catch {
            NSLog(
                "[ARMSX2 iOS Skins] manual catalog download failed serial=%@ error=%@",
                serial,
                error.localizedDescription
            )
        }

        return installedProposals(forSerial: serial)
    }

    private func proposalsWithCatalogPreviews(
        _ source: [AutomaticCustomSkinProposal]
    ) -> [AutomaticCustomSkinProposal] {
        source.map { proposal in
            guard proposal.previewURL == nil,
                  let skin = cachedSkins.first(where: {
                      $0.file == proposal.catalogID
                  }),
                  let previewURL = SkinCatalog.previewURL(for: skin) else {
                return proposal
            }
            var enriched = proposal
            enriched.previewURLString = previewURL.absoluteString
            return enriched
        }
    }

    /// Returns every locally usable skin for a manual per-game choice. Unlike
    /// the automatic launch prompt, this is available even when automatic skin
    /// downloads are disabled and includes skins imported outside the catalog.
    func installedProposals(
        forSerial rawSerial: String?
    ) -> [AutomaticCustomSkinProposal] {
        let serial = PadLayoutGameIdentity.normalizedSerial(rawSerial)
        guard !serial.isEmpty else { return [] }

        let skinLibrary = VPadSkinLibraryStore.shared
        let storedOptions = proposalOptions[serial]
            ?? proposals[serial].map { [$0] }
            ?? []
        var available = storedOptions.filter { proposal in
            guard let descriptor = skinLibrary.descriptor(id: proposal.skinID)
            else {
                return false
            }
            guard CatalogSkin.blackGameSkinIsCompatible(
                name: proposal.skinName,
                catalogID: proposal.catalogID,
                serial: serial
            ) else {
                return false
            }
            if proposal.isDefaultSelection {
                return descriptor.source == .builtIn
            }
            return descriptor.source == .imported
        }

        available.removeAll(where: \.isDefaultSelection)
        available.insert(makeDefaultProposal(forSerial: serial), at: 0)

        var includedSkinIDs = Set(available.map(\.skinID))
        // Keep imported/custom choices next to Default. Built-in alternatives
        // remain available afterward, but no longer force the downloaded skin
        // to the far end of the picker.
        let remainingDescriptors = skinLibrary.allDescriptors.sorted {
            if $0.source != $1.source {
                return $0.source == .imported
            }
            return $0.displayName.localizedStandardCompare($1.displayName)
                == .orderedAscending
        }
        for descriptor in remainingDescriptors {
            guard includedSkinIDs.insert(descriptor.id).inserted else { continue }
            guard CatalogSkin.blackGameSkinIsCompatible(
                name: descriptor.displayName,
                catalogID: descriptor.catalogID,
                serial: serial
            ) else {
                continue
            }
            available.append(
                AutomaticCustomSkinProposal(
                    serial: serial,
                    catalogID: descriptor.catalogID
                        ?? (descriptor.source == .builtIn
                            ? "__built_in_skin__\(descriptor.id)"
                            : "__installed_skin__\(descriptor.id)"),
                    skinID: descriptor.id,
                    layoutPresetID: descriptor.linkedLayoutPresetID,
                    skinName: descriptor.displayName,
                    selectionKind: descriptor.source == .builtIn
                        ? "builtIn"
                        : (descriptor.catalogID == nil
                            ? "installed"
                            : "catalog")
                )
            )
        }
        return proposalsWithCatalogPreviews(available)
    }

    func previewURL(for proposal: AutomaticCustomSkinProposal) -> URL? {
        guard !proposal.isDefaultSelection else { return nil }
        if let url = proposal.previewURL {
            return url
        }
        guard let skin = cachedSkins.first(where: {
            $0.file == proposal.catalogID
        }) else {
            return nil
        }
        return SkinCatalog.previewURL(for: skin)
    }

    @discardableResult
    func apply(
        _ proposal: AutomaticCustomSkinProposal,
        toISO isoName: String,
        metadata: [String: String],
        setCustomLayout: Bool,
        doNotShowAgain: Bool
    ) -> Bool {
        let skinLibrary = VPadSkinLibraryStore.shared
        guard skinLibrary.descriptor(id: proposal.skinID) != nil else {
            resolve(proposal)
            return false
        }

        var acceptedProposal = proposal
        acceptedProposal.appliesCustomLayout = setCustomLayout

        // Keep the accepted selection until GameScreenView can materialize it
        // against the identity reported by the running VM. This prevents a
        // valid download from silently falling back to Global Default when the
        // library's cached CRC and runtime CRC are not ready at the same time.
        acceptedAssignments[proposal.serial] = acceptedProposal
        storeAutomaticAssignment(acceptedProposal)

        var serial = metadata["serial"]
        var crc = metadata["crc"]
        var identity = PadLayoutGameIdentity(serial: serial, crc: crc)
        if identity == nil {
            let gameSettings = ARMSX2Bridge.gameSettings(forISO: isoName)
            serial = (gameSettings["serial"] as? String) ?? serial
            crc = (gameSettings["crc"] as? String) ?? crc
            identity = PadLayoutGameIdentity(serial: serial, crc: crc)
        }
        // The master toggle is part of the core per-game INI. Layout and skin
        // assignments live in the iOS preset library keyed by runtime identity.
        ARMSX2Bridge.setPerGameINIBool(
            "ARMSX2iOS/PerGame",
            key: "Enabled",
            value: true,
            forISO: isoName
        )
        if let identity {
            apply(acceptedProposal, to: identity, using: skinLibrary)
        }
        if doNotShowAgain {
            suppressedSerials.insert(proposal.serial)
            resolve(acceptedProposal)
        } else {
            persist()
        }
        return true
    }

    /// Rebinds an accepted catalog selection to the VM's canonical identity.
    /// This is idempotent and runs only while an accepted assignment is pending.
    func materializeAcceptedAssignment(for identity: PadLayoutGameIdentity) {
        guard let proposal = acceptedAssignments[identity.serial] else { return }
        let skinLibrary = VPadSkinLibraryStore.shared
        guard skinLibrary.descriptor(id: proposal.skinID) != nil else {
            acceptedAssignments[identity.serial] = nil
            persist()
            return
        }
        apply(proposal, to: identity, using: skinLibrary)
        acceptedAssignments[identity.serial] = nil
        persist()
    }

    private func apply(
        _ proposal: AutomaticCustomSkinProposal,
        to identity: PadLayoutGameIdentity,
        using skinLibrary: VPadSkinLibraryStore
    ) {
        let layouts = PadLayoutPresetStore.shared
        layouts.setSkin(proposal.skinID, for: identity, using: skinLibrary)
        if proposal.shouldApplyCustomLayout,
           let layoutPresetID = proposal.layoutPresetID,
           layouts.preset(id: layoutPresetID) != nil {
            layouts.setPreset(layoutPresetID, for: identity)
        }
    }

    private func storeAutomaticAssignment(
        _ proposal: AutomaticCustomSkinProposal
    ) {
        PadLayoutPresetStore.shared.setAutomaticAssignment(
            skinID: proposal.skinID,
            layoutPresetID: proposal.shouldApplyCustomLayout
                ? proposal.layoutPresetID
                : nil,
            forSerial: proposal.serial,
            using: VPadSkinLibraryStore.shared
        )
    }

    func decline(
        _ proposal: AutomaticCustomSkinProposal,
        doNotShowAgain: Bool
    ) {
        guard doNotShowAgain else { return }
        suppressedSerials.insert(proposal.serial)
        declinedCatalogIDs[proposal.serial] = proposal.catalogID
        resolve(proposal)
    }

    private func startWorkerIfNeeded() {
        guard worker == nil, !queuedSerials.isEmpty else { return }
        worker = Task { [weak self] in
            await self?.drainQueue()
        }
    }

    private func drainQueue() async {
        defer {
            worker = nil
            startWorkerIfNeeded()
        }

        do {
            let skins = try await fetchCatalogSkins()
            while !queuedSerials.isEmpty, !Task.isCancelled {
                let serial = queuedSerials.removeFirst()
                guard !suppressedSerials.contains(serial) else { continue }
                guard declinedCatalogIDs[serial] == nil else { continue }
                if let assignedSkinID = PadLayoutPresetStore.shared
                    .automaticSkinID(forSerial: serial),
                   VPadSkinLibraryStore.shared.descriptor(
                    id: assignedSkinID
                   ) != nil {
                    continue
                }

                let existingOptions = proposalOptions[serial] ?? []
                if !existingOptions.isEmpty,
                   existingOptions.allSatisfy({ proposal in
                       guard let descriptor = VPadSkinLibraryStore.shared.descriptor(
                           id: proposal.skinID
                       ) else {
                           return false
                       }
                       if proposal.isDefaultSelection {
                           return descriptor.source == .builtIn
                       }
                       return descriptor.catalogID == proposal.catalogID
                   }) {
                    continue
                }

                let availableOptions = await installCatalogProposals(
                    from: skins,
                    forSerial: serial
                )
                guard !availableOptions.isEmpty else { continue }
                if let recommendation = availableOptions.first {
                    proposals[serial] = recommendation
                    proposalOptions[serial] = availableOptions
                    persist()
                }
            }
        } catch {
            // A new game import or manual cover retry will enqueue another
            // lookup; do not spin immediately while the network is offline.
            queuedSerials.removeAll()
            NSLog(
                "[ARMSX2 iOS Skins] automatic catalog lookup failed: %@",
                error.localizedDescription
            )
        }
    }

    /// Downloads the exact serial matches first, followed by universal `any`
    /// skins, and reuses an installed descriptor whenever its linked layout is
    /// already complete. Both background matching and explicit pickers call
    /// this single implementation.
    private func installCatalogProposals(
        from skins: [CatalogSkin],
        forSerial serial: String
    ) async -> [AutomaticCustomSkinProposal] {
        let compatibleSkins = skins.filter {
            $0.isCompatible(withSerial: serial)
        }.sorted { lhs, rhs in
            let lhsExact = lhs.explicitlyMatches(serial: serial)
            let rhsExact = rhs.explicitlyMatches(serial: serial)
            if lhsExact != rhsExact { return lhsExact }
            return lhs.name.localizedStandardCompare(rhs.name)
                == .orderedAscending
        }
        guard !compatibleSkins.isEmpty else { return [] }

        var availableOptions: [AutomaticCustomSkinProposal] = []
        var insertedDefaultSelection = false
        for skin in compatibleSkins where !Task.isCancelled {
            if skin.isUniversal && !insertedDefaultSelection {
                availableOptions.append(makeDefaultProposal(forSerial: serial))
                insertedDefaultSelection = true
            }
            do {
                let installed = VPadSkinLibraryStore.shared
                    .importedDescriptors.first {
                        $0.catalogID == skin.file
                    }
                let descriptor: VPadSkinDescriptor
                if let installed,
                   skin.iosLayout == nil
                    || installed.linkedLayoutPresetID != nil {
                    descriptor = installed
                } else {
                    let result = try await SkinInstaller
                        .installForAutomaticAssignment(
                            skin,
                            replacing: installed?.id
                        )
                    descriptor = result.descriptor
                }
                await prefetchPreview(for: skin)
                availableOptions.append(
                    AutomaticCustomSkinProposal(
                        serial: serial,
                        catalogID: skin.file,
                        skinID: descriptor.id,
                        layoutPresetID: descriptor.linkedLayoutPresetID,
                        skinName: descriptor.displayName,
                        previewURLString: SkinCatalog.previewURL(
                            for: skin
                        )?.absoluteString,
                        selectionKind: skin.isUniversal
                            ? "universal"
                            : "exact"
                    )
                )
            } catch {
                NSLog(
                    "[ARMSX2 iOS Skins] catalog download failed serial=%@ skin=%@ error=%@",
                    serial,
                    skin.name,
                    error.localizedDescription
                )
            }
        }
        if !insertedDefaultSelection {
            availableOptions.append(makeDefaultProposal(forSerial: serial))
        }
        return availableOptions
    }

    private func fetchCatalogSkins() async throws -> [CatalogSkin] {
        if let catalogFetchedAt,
           Date().timeIntervalSince(catalogFetchedAt) < Self.catalogCacheLifetime,
           !cachedSkins.isEmpty {
            return cachedSkins
        }

        var request = URLRequest(url: SkinCatalog.manifestURL)
        request.cachePolicy = .reloadRevalidatingCacheData
        let (data, response) = try await URLSession.shared.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw SkinCatalogError.serverError(http.statusCode)
        }
        let manifest = try JSONDecoder().decode(SkinCatalogManifest.self, from: data)
        cachedSkins = manifest.skins
        catalogFetchedAt = Date()
        return manifest.skins
    }

    /// The confirmation should already have its artwork when the user opens
    /// the game. Warm URLCache as part of the background catalog job rather
    /// than starting a second network request on the launch path.
    private func prefetchPreview(for skin: CatalogSkin) async {
        guard let url = SkinCatalog.previewURL(for: skin) else { return }
        var request = URLRequest(url: url)
        request.cachePolicy = .returnCacheDataElseLoad
        _ = try? await URLSession.shared.data(for: request)
    }

    private func makeDefaultProposal(
        forSerial serial: String
    ) -> AutomaticCustomSkinProposal {
        let descriptor = VPadSkinLibraryStore.defaultDescriptor
        return AutomaticCustomSkinProposal(
            serial: serial,
            catalogID: Self.defaultSelectionCatalogID,
            skinID: descriptor.id,
            layoutPresetID: nil,
            skinName: "Default",
            selectionKind: "default",
            appliesCustomLayout: false
        )
    }

    private func resolve(_ proposal: AutomaticCustomSkinProposal) {
        proposals[proposal.serial] = nil
        proposalOptions[proposal.serial] = nil
        resolvedCatalogIDs[proposal.serial] = proposal.catalogID
        persist()
    }

    private func persist() {
        let value = AutomaticCustomSkinPersistence(
            catalogRepository: SkinCatalog.repo,
            proposals: proposals,
            proposalOptions: proposalOptions,
            resolvedCatalogIDs: resolvedCatalogIDs,
            acceptedAssignments: acceptedAssignments,
            declinedCatalogIDs: declinedCatalogIDs,
            suppressedSerials: suppressedSerials.sorted()
        )
        if let data = try? JSONEncoder().encode(value) {
            UserDefaults.standard.set(data, forKey: Self.persistenceKey)
        }
    }
}
