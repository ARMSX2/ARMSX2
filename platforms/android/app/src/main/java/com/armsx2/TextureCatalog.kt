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

    /**
     * One piece of a split archive. A GitHub release asset caps at 2 GB, so the larger packs cannot
     * be published as a single file at all — they arrive as N pieces which concatenate, in this
     * order, into the zip [Pack.sha256] describes.
     *
     * Each piece carries its own digest as well. That is not redundant with the whole-archive one:
     * it says WHICH piece was corrupted, and it catches a truncated or substituted piece before we
     * have spent the rest of the transfer on it.
     */
    data class Part(
        val downloadUrl: String,
        val sizeBytes: Long,
        val sha256: String,
    )

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
        /** Empty for a single-file pack, in which case [downloadUrl] is the archive. */
        val parts: List<Part> = emptyList(),
    ) {
        fun matchesSerial(serial: String?): Boolean =
            !serial.isNullOrBlank() && serials.any { it.equals(serial, ignoreCase = true) }

        /**
         * The pieces to fetch, in order. A single-file pack presents as one piece so the installer
         * has exactly one path to maintain rather than a split one.
         */
        fun effectiveParts(): List<Part> =
            parts.ifEmpty { listOf(Part(downloadUrl, sizeBytes, sha256)) }
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

        // A split pack has no single archive URL, so downloadUrl is required only without parts.
        // PARTS_MALFORMED is distinct from "absent": a split entry we cannot fully validate must be
        // dropped, not silently downgraded to fetching part one and calling it the pack.
        val parts = parseParts(o) ?: return null

        // https only, both for the archive and the credit link: these are URLs we hand to the
        // network stack and to the browser respectively, on someone else's say-so.
        val downloadUrl = if (parts.isEmpty()) {
            o.optString("downloadUrl").takeIf(::isHttps) ?: return null
        } else {
            // Older builds parse this catalogue too, and they know nothing about parts. Leaving
            // downloadUrl off a split entry makes THEM drop that one entry (this same check) and
            // keep the rest of the catalogue, instead of downloading a fragment and unpacking junk.
            o.optString("downloadUrl").takeIf(::isHttps).orEmpty()
        }
        val sourceUrl = o.optString("sourceUrl").takeIf(::isHttps) ?: return null

        val sizeBytes = o.optLong("sizeBytes", 0L)
        if (sizeBytes <= 0L || sizeBytes > MAX_ARCHIVE_BYTES) return null

        // The parts must add up to the archive they claim to be. Catch that here rather than after
        // several gigabytes have been transferred and the final length check fails.
        if (parts.isNotEmpty() && parts.sumOf { it.sizeBytes } != sizeBytes) {
            Log.w(TAG, "pack $id parts sum ${parts.sumOf { it.sizeBytes }} != sizeBytes $sizeBytes")
            return null
        }

        // The digest is the only thing standing between a corrupted or substituted download and the
        // user's texture folder, so an entry without a well-formed one is not installable.
        val sha256 = o.optString("sha256").trim().uppercase()
        if (!SHA256_RE.matches(sha256)) return null

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
            parts = parts,
        )
    }

    /**
     * Returns the pieces of a split archive, an empty list when the entry has none, or null when a
     * "parts" key is present but unusable — the caller drops the entry in that case. An entry that
     * declares parts and gets them wrong must not fall back to [Pack.downloadUrl]: that would fetch
     * one fragment and try to unzip it.
     */
    private fun parseParts(o: JSONObject): List<Part>? {
        val arr = o.optJSONArray("parts") ?: return emptyList()
        if (arr.length() == 0) return null
        val out = ArrayList<Part>(arr.length())
        var total = 0L
        for (i in 0 until arr.length()) {
            val p = arr.optJSONObject(i) ?: return null
            val url = p.optString("downloadUrl").takeIf(::isHttps) ?: return null
            val size = p.optLong("sizeBytes", 0L)
            if (size <= 0L) return null
            // Guard the running total, not just each piece: enough valid pieces would otherwise
            // overflow the archive cap that the single-file path enforces.
            total += size
            if (total > MAX_ARCHIVE_BYTES) return null
            val digest = p.optString("sha256").trim().uppercase()
            if (!SHA256_RE.matches(digest)) return null
            out.add(Part(url, size, digest))
        }
        return out
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

    private val SHA256_RE = Regex("^[0-9A-F]{64}$")

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
