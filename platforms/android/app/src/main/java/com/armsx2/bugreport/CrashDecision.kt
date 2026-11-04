// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File

/**
 * Whether the process a leftover [CrashMarker] describes died in a way that deserves a "send a crash
 * report?" prompt. The prompt must come for real crashes and stay quiet otherwise, so the rule is
 * written down once, as a function of facts the caller has already looked up, and tested.
 *
 *  1. A different boot of the device than the one the marker was written in: no prompt. A reboot or
 *     a battery pull ended the process.
 *  2. Android 11 and up (the system keeps an exit record per process):
 *      - no record for the marker's pid: prompt only if a Java crash file newer than the marker exists;
 *      - the app was not in the foreground when the marker was last updated: no prompt, whatever the
 *        record says. A process that dies in the background is never a crash prompt;
 *      - CRASH, CRASH_NATIVE, ANR, INITIALIZATION_FAILURE: prompt;
 *      - LOW_MEMORY, EXCESSIVE_RESOURCE_USAGE, SIGNALED, OTHER, UNKNOWN: prompt only if the record's
 *        importance was foreground, and say that Android closed the app. A foreground out-of-memory
 *        kill while someone plays is a real problem. Anything else about the process (it was cached,
 *        in the background) is not;
 *      - every other reason (EXIT_SELF, USER_REQUESTED, USER_STOPPED, PERMISSION_CHANGE,
 *        DEPENDENCY_DIED, FREEZER, PACKAGE_STATE_CHANGE, PACKAGE_UPDATED, or one that does not exist
 *        yet): no prompt.
 *  3. Android 8 to 10 (no exit records): prompt if the marker says foreground, or a Java crash file
 *     newer than the marker exists.
 */
object CrashDecision {
    sealed interface Verdict {
        /** Nothing to ask the user about. */
        data object None : Verdict

        /** Ask. [closedBySystem] is true when Android ended the process (memory, resources) rather than it crashing. */
        data class Prompt(val closedBySystem: Boolean) : Verdict
    }

    /**
     * @param sdk the Android API level the app is running on.
     * @param marker what the previous run left behind.
     * @param record Android's exit record for `marker.pid`, or null if there is none (always null below API 30).
     * @param javaCrashAfterMarker a `crash-*.txt` written after the game was started exists.
     * @param bootCount `Settings.Global.BOOT_COUNT` now, or -1 if unknown.
     */
    fun decide(sdk: Int, marker: CrashMarker.Marker, record: ExitRecord?, javaCrashAfterMarker: Boolean, bootCount: Int): Verdict {
        if (marker.bootCount >= 0 && bootCount >= 0 && marker.bootCount != bootCount) return Verdict.None

        if (sdk < 30) {
            return if (marker.foreground || javaCrashAfterMarker) Verdict.Prompt(closedBySystem = false) else Verdict.None
        }
        if (record == null) {
            return if (javaCrashAfterMarker) Verdict.Prompt(closedBySystem = false) else Verdict.None
        }
        if (!marker.foreground) return Verdict.None
        return when (record.reason) {
            ExitInfo.REASON_CRASH,
            ExitInfo.REASON_CRASH_NATIVE,
            ExitInfo.REASON_ANR,
            ExitInfo.REASON_INITIALIZATION_FAILURE,
            -> Verdict.Prompt(closedBySystem = false)

            ExitInfo.REASON_LOW_MEMORY,
            ExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE,
            ExitInfo.REASON_SIGNALED,
            ExitInfo.REASON_OTHER,
            ExitInfo.REASON_UNKNOWN,
            -> if (record.importance <= ExitInfo.IMPORTANCE_FOREGROUND) Verdict.Prompt(closedBySystem = true) else Verdict.None

            else -> Verdict.None
        }
    }

    private val CRASH_FILE = Regex("^crash-(\\d+)\\.txt$")

    /**
     * True if [name] (with last-modified time [lastModified]) is a Java crash log written after
     * [sinceMs]. The time is the one in the name (the handler names the file with the clock), since
     * a file's modified time moves when it is copied or restored.
     */
    fun isCrashFileAfter(name: String, lastModified: Long, sinceMs: Long): Boolean {
        val match = CRASH_FILE.matchEntire(name) ?: return false
        val written = match.groupValues[1].toLongOrNull() ?: lastModified
        return written > sinceMs
    }

    /** True if [logsDir] holds a Java crash log written after [sinceMs]. */
    fun javaCrashAfter(logsDir: File, sinceMs: Long): Boolean =
        logsDir.listFiles { f -> f.isFile && isCrashFileAfter(f.name, f.lastModified(), sinceMs) }.orEmpty().isNotEmpty()
}
