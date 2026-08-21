package com.armsx2

import android.content.Context
import androidx.compose.runtime.mutableIntStateOf
import com.armsx2.runtime.MainActivityRuntime
import java.io.File

/**
 * Show a game's cover art from a DIFFERENT region than the disc you own (requested by Sizor).
 *
 * Cover art is stored per SERIAL, and the region is baked into the serial with a different number
 * in every region (SLUS-21590 / SLES-54455 / SLPM-66271 are one game), so there is no URL to
 * switch — you have to know the OTHER region's serial. That mapping is built here from the
 * GameDB the app already ships: 12,800-odd entries keyed by serial, each with a `name` and, for
 * the 5,900 non-English ones, a `name-en`. Grouping serials by their ENGLISH title is what makes
 * Japanese art reachable at all — matching raw titles would fail on exactly the games people want
 * it for (Biohazard vs Resident Evil, Rockman vs Mega Man).
 *
 * Built lazily and only when a region is actually chosen: it is a 2.6 MB text scan, so a user on
 * the default never pays for it. Line-based rather than a real YAML parse — the file's shape is
 * fixed (serial at column 0, two-space-indented keys) and pulling in a parser for three fields
 * would cost more than it's worth.
 */
object CoverRegionIndex {

    /** 0 = the disc's own region (default), then the four regions worth switching between. */
    val region = mutableIntStateOf(0)

    private const val PREF_KEY = "library.coverRegion"
    private const val PREF_PER_GAME_KEY = "library.coverRegion.perGame"

    /** serial -> region index, for games the user pinned to a specific region.
     *
     *  The library-wide setting is the wrong grain on its own: someone who wants Japanese art for
     *  the handful of games whose Japanese covers are better does not want every Western game
     *  swapped too (Sizor). This overrides the global choice per serial, and 0 here means "this
     *  one follows the disc" — distinct from having no entry, which means "follow the library".
     *
     *  Bumped as a whole [mutableIntStateOf] generation rather than exposing the map as state: the
     *  cover URL is read from a plain getter on GameInfo, so the grid needs SOMETHING observable to
     *  recompose against, and the number of pinned games is far too small for the granularity to
     *  matter. */
    val perGameGeneration = mutableIntStateOf(0)
    @Volatile private var perGame: Map<String, Int> = emptyMap()

    fun load() {
        runCatching { region.intValue = MainActivityRuntime.prefs.getInt(PREF_KEY, 0) }
        runCatching {
            val raw = MainActivityRuntime.prefs.getString(PREF_PER_GAME_KEY, null).orEmpty()
            perGame = parsePerGame(raw)
        }
    }

    fun set(value: Int) {
        region.intValue = value
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_KEY, value).apply() }
        // The map is region-independent; only the lookup changes, so no rebuild is needed.
    }

    /** Whether anything currently asks for a non-disc region, so the caller knows to build the
     *  index at startup. A per-game pin counts: it is exactly as much of a reason as the
     *  library-wide setting, and checking only the latter left pinned games un-swapped until
     *  something else happened to trigger a build. */
    fun needsIndex(): Boolean = region.intValue != 0 || perGame.values.any { it != 0 }

    /** The region pinned for [serial], or null when it follows the library-wide choice. */
    fun regionFor(serial: String?): Int? =
        serial?.takeIf { it.isNotBlank() }?.let { perGame[it.uppercase()] }

    /** Pin [serial] to [value], or pass null to hand it back to the library-wide choice. */
    fun setFor(serial: String?, value: Int?) {
        val key = serial?.takeIf { it.isNotBlank() }?.uppercase() ?: return
        perGame = perGame.toMutableMap().also { if (value == null) it.remove(key) else it[key] = value }
        runCatching {
            MainActivityRuntime.prefs.edit()
                .putString(PREF_PER_GAME_KEY, perGame.entries.joinToString(",") { "${it.key}=${it.value}" })
                .apply()
        }
        perGameGeneration.intValue++
    }

    /** "SLUS-20946=3,SLES-51234=1" — a flat string rather than JSON because it is a serial-to-int
     *  map and nothing else, and prefs already store it as one value either way. */
    private fun parsePerGame(raw: String): Map<String, Int> =
        raw.split(',').mapNotNull { entry ->
            val i = entry.indexOf('=')
            if (i <= 0) return@mapNotNull null
            val v = entry.substring(i + 1).trim().toIntOrNull() ?: return@mapNotNull null
            entry.substring(0, i).trim().uppercase() to v.coerceIn(0, REGION_PREFIXES.lastIndex)
        }.toMap()

    /** Serial prefixes per region. Sony's own releases (SC*) sit alongside licensed ones (SL*). */
    private val REGION_PREFIXES: List<Set<String>> = listOf(
        emptySet(),                                   // 0 = disc's own region
        setOf("SLUS", "SCUS"),                        // 1 = USA
        setOf("SLES", "SCES", "SLED", "SCED"),        // 2 = Europe
        setOf("SLPS", "SLPM", "SCPS", "SLKA", "SCKA", "SCAJ", "SLAJ"), // 3 = Japan / Asia
    )

    /** englishTitleKey -> serials that share it. Null until built. */
    @Volatile private var byTitle: Map<String, List<String>>? = null
    /** serial -> englishTitleKey, so a game can find its own group. */
    @Volatile private var titleOf: Map<String, String>? = null
    @Volatile private var building = false

    /**
     * Serial whose cover should be shown for [serial], or null to use the disc's own.
     * Returns null (silently) whenever the index isn't built yet or the game has no counterpart in
     * the requested region — the caller then falls back, so a miss just looks like today.
     */
    fun coverSerialFor(serial: String?): String? {
        if (serial.isNullOrBlank()) return null
        // A per-game pin wins over the library-wide choice, including pinning back to "Disc".
        val effective = regionFor(serial) ?: region.intValue
        val wanted = REGION_PREFIXES.getOrNull(effective).orEmpty()
        if (wanted.isEmpty()) return null
        val key = titleOf?.get(serial.uppercase()) ?: return null
        val group = byTitle?.get(key) ?: return null
        // Already the right region? Keep it — no point swapping like for like.
        if (serial.substringBefore('-').uppercase() in wanted) return null
        return group.firstOrNull { it.substringBefore('-').uppercase() in wanted }
    }

    /** Parse the GameDB once, off the caller's thread. Safe to call repeatedly. */
    fun ensureBuilt(context: Context) {
        if (byTitle != null || building) return
        building = true
        Thread {
            runCatching { build(context) }
            building = false
        }.apply { isDaemon = true; name = "armsx2-cover-region" }.start()
    }

    private fun build(context: Context) {
        val file = File(MainActivityRuntime.assetCopyRoot(context), "resources/GameIndex.yaml")
        if (!file.isFile) return
        val groups = HashMap<String, MutableList<String>>(16384)
        val of = HashMap<String, String>(16384)
        var currentSerial: String? = null
        var name: String? = null
        var nameEn: String? = null

        fun flush() {
            val s = currentSerial ?: return
            // Prefer the English title so the Japanese release files under the same key as the
            // Western one — the whole point of the index.
            val title = nameEn ?: name ?: return
            val key = normalize(title)
            if (key.isNotEmpty()) {
                groups.getOrPut(key) { mutableListOf() }.add(s)
                of[s] = key
            }
            currentSerial = null; name = null; nameEn = null
        }

        file.bufferedReader().useLines { lines ->
            for (raw in lines) {
                if (raw.isEmpty() || raw.startsWith('#')) continue
                if (!raw[0].isWhitespace()) {
                    // Column 0 = a new serial key ("SLUS-20946:"), which ends the previous entry.
                    flush()
                    val s = raw.substringBefore(':').trim()
                    if (s.length in 8..12 && s.getOrNull(4) == '-') currentSerial = s.uppercase()
                } else if (currentSerial != null) {
                    val t = raw.trimStart()
                    when {
                        t.startsWith("name-en:") -> nameEn = unquote(t.removePrefix("name-en:"))
                        t.startsWith("name:") -> name = unquote(t.removePrefix("name:"))
                    }
                }
            }
        }
        flush()
        byTitle = groups
        titleOf = of
    }

    /** Strip the YAML quoting and any trailing `# comment`. */
    private fun unquote(v: String): String {
        var s = v.trim()
        // A '#' inside quotes is part of the title, so only cut one that follows the closing quote.
        if (s.startsWith("\"")) {
            val end = s.indexOf('"', 1)
            if (end > 0) return s.substring(1, end)
        }
        s = s.substringBefore('#').trim()
        return s.trim('"')
    }

    /**
     * Collapse a title to a comparison key: case, punctuation and the bracketed qualifiers the DB
     * appends ("[Asia Version]", "[Messiah Box]", "(Disc 1)") all differ between regional entries
     * for the same game, so keeping them would split the group and defeat the lookup.
     */
    private fun normalize(title: String): String {
        var s = title.lowercase()
        s = BRACKETS.replace(s, " ")
        s = NON_ALNUM.replace(s, " ")
        return s.trim().replace(WHITESPACE, " ")
    }

    private val BRACKETS = Regex("[\\[(][^\\])]*[\\])]")
    private val NON_ALNUM = Regex("[^a-z0-9 ]")
    private val WHITESPACE = Regex("\\s+")
}
