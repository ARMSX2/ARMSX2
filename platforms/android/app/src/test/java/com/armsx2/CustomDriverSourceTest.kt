package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CustomDriverSourceTest {
    // Every Mali of arch 7, 9, 10 and 11, as malisx2 0.5.0 accepts them. v9: G57, G68, G77, G78
    // (G78AE included). v10: G310, G510, G610, G710. v11: G615, G715 (Immortalis-G715 included).
    // Bifrost v7: G31, G51, G52, G76. Each part is listed bare, with MC<n>, and with MP<n> where
    // Arm's blobs use it. The app reads the GL_RENDERER string
    // (GpuInfo), so these are GL strings; the bare and MC<n> forms are also what malisx2 reports
    // as its Vulkan deviceName.
    private val valhallV9 = listOf(
        "Mali-G57", "Mali-G57 MC2", "Mali-G57 MC3", "Mali-G57 MC4", "Mali-G57 MC6",
        "Mali-G68", "Mali-G68 MC4",
        "Mali-G77 MC7", "Mali-G77 MC9",
        "Mali-G78", "Mali-G78 MC14", "Mali-G78 MP14", "Mali-G78 MP20",
        "Mali-G78AE", "Mali-G78AE MC10", "Mali-G78AE MC20",
    )
    private val valhallV10 = listOf(
        "Mali-G310", "Mali-G310 MC1", "Mali-G310 MC2",
        "Mali-G510", "Mali-G510 MC2", "Mali-G510 MC4",
        "Mali-G610", "Mali-G610 MC4", "Mali-G610 MC6",
        "Mali-G710", "Mali-G710 MC10", "Mali-G710 MP7",
    )
    private val valhallV11 = listOf(
        "Mali-G615", "Mali-G615 MC6", "Mali-G615 MC2",
        "Mali-G715", "Mali-G715 MC7", "Mali-G715-Immortalis MC11",
        "Immortalis-G715 MC11",
    )
    private val bifrostV7 = listOf(
        "Mali-G31", "Mali-G31 MP2",
        "Mali-G51", "Mali-G51 MP4",
        "Mali-G52", "Mali-G52 MC2", "Mali-G52 MC3", "Mali-G52 MP2",
        "Mali-G76", "Mali-G76 MC4",
    )

    // Everything that must not list malisx2: the arch 6 Bifrost parts (G71, G72, which sit a digit
    // away from G710/G715 and G720), Midgard and Utgard, the 5th-gen parts (G620, G720, G625, G725,
    // G925, G1), and non-Mali GPUs.
    private val notOffered = listOf(
        null, "", "Mali", "Immortalis",
        "Mali-G71", "Mali-G71 MP2", "Mali-G71 MP20", "Mali-G72", "Mali-G72 MP3", "Mali-G72 MP12",
        "Mali-T880 MP12", "Mali-T830 MP2", "Mali-T760 MP4", "Mali-T720", "Mali-450 MP", "Mali-400 MP",
        "Mali-G620 MC4", "Mali-G720-Immortalis MC12", "Mali-G625 MC6", "Mali-G725 MC11",
        "Mali-G925-Immortalis MC12", "Mali-G1-Ultra MC12",
        "Adreno (TM) 650", "Adreno (TM) 615", "PowerVR B-Series BXM-8-256",
    )

    @Test
    fun maliSX2ListsOnTheSupportedMalisOnly() {
        for (r in valhallV9 + valhallV10 + valhallV11 + bifrostV7)
            assertTrue(r, CustomDriver.isMaliSX2Supported(r))
        for (r in notOffered)
            assertFalse(r.toString(), CustomDriver.isMaliSX2Supported(r))
    }

    @Test
    fun supportedMatchIgnoresCase() {
        for (r in listOf("mali-g57 mc2", "MALI-G78AE MC20", "immortalis-g715 mc11", "mali-g76 mc4", "mali-g710 mc10",
                "mali-g310 mc1", "MALI-G510 MC2", "mali-g31 mp2"))
            assertTrue(r, CustomDriver.isMaliSX2Supported(r))
        for (r in listOf("mali-g71 mp20", "MALI-G720 MC7", "mali-t880 mp12"))
            assertFalse(r, CustomDriver.isMaliSX2Supported(r))
    }

    @Test
    fun theListOffersMaliSX2OnTheSupportedMalis() {
        // The wrong-driver notice shares the predicate, so it covers exactly the same parts.
        for (r in valhallV9 + valhallV10 + valhallV11 + bifrostV7)
            assertTrue(r, CustomDriver.offersMaliSX2(r))
        // Telling these users to download a driver that is not listed for them would be wrong.
        for (r in notOffered)
            assertFalse(r.toString(), CustomDriver.offersMaliSX2(r))
    }

    // Adreno 6xx and 7xx, as GL_RENDERER spells them. 8xx is out: Turnip's a8xx support is young.
    private val adreno6xxAnd7xx = listOf(
        "Adreno (TM) 610", "Adreno (TM) 650", "Adreno (TM) 662", "Adreno (TM) 690",
        "Adreno (TM) 702", "Adreno (TM) 730", "Adreno (TM) 740", "Adreno (TM) 750", "adreno (tm) 740",
        // The Anbernic RG 55G1's Adreno 613, as Qualcomm's GL driver names it.
        "Adreno (TM) A12", "adreno (tm) a12",
    )
    private val notAdrenoTurnipTarget = listOf(
        null, "", "Adreno", "Adreno (TM) 506", "Adreno (TM) 540", "Adreno (TM) 830", "Adreno (TM) 840",
        "Adreno (TM) 6500", "Mali-G715", "PowerVR B-Series BXM-8-256", "Xclipse 940",
        // Marketing names not identified on a device, and a bare 12 that is not one.
        "Adreno (TM) A32", "Adreno (TM) A11", "Adreno (TM) 12",
    )

    @Test
    fun theA12MarketingNameReadsAsTheAdreno613() {
        assertEquals("Adreno (TM) 613", CustomDriver.withAdrenoModelNumber("Adreno (TM) A12"))
        assertEquals("Adreno 613 MP2", CustomDriver.withAdrenoModelNumber("Adreno A12 MP2"))
        for (r in listOf(null, "", "Adreno (TM) 650", "Adreno (TM) A32", "Mali-G715", "Adreno (TM) A120"))
            assertEquals(r.toString(), r, CustomDriver.withAdrenoModelNumber(r))
    }

    @Test
    fun turnipTargetIsAdreno6xxAnd7xx() {
        for (r in adreno6xxAnd7xx)
            assertTrue(r, CustomDriver.isAdreno6xxOr7xx(r))
        for (r in notAdrenoTurnipTarget)
            assertFalse(r.toString(), CustomDriver.isAdreno6xxOr7xx(r))
    }

    @Test
    fun setupOffersOurDriverForTheGpuAndNothingElse() {
        for (r in valhallV9 + valhallV10 + valhallV11 + bifrostV7)
            assertEquals(r, "ARMSX2 · MaliSX2", CustomDriver.setupOfferLabel(r))
        for (r in adreno6xxAnd7xx)
            assertEquals(r, "ARMSX2 · turnip", CustomDriver.setupOfferLabel(r))
        val nothingOffered = listOf(
            null, "", "Adreno", "Adreno (TM) 506", "Adreno (TM) 840", "Adreno (TM) 6500",
            "Mali-G72 MP3", "Mali-G720 MC7", "PowerVR B-Series BXM-8-256", "Xclipse 940",
        )
        for (r in nothingOffered)
            assertNull(r.toString(), CustomDriver.setupOfferLabel(r))
    }

    // ---- Which sources the download list may show, by GPU family --------------------------

    // Every source, in display order (DRIVER_SOURCES). Updated together with that list.
    private val allSources = listOf(
        "ARMSX2 · MaliSX2",
        "ARMSX2 · turnip",
        "AdrenoToolsDrivers",
        "MrPurple · purple-turnip",
        "StevenMXZ · Adreno-Tools",
        "crueter · GameHub 8Elite",
        "PojavLauncherTeam · freedreno (A10)",
        "WearyConcern1165 · ExynosTools",
        "avavo · MdiEx",
    )
    // Every Adreno source: our Turnip and the five third-party Turnip / Adreno Tools repos.
    private val adrenoSources = listOf(
        "ARMSX2 · turnip",
        "AdrenoToolsDrivers",
        "MrPurple · purple-turnip",
        "StevenMXZ · Adreno-Tools",
        "crueter · GameHub 8Elite",
        "PojavLauncherTeam · freedreno (A10)",
    )

    @Test
    fun maliIsOfferedMaliSX2AndNoAdrenoPack() {
        for (r in valhallV9 + valhallV10 + valhallV11 + bifrostV7)
            assertEquals(r, listOf("ARMSX2 · MaliSX2"), CustomDriver.sourceLabelsFor(r))
    }

    @Test
    fun aMaliWithoutAMalisx2PartIsOfferedNothing() {
        // The Turnip and Adreno Tools packs load on Qualcomm's driver only, and the Xclipse packs on
        // Samsung's: none of them is worth a download here, so the system driver is the answer.
        for (r in listOf("Mali", "Immortalis", "Mali-G71 MP20", "Mali-G72 MP12", "Mali-G720 MC7", "Mali-G1-Ultra MC12", "Mali-T880 MP12"))
            assertEquals(r, emptyList<String>(), CustomDriver.sourceLabelsFor(r))
    }

    @Test
    fun adrenoIsOfferedTheTurnipSourcesAndNothingElse() {
        // No MaliSX2 on Adreno, and no Xclipse pack: the reverse of the Mali rule.
        for (r in listOf(
            "Adreno (TM) 506", "Adreno (TM) 540", "Adreno (TM) 610", "Adreno (TM) 650",
            "Adreno (TM) 740", "Adreno (TM) 840", "Adreno (TM) A12", "Adreno",
        ))
            assertEquals(r, adrenoSources, CustomDriver.sourceLabelsFor(r))
    }

    @Test
    fun xclipseIsOfferedTheExynosSourcesOnly() {
        for (r in listOf("Xclipse 920", "Xclipse 940"))
            assertEquals(r, listOf("WearyConcern1165 · ExynosTools", "avavo · MdiEx"), CustomDriver.sourceLabelsFor(r))
    }

    @Test
    fun everySourceIsReachableFromItsOwnFamily() {
        // The three families together reach the whole list: a source whose family predicate matches
        // nothing (a typo, the wrong predicate) would drop out here, as would one added to the list
        // without a row above.
        val reachable = listOf("Mali-G715 MC7", "Adreno (TM) 740", "Xclipse 940")
            .flatMap { CustomDriver.sourceLabelsFor(it) }
        assertEquals(allSources.toSet(), reachable.toSet())
    }

    @Test
    fun anUnidentifiedGpuIsOfferedNothing() {
        // The GL probe can fail, and an unreadable renderer names no family: offering every source
        // would put Adreno packs in front of a Mali device and the reverse. Local import stays.
        assertEquals(emptyList<String>(), CustomDriver.sourceLabelsFor(null))
        assertEquals(emptyList<String>(), CustomDriver.sourceLabelsFor(""))
        // A renderer that names no family we build for matches no source either.
        assertEquals(emptyList<String>(), CustomDriver.sourceLabelsFor("PowerVR B-Series BXM-8-256"))
    }
}
