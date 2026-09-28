package com.armsx2.memcard

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.MemoryCardBackup
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.security.MessageDigest

/**
 * Memory Card Covers: a game's save icon, from the player's own memory cards, as its library cover.
 *
 * Reads only the cards in the app's memcards folder; nothing is bundled or downloaded. Each game
 * shows the icon of its newest save, rendered once into a cached PNG the library loads like any
 * other cover, so scrolling costs nothing extra. A game with no save keeps its box art, and a
 * cover the player set by hand still wins over both.
 */
object MemcardCovers {
    private const val TAG = "MemcardCovers"
    private const val KEY_ENABLED = "library.memcardCovers"
    private const val KEY_ANIMATE = "library.memcardCovers.animate"
    private const val CACHE_DIR = "memcard_covers"

    /** Bump when the renderer changes, so every cached cover is drawn again. */
    private const val RENDER_VERSION = 4

    /** The library's 2D cover slot (0.72, coverAspectRatio in HomeScreen), with room to spare for
     *  large cover sizes. Every layout (grid, list, shelf, Recently Played) draws covers through
     *  the same GameCover, so one render serves them all. */
    const val COVER_W = 288
    const val COVER_H = 400

    /** A cover is just the icon: no gradient, nothing shaped like a box behind it, standing on
     *  the bottom edge so on the shelf it sits on the shelf. */
    val COVER_OPTIONS = Ps2IconRenderer.Options(background = false, anchorBottom = true)

    val enabled = mutableStateOf(false)

    /** The selected game's cover spins and plays its animation, as on the PS2's memory card
     *  screen. Only the selected one: animating every tile would cost a render per tile per frame. */
    val animateSelected = mutableStateOf(true)

    /** Bumped whenever the covers change, so tiles resolve their cover again. */
    val generation = mutableIntStateOf(0)

    @Volatile private var bySerial: Map<String, File> = emptyMap()

    /** Where each cover's save is, so the selected one can be drawn moving. */
    private class Source(val card: File, val folder: String)
    @Volatile private var sources: Map<String, Source> = emptyMap()

    /** A save's icon, parsed, with the pose its still cover was drawn in. */
    class Loaded(val icon: Ps2Icon, val sys: Ps2IconSys, val pose: Ps2IconRenderer.Pose)

    // The last few selected, so moving back and forth between tiles doesn't reread the card.
    private val loadedIcons = object : LinkedHashMap<String, Loaded>(8, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Loaded>?) = size > 6
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val scanLock = Mutex()
    @Volatile private var loaded = false

    fun load() {
        if (loaded) return
        loaded = true
        enabled.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_ENABLED, false) }.getOrDefault(false)
        animateSelected.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_ANIMATE, true) }.getOrDefault(true)
    }

    fun setAnimateSelected(on: Boolean) {
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ANIMATE, on).apply()
        animateSelected.value = on
    }

    /** The icon behind [serial]'s cover, parsed, or null. Reads the card, so call it off the main
     *  thread. */
    fun loadIcon(serial: String): Loaded? {
        val key = serial.uppercase()
        synchronized(loadedIcons) { loadedIcons[key]?.let { return it } }
        val src = sources[key] ?: return null
        val result = load(src.card, src.folder, COVER_OPTIONS) ?: return null
        synchronized(loadedIcons) { loadedIcons[key] = result }
        return result
    }

    private fun load(cardFile: File, folder: String, options: Ps2IconRenderer.Options): Loaded? = runCatching {
        Ps2MemoryCard.open(cardFile)?.use { card ->
            val save = card.saves().firstOrNull { it.folder == folder } ?: return@use null
            val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: return@use null
            val icon = Ps2Icon.parse(save.read(sys.iconNormal)) ?: return@use null
            Loaded(icon, sys, Ps2IconRenderer.choosePose(icon, sys, options))
        }
    }.getOrNull()

    // ---- Icon viewer ----------------------------------------------------------------------

    /** One save with an icon, for the Icon Viewer: every save on every card, not just the one
     *  per game a cover uses. */
    class SaveRef(val card: File, val folder: String, val title: String, val serial: String?, val modified: Long)

    /** Every save with a readable icon.sys across the cards, newest first. Reads the cards. */
    fun allSaves(context: Context): List<SaveRef> {
        val out = ArrayList<SaveRef>()
        for (card in cardFiles(context)) runCatching {
            Ps2MemoryCard.open(card)?.use { c ->
                for (save in c.saves()) {
                    val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: continue
                    out += SaveRef(card, save.folder, sys.title.ifBlank { save.folder }, save.serial, save.modifiedMillis)
                }
            }
        }
        return out.sortedByDescending { it.modified }
    }

    /** A save's icon for the viewer, which shows it on its own background. Reads the card. */
    fun loadForViewer(ref: SaveRef): Loaded? = load(ref.card, ref.folder, Ps2IconRenderer.Options())

    private fun cardFiles(context: Context): List<File> =
        (MemoryCardBackup.cardsDir(context).listFiles() ?: emptyArray()).filter { f ->
            (f.isFile && f.name.endsWith(".ps2", ignoreCase = true)) ||
                (f.isDirectory && File(f, "_pcsx2_superblock").exists())
        }

    fun setEnabled(context: Context, on: Boolean) {
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ENABLED, on).apply()
        enabled.value = on
        if (on) refresh(context)
    }

    /** The cover for a game with this serial, when Memory Card Covers is on and a save has one. */
    fun coverFor(serial: String?): File? =
        if (!enabled.value || serial.isNullOrBlank()) null else bySerial[serial.uppercase()]

    /** Scan every card in the memcards folder again, in the background, and render any cover that
     *  is new or changed. With several cards, each game takes its newest save across all of them.
     *  Cheap when nothing changed: a scan reads directories and each save's timestamps, and a
     *  cover is only drawn once per save version. */
    fun refresh(context: Context) {
        if (!enabled.value) return
        val app = context.applicationContext
        scope.launch {
            // One scan at a time; a request during a scan waits and then sees the finished cache.
            scanLock.withLock {
                val found = runCatching { scan(app) }.onFailure { Log.w(TAG, "scan failed", it) }.getOrNull()
                    ?: return@withLock
                if (found != bySerial) {
                    bySerial = found
                    // A changed save may be one already loaded for animating.
                    synchronized(loadedIcons) { loadedIcons.clear() }
                    withContext(Dispatchers.Main) { generation.intValue++ }
                }
            }
        }
    }

    private class Pick(val card: File, val folder: String, val modified: Long)

    private fun scan(context: Context): Map<String, File> {
        val cards = cardFiles(context)

        // The newest save of each game across every card, by the time the card itself recorded.
        val picks = HashMap<String, Pick>()
        for (card in cards) {
            Ps2MemoryCard.open(card)?.use { c ->
                for (save in c.saves()) {
                    val serial = save.serial ?: continue
                    if (save.fileNames.none { it.equals("icon.sys", ignoreCase = true) }) continue
                    val prev = picks[serial]
                    if (prev == null || save.modifiedMillis > prev.modified) picks[serial] = Pick(card, save.folder, save.modifiedMillis)
                }
            }
        }

        val cacheDir = File(context.cacheDir, CACHE_DIR).apply { mkdirs() }
        val out = HashMap<String, File>()
        // Render what is missing, one card open at a time.
        for ((card, group) in picks.entries.groupBy { it.value.card }) {
            val wanted = group.associate { (serial, pick) -> serial to File(cacheDir, "${serial}_${key(pick)}.png") }
            val missing = wanted.filterValues { !it.isFile }
            if (missing.isNotEmpty()) Ps2MemoryCard.open(card)?.use { c ->
                val saves = c.saves().associateBy { it.folder }
                for ((serial, file) in missing) {
                    val save = saves[picks.getValue(serial).folder] ?: continue
                    runCatching { renderTo(save, file) }.onFailure { Log.w(TAG, "render ${save.folder}", it) }
                }
            }
            for ((serial, file) in wanted) if (file.isFile) out[serial] = file
        }
        sources = out.keys.associateWith { serial -> picks.getValue(serial).let { Source(it.card, it.folder) } }
        // Drop covers no save points at any more.
        val keep = out.values.toSet()
        cacheDir.listFiles()?.forEach { if (it !in keep) it.delete() }
        Log.i(TAG, "${cards.size} card(s), ${picks.size} game(s) with a save icon, ${out.size} cover(s)")
        return out
    }

    private fun key(p: Pick): String {
        val md = MessageDigest.getInstance("SHA-1")
        md.update("${p.card.absolutePath}|${p.folder}|${p.modified}|$RENDER_VERSION".toByteArray())
        return md.digest().take(6).joinToString("") { "%02x".format(it) }
    }

    private fun renderTo(save: Ps2Save, file: File) {
        val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: return
        val icon = Ps2Icon.parse(save.read(sys.iconNormal)) ?: return
        val px = Ps2IconRenderer.render(icon, sys, COVER_W, COVER_H, COVER_OPTIONS)
        val bmp = Bitmap.createBitmap(px, COVER_W, COVER_H, Bitmap.Config.ARGB_8888)
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bmp.recycle()
        if (!tmp.renameTo(file)) tmp.delete()
    }
}
