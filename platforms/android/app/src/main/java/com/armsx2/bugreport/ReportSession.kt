// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.util.concurrent.atomic.AtomicBoolean

/** What the user can still say while a report is captured and packed. Read by the thread doing it. */
class SubmitControl {
    /** Send without the save state and graphics dump. */
    @Volatile
    var skipHeavy = false

    /** Throw the report away. */
    @Volatile
    var cancelled = false
}

/**
 * One report as the user builds it: what is wrong, what they typed, which heavy items they agreed
 * to. The flow's screens read and write this; [BugReport.prepare] and [BugReport.submit] act on it.
 */
class ReportSession(val origin: ReportOrigin, initialKind: ReportKind) {
    var kind by mutableStateOf(initialKind)
    var description by mutableStateOf("")

    /** The consent screen's switches. They start from the kind's defaults and are the user's after that. */
    var includeSavestate by mutableStateOf(origin.canCapture && initialKind.savestateByDefault)
    var includeGsDump by mutableStateOf(origin.canCapture && initialKind.gsDumpByDefault)

    /** Keep a copy of the graphics dump in the snaps folder, as the plain "Save GS dump" would. */
    var keepDumpCopy = false

    /** The collector, once the always-collected files have been gathered. */
    var collector: ReportCollector? = null

    /** What is in the folder so far, with real sizes. */
    var lightItems by mutableStateOf<List<BundleItem>>(emptyList())

    /** What could not be gathered and why. Shown on the consent screen so nothing is hidden. */
    var failedItems by mutableStateOf<Map<String, Failure>>(emptyMap())

    /** True while the always-collected files are being gathered. */
    var preparing by mutableStateOf(false)

    /** Gathering went wrong; the consent screen says so. What was gathered before it is still listed. */
    var prepareFailed by mutableStateOf(false)

    /** The user left the flow. A gathering still in progress removes its own folder when it ends. */
    @Volatile
    var closed = false

    /** Held while the files are being gathered, so going back and forward in the flow cannot start a second gathering. */
    val prepareLock = Any()

    /** Set when [BugReport.submit] starts, so a second Send does not start a second report. */
    val submitStarted = AtomicBoolean(false)

    /** The user's say while the report is captured and packed. */
    val stop = SubmitControl()

    /** True once the user has changed a switch, so changing the kind no longer resets them. */
    var switchesTouched by mutableStateOf(false)

    /** Applies [newKind]'s defaults to the switches, unless the user has already chosen. */
    fun chooseKind(newKind: ReportKind) {
        kind = newKind
        if (!switchesTouched) {
            includeSavestate = origin.canCapture && newKind.savestateByDefault
            includeGsDump = origin.canCapture && newKind.gsDumpByDefault
        }
    }

    /** Everything the consent screen lists, with the heavy items as estimates. */
    fun consentItems(): List<BundleItem> =
        ConsentItems.build(lightItems, origin.canCapture, includeSavestate, includeGsDump)

    fun totalBytes(): Long = consentItems().sumOf { it.bytes }
}
