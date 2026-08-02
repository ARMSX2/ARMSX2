// SettingsOptions.swift — option lists shared by the global settings screens and the per-game tabs.
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// One list per setting, so a global screen and its per-game tab cannot say different things.
/// UpscaleOptions did this for internal resolution after its two copies drifted; these are the rest.
///
/// Ids are the values the core stores. Titles use the global screen's wording, which is what the
/// translation tables already carry.
enum SettingsOptions {
    /// Prefix for a per-game picker: -1 means inherit rather than override.
    static let useGlobalID = -1

    static func withUseGlobal(_ options: [(id: Int, title: String)],
                              title: String = "Use Global") -> [(id: Int, title: String)] {
        [(id: useGlobalID, title: title)] + options
    }

    // Tags are GSInterlaceMode (pcsx2/Config.h). Automatic lets the game's own GameDB deinterlace
    // fix apply; anything else suppresses it.
    static let deinterlace: [(id: Int, title: String)] = [
        (0, "Automatic (Default)"), (1, "Off (No Deinterlacing)"),
        (2, "Weave (TFF)"), (3, "Weave (BFF)"),
        (4, "Bob (TFF)"), (5, "Bob (BFF)"),
        (6, "Blend (TFF)"), (7, "Blend (BFF)"),
        (8, "Adaptive (TFF)"), (9, "Adaptive (BFF)")
    ]

    static let tvShader: [(id: Int, title: String)] = [
        (0, "Off"), (1, "Scanline"), (2, "Diagonal"), (3, "Tri"),
        (4, "Wave"), (5, "Lottes"), (6, "4xRGSS"), (7, "NxAGSS")
    ]

    // Catalyst does not ship these, and both screens have to hide them together or the per-game
    // tab offers a renderer the global one denies.
    static let renderer: [(id: Int, title: String)] = {
        var options: [(id: Int, title: String)] = [(17, "Metal (Hardware)")]
#if !targetEnvironment(macCatalyst)
        options.append((13, "Software"))
        options.append((11, "Null (No Output)"))
#endif
        return options
    }()

    static let maxAnisotropy: [(id: Int, title: String)] = [
        (0, "Off"), (2, "2x"), (4, "4x"), (8, "8x"), (16, "16x")
    ]

    static let hardwareDownloadMode: [(id: Int, title: String)] = [
        (0, "Enabled"), (1, "Force Full"), (2, "No Readbacks"),
        (3, "Unsynchronized"), (4, "Disabled")
    ]

    static let cpuClutRender: [(id: Int, title: String)] = [
        (0, "Disabled"), (1, "Normal"), (2, "Aggressive")
    ]

    static let gpuTargetClut: [(id: Int, title: String)] = [
        (0, "Off"), (1, "Enabled (Exact Match)"), (2, "Enabled (Inside Target)")
    ]

    static let textureInsideRT: [(id: Int, title: String)] = [
        (0, "Off"), (1, "Inside Targets"), (2, "Merge Targets")
    ]

    // -1 is a real TriFiltering value (Automatic), which is why the per-game "use global" marker for
    // this one has to be Int32.min rather than the usual -1.
    static let trilinearFiltering: [(id: Int, title: String)] = [
        (-1, "Automatic / Default"), (0, "Off"), (1, "PS2"), (2, "Forced")
    ]

    static let halfPixelOffset: [(id: Int, title: String)] = [
        (0, "Off"), (1, "Normal / Vertex"), (2, "Special / Texture"),
        (3, "Special / Texture Aggressive"), (4, "Align to Native"),
        (5, "Align to Native + Texture Offset")
    ]

    static let roundSprite: [(id: Int, title: String)] = [
        (0, "Off"), (1, "Half"), (2, "Full")
    ]
}
