package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * JVM tests for how a cheat source's file list is narrowed to one game ([PatchRepo.matchCheatFiles])
 * and to the folders that count ([PatchRepo.inFolders]). File names are real ones from the sources.
 */
class PatchRepoMatchTest {
    private val largeDb = listOf(
        "cheats-v1/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
        "cheats-v2/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
        "cheats-v2/.hack - G.U. Vol. 1 - Saitan - Terminal Disc -THE END OF THE WORLD SLPS-25652.pnach",
        "cheats-v6/SCES-50139_B9373369.pnach",
        "patches-v3/patches/SCES-50139_B9373369.pnach",
        "PCSX2 Patches/SCES-50760_5C991F4E.pnach",
        "cheats-v5/cheats/02E1970F.pnach",
    )

    @Test
    fun crcMatchesEveryNameShape() {
        assertEquals(
            listOf(
                "cheats-v1/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
                "cheats-v2/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
            ),
            PatchRepo.matchCheatFiles(largeDb, "0001171a", "SLUS-20563"),
        )
    }

    @Test
    fun serialFindsTitledNamesWhenTheCrcDiffers() {
        // Another revision of the disc: its CRC is in no filename, so the serial decides.
        assertEquals(
            listOf(
                "cheats-v1/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
                "cheats-v2/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
            ),
            PatchRepo.matchCheatFiles(largeDb, "DEADBEEF", "slus-20563"),
        )
        assertEquals(
            listOf("cheats-v2/.hack - G.U. Vol. 1 - Saitan - Terminal Disc -THE END OF THE WORLD SLPS-25652.pnach"),
            PatchRepo.matchCheatFiles(largeDb, null, "SLPS-25652"),
        )
    }

    @Test
    fun serialFindsSerialCrcNames() {
        assertEquals(
            listOf("cheats-v6/SCES-50139_B9373369.pnach", "patches-v3/patches/SCES-50139_B9373369.pnach"),
            PatchRepo.matchCheatFiles(largeDb, null, "SCES-50139"),
        )
    }

    @Test
    fun serialDoesNotMatchANeighbour() {
        assertEquals(emptyList<String>(), PatchRepo.matchCheatFiles(largeDb, null, "SLUS-20564"))
        assertEquals(emptyList<String>(), PatchRepo.matchCheatFiles(largeDb, null, "SCES-5013"))
    }

    @Test
    fun crcMatchWinsOverSerial() {
        // A CRC hit in any file stops the serial search, so another revision's files stay out.
        val tree = listOf("cheats-v6/SLUS-20563_11111111.pnach", "cheats-v6/SLUS-20563_22222222.pnach")
        assertEquals(listOf("cheats-v6/SLUS-20563_22222222.pnach"), PatchRepo.matchCheatFiles(tree, "22222222", "SLUS-20563"))
    }

    @Test
    fun rawPathEncodesEachSegment() {
        assertEquals(
            "cheats-v1/1E65A50E%20-%20Resident%20Evil%20Outbreak%20File%20%232%20SLUS-20984.pnach",
            PatchRepo.rawPath("cheats-v1/1E65A50E - Resident Evil Outbreak File #2 SLUS-20984.pnach"),
        )
        assertEquals("cheats-v1/120%25%20a%2Bb%20%5Bx%5D.pnach", PatchRepo.rawPath("cheats-v1/120% a+b [x].pnach"))
        assertEquals("PCSX2%20Patches/SCES-50760_5C991F4E.pnach", PatchRepo.rawPath("PCSX2 Patches/SCES-50760_5C991F4E.pnach"))
    }

    @Test
    fun onlyTheLargeDatabasesOwnCheatFoldersCount() {
        // Its copies of the patch databases and of a collection searched anyway are left out.
        assertEquals(
            listOf(
                "cheats-v1/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
                "cheats-v2/0001171A - .hack - Outbreak Part 3 SLUS-20563.pnach",
                "cheats-v2/.hack - G.U. Vol. 1 - Saitan - Terminal Disc -THE END OF THE WORLD SLPS-25652.pnach",
                "cheats-v6/SCES-50139_B9373369.pnach",
            ),
            PatchRepo.inFolders(largeDb, listOf("cheats-v1/", "cheats-v2/", "cheats-v6/")),
        )
        assertEquals(largeDb, PatchRepo.inFolders(largeDb, emptyList()))
    }
}
