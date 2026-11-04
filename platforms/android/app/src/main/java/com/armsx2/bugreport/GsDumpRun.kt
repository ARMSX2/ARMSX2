// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

/** Where a requested graphics dump is in the core, in the order the core numbers them (`GSDumpPhase`). */
enum class GsDumpPhase {
    /** No request, or the renderer closed before it was served. */
    IDLE,

    /** Accepted; the next vertical sync opens the dump. */
    QUEUED,

    /** Open and taking frames. */
    RECORDING,

    /** Every frame is in the dump; it is being closed (screenshot written, zip built). */
    PACKING,

    /** Closed. The zip is on disk unless packing failed. */
    DONE,
}

/**
 * The core's answer to "where is the dump" (`NativeApp.getGsDumpStatus`). The counts are GS
 * vertical syncs since the request, which is made while the game is paused; 0 means not known yet.
 */
data class GsDumpStatus(val phase: GsDumpPhase, val vsyncsToOpen: Int, val vsyncsToRecorded: Int) {
    /** The game has nothing more to give the dump: its frames are in, or it will not be made. */
    val framesRecorded: Boolean get() = phase == GsDumpPhase.PACKING || phase == GsDumpPhase.DONE || phase == GsDumpPhase.IDLE

    /** Nothing more will happen to the zip: it is on disk, or it never will be. */
    val closed: Boolean get() = phase == GsDumpPhase.DONE || phase == GsDumpPhase.IDLE

    companion object {
        /** Reads the native array `{phase, vsyncsToOpen, vsyncsToRecorded}`; null if it is missing or makes no sense. */
        fun fromNative(raw: LongArray?): GsDumpStatus? {
            if (raw == null || raw.size < 3) return null
            val phase = GsDumpPhase.entries.getOrNull(raw[0].toInt()) ?: return null
            return GsDumpStatus(phase, raw[1].coerceIn(0, Int.MAX_VALUE.toLong()).toInt(), raw[2].coerceIn(0, Int.MAX_VALUE.toLong()).toInt())
        }
    }
}

/**
 * Runs the game for exactly as long as the graphics dump needs it. The dump opens at a vertical
 * sync and records the next few; once its last frame is in, closing it (the screenshot, the
 * compressed stream, the zip) takes seconds on a handheld and needs no running game. So the game is
 * resumed, stopped again the moment the core says the frames are recorded, and the zip is waited for
 * while it is stopped.
 *
 * If the core cannot be asked (a null status), the zip appearing is the only signal and the game
 * runs until it does, which is how this worked before the core could say.
 */
object GsDumpRun {
    /** How often the core is asked. Each answer is a few atomic loads, and a frame is 16 ms. */
    const val POLL_MS = 5L

    /**
     * Resumes the game if [resume] (it was paused), waits until the frames are recorded, pauses it
     * again, then waits until the dump is closed. All of it within [budgetMs] of the call. Returns the
     * last status read, or null if the core never answered. The caller looks at the zip itself: this
     * only decides when to stop waiting for it.
     */
    fun run(
        resume: Boolean,
        control: GameControl,
        status: () -> GsDumpStatus?,
        zipReady: () -> Boolean,
        abandoned: () -> Boolean,
        budgetMs: Long,
        now: () -> Long = { System.currentTimeMillis() },
        sleep: (Long) -> Unit = { Thread.sleep(it) },
    ): GsDumpStatus? {
        val deadline = now() + budgetMs
        var last = status()
        if (resume) control.resume()
        try {
            while (last?.framesRecorded != true && !zipReady() && now() < deadline && !abandoned()) {
                sleep(POLL_MS)
                last = status() ?: last
            }
        } finally {
            if (resume) control.pause()
        }
        while (last?.closed != true && !zipReady() && now() < deadline && !abandoned()) {
            sleep(POLL_MS)
            last = status() ?: last
        }
        return last
    }
}
