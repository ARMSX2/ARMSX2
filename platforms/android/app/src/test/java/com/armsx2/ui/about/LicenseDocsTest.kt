package com.armsx2.ui.about

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The About screen's license lists, read from the real files: PCSX2's ThirdPartyLicenses.html and
 * GPL.html (bin/docs, copied into the APK at build time) and the Android app's own android.html.
 * If PCSX2 changes the layout of its file, this is where it shows, not as an empty list on a phone.
 */
class LicenseDocsTest {
    private val docs = File("../../../bin/docs")
    private val android = File("src/main/assets/licenses/android.html")

    @Test
    fun everyPcsx2ComponentIsReadWithItsLicense() {
        val html = File(docs, "ThirdPartyLicenses.html").readText()
        val entries = LicenseDocs.parse(html)
        assertEquals(Regex("""<div id="""").findAll(html).count(), entries.size)

        val pcsx2 = entries.first()
        assertEquals("pcsx2", pcsx2.id)
        assertFalse(pcsx2.preformatted)
        assertTrue(pcsx2.text.contains("GNU General Public License"))

        val zlib = entries.single { it.id == "zlib" }
        assertEquals("zlib", zlib.name)
        assertEquals("https://github.com/madler/zlib", zlib.url)
        assertTrue(zlib.preformatted)
        assertTrue(zlib.text.contains("Copyright (C) 1995-2017 Jean-loup Gailly and Mark Adler"))
        // &#39; in the file is an apostrophe on screen.
        assertTrue(zlib.text.contains("'as-is'"))

        entries.forEach { entry ->
            assertTrue(entry.id, entry.name.isNotBlank() && entry.text.isNotBlank())
            assertFalse(entry.id, entry.name.contains("<") || entry.name.endsWith("-"))
            assertFalse(entry.id, Regex("""&(#[0-9]+|[a-z]+);""").containsMatchIn(entry.text))
            assertFalse(entry.id, entry.text.contains("<pre>") || entry.text.contains("<p>"))
        }
    }

    @Test
    fun theAndroidListEndsWithTheApacheLicense() {
        val entries = LicenseDocs.parse(android.readText())
        assertEquals(21, entries.size)
        assertEquals(entries.size, entries.map { it.id }.toSet().size)
        assertTrue(entries.all { it.preformatted && it.text.isNotBlank() })
        val apache = entries.last()
        assertEquals("Apache License 2.0", apache.name)
        assertTrue(apache.text.contains("TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION"))
        val webrtc = entries.single { it.id == "webrtc" }
        assertTrue(webrtc.text.startsWith("Copyright (c) 2011, The WebRTC project authors."))
    }

    @Test
    fun theGplReadsAsParagraphs() {
        val text = LicenseDocs.htmlToText(File(docs, "GPL.html").readText())
        assertTrue(text.startsWith("GNU GENERAL PUBLIC LICENSE\n\nVersion 3, 29 June 2007"))
        assertTrue(text.contains("Copyright © 2007 Free Software Foundation, Inc. <https://fsf.org/>"))
        assertTrue(text.contains("END OF TERMS AND CONDITIONS"))
        // No markup left. The license's own "<program>" and "<name of author>" placeholders stay.
        assertFalse(Regex("""</?(p|h[1-6]|li|ul|ol|pre|a)\b[^>]*>""").containsMatchIn(text))
        assertTrue(text.contains("<program>"))
        assertFalse(text.contains("&lt;") || text.contains("\n\n\n"))
    }
}
