package com.armsx2

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM tests for catalog schema and format handling: schema 1 stays ZIP-only, schema 2 requires
 * tar+zstd with revision and decompressed-size metadata, and unknown shapes drop entries without
 * poisoning the rest of the catalog. Runs against the real org.json implementation.
 */
class TextureCatalogTest {
    private val digest = "A".repeat(64)

    /** Programmatically built valid entry — string-template JSON invites stray commas. */
    private fun entryJson(
        format: String? = null,
        extra: (JSONObject) -> Unit = {},
    ): String = JSONObject().apply {
        put("id", "pack-1")
        put("name", "Test Pack")
        put("serials", arrayOf("SCUS-00000"))
        put("authors", arrayOf("someone"))
        put("version", "v1")
        put("downloadUrl", "https://cdn.example.com/pack.tar.zst")
        put("sourceUrl", "https://example.com/src")
        put("license", "")
        put("sizeBytes", 1000)
        put("sha256", digest)
        put("fileCount", 2)
        put("credits", "")
        put("description", "")
        put("previewUrls", arrayOf<String>())
        format?.let { put("format", it) }
        extra(this)
    }.toString()

    private fun catalog(schema: Int, vararg entries: String): String =
        """{"schemaVersion": $schema, "entries": [${entries.joinToString(",")}]}"""

    private fun parse(vararg entries: String, schema: Int = 1): List<TextureCatalog.Pack>? =
        TextureCatalog.parse(catalog(schema, *entries))

    // ---- schema 1: ZIP only -------------------------------------------------------------------

    @Test
    fun schema1AbsentFormatIsZip() {
        val packs = parse(entryJson())!!
        assertEquals(1, packs.size)
        assertEquals(TextureCatalog.ArchiveFormat.ZIP, packs[0].format)
        assertEquals(0L, packs[0].archiveRevision)
        assertEquals(0L, packs[0].decompressedSizeBytes)
    }

    @Test
    fun schema1ExplicitZipParses() {
        assertEquals(1, parse(entryJson(format = "zip"))!!.size)
    }

    @Test
    fun schema1TarZstdEntryDropped() {
        // A schema-1 reader cannot extract tar+zstd; the entry must go, not silently download.
        assertNull(parse(entryJson(format = "tar+zstd")))
    }

    @Test
    fun schema1UnknownFormatDropped() {
        assertNull(parse(entryJson(format = "rar")))
    }

    @Test
    fun schema1SplitZipStillParses() {
        val split = entryJson(format = "zip", extra = { o ->
            o.put("parts", org.json.JSONArray()
                .put(JSONObject().put("downloadUrl", "https://cdn.example.com/pa")
                    .put("sizeBytes", 600).put("sha256", digest))
                .put(JSONObject().put("downloadUrl", "https://cdn.example.com/pb")
                    .put("sizeBytes", 400).put("sha256", digest)))
        })
        val packs = parse(split)!!
        assertEquals(1, packs.size)
        assertEquals(2, packs[0].parts.size)
    }

    // ---- schema 2: tar+zstd with full metadata ------------------------------------------------

    private fun schema2Entry(extra: (JSONObject) -> Unit = {}): String =
        entryJson(format = "tar+zstd", extra = { o ->
            o.put("archiveRevision", 1)
            o.put("decompressedSizeBytes", 5000)
            extra(o)
        })

    @Test
    fun schema2FullMetadataParses() {
        val packs = parse(schema2Entry(), schema = 2)!!
        assertEquals(1, packs.size)
        assertEquals(TextureCatalog.ArchiveFormat.TAR_ZSTD, packs[0].format)
        assertEquals(1L, packs[0].archiveRevision)
        assertEquals(5000L, packs[0].decompressedSizeBytes)
    }

    @Test
    fun schema2RequiresPositiveRevision() {
        // absent revision drops the entry:
        assertNull(parse(entryJson(format = "tar+zstd", extra = { o ->
            o.put("decompressedSizeBytes", 5000)
        }), schema = 2))
        // revision zero or negative drops the entry:
        assertNull(parse(schema2Entry { o -> o.put("archiveRevision", 0) }, schema = 2))
        assertNull(parse(schema2Entry { o -> o.put("archiveRevision", -3) }, schema = 2))
        // positive revision parses:
        assertEquals(1, parse(schema2Entry { o -> o.put("archiveRevision", 7) }, schema = 2)!!.size)
    }

    @Test
    fun schema2RequiresBoundedDecompressedSize() {
        assertNull(parse(entryJson(format = "tar+zstd", extra = { o ->
            o.put("archiveRevision", 1)
        }), schema = 2))
        assertNull(parse(schema2Entry { o -> o.put("decompressedSizeBytes", 0) }, schema = 2))
        assertNull(parse(schema2Entry { o -> o.put("decompressedSizeBytes", 16L * 1024 * 1024 * 1024 + 1) }, schema = 2))
        assertEquals(1, parse(schema2Entry { o ->
            o.put("decompressedSizeBytes", 16L * 1024 * 1024 * 1024)
        }, schema = 2)!!.size)
    }

    @Test
    fun schema2ZipFormatDropped() {
        assertNull(parse(entryJson(format = "zip"), schema = 2))
    }

    // ---- who made the pack, and what kind of pack it is ---------------------------------------

    private fun creator(name: String, extra: (JSONObject) -> Unit = {}): JSONObject =
        JSONObject().put("name", name).also(extra)

    private fun onePack(extra: (JSONObject) -> Unit): TextureCatalog.Pack =
        parse(schema2Entry(extra), schema = 2)!!.single()

    /** An entry as `ps2ktxpak compile catalog` writes it, from the real data. The catalog is the only
     *  file the app reads about a pack, so a key renamed on either side has to break a test here. */
    private val generated = """
        {"id": "bl4ckh4nd-we-love-katamari-slus-21230-20260722", "name": "We Love Katamari HD Texture Pack",
         "gameTitle": "We Love Katamari", "serials": ["SLUS-21230"], "version": "2026-07-22 mirror",
         "authors": ["ironhulk33", "Bl4ckH4nd"], "credits": "Created by ironhulk33, Bl4ckH4nd.",
         "description": "A short description.", "license": "Redistribution permission not recorded",
         "downloadUrl": "https://dl.ps2ktxpak.net/packs/bl4ckh4nd-we-love-katamari-slus-21230-20260722.tar.zst",
         "format": "tar+zstd", "archiveRevision": 1, "decompressedSizeBytes": 1092782080,
         "sourceUrl": "https://www.mediafire.com/folder/k8yncbpnlckw3/ironhulk33#2ie0qajlodjxz",
         "sizeBytes": 506486922, "sha256": "e7b8eb04d4223eaed547be5aeefd9682e2ee05ef6ab5fdf57d18e0ba5d6992d1",
         "fileCount": 21316, "previewUrls": [],
         "creators": [
           {"name": "ironhulk33", "page": "https://gbatemp.net/members/ironhulk33.721797/",
            "avatar": "https://gbatemp.net/data/avatars/h/721/721797.jpg", "tip": "https://ko-fi.com/ironhulk33",
            "socials": "https://www.youtube.com/channel/UC_00zPyx8PXENf7OSqZRw_g"},
           {"name": "Bl4ckH4nd", "page": "https://gbatemp.net/members/bl4ckh4nd.609354/",
            "avatar": "https://gbatemp.net/data/avatars/h/609/609354.jpg", "tip": "https://www.patreon.com/Bl4ckH4nd"}],
         "type": "ai_upscale", "completeness": "complete"}
    """

    @Test
    fun anEntryAsTheGeneratorWritesItIsReadWhole() {
        val pack = parse(generated, schema = 2)!!.single()
        assertEquals("ironhulk33, Bl4ckH4nd", pack.creatorText)
        assertEquals(listOf("ironhulk33", "Bl4ckH4nd"), pack.creators.map { it.name })
        // The first creator named is the one whose page and picture stand for the pack.
        assertEquals("ironhulk33", pack.lead?.name)
        assertEquals("https://gbatemp.net/members/ironhulk33.721797/", pack.lead?.page)
        assertEquals("https://gbatemp.net/data/avatars/h/721/721797.jpg", pack.lead?.avatar)
        assertEquals("https://ko-fi.com/ironhulk33", pack.lead?.tip)
        assertEquals("https://www.youtube.com/channel/UC_00zPyx8PXENf7OSqZRw_g", pack.lead?.socials)
        // A creator without a socials link has none, rather than borrowing the lead's.
        assertNull(pack.creators[1].socials)
        assertEquals("ai_upscale", pack.type)
        assertEquals("complete", pack.completeness)
        assertEquals("https://www.mediafire.com/folder/k8yncbpnlckw3/ironhulk33#2ie0qajlodjxz", pack.sourceUrl)
    }

    @Test
    fun anEntryWithoutTheFieldsHasNoTypeOrCompleteness() {
        val pack = onePack {}
        assertNull(pack.type)
        assertNull(pack.completeness)
    }

    @Test
    fun anEntryFromBeforeCreatorsTakesItsAuthorsAsTheCreators() {
        // A catalog that predates the field said only the authors, so that is who made the pack.
        val pack = onePack {}
        assertEquals(listOf("someone"), pack.creators.map { it.name })
        assertNull(pack.lead?.page)
        assertEquals("someone", pack.creatorText)
    }

    @Test
    fun anEmptyCreatorsListMeansNoOneHasNamedTheCreator() {
        // The generator still puts a placeholder in authors for older apps; it is not a name.
        val pack = onePack { o ->
            o.put("authors", org.json.JSONArray().put("Unknown creator"))
            o.put("creators", org.json.JSONArray())
        }
        assertTrue(pack.creators.isEmpty())
        assertNull(pack.lead)
        assertEquals("", pack.creatorText)
    }

    @Test
    fun onlyHttpsLinksAreKept() {
        val pack = onePack { o ->
            o.put("creators", org.json.JSONArray().put(creator("Someone") { c ->
                c.put("page", "http://example.com/someone")
                c.put("avatar", "javascript:alert(1)")
                c.put("tip", "https://ko-fi.com/someone")
            }))
        }
        assertNull(pack.lead?.page)
        assertNull(pack.lead?.avatar)
        assertEquals("https://ko-fi.com/someone", pack.lead?.tip)
    }

    @Test
    fun aCreatorWithoutANameIsDropped() {
        val pack = onePack { o ->
            o.put("creators", org.json.JSONArray().put(creator("  ")).put(creator("Someone")))
        }
        assertEquals(listOf("Someone"), pack.creators.map { it.name })
    }

    // ---- whole-catalog behavior ---------------------------------------------------------------

    @Test
    fun unsupportedSchemaRejectedAtSourceLevel() {
        assertNull(TextureCatalog.parse(catalog(3, entryJson())))
        assertNull(TextureCatalog.parse(catalog(0, entryJson())))
    }

    @Test
    fun malformedEntryDroppedLocally() {
        val bad = entryJson().replace("\"fileCount\":2", "\"fileCount\":0")
        val packs = parse(entryJson(), bad)!!
        assertEquals(1, packs.size)
        assertEquals("pack-1", packs[0].id)
    }

    @Test
    fun digestMustBeWellFormed() {
        assertNull(parse(entryJson { o -> o.put("sha256", "ZZ".repeat(32)) }))
    }

    @Test
    fun realOrgJsonIsOnTheClasspath() {
        // Guards against silently running against the android.jar stub, which would make every
        // assertion above pass for the wrong reason (or fail confusingly).
        assertTrue(org.json.JSONObject("""{"a":1}""").optInt("a", 0) == 1)
    }
}
