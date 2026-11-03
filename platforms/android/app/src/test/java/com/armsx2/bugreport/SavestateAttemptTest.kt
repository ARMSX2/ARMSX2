package com.armsx2.bugreport

import com.armsx2.bugreport.SavestateAttempt.Result
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The save state after a recent memory card write: let the game run for a moment, then try again once. */
class SavestateAttemptTest {
    private class FakeControl(var paused: Boolean) : GameControl {
        val events = mutableListOf<String>()
        override fun isPaused() = paused
        override fun resume() { events += "resume"; paused = false }
        override fun pause() { events += "pause"; paused = true }
        override fun awaitRunning(timeoutMs: Long): Boolean { events += "await"; return true }
        override fun awaitPaused(timeoutMs: Long): Boolean { events += "awaitPaused"; return true }
    }

    private var clock = 0L
    private val slept = mutableListOf<Long>()
    private val now = { clock }
    private val sleep = { ms: Long -> slept += ms; clock += ms }

    @Test
    fun aStateTakenFirstTimeLeavesTheGameAlone() {
        val control = FakeControl(paused = true)
        val result = SavestateAttempt.run({ true }, { false }, control, now = now, sleep = sleep)
        assertEquals(Result.SAVED, result)
        assertTrue(control.events.isEmpty())
    }

    @Test
    fun onceMakesOneTryAndNeverRunsTheGame() {
        var saves = 0
        assertEquals(Result.SAVED, SavestateAttempt.once({ saves++; true }, { true }))
        assertEquals(Result.MEMCARD_BUSY, SavestateAttempt.once({ saves++; false }, { true }))
        assertEquals(Result.FAILED, SavestateAttempt.once({ saves++; false }, { false }))
        assertEquals(3, saves)
    }

    @Test
    fun aRefusalForAnyOtherReasonIsNotRetried() {
        val control = FakeControl(paused = true)
        var saves = 0
        val result = SavestateAttempt.run({ saves++; false }, { false }, control, now = now, sleep = sleep)
        assertEquals(Result.FAILED, result)
        assertEquals(1, saves)
        assertTrue(control.events.isEmpty())
    }

    @Test
    fun aBusyCardLetsAPausedGameRunUntilItIsFreeThenPausesAndTriesAgain() {
        val control = FakeControl(paused = true)
        var saves = 0
        var busyChecks = 0
        // Busy for the first three looks, free after.
        val busy = { busyChecks++ < 4 }
        val result = SavestateAttempt.run({ saves++ >= 1 }, busy, control, now = now, sleep = sleep)

        assertEquals(Result.SAVED, result)
        assertEquals(2, saves)
        assertEquals(listOf("resume", "await", "pause"), control.events)
        assertTrue(control.paused)
        assertTrue(slept.isNotEmpty())
    }

    @Test
    fun theWaitIsAtMostTwoSecondsAndTheRetryIsOnlyOnce() {
        val control = FakeControl(paused = true)
        var saves = 0
        val result = SavestateAttempt.run({ saves++; false }, { true }, control, now = now, sleep = sleep)

        assertEquals(Result.MEMCARD_BUSY, result)
        assertEquals(2, saves)
        assertTrue(slept.sum() <= SavestateAttempt.MEMCARD_WAIT_MS + 50)
        assertTrue(control.paused)
    }

    @Test
    fun aGameThatWasRunningIsNotPausedBehindItsBack() {
        val control = FakeControl(paused = false)
        var saves = 0
        SavestateAttempt.run({ saves++ >= 1 }, { saves < 2 }, control, now = now, sleep = sleep)
        assertTrue(control.events.isEmpty())
        assertFalse(control.paused)
    }

    @Test
    fun anAbandonedCaptureStopsWaitingAndDoesNotTryAgain() {
        val control = FakeControl(paused = true)
        var saves = 0
        val result = SavestateAttempt.run({ saves++; false }, { true }, control, abandoned = { true }, now = now, sleep = sleep)
        assertEquals(Result.FAILED, result)
        assertEquals(1, saves)
        assertTrue(control.paused)
    }

    @Test
    fun hashingCanBeStoppedBetweenBlocks() {
        val file = java.io.File.createTempFile("hash", ".bin").apply { writeBytes(ByteArray(300_000)); deleteOnExit() }
        var blocks = 0
        try {
            Scrubber.sha256(file) { blocks++ >= 2 }
            throw AssertionError("expected the hash to be stopped")
        } catch (_: UploadCancelled) {
        }
        assertEquals(3, blocks)
        // Not stopped: the real digest.
        assertEquals(64, Scrubber.sha256(file).length)
    }
}
