package com.armsx2.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import com.armsx2.EmuState
import com.armsx2.config.ConfigStore
import com.armsx2.config.GameDbOverrides
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.InGameOverlay
import kr.co.iefriends.pcsx2.NativeApp

/**
 * What PCSX2's game database sets for THIS game, with a switch each.
 *
 * The database is applied on top of the player's own settings, so for about one game in three
 * something on the other settings screens is not what the game actually runs with -- Final Fantasy
 * X gets its EE rounding, EE and VU0 clamping, blending, sprite rounding, auto flush and more from
 * it -- and nothing on screen used to say so. Each row names what the database sets and to what.
 * Switching one off keeps the game on your own setting for it instead; a row is also shown off
 * when a setting you changed for this game already outranks it.
 *
 * Only in per-game scope, and only when the database sets something for the game: with no game
 * there is nothing to show, and most games have no entry at all.
 */
@Composable
internal fun GameDbSection(state: MutableState<Settings>) {
    val serial = InGameOverlay.currentSerial.value?.takeIf { it.isNotBlank() } ?: return
    if (InGameOverlay.settingsScope.value != SettingsScope.Game) return
    val entries = remember(serial) { GameDbOverrides.entriesFor(serial) }
    if (entries.isEmpty()) return

    val s = state.value
    // Bumped after every switch: the switched-off list and the claims live in the store, not in
    // `s`, so a change here that leaves `s` alone still has to recompute them.
    var version by remember(serial) { mutableIntStateOf(0) }
    val global = remember(serial, s, version) { ConfigStore.loadGlobal() }
    val off = remember(serial, s, version) { GameDbOverrides.switchedOff(serial) }
    val claimedBySetting = remember(serial, s, version) {
        val overrides = ConfigStore.loadOverrides(serial)
        if (overrides == null) emptySet()
        else runCatching {
            GameDbOverrides.fieldsDriving(overrides, s, global).values.flatMapTo(HashSet()) { it }
        }.getOrDefault(emptySet())
    }
    val manualHardwareFixes = remember(s) { s.anyUserHackEnabled() }

    // The count is in the title so a collapsed section still says how much the database is doing.
    CollapsibleSection("${str("gamedb.title")} (${entries.size})") {
        HelpText(str("gamedb.help"))
        for (entry in entries) {
            val switchedOff = entry.name in off
            val bySetting = !switchedOff && entry.keys.any { it in claimedBySetting }
            val description = when {
                switchedOff -> str("gamedb.state.off")
                bySetting -> str("gamedb.state.yourSetting")
                entry.core && !s.emuCore.enableGameFixes -> str("gamedb.state.autoFixesOff")
                entry.userHack && manualHardwareFixes -> str("gamedb.state.manualFixes")
                else -> null
            }
            ToggleRow(
                label = "${entryName(entry)}: ${entryValue(entry)}",
                value = !switchedOff && !bySetting,
                description = description,
                onChange = { apply ->
                    if (apply) {
                        if (switchedOff) GameDbOverrides.setSwitchedOff(serial, entry.name, false)
                        if (bySetting) GameDbOverrides.releaseSetting(serial, entry, global)
                    } else {
                        GameDbOverrides.setSwitchedOff(serial, entry.name, true)
                    }
                    commit(serial, state)
                    version++
                },
            )
        }
    }
    SettingsDivider()
}

/**
 * Put a switch into effect. The switch changes this game's stored settings directly rather than
 * through InGameOverlay.saveSettings, whose pinning would put straight back a per-game value that
 * a switch just released.
 */
private fun commit(serial: String, state: MutableState<Settings>) {
    val resolved = ConfigStore.resolveForGame(serial)
    state.value = resolved
    InGameOverlay.settingsState.value = resolved
    val running = MainActivityRuntime.nativeReady.value &&
        MainActivityRuntime.eState.value != EmuState.STOPPED &&
        MainActivityRuntime.currentGame.value?.settingsKey == serial
    runCatching {
        if (running) {
            // File, then the core's copy of it, then the commit -- the same order as a settings
            // save in-game, so the change applies now rather than at the next boot.
            resolved.writeGameSettingsIni(ConfigStore.loadGlobal(), claimsFor = serial)
            NativeApp.reloadGameSettingsLayer()
            resolved.applyTo()
        } else {
            // Rewrites the file only if the game has one. Otherwise the next boot writes it
            // (MainActivityRuntime stages it from this game's stored settings).
            resolved.writeGameSettingsIni(ConfigStore.loadGlobal(), serial)
        }
    }
}

@Composable
private fun entryName(entry: GameDbOverrides.Entry): String {
    val key = when (entry.name) {
        "eeRoundMode" -> "perf.eeFpuRoundMode.label"
        "eeDivRoundMode" -> "gamedb.name.eeDivRoundMode"
        "vu0RoundMode" -> "perf.vu0RoundMode.label"
        "vu1RoundMode" -> "perf.vu1RoundMode.label"
        "eeClampMode" -> "perf.eeFpuClamping.label"
        "vu0ClampMode" -> "gamedb.name.vu0ClampMode"
        "vu1ClampMode" -> "perf.vu1Clamping.label"
        "mvuFlag" -> "perf.hack.vuFlagHack"
        "instantVU1" -> "perf.hack.instantVu1"
        "mtvu" -> "perf.hack.mtvu"
        "eeCycleRate" -> "perf.eeCycleRate.label"
        "FpuMul" -> "gamedb.name.fpuMul"
        "GoemonTlb" -> "perf.fix.goemonTlb"
        "SoftwareRendererFMV" -> "perf.fix.fmvSoftware"
        "SkipMPEG" -> "perf.fix.skipMpeg"
        "OPHFlag" -> "perf.fix.ophFlag"
        "EETiming" -> "perf.fix.eeTiming"
        "InstantDMA" -> "perf.fix.instantDma"
        "DMABusy" -> "perf.fix.dmaBusy"
        "GIFFIFO" -> "perf.fix.gifFifo"
        "VIFFIFO" -> "gamedb.name.vifFifo"
        "VIF1Stall" -> "perf.fix.vif1Stall"
        "VuAddSub" -> "perf.fix.vuAddSub"
        "Ibit" -> "perf.fix.iBit"
        "VUSync" -> "perf.fix.vuSync"
        "VUOverflow" -> "perf.fix.vuOverflow"
        "XGKick" -> "perf.fix.extraXgkick"
        "BlitInternalFPS" -> "perf.fix.blitFps"
        "FullVU0Sync" -> "perf.fix.fullVu0Sync"
        "autoFlush" -> "fixes.autoFlush.label"
        "cpuFramebufferConversion" -> "fixes.cpuFramebufferConversion.label"
        "readTCOnClose" -> "fixes.readTargetsWhenClosing.label"
        "disableDepthSupport" -> "fixes.disableDepthEmulation.label"
        "preloadFrameData" -> "fixes.preloadFrameData.label"
        "disablePartialInvalidation" -> "fixes.disablePartialInvalidation.label"
        "textureInsideRT" -> "fixes.textureInsideRt.label"
        "limit24BitDepth" -> "fixes.limit24BitDepth.label"
        "alignSprite" -> "fixes.alignSprite.label"
        "mergeSprite" -> "fixes.mergeSprite.label"
        "mipmap" -> "renderer.hwMipmapping.label"
        "accurateAlphaTest" -> "fixes.hwAccurateAlphaTest.label"
        "forceEvenSpritePosition" -> "gamedb.name.forceEvenSpritePosition"
        "bilinearUpscale" -> "fixes.bilinearDirty.label"
        "nativePaletteDraw" -> "fixes.unscaledPaletteDraw.label"
        "estimateTextureRegion" -> "fixes.estimateTextureRegion.label"
        "drawBuffering" -> "fixes.drawBuffering.label"
        "rewriteLargeST" -> "gamedb.name.rewriteLargeST"
        "PCRTCOffsets" -> "fixes.screenOffsets.label"
        "PCRTCOverscan" -> "fixes.showOverscan.label"
        "coalesceRenderPasses" -> "renderer.coalesceRenderPasses.label"
        "trilinearFiltering" -> "renderer.trilinear.label"
        "skipDrawStart" -> "fixes.skipDrawStart.label"
        "skipDrawEnd" -> "fixes.skipDrawEnd.label"
        "halfPixelOffset" -> "fixes.halfPixelOffset.label"
        "roundSprite" -> "fixes.roundSprite.label"
        "nativeScaling" -> "fixes.upscalingFixes.label"
        "texturePreloading" -> "renderer.texturePreloading.label"
        "deinterlace" -> "renderer.deinterlacing.label"
        "cpuSpriteRenderBW" -> "fixes.cpuSpriteBw.label"
        "cpuSpriteRenderLevel" -> "fixes.cpuSpriteRender.label"
        "cpuCLUTRender" -> "fixes.cpuClutRender.label"
        "gpuTargetCLUT" -> "fixes.gpuTargetClut.label"
        "gpuPaletteConversion" -> "fixes.gpuPaletteConversion.label"
        "minimumBlendingLevel" -> "gamedb.name.minBlending"
        "maximumBlendingLevel" -> "gamedb.name.maxBlending"
        // A ceiling that only bites on devices that pay for each read of the frame being drawn (a
        // copy of it, or a barrier), so the name says when it applies. Splashdown is the one user.
        "copyRoadMaximumBlendingLevel" -> "gamedb.name.copyRoadMaxBlending"
        "fieldShift" -> "gamedb.name.fieldShift"
        "hwDownloadMode" -> "renderer.hardwareDownloadMode.label"
        // The database's own name, which is also what the log prints: better than nothing for
        // an entry added upstream after this list.
        else -> return entry.name
    }
    return str(key)
}

@Composable
private fun entryValue(entry: GameDbOverrides.Entry): String {
    val v = entry.value
    fun pick(vararg options: String): String = options.getOrNull(v) ?: v.toString()
    val on = str("common.on")
    val off = str("common.off")
    return when (entry.name) {
        "eeRoundMode", "eeDivRoundMode", "vu0RoundMode", "vu1RoundMode" -> pick(
            str("perf.round.nearest"), str("perf.round.negative"), str("perf.round.positive"), str("perf.round.chop"),
        )
        "eeClampMode" -> pick(
            str("perf.clamp.none"), str("perf.clamp.normal"), str("perf.clamp.extra"), str("perf.clamp.full"), str("perf.clamp.exact"),
        )
        "vu0ClampMode", "vu1ClampMode" -> pick(
            str("perf.clamp.none"), str("perf.clamp.normal"), str("perf.clamp.extra"), str("perf.clamp.extraSign"), str("perf.clamp.exact"),
        )
        "eeCycleRate" -> if (v > 0) "+$v" else v.toString()
        "autoFlush" -> pick(str("fixes.opt.off"), str("fixes.opt.sprites"), str("fixes.opt.on"))
        "textureInsideRT" -> pick(str("fixes.opt.off"), str("fixes.opt.inside"), str("fixes.opt.merge"))
        "limit24BitDepth" -> pick(str("fixes.opt.off"), str("fixes.opt.upper"), str("fixes.opt.lower"))
        "halfPixelOffset" -> pick(
            str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.special"), str("fixes.opt.aggr"),
            str("fixes.opt.native"), str("fixes.opt.nwTex"),
        )
        "roundSprite" -> pick(str("fixes.opt.off"), str("fixes.opt.half"), str("fixes.opt.full"))
        "nativeScaling" -> pick(
            str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.aggr"), str("fixes.opt.normalPlus"), str("fixes.opt.aggrPlus"),
        )
        "cpuCLUTRender" -> pick(str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.aggr"))
        "trilinearFiltering" -> pick(off, "PS2", str("fixes.opt.forced"))
        "texturePreloading" -> pick(off, "Partial", str("fixes.opt.full"))
        "deinterlace" -> pick(
            str("fixes.opt.auto"), off, "Weave TFF", "Weave BFF", "Bob TFF", "Bob BFF",
            "Blend TFF", "Blend BFF", "Adapt TFF", "Adapt BFF",
        )
        "minimumBlendingLevel", "maximumBlendingLevel", "copyRoadMaximumBlendingLevel" ->
            pick("Minimum", "Basic", "Medium", "High", "Full", "Maximum")
        "hwDownloadMode" -> pick("Accurate", "Force Full", "No Readbacks", "Unsync", "Disabled", "Async")
        "cpuSpriteRenderBW" -> if (v <= 0) off else "${v * 64}px"
        "gpuPaletteConversion" -> when (v) { 0 -> off; 1 -> on; else -> str("gamedb.value.paletteWithFullPreload") }
        "cpuSpriteRenderLevel", "gpuTargetCLUT", "skipDrawStart", "skipDrawEnd" -> v.toString()
        // Everything left is an on/off fix; the game fixes are only ever listed switched on.
        else -> if (v != 0) on else off
    }
}
