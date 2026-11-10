// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.Context
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.zip.GZIPOutputStream

/**
 * Keeps what a crash left behind, before the next game can overwrite it.
 *
 * `logs/emulog.txt` is truncated when a game boots, and the system's log buffer rolls over, so the
 * evidence has to be taken at the first moment of the next launch. That happens in two parts:
 *
 *  - [preserve] is quick and runs on the startup path: for a crash [CrashDecision] said to ask
 *    about, it moves the emulog aside (a rename, not a copy, so it costs nothing).
 *  - [complete] runs on a background thread: the logcat snapshot, Android's exit records, the
 *    tombstone or ANR trace, and the emulog backup (see [LogBackup]), which is the log of the run
 *    before the crashed one.
 *
 * The folder is then a report waiting for the user's say-so. If they send it, [ReportCollector]
 * adds the rest and the same folder is uploaded; if not, it is deleted after a week.
 */
object CrashEvidence {
    const val NOTES_FILE = "evidence.json"
    const val MARKER_FILE = "marker.json"

    /** Written last. Anything that needs the evidence waits for it. */
    const val DONE_FILE = "evidence.done"
    const val FOLDER_PREFIX = "crash-"

    private const val RAW_EMULOG = "emulog.raw"
    private const val CLOSED_BY_SYSTEM = "closedBySystem"
    private const val MAX_TRACE_BYTES = 16L * 1024 * 1024

    /** How long unsent evidence is kept. */
    const val KEEP_MS = 7L * 24 * 3600 * 1000

    /** At most this many unsent crashes are kept; older ones are dropped first. */
    const val KEEP_COUNT = 3

    class Preserved(val dir: File, val marker: CrashMarker.Marker, val record: ExitRecord?, val closedBySystem: Boolean)

    /**
     * Makes the folder for a crash that [CrashDecision] said to ask about and moves the emulog into
     * it (a rename, so it costs nothing). The folder's `marker.json` also says whether Android
     * ended the process, so the prompt can word itself that way after a restart.
     */
    fun preserve(context: Context, dataRoot: File, marker: CrashMarker.Marker, record: ExitRecord?, closedBySystem: Boolean): Preserved? {
        val dir = File(ReportCollector.bundleRoot(context), "$FOLDER_PREFIX${marker.armedAt}")
        if (!dir.mkdirs() && !dir.isDirectory) return null
        File(dir, MARKER_FILE).writeText(marker.toJson().put(CLOSED_BY_SYSTEM, closedBySystem).toString())
        val emulog = File(dataRoot, "logs/emulog.txt")
        if (emulog.isFile) {
            val raw = File(dir, RAW_EMULOG)
            if (!emulog.renameTo(raw)) {
                runCatching { emulog.copyTo(raw, overwrite = true) }
            }
        }
        return Preserved(dir, marker, record, closedBySystem)
    }

    /** Reads back what [preserve] wrote to a crash folder: the marker, and whether Android ended the process. */
    fun readMarker(dir: File): Pair<CrashMarker.Marker, Boolean>? = runCatching {
        val json = JSONObject(File(dir, MARKER_FILE).readText())
        CrashMarker.Marker.fromJson(json) to json.optBoolean(CLOSED_BY_SYSTEM, false)
    }.getOrNull()

    /** The slow half of the evidence. Every piece is best effort; what could not be had is noted. */
    fun complete(context: Context, dataRoot: File, preserved: Preserved) {
        val dir = preserved.dir
        val marker = preserved.marker
        // Already done by an earlier launch that was cut short before it cleared the marker.
        if (File(dir, DONE_FILE).isFile) return
        val scrubber = Scrubber(listOf(dataRoot.path), AccountNames.read(dataRoot))
        val collected = JSONObject()
        val failed = JSONObject()

        fun piece(name: String, block: () -> Unit) {
            try {
                block()
                val f = File(dir, name)
                if (f.isFile && f.length() > 0) collected.put(name, f.length())
                else failed.put(name, "nothing to collect")
            } catch (t: Throwable) {
                File(dir, name).delete()
                failed.put(name, t.message ?: t.javaClass.simpleName)
            }
        }

        piece(ReportFiles.EMULOG) {
            val raw = File(dir, RAW_EMULOG)
            val done = File(dir, ReportFiles.EMULOG)
            // A launch cut short after the raw log was turned into the .gz leaves only the .gz.
            if (!raw.isFile && done.length() > 0) return@piece
            if (!raw.isFile) throw IOException("the previous run left no emulog.txt")
            // Written under another name and renamed, so a kill halfway never leaves a short .gz that looks whole.
            val partial = File(dir, ReportFiles.EMULOG + ".partial")
            raw.inputStream().use { input ->
                scrubber.copyText(input, raw.length(), GZIPOutputStream(partial.outputStream().buffered()))
            }
            if (!partial.renameTo(done)) throw IOException("could not finish the emulator log")
            raw.delete()
        }
        // The log of the run before the crashed one, if there is one. It is taken into the folder now
        // so that a report sent days later does not pick up a newer run's backup. No backup is not a
        // failure, so nothing is noted for it.
        val backup = LogBackup.previous(File(dataRoot, "logs"))
        if (backup.isFile && backup.length() > 0) {
            piece(ReportFiles.EMULOG_PREVIOUS) {
                // Already there from an earlier launch that was cut short.
                if (File(dir, ReportFiles.EMULOG_PREVIOUS).length() > 0) return@piece
                LogBackup.writeScrubbedGz(scrubber, backup, File(dir, ReportFiles.EMULOG_PREVIOUS))
            }
        }
        piece(ReportFiles.LOGCAT) {
            val result = LogcatSnapshot.write(scrubber, File(dir, ReportFiles.LOGCAT))
            if (!result.ok) throw IOException(result.error ?: "logcat failed")
        }
        piece(ReportFiles.EXIT_INFO) {
            val text = ExitInfo.format(ExitInfo.read(context), marker.pid)
            GZIPOutputStream(File(dir, ReportFiles.EXIT_INFO).outputStream().buffered()).use {
                it.write(scrubber.text(text).toByteArray(Charsets.UTF_8))
            }
        }
        // Not every crash has a trace: a Java exception does not, and below Android 12 there is no
        // tombstone. Only a failure to read one that exists is worth noting.
        runCatching {
            val trace = ExitInfo.openTrace(context, marker.pid, marker.armedAt)
            if (trace != null) {
                trace.stream.use { input ->
                    if (trace.reason == ExitInfo.REASON_CRASH_NATIVE) {
                        piece(ReportFiles.TOMBSTONE) {
                            File(dir, ReportFiles.TOMBSTONE).outputStream().use { out ->
                                input.copyTo(out, bufferSize = 64 * 1024)
                                if (File(dir, ReportFiles.TOMBSTONE).length() > MAX_TRACE_BYTES) throw IOException("tombstone too large")
                            }
                        }
                    } else {
                        piece(ReportFiles.ANR) {
                            scrubber.copyText(input, -1, GZIPOutputStream(File(dir, ReportFiles.ANR).outputStream().buffered()))
                        }
                    }
                }
            }
        }
        File(dir, NOTES_FILE).writeText(JSONObject().put("collected", collected).put("failed", failed).toString())
        File(dir, DONE_FILE).writeText("1")
    }

    /** Blocks up to [timeoutMs] until [complete] has finished for [dir]. Returns false if it has not. */
    fun awaitComplete(dir: File, timeoutMs: Long): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (!File(dir, DONE_FILE).isFile) {
            if (System.currentTimeMillis() > deadline) return false
            Thread.sleep(100)
        }
        return true
    }

    /**
     * The folders under [root] that are evidence nobody sent, newest first. A folder with a
     * `state.json` has been turned into a report and is not listed.
     */
    fun unsent(root: File): List<File> =
        root.listFiles { f -> f.isDirectory && f.name.startsWith(FOLDER_PREFIX) && !File(f, ReportUploader.STATE_FILE).isFile }
            ?.sortedByDescending { it.name.removePrefix(FOLDER_PREFIX).toLongOrNull() ?: 0L }
            .orEmpty()
}
