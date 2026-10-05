package com.armsx2.config

import com.armsx2.navigation.SettingsCategory
import com.armsx2.ui.settingshub.SETTINGS_CATEGORY_FIELDS
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The texture upscaler setting is one int field that has to survive every path a graphics field
 * takes: the native INI (EmuCore/GS/TextureUpscaleMode), the stored JSON, the per-game override
 * blob, and the live-GS-reconfigure check. A path that is missed does not fail loudly, the choice
 * just reverts, so each one is pinned here.
 */
class TextureUpscaleSettingTest {
    private val iniKey = "EmuCore/GS/TextureUpscaleMode"

    private fun withMode(mode: Int, base: Settings = Settings()): Settings =
        base.copy(graphics = base.graphics.copy(textureUpscaleMode = mode))

    @Test
    fun defaultsToOff() {
        assertEquals(0, Settings().graphics.textureUpscaleMode)
    }

    @Test
    fun writesTheCoreKeyAsAnInt() {
        assertEquals("0", withMode(0).emittedKeys()[iniKey])
        assertEquals("1", withMode(1).emittedKeys()[iniKey])
        assertEquals("2", withMode(2).emittedKeys()[iniKey])
        assertEquals("3", withMode(3).emittedKeys()[iniKey])
    }

    @Test
    fun clampsToTheCoreEnumRange() {
        assertEquals("0", withMode(-3).emittedKeys()[iniKey])
        // 3 is the highest core value (RaisrSmooth4x); anything above it is held there.
        assertEquals("3", withMode(4).emittedKeys()[iniKey])
        assertEquals("3", withMode(9).emittedKeys()[iniKey])
    }

    @Test
    fun readsBackFromTheNativeIni() {
        assertEquals(2, Settings().readFromIni(mapOf(iniKey to "2")).graphics.textureUpscaleMode)
        assertEquals(3, Settings().readFromIni(mapOf(iniKey to "3")).graphics.textureUpscaleMode)
        // A key the INI does not have leaves the current value alone.
        assertEquals(1, withMode(1).readFromIni(emptyMap()).graphics.textureUpscaleMode)
    }

    @Test
    fun roundTripsThroughStoredJson() {
        for (mode in 0..3) {
            assertEquals(mode, Settings.fromJson(withMode(mode).toJson()).graphics.textureUpscaleMode)
        }
    }

    @Test
    fun perGameOverrideRoundTrips() {
        val global = Settings()
        for (mode in 2..3) {
            val game = withMode(mode, global)
            val overrides = Settings.diff(global, game)
            assertEquals(mode, overrides.getInt("textureUpscaleMode"))
            assertEquals(mode, Settings.merge(global, overrides).graphics.textureUpscaleMode)
        }

        // Unchanged from global: nothing is stored, so the game follows later global changes.
        assertFalse(Settings.diff(global, global).has("textureUpscaleMode"))
        assertEquals(1, Settings.merge(withMode(1), Settings.diff(global, global)).graphics.textureUpscaleMode)
    }

    @Test
    fun changingItTriggersALiveGsReconfigure() {
        assertTrue(withMode(1).gsDiffersFrom(Settings()))
        // 2x Smooth to 4x Smooth is a different job scale, so it reconfigures too.
        assertTrue(withMode(3).gsDiffersFrom(withMode(2)))
        assertFalse(Settings().gsDiffersFrom(Settings()))
    }

    @Test
    fun graphicsTabResetCoversIt() {
        assertTrue("textureUpscaleMode" in SETTINGS_CATEGORY_FIELDS.getValue(SettingsCategory.Graphics))
        assertTrue(withMode(1).toJson().has("textureUpscaleMode"))
    }
}
