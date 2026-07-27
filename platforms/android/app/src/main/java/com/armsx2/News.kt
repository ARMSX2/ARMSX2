package com.armsx2

import android.content.Context
import android.util.Log
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Release notes from the GitHub releases feed, for the in-app news board.
 *
 * Deliberately in `main` rather than `src/github`: the updater lives in the github flavor because
 * it downloads and installs an APK, which Play forbids. Reading release notes is not that — it is
 * text — so both flavors get it. Nothing here links to or fetches an asset, and the UI shows no
 * download affordance, which is what keeps that true. Do NOT add one without moving this.
 *
 * Same shape as [TextureCatalog]: cache with a TTL, and on a total network failure fall back to
 * whatever is on disk rather than showing an error — stale notes still beat a blank page.
 */
object News {
    private const val TAG = "News"
    private const val RELEASES_URL = "https://api.github.com/repos/ARMSX2/ARMSX2/releases?per_page=20"
    private const val CACHE_FILE = "releases.json"
    private const val CACHE_TTL_MS = 6L * 60 * 60 * 1000 // 6h; releases are not frequent
    private const val MAX_BODY_BYTES = 512 * 1024

    data class Item(
        val tag: String,
        val title: String,
        val notes: String,
        val published: String,
        val prerelease: Boolean,
    )

    /** [fromCache] true when the network was not reached, so the UI can say so. */
    data class Result(val items: List<Item>, val fromCache: Boolean)

    /** Blocking. Call from a background dispatcher. Null only when there is neither network nor cache. */
    fun fetch(context: Context, forceRefresh: Boolean = false): Result? {
        val cache = File(cacheDir(context), CACHE_FILE)
        if (!forceRefresh && cache.isFile &&
            (System.currentTimeMillis() - cache.lastModified()) < CACHE_TTL_MS
        ) {
            parse(runCatching { cache.readText() }.getOrNull())?.let { return Result(it, false) }
        }
        val body = get(RELEASES_URL)
        val parsed = parse(body)
        if (parsed != null) {
            runCatching {
                cache.parentFile?.mkdirs()
                cache.writeText(body!!)
            }
            return Result(parsed, false)
        }
        return parse(runCatching { cache.readText() }.getOrNull())?.let { Result(it, true) }
    }

    private fun cacheDir(context: Context) = File(context.filesDir, "news")

    private fun parse(body: String?): List<Item>? {
        if (body.isNullOrBlank()) return null
        return runCatching {
            val arr = JSONArray(body)
            buildList {
                for (i in 0 until arr.length()) {
                    val o = arr.optJSONObject(i) ?: continue
                    if (o.optBoolean("draft", false)) continue
                    val tag = o.optString("tag_name").trim().ifEmpty { continue }
                    add(
                        Item(
                            tag = tag,
                            // Releases are often published with no name; the tag is the fallback
                            // rather than an empty heading.
                            title = o.optString("name").trim().ifEmpty { tag },
                            notes = tidy(o.optString("body", "")),
                            published = o.optString("published_at").take(10), // yyyy-MM-dd
                            prerelease = o.optBoolean("prerelease", false),
                        ),
                    )
                }
            }
        }.getOrNull()
    }

    /**
     * Release bodies are GitHub-flavoured markdown. Rendering markdown properly is a dependency and
     * a lot of surface for what is mostly bullet lists, so flatten the handful of constructs that
     * actually show up. Anything unrecognised is left alone rather than mangled.
     */
    private fun tidy(raw: String): String = raw
        .replace("\r\n", "\n")
        .lines()
        .joinToString("\n") { line ->
            line.trimEnd()
                .replace(Regex("^#{1,6}\\s*"), "")     // headings
                .replace(Regex("^\\s*[-*]\\s+"), "• ") // bullets
                .replace(Regex("\\*\\*(.+?)\\*\\*"), "$1")
                .replace(Regex("`([^`]+)`"), "$1")
        }
        .replace(Regex("\n{3,}"), "\n\n")
        .trim()

    private fun get(url: String): String? {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 15_000
                readTimeout = 20_000
                instanceFollowRedirects = true
                setRequestProperty("User-Agent", userAgent())
                setRequestProperty("Accept", "application/vnd.github+json")
            }
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "releases $url -> ${conn.responseCode}")
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
                    if (total > MAX_BODY_BYTES) {
                        Log.w(TAG, "releases body over ${MAX_BODY_BYTES}B, refusing")
                        return null
                    }
                    sb.append(String(buf, 0, n))
                }
                sb.toString()
            }
        } catch (e: Exception) {
            Log.w(TAG, "releases $url failed: ${e.message}")
            null
        } finally {
            conn?.disconnect()
        }
    }

    private fun userAgent(): String = "ARMSX2/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev")
}
