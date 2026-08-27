package com.armsx2.ui.saves

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyHorizontalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.IntSliderRow
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

/** Which flow the picker drives — Save writes a slot, Load restores one. */
enum class SaveMode { Save, Load }

private const val SLOTS = 10
private const val TILE_WIDTH_DP = 200
// Height for the two-row slot grid: two ~160dp tiles plus the 10dp row gap and 8dp of
// vertical content padding. Fixed so the tiles never shrink to fit whatever sits above.
private const val TILE_GRID_HEIGHT_DP = 338

/**
 * In-game save-state slot picker, the rich replacement for the pause menu's
 * quick Save / Load buttons (matches the old Refresh UI). Both modes show the
 * 10 numbered slots as thumbnail tiles previewed via `getImageSlot(slot)`; the
 * game path from `getGamePathSlot(slot)` marks a slot as occupied.
 *
 *  - Save: any tile is a valid target (tap = write/overwrite), then close +
 *    resume via onBack.
 *  - Load: only occupied slots are enabled; the leading Autosave tile (shown
 *    when `hasAutosaveState`) restores the on-exit state. Load runs to
 *    completion on the IO pool *before* onBack resumes the VM, so the game
 *    never resumes at the pre-load frame (the quick-load race).
 *
 * Load mode also surfaces the two persistence toggles the old UI had:
 * auto-save-on-exit and auto-load-last-state-on-boot (backed by the
 * `autoSaveOnExit` / `autoLoadOnBoot` prefs honoured in MainActivityRuntime).
 */
@Composable
fun SaveStatePickerScreen(mode: SaveMode, onBack: () -> Unit) {
    // Save/load run on Dispatchers.IO, not the VM thread — the single-threaded
    // eDispatcher is parked inside the VM main loop, so a Main-queued task would
    // never fire. onBack hops back to Main (it mutates overlay state + resumes).
    val scope = rememberCoroutineScope()
    // Probe the autosave slot once (Load only) — hasAutosaveState touches disk.
    val hasAutosave by produceState(initialValue = false, mode) {
        value = if (mode == SaveMode.Load) withContext(Dispatchers.IO) {
            runCatching { NativeApp.hasAutosaveState() }.getOrDefault(false)
        } else false
    }
    // i18n KEY of the last slot-tap failure, shown in place of silently closing the picker.
    var failure by remember { mutableStateOf<String?>(null) }
    // Import an external save-state file into this game's next free slot. A green success line +
    // a refresh bump so the slot tiles re-probe and show the imported save immediately.
    var notice by remember { mutableStateOf<String?>(null) }
    var refreshKey by remember { mutableStateOf(0) }
    // Slot pending deletion, confirmed before anything is removed.
    var deleteSlot by remember { mutableStateOf<Int?>(null) }
    // Delete MODE. Long-press works, but it is invisible and a controller cannot long-press —
    // both reported. With this armed, choosing a slot deletes it instead of loading/saving, so
    // the D-pad + A reaches deletion through the same path everything else uses.
    var deleteMode by remember { mutableStateOf(false) }
    val importContext = androidx.compose.ui.platform.LocalContext.current
    val importLauncher = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri != null) scope.launch {
            val slot = withContext(Dispatchers.IO) { importSaveStateToNextFreeSlot(importContext, uri) }
            when {
                slot >= 0 -> { failure = null; notice = "${com.armsx2.i18n.I18n.get("savestate.import")} · ${slot + 1}"; refreshKey++ }
                slot == SS_IMPORT_NO_GAME -> { notice = null; failure = "savestate.import.needsGame" }
                slot == SS_IMPORT_SLOTS_FULL -> { notice = null; failure = "savestate.import.slotsFull" }
                else -> { notice = null; failure = "savestate.import.failed" }
            }
        }
    }

    ArmsBackdrop {
        Column(Modifier.fillMaxSize()) {
            ArmsTopBar(
                title = if (mode == SaveMode.Save) str("savestate.title.save")
                else str("savestate.title.loadManage"),
                leading = { RoundAction("←", str("action.back"), onBack, controllerId = "save.back") },
                actions = {
                    RoundAction("⤓", str("savestate.import"), onClick = { importLauncher.launch(arrayOf("*/*")) }, controllerId = "save.import")
                    RoundAction(
                        "🗑",
                        str("savestate.delete.mode"),
                        onClick = { deleteMode = !deleteMode; notice = null; failure = null },
                        selected = deleteMode,
                        controllerId = "save.deleteMode",
                    )
                },
            )
            // Say what the screen can do. Long-press-to-delete was undiscoverable on its own —
            // there was nothing on screen to suggest it existed.
            Text(
                if (deleteMode) str("savestate.delete.modeHint") else str("savestate.hint"),
                color = if (deleteMode) Color(0xFFFFB4A2) else Color(0xFF9AA0A6),
                fontSize = 12.sp,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            failure?.let { key ->
                Text(
                    str(key),
                    color = Color(0xFFFFB4A2),
                    fontSize = 13.sp,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
            notice?.let { text ->
                Text(
                    text,
                    color = Color(0xFF9BE29B),
                    fontSize = 13.sp,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
            // Scrollable body: the Load screen stacks the auto-save options ABOVE the
            // slot grid, and the interval-autosave row made that block tall enough to
            // squeeze a weight(1f) grid — the two rows of tiles shrank to fit and looked
            // squished (the reported scaling bug). Give the grid a FIXED height so the
            // tiles are always full-size, and let the column scroll when options + grid
            // together exceed a short screen. The Save screen has no options block, so it
            // just shows the same full-size grid as before.
            Column(
                Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()),
            ) {
                if (mode == SaveMode.Load) {
                    AutoOptions(Modifier.fillMaxWidth().padding(horizontal = 8.dp))
                    Spacer(Modifier.height(10.dp))
                }
                LazyHorizontalGrid(
                    rows = GridCells.Fixed(2),
                    // Two comfortable rows of TILE_WIDTH_DP-wide tiles. Fixed, not
                    // weight — a bounded height is also required for a horizontal grid
                    // inside a vertical scroll.
                    modifier = Modifier.height(TILE_GRID_HEIGHT_DP.dp).fillMaxWidth().padding(horizontal = 8.dp),
                    contentPadding = PaddingValues(vertical = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                if (mode == SaveMode.Load && hasAutosave) {
                    item(key = "autosave") {
                        AutosaveTile {
                            scope.launch(Dispatchers.IO) {
                                NativeApp.loadAutosaveState()
                                withContext(Dispatchers.Main) { onBack() }
                            }
                        }
                    }
                }
                    items((0 until SLOTS).toList(), key = { "slot_$it" }) { slot ->
                        SlotTile(
                            slot,
                            mode,
                            refreshKey,
                            deleteMode = deleteMode,
                            onDelete = { deleteSlot = it },
                        ) { selected ->
                            // Armed for deletion: choosing a slot deletes it, so the pad reaches
                            // deletion through the same confirm press it uses everywhere else.
                            if (deleteMode) {
                                deleteSlot = selected
                                return@SlotTile
                            }
                            scope.launch(Dispatchers.IO) {
                                // The result used to be discarded and onBack() called either way, so
                                // a refused save closed the picker looking exactly like a successful
                                // one — no state written, no warning. That is the reported "closes
                                // as if saved, takes 2-3 attempts". Stay open and say why instead.
                                val ok = when (mode) {
                                    SaveMode.Save -> NativeApp.saveStateToSlot(selected)
                                    SaveMode.Load -> NativeApp.loadStateFromSlot(selected)
                                }
                                val busy = !ok && mode == SaveMode.Save &&
                                    runCatching { NativeApp.isMemcardBusy() }.getOrDefault(false)
                                val hardcore = !ok &&
                                    runCatching { NativeApp.isHardcoreMode() }.getOrDefault(false)
                                withContext(Dispatchers.Main) {
                                    if (ok) {
                                        failure = null
                                        onBack()
                                    } else {
                                        // Store the KEY, not the resolved text — str() is
                                        // @Composable and this is a coroutine, and keeping the key
                                        // lets the banner re-translate on a language switch.
                                        failure = when {
                                            // RA hardcore forbids save states outright, and the
                                            // refusal happens inside VMManager — below every exit
                                            // this JNI logs — so it surfaced as a bare "couldn't
                                            // load that slot" with nothing in logcat. Name it.
                                            hardcore -> "savestate.error.hardcore"
                                            busy -> "savestate.error.memcardBusy"
                                            mode == SaveMode.Save -> "savestate.error.save"
                                            else -> "savestate.error.load"
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Delete confirmation for a long-pressed slot. ConfirmOverlay (not a Dialog) so the pad can
    // reach it; the file is removed directly because there is no delete JNI — the slot path from
    // getGamePathSlot is the same file the manager deletes.
    deleteSlot?.let { slot ->
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("savestate.delete.title"),
            message = str("savestate.delete.body").format(slot + 1),
            confirmLabel = str("action.delete"),
            destructive = true,
            idPrefix = "savestate.delete",
            onDismiss = { deleteSlot = null },
            onConfirm = {
                deleteSlot = null
                scope.launch(Dispatchers.IO) {
                    val ok = runCatching {
                        val path = NativeApp.getGamePathSlot(slot)
                        !path.isNullOrBlank() && java.io.File(path).delete()
                    }.getOrDefault(false)
                    withContext(Dispatchers.Main) {
                        if (ok) {
                            failure = null
                            notice = "${com.armsx2.i18n.I18n.get("action.delete")} · ${slot + 1}"
                            refreshKey++
                        } else {
                            notice = null
                            failure = "savestate.delete.failed"
                        }
                    }
                }
            },
        )
    }
}

/** Auto-save-on-exit + auto-load-last-state-on-boot persistence toggles, and the
 *  interval autosave that writes the same slot while you play. */
@Composable
private fun AutoOptions(modifier: Modifier = Modifier) {
    val prefs = MainActivityRuntime.prefs
    var autoSave by remember { mutableStateOf(prefs.getBoolean("autoSaveOnExit", false)) }
    var autoLoad by remember { mutableStateOf(prefs.getBoolean("autoLoadOnBoot", false)) }
    var interval by remember {
        mutableIntStateOf(prefs.getInt(MainActivityRuntime.KEY_AUTOSAVE_INTERVAL_MIN, 0))
    }
    // str() is @Composable; valueFormatter is a plain lambda, so resolve up-front.
    val offLabel = str("savestate.autoSaveInterval.off")
    val everyLabel = str("savestate.autoSaveInterval.every")
    GlassPanel(modifier = modifier, contentPadding = 12.dp) {
        Column {
            ToggleRow("save.opt.autoSave", str("savestate.autoSaveOnExit"), autoSave) { value ->
                autoSave = value
                prefs.edit().putBoolean("autoSaveOnExit", value).apply()
            }
            Spacer(Modifier.height(6.dp))
            ToggleRow("save.opt.autoLoad", str("savestate.autoLoadOnBoot"), autoLoad) { value ->
                autoLoad = value
                prefs.edit().putBoolean("autoLoadOnBoot", value).apply()
            }
            Spacer(Modifier.height(6.dp))
            // Writes the SAME autosave slot the two toggles above use, so a crash and a
            // clean exit leave the state in one predictable place and the numbered slots
            // stay yours.
            IntSliderRow(
                label = str("savestate.autoSaveInterval.label"),
                value = interval,
                min = 0,
                max = AUTOSAVE_INTERVAL_MAX_MIN,
                description = str("savestate.autoSaveInterval.description"),
                valueFormatter = { if (it == 0) offLabel else everyLabel.format(it) },
                onReset = if (interval == 0) null else ({
                    interval = 0
                    prefs.edit().putInt(MainActivityRuntime.KEY_AUTOSAVE_INTERVAL_MIN, 0).apply()
                }),
                onChange = { value ->
                    interval = value
                    prefs.edit().putInt(MainActivityRuntime.KEY_AUTOSAVE_INTERVAL_MIN, value).apply()
                },
            )
        }
    }
}

/** Longest interval offered. Beyond half an hour the feature stops being a safety net and
 *  the slider stops being walkable on a d-pad. */
private const val AUTOSAVE_INTERVAL_MAX_MIN = 30

@Composable
private fun ToggleRow(controllerId: String, label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth()
            .controllerFocusable(
                controllerId,
                onConfirm = { onChange(!checked) },
                onLeft = { onChange(false) },
                onRight = { onChange(true) },
            )
            .clickable { onChange(!checked) },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            label,
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun AutosaveTile(onPick: () -> Unit) {
    val gamePath by produceState<String?>(initialValue = null) {
        value = withContext(Dispatchers.IO) { runCatching { NativeApp.getAutosaveGamePath() }.getOrNull() }
    }
    val stamp by produceState(initialValue = 0L, gamePath) {
        val path = gamePath
        value = if (path.isNullOrEmpty()) 0L else withContext(Dispatchers.IO) {
            runCatching { java.io.File(path).takeIf { it.isFile }?.lastModified() ?: 0L }.getOrDefault(0L)
        }
    }
    val image by produceState<android.graphics.Bitmap?>(initialValue = null) {
        value = withContext(Dispatchers.IO) {
            runCatching {
                val bytes = NativeApp.getAutosaveImage() ?: return@runCatching null
                if (bytes.isEmpty()) null else BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            }.getOrNull()
        }
    }
    TileFrame(
        controllerId = "save.autosave",
        borderColor = Color(0xFFFFB347).copy(alpha = 0.7f),
        backgroundColor = Color(0xFF2F2820),
        onClick = onPick,
    ) {
        image?.let {
            Image(it.asImageBitmap(), str("savestate.autosave.screenshotDesc"),
                Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        }
        BottomLabel(
            title = str("savestate.autosave.title"),
            subtitle = formatSlotStamp(stamp)
                ?: gamePath?.substringAfterLast('/')?.substringBeforeLast('.')
                ?: str("savestate.autosave.savedOnExit"),
            titleColor = Color(0xFFFFB347),
        )
    }
}

@Composable
private fun SlotTile(
    slot: Int,
    mode: SaveMode,
    refreshKey: Int = 0,
    deleteMode: Boolean = false,
    onDelete: ((Int) -> Unit)? = null,
    onPick: (Int) -> Unit,
) {
    val gamePath by produceState<String?>(initialValue = null, slot, refreshKey) {
        value = withContext(Dispatchers.IO) { runCatching { NativeApp.getGamePathSlot(slot) }.getOrNull() }
    }
    val image by produceState<android.graphics.Bitmap?>(initialValue = null, slot, refreshKey) {
        value = withContext(Dispatchers.IO) {
            runCatching {
                val bytes = NativeApp.getImageSlot(slot) ?: return@runCatching null
                if (bytes.isEmpty()) null else BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            }.getOrNull()
        }
    }
    // Same file getGamePathSlot names, stat'd for its mtime. Keyed on gamePath so it re-runs
    // when a save lands in this slot; 0L means "no usable timestamp", never "the epoch".
    val stamp by produceState(initialValue = 0L, gamePath, refreshKey) {
        val path = gamePath
        value = if (path.isNullOrEmpty()) 0L else withContext(Dispatchers.IO) {
            runCatching { java.io.File(path).takeIf { it.isFile }?.lastModified() ?: 0L }.getOrDefault(0L)
        }
    }
    val empty = gamePath.isNullOrEmpty()
    // Load: empty slots disabled. Save: any slot is a valid target. Delete mode overrides both —
    // only an OCCUPIED slot can be deleted, whichever screen you came in on.
    val enabled = if (deleteMode) !empty else (mode == SaveMode.Save || !empty)
    TileFrame(
        controllerId = if (enabled) "save.slot.$slot" else null,
        borderColor = if (deleteMode && !empty) Color(0xFFFFB4A2)
        else MaterialTheme.colorScheme.outline.copy(alpha = 0.6f),
        backgroundColor = MaterialTheme.colorScheme.surfaceVariant,
        enabled = enabled,
        onClick = { onPick(slot) },
        // Long-press an OCCUPIED slot to delete it — the reported gap ("tap and hold does
        // nothing", and deleting otherwise meant digging through a file manager).
        onLongClick = if (!empty && onDelete != null) ({ onDelete(slot) }) else null,
    ) {
        image?.let {
            Image(it.asImageBitmap(), "Slot ${slot + 1}",
                Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        }
        BottomLabel(
            title = "${str("memcard.slot1").substringBefore(' ')} ${slot + 1}",
            subtitle = when {
                !empty -> formatSlotStamp(stamp)
                    ?: gamePath?.substringAfterLast('/')?.substringBeforeLast('.') ?: ""
                mode == SaveMode.Save -> str("savestate.slot.emptyTapToSave")
                else -> null
            },
            titleColor = Color.White,
        )
    }
}

@Composable
private fun TileFrame(
    controllerId: String?,
    borderColor: Color,
    backgroundColor: Color,
    enabled: Boolean = true,
    onClick: () -> Unit,
    onLongClick: (() -> Unit)? = null,
    content: @Composable BoxScope.() -> Unit,
) {
    Box(
        Modifier
            .width(TILE_WIDTH_DP.dp)
            .fillMaxHeight()
            .controllerFocusable(controllerId, RoundedCornerShape(12.dp), onConfirm = { if (enabled) onClick() })
            .clip(RoundedCornerShape(12.dp))
            .background(backgroundColor)
            .border(1.dp, borderColor, RoundedCornerShape(12.dp))
            .combinedClickable(
                enabled = enabled || onLongClick != null,
                onClick = { if (enabled) onClick() },
                onLongClick = onLongClick,
            ),
        content = content,
    )
}

/** Slot timestamp for a tile subtitle, or null when the file gave us nothing usable. */
private fun formatSlotStamp(stamp: Long): String? =
    if (stamp <= 0L) null
    else runCatching {
        java.text.SimpleDateFormat("d MMM yyyy, HH:mm", java.util.Locale.getDefault())
            .format(java.util.Date(stamp))
    }.getOrNull()

@Composable
private fun BoxScope.BottomLabel(title: String, subtitle: String?, titleColor: Color) {
    Box(
        Modifier
            .align(Alignment.BottomStart)
            .fillMaxWidth()
            .background(
                Brush.verticalGradient(
                    0.0f to Color.Transparent,
                    1.0f to Color.Black.copy(alpha = 0.82f),
                ),
            )
            .padding(horizontal = 10.dp, vertical = 8.dp),
    ) {
        Column {
            Text(title, color = titleColor, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            if (!subtitle.isNullOrEmpty()) {
                Spacer(Modifier.height(2.dp))
                Text(
                    subtitle,
                    color = Color(0xFFCFE0FF),
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}
