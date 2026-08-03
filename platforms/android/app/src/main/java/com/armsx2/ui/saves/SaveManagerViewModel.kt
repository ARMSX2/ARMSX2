package com.armsx2.ui.saves

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

data class SaveStateItem(
    val slot: Int?,
    val file: File,
    val gameTitle: String,
    val serial: String,
    val preview: Bitmap?,
    val canUseWithActiveGame: Boolean,
)

data class SaveManagerUiState(
    val gameTitle: String? = null,
    val saves: List<SaveStateItem> = emptyList(),
    val loading: Boolean = true,
    val message: String? = null,
    val error: String? = null,
)

class SaveManagerViewModel(application: Application) : AndroidViewModel(application) {
    var state = androidx.compose.runtime.mutableStateOf(SaveManagerUiState())
        private set

    fun refresh() {
        val previous = state.value
        state.value = previous.copy(loading = true)
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { readSaves() }
            state.value = state.value.copy(
                gameTitle = result.gameTitle,
                saves = result.saves,
                loading = false,
            )
        }
    }

    fun save(slot: Int) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching { NativeApp.saveStateToSlot(slot) }.getOrDefault(false)
            }
            state.value = if (ok) {
                state.value.copy(message = "${I18n.get("action.save")} · ${slot + 1}")
            } else {
                state.value.copy(error = "${I18n.get("action.save")} · ${slot + 1}")
            }
            refresh()
        }
    }

    fun load(item: SaveStateItem) {
        val slot = item.slot ?: return
        viewModelScope.launch {
            val hasActiveVm = withContext(Dispatchers.IO) {
                runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)
            }
            val ok = if (hasActiveVm) {
                withContext(Dispatchers.IO) {
                    runCatching { NativeApp.loadStateFromSlot(slot) }.getOrDefault(false)
                }
            } else {
                MainActivityRuntime.launchCurrentGameFromSaveSlot(slot)
            }
            state.value = if (ok) {
                state.value.copy(message = "${I18n.get("touch.stateAction.load")} · ${slot + 1}")
            } else {
                state.value.copy(error = "${I18n.get("touch.stateAction.load")} · ${slot + 1}")
            }
        }
    }

    fun delete(item: SaveStateItem) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { runCatching { item.file.delete() }.getOrDefault(false) }
            state.value = if (ok) {
                state.value.copy(message = I18n.get("action.delete"))
            } else {
                state.value.copy(error = I18n.get("action.delete"))
            }
            refresh()
        }
    }

    fun backupAll() {
        val files = state.value.saves.map(SaveStateItem::file)
        if (files.isEmpty()) {
            state.value = state.value.copy(error = I18n.get("savestate.noSavesToBackUp"))
            return
        }
        viewModelScope.launch {
            val count = withContext(Dispatchers.IO) {
                val destination = File(files.first().parentFile, "backups/${System.currentTimeMillis()}").apply { mkdirs() }
                files.count { file ->
                    runCatching {
                        file.copyTo(File(destination, file.name), overwrite = true)
                        true
                    }.getOrDefault(false)
                }
            }
            state.value = state.value.copy(message = "${I18n.get("savestate.backup")} · $count")
        }
    }

    /**
     * Import an external save-state file (e.g. an AetherSX2 / NetherSX2 state, or a .p2s from another
     * install) into the ACTIVE game's next free slot. The bytes are copied verbatim to the slot's
     * on-disk path — the native loader detects the format by content on load (see the legacy
     * save-state reader), so the .p2s extension of the destination doesn't have to match the source.
     *
     * A save state belongs to one specific game, so this needs an active game to target; from the
     * global manager with nothing running it reports that. Slots are chosen automatically (first
     * free of 0..9) so it never silently overwrites an existing save.
     */
    fun importState(uri: android.net.Uri) {
        viewModelScope.launch {
            val slot = withContext(Dispatchers.IO) { importSaveStateToNextFreeSlot(getApplication(), uri) }
            state.value = when {
                slot >= 0 -> state.value.copy(message = "${I18n.get("savestate.import")} · ${slot + 1}")
                slot == SS_IMPORT_NO_GAME -> state.value.copy(error = I18n.get("savestate.import.needsGame"))
                slot == SS_IMPORT_SLOTS_FULL -> state.value.copy(error = I18n.get("savestate.import.slotsFull"))
                else -> state.value.copy(error = I18n.get("savestate.import.failed"))
            }
            refresh()
        }
    }

    fun dismissMessage() {
        state.value = state.value.copy(message = null, error = null)
    }

    private data class ReadResult(
        val gameTitle: String?,
        val saves: List<SaveStateItem>,
    )

    private fun readSaves(): ReadResult {
        val active = MainActivityRuntime.currentGame.value
        val activeSerial = active?.serial.orEmpty()
        val activePaths = (0 until SLOT_COUNT).mapNotNull { slot ->
            runCatching { NativeApp.getGamePathSlot(slot) }
                .getOrNull()
                ?.takeIf(String::isNotBlank)
                ?.let(::File)
                ?.takeIf(File::exists)
        }

        val roots = listOf("sstates", "savestates")
            .map { File(MainActivityRuntime.assetCopyRoot(getApplication()), it) }
        val discovered = roots.flatMap { root ->
            if (!root.isDirectory) emptyList()
            else root.walkTopDown().filter { it.isFile && it.extension.equals("p2s", true) }.toList()
        }
        val allFiles = (activePaths + discovered)
            .distinctBy { it.absolutePath.lowercase() }
            .filter { file -> active == null || activeSerial.isBlank() || serialFrom(file).equals(activeSerial, true) }

        val saves = allFiles.map { file ->
            val serial = serialFrom(file)
            val belongsToActiveGame = active != null && (
                activeSerial.isBlank() || serial.equals(activeSerial, true) || activePaths.any { it.absolutePath == file.absolutePath }
            )
            SaveStateItem(
                slot = slotFrom(file),
                file = file,
                gameTitle = if (belongsToActiveGame) active?.title.orEmpty().ifBlank { serial } else serial,
                serial = serial,
                preview = decodePreview(file),
                canUseWithActiveGame = belongsToActiveGame,
            )
        }.sortedWith(compareBy<SaveStateItem> { it.slot == null }.thenBy { it.slot ?: Int.MAX_VALUE }.thenByDescending { it.file.lastModified() })

        return ReadResult(
            gameTitle = active?.title,
            saves = saves,
        )
    }

    private fun decodePreview(file: File): Bitmap? {
        val bytes = runCatching { NativeApp.getSaveStateImage(file.absolutePath) }.getOrNull() ?: return null
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        var sample = 1
        while (bounds.outWidth / sample > 640 || bounds.outHeight / sample > 360) sample *= 2
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply {
            inSampleSize = sample
            inPreferredConfig = Bitmap.Config.RGB_565
        })
    }

    private fun slotFrom(file: File): Int? = SLOT_PATTERN.find(file.name)?.groupValues?.getOrNull(1)?.toIntOrNull()

    private fun serialFrom(file: File): String = file.name.substringBefore(" (").ifBlank {
        file.nameWithoutExtension.substringBeforeLast('.')
    }

    private companion object {
        const val SLOT_COUNT = 10
        val SLOT_PATTERN = Regex("\\.([0-9]{2})\\.p2s$", RegexOption.IGNORE_CASE)
    }
}

// Negative sentinels returned by importSaveStateToNextFreeSlot (a slot index >= 0 means success).
internal const val SS_IMPORT_SLOTS_FULL = -1
internal const val SS_IMPORT_FAILED = -2
internal const val SS_IMPORT_NO_GAME = -3

/**
 * Copy an external save-state file [uri] into the ACTIVE game's next free slot (0..9). BLOCKING —
 * call inside withContext(Dispatchers.IO). Returns the slot index on success, or a negative sentinel
 * above. Shared by [SaveManagerViewModel] (library manager) and the in-game SaveStatePicker so both
 * import identically: bytes are copied verbatim to the slot's on-disk path and the native loader
 * detects the format (p2s / AetherSX2 / NetherSX2) by content on load.
 */
internal fun importSaveStateToNextFreeSlot(context: android.content.Context, uri: android.net.Uri): Int {
    val active = MainActivityRuntime.currentGame.value
    if (active == null || active.serial.isNullOrBlank()) return SS_IMPORT_NO_GAME
    return runCatching {
        val free = (0 until 10).firstOrNull { s ->
            val p = NativeApp.getGamePathSlot(s)
            p.isNullOrBlank() || !File(p).exists()
        } ?: return@runCatching SS_IMPORT_SLOTS_FULL
        val destPath = NativeApp.getGamePathSlot(free)?.takeIf(String::isNotBlank) ?: return@runCatching SS_IMPORT_FAILED
        val dest = File(destPath)
        dest.parentFile?.mkdirs()
        val ok = context.contentResolver.openInputStream(uri)?.use { input ->
            dest.outputStream().use { output -> input.copyTo(output) }
            true
        } ?: false
        if (ok) free else SS_IMPORT_FAILED
    }.getOrDefault(SS_IMPORT_FAILED)
}
