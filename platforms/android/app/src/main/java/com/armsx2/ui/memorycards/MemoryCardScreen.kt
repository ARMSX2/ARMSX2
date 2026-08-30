package com.armsx2.ui.memorycards

import androidx.compose.foundation.layout.widthIn
import androidx.compose.ui.text.font.FontWeight
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.GameInfo
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.EmptyState
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.StatusChip
import com.armsx2.ui.settings.controllerFocusable
import com.armsx2.ui.theme.Success
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun MemoryCardScreen(onBack: () -> Unit, game: GameInfo? = null, viewModel: MemoryCardViewModel = viewModel()) {
    val state = viewModel.state.value
    val serial = game?.serial?.takeIf { it.isNotBlank() }
    var createDialog by remember { mutableStateOf(false) }
    var deleteTarget by remember { mutableStateOf<MemoryCardItem?>(null) }
    var backupsTarget by remember { mutableStateOf<MemoryCardItem?>(null) }
    var restoreTarget by remember {
        mutableStateOf<Pair<MemoryCardItem, com.armsx2.MemoryCardBackup.Snapshot>?>(null)
    }
    // Bumped after a snapshot or a restore so the open list re-reads from disk.
    var backupsRevision by remember { mutableStateOf(0) }
    val scope = rememberCoroutineScope()
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> uri?.let(viewModel::import) }
    // Folder memory cards are directories, which OpenDocument() cannot return — without
    // this there was no way to import one at all, and zipping it produced a "card.zip.ps2"
    // that read as unformatted.
    val folderImporter = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri -> uri?.let(viewModel::importFolder) }
    // Export a (file) card out to a user-chosen location — backup, or move to another device.
    var exportPending by remember { mutableStateOf<MemoryCardItem?>(null) }
    val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val src = exportPending; exportPending = null
        if (uri != null && src != null) viewModel.export(src.file, uri)
    }
    LaunchedEffect(Unit) { viewModel.refresh() }

    ArmsBackdrop {
        LazyColumn(
            Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            contentPadding = PaddingValues(horizontal = 8.dp),
        ) {
            item {
                ArmsTopBar(
                    title = str("memcard.title"),
                    leading = { RoundAction("←", str("action.back"), onBack) },
                    actions = {
                        RoundAction("＋", str("memcard.newCard"), { createDialog = true })
                        RoundAction("⇩", str("action.import"), { importer.launch(arrayOf("application/octet-stream", "*/*")) })
                        RoundAction("▣", str("memcard.importFolder"), { folderImporter.launch(null) })
                        RoundAction("↻", str("games.card.refresh"), viewModel::refresh)
                    },
                    horizontalPadding = 0.dp,
                )
            }
            if (state.cards.isEmpty()) {
                item { EmptyState(str("memcard.empty"), str("memcard.size.hint"), str("memcard.create"), { createDialog = true }, Modifier.fillMaxWidth().height(280.dp).controllerFocusable("memcard.empty.create", onConfirm = { createDialog = true })) }
            } else {
                items(state.cards, key = { it.file.absolutePath }) { item ->
                    // Scope-aware: with a game in context the slot buttons write a PER-GAME
                    // override; from the library they set the global default. Previously BOTH
                    // slot buttons were always global and the separate "This game" button was
                    // hardcoded to slot 1 — so "MC2 in slot 2 for this game" was impossible to
                    // express, and every game just kept whatever card was assigned globally last.
                    val gameSlot1 = serial?.let { viewModel.perGameCard(it, 1) }
                    val gameSlot2 = serial?.let { viewModel.perGameCard(it, 2) }
                    MemoryCardRow(
                        item = item,
                        perGame = serial != null,
                        slot1Active = if (serial != null) gameSlot1.equals(item.file.name, true) else item.slot1,
                        slot2Active = if (serial != null) gameSlot2.equals(item.file.name, true) else item.slot2,
                        onSlot1 = { if (serial != null) viewModel.assignToGame(serial, 1, item) else viewModel.assign(1, item) },
                        onSlot2 = { if (serial != null) viewModel.assignToGame(serial, 2, item) else viewModel.assign(2, item) },
                        onClearSlot1 = serial?.let { s -> { viewModel.clearGameCard(s, 1) } },
                        onClearSlot2 = serial?.let { s -> { viewModel.clearGameCard(s, 2) } },
                        onExport = item.takeIf { !it.file.isDirectory }?.let { card -> { exportPending = card; exporter.launch(card.file.name) } },
                        onBackups = { backupsTarget = item },
                        onDelete = { deleteTarget = item },
                    )
                }
            }
            item { Spacer(Modifier.height(12.dp)) }
        }
    }

    if (createDialog) {
        CreateCardDialog(
            onDismiss = { createDialog = false },
            onCreate = { name, type, size -> viewModel.create(name, type, size); createDialog = false },
        )
    }
    deleteTarget?.let { item ->
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("memcard.delete.confirm"),
            message = str("memcard.delete.body").format(item.file.name),
            confirmLabel = str("action.delete"),
            destructive = true,
            idPrefix = "memcard-delete",
            onConfirm = { viewModel.delete(item); deleteTarget = null },
            onDismiss = { deleteTarget = null },
        )
    }
    backupsTarget?.let { item ->
        // Re-read on every revision bump so a snapshot or restore taken from inside this panel is
        // reflected without closing it.
        val snapshots = remember(item.file.absolutePath, backupsRevision) { viewModel.backups(item.file) }
        com.armsx2.ui.common.PadModal(
            key = "memcard-backups",
            onDismiss = { backupsTarget = null },
            initialFocusId = "memcard.backups.close",
        ) {
            Surface(
                modifier = Modifier.padding(24.dp).widthIn(max = 460.dp),
                shape = RoundedCornerShape(20.dp),
                color = MaterialTheme.colorScheme.surface,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
                tonalElevation = 6.dp,
            ) {
                Column(Modifier.padding(20.dp)) {
                    Text(
                        str("memcard.backups.title").format(item.file.name),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Spacer(Modifier.height(12.dp))
                    if (snapshots.isEmpty()) {
                        Text(
                            str("memcard.backups.empty"),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        LazyColumn(
                            Modifier.heightIn(max = 320.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            items(snapshots, key = { it.file.absolutePath }) { snap ->
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Column(Modifier.weight(1f)) {
                                        Text(snap.takenAtText, style = MaterialTheme.typography.bodyMedium)
                                        Text(
                                            listOfNotNull(humanSize(snap.sizeBytes), snap.game)
                                                .joinToString(" · "),
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis,
                                        )
                                    }
                                    if (snap.healthy) {
                                        StatusChip(str("memcard.backups.good"), Success)
                                    } else {
                                        // Kept and shown rather than hidden: the pre-restore copy
                                        // of a broken card is exactly what someone may need back.
                                        Text(
                                            str("memcard.backups.suspect"),
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.error,
                                        )
                                    }
                                    Spacer(Modifier.width(8.dp))
                                    val restore = { restoreTarget = item to snap }
                                    TextButton(
                                        onClick = restore,
                                        modifier = Modifier.controllerFocusable(
                                            "memcard.backups.${snap.file.name}", onConfirm = restore),
                                    ) { Text(str("memcard.backups.restore")) }
                                }
                            }
                        }
                    }
                    Spacer(Modifier.height(12.dp))
                    // The switch lives here rather than in a settings tab because this panel is
                    // where someone goes when they are thinking about backups at all.
                    var autoBackups by remember { mutableStateOf(com.armsx2.MemoryCardBackup.isEnabled()) }
                    com.armsx2.ui.common.SettingSwitchRow(
                        title = str("memcard.backups.auto"),
                        description = str("memcard.backups.auto.help"),
                        checked = autoBackups,
                        onCheckedChange = { autoBackups = it; com.armsx2.MemoryCardBackup.setEnabled(it) },
                    )
                    Spacer(Modifier.height(12.dp))
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        val backupNow = {
                            scope.launch {
                                withContext(Dispatchers.IO) { viewModel.backupNow(item.file) }
                                backupsRevision++
                            }
                            Unit
                        }
                        TextButton(
                            onClick = backupNow,
                            modifier = Modifier.controllerFocusable("memcard.backups.now", onConfirm = backupNow),
                        ) { Text(str("memcard.backups.now")) }
                        Spacer(Modifier.width(8.dp))
                        val close = { backupsTarget = null }
                        TextButton(
                            onClick = close,
                            modifier = Modifier.controllerFocusable("memcard.backups.close", onConfirm = close),
                        ) { Text(str("action.ok")) }
                    }
                }
            }
        }
    }
    restoreTarget?.let { (item, snap) ->
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("memcard.backups.restore.confirm"),
            message = str("memcard.backups.restore.body").format(item.file.name, snap.takenAtText),
            confirmLabel = str("memcard.backups.restore"),
            destructive = true,
            idPrefix = "memcard-restore",
            onConfirm = {
                restoreTarget = null
                scope.launch {
                    withContext(Dispatchers.IO) { viewModel.restoreBackup(snap) }
                    backupsRevision++
                }
            },
            onDismiss = { restoreTarget = null },
        )
    }
    (state.error ?: state.message)?.let { message ->
        com.armsx2.ui.common.NotifyOverlay(
            title = if (state.error != null) str("memcard.title") else str("action.ok"),
            message = message,
            onDismiss = viewModel::dismissMessage,
            idPrefix = "memcard.message",
        )
    }
}

@Composable
private fun MemoryCardRow(
    item: MemoryCardItem,
    perGame: Boolean,
    slot1Active: Boolean,
    slot2Active: Boolean,
    onSlot1: () -> Unit,
    onSlot2: () -> Unit,
    onClearSlot1: (() -> Unit)?,
    onClearSlot2: (() -> Unit)?,
    onExport: (() -> Unit)?,
    onBackups: () -> Unit,
    onDelete: () -> Unit,
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(19.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
    ) {
        Column(Modifier.padding(13.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(if (item.file.isDirectory) "🗀" else "▤", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.headlineMedium)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(item.file.name, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(if (item.file.isDirectory) "Folder" else humanSize(item.size), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // The slot buttons ARE the per-game control when a game is in context, so the
                // old slot-1-only "This game" button is gone. "Use global" undoes a per-game
                // pick — previously there was no way to undo one at all.
                if (slot1Active) StatusChip(str("memcard.slot1.active"), Success) else OutlinedButton(onClick = onSlot1, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.slot1", onConfirm = onSlot1)) { Text(str("memcard.slot1")) }
                if (perGame && slot1Active && onClearSlot1 != null) {
                    Spacer(Modifier.width(7.dp))
                    TextButton(onClick = onClearSlot1, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.slot1.global", onConfirm = onClearSlot1)) { Text(str("memcard.useGlobal")) }
                }
                Spacer(Modifier.width(7.dp))
                if (slot2Active) StatusChip(str("memcard.slot2.active"), Success) else OutlinedButton(onClick = onSlot2, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.slot2", onConfirm = onSlot2)) { Text(str("memcard.slot2")) }
                if (perGame && slot2Active && onClearSlot2 != null) {
                    Spacer(Modifier.width(7.dp))
                    TextButton(onClick = onClearSlot2, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.slot2.global", onConfirm = onClearSlot2)) { Text(str("memcard.useGlobal")) }
                }
                if (onExport != null) {
                    Spacer(Modifier.width(7.dp))
                    TextButton(onClick = onExport, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.export", onConfirm = onExport)) { Text(str("action.export")) }
                }
                Spacer(Modifier.width(7.dp))
                TextButton(onClick = onBackups, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.backups", onConfirm = onBackups)) { Text(str("memcard.backups")) }
                TextButton(onClick = onDelete, enabled = !item.slot1 && !item.slot2, modifier = Modifier.controllerFocusable("memcard.${item.file.name}.delete", onConfirm = onDelete)) { Text(str("action.delete")) }
            }
        }
    }
}

@Composable
private fun CreateCardDialog(onDismiss: () -> Unit, onCreate: (String, Int, Int) -> Unit) {
    var name by remember { mutableStateOf("MemoryCard") }
    var size by remember { mutableIntStateOf(1) }
    var type by remember { mutableIntStateOf(1) } // 1 = File, 2 = Folder
    val nameLabel = str("memcard.cardName.label")
    // Live update, not "closing the keyboard is the done signal": this panel outlives the
    // keyboard, so the draft name has to be visible on the row behind it while you type.
    val editName = { com.armsx2.ui.home.LibraryKeyboard.open(name, { name = it }, nameLabel) }
    com.armsx2.ui.common.PadModal(
        key = "memcard-create",
        onDismiss = onDismiss,
        initialFocusId = "memcard.create.name",
    ) {
      Surface(
        modifier = Modifier
            .padding(24.dp)
            .widthIn(max = 420.dp),
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
        tonalElevation = 6.dp,
      ) {
        Column(Modifier.padding(20.dp)) {
                Text(
                    str("memcard.newCard.title"),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(12.dp))
                Surface(
                    onClick = editName,
                    modifier = Modifier
                        .fillMaxWidth()
                        .controllerFocusable("memcard.create.name", RoundedCornerShape(12.dp), onConfirm = editName),
                    shape = RoundedCornerShape(12.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant,
                ) {
                    Column(Modifier.padding(horizontal = 12.dp, vertical = 10.dp)) {
                        Text(nameLabel, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(name, style = MaterialTheme.typography.bodyLarge)
                    }
                }
                Spacer(Modifier.height(12.dp))
                Text(str("memcard.type.label"))
                Row(horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                    listOf(1 to str("memcard.type.file"), 2 to str("memcard.type.folder")).forEach { (id, label) ->
                        Surface(onClick = { type = id }, modifier = Modifier.controllerFocusable("memcard.create.type$id", RoundedCornerShape(10.dp), onConfirm = { type = id }), shape = RoundedCornerShape(10.dp), color = if (type == id) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant) {
                            Text(label, Modifier.padding(horizontal = 10.dp, vertical = 8.dp))
                        }
                    }
                }
                if (type == 1) {
                    Spacer(Modifier.height(12.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                        listOf(1 to "8 MB", 2 to "16 MB", 3 to "32 MB", 4 to "64 MB").forEach { (id, label) ->
                            Surface(onClick = { size = id }, modifier = Modifier.controllerFocusable("memcard.create.size$id", RoundedCornerShape(10.dp), onConfirm = { size = id }), shape = RoundedCornerShape(10.dp), color = if (size == id) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant) {
                                Text(label, Modifier.padding(horizontal = 10.dp, vertical = 8.dp))
                            }
                        }
                    }
                }
                Spacer(Modifier.height(18.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    TextButton(onClick = onDismiss, modifier = Modifier.controllerFocusable("memcard.create.cancel", onConfirm = onDismiss)) { Text(str("action.cancel")) }
                    Spacer(Modifier.width(8.dp))
                    Button(onClick = { onCreate(name, type, size) }, modifier = Modifier.controllerFocusable("memcard.create", onConfirm = { onCreate(name, type, size) })) { Text(str("memcard.create")) }
                }
        }
      }
    }
}

private fun humanSize(bytes: Long): String = if (bytes >= 1024L * 1024L) "%.1f MB".format(bytes / (1024f * 1024f)) else "${bytes / 1024L} KB"
