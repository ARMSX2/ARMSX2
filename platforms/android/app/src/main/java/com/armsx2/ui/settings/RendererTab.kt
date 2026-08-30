package com.armsx2.ui.settings

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts.OpenDocument
import androidx.activity.result.contract.ActivityResultContracts.OpenDocumentTree
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.armsx2.config.Settings
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp
import java.io.BufferedInputStream
import java.io.File
import java.util.zip.ZipInputStream
import androidx.compose.runtime.saveable.rememberSaveable
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Renderer section of the in-game settings overlay.
 *
 * Most fields write into [Settings] via [InGameOverlay.saveSettings],
 * which honors the overlay's scope toggle (Global / Game). Upscale is
 * the one outlier — it has its own dedicated `MainActivityRuntime.upscale` state that's
 * also consumed by `MainActivityRuntime.applyRendererPrefs` and the setup wizard. Upscale
 * uses a narrow native GS helper so it can visibly apply while a game is live
 * without running the full settings commit path.
 */
internal data class UpscaleOption(val value: Float, val label: String)

// Shared so the in-game quick Graphics pane (EmulationMenuScreen) shows the exact same
// scale list — including the sub-native 0.25/0.5/0.75/Native options that its old
// hardcoded list omitted.
internal val UPSCALE_OPTIONS = listOf(
    // Sub-native (issue #207) — fewer pixels = big perf win on low/mid devices,
    // at the cost of sharpness. The GS only clamps the upper bound, so these are
    // applied as-is.
    UpscaleOption(0.25f, "0.25x"),
    UpscaleOption(0.5f, "0.5x"),
    UpscaleOption(0.75f, "0.75x"),
    UpscaleOption(1.0f, "Native"),
    UpscaleOption(1.25f, "1.25x"),
    UpscaleOption(1.5f, "1.5x"),
    UpscaleOption(1.75f, "1.75x"),
    UpscaleOption(2.0f, "2x"),
    UpscaleOption(2.25f, "2.25x"),
    UpscaleOption(2.5f, "2.5x"),
    UpscaleOption(2.75f, "2.75x"),
    UpscaleOption(3.0f, "3x"),
    UpscaleOption(3.5f, "3.5x"),
    UpscaleOption(4.0f, "4x"),
    UpscaleOption(5.0f, "5x"),
    UpscaleOption(6.0f, "6x"),
    UpscaleOption(7.0f, "7x"),
    UpscaleOption(8.0f, "8x"),
)

@Composable
fun RendererTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth(),
    ) {
        CollapsibleSection(str("renderer.section.displayResolution"), initiallyExpanded = true) {
            // Graphics API (OpenGL / Vulkan) + Vulkan custom-driver picker.
            // from the removed first-run setup renderer page into settings.
            RendererBackendSection(state)
            SettingsDivider()
            // Clear Shader Cache — directly under the GPU driver picker. Switching the Vulkan driver
            // is exactly when you want to wipe the on-disk shader cache so it recompiles on the new
            // one, so the control lives with the driver rather than buried lower in the tab.
            ClearShaderCacheRow()
            SettingsDivider()
            // GS Multi-threading (GV7 front/back split), placed right under the
            // renderer/driver picker. Off = today's single-threaded path (the
            // default — opt-in); On = the GS runs on a dedicated back thread
            // (Pipelined, enum value 3). The enum's Inline/Lockstep rungs (1/2)
            // are dev-only and deliberately not exposed. Restart-required (in
            // RestartOptionsAreEqual); native-lib snapshots the field across a
            // live apply, so it only takes effect on the next game boot.
            ToggleRow(
                str("renderer.gsBackThread.label"),
                s.gsBackThreadMode >= 3,
                description = str("renderer.gsBackThread.description"),
            ) {
                apply(s.copy(gsBackThreadMode = if (it) 3 else 0))
            }
            SettingsDivider()
            // A value that matches no preset is a CUSTOM one (set below, per-game, or from an INI).
            // It used to fall back to index 0, which displayed 0.25x while the GS ran something
            // else entirely — so the row lied about the active resolution.
            val presetIndex = UPSCALE_OPTIONS.indexOfFirst { abs(it.value - s.upscaleFloat) < 0.01f }
            val customIndex = UPSCALE_OPTIONS.size
            // Whether the Custom row is OPEN has to be its own state, not "the value matches no
            // preset". Picking Custom leaves the value untouched by design, so deriving it from the
            // value alone made the tap a no-op: the selection snapped straight back to the preset
            // and the slider never appeared. A value that matches no preset (per-game, or an INI)
            // still forces it open, so the row can never misreport what the GS is running.
            val customOpen = rememberSaveable { mutableStateOf(false) }
            val showCustom = customOpen.value || presetIndex < 0
            val upscaleIndex = if (showCustom) customIndex else presetIndex
            SegmentedGridRow(
                label = str("renderer.upscale.label"),
                options = UPSCALE_OPTIONS.map { it.label } + str("renderer.upscale.custom"),
                selectedIndex = upscaleIndex,
                columns = 4,
                description = str("renderer.upscale.description"),
                onChange = { index ->
                    if (index == customIndex) {
                        // Reveal the slider and keep the current value as its starting point —
                        // jumping to some arbitrary default would throw away what they had.
                        customOpen.value = true
                    } else {
                        customOpen.value = false
                        val mult = UPSCALE_OPTIONS[index].value
                        // Persist scope-aware (per-game when the overlay scope is Game);
                        // the live GS apply happens in InGameOverlay's settings delta.
                        if (abs(s.upscaleFloat - mult) >= 0.01f) apply(s.copy(upscaleFloat = mult))
                    }
                },
            )
            // Custom resolution scale, as a PERCENTAGE of native — the Dolphin-style numeric
            // control people ask for when a preset step is too coarse. The GS multiplier is a
            // float, so e.g. 107% turns 512x448 into a true 480p-height render without paying for
            // a full 2x. Only shown on Custom, so the preset grid stays uncluttered.
            if (upscaleIndex == customIndex) {
                IntSliderRow(
                    label = str("renderer.upscale.customScale"),
                    value = (s.upscaleFloat * 100f).roundToInt().coerceIn(25, 800),
                    min = 25,
                    max = 800,
                    description = str("renderer.upscale.customScale.description"),
                    valueFormatter = { "$it%" },
                    onReset = { apply(s.copy(upscaleFloat = 1.0f)) },
                    onChange = { pct -> apply(s.copy(upscaleFloat = pct / 100f)) },
                )
            }
            SettingsDivider()
            SegmentedRow(
                label = str("renderer.displayMode.label"),
                // Adding an option here means widening BOTH clamps: this one and
                // EmulationMenuViewModel.setAspectRatio. A clamp left at the old maximum does not
                // fail loudly -- it silently snaps the new choice back to the previous entry.
                options = listOf("Stretch", "Auto", "4:3", "16:9", "10:7", "21:9", "20:9", "19.5:9", "Custom"),
                selectedIndex = s.aspectRatio.coerceIn(0, 8),
                description = str("renderer.displayMode.description"),
                onChange = { apply(s.copy(aspectRatio = it)) },
            )
            // Only meaningful for Custom (8), so it stays hidden otherwise rather than sitting there
            // inert. Shown when EITHER the main aspect or the FMV override is Custom, since the FMV
            // path reads the same ratio.
            if (s.aspectRatio == 8 || s.fmvAspectRatio == 8) {
                IntSliderRow(
                    label = str("renderer.customAspect.label"),
                    // Presented in hundredths: the slider is integral, the setting is a float.
                    value = (s.customAspectRatio * 100f).toInt().coerceIn(50, 500),
                    min = 50,
                    max = 500,
                    description = str("renderer.customAspect.description"),
                    // Show the ratio itself plus its :9 equivalent, which is how phone panels are
                    // quoted — makes "I want 19.5:9" reachable without mental arithmetic.
                    valueFormatter = { hundredths ->
                        val r = hundredths / 100f
                        "%.2f  (%.1f:9)".format(r, r * 9f)
                    },
                    onChange = { apply(s.copy(customAspectRatio = it / 100f)) },
                )
            }
            SettingsDivider()
            // FMV Aspect Ratio override — applies only during FMVs/cutscenes; "Off" keeps
            // the aspect above. Handy for games that render FMVs at a different ratio.
            SegmentedRow(
                label = str("renderer.fmvAspect.label"),
                options = listOf("Off", "Auto", "4:3", "16:9", "10:7", "21:9", "20:9", "19.5:9", "Custom"),
                selectedIndex = s.fmvAspectRatio.coerceIn(0, 8),
                description = str("renderer.fmvAspect.description"),
                onChange = { apply(s.copy(fmvAspectRatio = it)) },
            )
            SettingsDivider()
            // Emulation Screen Orientation — Android activity orientation, now scope-aware
            // (global ∘ per-game) like the rest of this tab. applyEmulationOrientation resolves
            // the running game's value at boot and reverts to global on exit-to-library.
            SegmentedRow(
                label = str("renderer.orientation.label"),
                options = listOf(
                    str("renderer.orientation.device"),
                    str("renderer.orientation.landscape"),
                    str("renderer.orientation.portrait"),
                    str("renderer.orientation.autoRotate"),
                ),
                selectedIndex = s.orientation.coerceIn(0, 3),
                description = str("renderer.orientation.description"),
                onChange = {
                    apply(s.copy(orientation = it))
                    MainActivityRuntime.instance?.applyEmulationOrientation()
                },
            )
            SettingsDivider()
            // GitHub #375: where the render sits in a PORTRAIT window. Top (default) frees the
            // bottom half for touch controls; Center keeps the old vertical-centered behavior.
            // Live via NativeApp.setPortraitRenderTop (through applyTo); only affects portrait.
            SegmentedRow(
                label = str("renderer.portraitPosition.label"),
                options = listOf(str("renderer.portraitPosition.top"), str("renderer.portraitPosition.center")),
                selectedIndex = if (s.portraitRenderTop) 0 else 1,
                description = str("renderer.portraitPosition.description"),
                onChange = { apply(s.copy(portraitRenderTop = it == 0)) },
            )
            SettingsDivider()
            // Where the render sits in a LANDSCAPE window. Center is the default; Top suits
            // foldables and clamshell controllers, whose screens open downward so a centred
            // image reads as sitting too low. Live via NativeApp.setLandscapeRenderTop.
            SegmentedRow(
                label = str("renderer.landscapePosition.label"),
                options = listOf(str("renderer.landscapePosition.center"), str("renderer.landscapePosition.top")),
                selectedIndex = if (s.landscapeRenderTop) 1 else 0,
                description = str("renderer.landscapePosition.description"),
                onChange = { apply(s.copy(landscapeRenderTop = it == 1)) },
            )
            SettingsDivider()
            // Auto Progressive Scan — holds Triangle+Cross through boot, the combo some titles
            // probe to offer 480p progressive. Takes effect on the next boot (it is a boot-time
            // pad hold, not a live setting), and only does anything on games that implement it.
            ToggleRow(
                str("renderer.autoProgressive.label"),
                s.autoProgressiveScan,
                description = str("renderer.autoProgressive.description"),
            ) {
                apply(s.copy(autoProgressiveScan = it))
            }
            SettingsDivider()
            SegmentedGridRow(
                label = str("renderer.deinterlacing.label"),
                options = listOf(
                    "Auto", "Off", "Weave TFF", "Weave BFF", "Bob TFF",
                    "Bob BFF", "Blend TFF", "Blend BFF", "Adapt TFF", "Adapt BFF",
                ),
                selectedIndex = s.deinterlaceMode.coerceIn(0, 9),
                columns = 5,
                description = str("renderer.deinterlacing.description"),
                onChange = { apply(s.copy(deinterlaceMode = it)) },
            )
        }
        SettingsDivider()
        CollapsibleSection(str("renderer.section.texturesFiltering")) {
            SegmentedRow(
                label = str("renderer.textureFiltering.label"),
                options = listOf("Nearest", "Forced", "PS2", "Sprite"),
                selectedIndex = s.textureFiltering.coerceIn(0, 3),
                description = str("renderer.textureFiltering.description"),
                onChange = { apply(s.copy(textureFiltering = it)) },
            )
            SettingsDivider()
            SegmentedRow(
                label = str("renderer.texturePreloading.label"),
                options = listOf("Off", "Partial", "Full"),
                selectedIndex = s.texturePreloading.coerceIn(0, 2),
                description = str("renderer.texturePreloading.description"),
                onChange = { apply(s.copy(texturePreloading = it)) },
            )
            SettingsDivider()
            SegmentedGridRow(
                label = str("renderer.hardwareDownloadMode.label"),
                // Index == GSHardwareDownloadMode; "Async" is 5 and must stay last. Keep this list
                // and the clamp below in sync with the enum AND with the in-game menu's copy in
                // EmulationMenuScreen — there are two independent pickers for this setting.
                options = listOf("Accurate", "Force Full", "No Readbacks", "Unsync", "Disabled", "Async"),
                selectedIndex = s.hardwareDownloadMode.coerceIn(0, 5),
                columns = 3,
                description = str("renderer.hardwareDownloadMode.description"),
                onChange = { apply(s.copy(hardwareDownloadMode = it)) },
            )
        }
        SettingsDivider()
        CollapsibleSection(str("renderer.section.displayEffects")) {
            SegmentedRow(
                label = str("renderer.displayFilter.label"),
                options = listOf("Nearest", "Smooth", "Sharp"),
                selectedIndex = s.displayBilinear.coerceIn(0, 2),
                description = str("renderer.displayFilter.description"),
                onChange = { apply(s.copy(displayBilinear = it)) },
            )
            SettingsDivider()
            SegmentedGridRow(
                label = str("renderer.tvShader.label"),
                options = listOf("Off", "Scanline", "Diagonal", "Tri", "Wave", "Lottes", "4xRGSS", "NxAGSS"),
                selectedIndex = s.tvShader.coerceIn(0, 7),
                columns = 4,
                description = str("renderer.tvShader.description"),
                onChange = { apply(s.copy(tvShader = it)) },
            )
            SettingsDivider()
            ToggleRow(
                "VSync",
                s.vsyncEnable,
                description = str("renderer.vsync.description"),
            ) {
                apply(s.copy(vsyncEnable = it))
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.shadeboost.label"),
                s.shadeBoost,
                description = str("renderer.shadeboost.description"),
            ) {
                apply(s.copy(shadeBoost = it))
            }
            if (s.shadeBoost) {
                SettingsDivider()
                IntSliderRow(
                    label = str("renderer.brightness.label"),
                    value = s.shadeBoostBrightness.coerceIn(1, 100),
                    min = 1,
                    max = 100,
                    description = str("renderer.shadeboost.fiftyIsNormal"),
                    valueFormatter = { "$it%" },
                    onChange = { apply(s.copy(shadeBoostBrightness = it)) },
                )
                SettingsDivider()
                IntSliderRow(
                    label = str("renderer.contrast.label"),
                    value = s.shadeBoostContrast.coerceIn(1, 100),
                    min = 1,
                    max = 100,
                    description = str("renderer.shadeboost.fiftyIsNormal"),
                    valueFormatter = { "$it%" },
                    onChange = { apply(s.copy(shadeBoostContrast = it)) },
                )
                SettingsDivider()
                IntSliderRow(
                    label = str("renderer.saturation.label"),
                    value = s.shadeBoostSaturation.coerceIn(1, 100),
                    min = 1,
                    max = 100,
                    description = str("renderer.shadeboost.fiftyIsNormal"),
                    valueFormatter = { "$it%" },
                    onChange = { apply(s.copy(shadeBoostSaturation = it)) },
                )
                SettingsDivider()
                IntSliderRow(
                    label = str("renderer.gamma.label"),
                    value = s.shadeBoostGamma.coerceIn(1, 100),
                    min = 1,
                    max = 100,
                    description = str("renderer.shadeboost.fiftyIsNormal"),
                    valueFormatter = { "$it%" },
                    onChange = { apply(s.copy(shadeBoostGamma = it)) },
                )
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.fxaa.label"),
                s.fxaa,
                description = str("renderer.fxaa.description"),
            ) {
                apply(s.copy(fxaa = it))
            }
            SettingsDivider()
            // A picker rather than the on/off toggle this was: with SGSR there are three
            // mutually exclusive upscalers, and two toggles that silently turn each other off
            // is a worse way to say that than one list. The UI index is NOT the enum value --
            // MetalFX is 1 and is Apple-only, so it has no row here.
            val upscalerValues = listOf(
                Settings.UPSCALER_OFF, Settings.UPSCALER_FSR1,
                Settings.UPSCALER_SGSR, Settings.UPSCALER_SGSR_EDGE,
            )
            val sgsrOn = s.upscaler == Settings.UPSCALER_SGSR || s.upscaler == Settings.UPSCALER_SGSR_EDGE
            val upscalerOn = s.upscaler == Settings.UPSCALER_FSR1 || sgsrOn
            SegmentedRow(
                label = str("renderer.upscaler.label"),
                options = listOf(str("common.off"), "FSR 1", "SGSR", "SGSR Edge"),
                selectedIndex = upscalerValues.indexOf(s.upscaler).coerceAtLeast(0),
                onChange = { apply(s.copy(upscaler = upscalerValues[it])) },
            )
            // One slider per upscaler, not one shared. They are never both on screen, and the
            // ranges genuinely differ: FSR1's RCAS is natively 0..100, SGSR's edge sharpness is
            // 0..2 with 100 as Qualcomm's default. Sharing a field would redefine an existing FSR
            // configuration the moment SGSR was picked.
            if (upscalerOn) {
                SettingsDivider()
                if (sgsrOn) {
                    IntSliderRow(
                        label = str("renderer.sgsr.sharpness.label"),
                        value = s.sgsrSharpness.coerceIn(0, 200),
                        min = 0,
                        max = 200,
                        valueFormatter = { "$it%" },
                        onChange = { apply(s.copy(sgsrSharpness = it)) },
                    )
                } else {
                    IntSliderRow(
                        label = str("renderer.fsr1.sharpness.label"),
                        value = s.fsrSharpness.coerceIn(0, 100),
                        min = 0,
                        max = 100,
                        valueFormatter = { "$it%" },
                        onChange = { apply(s.copy(fsrSharpness = it)) },
                    )
                }
            }
            val fsr1On = upscalerOn
            // FSR's second pass IS RCAS, a contrast-adaptive sharpener, so the core runs one or
            // the other and never both. Showing CAS while FSR is on would offer a slider that
            // does nothing.
            if (!fsr1On) {
                SettingsDivider()
                SegmentedRow(
                    label = str("renderer.cas.label"),
                    options = listOf(str("fixes.opt.off"), str("renderer.cas.sharpen"), str("renderer.cas.sharpenResize")),
                    selectedIndex = s.casMode.coerceIn(0, 2),
                    description = str("renderer.cas.description"),
                    onChange = { apply(s.copy(casMode = it)) },
                )
                if (s.casMode != 0) {
                    SettingsDivider()
                    IntSliderRow(
                        label = str("renderer.cas.sharpness.label"),
                        value = s.casSharpness.coerceIn(0, 100),
                        min = 0,
                        max = 100,
                        valueFormatter = { "$it%" },
                        onChange = { apply(s.copy(casSharpness = it)) },
                    )
                }
            }
            SettingsDivider()
            // RetroArch (.slangp) chains run last in the post-process order (after
            // ShadeBoost/FXAA/CAS), so they close out Display Effects. Presentation-only
            // section — the tier wiring stays here in apply(), like every other row on
            // this tab, which is what gives it per-game override for free.
            // Lifted to ui/common so the in-game pause menu renders the SAME toggle +
            // picker (it passes its own save lambda). Tier wiring stays here in apply(),
            // like every other row on this tab — that's what gives it per-game override.
            com.armsx2.ui.common.ShaderChainSection(
                enabled = s.shaderChainEnabled,
                preset = s.shaderChainPreset,
                params = s.shaderChainParams,
                onEnabledChange = { apply(s.copy(shaderChainEnabled = it)) },
                onPresetChange = { apply(s.copy(shaderChainPreset = it)) },
                onParamsChange = { apply(s.copy(shaderChainParams = it)) },
            )
            SettingsDivider()
            // Where the presets above come from. Not a setting — it only puts files in
            // <dataroot>/shaders/, which is the folder ShaderChainSection's picker scans —
            // so it takes no tier and needs no apply(), same as DriverManagerSection.
            com.armsx2.ui.common.ShaderManagerSection()
        }
        SettingsDivider()
        // Its OWN section, not a row at the bottom of Display Effects: buried under the whole
        // shader manager inside a collapsed section, nobody could find it.
        CollapsibleSection(str("renderer.section.overlayArt")) {
            OverlayArtSection()
        }
        SettingsDivider()
        CollapsibleSection(str("renderer.section.texturePacks")) {
            ToggleRow(
                str("renderer.loadTexturePacks.label"),
                s.loadTextureReplacements,
                description = str("renderer.loadTexturePacks.description"),
            ) {
                apply(s.copy(loadTextureReplacements = it))
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.asyncTextureLoading.label"),
                s.loadTextureReplacementsAsync,
                description = str("renderer.asyncTextureLoading.description"),
            ) {
                apply(s.copy(loadTextureReplacementsAsync = it))
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.precacheTexturePacks.label"),
                s.precacheTextureReplacements,
                description = str("renderer.precacheTexturePacks.description"),
            ) {
                apply(s.copy(precacheTextureReplacements = it))
            }
            SettingsDivider()
            TexturePackImportRow()
            SettingsDivider()
            GsDumpCaptureRow()
            SettingsDivider()
            ToggleRow(
                str("renderer.dumpReplaceableTextures.label"),
                s.dumpReplaceableTextures,
                description = str("renderer.dumpReplaceableTextures.description"),
            ) {
                apply(s.copy(dumpReplaceableTextures = it))
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.texturePackOsd.label"),
                s.osdShowTextureReplacements,
                description = str("renderer.texturePackOsd.description"),
            ) {
                apply(s.copy(osdShowTextureReplacements = it))
            }
        }
        SettingsDivider()
        CollapsibleSection(str("renderer.section.blendingAdvanced")) {
            SegmentedRow(
                label = str("renderer.blendingAccuracy.label"),
                options = listOf("Min", "Basic", "Med", "High", "Full", "Max"),
                selectedIndex = s.accurateBlendingUnit.coerceIn(0, 5),
                description = str("renderer.blendingAccuracy.description"),
                onChange = { apply(s.copy(accurateBlendingUnit = it)) },
            )
            // Blending-accuracy companion features (match upstream's grouping under
            // Blending Accuracy). ROV + Accurate Alpha Test apply live; AA1 needs a
            // game restart.
            SettingsDivider()
            ToggleRow(
                str("renderer.rov.label"),
                s.hwRov,
                description = str("renderer.rov.description"),
            ) {
                apply(s.copy(hwRov = it))
            }
            SettingsDivider()
            // Every Android GPU is a tiler, so this is aimed at us even though it landed with
            // only a desktop UI. Default OFF because it is brand new, not because it is risky.
            ToggleRow(
                str("renderer.coalesceRenderPasses.label"),
                s.coalesceRenderPasses,
                description = str("renderer.coalesceRenderPasses.description"),
            ) {
                apply(s.copy(coalesceRenderPasses = it))
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.accurateBlendingFastPath.label"),
                s.adrenoFbFetch,
                description = str("renderer.accurateBlendingFastPath.description"),
            ) {
                apply(s.copy(adrenoFbFetch = it))
            }
            SettingsDivider()
            // MediaTek Mali / Mali-G57 escape hatch: those drivers are force-excluded from
            // the fbfetch path natively, which costs a per-primitive texture barrier on a
            // GPU with no dual-source blend. Default OFF — on is a test, not a fix.
            ToggleRow(
                str("renderer.forceMaliFbFetch.label"),
                s.forceMaliFbFetch,
                description = str("renderer.forceMaliFbFetch.description"),
            ) {
                apply(s.copy(forceMaliFbFetch = it))
            }
            SettingsDivider()
            // ANGLE for the OpenGL renderer now lives in the graphics-API driver picker
            // (AngleDriverSection), shown when OpenGL is selected — see RendererBackendSection.
            ToggleRow(
                str("renderer.accurateAlphaTest.label"),
                s.hwAccurateAlphaTest,
                description = str("renderer.accurateAlphaTest.description"),
            ) {
                apply(s.copy(hwAccurateAlphaTest = it))
            }
            SettingsDivider()
            ToggleRow(
                str("renderer.hwAa1.label"),
                s.hwAa1,
                description = str("renderer.hwAa1.description"),
            ) {
                apply(s.copy(hwAa1 = it))
            }
            // Hardware & upscaling compatibility fixes now live in the dedicated
            // "Fixes" tab (FixesTab) to keep Render focused on quality/display.
            SettingsDivider()
            ToggleRow(
                str("renderer.hwMipmapping.label"),
                s.hwMipmap,
                description = str("renderer.hwMipmapping.description"),
            ) {
                apply(s.copy(hwMipmap = it))
            }
            SettingsDivider()
            // TriFilter is signed (-1 = Auto). Map enum range onto 0..3.
            val triLabels = listOf("Auto", "Off", "PS2", "Forced")
            val triIdx = (s.triFilter + 1).coerceIn(0, 3)
            SegmentedRow(
                label = str("renderer.trilinear.label"),
                options = triLabels,
                selectedIndex = triIdx,
                description = str("renderer.trilinear.description"),
                onChange = { apply(s.copy(triFilter = it - 1)) },
            )
            SettingsDivider()
            val anisoLabels = listOf("Off", "2x", "4x", "8x", "16x")
            val anisoVals = listOf(0, 2, 4, 8, 16)
            val anisoIdx = anisoVals.indexOf(s.maxAnisotropy).coerceAtLeast(0)
            SegmentedRow(
                label = str("renderer.anisotropic.label"),
                options = anisoLabels,
                selectedIndex = anisoIdx,
                description = str("renderer.anisotropic.description"),
                onChange = { apply(s.copy(maxAnisotropy = anisoVals[it])) },
            )
            SettingsDivider()
            // GPU profile override. Auto resolves at device init via
            // GpuProfileDetector::Resolve (vendor strings + Android ro.soc.*
            // hints). Mali uses ARM_shader_framebuffer_fetch over texture
            // barriers; Adreno uses the EXT fetch / generic path; PowerVR
            // (Imagination) uses EXT/PLS like Adreno but is its own tile-based
            // GPU family; Xclipse (Samsung Exynos, AMD-RDNA2) forces framebuffer
            // fetch OFF on Vulkan (its ROAA path is broken) — pick it if the auto
            // 0x144D vendor guess doesn't fire on your driver. Marginal otherwise.
            // Changing requires a renderer restart — CheckFeatures
            // runs once at device init, so we kick MainActivityRuntime.restart() the same way
            // RestartButton does.
            SegmentedRow(
                label = str("renderer.gpuProfile.label"),
                options = listOf("Auto", "Mali", "Adreno", "PowerVR", "Xclipse"),
                selectedIndex = s.gpuProfile.coerceIn(0, 4),
                description = str("renderer.gpuProfile.description"),
                onChange = {
                    apply(s.copy(gpuProfile = it))
                },
            )
        }
    }
}

@Composable
private fun TexturePackImportRow() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status = remember { mutableStateOf("") }

    // Shared handler: both the folder and .zip pickers need a booted game (for the
    // serial) and run the copy off the UI thread, reporting the file count the same way.
    fun runImport(doCopy: (String) -> Int) {
        val serial = activeTextureSerial()
        if (serial == null) {
            Toast.makeText(context, I18n.get("renderer.import.bootGameFirst"), Toast.LENGTH_LONG).show()
            return
        }
        scope.launch(Dispatchers.IO) {
            val copied = runCatching { doCopy(serial) }.getOrDefault(-1)
            withContext(Dispatchers.Main) {
                val msg = if (copied >= 0)
                    "Imported $copied texture files for $serial."
                else
                    I18n.get("renderer.import.failed")
                status.value = msg
                Toast.makeText(context, msg, Toast.LENGTH_LONG).show()
            }
        }
    }
    val folderLauncher = rememberLauncherForActivityResult(OpenDocumentTree()) { uri: Uri? ->
        if (uri != null) runImport { s -> importTexturePack(context, uri, s) }
    }
    val zipLauncher = rememberLauncherForActivityResult(OpenDocument()) { uri: Uri? ->
        if (uri != null) runImport { s -> importTexturePackZip(context, uri, s) }
    }

    Column(Modifier.fillMaxWidth()) {
        // Folder import (the pack's folder, with or without a nested "replacements/").
        Box(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(16.dp))
                .background(rowAura())
                .clickable { folderLauncher.launch(null) }
                .padding(horizontal = 6.dp, vertical = 5.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Column {
                Text(
                    str("renderer.importTexturePack.label"),
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    status.value.ifEmpty {
                        activeTextureSerial()?.let { "Copies into textures/$it/replacements" }
                            ?: I18n.get("renderer.importTexturePack.bootFirst")
                    },
                    color = Colors.pasx2_blue,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
        // .zip import — extracts the archive into the same per-game replacements folder.
        Box(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(16.dp))
                .background(rowAura())
                .clickable {
                    zipLauncher.launch(
                        arrayOf(
                            "application/zip", "application/x-zip-compressed",
                            "application/octet-stream", "*/*",
                        ),
                    )
                }
                .padding(horizontal = 6.dp, vertical = 5.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text(
                str("renderer.importTexturePackZip.label"),
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
        }
    }
}

/**
 * RetroArch overlay artwork (bezel / border): import a pack, pick one, set its opacity.
 *
 * Separate from the shader chain on purpose — an RA overlay is just an image, so it composites
 * for free and can be layered WITH a shader preset, which is what was asked for. Only the image
 * half of the .cfg is used; ARMSX2 has its own touch layout, so the format's input hitboxes are
 * deliberately ignored rather than fighting it.
 */
@Composable
private fun OverlayArtSection() {
    val context = LocalContext.current
    val refresh = remember { mutableStateOf(0) }
    val entries = remember(refresh.value) { com.armsx2.OverlayRepo.list(context) }
    // Result of the last import, so it can never be silent — a .cfg whose image didn't come with
    // it used to look identical to a successful import (nothing appeared, nothing was said).
    val status = remember { mutableStateOf("") }
    // MULTI-select, so a .cfg CAN be imported as a file: pick the cfg and its image(s) together in
    // one go. A single-document pick can't reach the cfg's siblings, which is why importing a lone
    // cfg silently produced nothing — but the fix for that is letting you select them both, not
    // forcing everyone to use a folder picker.
    val importer = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenMultipleDocuments(),
    ) { uris ->
        if (!uris.isNullOrEmpty()) {
            var total = 0
            var sawCfg = false
            var sawImage = false
            uris.forEach { uri ->
                val name = runCatching {
                    context.contentResolver.query(uri, null, null, null, null)?.use { c ->
                        val i = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                        if (i >= 0 && c.moveToFirst()) c.getString(i) else null
                    }
                }.getOrNull()
                if (name?.endsWith(".cfg", true) == true) sawCfg = true
                if (name?.substringAfterLast('.', "")?.lowercase() in setOf("png", "jpg", "jpeg", "webp")) sawImage = true
                total += com.armsx2.OverlayRepo.importFrom(context, uri, name)
            }
            refresh.value++
            status.value = when {
                total <= 0 -> I18n.get("renderer.overlayArt.importFailed")
                // A cfg with no artwork alongside it still can't resolve — say so rather than
                // leaving the list looking unchanged.
                sawCfg && !sawImage -> I18n.get("renderer.overlayArt.importCfgAlone")
                else -> I18n.get("renderer.overlayArt.imported").format(total)
            }
        }
    }
    val folderImporter = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenDocumentTree(),
    ) { uri ->
        if (uri != null) {
            val n = com.armsx2.OverlayRepo.importTree(context, uri)
            refresh.value++
            status.value = if (n > 0) I18n.get("renderer.overlayArt.imported").format(n)
            else I18n.get("renderer.overlayArt.importFailed")
        }
    }

    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .controllerFocusable("renderer.overlayArt.import", RoundedCornerShape(16.dp),
                onConfirm = { importer.launch(arrayOf("application/zip", "image/*", "*/*")) })
            .clickable { importer.launch(arrayOf("application/zip", "image/*", "*/*")) }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(
            str("renderer.overlayArt.import"),
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
    // ---- Download instead of import ----------------------------------------------------------
    // Importing by hand is genuinely fiddly (a .cfg is useless without the image it references),
    // so the primary path is now a browse-and-tap list from libretro's own overlay collection.
    // The file/folder importers stay for people bringing their own packs.
    val scope = rememberCoroutineScope()
    val catalog = remember { mutableStateOf<List<com.armsx2.OverlayRepo.CatalogEntry>>(emptyList()) }
    val catalogBusy = remember { mutableStateOf(false) }
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .controllerFocusable("renderer.overlayArt.browse", RoundedCornerShape(16.dp), onConfirm = {
                if (!catalogBusy.value) {
                    catalogBusy.value = true
                    scope.launch {
                        val list = withContext(Dispatchers.IO) { com.armsx2.OverlayRepo.fetchCatalog() }
                        catalog.value = list
                        catalogBusy.value = false
                        if (list.isEmpty()) status.value = I18n.get("renderer.overlayArt.browseFailed")
                    }
                }
            })
            .clickable {
                if (!catalogBusy.value) {
                    catalogBusy.value = true
                    scope.launch {
                        val list = withContext(Dispatchers.IO) { com.armsx2.OverlayRepo.fetchCatalog() }
                        catalog.value = list
                        catalogBusy.value = false
                        if (list.isEmpty()) status.value = I18n.get("renderer.overlayArt.browseFailed")
                    }
                }
            }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(
            if (catalogBusy.value) str("renderer.overlayArt.browsing") else str("renderer.overlayArt.browse"),
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
    catalog.value.forEach { entry ->
        val download = {
            scope.launch {
                status.value = I18n.get("renderer.overlayArt.downloading").format(entry.name)
                val n = withContext(Dispatchers.IO) {
                    com.armsx2.OverlayRepo.downloadFromCatalog(context, entry)
                }
                refresh.value++
                status.value = if (n > 0) I18n.get("renderer.overlayArt.downloaded").format(entry.name)
                else I18n.get("renderer.overlayArt.importFailed")
            }
            Unit
        }
        Box(
            Modifier
                .fillMaxWidth()
                .padding(start = 14.dp)
                .clip(RoundedCornerShape(12.dp))
                .controllerFocusable("renderer.overlayArt.dl.${entry.path}", RoundedCornerShape(12.dp), onConfirm = download)
                .clickable { download() }
                .padding(horizontal = 8.dp, vertical = 7.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text("⤓  ${entry.name}", color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 14.sp)
        }
    }
    // Folder import — the one that works for a RetroArch .cfg, because only a tree URI can bring
    // the artwork the cfg points at along with it.
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .controllerFocusable("renderer.overlayArt.importFolder", RoundedCornerShape(16.dp),
                onConfirm = { folderImporter.launch(null) })
            .clickable { folderImporter.launch(null) }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(
            str("renderer.overlayArt.importFolder"),
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
    if (status.value.isNotBlank()) {
        Text(
            status.value,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 13.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 4.dp),
        )
    }
    SettingsDivider()
    // "None" first so turning it off is always one tap away, even with a long pack list.
    val options = listOf(str("renderer.overlayArt.none")) + entries.map { it.name }
    val selected = entries.indexOfFirst { it.imagePath == com.armsx2.OverlayRepo.activePath.value }
        .let { if (it >= 0) it + 1 else 0 }
    SegmentedGridRow(
        label = str("renderer.overlayArt.label"),
        options = options,
        selectedIndex = selected,
        columns = 2,
        description = str("renderer.overlayArt.description"),
        onChange = { idx ->
            com.armsx2.OverlayRepo.setActive(
                if (idx == 0) "" else entries.getOrNull(idx - 1)?.imagePath.orEmpty(),
            )
        },
    )
    if (com.armsx2.OverlayRepo.activePath.value.isNotBlank()) {
        SettingsDivider()
        IntSliderRow(
            label = str("renderer.overlayArt.opacity"),
            value = (com.armsx2.OverlayRepo.opacity.floatValue * 100f).roundToInt(),
            min = 5,
            max = 100,
            description = str("renderer.overlayArt.opacity.description"),
            valueFormatter = { "$it%" },
            onReset = { com.armsx2.OverlayRepo.setOpacity(1f) },
            onChange = { com.armsx2.OverlayRepo.setOpacity(it / 100f) },
        )
    }
}

@Composable
private fun ClearShaderCacheRow() {
    val context = LocalContext.current
    val status = remember { mutableStateOf("") }
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable {
                val n = clearShaderCache(File(MainActivityRuntime.assetCopyRoot(context), "cache"))
                status.value = if (n > 0)
                    "Cleared $n shader-cache file${if (n == 1) "" else "s"} — restart the game to rebuild."
                else
                    I18n.get("renderer.clearShaderCache.alreadyEmpty")
                Toast.makeText(context, status.value, Toast.LENGTH_SHORT).show()
            }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(
                str("renderer.clearShaderCache.label"),
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                status.value.ifEmpty {
                    I18n.get("renderer.clearShaderCache.description")
                },
                color = Colors.pasx2_blue,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

/** Delete the on-disk compiled shader/pipeline caches (Vulkan + GL). They rebuild
 *  on the next renderer init; a stale/mismatched cache (e.g. after a driver change)
 *  can otherwise leave a game rendering corrupt. Returns how many files were removed. */
private fun clearShaderCache(cacheDir: File): Int {
    val names = listOf(
        "vulkan_pipelines.bin", "vulkan_shaders.bin", "vulkan_shaders.idx",
        "gl_programs.bin", "gl_programs.idx",
    )
    var removed = 0
    for (name in names) {
        val f = File(cacheDir, name)
        if (f.isFile && runCatching { f.delete() }.getOrDefault(false)) removed++
    }
    return removed
}

@Composable
private fun GsDumpCaptureRow() {
    val context = LocalContext.current
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable {
                if (MainActivityRuntime.eState.value == com.armsx2.EmuState.STOPPED) {
                    Toast.makeText(context, I18n.get("renderer.gsDump.startGameFirst"), Toast.LENGTH_LONG).show()
                } else {
                    runCatching { NativeApp.captureGsDump(1) }
                    Toast.makeText(context, I18n.get("renderer.gsDump.queued"), Toast.LENGTH_LONG).show()
                }
            }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(
                str("renderer.gsDump.label"),
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                str("renderer.gsDump.description"),
                color = Colors.pasx2_blue,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

private fun activeTextureSerial(): String? {
    return MainActivityRuntime.currentGame.value?.serial?.takeIf { it.isNotBlank() }
        ?: runCatching { NativeApp.getGameSerial() }.getOrNull()?.takeIf { it.isNotBlank() }
        // Last resort: the game the user most recently had open. Both sources above go blank the
        // moment you quit to the library (currentGame is nulled so per-game settings scope can't
        // leak, and the VM's serial dies with the VM), which stranded texture-pack import behind
        // "Boot a game first" even though the user had just played — and quit — that game.
        ?: MainActivityRuntime.contextGame.value?.serial?.takeIf { it.isNotBlank() }
}

private fun importTexturePack(context: Context, uri: Uri, serial: String): Int {
    val root = DocumentFile.fromTreeUri(context, uri) ?: return -1
    val source = root.findFile("replacements")?.takeIf { it.isDirectory } ?: root
    val dest = File(MainActivityRuntime.assetCopyRoot(context), "textures/$serial/replacements")
    if (!dest.exists() && !dest.mkdirs())
        return -1
    return copyDocumentTree(context, source, dest)
}

/** Extract a picked .zip of replacement textures into textures/<serial>/replacements,
 *  preserving the archive's internal folder structure. The native loader scans that
 *  directory RECURSIVELY and matches by hash filename, so a pack that nests its files
 *  (e.g. under its own "replacements/" or a pack-name folder) still resolves. Guards
 *  against Zip-Slip path traversal by rejecting any entry that escapes the dest dir. */
private fun importTexturePackZip(context: Context, zipUri: Uri, serial: String): Int {
    val dest = File(MainActivityRuntime.assetCopyRoot(context), "textures/$serial/replacements")
    if (!dest.exists() && !dest.mkdirs())
        return -1
    val destCanon = dest.canonicalPath
    var copied = 0
    context.contentResolver.openInputStream(zipUri)?.use { raw ->
        ZipInputStream(BufferedInputStream(raw)).use { zin ->
            while (true) {
                val e = zin.nextEntry ?: break
                if (!e.isDirectory) {
                    val rel = e.name.replace('\\', '/').trimStart('/')
                    val out = File(dest, rel)
                    // Zip-Slip guard: the resolved path must stay inside dest.
                    val outCanon = out.canonicalPath
                    if (outCanon == destCanon || outCanon.startsWith(destCanon + File.separator)) {
                        out.parentFile?.mkdirs()
                        // Per-entry guard: a mid-copy failure deletes that partial file
                        // and lets the remaining entries still import.
                        val ok = runCatching { out.outputStream().use { zin.copyTo(it) } }.isSuccess
                        if (ok) copied++ else out.delete()
                    }
                }
                zin.closeEntry()
            }
        }
    } ?: return -1
    return copied
}

private fun copyDocumentTree(context: Context, source: DocumentFile, dest: File): Int {
    var copied = 0
    for (child in source.listFiles()) {
        val name = child.name ?: continue
        if (child.isDirectory) {
            val childDest = File(dest, name)
            if (!childDest.exists())
                childDest.mkdirs()
            copied += copyDocumentTree(context, child, childDest)
        } else if (child.isFile) {
            context.contentResolver.openInputStream(child.uri)?.use { input ->
                File(dest, name).outputStream().use { output ->
                    input.copyTo(output)
                }
            } ?: continue
            copied++
        }
    }
    return copied
}
