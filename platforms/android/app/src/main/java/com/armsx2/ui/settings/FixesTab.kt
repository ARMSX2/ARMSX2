package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.ui.InGameOverlay

/**
 * Hardware / upscaling compatibility fixes — the PCSX2 "Hardware Fixes" and
 * "Upscaling Fixes" panels. Split out of [RendererTab] so Render keeps only
 * core quality/display settings.
 *
 * Every row writes into [Settings] via [InGameOverlay.saveSettings]; on a
 * running VM that reconfigures the GS live (Settings.applyGsLive → native
 * applyGSSettingsLive) so changes show without a restart. Note PCSX2 masks
 * upscaling hacks at native (1x) resolution and masks every UserHacks_* key
 * unless at least one fix is enabled — both are intentional parity behaviours.
 */
@Composable
fun FixesTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth(),
    ) {
        GameDbSection(state)
        CollapsibleSection(str("fixes.section.display")) {
        HelpText(
            str("fixes.section.display.help"),
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.antiBlur.label"),
            s.display.antiBlur,
            description = str("fixes.antiBlur.desc"),
        ) { apply(s.copy(display = s.display.copy(antiBlur = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.screenOffsets.label"),
            s.display.screenOffsets,
            description = str("fixes.screenOffsets.desc"),
        ) { apply(s.copy(display = s.display.copy(screenOffsets = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.showOverscan.label"),
            s.display.showOverscan,
            description = str("fixes.showOverscan.desc"),
        ) { apply(s.copy(display = s.display.copy(showOverscan = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableInterlaceOffset.label"),
            s.display.disableInterlaceOffset,
            description = str("fixes.disableInterlaceOffset.desc"),
        ) { apply(s.copy(display = s.display.copy(disableInterlaceOffset = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.syncToHostRefresh.label"),
            s.display.syncToHostRefresh,
            description = str("fixes.syncToHostRefresh.desc"),
        ) { apply(s.copy(display = s.display.copy(syncToHostRefresh = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableFramebufferFetch.label"),
            s.display.disableFramebufferFetch,
            description = str("fixes.disableFramebufferFetch.desc"),
        ) { apply(s.copy(display = s.display.copy(disableFramebufferFetch = it))) }
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.overrideTextureBarriers.label"),
            options = listOf(str("fixes.opt.auto"), str("fixes.opt.off"), str("fixes.opt.on")),
            selectedIndex = (s.display.overrideTextureBarriers + 1).coerceIn(0, 2),
            description = str("fixes.overrideTextureBarriers.desc"),
            onChange = { apply(s.copy(display = s.display.copy(overrideTextureBarriers = it - 1))) },
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.hwAccurateAlphaTest.label"),
            s.display.hwAccurateAlphaTest,
            description = str("fixes.hwAccurateAlphaTest.desc"),
        ) { apply(s.copy(display = s.display.copy(hwAccurateAlphaTest = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableVertexShaderExpand.label"),
            s.display.disableVertexShaderExpand,
            description = str("fixes.disableVertexShaderExpand.desc"),
        ) { apply(s.copy(display = s.display.copy(disableVertexShaderExpand = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.useBlitSwapChain.label"),
            s.display.useBlitSwapChain,
            description = str("fixes.useBlitSwapChain.desc"),
        ) { apply(s.copy(display = s.display.copy(useBlitSwapChain = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableShaderCache.label"),
            s.display.disableShaderCache,
            description = str("fixes.disableShaderCache.desc"),
        ) { apply(s.copy(display = s.display.copy(disableShaderCache = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.integerScaling.label"),
            s.hwFixes.integerScaling,
            description = str("fixes.integerScaling.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(integerScaling = it))) }
        SettingsDivider()
        // Display zoom (#383) — one AetherSX2-style slider that zooms into the picture, trimming
        // every edge by the same amount so it fills more of the screen without distorting. At
        // 100% it's off and the manual per-edge crops below apply instead; above 100% it takes
        // over. Nicer than juggling the four crops for the common "just zoom in a bit" case.
        IntSliderRow(
            str("fixes.zoom.label"), s.hwFixes.displayZoom, 100, 150,
            description = str("fixes.zoom.desc"),
            valueFormatter = { "$it%" },
            onReset = { apply(s.copy(hwFixes = s.hwFixes.copy(displayZoom = 100))) },
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(displayZoom = it))) }
        SettingsDivider()
        // Overscan crop (issue #293). Trims native PS2 pixels off each edge before aspect
        // and integer scaling. Plenty of games leave garbage or a black band in the region
        // a CRT's bezel would have covered; the core has always honoured GSConfig.Crop
        // (GSRenderer.cpp) but Android never surfaced it. Native pixels, so the value means
        // the same thing at any upscale multiplier. Ignored while Display Zoom is above 100%.
        Text(
            str("fixes.crop.header"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(horizontal = 4.dp, vertical = 4.dp),
        )
        IntSliderRow(
            str("fixes.crop.left"), s.hwFixes.cropLeft, 0, 128,
            onReset = { apply(s.copy(hwFixes = s.hwFixes.copy(cropLeft = 0))) },
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(cropLeft = it))) }
        IntSliderRow(
            str("fixes.crop.top"), s.hwFixes.cropTop, 0, 128,
            onReset = { apply(s.copy(hwFixes = s.hwFixes.copy(cropTop = 0))) },
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(cropTop = it))) }
        IntSliderRow(
            str("fixes.crop.right"), s.hwFixes.cropRight, 0, 128,
            onReset = { apply(s.copy(hwFixes = s.hwFixes.copy(cropRight = 0))) },
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(cropRight = it))) }
        IntSliderRow(
            str("fixes.crop.bottom"), s.hwFixes.cropBottom, 0, 128,
            description = str("fixes.crop.desc"),
            onReset = { apply(s.copy(hwFixes = s.hwFixes.copy(cropBottom = 0))) },
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(cropBottom = it))) }
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.dithering.label"),
            // "Force 32bit" (native Dithering==3) removes PS2 16-bit color banding — many
            // games look noticeably cleaner with it on.
            options = listOf(str("fixes.opt.off"), str("fixes.opt.scaled"), str("fixes.opt.unscaled"), str("fixes.opt.force32")),
            selectedIndex = s.hwFixes.dithering.coerceIn(0, 3),
            description = str("fixes.dithering.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(dithering = it))) },
        )
        }

        CollapsibleSection(str("fixes.section.upscaling")) {
        HelpText(
            str("fixes.section.upscaling.help"),
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.upscalingFixes.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.aggr"), str("fixes.opt.normalPlus"), str("fixes.opt.aggrPlus")),
            selectedIndex = s.hwFixes.nativeScaling.coerceIn(0, 4),
            description = str("fixes.upscalingFixes.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(nativeScaling = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.halfPixelOffset.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.special"), str("fixes.opt.aggr"), str("fixes.opt.native"), str("fixes.opt.nwTex")),
            selectedIndex = s.hwFixes.halfPixelOffset.coerceIn(0, 5),
            description = str("fixes.halfPixelOffset.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(halfPixelOffset = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.roundSprite.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.half"), str("fixes.opt.full")),
            selectedIndex = s.hwFixes.roundSprite.coerceIn(0, 2),
            description = str("fixes.roundSprite.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(roundSprite = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.bilinearDirty.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.half"), str("fixes.opt.forced")),
            selectedIndex = s.hwFixes.bilinearUpscale.coerceIn(0, 3),
            description = str("fixes.bilinearDirty.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(bilinearUpscale = it))) },
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.alignSprite.label"),
            s.hwFixes.alignSprite,
            description = str("fixes.alignSprite.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(alignSprite = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.mergeSprite.label"),
            s.hwFixes.mergeSprite,
            description = str("fixes.mergeSprite.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(mergeSprite = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.wildArmsOffset.label"),
            s.hwFixes.forceEvenSpritePosition,
            description = str("fixes.wildArmsOffset.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(forceEvenSpritePosition = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.unscaledPaletteDraw.label"),
            s.hwFixes.unscaledPaletteDraw,
            description = str("fixes.unscaledPaletteDraw.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(unscaledPaletteDraw = it))) }
        SettingsDivider()
        IntSliderRow(
            label = str("fixes.textureOffsetX.label"),
            value = s.hwFixes.textureOffsetX.coerceIn(0, 1000),
            min = 0,
            max = 1000,
            description = str("fixes.textureOffsetX.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(textureOffsetX = it))) },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("fixes.textureOffsetY.label"),
            value = s.hwFixes.textureOffsetY.coerceIn(0, 1000),
            min = 0,
            max = 1000,
            description = str("fixes.textureOffsetY.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(textureOffsetY = it))) },
        )
        }

        CollapsibleSection(str("fixes.section.hardware")) {
        HelpText(
            str("fixes.section.hardware.help"),
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.manualHardwareFixes.label"),
            s.hwFixes.manualUserHacks,
            description = str("fixes.manualHardwareFixes.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(manualUserHacks = it))) }
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.autoFlush.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.sprites"), str("fixes.opt.on")),
            selectedIndex = s.hwFixes.autoFlush.coerceIn(0, 2),
            description = str("fixes.autoFlush.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(autoFlush = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.textureInsideRt.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.inside"), str("fixes.opt.merge")),
            selectedIndex = s.hwFixes.textureInsideRt.coerceIn(0, 2),
            description = str("fixes.textureInsideRt.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(textureInsideRt = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.gpuTargetClut.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.inside"), str("fixes.opt.forced")),
            selectedIndex = s.hwFixes.gpuTargetClut.coerceIn(0, 2),
            description = str("fixes.gpuTargetClut.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(gpuTargetClut = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.cpuSpriteBw.label"),
            options = listOf(str("fixes.opt.off"), "64", "128", "256"),
            selectedIndex = s.hwFixes.cpuSpriteRenderBw.coerceIn(0, 3),
            description = str("fixes.cpuSpriteBw.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(cpuSpriteRenderBw = it))) },
        )
        SettingsDivider()
        SegmentedGridRow(
            label = str("fixes.cpuSpriteRender.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.sprite"), str("fixes.opt.triangle"), str("fixes.opt.aggressive"), str("fixes.opt.full"), str("fixes.opt.max")),
            selectedIndex = s.hwFixes.cpuSpriteRenderLevel.coerceIn(0, 5),
            columns = 3,
            description = str("fixes.cpuSpriteRender.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(cpuSpriteRenderLevel = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.cpuClutRender.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.normal"), str("fixes.opt.aggr")),
            selectedIndex = s.hwFixes.cpuClutRender.coerceIn(0, 2),
            description = str("fixes.cpuClutRender.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(cpuClutRender = it))) },
        )
        SettingsDivider()
        SegmentedRow(
            label = str("fixes.limit24BitDepth.label"),
            options = listOf(str("fixes.opt.off"), str("fixes.opt.upper"), str("fixes.opt.lower")),
            selectedIndex = s.hwFixes.limit24BitDepth.coerceIn(0, 2),
            description = str("fixes.limit24BitDepth.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(limit24BitDepth = it))) },
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.gpuPaletteConversion.label"),
            s.hwFixes.gpuPaletteConversion,
            description = str("fixes.gpuPaletteConversion.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(gpuPaletteConversion = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.cpuFramebufferConversion.label"),
            s.hwFixes.cpuFramebufferConversion,
            description = str("fixes.cpuFramebufferConversion.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(cpuFramebufferConversion = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.readTargetsWhenClosing.label"),
            s.hwFixes.readTargetsWhenClosing,
            description = str("fixes.readTargetsWhenClosing.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(readTargetsWhenClosing = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.preloadFrameData.label"),
            s.hwFixes.preloadFrameData,
            description = str("fixes.preloadFrameData.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(preloadFrameData = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.estimateTextureRegion.label"),
            s.hwFixes.estimateTextureRegion,
            description = str("fixes.estimateTextureRegion.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(estimateTextureRegion = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.drawBuffering.label"),
            s.hwFixes.drawBuffering,
            description = str("fixes.drawBuffering.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(drawBuffering = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableDepthEmulation.label"),
            s.hwFixes.disableDepthEmulation,
            description = str("fixes.disableDepthEmulation.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(disableDepthEmulation = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disablePartialInvalidation.label"),
            s.hwFixes.disablePartialInvalidation,
            description = str("fixes.disablePartialInvalidation.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(disablePartialInvalidation = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableSafeFeatures.label"),
            s.hwFixes.disableSafeFeatures,
            description = str("fixes.disableSafeFeatures.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(disableSafeFeatures = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.disableRenderFixes.label"),
            s.hwFixes.disableRenderFixes,
            description = str("fixes.disableRenderFixes.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(disableRenderFixes = it))) }
        SettingsDivider()
        IntSliderRow(
            label = str("fixes.skipDrawStart.label"),
            value = s.hwFixes.skipDrawStart.coerceIn(0, 5000),
            min = 0,
            max = 5000,
            description = str("fixes.skipDrawStart.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(skipDrawStart = it))) },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("fixes.skipDrawEnd.label"),
            value = s.hwFixes.skipDrawEnd.coerceIn(0, 5000),
            min = 0,
            max = 5000,
            description = str("fixes.skipDrawEnd.desc"),
            onChange = { apply(s.copy(hwFixes = s.hwFixes.copy(skipDrawEnd = it))) },
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.spinGpuReadbacks.label"),
            s.hwFixes.spinGpuReadbacks,
            description = str("fixes.spinGpuReadbacks.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(spinGpuReadbacks = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.spinCpuReadbacks.label"),
            s.hwFixes.spinCpuReadbacks,
            description = str("fixes.spinCpuReadbacks.desc"),
        ) { apply(s.copy(hwFixes = s.hwFixes.copy(spinCpuReadbacks = it))) }
        }

        CollapsibleSection(str("fixes.section.software")) {
        HelpText(
            str("fixes.section.software.help"),
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            str("fixes.autoFlushSw.label"),
            s.output.autoFlushSw,
            description = str("fixes.autoFlushSw.desc"),
        ) { apply(s.copy(output = s.output.copy(autoFlushSw = it))) }
        SettingsDivider()
        ToggleRow(
            str("fixes.mipmapSw.label"),
            s.output.mipmapSw,
            description = str("fixes.mipmapSw.desc"),
        ) { apply(s.copy(output = s.output.copy(mipmapSw = it))) }
        SettingsDivider()
        IntSliderRow(
            label = str("fixes.swThreads.label"),
            value = s.output.swThreads.coerceIn(0, 10),
            min = 0,
            max = 10,
            description = str("fixes.swThreads.desc"),
            onChange = { apply(s.copy(output = s.output.copy(swThreads = it))) },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("fixes.swThreadTileHeight.label"),
            value = s.output.swThreadsHeight.coerceIn(0, 8),
            min = 0,
            max = 8,
            description = str("fixes.swThreadTileHeight.desc"),
            onChange = { apply(s.copy(output = s.output.copy(swThreadsHeight = it))) },
        )
        }
        SettingsDivider()
        // GameDB fixes moved here from Performance: they are per-game compatibility switches the
        // GameDB already applies automatically, so they belong with the other advanced knobs
        // rather than in the tab people open to change speed settings.
        CollapsibleSection(str("perf.gamedbFixes.title")) {
            HelpText(str("perf.gamedbFixes.help"))
            ToggleRow(str("perf.fix.skipBios"), s.emuCore.enableFastBoot, description = str("perf.fix.skipBios.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableFastBoot = it))) }
            ToggleRow(str("perf.fix.gamedbFixes"), s.emuCore.enableGameFixes, description = str("perf.fix.gamedbFixes.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = it))) }
            // Compatibility patches sat in the Patches screen under the name "Enable Patches",
            // where nothing said they were the per-game COMPATIBILITY set PCSX2 ships — users
            // read it as "turn patches on/off" and switched it off, or blamed it for a
            // widescreen hack it never controlled. It is the same class of thing as the GameDB
            // fixes above, so it belongs beside them. Widescreen / cheats / no-interlacing stay
            // in the Patches screen; those really are patch choices.
            ToggleRow(str("perf.fix.compatPatches"), s.emuCore.enablePatches, description = str("perf.fix.compatPatches.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enablePatches = it))) }
            ToggleRow(str("perf.fix.skipMpeg"), s.emuCore.gamefixSkipMpeg, description = str("perf.fix.skipMpeg.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixSkipMpeg = it))) }
            if (s.emuCore.gamefixSkipMpeg) HelpText(str("perf.fix.skipMpeg.warning"))
            ToggleRow(str("perf.fix.fmvSoftware"), s.emuCore.gamefixSoftwareRendererFmv, description = str("perf.fix.fmvSoftware.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixSoftwareRendererFmv = it))) }
            ToggleRow(str("perf.fix.eeTiming"), s.emuCore.gamefixEETiming, description = str("perf.fix.eeTiming.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixEETiming = it))) }
            ToggleRow(str("perf.fix.instantDma"), s.emuCore.gamefixInstantDma, description = str("perf.fix.instantDma.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixInstantDma = it))) }
            ToggleRow(str("perf.fix.blitFps"), s.emuCore.gamefixBlitInternalFps, description = str("perf.fix.blitFps.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixBlitInternalFps = it))) }
            ToggleRow(str("perf.fix.ophFlag"), s.emuCore.gamefixOphFlag, description = str("perf.fix.ophFlag.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixOphFlag = it))) }
            ToggleRow(str("perf.fix.gifFifo"), s.emuCore.gamefixGifFifo, description = str("perf.fix.gifFifo.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixGifFifo = it))) }
            ToggleRow(str("perf.fix.dmaBusy"), s.emuCore.gamefixDmaBusy, description = str("perf.fix.dmaBusy.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixDmaBusy = it))) }
            ToggleRow(str("perf.fix.vif1Stall"), s.emuCore.gamefixVif1Stall, description = str("perf.fix.vif1Stall.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixVif1Stall = it))) }
            ToggleRow(str("perf.fix.iBit"), s.emuCore.gamefixIbit, description = str("perf.fix.iBit.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixIbit = it))) }
            ToggleRow(str("perf.fix.fullVu0Sync"), s.emuCore.gamefixFullVu0Sync, description = str("perf.fix.fullVu0Sync.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixFullVu0Sync = it))) }
            ToggleRow(str("perf.fix.vuAddSub"), s.emuCore.gamefixVuAddSub, description = str("perf.fix.vuAddSub.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixVuAddSub = it))) }
            ToggleRow(str("perf.fix.vuOverflow"), s.emuCore.gamefixVuOverflow, description = str("perf.fix.vuOverflow.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixVuOverflow = it))) }
            ToggleRow(str("perf.fix.extraXgkick"), s.emuCore.gamefixXgkick, description = str("perf.fix.extraXgkick.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixXgkick = it))) }
            ToggleRow(str("perf.fix.goemonTlb"), s.emuCore.gamefixGoemonTlb, description = str("perf.fix.goemonTlb.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixGoemonTlb = it))) }
            ToggleRow(str("perf.fix.vuSync"), s.emuCore.gamefixVuSync, description = str("perf.fix.vuSync.desc")) { apply(s.copy(emuCore = s.emuCore.copy(enableGameFixes = true, gamefixVuSync = it))) }
        }
        SettingsDivider()
        RecompilerSection(state)
        SettingsDivider()
        PineSection(state)
        Spacer(Modifier.height(8.dp))
    }
}

/** PINE, the IPC server external tools drive the emulator through. Lives beside the recompiler
 *  switches because it is the same class of control: a developer tool that a player has no reason
 *  to find, next to the other things you turn on to diagnose rather than to play.
 *
 *  The port is shown rather than edited. Changing it only matters when two emulators share a
 *  machine, which does not happen on a handheld, and stating it is the part that is actually
 *  needed — the listener is on loopback, so it does nothing until it is forwarded, and you cannot
 *  forward a port you were not told. */
@Composable
private fun PineSection(state: MutableState<Settings>) {
    val s = state.value
    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    CollapsibleSection(str("fixes.section.pine")) {
        HelpText(str("fixes.section.pine.help"))
        ToggleRow(
            str("fixes.pine.enable"),
            s.emuCore.pineEnabled,
            description = "${str("fixes.pine.enable.desc")} (127.0.0.1:${s.emuCore.pineSlot})",
        ) { apply(s.copy(emuCore = s.emuCore.copy(pineEnabled = it))) }
        if (s.emuCore.pineEnabled) HelpText("adb forward tcp:${s.emuCore.pineSlot} tcp:${s.emuCore.pineSlot}")
    }
}

// CollapsibleSection now lives in SettingsWidgets.kt (shared by the Fixes / Pad /
// Performance / Renderer tabs).

/** The former standalone Recompiler tab, folded in as a section. Turning a recompiler off drops
 *  that processor to an interpreter — correct but far slower — so it is a debugging control, not
 *  something to browse past on the way to a speed setting. */
@Composable
private fun RecompilerSection(state: MutableState<Settings>) {
    val settings = state.value
    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    CollapsibleSection(str("tab.recompiler")) {
        Text(
            str("jit.recompiler.warning"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 8.dp),
        )
        ToggleRow("EE (R5900)", settings.cpu.recEE) { apply(settings.copy(cpu = settings.cpu.copy(recEE = it))) }
        ToggleRow("IOP (R3000)", settings.cpu.recIOP) { apply(settings.copy(cpu = settings.cpu.copy(recIOP = it))) }
        ToggleRow("VU0", settings.cpu.recVU0) { apply(settings.copy(cpu = settings.cpu.copy(recVU0 = it))) }
        ToggleRow("VU1", settings.cpu.recVU1) { apply(settings.copy(cpu = settings.cpu.copy(recVU1 = it))) }
        ToggleRow("Fastmem", settings.cpu.enableFastmem) { apply(settings.copy(cpu = settings.cpu.copy(enableFastmem = it))) }
    }
}
