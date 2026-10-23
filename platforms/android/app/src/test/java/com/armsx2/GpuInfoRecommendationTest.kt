package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GpuInfoRecommendationTest {
    // The driver settings page recommends our Turnip on every Adreno 6xx and 7xx, the range the first-run
    // setup offers it on (CustomDriver.isAdreno6xxOr7xx), so a player is told the same thing in both places.
    @Test
    fun ourTurnipIsRecommendedOnAdreno6xxAnd7xx() {
        for (r in listOf(
            "Adreno (TM) 610", "Adreno (TM) 618", "Adreno (TM) 650", "Adreno (TM) 660", "Adreno (TM) 690",
            "Adreno (TM) 702", "Adreno (TM) 730", "Adreno (TM) 740", "Adreno (TM) 750",
        )) {
            assertEquals(r, "ARMSX2 Turnip", GpuInfo.recommendation(r)?.sourceLabel)
            assertTrue(r, CustomDriver.isAdreno6xxOr7xx(r))
        }
    }

    // The Anbernic RG 55G1's Adreno 613, which Qualcomm's GL driver names by its marketing name.
    @Test
    fun theAdrenoA12IsRecommendedOurTurnipAsTheAdreno613() {
        assertEquals("ARMSX2 Turnip", GpuInfo.recommendation("Adreno (TM) A12")?.sourceLabel)
    }

    // Marketing names nobody has identified on a device are not guessed at.
    @Test
    fun anUnlistedMarketingNameStaysOnTheGeneralPack() {
        assertEquals("KIMCHI", GpuInfo.recommendation("Adreno (TM) A32")?.sourceLabel)
    }

    @Test
    fun adreno8xxKeepsTheQualcommTunedPack() {
        for (r in listOf("Adreno (TM) 825", "Adreno (TM) 830", "Adreno (TM) 840"))
            assertEquals(r, "GameHub 8Elite", GpuInfo.recommendation(r)?.sourceLabel)
    }

    @Test
    fun olderAdrenoStaysOnTheGeneralPack() {
        assertEquals("KIMCHI", GpuInfo.recommendation("Adreno (TM) 540")?.sourceLabel)
        assertEquals("KIMCHI", GpuInfo.recommendation("Adreno (TM) 506")?.sourceLabel)
    }

    @Test
    fun otherGpusGetNoRecommendation() {
        for (r in listOf(null, "", "Mali-G715", "PowerVR B-Series BXM-8-256", "Xclipse 940"))
            assertNull(r.toString(), GpuInfo.recommendation(r))
    }
}
