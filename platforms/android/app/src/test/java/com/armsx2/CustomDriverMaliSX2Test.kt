package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * The driver list's malisx2 switch: which installed pack counts as malisx2, which one is the newest,
 * what the notices call it, and when the list offers the switch. The core decides the OSD notice
 * from the same facts (GSDriverReport::MaliSX2NoticeFor); the list offers the switch under the
 * same conditions, minus the open Vulkan device that only the core can read.
 */
class CustomDriverMaliSX2Test {
    private fun pack(id: String, name: String, lib: String) = CustomDriver.InstalledDriver(
        id = id, name = name, description = "", author = "", vendor = "", version = "",
        libraryName = lib, driverDir = File("/drivers/$id"),
    )

    private val malisx2_041 = pack("armsx2libmali-0.4.1", "malisx2 0.4.1 (g1a2b3c4)", "libvulkan_malisx2.so")
    private val malisx2_050 = pack("armsx2libmali-0.5.0", "malisx2 0.5.0 (g9f8e7d6)", "libvulkan_malisx2.so")
    private val turnip = pack("armsx2turnip-26.1", "turnip 26.1", "libvulkan_freedreno.so")
    private val g57 = "Mali-G57 MC2"

    @Test
    fun malisx2PacksAreToldByIdOrLibraryInEitherSpelling() {
        assertTrue(CustomDriver.isMaliSX2Pack("armsx2libmali-0.4.1", "libvulkan_freedreno.so"))
        assertTrue(CustomDriver.isMaliSX2Pack("some-id", "libvulkan_malisx2.so"))
        assertTrue(CustomDriver.isMaliSX2Pack("ARMSX2LIBMALI-0.3.1", "x.so"))
        assertFalse(CustomDriver.isMaliSX2Pack("armsx2turnip-26.1", "libvulkan_freedreno.so"))
    }

    @Test
    fun theNewestInstalledMaliSX2PackWins() {
        assertEquals(malisx2_050, CustomDriver.installedMaliSX2(listOf(malisx2_041, turnip, malisx2_050)))
        assertEquals(malisx2_050, CustomDriver.installedMaliSX2(listOf(malisx2_050, malisx2_041)))
        assertNull(CustomDriver.installedMaliSX2(listOf(turnip)))
        assertNull(CustomDriver.installedMaliSX2(emptyList()))
    }

    @Test
    fun theLabelDropsTheBuildTag() {
        assertEquals("malisx2 0.4.1", CustomDriver.maliSX2Label(malisx2_041))
        assertEquals("plain", CustomDriver.maliSX2Label(pack("armsx2libmali-x", "plain", "libvulkan_malisx2.so")))
        // A name that is only a tag still gives a label.
        assertEquals("(tag)", CustomDriver.maliSX2Label(pack("armsx2libmali-x", "(tag)", "libvulkan_malisx2.so")))
    }

    // The RG 476H case: malisx2 installed, "System Vulkan driver" selected.
    @Test
    fun offersTheSwitchWhenMaliSX2IsInstalledAndTheSystemDriverRuns() {
        assertEquals(malisx2_041, CustomDriver.maliSX2SwitchOffer(g57, listOf(malisx2_041), null))
    }

    @Test
    fun offersTheSwitchWhenAnotherPackRuns() {
        assertEquals(malisx2_041, CustomDriver.maliSX2SwitchOffer(g57, listOf(malisx2_041, turnip), turnip.id))
    }

    @Test
    fun offersNothingWhenAMaliSX2PackIsAlreadySelected() {
        assertNull(CustomDriver.maliSX2SwitchOffer(g57, listOf(malisx2_041, malisx2_050), malisx2_041.id))
    }

    @Test
    fun offersNothingWhenNoMaliSX2PackIsInstalled() {
        assertNull(CustomDriver.maliSX2SwitchOffer(g57, listOf(turnip), null))
        assertNull(CustomDriver.maliSX2SwitchOffer(g57, emptyList(), null))
    }

    @Test
    fun offersNothingOnAGpuTheListDoesNotOfferMaliSX2For() {
        assertNull(CustomDriver.maliSX2SwitchOffer("Adreno (TM) 650", listOf(malisx2_041), null))
        assertNull(CustomDriver.maliSX2SwitchOffer("Mali-T880", listOf(malisx2_041), null))
        assertNull(CustomDriver.maliSX2SwitchOffer(null, listOf(malisx2_041), null))
    }
}
