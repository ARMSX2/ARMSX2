// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONObject

/**
 * What was running when the process last started a game, written before the game starts and
 * removed when it stops in an orderly way. If it is still there at the next launch, the process
 * died with a game running.
 *
 * Same idea as the library background's "armed" preference, and written the same way: with
 * `commit()`, so it is on disk before the emulator can take the process down. Removing it also
 * commits, so a clean stop followed by an immediate exit is not reported as a crash.
 *
 * The record names the game and the GPU driver because by the next launch the settings may have
 * moved on, and "which driver was it using" is the question a crash report is mostly about.
 */
object CrashMarker {
    private const val PREFS = "armsx2_bugreport"
    private const val KEY = "crash.marker"

    data class Marker(
        /** Wall clock, milliseconds, when the game was started. */
        val armedAt: Long,
        /** The process that was running the game, to pick its entry out of Android's exit records. */
        val pid: Int,
        val serial: String,
        val title: String,
        /** Installed driver pack id, or "" for the system driver. */
        val driverId: String,
        /** Display name of that pack, or "" for the system driver. */
        val driverName: String,
    ) {
        fun toJson(): JSONObject = JSONObject()
            .put("armedAt", armedAt).put("pid", pid).put("serial", serial).put("title", title)
            .put("driverId", driverId).put("driverName", driverName)

        companion object {
            fun fromJson(json: JSONObject) = Marker(
                armedAt = json.optLong("armedAt", 0L),
                pid = json.optInt("pid", 0),
                serial = json.optString("serial", ""),
                title = json.optString("title", ""),
                driverId = json.optString("driverId", ""),
                driverName = json.optString("driverName", ""),
            )
        }
    }

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Records that a game is starting. Synchronous: on disk when this returns. */
    fun arm(context: Context, marker: Marker) {
        runCatching { prefs(context).edit().putString(KEY, marker.toJson().toString()).commit() }
    }

    /** The marker left by the previous run, or null if it ended in an orderly way. */
    fun read(context: Context): Marker? = runCatching {
        prefs(context).getString(KEY, null)?.let { Marker.fromJson(JSONObject(it)) }
    }.getOrNull()

    /** Marks the game as stopped in an orderly way. Synchronous. */
    fun clear(context: Context) {
        runCatching { prefs(context).edit().remove(KEY).commit() }
    }
}
