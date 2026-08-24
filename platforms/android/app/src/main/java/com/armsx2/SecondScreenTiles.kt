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
enum class SecondScreenTile(val id: String, val labelKey: String, val stat: Boolean = false,
    /**
     * A glyph shown above the label on ACTION tiles, so a tile can be recognised at a glance
     * from across a desk -- "much easier to quickly recognize pictures than text" (NiceRon).
     *
     * Deliberately geometric Unicode rather than emoji: emoji render in their own colours and
     * their own house style, which is exactly the stock-Android look the panel was being
     * restyled away from. These take the theme accent like everything else.
     *
     * Stat tiles leave this empty -- their label IS the identifier, and a value needs the
     * second line.
     */
    val icon: String = "",
) {
    // Read-outs. These fill their box with live text and ignore taps.
    TITLE("title", "secondScreen.tile.title", stat = true),
    FPS("fps", "secondScreen.tile.fps", stat = true),
    SPEED("speed", "secondScreen.tile.speed", stat = true),
    BATTERY("battery", "secondScreen.tile.battery", stat = true),
    CLOCK("clock", "secondScreen.tile.clock", stat = true),
    ACHIEVEMENTS("achievements", "secondScreen.tile.achievements", stat = true),
    // Thermals (Cotcho, Mike22). Stat tiles like the rest -- a device with no readable zone
    // simply shows a dash rather than the tile being hidden, so the grid does not reflow
    // depending on what the kernel happens to expose.
    // The game's cover, and more of what RetroAchievements already knows. The panel had the
    // title as text and a bare unlocked-count; on a screen sitting beside you, the cover is what
    // makes it read as "this game" at a glance.
    COVER("cover", "secondScreen.tile.cover", stat = true),
    RA_POINTS("rapoints", "secondScreen.tile.raPoints", stat = true),
    RA_RECENT("rarecent", "secondScreen.tile.raRecent", stat = true),
    RICH_PRESENCE("presence", "secondScreen.tile.presence", stat = true),

    // The rest of what the in-game OSD shows (Mike22). Backed by new JNI getters -- until those
    // existed, FPS was the only figure the panel could reach.
    VPS("vps", "secondScreen.tile.vps", stat = true),
    CPU_LOAD("cpuload", "secondScreen.tile.cpuLoad", stat = true),
    GS_LOAD("gsload", "secondScreen.tile.gsLoad", stat = true),
    GPU_LOAD("gpuload", "secondScreen.tile.gpuLoad", stat = true),
    FRAME_TIME("frametime", "secondScreen.tile.frameTime", stat = true),
    CPU_TEMP("cputemp", "secondScreen.tile.cpuTemp", stat = true),
    GPU_TEMP("gputemp", "secondScreen.tile.gpuTemp", stat = true),
    BATTERY_TEMP("battemp", "secondScreen.tile.batteryTemp", stat = true),

    // Actions.
    SAVE("save", "touch.stateAction.save", icon = "▼"),
    LOAD("load", "touch.stateAction.load", icon = "▲"),
    FAST_FORWARD("ff", "secondScreen.fastForward", icon = "▶▶"),
    PAUSE("pause", "secondScreen.pause", icon = "❚❚"),
    SCREENSHOT("screenshot", "touch.stateAction.screenshot", icon = "◉"),
    ASPECT("aspect", "secondScreen.tile.aspect", icon = "▭"),
    SLOT("slot", "secondScreen.tile.slot", icon = "▣"),
    // The way out from the panel itself — asked for after the panel landed on the display the game
    // was running on, with no way to dismiss it from there (BrainBeat: "I wonder if there is a way
    // to toggle it on inside the panel"). Turns the whole feature off, same as the App setting.
    HIDE("hide", "secondScreen.tile.hide", icon = "✕"),
    // "Not on THIS screen" as distinct from "off entirely" — the panel is the only place that
    // knows which display it landed on, so the opt-out belongs on it.
    NOT_HERE("nothere", "secondScreen.tile.notHere", icon = "⤫"),

    MACRO1("macro1", "secondScreen.tile.macro1", icon = "①"),
    MACRO2("macro2", "secondScreen.tile.macro2", icon = "②"),
    MACRO3("macro3", "secondScreen.tile.macro3", icon = "③"),
    MACRO4("macro4", "secondScreen.tile.macro4", icon = "④"),
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

    /**
     * Tile height in dp, or 0 for "as tall as the text needs".
     *
     * Asked for as "be able to size the tiles by myself based on a fixed max height/width"
     * (NiceRon). Width is already the column count -- tiles share the row equally, so choosing
     * columns IS choosing width, and a second width control would just be a way to disagree with
     * it. Height had no control at all, which is why a panel could only ever be as tall as its
     * text; this is the missing half.
     */
    private const val PREF_TILE_HEIGHT = "secondScreen.tileHeight"
    @Volatile private var tileHeightDp: Int = 0

    fun tiles(): List<SecondScreenTile> = tiles
    fun columns(): Int = columnCount
    fun tileHeight(): Int = tileHeightDp

    fun setTileHeight(dp: Int) {
        tileHeightDp = dp.coerceIn(0, 200)
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_TILE_HEIGHT, tileHeightDp).apply() }
        generation.intValue++
    }

    fun load() {
        runCatching {
            val raw = MainActivityRuntime.prefs.getString(PREF_TILES, null)
            tiles = if (raw == null) DEFAULT else parse(raw)
            columnCount = MainActivityRuntime.prefs.getInt(PREF_COLUMNS, 3).coerceIn(1, 6)
            tileHeightDp = MainActivityRuntime.prefs.getInt(PREF_TILE_HEIGHT, 0).coerceIn(0, 200)
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
        tileHeightDp = 0
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
