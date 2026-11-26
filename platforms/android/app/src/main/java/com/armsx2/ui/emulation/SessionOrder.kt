package com.armsx2.ui.emulation

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Rect
import com.armsx2.runtime.MainActivityRuntime

/**
 * The order of the pause menu's first tab, which players rearrange so what they use most is at the
 * top. Holding a row, or a card's title, opens the arrange view,
 * where rows are dragged into place; with a controller, A picks the selected row up, up and down
 * move it, and A puts it down.
 *
 * Saved as the rows' keys. A row a later version adds goes in after its default neighbour, and a
 * key that is not shown right now (the arcade buttons outside arcade games) keeps its saved place.
 */
internal object SessionOrder {
    private const val PREF = "menu.sessionOrder"

    private var saved by mutableStateOf<List<String>?>(null)
    private var loaded = false

    /** True while the arrange view replaces the tab. */
    var arranging by mutableStateOf(false)
        private set

    /** The row picked up, while one is. */
    var lifted by mutableStateOf<String?>(null)
        private set

    // The order being edited, and the rows the tab has right now.
    private var working by mutableStateOf<List<String>>(emptyList())
    private var defaults: List<String> = emptyList()
    private var beforeLift: List<String>? = null

    /** Where each arrange row sits on screen, for working out where a dragged row belongs. */
    internal val rowBounds = HashMap<String, Rect>()

    /** The visible part of the scrolling tab, so a drag near its edge can scroll it. */
    internal var viewport: Rect? = null

    private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        saved = runCatching { MainActivityRuntime.prefs.getString(PREF, null) }.getOrNull()
            ?.split(',')?.map(String::trim)?.filter(String::isNotEmpty)
    }

    /** The tab's rows in the player's order. */
    fun ordered(defaultKeys: List<String>): List<String> {
        ensureLoaded()
        defaults = defaultKeys
        return if (arranging) merge(working, defaultKeys) else merge(saved.orEmpty(), defaultKeys)
    }

    fun startArranging(lift: String? = null) {
        if (arranging) return
        working = merge(saved.orEmpty(), defaults)
        rowBounds.clear()
        arranging = true
        lifted = null
        if (lift != null && lift in working) lift(lift)
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
    }

    /** Closes the arrange view, keeping the order as it stands. */
    fun finishArranging() {
        if (!arranging) return
        lifted = null
        beforeLift = null
        arranging = false
        // Keys not shown right now keep their place from the saved order.
        val hidden = saved.orEmpty().filter { it !in working }
        val order = working + hidden
        saved = order
        runCatching { MainActivityRuntime.prefs.edit().putString(PREF, order.joinToString(",")).apply() }
    }

    /** Back to the default order, still in the arrange view so it can be adjusted from there. */
    fun resetOrder() {
        working = defaults
        lifted = null
        beforeLift = null
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.RESET)
    }

    fun lift(key: String) {
        if (!arranging || key !in working) return
        beforeLift = working
        lifted = key
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
    }

    /** Puts the picked-up row down where it is now. */
    fun drop() {
        if (lifted == null) return
        lifted = null
        beforeLift = null
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
    }

    /** Puts the picked-up row back where it was picked up. */
    fun putBack() {
        beforeLift?.let { working = it }
        lifted = null
        beforeLift = null
    }

    /** The order shown in the arrange view. */
    fun arrangedKeys(): List<String> = working

    fun indexOf(key: String): Int = working.indexOf(key)

    /** Moves the picked-up row [delta] places; false at either end. */
    fun step(delta: Int): Boolean {
        val key = lifted ?: return false
        val from = working.indexOf(key)
        val to = from + delta
        if (from < 0 || to !in working.indices) return false
        moveTo(key, to)
        return true
    }

    /** Moves [key] to [index] of the visible order. */
    fun moveTo(key: String, index: Int) {
        val from = working.indexOf(key)
        if (from < 0) return
        val to = index.coerceIn(0, working.lastIndex)
        if (to == from) return
        working = working.toMutableList().apply { add(to, removeAt(from)) }
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.NAV)
    }

    /** The saved keys that are in [defaults], in their saved order, with any row the saved order
     *  lacks inserted after its nearest default neighbour above (or first). */
    internal fun merge(order: List<String>, defaults: List<String>): List<String> {
        val out = order.filter { it in defaults }.distinct().toMutableList()
        defaults.forEachIndexed { i, key ->
            if (key in out) return@forEachIndexed
            val above = (i - 1 downTo 0).map { defaults[it] }.firstOrNull { it in out }
            out.add(if (above == null) 0 else out.indexOf(above) + 1, key)
        }
        return out
    }
}
