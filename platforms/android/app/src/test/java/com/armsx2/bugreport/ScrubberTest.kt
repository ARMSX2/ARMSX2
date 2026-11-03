package com.armsx2.bugreport

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/** What a report must not carry: paths, host names, credentials. */
class ScrubberTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val root = "/storage/emulated/0/Android/data/com.armsx2/files"
    private val scrubber = Scrubber(listOf(root))

    @Test
    fun theDataRootBecomesDataPlaceholder() {
        assertEquals("loaded <data>/bios/scph.bin ok", scrubber.text("loaded $root/bios/scph.bin ok"))
        assertEquals("<data>", scrubber.text(root))
    }

    @Test
    fun aSecondSpellingOfTheSameFolderLosesItsVolume() {
        assertEquals("<data>/Android/data/com.armsx2/files/logs", scrubber.text("/sdcard/Android/data/com.armsx2/files/logs"))
        assertEquals("<data>/Android/data/com.armsx2/files/x", scrubber.text("/storage/self/primary/Android/data/com.armsx2/files/x"))
    }

    @Test
    fun anSdCardVolumeIsReducedToo() {
        assertEquals("<data>/roms/ps2/memcards", scrubber.text("/storage/9C33-6BBD/roms/ps2/memcards"))
    }

    @Test
    fun aGamePathBecomesItsFileName() {
        assertEquals("Booting Ratchet & Clank.iso now", scrubber.text("Booting /storage/emulated/0/ROMs/PS2/Ratchet & Clank.iso now"))
        assertEquals("Game.chd", scrubber.text("/storage/9C33-6BBD/roms/ps2/Game.chd"))
        assertEquals("Game.ISO", scrubber.text("/sdcard/Game.ISO"))
        assertEquals("path=Game.iso, size=1", scrubber.text("path=/storage/emulated/0/x/Game.iso, size=1"))
    }

    @Test
    fun aContentUriKeepsOnlyTheFileName() {
        val uri = "content://com.android.externalstorage.documents/tree/primary%3AROMs/document/primary%3AROMs%2FKatamari%20Damacy.iso"
        assertEquals("uri=content://<redacted>/Katamari Damacy.iso", scrubber.text("uri=$uri"))
    }

    @Test
    fun textWithNoPathsIsUntouched() {
        val line = "11-03 10:00:00.000  123  456 I STDOUT  : EE: 59.94 fps (100%) GS: 42%"
        assertEquals(line, scrubber.text(line))
        assertEquals("", scrubber.text(""))
    }

    @Test
    fun privatePackagePathsAreKeptBecauseTheyNameTheDriverPack() {
        val line = "/data/user/0/com.armsx2/files/drivers/turnip-axfl3/libvulkan_freedreno.so"
        assertEquals(line, scrubber.text(line))
    }

    @Test
    fun aPathInsideAWordIsNotMistakenForAVolume() {
        // The volume rules only fire at the start of a path.
        assertEquals("a/storage/x", scrubber.text("a/storage/x"))
        assertEquals("relative/storage/emulated/0/y", scrubber.text("relative/storage/emulated/0/y"))
    }

    @Test
    fun settingsLoseCredentialKeysAtAnyDepth() {
        val settings = JSONObject(
            """{"mtvu":true,"raToken":"abc","nested":{"Password":"x","upscale":2,"list":[{"loginName":"bob","ok":1}]},"romPath":"$root/bios/x.bin"}""",
        )
        val out = scrubber.settings(settings)
        assertTrue(out.getBoolean("mtvu"))
        assertFalse(out.has("raToken"))
        assertFalse(out.getJSONObject("nested").has("Password"))
        assertEquals(2, out.getJSONObject("nested").getInt("upscale"))
        assertFalse(out.getJSONObject("nested").getJSONArray("list").getJSONObject(0).has("loginName"))
        assertEquals(1, out.getJSONObject("nested").getJSONArray("list").getJSONObject(0).getInt("ok"))
        assertEquals("<data>/bios/x.bin", out.getString("romPath"))
    }

    @Test
    fun theOriginalSettingsAreNotChanged() {
        val settings = JSONObject("""{"token":"abc"}""")
        scrubber.settings(settings)
        assertTrue(settings.has("token"))
    }

    @Test
    fun theHostNameIsDroppedFromTheDriverReport() {
        val report = """{"game":{"dump":"$root/snaps/x"},"device":{"model":"Pocket 5","uname":{"sysname":"Linux","nodename":"brians-phone","release":"5.4"}}}"""
        val out = JSONObject(scrubber.driverReport(report))
        assertFalse(out.getJSONObject("device").getJSONObject("uname").has("nodename"))
        assertEquals("Linux", out.getJSONObject("device").getJSONObject("uname").getString("sysname"))
        assertEquals("Pocket 5", out.getJSONObject("device").getString("model"))
        assertEquals("<data>/snaps/x", out.getJSONObject("game").getString("dump"))
    }

    @Test
    fun aDriverReportThatIsNotJsonStillLosesTheHostName() {
        val broken = """{"uname":{"nodename":"brians-phone","release":"5.4"}, "x": "$root/a"  """
        val out = scrubber.driverReport(broken)
        assertFalse(out.contains("brians-phone"))
        assertFalse(out.contains(root))
    }

    @Test
    fun copyTextScrubsEveryLineAndKeepsTheEndOfALongLog() {
        val lines = (1..100).map { "line $it $root/file$it" }
        val text = lines.joinToString("\n") + "\n"
        val out = ByteArrayOutputStream()
        val written = scrubber.copyText(ByteArrayInputStream(text.toByteArray()), text.length.toLong(), out)
        val result = out.toString(Charsets.UTF_8)
        assertEquals(result.length.toLong(), written)
        assertTrue(result.startsWith("line 1 <data>/file1\n"))
        assertTrue(result.trimEnd().endsWith("line 100 <data>/file100"))

        // Cap at 1 KiB: the first lines go, the last stay, and a note says so.
        val capped = ByteArrayOutputStream()
        scrubber.copyText(ByteArrayInputStream(text.toByteArray()), text.length.toLong(), capped, maxBytes = 1024)
        val cappedText = capped.toString(Charsets.UTF_8)
        assertTrue(cappedText.contains("were left out"))
        assertTrue(cappedText.trimEnd().endsWith("line 100 <data>/file100"))
        assertFalse(cappedText.contains("line 1 "))
    }

    @Test
    fun aGsBundleKeepsItsDumpByteForByteAndLosesTheHostNameInItsReport() {
        val dump = ByteArray(5000) { (it * 31).toByte() }
        val png = ByteArray(300) { (it + 7).toByte() }
        val source = tmp.newFile("in.gs.zip")
        ZipOutputStream(source.outputStream()).use { zip ->
            fun stored(name: String, bytes: ByteArray) {
                val checksum = java.util.zip.CRC32().also { it.update(bytes) }.value
                zip.putNextEntry(
                    ZipEntry(name).apply { method = ZipEntry.STORED; size = bytes.size.toLong(); compressedSize = bytes.size.toLong(); crc = checksum },
                )
                zip.write(bytes)
                zip.closeEntry()
            }
            stored("dump_1.gs.zst", dump)
            stored("dump_1.png", png)
            zip.putNextEntry(ZipEntry("dump_1.driver.json"))
            zip.write("""{"device":{"uname":{"nodename":"brians-phone"}},"p":"$root/x"}""".toByteArray())
            zip.closeEntry()
        }
        val dest = tmp.newFile("out.gs.zip")
        val report = scrubber.rewriteGsBundle(source, dest)

        assertNotNull(report)
        assertFalse(report!!.contains("brians-phone"))
        ZipFile(dest).use { zip ->
            assertEquals(3, zip.size())
            assertTrue(dump.contentEquals(zip.getInputStream(zip.getEntry("dump_1.gs.zst")).readBytes()))
            assertTrue(png.contentEquals(zip.getInputStream(zip.getEntry("dump_1.png")).readBytes()))
            assertEquals(ZipEntry.STORED, zip.getEntry("dump_1.gs.zst").method)
            val json = zip.getInputStream(zip.getEntry("dump_1.driver.json")).readBytes().toString(Charsets.UTF_8)
            assertFalse(json.contains("brians-phone"))
            assertFalse(json.contains(root))
        }
    }

    @Test
    fun aGsBundleWithNoReportReturnsNull() {
        val source = tmp.newFile("nr.gs.zip")
        ZipOutputStream(source.outputStream()).use { zip ->
            zip.putNextEntry(ZipEntry("a.png"))
            zip.write(byteArrayOf(1, 2, 3))
            zip.closeEntry()
        }
        assertNull(scrubber.rewriteGsBundle(source, tmp.newFile("nr-out.gs.zip")))
    }

    @Test
    fun sha256IsTheRealOne() {
        val f = tmp.newFile("hash.bin")
        f.writeText("abc")
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Scrubber.sha256(f))
    }
}
