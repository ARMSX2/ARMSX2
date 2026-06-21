package com.armsx2.ui

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.displayCutoutPadding
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Text
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.vector.ImageVector
import compose.icons.LineAwesomeIcons
import compose.icons.lineawesomeicons.CompactDiscSolid
import compose.icons.lineawesomeicons.CubeSolid
import compose.icons.lineawesomeicons.EyeSlashSolid
import compose.icons.lineawesomeicons.EyeSolid
import compose.icons.lineawesomeicons.FolderOpenSolid
import compose.icons.lineawesomeicons.PlaySolid
import compose.icons.lineawesomeicons.PowerOffSolid
import compose.icons.lineawesomeicons.RedoAltSolid
import compose.icons.lineawesomeicons.SaveSolid
import compose.icons.lineawesomeicons.TachometerAltSolid
import compose.icons.lineawesomeicons.ThLargeSolid
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.keyframes
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.foundation.focusable
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.draw.shadow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.runtime.setValue
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import com.armsx2.EmuState
import com.armsx2.GameInfo
import com.armsx2.Main
import com.armsx2.R
import com.armsx2.config.ConfigStore
import com.armsx2.config.LiveGsApplyQueue
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.ui.settings.AudioTab
import com.armsx2.ui.settings.FixesTab
import com.armsx2.ui.settings.HotkeysTab
import com.armsx2.ui.settings.NetworkTab
import com.armsx2.ui.settings.OverlayTab
import com.armsx2.ui.settings.PadTab
import com.armsx2.ui.settings.PatchesTab
import com.armsx2.ui.settings.PerformanceTab
import com.armsx2.ui.settings.RecompilerTab
import com.armsx2.ui.settings.RendererTab
import com.armsx2.ui.settings.SettingsControllerNav
import kr.co.iefriends.pcsx2.NativeApp

/**
 * In-game pause overlay. Layout matches the look of pcsx2-qt's in-game
 * fullscreen menu:
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ Game Title                          ARMSX2  [logo]   │
 *   │ SLUS-12345 · ★★★★☆                  v2.7.304-3       │
 *   │                                                       │
 *   │              (game surface visible through            │
 *   │               the partial-alpha backdrop)             │
 *   │                                                       │
 *   │ Resume Game                                           │
 *   │ Show Toolbar                                          │
 *   │ Save State                                            │
 *   │ ...                                                   │
 *   │ Close Game                                            │
 *   └──────────────────────────────────────────────────────┘
 *
 * Trigger: long-press on the game surface while RUNNING (Main.kt's
 * detectTapGestures handler). Open auto-pauses; close paths inside the
 * overlay decide whether to resume.
 *
 * Mirrors PCSX2 ImGui FullscreenUI's DrawPauseMenu (FullscreenUI.cpp:1549+)
 * minus features that aren't wired on the Android port (achievements,
 * per-game props, screenshot snapshot).
 */
object InGameOverlay {
    private sealed class State {
        data object Root : State()
        data object SaveStateSlots : State()
        data object LoadStateSlots : State()
        data object ExitConfirm : State()
        data object ResetConfirm : State()
        data object AchievementsLogin : State()
        data object Achievements : State()
        data object HardcoreEnableConfirm : State()
        data object HardcoreSaveStateBlocked : State()
    }

    private val state = mutableStateOf<State>(State.Root)
    private val settingsOnly = mutableStateOf(false)
    private val playSelection = mutableStateOf(0)
    private val modalSelection = mutableStateOf(0)
    private var settingsAdjustHeldDir = 0

    // Tab selection inside the Root state. Tabs are config groups —
    // PlayingNow holds the existing pause-menu options (Resume / Save
    // State / Swap Disc / Reset / Close / etc), Performance and
    // Renderer host the speedhack + GS toggles backed by ConfigStore.
    private enum class Tab(val label: String) {
        PlayingNow("Play"),
        Performance("Perf"),
        Renderer("Render"),
        Fixes("Fixes"),
        Audio("Audio"),
        Patches("Patches"),
        Network("Network"),
        Overlay("Overlay"),
        Pad("Pad"),
        Hotkeys("Hotkeys"),
        Recompiler("JIT"),
    }
    private val currentTab = mutableStateOf(Tab.PlayingNow)

    // Live Settings state shared across the Performance + Renderer tabs.
    // Hydrated from ConfigStore on every overlay open so we pick up any
    // out-of-band edits (e.g. from a future global Settings screen).
    private val settingsState = mutableStateOf(Settings())
    private val previewGame = mutableStateOf<GameInfo?>(null)

    /** The library game whose settings were opened via long-press before
     *  launch (null once a game is actually running). Lets the Patches tab
     *  browse the patch database for a game you haven't booted yet. */
    val patchPreviewGame: GameInfo? get() = previewGame.value

    // Settings scope picked by the overlay header switch. Defaults to
    // [Game] when a game is loaded on open; falls back to [Global] when
    // no game serial is available. The settings tabs read this to decide
    // which tier to persist a change to (see ConfigStore.save). Held on
    // the overlay singleton so it survives tab switches but resets on
    // each open (the default-from-serial pass runs in open()).
    val settingsScope = mutableStateOf(SettingsScope.Global)

    // Serial of the currently-loaded game (null when on BIOS / disc swap
    // limbo). Resolved on overlay open via the cached Main.currentGame
    // first, falling back to NativeApp.getPauseGameSerial — same chain
    // GameInfoHeader uses. The scope toggle's "Game" option is gated on
    // this being non-null.
    val currentSerial = mutableStateOf<String?>(null)

    // Polled from NativeApp.isHardcoreMode while the overlay is open. Drives
    // the Save/Load state row dimming and the AchievementsPanel button
    // colour. Updates on every overlay open and every AchievementsPanel
    // poll (which already runs every 4s) — see Render() below.
    val hardcoreOn = mutableStateOf(false)

    // Controller scroll signal for the signed-in achievement list (a lazy list,
    // so it's scrolled rather than item-nav'd). Each ±1 = one step up/down; the
    // AchievementsPanel observes the delta and scrolls its LazyColumn.
    val achievementsScroll = mutableStateOf(0)

    // Locally tracked frame-limit toggle. 0 = Nominal (60fps cap),
    // 3 = Unlimited. Matches LimiterModeType / SpeedhackButton wiring.
    //
    // Default ON at app start (60fps cap). The state is held by this
    // singleton object so within an app session toggling once persists
    // across game launches — Main.start() re-applies it to native after
    // Settings.applyTo() runs on each VM init. Process restart resets to
    // the default (ON).
    val frameLimitOn = mutableStateOf(true)

    /** Renderer pill mode. Cycle: Auto → Hardware → Software → Auto.
     *
     *  - Auto = the default. Honours the wizard's backend choice
     *    (Main.renderer = "opengl" / "vulkan") and uses its HW path, but
     *    does NOT pin anything — emucore is free to swap to SW for
     *    things like SoftwareRendererFMVHack during FMVs, and the pill
     *    sync stays out of the way so the label keeps reading "Auto".
     *  - Hardware = pin HW on the picked backend.
     *  - Software = pin SW (display still on the picked backend's device).
     *
     *  We do NOT call NativeApp.renderAuto() here because it writes
     *  GSRendererType::Auto which then asks GSUtil::GetPreferredRenderer
     *  to pick a backend — on Android that resolves to OpenGL regardless
     *  of the wizard pick, silently rebuilding the device and (for Vulkan
     *  users) breaking the user's chosen backend. Auto's HW path is the
     *  same JNI as Hardware; the difference is purely label + sync. */
    enum class RendererMode { Auto, Hardware, Software }

    // Mirrored from native via NativeApp.isHardwareRenderer() on open +
    // on the achievements panel's 4s poll so an emucore-driven swap
    // (e.g. SoftwareRendererFMVHack flipping to SW during an FMV) doesn't
    // desync the Hardware / Software label. Sync is suppressed while in
    // Auto so the label stays "Auto" — the user picked "let it manage".
    val rendererMode = mutableStateOf(RendererMode.Auto)

    // OSD master toggle. Default ON because native-lib's initialize() turns
    // every OsdShow* bit on at first init; we only need to mirror the state
    // here so the pill label is right and re-toggling flips them all back.
    // Singleton state means the in-session preference persists across game
    // launches, matching the frame-limit pill pattern.
    private val osdShown = mutableStateOf(true)

    // Live RetroAchievements rich-presence string. Written by
    // AchievementsPanel's 4s poll (it already polls the achievements JSON
    // on the same cadence; one more JNI call is free). Read by
    // GameInfoHeader so the in-game pause panel reflects the current RP
    // line right under the game serial / star rating row.
    val richPresence = mutableStateOf("")

    // Tracks whether THIS overlay is the one that paused the VM. If the
    // user already had the VM paused (e.g. via toolbar) before opening
    // the overlay, closing the overlay shouldn't auto-resume.
    private var pausedByOverlay = false

    /** Build version string read at runtime from BuildVersion::GitRev
     *  via NativeApp.getBuildVersion(). Format
     *  "GitTagHi.GitTagMid.GitTagLo.ARMSX2Build-SNAPSHOT". Lazy so the
     *  JNI call defers to first use; cached for subsequent reads.
     *  Mirrored by SetupImpl so the wizard's title row stays in sync. */
    private val versionString: String by lazy {
        runCatching { NativeApp.getBuildVersion() }
            .getOrNull()
            ?.takeIf { it.isNotEmpty() }
            ?.let { "v$it" }
            ?: ""
    }

    /**
     * Single entry point for the Performance / Renderer / Recompiler tabs
     * to persist a settings change. Routes through ConfigStore.save which
     * picks the global or per-game tier based on the overlay's current
     * scope, then pushes the merged effective settings to native.
     *
     * The Settings object held in [settingsState] is already the
     * effective (global ∘ overrides) view, so applying it directly to
     * native is correct regardless of where the persisted bytes landed.
     */
    fun saveSettings(updated: Settings) {
        val previous = settingsState.value
        settingsState.value = updated
        ConfigStore.save(settingsScope.value, currentSerial.value, updated)
        if (Main.eState.value == EmuState.STOPPED) {
            updated.applyTo()
        } else {
            applySafeLiveDelta(previous, updated)
        }
        frameLimitOn.value = updated.frameLimitEnable
        osdShown.value = anyOsdElementEnabled(updated)
    }

    private fun anyOsdElementEnabled(settings: Settings): Boolean =
        settings.osdShowFps ||
            settings.osdShowVps ||
            settings.osdShowSpeed ||
            settings.osdShowCpu ||
            settings.osdShowGpu ||
            settings.osdShowResolution ||
            settings.osdShowGsStats ||
            settings.osdShowFrameTimes

    private fun withAllOsdElements(settings: Settings, enabled: Boolean): Settings =
        settings.copy(
            osdShowFps = enabled,
            osdShowVps = enabled,
            osdShowSpeed = enabled,
            osdShowCpu = enabled,
            osdShowGpu = enabled,
            osdShowResolution = enabled,
            osdShowGsStats = enabled,
            osdShowFrameTimes = enabled,
        )

    private fun syncQuickTogglesFromSettings(settings: Settings) {
        frameLimitOn.value = settings.frameLimitEnable
        osdShown.value = anyOsdElementEnabled(settings)
    }

    private fun applySafeLiveDelta(previous: Settings, updated: Settings) {
        // Keep this path light. Full Settings.applyTo() calls native
        // commitSettings(), which parks the VM and can rebuild GS state; doing
        // that from every in-game tap caused ANRs/crashes when users scrubbed
        // display/renderer options. Only apply tiny runtime-safe deltas here.
        // Everything else is persisted by ConfigStore.save() above and takes
        // effect on restart / next launch.
        if (previous.frameLimitEnable != updated.frameLimitEnable) {
            NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", updated.frameLimitEnable.toString())
            NativeApp.speedhackLimitermode(if (updated.frameLimitEnable) 0 else 3)
        }

        // Speed Limit / Custom FPS — setNominalSpeed is a light direct re-pace
        // (sets EmuConfig.NominalScalar + UpdateTargetSpeed), no VM park, so it's
        // safe on this in-game delta path. Persistence is handled by ConfigStore.
        if (previous.nominalSpeedPercent != updated.nominalSpeedPercent)
            NativeApp.setNominalSpeed(updated.nominalSpeedPercent.coerceIn(10, 1000))

        // Audio — SPU2 setters apply live to the open stream, no VM park.
        if (previous.audioVolume != updated.audioVolume)
            NativeApp.setAudioVolume(updated.audioVolume.coerceIn(0, 200))
        if (previous.audioMuted != updated.audioMuted)
            NativeApp.setAudioMuted(updated.audioMuted)

        // SyncMode / buffer / latency / FF-volume reconfigure the SPU2 stream, so
        // they need the commit path (ApplySettings → spu2 ApplyConfig). Parks the
        // VM briefly; only fires when one of them actually changed.
        if (previous.audioTimeStretch != updated.audioTimeStretch ||
            previous.audioBufferMs != updated.audioBufferMs ||
            previous.audioOutputLatencyMs != updated.audioOutputLatencyMs ||
            previous.audioFastForwardVolume != updated.audioFastForwardVolume) {
            NativeApp.setSetting("SPU2/Output", "SyncMode", "string",
                if (updated.audioTimeStretch) "TimeStretch" else "Disabled")
            NativeApp.setSetting("SPU2/Output", "BufferMS", "int", updated.audioBufferMs.coerceIn(10, 200).toString())
            NativeApp.setSetting("SPU2/Output", "OutputLatencyMS", "int", updated.audioOutputLatencyMs.coerceIn(5, 200).toString())
            NativeApp.setSetting("SPU2/Output", "FastForwardVolume", "int", updated.audioFastForwardVolume.coerceIn(0, 200).toString())
            NativeApp.commitSettings()
        }

        if (previous.vu1Instant != updated.vu1Instant)
            NativeApp.setInstantVU1(updated.vu1Instant)

        // EE Cycle Rate / Skip are baked into compiled blocks (recScaleBlockCycles
        // runs at compile time), so a change only takes effect once the EE rec is
        // reset. That needs the full commit path: setSetting + commitSettings →
        // VMManager::ApplySettings → CheckForConfigChanges → ClearCPUExecutionCaches.
        // It parks the VM (heavier than the other live deltas), but it's the only
        // way these actually apply in-game — without it they silently do nothing.
        if (previous.eeCycleRate != updated.eeCycleRate || previous.eeCycleSkip != updated.eeCycleSkip) {
            NativeApp.setSetting("EmuCore/Speedhacks", "EECycleRate", "int", updated.eeCycleRate.toString())
            NativeApp.setSetting("EmuCore/Speedhacks", "EECycleSkip", "int", updated.eeCycleSkip.toString())
            NativeApp.commitSettings()
        }

        // Manual frameskip — GS-thread global, applies on the next VSync. No
        // VM park, so it's safe to push live.
        if (previous.frameSkip != updated.frameSkip)
            NativeApp.setFrameSkip(updated.frameSkip.coerceIn(0, 5))

        if (previous.aspectRatio != updated.aspectRatio) {
            val ratio = updated.aspectRatio.coerceIn(0, 4)
            val name = when (ratio) {
                0 -> "Stretch"
                2 -> "4:3"
                3 -> "16:9"
                4 -> "10:7"
                else -> "Auto 4:3/3:2"
            }
            NativeApp.setSetting("EmuCore/GS", "AspectRatio", "string", name)
            NativeApp.setAspectRatio(ratio)
        }

        if (previous.loadTextureReplacements != updated.loadTextureReplacements)
            NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacements", "bool", updated.loadTextureReplacements.toString())
        if (previous.loadTextureReplacementsAsync != updated.loadTextureReplacementsAsync)
            NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", updated.loadTextureReplacementsAsync.toString())
        if (previous.precacheTextureReplacements != updated.precacheTextureReplacements)
            NativeApp.setSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool", updated.precacheTextureReplacements.toString())
        if (previous.dumpReplaceableTextures != updated.dumpReplaceableTextures)
            NativeApp.setSetting("EmuCore/GS", "DumpReplaceableTextures", "bool", updated.dumpReplaceableTextures.toString())
        if (previous.osdShowTextureReplacements != updated.osdShowTextureReplacements)
            NativeApp.setSetting("EmuCore/GS", "OsdShowTextureReplacements", "bool", updated.osdShowTextureReplacements.toString())

        // Performance Overlay element toggles. Persist to base (survives the
        // next ApplySettings reload) AND push live via the native setter,
        // which flips EmuConfig.GS + MTGS::ApplySettings. Disabling GPU also
        // stops the GPU timing queries (the perf win the tester asked for).
        if (previous.osdShowFps != updated.osdShowFps) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowFPS", "bool", updated.osdShowFps.toString())
            NativeApp.osdShowFPS(updated.osdShowFps)
        }
        if (previous.osdShowVps != updated.osdShowVps) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowVPS", "bool", updated.osdShowVps.toString())
            NativeApp.osdShowVPS(updated.osdShowVps)
        }
        if (previous.osdShowSpeed != updated.osdShowSpeed) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowSpeed", "bool", updated.osdShowSpeed.toString())
            NativeApp.osdShowSpeed(updated.osdShowSpeed)
        }
        if (previous.osdShowCpu != updated.osdShowCpu) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowCPU", "bool", updated.osdShowCpu.toString())
            NativeApp.osdShowCPU(updated.osdShowCpu)
        }
        if (previous.osdShowGpu != updated.osdShowGpu) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowGPU", "bool", updated.osdShowGpu.toString())
            NativeApp.osdShowGPU(updated.osdShowGpu)
        }
        if (previous.osdShowResolution != updated.osdShowResolution) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowResolution", "bool", updated.osdShowResolution.toString())
            NativeApp.osdShowResolution(updated.osdShowResolution)
        }
        if (previous.osdShowGsStats != updated.osdShowGsStats) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowGSStats", "bool", updated.osdShowGsStats.toString())
            NativeApp.osdShowGSStats(updated.osdShowGsStats)
        }
        if (previous.osdShowFrameTimes != updated.osdShowFrameTimes) {
            NativeApp.setSetting("EmuCore/GS", "OsdShowFrameTimes", "bool", updated.osdShowFrameTimes.toString())
            NativeApp.osdShowFrameTimes(updated.osdShowFrameTimes)
        }

        // Renderer / hardware-fix / upscaling-fix changes apply live via a GS-only
        // reconfigure (Settings.applyGsLive → native applyGSSettingsLive). It does
        // NOT rebuild the CPU/JIT and preserves the device-identity fields, so it
        // can't trigger a GS device recreate. Gated on an actual GS diff so non-GS
        // taps (audio, frame limit, …) don't reconfigure the GS thread.
        if (previous.gsDiffersFrom(updated))
            LiveGsApplyQueue.applySettings(updated)
    }

    /** Toggle the overlay open/closed — for a physical "menu" button binding.
     *  Closing resumes the VM (the normal close-and-resume path). */
    fun toggle() {
        if (WindowImpl.overlayVisible.value) closeAndResume()
        else open()
    }

    /** Open the dedicated achievements view. Bound to the "Open Achievements"
     *  hotkey and the header trophy button. */
    fun openAchievements() {
        if (!WindowImpl.overlayVisible.value) open()
        SettingsControllerNav.clearSelection()
        state.value = State.Achievements
    }

    fun handleControllerMove(dx: Int, dy: Int): Boolean {
        if (!WindowImpl.overlayVisible.value) return false
        when (state.value) {
            is State.Root -> {
                if (currentTab.value == Tab.PlayingNow && !settingsOnly.value) {
                    val row = playSelection.value / 4
                    val col = playSelection.value % 4
                    val nextRow = (row + dy).coerceIn(0, 2)
                    val nextCol = (col + dx).coerceIn(0, 3)
                    playSelection.value = nextRow * 4 + nextCol
                    return true
                }
                if (settingsTabActive()) {
                    if (dy != 0) {
                        settingsAdjustHeldDir = 0
                        return SettingsControllerNav.move(dy)
                    }
                    if (dx != 0) {
                        // D-pad only (the stick no longer drives adjust). Each
                        // key press is a discrete move and the auto-repeat is
                        // ignored upstream, so adjust directly — no held-dir
                        // gate that could get stuck and block the next option.
                        return SettingsControllerNav.adjust(dx) || SettingsControllerNav.hasItems()
                    }
                    return SettingsControllerNav.hasItems()
                }
            }
            is State.ExitConfirm -> {
                if (dy != 0) modalSelection.value = (modalSelection.value + dy).coerceIn(0, 2)
                return true
            }
            is State.ResetConfirm, is State.HardcoreEnableConfirm -> {
                val delta = if (dy != 0) dy else dx
                if (delta != 0) modalSelection.value = (modalSelection.value + delta).coerceIn(0, 1)
                return true
            }
            is State.AchievementsLogin -> {
                // Manual model (touch mode blocks Compose focus). Any direction
                // steps the flat control list (login fields / cancel / sign in).
                val delta = if (dy != 0) dy else dx
                if (delta != 0) return SettingsControllerNav.move(delta)
                return SettingsControllerNav.hasItems()
            }
            is State.Achievements -> {
                val delta = if (dy != 0) dy else dx
                if (delta == 0) return true
                // Not signed in → the sign-in card is a nav item. Signed in →
                // no nav items, so scroll the (lazy) achievement list instead.
                if (SettingsControllerNav.hasItems()) return SettingsControllerNav.move(delta)
                achievementsScroll.value += delta
                return true
            }
            else -> return false
        }
        return false
    }

    fun handleControllerConfirm(): Boolean {
        if (!WindowImpl.overlayVisible.value) return false
        when (state.value) {
            is State.Root -> {
                if (currentTab.value == Tab.PlayingNow && !settingsOnly.value) {
                    activatePlaySelection(playSelection.value)
                    return true
                }
                if (settingsTabActive())
                    return SettingsControllerNav.confirm()
            }
            is State.ExitConfirm -> {
                when (modalSelection.value.coerceIn(0, 2)) {
                    0 -> exitSaveStateAndExit()
                    1 -> exitWithoutSaving()
                    else -> enterState(State.Root)
                }
                return true
            }
            is State.ResetConfirm -> {
                if (modalSelection.value.coerceIn(0, 1) == 0) resetSystem()
                else enterState(State.Root)
                return true
            }
            is State.HardcoreSaveStateBlocked -> {
                enterState(State.Root)
                return true
            }
            is State.HardcoreEnableConfirm -> {
                if (modalSelection.value.coerceIn(0, 1) == 0) {
                    enterState(State.Root)
                } else {
                    enableHardcoreMode()
                }
                return true
            }
            is State.Achievements, is State.AchievementsLogin ->
                return SettingsControllerNav.confirm()
            else -> return false
        }
        return false
    }

    fun handleControllerBack(): Boolean {
        if (!WindowImpl.overlayVisible.value) return false
        when (state.value) {
            is State.Root -> closeAndResume()
            else -> enterState(State.Root)
        }
        return true
    }

    private fun settingsTabActive(): Boolean =
        state.value is State.Root &&
            (settingsOnly.value || currentTab.value != Tab.PlayingNow)

    fun handleControllerHorizontalRelease() {
        settingsAdjustHeldDir = 0
        SettingsControllerNav.resetAdjustmentGate()
    }

    private fun resetSettingsAdjustGate() {
        settingsAdjustHeldDir = 0
        SettingsControllerNav.resetAdjustmentGate()
    }

    fun handleControllerTab(delta: Int): Boolean {
        if (!WindowImpl.overlayVisible.value || state.value !is State.Root || delta == 0) return false
        cycleTab(delta)
        return true
    }

    fun handleControllerScroll(velocity: Float): Boolean {
        if (!WindowImpl.overlayVisible.value || !settingsTabActive()) {
            SettingsControllerNav.setScrollVelocity(0f)
            return false
        }
        return SettingsControllerNav.setScrollVelocity(velocity)
    }

    private fun enterState(next: State) {
        state.value = next
        modalSelection.value = 0
    }

    private fun cycleTab(delta: Int) {
        val tabs = if (settingsOnly.value) {
            listOf(Tab.Performance, Tab.Renderer, Tab.Fixes, Tab.Audio, Tab.Patches, Tab.Network, Tab.Overlay, Tab.Pad, Tab.Hotkeys, Tab.Recompiler)
        } else {
            Tab.values().toList()
        }
        val index = tabs.indexOf(currentTab.value).takeIf { it >= 0 } ?: 0
        currentTab.value = tabs[(index + delta).floorMod(tabs.size)]
        SettingsControllerNav.clearSelection()
        resetSettingsAdjustGate()
    }

    private fun Int.floorMod(modulus: Int): Int =
        ((this % modulus) + modulus) % modulus

    private fun activatePlaySelection(index: Int) {
        when (index.coerceIn(0, 11)) {
            0 -> closeAndResume()
            1 -> openSaveStates()
            2 -> openLoadStates()
            3 -> swapDisc()
            4 -> bootDisc()
            5 -> openLibrary()
            6 -> cycleRendererMode()
            7 -> toggleFrameLimit()
            8 -> editTouchLayout()
            9 -> toggleOsd()
            10 -> enterState(State.ResetConfirm)
            11 -> closeGame()
        }
    }

    private fun openSaveStates() {
        enterState(if (hardcoreOn.value) State.HardcoreSaveStateBlocked else State.SaveStateSlots)
    }

    private fun openLoadStates() {
        enterState(if (hardcoreOn.value) State.HardcoreSaveStateBlocked else State.LoadStateSlots)
    }

    private fun swapDisc() {
        val intent = Intent(Intent.ACTION_GET_CONTENT)
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false)
        intent.setType("*/*")
        Main.instance?.swapDiscAction?.launch(intent)
        closeKeepingState()
    }

    private fun bootDisc() {
        val intent = Intent(Intent.ACTION_GET_CONTENT)
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false)
        intent.setType("*/*")
        Main.instance?.bootDiscAction?.launch(intent)
        closeKeepingState()
    }

    private fun openLibrary() {
        if (Main.eState.value == EmuState.PAUSED) Main.resume()
        WindowImpl.showLibrary.value = true
        closeKeepingState()
    }

    private fun cycleRendererMode() {
        val backendHw: () -> Unit = {
            if (Main.renderer.value == "vulkan")
                Main.renderVulkan() else Main.renderOpenGL()
        }
        rendererMode.value = when (rendererMode.value) {
            RendererMode.Auto -> {
                backendHw()
                RendererMode.Hardware
            }
            RendererMode.Hardware -> {
                Main.renderSoftware()
                RendererMode.Software
            }
            RendererMode.Software -> {
                backendHw()
                RendererMode.Auto
            }
        }
    }

    private fun toggleFrameLimit() {
        saveSettings(settingsState.value.copy(frameLimitEnable = !frameLimitOn.value))
    }

    private fun editTouchLayout() {
        com.armsx2.ui.touch.TouchControls.ensureLoaded()
        com.armsx2.ui.touch.TouchControls.editMode.value = true
        closeKeepingState()
    }

    private fun toggleOsd() {
        val enabled = !osdShown.value
        saveSettings(withAllOsdElements(settingsState.value, enabled))
        NativeApp.osdShowAll(enabled)
    }

    private fun closeGame() {
        if (Main.prefs.getBoolean("autoSaveOnExit", false)) {
            Main.stop(saveAutosave = true)
            closeKeepingState()
        } else {
            enterState(State.ExitConfirm)
        }
    }

    private fun exitSaveStateAndExit() {
        Main.stop(saveAutosave = true)
        closeKeepingState()
    }

    private fun exitWithoutSaving() {
        Main.stop()
        closeKeepingState()
    }

    private fun resetSystem() {
        Main.restart()
        closeKeepingState()
    }

    private fun enableHardcoreMode() {
        NativeApp.setHardcoreMode(true)
        hardcoreOn.value = true
        enterState(State.Root)
    }

    /** Open the overlay. Pauses the VM. Safe to call when already open. */
    fun open() {
        if (WindowImpl.overlayVisible.value) return
        settingsOnly.value = false
        previewGame.value = null
        pausedByOverlay = (Main.eState.value == EmuState.RUNNING)
        // ALWAYS pause while the overlay is up — even if eState already
        // says PAUSED. The Kotlin flag can run ahead of the actual VM,
        // and a stale PAUSED left the VM running underneath while settings
        // changes (upscale → live GS reconfig) applied mid-frame.
        // Main.pauseForOverlay() directly signals the nonblocking native pause
        // path so the EE breaks out of Execute() as soon as the overlay opens.
        // Mid-frame-settings safety doesn't depend on this call having
        // completed: commitSettings and the savestate/GS-reconfig JNI entry
        // points enforce VM quiescence themselves (ScopedVMPause).
        if (Main.eState.value != EmuState.STOPPED) Main.pauseForOverlay()
        state.value = State.Root
        playSelection.value = 0
        modalSelection.value = 0
        currentTab.value = Tab.PlayingNow
        SettingsControllerNav.clearSelection()
        resetSettingsAdjustGate()
        // Resolve the current game's serial first; scope and settings
        // hydration both depend on it. Mirrors GameInfoHeader's chain —
        // cached GameInfo (carries through file picker paths that lack a
        // native serial), then NativeApp.getPauseGameSerial. Empty
        // string from native means "no disc loaded" → fall back to
        // global scope/settings.
        val serial = Main.currentGame.value?.serial
            ?: runCatching { NativeApp.getPauseGameSerial() }.getOrNull()
                ?.takeIf { it.isNotEmpty() }
        currentSerial.value = serial
        settingsScope.value =
            if (serial != null) SettingsScope.Game else SettingsScope.Global
        // Re-hydrate the live Settings state from disk. When a game is
        // loaded we want to see the EFFECTIVE settings (global ∘ overrides)
        // so the user edits the merged value; otherwise just the global.
        settingsState.value = ConfigStore.resolveForGame(serial)
        syncQuickTogglesFromSettings(settingsState.value)
        // Sync pill state from native — covers emucore-driven swaps that
        // happened while the overlay was closed. Auto is sticky against
        // the sync (the user picked "let it decide", so we keep showing
        // Auto even though GS resolved it to HW underneath).
        if (rendererMode.value != RendererMode.Auto) {
            runCatching {
                rendererMode.value = if (NativeApp.isHardwareRenderer())
                    RendererMode.Hardware else RendererMode.Software
            }
        }
        WindowImpl.overlayVisible.value = true
    }

    /** Open the same settings tabs from the stopped/library UI. */
    fun openGlobalSettings() {
        if (WindowImpl.overlayVisible.value) return
        settingsOnly.value = true
        previewGame.value = null
        pausedByOverlay = false
        state.value = State.Root
        playSelection.value = 0
        modalSelection.value = 0
        currentTab.value = Tab.Performance
        SettingsControllerNav.clearSelection()
        resetSettingsAdjustGate()
        currentSerial.value = null
        settingsScope.value = SettingsScope.Global
        settingsState.value = ConfigStore.loadGlobal()
        syncQuickTogglesFromSettings(settingsState.value)
        WindowImpl.overlayVisible.value = true
    }

    /** Open per-game settings from a long-pressed library card before launch. */
    fun openGameSettings(game: GameInfo) {
        if (WindowImpl.overlayVisible.value) return
        settingsOnly.value = true
        previewGame.value = game
        pausedByOverlay = false
        state.value = State.Root
        playSelection.value = 0
        modalSelection.value = 0
        currentTab.value = Tab.Performance
        SettingsControllerNav.clearSelection()
        resetSettingsAdjustGate()
        currentSerial.value = game.serial?.takeIf { it.isNotEmpty() }
        settingsScope.value =
            if (currentSerial.value != null) SettingsScope.Game else SettingsScope.Global
        settingsState.value = ConfigStore.resolveForGame(currentSerial.value)
        syncQuickTogglesFromSettings(settingsState.value)
        WindowImpl.overlayVisible.value = true
    }

    private fun closeAndResume() {
        WindowImpl.overlayVisible.value = false
        state.value = State.Root
        playSelection.value = 0
        modalSelection.value = 0
        settingsOnly.value = false
        previewGame.value = null
        SettingsControllerNav.clearSelection()
        resetSettingsAdjustGate()
        // Always resume if the VM is paused — close-paths that should
        // preserve a paused VM (Swap Disc picker, library, edit mode)
        // go through closeKeepingState instead. The earlier
        // pausedByOverlay gate stale-locked the VM after the user
        // bounced through closeKeepingState (e.g. entered edit mode then
        // re-opened pause): pausedByOverlay was cleared, so the next
        // Resume tap did nothing.
        if (pausedByOverlay || Main.eState.value == EmuState.PAUSED) Main.resume()
        pausedByOverlay = false
    }

    private fun closeKeepingState() {
        WindowImpl.overlayVisible.value = false
        state.value = State.Root
        playSelection.value = 0
        modalSelection.value = 0
        settingsOnly.value = false
        previewGame.value = null
        SettingsControllerNav.clearSelection()
        resetSettingsAdjustGate()
        pausedByOverlay = false
    }

    @Composable
    fun Render() {
        // Hardware back: descends submenu first, then dismisses (resumes).
        BackHandler(enabled = WindowImpl.overlayVisible.value) {
            when (state.value) {
                is State.Root -> closeAndResume()
                else -> state.value = State.Root
            }
        }

        // Backdrop swallows taps so the running surface beneath doesn't
        // pick up the press as a button event. Tap-outside on Root acts
        // as Resume Game. The dim layer fills the whole screen (cutout
        // included) so the partial-alpha aesthetic is uniform; only the
        // INNER content box gets displayCutoutPadding so headers / menu
        // rows aren't obscured by punch-hole or notch hardware.
        val backdropInteraction = remember { MutableInteractionSource() }
        // Controller navigation: pull Compose focus into the overlay when it
        // opens so the D-pad can traverse its buttons (focusGroup makes the
        // first focusable child take focus). Gamepad A is translated to
        // DPAD_CENTER in Main.dispatchKeyEvent so clickable items activate;
        // B / Back drills up via the BackHandler above. Main re-grabs the game
        // surface's focus when the overlay closes so controller input returns
        // to the game.
        val navFocus = remember { FocusRequester() }
        val navFocusManager = androidx.compose.ui.platform.LocalFocusManager.current
        LaunchedEffect(Unit) {
            // Let the game SurfaceView relinquish focus first (it's gated
            // non-focusable while the overlay is up), then forcibly pull focus
            // off it and into the overlay's focus group so the D-pad can
            // traverse the buttons and A (-> DPAD_CENTER) can activate them.
            runCatching { navFocusManager.clearFocus(force = true) }
            // Retry until the focus group is placed and accepts focus — the game
            // surface / library can briefly hold focus as the overlay opens, so a
            // single request may land before the container is ready.
            repeat(15) {
                if (runCatching { navFocus.requestFocus() }.isSuccess) return@LaunchedEffect
                kotlinx.coroutines.delay(20)
            }
        }
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.68f))
                // Backdrop taps are swallowed (no close) so an accidental tap on
                // empty space can't drop you out of the overlay. Exit is the ✕
                // button (touch) or B (controller).
                .clickable(
                    indication = null,
                    interactionSource = backdropInteraction,
                    onClick = { }
                ),
        ) {
            // Inner safe-area container — content laid out against this
            // box's edges automatically gets cutout-aware insets. Tap on
            // the dim band outside still falls through to the backdrop's
            // close-on-tap handler because this inner Box is non-clickable.
            Box(Modifier.fillMaxSize().displayCutoutPadding().focusRequester(navFocus).focusable()) {
            // Settings tabs use (nearly) the full width so long rows / the tab
            // strip aren't cut off. The Play tab stays compact — its 4-column
            // action grid is laid out for the narrow column (full width spread it
            // out and broke the layout).
            val wideContent = state.value is State.Root &&
                (settingsOnly.value || currentTab.value != Tab.PlayingNow)
            // Headless poll keeps hardcore / renderer / rich-presence state in
            // sync even though the inline achievements panel is gone.
            AchievementsSync()
            Box(
                Modifier
                    .align(Alignment.TopStart)
                    .fillMaxHeight()
                    .then(if (wideContent) Modifier.fillMaxWidth(0.94f) else Modifier.width(560.dp))
                    .background(
                        Brush.horizontalGradient(
                            listOf(
                                Color.Black.copy(alpha = 0.54f),
                                Color.Black.copy(alpha = 0.28f),
                                Color.Transparent,
                            )
                        )
                    )
            )
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .fillMaxHeight()
                    .width(520.dp)
                    .background(
                        Brush.horizontalGradient(
                            listOf(
                                Color.Transparent,
                                Color.Black.copy(alpha = 0.22f),
                                Color.Black.copy(alpha = 0.50f),
                            )
                        )
                    )
            )
            // Top-left: game info, then (on Root) the tab strip and the
            // active tab's body stacked directly beneath it. Keeping the
            // strip and its content in the same column means tabs always
            // own their entries — no detached "strip up here, rows down
            // there" split. Submenu / confirm states fall through to the
            // bottom-left panel below.
            Column(
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .fillMaxHeight()
                    .padding(20.dp)
                    .then(if (wideContent) Modifier.fillMaxWidth(0.90f) else Modifier.width(520.dp)),
            ) {
                GameInfoHeader()
                if (state.value is State.Root) {
                    Spacer(Modifier.height(12.dp))
                    TabStrip()
                    Text(
                        "Press L and R to navigate between sections, on controller\n" +
                            "For touch, swipe to see all sections",
                        color = Color.White.copy(alpha = 0.45f),
                        fontSize = 10.sp,
                        lineHeight = 13.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.padding(top = 3.dp),
                    )
                    // The scope toggle only matters for settings tabs.
                    // PlayingNow's actions (Resume / Save State / etc.)
                    // are session controls, not persisted settings — no
                    // notion of "global vs game" applies.
                    if (currentTab.value != Tab.PlayingNow && !settingsOnly.value) {
                        Spacer(Modifier.height(4.dp))
                        ScopeToggle()
                    }
                    Spacer(Modifier.height(6.dp))
                    // weight(1f) gives RootTabs the remaining vertical
                    // space, bounding Performance/Renderer's verticalScroll
                    // so it actually scrolls instead of expanding off-screen.
                    Box(modifier = Modifier.weight(1f)) {
                        RootTabs()
                        if (settingsTabActive()) {
                            SettingsScrollHint(
                                modifier = Modifier
                                    .align(Alignment.BottomEnd)
                                    .padding(end = 4.dp, bottom = 4.dp),
                            )
                        }
                    }
                }
            }

            Column(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(20.dp),
                horizontalAlignment = Alignment.End,
            ) {
                // Dedicated close button so touch users don't have to tap the
                // dim backdrop (easy to hit by accident) to leave the overlay.
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .clip(RoundedCornerShape(20.dp))
                        .background(Color.White.copy(alpha = 0.12f))
                        .border(1.dp, Color.White.copy(alpha = 0.22f), RoundedCornerShape(20.dp))
                        .clickable { closeAndResume() },
                    contentAlignment = Alignment.Center,
                ) {
                    Text("✕", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold)
                }
                Spacer(Modifier.height(4.dp))
                Text(
                    "Press B to exit on controller",
                    color = Color.White.copy(alpha = 0.45f),
                    fontSize = 9.sp,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.End,
                )
                Text(
                    "Press Y to open RetroAchievements on controller",
                    color = Color.White.copy(alpha = 0.45f),
                    fontSize = 9.sp,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.End,
                )
            }

            // Brand sits in the top-right-of-centre band: right of long game
            // titles (which start top-left) but left of the close button, so it
            // stops clashing with both. Anchored to the end edge for a stable
            // gap from the ✕ across screen sizes.
            BrandHeader(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(top = 20.dp, end = 135.dp)
            )

            // (The inline bottom-right achievements panel was removed — it's now
            // the header trophy button → dedicated achievements view, so the Play
            // tab can use the full width like every other tab.)

            // Bottom-left: confirm dialogs and slot pickers only. Root
            // content lives in the top-left column above with its tab
            // strip; this panel just hosts modal-ish flows that should
            // sit at the bottom of the screen (Exit / Reset confirms,
            // save-state slot grid).
            if (state.value !is State.Root) {
                // The login form needs more vertical room than the other
                // bottom-left states (it has 2 text fields + disclaimer +
                // buttons), and on landscape phones with the keyboard up
                // 75% of the screen isn't quite enough — content clips and
                // verticalScroll only barely engages. Give the login state
                // a taller box.
                val maxFrac = when {
                    state.value is State.AchievementsLogin -> 0.92f
                    state.value is State.Achievements -> 0.88f
                    else -> 0.75f
                }
                // Slot pickers (Save/Load) span the full screen width so
                // the 2-row horizontal grid actually reaches across. Other
                // states (confirms, login) stay at the 360dp left column.
                val isSlotPicker = state.value is State.SaveStateSlots
                    || state.value is State.LoadStateSlots
                Box(
                    Modifier
                        .align(Alignment.BottomStart)
                        .fillMaxHeight(maxFrac)
                        .let { if (isSlotPicker) it.fillMaxWidth() else it.width(520.dp) }
                        .padding(start = 20.dp, end = if (isSlotPicker) 20.dp else 0.dp, bottom = 20.dp, top = 20.dp),
                    contentAlignment = Alignment.BottomStart,
                ) {
                    when (state.value) {
                        is State.SaveStateSlots -> SaveStatePicker.Render(
                            mode = SaveStatePicker.Mode.Save,
                            onDone = { closeAndResume() },
                            onBack = { state.value = State.Root },
                        )
                        is State.LoadStateSlots -> SaveStatePicker.Render(
                            mode = SaveStatePicker.Mode.Load,
                            onDone = { closeAndResume() },
                            onBack = { state.value = State.Root },
                        )
                        is State.ExitConfirm -> ExitConfirm()
                        is State.ResetConfirm -> ResetConfirm()
                        is State.AchievementsLogin -> AchievementsLoginPanel(
                            onClose = {
                                SettingsControllerNav.clearSelection()
                                state.value = State.Root
                            },
                        )
                        is State.Achievements -> AchievementsPanel(
                            modifier = Modifier.fillMaxWidth(),
                            onSignInClick = {
                                SettingsControllerNav.clearSelection()
                                state.value = State.AchievementsLogin
                            },
                            onHardcoreToggle = {
                                if (hardcoreOn.value) {
                                    NativeApp.setHardcoreMode(false)
                                    hardcoreOn.value = false
                                } else {
                                    state.value = State.HardcoreEnableConfirm
                                }
                            },
                        )
                        is State.HardcoreSaveStateBlocked -> HardcoreBlockedBubble()
                        is State.HardcoreEnableConfirm -> Unit // rendered fullscreen below
                        is State.Root -> Unit
                    }
                }
            }

            // PS2-BIOS-style fullscreen confirm for enabling hardcore mode.
            // Painted on top of everything else, so it eats taps and the
            // user must explicitly confirm or cancel before returning to
            // the menu. Enabling hardcore resets the running game (per
            // upstream Achievements::ResetHardcoreMode) so we want the
            // user to know exactly what's about to happen.
            if (state.value is State.HardcoreEnableConfirm) {
                HardcoreEnableConfirmFullscreen()
            }
            } // displayCutoutPadding inner box
        }
    }

    @Composable
    private fun GameInfoHeader(modifier: Modifier = Modifier) {
        // Prefer the cached GameInfo from Main.currentGame — it has the
        // pre-resolved compat (computed at library scan time), the cover
        // URL, and the container extension. Fallback to NativeApp.getPause*
        // for paths that lack a GameInfo (Swap/Boot Disc file picker, BIOS).
        val globalSettingsView =
            settingsOnly.value && currentSerial.value == null && previewGame.value == null
        val cached = if (globalSettingsView) null else (previewGame.value ?: Main.currentGame.value)
        val cachedTitle = cached?.title?.takeIf { it.isNotEmpty() }
        val nativeTitle = if (!globalSettingsView)
            NativeApp.getPauseGameTitle()?.takeIf { it.isNotEmpty() }
        else null
        val title = cachedTitle ?: nativeTitle ?: if (globalSettingsView) "General Settings" else "PS2 BIOS"
        val serial = cached?.serial
            ?: if (!globalSettingsView) NativeApp.getPauseGameSerial()?.takeIf { it.isNotEmpty() } else null
        val compatStars = when {
            cached != null -> cached.compatibility
            serial != null -> (NativeApp.getCompatibilityForSerial(serial) - 1).coerceIn(0, 5)
            else -> 0
        }
        val coverUrl = cached?.coverUrl
        val extension = cached?.extension?.takeIf { it.isNotEmpty() }

        Row(modifier = modifier, verticalAlignment = Alignment.Top) {
            // Cover thumbnail to the left, matching the library card's
            // 0.7 aspect ratio. 72dp tall fits next to a 22sp title +
            // 13sp serial line without crowding.
            Box(
                Modifier
                    .height(72.dp)
                    .aspectRatio(0.7f)
                    .clip(RoundedCornerShape(4.dp))
                    .background(Color(0xFF1B1A1A).copy(alpha = 0.5f)),
                contentAlignment = Alignment.Center,
            ) {
                val context = LocalContext.current
                if (coverUrl != null) {
                    // PS2 boxart matches the 0.7 aspect cell — Crop fills.
                    // PS1 jewel-case covers are squarer; Fit + Center
                    // letterboxes them inside the same cell so the full
                    // art is visible without cropping.
                    val scale = when (cached?.platform) {
                        com.armsx2.GamePlatform.PS1 -> ContentScale.Fit
                        else -> ContentScale.Crop
                    }
                    SubcomposeAsyncImage(
                        model = ImageRequest.Builder(context).data(coverUrl).crossfade(true).build(),
                        contentDescription = "$title cover",
                        contentScale = scale,
                        alignment = Alignment.Center,
                        modifier = Modifier.fillMaxSize(),
                        loading = { /* dim background shows through */ },
                        error = { Text("📀", color = Color(0xFF3F3F3F), fontSize = 28.sp) },
                    )
                } else {
                    Text("📀", color = Color(0xFF3F3F3F), fontSize = 28.sp)
                }
            }
            Spacer(Modifier.width(12.dp))

            // weight(fill = false) keeps the title from squeezing the trailing
            // RetroAchievements button off-screen on the narrow Play tab; the
            // widthIn cap keeps the button at a CONSISTENT position so it doesn't
            // drift toward the brand on the wider settings tabs.
            Column(modifier = Modifier.weight(1f, fill = false).widthIn(max = 240.dp)) {
                Text(
                    title,
                    color = Color.White,
                    fontSize = 22.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(4.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        if (globalSettingsView) "Global" else serial ?: "No disc",
                        color = if (serial != null || globalSettingsView) Color(0xFFAACCFF) else Color(0xFF808080),
                        fontSize = 13.sp,
                    )
                    if (serial != null) {
                        Spacer(Modifier.width(10.dp))
                        CompatStars(compatStars)
                    }
                    if (extension != null) {
                        Spacer(Modifier.width(8.dp))
                        ExtensionBadge(extension)
                    }
                    if (hardcoreOn.value) {
                        Spacer(Modifier.width(6.dp))
                        HardcoreBadge()
                    }
                }
                // Live RetroAchievements rich-presence subtitle. Mirrors
                // what RA's website shows under your active game; we surface
                // it locally so the user can see what the server is being
                // told. AchievementsPanel's 4s poll owns the writes.
                val rp = richPresence.value
                if (rp.isNotEmpty()) {
                    Spacer(Modifier.height(3.dp))
                    MarqueeRichPresence(rp)
                }
            }
            // RetroAchievements entry — sits just after the title (or next to
            // "General Settings" in the global view). Opens the dedicated
            // achievements view; controller users can also use the Open
            // Achievements hotkey / B to exit it.
            Spacer(Modifier.width(16.dp))
            Box(
                Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color(0xFFB7892B).copy(alpha = 0.20f))
                    .border(1.dp, Color(0xFFE0A93A).copy(alpha = 0.55f), RoundedCornerShape(8.dp))
                    .clickable { state.value = State.Achievements }
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    "🏆 RetroAchievements",
                    color = Color(0xFFFFD98A),
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                )
            }
        }
    }

    /** One-line rich-presence subtitle. Renders directly when the string
     *  fits within the available width; runs a back-and-forth ping-pong
     *  marquee when it overflows. Holds at each end so the user has time
     *  to read both the start and the tail before the slide flips
     *  direction. Scrolls just far enough to align the right edge of the
     *  text with the right edge of the container — never past, never
     *  off-screen. */
    @Composable
    private fun MarqueeRichPresence(text: String) {
        // Measure the text's INTRINSIC width via TextMeasurer. onSizeChanged
        // on the rendered Text reports the constrained width (== container)
        // when the parent caps it, which makes overflow detection a no-op.
        val style = remember {
            TextStyle(color = Color(0xFFBBBBBB), fontSize = 11.sp)
        }
        val measurer = rememberTextMeasurer()
        val intrinsicTextWidth = remember(text, style) {
            measurer.measure(text = text, style = style, softWrap = false, maxLines = 1).size.width
        }
        var containerWidth by remember(text) { mutableIntStateOf(0) }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .clipToBounds()
                .onSizeChanged { containerWidth = it.width },
        ) {
            val overflowPx = intrinsicTextWidth - containerWidth
            if (containerWidth <= 0 || overflowPx <= 0) {
                // Fits, or container not yet measured — plain Text. Compose
                // settles the layout in the next frame and recomposes us
                // with a real containerWidth, at which point we either stay
                // here (fits) or drop into the marquee branch below.
                Text(
                    text = text,
                    style = style,
                    maxLines = 1,
                    softWrap = false,
                    overflow = TextOverflow.Visible,
                )
            } else {
                // Marquee branch. Composables here only enter the slot
                // table when overflowPx is real, so the keyframes can't
                // capture a "containerWidth = 0" max-offset and stick.
                MarqueeText(text = text, style = style, overflowPx = overflowPx)
            }
        }
    }

    @Composable
    private fun MarqueeText(text: String, style: TextStyle, overflowPx: Int) {
        // ~40 px/sec scroll. Brisk enough to feel responsive on long
        // rich-presence strings without overrunning the reading pace.
        // 1.5s hold at each end so eye has time to land before the slide
        // resumes.
        val holdMs = 1500
        val scrollMs = (overflowPx * 1000 / 40).coerceAtLeast(1200)
        val totalMs = holdMs * 2 + scrollMs * 2
        val maxOffset = overflowPx.toFloat()

        val transition = rememberInfiniteTransition(label = "rp-marquee")
        val offsetPx by transition.animateFloat(
            initialValue = 0f,
            targetValue = 0f,
            animationSpec = infiniteRepeatable(
                animation = keyframes {
                    durationMillis = totalMs
                    0f at 0
                    0f at holdMs                                using LinearEasing
                    -maxOffset at holdMs + scrollMs              using LinearEasing
                    -maxOffset at holdMs + scrollMs + holdMs    using LinearEasing
                    0f at totalMs                                using LinearEasing
                },
                repeatMode = RepeatMode.Restart,
            ),
            label = "rp-offset",
        )

        Text(
            text = text,
            style = style,
            maxLines = 1,
            softWrap = false,
            overflow = TextOverflow.Visible,
            modifier = Modifier.offset { IntOffset(offsetPx.toInt(), 0) },
        )
    }

    @Composable
    private fun CompatStars(filled: Int) {
        Row {
            repeat(5) { i ->
                val on = i < filled
                Text(
                    if (on) "★" else "☆",
                    color = if (on) Color(0xFFFFD33A) else Color(0xFF555555),
                    fontSize = 13.sp,
                )
            }
        }
    }

    /** PS2-blue rounded chip showing the container format (ISO / CHD /
     *  BIN / etc.). Mirrors GamesList's ExtensionBadge for parity with
     *  the library card chrome. */
    @Composable
    private fun ExtensionBadge(ext: String) {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Colors.pasx2_blue)
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                ext,
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    /** Small red badge displayed next to the extension badge when
     *  RetroAchievements Hardcore mode is active. Same shape as
     *  ExtensionBadge so they line up visually. */
    @Composable
    private fun HardcoreBadge() {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Color(0xFFB22222))
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                "HC",
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    @Composable
    private fun BrandHeader(modifier: Modifier = Modifier) {
        // Brand text column (ARMSX2 + version stacked, version centered
        // under the ARMSX2 text only) sits beside the logo. The version
        // line is horizontally centered against the ARMSX2 word's
        // intrinsic width — not against the whole row including the
        // icon — so it reads as a label belonging to the wordmark.
        Row(
            modifier = modifier,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("ARMSX2", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold)
                Text(versionString, color = Color(0xFF888888), fontSize = 11.sp)
            }
            Spacer(Modifier.width(8.dp))
            Image(
                painter = painterResource(id = R.drawable.savetowerforeground),
                contentDescription = null,
                modifier = Modifier.size(32.dp),
            )
        }
    }

    @Composable
    private fun SettingsScrollHint(modifier: Modifier = Modifier) {
        Text(
            "Scroll up/down and left/right to navigate",
            color = Color.White.copy(alpha = 0.58f),
            fontSize = 10.sp,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            modifier = modifier
                .background(Color.Black.copy(alpha = 0.28f))
                .padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }

    /** Active tab body. Rendered in the top-left column directly under
     *  TabStrip, so the strip and its entries stay visually attached.
     *  Width and horizontal padding come from the parent column. */
    @Composable
    private fun RootTabs() {
        val tab = currentTab.value
        if (tab == Tab.PlayingNow && !settingsOnly.value) {
            PlayingNowTab()
            return
        }

        SettingsControllerNav.begin(tab.name)
        when (tab) {
            Tab.PlayingNow -> PlayingNowTab()
            Tab.Performance -> PerformanceTab(settingsState)
            Tab.Renderer -> RendererTab(settingsState)
            Tab.Fixes -> FixesTab(settingsState)
            Tab.Audio -> AudioTab(settingsState)
            Tab.Patches -> PatchesTab(settingsState)
            Tab.Network -> NetworkTab(settingsState)
            Tab.Overlay -> OverlayTab(settingsState)
            Tab.Pad -> PadTab(settingsState)
            Tab.Hotkeys -> HotkeysTab(settingsState)
            Tab.Recompiler -> RecompilerTab(settingsState)
        }
        SettingsControllerNav.end()
    }

    /** Horizontal tab chip strip. Active tab gets PS2-blue underline +
     *  brighter text; inactive tabs are dim. Tappable across the whole
     *  chip area. */
    @Composable
    private fun TabStrip() {
        val scroll = rememberScrollState()
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(scroll),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            val tabs = if (settingsOnly.value) {
                listOf(Tab.Performance, Tab.Renderer, Tab.Fixes, Tab.Audio, Tab.Patches, Tab.Network, Tab.Overlay, Tab.Pad, Tab.Hotkeys, Tab.Recompiler)
            } else {
                Tab.values().toList()
            }
            tabs.forEach { tab ->
                val active = currentTab.value == tab
                val chipWidth = when (tab) {
                    Tab.Patches, Tab.Network, Tab.Overlay, Tab.Hotkeys -> 72.dp
                    Tab.Pad -> 52.dp
                    Tab.PlayingNow -> 52.dp
                    else -> 64.dp
                }
                Column(
                    modifier = Modifier
                        .width(chipWidth)
                        .clickable {
                            currentTab.value = tab
                            SettingsControllerNav.clearSelection()
                            resetSettingsAdjustGate()
                        }
                        .padding(vertical = 4.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        tab.label,
                        color = if (active) Color.White else Color(0xFF888888),
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    Spacer(Modifier.height(3.dp))
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(2.dp)
                            .background(
                                if (active) Colors.pasx2_blue else Color.Transparent
                            ),
                    )
                }
            }
        }
    }

    /** Tiny Global / Game pill that decides where settings tab edits
     *  land. Game side is disabled (and the toggle locked to Global)
     *  when there's no current serial — BIOS boots have nowhere to write
     *  per-game overrides. Active half gets the PS2-blue accent so the
     *  user can see at a glance what scope is "live". */
    @Composable
    private fun ScopeToggle() {
        val serial = currentSerial.value
        val gameEnabled = serial != null
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(4.dp))
                .background(Color(0xFF1A1A1A))
                .border(1.dp, Color.White.copy(alpha = 0.08f), RoundedCornerShape(4.dp))
                .height(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ScopeHalf(
                label = "Global",
                active = settingsScope.value == SettingsScope.Global,
                enabled = true,
                modifier = Modifier.weight(1f),
            ) { settingsScope.value = SettingsScope.Global }
            ScopeHalf(
                label = if (serial != null) "Game · $serial" else "Game",
                active = settingsScope.value == SettingsScope.Game,
                enabled = gameEnabled,
                modifier = Modifier.weight(1f),
            ) {
                if (gameEnabled) settingsScope.value = SettingsScope.Game
            }
        }
    }

    @Composable
    private fun ScopeHalf(
        label: String,
        active: Boolean,
        enabled: Boolean,
        modifier: Modifier = Modifier,
        onClick: () -> Unit,
    ) {
        val bg = when {
            !enabled -> Color.Transparent
            active   -> Colors.pasx2_blue.copy(alpha = 0.30f)
            else     -> Color.Transparent
        }
        val fg = when {
            !enabled -> Color(0xFF555555)
            active   -> Color.White
            else     -> Color(0xFF888888)
        }
        Box(
            modifier = modifier
                .fillMaxHeight()
                .background(bg)
                .clickable(enabled = enabled, onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                label,
                color = fg,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }

    /** Playing Now — pause-menu actions laid out as a 4-wide bubble grid.
     *  Three rows of four cells keep widths constant and fit without
     *  scrolling on phone landscape. Primary (Resume) and Danger (Close)
     *  accents land the eye on the most common entry and the destructive
     *  exit. Toggleable bubbles (Renderer, Frame Limit) carry their
     *  current value on a second line. */
    @Composable
    private fun PlayingNowTab() {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // Row 1: primary + save/load + swap disc.
	            BubbleRow {
	                BubbleButton(
	                    "Resume",
	                    LineAwesomeIcons.PlaySolid,
	                    accent = BubbleAccent.Primary,
	                    selected = playSelection.value == 0,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(0) }
	                BubbleButton(
	                    "Save State",
	                    LineAwesomeIcons.SaveSolid,
	                    dim = hardcoreOn.value,
	                    selected = playSelection.value == 1,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(1) }
	                BubbleButton(
	                    "Load State",
	                    LineAwesomeIcons.FolderOpenSolid,
	                    dim = hardcoreOn.value,
	                    selected = playSelection.value == 2,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(2) }
	                BubbleButton(
	                    "Swap Disc",
	                    LineAwesomeIcons.CompactDiscSolid,
	                    selected = playSelection.value == 3,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(3) }
	            }
            // Row 2: boot disc + library + renderer + frame limit.
            BubbleRow {
	                BubbleButton(
	                    "Boot Disc",
	                    LineAwesomeIcons.CompactDiscSolid,
	                    selected = playSelection.value == 4,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(4) }
	                BubbleButton(
	                    "Library",
	                    LineAwesomeIcons.ThLargeSolid,
	                    selected = playSelection.value == 5,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(5) }
	                BubbleButton(
	                    "Renderer",
	                    LineAwesomeIcons.CubeSolid,
                    stateLine = when (rendererMode.value) {
                        RendererMode.Auto -> "Auto"
                        RendererMode.Hardware -> "Hardware"
                        RendererMode.Software -> "Software"
	                    },
	                    accent = BubbleAccent.Active,
	                    selected = playSelection.value == 6,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(6) }
	                BubbleButton(
	                    "Frame Limit",
	                    LineAwesomeIcons.TachometerAltSolid,
                    stateLine = if (frameLimitOn.value) "On" else "Off",
	                    accent = if (frameLimitOn.value)
	                        BubbleAccent.Active else BubbleAccent.Normal,
	                    selected = playSelection.value == 7,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(7) }
	            }
            // Row 3: touch layout, OSD master toggle, reset, close.
            BubbleRow {
	                BubbleButton(
	                    "Touch Layout",
	                    LineAwesomeIcons.ThLargeSolid,
	                    selected = playSelection.value == 8,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(8) }
	                BubbleButton(
	                    "OSD",
	                    if (osdShown.value) LineAwesomeIcons.EyeSolid
                    else LineAwesomeIcons.EyeSlashSolid,
                    stateLine = if (osdShown.value) "On" else "Off",
	                    accent = if (osdShown.value)
	                        BubbleAccent.Active else BubbleAccent.Normal,
	                    selected = playSelection.value == 9,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(9) }
	                BubbleButton(
	                    "Reset",
	                    LineAwesomeIcons.RedoAltSolid,
	                    selected = playSelection.value == 10,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(10) }
	                BubbleButton(
	                    "Close Game",
	                    LineAwesomeIcons.PowerOffSolid,
	                    accent = BubbleAccent.Danger,
	                    selected = playSelection.value == 11,
	                    modifier = Modifier.weight(1f),
	                ) { activatePlaySelection(11) }
	            }
        }
    }

    /** Even-spaced four-cell row, used by [PlayingNowTab] for grid rows. */
    @Composable
    private fun BubbleRow(content: @Composable RowScope.() -> Unit) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }

    /** Visual variants for [BubbleButton]. Normal is the default surface;
     *  Primary tints the bubble with the PS2-blue accent (Resume); Active
     *  is a softer accent used for toggleable bubbles whose state is
     *  currently "on"; Danger paints the bubble red for destructive
     *  actions. */
    private enum class BubbleAccent { Normal, Primary, Active, Danger }

    /** Square bubble action. Icon centered on top, label below, optional
     *  state line under the label (e.g. "On" / "Software") so toggleable
     *  bubbles communicate their current value without separate text. */
    @Composable
	    private fun BubbleButton(
	        label: String,
	        icon: ImageVector,
	        modifier: Modifier = Modifier,
	        stateLine: String? = null,
	        accent: BubbleAccent = BubbleAccent.Normal,
	        dim: Boolean = false,
	        selected: Boolean = false,
	        onClick: () -> Unit,
	    ) {
        // Per-variant palette. Dim wins over any accent so a Hardcore-mode
        // Save State row still reads "blocked".
        val bg: Color
        val border: Color
        val fg: Color
        when {
            dim -> {
                bg = Color(0xFF1F1F1F)
                border = Color(0xFF2E2E2E)
                fg = Color(0xFF666666)
            }
            accent == BubbleAccent.Primary -> {
                bg = Colors.pasx2_blue.copy(alpha = 0.22f)
                border = Colors.pasx2_blue.copy(alpha = 0.65f)
                fg = Color.White
            }
            accent == BubbleAccent.Active -> {
                bg = Color(0xFF222F40)
                border = Colors.pasx2_blue.copy(alpha = 0.50f)
                fg = Color.White
            }
            accent == BubbleAccent.Danger -> {
                bg = Color(0xFF2E1818)
                border = Color(0xFFFF6B6B).copy(alpha = 0.55f)
                fg = Color(0xFFFF8B8B)
            }
            else -> {
                bg = Color(0xFF1F2123)
                border = Color.White.copy(alpha = 0.10f)
                fg = Color.White
            }
        }

	        var focused by remember { mutableStateOf(false) }
	        val highlighted = focused || selected
	        val glowBlue = Color(0xFF3DA5FF)
	        Column(
	            modifier = modifier
	                .aspectRatio(1.35f)
	                .onFocusChanged { focused = it.isFocused }
                // Controller selection highlight: blue glow + outline when this
	                // bubble has D-pad focus.
	                .then(
	                    if (highlighted)
	                        Modifier.shadow(10.dp, RoundedCornerShape(10.dp), ambientColor = glowBlue, spotColor = glowBlue)
	                    else Modifier
	                )
	                .clip(RoundedCornerShape(10.dp))
	                .background(bg)
	                .border(if (highlighted) 2.dp else 1.dp, if (highlighted) glowBlue else border, RoundedCornerShape(10.dp))
                .clickable(enabled = !dim, onClick = onClick)
                .padding(horizontal = 4.dp, vertical = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Image(
                imageVector = icon,
                contentDescription = null,
                colorFilter = ColorFilter.tint(fg),
                modifier = Modifier.size(16.dp),
            )
            Spacer(Modifier.height(2.dp))
            Text(
                label,
                color = fg,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            if (stateLine != null) {
                // Active accent → state line in PS2 blue (matches the
                // ToggleBubble "On" treatment in the Performance grid).
                // Other variants keep the muted-fg colour so Normal bubbles
                // don't accidentally read as active.
                val stateColor = if (accent == BubbleAccent.Active)
                    Colors.pasx2_blue else fg.copy(alpha = 0.7f)
                Text(
                    stateLine,
                    color = stateColor,
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                )
            }
        }
    }


    /**
     * Thin horizontal divider with a left-anchored fade — opaque at the
     * left edge (where the row label starts) and fading to transparent
     * at the right edge. The row "aura" backgrounds use a fainter
     * version of this same gradient so the dividers and rows visually
     * tie together.
     */
    @Composable
    private fun MenuDivider() {
        Box(
            Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(
                    Brush.horizontalGradient(
                        listOf(
                            Color.White.copy(alpha = 0.35f),
                            Color.Transparent,
                        )
                    )
                ),
        )
    }

    @Composable
    private fun ExitConfirm() {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                "Close current game?",
                color = Color.White,
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 6.dp),
            )
	            MenuRow("Save State And Exit", selected = modalSelection.value == 0) {
	                exitSaveStateAndExit()
	            }
	            MenuRow("Exit Without Saving", danger = true, selected = modalSelection.value == 1) {
	                exitWithoutSaving()
	            }
	            MenuRow("Back", selected = modalSelection.value == 2) { enterState(State.Root) }
        }
    }

    @Composable
    private fun ResetConfirm() {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                "Reset the system? Unsaved progress will be lost.",
                color = Color.White,
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 6.dp),
            )
	            MenuRow("Yes, Reset", danger = true, selected = modalSelection.value == 0) {
	                resetSystem()
	            }
	            MenuRow("Back", selected = modalSelection.value == 1) { enterState(State.Root) }
        }
    }

    /** Modern red-bubble hint shown when the user taps Save / Load State
     *  while hardcore mode is active. Fits inside the bottom-left modal
     *  box; auto-dismisses on tap. */
    @Composable
    private fun HardcoreBlockedBubble() {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0xFF5A1A1A))
                .clickable { state.value = State.Root }
                .padding(14.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("⛔", fontSize = 16.sp, color = Color(0xFFFF6B6B))
                Spacer(Modifier.width(8.dp))
                Text(
                    "Disabled in Hardcore",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
            Spacer(Modifier.height(6.dp))
            Text(
                "Save states and load states are blocked while RetroAchievements Hardcore mode is active. Disable Hardcore from the Achievements panel to use them.",
                color = Color(0xFFEEDDDD),
                fontSize = 11.sp,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                "Tap to dismiss",
                color = Color(0xFFFF6B6B),
                fontSize = 10.sp,
            )
        }
    }

    /** Fullscreen confirmation modal for enabling hardcore mode. Styled
     *  to evoke the PS2 BIOS UI — centered black panel with a thin double
     *  border and chunky monospace-feeling title. The action enables the
     *  Achievements/HardcoreMode flag in the BASE settings layer; native
     *  ApplySettings folds that into Achievements::ResetHardcoreMode
     *  which resets the running game. */
    @Composable
    private fun HardcoreEnableConfirmFullscreen() {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xCC000000))
                // Eat taps so background controls don't trigger.
                .clickable(
                    indication = null,
                    interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
                ) {},
            contentAlignment = Alignment.Center,
        ) {
            Column(
                modifier = Modifier
                    .width(420.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(Color(0xFF0A0A18))
                    .border(2.dp, Color(0xFF8888AA), RoundedCornerShape(2.dp))
                    .padding(2.dp)
                    .border(1.dp, Color(0xFF333366), RoundedCornerShape(2.dp))
                    .padding(20.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    "ENABLE HARDCORE MODE",
                    color = Color(0xFFFF8888),
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 2.sp,
                )
                Spacer(Modifier.height(2.dp))
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(1.dp)
                        .background(Color(0xFF8888AA)),
                )
                Spacer(Modifier.height(14.dp))
                Text(
                    "Hardcore mode disables save states, load states, and cheats. Achievements unlocked while hardcore is active are recorded as such on RetroAchievements.",
                    color = Color(0xFFDDDDEE),
                    fontSize = 13.sp,
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "The current game will reset.",
                    color = Color(0xFFFFAAAA),
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.height(20.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
	                    BiosLikeButton(
	                        label = "CANCEL",
	                        primary = false,
	                        selected = modalSelection.value == 0,
	                        modifier = Modifier.weight(1f),
	                    ) { enterState(State.Root) }
	                    BiosLikeButton(
	                        label = "ENABLE",
	                        primary = true,
	                        selected = modalSelection.value == 1,
	                        modifier = Modifier.weight(1f),
	                    ) { enableHardcoreMode() }
                }
            }
        }
    }

    @Composable
	    private fun BiosLikeButton(
	        label: String,
	        primary: Boolean,
	        selected: Boolean = false,
	        modifier: Modifier = Modifier,
	        onClick: () -> Unit,
	    ) {
        val bg = if (primary) Color(0xFF5A1A1A) else Color(0xFF1A1A2A)
        val border = if (primary) Color(0xFFFF6B6B) else Color(0xFF8888AA)
        val fg = if (primary) Color(0xFFFFCCCC) else Color(0xFFDDDDEE)
        Box(
            modifier = modifier
	                .clip(RoundedCornerShape(2.dp))
	                .background(bg)
	                .border(if (selected) 2.dp else 1.dp, if (selected) Color(0xFF3DA5FF) else border, RoundedCornerShape(2.dp))
                .clickable(onClick = onClick)
                .padding(vertical = 12.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                label,
                color = fg,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                letterSpacing = 2.sp,
            )
        }
    }

    /** Single menu row. Compact (24dp) so all 10 root items fit without
     *  scrolling on phone landscape. Background is a left-anchored
     *  alpha gradient that "auras" the text and matches the divider
     *  fade direction. Danger variant tints text red for destructive
     *  actions (Close Game / Exit Without Saving). Optional icon
     *  rendered on the left, tinted with the same color as the label
     *  text — mirrors PCSX2 ImGui FullscreenUI's leading-icon menu
     *  rows in DrawPauseMenu. */
    @Composable
	    private fun MenuRow(
	        label: String,
	        icon: ImageVector? = null,
	        danger: Boolean = false,
	        dim: Boolean = false,
	        selected: Boolean = false,
	        onClick: () -> Unit,
	    ) {
        val textColor = when {
            dim -> Color(0xFF666666)
            danger -> Color(0xFFFF6B6B)
            else -> Color.White
        }
        val auraStart = when {
            dim -> Color.White.copy(alpha = 0.02f)
            danger -> Color(0xFFFF6B6B).copy(alpha = 0.10f)
            else -> Color.White.copy(alpha = 0.06f)
        }
        Box(
	            Modifier
	                .fillMaxWidth()
	                .height(24.dp)
	                .background(
                    Brush.horizontalGradient(
                        listOf(
                            auraStart,
                            Color.Transparent,
                        )
                    )
	                )
	                .border(
	                    if (selected) 1.dp else 0.dp,
	                    if (selected) Color(0xFF3DA5FF) else Color.Transparent,
	                    RoundedCornerShape(3.dp)
	                )
	                .clickable(onClick = onClick)
	                .padding(horizontal = 6.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (icon != null) {
                    Image(
                        imageVector = icon,
                        contentDescription = null,
                        colorFilter = ColorFilter.tint(textColor),
                        modifier = Modifier.size(14.dp),
                    )
                    Spacer(Modifier.width(8.dp))
                } else {
                    // Reserve same space so labels align across rows w/
                    // and w/o icons (Confirm-screen Back rows skip icons).
                    Spacer(Modifier.width(22.dp))
                }
                Text(label, color = textColor, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}
