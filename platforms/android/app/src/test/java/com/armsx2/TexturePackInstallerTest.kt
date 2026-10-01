package com.armsx2

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeFalse
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class TexturePackInstallerTest {
    @get:Rule
    val tmp = TemporaryFolder()

    @Test
    fun recognizesKtxTexturesCaseInsensitively() {
        // The policy itself moved to TextureArchivePath so ZIP and tar extraction share it.
        assertTrue(TextureArchivePath.isTextureFile("texture.ktx"))
        assertTrue(TextureArchivePath.isTextureFile("nested/TEXTURE.KTX"))
        assertFalse(TextureArchivePath.isTextureFile("texture.ktx2"))
        assertFalse(TextureArchivePath.isTextureFile("texture.ktx.txt"))
    }

    @Test
    fun tarPathsValidate() {
        // The producer's leading "./" is stripped; traversal and absolute paths are refused.
        assertTrue(TextureArchivePath.validateTarPath("./replacements/a.ktx") == "replacements/a.ktx")
        assertTrue(TextureArchivePath.validateTarPath("./") == "")
        assertTrue(TextureArchivePath.validateTarPath("/etc/passwd") == null)
        assertTrue(TextureArchivePath.validateTarPath("replacements/../../x") == null)
        assertTrue(TextureArchivePath.validateTarPath("a//b") == null)
        assertTrue(TextureArchivePath.validateTarPath("a/" + "c".repeat(300)) == null)
    }

    @Test
    fun collisionKeyIsNfcAndLocaleIndependent() {
        val a = TextureArchivePath.collisionKey("Replacements/Tür.ktx")
        val b = TextureArchivePath.collisionKey("replacements/tür.ktx") // decomposed ü
        assertTrue(a == b)
        // Locale-independent: Turkish "I" must not dot itself, regardless of device locale.
        assertTrue(TextureArchivePath.collisionKey("FILE.KTX") == TextureArchivePath.collisionKey("file.ktx"))
    }

    // ---- Removing a pack ----

    /** `textures/SLES-53058/replacements` holding a small pack, one level nested. */
    private fun gameFolder(): File {
        val game = File(tmp.newFolder("textures"), "SLES-53058")
        File(game, "replacements/sub").mkdirs()
        File(game, "replacements/a.ktx").writeText("a")
        File(game, "replacements/sub/b.ktx").writeText("b")
        return game
    }

    @Test
    fun removeTakesThePackAndAnOldCopyButKeepsDumps() {
        val game = gameFolder()
        File(game, "replacements.old").mkdirs()
        File(game, "replacements.old/c.ktx").writeText("c")
        File(game, "dumps").mkdirs()
        File(game, "dumps/d.png").writeText("d")

        assertTrue(TexturePackInstaller.removePack(game))
        assertFalse(File(game, "replacements").exists())
        assertFalse(File(game, "replacements.old").exists())
        assertTrue(File(game, "dumps/d.png").isFile)
        assertEquals(listOf("SLES-53058"), game.parentFile!!.list()!!.toList())
    }

    @Test
    fun removeDropsTheGameFolderOnceItIsEmpty() {
        val game = gameFolder()
        assertTrue(TexturePackInstaller.removePack(game))
        assertFalse(game.exists())
        assertEquals(0, game.parentFile!!.list()!!.size)
    }

    @Test
    fun aFileThatWillNotDeleteStillTakesThePackOutOfUse() {
        // Root ignores the permission this relies on.
        assumeFalse(System.getProperty("user.name") == "root")
        val game = gameFolder()
        val textures = game.parentFile!!
        val stuck = File(game, "replacements/sub")
        assertTrue(stuck.setWritable(false)) // b.ktx inside it can no longer be deleted
        try {
            assertTrue(TexturePackInstaller.removePack(game))
            assertFalse(File(game, "replacements").exists())
            val leftover = textures.listFiles()!!.filter { it.name.startsWith(".deleting-") }
            assertEquals(1, leftover.size)

            // Once the file can go, the sweep finishes the job.
            leftover.single().walkTopDown().forEach { it.setWritable(true) }
            TexturePackInstaller.sweepRemovedPacks(textures)
            assertTrue(textures.listFiles()!!.none { it.name.startsWith(".deleting-") })
        } finally {
            textures.walkTopDown().forEach { it.setWritable(true) }
        }
    }
}
