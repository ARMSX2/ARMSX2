package com.armsx2

import android.content.Context
import android.util.Log
import org.json.JSONObject

/**
 * Each online texture pack's links and details, from Sad Origami's Texture Packs Archive: the page
 * the pack comes from, its creator, their tip and socials pages, its texture type and status.
 *
 * Bundled as assets/texture-pack-links.json, keyed by catalog pack id, and built from the archive's
 * sheet by tools/texture-pack-links.py. The catalog's own sourceUrl is often a mirror (archive.org, a
 * MediaFire copy) rather than the creator's page; this is what replaces it. A pack the file does not
 * list keeps the catalog's source and shows nothing more.
 */
object TexturePackLinks {
    private const val TAG = "TexturePackLinks"
    private const val ASSET = "texture-pack-links.json"

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

    @Volatile private var loaded: Map<String, Links>? = null

    /** Every pack's links. Reads and parses the asset on first use, so call it off the main thread. */
    fun all(context: Context): Map<String, Links> {
        loaded?.let { return it }
        synchronized(this) {
            loaded?.let { return it }
            val map = runCatching {
                parse(context.assets.open(ASSET).bufferedReader().use { it.readText() })
            }.onFailure { Log.w(TAG, "could not read $ASSET: ${it.message}") }.getOrDefault(emptyMap())
            loaded = map
            return map
        }
    }

    internal fun parse(body: String): Map<String, Links> {
        val root = JSONObject(body)
        val packs = root.optJSONObject("packs") ?: return emptyMap()
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
        return out
    }

    private fun JSONObject.text(key: String): String? = optString(key).trim().ifEmpty { null }

    // https only, like the catalog's own links: these go straight to the browser.
    private fun JSONObject.url(key: String): String? =
        text(key)?.takeIf { it.startsWith("https://", ignoreCase = true) }
}
