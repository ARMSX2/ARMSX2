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

    fun load() {
        runCatching { region.intValue = MainActivityRuntime.prefs.getInt(PREF_KEY, 0) }
    }

    fun set(value: Int) {
        region.intValue = value
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_KEY, value).apply() }
        // The map is region-independent; only the lookup changes, so no rebuild is needed.
    }

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
        val wanted = REGION_PREFIXES.getOrNull(region.intValue).orEmpty()
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
