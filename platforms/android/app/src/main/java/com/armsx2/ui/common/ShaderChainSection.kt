package com.armsx2.ui.common

import android.content.Context
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.R
import com.armsx2.ShaderParam
import com.armsx2.ShaderParams
import com.armsx2.ShaderRepo
import com.armsx2.i18n.str
import com.armsx2.ui.settings.HelpText
import com.armsx2.ui.settings.IntSliderRow
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.SettingsDivider
import com.armsx2.ui.settings.ToggleRow
import com.armsx2.ui.settings.controllerFocusable
import com.armsx2.ui.theme.Success
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** One `.slangp` on disk. [label] is the bare filename shown inside its directory;
 *  [path] is the absolute filesystem path stored in
 *  EmuCore/GS/ShaderChainPreset; [passes] is the resolved pass count — null when the file
 *  never yields one (see [resolvePasses]).
 *
 *  [passes] is reported as a FACT on the row, never used to rank or classify. Pass count is
 *  not a cost proxy and we're not going to pretend it is: xBRZ does enormous per-pixel
 *  neighbourhood comparison in ~5 passes and runs heavy, while koko-aio's ambilight chain
 *  runs ~19 passes at tiny scale and runs great. Cost is dominated by per-pass resolution
 *  and per-pass ALU, neither of which is knowable from the preset file — so the number is
 *  shown and the judgement is left to the user. Do NOT resurrect a Lightweight/Heavy split
 *  on top of it: a wrong "Lightweight" badge misleads exactly the user who picked that tier
 *  because they wanted cheap. */
private data class ShaderPreset(val label: String, val path: String, val passes: Int?)

/** One directory in the on-disk shader tree. Only this directory's immediate [folders] and
 *  [presets] are composed at a time; navigating into a child replaces the visible rows
 *  instead of adding another expanded section to an ever-growing list. */
private data class ShaderDirectory(
    val key: String,
    val label: String,
    val count: Int,
    val folders: List<ShaderDirectory>,
    val presets: List<ShaderPreset>,
)

/** Result of one scan, including the filesystem path used by the empty-state message. */
private data class ShaderScan(val dir: String, val root: ShaderDirectory)

/** A preset plus the path segments of its folder, relative to the shaders root. */
private class FoundPreset(val preset: ShaderPreset, val segments: List<String>)

/** Mutable construction node used only on the scan's IO thread, then frozen for Compose. */
private class ShaderDirectoryBuilder(val key: String, val name: String) {
    val folders = linkedMapOf<String, ShaderDirectoryBuilder>()
    val presets = mutableListOf<ShaderPreset>()
}

/** Depth cap for #reference chains. The deepest real chain in the stock pack is 7 hops
 *  (bezel/koko-aio/Presets-4.1/FXAA-bloom-immersive.slangp), so this is pure headroom for a
 *  future pack; the actual loop guard is the visited set in [resolvePasses]. */
private const val MAX_REFERENCE_DEPTH = 16

/** Download directory used by ShaderRepo's standard RetroArch pack. It is an installation
 *  wrapper, not a useful category, so [promoteDefaultPackContents] hides this one level
 *  while preserving the real paths stored in every preset. */
private const val DEFAULT_SHADER_PACK_DIR = "shaders_slang"

/** `shaders = 12` or `shaders = "12"` — 423 presets in the stock pack quote the value, so
 *  the quotes are not optional to handle. */
private val SHADERS_RE = Regex("""^\s*shaders\s*=\s*"?(\d+)"?""", RegexOption.IGNORE_CASE)

/** `#reference "../../Root_Presets/MBZ__3__STD__GDV.slangp"` — a preset that inherits its
 *  whole chain from another file and only overrides parameters. */
private val REFERENCE_RE = Regex("""^\s*#reference\s+(.+?)\s*$""", RegexOption.IGNORE_CASE)

/** Folder-name tokens that are initialisms, so [folderLabel] renders "CRT" not "Crt". */
private val FOLDER_ACRONYMS = setOf(
    "crt", "gpu", "hdr", "ntsc", "pal", "vhs", "nes", "bfi", "fsr", "lcd", "nis", "3d",
)

/** Folder names whose own branding is lowercase, where the mechanical title-casing in
 *  [folderLabel] would be wrong. Deliberately tiny: everything else in the stock pack
 *  humanises correctly by rule, and a hand-written table of all 39 names would rot the
 *  moment someone installs a pack we have never seen. */
private val FOLDER_NAME_OVERRIDES = mapOf("koko-aio" to "koko-aio", "uborder" to "uborder")

/** Renders a caption parameter's description as a group heading: "[ --- BLACK TINT --- ]:"
 *  reads "BLACK TINT". Cosmetic only — a parameter is identified as a caption by the fact
 *  that it cannot be adjusted ([ShaderParam.isAdjustable]), never by how it's written.
 *  Reading brackets to decide what a caption IS would hide real controls: the stock pack
 *  ships working sliders described "[ Adaptive Strobe (≈BFI) Strength: ]" and "[IMG]
 *  Contrast (squared) [luma]". Shared with the full-screen editor. */
internal fun captionLabel(description: String): String =
    description.trim().trim('[', ']', ':', '-', ' ').trim('-', ' ').ifBlank { description.trim() }

/**
 * RetroArch (.slangp) shader-chain rows: a master toggle plus an inline preset picker.
 *
 * Presentation only — the caller wires [onEnabledChange] / [onPresetChange] to its OWN
 * settings tier (the same split as [AngleDriverSection]). That's what lets one definition
 * serve both hosts without either knowing about the other's save path:
 *   - the Settings renderer tab passes its `apply()`, so the rows honour the Global / Game
 *     scope like every other row on that tab;
 *   - the in-game pause menu passes `viewModel.updateSettings`, which routes through
 *     InGameOverlay.saveSettings → ConfigStore.save(scope, serial) + Settings.applyTo().
 * Do NOT reach for a settings tier from in here — the tier is the caller's to decide.
 *
 * The picker is an INLINE expandable list modelled on DriverManagerSection. Two things it
 * deliberately is NOT:
 *   - an AlertDialog: a dialog is its own focused WINDOW and swallows gamepad key events,
 *     which would strand the list behind a controller.
 *   - a Lazy list: controllerFocusable only registers COMPOSED rows, so a LazyColumn would
 *     leave every off-screen preset out of the nav registry.
 * A plain Column composes every row, and the HOST's own verticalScroll does the scrolling
 * (the settings hub's LocalSettingsScrollState, or the pause menu's pane scroll) — nesting
 * a second vertical scroll here would measure against infinite height constraints and throw.
 */
@Composable
fun ShaderChainSection(
    enabled: Boolean,
    preset: String,
    params: Map<String, Map<String, Float>>,
    onEnabledChange: (Boolean) -> Unit,
    onPresetChange: (String) -> Unit,
    onParamsChange: (Map<String, Map<String, Float>>) -> Unit,
) {
    ToggleRow(
        str("renderer.shaderChain.label"),
        enabled,
        description = str("renderer.shaderChain.description"),
    ) {
        onEnabledChange(it)
    }
    // Gate: the picker only exists while the chain is on — the same shape as the
    // Shadeboost sliders / CAS sharpness rows above, and it keeps a dead row out of the
    // controller focus registry rather than parking focus on something inert.
    if (enabled) {
        SettingsDivider()
        ShaderPresetPicker(preset, onPresetChange)
        // Same gate one level down: with no preset there is nothing to enumerate, and the
        // section would be a row that opens onto an empty list.
        if (preset.isNotBlank()) {
            SettingsDivider()
            ShaderParamPicker(preset, params, onParamsChange, onPresetChange)
        }
    }
}

/**
 * The tweakable-parameter list for the selected preset.
 *
 * Values are written straight through to the caller's settings tier (same contract as the
 * preset picker) AND pushed at the running chain here, because those are two different
 * jobs and only one of them can be left to [com.armsx2.config.Settings.applyTo]:
 *  - persisting is the caller's, via [onParamsChange];
 *  - the LIVE apply has to send the effective value of every parameter, not just the
 *    overrides, so that resetting one restores the author's default on a chain that is
 *    already running. applyTo can't do that — it has no enumeration to read initials from.
 */
@Composable
private fun ShaderParamPicker(
    preset: String,
    allParams: Map<String, Map<String, Float>>,
    onParamsChange: (Map<String, Map<String, Float>>) -> Unit,
    onPresetChange: (String) -> Unit,
) {
    val overrides = allParams[preset].orEmpty()

    /** Drop the preset's entry entirely once nothing is overridden, so a reset-to-default
     *  leaves no trace in the config rather than an empty object reading as "tweaked". */
    fun persist(next: Map<String, Float>) {
        onParamsChange(if (next.isEmpty()) allParams - preset else allParams + (preset to next))
    }

    val open: () -> Unit = { ShaderParamsEditor.open(preset, overrides, ::persist, onPresetChange) }

    // Note what this row does NOT do: enumerate. The count of CHANGED parameters is just
    // the override map's size, and the total only matters once you're looking at the list —
    // so the (heavy) chain parse happens when the editor opens, not on every visit to this
    // tab.
    Surface(
        onClick = open,
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .controllerFocusable(
                controllerId = "shaderChain:params",
                shape = RoundedCornerShape(22.dp),
                onConfirm = open,
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
                    str("renderer.shaderChain.params.label"),
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 18.sp,
                    lineHeight = 23.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    if (overrides.isEmpty()) str("renderer.shaderChain.params.description")
                    else str("renderer.shaderChain.params.countModified").format(overrides.size),
                    color = MaterialTheme.colorScheme.primary,
                    fontSize = 14.sp,
                    lineHeight = 19.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.width(12.dp))
            // Opens a screen rather than expanding, so it points forward, not down.
            Text(
                "›",
                color = MaterialTheme.colorScheme.primary,
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }

    // Reset-all and Save-as live INSIDE the editor, not here. They belong with the list
    // they act on, and both need input this pane can't give them: the reset wants a
    // confirmation, and naming a preset wants a keyboard a controller can actually drive.
    // The editor is a surface that owns the pad, so it can do both.
}

@Composable
private fun ShaderPresetPicker(preset: String, onPresetChange: (String) -> Unit) {
    val context = LocalContext.current
    val expanded = remember { mutableStateOf(false) }
    val scan = remember { mutableStateOf<ShaderScan?>(null) }
    val scanning = remember { mutableStateOf(false) }
    val pickerBringIntoView = remember { BringIntoViewRequester() }
    // The path relative to the shader root. An empty key is the root itself. Keeping one
    // current location is what makes this a browser rather than a set of nested accordions.
    val currentFolderKey = remember { mutableStateOf("") }

    // Rescan on every open: packs get dropped in with a file manager (or the Shader Packs
    // downloader) while the app is alive, so a one-shot scan at first composition goes
    // stale. Off the UI thread — a slang-shaders pack is a deep tree of thousands of files,
    // and the costing/grouping/sorting rides the same IO hop rather than landing in
    // composition.
    LaunchedEffect(expanded.value) {
        if (!expanded.value) return@LaunchedEffect
        scanning.value = true
        try {
            val result = withContext(Dispatchers.IO) { scanShaderPresets(context) }
            scan.value = result
            // Always reopen at the root. This gives both hosts a predictable first screen:
            // top-level shader packs only, regardless of where the last preset lives.
            currentFolderKey.value = ""
        } finally {
            // Files can disappear while a pack manager extracts/deletes them. Never leave
            // the picker permanently reporting a scan merely because traversal threw.
            scanning.value = false
        }
    }

    val root = scan.value?.root
    val currentFolder = remember(root, currentFolderKey.value) {
        root?.findDirectory(currentFolderKey.value) ?: root
    }
    // A folder is marked Active when it is the selected preset's directory or one of its
    // ancestors. Users can therefore follow the marker down without expanding everything.
    val activeFolderKey = remember(root, preset) { root?.findPresetDirectory(preset) }

    fun navigateTo(folderKey: String) {
        // A controller-confirmed folder is about to dispose its own selected row. ARMSX2's
        // registry otherwise falls back to the FIRST control in the whole tab, which also
        // scrolls the pause menu to its top. Move controller selection to this stable header
        // first. A touch click has no selected folder, so it does not gain a focus ring.
        if (SettingsControllerNav.currentSelectedId()?.startsWith("shaderChain:folder:") == true) {
            SettingsControllerNav.selectById("shaderChain:preset")
        }
        currentFolderKey.value = folderKey
    }

    // Replacing a long directory with a short child can clamp the HOST scroll position to
    // its new maximum, leaving this picker above the viewport. Wait for the shorter tree to
    // be measured, then ask whichever host owns the scroll (settings or pause menu) to show
    // the picker header again. This stays host-agnostic and works with both verticalScrolls.
    LaunchedEffect(currentFolderKey.value) {
        if (expanded.value && scan.value != null) {
            withFrameNanos { }
            runCatching { pickerBringIntoView.bringIntoView() }
        }
    }

    Surface(
        onClick = { expanded.value = !expanded.value },
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .controllerFocusable(
                controllerId = "shaderChain:preset",
                shape = RoundedCornerShape(22.dp),
                onConfirm = { expanded.value = !expanded.value },
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
                // Keep the folder-navigation reveal anchor off the outer Surface: that
                // Surface's controllerFocusable modifier owns a separate requester. Two
                // relocation anchors on the same node made the pause pane reveal the whole
                // tab instead of this header, while this inner row has one unambiguous bound.
                .bringIntoViewRequester(pickerBringIntoView)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    str("renderer.shaderChain.preset.label"),
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 18.sp,
                    lineHeight = 23.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    // Filename without extension — the folder-qualified label is only
                    // needed inside the list, where two packs can share a basename.
                    if (preset.isBlank()) str("renderer.shaderChain.preset.none")
                    else File(preset).nameWithoutExtension,
                    color = MaterialTheme.colorScheme.primary,
                    fontSize = 14.sp,
                    lineHeight = 19.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.width(12.dp))
            Text(
                if (expanded.value) "▾" else "▸",
                color = MaterialTheme.colorScheme.primary,
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }

    if (expanded.value) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            // "None" clears the setting; the chain then no-ops even with the toggle on.
            ShaderPresetRow(
                controllerId = "shaderChain:preset:none",
                label = str("renderer.shaderChain.preset.none"),
                passes = null,
                showPasses = false,
                selected = preset.isBlank(),
                onClick = { onPresetChange("") },
            )
            if (currentFolder?.key?.isNotEmpty() == true) {
                ShaderFolderRow(
                    controllerId = "shaderChain:folder:up:${currentFolder.key}",
                    label = str("renderer.shaderChain.folder.up"),
                    count = null,
                    holdsActive = false,
                    isFolder = false,
                    onClick = { navigateTo(currentFolder.parentKeyWithin(root)) },
                )
            }
            currentFolder?.folders.orEmpty().forEach { folder ->
                ShaderFolderRow(
                    controllerId = "shaderChain:folder:${folder.key}",
                    label = folder.label,
                    count = folder.count,
                    holdsActive = activeFolderKey == folder.key ||
                        activeFolderKey?.startsWith("${folder.key}/") == true,
                    isFolder = true,
                    onClick = { navigateTo(folder.key) },
                )
            }
            currentFolder?.presets.orEmpty().forEach { p -> PresetRow(p, preset, onPresetChange) }
            if (root?.count == 0 && !scanning.value) {
                HelpText(str("renderer.shaderChain.empty") + "\n\n" + scan.value?.dir.orEmpty())
            }
        }
    }
}

/** Picking deliberately does NOT collapse the list: the row would dispose and unregister
 *  itself, and SettingsControllerNav re-points the selection at the FIRST row of the whole
 *  tab — throwing a controller user back to the top of Renderer. Same as
 *  DriverManagerSection: the list stays open and only the chip moves. */
@Composable
private fun PresetRow(p: ShaderPreset, preset: String, onPresetChange: (String) -> Unit) {
    ShaderPresetRow(
        controllerId = "shaderChain:preset:${p.path}",
        label = p.label,
        passes = p.passes,
        selected = p.path == preset,
        onClick = { onPresetChange(p.path) },
    )
}

@Composable
private fun ShaderFolderRow(
    controllerId: String,
    label: String,
    count: Int?,
    holdsActive: Boolean,
    isFolder: Boolean,
    onClick: () -> Unit,
) {
    Surface(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth()
            .controllerFocusable(controllerId, RoundedCornerShape(18.dp), onConfirm = onClick),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.92f),
        border = BorderStroke(
            1.dp,
            if (holdsActive) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.5f),
        ),
    ) {
        Row(
            Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (isFolder) {
                // A vector resource, not a Unicode/emoji glyph: Android's text fonts do not
                // reliably contain folder symbols, which otherwise render as a tofu square.
                Icon(
                    painter = painterResource(R.drawable.ic_folder),
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(20.dp),
                )
            } else {
                Text("←", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
            }
            Spacer(Modifier.width(10.dp))
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                lineHeight = 21.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (holdsActive) {
                StatusChip(str("backend.driver.active"), Success)
                Spacer(Modifier.width(8.dp))
            }
            count?.let {
                Text("$it", style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (isFolder) {
                Spacer(Modifier.width(8.dp))
                Text("›", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold)
            }
        }
    }
}

@Composable
private fun ShaderPresetRow(
    controllerId: String,
    label: String,
    passes: Int?,
    selected: Boolean,
    onClick: () -> Unit,
    showPasses: Boolean = true,
) {
    Surface(
        onClick = onClick,
        modifier = Modifier
            .fillMaxWidth()
            .controllerFocusable(controllerId, RoundedCornerShape(16.dp), onConfirm = onClick),
        shape = RoundedCornerShape(16.dp),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer
                else MaterialTheme.colorScheme.surfaceVariant,
        border = BorderStroke(
            1.dp,
            if (selected) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.outline.copy(alpha = 0.45f),
        ),
    ) {
        Row(
            Modifier.padding(13.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 15.sp,
                lineHeight = 20.sp,
                // Folder names are the strong navigational labels. Presets stay regular
                // weight so a mixed directory is immediately scannable as folders vs files.
                fontWeight = FontWeight.Normal,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            // A fact the user can read, not a verdict we assign — see [ShaderPreset]. str()
            // takes no format args (see I18n.kt), so the count is concatenated the same way
            // renderer.shaderPack.presets already does it.
            if (showPasses) {
                Text(
                    when {
                        passes == null -> str("renderer.shaderChain.passesUnknown")
                        passes == 1 -> "$passes " + str("renderer.shaderChain.pass")
                        else -> "$passes " + str("renderer.shaderChain.passes")
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                )
            }
            if (selected) StatusChip(str("backend.driver.active"), Success)
        }
    }
}

/** Scan `<DataRoot>/shaders/` recursively for `*.slangp`. The folder is
 *  [ShaderRepo.shadersRoot] — the same one the Shader Packs downloader extracts into (and
 *  it creates the dir on demand, so the empty state can name a folder the user will
 *  actually find in a file manager). That helper resolves through assetCopyRoot, like the
 *  texture / cache / memcard folders, so this follows a moved data folder.
 *
 *  Blocking I/O — call it off the UI thread. It READS every preset (and follows the
 *  #reference chains between them) rather than just listing names, so the one memo cache is
 *  load-bearing: 691 of the stock pack's 2542 presets are thin wrappers pointing at a
 *  handful of shared roots, and without memoising those roots would be re-parsed hundreds
 *  of times each. */
private fun scanShaderPresets(context: Context): ShaderScan {
    val root = ShaderRepo.shadersRoot(context)
    if (!root.isDirectory) return ShaderScan(root.absolutePath, emptyShaderDirectory())
    val cache = HashMap<String, Int?>()
    val found = try {
        root.walkTopDown()
            .filter { it.isFile && it.extension.equals("slangp", ignoreCase = true) }
            .map { file ->
                val dir = file.parentFile?.relativeToOrNull(root)?.invariantSeparatorsPath.orEmpty()
                    .let { if (it == ".") "" else it }
                FoundPreset(
                    preset = ShaderPreset(
                        label = file.nameWithoutExtension,
                        path = file.absolutePath,
                        passes = resolvePasses(file, cache, HashSet(), 0),
                    ),
                    segments = if (dir.isEmpty()) emptyList() else dir.split('/'),
                )
            }
            .toList()
    } catch (_: Exception) {
        // A pack can be replaced from a file manager while this background walk is active.
        // Treat that one scan as empty; reopening immediately rescans the completed tree.
        emptyList()
    }

    val builder = ShaderDirectoryBuilder("", "")
    found.forEach { foundPreset ->
        var directory = builder
        foundPreset.segments.forEach { segment ->
            val childKey = if (directory.key.isEmpty()) segment else "${directory.key}/$segment"
            directory = directory.folders.getOrPut(segment) {
                ShaderDirectoryBuilder(childKey, segment)
            }
        }
        directory.presets += foundPreset.preset
    }
    return ShaderScan(root.absolutePath, builder.freeze().promoteDefaultPackContents())
}

private fun emptyShaderDirectory() = ShaderDirectory("", "", 0, emptyList(), emptyList())

/** Convert the scan-only builder into a stable, alphabetically sorted UI tree. */
private fun ShaderDirectoryBuilder.freeze(): ShaderDirectory {
    val children = folders.values.map { it.freeze() }.sortedBy { it.label.lowercase() }
    val directPresets = presets.sortedBy { it.label.lowercase() }
    return ShaderDirectory(
        key = key,
        label = folderLabel(name),
        count = directPresets.size + children.sumOf { it.count },
        folders = children,
        presets = directPresets,
    )
}

/**
 * Make the standard pack's categories the browser root instead of displaying the redundant
 * `shaders_slang` installation folder first. Other root-level folders are kept alongside
 * them — notably `My Presets` and manually installed packs — so simplifying the common path
 * does not make less-common content unreachable.
 */
private fun ShaderDirectory.promoteDefaultPackContents(): ShaderDirectory {
    val defaultPack = folders.firstOrNull { it.key == DEFAULT_SHADER_PACK_DIR } ?: return this
    val otherFolders = folders.filterNot { it.key == defaultPack.key }
    val promotedFolders = (defaultPack.folders + otherFolders).sortedBy { it.label.lowercase() }
    val promotedPresets = (defaultPack.presets + presets).sortedBy { it.label.lowercase() }
    return copy(
        count = promotedPresets.size + promotedFolders.sumOf { it.count },
        folders = promotedFolders,
        presets = promotedPresets,
    )
}

private fun ShaderDirectory.findDirectory(targetKey: String): ShaderDirectory? {
    if (key == targetKey) return this
    return folders.firstNotNullOfOrNull { it.findDirectory(targetKey) }
}

private fun ShaderDirectory.findPresetDirectory(presetPath: String): String? {
    if (presetPath.isBlank()) return null
    if (presets.any { it.path == presetPath }) return key
    return folders.firstNotNullOfOrNull { it.findPresetDirectory(presetPath) }
}

/** Parent inside the PRESENTED tree. The promoted pack wrapper is absent, so a category
 *  such as `shaders_slang/crt` returns the visible root instead of that hidden wrapper. */
private fun ShaderDirectory.parentKeyWithin(root: ShaderDirectory?): String {
    if (root == null) return ""
    val parent = key.substringBeforeLast('/', "")
    return if (root.findDirectory(parent) != null) parent else root.key
}

/**
 * Human display name for a shader folder: "Mega_Bezel" → "Mega Bezel", "crt" → "CRT",
 * "edge-smoothing" → "Edge Smoothing", "nes_raw_palette" → "NES Raw Palette".
 *
 * Mechanical (split the leaf on _ and -, title-case, uppercase known initialisms) plus a
 * two-entry override map, rather than a hand-written table of all 39 names: the rule gets
 * every folder in the stock pack right except the two packs whose branding is deliberately
 * lowercase, and unlike a table it still does something sensible for a pack nobody has seen
 * yet.
 *
 * NOT routed through str(): these are folder names read off the user's disk — data, not UI
 * chrome — only fixed interface labels such as the Up row are translated.
 */
private fun folderLabel(leaf: String): String {
    FOLDER_NAME_OVERRIDES[leaf.lowercase()]?.let { return it }
    return leaf.split('_', '-', ' ')
        .filter { it.isNotEmpty() }
        .joinToString(" ") { token ->
            if (token.lowercase() in FOLDER_ACRONYMS) token.uppercase()
            else token.replaceFirstChar { it.uppercase() }
        }
        .ifEmpty { leaf }
}

/** What one `.slangp` says about its own cost: its pass count, or the presets it inherits
 *  one from. Never both in the stock pack — a `#reference` preset overrides parameters
 *  only — but [resolvePasses] prefers a local count anyway, which is the RetroArch rule. */
private class PresetFacts(val shaders: Int?, val references: List<String>)

private fun readPreset(file: File): PresetFacts {
    var shaders: Int? = null
    val references = ArrayList<String>(2)
    try {
        file.bufferedReader().useLines { lines ->
            for (line in lines) {
                val hit = SHADERS_RE.find(line)
                if (hit != null) {
                    shaders = hit.groupValues[1].toIntOrNull()
                    // A local count wins outright, so the references can't change the
                    // answer — stop reading. Bails on line 1 for most of the pack.
                    if (shaders != null) return@useLines
                }
                REFERENCE_RE.find(line)?.let {
                    references.add(it.groupValues[1].trim().trim('"', '\''))
                }
            }
        }
    } catch (_: Exception) {
        // Unreadable / vanished mid-scan: the row says "cost unknown" rather than
        // inventing a number.
    }
    return PresetFacts(shaders, references)
}

/**
 * The preset's pass count, following `#reference` chains. null = undeterminable, which the
 * row reports honestly as "cost unknown".
 *
 * The reference hop is the whole reason this function exists rather than a one-line regex.
 * 691 of the stock pack's 2542 presets — essentially all of Mega Bezel and koko-aio — carry
 * no `shaders` key at all; they are thin files whose entire body is `#reference
 * "../some/root.slangp"` plus parameter overrides. Reading `shaders` naively finds nothing
 * for exactly those files, so without this the headline number would be blank on precisely
 * the biggest chains in the pack.
 *
 * Targets resolve against the REFERENCING file's directory (they are written `../../…`).
 * [seen] is the loop guard (canonical paths, so a symlinked pack can't dodge it); [cache]
 * memoises across the whole scan.
 */
private fun resolvePasses(
    file: File,
    cache: MutableMap<String, Int?>,
    seen: MutableSet<String>,
    depth: Int,
): Int? {
    val canonical = runCatching { file.canonicalPath }.getOrDefault(file.absolutePath)
    if (depth > MAX_REFERENCE_DEPTH || !seen.add(canonical)) return null
    if (cache.containsKey(canonical)) return cache[canonical]
    val facts = readPreset(file)
    val result = facts.shaders ?: facts.references.firstNotNullOfOrNull { ref ->
        val target = File(file.parentFile, ref)
        // Multi-reference presets (324 of them) list the .slangp first and .params after;
        // taking the first target that actually yields a count skips the parameter files
        // without having to sniff extensions.
        if (target.isFile) resolvePasses(target, cache, seen, depth + 1) else null
    }
    cache[canonical] = result
    return result
}
