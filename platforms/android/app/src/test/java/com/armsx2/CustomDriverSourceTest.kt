package com.armsx2

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CustomDriverSourceTest {
    @Test
    fun libmaliListsOnMaliV11Only() {
        for (r in listOf("Mali-G615 MC6", "Mali-G615 MC2", "Mali-G715 MC7", "Mali-G715-Immortalis MC11", "Immortalis-G715 MC11"))
            assertTrue(r, CustomDriver.isMaliV11(r))
        for (r in listOf(
            null, "", "Mali-G610 MC6", "Mali-G710 MC10", "Mali-G57 MC2", "Mali-G720-Immortalis MC12",
            "Mali-G925-Immortalis MC12", "Adreno (TM) 650", "Adreno (TM) 615", "PowerVR B-Series BXM-8-256",
        ))
            assertFalse(r.toString(), CustomDriver.isMaliV11(r))
    }
}
