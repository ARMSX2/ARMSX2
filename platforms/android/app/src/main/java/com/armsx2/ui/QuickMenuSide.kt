package com.armsx2.ui

import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime

/**
 * Which edge the in-game quick menu slides in from.
 *
 * A plain UI preference rather than a [com.armsx2.config.Settings] field: it never reaches the
 * core, and a per-game override would be actively wrong — handedness follows the person, not the
 * disc. Held in a [mutableStateOf] so flipping it re-lays-out the open menu immediately, which is
 * the point: you pick a side by looking at it, not by imagining it.
 *
 * Requested because the menu always came in from the right, so on a handheld it opens under the
 * hand holding the face buttons.
 */
object QuickMenuSide {
    private const val KEY = "ui.quickMenuLeft"

    /** True = the menu docks to the left edge. Default false, the original right-edge behaviour. */
    val left = mutableStateOf(false)

    fun load() {
        left.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY, false) }
            .getOrDefault(false)
    }

    fun set(onLeft: Boolean) {
        left.value = onLeft
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(KEY, onLeft).apply() }
    }
}
