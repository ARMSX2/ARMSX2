// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File
import java.io.IOException
import java.util.zip.GZIPOutputStream

/**
 * One backup of the emulator log: the log of the most recent earlier run that started a game.
 *
 * The core truncates `logs/emulog.txt` whenever it opens it. It opens it when the app starts, and
 * again at the boot of every game after the first in the same process (it closes the file when a
 * game ends). A game that hangs and is force-closed leaves its log in the file the next launch
 * truncates, so the report filed after that launch carried only app start-up. Just before a
 * truncation the log is renamed to `logs/emulog.previous.txt`, replacing the last backup; every
 * report sends both.
 *
 * Two rules keep the backup useful:
 *
 *  - Only a log in which a game was started may become the backup. Otherwise a relaunch, or a
 *    second relaunch, would replace the hung run's log with app start-up. "A game was started" is
 *    a flag [CrashMarker] commits together with the crash marker, so it survives the process dying.
 *  - The file is never moved while the core has it open. A file renamed under an open handle keeps
 *    receiving the log, and `emulog.txt` is not created again. The core holds it open from start-up
 *    through the first game, so the moves happen only at app launch (before the core starts) and at
 *    the start of a later game (after the previous one closed it); [rotate] is told which it is.
 *
 * A rename, not a copy, so it costs nothing on the startup path. All of this is best effort.
 */
object LogBackup {
    const val CURRENT = "emulog.txt"
    const val PREVIOUS = "emulog.previous.txt"

    /** What [rotate] did, and whether the caller should now clear the "this log holds a game" flag. */
    enum class Outcome(val clearsFlag: Boolean) {
        /** The core has the log open. Nothing was touched, and the flag still describes the file. */
        HELD_OPEN(false),

        /** The log holds only app start-up. The backup was left as it was; the core truncates the log next. */
        NO_GAME(true),

        /** There is no log to move (the crash evidence took it, or none has been written). */
        NO_LOG(true),

        /** The log is now the backup. */
        ROTATED(true),

        /** The rename failed. The core truncates the log next, so the flag is cleared all the same. */
        FAILED(true),
    }

    fun current(logsDir: File) = File(logsDir, CURRENT)

    fun previous(logsDir: File) = File(logsDir, PREVIOUS)

    /**
     * Makes the log in [logsDir] the backup if [logHadGame] and the core does not have it open
     * ([coreHoldsLog]). The facts come from the caller: the flag from [CrashMarker], the handle
     * from the core.
     */
    fun rotate(logsDir: File, logHadGame: Boolean, coreHoldsLog: Boolean): Outcome {
        if (coreHoldsLog) return Outcome.HELD_OPEN
        if (!logHadGame) return Outcome.NO_GAME
        val log = current(logsDir)
        if (!log.isFile) return Outcome.NO_LOG
        val backup = previous(logsDir)
        if (log.renameTo(backup)) return Outcome.ROTATED
        // Some filesystems will not rename over an existing file. The log is still in place at this
        // point, so a failure between these two lines loses only the older backup.
        backup.delete()
        return if (log.renameTo(backup)) Outcome.ROTATED else Outcome.FAILED
    }

    /**
     * Writes [src] to [dest] as gzip text, scrubbed. [dest] only appears once it is whole: a kill
     * halfway never leaves a short file that looks finished. Throws [IOException] if [src] cannot
     * be read or [dest] cannot be written.
     */
    fun writeScrubbedGz(scrubber: Scrubber, src: File, dest: File) {
        val partial = File(dest.path + ".partial")
        try {
            src.inputStream().use { input ->
                scrubber.copyText(input, src.length(), GZIPOutputStream(partial.outputStream().buffered()))
            }
            if (!partial.renameTo(dest)) throw IOException("could not finish ${dest.name}")
        } finally {
            partial.delete()
        }
    }
}
