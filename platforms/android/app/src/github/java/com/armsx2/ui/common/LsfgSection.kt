package com.armsx2.ui.common

import android.content.Context
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.BuildConfig
import com.armsx2.i18n.str
import com.armsx2.ui.settings.IntSliderRow
import com.armsx2.ui.settings.SegmentedRow
import com.armsx2.ui.settings.SettingsDivider
import com.armsx2.ui.settings.ToggleRow
import com.armsx2.ui.settings.controllerFocusable
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

/**
 * LSFG frame-generation rows: a master toggle, a multiplier, the shader family, the motion-detail
 * slider, and the Lossless.dll picker.
 *
 * Presentation only — the caller wires [onChange] to its OWN settings tier, exactly like
 * [ShaderChainSection]. That is what lets one definition serve both the All Settings
 * performance tab (which honours the Global / Game scope) and the in-game pause menu
 * (which routes through InGameOverlay.saveSettings). Do NOT reach for a settings tier from
 * in here.
 *
 * ## Nothing proprietary ships with ARMSX2
 *
 * The interpolation shaders are read at runtime out of the user's own legitimately
 * purchased Lossless.dll, supplied through the Storage Access Framework exactly as a PS2
 * BIOS is. Absent that file the feature stays off, and the dialog below says so before
 * anything is enabled rather than after it silently fails.
 *
 * ## Github flavour only
 *
 * This file lives in the github source set; the play one has a stub of the same name, so the Play
 * build does not CONTAIN it. That is a stronger claim than the [BuildConfig.LSFG] check below,
 * which stops the rows being drawn and does nothing about the code or its strings shipping. The
 * guard is kept anyway so a future caller that forgets cannot turn it on.
 *
 * [BuildConfig.LSFG] is false in the Play build and the whole section compiles out of it —
 * the native side is not there either (ARMSX2_ENABLE_LSFG is off, so GSLsfg answers
 * NOT_COMPILED_IN), and build-play-aab.sh fails the build if the ported frame-generation code ever
 * appears in the bundle.
 */

/** Mirrors the native `GSLsfg::Unavailable` ordinals. Kept in the same order as the C++
 *  enum — the JNI query returns a raw ordinal, so a reordering on either side silently
 *  mislabels every reason. */
private enum class LsfgReason {
    AVAILABLE,
    NOT_COMPILED_IN,
    NOT_VULKAN,
    GPU_UNSUPPORTED,
    NO_DLL,
    DLL_UNREADABLE,
    INIT_FAILED,
    ;

    companion object {
        fun of(ordinal: Int): LsfgReason = values().getOrElse(ordinal) { NOT_COMPILED_IN }
    }
}

@Composable
private fun LsfgReason.message(): String = when (this) {
    LsfgReason.AVAILABLE -> ""
    LsfgReason.NOT_COMPILED_IN -> str("perf.lsfg.unavailable.notBuilt")
    LsfgReason.NOT_VULKAN -> str("perf.lsfg.unavailable.notVulkan")
    LsfgReason.GPU_UNSUPPORTED -> str("perf.lsfg.unavailable.gpu")
    LsfgReason.NO_DLL -> str("perf.lsfg.unavailable.noDll")
    LsfgReason.DLL_UNREADABLE -> str("perf.lsfg.unavailable.badDll")
    LsfgReason.INIT_FAILED -> str("perf.lsfg.unavailable.initFailed")
}

/** Where an imported Lossless.dll lives. Copied out of the SAF pick rather than referenced
 *  by content:// URI because the native side opens it as an ordinary file — pe-parse takes
 *  a path, not a file descriptor. */
private fun lsfgDllTarget(context: Context): File =
    File(File(context.filesDir, "lsfg").apply { mkdirs() }, "Lossless.dll")

@Composable
fun LsfgSection(
    enabled: Boolean,
    multiplier: Int,
    dllPath: String,
    performance: Boolean,
    flowScale: Int,
    targetRate: Int,
    onChange: (enabled: Boolean, multiplier: Int, dllPath: String, performance: Boolean, flowScale: Int, targetRate: Int) -> Unit,
) {
    if (!BuildConfig.LSFG) return

    val context = LocalContext.current
    var path by remember { mutableStateOf(dllPath) }
    var showRequirements by remember { mutableStateOf(false) }
    var importError by remember { mutableStateOf<String?>(null) }
    // str() is @Composable and the picker callback runs OUTSIDE composition, so the two
    // failure messages are resolved here and captured. Same pattern as UpdaterEntry.
    val importFailedMsg = str("perf.lsfg.dll.importFailed")
    val notADllMsg = str("perf.lsfg.dll.notADll")

    // Asked on every recomposition of this section rather than cached: the answer moves with
    // the renderer the user just switched to and with the file they just picked, and it is a
    // handful of stat calls.
    val reason = LsfgReason.of(NativeApp.lsfgAvailability(path))

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        val target = lsfgDllTarget(context)
        val copied = runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            } ?: error("could not open the selected file")
        }.isSuccess
        // Validate before storing the path. A wrong pick — a .txt, a truncated download, some
        // other DLL — otherwise sits in the config looking correct and fails much later,
        // inside a frame, where the only symptom is that frame generation quietly never
        // engages. NativeApp answers DLL_UNREADABLE for anything that is not a readable PE.
        if (!copied) {
            target.delete()
            importError = importFailedMsg
        } else if (LsfgReason.of(NativeApp.lsfgAvailability(target.absolutePath)) == LsfgReason.DLL_UNREADABLE) {
            target.delete()
            importError = notADllMsg
        } else {
            importError = null
            path = target.absolutePath
            onChange(enabled, multiplier, path, performance, flowScale, targetRate)
        }
    }

    ToggleRow(
        label = str("perf.lsfg.label"),
        value = enabled,
        description = str("perf.lsfg.description"),
    ) { on ->
        // The requirements dialog fires on the way ON only, and BEFORE the toggle commits.
        // Turning something on and then being told it cannot work is the shape of this that
        // wastes the user's time; being told what it needs first is the shape that does not.
        if (on) showRequirements = true else onChange(false, multiplier, path, performance, flowScale, targetRate)
    }

    if (enabled) {
        SettingsDivider()
        SegmentedRow(
            label = str("perf.lsfg.multiplier.label"),
            options = listOf("x2", "x3", "x4"),
            selectedIndex = (multiplier - 2).coerceIn(0, 2),
            description = str("perf.lsfg.multiplier.description"),
        ) { index -> onChange(enabled, index + 2, path, performance, flowScale, targetRate) }

        SettingsDivider()
        // Adaptive pacing. Stored as a concrete Hz because the native pacer needs a number, but
        // presented as a switch: picking a target rate by hand is not a decision anyone can make
        // usefully, and the only sensible answer is the panel's own refresh rate.
        ToggleRow(
            label = str("perf.lsfg.adaptive.label"),
            value = targetRate > 0,
            description = str("perf.lsfg.adaptive.description"),
        ) { on ->
            val hz = if (!on) 0 else runCatching {
                val d = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) context.display else null
                // ★ The panel's CAPABILITY, not Display.getRefreshRate().
                //
                // getRefreshRate() reports the rate the app's window is being driven at right now,
                // which on a 120Hz phone is usually 60 — Android only lifts a window to the high
                // mode when something asks. Capturing that made the target 60, and a 60fps game
                // then needs zero interpolated frames (desired_outputs = interval * target = 1.0),
                // so turning adaptive pacing ON silently turned frame generation OFF: FPS 60,
                // LSFG 60. Same resolution filter as everywhere else, so the target can never be a
                // rate that also implies a mode switch.
                val best = d?.supportedModes
                    ?.filter {
                        it.physicalWidth == d.mode.physicalWidth &&
                            it.physicalHeight == d.mode.physicalHeight
                    }
                    ?.maxOfOrNull { it.refreshRate }
                (best ?: d?.refreshRate ?: 60f).toInt().coerceIn(30, 480)
            }.getOrDefault(60)
            onChange(enabled, multiplier, path, performance, flowScale, hz)
        }

        SettingsDivider()
        ToggleRow(
            label = str("perf.lsfg.performance.label"),
            value = performance,
            description = str("perf.lsfg.performance.description"),
        ) { on -> onChange(enabled, multiplier, path, on, flowScale, targetRate) }

        SettingsDivider()
        // A percentage, not the divisor the library takes — the native side inverts it. Presented
        // this way round because "less detail" has to mean "cheaper" on the slider, and passing
        // the raw divisor through would put the cheap end at the top.
        IntSliderRow(
            label = str("perf.lsfg.flowScale.label"),
            value = flowScale.coerceIn(25, 100),
            min = 25,
            max = 100,
            description = str("perf.lsfg.flowScale.description"),
            valueFormatter = { "$it%" },
        ) { value -> onChange(enabled, multiplier, path, performance, value, targetRate) }

        SettingsDivider()
        LsfgDllRow(path, importError) { picker.launch(arrayOf("*/*")) }

        // Say WHY, not just "off". "Requires an Adreno 7xx GPU" and "you haven't picked a
        // Lossless.dll yet" are the same greyed row otherwise, and only one of them is
        // something the user can act on.
        if (reason != LsfgReason.AVAILABLE) {
            Text(
                reason.message(),
                color = MaterialTheme.colorScheme.error,
                fontSize = 14.sp,
                lineHeight = 19.sp,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
            )
        }
    }

    if (showRequirements) {
        LsfgRequirementsDialog(
            onDismiss = { showRequirements = false },
            onAccept = {
                showRequirements = false
                onChange(true, multiplier, path, performance, flowScale, targetRate)
                // Straight into the picker when there is nothing to run against — the first
                // thing the dialog just asked for is the file, so asking for it is the next
                // step rather than a second row to go and find.
                if (path.isBlank()) picker.launch(arrayOf("*/*"))
            },
        )
    }
}

@Composable
private fun LsfgDllRow(path: String, error: String?, onPick: () -> Unit) {
    Surface(
        onClick = onPick,
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .controllerFocusable(
                controllerId = "lsfg:dll",
                shape = RoundedCornerShape(22.dp),
                onConfirm = onPick,
            ),
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f)),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = 78.dp)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    str("perf.lsfg.dll.label"),
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 18.sp,
                    lineHeight = 23.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    error ?: if (path.isBlank()) str("perf.lsfg.dll.none") else str("perf.lsfg.dll.selected"),
                    color = if (error != null) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 14.sp,
                    lineHeight = 19.sp,
                )
            }
        }
    }
}

/**
 * What the user has to own and what hardware this needs, shown once before the toggle
 * commits.
 *
 * Not an AlertDialog: a dialog is its own focused WINDOW and swallows gamepad key events,
 * which would strand a controller user on a screen they cannot dismiss — the same reason
 * ShaderChainSection's picker is inline. This is a GlassPanel overlay inside the current
 * window, so the existing controller nav registry keeps working.
 */
@Composable
private fun LsfgRequirementsDialog(onDismiss: () -> Unit, onAccept: () -> Unit) {
    GlassPanel(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp)) {
        Column(Modifier.padding(12.dp)) {
            Text(
                str("perf.lsfg.requirements.title"),
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                str("perf.lsfg.requirements.body"),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 15.sp,
                lineHeight = 21.sp,
            )
            Spacer(Modifier.height(10.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = androidx.compose.foundation.layout.Arrangement.End) {
                TextButton(
                    onClick = onDismiss,
                    modifier = Modifier.controllerFocusable("lsfg:cancel", onConfirm = onDismiss),
                ) { Text(str("action.cancel")) }
                TextButton(
                    onClick = onAccept,
                    modifier = Modifier.controllerFocusable("lsfg:accept", onConfirm = onAccept),
                ) { Text(str("perf.lsfg.requirements.accept")) }
            }
        }
    }
}

/**
 * The in-game pause menu's frame-generation card.
 *
 * Exists so [com.armsx2.ui.emulation.EmulationMenuScreen] can call one thing and name nothing
 * else — with the SectionCard built here, the shared file no longer carries the section title's
 * string key. The play stub renders nothing, so the card is simply not in that menu.
 */
@Composable
fun LsfgEmulationCard(
    enabled: Boolean,
    multiplier: Int,
    dllPath: String,
    performance: Boolean,
    flowScale: Int,
    targetRate: Int,
    onChange: (enabled: Boolean, multiplier: Int, dllPath: String, performance: Boolean, flowScale: Int, targetRate: Int) -> Unit,
) {
    if (!BuildConfig.LSFG) return
    com.armsx2.ui.emulation.SectionCard(str("perf.lsfg.label")) {
        LsfgSection(enabled, multiplier, dllPath, performance, flowScale, targetRate, onChange)
    }
}
