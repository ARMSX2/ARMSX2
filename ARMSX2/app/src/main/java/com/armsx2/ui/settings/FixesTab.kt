package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.ui.Colors
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
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        SectionHeader("Display Fixes")
        HelpText(
            "PCRTC / presentation fixes for the displayed image. Anti-Blur is on by " +
                "default; the rest are off unless a game needs them.",
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            "Anti-Blur",
            s.antiBlur,
            description = "Reduces the blur PCSX2 adds to mimic the PS2's blend. On by default.",
        ) { apply(s.copy(antiBlur = it)) }
        SettingsDivider()
        ToggleRow(
            "Screen Offsets",
            s.screenOffsets,
            description = "Applies the PCRTC screen offsets (centres the image like real hardware).",
        ) { apply(s.copy(screenOffsets = it)) }
        SettingsDivider()
        ToggleRow(
            "Show Overscan",
            s.showOverscan,
            description = "Shows the overscan border area some games render into.",
        ) { apply(s.copy(showOverscan = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Interlace Offset",
            s.disableInterlaceOffset,
            description = "Removes the interlace field offset; can stop shimmer but may add combing.",
        ) { apply(s.copy(disableInterlaceOffset = it)) }
        SettingsDivider()
        ToggleRow(
            "Sync To Host Refresh",
            s.syncToHostRefresh,
            description = "Paces emulation to your screen's refresh rate for smoother scrolling.",
        ) { apply(s.copy(syncToHostRefresh = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Framebuffer Fetch",
            s.disableFramebufferFetch,
            description = "Disables the framebuffer-fetch blending path. Diagnostic / compatibility.",
        ) { apply(s.copy(disableFramebufferFetch = it)) }
        SettingsDivider()
        SegmentedRow(
            label = "Override Texture Barriers",
            options = listOf("Auto", "Off", "On"),
            selectedIndex = (s.overrideTextureBarriers + 1).coerceIn(0, 2),
            description = "Forces the renderer's texture-barrier support on/off. Auto is recommended.",
            onChange = { apply(s.copy(overrideTextureBarriers = it - 1)) },
        )
        SettingsDivider()
        ToggleRow(
            "Integer Scaling",
            s.integerScaling,
            description = "Scales the image by whole-number factors for crisp, even pixels.",
        ) { apply(s.copy(integerScaling = it)) }
        SettingsDivider()
        SegmentedRow(
            label = "Dithering",
            options = listOf("Off", "Scaled", "Unscaled"),
            selectedIndex = s.dithering.coerceIn(0, 2),
            description = "Reduces colour banding. Unscaled matches the PS2 most closely.",
            onChange = { apply(s.copy(dithering = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Vsync Queue Size",
            value = s.vsyncQueueSize.coerceIn(0, 3),
            min = 0,
            max = 3,
            description = "Frames the GS thread may queue ahead. Higher can smooth pacing; adds latency.",
            onChange = { apply(s.copy(vsyncQueueSize = it)) },
        )

        SectionHeader("Upscaling Fixes")
        HelpText(
            "Only active when upscaling above Native. They reduce alignment/seam " +
                "artifacts but won't remove every bloom or glow.",
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        SegmentedRow(
            label = "Upscaling Fixes",
            options = listOf("Off", "Normal", "Aggr.", "Normal+", "Aggr.+"),
            selectedIndex = s.nativeScaling.coerceIn(0, 4),
            description = "Texture alignment hacks for upscaling (UserHacks_native_scaling).",
            onChange = { apply(s.copy(nativeScaling = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Half-Pixel Offset",
            options = listOf("Off", "Normal", "Special", "Aggr.", "Native", "NW-Tex"),
            selectedIndex = s.halfPixelOffset.coerceIn(0, 5),
            description = "Fixes shifted or blurry geometry/textures in some upscaled games.",
            onChange = { apply(s.copy(halfPixelOffset = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Round Sprite",
            options = listOf("Off", "Half", "Full"),
            selectedIndex = s.roundSprite.coerceIn(0, 2),
            description = "Rounds sprite coordinates to reduce seams or lines in 2D elements.",
            onChange = { apply(s.copy(roundSprite = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Bilinear Dirty",
            options = listOf("Off", "Normal", "Half", "Forced"),
            selectedIndex = s.bilinearUpscale.coerceIn(0, 3),
            description = "Changes bilinear filtering behavior for upscaled textures.",
            onChange = { apply(s.copy(bilinearUpscale = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "Align Sprite",
            s.alignSprite,
            description = "Fixes vertical lines/gaps in some 2D games when upscaling.",
        ) { apply(s.copy(alignSprite = it)) }
        SettingsDivider()
        ToggleRow(
            "Merge Sprite",
            s.mergeSprite,
            description = "Merges adjacent post-process sprites to remove seams.",
        ) { apply(s.copy(mergeSprite = it)) }
        SettingsDivider()
        ToggleRow(
            "Wild Arms Offset",
            s.forceEvenSpritePosition,
            description = "Forces even sprite/texture positions (UserHacks_ForceEvenSpritePosition).",
        ) { apply(s.copy(forceEvenSpritePosition = it)) }
        SettingsDivider()
        ToggleRow(
            "Unscaled Palette Draw",
            s.unscaledPaletteDraw,
            description = "Draws palette textures at native res to fix colour issues when upscaling.",
        ) { apply(s.copy(unscaledPaletteDraw = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "Texture Offset X",
            value = s.textureOffsetX.coerceIn(0, 1000),
            min = 0,
            max = 1000,
            description = "Horizontal texture-coordinate offset. 0 unless a game needs it.",
            onChange = { apply(s.copy(textureOffsetX = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Texture Offset Y",
            value = s.textureOffsetY.coerceIn(0, 1000),
            min = 0,
            max = 1000,
            description = "Vertical texture-coordinate offset. 0 unless a game needs it.",
            onChange = { apply(s.copy(textureOffsetY = it)) },
        )

        SectionHeader("Hardware Fixes")
        HelpText(
            "Manual renderer hacks. The master toggle auto-enables when any fix is " +
                "set. Leave these off unless fixing a specific visual issue.",
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            "Manual Hardware Fixes",
            s.manualUserHacks,
            description = "Force-enables the PCSX2 hardware-fix layer (UserHacks).",
        ) { apply(s.copy(manualUserHacks = it)) }
        SettingsDivider()
        SegmentedRow(
            label = "Auto Flush",
            options = listOf("Off", "Sprites", "On"),
            selectedIndex = s.autoFlush.coerceIn(0, 2),
            description = "Helps some sprite/alpha effects update correctly; can cost performance.",
            onChange = { apply(s.copy(autoFlush = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Texture Inside RT",
            options = listOf("Off", "Inside", "Merge"),
            selectedIndex = s.textureInsideRt.coerceIn(0, 2),
            description = "Helps effects that sample from render targets; can alter or slow rendering.",
            onChange = { apply(s.copy(textureInsideRt = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "GPU Target CLUT",
            options = listOf("Off", "Inside", "Forced"),
            selectedIndex = s.gpuTargetClut.coerceIn(0, 2),
            description = "Palette handling hack for games with broken colours or CLUT effects.",
            onChange = { apply(s.copy(gpuTargetClut = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "CPU Sprite BW",
            options = listOf("Off", "64", "128", "256"),
            selectedIndex = s.cpuSpriteRenderBw.coerceIn(0, 3),
            description = "CPU sprite-render bandwidth limit. Useful only for specific sprite glitches.",
            onChange = { apply(s.copy(cpuSpriteRenderBw = it)) },
        )
        SettingsDivider()
        SegmentedGridRow(
            label = "CPU Sprite Render",
            options = listOf("Off", "Sprite", "Triangle", "Aggressive", "Full", "Max"),
            selectedIndex = s.cpuSpriteRenderLevel.coerceIn(0, 5),
            columns = 3,
            description = "Renders selected sprite work on CPU to fix difficult hardware-renderer issues.",
            onChange = { apply(s.copy(cpuSpriteRenderLevel = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "CPU CLUT Render",
            options = listOf("Off", "Normal", "Aggr."),
            selectedIndex = s.cpuClutRender.coerceIn(0, 2),
            description = "Renders CLUTs on the CPU to fix palette/colour issues in some games.",
            onChange = { apply(s.copy(cpuClutRender = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Limit 24-Bit Depth",
            options = listOf("Off", "Upper", "Lower"),
            selectedIndex = s.limit24BitDepth.coerceIn(0, 2),
            description = "Depth-buffer hack that can reduce z-fighting in some hardware-rendered games.",
            onChange = { apply(s.copy(limit24BitDepth = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "GPU Palette Conversion",
            s.gpuPaletteConversion,
            description = "Does palette conversion on the GPU. Can help or hurt depending on the game.",
        ) { apply(s.copy(gpuPaletteConversion = it)) }
        SettingsDivider()
        ToggleRow(
            "CPU Framebuffer Conversion",
            s.cpuFramebufferConversion,
            description = "Converts framebuffer formats on the CPU to fix specific effects.",
        ) { apply(s.copy(cpuFramebufferConversion = it)) }
        SettingsDivider()
        ToggleRow(
            "Read Targets When Closing",
            s.readTargetsWhenClosing,
            description = "Flushes render targets back to memory when closing them.",
        ) { apply(s.copy(readTargetsWhenClosing = it)) }
        SettingsDivider()
        ToggleRow(
            "Preload Frame Data",
            s.preloadFrameData,
            description = "Uploads the previous frame's data before drawing. Fixes some effects.",
        ) { apply(s.copy(preloadFrameData = it)) }
        SettingsDivider()
        ToggleRow(
            "Estimate Texture Region",
            s.estimateTextureRegion,
            description = "Estimates the used texture region. Helps games that read odd regions.",
        ) { apply(s.copy(estimateTextureRegion = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Depth Emulation",
            s.disableDepthEmulation,
            description = "Disables depth emulation. Faster but breaks many games — last resort.",
        ) { apply(s.copy(disableDepthEmulation = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Partial Invalidation",
            s.disablePartialInvalidation,
            description = "Disables partial texture-cache source invalidation.",
        ) { apply(s.copy(disablePartialInvalidation = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Safe Features",
            s.disableSafeFeatures,
            description = "Turns off internal safe-feature workarounds. Advanced/diagnostic only.",
        ) { apply(s.copy(disableSafeFeatures = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Render Fixes",
            s.disableRenderFixes,
            description = "Disables automatic render fixes. Advanced/diagnostic only.",
        ) { apply(s.copy(disableRenderFixes = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "Skip Draw Start",
            value = s.skipDrawStart.coerceIn(0, 5000),
            min = 0,
            max = 5000,
            description = "First draw call to skip (UserHacks_SkipDraw). 0 = off. Advanced.",
            onChange = { apply(s.copy(skipDrawStart = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Skip Draw End",
            value = s.skipDrawEnd.coerceIn(0, 5000),
            min = 0,
            max = 5000,
            description = "Last draw call to skip. 0 = off. Advanced.",
            onChange = { apply(s.copy(skipDrawEnd = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "Spin GPU For Readbacks",
            s.spinGpuReadbacks,
            description = "Busy-waits the GPU on readbacks to reduce stalls. Can raise power use.",
        ) { apply(s.copy(spinGpuReadbacks = it)) }
        SettingsDivider()
        ToggleRow(
            "Spin CPU For Readbacks",
            s.spinCpuReadbacks,
            description = "Busy-waits the CPU on readbacks to reduce stalls. Can raise power use.",
        ) { apply(s.copy(spinCpuReadbacks = it)) }

        SectionHeader("Software Renderer")
        HelpText(
            "Apply when the Software renderer is selected.",
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            "Auto-Flush (SW)",
            s.autoFlushSw,
            description = "Software-renderer auto-flush. On by default for correctness.",
        ) { apply(s.copy(autoFlushSw = it)) }
        SettingsDivider()
        ToggleRow(
            "Mipmapping (SW)",
            s.mipmapSw,
            description = "Software-renderer mipmapping. On by default.",
        ) { apply(s.copy(mipmapSw = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "SW Rendering Threads",
            value = s.swThreads.coerceIn(0, 10),
            min = 0,
            max = 10,
            description = "Extra worker threads for the software renderer. 0 = single-threaded.",
            onChange = { apply(s.copy(swThreads = it)) },
        )
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun SectionHeader(title: String) {
    Spacer(Modifier.height(8.dp))
    Text(
        title,
        color = Colors.pasx2_blue,
        fontSize = 12.sp,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
    )
}
