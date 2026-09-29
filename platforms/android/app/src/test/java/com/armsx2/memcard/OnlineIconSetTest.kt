package com.armsx2.memcard

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.InputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class OnlineIconSetTest {
    @get:Rule val tmp = TemporaryFolder()

    // The app decodes each entry as a zstd frame through JNI; here the entries are stored plain
    // and read back as they are, which is everything but the decoding.
    private val plain: (InputStream, Long) -> ByteArray = { input, _ -> input.use { it.readBytes() } }

    private fun zip(vararg entries: Pair<String, ByteArray>): File {
        val f = tmp.newFile()
        ZipOutputStream(f.outputStream()).use { z ->
            for ((name, bytes) in entries) {
                z.putNextEntry(ZipEntry(name))
                z.write(bytes)
                z.closeEntry()
            }
        }
        return f
    }

    @Test
    fun readsTheIndexTheCatalogAndIcons() {
        val icon = ByteArray(1200) { it.toByte() }
        val set = OnlineIconSet.open(
            zip(
                "memcard-icons/index.txt.zst" to "# ARMSX2 memory card icons\nslus-20312 0a1b2c3d4e5f6a7b\nSCES-50760 1111222233334444\n".toByteArray(),
                "memcard-icons/catalog.txt.zst" to "0a1b2c3d4e5f6a7b\tFinal Fantasy X\tSave Data\tCajas, Issung\n1111222233334444\tIco\t\t\n".toByteArray(),
                "memcard-icons/icons/0a1b2c3d4e5f6a7b.zst" to icon,
            ),
            plain,
        )
        assertNotNull(set)
        set!!.use {
            assertEquals("0a1b2c3d4e5f6a7b", it.index["SLUS-20312"]) // serials are kept upper case
            assertEquals("1111222233334444", it.index["SCES-50760"])
            assertEquals(2, it.index.size)
            val ffx = it.catalog.first { e -> e.hash == "0a1b2c3d4e5f6a7b" }
            assertEquals("Final Fantasy X", ffx.title)
            assertEquals("Save Data", ffx.label)
            assertEquals("Cajas, Issung", ffx.contributors)
            assertEquals("", it.catalog.first { e -> e.title == "Ico" }.label)
            assertArrayEquals(icon, it.read("0a1b2c3d4e5f6a7b"))
            assertNull(it.read("1111222233334444")) // in the index, not in the zip
        }
    }

    @Test
    fun aZipWithoutAnIndexIsNotASet() {
        assertNull(OnlineIconSet.open(zip("memcard-icons/icons/a.zst" to ByteArray(4)), plain))
        assertNull(OnlineIconSet.open(zip("memcard-icons/index.txt.zst" to "# nothing but comments\n".toByteArray()), plain))
    }

    @Test
    fun aFileThatIsNotAZipIsNotASet() {
        val f = tmp.newFile().apply { writeText("<html>not found</html>") }
        assertNull(OnlineIconSet.open(f, plain))
    }
}
