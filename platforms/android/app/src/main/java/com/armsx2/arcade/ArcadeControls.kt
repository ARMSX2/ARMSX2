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
 * job's own button there (NativeApp.setArcadeRemap). There is a layout for every arcade game (All arcade
 * games: the pad button each button acts as) and one per game on top of it, button by button. A game with
 * nothing changed plays as it always did, and the sticks and analog pedals stay where they are.
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

    /** The layout every arcade game starts from (All arcade games), where a per-game one would be. */
    const val GLOBAL = "global"

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

    private fun prefKey(id: String) = "arcade.controls.$id"

    /** [gameId]'s own changes, over the All arcade games ones: pad key -> the job's key, or NOTHING (a
     *  button's own key undoes the global change for it). */
    fun changes(gameId: String): Map<Int, Int> = read(gameId)

    /** The All arcade games changes: pad key -> the pad key it acts as, or NOTHING. */
    fun globalChanges(): Map<Int, Int> = read(GLOBAL)

    private fun read(id: String): Map<Int, Int> =
        MainActivityRuntime.prefs.getString(prefKey(id), null).orEmpty()
            .split(',')
            .mapNotNull { pair ->
                val (from, to) = pair.split('=').takeIf { it.size == 2 } ?: return@mapNotNull null
                val f = from.toIntOrNull() ?: return@mapNotNull null
                val t = to.toIntOrNull() ?: return@mapNotNull null
                f to t
            }
            .toMap()

    /** The job [button] does in [gameId] now (its own change, else the All arcade games one), or null. */
    fun jobOf(gameId: String, layout: Layout, button: Int): Job? {
        val target = changes(gameId)[button] ?: globalChanges()[button] ?: button
        if (target == NOTHING) return null
        return layout.jobs.firstOrNull { target in it.keys }
    }

    /** Gives [button] the [job] (null: none) in [gameId], and to the game being played at once. The job
     *  All arcade games gives it is no change of the game's own. */
    fun set(gameId: String, layout: Layout, button: Int, job: Job?) {
        val all = changes(gameId).toMutableMap()
        val inherited = globalChanges()[button] ?: button
        val inheritedJob = if (inherited == NOTHING) null else layout.jobs.firstOrNull { inherited in it.keys }
        if (job == inheritedJob) all.remove(button)
        else all[button] = job?.key ?: NOTHING // over a global change too: the game's entry replaces it
        save(gameId, all)
    }

    /** Makes [button] act as the pad's [target] button (NOTHING: none, itself: its own) in every game. */
    fun setGlobal(button: Int, target: Int) {
        val all = globalChanges().toMutableMap()
        if (target == button) all.remove(button) else all[button] = target
        save(GLOBAL, all)
    }

    /** Every button of [id] (a game, or GLOBAL) back to what it does without changes there. */
    fun reset(id: String) = save(id, emptyMap())

    private fun save(id: String, all: Map<Int, Int>) {
        MainActivityRuntime.prefs.edit {
            if (all.isEmpty()) remove(prefKey(id))
            else putString(prefKey(id), all.entries.joinToString(",") { "${it.key}=${it.value}" })
        }
        Arcade.sessionGameId.value?.let { running -> if (id == GLOBAL || id == running) apply(running) }
    }

    /** Hands the core [gameId]'s layout, the All arcade games one with the game's own over it, for the game
     *  about to be played (null: the cabinet's own). An analog pedal keeps its trigger. */
    fun apply(gameId: String?) {
        if (!MainActivityRuntime.nativeReady.value) return
        val pairs = if (gameId == null) IntArray(0) else {
            val fixed = layout(gameId)?.jobs.orEmpty().filter { it.fixed }.flatMap { it.keys }.toSet()
            (globalChanges() + changes(gameId))
                .filter { (from, to) -> from !in fixed && from != to }
                .flatMap { listOf(it.key, it.value) }
                .toIntArray()
        }
        runCatching { NativeApp.setArcadeRemap(pairs) }
    }
}
