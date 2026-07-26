package com.armsx2

import com.armsx2.runtime.MainActivityRuntime

import android.os.SystemClock
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Per-game play-time + last-played tracking, persisted in [MainActivityRuntime.prefs] keyed by
 * PS2 serial. A "session" runs while the VM is RUNNING; pausing, stopping or
 * backgrounding accumulates the elapsed time into the per-serial total. Shown in
 * the Game Properties info tab.
 *
 * Session length uses the monotonic [SystemClock.elapsedRealtime] (immune to
 * wall-clock changes); the last-played stamp uses real time.
 */
object PlayTime {
    private const val SECS_PREFIX = "playtime.secs."
    private const val LAST_PREFIX = "playtime.last."
    // Last-known RetroAchievements progress, per serial. The core only reports achievements for the
    // game it currently has loaded (getAchievementsJSON is VM-scoped), so the library has no way to
    // ask "how far am I in this one" — the numbers have to be remembered while the game IS running.
    // Stored here because this object already owns the per-serial pref namespace.
    private const val ACH_UNLOCKED_PREFIX = "ach.unlocked."
    private const val ACH_TOTAL_PREFIX = "ach.total."

    private var sessionSerial: String? = null
    private var sessionStart: Long = 0L

    /** Bumped whenever a stored total changes. The library reads these figures inside `remember`,
     *  which would otherwise keep showing the value from before the session — play a game for an
     *  hour, come back, and the card would still say what it said on the way in. */
    val revision = androidx.compose.runtime.mutableStateOf(0)

    private fun bumpRevision() {
        // Called from the VM/GS threads as well as the UI thread; SnapshotState writes are safe off
        // the main thread, and Compose picks the change up on its next frame either way.
        revision.value = revision.value + 1
    }

    /** Begin counting for [serial], closing any open session first. No-op for a
     *  blank serial (BIOS boot / unidentified disc). */
    @Synchronized
    fun startSession(serial: String?) {
        endSession()
        val s = serial?.takeIf { it.isNotEmpty() } ?: return
        sessionSerial = s
        sessionStart = SystemClock.elapsedRealtime()
        MainActivityRuntime.prefs.edit().putLong(LAST_PREFIX + s, System.currentTimeMillis()).apply()
    }

    /** Accumulate the open session's elapsed time into its serial's total. */
    @Synchronized
    fun endSession() {
        val s = sessionSerial ?: return
        sessionSerial = null
        val elapsedMs = SystemClock.elapsedRealtime() - sessionStart
        if (elapsedMs < 1000L) return
        val prev = MainActivityRuntime.prefs.getLong(SECS_PREFIX + s, 0L)
        MainActivityRuntime.prefs.edit().putLong(SECS_PREFIX + s, prev + elapsedMs / 1000L).apply()
        bumpRevision()
    }

    fun playedSeconds(serial: String?): Long =
        serial?.takeIf { it.isNotEmpty() }?.let { MainActivityRuntime.prefs.getLong(SECS_PREFIX + it, 0L) } ?: 0L

    fun lastPlayedMillis(serial: String?): Long =
        serial?.takeIf { it.isNotEmpty() }?.let { MainActivityRuntime.prefs.getLong(LAST_PREFIX + it, 0L) } ?: 0L

    /** Remember achievement progress for [serial] so the library can show it later. Ignores a
     *  total of 0 — that means the set had not loaded yet, and writing it would blank a real
     *  figure every time a game starts. */
    fun recordAchievements(serial: String?, unlocked: Int, total: Int) {
        val s = serial?.takeIf { it.isNotEmpty() } ?: return
        if (total <= 0) return
        runCatching {
            MainActivityRuntime.prefs.edit()
                .putInt(ACH_UNLOCKED_PREFIX + s, unlocked.coerceIn(0, total))
                .putInt(ACH_TOTAL_PREFIX + s, total)
                .apply()
        }
        bumpRevision()
    }

    /** Last-known "unlocked / total", or null if this game has never reported a set. */
    fun achievements(serial: String?): Pair<Int, Int>? {
        val s = serial?.takeIf { it.isNotEmpty() } ?: return null
        val total = runCatching { MainActivityRuntime.prefs.getInt(ACH_TOTAL_PREFIX + s, 0) }.getOrDefault(0)
        if (total <= 0) return null
        val unlocked = runCatching { MainActivityRuntime.prefs.getInt(ACH_UNLOCKED_PREFIX + s, 0) }.getOrDefault(0)
        return unlocked to total
    }

    /** "" when zero, else e.g. "2h 15m", "45m", "30s". */
    fun formatPlayed(seconds: Long): String {
        if (seconds <= 0L) return ""
        val h = seconds / 3600L
        val m = (seconds % 3600L) / 60L
        return when {
            h > 0L -> "${h}h ${m}m"
            m > 0L -> "${m}m"
            else -> "${seconds}s"
        }
    }

    /** "" when never played, else e.g. "Jun 26, 2026 04:02". */
    fun formatLastPlayed(millis: Long): String {
        if (millis <= 0L) return ""
        return SimpleDateFormat("MMM d, yyyy HH:mm", Locale.getDefault()).format(Date(millis))
    }
}
