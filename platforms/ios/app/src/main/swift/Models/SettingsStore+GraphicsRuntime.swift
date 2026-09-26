// SPDX-License-Identifier: GPL-3.0+
import Foundation

// Graphics apply and shader-selection workflows compile as their
// own primary file instead of enlarging the observable property declaration.
extension SettingsStore {
    /// Coalesces live applies of visual settings so rapid changes reload GS settings
    /// at most once per short window. It is a no-op while a visual slider is being
    /// dragged; the slider's editing-ended handler triggers the apply on release so a
    /// drag does not fire one apply per tick.
    func requestGraphicsApply() {
        guard visualSliderDragCount == 0 else {
            // Remember that something graphics-shaped moved, so the release knows whether
            // it has anything to apply. Every slider brackets now, including the audio and
            // virtual pad ones that never touch GS.
            graphicsApplyDeferred = true
            return
        }
        graphicsApplyWorkItem?.cancel()
        let workItem = DispatchWorkItem { ARMSX2Bridge.applyGraphicsSettingsNow() }
        graphicsApplyWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12, execute: workItem)
    }

    func commit<T>(_ setting: Setting<T>, _ value: T) {
        guard !(setting.suppressible && suppressINIWrites) else { return }
        setting.codec.write(setting.section, setting.key, value)
        if setting.appliesGraphics { requestGraphicsApplyGuarded() }
    }

    /// Nothing should reach this while the INI is loading, but it checks anyway:
    /// a reload there would throw away the values init has just read.
    func requestGraphicsApplyGuarded() {
        guard !suppressINIWrites else { return }
        requestGraphicsApply()
    }

    /// Marks the start of a visual slider drag so per-tick value changes do not each
    /// trigger a graphics reload. Balanced by endVisualSliderEdit(), which fires a
    /// single coalesced apply when the last drag ends.
    ///
    /// The watchdog releases a drag that is torn down without its editing-ended
    /// handler, preventing live apply from remaining disabled for the session.
    func beginVisualSliderEdit() {
        visualSliderDragCount += 1
        visualSliderWatchdog?.cancel()
        let watchdog = DispatchWorkItem { [weak self] in
            guard let self, self.visualSliderDragCount > 0 else { return }
            self.visualSliderDragCount = 0
            self.finishVisualSliderEdits()
        }
        visualSliderWatchdog = watchdog
        DispatchQueue.main.asyncAfter(deadline: .now() + 30, execute: watchdog)
    }

    func endVisualSliderEdit() {
        if visualSliderDragCount > 0 { visualSliderDragCount -= 1 }
        guard visualSliderDragCount == 0 else { return }
        finishVisualSliderEdits()
    }

    /// Only reload if a graphics key actually moved while the bracket was up. Dragging an
    /// audio or virtual pad slider raises the same count and has nothing to apply.
    func finishVisualSliderEdits() {
        visualSliderWatchdog?.cancel()
        visualSliderWatchdog = nil
        guard graphicsApplyDeferred else { return }
        graphicsApplyDeferred = false
        requestGraphicsApply()
    }

    /// Keeps core's absolute ShaderChainPreset in step with the token, and sends the saved
    /// parameter values for it. A token that no longer names a file clears the selection
    /// instead of leaving core pointed at a missing preset.
    func applyShaderChainSelection() {
        guard !shaderChainPresetRef.isEmpty else {
            ARMSX2Bridge.setINIString("EmuCore/GS", key: "ShaderChainPreset", value: "")
            return
        }
        guard let url = ShaderPresetLibrary.resolve(shaderChainPresetRef) else {
            shaderChainEnabled = false
            shaderChainPresetRef = ""
            return
        }
        ARMSX2Bridge.setINIString("EmuCore/GS", key: "ShaderChainPreset", value: url.path)
        ShaderParams.pushStored(token: shaderChainPresetRef)
        ARMSX2Bridge.retryShaderChain()
    }

    /// Re-roots the selection against the container this launch got, before the GS device
    /// first reads the config. A value an older build left is a bare absolute path, which is
    /// turned into a token here or dropped if it names nothing under either root any more.
    static func migrateShaderChainSelectionV1() {
        let stored = ARMSX2Bridge.getINIString("EmuCore/GS", key: "ShaderChainPreset", defaultValue: "")
        var token = ARMSX2Bridge.getINIString("EmuCore/GS", key: "ShaderChainPresetRef", defaultValue: "")
        guard !token.isEmpty || !stored.isEmpty else { return }
        if token.isEmpty { token = ShaderPresetLibrary.token(forLegacyPath: stored) ?? "" }
        guard let url = ShaderPresetLibrary.resolve(token) else {
            ARMSX2Bridge.setINIString("EmuCore/GS", key: "ShaderChainPresetRef", value: "")
            ARMSX2Bridge.setINIString("EmuCore/GS", key: "ShaderChainPreset", value: "")
            ARMSX2Bridge.setINIBool("EmuCore/GS", key: "ShaderChainEnabled", value: false)
            return
        }
        ARMSX2Bridge.setINIString("EmuCore/GS", key: "ShaderChainPresetRef", value: token)
        ARMSX2Bridge.setINIString("EmuCore/GS", key: "ShaderChainPreset", value: url.path)
        // The only other push is a selection changing, which a launch does not do, so without
        // this one the chain reaches its first frame on the preset's own numbers. Static and
        // bridge-only, like the rest of this function: init must not touch SettingsStore.shared.
        ShaderParams.pushStored(token: token)
    }
}
