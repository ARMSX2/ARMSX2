package com.armsx2.ui.settings

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SettingsSearchJumpTest {
    @Test
    fun matchesRegisteredSettingsRows() {
        val label = "CPU Sprite Render"
        assertTrue(settingsRowMatchesLabel("toggle:$label", label))
        assertTrue(settingsRowMatchesLabel("segmented:$label", label))
        assertTrue(settingsRowMatchesLabel("segmented-grid:$label", label))
        assertTrue(settingsRowMatchesLabel("slider:$label", label))
        assertTrue(settingsRowMatchesLabel("slider:$label:some-composition-id", label))
    }

    @Test
    fun doesNotExpandForOtherRowsOrSectionHeaders() {
        val label = "CPU Sprite Render"
        assertFalse(settingsRowMatchesLabel("toggle:CPU Sprite Render Extra", label))
        assertFalse(settingsRowMatchesLabel("slider:CPU Sprite Render Extra:42", label))
        assertFalse(settingsRowMatchesLabel("section:$label", label))
        assertFalse(settingsRowMatchesLabel("toggle:Other Setting", label))
    }

    @Test
    fun sectionSearchMatchesItsOwnHeaderOnly() {
        assertTrue(settingsSectionMatchesLabel("section.Hardware Fixes", "Hardware Fixes"))
        assertFalse(settingsSectionMatchesLabel("section.Hardware Fixes Extra", "Hardware Fixes"))
        assertFalse(settingsSectionMatchesLabel("toggle:Hardware Fixes", "Hardware Fixes"))
    }
}
