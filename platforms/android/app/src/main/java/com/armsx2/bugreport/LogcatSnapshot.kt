// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File
import java.util.zip.GZIPOutputStream
import kotlin.concurrent.thread

/**
 * `logcat -d -v threadtime`, run as the app. Android only returns a process the entries written by
 * its own user id, which here means this app's processes, including the one that has just died,
 * for as long as the system's log buffer has not rolled over them. That is why a crash report's
 * snapshot is taken at the first moment of the next launch.
 */
object LogcatSnapshot {
    /** The buffer is a few megabytes at most on current devices; this only stops a runaway. */
    private const val MAX_RAW_BYTES = 96L * 1024 * 1024

    data class Result(val ok: Boolean, val lines: Long, val rawBytes: Long, val error: String?)

    /**
     * Writes the log through [scrubber] into [dest] as gzip. Returns a failed [Result] (and leaves no
     * file) if logcat cannot be run or says nothing. [timeoutMs] bounds the whole call.
     */
    fun write(scrubber: Scrubber, dest: File, timeoutMs: Long = 20_000): Result {
        val process = try {
            ProcessBuilder("logcat", "-d", "-v", "threadtime").redirectErrorStream(true).start()
        } catch (e: Exception) {
            return Result(false, 0, 0, "logcat could not be started: ${e.message}")
        }
        val watchdog = thread(isDaemon = true, name = "bugreport-logcat-watchdog") {
            try {
                Thread.sleep(timeoutMs)
                process.destroy()
            } catch (_: InterruptedException) {
            }
        }
        var lines = 0L
        var raw = 0L
        var error: String? = null
        try {
            GZIPOutputStream(dest.outputStream().buffered()).bufferedWriter(Charsets.UTF_8).use { out ->
                process.inputStream.bufferedReader(Charsets.UTF_8).use { reader ->
                    while (true) {
                        val line = reader.readLine() ?: break
                        raw += line.length + 1
                        if (raw > MAX_RAW_BYTES) {
                            out.write("[report: logcat output stopped here to keep the report small]\n")
                            break
                        }
                        out.write(scrubber.text(line))
                        out.write("\n")
                        lines++
                    }
                }
            }
        } catch (e: Exception) {
            error = "logcat could not be read: ${e.message}"
        } finally {
            watchdog.interrupt()
            process.destroy()
        }
        if (error == null && lines == 0L) error = "logcat returned nothing"
        if (error != null) {
            dest.delete()
            return Result(false, lines, raw, error)
        }
        return Result(true, lines, raw, null)
    }
}
