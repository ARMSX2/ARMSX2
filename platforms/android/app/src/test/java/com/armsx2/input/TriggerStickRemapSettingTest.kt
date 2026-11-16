package com.armsx2.input

import android.view.KeyEvent
import com.armsx2.config.FakeSharedPreferences
import com.armsx2.runtime.MainActivityRuntime
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class TriggerStickRemapSettingTest {
    @Before
    fun freshStore() {
        MainActivityRuntime.prefs = FakeSharedPreferences()
        ControllerMappings.invalidateRuntimeCaches()
    }

    @Test
    fun remapDefaultsOffAndOnlyEnablesTheSelectedGame() {
        assertFalse(ControllerMappings.remapTriggersToRightStickForGame("SLUS-12345"))
        ControllerMappings.setRemapTriggersToRightStick("SLUS-12345", true)
        assertTrue(ControllerMappings.remapTriggersToRightStickForGame("SLUS-12345"))
        assertFalse(ControllerMappings.remapTriggersToRightStickForGame("SLUS-12346"))
        assertFalse(ControllerMappings.remapTriggersToRightStick())
        assertEquals(KeyEvent.KEYCODE_BUTTON_L2, ControllerMappings.targetForPhysical(KeyEvent.KEYCODE_BUTTON_L2, 0))
        assertEquals(KeyEvent.KEYCODE_BUTTON_R2, ControllerMappings.targetForPhysical(KeyEvent.KEYCODE_BUTTON_R2, 0))

        ControllerMappings.setRemapTriggersToRightStick("SLUS-12345", false)
        assertFalse(ControllerMappings.remapTriggersToRightStickForGame("SLUS-12345"))
    }

    @Test
    fun perGameControlsResetClearsOnlyThatGamesRemap() {
        ControllerMappings.setRemapTriggersToRightStick("SLUS-12345", true)
        ControllerMappings.setRemapTriggersToRightStick("Homebrew", true)
        ControllerMappings.resetAllControls("SLUS-12345")

        assertFalse(ControllerMappings.remapTriggersToRightStickForGame("SLUS-12345"))
        assertTrue(ControllerMappings.remapTriggersToRightStickForGame("Homebrew"))
    }
}
