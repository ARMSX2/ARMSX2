package com.armsx2

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CustomDriverSourceTest {
    // v9: G57, G68, G77, G78 (G78AE included). v11: G615, G715 (Immortalis-G715 included).
    // Each part is listed bare, with MC<n>, and with MP<n> where Arm's blobs use it. The app reads
    // the GL_RENDERER string (GpuInfo), so these are GL strings; the bare and MC<n> forms are also
    // what malisx2 reports as its Vulkan deviceName.
    private val valhallV9 = listOf(
        "Mali-G57", "Mali-G57 MC2", "Mali-G57 MC3", "Mali-G57 MC4", "Mali-G57 MC6",
        "Mali-G68", "Mali-G68 MC4",
        "Mali-G77 MC7", "Mali-G77 MC9",
        "Mali-G78", "Mali-G78 MC14", "Mali-G78 MP14", "Mali-G78 MP20",
        "Mali-G78AE", "Mali-G78AE MC10", "Mali-G78AE MC20",
    )
    private val valhallV11 = listOf(
        "Mali-G615", "Mali-G615 MC6", "Mali-G615 MC2",
        "Mali-G715", "Mali-G715 MC7", "Mali-G715-Immortalis MC11",
        "Immortalis-G715 MC11",
    )

    // Everything that must not list malisx2: the near-miss model numbers (G57 vs G52/G51/G510,
    // G68 vs G610, G77 vs G710/G720, G78 vs G725), v10, Bifrost, Midgard, the 5th-gen parts,
    // G1, and non-Mali GPUs.
    private val notOffered = listOf(
        null, "", "Mali", "Immortalis",
        "Mali-G51", "Mali-G52 MC2", "Mali-G510 MC2", "Mali-G31 MP2", "Mali-G71 MP20",
        "Mali-G72 MP12", "Mali-G76 MC4", "Mali-T880 MP12",
        "Mali-G310 MC2", "Mali-G610 MC6", "Mali-G710 MC10", "Mali-G710-Immortalis MC10",
        "Mali-G620 MC4", "Mali-G720-Immortalis MC12", "Mali-G625 MC6", "Mali-G725 MC11",
        "Mali-G925-Immortalis MC12", "Mali-G1-Ultra MC12",
        "Adreno (TM) 650", "Adreno (TM) 615", "PowerVR B-Series BXM-8-256",
    )

    @Test
    fun maliSX2ListsOnMaliValhallV9AndV11Only() {
        for (r in valhallV9 + valhallV11)
            assertTrue(r, CustomDriver.isMaliValhallV9OrV11(r))
        for (r in notOffered)
            assertFalse(r.toString(), CustomDriver.isMaliValhallV9OrV11(r))
    }

    @Test
    fun valhallMatchIgnoresCase() {
        for (r in listOf("mali-g57 mc2", "MALI-G78AE MC20", "immortalis-g715 mc11"))
            assertTrue(r, CustomDriver.isMaliValhallV9OrV11(r))
        for (r in listOf("mali-g710 mc10", "MALI-G510 MC2"))
            assertFalse(r, CustomDriver.isMaliValhallV9OrV11(r))
    }

    @Test
    fun theListOffersMaliSX2OnValhallV9AndV11() {
        // The wrong-driver notice shares the predicate, so it covers exactly the same parts.
        for (r in valhallV9 + valhallV11)
            assertTrue(r, CustomDriver.offersMaliSX2(r))
        // Telling these users to download a driver that is not listed for them would be wrong.
        for (r in notOffered)
            assertFalse(r.toString(), CustomDriver.offersMaliSX2(r))
    }
}
