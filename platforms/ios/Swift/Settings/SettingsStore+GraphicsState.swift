// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Graphics-hack state mutations compile independently from the
// observable stored-property declaration.
extension SettingsStore {
    func graphicsHack(_ key: String) -> GraphicsHackStatus? {
        graphicsHackStatus[key]
    }

    func refreshGraphicsHackStatus() {
        var parsed: [String: GraphicsHackStatus] = [:]
        for (key, value) in ARMSX2Bridge.graphicsHackState() {
            guard let entry = value as? [String: Any],
                  let effective = entry["effective"] as? Int,
                  let rawReason = entry["reason"] as? Int,
                  let reason = GraphicsHackReason(rawValue: rawReason),
                  let pinned = entry["pinned"] as? Bool else { continue }
            parsed[key] = GraphicsHackStatus(effective: effective, reason: reason, pinned: pinned)
        }
        graphicsHackStatus = parsed
    }

    /// Claims a hack for the player so the GameDB stops overwriting that one. Shares the
    /// debounced apply with the row's own write, so claiming and changing in one gesture
    /// costs one apply rather than two.
    func setGraphicsHackPinned(_ key: String, _ pinned: Bool) {
        ARMSX2Bridge.setGraphicsHackPinned(key, pinned: pinned)
        requestGraphicsApplyGuarded()
    }

    /// Unpinning hands the hack back to the automatics, so the stored value goes back
    /// to its default too, or the row keeps showing a value the core will discard.
    func resetGraphicsHackValue(_ key: String) {
        switch key {
        case "UserHacks_align_sprite_X": alignSprite = false
        case "UserHacks_merge_pp_sprite": mergeSprite = false
        case "UserHacks_round_sprite_offset": roundSprite = 0
        case "UserHacks_HalfPixelOffset": halfPixelOffset = 0
        case "UserHacks_ForceEvenSpritePosition": wildArmsOffset = false
        case "UserHacks_native_scaling": nativeScaling = 0
        case "UserHacks_TCOffsetX": textureOffsetX = 0
        case "UserHacks_TCOffsetY": textureOffsetY = 0
        case "UserHacks_TextureInsideRt": textureInsideRt = 0
        case "UserHacks_BilinearHack": bilinearUpscaleHack = 0
        case "UserHacks_Limit24BitDepth": limit24BitDepth = 0
        case "UserHacks_CPUSpriteRenderBW": cpuSpriteRenderBw = 0
        case "UserHacks_CPUSpriteRenderLevel": cpuSpriteRenderLevel = 0
        case "UserHacks_CPUCLUTRender": cpuClutRender = 0
        case "UserHacks_GPUTargetCLUTMode": gpuTargetClut = 0
        default: setGSBoolHack(key, false)
        }
    }

    /// Homogeneous bool GS hacks — see SettingsStore+Graphics.swift for the option list.

    func gsBoolHackEnabled(_ key: String) -> Bool {
        gsBoolHacks[key] ?? false
    }

    /// The single write funnel for those hacks. They live in a dictionary rather
    /// than a Setting<T>, so the default EmuCore/GS apply hook cannot reach them;
    /// this is their equivalent. Everything that changes one goes through here.
    func setGSBoolHack(_ key: String, _ value: Bool) {
        gsBoolHacks[key] = value
        guard !suppressINIWrites else { return }
        ARMSX2Bridge.setINIBool("EmuCore/GS", key: key, value: value)
        ARMSX2Bridge.setGraphicsHackPinned(key, pinned: true)
        requestGraphicsApplyGuarded()
    }

    static func loadGSBoolHacks() -> [String: Bool] {
        var values: [String: Bool] = [:]
        for option in gsBoolHackOptions {
            values[option.key] = ARMSX2Bridge.getINIBool("EmuCore/GS", key: option.key, defaultValue: false)
        }
        return values
    }
}
