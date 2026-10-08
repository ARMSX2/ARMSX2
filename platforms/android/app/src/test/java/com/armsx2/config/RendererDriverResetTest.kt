package com.armsx2.config

import com.armsx2.navigation.SettingsCategory
import com.armsx2.ui.settingshub.categoryOverrideKeys
import com.armsx2.ui.settingshub.pruneOverrides
import com.armsx2.ui.settingshub.resetCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The Renderer tab's Reset now includes the Graphics API, GPU driver and ANGLE settings. For
 * per-game settings, Reset goes back to whatever the global values are, and for global
 * settings it goes back to the defaults.
 */
class RendererDriverResetTest {
    private val global = Settings().let { it.copy(output = it.output.copy(renderer = "vulkan", customDriverId = "turnip")) }

    @Test
    fun graphicsResetClearsRendererAndDriverOverrides() {
        val game = global.copy(
            output = global.output.copy(renderer = "opengl", customDriverId = ""),
            display = global.display.copy(useAngleOpenGL = true),
        )
        val overrides = Settings.diff(global, game)
        assertEquals(setOf("renderer", "customDriverId", "useAngleOpenGL"), overrides.keys().asSequence().toSet())
        assertNull(pruneOverrides(overrides, categoryOverrideKeys(SettingsCategory.Graphics)))
    }

    @Test
    fun globalGraphicsResetRestoresDefaults() {
        val reset = global.copy(display = global.display.copy(useAngleOpenGL = true)).resetCategory(SettingsCategory.Graphics)
        assertEquals(Settings().output.renderer, reset.output.renderer)
        assertEquals(Settings().output.customDriverId, reset.output.customDriverId)
        assertEquals(Settings().display.useAngleOpenGL, reset.display.useAngleOpenGL)
    }
}
