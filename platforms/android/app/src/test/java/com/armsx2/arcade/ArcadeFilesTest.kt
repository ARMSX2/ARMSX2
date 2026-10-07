package com.armsx2.arcade

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM tests for how arcade games kept as their own files are told apart: by the game ID in a file's or
 * its folder's name, and for a .bin by its size, an image or a dongle. Whatever they decide is what the
 * library lists, and what it keeps out of its list.
 */
class ArcadeFilesTest {
    private val mb = 1L shl 20

    @Test
    fun theGameIdInAName() {
        assertEquals("NM00010", ArcadeFiles.idIn("NM00010.chd"))
        assertEquals("NM00010", ArcadeFiles.idIn("nm00010.iso"))
        assertEquals("NM00010", ArcadeFiles.idIn("Battle Gear 3 (NM00010).chd"))
        assertEquals("NM00031", ArcadeFiles.idIn("NM00031_DVD0-B.iso.gz"))
        assertNull(ArcadeFiles.idIn("SLUS-21410.iso"))
        // Not part of a longer token on either side.
        assertNull(ArcadeFiles.idIn("XNM00010.chd"))
        assertNull(ArcadeFiles.idIn("NM000101.chd"))
        assertNull(ArcadeFiles.idIn(null))
    }

    @Test
    fun whatAFileIs() {
        fun kind(name: String, size: Long = 0) = ArcadeFiles.kindOf(name) { size }
        assertEquals(ArcadeFiles.Kind.IMAGE, kind("NM00010.chd"))
        assertEquals(ArcadeFiles.Kind.IMAGE, kind("NM00010.iso"))
        assertEquals(ArcadeFiles.Kind.PACKED_IMAGE, kind("NM00031.iso.gz"))
        assertEquals(ArcadeFiles.Kind.CARD, kind("NM00010.ps2"))
        // A .bin is an image or a dongle by its size, and a small one neither.
        assertEquals(ArcadeFiles.Kind.IMAGE, kind("NM00031.bin", 600 * mb))
        assertEquals(ArcadeFiles.Kind.CARD, kind("NM00031.bin", 8 * mb))
        assertEquals(ArcadeFiles.Kind.OTHER, kind("NM00031.bin", 2 * mb))
        // Packed: a dongle shrinks far below its card's size; an image stays big.
        assertEquals(ArcadeFiles.Kind.CARD, kind("NM00031.bin.gz", 3 * mb))
        assertEquals(ArcadeFiles.Kind.PACKED_IMAGE, kind("NM00031.bin.gz", 2600 * mb))
        assertEquals(ArcadeFiles.Kind.OTHER, kind("boot.elf"))
        assertNull(kind("title.txt"))
        assertNull(kind("NM00010.acgame"))
    }

    @Test
    fun aGameKeptAsItsOwnFiles() {
        val sizes = mapOf("NM00010.chd" to 900 * mb, "NM00010.ps2" to 8650752L, "notes.txt" to 1L)
        val folder = ArcadeFiles.find(sizes.keys, folderId = null, covered = emptySet()) { sizes.getValue(it) }
        assertEquals(1, folder.games.size)
        assertEquals("NM00010", folder.games[0].id)
        assertEquals("NM00010.chd", folder.games[0].image)
        assertFalse(folder.games[0].packed)
        // The image and its dongle are kept out of the library's list; other files are not touched.
        assertEquals(setOf("nm00010.chd", "nm00010.ps2"), folder.claimed)
    }

    @Test
    fun aFolderNamedAfterItsGame() {
        val sizes = mapOf("game.chd" to 900 * mb, "dongle.bin" to 8 * mb, "boot.elf" to 1L, "sram.bin" to 1L)
        val folder = ArcadeFiles.find(sizes.keys, folderId = "NM00010", covered = emptySet()) { sizes.getValue(it) }
        assertEquals(listOf("game.chd"), folder.games.map { it.image })
        assertEquals(setOf("game.chd", "dongle.bin", "boot.elf", "sram.bin"), folder.claimed)
    }

    @Test
    fun aGameAnAcgameCoversIsListedOnceOnly() {
        val sizes = mapOf("NM00004.chd" to 900 * mb, "NM00004.ps2" to 8650752L)
        val folder = ArcadeFiles.find(sizes.keys, folderId = null, covered = setOf("NM00004")) { sizes.getValue(it) }
        assertTrue(folder.games.isEmpty())
        // Still kept out of the list: they are the .acgame's files, not PS2 games.
        assertEquals(setOf("nm00004.chd", "nm00004.ps2"), folder.claimed)
    }

    @Test
    fun twoImagesOfOneGameAreTwoEntries() {
        val sizes = mapOf("NM00031_DVD0-A.iso" to 4000 * mb, "NM00031_DVD0-B.iso" to 4000 * mb)
        val folder = ArcadeFiles.find(sizes.keys, folderId = null, covered = emptySet()) { sizes.getValue(it) }
        assertEquals(2, folder.games.size)
    }

    @Test
    fun aDongleIsNamedForWhatItHolds() {
        assertEquals("NM00010.bin", ArcadeFiles.dongleName("NM00010", 8 * mb))
        assertEquals("NM00010.ps2", ArcadeFiles.dongleName("NM00010", 8650752L))
    }

    @Test
    fun aSizeIsAskedForOnlyWhenTheNameCannotTell() {
        var asked = 0
        ArcadeFiles.find(listOf("NM00010.chd", "NM00010.ps2", "readme.txt"), null, emptySet()) { asked++; 0L }
        assertEquals(0, asked)
    }

    @Test
    fun aConquestCardIsToldFromADongleByWhatItHolds() {
        // Both are card files of Soul Calibur II's set, kept out of the library's list.
        assertEquals(ArcadeFiles.Kind.CARD, ArcadeFiles.kindOf("NM00007.conquestcard") { 0 })
        assertEquals(ArcadeFiles.Kind.CARD, ArcadeFiles.kindOf("cardmaterial.bin") { 8650752L })
        assertTrue(ArcadeFiles.isConquestCard("Memory Card for SoulCaliburII (C)1995 1998 2002 NAMCO LTD.".toByteArray()))
        assertFalse(ArcadeFiles.isConquestCard("Sony PS2 Memory Card Format 1.2.0.0".toByteArray()))
        assertFalse(ArcadeFiles.isConquestCard("Memory Card for Soul".toByteArray()))
        assertFalse(ArcadeFiles.isConquestCard(ByteArray(0)))
        // Never a .bin, which the core would convert as a card without ECC.
        assertEquals("NM00007.conquestcard", ArcadeFiles.CONQUEST_CARD)
    }

    @Test
    fun theBlankConquestCardThatComesWithTheApp() {
        val packed = listOf("src/main/assets", "app/src/main/assets", "platforms/android/app/src/main/assets")
            .map { java.io.File(it, "arcade/NM00007.conquestcard.gzip") }
            .first { it.isFile }
        val card = java.util.zip.GZIPInputStream(packed.inputStream()).use { it.readBytes() }
        // SC2MAKER's bin/cardmaterial.bin as it was taken, with its spare bytes: 16384 pages of 512 + 16.
        assertEquals(ArcadeFiles.CONQUEST_CARD_BYTES, card.size.toLong())
        assertTrue(ArcadeFiles.isConquestCard(card.copyOf(ArcadeFiles.CONQUEST_HEADER_BYTES)))
        val sha256 = java.security.MessageDigest.getInstance("SHA-256").digest(card).joinToString("") { "%02x".format(it) }
        assertEquals("fe7eac4c5566fa4f16680e2ce9ea682215207f87688dabc4b9065a30050c71f8", sha256)
    }
}
