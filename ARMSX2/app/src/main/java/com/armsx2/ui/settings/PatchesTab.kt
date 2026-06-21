package com.armsx2.ui.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Checkbox
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.rememberCoroutineScope
import com.armsx2.EmuState
import com.armsx2.Main
import com.armsx2.PatchRepo
import com.armsx2.config.Settings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

private data class PnachGameId(val serial: String, val crc: String) {
    val prefix: String get() = "${serial}_${crc}"
}

private val activeGameIdRegex = Regex("""([A-Za-z]{4}-\d{5})\s*\(([0-9A-Fa-f]{8})\)""")
private val serialRegex = Regex("""([A-Za-z]{4})[\s_-]?(\d{3})\.?(\d{2})""")
private val crcRegex = Regex("""(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])""")
private val loadableSerialPnachRegex = Regex("""^[A-Z]{4}-\d{5}_[0-9A-F]{8}.*\.pnach$""", RegexOption.IGNORE_CASE)
private val loadableCrcPnachRegex = Regex("""^[0-9A-F]{8}.*\.pnach$""", RegexOption.IGNORE_CASE)

private fun normalizeSerial(value: String?): String? {
    if (value.isNullOrBlank()) return null
    val match = serialRegex.find(value) ?: return null
    return "${match.groupValues[1].uppercase()}-${match.groupValues[2]}${match.groupValues[3]}"
}

private fun activePnachGameId(): PnachGameId? {
    val pauseLabel = runCatching { NativeApp.getPauseGameSerial() }.getOrNull().orEmpty()
    val currentCrc = runCatching { NativeApp.getGameCRC() }.getOrNull()
        ?.trim()
        ?.uppercase()
        ?.takeIf { crcRegex.matches(it) && it != "00000000" }
    activeGameIdRegex.find(pauseLabel)?.let { match ->
        val crc = currentCrc ?: match.groupValues[2].uppercase()
        if (crc != "00000000")
            return PnachGameId(normalizeSerial(match.groupValues[1]) ?: return null, crc)
    }

    val serial = normalizeSerial(Main.currentGame.value?.serial)
        ?: normalizeSerial(runCatching { NativeApp.getGameSerial() }.getOrNull())
    val crc = currentCrc ?: crcRegex.find(pauseLabel)?.groupValues?.get(1)?.uppercase()
        ?.takeIf { it != "00000000" }
    return if (serial != null && crc != null) PnachGameId(serial, crc) else null
}

private fun safePnachStem(rawName: String): String {
    val base = rawName.substringBeforeLast('.')
        .replace(Regex("""[^A-Za-z0-9._-]+"""), "_")
        .trim('_', '.', '-')
        .take(48)
    return base.ifEmpty { "manual" }
}

private fun loadablePnachName(fileName: String): Boolean =
    loadableSerialPnachRegex.matches(fileName) || loadableCrcPnachRegex.matches(fileName)

private fun importPnachFileName(sourceName: String, gameId: PnachGameId?): String {
    val stem = safePnachStem(sourceName)
    if (gameId != null) {
        return if (stem.startsWith(gameId.prefix, ignoreCase = true))
            "$stem.pnach"
        else
            "${gameId.prefix}_${stem}.pnach"
    }

    val serial = normalizeSerial(sourceName)
    val crc = crcRegex.find(sourceName)?.groupValues?.get(1)?.uppercase()
    return when {
        serial != null && crc != null -> "${serial}_${crc}_${stem}.pnach"
        crc != null -> "${crc}_${stem}.pnach"
        sourceName.endsWith(".pnach", ignoreCase = true) -> sourceName
        else -> "$stem.pnach"
    }
}

private fun manualPnachFileName(title: String, gameId: PnachGameId?): String {
    val stem = safePnachStem(title.ifBlank { "manual" })
    return if (gameId != null) "${gameId.prefix}_${stem}.pnach" else "$stem.pnach"
}

private fun pnachTargetFile(dir: File, desiredName: String): File {
    val normalizedName = if (desiredName.endsWith(".pnach", ignoreCase = true)) desiredName else "$desiredName.pnach"
    return File(dir, normalizedName)
}

private fun executablePnachBody(body: String): String =
    body.trim().lines()
        .mapNotNull { rawLine ->
            val line = rawLine.trimEnd()
            val trimmed = line.trim()
            // Android's importer/entry UI means "run this code". PCSX2 treats
            // labelled groups like [60 FPS] as disabled until the label is
            // added to Cheats/Enable, so flatten labels into comments and let
            // patch= lines auto-activate as unlabelled legacy PNACH commands.
            if (trimmed.length > 2 && trimmed.first() == '[' && trimmed.last() == ']')
                "// $trimmed"
            else
                line
        }
        .joinToString("\n")
        .trim()

private fun manualPnachContents(title: String, body: String, gameId: PnachGameId?): String {
    val header = buildList {
        add("// ARMSX2 manual PNACH")
        if (title.isNotBlank()) add("// $title")
        if (gameId != null) add("// ${gameId.serial} ${gameId.crc}")
    }.joinToString("\n")
    val normalizedBody = executablePnachBody(body)
    return "$header\n$normalizedBody\n"
}

/**
 * Patch / cheat toggles + a PNACH importer.
 *
 * Widescreen / no-interlacing come from the bundled patch database (just toggle
 * them). User cheats are `.pnach` files dropped into <dataRoot>/cheats/ — import
 * them here, enable "Cheats (PNACH)", and restart the game. The importer names
 * files from the active game when possible so emucore can find them at boot.
 */
@Composable
fun PatchesTab(state: MutableState<Settings>) {
    val s = state.value
    val context = LocalContext.current
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)
    val activeGameId = activePnachGameId()

    val cheatsDir = remember { File(Main.assetCopyRoot(context), "cheats").apply { mkdirs() } }
    // Loose patches load from <DataRoot>/patches (EmuFolders::Patches); cheats
    // from <DataRoot>/cheats. Downloaded files land in the matching dir.
    val patchesDir = remember { File(Main.assetCopyRoot(context), "patches").apply { mkdirs() } }
    // List both folders so the manager shows everything that's installed (and
    // lets the user confirm a browse actually wrote files).
    fun listPnach(): List<File> =
        listOf(patchesDir, cheatsDir)
            .flatMap { it.listFiles()?.toList() ?: emptyList() }
            .filter { it.isFile && it.name.endsWith(".pnach", ignoreCase = true) }
            .sortedBy { it.name.lowercase() }
    var pnachFiles: List<File> by remember { mutableStateOf(listPnach()) }
    fun refresh() { pnachFiles = listPnach() }
    var pnachStatus by remember { mutableStateOf("") }
    var showManualDialog by remember { mutableStateOf(false) }
    var showPatchWarning by remember { mutableStateOf(false) }
    var downloading by remember { mutableStateOf(false) }
    var browseResult by remember { mutableStateOf<PatchRepo.Result?>(null) }
    // Game the open browser results belong to (running game, or a library game
    // picked before launch). Drives the saved file name so emucore loads it.
    var browseGameId by remember { mutableStateOf<PnachGameId?>(null) }
    val selected = remember { mutableStateMapOf<Int, Boolean>() } // entry index -> checked
    val scope = rememberCoroutineScope()
    // Game whose settings were opened from the library via long-press (null
    // when a game is actually running). Lets us browse before booting.
    val libraryGame = InGameOverlay.patchPreviewGame

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)
    fun activateCheatsAndReload(): Int {
        if (!state.value.enableCheats)
            apply(state.value.copy(enableCheats = true))

        // The overlay's fast live-settings path intentionally skips patch
        // toggles, so push this one directly before reloading PNACH files.
        NativeApp.setSetting("EmuCore", "EnableCheats", "bool", "true")
        NativeApp.commitSettings()
        return NativeApp.reloadPatches()
    }

    fun pnachResultMessage(action: String, savedName: String, activeCheats: Int): String =
        when {
            Main.eState.value == EmuState.STOPPED ->
                "$action $savedName. Cheats are enabled; start the game to load it."
            activeCheats > 0 ->
                "$action $savedName. $activeCheats active cheat patch${if (activeCheats == 1) "" else "es"}."
            activeCheats == 0 ->
                "$action $savedName, but 0 cheats are active. Check PNACH syntax/CRC, then restart."
            else ->
                "$action $savedName. Reload skipped; restart the game to load it."
        }

    // Fetch + parse this game's patch/cheat entries from the PCSX2 database,
    // then open the per-entry browser dialog. Works two ways:
    //   • in-game  — exact serial+CRC are known, fetch the file directly.
    //   • library  — a game was long-pressed but not booted; we only have its
    //                serial, so look it up in the repo file tree by name (the
    //                CRC comes back from the matched filename).
    fun startBrowse() {
        val running = activePnachGameId()
        val librarySerial = libraryGame?.serial?.takeIf { it.isNotBlank() }
        if ((running == null || running.crc.isBlank()) && librarySerial == null) {
            pnachStatus = "Boot the game, or long-press it in your library, to browse its patches."
            return
        }
        downloading = true
        pnachStatus = "Searching the PCSX2 patch database…"
        scope.launch(Dispatchers.IO) {
            val res = if (running != null && running.crc.isNotBlank())
                PatchRepo.fetchForGame(running.serial, running.crc)
            else
                PatchRepo.fetchForSerial(librarySerial)
            val gid = res.takeIf { it.error == null && it.crc.isNotBlank() }
                ?.let { PnachGameId(it.serial.ifBlank { librarySerial ?: running?.serial ?: "" }, it.crc) }
                ?: running
            withContext(Dispatchers.Main) {
                downloading = false
                if (res.error != null) {
                    pnachStatus = res.error
                } else {
                    selected.clear()
                    browseGameId = gid
                    browseResult = res
                    pnachStatus = ""
                }
            }
        }
    }

    // AetherSX2-style gate: warn before browsing/enabling patch codes unless the
    // user opted out ("Don't ask again").
    fun onDownloadClick() {
        if (downloading) return
        if (Main.prefs.getBoolean("patchCodesWarnAck", false)) startBrowse()
        else showPatchWarning = true
    }

    // Write ONLY the checked entries to disk, with their [labels] flattened to
    // comments so the patch= lines auto-run as unlabelled legacy PNACH. PCSX2
    // auto-enables unlabelled patches (Patch.cpp), so this activates exactly the
    // selected items and persists across reset — no [Patches]/[Cheats] "Enable"
    // list needed (that path doesn't survive on Android). Deselecting all for a
    // category removes its file. This is the same mechanism the manual importer
    // uses, which is why it reliably takes effect.
    fun applySelected() {
        val res = browseResult ?: return
        val gid = browseGameId
        val chosen = res.entries.filterIndexed { i, _ -> selected[i] == true }
        browseResult = null
        val base = gid?.let { if (it.serial.isNotBlank()) "${it.serial}_${it.crc}" else it.crc }
            ?.ifBlank { null } ?: "patch"
        var anyCheatChosen = false
        val saved = runCatching {
            listOf("patches", "cheats").forEach { source ->
                val picked = chosen.filter { it.source == source }
                val dir = if (source == "cheats") cheatsDir else patchesDir
                val file = File(dir, "$base.pnach")
                if (picked.isEmpty()) {
                    file.delete() // nothing selected here -> turn it off
                    return@forEach
                }
                if (source == "cheats") anyCheatChosen = true
                file.writeText(buildString {
                    if (res.gametitle.isNotEmpty()) append("gametitle=").append(res.gametitle).append("\n\n")
                    picked.forEach { append(executablePnachBody(it.body)).append("\n\n") }
                })
            }
        }
        if (saved.isFailure) {
            pnachStatus = "Save failed: ${saved.exceptionOrNull()?.message ?: "unknown error"}"
            return
        }
        if (!state.value.enablePatches) apply(state.value.copy(enablePatches = true))
        NativeApp.setSetting("EmuCore", "EnablePatches", "bool", "true")
        val active = if (anyCheatChosen) activateCheatsAndReload() else {
            NativeApp.commitSettings()
            NativeApp.reloadPatches()
        }
        pnachStatus = when {
            chosen.isEmpty() -> "Cleared all patches for ${gid?.serial ?: "this game"}."
            Main.eState.value == EmuState.STOPPED ->
                "Saved ${chosen.size} item${if (chosen.size == 1) "" else "s"} for ${gid?.serial ?: "this game"}. Start the game to load them."
            else ->
                "Enabled ${chosen.size} item${if (chosen.size == 1) "" else "s"} ($active live). Restart the game to (re)load boot-time patches."
        }
        refresh()
    }

    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        val name = DocumentFile.fromSingleUri(context, uri)?.name
            ?.takeIf { it.isNotEmpty() }
            ?: "imported.pnach"
        val outName = importPnachFileName(name, activePnachGameId())
        val result = runCatching {
            val outFile = pnachTargetFile(cheatsDir, outName)
            val importedText = context.contentResolver.openInputStream(uri)?.use { ins ->
                ins.reader().readText()
            } ?: error("could not open selected file")
            outFile.writeText(manualPnachContents(name, importedText, activePnachGameId()))
            val activeCheats = activateCheatsAndReload()
            outFile.name to activeCheats
        }
        pnachStatus = result.fold(
            onSuccess = { (savedName, activeCheats) ->
                if (activePnachGameId() != null || loadablePnachName(savedName))
                    pnachResultMessage("Imported as", savedName, activeCheats)
                else
                    "Imported as $savedName, but it may need SERIAL_CRC naming to load."
            },
            onFailure = { "Import failed: ${it.message ?: "unknown error"}" },
        )
        refresh()
    }

    if (showManualDialog) {
        ManualPnachDialog(
            gameId = activeGameId,
            onDismiss = { showManualDialog = false },
            onSave = { title, body ->
                val result = runCatching {
                    val outFile = pnachTargetFile(cheatsDir, manualPnachFileName(title, activePnachGameId()))
                    outFile.writeText(manualPnachContents(title, body, activePnachGameId()))
                    val activeCheats = activateCheatsAndReload()
                    outFile.name to activeCheats
                }
                pnachStatus = result.fold(
                    onSuccess = { (savedName, activeCheats) ->
                        if (activePnachGameId() != null || loadablePnachName(savedName))
                            pnachResultMessage("Executed", savedName, activeCheats)
                        else
                            "Saved $savedName, but it may need SERIAL_CRC naming to load."
                    },
                    onFailure = { "Save failed: ${it.message ?: "unknown error"}" },
                )
                refresh()
                showManualDialog = false
            },
        )
    }

    if (showPatchWarning) {
        AlertDialog(
            onDismissRequest = { showPatchWarning = false },
            containerColor = Colors.surfaceColor,
            titleContentColor = Color.White,
            textContentColor = Color.White,
            title = { Text("Patch codes") },
            text = {
                Text(
                    "Using patch codes can have unpredictable effects on games, causing " +
                        "crashes, graphical glitches, and corrupted saves. By using patch " +
                        "codes, you agree that it is an unsupported configuration, and we " +
                        "will not provide you with any assistance when games break.\n\n" +
                        "Some codes persist through save states even after being disabled, " +
                        "please remember to reset/reboot the game after turning off any " +
                        "codes.\n\nAre you sure you want to continue?",
                    fontSize = 13.sp,
                )
            },
            confirmButton = {
                TextButton(onClick = { showPatchWarning = false; startBrowse() }) { Text("YES") }
            },
            dismissButton = {
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(onClick = {
                        Main.prefs.edit().putBoolean("patchCodesWarnAck", true).apply()
                        showPatchWarning = false
                        startBrowse()
                    }) { Text("DON'T ASK AGAIN") }
                    TextButton(onClick = { showPatchWarning = false }) { Text("NO") }
                }
            },
        )
    }

    browseResult?.let { res ->
        AlertDialog(
            onDismissRequest = { browseResult = null },
            containerColor = Colors.surfaceColor,
            titleContentColor = Color.White,
            textContentColor = Color.White,
            title = {
                Text(
                    if (res.gametitle.isNotEmpty()) res.gametitle else "Patches & cheats",
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                )
            },
            text = {
                Column(modifier = Modifier.heightIn(max = 380.dp).verticalScroll(rememberScrollState())) {
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        TextButton(onClick = { res.entries.indices.forEach { selected[it] = true } }) { Text("Select all") }
                        TextButton(onClick = { selected.clear() }) { Text("None") }
                    }
                    res.entries.forEachIndexed { i, e ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { selected[i] = !(selected[i] ?: false) }
                                .padding(vertical = 2.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Checkbox(checked = selected[i] == true, onCheckedChange = { selected[i] = it })
                            Column(modifier = Modifier.weight(1f).padding(start = 4.dp)) {
                                Text(e.name, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = Color.White)
                                Text(
                                    listOfNotNull(e.description.takeIf { it.isNotEmpty() }, "[${e.source}]").joinToString("  •  "),
                                    fontSize = 10.sp,
                                    color = Color(0xFF9A9A9A),
                                    maxLines = 2,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { applySelected() }) { Text("APPLY") } },
            dismissButton = { TextButton(onClick = { browseResult = null }) { Text("CANCEL") } },
        )
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            "Patches apply at boot — restart the game after changing these.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        ToggleRow("Enable Patches", s.enablePatches) { apply(s.copy(enablePatches = it)) }
        SettingsDivider()
        ToggleRow("Widescreen (16:9) Patches", s.enableWideScreenPatches) {
            apply(s.copy(enableWideScreenPatches = it))
        }
        SettingsDivider()
        ToggleRow("No-Interlacing Patches", s.enableNoInterlacingPatches) {
            apply(s.copy(enableNoInterlacingPatches = it))
        }
        SettingsDivider()
        ToggleRow("Cheats (PNACH)", s.enableCheats) { apply(s.copy(enableCheats = it)) }
        SettingsDivider()

        // ---- PNACH importer ----
        Text(
            "Installed patches & cheats (.pnach)",
            color = Color.White,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
        )
        Text(
            when {
                activeGameId != null -> "Active game: ${activeGameId.serial} / CRC ${activeGameId.crc}"
                libraryGame?.serial?.isNotBlank() == true -> "Selected game: ${libraryGame.serial} — browse its patches below."
                else -> "Start a game, or long-press one in your library, to browse its patches."
            },
            color = Color(0xFF8C8C8C),
            fontSize = 10.sp,
            modifier = Modifier.padding(bottom = 4.dp),
        )
        Text(
            "Paste/import PCSX2 PNACH patch= lines. Hardcore achievements disables cheats.",
            color = Color(0xFF8C8C8C),
            fontSize = 10.sp,
            modifier = Modifier.padding(bottom = 4.dp),
        )
        // Online fetch from the PCSX2 patch database (mirrors the GPU driver
        // downloader). Gated by the AetherSX2 patch-codes warning.
        Box(
            Modifier
                .fillMaxWidth()
                .height(36.dp)
                .background(rowAura())
                .controllerFocusable(
                    controllerId = "patch:browse",
                    onConfirm = { if (!downloading) onDownloadClick() },
                )
                .clickable(enabled = !downloading) { onDownloadClick() }
                .padding(horizontal = 8.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text(
                if (downloading) "Fetching…" else "⤓  Browse patches & cheats online",
                color = Colors.pasx2_blue,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
            )
        }
        Spacer(Modifier.height(6.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Box(
                Modifier
                    .weight(1f)
                    .height(36.dp)
                    .background(rowAura())
                    .controllerFocusable(
                        controllerId = "patch:import",
                        onConfirm = { importLauncher.launch(arrayOf("*/*")) },
                    )
                    .clickable { importLauncher.launch(arrayOf("*/*")) }
                    .padding(horizontal = 8.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                Text("+ Import .pnach", color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            }
            Box(
                Modifier
                    .weight(1f)
                    .height(36.dp)
                    .background(rowAura())
                    .controllerFocusable(
                        controllerId = "patch:enter",
                        onConfirm = { showManualDialog = true },
                    )
                    .clickable { showManualDialog = true }
                    .padding(horizontal = 8.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                Text("+ Enter codes", color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            }
        }
        if (pnachStatus.isNotEmpty()) {
            Text(
                pnachStatus,
                color = Color(0xFFB0B0B0),
                fontSize = 10.sp,
                modifier = Modifier.padding(vertical = 4.dp, horizontal = 4.dp),
            )
        }
        if (pnachFiles.isEmpty()) {
            Text(
                "No patch or cheat files installed yet.",
                color = Color(0xFF8C8C8C),
                fontSize = 11.sp,
                modifier = Modifier.padding(vertical = 4.dp, horizontal = 4.dp),
            )
        } else {
            pnachFiles.forEach { file ->
                val kind = if (file.parentFile?.name == "cheats") "cheat" else "patch"
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(30.dp)
                        .padding(horizontal = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "[$kind] ${file.name}",
                        color = Color(0xFFCCCCCC),
                        fontSize = 11.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        "Delete",
                        color = Color(0xFFFF6B6B),
                        fontSize = 11.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable {
                                runCatching { file.delete() }
                                refresh()
                            }
                            .padding(start = 8.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun ManualPnachDialog(
    gameId: PnachGameId?,
    onDismiss: () -> Unit,
    onSave: (title: String, body: String) -> Unit,
) {
    var title by remember { mutableStateOf("") }
    var body by remember { mutableStateOf("") }
    fun execute() {
        if (body.isNotBlank())
            onSave(title, body)
    }
    val tfColors = TextFieldDefaults.colors(
        focusedTextColor = Color.White,
        unfocusedTextColor = Color.White,
        focusedContainerColor = Color(0xFF111111),
        unfocusedContainerColor = Color(0xFF111111),
        disabledContainerColor = Color(0xFF111111),
        focusedLabelColor = Colors.pasx2_blue,
        unfocusedLabelColor = Color(0xFFAAAAAA),
        focusedIndicatorColor = Colors.pasx2_blue,
        unfocusedIndicatorColor = Color(0xFF555555),
        cursorColor = Colors.pasx2_blue,
    )

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF151515),
        title = {
            Text("Enter PNACH Codes", color = Color.White, fontWeight = FontWeight.Bold)
        },
        text = {
            Column {
                Text(
                    gameId?.let { "Saving for ${it.serial} / ${it.crc}" }
                        ?: "No active CRC found; start the game first for auto-naming.",
                    color = Color(0xFFAAAAAA),
                    fontSize = 11.sp,
                    modifier = Modifier.padding(bottom = 6.dp),
                )
                OutlinedTextField(
                    value = title,
                    onValueChange = { title = it },
                    label = { Text("Name") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                    colors = tfColors,
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = body,
                    onValueChange = { body = it },
                    label = { Text("PNACH patch= lines") },
                    minLines = 6,
                    maxLines = 10,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { execute() }),
                    colors = tfColors,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        },
        confirmButton = {
            TextButton(
                enabled = body.isNotBlank(),
                onClick = { execute() },
            ) {
                Text("Execute", color = if (body.isNotBlank()) Colors.pasx2_blue else Color(0xFF777777))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("Cancel", color = Color(0xFFCCCCCC))
            }
        },
    )
}
