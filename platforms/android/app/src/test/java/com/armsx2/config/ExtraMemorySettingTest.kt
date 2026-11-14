package com.armsx2.config

import com.armsx2.navigation.SettingsCategory
import com.armsx2.ui.settingshub.resetCategory
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Extended RAM (EmuCore/CPU/ExtraMemory): off unless asked for, kept across a save and load, and a
 * per-game override like any other setting.
 */
class ExtraMemorySettingTest {
    private val on = Settings().let { it.copy(cpu = it.cpu.copy(extraMemory = true)) }

    @Test
    fun itIsOffByDefault() {
        assertFalse(Settings().cpu.extraMemory)
        assertFalse(Settings.fromJson(JSONObject()).cpu.extraMemory)
    }

    @Test
    fun theSettingsFileKeepsIt() {
        assertTrue(Settings.fromJson(on.toJson()).cpu.extraMemory)
        assertFalse(Settings.fromJson(Settings().toJson()).cpu.extraMemory)
    }

    @Test
    fun aGameCanTurnItOnOverTheGlobalSetting() {
        val overrides = Settings.diff(Settings(), on)
        assertEquals(true, overrides.opt("extraMemory"))
        assertTrue(Settings.merge(Settings(), overrides).cpu.extraMemory)
    }

    @Test
    fun aGameThatDoesNotTouchItFollowsTheGlobalSetting() {
        assertFalse(Settings.diff(on, on).has("extraMemory"))
        assertTrue(Settings.merge(on, JSONObject()).cpu.extraMemory)
    }

    @Test
    fun itIsReadBackFromTheCoresIniKey() {
        assertTrue(Settings().readFromIni(mapOf("EmuCore/CPU/ExtraMemory" to "true")).cpu.extraMemory)
        assertFalse(on.readFromIni(mapOf("EmuCore/CPU/ExtraMemory" to "false")).cpu.extraMemory)
        assertTrue(on.readFromIni(emptyMap()).cpu.extraMemory)
    }

    @Test
    fun theAdvancedTabsResetTurnsItOff() {
        assertFalse(on.resetCategory(SettingsCategory.Advanced).cpu.extraMemory)
    }
}
