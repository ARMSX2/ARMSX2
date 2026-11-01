package com.armsx2.ui.saves

import android.app.Application
import android.net.Uri
import android.widget.Toast
import androidx.compose.runtime.mutableStateOf
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.EmuState
import com.armsx2.EnglishTitles
import com.armsx2.GameInfo
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.i18n.I18n
import com.armsx2.navigation.UiNavigator
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.WindowImpl
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

/**
 * One game's saves, under the game's real title.
 *
 * [game] is the library entry the saves belong to, and null when the library no longer has the game
 * (its folder was removed, or it was never scanned). Such saves are still listed under a remembered
 * title and can be deleted or backed up, but not loaded, because nothing says where the game is.
 */
data class SaveGameGroup(
    val set: SaveSet,
    val title: String,
    val game: GameInfo?,
) {
    val serial: String get() = set.serial
}

data class SaveManagerUiState(
    val groups: List<SaveGameGroup> = emptyList(),
    val loading: Boolean = true,
    /** Status lines shown inline on the screen. Never a dialog: a dialog left over a game that is
     *  already booting had to be dismissed by hand after leaving it. */
    val notice: String? = null,
    val failure: String? = null,
)

/**
 * Every game's saves, grouped by game. The same list whether the screen was opened from the drawer
 * or from a game's long-press menu, and whether or not a game is running or was run before: it reads
 * the save folders and the library, never "the game in context", which is why it used to show only
 * the last game played, or bare serial numbers after the app was killed.
 */
class SaveManagerViewModel(application: Application) : AndroidViewModel(application) {
    var state = mutableStateOf(SaveManagerUiState())
        private set

    fun refresh() {
        viewModelScope.launch {
            val groups = withContext(Dispatchers.IO) { readGroups() }
            state.value = state.value.copy(groups = groups, loading = false)
        }
    }

    private fun readGroups(): List<SaveGameGroup> {
        val sets = SaveCatalog.groupBySerial(SaveCatalog.scan(getApplication<Application>()))
        val library = runCatching { GameLibraryRepository(getApplication()).loadCached().games }
            .getOrDefault(emptyList())
        val bySerial = HashMap<String, GameInfo>()
        for (game in library) {
            val serial = game.serial?.uppercase() ?: continue
            bySerial.putIfAbsent(serial, game)
        }
        val english = EnglishTitles.enabled.value
        return sets.map { set ->
            val game = bySerial[set.serial]
            val shown = game?.displayTitle(english)?.takeIf(String::isNotBlank)
            if (shown != null) SaveTitles.remember(set.serial, shown)
            SaveGameGroup(set, shown ?: SaveTitles.recall(set.serial) ?: set.serial, game)
        }
    }

    /**
     * Load [entry] of [group]. A save of the game that is running right now loads in place; any other
     * boots its game first (stopping whatever is running) and loads once it is up. Either way
     * [leave] runs as soon as the load is under way, so the manager is gone and nothing from it can
     * sit over the game. Success says nothing; only a failure is reported.
     */
    fun load(group: SaveGameGroup, entry: SaveEntry, leave: () -> Unit) {
        val game = group.game
        if (game == null) {
            fail(I18n.get("savestate.notInLibrary"))
            return
        }
        if (!entry.canLoad) {
            fail(I18n.get("savestate.error.load"))
            return
        }
        val autosave = if (entry.id.kind == SaveKind.Autosave) entry.id.autosaveIndex ?: 1 else null
        viewModelScope.launch {
            val running = withContext(Dispatchers.IO) { runningGameSerial() }
            if (running.equals(group.serial, ignoreCase = true)) {
                val ok = withContext(Dispatchers.IO) { runCatching { loadIntoRunningGame(entry, autosave) }.getOrDefault(false) }
                if (ok) {
                    leave()
                    if (MainActivityRuntime.eState.value == EmuState.PAUSED) MainActivityRuntime.resume()
                } else {
                    fail(I18n.get("savestate.error.load"))
                }
            } else {
                val onResult = { ok: Boolean ->
                    // The manager is gone by the time a boot-time load fails, so say so on the game.
                    if (!ok) Toast.makeText(getApplication(), I18n.get("savestate.error.load"), Toast.LENGTH_LONG).show()
                }
                val queued = if (autosave != null) {
                    MainActivityRuntime.launchGameFromAutosave(game, autosave, onResult)
                } else {
                    MainActivityRuntime.launchGameFromSaveFile(game, entry.file.absolutePath, onResult)
                }
                if (queued) leave() else fail(I18n.get("savestate.error.load"))
            }
        }
    }

    /** An autosave goes through the core's autosave loaders (1 is the newest), a numbered slot or a
     *  backup copy of one through load-by-file; the core accepts nothing else by file. */
    private fun loadIntoRunningGame(entry: SaveEntry, autosave: Int?): Boolean = when {
        autosave == null -> NativeApp.loadStateFromFile(entry.file.absolutePath) == 1
        autosave > 1 -> NativeApp.loadAutosaveStateAt(autosave)
        else -> NativeApp.loadAutosaveState()
    }

    private fun runningGameSerial(): String =
        runCatching { if (NativeApp.hasActiveVM()) NativeApp.getGameSerial().orEmpty() else "" }.getOrDefault("")

    fun delete(entry: SaveEntry) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { runCatching { entry.file.delete() }.getOrDefault(false) }
            state.value = if (ok) {
                state.value.copy(notice = I18n.get("savestate.deleted"), failure = null)
            } else {
                state.value.copy(failure = I18n.get("savestate.delete.failed"), notice = null)
            }
            refresh()
        }
    }

    /** Copy this game's numbered saves into a new timestamped folder under `backups/`. Autosaves are
     *  left out: a copy of one could not be loaded back from there (see [SaveEntry.canLoad]). */
    fun backup(group: SaveGameGroup) {
        val files = group.set.slots
        if (files.isEmpty()) {
            fail(I18n.get("savestate.noSavesToBackUp"))
            return
        }
        viewModelScope.launch {
            val count = withContext(Dispatchers.IO) {
                val destination = File(files.first().root, "backups/${System.currentTimeMillis()}").apply { mkdirs() }
                files.count { entry ->
                    runCatching {
                        entry.file.copyTo(File(destination, entry.file.name), overwrite = true)
                        true
                    }.getOrDefault(false)
                }
            }
            state.value = state.value.copy(notice = I18n.get("savestate.backedUp").format(count), failure = null)
            refresh()
        }
    }

    /**
     * Import an external save-state file (an AetherSX2 / NetherSX2 state, or a .p2s from another
     * install) into this game's first free slot. The bytes are copied as they are, under the same
     * name pattern as the game's other saves, and the native loader detects the format by content on
     * load. The game's CRC comes from its existing saves' names, so it works without the game running.
     */
    fun importState(group: SaveGameGroup, uri: Uri) {
        viewModelScope.launch {
            val slot = withContext(Dispatchers.IO) { importInto(group, uri) }
            state.value = when {
                slot >= 0 -> state.value.copy(notice = I18n.get("savestate.imported").format(slot + 1), failure = null)
                slot == SS_IMPORT_SLOTS_FULL -> state.value.copy(failure = I18n.get("savestate.import.slotsFull"), notice = null)
                else -> state.value.copy(failure = I18n.get("savestate.import.failed"), notice = null)
            }
            refresh()
        }
    }

    private fun importInto(group: SaveGameGroup, uri: Uri): Int {
        val taken = group.set.slots.mapNotNull { it.id.slot }.toSet()
        val free = (0 until SLOT_COUNT).firstOrNull { it !in taken } ?: return SS_IMPORT_SLOTS_FULL
        val anchor = (group.set.slots + group.set.autosaves).firstOrNull() ?: return SS_IMPORT_FAILED
        // "<serial> (<CRC>)" is everything before the first ")." of an existing save's name.
        val stem = anchor.file.name.substringBefore(").")
        val dest = File(anchor.file.parentFile, "$stem).%02d.p2s".format(free))
        return runCatching {
            val copied = getApplication<Application>().contentResolver.openInputStream(uri)?.use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
                true
            } ?: false
            if (copied) free else SS_IMPORT_FAILED
        }.getOrDefault(SS_IMPORT_FAILED)
    }

    fun dismissMessage() {
        state.value = state.value.copy(notice = null, failure = null)
    }

    private fun fail(message: String) {
        state.value = state.value.copy(failure = message, notice = null)
    }

    /** Leave the manager for the game: back to the library layer, and clear the library layer if it
     *  was pulled up over a running game. */
    fun leaveForGame() {
        UiNavigator.home()
        WindowImpl.showLibrary.value = false
    }

    private companion object {
        const val SLOT_COUNT = 10
    }
}

/**
 * Titles remembered for serials the library has shown us, so a game whose folder was later removed
 * is still listed by name instead of by serial.
 */
private object SaveTitles {
    private fun key(serial: String) = "saves.title.${serial.uppercase()}"

    fun recall(serial: String): String? =
        runCatching { MainActivityRuntime.prefs.getString(key(serial), null) }.getOrNull()?.takeIf(String::isNotBlank)

    fun remember(serial: String, title: String) {
        if (recall(serial) == title) return
        runCatching { MainActivityRuntime.prefs.edit().putString(key(serial), title).apply() }
    }
}

// Negative sentinels returned by the import helpers (a slot index >= 0 means success).
internal const val SS_IMPORT_SLOTS_FULL = -1
internal const val SS_IMPORT_FAILED = -2
internal const val SS_IMPORT_NO_GAME = -3

/**
 * Copy an external save-state file [uri] into the RUNNING game's next free slot (0..9). BLOCKING —
 * call inside withContext(Dispatchers.IO). Returns the slot index on success, or a negative sentinel
 * above. Used by the in-game SaveStatePicker, where the game's slot paths come from the core: bytes
 * are copied verbatim to the slot's on-disk path and the native loader detects the format (p2s /
 * AetherSX2 / NetherSX2) by content on load. The Save Manager imports per game instead, see
 * [SaveManagerViewModel.importState].
 */
internal fun importSaveStateToNextFreeSlot(context: android.content.Context, uri: Uri): Int {
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
