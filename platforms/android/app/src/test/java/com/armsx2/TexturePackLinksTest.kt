package com.armsx2

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM tests for the texture pack links file the app downloads: the format check that lets the file
 * change later without breaking this build, a broken upload never replacing the last good copy, and
 * what reaches the browser. Runs against the real org.json implementation.
 */
class TexturePackLinksTest {
    /** Programmatically built, like the server's file: string-template JSON invites stray commas. */
    private fun file(
        schema: Int? = 1,
        packs: JSONObject = pack(),
        creators: JSONObject? = creators(),
    ): String = JSONObject().apply {
        schema?.let { put("schemaVersion", it) }
        put("about", "test")
        creators?.let { put("creators", it) }
        put("packs", packs)
    }.toString()

    private fun pack(extra: (JSONObject) -> Unit = {}): JSONObject =
        JSONObject().put("pack-1", JSONObject().apply {
            put("source", "https://gbatemp.net/threads/pack.1/")
            put("creator", "Someone, Other")
            put("tip", "https://ko-fi.com/someone")
            put("socials", "https://youtube.com/@someone")
            put("type", "Handcrafted")
            put("status", "Complete")
            extra(this)
        })

    private fun creators(): JSONObject = JSONObject().put("Someone", JSONObject().apply {
        put("page", "https://gbatemp.net/members/someone.1/")
        put("avatar", "https://gbatemp.net/data/avatars/h/0/1.jpg")
    })

    @Test
    fun readsAPack() {
        val links = TexturePackLinks.parse(file())!!.getValue("pack-1")
        assertEquals("https://gbatemp.net/threads/pack.1/", links.source)
        assertEquals("Someone, Other", links.creator)
        assertEquals("https://ko-fi.com/someone", links.tip)
        assertEquals("https://youtube.com/@someone", links.socials)
        assertEquals("Handcrafted", links.type)
        assertEquals("Complete", links.status)
        // The first creator named is the one whose page and picture show.
        assertEquals("https://gbatemp.net/members/someone.1/", links.creatorPage)
        assertEquals("https://gbatemp.net/data/avatars/h/0/1.jpg", links.avatar)
    }

    @Test
    fun anotherFormatIsLeftAlone() {
        assertNull(TexturePackLinks.parse(file(schema = null)))
        assertNull(TexturePackLinks.parse(file(schema = 2)))
    }

    @Test
    fun anEmptyOrBrokenFileIsNotTakenForOne() {
        // Each of these would otherwise replace the last good copy with nothing.
        assertNull(TexturePackLinks.parse(file(packs = JSONObject())))
        assertNull(TexturePackLinks.parse(""))
        assertNull(TexturePackLinks.parse("{"))
        assertNull(TexturePackLinks.parse("<html><body>404 Not Found</body></html>"))
    }

    @Test
    fun onlyHttpsLinksReachTheBrowser() {
        val links = TexturePackLinks.parse(file(packs = pack {
            it.put("source", "http://example.com/pack")
            it.put("tip", "javascript:alert(1)")
        }))!!.getValue("pack-1")
        assertNull(links.source)
        assertNull(links.tip)
        assertEquals("https://youtube.com/@someone", links.socials)
    }

    @Test
    fun aPackWithNoNamedCreator() {
        val links = TexturePackLinks.parse(file(packs = JSONObject().put("pack-2", JSONObject().put("unknown", true))))!!
            .getValue("pack-2")
        assertTrue(links.unknownCreator)
        assertNull(links.creator)
        assertNull(links.source)
    }

    @Test
    fun aCreatorTheFileHasNoPageForShowsNoPicture() {
        val links = TexturePackLinks.parse(file(creators = null))!!.getValue("pack-1")
        assertEquals("Someone, Other", links.creator)
        assertNull(links.creatorPage)
        assertNull(links.avatar)
    }
}
