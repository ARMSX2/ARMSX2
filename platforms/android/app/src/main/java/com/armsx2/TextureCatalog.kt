package com.armsx2

import android.content.Context
import android.util.Log
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * The online texture-pack catalog, hosted by sashkinbro and shared with us with his approval.
 *
 * Structure follows [SkinRepo]: fetch a manifest, list what matches, install on demand. The parsing
 * is deliberately written against `org.json` rather than kotlinx-serialization as upstream does —
 * `org.json` is already used everywhere in this app and needs no new Gradle dependency or plugin.
 *
 * Packs are keyed by PS2 **serial** only; the catalog carries no CRC. A pack whose serial list
 * contains the running game's serial is a match, and everything else is offered as a possible
 * other-region build of the same title.
 */
object TextureCatalog {
    private const val TAG = "TextureCatalog"

    /** Mirrors, tried in order. Same three upstream uses: the raw host is fastest, the second is a
     *  different GitHub edge, and jsDelivr survives GitHub being blocked on some networks. */
    private val CATALOG_URLS = listOf(
        "https://raw.githubusercontent.com/sashkinbro/EmuCoreX-Textures/main/textures.json",
        "https://github.com/sashkinbro/EmuCoreX-Textures/raw/main/textures.json",
        "https://cdn.jsdelivr.net/gh/sashkinbro/EmuCoreX-Textures@main/textures.json",
    )

    private const val SCHEMA_VERSION = 1
    private const val MAX_CATALOG_BYTES = 8L * 1024 * 1024
    private const val CACHE_TTL_MS = 6L * 60 * 60 * 1000
    private const val CACHE_FILE = "textures-v1.json"

    /** Upper bound on a single archive. Guards against a malformed entry proposing a download that
     *  could never fit; the real free-space check happens in [TexturePackInstaller]. */
    private const val MAX_ARCHIVE_BYTES = 4L * 1024 * 1024 * 1024

    data class Pack(
        val id: String,
        val name: String,
        val gameTitle: String,
        val serials: List<String>,
        val version: String,
        val authors: List<String>,
        val credits: String,
        val description: String,
        val license: String,
        val downloadUrl: String,
        val sourceUrl: String,
        val sizeBytes: Long,
        val sha256: String,
        val fileCount: Int,
    ) {
        fun matchesSerial(serial: String?): Boolean =
            !serial.isNullOrBlank() && serials.any { it.equals(serial, ignoreCase = true) }
    }

    /** [fromCache] true when the network was not reached and this is what we had on disk — the UI
     *  says so rather than presenting stale data as live. */
    data class Result(val packs: List<Pack>, val fromCache: Boolean)

    /** Blocking. Returns null only when there is neither a usable network response nor a cache. */
    fun fetch(context: Context, forceRefresh: Boolean = false): Result? {
        val cache = File(cacheDir(context), CACHE_FILE)
        if (!forceRefresh && cache.isFile &&
            (System.currentTimeMillis() - cache.lastModified()) < CACHE_TTL_MS
        ) {
            parse(runCatching { cache.readText() }.getOrNull())?.let { return Result(it, false) }
        }
        for (url in CATALOG_URLS) {
            val body = get(url) ?: continue
            val packs = parse(body) ?: continue
            runCatching {
                cache.parentFile?.mkdirs()
                cache.writeText(body)
            }
            return Result(packs, false)
        }
        // Every mirror failed. A stale catalogue still lets someone install, so it beats an error.
        return parse(runCatching { cache.readText() }.getOrNull())?.let { Result(it, true) }
    }

    private fun cacheDir(context: Context) = File(context.filesDir, "texture-catalog")

    // ---- parsing ------------------------------------------------------------------------------

    private fun parse(body: String?): List<Pack>? {
        if (body.isNullOrBlank()) return null
        return runCatching {
            val root = JSONObject(body)
            // A schema bump means fields we do not understand; refuse rather than guess.
            if (root.optInt("schemaVersion", -1) != SCHEMA_VERSION) {
                Log.w(TAG, "catalog schemaVersion=${root.opt("schemaVersion")} != $SCHEMA_VERSION")
                return null
            }
            val entries = root.optJSONArray("entries") ?: return null
            val out = ArrayList<Pack>(entries.length())
            val seen = HashSet<String>()
            for (i in 0 until entries.length()) {
                // One malformed entry must not cost the user the whole catalogue.
                val pack = runCatching { parsePack(entries.optJSONObject(i)) }.getOrNull() ?: continue
                if (seen.add(pack.id)) out.add(pack)
            }
            out
        }.getOrNull()
    }

    private fun parsePack(o: JSONObject?): Pack? {
        if (o == null) return null
        val id = o.optString("id").trim().ifEmpty { return null }
        val name = o.optString("name").trim().ifEmpty { return null }

        val serials = strings(o, "serials").mapNotNull(::normaliseSerial)
        if (serials.isEmpty()) return null

        val authors = strings(o, "authors")
        if (authors.isEmpty()) return null

        // https only, both for the archive and the credit link: these are URLs we hand to the
        // network stack and to the browser respectively, on someone else's say-so.
        val downloadUrl = o.optString("downloadUrl").takeIf(::isHttps) ?: return null
        val sourceUrl = o.optString("sourceUrl").takeIf(::isHttps) ?: return null

        val sizeBytes = o.optLong("sizeBytes", 0L)
        if (sizeBytes <= 0L || sizeBytes > MAX_ARCHIVE_BYTES) return null

        // The digest is the only thing standing between a corrupted or substituted download and the
        // user's texture folder, so an entry without a well-formed one is not installable.
        val sha256 = o.optString("sha256").trim().uppercase()
        if (!Regex("^[0-9A-F]{64}$").matches(sha256)) return null

        val fileCount = o.optInt("fileCount", 0)
        if (fileCount <= 0) return null

        return Pack(
            id = id,
            name = name,
            gameTitle = o.optString("gameTitle").trim(),
            serials = serials,
            version = o.optString("version").trim().ifEmpty { return null },
            authors = authors,
            credits = o.optString("credits").trim(),
            description = o.optString("description").trim(),
            license = o.optString("license").trim(),
            downloadUrl = downloadUrl,
            sourceUrl = sourceUrl,
            sizeBytes = sizeBytes,
            sha256 = sha256,
            fileCount = fileCount,
        )
    }

    private fun strings(o: JSONObject, key: String): List<String> {
        val arr = o.optJSONArray(key) ?: return emptyList()
        return (0 until arr.length()).mapNotNull { arr.optString(it).trim().ifEmpty { null } }
    }

    /** "slus 21287" / "SLUS_21287" -> "SLUS-21287". The catalogue is hand-maintained, so accept the
     *  separators people actually type and emit the one the emulator uses. */
    private fun normaliseSerial(raw: String): String? {
        val compact = raw.uppercase().filter { it.isLetterOrDigit() }
        if (!Regex("^[A-Z]{4}[0-9]{5}$").matches(compact)) return null
        return compact.substring(0, 4) + "-" + compact.substring(4)
    }

    private fun isHttps(url: String) = url.startsWith("https://", ignoreCase = true)

    /** Loose title key for "this pack is for another region of the same game". */
    fun titleKey(title: String): String =
        title.lowercase().filter { it.isLetterOrDigit() }

    // ---- http ---------------------------------------------------------------------------------

    private fun get(url: String): String? {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 15_000
                readTimeout = 20_000
                instanceFollowRedirects = true
                setRequestProperty("User-Agent", userAgent())
                setRequestProperty("Accept", "application/json")
            }
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "catalog $url -> ${conn.responseCode}")
                return null
            }
            conn.inputStream.use { input ->
                val buf = ByteArray(64 * 1024)
                val sb = StringBuilder()
                var total = 0L
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    total += n
                    if (total > MAX_CATALOG_BYTES) {
                        Log.w(TAG, "catalog $url exceeded $MAX_CATALOG_BYTES bytes")
                        return null
                    }
                    sb.append(String(buf, 0, n, Charsets.UTF_8))
                }
                sb.toString()
            }
        } catch (e: Exception) {
            Log.w(TAG, "catalog $url failed: ${e.message}")
            null
        } finally {
            conn?.disconnect()
        }
    }

    private fun userAgent(): String = "ARMSX2/" + runCatching {
        NativeApp.getBuildVersion()
    }.getOrNull().orEmpty().ifEmpty { "dev" }
}
