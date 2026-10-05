// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.arcade

import androidx.core.content.edit
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp

/**
 * The player's own layout for each arcade game: which job each pad button does on the game's cabinet
 * (Arcade controls, in All Settings > Controls and the pause menu's Controls). The jobs come from the core,
 * which knows each cabinet's controls (PCSX2x6's layouts); a button given another job stands in for that
 * job's own button there (NativeApp.setArcadeRemap). Saved per game. A game with nothing changed plays as
 * it always did, and the sticks and analog pedals stay where they are.
 */
object ArcadeControls {
    /** A job: its name, the pad keys doing it by default (the first stands in for it), and whether it is
     *  fixed (an analog pedal, which stays on its trigger). */
    data class Job(val label: String, val keys: List<Int>, val fixed: Boolean) {
        val key: Int get() = keys.first()
    }

    /** A game's jobs, and the mode its cabinet plays in (ACJV's JVS_MODE). */
    data class Layout(val mode: Int, val jobs: List<Job>) {
        /** The job [key] does when nothing is changed, if any. */
        fun defaultJob(key: Int): Job? = jobs.firstOrNull { key in it.keys }
    }

    /** The job of a button that does nothing on the cabinet. */
    const val NOTHING = -1

    // ACJV's JVS_MODE values, for what the settings say about the sticks.
    const val MODE_LIGHTGUN = 1
    const val MODE_DRIVE = 3
    const val MODE_DRUM = 4
    const val MODE_TWINSTICK = 7

    /** The pad's buttons, in the order the settings list them: pad keycodes (applyPadButton's). */
    val buttons: List<Int> = listOf(19, 20, 21, 22, 96, 97, 99, 100, 102, 103, 104, 105, 106, 107, 108, 109)

    /** A pad button's name, as the controller settings name it. */
    fun buttonName(key: Int): String =
        com.armsx2.input.ControllerMappings.stickTargets.firstOrNull { it.code == key }?.label ?: "Key $key"

    /** [gameId]'s jobs, from the core; null before the core is up, or when it cannot tell. */
    fun layout(gameId: String): Layout? {
        if (!MainActivityRuntime.nativeReady.value) return null
        val text = runCatching { NativeApp.getArcadeControls(gameId) }.getOrNull() ?: return null
        var mode = 0
        val jobs = ArrayList<Job>()
        for (line in text.lineSequence()) {
            val f = line.split('\t')
            if (f.size == 2 && f[0] == "mode") {
                mode = f[1].toIntOrNull() ?: 0
            } else if (f.size >= 3) {
                val keys = f[1].split(',').mapNotNull { it.trim().toIntOrNull() }
                if (keys.isNotEmpty()) jobs += Job(label(f[0]), keys, f[2] == "1")
            }
        }
        return Layout(mode, jobs).takeIf { jobs.isNotEmpty() }
    }

    /** A job's name: the app's own string ("@key", "@key:N" for "Button N"), or the cabinet's label. */
    private fun label(raw: String): String {
        if (!raw.startsWith("@")) return raw
        val key = raw.substring(1).substringBefore(':')
        val number = raw.substringAfter(':', "").toIntOrNull()
        return if (number != null) I18n.get(key).format(number) else I18n.get(key)
    }

    private fun prefKey(gameId: String) = "arcade.controls.$gameId"

    /** The buttons given another job in [gameId] than their own: pad key -> the job's key, or NOTHING. */
    fun changes(gameId: String): Map<Int, Int> =
        MainActivityRuntime.prefs.getString(prefKey(gameId), null).orEmpty()
            .split(',')
            .mapNotNull { pair ->
                val (from, to) = pair.split('=').takeIf { it.size == 2 } ?: return@mapNotNull null
                val f = from.toIntOrNull() ?: return@mapNotNull null
                val t = to.toIntOrNull() ?: return@mapNotNull null
                f to t
            }
            .toMap()

    /** The job [button] does in [gameId] now, or null for none. */
    fun jobOf(gameId: String, layout: Layout, button: Int): Job? {
        val target = changes(gameId)[button] ?: button
        if (target == NOTHING) return null
        return layout.jobs.firstOrNull { target in it.keys }
    }

    /** Gives [button] the [job] (null: none) in [gameId], and to the game being played at once. */
    fun set(gameId: String, layout: Layout, button: Int, job: Job?) {
        val all = changes(gameId).toMutableMap()
        if (job == layout.defaultJob(button)) all.remove(button)
        else all[button] = job?.key ?: NOTHING
        save(gameId, all)
    }

    /** Every button of [gameId] back to its own job. */
    fun reset(gameId: String) = save(gameId, emptyMap())

    private fun save(gameId: String, all: Map<Int, Int>) {
        MainActivityRuntime.prefs.edit {
            if (all.isEmpty()) remove(prefKey(gameId))
            else putString(prefKey(gameId), all.entries.joinToString(",") { "${it.key}=${it.value}" })
        }
        if (Arcade.sessionGameId.value == gameId) apply(gameId)
    }

    /** Hands the core [gameId]'s layout for the game about to be played (null: the cabinet's own). */
    fun apply(gameId: String?) {
        if (!MainActivityRuntime.nativeReady.value) return
        val pairs = gameId?.let { changes(it) }.orEmpty().flatMap { listOf(it.key, it.value) }.toIntArray()
        runCatching { NativeApp.setArcadeRemap(pairs) }
    }
}
