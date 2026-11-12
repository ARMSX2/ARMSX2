package com.armsx2.config

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

class RendererDefaultTest {
    @Test
    fun theDefaultRendererIsVulkan() {
        assertEquals("vulkan", Settings().output.renderer)
    }

    @Test
    fun aNewInstallStartsOnVulkanWhereTheDeviceHasVulkan11() {
        assertEquals("vulkan", ConfigStore.freshInstallSettings({ false }, hasVulkan11 = true).output.renderer)
    }

    @Test
    fun aNewInstallWithoutVulkan11StartsOnAuto() {
        assertEquals("auto", ConfigStore.freshInstallSettings({ false }, hasVulkan11 = false).output.renderer)
    }

    @Test
    fun aSavedAutoStaysAuto() {
        // Every global save written so far holds the renderer explicitly, Auto included; the new
        // default does not reach it.
        val saved = Settings().let { it.copy(output = it.output.copy(renderer = "auto")) }.toJson()
        assertEquals("auto", Settings.fromJson(JSONObject(saved.toString())).output.renderer)
    }
}
