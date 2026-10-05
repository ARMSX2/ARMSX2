package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * JVM tests for [ShaderIndex]: pass counting through `#reference`, that a valid index is
 * actually used instead of rescanning, and every way it is supposed to be thrown away.
 *
 * "Used instead of rescanning" is proven by editing a preset's contents in place, which
 * changes no directory's modified time: an index that is read back still reports the old pass
 * count, and a rescan reports the new one.
 */
class ShaderIndexTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private fun root(): File = File(tmp.root, "shaders").apply { mkdirs() }

    private fun write(root: File, rel: String, text: String): File =
        File(root, rel).apply { parentFile!!.mkdirs(); writeText(text) }

    private fun passes(entries: List<ShaderIndex.Entry>): Map<String, Int?> =
        entries.associate { it.relPath to it.passes }

    /** A small pack: a direct preset, a quoted count, and a two-hop #reference chain. */
    private fun seed(root: File) {
        write(root, "pack/crt/a.slangp", "shaders = 3\n")
        write(root, "pack/crt/b.slangp", "shaders = \"5\"\n")
        write(root, "pack/bezel/wrap.slangp", "#reference \"../crt/a.slangp\"\nfoo = 1\n")
        write(root, "pack/bezel/wrap2.slangp", "#reference \"wrap.slangp\"\n")
        write(root, "pack/crt/stage.slang", "not a preset")
    }

    /** Push every directory's modified time forward, as a file manager's write would. */
    private fun touchDirs(vararg dirs: File) = dirs.forEach { it.setLastModified(it.lastModified() + 10_000) }

    @Test
    fun scanCountsPassesThroughReferences() {
        val root = root().also(::seed)
        assertEquals(
            mapOf(
                "pack/crt/a.slangp" to 3,
                "pack/crt/b.slangp" to 5,
                "pack/bezel/wrap.slangp" to 3,
                "pack/bezel/wrap2.slangp" to 3,
            ),
            passes(ShaderIndex.load(root)),
        )
        assertTrue(ShaderIndex.indexFile(root).isFile)
        // Beside the root, never inside it: writing it inside would move the root's own
        // modified time and invalidate the fingerprint it was written with.
        assertEquals(root.parentFile!!.absoluteFile, ShaderIndex.indexFile(root).parentFile!!.absoluteFile)
    }

    @Test
    fun validIndexIsReadInsteadOfRescanning() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        File(root, "pack/crt/a.slangp").writeText("shaders = 9\n")
        assertEquals(3, passes(ShaderIndex.load(root))["pack/crt/a.slangp"])
    }

    @Test
    fun aNewPackFolderForcesARescan() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        write(root, "other/x.slangp", "shaders = 2\n")
        touchDirs(root)
        assertEquals(2, passes(ShaderIndex.load(root))["other/x.slangp"])
    }

    @Test
    fun aNewCategoryInsideAPackForcesARescan() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        write(root, "pack/ntsc/n.slangp", "shaders = 4\n")
        touchDirs(File(root, "pack"))
        assertEquals(4, passes(ShaderIndex.load(root))["pack/ntsc/n.slangp"])
    }

    @Test
    fun changeRemovesTheIndex() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        ShaderIndex.change(root) {
            assertFalse("index must be gone while the tree is changing", ShaderIndex.indexFile(root).exists())
            File(root, "pack/crt/a.slangp").writeText("shaders = 9\n")
        }
        assertFalse(ShaderIndex.indexFile(root).exists())
        assertEquals(9, passes(ShaderIndex.load(root))["pack/crt/a.slangp"])
    }

    @Test
    fun changeRemovesTheIndexEvenWhenTheBlockThrows() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        runCatching { ShaderIndex.change(root) { error("boom") } }
        assertFalse(ShaderIndex.indexFile(root).exists())
    }

    @Test
    fun changeFilesPatchesAValidIndexWithoutRescanning() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        // A stale value the patch must keep: proof the rest of the index was not rescanned.
        File(root, "pack/crt/b.slangp").writeText("shaders = 9\n")

        val saved = File(root, "My Presets/mine.slangp")
        ShaderIndex.changeFiles(root, listOf(saved)) {
            write(root, "My Presets/mine.slangp", "#reference \"../pack/crt/a.slangp\"\n")
        }
        assertTrue(ShaderIndex.indexFile(root).isFile)
        val after = passes(ShaderIndex.load(root))
        assertEquals(3, after["My Presets/mine.slangp"])
        assertEquals(5, after["pack/crt/b.slangp"])

        ShaderIndex.changeFiles(root, listOf(saved)) { saved.delete() }
        assertFalse("My Presets/mine.slangp" in passes(ShaderIndex.load(root)))
    }

    @Test
    fun anIndexWrittenForAnotherFolderIsIgnored() {
        val first = root().also(::seed)
        ShaderIndex.load(first)
        val moved = File(tmp.root, "moved/shaders")
        first.copyRecursively(moved)
        ShaderIndex.indexFile(first).copyTo(ShaderIndex.indexFile(moved))
        File(moved, "pack/crt/a.slangp").writeText("shaders = 9\n")
        assertEquals(9, passes(ShaderIndex.load(moved))["pack/crt/a.slangp"])
    }

    @Test
    fun aCorruptIndexIsRebuilt() {
        val root = root().also(::seed)
        ShaderIndex.load(root)
        val index = ShaderIndex.indexFile(root)
        index.writeText(index.readText().lines().take(3).joinToString("\n") + "\ngarbage-without-a-tab\n")
        assertEquals(4, ShaderIndex.load(root).size)
    }
}
