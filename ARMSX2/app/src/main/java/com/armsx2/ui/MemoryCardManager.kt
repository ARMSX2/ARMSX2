package com.armsx2.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.armsx2.Main
import com.armsx2.config.ConfigStore
import com.armsx2.ui.settings.SettingsControllerNav
import com.armsx2.ui.settings.controllerFocusable
import java.io.File
import java.io.FileOutputStream
import kr.co.iefriends.pcsx2.NativeApp

object MemoryCardManager {
    val visible = mutableStateOf(false)
    private val files = mutableStateListOf<File>()
    private val status = mutableStateOf<String?>(null)
    // Which card file is bound to each slot, so the list can mark the active
    // cards. Read from the persisted global config; updated on assign.
    private val slot1Filename = mutableStateOf("")
    private val slot2Filename = mutableStateOf("")
    private val slot1Enabled = mutableStateOf(false)
    private val slot2Enabled = mutableStateOf(false)

    @Composable
    fun Render() {
        val context = LocalContext.current
        var creating by remember { mutableStateOf(false) }
        var newName by remember { mutableStateOf("") }
        var newType by remember { mutableStateOf(1) }  // 1 = File, 2 = Folder
        var newSize by remember { mutableStateOf(1) }  // 1=8MB 2=16 3=32 4=64
        // Card pending a delete-confirm (file name), so a single tap can't wipe saves.
        var deleteArmed by remember { mutableStateOf<String?>(null) }
        val dialogScroll = rememberScrollState()
        val maxDialogHeight = (LocalConfiguration.current.screenHeightDp * 0.92f).dp
        val nameFocus = remember { FocusRequester() }
        val keyboard = LocalSoftwareKeyboardController.current
        // Manual controller nav (touch mode blocks Compose D-pad focus, so this
        // dialog uses the same state-driven model as the settings tabs). Start
        // with a clean selection each open; the controls unregister on dispose.
        DisposableEffect(Unit) {
            SettingsControllerNav.clearSelection()
            onDispose { SettingsControllerNav.clearSelection() }
        }
        val fileLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.GetContent()
        ) { uri: Uri? ->
            if (uri != null) {
                importCardFile(context, uri)
                refresh(context)
            }
        }
        val folderLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocumentTree()
        ) { uri: Uri? ->
            if (uri != null) {
                importFolder(context, uri)
                refresh(context)
            }
        }

        LaunchedEffect(Unit) {
            refresh(context)
        }

        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.58f)),
            contentAlignment = Alignment.Center,
        ) {
            Column(
                Modifier
                    .fillMaxWidth(0.78f)
                    .heightIn(max = maxDialogHeight)
                    .border(1.dp, Color(0xFF333333), RoundedCornerShape(12.dp))
                    .background(Colors.surface.value, RoundedCornerShape(12.dp))
                    .verticalScroll(dialogScroll)
                    .padding(18.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            "Memory Cards",
                            color = Color.White,
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                        )
                        Text(
                            memcardsDir(context).absolutePath,
                            color = Color(0xFFAAAAAA),
                            fontSize = 11.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    Button(
                        onClick = { visible.value = false },
                        modifier = Modifier.controllerFocusable("mc:close", onConfirm = { visible.value = false }),
                        colors = darkButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Close")
                    }
                }

                Spacer(Modifier.height(14.dp))

                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Button(
                        onClick = { creating = !creating },
                        modifier = Modifier.controllerFocusable("mc:new", onConfirm = { creating = !creating }),
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text(if (creating) "Cancel New" else "+ New Card")
                    }
                    Button(
                        onClick = { fileLauncher.launch("*/*") },
                        modifier = Modifier.controllerFocusable("mc:importfile", onConfirm = { fileLauncher.launch("*/*") }),
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Import File")
                    }
                    Button(
                        onClick = { folderLauncher.launch(null) },
                        modifier = Modifier.controllerFocusable("mc:importfolder", onConfirm = { folderLauncher.launch(null) }),
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Import Folder")
                    }
                    Button(
                        onClick = {
                            ensureDefaultSlots(context)
                            refresh(context)
                        },
                        modifier = Modifier.controllerFocusable("mc:default", onConfirm = {
                            ensureDefaultSlots(context)
                            refresh(context)
                        }),
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Use Default Slots")
                    }
                }

                status.value?.let {
                    Spacer(Modifier.height(10.dp))
                    Text(it, color = Color(0xFFBFD7FF), fontSize = 12.sp)
                }

                if (creating) {
                    Spacer(Modifier.height(12.dp))
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .background(Color(0xFF151515), RoundedCornerShape(8.dp))
                            .padding(12.dp),
                    ) {
                        Text("New Memory Card", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Bold)
                        Spacer(Modifier.height(8.dp))
                        Box(
                            Modifier
                                .fillMaxWidth()
                                .controllerFocusable("mc:name", onConfirm = {
                                    runCatching { nameFocus.requestFocus() }
                                    keyboard?.show()
                                }),
                        ) {
                            OutlinedTextField(
                                value = newName,
                                onValueChange = { newName = it },
                                singleLine = true,
                                label = { Text("Card name (A to type)") },
                                colors = OutlinedTextFieldDefaults.colors(
                                    focusedTextColor = Color.White,
                                    unfocusedTextColor = Color.White,
                                    cursorColor = Color.White,
                                    focusedBorderColor = Colors.pasx2_blue,
                                    unfocusedBorderColor = Color(0xFF444444),
                                    focusedLabelColor = Colors.pasx2_blue,
                                    unfocusedLabelColor = Color(0xFF999999),
                                ),
                                modifier = Modifier.fillMaxWidth().focusRequester(nameFocus),
                            )
                        }
                        Spacer(Modifier.height(10.dp))
                        Text("Type", color = Color(0xFF999999), fontSize = 12.sp)
                        Spacer(Modifier.height(4.dp))
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            SelectChip("File", newType == 1, "mc:typefile") { newType = 1 }
                            SelectChip("Folder", newType == 2, "mc:typefolder") { newType = 2 }
                        }
                        if (newType == 1) {
                            Spacer(Modifier.height(10.dp))
                            Text("Size", color = Color(0xFF999999), fontSize = 12.sp)
                            Spacer(Modifier.height(4.dp))
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                SelectChip("8 MB", newSize == 1, "mc:size8") { newSize = 1 }
                                SelectChip("16 MB", newSize == 2, "mc:size16") { newSize = 2 }
                                SelectChip("32 MB", newSize == 3, "mc:size32") { newSize = 3 }
                                SelectChip("64 MB", newSize == 4, "mc:size64") { newSize = 4 }
                            }
                            Spacer(Modifier.height(4.dp))
                            Text(
                                "8 MB is the most compatible. Larger cards work in many but not all games.",
                                color = Color(0xFF888888),
                                fontSize = 10.sp,
                            )
                        }
                        Spacer(Modifier.height(12.dp))
                        Button(
                            onClick = {
                                if (createCard(context, newName, newType, newSize)) {
                                    creating = false
                                    newName = ""
                                    refresh(context)
                                }
                            },
                            modifier = Modifier.controllerFocusable("mc:create", onConfirm = {
                                if (createCard(context, newName, newType, newSize)) {
                                    creating = false
                                    newName = ""
                                    refresh(context)
                                }
                            }),
                            colors = ps2ButtonColors(),
                            shape = RoundedCornerShape(8.dp),
                        ) {
                            Text("Create")
                        }
                    }
                }

                Spacer(Modifier.height(14.dp))

                if (files.isEmpty()) {
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(96.dp)
                            .background(Color(0xFF151515), RoundedCornerShape(8.dp)),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            "No memory-card files found yet.",
                            color = Color(0xFF999999),
                            fontSize = 13.sp,
                        )
                    }
                } else {
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .background(Color(0xFF151515), RoundedCornerShape(8.dp))
                            .padding(8.dp),
                    ) {
                        files.forEach { file ->
                            val inSlot1 = slot1Enabled.value && file.name.equals(slot1Filename.value, ignoreCase = true)
                            val inSlot2 = slot2Enabled.value && file.name.equals(slot2Filename.value, ignoreCase = true)
                            val active = inSlot1 || inSlot2
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .then(
                                        if (active)
                                            Modifier.background(Color(0xFF14361F), RoundedCornerShape(6.dp))
                                        else Modifier
                                    )
                                    .padding(vertical = 5.dp, horizontal = 6.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(
                                    file.name,
                                    color = Color.White,
                                    fontSize = 13.sp,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f),
                                )
                                if (active) {
                                    Spacer(Modifier.width(6.dp))
                                    Text(
                                        when {
                                            inSlot1 && inSlot2 -> "● SLOTS 1 & 2"
                                            inSlot1 -> "● SLOT 1"
                                            else -> "● SLOT 2"
                                        },
                                        color = Color(0xFF6FCF7F),
                                        fontSize = 10.sp,
                                        fontWeight = FontWeight.Bold,
                                        maxLines = 1,
                                    )
                                }
                                Spacer(Modifier.width(8.dp))
                                Text(
                                    if (file.isDirectory) "Folder" else readableSize(file.length()),
                                    color = Color(0xFFAAAAAA),
                                    fontSize = 11.sp,
                                )
                                Spacer(Modifier.width(8.dp))
                                Button(
                                    onClick = { assignSlot(context, 1, file) },
                                    colors = darkButtonColors(),
                                    shape = RoundedCornerShape(6.dp),
                                    modifier = Modifier
                                        .height(32.dp)
                                        .controllerFocusable("mc:slot1:${file.name}", onConfirm = { assignSlot(context, 1, file) }),
                                ) {
                                    Text(if (inSlot1) "✓ Slot 1" else "Slot 1", fontSize = 11.sp)
                                }
                                Spacer(Modifier.width(6.dp))
                                Button(
                                    onClick = { assignSlot(context, 2, file) },
                                    colors = darkButtonColors(),
                                    shape = RoundedCornerShape(6.dp),
                                    modifier = Modifier
                                        .height(32.dp)
                                        .controllerFocusable("mc:slot2:${file.name}", onConfirm = { assignSlot(context, 2, file) }),
                                ) {
                                    Text(if (inSlot2) "✓ Slot 2" else "Slot 2", fontSize = 11.sp)
                                }
                                Spacer(Modifier.width(6.dp))
                                val armed = deleteArmed == file.name
                                Button(
                                    onClick = {
                                        if (armed) { deleteCard(context, file); deleteArmed = null }
                                        else deleteArmed = file.name
                                    },
                                    colors = ButtonDefaults.buttonColors(
                                        containerColor = if (armed) Color(0xFFC62828) else Color(0xFF2A2A2A),
                                        contentColor = Color.White,
                                    ),
                                    shape = RoundedCornerShape(6.dp),
                                    modifier = Modifier
                                        .height(32.dp)
                                        .controllerFocusable("mc:del:${file.name}", onConfirm = {
                                            if (armed) { deleteCard(context, file); deleteArmed = null }
                                            else deleteArmed = file.name
                                        }),
                                ) {
                                    Text(if (armed) "Sure?" else "Delete", fontSize = 11.sp)
                                }
                            }
                        }
                    }
                }

                Spacer(Modifier.height(12.dp))
                Text(
                    "Import File adds a memory card (then tap Slot 1 or Slot 2 to use it). Import Folder imports .ps2/.mcr cards found in a folder, or the folder itself as a folder memory card. Delete removes a card and clears its slot.",
                    color = Color(0xFF888888),
                    fontSize = 11.sp,
                )
            }
        }
    }

    fun refresh(context: Context) {
        val dir = memcardsDir(context)
        if (!dir.exists()) dir.mkdirs()
        files.clear()
        dir.listFiles()
            ?.filter { it.isFile || it.isDirectory }
            ?.sortedWith(compareBy<File> { !it.name.endsWith(".ps2", ignoreCase = true) }.thenBy { it.name.lowercase() })
            ?.let { files.addAll(it) }
        reconcileOrphanedSlots()
        readSlotState()
    }

    /** Self-heal slot assignments after an OUT-OF-BAND card deletion — i.e. deleting the .ps2 with
     *  a file manager instead of the in-app Delete button (which already runs clearSlot +
     *  restoreDefaultSlot). If an enabled slot still names a card that is no longer on disk, run
     *  that same cleanup here. Otherwise the stale SlotN_Filename lingers, the BIOS recreates the
     *  deleted card empty at boot, and the active-slot marker (which matches by filename) lands on
     *  the wrong card — the "the next card got renamed to the deleted one" report (issue #-memcard).
     *  This makes the external-delete path converge to the same clean state as the in-app Delete. */
    private fun reconcileOrphanedSlots() {
        if (!Main.nativeReady.value) return
        val onDisk = files.map { it.name.lowercase() }.toSet()
        val g = ConfigStore.loadGlobal()
        // Only reconcile a genuinely-orphaned CUSTOM card. A missing DEFAULT card (Mcd00N.ps2) needs
        // no cleanup: the core recreates it at boot and the slot already names it, so the active-slot
        // marker still matches — there is no wrong-card problem to fix. Skipping defaults also avoids
        // redundant config writes before the first boot, when Mcd00N.ps2 don't exist on disk yet.
        fun isDefault(slot: Int, name: String) =
            name.equals(if (slot == 2) "Mcd002.ps2" else "Mcd001.ps2", ignoreCase = true)
        if (g.memoryCardSlot1Enabled && g.memoryCardSlot1Filename.isNotEmpty() &&
            !isDefault(1, g.memoryCardSlot1Filename) &&
            g.memoryCardSlot1Filename.lowercase() !in onDisk) {
            clearSlot(1); restoreDefaultSlot(1)
        }
        if (g.memoryCardSlot2Enabled && g.memoryCardSlot2Filename.isNotEmpty() &&
            !isDefault(2, g.memoryCardSlot2Filename) &&
            g.memoryCardSlot2Filename.lowercase() !in onDisk) {
            clearSlot(2); restoreDefaultSlot(2)
        }
    }

    /** Refresh the cached per-slot bindings shown as the "active" markers. */
    private fun readSlotState() {
        val g = ConfigStore.loadGlobal()
        slot1Filename.value = g.memoryCardSlot1Filename
        slot2Filename.value = g.memoryCardSlot2Filename
        slot1Enabled.value = g.memoryCardSlot1Enabled
        slot2Enabled.value = g.memoryCardSlot2Enabled
    }

    private fun importCardFile(context: Context, uri: Uri) {
        val dir = memcardsDir(context).apply { mkdirs() }
        // Keep the source filename (unique-ified) instead of always writing
        // Mcd001.ps2 — re-importing previously OVERWROTE the first card and forced
        // Slot 1. Now each import is a distinct card the user assigns to a slot.
        val rawName = runCatching { DocumentFile.fromSingleUri(context, uri)?.name }.getOrNull()
        var name = sanitizeFileName(rawName?.takeIf { it.isNotBlank() } ?: "Mcd001.ps2")
        if (!name.endsWith(".ps2", true) && !name.endsWith(".mcr", true)) name += ".ps2"
        val outFile = uniqueFile(dir, name)
        try {
            context.contentResolver.openInputStream(uri)?.use { ins ->
                FileOutputStream(outFile).use { outs -> ins.copyTo(outs) }
            } ?: error("Could not open selected file")
            status.value = "Imported ${outFile.name} — tap Slot 1 or Slot 2 to use it."
            Toast.makeText(context, "Memory card imported", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            status.value = "Import failed: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun importFolder(context: Context, uri: Uri) {
        val dir = memcardsDir(context).apply { mkdirs() }
        try {
            runCatching {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION,
                )
            }

            val source = DocumentFile.fromTreeUri(context, uri)
                ?: error("Could not open selected folder")

            // Common case: the folder holds loose .ps2/.mcr card FILES — import each
            // as its own card (the user expected this, not "wrap the folder as one
            // folder-card"). Only fall back to a folder (directory) memory card when
            // there are no loose card files.
            val cardFiles = source.listFiles().filter {
                it.isFile && (it.name?.endsWith(".ps2", true) == true ||
                    it.name?.endsWith(".mcr", true) == true)
            }
            if (cardFiles.isNotEmpty()) {
                var imported = 0
                for (f in cardFiles) {
                    val out = uniqueFile(dir, sanitizeFileName(f.name ?: continue))
                    val ok = runCatching {
                        context.contentResolver.openInputStream(f.uri)?.use { input ->
                            FileOutputStream(out).use { output -> input.copyTo(output) }
                        } != null
                    }.getOrDefault(false)
                    if (ok && out.isFile && out.length() > 0L) imported++
                    else runCatching { out.delete() } // drop a partial / failed copy
                }
                if (imported == 0) error("Could not read the card files in that folder")
                status.value = "Imported $imported memory card${if (imported == 1) "" else "s"} — assign one to a slot below."
                Toast.makeText(context, status.value, Toast.LENGTH_SHORT).show()
                return
            }

            // No loose card files → import the folder itself as a folder memory card.
            val dest = uniqueChild(dir, sanitizeFileName(source.name ?: "FolderCard"))
            dest.mkdirs()
            val copied = copyDocumentFolder(context, source, dest)
            if (copied == 0)
                error("Selected folder is empty (no card files or folder-card contents)")
            if (assignSlot(context, 1, dest)) {
                status.value = "Imported folder card ${dest.name} to Slot 1."
                Toast.makeText(context, "Folder memory card imported", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            status.value = "Folder import failed: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun ensureDefaultSlots(context: Context) {
        memcardsDir(context).mkdirs()
        try {
            setDefaultSlots()
            status.value = "Default memory-card slots enabled."
            Toast.makeText(context, "Default memory-card slots enabled", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            status.value = "Could not update memory-card settings: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun assignSlot(context: Context, slot: Int, file: File): Boolean {
        if (!Main.nativeReady.value) {
            status.value = "Core settings are still starting up."
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
            return false
        }

        try {
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "false")
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Filename", "string", file.name)
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "true")
            NativeApp.commitSettings()
            persistSlot(slot, file.name)
            readSlotState()
            status.value = "${file.name} assigned to Slot $slot."
            Toast.makeText(context, status.value, Toast.LENGTH_SHORT).show()
            return true
        } catch (e: Exception) {
            status.value = "Could not assign card: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
            return false
        }
    }

    private fun setDefaultSlots() {
        if (!Main.nativeReady.value)
            error("core settings are still starting up")

        NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", "false")
        NativeApp.setSetting("MemoryCards", "Slot1_Filename", "string", "Mcd001.ps2")
        NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", "true")
        NativeApp.setSetting("MemoryCards", "Slot2_Enable", "bool", "false")
        NativeApp.setSetting("MemoryCards", "Slot2_Filename", "string", "Mcd002.ps2")
        NativeApp.setSetting("MemoryCards", "Slot2_Enable", "bool", "true")
        NativeApp.commitSettings()
        persistDefaultSlots()
        readSlotState()
    }

    private fun persistSlot(slot: Int, filename: String) {
        val current = ConfigStore.loadGlobal()
        val updated = when (slot) {
            1 -> current.copy(
                memoryCardSlot1Enabled = true,
                memoryCardSlot1Filename = filename,
            )
            2 -> current.copy(
                memoryCardSlot2Enabled = true,
                memoryCardSlot2Filename = filename,
            )
            else -> current
        }
        ConfigStore.saveGlobal(updated)
    }

    private fun persistDefaultSlots() {
        ConfigStore.saveGlobal(
            ConfigStore.loadGlobal().copy(
                memoryCardSlot1Enabled = true,
                memoryCardSlot1Filename = "Mcd001.ps2",
                memoryCardSlot2Enabled = true,
                memoryCardSlot2Filename = "Mcd002.ps2",
            )
        )
    }

    /** Delete a card file/folder. Any slot pointing at it is cleared FIRST so the
     *  BIOS doesn't recreate an empty card from a now-stale slot assignment (the
     *  "deleted card still shows at BIOS boot" bug). */
    private fun deleteCard(context: Context, file: File) {
        val g = ConfigStore.loadGlobal()
        // Case-insensitive, to match the active-card display + the lowercase
        // default filenames — otherwise the slot isn't cleared and the BIOS
        // recreates the deleted card from the stale slot assignment.
        val emptiedSlots = mutableListOf<Int>()
        if (g.memoryCardSlot1Filename.equals(file.name, ignoreCase = true)) { clearSlot(1); emptiedSlots.add(1) }
        if (g.memoryCardSlot2Filename.equals(file.name, ignoreCase = true)) { clearSlot(2); emptiedSlots.add(2) }
        val ok = runCatching {
            if (file.isDirectory) file.deleteRecursively() else file.delete()
        }.getOrDefault(false)
        // A slot the deleted card occupied is restored to its fresh DEFAULT card
        // (Mcd00N.ps2) instead of being left empty — the user shouldn't have to hit
        // "Use Default Slots" after a delete (matches NetherSX2). Reassigned AFTER the
        // delete so the core regenerates a clean card; the deleted card's own name is
        // gone from every slot, so the BIOS-recreates-the-deleted-card bug stays fixed.
        if (ok && emptiedSlots.isNotEmpty()) {
            emptiedSlots.forEach { restoreDefaultSlot(it) }
            if (Main.nativeReady.value) readSlotState()
        }
        status.value = if (ok) "Deleted ${file.name}." else "Could not delete ${file.name}."
        Toast.makeText(context, status.value, Toast.LENGTH_SHORT).show()
        refresh(context)
    }

    /** Reassign one slot to its fresh default card (Mcd00N.ps2). The core recreates
     *  the file when the slot is enabled, so a slot is never left card-less. */
    private fun restoreDefaultSlot(slot: Int) {
        if (!Main.nativeReady.value) return
        val name = if (slot == 2) "Mcd002.ps2" else "Mcd001.ps2"
        runCatching {
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "false")
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Filename", "string", name)
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "true")
            NativeApp.commitSettings()
        }
        persistSlot(slot, name)
    }

    /** Disable a slot and clear its filename, in both the native config and the
     *  persisted ConfigStore, so nothing references a removed card. */
    private fun clearSlot(slot: Int) {
        runCatching {
            if (Main.nativeReady.value) {
                NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "false")
                NativeApp.setSetting("MemoryCards", "Slot${slot}_Filename", "string", "")
                NativeApp.commitSettings()
            }
        }
        val cur = ConfigStore.loadGlobal()
        val updated = when (slot) {
            1 -> cur.copy(memoryCardSlot1Enabled = false, memoryCardSlot1Filename = "")
            2 -> cur.copy(memoryCardSlot2Enabled = false, memoryCardSlot2Filename = "")
            else -> cur
        }
        ConfigStore.saveGlobal(updated)
        readSlotState()
    }

    private fun memcardsDir(context: Context): File =
        File(Main.assetCopyRoot(context), "memcards")

    private fun copyDocumentFolder(context: Context, source: DocumentFile, dest: File): Int {
        var copied = 0
        for (child in source.listFiles()) {
            val name = sanitizeFileName(child.name ?: continue)
            if (child.isDirectory) {
                val childDest = File(dest, name).apply { mkdirs() }
                copied += copyDocumentFolder(context, child, childDest)
            } else if (child.isFile) {
                context.contentResolver.openInputStream(child.uri)?.use { input ->
                    FileOutputStream(File(dest, name)).use { output ->
                        input.copyTo(output)
                    }
                }
                copied++
            }
        }
        return copied
    }

    private fun uniqueChild(parent: File, requestedName: String): File {
        val base = requestedName.ifBlank { "FolderCard" }
        var candidate = File(parent, base)
        var suffix = 2
        while (candidate.exists()) {
            candidate = File(parent, "$base-$suffix")
            suffix++
        }
        return candidate
    }

    /** Like [uniqueChild] but keeps the file extension (so "Mcd001.ps2" → "Mcd001-2.ps2",
     *  not "Mcd001.ps2-2"). Used for imported card FILES. */
    private fun uniqueFile(parent: File, requestedName: String): File {
        val dot = requestedName.lastIndexOf('.')
        val base = if (dot > 0) requestedName.substring(0, dot) else requestedName
        val ext = if (dot > 0) requestedName.substring(dot) else ""
        var candidate = File(parent, "$base$ext")
        var suffix = 2
        while (candidate.exists()) {
            candidate = File(parent, "$base-$suffix$ext")
            suffix++
        }
        return candidate
    }

    private fun sanitizeFileName(name: String): String =
        name.replace(Regex("""[\\/:*?"<>|]"""), "_").trim().ifBlank { "FolderCard" }

    @Composable
    private fun ps2ButtonColors() = ButtonDefaults.buttonColors(
        containerColor = Colors.pasx2_blue,
        contentColor = Color.White,
    )

    @Composable
    private fun darkButtonColors() = ButtonDefaults.buttonColors(
        containerColor = Color(0xFF2A2A2A),
        contentColor = Color.White,
    )

    @Composable
    private fun SelectChip(label: String, selected: Boolean, id: String, onClick: () -> Unit) {
        Button(
            onClick = onClick,
            colors = if (selected) ps2ButtonColors() else darkButtonColors(),
            shape = RoundedCornerShape(6.dp),
            modifier = Modifier.height(36.dp).controllerFocusable(id, onConfirm = onClick),
        ) {
            Text(label, fontSize = 12.sp)
        }
    }

    /** Create a new card via native FileMcd_CreateNewCard. type: 1=File, 2=Folder;
     *  size (File only): 1=8MB 2=16 3=32 4=64. Returns true on success. */
    private fun createCard(context: Context, rawName: String, type: Int, size: Int): Boolean {
        if (!Main.nativeReady.value) {
            status.value = "Core settings are still starting up."
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
            return false
        }
        val trimmed = rawName.trim()
        if (trimmed.isEmpty()) {
            status.value = "Enter a card name first."
            Toast.makeText(context, status.value, Toast.LENGTH_SHORT).show()
            return false
        }
        val name = if (type == 2) {
            sanitizeFileName(trimmed)
        } else {
            val base = sanitizeFileName(trimmed)
            if (base.endsWith(".ps2", ignoreCase = true)) base else "$base.ps2"
        }
        // Folders ignore the file-type; files map 1..4 → PS2_8/16/32/64MB.
        val fileType = if (type == 2) 1 else size.coerceIn(1, 4)
        return try {
            val ok = NativeApp.createMemoryCard(name, type, fileType)
            if (ok) {
                status.value = "Created $name."
                Toast.makeText(context, "Memory card created", Toast.LENGTH_SHORT).show()
                true
            } else {
                status.value = "Could not create $name (it may already exist)."
                Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
                false
            }
        } catch (e: Exception) {
            status.value = "Create failed: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
            false
        }
    }

    private fun readableSize(bytes: Long): String {
        if (bytes <= 0L) return "-"
        val mib = bytes / (1024.0 * 1024.0)
        return if (mib >= 1.0) "%.1f MB".format(mib) else "${bytes / 1024L} KB"
    }
}
