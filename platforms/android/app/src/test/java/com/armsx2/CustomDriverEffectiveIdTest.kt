package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Which driver a game boots with: its own pick, else the global pick, else the system driver. */
class CustomDriverEffectiveIdTest {
    private val installed = listOf("turnip-new", "turnip-old")

    @Test
    fun installedGamePickWins() {
        assertEquals("turnip-old", CustomDriver.effectiveId("turnip-old", "turnip-new", installed))
    }

    @Test
    fun deletedGamePickFollowsGlobal() {
        assertEquals("turnip-new", CustomDriver.effectiveId("turnip-deleted", "turnip-new", installed))
    }

    @Test
    fun deletedGameAndGlobalPicksUseSystem() {
        assertNull(CustomDriver.effectiveId("turnip-deleted", "turnip-gone", installed))
        assertNull(CustomDriver.effectiveId("turnip-deleted", "", installed))
    }

    @Test
    fun blankGamePickIsSystemEvenWithGlobalDriver() {
        // Blank in the resolved settings is an explicit system choice (or a global one).
        assertNull(CustomDriver.effectiveId("", "turnip-new", installed))
    }
}
