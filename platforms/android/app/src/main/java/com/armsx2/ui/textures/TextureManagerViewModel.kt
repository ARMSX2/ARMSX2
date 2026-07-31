package com.armsx2.ui.textures

import android.app.Application
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.TexturePackInstallState
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

data class TexturePackItem(
    val serial: String,
    val directory: File,
    val fileCount: Int,
    val size: Long,
    /** Resolved from the library scan cache so the row can say "Guitar Hero II" and not just
     *  "SLUS-21447". Null when the pack's serial isn't in the library (installed for a game the
     *  user doesn't have, or an .ELF-named folder), in which case the serial is all we can show. */
    val gameTitle: String? = null,
)

data class TextureManagerUiState(
    val settings: Settings = Settings(),
    val packs: List<TexturePackItem> = emptyList(),
    val activeSerial: String? = null,
    /** Serials in the user's ROM library, upper-cased. Read from the scan cache, never scanned here. */
    val librarySerials: Set<String> = emptySet(),
    val busy: Boolean = false,
    val progress: Int = 0,
    val message: String? = null,
    val error: String? = null,
)

class TextureManagerViewModel(application: Application) : AndroidViewModel(application) {
    var state = androidx.compose.runtime.mutableStateOf(TextureManagerUiState())
        private set

    /**
     * Rescan installed packs and refresh the library set.
     *
     * All of the disk work is on Dispatchers.IO. It used to run inline, and opening this screen
     * then walked every installed pack on the MAIN thread — a real pack is thousands of files
     * (DBZ BT3: 4277 / 1.8 GB), so the screen froze on open and could ANR outright, which is what
     * "the texture packs menu hangs and I have to close the app" was.
     */
    fun refresh() {
        viewModelScope.launch {
            val scanned = withContext(Dispatchers.IO) {
                val root = textureRoot().apply { mkdirs() }
                val serialDirs = root.listFiles().orEmpty().filter(File::isDirectory)
                val packs = serialDirs.mapNotNull { serialDir ->
                    val replacements = File(serialDir, "replacements")
                    if (!replacements.isDirectory) return@mapNotNull null
                    // Sizes come from a CACHE, not a fresh walk. Walking every pack's tree on every
                    // open is what made this screen hang: a real pack is thousands of files (DBZ
                    // BT3: 4277), so ~60 installed packs is a quarter of a million stat() calls
                    // before the first frame — reported as "opening the texture packs menu freezes
                    // the app, sometimes I have to close it". The cache is keyed on the folder's
                    // mtime, so a pack that changed is re-measured and everything else is free.
                    val cached = PackSizeCache.get(serialDir.name, replacements.lastModified())
                    val (count, bytes) = cached ?: run {
                        var c = 0
                        var b = 0L
                        replacements.walkTopDown().forEach { f ->
                            if (f.isFile) { c++; b += f.length() }
                        }
                        PackSizeCache.put(serialDir.name, replacements.lastModified(), c, b)
                        c to b
                    }
                    TexturePackItem(serialDir.name, replacements, count, bytes)
                }

                // The install record is a JSON file that can drift from the filesystem: deleting a
                // pack in a file manager, or a delete that only partly succeeded, used to leave the
                // catalogue entry greyed out as "Installed" forever, which also blocked reinstalling
                // or switching packs for that game. Reconcile against what is actually on disk.
                TexturePackInstallState.reconcile(packs.map { it.serial.uppercase() }.toSet())

                val settings = ConfigStore.loadGlobal()
                // Cached scan results only — loadCached() reads the stored list, it does not walk
                // storage. Without this the catalogue's "your games" grouping only knew about games
                // that already had a pack or had been launched this session, so an owned game sat
                // under "Other games" until you played it once (reported by Beep).
                // Titles come off the same cached list, so naming the packs costs nothing extra.
                val cachedGames = runCatching {
                    GameLibraryRepository(getApplication()).loadCached().games
                }.getOrDefault(emptyList())
                val library = cachedGames
                    .mapNotNull { it.serial?.takeIf(String::isNotBlank)?.uppercase() }
                    .toSet()
                // displayTitle, not title — so a game the user renamed shows the name they gave it,
                // matching the library list rather than contradicting it.
                val forceEn = com.armsx2.EnglishTitles.enabled.value
                val titles = cachedGames.mapNotNull { g ->
                    val s = g.serial?.takeIf(String::isNotBlank)?.uppercase() ?: return@mapNotNull null
                    g.displayTitle(forceEn).takeIf(String::isNotBlank)?.let { s to it }
                }.toMap()

                // Sort by game name so packs read like the library does, falling back to the serial
                // for anything the library can't name (which also keeps the order stable).
                val named = packs
                    .map { it.copy(gameTitle = titles[it.serial.uppercase()]) }
                    .sortedBy { (it.gameTitle ?: it.serial).lowercase() }

                Triple(named, settings, library)
            }
            state.value = state.value.copy(
                settings = scanned.second,
                packs = scanned.first,
                librarySerials = scanned.third,
                activeSerial = runtimeSerial(),
            )
        }
    }

    /**
     * The serial the CORE will look under — VMManager::GetDiscSerial(), the same value
     * GSTextureReplacements keys its `textures/<serial>/replacements` path off.
     *
     * This is NOT always the library's serial. Booting a raw .ELF (Persona/Aemulus mod
     * setups do this) takes VMManager's elf-override branch, where the serial becomes
     * Path::GetFileTitle(elf) — e.g. "PERSONA 3 FES" for PERSONA 3 FES.ELF — because with
     * no disc mounted there is no real serial to read. Importing under the library serial
     * ("SLUS-21621") then writes to a folder the core never scans, and the pack silently
     * does nothing. Prefer the running core's answer; fall back to the library entry when
     * nothing is running.
     */
    private fun runtimeSerial(): String? {
        if (MainActivityRuntime.nativeReady.value) {
            val live = runCatching { NativeApp.getGameSerial() }.getOrNull()
            if (!live.isNullOrBlank()) return live
        }
        return MainActivityRuntime.currentGame.value?.serial
            // Survives quitting to the library, where the two sources above are both blank.
            ?: MainActivityRuntime.contextGame.value?.serial?.takeIf { it.isNotBlank() }
    }

    fun update(transform: (Settings) -> Settings) {
        // Transform the CURRENT global, not this screen's snapshot — see the note in
        // EmulationMenuViewModel.updateSettings. This one saves to global explicitly, so read
        // global explicitly rather than whatever was loaded when the screen opened.
        val updated = transform(ConfigStore.loadGlobal())
        ConfigStore.saveGlobal(updated)
        if (MainActivityRuntime.nativeReady.value) runCatching { updated.applyTo() }
        state.value = state.value.copy(settings = updated)
    }

    fun importFolder(uri: Uri) {
        val context = getApplication<Application>()
        val source = DocumentFile.fromTreeUri(context, uri)
        if (source == null) {
            state.value = state.value.copy(error = "Unable to open the selected folder.")
            return
        }
        val serial = state.value.activeSerial
            ?.takeIf(String::isNotBlank)
            ?: source.name?.let(::normalizeSerial)
        if (serial.isNullOrBlank()) {
            state.value = state.value.copy(error = "Name the selected folder with the game's serial, for example SLES-52187.")
            return
        }
        // Off the main thread. A real pack is thousands of files (DBZ BT3: 4277 files /
        // 1.8 GB) and copyTree walks it over SAF, where every listFiles()/openInputStream()
        // is an IPC round-trip to the DocumentsProvider. Run inline this blocked the UI
        // thread for minutes — the busy state never even rendered — so the user got a black
        // screen and an ANR, worse still with the folder on SD. Progress is published as it
        // copies so the screen can show something moving.
        state.value = state.value.copy(busy = true, progress = 0, message = null, error = null)
        viewModelScope.launch {
            val copied = withContext(Dispatchers.IO) {
                val destination = File(textureRoot(), "$serial/replacements").apply { mkdirs() }
                val contentRoot = source.findFile("replacements")?.takeIf(DocumentFile::isDirectory) ?: source
                runCatching { copyTree(contentRoot, destination) }.getOrDefault(0)
            }
            state.value = if (copied > 0) {
                state.value.copy(busy = false, progress = 0, message = "Imported $copied texture files for $serial.")
            } else {
                state.value.copy(busy = false, progress = 0, error = "No texture files were imported.")
            }
            if (copied > 0) reloadCore()
            refresh()
        }
    }

    fun delete(pack: TexturePackItem) {
        val serialDirectory = pack.directory.parentFile ?: return
        // Also off the main thread: deleteRecursively over a 1.8 GB / 4000-file pack is
        // just as capable of ANRing as the import was.
        state.value = state.value.copy(busy = true, message = null, error = null)
        viewModelScope.launch {
            val deleted = withContext(Dispatchers.IO) {
                val ok = runCatching { serialDirectory.deleteRecursively() }.getOrDefault(false)
                if (!ok) {
                    // deleteRecursively() is all-or-nothing in its return value and says nothing
                    // about WHICH entry refused, so a failure was previously unactionable — the
                    // user just saw "Unable to delete". Name the survivors.
                    val left = runCatching {
                        serialDirectory.walkTopDown().filter { it != serialDirectory }.take(5).toList()
                    }.getOrDefault(emptyList())
                    android.util.Log.w(
                        "TextureManager",
                        "delete ${serialDirectory.absolutePath} failed; exists=${serialDirectory.exists()} " +
                            "survivors=${left.joinToString { it.relativeTo(serialDirectory).path }}",
                    )
                }
                // Treat "the directory is gone" as success regardless of what the walk returned:
                // a partial failure that still removed everything is a success to the user, and
                // the state reconcile below keys off the filesystem anyway.
                ok || !serialDirectory.exists()
            }
            state.value = if (deleted) {
                state.value.copy(busy = false, message = "Deleted texture pack ${pack.serial}.")
            } else {
                state.value.copy(busy = false, error = "Unable to delete ${pack.serial}.")
            }
            if (deleted) {
                com.armsx2.TexturePackInstallState.forgetSerial(pack.serial)
                // Drop the cached size too, or a reinstall of the same serial could be measured
                // against the deleted pack's mtime and report stale counts.
                PackSizeCache.forget(pack.serial)
            }
            if (deleted && pack.serial.equals(state.value.activeSerial, true)) reloadCore()
            refresh()
        }
    }

    /**
     * A catalog pack just landed. Texture replacement defaults to OFF, so without this the files
     * are on disk and the game looks exactly the same — the single most confusing possible outcome.
     */
    fun onPackInstalled() {
        if (!state.value.settings.loadTextureReplacements) {
            update { it.copy(loadTextureReplacements = true) }
        }
        refresh()
        reloadCore()
    }

    fun dismissMessage() {
        state.value = state.value.copy(message = null, error = null)
    }

    private fun copyTree(source: DocumentFile, destination: File, running: IntArray = IntArray(1)): Int {
        var copied = 0
        source.listFiles().forEach { child ->
            val name = child.name?.replace(Regex("[/:*?\"<>|]"), "_") ?: return@forEach
            if (child.isDirectory) {
                copied += copyTree(child, File(destination, name).apply { mkdirs() }, running)
            } else if (child.isFile) {
                val success = getApplication<Application>().contentResolver.openInputStream(child.uri)?.use { input ->
                    File(destination, name).outputStream().use(input::copyTo)
                    true
                } ?: false
                if (success) {
                    copied++
                    // Publish a running count so a multi-thousand-file import shows movement
                    // instead of looking hung. Throttled — this runs on Dispatchers.IO and
                    // state is read by Compose, so every file would be needless recomposition.
                    running[0]++
                    if (running[0] % 25 == 0) {
                        state.value = state.value.copy(progress = running[0])
                    }
                }
            }
        }
        return copied
    }

    private fun textureRoot(): File = File(MainActivityRuntime.assetCopyRoot(getApplication()), "textures")

    private fun normalizeSerial(value: String): String? {
        val match = Regex("([A-Za-z]{4})[-_ ]?(\\d{3})[._ ]?(\\d{2})").find(value) ?: return null
        return "${match.groupValues[1].uppercase()}-${match.groupValues[2]}${match.groupValues[3]}"
    }

    private fun reloadCore() {
        if (!MainActivityRuntime.nativeReady.value) return
        runCatching { NativeApp.reloadTextureReplacements() }
    }
}

/**
 * Remembers each pack's file count and byte total so the Texture Manager doesn't re-walk every
 * pack's tree every time it opens.
 *
 * Keyed on the `replacements` folder's mtime: install, delete or overwrite touches the directory,
 * so a changed pack misses the cache and is re-measured while untouched ones cost nothing. Backed
 * by SharedPreferences (a few dozen short entries) so it also survives a restart — the first open
 * after a cold start was the worst case.
 */
private object PackSizeCache {
    private const val KEY = "textures.sizeCache"

    private fun load(): org.json.JSONObject = runCatching {
        MainActivityRuntime.prefs.getString(KEY, null)?.let { org.json.JSONObject(it) }
    }.getOrNull() ?: org.json.JSONObject()

    fun get(serial: String, mtime: Long): Pair<Int, Long>? {
        val o = load().optJSONObject(serial) ?: return null
        // Stale mtime => the pack changed on disk; force a re-measure.
        if (o.optLong("m", -1L) != mtime) return null
        val c = o.optInt("c", -1)
        val b = o.optLong("b", -1L)
        return if (c >= 0 && b >= 0) c to b else null
    }

    fun put(serial: String, mtime: Long, count: Int, bytes: Long) {
        runCatching {
            val root = load()
            root.put(serial, org.json.JSONObject().apply {
                put("m", mtime); put("c", count); put("b", bytes)
            })
            MainActivityRuntime.prefs.edit().putString(KEY, root.toString()).apply()
        }
    }

    /** Drop one pack's entry, so the next scan re-measures it. */
    fun forget(serial: String) {
        runCatching {
            val root = load()
            if (!root.has(serial)) return
            root.remove(serial)
            MainActivityRuntime.prefs.edit().putString(KEY, root.toString()).apply()
        }
    }
}
