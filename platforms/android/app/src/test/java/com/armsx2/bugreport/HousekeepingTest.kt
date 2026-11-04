package com.armsx2.bugreport

import com.armsx2.bugreport.Housekeeping.Verdict
import org.junit.Assert.assertEquals
import org.junit.Test

/** When a report's folder on the device is no longer needed. */
class HousekeepingTest {
    private val day = 24L * 3600 * 1000
    private val now = 100 * day

    private fun info(
        name: String = "report-1", hasState: Boolean = true, phase: ReportPhase? = ReportPhase.FINISHED,
        createdAt: Long = now, finishedAt: Long = now, lastModified: Long = now, rank: Int = 0,
    ) = Housekeeping.Info(name, hasState, phase, createdAt, finishedAt, lastModified, rank)

    @Test
    fun aSentReportKeepsItsFilesForAWeek() {
        assertEquals(Verdict.KEEP, Housekeeping.decide(info(finishedAt = now - 6 * day), now))
        assertEquals(Verdict.DELETE, Housekeeping.decide(info(finishedAt = now - 8 * day), now))
    }

    @Test
    fun anUnfinishedReportExpiresAfterAWeekAndIsMarkedSo() {
        for (phase in listOf(ReportPhase.WAITING_RETRY, ReportPhase.UPLOADING, ReportPhase.READY, ReportPhase.FAILED)) {
            assertEquals(Verdict.KEEP, Housekeeping.decide(info(phase = phase, createdAt = now - 2 * day), now))
            assertEquals(Verdict.DELETE_AND_MARK_EXPIRED, Housekeeping.decide(info(phase = phase, createdAt = now - 8 * day), now))
        }
    }

    @Test
    fun unsentCrashEvidenceGoesAfterAWeekOrWhenThereAreTooMany() {
        val crash = "crash-123"
        assertEquals(Verdict.KEEP, Housekeeping.decide(info(crash, hasState = false, phase = null, lastModified = now - 2 * day), now))
        assertEquals(Verdict.DELETE, Housekeeping.decide(info(crash, hasState = false, phase = null, lastModified = now - 8 * day), now))
        assertEquals(Verdict.KEEP, Housekeeping.decide(info(crash, hasState = false, phase = null, rank = CrashEvidence.KEEP_COUNT - 1), now))
        assertEquals(Verdict.DELETE, Housekeeping.decide(info(crash, hasState = false, phase = null, rank = CrashEvidence.KEEP_COUNT), now))
    }

    @Test
    fun aReportFolderWithNoStateIsLeftAloneForADayThenCleared() {
        assertEquals(Verdict.KEEP, Housekeeping.decide(info(hasState = false, phase = null, lastModified = now - day / 2), now))
        assertEquals(Verdict.DELETE, Housekeeping.decide(info(hasState = false, phase = null, lastModified = now - 2 * day), now))
    }

    @Test
    fun aStoppedReportKeepsItsLineAndLosesItsFilesAfterAWeek() {
        assertEquals(Verdict.KEEP, Housekeeping.decide(info(phase = ReportPhase.STOPPED, createdAt = now - 2 * day), now))
        assertEquals(Verdict.DELETE, Housekeeping.decide(info(phase = ReportPhase.STOPPED, createdAt = now - 8 * day), now))
    }
}
