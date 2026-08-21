package com.armsx2

import androidx.compose.runtime.mutableIntStateOf
import com.armsx2.runtime.MainActivityRuntime

/**
 * What the second-screen panel shows, and in what order (NiceRon: "I'd love to be able to
 * customize what options are being shown... a grid which can be filled with boxes containing the
 * things one need like quick save/load, fps info, fast forward, aspect ratio and so on").
 *
 * A declared registry rather than hard-coded rows in [SecondScreen] so the panel builder is one
 * loop over the user's list, and so the settings UI can enumerate the choices without knowing
 * anything about them. Adding a tile is one entry here plus one `when` branch in the panel.
 *
 * ORDER IS PART OF THE PREFERENCE — the stored list is what gets laid out, top-left to
 * bottom-right, which is why it is a List and not a Set.
 */
enum class SecondScreenTile(val id: String, val labelKey: String, val stat: Boolean = false) {
    // Read-outs. These fill their box with live text and ignore taps.
    TITLE("title", "secondScreen.tile.title", stat = true),
    FPS("fps", "secondScreen.tile.fps", stat = true),
    SPEED("speed", "secondScreen.tile.speed", stat = true),
    BATTERY("battery", "secondScreen.tile.battery", stat = true),
    CLOCK("clock", "secondScreen.tile.clock", stat = true),
    ACHIEVEMENTS("achievements", "secondScreen.tile.achievements", stat = true),

    // Actions.
    SAVE("save", "touch.stateAction.save"),
    LOAD("load", "touch.stateAction.load"),
    FAST_FORWARD("ff", "secondScreen.fastForward"),
    PAUSE("pause", "secondScreen.pause"),
    SCREENSHOT("screenshot", "touch.stateAction.screenshot"),
    ASPECT("aspect", "secondScreen.tile.aspect"),
    SLOT("slot", "secondScreen.tile.slot"),
    // The way out from the panel itself — asked for after the panel landed on the display the game
    // was running on, with no way to dismiss it from there (BrainBeat: "I wonder if there is a way
    // to toggle it on inside the panel"). Turns the whole feature off, same as the App setting.
    HIDE("hide", "secondScreen.tile.hide"),

    MACRO1("macro1", "secondScreen.tile.macro1"),
    MACRO2("macro2", "secondScreen.tile.macro2"),
    MACRO3("macro3", "secondScreen.tile.macro3"),
    MACRO4("macro4", "secondScreen.tile.macro4"),
}

object SecondScreenLayout {

    private const val PREF_TILES = "secondScreen.tiles"
    private const val PREF_COLUMNS = "secondScreen.columns"

    /** What the panel showed before it was customisable, so an existing user's panel is unchanged
     *  by the update and a new one starts somewhere sensible. */
    private val DEFAULT = listOf(
        SecondScreenTile.TITLE,
        SecondScreenTile.FPS,
        SecondScreenTile.BATTERY,
        SecondScreenTile.CLOCK,
        SecondScreenTile.SAVE,
        SecondScreenTile.LOAD,
        SecondScreenTile.FAST_FORWARD,
        SecondScreenTile.PAUSE,
        SecondScreenTile.SCREENSHOT,
    )

    /** Bumped on any change so the panel knows to rebuild and Compose knows to recompose. */
    val generation = mutableIntStateOf(0)

    @Volatile private var tiles: List<SecondScreenTile> = DEFAULT
    @Volatile private var columnCount: Int = 3

    fun tiles(): List<SecondScreenTile> = tiles
    fun columns(): Int = columnCount

    fun load() {
        runCatching {
            val raw = MainActivityRuntime.prefs.getString(PREF_TILES, null)
            tiles = if (raw == null) DEFAULT else parse(raw)
            columnCount = MainActivityRuntime.prefs.getInt(PREF_COLUMNS, 3).coerceIn(1, 6)
        }
    }

    fun setColumns(value: Int) {
        columnCount = value.coerceIn(1, 6)
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_COLUMNS, columnCount).apply() }
        generation.intValue++
    }

    /** Add or remove a tile. Adding appends, so the user builds the order by the sequence they
     *  turn things on — the alternative (snapping back to enum order) silently discards an
     *  arrangement they may have just spent time on. */
    fun toggle(tile: SecondScreenTile) {
        tiles = if (tile in tiles) tiles - tile else tiles + tile
        persist()
    }

    /** Move a tile one place earlier/later in the layout. */
    fun move(tile: SecondScreenTile, delta: Int) {
        val from = tiles.indexOf(tile)
        if (from < 0) return
        val to = (from + delta).coerceIn(0, tiles.lastIndex)
        if (to == from) return
        tiles = tiles.toMutableList().also { it.removeAt(from); it.add(to, tile) }
        persist()
    }

    fun reset() {
        tiles = DEFAULT
        columnCount = 3
        runCatching {
            MainActivityRuntime.prefs.edit().remove(PREF_TILES).remove(PREF_COLUMNS).apply()
        }
        generation.intValue++
    }

    private fun persist() {
        runCatching {
            MainActivityRuntime.prefs.edit()
                .putString(PREF_TILES, tiles.joinToString(",") { it.id })
                .apply()
        }
        generation.intValue++
    }

    /** Stored by [SecondScreenTile.id], not by ordinal — a tile inserted mid-enum later must not
     *  re-point everyone's saved layout at a different tile. Unknown ids are dropped, which is how
     *  a layout survives a tile being removed in a future build. */
    private fun parse(raw: String): List<SecondScreenTile> {
        val byId = SecondScreenTile.entries.associateBy { it.id }
        return raw.split(',').mapNotNull { byId[it.trim()] }
    }
}
