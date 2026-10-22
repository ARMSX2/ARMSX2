package com.armsx2.ui

import androidx.compose.runtime.mutableStateOf
import com.armsx2.EmuState
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.config.AutoTuneStore
import com.armsx2.config.ConfigStore
import com.armsx2.config.LiveGsApplyQueue
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import kr.co.iefriends.pcsx2.NativeApp

object InGameOverlay {
    val settingsState = mutableStateOf(Settings())
    val settingsScope = mutableStateOf(SettingsScope.Global)
    val currentSerial = mutableStateOf<String?>(null)
    val hardcoreOn = mutableStateOf(false)
    val frameLimitOn = mutableStateOf(true)

    /** OSD hotkey mode. The hotkey now CYCLES rather than plain on/off (Cotcho): Full (every
     *  stat) → Min (fps + CPU/EE/VU line) → Custom (the user's saved per-stat selection) → Off →
     *  Full. Transient and in-game only; never mutates the saved Settings selection, so Custom
     *  always reflects exactly what the user chose. Starts at Custom so a fresh boot shows their
     *  stats and the first press advances to Off, matching the old "first press hides". */
    enum class OsdMode { Full, Min, Custom, Off }
    val osdMode = mutableStateOf(OsdMode.Custom)
    private val osdCycle = listOf(OsdMode.Full, OsdMode.Min, OsdMode.Custom, OsdMode.Off)
    private const val OsdModeKey = "ui.osdMode"
    private var osdLoaded = false

    private fun ensureOsdLoaded() {
        if (osdLoaded) return
        osdLoaded = true
        val name = runCatching { MainActivityRuntime.prefs.getString(OsdModeKey, null) }.getOrNull()
        osdMode.value = OsdMode.entries.firstOrNull { it.name == name } ?: OsdMode.Custom
    }

    /** Set the OSD mode (from the in-game menu selector or the hotkey), apply it live, and
     *  persist it so it survives a relaunch — matching the old per-stat toggles' persistence. */
    fun setOsdMode(mode: OsdMode) {
        ensureOsdLoaded()
        osdMode.value = mode
        reapplyOsdMode()
        runCatching { MainActivityRuntime.prefs.edit().putString(OsdModeKey, mode.name).apply() }
    }

    /** Re-assert the stored OSD mode after a game's settings apply on boot, so a non-Custom
     *  choice (Full / Min / Off) isn't reset to the per-stat selection every launch. */
    fun applyStoredOsdMode() {
        ensureOsdLoaded()
        // Custom = "the user's saved per-stat flags". At boot settingsState is NOT yet populated
        // with THIS game's resolved settings, so reading it here applied stale/empty flags — which
        // hid an enabled stat until a reset repopulated it (#385). Resolve the current game's
        // settings ourselves. (A fresh install has every stat defaulting off, so this also cleanly
        // shows nothing by default rather than whatever stale state was left in settingsState.)
        if (osdMode.value == OsdMode.Custom) {
            applyOsdModeFlags(
                OsdMode.Custom,
                com.armsx2.config.ConfigStore.resolveForGame(
                    MainActivityRuntime.currentGame.value?.settingsKey,
                ),
            )
        } else {
            applyOsdMode(osdMode.value)
        }
    }

    /**
     * Re-assert a non-Custom OSD mode after [com.armsx2.config.Settings.applyTo] has run.
     *
     * applyTo pushes the per-stat osdShow* flags unconditionally, and it runs on EVERY settings
     * change — so changing anything at all, brightness, a speedhack, a controller binding, would
     * silently replace an active Full / Minimal / Off mode with the Custom flag set. For most
     * users the Custom set is mostly off, so what that looks like is the OSD vanishing whenever
     * you touch settings.
     *
     * Custom needs nothing done: applyTo just wrote exactly what Custom means. Only the three
     * modes that OVERRIDE the per-stat flags have to be restored, which is what
     * [reapplyOsdMode] already does for the second-display case.
     */
    fun reassertOsdModeAfterSettingsApply() {
        ensureOsdLoaded()
        if (osdMode.value != OsdMode.Custom)
            runCatching { applyOsdMode(osdMode.value) }
    }

    /** Short label for [mode], shown by the hotkey toast and the menu selector. */
    fun osdModeLabel(mode: OsdMode): String = when (mode) {
        OsdMode.Full -> "Full"
        OsdMode.Min -> "Minimal"
        OsdMode.Custom -> "Custom"
        OsdMode.Off -> "Off"
    }

    /** Per-tab scroll offset (px) of the in-game pause menu, retained across menu open/close so
     *  reopening a tab — especially the long Fixes list — returns to where you were instead of
     *  snapping back to the top. Keyed by EmulationMenuTab.name to avoid coupling to that enum. */
    val menuTabScroll = HashMap<String, Int>()

    fun saveSettings(changed: Settings) {
        val previous = settingsState.value
        val scope = settingsScope.value
        val serial = currentSerial.value?.takeIf { it.isNotBlank() }
        val running = MainActivityRuntime.eState.value != EmuState.STOPPED
        // A setting auto-tune moves, set by hand in a game's own settings, turns auto-tune off for that game. The global
        // settings are not a game's: changing those leaves the switch alone.
        val updated = if (scope == SettingsScope.Game) AutoTuneStore.withAutoTuneOffForManualChange(previous, changed) else changed
        settingsState.value = updated
        // `previous` matters: it's how ConfigStore tells a field the user just changed in
        // Game scope from one they never touched, so setting a per-game value that happens
        // to equal global still pins it instead of vanishing.
        ConfigStore.save(scope, currentSerial.value, updated, previous)
        frameLimitOn.value = updated.frameLimit.frameLimitEnable

        if (MainActivityRuntime.nativeReady.value) {
            val identity = LiveGsApplyQueue.captureVmIdentity()
            val stored = if (running && serial != null) ConfigStore.resolveForGame(serial) else null
            val global = if (stored != null) ConfigStore.loadGlobal() else null
            val restoreOsd = captureOsdMode(updated)
            LiveGsApplyQueue.previousSettings(identity, previous)
            if (previous.frameLimit.frameLimitEnable != updated.frameLimit.frameLimitEnable)
                MainActivityRuntime.fastForwardToggleActive = false
            if (running && previous.output.upscaleFloat != updated.output.upscaleFloat)
                MainActivityRuntime.upscale.value = updated.output.upscaleFloat.coerceIn(0.25f, 8.0f)
            LiveGsApplyQueue.enqueueSave(identity) {
                if (scope == SettingsScope.Game && serial != (identity.appKey ?: identity.serial)) return@enqueueSave
                val applied = LiveGsApplyQueue.previousSettings(identity, previous)
                if (applied.frameLimit.frameLimitEnable != updated.frameLimit.frameLimitEnable) {
                    NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", updated.frameLimit.frameLimitEnable.toString())
                    NativeApp.speedhackLimitermode(if (updated.frameLimit.frameLimitEnable) 0 else 3)
                }
                if (running && applied.output.upscaleFloat != updated.output.upscaleFloat)
                    NativeApp.renderUpscalemultiplier(updated.output.upscaleFloat.coerceIn(0.25f, 8.0f))
                if (running) {
                    // The commit reloads the game layer, so replace its file before applying.
                    if (stored != null && global != null) {
                        stored.writeGameSettingsIni(global, serial = serial, liveGame = true)
                        if (!identity.isCurrent()) return@enqueueSave
                        NativeApp.reloadGameSettingsLayer()
                    }
                    if (!identity.isCurrent()) return@enqueueSave
                    (if (scope == SettingsScope.Game) stored ?: updated else updated).applyTo(restoreOsd)
                }
                if (identity.isCurrent()) restoreOsd()
            }
        }

        // ...and the same regeneration when there is NO VM. Without this, editing settings from the
        // library left a stale gamesettings/<serial>_<CRC>.ini in place, and because that file loads
        // into a HIGHER-priority layer than anything we write, every key it already contained
        // silently ignored the user forever. Confirmed the hard way: Local Link came up correctly on
        // a game with no INI and never initialised on one with an old [DEV9/Eth] block, across six
        // back-to-back boots. Only the category-Reset path rewrote the INI, which is why Reset was
        // the only thing that ever "worked". Uses the by-serial overload since there is no running
        // game to reach it through; a no-op when the game never wrote an INI.
        if (MainActivityRuntime.eState.value == EmuState.STOPPED) {
            runCatching {
                currentSerial.value?.takeIf { it.isNotBlank() }?.let { serial ->
                    ConfigStore.resolveForGame(serial)
                        .writeGameSettingsIni(ConfigStore.loadGlobal(), serial, liveGame = false)
                }
            }
        }

        // What a game's screens show is what the game runs, and a save can change that beyond the
        // field it touched: turning the database's fixes off, or setting hardware fixes by hand,
        // stops entries applying, and putting a setting at the database's own value hands it back.
        // So show what is stored now rather than what was just edited.
        if (settingsScope.value == SettingsScope.Game) {
            currentSerial.value?.takeIf { it.isNotBlank() }?.let { serial ->
                if (com.armsx2.config.GameDbOverrides.entriesFor(serial).isNotEmpty()) {
                    settingsState.value = ConfigStore.resolveForDisplay(serial)
                }
            }
        }
    }

    fun open() {
        if (WindowImpl.overlayVisible.value) return
        // getPauseGameSerial() formats as "SLUS-21621 (A422BB13)", but GameInfo.settingsKey — what
        // every other reader and writer keys on — is the BARE serial. Used raw, this stored
        // settings under "SLUS-21621 (A422BB13)" while boot looked up "SLUS-21621", so per-game
        // settings silently never applied on any launch without a GameInfo (Boot Disc, Swap Disc,
        // BIOS). A BIOS boot is worse still: CRC 0 yields " (00000000)", which is not blank, so it
        // forced Game scope onto a phantom key. Strip to the serial and drop what's left if empty.
        val serial = MainActivityRuntime.currentGame.value?.settingsKey
            ?: runCatching { NativeApp.getPauseGameSerial() }.getOrNull()
                ?.substringBefore(" (")?.trim()?.takeIf(String::isNotBlank)
        currentSerial.value = serial
        settingsScope.value = if (serial == null) SettingsScope.Global else SettingsScope.Game
        settingsState.value = ConfigStore.resolveForDisplay(serial)
        frameLimitOn.value = settingsState.value.frameLimit.frameLimitEnable
        hardcoreOn.value = runCatching { NativeApp.isHardcoreMode() }.getOrDefault(false)
        if (MainActivityRuntime.eState.value != EmuState.STOPPED) MainActivityRuntime.pauseForOverlay()
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.MENU_OPEN)
        WindowImpl.overlayVisible.value = true
    }

    fun toggle() {
        if (WindowImpl.overlayVisible.value) closeAndResume() else open()
    }

    /** Advance the OSD cycle one step, apply it live, and return a short label for the
     *  on-screen note. Applies live-only — the saved per-stat Settings are never mutated. */
    fun cycleOsd(): String {
        ensureOsdLoaded()
        val next = osdCycle[(osdCycle.indexOf(osdMode.value) + 1) % osdCycle.size]
        setOsdMode(next)
        return "OSD: " + osdModeLabel(next)
    }

    fun captureOsdMode(settings: Settings = settingsState.value): () -> Unit {
        ensureOsdLoaded()
        val mode = osdMode.value
        return { applyOsdModeFlags(mode, settings) }
    }

    /** Re-push the CURRENT osd mode to native. Used when something suppressed the OSD live (the
     *  second-display panel routing it to the other screen) and needs to hand it back exactly as
     *  the user had it, without touching any saved setting. */
    fun reapplyOsdMode() {
        val restore = captureOsdMode()
        LiveGsApplyQueue.enqueueForVm { restore() }
    }

    private fun applyOsdMode(mode: OsdMode) {
        applyOsdModeFlags(mode, settingsState.value)
    }

    private fun applyOsdModeFlags(mode: OsdMode, settings: Settings) {
        if (com.armsx2.SecondScreen.osdSuppressed) {
            NativeApp.osdApplyFlags(false, false, false, false, false, false, false, false, false, false, false, false)
            NativeApp.osdShowGpuStats(false)
            return
        }
        // Every mode also drives the GPU pipeline-stats line (VSI/PSI) explicitly: it has its
        // own setter outside osdApplyFlags' 12 flags, so leaving it out is what let it survive
        // the old "off" toggle and stay on screen (Cotcho).
        when (mode) {
            OsdMode.Full -> {
                NativeApp.osdApplyFlags(true, true, true, true, true, true, true, true, true, true, true, true)
                NativeApp.osdShowGpuStats(true)
            }
            OsdMode.Min -> {
                // fps + the CPU line (EE/VU/GS breakdown) — the at-a-glance set Cotcho described.
                NativeApp.osdApplyFlags(true, false, false, true, false, false, false, false, false, false, false, false)
                NativeApp.osdShowGpuStats(false)
            }
            OsdMode.Custom -> applyOsdFlags(settings)
            OsdMode.Off -> {
                NativeApp.osdApplyFlags(false, false, false, false, false, false, false, false, false, false, false, false)
                NativeApp.osdShowGpuStats(false)
            }
        }
    }

    /** Push a Settings object's saved per-stat OSD selection to native — the Custom mode. Split
     *  out so applyStoredOsdMode can feed it the boot-resolved settings (settingsState isn't ready
     *  yet at boot), while the live path feeds it settingsState. */
    private fun applyOsdFlags(s: com.armsx2.config.Settings) {
        NativeApp.osdApplyFlags(
            s.osd.osdShowFps, s.osd.osdShowVps, s.osd.osdShowSpeed, s.osd.osdShowCpu, s.osd.osdShowGpu,
            s.osd.osdShowResolution, s.osd.osdShowGsStats, s.osd.osdShowFrameTimes, s.osd.osdShowHardwareInfo,
            s.osd.osdShowVersion, s.osd.osdShowSettings, s.osd.osdShowInputs,
        )
        NativeApp.osdShowGpuStats(s.osd.osdShowGpuStats)
    }

    fun editTouchLayout() {
        com.armsx2.ui.touch.TouchControls.ensureLoaded()
        com.armsx2.ui.touch.TouchControls.editMode.value = true
        WindowImpl.overlayVisible.value = false
    }

    fun openSaveStatePicker() {
        // Freeze the game behind the (opaque) picker; dismiss resumes it. The
        // pause-menu path is already paused, so this only bites the on-screen
        // touch-button path that fires mid-game.
        if (MainActivityRuntime.eState.value != EmuState.STOPPED) MainActivityRuntime.pauseForOverlay()
        WindowImpl.openInGameScreen(InGameScreen.SaveState)
    }

    fun openLoadStatePicker() {
        if (MainActivityRuntime.eState.value != EmuState.STOPPED) MainActivityRuntime.pauseForOverlay()
        WindowImpl.openInGameScreen(InGameScreen.LoadState)
    }

    private fun closeAndResume() {
        WindowImpl.overlayVisible.value = false
        // Resume on PAUSED *or* RUNNING. Opening the menu queues the pause asynchronously —
        // eState only flips to PAUSED once Host::OnVMPaused fires on the CPU thread — so a quick
        // open→close can reach here with eState still RUNNING, and gating on == PAUSED skipped
        // the resume, leaving the game stuck paused once the queued pause landed. resume() is safe
        // in both cases: it's ordered after the pending pause (FIFO on the CPU thread), and it's a
        // no-op when the VM is genuinely still running.
        val st = MainActivityRuntime.eState.value
        if ((st == EmuState.PAUSED || st == EmuState.RUNNING) && !WindowImpl.showLibrary.value &&
            !com.armsx2.ui.touch.TouchControls.editMode.value
        ) {
            MainActivityRuntime.resume()
        }
    }
}
