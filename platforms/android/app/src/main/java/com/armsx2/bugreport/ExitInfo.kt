// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.io.InputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** One entry of Android's record of how this app's processes ended. */
data class ExitRecord(
    val pid: Int,
    val reason: Int,
    val status: Int,
    val importance: Int,
    val description: String,
    /** Milliseconds since the epoch. */
    val timestamp: Long,
    val pssKb: Long,
    val rssKb: Long,
    val processName: String,
) {
    val reasonName: String get() = ExitInfo.reasonName(reason)
}

/**
 * `ActivityManager.getHistoricalProcessExitReasons`: why the system says each of this app's recent
 * processes ended. The only place a native crash, an ANR or a low-memory kill is recorded in a form
 * the app itself can read afterwards. API 30 and up; before that there is nothing to read and every
 * function here reports that.
 */
object ExitInfo {
    // ApplicationExitInfo.REASON_*. Spelled as numbers because the newer ones do not exist below the
    // API level that added them; the values are part of the platform contract.
    private val NAMES = mapOf(
        0 to "UNKNOWN", 1 to "EXIT_SELF", 2 to "SIGNALED", 3 to "LOW_MEMORY", 4 to "CRASH",
        5 to "CRASH_NATIVE", 6 to "ANR", 7 to "INITIALIZATION_FAILURE", 8 to "PERMISSION_CHANGE",
        9 to "EXCESSIVE_RESOURCE_USAGE", 10 to "USER_REQUESTED", 11 to "USER_STOPPED",
        12 to "DEPENDENCY_DIED", 13 to "OTHER", 14 to "FREEZER", 15 to "PACKAGE_STATE_CHANGE",
        16 to "PACKAGE_UPDATED",
    )

    const val REASON_CRASH_NATIVE = 5
    const val REASON_ANR = 6

    fun reasonName(reason: Int): String = NAMES[reason] ?: "REASON_$reason"

    /**
     * True if the process ended because the user or the app asked for it: swiped away, force
     * stopped, the app's own exit(), or a permission or package change. A game that was running at
     * that point did not crash, so no crash prompt should follow.
     */
    fun endedOnRequest(reason: Int): Boolean = when (reason) {
        1, 8, 10, 11, 15, 16 -> true // EXIT_SELF, PERMISSION_CHANGE, USER_REQUESTED, USER_STOPPED, PACKAGE_*
        else -> false
    }

    /** Recent exits of this app, newest first. Empty below API 30 or if the platform will not say. */
    fun read(context: Context, maxNum: Int = 8): List<ExitRecord> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return emptyList()
        return runCatching {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            am.getHistoricalProcessExitReasons(context.packageName, 0, maxNum).map { it.toRecord() }
        }.getOrDefault(emptyList())
    }

    /** A trace the system kept for an exit: [reason] says whether it is a tombstone or an ANR trace. */
    class Trace(val reason: Int, val stream: InputStream)

    /**
     * The trace of the exit of process [pid] since [sinceMs], if the system kept one: the tombstone
     * (a protobuf, API 31 and up) for a native crash, the ANR trace for an ANR. Null when there is
     * none. The caller closes the stream.
     */
    fun openTrace(context: Context, pid: Int, sinceMs: Long): Trace? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
        return runCatching {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val info = am.getHistoricalProcessExitReasons(context.packageName, pid, 4)
                .firstOrNull { it.timestamp >= sinceMs && (it.reason == REASON_CRASH_NATIVE || it.reason == REASON_ANR) }
            val stream = info?.traceInputStream
            if (info != null && stream != null) Trace(info.reason, stream) else null
        }.getOrNull()
    }

    /** The records as the text file that goes in the report. */
    fun format(records: List<ExitRecord>, markedPid: Int = 0): String = buildString {
        if (records.isEmpty()) {
            appendLine("No exit records are available (Android 11 or later keeps them).")
            return@buildString
        }
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS Z", Locale.US)
        for (r in records) {
            append("pid=").append(r.pid)
            if (r.pid == markedPid) append("  <- the process that was running the game")
            appendLine()
            appendLine("  time=${stamp.format(Date(r.timestamp))} (ms=${r.timestamp})")
            appendLine("  reason=${r.reasonName} (${r.reason}) status=${r.status} importance=${r.importance}")
            appendLine("  process=${r.processName} pss=${r.pssKb}KB rss=${r.rssKb}KB")
            appendLine("  description=${r.description}")
        }
    }

    private fun ApplicationExitInfo.toRecord() = ExitRecord(
        pid = pid,
        reason = reason,
        status = status,
        importance = importance,
        description = description.orEmpty(),
        timestamp = timestamp,
        pssKb = pss,
        rssKb = rss,
        processName = processName.orEmpty(),
    )
}
