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

    @Composable
    fun Render() {
        val context = LocalContext.current
        var creating by remember { mutableStateOf(false) }
        var newName by remember { mutableStateOf("") }
        var newType by remember { mutableStateOf(1) }  // 1 = File, 2 = Folder
        var newSize by remember { mutableStateOf(1) }  // 1=8MB 2=16 3=32 4=64
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
                importSlot1(context, uri)
                refresh(context)
            }
        }
        val folderLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocumentTree()
        ) { uri: Uri? ->
            if (uri != null) {
                importFolderCardSlot1(context, uri)
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
                    Button(
                        onClick = { refresh(context) },
                        modifier = Modifier.controllerFocusable("mc:refresh", onConfirm = { refresh(context) }),
                        colors = darkButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Refresh")
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
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .padding(vertical = 5.dp),
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
                                    Text("Slot 1", fontSize = 11.sp)
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
                                    Text("Slot 2", fontSize = 11.sp)
                                }
                            }
                        }
                    }
                }

                Spacer(Modifier.height(12.dp))
                Text(
                    "File imports are copied to Mcd001.ps2. Folder imports are copied as folder cards. Existing .ps2/.mcr files and folder cards can be assigned to either slot.",
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
    }

    private fun importSlot1(context: Context, uri: Uri) {
        val dir = memcardsDir(context).apply { mkdirs() }
        val outFile = File(dir, "Mcd001.ps2")
        try {
            context.contentResolver.openInputStream(uri)?.use { ins ->
                FileOutputStream(outFile).use { outs -> ins.copyTo(outs) }
            } ?: error("Could not open selected file")
            setDefaultSlots()
            status.value = "Imported Slot 1 as ${outFile.name}."
            Toast.makeText(context, "Memory card imported", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            status.value = "Import failed: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun importFolderCardSlot1(context: Context, uri: Uri) {
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
            val dest = uniqueChild(dir, sanitizeFileName(source.name ?: "FolderCard"))
            dest.mkdirs()

            val copied = copyDocumentFolder(context, source, dest)
            if (copied == 0)
                error("Selected folder did not contain any files")

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
