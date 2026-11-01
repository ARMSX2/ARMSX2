package com.armsx2.ui.saves

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.ConfirmOverlay
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

/** Wrapping grid cells. Headers and the backups row span the whole line, tiles take one cell. */
private sealed interface Cell {
    val key: String

    data class Header(val group: SaveGameGroup) : Cell {
        override val key: String get() = "header:${group.serial}"
    }

    data class Tile(val group: SaveGameGroup, val entry: SaveEntry) : Cell {
        override val key: String get() = "tile:${entry.file.absolutePath}"
    }

    data class Backups(val group: SaveGameGroup, val open: Boolean) : Cell {
        override val key: String get() = "backups:${group.serial}"
    }
}

private fun buildCells(groups: List<SaveGameGroup>, openBackups: Set<String>): List<Cell> = buildList {
    for (group in groups) {
        add(Cell.Header(group))
        group.set.autosaves.forEach { add(Cell.Tile(group, it)) }
        group.set.slots.forEach { add(Cell.Tile(group, it)) }
        if (group.set.backups.isNotEmpty()) {
            val open = group.serial in openBackups
            add(Cell.Backups(group, open))
            if (open) group.set.backups.forEach { add(Cell.Tile(group, it)) }
        }
    }
}

private val AutosaveColor = Color(0xFFFFB347)
private val DeleteColor = Color(0xFFFFB4A2)

/**
 * Load / Manage Saves: every game's save states, grouped under the game's title, as the same
 * thumbnail tiles the in-game picker uses. One tap loads (booting the game first if it is not the
 * one running); a long press, or the trash button's delete mode, deletes. Opened from a game's
 * long-press menu it is the same list, scrolled to that game, so the two entry points can't differ.
 */
@Composable
fun SaveManagerScreen(
    onBack: () -> Unit,
    focusSerial: String? = null,
    viewModel: SaveManagerViewModel = viewModel(),
) {
    val state = viewModel.state.value
    // The view model outlives the screen, so clear what the last visit left on the status lines.
    LaunchedEffect(Unit) {
        viewModel.dismissMessage()
        viewModel.refresh()
    }

    // Delete MODE as in the in-game picker: long-press works but is invisible and a controller
    // cannot do it, so with this armed, choosing a tile deletes it through the normal confirm path.
    var deleteMode by remember { mutableStateOf(false) }
    var pendingDelete by remember { mutableStateOf<Pair<SaveGameGroup, SaveEntry>?>(null) }
    var openBackups by remember { mutableStateOf(setOf<String>()) }
    var importFor by remember { mutableStateOf<SaveGameGroup?>(null) }
    // OpenDocument with "*/*" because save states have no single MIME type and the formats vary;
    // the native loader detects the format by content.
    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val target = importFor
        importFor = null
        if (uri != null && target != null) viewModel.importState(target, uri)
    }

    val gridState = rememberLazyGridState()
    val cells = remember(state.groups, openBackups) { buildCells(state.groups, openBackups) }

    // Opened for one game: bring its section to the top once the list has loaded. Only once, so the
    // list does not jump back after the user scrolls away or a refresh rebuilds it.
    var scrolledToFocus by remember(focusSerial) { mutableStateOf(false) }
    LaunchedEffect(cells, focusSerial) {
        if (scrolledToFocus || focusSerial == null) return@LaunchedEffect
        val at = cells.indexOfFirst { it is Cell.Header && it.group.serial.equals(focusSerial, ignoreCase = true) }
        if (at >= 0) {
            gridState.scrollToItem(at)
            scrolledToFocus = true
        }
    }

    ArmsBackdrop {
        Column(Modifier.fillMaxSize()) {
            ArmsTopBar(
                title = str("savestate.title.loadManage"),
                leading = { RoundAction("←", str("action.back"), onBack, controllerId = "saves.back") },
                actions = {
                    RoundAction(
                        "🗑",
                        str("savestate.delete.mode"),
                        onClick = { deleteMode = !deleteMode; viewModel.dismissMessage() },
                        selected = deleteMode,
                        controllerId = "saves.deleteMode",
                    )
                    RoundAction("↻", str("games.card.refresh"), onClick = viewModel::refresh, controllerId = "saves.refresh")
                },
            )
            Text(
                if (deleteMode) str("savestate.delete.modeHint") else str("savestate.hint"),
                color = if (deleteMode) DeleteColor else Color(0xFF9AA0A6),
                fontSize = 12.sp,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            state.failure?.let {
                Text(
                    it,
                    color = DeleteColor,
                    fontSize = 13.sp,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
                )
            }
            state.notice?.let {
                Text(
                    it,
                    color = Color(0xFF9BE29B),
                    fontSize = 13.sp,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
                )
            }

            when {
                state.loading && state.groups.isEmpty() -> Box(
                    Modifier.fillMaxWidth().height(180.dp),
                    contentAlignment = Alignment.Center,
                ) { CircularProgressIndicator() }

                state.groups.isEmpty() -> Box(Modifier.padding(horizontal = 8.dp)) { SaveManagerEmptyState() }

                else -> LazyVerticalGrid(
                    state = gridState,
                    columns = GridCells.Adaptive(minSize = 170.dp),
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                    contentPadding = PaddingValues(start = 12.dp, end = 12.dp, top = 4.dp, bottom = 28.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    items(
                        cells,
                        key = { it.key },
                        span = { cell -> if (cell is Cell.Tile) GridItemSpan(1) else GridItemSpan(maxLineSpan) },
                    ) { cell ->
                        when (cell) {
                            is Cell.Header -> GroupHeader(
                                group = cell.group,
                                onImport = {
                                    importFor = cell.group
                                    importLauncher.launch(arrayOf("*/*"))
                                },
                                onBackup = { viewModel.backup(cell.group) },
                            )

                            is Cell.Tile -> SaveTile(
                                group = cell.group,
                                entry = cell.entry,
                                deleteMode = deleteMode,
                                onClick = {
                                    if (deleteMode) pendingDelete = cell.group to cell.entry
                                    else viewModel.load(cell.group, cell.entry, viewModel::leaveForGame)
                                },
                                onLongClick = { pendingDelete = cell.group to cell.entry },
                            )

                            is Cell.Backups -> BackupsRow(
                                count = cell.group.set.backups.size,
                                open = cell.open,
                                serial = cell.group.serial,
                                onToggle = {
                                    val serial = cell.group.serial
                                    openBackups = if (cell.open) openBackups - serial else openBackups + serial
                                },
                            )
                        }
                    }
                }
            }
        }
    }

    // ConfirmOverlay rather than a dialog so a controller can reach it. Deleting is the one
    // action here that needs a confirmation; loading and the rest do not.
    pendingDelete?.let { (group, entry) ->
        ConfirmOverlay(
            title = str("savestate.delete.title"),
            message = str("savestate.delete.bodyNamed").format(tileTitle(entry), group.title),
            confirmLabel = str("action.delete"),
            destructive = true,
            idPrefix = "saves.delete",
            onDismiss = { pendingDelete = null },
            onConfirm = {
                pendingDelete = null
                viewModel.delete(entry)
            },
        )
    }
}

@Composable
private fun GroupHeader(group: SaveGameGroup, onImport: () -> Unit, onBackup: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 10.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    group.title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    "${group.serial} · ${group.set.count}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            TextButton(
                onClick = onImport,
                modifier = Modifier.controllerFocusable("saves.import.${group.serial}", onConfirm = onImport),
            ) { Text(str("savestate.import")) }
            TextButton(
                onClick = onBackup,
                modifier = Modifier.controllerFocusable("saves.backup.${group.serial}", onConfirm = onBackup),
            ) { Text(str("savestate.backup")) }
        }
        if (group.game == null) {
            Text(
                str("savestate.notInLibrary"),
                color = AutosaveColor,
                fontSize = 12.sp,
            )
        }
    }
}

@Composable
private fun BackupsRow(count: Int, open: Boolean, serial: String, onToggle: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .controllerFocusable("saves.backups.$serial", onConfirm = onToggle)
            .clickable(onClick = onToggle)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            "${str("savestate.backups")} ($count)",
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(if (open) "▾" else "▸", color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** A tile's name: "Slot 3", "Autosave", "Autosave 2", and "Backup · …" for a backup copy. */
@Composable
private fun tileTitle(entry: SaveEntry): String {
    val base = when (entry.id.kind) {
        SaveKind.Autosave -> {
            val n = entry.id.autosaveIndex ?: 1
            if (n <= 1) str("savestate.autosave.title") else str("savestate.autosave.older").format(n)
        }
        SaveKind.Slot -> "${str("memcard.slot1").substringBefore(' ')} ${(entry.id.slot ?: 0) + 1}"
    }
    return if (entry.isBackup) "${str("savestate.backup")} · $base" else base
}

@Composable
private fun SaveTile(
    group: SaveGameGroup,
    entry: SaveEntry,
    deleteMode: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
) {
    // Decoded as the tile scrolls into view, not for the whole list up front: a library with many
    // games has hundreds of states and decoding every screenshot made opening the screen slow.
    val image by produceState<Bitmap?>(initialValue = null, entry.file.absolutePath, entry.modified) {
        value = withContext(Dispatchers.IO) { decodePreview(entry.file.absolutePath) }
    }
    // A save whose game the library does not have can be deleted but not loaded: there is no
    // path to boot from. The same goes for a backup copy of an autosave. Dimmed so that reads as
    // deliberate, not as a broken tile.
    val loadable = group.game != null && entry.canLoad
    val autosave = entry.id.kind == SaveKind.Autosave && !entry.isBackup
    TileFrame(
        controllerId = if (loadable || deleteMode) "saves.tile.${entry.file.absolutePath}" else null,
        borderColor = when {
            deleteMode -> DeleteColor
            autosave -> AutosaveColor.copy(alpha = 0.7f)
            else -> MaterialTheme.colorScheme.outline.copy(alpha = 0.6f)
        },
        backgroundColor = if (autosave) Color(0xFF2F2820) else MaterialTheme.colorScheme.surfaceVariant,
        enabled = loadable || deleteMode,
        onClick = onClick,
        onLongClick = onLongClick,
        sizing = Modifier
            .fillMaxWidth()
            .aspectRatio(4f / 3f)
            .alpha(if (loadable) 1f else 0.55f),
    ) {
        image?.let {
            Image(
                it.asImageBitmap(),
                tileTitle(entry),
                Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        }
        BottomLabel(
            title = tileTitle(entry),
            subtitle = formatSlotStamp(entry.modified),
            titleColor = if (autosave) AutosaveColor else Color.White,
        )
    }
}

/** The save's screenshot at tile size. Null when the file has none or cannot be read. */
private fun decodePreview(path: String): Bitmap? {
    val bytes = runCatching { NativeApp.getSaveStateImage(path) }.getOrNull() ?: return null
    if (bytes.isEmpty()) return null
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    var sample = 1
    while (bounds.outWidth / sample > 640 || bounds.outHeight / sample > 360) sample *= 2
    return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply {
        inSampleSize = sample
        inPreferredConfig = Bitmap.Config.RGB_565
    })
}

@Composable
private fun SaveManagerEmptyState() {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.42f)),
        tonalElevation = 1.dp,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 18.dp, vertical = 18.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.primaryContainer,
            ) {
                Box(
                    modifier = Modifier.width(48.dp).height(48.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = "▣",
                        style = MaterialTheme.typography.headlineSmall,
                        color = MaterialTheme.colorScheme.onPrimaryContainer,
                    )
                }
            }
            Column(Modifier.weight(1f)) {
                Text(
                    text = str("savestate.empty.title"),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    text = str("savestate.empty.description"),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
