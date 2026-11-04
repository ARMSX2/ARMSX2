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
 *  - The app's own folders under `/data` become `<app>`: its private data (`/data/user/0/<package>`)
 *    and where Android installed it (`/data/app/~~<random>==/<package>-<random>==`, whose random
 *    parts are specific to the install). What follows is kept, so a driver pack's folder still
 *    shows as `<app>/files/drivers/<pack id>/`.
 *  - A game's path becomes its file name (a `content://` URI becomes `content://<redacted>/name`).
 *    The folders someone keeps their games in say nothing a developer needs.
 *  - `uname.nodename` in the driver report is dropped. It is the device's host name.
 *  - Settings keys that hold a token, password or login are dropped, and so are the ones that name
 *    a person's network or hardware: the Local Link room code and the microphone device names go,
 *    the Local Link address and the DEV9 network addresses are blanked.
 *  - The account names in [accountNames] (the RetroAchievements user) become `<user>` wherever they
 *    appear, and the log lines that print one by shape do too, whatever the name was.
 *
 * Never sent at all, by the collector rather than here: `achievements.ini`, memory cards, BIOS
 * images and game images. The one file that is sent without passing through here is the native
 * crash report (`tombstone.pb`): it is Android's own protobuf, copied as it came, and is the
 * most useful thing a crash can give us.
 *
 * [dataRoots] are the folders to name `<data>` first: the root the core was started with, and any
 * other spelling of it the caller knows. The generic volume-prefix rule catches the rest, so a log
 * line that spells the same folder as `/sdcard/Android/data/...` still loses its volume.
 */
class Scrubber(dataRoots: Collection<String>, accountNames: Collection<String> = emptyList()) {
    private val roots: List<String> = dataRoots
        .map { it.trimEnd('/') }
        .filter { it.length > 1 }
        .distinct()
        .sortedByDescending { it.length }

    // One alternation of the names, longest first so a name that contains another is taken whole.
    // A name is only matched as a whole word, and names shorter than MIN_NAME_LENGTH are left to
    // the log-shape rules: a two-letter user name would otherwise eat every "GS" in the log.
    private val accountPattern: Regex? = accountNames
        .map { it.trim() }
        .filter { it.length >= MIN_NAME_LENGTH }
        .distinct()
        .sortedByDescending { it.length }
        .takeIf { it.isNotEmpty() }
        ?.let { names ->
            Regex("(?<![A-Za-z0-9])(?:" + names.joinToString("|") { Regex.escape(it) } + ")(?![A-Za-z0-9])", RegexOption.IGNORE_CASE)
        }

    /** [input] with paths and URIs reduced as described above. */
    fun text(input: String): String {
        if (input.isEmpty()) return input
        var s = input
        if (accountPattern != null) s = accountPattern.replace(s, USER)
        if (s.indexOf(' ') >= 0) s = reduceAccountLines(s)
        for (root in roots) {
            if (s.contains(root)) s = s.replace(root, DATA)
        }
        if (s.contains("/data/")) s = APP_INSTALL.replace(APP_DATA.replace(s, APP), APP)
        if (s.contains("content://")) s = CONTENT_URI.replace(s) { contentUri(it.value) }
        if (s.indexOf('/') >= 0) {
            val before = s
            s = GAME_PATH.replace(before) { m ->
                // A file URI keeps its scheme, with the folders replaced the way content:// ones are.
                val isUri = before.startsWith(FILE_SCHEME, m.range.first - FILE_SCHEME.length)
                val name = gameFileName(m.groupValues[2], decode = isUri)
                if (isUri) "$REDACTED/$name" else name
            }
            s = VOLUME_PREFIX.replace(s, DATA)
        }
        return s
    }

    /** The log lines that print an account name by shape, so a name nobody told us about still goes. */
    private fun reduceAccountLines(line: String): String {
        var s = line
        if (s.contains("log in ")) s = LOGIN_ATTEMPT.replace(s) { "${it.groupValues[1]}$USER${it.groupValues[3]}" }
        if (s.contains("login with user")) s = LOGIN_USER.replace(s) { "${it.groupValues[1]}$USER${it.groupValues[3]}" }
        if (s.contains("logged in successfully")) s = LOGIN_DONE.replace(s) { "$USER${it.groupValues[2]}" }
        if (s.contains("Logging ")) s = LOGGING_OUT.replace(s) { "${it.groupValues[1]}$USER${it.groupValues[3]}" }
        return s
    }

    private fun gameFileName(name: String, decode: Boolean): String {
        if (!decode || !name.contains('%')) return name
        return try {
            URLDecoder.decode(name.replace("+", "%2B"), "UTF-8")
        } catch (_: Exception) {
            name
        }
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
     * The settings document (global settings, per-game overrides) with credentials removed, the
     * network and hardware names blanked, and paths reduced. The key filter catches a credential
     * added later without anyone remembering to list it; the explicit lists in [DROPPED_SETTINGS]
     * and [BLANKED_SETTINGS] cover the keys that are not credentials by name but identify a person
     * or their network.
     */
    fun settings(settings: JSONObject): JSONObject =
        blank(json(settings) { isSensitiveKey(it) || it in DROPPED_SETTINGS }) as JSONObject

    /** Replaces the value of a [BLANKED_SETTINGS] key (and of a DNS-host mapping's `url` and `ip`) with a marker, anywhere in [value]. */
    private fun blank(value: Any?, inHostMap: Boolean = false): Any? = when (value) {
        is JSONObject -> value.also { obj ->
            for (key in obj.keys().asSequence().toList()) {
                val child = obj.get(key)
                if (child is String && (key in BLANKED_SETTINGS || (inHostMap && (key == "url" || key == "ip")))) {
                    obj.put(key, blankedValue(child))
                } else {
                    obj.put(key, blank(child, inHostMap || key == "dev9EthHosts"))
                }
            }
        }
        is JSONArray -> value.also { array ->
            for (i in 0 until array.length()) array.put(i, blank(array.get(i), inHostMap))
        }
        else -> value
    }

    private fun blankedValue(text: String): String =
        if (text.isEmpty() || text == "0.0.0.0") text else REDACTED

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
        const val USER = "<user>"
        const val REDACTED = "<redacted>"
        const val APP = "<app>"
        const val DEFAULT_MAX_TEXT_BYTES = 48L * 1024 * 1024
        private const val MIN_NAME_LENGTH = 3
        private const val FILE_SCHEME = "file://"

        /** Settings that are removed outright: a secret, and names of hardware that identify the owner. */
        val DROPPED_SETTINGS = setOf("localLinkRoomCode", "micDevice1", "micDevice2")

        /** Settings whose value is a person's network address; a value that is set becomes `<redacted>`. */
        val BLANKED_SETTINGS = setOf("localLinkAddress", "dev9Ps2Ip", "dev9Mask", "dev9Gateway", "dev9Dns1", "dev9Dns2")

        // Lines the core and rcheevos print with an account name in them (Achievements.cpp, rc_client.c).
        private val LOGIN_USER = Regex("(login with user ')([^']*)(')")
        private val LOGIN_ATTEMPT = Regex("(Attempting to log in )(\\S+)( \\(with (?:password|token)\\))")
        private val LOGIN_DONE = Regex("(\\S+)( logged in successfully)")
        private val LOGGING_OUT = Regex("(Logging )(\\S+)( out)")

        private val SENSITIVE_KEY = Regex("token|passw|secret|login|username|userid|apikey|api_key|credential|cookie|bearer", RegexOption.IGNORE_CASE)

        /** A settings key that may hold a credential. */
        fun isSensitiveKey(key: String): Boolean = SENSITIVE_KEY.containsMatchIn(key)

        /** A key in the driver report that identifies the device's owner rather than its hardware. */
        fun isDeviceIdentifierKey(key: String): Boolean = key == "nodename"

        private val NODENAME_FIELD = Regex("\"nodename\"\\s*:\\s*\"(?:[^\"\\\\]|\\\\.)*\"\\s*,?")

        private val CONTENT_URI = Regex("content://[^\\s\"'<>)\\]]+")

        // Where an absolute path may start: not in the middle of a word or of a longer path
        // ("a/storage/x", "../storage/x"), but right after "file://" ("file:///storage/x"), where the
        // character before the path is the third slash of the scheme.
        private const val PATH_START = "(?<![A-Za-z0-9_.-])(?<![A-Za-z0-9_.-]/)"

        // An application id: reverse-DNS, so at least one dot. That keeps "/data/data/0/..." and the
        // like from being taken for a package.
        private const val PACKAGE = "[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"

        // The app's private data: /data/user/0/<package>, /data/user_de/0/<package>, /data/data/<package>.
        private val APP_DATA = Regex("$PATH_START/data/(?:user(?:_de)?/\\d+|data)/$PACKAGE")

        // Where it is installed: /data/app/~~<random>==/<package>-<random>== (Android 11+), or
        // /data/app/<package>-<random> on older versions. The random parts are base64 and may hold - and _.
        private val APP_INSTALL = Regex("$PATH_START/data/app/(?:~~[^/\\s\"'<>|]+/)?$PACKAGE-[^/\\s\"'<>|]+")

        // The image extensions a game can have. A path ending in one is a game path, whatever
        // folder it sits in, and is reduced to its file name.
        private const val GAME_EXT = "iso|bin|chd|cso|zso|gz|elf|cue|img|mdf|nrg|isz|dax|ciso|cdi|ps2"

        // "/storage/..." and friends, up to a game image's name. The folders are matched lazily and
        // may hold spaces, the name may hold spaces; the match ends where a delimiter follows the extension.
        private val GAME_PATH = Regex(
            "$PATH_START(/(?:storage|sdcard|mnt)(?:/[^\\r\\n\"'<>|]*?)?/([^/\\r\\n\"'<>|]+?\\.(?:$GAME_EXT)))" +
                "(?=$|[\\s\"'<>|,;:)\\]}])",
            RegexOption.IGNORE_CASE,
        )

        // The volume part of a shared-storage path. What follows it is kept.
        private val VOLUME_PREFIX = Regex(
            "$PATH_START(?:/storage/emulated/\\d+|/storage/self/primary|/storage/[^/\\s\"'<>|]+" +
                "|/sdcard\\d?|/mnt/sdcard|/mnt/media_rw/[^/\\s\"'<>|]+|/mnt/user/\\d+/[^/\\s\"'<>|]+)" +
                "(?=/|$|[\\s\"'<>|,;:)\\]}])",
        )

        /**
         * Lower-case hex SHA-256 of [file], read in blocks. [shouldStop] is asked between blocks; when
         * it says yes the hash is abandoned with [UploadCancelled], so a big file does not hold up a cancel.
         */
        fun sha256(file: File, shouldStop: () -> Boolean = { false }): String {
            val digest = java.security.MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    if (shouldStop()) throw UploadCancelled()
                    val n = input.read(buffer)
                    if (n < 0) break
                    digest.update(buffer, 0, n)
                }
            }
            return digest.digest().joinToString("") { "%02x".format(it) }
        }
    }
}
