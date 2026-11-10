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
 *
 * It also says whether the app was in the foreground, and which boot of the device it was written
 * in. A process that dies while the app is in the background is not something to ask about, and
 * neither is one that a reboot ended; [CrashDecision] uses both.
 *
 * Two more things are recorded because only the crashed process knew them. The disc's CRC is
 * resolved before the game boots (the running VM often reports 0 for ISO and CHD images). The
 * native custom-driver status is added once the renderer has opened ([setDriverStatus]): the core
 * keeps it in process memory, so the new process that collects the report cannot ask for it.
 */
object CrashMarker {
    private const val PREFS = "armsx2_bugreport"
    private const val KEY = "crash.marker"

    /**
     * True once a game has been started in the log that is `logs/emulog.txt` now. Not part of the
     * marker, which is cleared when a game stops in an orderly way: a log that holds a finished game
     * is as worth keeping as one that holds a hang. See [LogBackup].
     */
    private const val KEY_LOG_HAS_GAME = "log.hasGame"

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
        /** True while the activity is started (visible); false once it has stopped. See [CrashMarker.setForeground]. */
        val foreground: Boolean = true,
        /** `Settings.Global.BOOT_COUNT` when the marker was written, or -1 if the system would not say. */
        val bootCount: Int = -1,
        /** The disc's CRC as 8 upper-case hex digits, or "" if it could not be worked out before boot. */
        val crc: String = "",
        /**
         * The core's custom-driver status (the JSON `NativeApp.getCustomDriverStatus` returns), taken
         * once the renderer had opened. "" while the game has not got that far, and in markers
         * written before this was recorded.
         */
        val nativeDriverStatus: String = "",
    ) {
        fun toJson(): JSONObject = JSONObject()
            .put("armedAt", armedAt).put("pid", pid).put("serial", serial).put("title", title)
            .put("driverId", driverId).put("driverName", driverName)
            .put("foreground", foreground).put("bootCount", bootCount)
            .put("crc", crc).put("nativeDriverStatus", nativeDriverStatus)

        companion object {
            fun fromJson(json: JSONObject) = Marker(
                armedAt = json.optLong("armedAt", 0L),
                pid = json.optInt("pid", 0),
                serial = json.optString("serial", ""),
                title = json.optString("title", ""),
                driverId = json.optString("driverId", ""),
                driverName = json.optString("driverName", ""),
                // A marker written before these were recorded says nothing; read it as "in the
                // foreground, same boot", which is what the older rule assumed.
                foreground = json.optBoolean("foreground", true),
                bootCount = json.optInt("bootCount", -1),
                crc = ReportCrc.normalize(json.optString("crc", "")),
                nativeDriverStatus = json.optString("nativeDriverStatus", ""),
            )
        }
    }

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /**
     * Records that a game is starting, and that the emulog now holds one. Synchronous: on disk when
     * this returns, in one commit, so the marker and the flag cannot disagree after a kill.
     */
    @Synchronized
    fun arm(context: Context, marker: Marker) {
        runCatching {
            prefs(context).edit().putString(KEY, marker.toJson().toString()).putBoolean(KEY_LOG_HAS_GAME, true).commit()
        }
    }

    /** Whether a game was started in the current `logs/emulog.txt`. False if it never was, or the log was moved aside since. */
    @Synchronized
    fun logHasGame(context: Context): Boolean = runCatching { prefs(context).getBoolean(KEY_LOG_HAS_GAME, false) }.getOrDefault(false)

    /** The emulog no longer holds a game: it was moved to the backup, taken as crash evidence, or is about to be truncated. Synchronous. */
    @Synchronized
    fun clearLogHasGame(context: Context) {
        runCatching { prefs(context).edit().remove(KEY_LOG_HAS_GAME).commit() }
    }

    /** The marker left by the previous run, or null if it ended in an orderly way. */
    @Synchronized
    fun read(context: Context): Marker? = runCatching {
        prefs(context).getString(KEY, null)?.let { Marker.fromJson(JSONObject(it)) }
    }.getOrNull()

    /** Marks the game as stopped in an orderly way. Synchronous. */
    @Synchronized
    fun clear(context: Context) {
        runCatching { prefs(context).edit().remove(KEY).commit() }
    }

    /**
     * Removes the marker only if it is still [expected], the one an earlier run left. The startup
     * work that deals with that marker finishes some seconds into the new run, by which time a game
     * may have been started and have armed a marker of its own; that one must stay.
     */
    @Synchronized
    fun clearIfUnchanged(context: Context, expected: Marker) {
        val now = read(context) ?: return
        if (now.pid == expected.pid && now.armedAt == expected.armedAt) clear(context)
    }

    /**
     * Records that the activity came into view ([foreground] true) or went out of it. Only a marker
     * written by this process is touched: at startup the marker on disk may still be the previous
     * run's, and a process that died in the background must stay recorded as one that did.
     */
    @Synchronized
    fun setForeground(context: Context, foreground: Boolean, pid: Int = android.os.Process.myPid()) {
        val now = read(context) ?: return
        if (now.pid != pid || now.foreground == foreground) return
        arm(context, now.copy(foreground = foreground))
    }

    /**
     * Adds the native custom-driver [status] to the marker of the game this process is running. Only
     * a marker written by this process is touched, for the reason given on [setForeground]; the
     * first status recorded for a run stays, since the driver cannot change under a running game.
     */
    @Synchronized
    fun setDriverStatus(context: Context, status: String, pid: Int = android.os.Process.myPid()) {
        val now = read(context) ?: return
        if (now.pid != pid || now.nativeDriverStatus.isNotEmpty() || status.isBlank()) return
        arm(context, now.copy(nativeDriverStatus = status))
    }
}
