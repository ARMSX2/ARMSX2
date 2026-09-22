// PerGameShaderSelection.swift — the shader chain one game gets, and how it survives a reinstall
// SPDX-License-Identifier: GPL-3.0+

import Foundation

/// No keys means the global chain. `ShaderChainEnabled = false`, or a token naming no file, means none.
enum PerGameShaderSelection {
    static let section = "EmuCore/GS"

    /// Written and cleared together: the token names the preset, the path caches it for core.
    static let keys = (
        enabled: "ShaderChainEnabled",
        presetRef: "ShaderChainPresetRef",
        presetPath: "ShaderChainPreset")

    /// Re-roots a game's token against this launch's container, and pushes what it resolves to.
    static func repair(forISO isoName: String) {
        let token = string(keys.presetRef, useCurrent: false, iso: isoName)
        guard !token.isEmpty else { return }
        guard let url = ShaderPresetLibrary.resolve(token) else {
            delete(keys.presetRef, useCurrent: false, iso: isoName)
            delete(keys.presetPath, useCurrent: false, iso: isoName)
            setBool(keys.enabled, false, useCurrent: false, iso: isoName)
            return
        }
        // Nothing else pushes this one: the settings store never reads the per-game file.
        ShaderParams.pushStored(token: token)
        guard string(keys.presetPath, useCurrent: false, iso: isoName) != url.path else { return }
        setString(keys.presetPath, url.path, useCurrent: false, iso: isoName)
    }

    /// -1 use global, 0 off, 1 on, the sentinel every other per-game control uses.
    static func loadedChain(useCurrent: Bool, iso: String) -> Int {
        guard has(keys.enabled, useCurrent: useCurrent, iso: iso) else { return -1 }
        return bool(keys.enabled, useCurrent: useCurrent, iso: iso) ? 1 : 0
    }

    static func loadedPresetRef(useCurrent: Bool, iso: String) -> String {
        string(keys.presetRef, useCurrent: useCurrent, iso: iso)
    }

    /// Off writes enabled false and drops the preset; no keys at all inherits the global setting.
    static func write(chain: Int, presetRef: String, useCurrent: Bool, iso: String) {
        // Enabled with no preset key would read the global preset from the base layer.
        let resolved = chain == 1 && !presetRef.isEmpty
            ? ShaderPresetLibrary.resolve(presetRef)
            : nil
        setBool(keys.enabled, resolved != nil, useCurrent: useCurrent, iso: iso)
        guard let url = resolved else {
            delete(keys.presetRef, useCurrent: useCurrent, iso: iso)
            delete(keys.presetPath, useCurrent: useCurrent, iso: iso)
            return
        }
        setString(keys.presetRef, presetRef, useCurrent: useCurrent, iso: iso)
        setString(keys.presetPath, url.path, useCurrent: useCurrent, iso: iso)
        ARMSX2Bridge.retryShaderChain()
    }

    static func clear(useCurrent: Bool, iso: String) {
        for key in [keys.enabled, keys.presetRef, keys.presetPath] {
            delete(key, useCurrent: useCurrent, iso: iso)
        }
    }

    private static func targetISO(useCurrent: Bool, iso: String) -> String? {
        useCurrent ? nil : iso
    }

    private static func has(_ key: String, useCurrent: Bool, iso: String) -> Bool {
        ARMSX2Bridge.hasPerGameINIValue(section, key: key, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func bool(_ key: String, useCurrent: Bool, iso: String) -> Bool {
        ARMSX2Bridge.getPerGameINIBool(section, key: key, defaultValue: false, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func string(_ key: String, useCurrent: Bool, iso: String) -> String {
        ARMSX2Bridge.getPerGameINIString(section, key: key, defaultValue: "", forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func setBool(_ key: String, _ value: Bool, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIBool(section, key: key, value: value, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func setString(_ key: String, _ value: String, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.setPerGameINIString(section, key: key, value: value, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }

    private static func delete(_ key: String, useCurrent: Bool, iso: String) {
        ARMSX2Bridge.deletePerGameINIValue(section, key: key, forISO: targetISO(useCurrent: useCurrent, iso: iso))
    }
}
