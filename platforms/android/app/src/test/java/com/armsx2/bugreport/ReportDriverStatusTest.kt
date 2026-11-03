package com.armsx2.bugreport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What a report says about the native custom-driver status, for a crash and for everything else. */
class ReportDriverStatusTest {
    private val recorded = """{"requested":true,"opened":true,"required":false,"dir":"/d","name":"libvulkan_x.so","redirect_dir":"","hook_lib_dir":"","failure":""}"""
    private val fresh = """{"requested":false,"opened":false,"required":false,"dir":"","name":"","redirect_dir":"","hook_lib_dir":"","failure":""}"""

    private fun marker(status: String) = CrashMarker.Marker(1L, 4242, "SCUS-97481", "God of War II", "pack", "Pack", nativeDriverStatus = status)

    @Test
    fun aCrashReportUsesWhatTheCrashedRunRecordedAndNeverAsksTheCore() {
        var asked = false
        val entry = DriverStatusEntry.choose(marker(recorded)) { asked = true; fresh }
        assertTrue(entry is DriverStatusEntry.Status)
        assertEquals(recorded, (entry as DriverStatusEntry.Status).json)
        assertFalse(asked)
    }

    @Test
    fun aCrashThatNeverRecordedAStatusOmitsItAndSaysWhyInsteadOfReadingTheNewProcess() {
        var asked = false
        // The new process's own answer is the misleading all-false one from the real report.
        val entry = DriverStatusEntry.choose(marker("")) { asked = true; fresh }
        assertTrue(entry is DriverStatusEntry.Note)
        assertEquals("not available: collected after the crash, in a new process", (entry as DriverStatusEntry.Note).text)
        assertFalse(asked)
    }

    @Test
    fun aReportAboutTheRunningProcessAsksTheCore() {
        val entry = DriverStatusEntry.choose(null) { recorded }
        assertEquals(recorded, (entry as DriverStatusEntry.Status).json)
    }

    @Test
    fun aReportAboutTheRunningProcessWithoutAnAnswerSaysNothing() {
        assertNull(DriverStatusEntry.choose(null) { null })
        assertNull(DriverStatusEntry.choose(null) { "  " })
    }
}
