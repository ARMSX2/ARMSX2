package com.armsx2.memcard

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class OnlineIconStoreTest {
    @get:Rule val tmp = TemporaryFolder()

    // The app decodes each file as a zstd frame through JNI; here the files are plain and read
    // back as they are, which is everything but the decoding.
    private val plain: (InputStream, Long) -> ByteArray = { input, _ -> input.use { it.readBytes() } }

    private fun store() = OnlineIconStore(tmp.newFolder(), plain)

    /** An icon's bytes: an icon.sys ("PS2D" and 960 more bytes), then [body]. */
    private fun icon(body: Int): ByteArray = ByteArray(964 + 100) { i -> if (i < 4) "PS2D"[i].code.toByte() else (i * 7 + body).toByte() }

    private fun hashOf(b: ByteArray) = MessageDigest.getInstance("SHA-1").digest(b).joinToString("") { "%02x".format(it) }.take(16)

    @Test
    fun readsTheIndexAndTheCatalog() {
        val s = store()
        assertTrue(s.putList(OnlineIconStore.INDEX, "# icons\nslus-20312 0a1b2c3d4e5f6a7b\nSCES-50760 1111222233334444\n".toByteArray()))
        assertTrue(s.putList(OnlineIconStore.CATALOG, "1111222233334444\tIco\t\t\n0a1b2c3d4e5f6a7b\tFinal Fantasy X\tSave Data\tCajas, Issung\nnot-a-hash\tJunk\n".toByteArray()))
        assertEquals("0a1b2c3d4e5f6a7b", s.hashFor("SLUS-20312"))
        assertEquals("1111222233334444", s.hashFor("sces-50760"))
        assertNull(s.hashFor("SLUS-99999"))
        val catalog = s.catalog()
        assertEquals(listOf("Final Fantasy X", "Ico"), catalog.map { it.title }) // by title; junk dropped
        assertEquals("Save Data", catalog[0].label)
        assertEquals("Cajas, Issung", catalog[0].contributors)
        assertFalse(s.putList("elsewhere.txt", ByteArray(4)))
    }

    @Test
    fun keepsAnIconOnlyWhenItIsWhatItsHashSays() {
        val s = store()
        val good = icon(1)
        val hash = hashOf(good)
        assertFalse(s.install(hashOf(icon(2)), good)) // another icon's hash
        assertFalse(s.install(hash, good.copyOf(500))) // cut short: no longer its hash
        assertFalse(s.isInstalled(hash))
        assertTrue(s.install(hash, good))
        assertTrue(s.isInstalled(hash))
        assertArrayEquals(good, s.read(hash))
        assertEquals(setOf(hash), s.installedHashes())
        val notAnIcon = ByteArray(2000) { 1 }
        assertFalse(s.install(hashOf(notAnIcon), notAnIcon)) // no icon.sys
    }

    @Test
    fun removesOneOrAll() {
        val s = store()
        val a = icon(1)
        val b = icon(2)
        assertTrue(s.install(hashOf(a), a))
        assertTrue(s.install(hashOf(b), b))
        s.uninstall(hashOf(a))
        assertEquals(setOf(hashOf(b)), s.installedHashes())
        s.uninstallAll()
        assertTrue(s.installedHashes().isEmpty())
        assertEquals(0L, s.installedBytes())
    }

    @Test
    fun installsThePublishedZipAsItStreams() {
        val a = icon(1)
        val b = icon(2)
        val bytes = ByteArrayOutputStream()
        ZipOutputStream(bytes).use { z ->
            fun put(name: String, data: ByteArray) { z.putNextEntry(ZipEntry(name)); z.write(data); z.closeEntry() }
            put("memcard-icons/index.txt.zst", "SLUS-20312 ${hashOf(a)}\n".toByteArray())
            put("memcard-icons/catalog.txt.zst", "${hashOf(a)}\tFinal Fantasy X\tSave Data\tCajas\n".toByteArray())
            put("memcard-icons/icons/${hashOf(a)}.zst", a)
            put("memcard-icons/icons/${hashOf(b)}.zst", b)
            put("memcard-icons/icons/0000000000000000.zst", b) // wrong name: refused
        }
        val s = store()
        assertEquals(2, s.installFromZip(bytes.toByteArray().inputStream()) { true })
        assertEquals(setOf(hashOf(a), hashOf(b)), s.installedHashes())
        assertEquals(hashOf(a), s.hashFor("SLUS-20312"))
        assertEquals("Final Fantasy X", s.catalog().single().title)
    }
}
