package com.armsx2

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Who is offered the newest ARMSX2 driver after an app update, and whether it reads as an update. */
class DriverUpdateOfferTest {
    private fun pack(id: String, source: String) = CustomDriver.RemoteDriver(
        id = id, releaseName = "", assetName = "$id.zip", tagName = "", publishedAt = "",
        assetUrl = "", sizeBytes = 0, source = source,
    )

    private val newestMali = pack("armsx2libmali-v0.5.1-malisx2-0.5.1", "ARMSX2 · MaliSX2")
    private val newestTurnip = pack("armsx2turnip-axfl3-003-armsx2-turnip-axfl3-003", "ARMSX2 · turnip")

    @Test
    fun dueOncePerVersion() {
        assertTrue(DriverUpdateOffer.due(settledVersion = 0, versionCode = 1090))
        assertTrue(DriverUpdateOffer.due(settledVersion = 1089, versionCode = 1090))
        assertFalse(DriverUpdateOffer.due(settledVersion = 1090, versionCode = 1090))
    }

    @Test
    fun offeredToAnyoneNotOnTheNewestRelease() {
        assertTrue(DriverUpdateOffer.offers("", newestMali))
        assertTrue(DriverUpdateOffer.offers("armsx2libmali-v0.4.1-malisx2-0.4.1", newestMali))
        assertTrue(DriverUpdateOffer.offers("v25.0.0-turnip_v25", newestTurnip))
        assertTrue(DriverUpdateOffer.offers("local-turnip.zip", newestTurnip))
    }

    @Test
    fun notOfferedOnTheNewestReleaseOrWithoutOne() {
        assertFalse(DriverUpdateOffer.offers(newestMali.id, newestMali))
        assertFalse(DriverUpdateOffer.offers(newestTurnip.id, newestTurnip))
        assertFalse(DriverUpdateOffer.offers("", null))
    }

    @Test
    fun anOlderReleaseOfOurDriverIsAnUpdate() {
        assertTrue(CustomDriver.isFromSource("armsx2libmali-v0.4.1-malisx2-0.4.1", newestMali.source))
        assertTrue(CustomDriver.isFromSource("armsx2turnip-axfl1-011-armsx2-turnip", newestTurnip.source))
    }

    @Test
    fun anotherDriverIsNotAnUpdate() {
        // K11MCH1's ids carry no prefix, a local import is "local-", and the two of ours differ.
        assertFalse(CustomDriver.isFromSource("v25.0.0-turnip_v25", newestTurnip.source))
        assertFalse(CustomDriver.isFromSource("local-malisx2-0.4.1.zip", newestMali.source))
        assertFalse(CustomDriver.isFromSource("armsx2turnip-axfl1-011-armsx2-turnip", newestMali.source))
        assertFalse(CustomDriver.isFromSource("purpleturnip-v1-turnip", newestTurnip.source))
        assertFalse(CustomDriver.isFromSource("armsx2libmali-v0.4.1", "no such source"))
    }
}
