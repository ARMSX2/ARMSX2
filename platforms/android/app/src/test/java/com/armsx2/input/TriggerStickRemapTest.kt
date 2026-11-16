package com.armsx2.input

import com.armsx2.input.TriggerStickRemap.Companion.RIGHT_STICK_DOWN
import com.armsx2.input.TriggerStickRemap.Companion.RIGHT_STICK_UP
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TriggerStickRemapTest {
    private val remap = TriggerStickRemap()

    @Test
    fun triggerPullsKeepTheirMagnitudeAndDirection() {
        remap.update(10, 0, left = 0f, right = 0.25f)
        assertEquals(-0.25f, remap.vertical(0), 0f)
        remap.update(10, 0, left = 0.75f, right = 0f)
        assertEquals(0.75f, remap.vertical(0), 0f)
        remap.update(10, 0, left = 0f, right = 0f)
        assertEquals(0f, remap.vertical(0), 0f)
    }

    @Test
    fun simultaneousTriggerPullsCancelOrUseTheDifference() {
        remap.update(10, 0, left = 0.75f, right = 0.75f)
        assertEquals(0f, remap.vertical(0), 0f)
        remap.update(10, 0, left = 0.375f, right = 0.75f)
        assertEquals(-0.375f, remap.vertical(0), 0f)
        remap.update(10, 0, left = 0.375f, right = 0f)
        assertEquals(0.375f, remap.vertical(0), 0f)
    }

    @Test
    fun triggerDeflectionMergesWithPhysicalRightStick() {
        val output = mutableMapOf<Int, Float>()
        remap.update(10, 0, left = 0.25f, right = 0.75f)
        remap.emitVertical(0, stickUp = 0f, stickDown = 0.125f) { code, value -> output[code] = value }
        assertEquals(mapOf(RIGHT_STICK_UP to 0.375f, RIGHT_STICK_DOWN to 0f), output)

        remap.update(10, 0, left = 0.75f, right = 0.75f)
        remap.emitVertical(0, stickUp = 0f, stickDown = 0.125f) { code, value -> output[code] = value }
        assertEquals(mapOf(RIGHT_STICK_UP to 0f, RIGHT_STICK_DOWN to 0.125f), output)
    }

    @Test
    fun missingAxisSamplesPreserveKeyOnlyTriggersUntilTheirRelease() {
        remap.update(10, 0, left = null, right = 1f)
        remap.update(10, 0, left = 0.5f, right = null)
        assertEquals(-0.5f, remap.vertical(0), 0f)
        remap.update(10, 0, left = null, right = 0f)
        assertEquals(0.5f, remap.vertical(0), 0f)
    }

    @Test
    fun digitalTriggerReleasesPreserveTheOtherHeldSide() {
        remap.update(10, 0, left = 1f, right = null)
        remap.update(10, 0, left = null, right = 1f)
        assertEquals(0f, remap.vertical(0), 0f)
        remap.update(10, 0, left = 0f, right = null)
        assertEquals(-1f, remap.vertical(0), 0f)
        remap.update(10, 0, left = null, right = 0f)
        assertNull(remap.portForDevice(10))
    }

    @Test
    fun sharedPortsKeepEachDevicesContributionAndDisconnectReleasesOnlyThatDevice() {
        remap.update(10, 0, left = 0.5f, right = 0f)
        remap.update(11, 0, left = 0f, right = 0.75f)
        remap.update(12, 1, left = 0f, right = 1f)
        assertEquals(-0.25f, remap.vertical(0), 0f)
        assertEquals(0, remap.remove(11))
        assertEquals(0.5f, remap.vertical(0), 0f)
        assertEquals(-1f, remap.vertical(1), 0f)
        assertEquals(0, remap.update(10, 2, left = 0.5f, right = 0f))
        assertEquals(0f, remap.vertical(0), 0f)
        assertEquals(setOf(1, 2), remap.clear())
        assertEquals(0f, remap.vertical(1), 0f)
        assertEquals(0f, remap.vertical(2), 0f)
    }
}
