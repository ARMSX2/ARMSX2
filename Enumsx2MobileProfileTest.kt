package com.armsx2.enumsx2

import com.armsx2.config.Settings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class Enumsx2MobileProfileTest {
    @Test fun matchesA67l() =
        assertTrue(Enumsx2MobileProfile.matches(listOf("A67L Gen2")))

    @Test fun matchesBluViewUltra() =
        assertTrue(Enumsx2MobileProfile.matches(listOf("BLU", "View Ultra 5G")))

    @Test fun matchesSc9863Family() =
        assertTrue(Enumsx2MobileProfile.matches(listOf("ums512", "SC9863A")))

    @Test fun doesNotMatchUnrelatedPhone() =
        assertFalse(Enumsx2MobileProfile.matches(listOf("Pixel 10", "Tensor")))

    @Test fun appliesConservativeMobileDefaults() {
        val s = Enumsx2MobileProfile.apply(Settings())
        assertEquals("vulkan", s.output.renderer)
        assertEquals(1.0f, s.output.upscaleFloat)
        assertEquals("1280x720", s.output.screenResOverride)
        assertEquals(3, s.graphics.gpuProfile)
        assertEquals(1, s.graphics.texturePreloading)
        assertEquals(0, s.graphics.maxAnisotropy)
        assertFalse(s.graphics.disableShaderCache)
        assertTrue(s.display.coalesceRenderPasses)
        assertEquals(2, s.hwFixes.vsyncQueueSize)
        assertTrue(s.cpu.enableFastmem)
        assertTrue(s.cpu.vuAot)
    }
}
