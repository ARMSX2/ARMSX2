package com.armsx2.bugreport

import com.armsx2.bugreport.LogBackup.Outcome
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.FileOutputStream
import java.util.zip.GZIPInputStream

/**
 * The one backup of the emulator log: which log may become it, and when the log can be moved.
 *
 * The scenarios drive [Device], which plays the core (it truncates the log when it opens it, closes
 * it when a game ends, and keeps writing to whatever file its handle points at) and the frontend's
 * two rotation points (app launch, and the start of a game). The rotation decision under test is
 * [LogBackup.rotate]; the durable "this log holds a game" fact is a plain boolean here.
 */
class LogBackupTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private lateinit var logs: File

    @Before
    fun makeLogsDir() {
        logs = tmp.newFolder("logs")
    }

    private val log get() = File(logs, "emulog.txt")
    private val backup get() = File(logs, "emulog.previous.txt")

    private inner class Device {
        /** What the frontend commits with the crash marker: the log now holds a game. */
        var logHasGame = false

        /** The core's open handle on the log. A write goes to the file the handle was opened on, whatever it is called now. */
        private var handle: FileOutputStream? = null
        val coreHoldsLog get() = handle != null

        /** A new process. The frontend rotates first, then the core opens the log for writing, which truncates it. */
        fun launch() {
            rotate()
            open()
            write("start-up\n")
        }

        /** A game starts: the frontend rotates, then the core boots; it reopens the log if it had closed it. */
        fun startGame(name: String) {
            rotate()
            if (!coreHoldsLog) open()
            logHasGame = true
            write("game $name\n")
        }

        /** The game stopped in an orderly way; the core closes the log (the file stays). */
        fun stopGame() = close()

        /** The game hung and the user closed the app. The handle goes with the process; the file and the flag stay. */
        fun forceClose() = close()

        private fun rotate() {
            val outcome = LogBackup.rotate(logs, logHasGame, coreHoldsLog)
            if (outcome.clearsFlag) logHasGame = false
        }

        private fun open() {
            handle = FileOutputStream(log)
        }

        private fun write(text: String) {
            handle!!.write(text.toByteArray())
            handle!!.flush()
        }

        private fun close() {
            handle?.close()
            handle = null
        }
    }

    @Test
    fun aHungGamesLogSurvivesForceCloseAndRelaunch() {
        val d = Device()
        d.launch()
        d.startGame("A")
        d.forceClose()
        d.launch()
        assertEquals("start-up\ngame A\n", backup.readText())
        assertEquals("start-up\n", log.readText())
    }

    @Test
    fun relaunchingWithNoGameStartedLeavesTheBackupAlone() {
        val d = Device()
        d.launch()
        d.startGame("A")
        d.forceClose()
        repeat(5) {
            d.launch()
            d.forceClose()
            assertEquals("start-up\ngame A\n", backup.readText())
            assertEquals("start-up\n", log.readText())
        }
    }

    @Test
    fun theNextGameStartedAfterThoseRelaunchesDoesNotCostTheBackup() {
        // The case a plain "copy aside before every truncation" gets wrong: the backup would end up
        // holding app start-up and the hung run's log would be gone.
        val d = Device()
        d.launch()
        d.startGame("A")
        d.forceClose()
        d.launch()
        d.forceClose()
        d.launch()
        d.startGame("B")
        assertEquals("start-up\ngame A\n", backup.readText())
        assertEquals("start-up\ngame B\n", log.readText())
    }

    @Test
    fun theBackupMovesOnWhenALaterRunThatStartedAGameEnds() {
        val d = Device()
        d.launch()
        d.startGame("A")
        d.forceClose()
        d.launch()
        d.startGame("B")
        d.forceClose()
        d.launch()
        assertEquals("start-up\ngame B\n", backup.readText())
        assertEquals("start-up\n", log.readText())
    }

    @Test
    fun aSecondGameInTheSameProcessBacksUpTheFirst() {
        val d = Device()
        d.launch()
        d.startGame("A")
        d.stopGame()
        d.startGame("B")
        assertEquals("start-up\ngame A\n", backup.readText())
        // The core reopened emulog.txt, so the file exists again and holds only the new game.
        assertEquals("game B\n", log.readText())
    }

    @Test
    fun aThirdGameReplacesTheBackupInsteadOfAddingToIt() {
        val d = Device()
        d.launch()
        d.startGame("A")
        d.stopGame()
        d.startGame("B")
        d.stopGame()
        d.startGame("C")
        assertEquals("game B\n", backup.readText())
        assertEquals("game C\n", log.readText())
        assertEquals(setOf("emulog.txt", "emulog.previous.txt"), logs.list()!!.toSet())
    }

    @Test
    fun theFirstGameOfAProcessLeavesTheLogWhereTheCoreIsWritingIt() {
        // The core has the log open from start-up through the first game. Moving it then would send
        // every later line to the backup and leave no emulog.txt.
        val d = Device()
        d.launch()
        d.startGame("A")
        assertFalse(backup.exists())
        assertEquals("start-up\ngame A\n", log.readText())
    }

    @Test
    fun aLogTheCoreHasOpenIsNotMovedAndKeepsItsFlag() {
        log.writeText("a run\n")
        val outcome = LogBackup.rotate(logs, logHadGame = true, coreHoldsLog = true)
        assertEquals(Outcome.HELD_OPEN, outcome)
        assertFalse(outcome.clearsFlag)
        assertEquals("a run\n", log.readText())
        assertFalse(backup.exists())
    }

    @Test
    fun aLogWithNoGameInItNeverReplacesTheBackup() {
        backup.writeText("the hung run\n")
        log.writeText("start-up\n")
        val outcome = LogBackup.rotate(logs, logHadGame = false, coreHoldsLog = false)
        assertEquals(Outcome.NO_GAME, outcome)
        assertEquals("the hung run\n", backup.readText())
        assertEquals("start-up\n", log.readText())
    }

    @Test
    fun aMissingLogLeavesTheBackupAloneAndClearsTheFlag() {
        // The crash evidence took emulog.txt into its own folder before this ran.
        backup.writeText("the run before\n")
        val outcome = LogBackup.rotate(logs, logHadGame = true, coreHoldsLog = false)
        assertEquals(Outcome.NO_LOG, outcome)
        assertTrue(outcome.clearsFlag)
        assertEquals("the run before\n", backup.readText())
        assertFalse(log.exists())
    }

    @Test
    fun aLogWithAGameReplacesTheOlderBackup() {
        backup.writeText("older\n")
        log.writeText("newer\n")
        assertEquals(Outcome.ROTATED, LogBackup.rotate(logs, logHadGame = true, coreHoldsLog = false))
        assertEquals("newer\n", backup.readText())
        assertFalse(log.exists())
    }

    @Test
    fun aMissingLogsFolderIsNotAnError() {
        val nowhere = File(tmp.root, "no-such-folder")
        assertEquals(Outcome.NO_LOG, LogBackup.rotate(nowhere, logHadGame = true, coreHoldsLog = false))
    }

    @Test
    fun theBackupGoesOutAsScrubbedGzipText() {
        val root = "/storage/emulated/0/Android/data/com.armsx2/files"
        backup.writeText("opened $root/bios/scph.bin\nsecond line\n")
        val dest = File(tmp.root, "emulog-previous.txt.gz")
        LogBackup.writeScrubbedGz(Scrubber(listOf(root)), backup, dest)
        val text = GZIPInputStream(dest.inputStream()).bufferedReader().readText()
        assertEquals("opened <data>/bios/scph.bin\nsecond line\n", text)
        assertFalse(File(tmp.root, "emulog-previous.txt.gz.partial").exists())
    }

    @Test
    fun aBackupThatCannotBeReadLeavesNoFileBehind() {
        val dest = File(tmp.root, "emulog-previous.txt.gz")
        try {
            LogBackup.writeScrubbedGz(Scrubber(emptyList()), File(logs, "absent.txt"), dest)
        } catch (_: java.io.IOException) {
        }
        assertFalse(dest.exists())
        assertFalse(File(tmp.root, "emulog-previous.txt.gz.partial").exists())
    }
}
