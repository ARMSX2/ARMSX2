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
    fun theAppsPrivateFolderBecomesAppAndTheDriverPackKeepsItsId() {
        val dir = "/data/user/0/com.armsx2.reporttest/files/drivers/armsx2libmali-v0.3.2-malisx2-0.3.2-4d0ebb3d63/"
        assertEquals("<app>/files/drivers/armsx2libmali-v0.3.2-malisx2-0.3.2-4d0ebb3d63/", scrubber.text(dir))
        assertEquals("<app>/files/drivers/p/cache/", scrubber.text("/data/user/0/com.armsx2.reporttest/files/drivers/p/cache/"))
        assertEquals("<app>/files/x", scrubber.text("/data/data/com.armsx2/files/x"))
        assertEquals("<app>/cache", scrubber.text("/data/user_de/0/com.armsx2.nightly/cache"))
        assertEquals("opened <app>/files/lib.so ok", scrubber.text("opened /data/user/10/com.armsx2/files/lib.so ok"))
    }

    @Test
    fun theInstallFolderLosesItsRandomPartsOnBothAndroidLayouts() {
        assertEquals(
            "<app>/lib/arm64",
            scrubber.text("/data/app/~~S3j43didhb3PeO9tNa4ACQ==/com.armsx2.reporttest-aX7YEbFbOM7E04PLCVaMtw==/lib/arm64"),
        )
        // base64url alphabet: - and _ inside the random parts.
        assertEquals("<app>/base.apk", scrubber.text("/data/app/~~a-b_c==/com.armsx2-x_y-z==/base.apk"))
        assertEquals("<app>/base.apk", scrubber.text("/data/app/com.armsx2-1/base.apk"))
        assertEquals("dlopen <app>/lib/arm64/libemucore.so failed", scrubber.text("dlopen /data/app/~~Zz9==/com.armsx2-Qq1==/lib/arm64/libemucore.so failed"))
    }

    @Test
    fun theDriverStatusPathsInReportJsonAreScrubbed() {
        val status = JSONObject(
            """{"requested":true,"opened":true,"required":false,
            "dir":"/data/user/0/com.armsx2.reporttest/files/drivers/pack-1/","name":"libvulkan_x.so",
            "redirect_dir":"/data/user/0/com.armsx2.reporttest/files/drivers/pack-1/cache/",
            "hook_lib_dir":"/data/app/~~S3j43didhb3PeO9tNa4ACQ==/com.armsx2.reporttest-aX7YEbFbOM7E04PLCVaMtw==/lib/arm64","failure":""}""",
        )
        val out = scrubber.json(status) as JSONObject
        assertEquals("<app>/files/drivers/pack-1/", out.getString("dir"))
        assertEquals("<app>/files/drivers/pack-1/cache/", out.getString("redirect_dir"))
        assertEquals("<app>/lib/arm64", out.getString("hook_lib_dir"))
        assertEquals("libvulkan_x.so", out.getString("name"))
        assertFalse(out.toString().contains("S3j43"))
        assertFalse(out.toString().contains("/data/user"))
    }

    @Test
    fun aDriverReportsPackPathsAreScrubbedToo() {
        val report = """{"selected_driver":{"pack_path":"/data/user/0/com.armsx2/files/drivers/p/","driver_file":{"path":"/data/user/0/com.armsx2/files/drivers/p/lib.so"}}}"""
        val out = scrubber.driverReport(report)
        assertFalse(out.contains("/data/user"))
        assertTrue(out.contains("<app>/files/drivers/p/lib.so"))
    }

    @Test
    fun otherDataPathsAreNotTakenForTheApp() {
        // Not an application id (no dot), and not under /data at its root.
        assertEquals("/data/user/0/files/x", scrubber.text("/data/user/0/files/x"))
        assertEquals("/data/local/tmp/x", scrubber.text("/data/local/tmp/x"))
        assertEquals("/data/app/something/x", scrubber.text("/data/app/something/x"))
        // The shared-storage spelling of the app folder is the data root's job, and already is.
        assertEquals("<data>/Android/data/com.armsx2/files/logs", scrubber.text("/sdcard/Android/data/com.armsx2/files/logs"))
        assertEquals("x/data/user/0/com.armsx2/y", scrubber.text("x/data/user/0/com.armsx2/y"))
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
    fun aPrivatePackagePathKeepsTheDriverPackItNamesAndLosesThePackage() {
        // The pack id is what a developer needs from this path; the package and user number are not.
        val line = "/data/user/0/com.armsx2/files/drivers/turnip-axfl3/libvulkan_freedreno.so"
        assertEquals("<app>/files/drivers/turnip-axfl3/libvulkan_freedreno.so", scrubber.text(line))
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
    fun aFileUriLosesItsFoldersAndKeepsTheGameName() {
        assertEquals("file://<redacted>/Jak.iso", scrubber.text("file:///storage/emulated/0/Games/Jak.iso"))
        assertEquals("opening file://<redacted>/Katamari Damacy.iso now", scrubber.text("opening file:///storage/9C33-6BBD/roms/Katamari%20Damacy.iso now"))
        assertEquals("file://<data>/logs/emulog.txt", scrubber.text("file://$root/logs/emulog.txt"))
        assertEquals("file://<data>/Android/data/com.armsx2/files/x", scrubber.text("file:///sdcard/Android/data/com.armsx2/files/x"))
    }

    @Test
    fun aContentUriStillKeepsOnlyTheFileName() {
        val uri = "content://com.android.externalstorage.documents/document/primary%3AGames%2FJak.iso"
        assertEquals("content://<redacted>/Jak.iso", scrubber.text(uri))
    }

    @Test
    fun theUsernameIsReplacedWhereverItAppears() {
        val named = Scrubber(listOf(root), listOf("BobTheGamer"))
        assertEquals(
            "Achievements: Attempting login with user '<user>'...",
            named.text("Achievements: Attempting login with user 'BobTheGamer'..."),
        )
        assertEquals("badge user_<user>.png", named.text("badge user_bobthegamer.png"))
        // A name inside a longer word is a different word.
        assertEquals("BobTheGamerPro is fine", named.text("BobTheGamerPro is fine"))
    }

    @Test
    fun theLoginLogLinesAreReducedEvenForANameNobodyToldUsAbout() {
        assertEquals(
            "Achievements: Attempting login with user '<user>'...",
            scrubber.text("Achievements: Attempting login with user 'someone_else'..."),
        )
        assertEquals("RCHEEVOS: <user> logged in successfully", scrubber.text("RCHEEVOS: Some1 logged in successfully"))
        assertEquals("Attempting to log in <user> (with token)", scrubber.text("Attempting to log in Some1 (with token)"))
        assertEquals("Attempting to log in <user> (with password)", scrubber.text("Attempting to log in Some1 (with password)"))
        assertEquals("Logging <user> out", scrubber.text("Logging Some1 out"))
    }

    @Test
    fun aVeryShortUsernameDoesNotEatTheLog() {
        val named = Scrubber(listOf(root), listOf("GS"))
        assertEquals("GS: 42% busy", named.text("GS: 42% busy"))
        assertEquals("<user> logged in successfully", named.text("GS logged in successfully"))
    }

    @Test
    fun iniUsernamesAreReadFromTheAchievementsSectionOnly() {
        val ini = "[EmuCore]\nUsername = nobody\n[Achievements]\nEnabled = true\nUsername = BobTheGamer\nToken = x\n[Other]\nUsername = other\n"
        assertEquals(listOf("BobTheGamer"), AccountNames.fromIni(ini))
        assertEquals(emptyList<String>(), AccountNames.fromIni("[Achievements]\nUsername =\n"))
    }

    @Test
    fun settingsDropTheRoomCodeAndMicrophoneNames() {
        val settings = JSONObject(
            """{"localLinkRoomCode":"ABCD","micDevice1":"Pixel Buds of Brian","micDevice2":"","mtvu":true}""",
        )
        val out = scrubber.settings(settings)
        assertFalse(out.has("localLinkRoomCode"))
        assertFalse(out.has("micDevice1"))
        assertFalse(out.has("micDevice2"))
        assertTrue(out.getBoolean("mtvu"))
    }

    @Test
    fun settingsBlankNetworkAddressesButKeepTheUnset() {
        val settings = JSONObject(
            """{"localLinkAddress":"192.168.1.20","dev9Ps2Ip":"192.168.1.50","dev9Mask":"255.255.255.0","dev9Gateway":"192.168.1.1",""" +
                """"dev9Dns1":"8.8.8.8","dev9Dns2":"0.0.0.0","localLinkPort":19072,""" +
                """"dev9EthHosts":[{"name":"My server","url":"my.private.server","ip":"10.0.0.7","enabled":true}]}""",
        )
        val out = scrubber.settings(settings)
        assertEquals("<redacted>", out.getString("localLinkAddress"))
        assertEquals("<redacted>", out.getString("dev9Ps2Ip"))
        assertEquals("<redacted>", out.getString("dev9Mask"))
        assertEquals("<redacted>", out.getString("dev9Gateway"))
        assertEquals("<redacted>", out.getString("dev9Dns1"))
        assertEquals("0.0.0.0", out.getString("dev9Dns2"))
        assertEquals(19072, out.getInt("localLinkPort"))
        val host = out.getJSONArray("dev9EthHosts").getJSONObject(0)
        assertEquals("<redacted>", host.getString("name"))
        assertEquals("<redacted>", host.getString("url"))
        assertEquals("<redacted>", host.getString("ip"))
        assertTrue(host.getBoolean("enabled"))
        assertFalse(out.toString().contains("192.168"))
        assertFalse(out.toString().contains("my.private.server"))
        assertFalse(out.toString().contains("My server"))
    }

    @Test
    fun theOriginalSettingsAreNotChangedByBlanking() {
        val settings = JSONObject("""{"localLinkAddress":"192.168.1.20","micDevice1":"x"}""")
        scrubber.settings(settings)
        assertEquals("192.168.1.20", settings.getString("localLinkAddress"))
        assertTrue(settings.has("micDevice1"))
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
