package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay

/**
 * Performance section of the in-game settings overlay.
 *
 * Mutates the live [Settings] state and routes the write through
 * [InGameOverlay.saveSettings], which picks the global or per-game
 * storage tier based on the overlay's current scope. Applies via
 * [Settings.applyTo] so toggles take effect immediately on a running
 * VM. Every visible setting maps 1-1 onto an EmuCore key (see Settings
 * field comments for the exact `<section>/<key>`).
 *
 * Column + verticalScroll instead of LazyColumn so the tab can sit
 * inside the wrap-content RootTabs container without needing a hard
 * height bound. List is short (~9 rows) so non-lazy is fine.
 */
@Composable
fun PerformanceTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        // Speedhack profile presets. Equality against s.copy(...) means the
        // segment auto-reflects "Custom" once the user tweaks any speedhack below.
        run {
            val safe = s.copy(eeCycleRate = 0, eeCycleSkip = 0, mtvu = true, vu1Instant = true,
                vuFlagHack = true, intcStat = true, waitLoop = true, fastCDVD = false,
                // Restore the GPU-quality levers the Fast/Low-End presets lower, so
                // Optimal is a COMPLETE reset to recommended defaults — not just the
                // speedhacks (e.g. Texture Preloading back to Full, blending to Basic).
                // Resolution is left as-is so upscalers aren't dropped to native.
                accurateBlendingUnit = 1, hwMipmap = true, texturePreloading = 2, hwRov = false)
            // Fast = speed-first: EE cycle skip + fast CDVD, plus render-side wins
            // that are safe for most games (native resolution + Basic blending).
            val fast = s.copy(eeCycleRate = 0, eeCycleSkip = 2, mtvu = true, vu1Instant = true,
                vuFlagHack = true, intcStat = true, waitLoop = true, fastCDVD = true,
                upscaleFloat = 1.0f, accurateBlendingUnit = 1)
            // Low-End = every cheap GPU/CPU lever, MTVU gated on core count. Built
            // from the shared Settings.lowEndPreset so it matches the setup wizard.
            val lowEnd = com.armsx2.config.Settings.lowEndPreset(
                s.copy(eeCycleRate = 0, mtvu = true, vu1Instant = true,
                    vuFlagHack = true, intcStat = true, waitLoop = true, fastCDVD = true),
                mtvu = com.armsx2.DeviceTier.mtvuDefault(),
            )
            // -1 = no preset matches (custom): no segment highlighted.
            val idx = when (s) { safe -> 0; fast -> 1; lowEnd -> 2; else -> -1 }
            SegmentedRow(
                label = "Speedhack Profile",
                options = listOf("Optimal", "Fast", "Low-End"),
                selectedIndex = idx,
                onChange = { when (it) { 0 -> apply(safe); 1 -> apply(fast); 2 -> apply(lowEnd) } },
            )
        }
        HelpText("Tap a preset. Optimal = safe for most games. Fast = aggressive speedhacks + native resolution for low-end devices; may glitch some. Low-End = Fast plus every cheap GPU lever (native res, min blending, no mipmaps/palette-conv, partial texture preload) with MTVU auto-set from your CPU. Tweaking any setting un-highlights the presets (custom).")
        SettingsDivider()
        // ---- Display Resolution (HW scaler), NetherSX2-style ----------------
        // Shrinks the game's OUTPUT surface (hardware-composer upscales to the
        // screen) to cut GPU present cost, heat and battery. Global pref (not a
        // Settings/EmuCore field) applied live via SurfaceCallbacks.applyHwScaler.
        run {
            val hwScaler = com.armsx2.Main.prefs.getInt("ui.hwScaler", 0)
            SegmentedRow(
                label = "Display Resolution (HW scaler)",
                options = listOf("Screen", "3x PS2", "2x PS2", "1x PS2"),
                selectedIndex = when (hwScaler) { 3 -> 1; 2 -> 2; 1 -> 3; else -> 0 },
                description = "Reduces the display resolution to significantly decrease device heat and battery drain. Screen = full quality (off). 3x PS2 = High Quality, 2x PS2 = Balanced, 1x PS2 = Battery Saver / Max Performance. Separate from the internal rendering resolution — menus stay sharp either way.",
                onChange = {
                    val n = when (it) { 1 -> 3; 2 -> 2; 3 -> 1; else -> 0 }
                    com.armsx2.Main.prefs.edit().putInt("ui.hwScaler", n).apply()
                    (com.armsx2.Main.surface.value as? com.armsx2.SurfaceCallbacks)?.applyHwScaler()
                },
            )
        }
        SettingsDivider()
        CollapsibleSection("Speedhacks", initiallyExpanded = false) {
            IntSliderRow(
                label = "EE Cycle Rate",
                value = s.eeCycleRate,
                min = -3,
                max = 3,
                description = "CPU overclock/underclock. Higher can help CPU-bound games but may break timing.",
                valueFormatter = { rate ->
                    when (rate) {
                        -3 -> "50%"
                        -2 -> "60%"
                        -1 -> "75%"
                        0 -> "100%"
                        1 -> "130%"
                        2 -> "180%"
                        3 -> "300%"
                        else -> "$rate"
                    }
                },
                onChange = { apply(s.copy(eeCycleRate = it)) },
            )
            SettingsDivider()
            IntSliderRow(
                label = "EE Cycle Skip",
                value = s.eeCycleSkip,
                min = 0,
                max = 3,
                description = "Skips EE cycles for speed. Can cause stutter, physics bugs, or crashes.",
                onChange = { apply(s.copy(eeCycleSkip = it)) },
            )
            SettingsDivider()
            // Recompiler float-clamping accuracy (PCSX2 parity). Higher = more
            // accurate float handling (fixes SPS / missing geometry / VU glitches)
            // at a speed cost. Needs a recompiler reset, so restart the game.
            SegmentedRow(
                label = "EE/FPU Clamping",
                options = listOf("None", "Normal", "Extra", "Full"),
                selectedIndex = s.eeClampMode.coerceIn(0, 3),
                description = "FPU overflow/rounding accuracy. Restart the game to apply.",
                onChange = { apply(s.copy(eeClampMode = it)) },
            )
            SettingsDivider()
            SegmentedRow(
                label = "VU Clamping",
                options = listOf("None", "Normal", "Extra", "Extra+Sign"),
                selectedIndex = s.vuClampMode.coerceIn(0, 3),
                description = "VU float clamping (both VU0 + VU1). Restart the game to apply.",
                onChange = { apply(s.copy(vuClampMode = it)) },
            )
            SettingsDivider()
            SegmentedRow(
                label = "EE FPU Round Mode",
                options = listOf("Nearest", "Negative", "Positive", "Chop"),
                selectedIndex = s.eeFpuRoundMode.coerceIn(0, 3),
                description = "EE FPU float rounding. Chop (toward zero) is the PS2 default — change only if a game needs it. Restart the game to apply.",
                onChange = { apply(s.copy(eeFpuRoundMode = it)) },
            )
            SettingsDivider()
            SegmentedRow(
                label = "VU0 Round Mode",
                options = listOf("Nearest", "Negative", "Positive", "Chop"),
                selectedIndex = s.vu0RoundMode.coerceIn(0, 3),
                description = "VU0 float rounding. Chop is the PS2 default. Restart the game to apply.",
                onChange = { apply(s.copy(vu0RoundMode = it)) },
            )
            SettingsDivider()
            SegmentedRow(
                label = "VU1 Round Mode",
                options = listOf("Nearest", "Negative", "Positive", "Chop"),
                selectedIndex = s.vu1RoundMode.coerceIn(0, 3),
                description = "VU1 float rounding. Chop is the PS2 default. Restart the game to apply.",
                onChange = { apply(s.copy(vu1RoundMode = it)) },
            )
            SettingsDivider()
            // Speed Limit % — caps emulation speed as a % of native (100 = full speed).
            // Arbitrary value; default stays 100. Affects audio pitch / timing / RA.
            IntSliderRow(
                label = "Speed Limit %",
                value = s.nominalSpeedPercent.coerceIn(10, 1000),
                min = 10,
                max = 1000,
                description = "Emulation speed as % of native (100 = full speed). Affects audio pitch, game timing and RetroAchievements (hardcore stays at/above 100%). This is NOT a display cap — pair it with Display FPS Cap for per-game tuning. Best left at 100 unless a game needs it.",
                valueFormatter = { "$it%" },
                onChange = { apply(s.copy(nominalSpeedPercent = it)) },
            )
            SettingsDivider()
            // Display FPS Cap — caps the PRESENTED frame rate independently of Speed %.
            // The GS thread paces presents (accumulator) to hold ANY target rate while
            // emulation runs full speed (no slowdown). Arbitrary value; 0 = off.
            IntSliderRow(
                label = "Display FPS Cap",
                value = s.fpsLimit.coerceIn(0, 60),
                min = 0,
                max = 60,
                description = "Caps the PRESENTED (display) frame rate — emulation keeps full speed, so this is a display cap, not true emulation FPS. Any value works (use with Speed Limit % to fine-tune per game). 0 = off; values between the clean rates (60/30/20/15) pace a little unevenly.",
                valueFormatter = { if (it == 0) "Off" else "$it fps" },
                onChange = { apply(s.copy(fpsLimit = it)) },
            )
            SettingsDivider()
            // Per-region emulated vsync rate (PCSX2 EmuCore/GS FramerateNTSC / FrameratePAL)
            // — the PS2 refresh the game targets. Defaults 60/50 Hz (true 59.94/50.00).
            // Speed Limit % is relative to this; this is the rate, not a display cap.
            IntSliderRow(
                label = "NTSC Framerate (Hz)",
                value = Math.round(s.framerateNtsc).coerceIn(20, 75),
                min = 20,
                max = 75,
                description = "Emulated refresh for NTSC (US/JP) games. Default 60 (59.94 Hz). Like NetherSX2's per-region rate; games internally at 30fps run at half this.",
                valueFormatter = { "$it Hz" },
                onChange = { apply(s.copy(framerateNtsc = it.toFloat())) },
            )
            SettingsDivider()
            IntSliderRow(
                label = "PAL Framerate (Hz)",
                value = Math.round(s.frameratePal).coerceIn(20, 75),
                min = 20,
                max = 75,
                description = "Emulated refresh for PAL (EU) games. Default 50 Hz. Games internally at 25fps run at half this.",
                valueFormatter = { "$it Hz" },
                onChange = { apply(s.copy(frameratePal = it.toFloat())) },
            )
            SettingsDivider()
            IntSliderRow(
                label = "Frame Skip",
                value = s.frameSkip,
                min = 0,
                max = 5,
                description = "Low-end devices: draw 1 of every (N+1) frames to free up GPU. Emulation still runs full speed; higher = choppier but faster.",
                valueFormatter = { if (it == 0) "Off" else "Skip $it" },
                onChange = { apply(s.copy(frameSkip = it)) },
            )
        }
        SettingsDivider()
        CollapsibleSection("GameDB Fixes") {
            HelpText("Compatibility shortcuts. Leave GameDB Fixes off unless a game needs one of the fixes below.")
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                BubbleGridRow {
                    ToggleBubble("Skip BIOS", s.enableFastBoot, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableFastBoot = it))
                    }
                    ToggleBubble("GameDB Fixes", s.enableGameFixes, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = it))
                    }
                    ToggleBubble("Skip MPEG", s.gamefixSkipMpeg, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixSkipMpeg = it))
                    }
                    ToggleBubble("FMV Software", s.gamefixSoftwareRendererFmv, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixSoftwareRendererFmv = it))
                    }
                }
                BubbleGridRow {
                    ToggleBubble("EE Timing", s.gamefixEETiming, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixEETiming = it))
                    }
                    ToggleBubble("Instant DMA", s.gamefixInstantDma, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixInstantDma = it))
                    }
                    ToggleBubble("Blit FPS", s.gamefixBlitInternalFps, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixBlitInternalFps = it))
                    }
                    ToggleBubble("FPU Multiply", s.gamefixFpuMul, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixFpuMul = it))
                    }
                }
                BubbleGridRow {
                    ToggleBubble("OPH Flag", s.gamefixOphFlag, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixOphFlag = it))
                    }
                    ToggleBubble("GIF FIFO", s.gamefixGifFifo, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixGifFifo = it))
                    }
                    ToggleBubble("DMA Busy", s.gamefixDmaBusy, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixDmaBusy = it))
                    }
                    ToggleBubble("VIF1 Stall", s.gamefixVif1Stall, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixVif1Stall = it))
                    }
                }
                BubbleGridRow {
                    ToggleBubble("I-Bit", s.gamefixIbit, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixIbit = it))
                    }
                    ToggleBubble("Full VU0 Sync", s.gamefixFullVu0Sync, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixFullVu0Sync = it))
                    }
                    ToggleBubble("VU Add-Sub", s.gamefixVuAddSub, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixVuAddSub = it))
                    }
                    ToggleBubble("VU Overflow", s.gamefixVuOverflow, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixVuOverflow = it))
                    }
                }
                BubbleGridRow {
                    ToggleBubble("Extra XGKICK", s.gamefixXgkick, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixXgkick = it))
                    }
                    ToggleBubble("Goemon TLB", s.gamefixGoemonTlb, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixGoemonTlb = it))
                    }
                    ToggleBubble("VU Sync", s.gamefixVuSync, modifier = Modifier.weight(1f)) {
                        apply(s.copy(enableGameFixes = true, gamefixVuSync = it))
                    }
                    Spacer(Modifier.weight(1f))
                }
            }
            HelpText(
                "Skip BIOS - bypasses the PS2 startup screen.\n" +
                    "GameDB Fixes - master switch for manual compatibility fixes.\n" +
                    "Skip MPEG - skips problematic video playback.\n" +
                    "FMV Software - switches FMVs to software rendering.\n" +
                    "EE Timing - adjusts CPU timing for sensitive games.\n" +
                    "Instant DMA - completes DMA transfers immediately.\n" +
                    "Blit FPS - uses PCSX2's internal FPS blit workaround.\n" +
                    "FPU Multiply - fixes FPU multiply accuracy (Tales of Destiny).\n" +
                    "OPH Flag - VU OPH flag hack (Bleach Blade Battlers, Growlanser II).\n" +
                    "GIF FIFO - accurately emulates the GIF FIFO (fixes some hangs).\n" +
                    "DMA Busy - handles the DMA busy flag (Mana Khemia, Metal Saga).\n" +
                    "VIF1 Stall - emulates VIF1 FIFO stalls (SOCOM 2 HUD, Spy Hunter).\n" +
                    "I-Bit - VU I-bit branch-delay fix (Scarface, Crash Tag Team Racing).\n" +
                    "Full VU0 Sync - fully synchronizes VU0 with the EE.\n" +
                    "VU Add-Sub - VU add/sub accuracy hack (Tri-Ace: Star Ocean 3, VP2, RadiataStories).\n" +
                    "VU Overflow - clamps VU overflow (Superman Returns).\n" +
                    "Extra XGKICK - extra VU XGKICK sync (Erementar Gerad).\n" +
                    "Goemon TLB - preloads TLB map for Goemon.\n" +
                    "VU Sync - runs VU behind the EE for tight sync (Gitaroo Man, Simple 2000 games)."
            )
        }
        SettingsDivider()
        CollapsibleSection("Advanced Speedhacks") {
            Spacer(Modifier.height(8.dp))
            // On/Off toggles as a 4-wide bubble grid, matching the Playing-Now
            // action grid in InGameOverlay. Two rows of four cells — last cell
            // is a Spacer so the seven toggles keep uniform widths across both
            // rows. Labels are abbreviated to fit the ~74dp cell.
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                BubbleGridRow {
                    ToggleBubble("MTVU", s.mtvu, modifier = Modifier.weight(1f)) {
                        apply(s.copy(mtvu = it))
                    }
                    ToggleBubble("Instant VU1", s.vu1Instant, modifier = Modifier.weight(1f)) {
                        apply(s.copy(vu1Instant = it))
                    }
                    ToggleBubble("VU Flag Hack", s.vuFlagHack, modifier = Modifier.weight(1f)) {
                        apply(s.copy(vuFlagHack = it))
                    }
                    ToggleBubble("Fast CDVD", s.fastCDVD, modifier = Modifier.weight(1f)) {
                        apply(s.copy(fastCDVD = it))
                    }
                }
                BubbleGridRow {
                    ToggleBubble("INTC Stat", s.intcStat, modifier = Modifier.weight(1f)) {
                        apply(s.copy(intcStat = it))
                    }
                    ToggleBubble("Wait Loop", s.waitLoop, modifier = Modifier.weight(1f)) {
                        apply(s.copy(waitLoop = it))
                    }
                    // Frame Limiter lives on the Play tab's quick toggles — no need
                    // to duplicate it here.
                    // ARMSX2 perf-knob: A/B disable for the arm64 VU1 JIT
                    // NEON peephole fusions. Default on; flip off to confirm
                    // whether a per-game regression traces back to our
                    // matrix-transform / cross-product fusion peepholes.
                    // Block recompilation is not invalidated on toggle —
                    // already-compiled blocks keep their fused path; new
                    // blocks pick up the new gate. Restart the game for a
                    // clean A/B (or hit the recompiler tab to flip a CPU off
                    // then on, which forces a cache rebuild).
                    ToggleBubble("VU NEON Fusions", s.vuNeonFusions, modifier = Modifier.weight(1f)) {
                        apply(s.copy(vuNeonFusions = it))
                    }
                }
                // Heavy / aggressive perf levers — off by default, may break
                // some games. Effective on next block recompile; for a clean
                // A/B, restart the game (or bounce a CPU recompiler toggle
                // off→on to force a JIT cache rebuild).
                //
                //   Skip VU Stall Sim — drops the vu1_TestPipes_VU1 BL
                //     entirely. Was 19-32% of total CPU on Futurama / GoW2 /
                //     Ape Escape 3. Breaks games that depend on accurate
                //     FMAC / FDIV / EFU / IALU pipeline-stall timing
                //     (glitched models, missing geometry, audio crackle).
                //   Defer VU Writes — VF stores held in the NEON cache,
                //     committed at flush sites. Saves 1 Str per FMAC pair on
                //     transforms. Known to break SH2 graphics (cross-pair
                //     coherence) and similar.
                BubbleGridRow {
                    ToggleBubble("Skip VU Stall Sim", s.vuSkipStallSim, modifier = Modifier.weight(1f)) {
                        apply(s.copy(vuSkipStallSim = it))
                    }
                    ToggleBubble("Defer VU Writes", s.vuDeferredWrites, modifier = Modifier.weight(1f)) {
                        apply(s.copy(vuDeferredWrites = it))
                    }
                    ToggleBubble("Skip Dupe Frames", s.skipDuplicateFrames, modifier = Modifier.weight(1f)) {
                        apply(s.copy(skipDuplicateFrames = it))
                    }
                    Spacer(Modifier.weight(1f))
                }
            }
            HelpText(
                "MTVU - runs VU1 on its own thread (faster on multi-core); can break games needing tight EE/VU1 sync.\n" +
                    "Instant VU1 - runs VU1 microprograms in one shot instead of time-sliced. Faster, safe for most.\n" +
                    "VU Flag Hack - skips redundant VU status-flag updates. Safe for most games.\n" +
                    "Fast CDVD - shortens disc read timing to speed up loads. Can break timing-sensitive games.\n" +
                    "INTC Stat - fast-forwards INTC-status wait loops. Safe for most.\n" +
                    "Wait Loop - detects and skips EE idle loops. Safe for most.\n" +
                    "VU NEON Fusions - ARMSX2's VU1 JIT NEON optimizations (on by default). Turn off to test whether a per-game VU glitch traces back to them.\n" +
                    "Skip VU Stall Sim - drops VU pipeline-stall timing for a big speed win. Breaks games needing accurate VU timing (glitched models, missing geometry, audio crackle).\n" +
                    "Defer VU Writes - caches VU register writes in NEON registers (faster transforms). Can break games with cross-pair coherence (e.g. Silent Hill 2).\n" +
                    "Skip Dupe Frames - skips presenting identical frames to save GPU."
            )
        }
    }
}
