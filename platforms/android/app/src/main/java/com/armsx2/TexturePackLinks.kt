package com.armsx2

import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.HttpURLConnection

/**
 * Each online texture pack's links and details, from Sad Origami's Texture Packs Archive: the page
 * the pack comes from, its creator, their tip and socials pages, its texture type and status.
 *
 * Read-only data on our server, next to the catalog, so a link can be fixed or a creator added
 * without a release: texture-pack-links.json, keyed by catalog pack id, built from the archive's
 * sheet by tools/texture-pack-links.py. The app only ever downloads it. The catalog's own sourceUrl
 * is often a mirror (archive.org, a MediaFire copy) rather than the creator's page; this is what
 * replaces it. A pack the file does not list keeps the catalog's source and shows nothing more.
 */
object TexturePackLinks {
    private const val TAG = "TexturePackLinks"
    private const val URL = "https://dl.ps2ktxpak.net/texture-pack-links.json"

    /** The file's format, as this build reads it. A file in any other is left alone, and the copy
     *  from the last fetch kept, so the format can change later without breaking this build. */
    private const val SCHEMA_VERSION = 1
    private const val MAX_BYTES = 2 * 1024 * 1024
    /** Asked for again after this long, like the catalog. */
    private const val CACHE_TTL_MS = 6L * 60 * 60 * 1000
    private const val CACHE_FILE = "texture-pack-links.json"

    data class Links(
        val source: String? = null,
        val creator: String? = null,
        val tip: String? = null,
        val socials: String? = null,
        /** Worded as the archive words it: "AI Upscale", "Handcrafted", "Mixed", ... */
        val type: String? = null,
        /** Worded as the archive words it: "Complete", "In-Progress", "Incomplete", "Partial". */
        val status: String? = null,
        /** The creator's own page (their GBAtemp profile, else their socials or GitHub). */
        val creatorPage: String? = null,
        /** The creator's profile picture: their GBAtemp, YouTube or GitHub one. */
        val avatar: String? = null,
        /** No one has named this pack's creator: the catalog's authors field is a sentence about
         *  that, not a name, and is not shown as one. */
        val unknownCreator: Boolean = false,
    )

    /** Blocking. Every pack's links as the server has them, else as they were last fetched (offline,
     *  or the file is unreadable), else none. */
    fun fetch(context: Context): Map<String, Links> {
        val cache = File(File(context.filesDir, "texture-catalog"), CACHE_FILE)
        val cached = { runCatching { parse(cache.readText()) }.getOrNull() }
        if (cache.isFile && System.currentTimeMillis() - cache.lastModified() < CACHE_TTL_MS) {
            cached()?.let { return it }
        }
        val body = get()
        val links = body?.let(::parse)
        if (body != null && links != null) {
            runCatching {
                cache.parentFile?.mkdirs()
                val part = File(cache.path + ".part")
                part.writeText(body)
                if (!part.renameTo(cache)) part.delete()
            }
            return links
        }
        return cached().orEmpty()
    }

    /** Null when the file is not one this build reads: another format, malformed, or empty (an empty
     *  file is a mistake on the server, and must not replace the last good copy). */
    internal fun parse(body: String): Map<String, Links>? {
        val root = runCatching { JSONObject(body) }.getOrNull() ?: return null
        if (root.optInt("schemaVersion", -1) != SCHEMA_VERSION) {
            Log.w(TAG, "schemaVersion ${root.opt("schemaVersion")}, this build reads $SCHEMA_VERSION")
            return null
        }
        val packs = root.optJSONObject("packs") ?: return null
        // A pack names its creator; their page and picture are kept once per creator, not per pack.
        val creators = root.optJSONObject("creators")
        val out = HashMap<String, Links>(packs.length())
        for (id in packs.keys()) {
            val o = packs.optJSONObject(id) ?: continue
            val creator = o.text("creator")
            // "Panda_Venom, Yonko": the first named is the one whose page and picture show.
            val person = creator?.substringBefore(", ")?.let { creators?.optJSONObject(it) }
            out[id] = Links(
                source = o.url("source"),
                creator = creator,
                tip = o.url("tip"),
                socials = o.url("socials"),
                type = o.text("type"),
                status = o.text("status"),
                creatorPage = person?.url("page"),
                avatar = person?.url("avatar"),
                unknownCreator = o.optBoolean("unknown", false),
            )
        }
        return out.ifEmpty { null }
    }

    private fun JSONObject.text(key: String): String? = optString(key).trim().ifEmpty { null }

    // https only, like the catalog's own links: these go straight to the browser.
    private fun JSONObject.url(key: String): String? =
        text(key)?.takeIf { it.startsWith("https://", ignoreCase = true) }

    private fun get(): String? {
        val conn = TextureCatalog.RedirectingHttps.open(
            URL, connectTimeoutMs = 15_000, readTimeoutMs = 20_000, tag = TAG,
        ) {
            requestMethod = "GET"
            setRequestProperty("User-Agent", "ARMSX2/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev"))
            setRequestProperty("Accept", "application/json")
        } ?: return null
        return try {
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "$URL -> ${conn.responseCode}")
                return null
            }
            // Read whole, then decoded once: a character split across two reads stays whole.
            conn.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                val buf = ByteArray(32 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    if (out.size() + n > MAX_BYTES) {
                        Log.w(TAG, "$URL is over $MAX_BYTES bytes")
                        return null
                    }
                    out.write(buf, 0, n)
                }
                String(out.toByteArray(), Charsets.UTF_8)
            }
        } catch (e: Exception) {
            Log.w(TAG, "$URL failed: ${e.message}")
            null
        } finally {
            conn.disconnect()
        }
    }
}
