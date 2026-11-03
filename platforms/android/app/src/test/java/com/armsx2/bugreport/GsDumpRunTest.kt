package com.armsx2.bugreport

import com.armsx2.bugreport.GsDumpPhase.DONE
import com.armsx2.bugreport.GsDumpPhase.IDLE
import com.armsx2.bugreport.GsDumpPhase.PACKING
import com.armsx2.bugreport.GsDumpPhase.QUEUED
import com.armsx2.bugreport.GsDumpPhase.RECORDING
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The game runs until the dump's frames are recorded, not until its zip is built. */
class GsDumpRunTest {
    /** A core that answers from a script, one entry per question, and the last one for ever after. */
    private class Script(private val answers: List<GsDumpStatus?>) {
        var asked = 0
        fun next(): GsDumpStatus? = answers[minOf(asked++, answers.size - 1)]
    }

    private fun st(phase: GsDumpPhase, open: Int = 0, rec: Int = 0) = GsDumpStatus(phase, open, rec)

    private class Control(private val script: Script) : GameControl {
        val events = mutableListOf<String>()

        /** How many times the core had been asked when the game was resumed and when it was paused. */
        var resumedAtAsk = -1
        var pausedAtAsk = -1
        var paused = true
        override fun isPaused() = paused
        override fun resume() { events += "resume"; resumedAtAsk = script.asked; paused = false }
        override fun pause() { events += "pause"; pausedAtAsk = script.asked; paused = true }
        override fun awaitRunning(timeoutMs: Long) = true
        override fun awaitPaused(timeoutMs: Long) = true
    }

    private var clock = 0L
    private val now = { clock }
    private val sleep = { ms: Long -> clock += ms }

    private fun run(
        script: Script,
        control: Control,
        resume: Boolean = true,
        zipReady: () -> Boolean = { false },
        abandoned: () -> Boolean = { false },
        budgetMs: Long = 20_000,
    ) = GsDumpRun.run(resume, control, script::next, zipReady, abandoned, budgetMs, now, sleep)

    @Test
    fun theGameIsPausedOnTheFirstAnswerThatSaysTheFramesAreRecordedNotWhenTheZipIsDone() {
        val script = Script(
            listOf(
                st(QUEUED), st(QUEUED), st(RECORDING, open = 1), st(RECORDING, open = 1),
                st(PACKING, open = 1, rec = 6), st(PACKING, open = 1, rec = 6), st(PACKING, open = 1, rec = 6), st(DONE, open = 1, rec = 6),
            ),
        )
        val control = Control(script)
        val last = run(script, control)

        assertEquals(listOf("resume", "pause"), control.events)
        // Questions 1..4 saw queued/recording; the fifth was the first "packing" and the game was
        // paused right after it, before the three more it took for the zip to finish.
        assertEquals(5, control.pausedAtAsk)
        assertEquals(DONE, last?.phase)
        assertEquals(8, script.asked)
        assertEquals(1, last?.vsyncsToOpen)
        assertEquals(6, last?.vsyncsToRecorded)
    }

    @Test
    fun theGameIsResumedBeforeTheCoreIsAskedAgain() {
        val script = Script(listOf(st(QUEUED), st(PACKING, 1, 5), st(DONE, 1, 5)))
        val control = Control(script)
        run(script, control)
        assertEquals(1, control.resumedAtAsk)
    }

    @Test
    fun aGameThatWasRunningIsNeitherResumedNorPausedByTheCapture() {
        val script = Script(listOf(st(QUEUED), st(RECORDING, 1), st(PACKING, 1, 5), st(DONE, 1, 5)))
        val control = Control(script).apply { paused = false }
        val last = run(script, control, resume = false)
        assertTrue(control.events.isEmpty())
        assertEquals(DONE, last?.phase)
    }

    @Test
    fun theZipAppearingEndsTheWaitWhenTheCoreCannotBeAsked() {
        // An old core: no answer at all. The zip is the only signal, as it was before the core could say.
        var polls = 0
        val script = Script(listOf(null))
        val control = Control(script)
        val last = run(script, control, zipReady = { polls++ >= 4 })
        assertNull(last)
        assertEquals(listOf("resume", "pause"), control.events)
        assertTrue(clock in 1L..100L)
    }

    @Test
    fun aDumpThatNeverOpensGivesUpAtTheBudgetAndStillPausesTheGame() {
        val script = Script(listOf(st(QUEUED)))
        val control = Control(script)
        val last = run(script, control, budgetMs = 1_000)
        assertEquals(QUEUED, last?.phase)
        assertEquals(listOf("resume", "pause"), control.events)
        assertTrue("waited about the budget, not much more", clock in 1_000L..1_100L)
    }

    @Test
    fun aGivenUpCaptureStopsWaitingAndPausesTheGame() {
        val script = Script(listOf(st(QUEUED), st(RECORDING, 1)))
        val control = Control(script)
        var polls = 0
        run(script, control, abandoned = { polls++ >= 3 })
        assertEquals(listOf("resume", "pause"), control.events)
        assertTrue(control.paused)
    }

    @Test
    fun aRendererThatClosedMeansNoDumpAndNoFurtherWaiting() {
        val script = Script(listOf(st(QUEUED), st(IDLE)))
        val control = Control(script)
        val last = run(script, control)
        assertEquals(IDLE, last?.phase)
        assertEquals(listOf("resume", "pause"), control.events)
        assertTrue(clock < 100)
    }

    @Test
    fun theNativeArrayIsReadAsPhaseAndCounts() {
        val status = GsDumpStatus.fromNative(longArrayOf(3, 2, 7))
        assertNotNull(status)
        assertEquals(PACKING, status?.phase)
        assertEquals(2, status?.vsyncsToOpen)
        assertEquals(7, status?.vsyncsToRecorded)
        assertTrue(status!!.framesRecorded)
        assertFalse(status.closed)
        assertTrue(GsDumpStatus.fromNative(longArrayOf(4, 0, 0))!!.closed)
        assertFalse(GsDumpStatus.fromNative(longArrayOf(2, 1, 0))!!.framesRecorded)
    }

    @Test
    fun anAnswerThatMakesNoSenseIsNoAnswer() {
        assertNull(GsDumpStatus.fromNative(null))
        assertNull(GsDumpStatus.fromNative(longArrayOf(3, 2)))
        assertNull(GsDumpStatus.fromNative(longArrayOf(9, 0, 0)))
        assertNull(GsDumpStatus.fromNative(longArrayOf(-1, 0, 0)))
    }
}
