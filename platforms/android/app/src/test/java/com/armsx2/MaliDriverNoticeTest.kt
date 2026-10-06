package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MaliDriverNoticeTest {
    private val g615 = "Mali-G615 MC6"
    private val g715 = "Mali-G715-Immortalis MC11"

    @Test
    fun warnsOnlyForVulkanOnAnOfferedGpuWithoutMaliSX2() {
        for (vulkan in listOf(false, true))
            for (offered in listOf(false, true))
                for (sx2 in listOf(false, true))
                    assertEquals(
                        "vulkan=$vulkan offered=$offered sx2=$sx2",
                        vulkan && offered && !sx2,
                        MaliDriverNotice.shouldWarn(vulkan, offered, sx2),
                    )
    }

    @Test
    fun warnsForArmsDriverAndForAnyOtherPackOnAnOfferedGpu() {
        // Native reports OTHER for Arm's stock driver and for every pack that is not malisx2.
        for (r in listOf(g615, g715))
            assertTrue(r, MaliDriverNotice.shouldWarn(MaliDriverNotice.DRIVER_OTHER, true, r))
    }

    @Test
    fun staysQuietOnMaliSX2() {
        for (r in listOf(g615, g715))
            assertFalse(r, MaliDriverNotice.shouldWarn(MaliDriverNotice.DRIVER_MALISX2, true, r))
    }

    @Test
    fun staysQuietWhenVulkanIsNotRunning() {
        // OpenGL, or no device yet.
        assertFalse(MaliDriverNotice.shouldWarn(MaliDriverNotice.DRIVER_NONE, true, g615))
        // A value native does not define is not a Vulkan driver either.
        assertFalse(MaliDriverNotice.shouldWarn(3, true, g615))
        assertFalse(MaliDriverNotice.shouldWarn(-1, true, g615))
    }

    @Test
    fun staysQuietUnderTheSoftwareRenderer() {
        // The software renderer can sit on a Vulkan device, but only to present a frame the CPU
        // drew, so a different driver costs the user nothing.
        for (r in listOf(g615, g715))
            assertFalse(r, MaliDriverNotice.shouldWarn(MaliDriverNotice.DRIVER_OTHER, false, r))
    }

    @Test
    fun staysQuietOnGpusTheDriverListDoesNotOffer() {
        // Telling these users to download a driver that is not listed for them would be wrong.
        for (r in listOf(
            null, "", "Mali-G57 MC2", "Mali-G610 MC6", "Mali-G720-Immortalis MC12",
            "Adreno (TM) 650", "PowerVR B-Series BXM-8-256",
        ))
            assertFalse(r.toString(), MaliDriverNotice.shouldWarn(MaliDriverNotice.DRIVER_OTHER, true, r))
    }

    @Test
    fun followsTheDriverListForEveryGpu() {
        // The notice keys on the same predicate the list uses to offer malisx2, so whatever that
        // is widened to (the G57, when its release ships) the notice follows with no edit here.
        for (r in listOf(
            null, g615, g715, "Mali-G615 MC2", "Mali-G57 MC2", "Mali-G610 MC6", "Adreno (TM) 740",
        ))
            assertEquals(
                r.toString(),
                CustomDriver.offersMaliSX2(r),
                MaliDriverNotice.shouldWarn(MaliDriverNotice.DRIVER_OTHER, true, r),
            )
    }

    @Test
    fun theListOffersMaliSX2OnV11OnlyForNow() {
        // Moves with CustomDriverSourceTest when the G57 is added to the list.
        assertTrue(CustomDriver.offersMaliSX2(g615))
        assertTrue(CustomDriver.offersMaliSX2(g715))
        assertFalse(CustomDriver.offersMaliSX2("Mali-G57 MC2"))
    }
}
