package com.armsx2.ui.settings

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts.OpenDocumentTree
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.armsx2.Main
import com.armsx2.config.LiveGsApplyQueue
import com.armsx2.config.Settings
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import kr.co.iefriends.pcsx2.NativeApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.abs

/**
 * Renderer section of the in-game settings overlay.
 *
 * Most fields write into [Settings] via [InGameOverlay.saveSettings],
 * which honors the overlay's scope toggle (Global / Game). Upscale is
 * the one outlier — it has its own dedicated `Main.upscale` state that's
 * also consumed by `Main.applyRendererPrefs` and the setup wizard. Upscale
 * uses a narrow native GS helper so it can visibly apply while a game is live
 * without running the full settings commit path.
 */
private data class UpscaleOption(val value: Float, val label: String)

private val UPSCALE_OPTIONS = listOf(
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
)

@Composable
fun RendererTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        // Graphics API (OpenGL / Vulkan) + Vulkan custom-driver picker. Ported
        // from the removed first-run setup renderer page into settings.
        RendererBackendSection()
        SettingsDivider()
        val upscaleIndex = UPSCALE_OPTIONS
            .indexOfFirst { abs(it.value - Main.upscale.value) < 0.01f }
            .takeIf { it >= 0 } ?: 0
        SegmentedGridRow(
            label = "Upscale",
            options = UPSCALE_OPTIONS.map { it.label },
            selectedIndex = upscaleIndex,
            columns = 4,
            description = "Internal resolution. Higher values are sharper but can expose game-specific bloom or alignment artifacts.",
            onChange = { index ->
                val mult = UPSCALE_OPTIONS[index].value
                if (abs(Main.upscale.value - mult) >= 0.01f) {
                    Main.upscale.value = mult
                    Main.prefs.edit().putFloat("upscaleFloat", mult).apply()
                    LiveGsApplyQueue.applyUpscale(mult)
                }
            },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Display Mode",
            options = listOf("Stretch", "Auto", "4:3", "16:9", "10:7"),
            selectedIndex = s.aspectRatio.coerceIn(0, 4),
            description = "Controls how the PS2 image fits the screen.",
            onChange = { apply(s.copy(aspectRatio = it)) },
        )
        SettingsDivider()
        SegmentedGridRow(
            label = "Deinterlacing",
            options = listOf(
                "Auto", "Off", "Weave TFF", "Weave BFF", "Bob TFF",
                "Bob BFF", "Blend TFF", "Blend BFF", "Adapt TFF", "Adapt BFF",
            ),
            selectedIndex = s.deinterlaceMode.coerceIn(0, 9),
            columns = 5,
            description = "Changes how interlaced video is displayed. Auto is safest.",
            onChange = { apply(s.copy(deinterlaceMode = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Texture Filtering",
            options = listOf("Nearest", "Forced", "PS2", "Sprite"),
            selectedIndex = s.textureFiltering.coerceIn(0, 3),
            description = "Controls texture smoothing. PS2 is safest; Forced can soften or brighten some games.",
            onChange = { apply(s.copy(textureFiltering = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Texture Preloading",
            options = listOf("Off", "Partial", "Full"),
            selectedIndex = s.texturePreloading.coerceIn(0, 2),
            description = "Preloads textures to avoid missing or late texture uploads. Full is the safe default.",
            onChange = { apply(s.copy(texturePreloading = it)) },
        )
        SettingsDivider()
        SegmentedGridRow(
            label = "Hardware Download Mode",
            options = listOf("Accurate", "Force Full", "No Readbacks", "Unsync", "Disabled"),
            selectedIndex = s.hardwareDownloadMode.coerceIn(0, 4),
            columns = 3,
            description = "Readback accuracy for effects that need GPU data. Faster modes may break effects.",
            onChange = { apply(s.copy(hardwareDownloadMode = it)) },
        )
        SettingsDivider()
        SegmentedGridRow(
            label = "CRT / TV Shader",
            options = listOf("Off", "Scanline", "Diagonal", "Tri", "Wave", "Lottes", "4xRGSS", "NxAGSS"),
            selectedIndex = s.tvShader.coerceIn(0, 7),
            columns = 4,
            description = "Post-process CRT/TV filters. Applies live on supported renderers.",
            onChange = { apply(s.copy(tvShader = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "VSync",
            s.vsyncEnable,
            description = "Sync presentation to the display refresh — less tearing/smoother, slightly more latency. Restart the game to apply.",
        ) {
            apply(s.copy(vsyncEnable = it))
        }
        SettingsDivider()
        ToggleRow(
            "Shadeboost",
            s.shadeBoost,
            description = "Post-process colour controls for brightness, contrast, saturation, and gamma.",
        ) {
            apply(s.copy(shadeBoost = it))
        }
        if (s.shadeBoost) {
            SettingsDivider()
            IntSliderRow(
                label = "Brightness",
                value = s.shadeBoostBrightness.coerceIn(1, 100),
                min = 1,
                max = 100,
                description = "50 is normal.",
                valueFormatter = { "$it%" },
                onChange = { apply(s.copy(shadeBoostBrightness = it)) },
            )
            SettingsDivider()
            IntSliderRow(
                label = "Contrast",
                value = s.shadeBoostContrast.coerceIn(1, 100),
                min = 1,
                max = 100,
                description = "50 is normal.",
                valueFormatter = { "$it%" },
                onChange = { apply(s.copy(shadeBoostContrast = it)) },
            )
            SettingsDivider()
            IntSliderRow(
                label = "Saturation",
                value = s.shadeBoostSaturation.coerceIn(1, 100),
                min = 1,
                max = 100,
                description = "50 is normal.",
                valueFormatter = { "$it%" },
                onChange = { apply(s.copy(shadeBoostSaturation = it)) },
            )
            SettingsDivider()
            IntSliderRow(
                label = "Gamma",
                value = s.shadeBoostGamma.coerceIn(1, 100),
                min = 1,
                max = 100,
                description = "50 is normal.",
                valueFormatter = { "$it%" },
                onChange = { apply(s.copy(shadeBoostGamma = it)) },
            )
        }
        SettingsDivider()
        ToggleRow(
            "Load Texture Packs",
            s.loadTextureReplacements,
            description = "Loads replacement textures from the active game's texture folder.",
        ) {
            apply(s.copy(loadTextureReplacements = it))
        }
        SettingsDivider()
        ToggleRow(
            "Async Texture Loading",
            s.loadTextureReplacementsAsync,
            description = "Loads replacements in the background to reduce stalls.",
        ) {
            apply(s.copy(loadTextureReplacementsAsync = it))
        }
        SettingsDivider()
        ToggleRow(
            "Precache Texture Packs",
            s.precacheTextureReplacements,
            description = "Scans replacements at boot. Slower startup, fewer in-game hitches.",
        ) {
            apply(s.copy(precacheTextureReplacements = it))
        }
        SettingsDivider()
        TexturePackImportRow()
        SettingsDivider()
        ToggleRow(
            "Dump Replaceable Textures",
            s.dumpReplaceableTextures,
            description = "Writes textures used by the game to disk for pack creation.",
        ) {
            apply(s.copy(dumpReplaceableTextures = it))
        }
        SettingsDivider()
        ToggleRow(
            "Texture Pack OSD",
            s.osdShowTextureReplacements,
            description = "Shows texture replacement status messages in-game.",
        ) {
            apply(s.copy(osdShowTextureReplacements = it))
        }
        SettingsDivider()
        SegmentedRow(
            label = "Blending Accuracy",
            options = listOf("Min", "Basic", "Med", "High", "Full", "Max"),
            selectedIndex = s.accurateBlendingUnit.coerceIn(0, 5),
            description = "Controls alpha/blending precision. Basic is faster; higher can fix effects.",
            onChange = { apply(s.copy(accurateBlendingUnit = it)) },
        )
        // Hardware & upscaling compatibility fixes now live in the dedicated
        // "Fixes" tab (FixesTab) to keep Render focused on quality/display.
        SettingsDivider()
        ToggleRow(
            "HW Mipmapping",
            s.hwMipmap,
            description = "Uses mipmaps in hardware renderers. Can fix texture shimmer or broken effects.",
        ) {
            apply(s.copy(hwMipmap = it))
        }
        SettingsDivider()
        // TriFilter is signed (-1 = Auto). Map enum range onto 0..3.
        val triLabels = listOf("Auto", "Off", "PS2", "Forced")
        val triIdx = (s.triFilter + 1).coerceIn(0, 3)
        SegmentedRow(
            label = "Trilinear",
            options = triLabels,
            selectedIndex = triIdx,
            description = "Mip texture filtering. Auto is safest for compatibility.",
            onChange = { apply(s.copy(triFilter = it - 1)) },
        )
        SettingsDivider()
        val anisoLabels = listOf("Off", "2x", "4x", "8x", "16x")
        val anisoVals = listOf(0, 2, 4, 8, 16)
        val anisoIdx = anisoVals.indexOf(s.maxAnisotropy).coerceAtLeast(0)
        SegmentedRow(
            label = "Anisotropic",
            options = anisoLabels,
            selectedIndex = anisoIdx,
            description = "Sharpens angled textures. Higher values can cost GPU time.",
            onChange = { apply(s.copy(maxAnisotropy = anisoVals[it])) },
        )
        SettingsDivider()
        // GPU profile override. Auto resolves at device init via
        // GpuProfileDetector::Resolve (vendor strings + Android ro.soc.*
        // hints). Mali uses ARM_shader_framebuffer_fetch over texture
        // barriers; Adreno uses the EXT fetch / generic path; PowerVR
        // (Imagination) uses EXT/PLS like Adreno but is its own tile-based
        // GPU family. Changing requires a renderer restart — CheckFeatures
        // runs once at device init, so we kick Main.restart() the same way
        // RestartButton does.
        SegmentedRow(
            label = "GPU Profile",
            options = listOf("Auto", "Mali", "Adreno", "PowerVR"),
            selectedIndex = s.gpuProfile.coerceIn(0, 3),
            description = "Overrides Android GPU workaround selection. Auto is recommended.",
            onChange = {
                apply(s.copy(gpuProfile = it))
            },
        )
    }
}

@Composable
private fun TexturePackImportRow() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status = remember { mutableStateOf("") }
    val launcher = rememberLauncherForActivityResult(OpenDocumentTree()) { uri: Uri? ->
        val serial = activeTextureSerial()
        if (uri == null) {
            return@rememberLauncherForActivityResult
        } else if (serial == null) {
            Toast.makeText(context, "Boot a game before importing its texture pack.", Toast.LENGTH_LONG).show()
        } else {
            scope.launch(Dispatchers.IO) {
                val copied = runCatching { importTexturePack(context, uri, serial) }.getOrDefault(-1)
                withContext(Dispatchers.Main) {
                    val msg = if (copied >= 0)
                        "Imported $copied texture files for $serial."
                    else
                        "Texture pack import failed."
                    status.value = msg
                    Toast.makeText(context, msg, Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .clickable { launcher.launch(null) }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(
                "Import Texture Pack",
                color = Color.White,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                status.value.ifEmpty {
                    activeTextureSerial()?.let { "Copies into textures/$it/replacements" }
                        ?: "Boot a game first so ARMSX2 knows the serial."
                },
                color = Colors.pasx2_blue,
                fontSize = 10.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

private fun activeTextureSerial(): String? {
    return Main.currentGame.value?.serial?.takeIf { it.isNotBlank() }
        ?: runCatching { NativeApp.getGameSerial() }.getOrNull()?.takeIf { it.isNotBlank() }
}

private fun importTexturePack(context: Context, uri: Uri, serial: String): Int {
    val root = DocumentFile.fromTreeUri(context, uri) ?: return -1
    val source = root.findFile("replacements")?.takeIf { it.isDirectory } ?: root
    val dest = File(Main.assetCopyRoot(context), "textures/$serial/replacements")
    if (!dest.exists() && !dest.mkdirs())
        return -1
    return copyDocumentTree(context, source, dest)
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
