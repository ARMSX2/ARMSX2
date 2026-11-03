// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.URLDecoder
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/**
 * What a problem report is not allowed to carry, applied to everything that leaves the device as
 * text.
 *
 *  - The data root, and any other `/storage/...` or `/sdcard/...` prefix, becomes `<data>`.
 *  - A game's path becomes its file name (a `content://` URI becomes `content://<redacted>/name`).
 *    The folders someone keeps their games in say nothing a developer needs.
 *  - `uname.nodename` in the driver report is dropped. It is the device's host name.
 *  - Settings keys that hold a token, password or login are dropped.
 *
 * Never sent at all, by the collector rather than here: `achievements.ini`, memory cards, BIOS
 * images and game images.
 *
 * [dataRoots] are the folders to name `<data>` first: the root the core was started with, and any
 * other spelling of it the caller knows. The generic volume-prefix rule catches the rest, so a log
 * line that spells the same folder as `/sdcard/Android/data/...` still loses its volume.
 */
class Scrubber(dataRoots: Collection<String>) {
    private val roots: List<String> = dataRoots
        .map { it.trimEnd('/') }
        .filter { it.length > 1 }
        .distinct()
        .sortedByDescending { it.length }

    /** [input] with paths and URIs reduced as described above. */
    fun text(input: String): String {
        if (input.isEmpty()) return input
        var s = input
        for (root in roots) {
            if (s.contains(root)) s = s.replace(root, DATA)
        }
        if (s.contains("content://")) s = CONTENT_URI.replace(s) { contentUri(it.value) }
        if (s.indexOf('/') >= 0) {
            s = GAME_PATH.replace(s) { it.groupValues[2] }
            s = VOLUME_PREFIX.replace(s, DATA)
        }
        return s
    }

    /** A copy of [value] (a JSON object, array or scalar) with every string passed through [text] and every key [dropKey] names removed. */
    fun json(value: Any?, dropKey: (String) -> Boolean = { false }): Any? = when (value) {
        is JSONObject -> JSONObject().also { out ->
            for (key in value.keys()) {
                if (!dropKey(key)) out.put(key, json(value.get(key), dropKey))
            }
        }
        is JSONArray -> JSONArray().also { out ->
            for (i in 0 until value.length()) out.put(json(value.get(i), dropKey))
        }
        is String -> text(value)
        else -> value
    }

    /**
     * The settings document (global settings, per-game overrides) with credentials removed and
     * paths reduced. Nothing in the settings the app keeps today is a credential; the key filter is
     * there so one added later does not leave the device by accident.
     */
    fun settings(settings: JSONObject): JSONObject =
        json(settings, ::isSensitiveKey) as JSONObject

    /**
     * The GS driver report with the host name removed and paths reduced. If [reportJson] is not
     * valid JSON the host name is cut out of the text and the text is reduced the same way, so a
     * truncated report still leaves without it.
     */
    fun driverReport(reportJson: String): String {
        val parsed = try {
            JSONObject(reportJson)
        } catch (_: Exception) {
            null
        }
        if (parsed == null) return text(NODENAME_FIELD.replace(reportJson, ""))
        return (json(parsed, ::isDeviceIdentifierKey) as JSONObject).toString(2)
    }

    /**
     * Copies [source] to [out] line by line through [text], so a multi-megabyte log is never held
     * in memory. [out] is closed. A source longer than [maxBytes] keeps its end, which is where a
     * crash is; a one-line note says how much was dropped. Returns the number of bytes written.
     */
    fun copyText(source: InputStream, sourceLength: Long, out: OutputStream, maxBytes: Long = DEFAULT_MAX_TEXT_BYTES): Long {
        var written = 0L
        out.use { sink ->
            val reader = source.bufferedReader(Charsets.UTF_8)
            fun write(line: String) {
                val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
                sink.write(bytes)
                written += bytes.size
            }
            if (sourceLength > maxBytes) {
                var toSkip = sourceLength - maxBytes
                // Skip whole lines, so a line is never cut in the middle.
                while (toSkip > 0) {
                    val line = reader.readLine() ?: break
                    toSkip -= line.toByteArray(Charsets.UTF_8).size + 1
                }
                write("[report: the first ${sourceLength - maxBytes} bytes of this log were left out to keep the report small]")
            }
            while (true) {
                val line = reader.readLine() ?: break
                write(text(line))
            }
        }
        return written
    }

    /**
     * Rewrites the GS dump bundle [source] into [dest] with its driver report scrubbed. The dump and
     * the screenshot are copied entry for entry, byte for byte, with the method they were stored
     * with (the dump and the PNG are already compressed and are stored, not deflated again); only
     * `*.driver.json` is read, scrubbed and written back. Returns the scrubbed report, or null if the
     * bundle held none.
     */
    fun rewriteGsBundle(source: File, dest: File): String? {
        var report: String? = null
        ZipFile(source).use { zip ->
            ZipOutputStream(dest.outputStream().buffered()).use { out ->
                val entries = zip.entries()
                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (entry.isDirectory) continue
                    if (entry.name.endsWith(".driver.json")) {
                        val scrubbed = driverReport(zip.getInputStream(entry).readBytes().toString(Charsets.UTF_8))
                        report = scrubbed
                        out.putNextEntry(ZipEntry(entry.name).apply { method = ZipEntry.DEFLATED })
                        out.write(scrubbed.toByteArray(Charsets.UTF_8))
                        out.closeEntry()
                    } else if (entry.method == ZipEntry.STORED && entry.size >= 0 && entry.crc >= 0) {
                        out.putNextEntry(
                            ZipEntry(entry.name).apply {
                                method = ZipEntry.STORED
                                size = entry.size
                                compressedSize = entry.size
                                crc = entry.crc
                            },
                        )
                        zip.getInputStream(entry).use { it.copyTo(out) }
                        out.closeEntry()
                    } else {
                        // Deflated by whoever wrote it: recompress. Only small parts are written this way.
                        out.putNextEntry(ZipEntry(entry.name).apply { method = ZipEntry.DEFLATED })
                        zip.getInputStream(entry).use { it.copyTo(out) }
                        out.closeEntry()
                    }
                }
            }
        }
        return report
    }

    private fun contentUri(uri: String): String {
        val decoded = try {
            URLDecoder.decode(uri.replace("+", "%2B"), "UTF-8")
        } catch (_: Exception) {
            uri
        }
        // A tree or document URI ends in "<volume>:<path/to/Name.iso>"; the name is after the last
        // separator of either kind.
        val name = decoded.substringAfterLast('/').substringAfterLast(':')
        return if (name.isEmpty()) "content://<redacted>" else "content://<redacted>/$name"
    }

    companion object {
        const val DATA = "<data>"
        const val DEFAULT_MAX_TEXT_BYTES = 48L * 1024 * 1024

        private val SENSITIVE_KEY = Regex("token|passw|secret|login|username|userid|apikey|api_key|credential|cookie|bearer", RegexOption.IGNORE_CASE)

        /** A settings key that may hold a credential. */
        fun isSensitiveKey(key: String): Boolean = SENSITIVE_KEY.containsMatchIn(key)

        /** A key in the driver report that identifies the device's owner rather than its hardware. */
        fun isDeviceIdentifierKey(key: String): Boolean = key == "nodename"

        private val NODENAME_FIELD = Regex("\"nodename\"\\s*:\\s*\"(?:[^\"\\\\]|\\\\.)*\"\\s*,?")

        private val CONTENT_URI = Regex("content://[^\\s\"'<>)\\]]+")

        // The image extensions a game can have. A path ending in one is a game path, whatever
        // folder it sits in, and is reduced to its file name.
        private const val GAME_EXT = "iso|bin|chd|cso|zso|gz|elf|cue|img|mdf|nrg|isz|dax|ciso|cdi|ps2"

        // "/storage/..." and friends, up to a game image's name. The folders are matched lazily and
        // may hold spaces, the name may hold spaces; the match ends where a delimiter follows the extension.
        private val GAME_PATH = Regex(
            "(?<![A-Za-z0-9_./-])(/(?:storage|sdcard|mnt)(?:/[^\\r\\n\"'<>|]*?)?/([^/\\r\\n\"'<>|]+?\\.(?:$GAME_EXT)))" +
                "(?=$|[\\s\"'<>|,;:)\\]}])",
            RegexOption.IGNORE_CASE,
        )

        // The volume part of a shared-storage path. What follows it is kept.
        private val VOLUME_PREFIX = Regex(
            "(?<![A-Za-z0-9_./-])(?:/storage/emulated/\\d+|/storage/self/primary|/storage/[^/\\s\"'<>|]+" +
                "|/sdcard\\d?|/mnt/sdcard|/mnt/media_rw/[^/\\s\"'<>|]+|/mnt/user/\\d+/[^/\\s\"'<>|]+)" +
                "(?=/|$|[\\s\"'<>|,;:)\\]}])",
        )

        /** Lower-case hex SHA-256 of [file], read in blocks. */
        fun sha256(file: File): String {
            val digest = java.security.MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    digest.update(buffer, 0, n)
                }
            }
            return digest.digest().joinToString("") { "%02x".format(it) }
        }
    }
}
