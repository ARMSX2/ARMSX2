package com.armsx2.ui.textures

import com.armsx2.TextureCatalog
import com.armsx2.TextureCatalog.Creator
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** How the pack browser sorts packs under their creators. */
class CreatorGroupsTest {
    private fun pack(id: String, game: String, vararg creators: Creator) = TextureCatalog.Pack(
        id = id, name = id, gameTitle = game, serials = listOf("SCUS-00000"), version = "v1",
        authors = creators.map { it.name }.ifEmpty { listOf("Unknown creator") }, credits = "", description = "",
        license = "", downloadUrl = "https://example.com/$id", sourceUrl = "https://example.com/src",
        sizeBytes = 1, sha256 = "A".repeat(64), fileCount = 1, creators = creators.toList(),
    )

    @Test
    fun aPackIsItsFirstNamedCreatorsAndAnUnknownCreatorIsLast() {
        val groups = creatorGroups(listOf(
            pack("a", "Zed", Creator("Zoe")),
            pack("b", "Alpha"),
            pack("c", "Beta", Creator("alice"), Creator("Zoe")),
            pack("d", "Gamma", Creator("Zoe")),
        ))
        // Case-insensitive, so "alice" does not sort after "Zoe"; no one named comes last.
        assertEquals(listOf("alice", "Zoe", ""), groups.map { it.key })
        // A pack credited "alice, Zoe" is alice's. Within a creator, by game.
        assertEquals(listOf("c"), groups[0].packs.map { it.id })
        assertEquals(listOf("d", "a"), groups[1].packs.map { it.id })
    }

    @Test
    fun aCreatorsPictureIsTheFirstOneTheirPacksHave() {
        val groups = creatorGroups(listOf(
            pack("a", "Alpha", Creator("Zoe")),
            pack("b", "Beta", Creator("Zoe", avatar = "https://example.com/zoe.jpg")),
            pack("c", "Gamma", Creator("Kim")),
        ))
        assertEquals("https://example.com/zoe.jpg", groups.single { it.key == "Zoe" }.avatar)
        assertNull(groups.single { it.key == "Kim" }.avatar)
    }
}
