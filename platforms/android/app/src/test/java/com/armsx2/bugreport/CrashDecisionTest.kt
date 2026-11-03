package com.armsx2.bugreport

import com.armsx2.bugreport.CrashDecision.Verdict
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** When the app asks "send a crash report?" after a launch that found a leftover marker. */
class CrashDecisionTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val armedAt = 1_762_000_000_000L
    private val boot = 40

    private fun marker(foreground: Boolean = true, bootCount: Int = boot) =
        CrashMarker.Marker(armedAt, 4242, "SCUS-97265", "Ratchet & Clank", "turnip", "turnip axfl3", foreground, bootCount)

    private fun record(reason: Int, importance: Int = ExitInfo.IMPORTANCE_FOREGROUND) =
        ExitRecord(4242, reason, 0, importance, "", armedAt + 60_000, 0, 0, "com.armsx2")

    private fun decide(
        sdk: Int = 34,
        marker: CrashMarker.Marker = marker(),
        record: ExitRecord? = null,
        javaCrash: Boolean = false,
        bootCount: Int = boot,
    ) = CrashDecision.decide(sdk, marker, record, javaCrash, bootCount)

    private val prompt = Verdict.Prompt(closedBySystem = false)
    private val closedBySystem = Verdict.Prompt(closedBySystem = true)

    @Test
    fun aCrashOrAnAnrOrAFailedStartIsAskedAbout() {
        for (reason in listOf(ExitInfo.REASON_CRASH, ExitInfo.REASON_CRASH_NATIVE, ExitInfo.REASON_ANR, ExitInfo.REASON_INITIALIZATION_FAILURE)) {
            assertEquals("reason $reason", prompt, decide(record = record(reason)))
        }
    }

    @Test
    fun aCrashIsAskedAboutEvenIfTheSystemRecordedItAsCachedByThenButTheAppWasInView() {
        // The importance of a crash record does not matter: the marker says the user was there.
        assertEquals(prompt, decide(record = record(ExitInfo.REASON_CRASH_NATIVE, importance = 400)))
    }

    @Test
    fun aKillByTheSystemIsAskedAboutOnlyIfTheProcessWasInTheForeground() {
        for (reason in listOf(ExitInfo.REASON_LOW_MEMORY, ExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE, ExitInfo.REASON_SIGNALED, ExitInfo.REASON_OTHER, ExitInfo.REASON_UNKNOWN)) {
            assertEquals("foreground $reason", closedBySystem, decide(record = record(reason, ExitInfo.IMPORTANCE_FOREGROUND)))
            assertEquals("visible $reason", Verdict.None, decide(record = record(reason, 200)))
            assertEquals("cached $reason", Verdict.None, decide(record = record(reason, 400)))
        }
    }

    @Test
    fun aDeliberateOrBookkeepingExitIsNeverAskedAbout() {
        val never = listOf(
            ExitInfo.REASON_EXIT_SELF, ExitInfo.REASON_USER_REQUESTED, ExitInfo.REASON_USER_STOPPED,
            ExitInfo.REASON_PERMISSION_CHANGE, ExitInfo.REASON_DEPENDENCY_DIED, ExitInfo.REASON_FREEZER,
            ExitInfo.REASON_PACKAGE_STATE_CHANGE, ExitInfo.REASON_PACKAGE_UPDATED,
        )
        for (reason in never) assertEquals("reason $reason", Verdict.None, decide(record = record(reason)))
        // Even with a Java crash log lying around: the record names this process and says what happened to it.
        for (reason in never) assertEquals("reason $reason", Verdict.None, decide(record = record(reason), javaCrash = true))
    }

    @Test
    fun aReasonThatDoesNotExistYetIsNotAskedAbout() {
        assertEquals(Verdict.None, decide(record = record(99)))
    }

    @Test
    fun aProcessThatDiedInTheBackgroundIsNeverAskedAbout() {
        val background = marker(foreground = false)
        for (reason in listOf(ExitInfo.REASON_CRASH, ExitInfo.REASON_CRASH_NATIVE, ExitInfo.REASON_ANR, ExitInfo.REASON_LOW_MEMORY, ExitInfo.REASON_SIGNALED)) {
            assertEquals("reason $reason", Verdict.None, decide(marker = background, record = record(reason)))
        }
        // Below API 30 the marker alone says so.
        assertEquals(Verdict.None, decide(sdk = 29, marker = background))
    }

    @Test
    fun aRebootBetweenTheMarkerAndNowIsNeverAskedAbout() {
        assertEquals(Verdict.None, decide(record = record(ExitInfo.REASON_CRASH), bootCount = boot + 1))
        assertEquals(Verdict.None, decide(sdk = 29, bootCount = boot + 1))
        assertEquals(Verdict.None, decide(sdk = 29, javaCrash = true, bootCount = boot + 1))
    }

    @Test
    fun anUnknownBootCountDoesNotSuppressAnything() {
        assertEquals(prompt, decide(record = record(ExitInfo.REASON_CRASH), bootCount = -1))
        assertEquals(prompt, decide(marker = marker(bootCount = -1), record = record(ExitInfo.REASON_CRASH), bootCount = 7))
        assertEquals(prompt, decide(sdk = 28, marker = marker(bootCount = -1), bootCount = 7))
    }

    @Test
    fun withoutAnExitRecordOnlyAJavaCrashLogCounts() {
        assertEquals(Verdict.None, decide(record = null))
        assertEquals(prompt, decide(record = null, javaCrash = true))
        // The app was out of view, but a Java exception is a Java exception.
        assertEquals(prompt, decide(marker = marker(foreground = false), record = null, javaCrash = true))
    }

    @Test
    fun belowAndroid11TheMarkerAndTheCrashLogDecide() {
        assertEquals(prompt, decide(sdk = 29))
        assertEquals(prompt, decide(sdk = 26))
        assertEquals(prompt, decide(sdk = 29, marker = marker(foreground = false), javaCrash = true))
        assertEquals(Verdict.None, decide(sdk = 29, marker = marker(foreground = false), javaCrash = false))
        // No exit record exists there, so one that were passed in would not be consulted either.
        assertEquals(prompt, decide(sdk = 29, record = record(ExitInfo.REASON_USER_REQUESTED)))
    }

    @Test
    fun theLevelWhereRecordsStartIsAndroid11() {
        assertEquals(Verdict.None, decide(sdk = 30, record = record(ExitInfo.REASON_EXIT_SELF)))
        assertEquals(prompt, decide(sdk = 29, record = record(ExitInfo.REASON_EXIT_SELF)))
    }

    @Test
    fun crashLogsAreDatedByTheirNameNotTheirModifiedTime() {
        assertTrue(CrashDecision.isCrashFileAfter("crash-1762000060000.txt", 0L, armedAt))
        assertFalse(CrashDecision.isCrashFileAfter("crash-1761999999000.txt", Long.MAX_VALUE, armedAt))
        assertFalse(CrashDecision.isCrashFileAfter("session.log", Long.MAX_VALUE, armedAt))
        assertFalse(CrashDecision.isCrashFileAfter("crash-1762000060000.txt.gz", Long.MAX_VALUE, armedAt))
    }

    @Test
    fun theLogsFolderIsSearchedForALaterCrashLog() {
        val logs = tmp.newFolder("logs")
        assertFalse(CrashDecision.javaCrashAfter(logs, armedAt))
        java.io.File(logs, "crash-${armedAt - 5000}.txt").writeText("old")
        assertFalse(CrashDecision.javaCrashAfter(logs, armedAt))
        java.io.File(logs, "crash-${armedAt + 5000}.txt").writeText("new")
        assertTrue(CrashDecision.javaCrashAfter(logs, armedAt))
        assertFalse(CrashDecision.javaCrashAfter(java.io.File(tmp.root, "nothing-here"), armedAt))
    }
}
