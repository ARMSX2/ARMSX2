package com.armsx2.ui

import android.graphics.BitmapFactory
import android.widget.Toast
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyHorizontalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.Main
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Save-state slot picker, doubling as the per-game Save Manager.
 *
 * 10 numbered slots — matches PCSX2 convention. Each tile previews via
 * `NativeApp.getImageSlot(slot)` (PNG-encoded snapshot) plus the on-disk
 * `.p2s` file from `getGamePathSlot(slot)`. Slots are inherently per-game
 * (the filename encodes serial + CRC), so this whole screen is scoped to
 * the running game.
 *
 * Save mode: tap a tile to write that slot. Load mode = the manager:
 *   - tap a non-empty tile to load it,
 *   - the red ✕ deletes a slot (with confirm),
 *   - Backup snapshots every existing slot into `savestates/backups/`,
 *     Restore copies the last snapshot back (with confirm),
 *   - the "Auto-save on exit" switch (global pref) makes Close Game write
 *     the autosave slot automatically instead of prompting.
 * File ops run in Kotlin against the emulator data folder off the main
 * thread; `refreshTick` re-probes the tiles afterward.
 *
 * Autosave tile (Load mode only, shown above slot 0 when present): the
 * "Save State And Exit" action writes a dedicated `.autosave.p2s` rather
 * than slot 0, so numbered slots stay user-controlled.
 */
object SaveStatePicker {
    enum class Mode { Save, Load }

    private const val SLOTS = 10
    // Fixed tile width — LazyHorizontalGrid computes tile height from the
    // grid's intrinsic height ÷ rows. Width is up to the tile to set.
    private const val TILE_WIDTH_DP = 180

    // ---- File-backed manager ops (savestates live under a MANAGE_EXTERNAL_
    // STORAGE path, so plain File access works) -------------------------------

    /** Directory that holds this game's `.p2s` slots (parent of any slot path). */
    private fun savestateDir(): File? =
        runCatching { File(NativeApp.getGamePathSlot(0)).parentFile }.getOrNull()

    private fun backupDir(): File? = savestateDir()?.let { File(it, "backups") }

    /** The slot's `.p2s` file iff it exists on disk, else null. */
    private fun existingSlotFile(slot: Int): File? =
        runCatching { File(NativeApp.getGamePathSlot(slot)) }.getOrNull()?.takeIf { it.exists() }

    private fun deleteSlot(slot: Int): Boolean =
        runCatching { existingSlotFile(slot)?.delete() ?: false }.getOrDefault(false)

    /** Copy every existing slot for this game into backups/. Returns the count. */
    private fun backupAll(): Int {
        val bdir = backupDir()?.apply { mkdirs() } ?: return 0
        var n = 0
        (0 until SLOTS).forEach { s ->
            val f = existingSlotFile(s) ?: return@forEach
            if (runCatching { f.copyTo(File(bdir, f.name), overwrite = true) }.isSuccess) n++
        }
        return n
    }

    /** Copy the last backup of each slot back over the live slots. */
    private fun restoreAll(): Int {
        val dir = savestateDir() ?: return 0
        val bdir = backupDir() ?: return 0
        var n = 0
        (0 until SLOTS).forEach { s ->
            val name = runCatching { File(NativeApp.getGamePathSlot(s)).name }.getOrNull() ?: return@forEach
            val bak = File(bdir, name)
            if (bak.exists() && runCatching { bak.copyTo(File(dir, name), overwrite = true) }.isSuccess) n++
        }
        return n
    }

    @Composable
    fun Render(mode: Mode, onDone: () -> Unit, onBack: () -> Unit) {
        // Save/load run on Dispatchers.IO — Main.invoke would have queued
        // the task behind the VM thread (single-threaded eDispatcher is
        // permanently blocked inside runVMThread's main loop), so the save
        // would never actually fire. IO pool is a separate thread, JNI
        // handles thread attachment fine. onDone hops back to Main for
        // the overlay state mutation.
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val manage = mode == Mode.Load
        // Bumped after a delete/restore so the slot tiles re-probe disk.
        var refreshTick by remember { mutableStateOf(0) }
        var pendingDelete by remember { mutableStateOf<Int?>(null) }
        var pendingRestore by remember { mutableStateOf(false) }
        // Probe the autosave slot once at composition (Load mode only).
        val hasAutosave by produceState<Boolean>(initialValue = false, mode) {
            value = if (mode == Mode.Load) withContext(Dispatchers.IO) {
                NativeApp.hasAutosaveState()
            } else false
        }

        // Destructive-action confirmations.
        pendingDelete?.let { slot ->
            ConfirmDialog(
                title = "Delete Slot $slot?",
                message = "Permanently removes the save in slot $slot for this game.",
                confirmLabel = "DELETE",
                onConfirm = {
                    pendingDelete = null
                    scope.launch(Dispatchers.IO) {
                        deleteSlot(slot)
                        withContext(Dispatchers.Main) { refreshTick++ }
                    }
                },
                onDismiss = { pendingDelete = null },
            )
        }
        if (pendingRestore) {
            ConfirmDialog(
                title = "Restore backup?",
                message = "Replaces this game's current slots with the last backup. Current saves are overwritten.",
                confirmLabel = "RESTORE",
                onConfirm = {
                    pendingRestore = false
                    scope.launch(Dispatchers.IO) {
                        val n = restoreAll()
                        withContext(Dispatchers.Main) {
                            refreshTick++
                            Toast.makeText(
                                context,
                                if (n > 0) "Restored $n save(s)" else "No backup found",
                                Toast.LENGTH_SHORT,
                            ).show()
                        }
                    }
                },
                onDismiss = { pendingRestore = false },
            )
        }

        Column(Modifier.fillMaxSize()) {
            Text(
                if (mode == Mode.Save) "Save State" else "Load / Manage Saves",
                color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
            )
            if (manage) {
                Spacer(Modifier.height(8.dp))
                ManagerHeader(scope, context) { pendingRestore = true }
            }
            Spacer(Modifier.height(8.dp))
            // 2-row horizontal grid. Autosave (Load mode only) is the
            // leading tile, then numbered slots 0-9 flow column-by-column.
            LazyHorizontalGrid(
                rows = GridCells.Fixed(2),
                contentPadding = PaddingValues(2.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.weight(1f).fillMaxWidth(),
            ) {
                if (mode == Mode.Load && hasAutosave) {
                    item(key = "autosave") {
                        AutosaveTile {
                            scope.launch(Dispatchers.IO) {
                                NativeApp.loadAutosaveState()
                                withContext(Dispatchers.Main) { onDone() }
                            }
                        }
                    }
                }
                items((0 until SLOTS).toList(), key = { "slot_$it" }) { slot ->
                    SlotTile(
                        slot = slot,
                        mode = mode,
                        refreshTick = refreshTick,
                        onPick = { selected ->
                            when (mode) {
                                Mode.Save -> scope.launch(Dispatchers.IO) {
                                    NativeApp.saveStateToSlot(selected)
                                    withContext(Dispatchers.Main) { onDone() }
                                }
                                Mode.Load -> scope.launch(Dispatchers.IO) {
                                    NativeApp.loadStateFromSlot(selected)
                                    withContext(Dispatchers.Main) { onDone() }
                                }
                            }
                        },
                        onDelete = { pendingDelete = it },
                    )
                }
            }
            Spacer(Modifier.height(8.dp))
            BackRow(onBack)
        }
    }

    // Manager controls: auto-save-on-exit switch + Backup / Restore.
    @Composable
    private fun ManagerHeader(
        scope: CoroutineScope,
        context: android.content.Context,
        onRestoreRequest: () -> Unit,
    ) {
        var autoSave by remember { mutableStateOf(Main.prefs.getBoolean("autoSaveOnExit", false)) }
        var autoLoad by remember { mutableStateOf(Main.prefs.getBoolean("autoLoadOnBoot", false)) }
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("Auto-save on exit", color = Color.White, fontSize = 12.sp)
            Spacer(Modifier.width(6.dp))
            Switch(
                checked = autoSave,
                onCheckedChange = {
                    autoSave = it
                    Main.prefs.edit().putBoolean("autoSaveOnExit", it).apply()
                },
            )
            Spacer(Modifier.weight(1f))
            PillButton("Backup") {
                scope.launch(Dispatchers.IO) {
                    val n = backupAll()
                    withContext(Dispatchers.Main) {
                        Toast.makeText(
                            context,
                            if (n > 0) "Backed up $n save(s)" else "No saves to back up",
                            Toast.LENGTH_SHORT,
                        ).show()
                    }
                }
            }
            Spacer(Modifier.width(6.dp))
            PillButton("Restore", onRestoreRequest)
        }
        Row(
            Modifier.fillMaxWidth().padding(top = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Auto-load last state on boot", color = Color.White, fontSize = 12.sp)
            Spacer(Modifier.width(6.dp))
            Switch(
                checked = autoLoad,
                onCheckedChange = {
                    autoLoad = it
                    Main.prefs.edit().putBoolean("autoLoadOnBoot", it).apply()
                },
            )
        }
    }

    @Composable
    private fun PillButton(label: String, onClick: () -> Unit) {
        Box(
            Modifier
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF272525).copy(alpha = 0.6f))
                .border(1.dp, Color(0xFF3A3A3A).copy(alpha = 0.7f), RoundedCornerShape(8.dp))
                .clickable(onClick = onClick)
                .padding(horizontal = 12.dp, vertical = 6.dp),
        ) {
            Text(label, color = Color(0xFFAACCFF), fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        }
    }

    @Composable
    private fun ConfirmDialog(
        title: String,
        message: String,
        confirmLabel: String,
        onConfirm: () -> Unit,
        onDismiss: () -> Unit,
    ) {
        AlertDialog(
            onDismissRequest = onDismiss,
            containerColor = Color(0xFF1B1A1A),
            titleContentColor = Color.White,
            textContentColor = Color.White,
            title = { Text(title, fontWeight = FontWeight.Bold, fontSize = 15.sp) },
            text = { Text(message, fontSize = 13.sp) },
            confirmButton = { TextButton(onClick = onConfirm) { Text(confirmLabel) } },
            dismissButton = { TextButton(onClick = onDismiss) { Text("CANCEL") } },
        )
    }

    @Composable
    private fun AutosaveTile(onPick: () -> Unit) {
        val gamePath by produceState<String?>(initialValue = null) {
            value = withContext(Dispatchers.IO) { NativeApp.getAutosaveGamePath() }
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
            borderColor = Color(0xFFFFB347).copy(alpha = 0.7f),
            backgroundColor = Color(0xFF2F2820).copy(alpha = 0.45f),
            onClick = onPick,
        ) {
            val bmp = image
            if (bmp != null) {
                Image(
                    bitmap = bmp.asImageBitmap(),
                    contentDescription = "Autosave screenshot",
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            }
            BottomLabel(
                title = "Autosave",
                subtitle = gamePath?.substringAfterLast('/')?.substringBeforeLast('.')
                    ?: "(saved on exit)",
                titleColor = Color(0xFFFFB347),
            )
        }
    }

    @Composable
    private fun SlotTile(slot: Int, mode: Mode, refreshTick: Int, onPick: (Int) -> Unit, onDelete: (Int) -> Unit) {
        // Re-probe whenever a delete/restore bumps the tick. File existence is
        // the authoritative "empty" signal (getGamePathSlot builds a name even
        // for empty slots).
        val tick = refreshTick
        val file by produceState<File?>(initialValue = null, slot, tick) {
            value = withContext(Dispatchers.IO) { existingSlotFile(slot) }
        }
        val image by produceState<android.graphics.Bitmap?>(initialValue = null, slot, tick) {
            value = withContext(Dispatchers.IO) {
                runCatching {
                    val bytes = NativeApp.getImageSlot(slot) ?: return@runCatching null
                    if (bytes.isEmpty()) null else BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                }.getOrNull()
            }
        }

        val empty = file == null
        // Load mode: empty slots are disabled. Save mode: tap = overwrite,
        // empty slots are valid targets.
        val enabled = mode == Mode.Save || !empty
        val meta = remember(file) {
            file?.let { f ->
                val date = SimpleDateFormat("dd/MM/yy HH:mm", Locale.getDefault()).format(Date(f.lastModified()))
                val mb = f.length() / 1024.0 / 1024.0
                "$date  •  ${"%.1f".format(mb)} MB"
            }
        }
        TileFrame(
            borderColor = Color(0xFF3A3A3A).copy(alpha = 0.6f),
            backgroundColor = Color(0xFF272525).copy(alpha = 0.3f),
            enabled = enabled,
            onClick = { onPick(slot) },
        ) {
            val bmp = image
            if (bmp != null) {
                Image(
                    bitmap = bmp.asImageBitmap(),
                    contentDescription = "Slot $slot screenshot",
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            }
            BottomLabel(
                title = "Slot $slot",
                subtitle = when {
                    !empty -> meta
                    mode == Mode.Save -> "(empty — tap to save here)"
                    else -> null
                },
                titleColor = Color.White,
            )
            // Delete affordance — manager (Load) mode, non-empty slots only.
            if (mode == Mode.Load && !empty) {
                Box(
                    Modifier
                        .align(Alignment.TopEnd)
                        .padding(6.dp)
                        .size(26.dp)
                        .clip(RoundedCornerShape(13.dp))
                        .background(Color(0xCCB00020))
                        .clickable { onDelete(slot) },
                    contentAlignment = Alignment.Center,
                ) {
                    Text("✕", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }

    // Tile chrome shared by AutosaveTile + SlotTile. Body content is rendered
    // edge-to-edge inside the rounded clip; BottomLabel can be dropped into
    // the same scope to overlay the title/subtitle row over the bottom of
    // the image.
    @Composable
    private fun TileFrame(
        borderColor: Color,
        backgroundColor: Color,
        enabled: Boolean = true,
        onClick: () -> Unit,
        content: @Composable androidx.compose.foundation.layout.BoxScope.() -> Unit,
    ) {
        Box(
            Modifier
                .width(TILE_WIDTH_DP.dp)
                .fillMaxHeight()
                .clip(RoundedCornerShape(8.dp))
                .background(backgroundColor)
                .border(1.dp, borderColor, RoundedCornerShape(8.dp))
                .clickable(enabled = enabled, onClick = onClick),
        ) {
            content()
        }
    }

    // Title + subtitle in the bottom-left of the tile, painted over a
    // left-to-right fade so the text stays readable against any thumbnail.
    // Designed to be invoked inside TileFrame so its Box parent provides
    // BoxScope alignment.
    @Composable
    private fun androidx.compose.foundation.layout.BoxScope.BottomLabel(
        title: String,
        subtitle: String?,
        titleColor: Color,
    ) {
        Box(
            Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth()
                .background(
                    Brush.horizontalGradient(
                        0.0f to Color.Black.copy(alpha = 0.75f),
                        0.6f to Color.Black.copy(alpha = 0.45f),
                        1.0f to Color.Transparent,
                    )
                )
                .padding(horizontal = 8.dp, vertical = 6.dp),
        ) {
            Column {
                Text(
                    title,
                    color = titleColor,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
                if (!subtitle.isNullOrEmpty()) {
                    Spacer(Modifier.height(2.dp))
                    Text(
                        subtitle,
                        color = Color(0xFFAACCFF),
                        fontSize = 10.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }

    @Composable
    private fun BackRow(onBack: () -> Unit) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(40.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF272525).copy(alpha = 0.3f))
                .border(1.dp, Color(0xFF3A3A3A).copy(alpha = 0.6f), RoundedCornerShape(8.dp))
                .clickable(onClick = onBack)
                .padding(horizontal = 14.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text("Back", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}
