package com.armsx2.ui.saves

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The Save Manager's view of the save folders: what a file name says, and how games group. */
class SaveCatalogTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private fun save(root: File, name: String, modified: Long, sub: String = ""): File {
        val dir = if (sub.isEmpty()) root else File(root, sub).apply { mkdirs() }
        return File(dir, name).apply {
            writeText("x")
            setLastModified(modified)
        }
    }

    @Test
    fun aNumberedSlotNamesItsGameAndSlot() {
        val id = SaveFileName.parse("SCUS-97328 (77E61C8A).02.p2s")!!
        assertEquals("SCUS-97328", id.serial)
        assertEquals("77E61C8A", id.crc)
        assertEquals(SaveKind.Slot, id.kind)
        assertEquals(2, id.slot)
        assertNull(id.autosaveIndex)
    }

    @Test
    fun theNewestAutosaveIsOneAndOlderOnesCountUp() {
        assertEquals(1, SaveFileName.parse("SLES-53998 (03F8D7E0).autosave.p2s")!!.autosaveIndex)
        val older = SaveFileName.parse("SLES-53998 (03F8D7E0).autosave.3.p2s")!!
        assertEquals(SaveKind.Autosave, older.kind)
        assertEquals(3, older.autosaveIndex)
        assertNull(older.slot)
    }

    @Test
    fun filesThatAreNotSavesAreIgnored() {
        assertNull(SaveFileName.parse("SCUS-97353 (45FE0CC4).00.p2s.backup"))
        assertNull(SaveFileName.parse("SCUS-97353 (45FE0CC4).autosave.5.p2s.drop"))
        assertNull(SaveFileName.parse("notes.txt"))
        assertNull(SaveFileName.parse("SCUS-97353.00.p2s"))
    }

    @Test
    fun theSameSaveInTwoFoldersIsListedOnceWithTheNewerFile() {
        val a = tmp.newFolder("a", "sstates")
        val b = tmp.newFolder("b", "sstates")
        save(a, "SCUS-97328 (77E61C8A).02.p2s", modified = 1_000)
        val newer = save(b, "SCUS-97328 (77E61C8A).02.p2s", modified = 9_000)
        val found = SaveCatalog.scan(listOf(a, b))
        assertEquals(1, found.size)
        assertEquals(newer.absolutePath, found[0].file.absolutePath)
    }

    @Test
    fun backupsAreFlaggedAndNeverMergedWithTheLiveSave() {
        val root = tmp.newFolder("sstates")
        save(root, "SCUS-97328 (77E61C8A).02.p2s", modified = 5_000)
        save(root, "SCUS-97328 (77E61C8A).02.p2s", modified = 2_000, sub = "backups/100")
        save(root, "SCUS-97328 (77E61C8A).02.p2s", modified = 3_000, sub = "backups/200")
        val found = SaveCatalog.scan(listOf(root))
        assertEquals(3, found.size)
        assertEquals(2, found.count { it.isBackup })
        assertFalse(found.single { !it.isBackup }.isBackup)
    }

    @Test
    fun gamesGroupByTheirSerialInOrderOfTheirLastSave() {
        val root = tmp.newFolder("sstates")
        save(root, "SCUS-97328 (77E61C8A).03.p2s", modified = 1_000)
        save(root, "SCUS-97328 (77E61C8A).00.p2s", modified = 2_000)
        save(root, "SCUS-97328 (77E61C8A).autosave.2.p2s", modified = 1_500)
        save(root, "SCUS-97328 (77E61C8A).autosave.p2s", modified = 1_600)
        save(root, "SLES-53998 (03F8D7E0).01.p2s", modified = 8_000)
        save(root, "SCUS-97328 (77E61C8A).00.p2s", modified = 9_999, sub = "backups/1")

        val sets = SaveCatalog.groupBySerial(SaveCatalog.scan(listOf(root)))

        // The backup is newer than everything, but a backup is not "the last time you played".
        assertEquals(listOf("SLES-53998", "SCUS-97328"), sets.map { it.serial })
        val game = sets.single { it.serial == "SCUS-97328" }
        assertEquals(listOf(1, 2), game.autosaves.map { it.id.autosaveIndex })
        assertEquals(listOf(0, 3), game.slots.map { it.id.slot })
        assertEquals(1, game.backups.size)
        assertEquals(4, game.count)
        assertEquals(2_000L, game.newest)
    }

    @Test
    fun aBackupCopyOfAnAutosaveCannotBeLoadedButEverythingElseCan() {
        val root = tmp.newFolder("sstates")
        save(root, "SLES-53998 (03F8D7E0).autosave.p2s", modified = 1_000)
        save(root, "SLES-53998 (03F8D7E0).autosave.p2s", modified = 2_000, sub = "backups/1")
        save(root, "SLES-53998 (03F8D7E0).01.p2s", modified = 3_000, sub = "backups/1")
        val found = SaveCatalog.scan(listOf(root))
        val (cannot, can) = found.partition { it.isBackup && it.id.kind == SaveKind.Autosave }
        assertEquals(1, cannot.size)
        assertTrue(cannot.none { it.canLoad })
        assertEquals(2, can.size)
        assertTrue(can.all { it.canLoad })
    }

    @Test
    fun aGameWithOnlyBackupsStillAppearsWithNoLiveSaves() {
        val root = tmp.newFolder("sstates")
        save(root, "SLES-53998 (03F8D7E0).01.p2s", modified = 4_000, sub = "backups/1")
        val set = SaveCatalog.groupBySerial(SaveCatalog.scan(listOf(root))).single()
        assertEquals(0, set.count)
        assertEquals(4_000L, set.newest)
        assertTrue(set.slots.isEmpty())
    }
}
