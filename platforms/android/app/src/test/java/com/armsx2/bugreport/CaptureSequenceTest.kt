package com.armsx2.bugreport

import com.armsx2.bugreport.SavestateAttempt.Result
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** The order the save state and the graphics dump are taken in: the save state first, while the game is paused where the user left it. */
class CaptureSequenceTest {
    private class Recorder(
        private val first: Result,
        private val retry: Result = Result.SAVED,
    ) : CaptureSequence.Steps {
        val events = mutableListOf<String>()
        override fun saveNow(): Result { events += "saveNow"; return first }
        override fun saveAfterResume(): Result { events += "saveAfterResume"; return retry }
        override fun dump() { events += "dump" }
    }

    @Test
    fun theSaveStateIsTakenBeforeTheGameIsEverResumedForTheDump() {
        val steps = Recorder(Result.SAVED)
        val outcome = CaptureSequence.run(savestate = true, dump = true, gameWasPaused = true, steps = steps)
        assertEquals(listOf("saveNow", "dump"), steps.events)
        assertEquals(Result.SAVED, outcome.savestate)
        assertEquals(SavestateTiming.PAUSED_MOMENT, outcome.timing)
    }

    @Test
    fun aBusyCardIsRetriedAfterTheDumpAndTheStateIsLabelledAsNotFromThePausedMoment() {
        val steps = Recorder(Result.MEMCARD_BUSY, retry = Result.SAVED)
        val outcome = CaptureSequence.run(savestate = true, dump = true, gameWasPaused = true, steps = steps)
        assertEquals(listOf("saveNow", "dump", "saveAfterResume"), steps.events)
        assertEquals(SavestateTiming.AFTER_RESUME, outcome.timing)
    }

    @Test
    fun aBusyCardWithoutADumpIsRetriedRightAway() {
        val steps = Recorder(Result.MEMCARD_BUSY, retry = Result.SAVED)
        val outcome = CaptureSequence.run(savestate = true, dump = false, gameWasPaused = true, steps = steps)
        assertEquals(listOf("saveNow", "saveAfterResume"), steps.events)
        assertEquals(SavestateTiming.AFTER_RESUME, outcome.timing)
    }

    @Test
    fun aRetryThatIsRefusedTooLeavesNoTimingAndTheRetrysAnswer() {
        val steps = Recorder(Result.MEMCARD_BUSY, retry = Result.MEMCARD_BUSY)
        val outcome = CaptureSequence.run(savestate = true, dump = true, gameWasPaused = true, steps = steps)
        assertEquals(Result.MEMCARD_BUSY, outcome.savestate)
        assertNull(outcome.timing)
    }

    @Test
    fun aRefusalForAnyOtherReasonIsNotRetriedAndNeverRunsTheGameForIt() {
        val steps = Recorder(Result.FAILED)
        val outcome = CaptureSequence.run(savestate = true, dump = true, gameWasPaused = true, steps = steps)
        assertEquals(listOf("saveNow", "dump"), steps.events)
        assertEquals(Result.FAILED, outcome.savestate)
        assertNull(outcome.timing)
    }

    @Test
    fun aDumpAloneDoesNotTouchTheSaveState() {
        val steps = Recorder(Result.SAVED)
        val outcome = CaptureSequence.run(savestate = false, dump = true, gameWasPaused = true, steps = steps)
        assertEquals(listOf("dump"), steps.events)
        assertNull(outcome.savestate)
        assertNull(outcome.timing)
    }

    @Test
    fun aSaveStateAloneTakesNoDump() {
        val steps = Recorder(Result.SAVED)
        CaptureSequence.run(savestate = true, dump = false, gameWasPaused = true, steps = steps)
        assertEquals(listOf("saveNow"), steps.events)
    }

    @Test
    fun aGameThatWasNotPausedIsNotClaimedToHaveBeen() {
        val steps = Recorder(Result.SAVED)
        val outcome = CaptureSequence.run(savestate = true, dump = true, gameWasPaused = false, steps = steps)
        assertEquals(SavestateTiming.RUNNING, outcome.timing)
        assertEquals("running", outcome.timing?.wire)
    }

    @Test
    fun theWireNamesAreWhatTheReportSays() {
        assertEquals("paused_moment", SavestateTiming.PAUSED_MOMENT.wire)
        assertEquals("after_resume", SavestateTiming.AFTER_RESUME.wire)
    }

    @Test
    fun theRunClockCountsOnlyTheStretchesTheGameWasAskedToRun() {
        var clock = 1_000L
        val run = RunClock { clock }
        assertEquals(0L, run.totalMs())
        run.resumed()
        clock += 300
        assertEquals(300L, run.totalMs())
        run.paused()
        clock += 5_000
        assertEquals(300L, run.totalMs())
        // A second stretch adds to the first; a repeated resume or pause changes nothing.
        run.resumed()
        run.resumed()
        clock += 200
        run.paused()
        run.paused()
        clock += 9_000
        assertEquals(500L, run.totalMs())
    }

    @Test
    fun aTimedControlTellsTheClockAndPassesEverythingThrough() {
        var now = 0L
        val run = RunClock { now }
        val events = mutableListOf<String>()
        val inner = object : GameControl {
            var paused = true
            override fun isPaused() = paused
            override fun resume() { events += "resume"; paused = false }
            override fun pause() { events += "pause"; paused = true }
            override fun awaitRunning(timeoutMs: Long) = true.also { events += "awaitRunning" }
            override fun awaitPaused(timeoutMs: Long) = true.also { events += "awaitPaused" }
        }
        val timed = TimedControl(inner, run)
        timed.resume()
        now += 120
        timed.pause()
        timed.awaitRunning(1)
        timed.awaitPaused(1)
        assertEquals(listOf("resume", "pause", "awaitRunning", "awaitPaused"), events)
        assertEquals(120L, run.totalMs())
        assertEquals(true, timed.isPaused())
    }
}
