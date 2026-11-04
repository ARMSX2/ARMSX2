// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

/**
 * What `report.json`'s `driver` object says about the core's custom-driver status (whether the
 * pack was requested and whether it opened).
 *
 * The core keeps that status in process memory, set when the Vulkan library loads. A report built
 * from a preserved crash is collected in a new process where no game has run, so asking the core
 * there answers "nothing requested, nothing opened" whatever the crashed run did. For a crash the
 * status therefore comes from the marker, where the crashed process wrote it, or is left out and
 * said to be missing; it is never read live.
 */
sealed interface DriverStatusEntry {
    /** The status, as the JSON text the core produced. Goes in as `native_status`. */
    class Status(val json: String) : DriverStatusEntry

    /** No usable status. Goes in as `native_status_note`. */
    class Note(val text: String) : DriverStatusEntry

    companion object {
        const val AFTER_CRASH_NOTE = "not available: collected after the crash, in a new process"

        /**
         * [crashMarker] is the marker of a preserved crash, or null for a report about the process
         * the user is in. [live] asks the core now; it is only called when that answer is true.
         * Returns null when there is nothing to say.
         */
        fun choose(crashMarker: CrashMarker.Marker?, live: () -> String?): DriverStatusEntry? {
            if (crashMarker != null) {
                return if (crashMarker.nativeDriverStatus.isNotBlank()) Status(crashMarker.nativeDriverStatus)
                else Note(AFTER_CRASH_NOTE)
            }
            return live()?.takeIf { it.isNotBlank() }?.let(::Status)
        }
    }
}
