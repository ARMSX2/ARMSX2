// GraphicsSettingsView.swift — Renderer, upscale, filter, and display settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GraphicsSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var appState = AppState.shared
    @State private var showShaderCacheClearConfirm = false
    @State private var shaderCacheResult: String?
    @State private var showShaderCacheResult = false

    // Returning to the menu only pauses the VM, so a game can still be loaded
    // while this screen is open. Switching renderer then sends the next settings
    // apply through a full GS teardown under that game.
    private var gameIsLoaded: Bool {
        appState.runningGameName != nil
    }

    private var manualAdvancedHacks: Bool {
        !settings.enableGameDBHardwareFixes
    }

    /// Touching a hack claims it, which is what stops the game database overwriting that
    /// one on the next apply. Without this the row writes the INI and the database wins.
    private func claiming<T: Equatable>(_ key: String, _ value: Binding<T>) -> Binding<T> {
        Binding(
            get: { value.wrappedValue },
            set: { newValue in
                guard newValue != value.wrappedValue else { return }
                value.wrappedValue = newValue
                settings.setGraphicsHackPinned(key, true)
            }
        )
    }

    /// One line under a row when what the game is running isn't what the row says, plus
    /// the way back to the database value once the player has claimed it.
    @ViewBuilder
    private func hackNote(_ key: String, shown: Int) -> some View {
        if let status = settings.graphicsHack(key), status.reason != .noGame {
            if status.effective != shown, let explanation = explanation(for: status.reason) {
                Text(explanation)
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            if status.pinned {
                Button(settings.localized("Use the game database value")) {
                    settings.resetGraphicsHackValue(key)
                    settings.setGraphicsHackPinned(key, false)
                }
                .font(.caption)
            }
        }
    }

    private func explanation(for reason: SettingsStore.GraphicsHackReason) -> String? {
        switch reason {
        case .needsUpscaling:
            return settings.localized("Ignored at 1x Internal Resolution.")
        case .fromGameDatabase:
            if let game = appState.runningGameName {
                return settings.localized("The game database is setting this for") + " \(game)."
            }
            return settings.localized("The game database is setting this for this game.")
        case .needsManualHacks:
            return settings.localized("The automatic graphics fixes are in charge of this one.")
        case .perGame:
            // This screen edits the global value, so the row can honestly disagree with
            // what the game is running. Change it in the game's own settings.
            return settings.localized("This game has its own setting for this, and that is what it is using.")
        case .applied, .noGame:
            return nil
        }
    }

    private var skipDrawStartBinding: Binding<Int> {
        Binding(
            get: { settings.skipDrawStart },
            set: { newValue in
                settings.skipDrawStart = min(max(newValue, SettingsStore.skipDrawRange.lowerBound), SettingsStore.skipDrawRange.upperBound)
                settings.skipDrawEnd = SettingsStore.normalizedSkipDrawEnd(start: settings.skipDrawStart, end: settings.skipDrawEnd)
            }
        )
    }

    private var skipDrawEndBinding: Binding<Int> {
        Binding(
            get: { settings.skipDrawEnd },
            set: { newValue in
                settings.skipDrawEnd = SettingsStore.normalizedSkipDrawEnd(start: settings.skipDrawStart, end: newValue)
            }
        )
    }

    var body: some View {
        Form {
            Section {
                intPicker("GS Back Thread", selection: $settings.backThreadMode, options: [
                    ("Disabled (Default)", 0),
                    ("Inline Records (Debug)", 1),
                    ("Lockstep (Debug)", 2),
                    ("Pipelined (Second GS Thread)", 3),
                ])
                if settings.backThreadMode == 1 || settings.backThreadMode == 2 {
                    Text(settings.localized("Debug mode — much slower than the default. Do not use for play."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            } header: {
                Text(settings.localized("Performance"))
            } footer: {
                Text(settings.localized("Pipelined splits GS emulation across two threads on multi-core systems and competes for cores with EE/VU threads. The debug modes are much slower — do not use them for play. Requires restart."))
            }

            Section(settings.localized("Renderer")) {
                Picker(settings.localized("Renderer"), selection: $settings.renderer) {
                    ForEach(SettingsOptions.renderer, id: \.id) { option in
                        Text(settings.localized(option.title)).tag(option.id)
                    }
                }
                .disabled(gameIsLoaded)
                if gameIsLoaded {
                    Text(settings.localized("Close the running game to change the renderer."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
#if targetEnvironment(macCatalyst)
                Text(settings.localized("Metal is required for the Mac Catalyst build. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
#else
                Text(settings.localized("Metal is the supported iOS renderer. Software is slow but useful for debugging. Null disables rendering. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
#endif
#if !targetEnvironment(macCatalyst)
                if settings.renderer == 11 {
                    Text(settings.localized("Null renderer may show no video output or a black screen. It is mainly useful for testing. Switch back to Metal and restart if selected by mistake."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
#endif

                Button(role: .destructive) {
                    showShaderCacheClearConfirm = true
                } label: {
                    Label(settings.localized("Clear Shader Cache"), systemImage: "square.slash")
                }
                Text(settings.localized("Removes cached GS/Metal shader and pipeline artifacts so they rebuild from scratch. Use this if visuals glitch after a settings change. Effects apply on the next frame or restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Upscaling")) {
                Picker(settings.localized("Internal Resolution"), selection: $settings.upscaleMultiplier) {
                    ForEach(UpscaleOptions.all, id: \.id) { option in
                        Text(settings.localized(option.title)).tag(option.id)
                    }
                }
                Text(settings.localized("Lower values can help performance on heavy games. Higher values improve visual quality but reduce performance significantly. Applies immediately; the renderer may briefly stutter while it reinits."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if settings.upscaleMultiplier >= 4.0 {
                    Text(settings.localized("4x and higher can cause poor performance, heat, stutter, or instability on iPhone and iPad."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }

            if settings.isMetalFXAvailable {
                Section(settings.localized("Upscaler")) {
                    Picker(settings.localized("Spatial Upscaler"), selection: $settings.upscaler) {
                        Text(settings.localized("Off (Bilinear)")).tag(0)
                        Text(settings.localized("MetalFX Spatial")).tag(1)
                    }
                    Text(settings.localized(
                        "GPU-accelerated upscaling via MetalFX. Renders at the native PS2 "
                        + "resolution and upscales to the display for sharper visuals at "
                        + "lower cost than a higher internal resolution. Applies immediately."
                    ))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
            }

            Section(settings.localized("Filtering")) {
                Picker(settings.localized("Texture Filtering"), selection: $settings.textureFiltering) {
                    Text(settings.localized("Nearest (Pixelated)")).tag(0)
                    Text(settings.localized("Bilinear (Forced)")).tag(1)
                    Text(settings.localized("Bilinear (PS2 Default)")).tag(2)
                    Text(settings.localized("Bilinear (Forced excl. Sprite)")).tag(3)
                }

                Toggle(settings.localized("Hardware Mipmapping"), isOn: $settings.hardwareMipmapping)
                Text(settings.localized("Emulates PS2 texture mipmaps in the hardware renderer. Leave on by default; turn off only if a game has mipmap shimmer, stripes, or bad texture LOD behavior. Requires reset/relaunch for safest results."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("FXAA", isOn: $settings.fxaa)
                Text(settings.localized("Fast anti-aliasing. Smooths edges but may blur textures slightly."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("CAS Sharpening"), isOn: Binding(
                    get: { settings.casMode > 0 },
                    set: { settings.casMode = $0 ? 1 : 0 }
                ))
                if settings.casMode > 0 {
                    NumberRow(.casSharpness, value: $settings.casSharpness, settings: settings)
                }
                Text(settings.localized("Contrast Adaptive Sharpening via Metal. Sharpens the image after rendering."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Display")) {
                Picker(settings.localized("Deinterlace"), selection: $settings.interlaceMode) {
                    // Tags are GSInterlaceMode values. This list used to be shifted by one, so
                    // every label from Weave down named the mode below it.
                    Text(settings.localized("Automatic (Default)")).tag(0)
                    Text(settings.localized("Off (No Deinterlacing)")).tag(1)
                    Text(settings.localized("Weave (TFF)")).tag(2)
                    Text(settings.localized("Weave (BFF)")).tag(3)
                    Text(settings.localized("Bob (TFF)")).tag(4)
                    Text(settings.localized("Bob (BFF)")).tag(5)
                    Text(settings.localized("Blend (TFF)")).tag(6)
                    Text(settings.localized("Blend (BFF)")).tag(7)
                    Text(settings.localized("Adaptive (TFF)")).tag(8)
                    Text(settings.localized("Adaptive (BFF)")).tag(9)
                }

                Picker(settings.localized("Aspect Ratio"), selection: $settings.aspectRatio) {
                    Text(settings.localized("Auto 4:3 / 3:2 (Default)")).tag(1)
                    Text("4:3").tag(2)
                    Text(settings.localized("16:9 (Widescreen)")).tag(3)
                    Text("10:7").tag(4)
                    Text(settings.localized("Stretch to Window")).tag(0)
                }
            }

            Section {
                Toggle(settings.localized("Screen Offsets"), isOn: $settings.pcrtcOffsets)
                Toggle(settings.localized("Show Overscan"), isOn: $settings.pcrtcOverscan)
                Toggle(settings.localized("Anti-Blur"), isOn: $settings.pcrtcAntiBlur)
                Toggle(settings.localized("Disable Interlace Offset"), isOn: $settings.disableInterlaceOffset)
                Toggle(settings.localized("Skip Duplicate Frames"), isOn: $settings.skipDuplicateFrames)
                Toggle(settings.localized("Integer Scaling"), isOn: $settings.integerScaling)
            } header: {
                Text(settings.localized("Screen / PCRTC"))
            } footer: {
                Text(settings.localized("Display output options. Most apply immediately."))
            }

            Section(settings.localized("Quality")) {
                Picker(settings.localized("Blending Accuracy"), selection: $settings.blendingAccuracy) {
                    Text(settings.localized("Minimum (Fast)")).tag(0)
                    Text(settings.localized("Basic (Default)")).tag(1)
                    Text(settings.localized("Medium")).tag(2)
                    Text(settings.localized("High")).tag(3)
                    Text(settings.localized("Full (Slow)")).tag(4)
                    Text(settings.localized("Ultra (Very Slow)")).tag(5)
                }
                Text(settings.localized("Higher accuracy fixes transparency issues but reduces performance."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker(settings.localized("Dithering"), selection: $settings.dithering) {
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("Unscaled")).tag(1)
                    Text(settings.localized("Scaled (Default)")).tag(2)
                }
            }

            Section {
                Toggle(settings.localized("Shade Boost"), isOn: $settings.shadeBoost)
                if settings.shadeBoost {
                    NumberRow(.shadeBoostBrightness, value: $settings.shadeBoostBrightness,
                              settings: settings)
                    NumberRow(.shadeBoostContrast, value: $settings.shadeBoostContrast,
                              settings: settings)
                    NumberRow(.shadeBoostSaturation, value: $settings.shadeBoostSaturation,
                              settings: settings)
                    NumberRow(.shadeBoostGamma, value: $settings.shadeBoostGamma,
                              settings: settings)
                }
            } header: {
                Text(settings.localized("Shade Boost"))
            } footer: {
                Text(settings.localized("Adjusts brightness, contrast, saturation, and gamma of the output image. Applies immediately."))
            }

            Section(settings.localized("Advanced Upscaling Hacks")) {
                Toggle(settings.localized("Manual Advanced Hacks"), isOn: Binding(
                    get: { manualAdvancedHacks },
                    set: { settings.enableGameDBHardwareFixes = !$0 }
                ))
                Text(settings.localized("GameDB Graphics Fixes are safest for most games, and turning this on drops all of them. You don't need it to change one sprite or texture-offset value below: changing one keeps your answer for that setting and leaves the rest automatic. Skipdraw still needs this on."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                // MaskUpscalingHacks() zeroes these unless the multiplier is above 1, so the
                // toggles read on and do nothing.
                if settings.upscaleMultiplier <= 1.0 {
                    Text(settings.localized("Half-pixel Offset, Round Sprite, Align Sprite, Merge Sprite, Wild Arms Offset and the texture offsets only apply above 1x Internal Resolution. At 1x or below they are ignored."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }

                Picker(settings.localized("Trilinear Filtering"), selection: $settings.trilinearFiltering) {
                    Text(settings.localized("Automatic / Default")).tag(-1)
                    Text(settings.localized("Off")).tag(0)
                    Text("PS2").tag(1)
                    Text(settings.localized("Forced")).tag(2)
                }
                if settings.trilinearFiltering != -1 {
                    Text(settings.localized("Non-automatic trilinear filtering may break textures in some games."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }

                // These stay usable with the automatic fixes on. Changing one claims it,
                // so the game database keeps every other fix for the game and loses only
                // the one that was argued with.
                Picker(settings.localized("Half-pixel Offset"), selection: claiming("UserHacks_HalfPixelOffset", $settings.halfPixelOffset)) {
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("Normal / Vertex")).tag(1)
                    Text(settings.localized("Special / Texture")).tag(2)
                    Text(settings.localized("Special / Texture Aggressive")).tag(3)
                    Text(settings.localized("Align to Native")).tag(4)
                    Text(settings.localized("Align to Native + Texture Offset")).tag(5)
                }
                hackNote("UserHacks_HalfPixelOffset", shown: settings.halfPixelOffset)

                Picker(settings.localized("Round Sprite"), selection: claiming("UserHacks_round_sprite_offset", $settings.roundSprite)) {
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("Half")).tag(1)
                    Text(settings.localized("Full")).tag(2)
                }
                hackNote("UserHacks_round_sprite_offset", shown: settings.roundSprite)

                Toggle(settings.localized("Align Sprite"), isOn: claiming("UserHacks_align_sprite_X", $settings.alignSprite))
                hackNote("UserHacks_align_sprite_X", shown: settings.alignSprite ? 1 : 0)
                Toggle(settings.localized("Merge Sprite"), isOn: claiming("UserHacks_merge_pp_sprite", $settings.mergeSprite))
                hackNote("UserHacks_merge_pp_sprite", shown: settings.mergeSprite ? 1 : 0)
                Toggle(settings.localized("Wild Arms Offset"), isOn: claiming("UserHacks_ForceEvenSpritePosition", $settings.wildArmsOffset))
                hackNote("UserHacks_ForceEvenSpritePosition", shown: settings.wildArmsOffset ? 1 : 0)

                NumberRow(.textureOffsetX,
                          value: claiming("UserHacks_TCOffsetX", $settings.textureOffsetX),
                          settings: settings)
                NumberRow(.textureOffsetY,
                          value: claiming("UserHacks_TCOffsetY", $settings.textureOffsetY),
                          settings: settings)
                Text(settings.localized("Texture offsets are advanced troubleshooting values. Type a value and clamp to range. Default is 0."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                NumberRow(.skipDrawStart, value: skipDrawStartBinding, settings: settings)
                    .disabled(!manualAdvancedHacks)
                NumberRow(.skipDrawEnd, value: skipDrawEndBinding, settings: settings)
                    .disabled(!manualAdvancedHacks)
                Text(settings.localized("For Skipdraw 1, use Start 1 and End 1. Applies immediately."))
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            Section {
                Toggle(settings.localized("Accurate Alpha Test"), isOn: $settings.hwAccurateAlphaTest)
                Text(settings.localized("Improves alpha-test accuracy for shadows and decals. Some titles look better with this on."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                intPicker("Texture Inside RT", selection: $settings.textureInsideRt, shared: SettingsOptions.textureInsideRT)
                intPicker("Limit 24-Bit Depth", selection: $settings.limit24BitDepth, options: [
                    ("Off", 0), ("Prioritise Upper Bits", 1), ("Prioritise Lower Bits", 2)
                ])
                intPicker("Native Scaling", selection: $settings.nativeScaling, options: [
                    ("Off", 0), ("Normal", 1), ("Aggressive", 2), ("Normal (Maintain Upscale)", 3), ("Aggressive (Maintain Upscale)", 4)
                ])
                intPicker("CPU CLUT Render", selection: $settings.cpuClutRender, shared: SettingsOptions.cpuClutRender)
                intPicker("GPU Target CLUT", selection: $settings.gpuTargetClut, shared: SettingsOptions.gpuTargetClut)
                intPicker("Bilinear Upscale", selection: $settings.bilinearUpscaleHack, options: [
                    ("Automatic", 0), ("Force Bilinear", 1), ("Force Nearest", 2)
                ])
                NumberRow(.cpuSpriteRenderBw, value: $settings.cpuSpriteRenderBw,
                          settings: settings)
                intPicker("CPU Sprite Render Level", selection: $settings.cpuSpriteRenderLevel,
                          shared: SettingsOptions.cpuSpriteRenderLevel)
                intPicker("Max Anisotropy", selection: $settings.maxAnisotropy, shared: SettingsOptions.maxAnisotropy)
                intPicker("Hardware Download Mode", selection: $settings.hardwareDownloadMode, shared: SettingsOptions.hardwareDownloadMode)
                intPicker("TV/CRT Shader", selection: $settings.tvShader, shared: SettingsOptions.tvShader)

                ForEach(SettingsStore.gsBoolHackOptions) { option in
                    Toggle(settings.localized(option.label), isOn: Binding(
                        get: { settings.gsBoolHackEnabled(option.key) },
                        set: { settings.setGSBoolHack(option.key, $0) }
                    ))
                }
            } header: {
                Text(settings.localized("Hardware Fixes"))
            } footer: {
                Text(settings.localized("These hardware fixes are for compatibility. Most games should use Automatic or Default values."))
            }

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

            Section {
                Button(settings.localized("Reset Graphics to Defaults")) {
                    settings.resetGraphicsDefaults()
                }
                .foregroundStyle(.red)
            }
        }
        .navigationTitle(settings.localized("Graphics"))
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { settings.refreshGraphicsHackStatus() }
        // The core re-snapshots after every apply and on a game change, since that is
        // when the masks and the database have finished arguing.
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2GraphicsHackStateChanged"))) { _ in
            settings.refreshGraphicsHackStatus()
        }
        .confirmationDialog(
            settings.localized("Clear Shader Cache?"),
            isPresented: $showShaderCacheClearConfirm,
            titleVisibility: .visible
        ) {
            Button(settings.localized("Clear"), role: .destructive) {
                clearShaderCache()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("This removes cached shader and GS artifacts. The renderer may briefly stutter as they rebuild."))
        }
        .alert(settings.localized("Shader Cache"), isPresented: $showShaderCacheResult) {
            Button(settings.localized("OK")) {}
        } message: {
            Text(settings.localized(shaderCacheResult ?? ""))
        }
    }

    /// Clears the GS/Metal shader and pipeline cache. The Metal renderer builds
    /// shaders in memory from the compiled metallib, so there is no dedicated
    /// on-disk shader directory; the GS-generated cache under the app's cache
    /// directory holds the rebuildable artifacts this targets.
    private func clearShaderCache() {
        let docsPath = ARMSX2Bridge.documentsDirectory()
        let cacheURL = URL(fileURLWithPath: (docsPath as NSString).appendingPathComponent("cache"), isDirectory: true)
        let fileManager = FileManager.default
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: cacheURL.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            shaderCacheResult = "Shader cache is already empty."
            showShaderCacheResult = true
            return
        }
        do {
            for child in try fileManager.contentsOfDirectory(at: cacheURL, includingPropertiesForKeys: nil) {
                try? fileManager.removeItem(at: child)
            }
            shaderCacheResult = "Shader cache cleared."
        } catch {
            shaderCacheResult = "Could not fully clear the cache: \(error.localizedDescription)"
        }
        showShaderCacheResult = true
    }

    /// Labeled picker over an explicit (label, value) option list, used for the GS
    /// hardware-fix enums whose values may be non-contiguous (e.g. anisotropy).
    @ViewBuilder
    private func intPicker(_ title: String, selection: Binding<Int>, options: [(String, Int)]) -> some View {
        Picker(settings.localized(title), selection: selection) {
            ForEach(Array(options.enumerated()), id: \.offset) { _, option in
                Text(settings.localized(option.0)).tag(option.1)
            }
        }
    }

    /// Same picker over a shared `SettingsOptions` list, which the per-game tabs read too.
    private func intPicker(_ title: String, selection: Binding<Int>, shared: [(id: Int, title: String)]) -> some View {
        intPicker(title, selection: selection, options: shared.map { ($0.title, $0.id) })
    }
}
