package com.armsx2.memcard

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.util.Log
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.EmuState
import com.armsx2.MemoryCardBackup
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * Memory Card Covers: a game's save icon, from the player's own memory cards, as its library cover.
 *
 * Reads only the cards in the app's memcards folder and the player's own disc images; nothing is
 * bundled or downloaded. Each game shows the icon of its newest save, rendered once into a cached
 * PNG the library loads like any other cover, so scrolling costs nothing extra. A game with no
 * save gets the icon from its own disc when [DiscIcons] can find it there (looked for once per
 * disc, in the background), and keeps its box art when not. A cover set by hand wins over all.
 */
object MemcardCovers {
    private const val TAG = "MemcardCovers"
    private const val KEY_ENABLED = "library.memcardCovers"
    private const val KEY_ANIMATE = "library.memcardCovers.animate"
    private const val KEY_SPIN = "library.memcardCovers.spin"
    private const val CACHE_DIR = "memcard_covers"
    private const val DISC_COVER_DIR = "disc_covers"
    private const val DISC_ICON_DIR = "disc_icons"

    /** Bump when the renderer changes, so every cached cover is drawn again. */
    private const val RENDER_VERSION = 5

    /** The library's 2D cover slot (0.72, coverAspectRatio in HomeScreen), with room to spare for
     *  large cover sizes. Every layout (grid, list, shelf, Recently Played) draws covers through
     *  the same GameCover, so one render serves them all. */
    const val COVER_W = 288
    const val COVER_H = 400

    /** A cover is just the icon: no gradient, nothing shaped like a box behind it, standing on
     *  the bottom edge so on the shelf it sits on the shelf. */
    val COVER_OPTIONS = Ps2IconRenderer.Options(background = false, anchorBottom = true)

    val enabled = mutableStateOf(false)

    /** Covers turn and play their animations, as on the PS2's memory card screen ("Animated"),
     *  rather than standing still ("Still"). Each tile on screen draws its own frames. */
    val animate = mutableStateOf(true)

    /** Moving icons turn as well ("Spin"), or only play their own animation, facing the same way
     *  ("No spin"). In the library, the Icon Viewer and the screensaver alike. */
    val spin = mutableStateOf(true)

    /** Bumped whenever the covers change, so tiles resolve their cover again. */
    val generation = mutableIntStateOf(0)

    // Covers from saves and covers from discs, kept apart so a rescan of the cards never loses the
    // disc ones; a save's wins where both exist.
    @Volatile private var cardCovers: Map<String, File> = emptyMap()
    @Volatile private var discCovers: Map<String, File> = emptyMap()
    @Volatile private var bySerial: Map<String, File> = emptyMap()

    /** Where each cover's icon comes from, so it can be drawn moving. */
    private sealed interface Source
    private class CardSource(val card: File, val folder: String) : Source
    private class DiscSource(val icon: File) : Source
    @Volatile private var cardSources: Map<String, Source> = emptyMap()
    @Volatile private var discSources: Map<String, Source> = emptyMap()
    @Volatile private var sources: Map<String, Source> = emptyMap()

    private fun merge() {
        bySerial = discCovers + cardCovers
        sources = discSources + cardSources
    }

    /** A save's icon, parsed, with the pose its still cover was drawn in. A disc's icon has no
     *  icon.sys, so it is lit the renderer's own way. */
    class Loaded(val icon: Ps2Icon, val sys: Ps2IconSys?, val pose: Ps2IconRenderer.Pose)

    // Every tile on screen moves, so keep a screenful and then some: scrolling back and forth
    // shouldn't reread the cards. An icon is a few hundred KB at most.
    private val loadedIcons = object : LinkedHashMap<String, Loaded>(64, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Loaded>?) = size > 48
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val scanLock = Mutex()
    @Volatile private var loaded = false

    fun load() {
        if (loaded) return
        loaded = true
        enabled.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_ENABLED, false) }.getOrDefault(false)
        animate.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_ANIMATE, true) }.getOrDefault(true)
        spin.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_SPIN, true) }.getOrDefault(true)
    }

    fun setSpin(on: Boolean) {
        MainActivityRuntime.prefs.edit().putBoolean(KEY_SPIN, on).apply()
        spin.value = on
    }

    fun setAnimate(on: Boolean) {
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ANIMATE, on).apply()
        animate.value = on
    }

    /** The icon behind [serial]'s cover, parsed, or null. Reads the card, so call it off the main
     *  thread. */
    fun loadIcon(serial: String): Loaded? {
        val key = serial.uppercase()
        synchronized(loadedIcons) { loadedIcons[key]?.let { return it } }
        val result = when (val src = sources[key] ?: return null) {
            is CardSource -> load(src.card, src.folder, COVER_OPTIONS)
            is DiscSource -> Ps2Icon.parse(runCatching { src.icon.readBytes() }.getOrNull())?.let { icon ->
                Loaded(icon, null, Ps2IconRenderer.choosePose(icon, null, COVER_OPTIONS))
            }
        } ?: return null
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

    /** Every save with a readable icon.sys across the cards, newest first. Reads the cards. A save
     *  whose icon isn't on the card is left out: the BIOS's own "Your System Configuration" names
     *  one that lives in the console, and would be a blank page. */
    fun allSaves(context: Context): List<SaveRef> {
        val out = ArrayList<SaveRef>()
        for (card in cardFiles(context)) runCatching {
            Ps2MemoryCard.open(card)?.use { c ->
                for (save in c.saves()) {
                    val sys = Ps2IconSys.parse(save.read("icon.sys")) ?: continue
                    if (save.fileNames.none { it.equals(sys.iconNormal, ignoreCase = true) }) continue
                    out += SaveRef(card, save.folder, sys.title.ifBlank { save.folder }, save.serial, save.modifiedMillis)
                }
            }
        }
        return out.sortedByDescending { it.modified }
    }

    /** A save's icon for the viewer, which shows it on its own background. Reads the card. */
    fun loadForViewer(ref: SaveRef): Loaded? = load(ref.card, ref.folder, Ps2IconRenderer.Options())

    /** An icon the viewer or the screensaver can show: what to call it, where it is from (a card's
     *  name, or null for a disc), and how to load it (off the main thread). */
    class ShowIcon(val key: String, val title: String, val serial: String? = null, val card: String? = null, val load: () -> Loaded?)

    /**
     * Everything the Icon Viewer shows: every save on every card, newest first, then the icons
     * found on the discs of games with no save, by name. [titles] maps serials to the library's
     * names; a save keeps its own ("Adventure Slot 2"), it says which save it is. Reads the cards.
     */
    fun viewerIcons(context: Context, titles: Map<String, String>): List<ShowIcon> {
        val out = ArrayList<ShowIcon>()
        for (ref in allSaves(context)) {
            out += ShowIcon("card:${ref.card.path}|${ref.folder}", ref.title, ref.serial, ref.card.nameWithoutExtension) { loadForViewer(ref) }
        }
        out += discIcons(titles).sortedBy { it.title.lowercase() }
        return out
    }

    private fun discIcons(titles: Map<String, String>): List<ShowIcon> = discSources.mapNotNull { (serial, src) ->
        if (src !is DiscSource) return@mapNotNull null
        ShowIcon("disc:$serial", titles[serial] ?: serial, serial, null) {
            Ps2Icon.parse(runCatching { src.icon.readBytes() }.getOrNull())?.let { icon ->
                Loaded(icon, null, Ps2IconRenderer.choosePose(icon, null, Ps2IconRenderer.Options()))
            }
        }
    }

    /**
     * One icon per game for the screensaver: each game's newest save across the cards, then, for a
     * game with no save, the icon found on its disc. [titles] maps serials to the library's names,
     * which read better than a save's own ("MGS3 GAME DATA 001"). Reads the cards.
     */
    fun showIcons(context: Context, titles: Map<String, String>): List<ShowIcon> {
        val out = ArrayList<ShowIcon>()
        val seen = HashSet<String>()
        for (ref in allSaves(context)) { // newest first
            val serial = ref.serial?.uppercase()
            if (!seen.add(serial ?: "${ref.card.path}|${ref.folder}")) continue
            out += ShowIcon("card:${ref.card.path}|${ref.folder}", serial?.let { titles[it] } ?: ref.title, serial, ref.card.nameWithoutExtension) { loadForViewer(ref) }
        }
        for (icon in discIcons(titles)) if (seen.add(icon.serial ?: icon.key)) out += icon
        return out
    }

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

    /** A library game, for looking on its disc: its serial and where its image is. */
    class DiscGame(val serial: String, val uri: Uri)

    /** Scan every card in the memcards folder again, in the background, and render any cover that
     *  is new or changed. With several cards, each game takes its newest save across all of them.
     *  Cheap when nothing changed: a scan reads directories and each save's timestamps, and a
     *  cover is only drawn once per save version. Then, for [games] with no save, look on their
     *  discs (see [discPass]). */
    fun refresh(context: Context, games: List<DiscGame> = emptyList()) {
        if (!enabled.value) return
        val app = context.applicationContext
        scope.launch {
            // One scan at a time; a request during a scan waits and then sees the finished cache.
            scanLock.withLock {
                val found = runCatching { scan(app) }.onFailure { Log.w(TAG, "scan failed", it) }.getOrNull()
                    ?: return@withLock
                if (found != cardCovers) {
                    cardCovers = found
                    merge()
                    // A changed save may be one already loaded for animating.
                    synchronized(loadedIcons) { loadedIcons.clear() }
                    withContext(Dispatchers.Main) { generation.intValue++ }
                }
            }
            if (games.isNotEmpty()) discPass(app, games)
        }
    }

    private var discJob: Job? = null

    /**
     * Games with no save still get a cover when their own disc has the icon (see [DiscIcons]).
     * Each disc is looked at once, a second or so at most, one at a time, and what was found is
     * kept, "nothing" included, so later passes only check the cache. Stops while a game is loaded,
     * so it never competes with the emulator for the storage; the next pass picks up from there.
     */
    private fun discPass(app: Context, games: List<DiscGame>) {
        discJob?.cancel()
        discJob = scope.launch {
            val icons = File(app.cacheDir, DISC_ICON_DIR).apply { mkdirs() }
            val coversDir = File(app.cacheDir, DISC_COVER_DIR).apply { mkdirs() }
            var changed = false
            var lastShown = System.nanoTime()
            val covers = HashMap(discCovers)
            val srcs = HashMap(discSources)
            for (game in games.distinctBy { it.serial }) {
                if (!isActive || !enabled.value) break
                if (MainActivityRuntime.eState.value != EmuState.STOPPED) break
                val serial = game.serial.uppercase()
                if (serial in cardCovers || serial in covers) continue
                val key = discKey(game.uri)
                val icn = File(icons, "$key.icn")
                val none = File(icons, "$key.none")
                if (none.isFile) continue
                if (!icn.isFile) {
                    val found = runCatching { findOnDisc(app, game.uri) }
                        .onFailure { Log.w(TAG, "disc ${game.uri}", it) }.getOrNull()
                    if (found == null) {
                        runCatching { none.createNewFile() }
                        continue
                    }
                    Log.i(TAG, "$serial: icon on its disc, ${found.where}")
                    val tmp = File(icons, "$key.tmp")
                    tmp.writeBytes(found.icon)
                    if (!tmp.renameTo(icn)) { tmp.delete(); continue }
                }
                val png = File(coversDir, "${serial}_$key.png")
                if (!png.isFile) {
                    val icon = Ps2Icon.parse(icn.readBytes()) ?: continue
                    runCatching { writePng(Ps2IconRenderer.render(icon, null, COVER_W, COVER_H, COVER_OPTIONS), png) }
                        .onFailure { Log.w(TAG, "render disc icon $serial", it) }
                    if (!png.isFile) continue
                }
                covers[serial] = png
                srcs[serial] = DiscSource(icn)
                changed = true
                // Show them as they come rather than all at the end, a few at a time.
                if (System.nanoTime() - lastShown > 1_500_000_000L) {
                    publishDisc(covers, srcs)
                    changed = false
                    lastShown = System.nanoTime()
                }
            }
            if (changed) publishDisc(covers, srcs)
        }
    }

    private suspend fun publishDisc(covers: Map<String, File>, srcs: Map<String, Source>) {
        discCovers = HashMap(covers)
        discSources = HashMap(srcs)
        merge()
        withContext(Dispatchers.Main) { generation.intValue++ }
    }

    private fun findOnDisc(app: Context, uri: Uri): DiscIcons.Found? {
        val pfd = app.contentResolver.openFileDescriptor(uri, "r") ?: return null
        val stream = FileInputStream(pfd.fileDescriptor)
        val disc = DiscImage.open(ChannelSource(stream.channel) { stream.close(); pfd.close() }) ?: return null
        return disc.use { DiscIcons.find(it) }
    }

    // A disc is known by where it is; the renderer's version is in it so a change redraws.
    private fun discKey(uri: Uri): String {
        val md = MessageDigest.getInstance("SHA-1")
        md.update("$uri|$RENDER_VERSION".toByteArray())
        return md.digest().take(8).joinToString("") { "%02x".format(it) }
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
        cardSources = out.keys.associateWith { serial -> picks.getValue(serial).let { CardSource(it.card, it.folder) } }
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
        writePng(Ps2IconRenderer.render(icon, sys, COVER_W, COVER_H, COVER_OPTIONS), file)
    }

    private fun writePng(px: IntArray, file: File) {
        val bmp = Bitmap.createBitmap(px, COVER_W, COVER_H, Bitmap.Config.ARGB_8888)
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bmp.recycle()
        if (!tmp.renameTo(file)) tmp.delete()
    }
}
