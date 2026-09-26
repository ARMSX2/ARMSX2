// TexturePacksView.swift — texture packs and the settings that load them
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// Every installed pack, or only one game's when serial is set, with options below them.
struct TexturePacksView<Options: View>: View {
    private let serial: String?
    private let options: Options
    @State private var settings = SettingsStore.shared
    @State private var packs: [TexturePack]?
    @State private var titles: [String: String] = [:]
    @State private var pendingRemoval: TexturePack?
    @State private var failure: String?
    @State private var showCatalog = false
    @State private var showPicker = false
    @State private var importing = false

    init(serial: String? = nil, @ViewBuilder options: () -> Options) {
        self.serial = serial
        self.options = options()
    }

    var body: some View {
        let running = ARMSX2Bridge.currentTextureSerial()
        Form {
            if serial == nil && !running.isEmpty {
                Section {
                    LabeledContent(settings.localized("Now Running"), value: running)
                }
            }

            // Buttons and sheets, not links and a toolbar: the wide per-game panel has no navigation stack.
            Section {
                Button {
                    showCatalog = true
                } label: {
                    Label(settings.localized("Download Texture Packs"), systemImage: "arrow.down.circle")
                }
                Button {
                    showPicker = true
                } label: {
                    Label(settings.localized("Import"), systemImage: "square.and.arrow.down")
                }
                .disabled(importing)
            }

            Section {
                if importing {
                    ProgressView(settings.localized("Installing..."))
                        .frame(maxWidth: .infinity)
                }
                if let packs {
                    if packs.isEmpty && !importing {
                        ContentUnavailableView(settings.localized("No texture packs installed."), systemImage: "photo.stack")
                            .frame(maxWidth: .infinity)
                    }
                    ForEach(packs) { pack in
                        row(pack, running: pack.serial == running)
                    }
                } else {
                    ProgressView()
                }
            }

            options
        }
        .navigationTitle(settings.localized("Texture Packs"))
        .navigationBarTitleDisplayMode(.inline)
        .task { await reload() }
        .sheet(isPresented: $showCatalog, onDismiss: { Task { await reload() } }) {
            NavigationStack {
                TextureCatalogView(serial: serial)
            }
        }
        .sheet(isPresented: $showPicker) {
            ImportDocumentPicker(
                allowsMultipleSelection: false,
                legacyDocumentTypes: ["public.zip-archive", "public.data"],
                legacyDocumentMode: .open
            ) { result in
                showPicker = false
                switch result {
                case .success(let urls):
                    if let url = urls.first {
                        Task { await install(url) }
                    }
                case .failure(let error):
                    if !FileImportHandler.isUserCancelledPickerError(error) {
                        failure = error.localizedDescription
                    }
                }
            }
        }
        .confirmationDialog(
            String(format: settings.localized("Remove %@"), pendingRemoval.map(name) ?? ""),
            isPresented: Binding(
                get: { pendingRemoval != nil },
                set: { if !$0 { pendingRemoval = nil } }
            ),
            titleVisibility: .visible
        ) {
            if let pack = pendingRemoval {
                Button(String(format: settings.localized("Remove %@"), name(pack)), role: .destructive) {
                    pendingRemoval = nil
                    Task { await remove(pack) }
                }
            }
            Button(settings.localized("Cancel"), role: .cancel) {
                pendingRemoval = nil
            }
        } message: {
            Text(settings.localized("This cannot be undone."))
        }
        .alert(
            settings.localized("Texture Packs"),
            isPresented: Binding(
                get: { failure != nil },
                set: { if !$0 { failure = nil } }
            )
        ) {
            Button(settings.localized("OK")) { failure = nil }
        } message: {
            Text(failure ?? "")
        }
    }

    private func row(_ pack: TexturePack, running: Bool) -> some View {
        HStack(spacing: 12) {
            Image(systemName: running ? "play.circle.fill" : "photo.stack")
                .foregroundStyle(running ? Color.accentColor : .secondary)
                .frame(width: 24)

            VStack(alignment: .leading, spacing: 4) {
                Text(name(pack))
                    .font(.body.weight(.medium))
                Text("\(pack.serial) · \(ByteCountFormatter.string(fromByteCount: pack.bytes, countStyle: .file))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button(role: .destructive) {
                pendingRemoval = pack
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .accessibilityLabel(String(format: settings.localized("Remove %@"), name(pack)))
        }
        .padding(.vertical, 4)
    }

    private func name(_ pack: TexturePack) -> String {
        titles[pack.serial.uppercased()] ?? pack.serial
    }

    private func reload() async {
        let root = TexturePackLibrary.root
        let (found, names) = await Task.detached(priority: .utility) {
            (TexturePackLibrary.installed(in: root), TexturePackLibrary.titlesBySerial())
        }.value
        titles = names
        packs = found.filter { pack in serial.map { $0 == pack.serial } ?? true }
            .sorted { name($0).localizedStandardCompare(name($1)) == .orderedAscending }
    }

    private func install(_ url: URL) async {
        importing = true
        let running = ARMSX2Bridge.currentTextureSerial()
        // Read in place: a pack can be gigabytes, and a picker copy would need that space twice.
        let result = await Task.detached(priority: .userInitiated) { () -> Result<String, Error> in
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            return Result { try ARMSX2Bridge.installTexturePack(at: url, serial: serial ?? "", fallbackSerial: running) }
        }.value
        importing = false
        switch result {
        case .success(let serial):
            // With replacement off a new pack changes nothing on screen and reads as broken.
            settings.loadTextureReplacements = true
            if serial == running {
                ARMSX2Bridge.reloadTextureReplacements()
            }
        case .failure(let error):
            failure = error.localizedDescription
        }
        await reload()
    }

    private func remove(_ pack: TexturePack) async {
        do {
            try await Task.detached(priority: .utility) { try TexturePackLibrary.remove(pack) }.value
        } catch {
            failure = "\(settings.localized("Some files could not be removed:"))\n\(error.localizedDescription)"
        }
        if pack.serial == ARMSX2Bridge.currentTextureSerial() {
            ARMSX2Bridge.reloadTextureReplacements()
        }
        await reload()
    }
}

struct TextureReplacementSettings: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Section(settings.localized("Texture Replacement")) {
            Toggle(settings.localized("Load Replacement Textures"), isOn: $settings.loadTextureReplacements)
            Text(settings.localized("Loads PNG or DDS texture packs from Documents/textures/[Game Serial]/replacements/. Texture packs use app storage and may be large. Requires restart."))
                .font(.caption)
                .foregroundStyle(.secondary)

            Toggle(settings.localized("Async Loading"), isOn: $settings.loadTextureReplacementsAsync)
                .disabled(!settings.loadTextureReplacements)
            Text(settings.localized("Loads replacement textures in the background to reduce boot stalls."))
                .font(.caption)
                .foregroundStyle(.secondary)

            Toggle(settings.localized("Precache Textures"), isOn: $settings.precacheTextureReplacements)
                .disabled(!settings.loadTextureReplacements)
            Text(settings.localized("Loads all replacements when the game starts. Faster in-game, but uses more RAM."))
                .font(.caption)
                .foregroundStyle(.secondary)

            Picker(settings.localized("Texture Preloading"), selection: $settings.texturePreloading) {
                Text(settings.localized("Off")).tag(0)
                Text(settings.localized("Partial")).tag(1)
                Text(settings.localized("Full")).tag(2)
            }
            Text(settings.localized("Core texture preloading mode. Full can improve replacement behavior but may increase memory use."))
                .font(.caption)
                .foregroundStyle(.secondary)
            if settings.loadTextureReplacements && (settings.precacheTextureReplacements || settings.texturePreloading > 0) {
                Text(settings.localized("Large texture packs can use a lot of RAM when preload/precache is active and may cause stalls or crashes."))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        }

        Section(settings.localized("Texture Dumping")) {
            Toggle(settings.localized("Dump Replaceable Textures"), isOn: $settings.dumpReplaceableTextures)
            Text(settings.localized("Writes discovered textures to Documents/textures/[Game Serial]/dumps/. This can heavily reduce performance and grow app storage quickly."))
                .font(.caption)
                .foregroundStyle(.secondary)
            if settings.dumpReplaceableTextures {
                Text(settings.localized("Texture dumping can heavily slow games and create very large dump folders. Turn it off after collecting the textures you need."))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            Toggle(settings.localized("Dump Mipmaps"), isOn: $settings.dumpReplaceableMipmaps)
                .disabled(!settings.dumpReplaceableTextures)
            Toggle(settings.localized("Dump During FMV"), isOn: $settings.dumpTexturesWithFMVActive)
                .disabled(!settings.dumpReplaceableTextures)
            Toggle(settings.localized("Dump Direct Textures"), isOn: $settings.dumpDirectTextures)
                .disabled(!settings.dumpReplaceableTextures)
            Toggle(settings.localized("Dump Palette Textures"), isOn: $settings.dumpPaletteTextures)
                .disabled(!settings.dumpReplaceableTextures)
        }
    }
}

extension TexturePackLibrary {
    static var root: URL {
        URL(fileURLWithPath: ARMSX2Bridge.documentsDirectory(), isDirectory: true)
            .appendingPathComponent("textures", isDirectory: true)
    }
}
