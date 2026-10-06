package com.armsx2.config

import com.armsx2.navigation.SettingsCategory
import com.armsx2.ui.settingshub.SETTINGS_CATEGORY_FIELDS
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The texture upscaler setting is one int field that has to survive every path a graphics field
 * takes: the native INI (EmuCore/GS/TextureUpscale), the stored JSON, the per-game override
 * blob, and the live-GS-reconfigure check. A path that is missed does not fail loudly, the choice
 * just reverts, so each one is pinned here.
 *
 * The values are 0 Off, 1 RAISR 2x, 2 RAISR 4x. The core's first key for this setting was
 * TextureUpscaleMode, where 2 meant Smooth 2x; a leftover value of that key must not be read as a
 * 4x choice, so the old key and the old stored JSON field are ignored.
 */
class TextureUpscaleSettingTest {
    private val iniKey = "EmuCore/GS/TextureUpscale"
    private val oldIniKey = "EmuCore/GS/TextureUpscaleMode"

    private fun withMode(mode: Int, base: Settings = Settings()): Settings =
        base.copy(graphics = base.graphics.copy(textureUpscale = mode))

    @Test
    fun defaultsToOff() {
        assertEquals(0, Settings().graphics.textureUpscale)
    }

    @Test
    fun writesTheCoreKeyAsAnInt() {
        assertEquals("0", withMode(0).emittedKeys()[iniKey])
        assertEquals("1", withMode(1).emittedKeys()[iniKey])
        assertEquals("2", withMode(2).emittedKeys()[iniKey])
    }

    @Test
    fun doesNotWriteTheOldCoreKey() {
        assertFalse(withMode(2).emittedKeys().containsKey(oldIniKey))
    }

    @Test
    fun clampsToTheCoreEnumRange() {
        assertEquals("0", withMode(-3).emittedKeys()[iniKey])
        // 2 is the highest core value (Raisr4x); anything above it is held there.
        assertEquals("2", withMode(3).emittedKeys()[iniKey])
        assertEquals("2", withMode(9).emittedKeys()[iniKey])
    }

    @Test
    fun readsBackFromTheNativeIni() {
        assertEquals(1, Settings().readFromIni(mapOf(iniKey to "1")).graphics.textureUpscale)
        assertEquals(2, Settings().readFromIni(mapOf(iniKey to "2")).graphics.textureUpscale)
        // A key the INI does not have leaves the current value alone.
        assertEquals(1, withMode(1).readFromIni(emptyMap()).graphics.textureUpscale)
    }

    @Test
    fun ignoresTheOldIniKey() {
        // An old 2 meant Smooth 2x. It must not turn into 4x.
        assertEquals(0, Settings().readFromIni(mapOf(oldIniKey to "2")).graphics.textureUpscale)
        assertEquals(1, withMode(1).readFromIni(mapOf(oldIniKey to "2")).graphics.textureUpscale)
    }

    @Test
    fun roundTripsThroughStoredJson() {
        for (mode in 0..2) {
            assertEquals(mode, Settings.fromJson(withMode(mode).toJson()).graphics.textureUpscale)
        }
    }

    @Test
    fun ignoresTheOldStoredJsonField() {
        val stored = Settings().toJson().put("textureUpscaleMode", 2)
        assertEquals(0, Settings.fromJson(stored).graphics.textureUpscale)
    }

    @Test
    fun perGameOverrideRoundTrips() {
        val global = Settings()
        for (mode in 1..2) {
            val game = withMode(mode, global)
            val overrides = Settings.diff(global, game)
            assertEquals(mode, overrides.getInt("textureUpscale"))
            assertEquals(mode, Settings.merge(global, overrides).graphics.textureUpscale)
        }

        // Unchanged from global: nothing is stored, so the game follows later global changes.
        assertFalse(Settings.diff(global, global).has("textureUpscale"))
        assertEquals(1, Settings.merge(withMode(1), Settings.diff(global, global)).graphics.textureUpscale)
    }

    @Test
    fun changingItTriggersALiveGsReconfigure() {
        assertTrue(withMode(1).gsDiffersFrom(Settings()))
        // 2x to 4x is a different job scale, so it reconfigures too.
        assertTrue(withMode(2).gsDiffersFrom(withMode(1)))
        assertFalse(Settings().gsDiffersFrom(Settings()))
    }

    @Test
    fun graphicsTabResetCoversIt() {
        assertTrue("textureUpscale" in SETTINGS_CATEGORY_FIELDS.getValue(SettingsCategory.Graphics))
        assertTrue(withMode(1).toJson().has("textureUpscale"))
    }
}
