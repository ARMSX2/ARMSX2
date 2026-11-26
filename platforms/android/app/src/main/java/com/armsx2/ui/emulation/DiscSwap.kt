package com.armsx2.ui.emulation

import android.net.Uri
import androidx.compose.runtime.mutableStateOf
import com.armsx2.GameInfo
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.runtime.MainActivityRuntime
import java.util.Locale

/**
 * Swap Disc for a game on more than one disc: the game's other discs from the library, rather than
 * a file browser on whatever folder it showed last. A library entry is another disc of the game in
 * the drive when its file name is the same once the disc number is taken out ("Game (Disc 1).chd"
 * and "Game (Disc 2).chd") and its extension matches. Anything less certain gets the file browser,
 * which the chooser also offers.
 */
internal object DiscSwap {
    /** The discs on offer, while the chooser is up. */
    val choices = mutableStateOf<List<GameInfo>?>(null)

    // The image put in the drive by a swap, with the game it was swapped into: forgotten when
    // another game boots.
    private var mounted: Pair<Uri, String>? = null

    private val marker = Regex("""(?i)\b(?:disc|disk|cd)\s*[._-]?\s*(\d{1,2})\b""")

    /** The disc number in a file name, if it has one. */
    fun number(fileName: String): Int? =
        marker.find(fileName.substringBeforeLast('.'))?.groupValues?.get(1)?.toIntOrNull()

    private fun family(fileName: String): String? {
        val stem = fileName.substringBeforeLast('.')
        if (!marker.containsMatchIn(stem)) return null
        return marker.replace(stem, " ").lowercase(Locale.ROOT)
            .replace(Regex("[^\\p{L}\\p{N}]+"), " ").trim()
    }

    /** True when [a] and [b] are discs of one set: both carry a disc number, and they match once it
     *  is taken out, extension included. */
    fun sameDiscSet(a: String, b: String): Boolean {
        val key = family(a) ?: return false
        return a.substringAfterLast('.', "").equals(b.substringAfterLast('.', ""), ignoreCase = true) && family(b) == key
    }

    /** The discs of [current]'s game in [library], [current] among them, in disc order; empty when
     *  there is only the one. */
    fun siblings(current: GameInfo, library: List<GameInfo>): List<GameInfo> {
        val name = current.fileName ?: return emptyList()
        return library
            .filter { game -> game.fileName?.let { sameDiscSet(name, it) } == true }
            .distinctBy { it.uri }
            .sortedBy { number(it.fileName.orEmpty()) ?: Int.MAX_VALUE }
            .takeIf { it.size > 1 }
            .orEmpty()
    }

    /** The menu's Swap Disc: the chooser when the library holds this game's other discs, the file
     *  browser otherwise. */
    fun open() {
        val game = MainActivityRuntime.currentGame.value
        val context = MainActivityRuntime.instance?.applicationContext
        if (game == null || context == null) {
            MainActivityRuntime.promptSwapDisc()
            return
        }
        kotlin.concurrent.thread(name = "swap-disc-list") {
            val library = runCatching { GameLibraryRepository(context).loadCached().games }.getOrDefault(emptyList())
            val discs = siblings(game, library)
            MainActivityRuntime.instance?.runOnUiThread {
                if (discs.isEmpty()) MainActivityRuntime.promptSwapDisc() else choices.value = discs
            }
        }
    }

    fun choose(game: GameInfo) {
        choices.value = null
        if (!inDrive(game)) MainActivityRuntime.swapDiscTo(game.uri.toString())
    }

    fun browse() {
        choices.value = null
        MainActivityRuntime.promptSwapDisc()
    }

    fun close() {
        choices.value = null
    }

    /** Called once a swap has gone through. */
    fun noteMounted(uri: String) {
        val booted = MainActivityRuntime.currentGame.value?.uri ?: return
        mounted = booted to uri
    }

    fun inDrive(game: GameInfo): Boolean {
        val booted = MainActivityRuntime.currentGame.value ?: return false
        val swapped = mounted?.takeIf { it.first == booted.uri }?.second ?: return game.uri == booted.uri
        return game.uri.toString() == swapped
    }
}
