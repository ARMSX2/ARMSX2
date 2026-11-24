package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GpuInfoRecommendationTest {
    // The driver settings page recommends our Turnip on every Adreno 6xx and 7xx and on the 8xx parts its device table
    // lists (810, 829, 830, 840), the range the first-run setup offers it on (CustomDriver.isArmsx2TurnipPart), so a
    // player is told the same thing in both places.
    @Test
    fun ourTurnipIsRecommendedOnAdreno6xx7xxAndTheListed8xx() {
        for (r in listOf(
            "Adreno (TM) 610", "Adreno (TM) 618", "Adreno (TM) 650", "Adreno (TM) 660", "Adreno (TM) 690",
            "Adreno (TM) 702", "Adreno (TM) 730", "Adreno (TM) 740", "Adreno (TM) 750",
            "Adreno (TM) 810", "Adreno (TM) 829", "Adreno (TM) 830", "Adreno (TM) 840",
        )) {
            assertEquals(r, "ARMSX2 Turnip", GpuInfo.recommendation(r)?.sourceLabel)
            assertTrue(r, CustomDriver.isArmsx2TurnipPart(r))
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

    // Every number from 800 up that our Turnip has no table row for keeps the StevenMXZ recommendation it had:
    // the 8xx parts the table lacks (on those our pack finds no device), and the 9xx and four-digit numbers the
    // same rule always caught.
    @Test
    fun otherNumbersFrom800UpStayOnStevenMXZsGen8Turnip() {
        for (r in listOf(
            "Adreno (TM) 812", "Adreno (TM) 825", "Adreno (TM) 845", "Adreno (TM) 850",
            "Adreno (TM) 900", "Adreno (TM) 8300", "Adreno (TM) 6500",
        )) {
            assertEquals(r, "StevenMXZ · Adreno-Tools", GpuInfo.recommendation(r)?.sourceLabel)
            assertFalse(r, CustomDriver.isArmsx2TurnipPart(r))
        }
    }

    @Test
    fun olderAdrenoStaysOnTheGeneralPack() {
        assertEquals("KIMCHI", GpuInfo.recommendation("Adreno (TM) 540")?.sourceLabel)
        assertEquals("KIMCHI", GpuInfo.recommendation("Adreno (TM) 506")?.sourceLabel)
    }

    // Mali parts malisx2 lists for get malisx2 — the same predicate the download list uses, so the
    // card above the list never says "system" while the only listed driver is ours.
    @Test
    fun supportedMaliIsRecommendedMaliSX2() {
        for (r in listOf(
            "Mali-G57 MC2", "Mali-G78AE MC20", "Mali-G310 MC1", "Mali-G510 MC2", "Mali-G610 MC4", "Mali-G710 MC10",
            "Mali-G615 MC6", "Mali-G715-Immortalis MC11", "Mali-G31 MP2", "Mali-G51 MP4", "Mali-G52 MC2", "Mali-G76 MC4",
        ))
            assertEquals(r, "ARMSX2 · MaliSX2", GpuInfo.recommendation(r)?.sourceLabel)
    }

    @Test
    fun otherGpusGetNoRecommendation() {
        for (r in listOf(
            null, "", "Mali", "Mali-G71 MP20", "Mali-G72 MP12", "Mali-G720 MC7", "Mali-T880 MP12",
            "PowerVR B-Series BXM-8-256", "Xclipse 940",
        ))
            assertNull(r.toString(), GpuInfo.recommendation(r))
    }
}
