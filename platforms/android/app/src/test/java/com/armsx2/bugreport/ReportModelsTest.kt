package com.armsx2.bugreport

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The kinds, the file names, the summary the server stores, and the consent list. */
class ReportModelsTest {
    @Test
    fun heavyItemDefaultsFollowTheDesignTable() {
        assertTrue(ReportKind.GRAPHICS.gsDumpByDefault)
        assertFalse(ReportKind.GRAPHICS.savestateByDefault)
        assertTrue(ReportKind.SLOW.gsDumpByDefault)
        assertTrue(ReportKind.SLOW.savestateByDefault)
        assertFalse(ReportKind.OTHER.gsDumpByDefault)
        assertFalse(ReportKind.OTHER.savestateByDefault)
        assertFalse(ReportKind.CRASH.gsDumpByDefault)
        assertFalse(ReportKind.CRASH.savestateByDefault)
    }

    @Test
    fun kindsRoundTripThroughTheirServerSpelling() {
        for (k in ReportKind.entries) assertEquals(k, ReportKind.fromWire(k.wire))
        assertEquals(ReportKind.OTHER, ReportKind.fromWire("nonsense"))
    }

    @Test
    fun everyFileNameIsOneTheServerAccepts() {
        for (name in ReportFiles.ALL) assertTrue(name, ReportFiles.isValidName(name))
        assertFalse(ReportFiles.isValidName("Report.json"))
        assertFalse(ReportFiles.isValidName("../x"))
        assertFalse(ReportFiles.isValidName(".hidden"))
        assertFalse(ReportFiles.isValidName("a".repeat(65)))
        // The server writes manifest.json itself when a report is finished.
        assertFalse(ReportFiles.isValidName("manifest.json"))
        assertTrue(ReportFiles.ALL.size <= ReportLimits.MAX_FILES)
    }

    @Test
    fun theSummaryFitsTheServersLimitEvenWithAFullWidthDescription() {
        val long = "あ".repeat(ReportLimits.MAX_DESCRIPTION)
        val summary = ReportJson.summary(long, ReportGame("SCUS-97265", "ABCD1234", "あ".repeat(500), "k"), "d".repeat(500), "s", "g", "x".repeat(500), "os")
        assertTrue(summary.toString().toByteArray(Charsets.UTF_8).size < 8 * 1024)
        assertEquals(128, summary.getString("title").length)
        assertEquals("SCUS-97265", summary.getString("serial"))
    }

    @Test
    fun aShortSummaryIsKeptWhole() {
        val summary = ReportJson.summary("it flickers", null, "Pocket 5", "SM8250", "Adreno 650", "system", "Android 14 (SDK 34)")
        assertEquals("it flickers", summary.getString("description"))
        assertEquals("", summary.getString("serial"))
        assertEquals("system", summary.getString("driver"))
    }

    @Test
    fun theCreateRequestCarriesTheContractFields() {
        val request = ReportJson.createRequest(ReportKind.CRASH, "2.6.9", "05f55b8e19", "play", JSONObject().put("description", "x"))
        assertEquals(1, request.getInt("schema"))
        assertEquals("crash", request.getString("kind"))
        assertEquals("android", request.getString("platform"))
        assertEquals("05f55b8e19", request.getJSONObject("app").getString("commit"))
        assertEquals("play", request.getJSONObject("app").getString("flavor"))
    }

    @Test
    fun theConsentListShowsHeavyItemsOnlyWhenTheyCanBeTakenAndAreOn() {
        val light = listOf(BundleItem(ReportFiles.EMULOG, 1000), BundleItem(ReportFiles.SYSTEM, 500))
        val names = { items: List<BundleItem> -> items.map { it.name } }

        assertEquals(
            listOf(ReportFiles.REPORT, ReportFiles.SYSTEM, ReportFiles.EMULOG),
            names(ConsentItems.build(light, canCapture = false, savestate = true, gsDump = true)),
        )
        assertEquals(
            listOf(ReportFiles.REPORT, ReportFiles.SYSTEM, ReportFiles.EMULOG, ReportFiles.GS_DUMP),
            names(ConsentItems.build(light, canCapture = true, savestate = false, gsDump = true)),
        )
        val all = ConsentItems.build(light, canCapture = true, savestate = true, gsDump = true)
        assertEquals(listOf(ReportFiles.REPORT, ReportFiles.SYSTEM, ReportFiles.EMULOG, ReportFiles.GS_DUMP, ReportFiles.SAVESTATE), names(all))
        assertTrue(all.first { it.name == ReportFiles.GS_DUMP }.estimate)
        assertFalse(all.first { it.name == ReportFiles.EMULOG }.estimate)
    }

    @Test
    fun theMeteredNoteThresholdIsTwentyFiveMegabytes() {
        assertEquals(25L * 1024 * 1024, ReportLimits.METERED_NOTE_BYTES)
    }

    @Test
    fun exitReasonsAreNamed() {
        assertEquals("CRASH_NATIVE", ExitInfo.reasonName(5))
        assertEquals("ANR", ExitInfo.reasonName(6))
        assertEquals("REASON_99", ExitInfo.reasonName(99))
    }

    @Test
    fun theCrashMarkerRoundTrips() {
        val marker = CrashMarker.Marker(
            1_762_000_000_000L, 4242, "SCUS-97265", "Ratchet & Clank", "turnip-axfl3", "turnip axfl3-1",
            foreground = false, bootCount = 17,
        )
        assertEquals(marker, CrashMarker.Marker.fromJson(JSONObject(marker.toJson().toString())))
    }

    @Test
    fun aMarkerFromBeforeTheForegroundFlagReadsAsForegroundOfAnUnknownBoot() {
        val old = JSONObject("""{"armedAt":5,"pid":9,"serial":"S","title":"T","driverId":"","driverName":""}""")
        val marker = CrashMarker.Marker.fromJson(old)
        assertTrue(marker.foreground)
        assertEquals(-1, marker.bootCount)
    }
}
