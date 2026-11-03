// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

/** When a report's save state was taken, relative to the moment the user paused the game. Goes in `report.json`. */
enum class SavestateTiming(val wire: String) {
    /** Before the game was resumed: the scene the user was looking at. */
    PAUSED_MOMENT("paused_moment"),

    /** After the game had run again. A busy memory card refused the first try and the game had to run to clear it. */
    AFTER_RESUME("after_resume"),

    /** The game was not paused when the capture began, so the state is from whenever the capture got to it. */
    RUNNING("running"),
}

/**
 * The order the captures run in. The report is evidence of what the user saw when they paused, and
 * every moment the game runs moves it further from that, so:
 *
 *  1. The save state is taken first, while the game is still paused where the user left it.
 *  2. The graphics dump runs the game just long enough to record a frame (see [GsDumpRun]).
 *  3. Only if the save state was refused for a busy memory card, it is tried again now. The card's
 *     write counter only goes down while the game runs, so the dump's run may already have cleared
 *     it; if not, [SavestateAttempt] lets the game run a little longer. Either way the state is no
 *     longer from the paused moment, and [Outcome.timing] says so.
 *
 * A refusal for any other reason is final: running the game would not help.
 */
object CaptureSequence {
    interface Steps {
        /** One try at the save state with the game as it is. A busy card is not an answer yet and is not recorded. */
        fun saveNow(): SavestateAttempt.Result

        /** The retry that may run the game to clear a busy card. Its answer is final. */
        fun saveAfterResume(): SavestateAttempt.Result

        /** Takes the graphics dump. */
        fun dump()
    }

    /** What happened to the save state: null if none was asked for. [timing] is null unless it was saved. */
    class Outcome(val savestate: SavestateAttempt.Result?, val timing: SavestateTiming?)

    fun run(savestate: Boolean, dump: Boolean, gameWasPaused: Boolean, steps: Steps): Outcome {
        var result: SavestateAttempt.Result? = null
        var timing: SavestateTiming? = null
        if (savestate) {
            result = steps.saveNow()
            if (result == SavestateAttempt.Result.SAVED) {
                timing = if (gameWasPaused) SavestateTiming.PAUSED_MOMENT else SavestateTiming.RUNNING
            }
        }
        if (dump) steps.dump()
        if (savestate && result == SavestateAttempt.Result.MEMCARD_BUSY) {
            result = steps.saveAfterResume()
            if (result == SavestateAttempt.Result.SAVED) timing = SavestateTiming.AFTER_RESUME
        }
        return Outcome(result, timing)
    }
}

/**
 * Adds up the wall-clock time the game was running because the capture asked it to: from each
 * resume request to the next pause request. The game stops a few milliseconds after a pause request,
 * so this is a little under the truth, never over.
 */
class RunClock(private val now: () -> Long = { System.currentTimeMillis() }) {
    private var since = -1L
    private var total = 0L

    @Synchronized
    fun resumed() {
        if (since < 0) since = now()
    }

    @Synchronized
    fun paused() {
        if (since >= 0) {
            total += now() - since
            since = -1
        }
    }

    /** The total so far, counting a stretch that is still going. */
    @Synchronized
    fun totalMs(): Long = total + if (since >= 0) now() - since else 0
}

/** A [GameControl] that tells [clock] when the game was asked to run and to stop. */
class TimedControl(private val inner: GameControl, private val clock: RunClock) : GameControl {
    override fun isPaused() = inner.isPaused()

    override fun resume() {
        clock.resumed()
        inner.resume()
    }

    override fun pause() {
        inner.pause()
        clock.paused()
    }

    override fun awaitRunning(timeoutMs: Long) = inner.awaitRunning(timeoutMs)

    override fun awaitPaused(timeoutMs: Long) = inner.awaitPaused(timeoutMs)
}
